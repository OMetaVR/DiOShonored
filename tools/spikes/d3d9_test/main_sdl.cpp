/* Linux/desktop driver: SDL3 window + DXVK-native d3d9 (loaded with dlopen, same as the iOS app will do). */
#include "d3d9_scene.h"
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    int frames = 0 /* 0 = run until closed */, vsync = 1, w = 1280, h = 720;
    const char *shot = NULL, *lib = "libdxvk_d3d9.so.0";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--novsync")) vsync = 0;
        else if (!strcmp(argv[i], "--lib") && i + 1 < argc) lib = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) { w = atoi(argv[++i]); h = atoi(argv[++i]); }
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window *win = SDL_CreateWindow("d3d9_test (DXVK-native)", w, h, SDL_WINDOW_VULKAN);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    setenv("DXVK_WSI_DRIVER", "SDL3", 0); /* DXVK >= 3.x refuses to guess the WSI on non-Windows */
    void *h9 = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h9) { fprintf(stderr, "dlopen(%s): %s\n", lib, dlerror()); return 1; }
    void *fn = dlsym(h9, "Direct3DCreate9");
    if (!fn) { fprintf(stderr, "Direct3DCreate9 not exported\n"); return 1; }

    d3d9_scene_desc d = {}; d.create9 = (void *(*)(unsigned))fn; d.window = win; d.width = w; d.height = h; d.vsync = vsync;
    d3d9_scene *sc = NULL; char err[512] = {};
    if (d3d9_scene_create(&d, &sc, err, sizeof err, NULL, NULL)) { fprintf(stderr, "create failed: %s\n", err); return 2; }

    Uint64 t0 = SDL_GetTicks(), last = t0; int n = 0, rc = 0; bool run = true;
    while (run) {
        SDL_Event e; while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_KEY_DOWN) run = false;
        float t = (SDL_GetTicks() - t0) / 1000.f;
        int r = d3d9_scene_frame(sc, t);
        if (r) { fprintf(stderr, "frame failed: %d\n", r); rc = 3; break; }
        n++;
        Uint64 now = SDL_GetTicks();
        if (now - last >= 1000) { fprintf(stderr, "fps: %.1f\n", n * 1000.0 / (now - t0)); last = now; }
        if (frames && n >= frames) {
            char rep[1024]; int st = d3d9_scene_selftest(sc, rep, sizeof rep); fputs(rep, stderr);
            if (shot) fprintf(stderr, "screenshot %s: %s\n", shot, d3d9_scene_screenshot_ppm(sc, shot) ? "FAILED" : "ok");
            rc = st; break;
        }
    }
    d3d9_scene_destroy(sc);
    SDL_DestroyWindow(win); SDL_Quit();
    return rc;
}
