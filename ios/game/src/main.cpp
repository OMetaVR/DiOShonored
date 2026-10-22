// Dishonored for iOS: app shell.
//
// SDL owns the UIKit main thread (SDL_RunApp -> UIApplicationMain with SDL's UIScene delegate; SDL_main runs here).
// This thread is the host main loop: it shows the import screen until the user's game files are in place, then
// runs the SDL event pump and the runtime's main-thread service queue while the recompiled game runs on its own
// pthread (16 MiB host stack; the runtime adds the guest main thread on top).
//
// The game root is a folder in the app's Documents directory, filled through the Files app (UIFileSharingEnabled +
// LSSupportsOpeningDocumentsInPlace). With the placeholder program linked (no generated/ at build time) the app stays
// on the import screen and reports what it found.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dish/call.h"
#include "dish/config.h"
#include "dish/fault.h"
#include "dish/gamefiles.h"
#include "dish/lifecycle.h"
#include "dish/loader.h"
#include "dish/log.h"
#include "dish/mainthread.h"
#include "dish/memacct.h"
#include "dish/platform.h"
#include "dish/shader_prep.h"
#include "dish/watchdog.h"
#include "guest_abi.h"
#include "download.h"
#include "libs.h"
#include "screen.h"

using namespace dish;

namespace {

struct Paths {
    std::string docs, save_dir, log_file, game_root_hint;
};

std::string with_slash(std::string s) {
    if (!s.empty() && s.back() != '/') s += '/';
    return s;
}

Paths setup_paths() {
    Paths p;
    const char *d = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    p.docs = with_slash(d ? d : "/tmp/");
    p.save_dir = p.docs + "DishonoredData";
    mkdir(p.save_dir.c_str(), 0755);
    p.log_file = p.save_dir + "/dishonored.log";
    // Make the Documents folder show up in Files, with a hint where to put the game.
    p.game_root_hint = p.docs + "Dishonored";
    mkdir(p.game_root_hint.c_str(), 0755);
    std::string readme = p.game_root_hint + "/PUT_YOUR_DISHONORED_FILES_HERE.txt";
    struct stat st;
    if (stat(readme.c_str(), &st) != 0) {
        if (FILE *f = fopen(readme.c_str(), "w")) {
            fputs("Copy the contents of your Steam Dishonored folder (the one that contains Binaries/Win32/Dishonored.exe)\n"
                  "into this folder, using the Files app. Then reopen the game.\n",
                  f);
            fclose(f);
        }
    }
    return p;
}

// Lifecycle events must be handled synchronously inside the event watch: iOS gives the app only moments after
// WILL_ENTER_BACKGROUND, and GPU work submitted afterwards is an error.
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

// ---- verification of the game files runs on a worker (hashing ~30 MB takes a moment on a phone)
struct Verify {
    std::mutex mu;
    std::string root;
    bool running = false, done = false, ok = false;
    size_t progress_i = 0, progress_n = 0;
    std::vector<FileCheck> files;
};

void start_verify(Verify &v, const std::string &root) {
    {
        std::lock_guard<std::mutex> lk(v.mu);
        v.root = root;
        v.running = true;
        v.done = false;
        v.ok = false;
        v.files.clear();
    }
    std::thread([&v, root] {
        std::vector<FileCheck> files;
        bool ok = gamefiles_verify(guest_program, root, true, &files, [&](size_t i, size_t n) {
            std::lock_guard<std::mutex> lk(v.mu);
            v.progress_i = i;
            v.progress_n = n;
        });
        std::lock_guard<std::mutex> lk(v.mu);
        v.files = std::move(files);
        v.ok = ok;
        v.running = false;
        v.done = true;
    }).detach();
}

std::string mib(uint64_t b) { return std::to_string(b >> 20) + " MiB"; }

struct GuestRun {
    std::atomic<bool> done{false};
    int rc = 0;
};

void *guest_main(void *p) {
    GuestRun *g = static_cast<GuestRun *>(p);
    g->rc = runtime_run(guest_program);
    g->done = true;
    mainthread_post([] {});
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_USER;
    SDL_PushEvent(&e);
    return nullptr;
}

void wake_sdl() {
    if (!SDL_WasInit(SDL_INIT_EVENTS)) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_USER;
    SDL_PushEvent(&e);
}

}  // namespace

