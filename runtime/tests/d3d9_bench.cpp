// d3d9 bench / on-device acceptance app. Runs the fake guest client of guest_scene.h through the thunk layer in the real process shape
// (guest on its own thread, SDL on the main thread) with whatever d3d9 backend the platform picks (Metal on iOS, DXVK-native on Linux):
//
//   1. the reference scene at 640x360, cold first frame (shader translation, pipeline creation), read back -> d3d9_shot.png
//   2. the animated scene, warm frames
//   3. a 1280x720 device (the iOS game's internal resolution) and stress frames with 1000 and 3000 draws, read back -> d3d9_stress.png
//   4. the scene keeps animating on screen afterwards, so a finished run is visible
//
// Results go to <out>/d3d9_report.json (rewritten after every phase, so a crash still leaves the earlier phases) and <out>/d3d9_log.txt.
// <out> is $DISH_BENCH_OUT, else the app's Documents folder (iOS) / the current directory.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <pthread.h>
#include <unistd.h>
#include <string>
#include <vector>

#include "dish/fault.h"
#include "dish/mainthread.h"
#include "dish/memacct.h"
#include "dish/platform.h"
#include "guest_scene.h"

extern "C" int dish_metal_stats_json(char *buf, size_t n) __attribute__((weak));
namespace dish { struct D3d9Stats { uint64_t locks, lock_bytes_in, lock_bytes_out, unsupported_calls; }; D3d9Stats d3d9_stats(); }

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

std::string g_out;

// ---- PNG (stored deflate blocks: no zlib needed) ---------------------------------------------------------------------------------
uint32_t crc32(const uint8_t *p, size_t n, uint32_t c = 0) {
    static uint32_t tab[256];
    if (!tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; tab[i] = v; }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
void put_be32(std::vector<uint8_t> &v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s)); }
void chunk(std::vector<uint8_t> &png, const char *type, const std::vector<uint8_t> &data) {
    put_be32(png, (uint32_t)data.size());
    size_t at = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    put_be32(png, crc32(&png[at], png.size() - at));
}
bool write_png(const std::string &path, const std::vector<uint8_t> &bgra, int w, int h) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (w * 3 + 1));
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        for (int x = 0; x < w; x++) { const uint8_t *p = &bgra[((size_t)y * w + x) * 4]; raw.push_back(p[2]); raw.push_back(p[1]); raw.push_back(p[0]); }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size() || off == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8)); z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + (long)off, raw.begin() + (long)(off + n));
        off += n;
        if (!n) break;
    }
    put_be32(z, (b << 16) | a);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'}, ihdr;
    put_be32(ihdr, (uint32_t)w); put_be32(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});   // 8-bit RGB
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    return fclose(f) == 0 && ok;
}

// ---- report ---------------------------------------------------------------------------------------------------------------------
struct Series {
    std::vector<double> v;
    std::string json() const {
        if (v.empty()) return "null";
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        double sum = 0;
        for (double x : s) sum += x;
        char b[256];
        snprintf(b, sizeof b, "{\"n\":%zu,\"avg\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"max\":%.3f}", s.size(), sum / s.size(), s[s.size() / 2],
                 s[std::min(s.size() - 1, s.size() * 95 / 100)], s.back());
        return b;
    }
};

struct Report {
    std::vector<std::pair<std::string, std::string>> fields;   // key -> JSON value, in order
    void set(const std::string &k, const std::string &json_value) {
        for (auto &f : fields) if (f.first == k) { f.second = json_value; return; }
        fields.emplace_back(k, json_value);
    }
    void num(const std::string &k, double v) { char b[64]; snprintf(b, sizeof b, "%.3f", v); set(k, b); }
    void str(const std::string &k, const std::string &v) {
        std::string q = "\"";
        for (char c : v) { if (c == '"' || c == '\\') q += '\\'; if ((unsigned char)c >= 0x20) q += c; }
        set(k, q + "\"");
    }
    std::string backend_stats() {
        std::string s = "{";
        if (dish_metal_stats_json) {
            char b[4096];
            dish_metal_stats_json(b, sizeof b);
            s = b;
            s.pop_back();
            s += ",";
        }
        dish::D3d9Stats d = dish::d3d9_stats();
        MemStats m = memacct_query();
        char b[512];
        snprintf(b, sizeof b, "\"thunk_locks\":%llu,\"unsupported_calls\":%llu,\"mem_footprint_mb\":%.1f,\"mem_available_mb\":%.1f,\"guest_committed_mb\":%.1f}",
                 (unsigned long long)d.locks, (unsigned long long)d.unsupported_calls, m.footprint / 1048576.0, m.os_available / 1048576.0,
                 m.guest_committed / 1048576.0);
        return s + b;
    }
    void write() {
        std::string s = "{\n";
        for (size_t i = 0; i < fields.size(); i++) s += "  \"" + fields[i].first + "\": " + fields[i].second + (i + 1 < fields.size() ? ",\n" : "\n");
        s += "}\n";
        std::string tmp = g_out + "d3d9_report.json.tmp";
        if (FILE *f = fopen(tmp.c_str(), "w")) {
            fwrite(s.data(), 1, s.size(), f);
            fclose(f);
            rename(tmp.c_str(), (g_out + "d3d9_report.json").c_str());
        }
        LOGI(LC_CORE, "report updated (%zu fields)", fields.size());
        log_flush();
    }
};

