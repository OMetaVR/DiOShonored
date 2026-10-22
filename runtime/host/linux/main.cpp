// dishonored_linux: Linux host for the recompiled Dishonored.
//   dishonored_linux --game-root <dir> --save-dir <dir> [options] [-- guest args...]
#include "dish/call.h"
#include "dish/config.h"
#include "dish/hle.h"
#include "dish/loader.h"
#include "dish/log.h"
#include "dish/mainthread.h"
#include "dish/platform.h"
#include "dish/thunk.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <SDL3/SDL.h>

using namespace dish;

static void usage() {
    fprintf(stderr,
            "usage: dishonored_linux --game-root DIR --save-dir DIR [options] [-- guest args]\n"
            "  --game-root DIR   the user's Dishonored install (read-only)\n"
            "  --save-dir DIR    where saves, config, logs and temp files go\n"
            "  --log SPEC        log levels, e.g. info,file=debug,hle=trace (also DISH_LOG)\n"
            "  --log-file PATH   also write the log to PATH\n"
            "  --trace-calls     log every HLE thunk call (very verbose)\n"
            "  --no-verify       skip the SHA-256 check of the game files (development only)\n"
            "  --check           verify the game files against the recompiled program and exit\n"
            "  --dump-hle        print implemented / stubbed imports as text and exit\n");
}

static void wake_sdl() {
    if (!SDL_WasInit(SDL_INIT_EVENTS)) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_USER;
    SDL_PushEvent(&e);
}

static int dump_hle() {
    std::map<std::string, std::vector<std::string>> impl, stub;
    for (uint32_t i = 0; i < guest_program.n_imports; i++) {
        std::string dll = guest_program.imports[i].dll;
        for (auto &c : dll) c = (char)tolower((unsigned char)c);
        std::string nm = guest_program.imports[i].name ? guest_program.imports[i].name : "";
        if (nm.empty()) {
            const HleSig *s = sig_find_ordinal(dll.c_str(), guest_program.imports[i].ordinal);
            nm = s ? s->name : "ord" + std::to_string(guest_program.imports[i].ordinal);
        }
        (hle_find(dll.c_str(), nm.c_str()) ? impl : stub)[dll].push_back(nm);
    }
    printf("# implemented HLE functions registered in this build: %zu\n", hle_impl_count());
    printf("# program imports: %u (implemented %zu)\n", guest_program.n_imports, [&] { size_t n = 0; for (auto &kv : impl) n += kv.second.size(); return n; }());
    for (auto *m : {&impl, &stub})
        for (auto &kv : *m) for (auto &n : kv.second) printf("%s\t%s\t%s\n", m == &impl ? "implemented" : "stub", kv.first.c_str(), n.c_str());
    // registry-wide listing for the docs generator
    struct Ctx {} c;
    hle_for_each([](const HleImpl &i, void *) { printf("registered\t%s\t%s\n", i.dll, i.name); }, &c);
    return 0;
}

int main(int argc, char **argv) {
    RuntimeConfig &cfg = config();
    bool check = false, dump = false;
    std::string log_spec, log_file;
    int i = 1;
    for (; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&](const char *name) -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", name); exit(2); }
            return argv[++i];
        };
        if (a == "--game-root") cfg.game_root = val("--game-root");
        else if (a == "--save-dir") cfg.save_dir = val("--save-dir");
        else if (a == "--log") log_spec = val("--log");
        else if (a == "--log-file") log_file = val("--log-file");
        else if (a == "--trace-calls") setenv("DISH_TRACE_CALLS", "1", 1);
        else if (a == "--no-verify") cfg.verify_hashes = false;
        else if (a == "--check") check = true;
        else if (a == "--dump-hle") dump = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--") { i++; break; }
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    for (; i < argc; i++) cfg.args.push_back(argv[i]);
    log_init_from_env();
    if (!log_spec.empty() && !log_set_spec(log_spec.c_str())) fprintf(stderr, "warning: bad --log spec '%s'\n", log_spec.c_str());
    if (!log_file.empty()) log_set_file(log_file.c_str());
    if (dump) return dump_hle();
    if (cfg.game_root.empty() || cfg.save_dir.empty()) { usage(); return 2; }
    if (guest_program.n_modules == 0) {
        fprintf(stderr, "this build contains no recompiled program (generated/program.c was not found at configure time).\n"
                        "Run the lifter, then re-run cmake with -DDISH_GENERATED_DIR=<path to generated/>.\n");
        return 3;
    }

    mainthread_init();
    // SDL video is initialised lazily on this thread (platform_video_init via mainthread_call) when the guest first needs a window;
    // the window itself is created when the guest calls CreateWindowEx.
    mainthread_set_waker(wake_sdl);
    std::atomic<bool> done{false};
    int rc = 0;
    std::thread guest([&] {
        if (check) {
            if (!runtime_init(guest_program)) { rc = 1; }
            else {
                std::string err;
                rc = loader_map_all(guest_program, &err) ? 0 : 1;
                if (rc) fprintf(stderr, "check failed: %s\n", err.c_str());
                else printf("all %u modules verified\n", guest_program.n_modules);
            }
        } else {
            rc = runtime_run(guest_program);
        }
        done = true;
        mainthread_post([] {});
        wake_sdl();
    });
    while (!done) {
        mainthread_pump(0);
        if (SDL_WasInit(SDL_INIT_EVENTS)) {
            platform_dev_input();
            SDL_Event e;
            while (SDL_PollEvent(&e)) platform_route_event(e);
            SDL_WaitEventTimeout(nullptr, 10);
        } else {
            mainthread_pump(20);
        }
    }
    guest.join();
    platform_shutdown();
    mainthread_shutdown();
    stub_report(true);
    log_flush();
    fflush(nullptr);
    _exit(rc);   // like ExitProcess: skip static destructors while DXVK / driver threads may still be running
}