// ---- shader preparation (see dish/shader_prep.h): compile the shaders and pipelines recorded in earlier sessions before the game
// starts, so they are not compiled mid-game (stutter). The player can skip it or let it continue in the background.
static ShaderPrepStatus g_prep;
static void run_shader_prep(TextScreen &screen) {
    const ShaderPrepSummary sum = shader_prep_summary();
    if (sum.shaders == 0 && sum.pipelines == 0) return;   // first session: nothing recorded yet
    if (sum.all_prepared) {                              // nothing new since the last full preparation: refresh quietly
        g_prep.background = true;
        shader_prep_start(&g_prep);
        return;
    }
    shader_prep_start(&g_prep);
    std::vector<SDL_Gamepad *> pads;
    int n = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&n)) {
        for (int i = 0; i < n; i++) if (SDL_Gamepad *g = SDL_OpenGamepad(ids[i])) pads.push_back(g);
        SDL_free(ids);
    }
    int choice = 0;   // 0 wait, 1 continue in the background, 2 skip
    const Uint64 t0 = SDL_GetTicks();
    while (!g_prep.finished.load() && choice == 0) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_FINGER_DOWN) choice = e.tfinger.x < 0.5f ? 2 : 1;
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED) { if (SDL_Gamepad *g = SDL_OpenGamepad(e.gdevice.which)) pads.push_back(g); }
            else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e.gbutton.button == SDL_GAMEPAD_BUTTON_START) choice = 1;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) choice = 2;
            }
        }
        const uint32_t sd = g_prep.shaders_done, st = g_prep.shaders_total, pd = g_prep.pipelines_done, pt = g_prep.pipelines_total;
        const double frac = (st + pt) ? (double)(sd + pd) / (double)(st + pt) : 0.0;
        const double el = (SDL_GetTicks() - t0) / 1000.0;
        char line[200];
        std::vector<ScreenLine> lines = {L("DISHONORED", kTitle), L(""), L("Preparing shaders", kTitle),
                                         L("New areas were played since the last preparation. Compiling their shaders now avoids stutter "
                                           "the next time they appear. This is needed once; later launches skip it.")};
        lines.push_back(L(""));
        int bar = (int)(frac * 40 + 0.5);
        lines.push_back(L("[" + std::string((size_t)bar, '#') + std::string((size_t)(40 - bar), '.') + "]", kGood));
        snprintf(line, sizeof line, "shaders %u / %u, pipelines %u / %u", sd, st, pd, pt);
        lines.push_back(L(line));
        if (frac > 0.02 && el > 3) {
            snprintf(line, sizeof line, "about %.0f s left", el / frac - el);
            lines.push_back(L(line, kDim));
        }
        lines.push_back(L(""));
        lines.push_back(L("Tap the right half (or press A): play now, keep preparing in the background (some stutter until done).", kDim));
        lines.push_back(L("Tap the left half (or press B): skip, shaders compile while you play (more stutter).", kDim));
        screen.draw(lines);
        SDL_WaitEventTimeout(nullptr, 100);
    }
    if (choice == 2) g_prep.cancel = true;
    else if (choice == 1) g_prep.background = true;
    LOGI(LC_CORE, "shader prep: %s", g_prep.finished.load() ? "finished before the game started" : choice == 2 ? "skipped" : "continues in the background");
    for (SDL_Gamepad *g : pads) SDL_CloseGamepad(g);
}

// ---- player settings: DishonoredData/dish_settings.ini (editable in the Files app; created with defaults on first launch)
static void apply_settings(const std::string &dir) {
    const std::string path = dir + "/dish_settings.ini";
    FILE *f = fopen(path.c_str(), "r");
    if (!f) {
        if ((f = fopen(path.c_str(), "w"))) {
            fputs("# Dishonored for iOS settings. Edit in the Files app; changes apply the next time the app starts.\n"
                  "#\n"
                  "# render_height: lines the game renders before MetalFX upscales to the screen (the screen has 1170).\n"
                  "#   720 = fastest, 900 = default, 1080 = sharp, 1170 = native. Higher costs GPU time (about pixels squared).\n"
                  "render_height = 900\n", f);
            fclose(f);
        }
        f = fopen(path.c_str(), "r");
    }
    int height = 900;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            int v;
            if (sscanf(line, " render_height = %d", &v) == 1) height = v;
        }
        fclose(f);
    }
    height = std::max(480, std::min(height, 1440));
    char v[16];
    snprintf(v, sizeof v, "%d", height);
    setenv("DISH_METAL_INTERNAL_HEIGHT", v, 0);   // the environment still wins (tests)
    LOGI(LC_CORE, "settings: render_height %s", getenv("DISH_METAL_INTERNAL_HEIGHT"));
}