struct Bench {
    Report rep;
    std::atomic<bool> finished{false};
};

// Runs `frames` frames of `fn`, timing the guest-side submission (everything up to Present) and the full frame interval.
template <class F> void timed_frames(Report &rep, const std::string &key, int frames, F fn) {
    Series submit, interval;
    Clock::time_point start = Clock::now(), prev = start;
    for (int i = 0; i < frames; i++) {
        double sub = fn(i);
        Clock::time_point now = Clock::now();
        submit.v.push_back(sub);
        interval.v.push_back(std::chrono::duration<double, std::milli>(now - prev).count());
        prev = now;
    }
    double total = ms_since(start);
    char fps[64];
    snprintf(fps, sizeof fps, "%.2f", frames * 1000.0 / total);
    rep.set(key, "{\"frames\":" + std::to_string(frames) + ",\"fps\":" + fps + ",\"submit_ms\":" + submit.json() + ",\"frame_ms\":" +
                     interval.json() + ",\"backend\":" + rep.backend_stats() + "}");
    LOGI(LC_CORE, "%s: %d frames, %.2f fps", key.c_str(), frames, frames * 1000.0 / total);
}

int guest_main(void *arg) {
    Bench &b = *static_cast<Bench *>(arg);
    Report &rep = b.rep;
    rep.str("phase", "create");
    rep.write();

    // 1. reference scene, cold
    const int w0 = 640, h0 = 360;
    uint32_t hwnd = make_window(u"D3D9Bench", w0, h0);
    GuestScene sc;
    Clock::time_point t0 = Clock::now();
    if (!hwnd || !sc.create(hwnd, w0, h0)) {
        rep.str("error", "scene creation failed (see d3d9_log.txt)");
        rep.write();
        return 1;
    }
    rep.num("create_ms", ms_since(t0));
    t0 = Clock::now();
    sc.frame(1.0f);
    rep.num("cold_frame_ms", ms_since(t0));
    rep.num("cold_submit_ms", sc.last_submit_ms);
    std::vector<uint8_t> px;
    if (sc.readback(&px)) {
        bool ok = write_png(g_out + "d3d9_shot.png", px, w0, h0);
        rep.str("shot", ok ? "d3d9_shot.png" : "write failed");
    } else {
        rep.str("shot", "readback failed");
    }
    rep.set("after_cold", rep.backend_stats());
    rep.str("phase", "warm");
    rep.write();

    // 2. animated scene, warm
    timed_frames(rep, "scene_640x360", 240, [&](int i) { sc.frame(1.0f + i / 60.0f); return sc.last_submit_ms; });
    rep.str("phase", "recreate");
    rep.write();

    // 3. device at the game's internal resolution, stress
    sc.destroy();
    const int w1 = 1280, h1 = 720;
    GuestScene st;
    t0 = Clock::now();
    if (!st.create(hwnd, w1, h1)) {
        rep.str("error", "second device creation failed");
        rep.write();
        return 1;
    }
    rep.num("recreate_ms", ms_since(t0));
    rep.str("phase", "stress");
    rep.write();
    timed_frames(rep, "scene_1280x720", 120, [&](int i) { st.frame(1.0f + i / 60.0f); return st.last_submit_ms; });
    rep.write();
    for (int draws : {1000, 3000}) {
        std::vector<std::vector<double>> marks;   // per frame: ms at the first draw, then after every 250 draws, then after the last
        timed_frames(rep, "stress_" + std::to_string(draws), draws == 3000 ? 300 : 180, [&](int i) {
            marks.emplace_back();
            st.stress_frame(i / 60.0f, draws, &marks.back());
            return st.last_submit_ms;
        });
        // median time of each segment: [0] = state setup before the first draw, then the 250-draw blocks
        std::string seg = "[";
        for (size_t k = 0; k < marks[0].size(); k++) {
            Series s;
            for (auto &m : marks) if (k < m.size()) s.v.push_back(k ? m[k] - m[k - 1] : m[0]);
            std::sort(s.v.begin(), s.v.end());
            char b[32];
            snprintf(b, sizeof b, "%s%.3f", k ? "," : "", s.v[s.v.size() / 2]);
            seg += b;
        }
        rep.set("stress_" + std::to_string(draws) + "_segments_ms", seg + "]");
        rep.write();
    }
    if (st.readback(&px)) write_png(g_out + "d3d9_stress.png", px, w1, h1);
    rep.str("phase", "done");
    rep.write();
    b.finished = true;

    // 4. keep the scene on screen
    if (getenv("DISH_BENCH_EXIT")) { st.destroy(); return 0; }
    for (int i = 0;; i++) st.frame(1.0f + i / 60.0f);
}

void *guest_thread_main(void *arg) {
    int rc = thread_run_main(guest_main, arg, 4u << 20);
    LOGI(LC_CORE, "guest finished: %d", rc);
    log_flush();
    static_cast<Bench *>(arg)->finished = true;
    mainthread_post([] {});
    return nullptr;
}

void wake_sdl() {
    if (!SDL_WasInit(SDL_INIT_EVENTS)) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_USER;
    SDL_PushEvent(&e);
}

bool SDLCALL lifecycle_watch(void *, SDL_Event *e) {
    switch (e->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_DID_ENTER_FOREGROUND:
        case SDL_EVENT_LOW_MEMORY:
            platform_route_event(*e);
            break;
        default: break;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (const char *o = getenv("DISH_BENCH_OUT")) g_out = o;
#ifdef __APPLE__
    else if (const char *d = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)) g_out = d;
    const char *tmp = getenv("TMPDIR");
    if ((!tmp || !*tmp) && getenv("HOME")) setenv("TMPDIR", (std::string(getenv("HOME")) + "/tmp").c_str(), 1);   // the container's tmp/
#endif
    if (!g_out.empty() && g_out.back() != '/') g_out += '/';
    log_init_from_env();
    log_set_file((g_out + "d3d9_log.txt").c_str());
    fault_set_log_path((g_out + "d3d9_fault.log").c_str());

    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
    setenv("SDL_AUDIODRIVER", "dummy", 0);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_DisableScreenSaver();
    SDL_AddEventWatch(lifecycle_watch, nullptr);
    memacct_set_budget(1800ull << 20, 1400ull << 20);

    t::setup_runtime("d3d9bench");
    mainthread_init();
    mainthread_set_waker(wake_sdl);
    if (!platform_video_init()) { fprintf(stderr, "no video\n"); return 1; }

    Bench bench;
    bench.rep.str("build", __DATE__ " " __TIME__);
    bench.rep.str("platform", SDL_GetPlatform());
    bench.rep.str("backend_env", getenv("DISH_D3D9_BACKEND") ? getenv("DISH_D3D9_BACKEND") : "(platform default)");
    LOGI(LC_CORE, "d3d9 bench: output to %s", g_out.empty() ? "." : g_out.c_str());

    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16u << 20);   // docs/guest_abi.md §6: >= 16 MiB host stack for every thread that runs guest code
    if (pthread_create(&th, &at, guest_thread_main, &bench) != 0) { fprintf(stderr, "cannot start the guest thread\n"); return 1; }
    pthread_attr_destroy(&at);

    for (;;) {
        mainthread_pump(0);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { log_flush(); _exit(0); }
            platform_route_event(e);
        }
        if (bench.finished && getenv("DISH_BENCH_EXIT")) {
            pthread_join(th, nullptr);
            log_flush();
            fflush(nullptr);
            _exit(0);
        }
        SDL_WaitEventTimeout(nullptr, 5);
    }
}