// The game remembers its last resolution in DishonoredEngine.ini; when it differs from the one display mode we offer, it lays the
// frame out for one size and renders at another (the menu in a corner). Point ResX/ResY at the mode before the game starts.
static void sync_game_resolution(const std::string &save_dir) {
    const char *ih = getenv("DISH_METAL_INTERNAL_HEIGHT");
    int h = ih ? atoi(ih) : 720;
    double aspect = 16.0 / 9.0;
    SDL_Rect r;
    if (SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &r) && r.w > 0 && r.h > 0) aspect = (double)std::max(r.w, r.h) / std::min(r.w, r.h);
    int w = ((int)(h * aspect + 0.5)) & ~1;   // same rule as the virtual desktop (win32_user32.cpp)
    const std::string path = save_dir + "/Documents/My Games/Dishonored/DishonoredGame/Config/DishonoredEngine.ini";
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return;   // first launch: the game creates it from the defaults
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    std::string out;
    size_t pos = 0;
    int changed = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        std::string cr = (!line.empty() && line.back() == '\r') ? "\r" : "";
        if (line.rfind("ResX=", 0) == 0) { std::string nl = "ResX=" + std::to_string(w) + cr; changed += nl != line; line = nl; }
        else if (line.rfind("ResY=", 0) == 0) { std::string nl = "ResY=" + std::to_string(h) + cr; changed += nl != line; line = nl; }
        out += line;
        if (e < text.size()) out += '\n';
        pos = e + 1;
    }
    if (!changed) return;
    if ((f = fopen(path.c_str(), "wb"))) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
    LOGI(LC_CORE, "settings: game resolution set to %dx%d", w, h);
}

int main(int argc, char **argv) {
    Paths paths = setup_paths();
    RuntimeConfig &cfg = config();
    cfg.save_dir = paths.save_dir;
    log_init_from_env();
    log_set_file(paths.log_file.c_str());
    fault_set_log_path((paths.save_dir + "/fault.log").c_str());
    set_unresolved_log_path((paths.save_dir + "/unresolved.log").c_str());
    apply_settings(paths.save_dir);
    std::string libs = dish_ios_configure_libraries();
    LOGI(LC_CORE, "Dishonored iOS shell: documents=%s libraries=%s program=%u modules", paths.docs.c_str(), libs.c_str(),
         guest_program.n_modules);

    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");   // the import window is destroyed before the guest creates its own
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   // controller input must not depend on SDL focus tracking
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_DisableScreenSaver();
    SDL_AddEventWatch(lifecycle_watch, nullptr);

    // Memory: ~2.1 GB jetsam ceiling on a 4 GB iPhone; the footprint includes Metal allocations.
    uint64_t budget = 1800ull << 20, commit = 1400ull << 20;
    if (const char *e = getenv("DISH_COMMIT_LIMIT_MB")) commit = strtoull(e, nullptr, 10) << 20;
    memacct_set_budget(budget, commit);

    TextScreen screen;
    if (!screen.open()) return 1;
    Verify verify;
    std::string found_root;
    Uint64 last_scan = 0;
    bool launched = false;
    GuestRun run;
    pthread_t guest_thread{};

    std::string env_root = getenv("DISH_GAME_ROOT") ? getenv("DISH_GAME_ROOT") : "";
#ifdef DISH_DEV_FILE_SERVER
    const std::string dev_server = DISH_DEV_FILE_SERVER;
    // dev builds: save a frame every 15 s (Files > Dishonored > DishonoredData > frames) so a test run shows what was drawn
    std::string frames_dir = paths.save_dir + "/frames";
    mkdir(frames_dir.c_str(), 0755);
    setenv("DISH_FRAME_DUMP", frames_dir.c_str(), 0);
    setenv("DISH_FRAME_DUMP_EVERY", "15", 0);
    // and log every D3D9 call of one frame on the main menu (viewports, render-target sizes) for back-buffer layout checks
    setenv("DISH_D3D9_TRACE_FRAME", "4000", 0);
    // and a CPU profile of every thread from 200 to 320 s after launch (DishonoredData/profile.txt, tools/analysis/profile_report.py)
    setenv("DISH_PROFILE", "200:120", 0);
#else
    const std::string dev_server;
#endif
    GameDownload download;
    for (;;) {
        SDL_Event e;
        bool tapped = false;
        if (launched) platform_dev_input();
        while (SDL_PollEvent(&e)) {
            if (launched) platform_route_event(e);
            else if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) tapped = true;
        }
        if (launched) {
            mainthread_pump(0);
            if (run.done && !screen.is_open()) {
                pthread_join(guest_thread, nullptr);
                platform_shutdown();
                int rc = run.rc;
                LOGI(LC_CORE, "guest exited with code %d", rc);
                log_flush();
                if (screen.open()) {
                    std::vector<ScreenLine> lines = {L("DISHONORED", kTitle), L(""), L("The game exited with code " + std::to_string(rc) + ".", rc == 0 ? kGood : kBad),
                                                     L(""), L("Log: Files > Dishonored > DishonoredData > dishonored.log", kDim),
                                                     L("Swipe the app away and reopen it to play again.", kDim)};
                    for (;;) {   // the runtime cannot be restarted in-process
                        SDL_Event ev;
                        while (SDL_WaitEventTimeout(&ev, 500)) {}
                        screen.draw(lines);
                    }
                }
                return rc;
            }
            if (!run.done) {
                if (SDL_WasInit(SDL_INIT_EVENTS)) SDL_WaitEventTimeout(nullptr, 5);
            }
            continue;
        }

        // ---- import screen
        Uint64 now = SDL_GetTicks();
        if (now - last_scan >= 1000 || last_scan == 0) {
            last_scan = now;
            std::string marker = cfg.exe_relpath;
            std::string root = env_root.empty() ? gamefiles_find_root(paths.docs, marker) : env_root;
            found_root = root;
        }
        // a root counts only when the copy is complete: dev builds wait for the downloader's marker; every build checks key data files
        // (the binaries arrive first, and starting the game from a partial copy crashes it)
        struct stat sb;
        auto exists = [&](const std::string &rel) { return stat((found_root + "/" + rel).c_str(), &sb) == 0; };
        const bool download_incomplete = !dev_server.empty() && !found_root.empty() && !exists(kGameDownloadCompleteMarker);
        const bool data_missing = !found_root.empty() && (!exists("DishonoredGame/CookedPCConsole/Engine.upk") || !exists("DishonoredGame/Config/DefaultEngine.ini"));
        const bool incomplete = download_incomplete || data_missing;
        bool want_verify = false;
        {
            std::lock_guard<std::mutex> lk(verify.mu);
            want_verify = !found_root.empty() && !incomplete && guest_program.n_modules > 0 && !verify.running && verify.root != found_root;
        }
        if (want_verify) start_verify(verify, found_root);

        std::vector<ScreenLine> lines;
        lines.push_back(L("DISHONORED for iOS", kTitle));
        lines.push_back(L(guest_program.n_modules > 0
                              ? "recompiled program: " + std::to_string(guest_program.n_modules) + " modules"
                              : "placeholder build: no recompiled game code is linked in yet",
                          guest_program.n_modules > 0 ? kDim : SDL_Color{255, 190, 90, 255}));
        lines.push_back(L(""));
        const size_t help_begin = lines.size();
        lines.push_back(L("Import your game files", kTitle));
        lines.push_back(L("This app does not include the game. Use your own copy of Dishonored (Steam, PC)."));
        lines.push_back(L("1. Open the Files app, then On My iPhone, then this app's folder (under LiveContainer: the app's Documents)."));
        lines.push_back(L("2. Copy the contents of your Dishonored install folder into the Dishonored folder there. It must contain Binaries/Win32/Dishonored.exe."));
        lines.push_back(L("3. Come back here. This screen checks again every second."));
        lines.push_back(L(""));
        lines.push_back(L("Looking in: " + (env_root.empty() ? paths.docs : env_root), kDim));
        const size_t help_end = lines.size();
        if (!dev_server.empty()) {
            std::lock_guard<std::mutex> lk(download.mu);
            if (download.running) {
                char b[160];
                snprintf(b, sizeof b, "Downloading from your PC: %zu / %zu files, %.2f / %.2f GB, %.0f MB/s", download.files_done, download.files_total,
                         download.bytes_done / 1e9, download.bytes_total / 1e9, download.mb_per_s);
                lines.push_back(L(b, kTitle));
                lines.push_back(L(download.current, kDim));
            } else if (download.done && !download.ok) {
                lines.push_back(L("Download failed: " + download.error + ". Tap to retry (it resumes).", kBad));
            } else if (download.done) {
                lines.push_back(L("Download complete.", kGood));
            } else if (found_root.empty() || incomplete) {
                lines.push_back(L("Dev build: tap anywhere to download your game files from your PC (" + dev_server + ").", kTitle));
                lines.push_back(L("On the PC: python3 tools/dev/game_files_server.py <Dishonored folder>", kDim));
            }
        }
        if (!dev_server.empty() && (found_root.empty() || incomplete)) {
            static bool auto_resumed = false;
            bool idle;
            { std::lock_guard<std::mutex> lk(download.mu); idle = !download.running && !(download.done && download.ok); }
            bool resume = idle && download_incomplete && !auto_resumed;   // an interrupted copy continues by itself
            if (idle && (tapped || resume)) {
                auto_resumed = true;
                LOGI(LC_CORE, "downloading game files from %s", dev_server.c_str());
                game_download_start(download, dev_server, paths.game_root_hint);
            }
        }
        {
            std::lock_guard<std::mutex> lk(download.mu);
            if (download.done && download.ok) { last_scan = 0; download.done = false; }   // rescan now (and only once)
        }
        bool ready = false;
        if (found_root.empty()) {
            lines.push_back(L("Status: game files not found yet.", SDL_Color{255, 190, 90, 255}));
        } else if (incomplete) {
            lines.push_back(L("Found " + found_root + ", but the copy is incomplete" + (download_incomplete ? " (download not finished)." : " (DishonoredGame/CookedPCConsole or Config missing)."),
                              SDL_Color{255, 190, 90, 255}));
        } else {
            lines.push_back(L("Found: " + found_root, kGood));
            if (guest_program.n_modules == 0) {
                lines.push_back(L("This build has no recompiled game code, so there is nothing to run yet.", SDL_Color{255, 190, 90, 255}));
            } else {
                std::lock_guard<std::mutex> lk(verify.mu);
                if (verify.running) {
                    lines.push_back(L("Verifying files " + std::to_string(verify.progress_i) + "/" + std::to_string(verify.progress_n) + " ..."));
                } else if (verify.done) {
                    for (const FileCheck &f : verify.files) {
                        const char *st = f.state == FileCheck::Ok ? "ok" : f.state == FileCheck::Missing ? "MISSING" : f.state == FileCheck::Mismatch ? "WRONG VERSION" : "UNREADABLE";
                        lines.push_back(L(f.relpath + "  " + st, f.state == FileCheck::Ok ? kGood : kBad));
                    }
                    if (verify.ok) { lines.push_back(L("All files verified. Starting ...", kGood)); ready = true; }
                    else lines.push_back(L("Some files are missing or are not the supported Steam build.", kBad));
                }
            }
        }
        // once something is going on (download, files found), the status goes first and the copy instructions to the end
        bool busy;
        { std::lock_guard<std::mutex> lk(download.mu); busy = download.running || download.done; }
        if (busy || !found_root.empty()) {
            std::vector<ScreenLine> help(lines.begin() + (long)help_begin, lines.begin() + (long)help_end);
            lines.erase(lines.begin() + (long)help_begin, lines.begin() + (long)help_end);
            lines.push_back(L(""));
            lines.insert(lines.end(), help.begin(), help.end());
        }
        MemStats ms = memacct_query();
        lines.push_back(L(""));
        lines.push_back(L("memory available to the app: " + mib(ms.os_available) + "   libraries: " + libs + "   build " __DATE__ " " __TIME__, kDim));
        screen.draw(lines);

        if (ready) {
            LOGI(LC_CORE, "starting the game from %s", found_root.c_str());
            cfg.game_root = found_root;
            cfg.verify_hashes = false;   // just verified
            sync_game_resolution(paths.save_dir);
            run_shader_prep(screen);
            screen.close();
            mainthread_init();
            mainthread_set_waker(wake_sdl);
            watchdog_start((paths.save_dir + "/stall.log").c_str());
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setstacksize(&at, 16u << 20);
            if (pthread_create(&guest_thread, &at, guest_main, &run) != 0) {
                fprintf(stderr, "cannot start the guest thread\n");
                return 1;
            }
            pthread_attr_destroy(&at);
            launched = true;
            continue;
        }
        SDL_WaitEventTimeout(nullptr, 250);
    }
    (void)argc; (void)argv;
}
