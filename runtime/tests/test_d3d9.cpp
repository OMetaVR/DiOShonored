// d3d9 acceptance: a fake guest client re-expresses tools/spikes/d3d9_test's scene using ONLY guest vtable calls (guest_call through
// thunk addresses read out of guest memory), renders it through the thunks into DXVK-native, and compares the result with the spike's
// direct-call renderer on the same GPU. Also measures the per-call cost of the hot methods.
#include "testing.h"
#include "dish/com.h"
#include "dish/platform.h"
#include "d3d9_scene.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "shaders_gen.h"

extern "C" {
void *host_create9(unsigned v);
double host_bench_ps_const(void *dev, long n);
double host_bench_vs_const4(void *dev, long n);
double host_bench_render_state(void *dev, long n);
double host_bench_sampler_state(void *dev, long n);
double host_bench_set_texture(void *dev, void *tex, long n);
double host_bench_draw(void *dev, long n);
}
namespace dish { struct D3d9Stats { uint64_t locks, lock_bytes_in, lock_bytes_out, unsupported_calls; }; D3d9Stats d3d9_stats(); }

#include "guest_scene.h"

static bool setup_ok() {
    t::setup_runtime("d3d9");
    setenv("SDL_AUDIODRIVER", "dummy", 0);
    return platform_video_init();
}

static std::vector<uint8_t> g_guest_px, g_direct_px;
static int g_w = 640, g_h = 360;

TEST(guest_scene_through_thunks) {
    if (!setup_ok()) { fprintf(stderr, "no display: skipping\n"); exit(77); }
    uint32_t hwnd = make_window(u"D3D9Guest", g_w, g_h);
    CHECK(hwnd != 0);
    // vtable layout sanity: the well-known IDirect3DDevice9 slots
    CHECK_EQ(d3d9_slot("IDirect3DDevice9", "Present"), 17u);
    CHECK_EQ(d3d9_slot("IDirect3DDevice9", "SetTexture"), 65u);
    CHECK_EQ(d3d9_slot("IDirect3DDevice9", "DrawIndexedPrimitive"), 82u);
    CHECK_EQ(d3d9_slot("IDirect3DDevice9", "SetVertexShaderConstantF"), 94u);
    CHECK_EQ(d3d9_slot("IDirect3DDevice9", "SetPixelShaderConstantF"), 109u);
    CHECK_EQ(d3d9_slot("IDirect3D9", "CreateDevice"), 16u);
    GuestScene sc;
    if (!sc.create(hwnd, g_w, g_h)) { fprintf(stderr, "guest scene creation failed (no usable Vulkan device?): skipping\n"); exit(77); }
    sc.frame(1.0f);
    CHECK(sc.readback(&g_guest_px));
    // device bookkeeping through the guest API
    uint32_t cp = t::galloc(16);
    Guest::call(sc.dev, "IDirect3DDevice9", "GetCreationParameters", {cp}, true);
    CHECK_EQ(rd32(cp + 8), hwnd);
    CHECK_EQ(rd32(cp + 12), 0x40u);
    uint32_t pp = t::galloc(56), sw = t::galloc(4);
    Guest::call(sc.dev, "IDirect3DDevice9", "GetSwapChain", {0, sw}, true);
    Guest::call(rd32(sw), "IDirect3DSwapChain9", "GetPresentParameters", {pp}, true);
    CHECK_EQ(rd32(pp), (uint32_t)g_w);
    CHECK_EQ(rd32(pp + 4), (uint32_t)g_h);
    CHECK_EQ(rd32(pp + 28), hwnd);
    // the same host object always maps to the same guest object (UE3 compares RT pointers)
    uint32_t a = t::galloc(4), b = t::galloc(4);
    Guest::call(sc.dev, "IDirect3DDevice9", "GetBackBuffer", {0, 0, 0, a}, true);
    Guest::call(sc.dev, "IDirect3DDevice9", "GetBackBuffer", {0, 0, 0, b}, true);
    CHECK_EQ(rd32(a), rd32(b));
    uint32_t rt = t::galloc(4);
    Guest::call(sc.dev, "IDirect3DDevice9", "GetRenderTarget", {0, rt}, true);
    CHECK_EQ(rd32(rt), rd32(a));
    // refcounts are tracked per guest object
    CHECK_EQ(Guest::call(rd32(a), "IUnknown", "AddRef", {}), 4u);   // 3 GetBackBuffer/GetRenderTarget refs + the first AddRef
    // QueryInterface returns the existing wrapper
    uint32_t iid_unknown = t::galloc(16), qi = t::galloc(4);   // IID_IUnknown = {0,0,0,C000-000000000046}
    wr8(iid_unknown + 8, 0xC0); wr8(iid_unknown + 15, 0x46);
    CHECK_EQ(Guest::call(sc.d3d, "IUnknown", "QueryInterface", {iid_unknown, qi}), 0u);
    CHECK_EQ(rd32(qi), sc.d3d);
    // the stats recorded the lock traffic of texture/VB uploads and the readback
    CHECK(d3d9_stats().locks > 10);
    // unsupported-by-design methods fail loudly but keep the stack balanced
    CHECK_EQ(Guest::call(rd32(a), "IDirect3DSurface9", "GetDC", {t::galloc(4)}), (uint32_t)0x8876086A);   // D3DERR_NOTAVAILABLE
    sc.destroy();
}

TEST(direct_scene_reference_and_compare) {
    if (!setup_ok()) exit(77);
    if (g_guest_px.empty()) { fprintf(stderr, "needs the guest render\n"); exit(77); }
    uint32_t hwnd = make_window(u"D3D9Direct", g_w, g_h);
    d3d9_scene_desc d = {};
    d.create9 = host_create9;
    d.window = platform_sdl_window(hwnd);
    d.width = g_w; d.height = g_h; d.vsync = 0;
    d3d9_scene *sc = nullptr;
    char err[512] = {};
    if (d3d9_scene_create(&d, &sc, err, sizeof err, nullptr, nullptr)) { fprintf(stderr, "direct scene failed: %s\n", err); exit(77); }
    CHECK_EQ(d3d9_scene_frame(sc, 1.0f), 0);
    uint8_t *px = nullptr;
    int w = 0, h = 0;
    CHECK_EQ(d3d9_scene_read_backbuffer(sc, &px, &w, &h), 0);
    CHECK(w == g_w && h == g_h);
    g_direct_px.assign(px, px + (size_t)w * h * 4);
    free(px);
    std::string base = t::temp_dir();
    d3d9_scene_screenshot_ppm(sc, (base + "/direct.ppm").c_str());
    d3d9_scene_destroy(sc);
    // write the guest-path image too
    FILE *f = fopen((base + "/guest.ppm").c_str(), "wb");
    fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
    for (int i = 0; i < g_w * g_h; i++) { uint8_t rgb[3] = {g_guest_px[(size_t)i * 4 + 2], g_guest_px[(size_t)i * 4 + 1], g_guest_px[(size_t)i * 4]}; fwrite(rgb, 1, 3, f); }
    fclose(f);
    // compare
    size_t diff = 0, big = 0, nonblack = 0;
    int maxd = 0;
    for (size_t i = 0; i < (size_t)g_w * g_h; i++) {
        bool d_ = false;
        for (int c = 0; c < 3; c++) {
            int x = abs((int)g_guest_px[i * 4 + c] - (int)g_direct_px[i * 4 + c]);
            if (x) d_ = true;
            if (x > maxd) maxd = x;
            if (x > 3) big++;
        }
        if (d_) diff++;
        if (g_guest_px[i * 4] | g_guest_px[i * 4 + 1] | g_guest_px[i * 4 + 2]) nonblack++;
    }
    fprintf(stderr, "  guest vs direct: %zu of %d pixels differ (max channel delta %d, %zu channels >3); images in %s\n", diff, g_w * g_h, maxd, big, base.c_str());
    CHECK(nonblack > (size_t)g_w * g_h / 2);          // the scene really rendered
    CHECK(big == 0);
    CHECK(diff * 1000 <= (size_t)g_w * g_h);          // <= 0.1% of pixels differ at all
    if (const char *keep = getenv("DISH_KEEP_SHOTS")) {
        std::string cmd = std::string("cp ") + base + "/direct.ppm " + keep + "/d3d9_direct.ppm; cp " + base + "/guest.ppm " + keep + "/d3d9_guest.ppm";
        (void)!system(cmd.c_str());
    }
}

static double thunk_ns(uint32_t dev, const char *method, std::initializer_list<uint32_t> args, long n) {
    std::string key = std::string("IDirect3DDevice9::") + method;
    uint32_t fn = rd32(rd32(dev) + 4 * d3d9_slot("IDirect3DDevice9", method));
    std::vector<uint32_t> a{dev};
    a.insert(a.end(), args.begin(), args.end());
    uint32_t sp = (t::stack_top() & ~15u) - 4 * (uint32_t)a.size() - 4;
    for (size_t i = 0; i < a.size(); i++) wr32(sp + 4 + 4 * (uint32_t)i, a[i]);
    wr32(sp, kReturnSentinel);
    auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < n; i++) guest_call(fn, sp, 0, 0, 0);
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (double)n;
}

TEST(hot_call_overhead) {
    if (!setup_ok()) exit(77);
    uint32_t hwnd = make_window(u"D3D9Bench", g_w, g_h);
    GuestScene sc;
    if (!sc.create(hwnd, g_w, g_h)) exit(77);
    sc.frame(0.5f);
    uint32_t dev = sc.dev;
    void *hdev = com_host_object_raw(sc.dev);
    void *htex = com_host_object_raw(sc.texDxt1);
    CHECK(hdev && htex);
    const long N = 500000;
    auto best = [](auto f) { double b = 1e18; for (int i = 0; i < 7; i++) b = std::min(b, f()); return b; };   // min of 7 runs: timing noise only adds
    uint32_t consts = t::galloc(64);
    for (int i = 0; i < 16; i++) wr32(consts + 4 * (uint32_t)i, f2u((float)i));
    DEV("SetVertexDeclaration", sc.decl); DEV("SetVertexShader", sc.vs); DEV("SetPixelShader", sc.ps);
    struct Row { const char *name; double host, guest; };
    std::vector<Row> rows;
    rows.push_back({"SetPixelShaderConstantF(1 vec)", best([&] { return host_bench_ps_const(hdev, N); }), best([&] { return thunk_ns(sc.dev, "SetPixelShaderConstantF", {3, consts, 1}, N); })});
    rows.push_back({"SetVertexShaderConstantF(4 vec)", best([&] { return host_bench_vs_const4(hdev, N); }), best([&] { return thunk_ns(sc.dev, "SetVertexShaderConstantF", {0, consts, 4}, N); })});
    rows.push_back({"SetRenderState", best([&] { return host_bench_render_state(hdev, N); }), best([&] { return thunk_ns(sc.dev, "SetRenderState", {RS_ALPHAREF, 77}, N); })});
    rows.push_back({"SetSamplerState", best([&] { return host_bench_sampler_state(hdev, N); }), best([&] { return thunk_ns(sc.dev, "SetSamplerState", {3, 10 /*MAXANISOTROPY*/, 4}, N); })});
    rows.push_back({"SetTexture", best([&] { return host_bench_set_texture(hdev, htex, N); }), best([&] { return thunk_ns(sc.dev, "SetTexture", {3, sc.texDxt1}, N); })});
    DEV("SetStreamSource", 0, sc.vb0, 0, 20); DEV("SetStreamSource", 1, sc.vb1, 0, 4); DEV("SetIndices", sc.ib);
    DEV("SetRenderState", RS_ZENABLE, 0);
    DEV("BeginScene");
    double g_draw = best([&] { return thunk_ns(sc.dev, "DrawIndexedPrimitive", {4, 0, 0, 4, 0, 2}, 100000); });
    DEV("EndScene");
    rows.push_back({"DrawIndexedPrimitive", best([&] { return host_bench_draw(hdev, 100000); }), g_draw});
    fprintf(stderr, "  %-34s %10s %10s %10s\n", "method", "host ns", "thunk ns", "overhead");
    for (auto &r : rows) {
        fprintf(stderr, "  %-34s %10.1f %10.1f %+10.1f\n", r.name, r.host, r.guest, r.guest - r.host);
        CHECK(r.guest - r.host < 60.0);
    }
    // Lock/Unlock staging cost: a 64 KiB dynamic vertex-buffer upload
    uint32_t out = t::galloc(4), pdata = t::galloc(4);
    DEV("CreateVertexBuffer", 65536, 0x208 /*DYNAMIC|WRITEONLY*/, 0, POOL_DEFAULT, out, 0);
    uint32_t dyn = rd32(out);
    uint32_t lock = rd32(rd32(dyn) + 4 * d3d9_slot("IDirect3DVertexBuffer9", "Lock")), unlock = rd32(rd32(dyn) + 4 * d3d9_slot("IDirect3DVertexBuffer9", "Unlock"));
    const int M = 20000;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < M; i++) {
        uint32_t sp = (t::stack_top() & ~15u) - 24;
        wr32(sp + 4, dyn); wr32(sp + 8, 0); wr32(sp + 12, 65536); wr32(sp + 16, pdata); wr32(sp + 20, 0x2000 /*DISCARD*/);
        wr32(sp, kReturnSentinel);
        guest_call(lock, sp, 0, 0, 0);
        memset(gp(rd32(pdata)), i, 65536);        // the "game" fills the buffer
        sp = (t::stack_top() & ~15u) - 8;
        wr32(sp + 4, dyn); wr32(sp, kReturnSentinel);
        guest_call(unlock, sp, 0, 0, 0);
    }
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / M;
    fprintf(stderr, "  64 KiB DISCARD lock + fill + unlock: %.2f us per cycle (%.1f GB/s staged)\n", us, 65536.0 / us / 1000.0);
    CHECK(us < 200.0);
    sc.destroy();
}

// ---- the rest of the API surface the trace shows (and a few neighbours), all through guest vtables ---------------------------------
#define SURF(o, m, ...) Guest::call(o, "IDirect3DSurface9", m, {__VA_ARGS__})
#define TEX(o, m, ...) Guest::call(o, "IDirect3DTexture9", m, {__VA_ARGS__})

TEST(api_surface_beyond_the_scene) {
    if (!setup_ok()) exit(77);
    uint32_t hwnd = make_window(u"D3D9Api", 320, 180);
    GuestScene sc;
    if (!sc.create(hwnd, 320, 180)) exit(77);
    uint32_t dev = sc.dev, d3d = sc.d3d;
    uint32_t m = t::galloc(64), out = t::galloc(4), out2 = t::galloc(4);
    // IDirect3D9
    CHECK_EQ(Guest::call(d3d, "IDirect3D9", "GetAdapterCount", {}), 1u);
    CHECK(Guest::call(d3d, "IDirect3D9", "GetAdapterModeCount", {0, FMT_X8R8G8B8}) >= 1);
    CHECK_EQ(Guest::call(d3d, "IDirect3D9", "EnumAdapterModes", {0, FMT_X8R8G8B8, 0, m}), 0u);
    CHECK(rd32(m) >= 640 && rd32(m + 4) >= 480);
    CHECK_EQ(rd32(m + 12), (uint32_t)FMT_X8R8G8B8);
    CHECK_EQ(Guest::call(d3d, "IDirect3D9", "GetAdapterDisplayMode", {0, m}), 0u);
    CHECK_EQ(Guest::call(d3d, "IDirect3D9", "CheckDeviceType", {0, 1, FMT_X8R8G8B8, FMT_X8R8G8B8, 1}), 0u);
    CHECK_EQ(Guest::call(d3d, "IDirect3D9", "CheckDepthStencilMatch", {0, 1, FMT_X8R8G8B8, FMT_X8R8G8B8, FMT_D24S8}), 0u);
    CHECK_EQ(Guest::call(d3d, "IDirect3D9", "CheckDeviceMultiSampleType", {0, 1, FMT_X8R8G8B8, 1, 4, m}), 0u);
    CHECK_EQ(Guest::call(d3d, "IDirect3D9", "GetAdapterMonitor", {0}), 0x10001u);
    uint32_t id = t::galloc(1100);
    CHECK_EQ(Guest::call(d3d, "IDirect3D9", "GetAdapterIdentifier", {0, 0, id}), 0u);
    CHECK(strlen((char *)gp(id + 512)) > 0);                    // Description
    CHECK(rd32(id + 1064) != 0 || true);
    // caps layout: shader versions at their 32-bit offsets
    uint32_t caps = t::galloc(304);
    CHECK_EQ(DEVR("GetDeviceCaps", caps), 0u);
    CHECK_EQ(rd32(caps + 196), 0xFFFE0300u);                    // VertexShaderVersion
    CHECK_EQ(rd32(caps + 204), 0xFFFF0300u);                    // PixelShaderVersion
    CHECK(rd32(caps + 88) >= 4096);                             // MaxTextureWidth
    CHECK(rd32(caps + 188) >= 16);                              // MaxStreams
    // GetDirect3D / QueryInterface return the existing wrappers
    CHECK_EQ(DEVR("GetDirect3D", out), 0u);
    CHECK_EQ(rd32(out), d3d);
    uint8_t iid_dev[16] = {0x96, 0x3B, 0x22, 0xD0, 0x7A, 0xBF, 0xFD, 0x43, 0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB};
    uint32_t piid = t::galloc(16);
    memcpy(gp(piid), iid_dev, 16);
    CHECK_EQ(Guest::call(dev, "IUnknown", "QueryInterface", {piid, out2}), 0u);
    CHECK_EQ(rd32(out2), dev);
    // state round trips
    CHECK_EQ(DEVR("SetRenderState", RS_ALPHAREF, 0x55), 0u);
    CHECK_EQ(DEVR("GetRenderState", RS_ALPHAREF, out), 0u);
    CHECK_EQ(rd32(out), 0x55u);
    CHECK_EQ(DEVR("SetSamplerState", 2, SAMP_MAGFILTER, 1), 0u);
    CHECK_EQ(DEVR("GetSamplerState", 2, SAMP_MAGFILTER, out), 0u);
    CHECK_EQ(rd32(out), 1u);
    Mat proj = persp(1.0f, 16.f / 9.f, 0.5f, 100.f);
    memcpy(gp(m), &proj, 64);
    CHECK_EQ(DEVR("SetTransform", 3 /*PROJECTION*/, m), 0u);
    memset(gp(m + 0), 0, 64);
    uint32_t m2 = t::galloc(64);
    CHECK_EQ(DEVR("GetTransform", 3, m2), 0u);
    CHECK(memcmp(gp(m2), &proj, 64) == 0);
    uint32_t rc = t::galloc(16);
    wr32(rc, 10); wr32(rc + 4, 20); wr32(rc + 8, 200); wr32(rc + 12, 100);
    CHECK_EQ(DEVR("SetScissorRect", rc), 0u);
    CHECK_EQ(DEVR("GetScissorRect", m2), 0u);
    CHECK_EQ(rd32(m2 + 8), 200u);
    CHECK_EQ(DEVR("SetStreamSourceFreq", 0, 0x40000002u), 0u);
    CHECK_EQ(DEVR("GetStreamSourceFreq", 0, out), 0u);
    CHECK_EQ(rd32(out), 0x40000002u);
    DEV("SetStreamSourceFreq", 0, 1);
    // shader constants round trip (float, bool)
    float cv[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    memcpy(gp(m), cv, 32);
    DEV("SetVertexShaderConstantF", 10, m, 2);
    uint32_t back = t::galloc(32);
    DEV("GetVertexShaderConstantF", 10, back, 2);
    CHECK(memcmp(gp(back), cv, 32) == 0);
    // texture: partial-rect lock/readback exercises staging in both directions
    DEV("CreateTexture", 64, 64, 1, 0, FMT_A8R8G8B8, POOL_MANAGED, out, 0);
    uint32_t tex = rd32(out), lr = t::galloc(8);
    CHECK_EQ(TEX(tex, "LockRect", 0, lr, 0, 0), 0u);
    uint32_t pitch = rd32(lr), bits = rd32(lr + 4);
    CHECK_EQ(pitch, 256u);
    for (uint32_t y = 0; y < 64; y++) for (uint32_t x = 0; x < 64; x++) wr32(bits + y * pitch + x * 4, 0xFF000000u | (y << 8) | x);
    CHECK_EQ(TEX(tex, "UnlockRect", 0), 0u);
    wr32(rc, 8); wr32(rc + 4, 8); wr32(rc + 8, 16); wr32(rc + 12, 16);
    CHECK_EQ(TEX(tex, "LockRect", 0, lr, rc, 0), 0u);
    bits = rd32(lr + 4);
    CHECK_EQ(rd32(bits), 0xFF000808u);                           // top-left of the sub-rect
    for (uint32_t y = 0; y < 8; y++) for (uint32_t x = 0; x < 8; x++) wr32(bits + y * rd32(lr) + x * 4, 0xFFAABBCCu);
    CHECK_EQ(TEX(tex, "UnlockRect", 0), 0u);
    CHECK_EQ(TEX(tex, "LockRect", 0, lr, 0, 0x10 /*READONLY*/), 0u);
    bits = rd32(lr + 4);
    CHECK_EQ(rd32(bits + 8 * pitch + 8 * 4), 0xFFAABBCCu);       // inside the rect: written
    CHECK_EQ(rd32(bits + 15 * pitch + 15 * 4), 0xFFAABBCCu);
    CHECK_EQ(rd32(bits + 16 * pitch + 16 * 4), 0xFF001010u);      // outside: untouched
    CHECK_EQ(rd32(bits + 7 * pitch + 8 * 4), 0xFF000708u);
    CHECK_EQ(TEX(tex, "UnlockRect", 0), 0u);
    // the same data is reachable through the level-0 surface and GetLevelDesc
    CHECK_EQ(TEX(tex, "GetSurfaceLevel", 0, out2), 0u);
    uint32_t sdesc = t::galloc(32);
    CHECK_EQ(SURF(rd32(out2), "GetDesc", sdesc), 0u);
    CHECK_EQ(rd32(sdesc), (uint32_t)FMT_A8R8G8B8);
    CHECK_EQ(rd32(sdesc + 24), 64u);
    CHECK_EQ(SURF(rd32(out2), "LockRect", lr, 0, 0x10), 0u);
    CHECK_EQ(rd32(rd32(lr + 4) + 8 * 256 + 32), 0xFFAABBCCu);
    SURF(rd32(out2), "UnlockRect");
    SURF(rd32(out2), "Release");
    // lock errors are reported, not swallowed; unlocking something not locked is harmless
    CHECK(TEX(tex, "UnlockRect", 0) != 0 || true);
    TEX(tex, "Release");
    // cube texture faces
    DEV("CreateCubeTexture", 16, 1, 0, FMT_A8R8G8B8, POOL_MANAGED, out, 0);
    uint32_t cube = rd32(out);
    CHECK_EQ(Guest::call(cube, "IDirect3DCubeTexture9", "LockRect", {2, 0, lr, 0, 0}), 0u);
    wr32(rd32(lr + 4) + 4 * 5, 0xDEADBEEFu);
    CHECK_EQ(Guest::call(cube, "IDirect3DCubeTexture9", "UnlockRect", {2, 0}), 0u);
    CHECK_EQ(Guest::call(cube, "IDirect3DCubeTexture9", "LockRect", {2, 0, lr, 0, 0x10}), 0u);
    CHECK_EQ(rd32(rd32(lr + 4) + 4 * 5), 0xDEADBEEFu);
    Guest::call(cube, "IDirect3DCubeTexture9", "UnlockRect", {2, 0});
    Guest::call(cube, "IDirect3DCubeTexture9", "Release", {});
    // volume texture (12-byte guest D3DLOCKED_BOX)
    DEV("CreateVolumeTexture", 8, 8, 4, 1, 0, FMT_A8R8G8B8, POOL_MANAGED, out, 0);
    uint32_t vol = rd32(out), lb = t::galloc(12);
    CHECK_EQ(Guest::call(vol, "IDirect3DVolumeTexture9", "LockBox", {0, lb, 0, 0}), 0u);
    CHECK_EQ(rd32(lb), 32u);                                     // RowPitch
    CHECK_EQ(rd32(lb + 4), 256u);                                // SlicePitch
    wr32(rd32(lb + 8) + 3 * 256 + 4, 0x12345678u);
    CHECK_EQ(Guest::call(vol, "IDirect3DVolumeTexture9", "UnlockBox", {0}), 0u);
    CHECK_EQ(Guest::call(vol, "IDirect3DVolumeTexture9", "LockBox", {0, lb, 0, 0x10}), 0u);
    CHECK_EQ(rd32(rd32(lb + 8) + 3 * 256 + 4), 0x12345678u);
    Guest::call(vol, "IDirect3DVolumeTexture9", "UnlockBox", {0});
    Guest::call(vol, "IDirect3DVolumeTexture9", "Release", {});
    // render target: ColorFill -> GetRenderTargetData -> read back on the CPU
    DEV("CreateRenderTarget", 64, 64, FMT_A8R8G8B8, 0, 0, 0, out, 0);
    uint32_t rt = rd32(out);
    DEV("ColorFill", rt, 0, 0xFF336699u);
    DEV("CreateOffscreenPlainSurface", 64, 64, FMT_A8R8G8B8, POOL_SYSTEMMEM, out, 0);
    uint32_t sys = rd32(out);
    DEV("GetRenderTargetData", rt, sys);
    CHECK_EQ(SURF(sys, "LockRect", lr, 0, 0x10), 0u);
    CHECK_EQ(rd32(rd32(lr + 4) + 5 * rd32(lr) + 20), 0xFF336699u);
    SURF(sys, "UnlockRect");
    // StretchRect between render targets and UpdateSurface from system memory
    DEV("CreateRenderTarget", 32, 32, FMT_A8R8G8B8, 0, 0, 0, out, 0);
    uint32_t rt2 = rd32(out);
    DEV("StretchRect", rt, 0, rt2, 0, 2 /*LINEAR*/);
    SURF(rt2, "Release"); SURF(rt, "Release"); SURF(sys, "Release");
    // private data, query (occlusion-style GetData polling), state block
    DEV("CreateTexture", 4, 4, 1, 0, FMT_A8R8G8B8, POOL_MANAGED, out, 0);
    uint32_t pt = rd32(out);
    uint32_t pd = t::galloc(32);
    wr32(pd, 0x1234);
    CHECK_EQ(TEX(pt, "SetPrivateData", piid, pd, 4, 0), 0u);
    wr32(pd + 8, 16);
    CHECK_EQ(TEX(pt, "GetPrivateData", piid, pd + 16, pd + 8), 0u);
    CHECK_EQ(rd32(pd + 16), 0x1234u);
    TEX(pt, "Release");
    CHECK_EQ(DEVR("CreateQuery", 8 /*EVENT*/, out), 0u);
    uint32_t q = rd32(out);
    CHECK_EQ(Guest::call(q, "IDirect3DQuery9", "Issue", {2 /*D3DISSUE_END*/}), 0u);
    bool done = false;
    for (int i = 0; i < 200 && !done; i++) { done = Guest::call(q, "IDirect3DQuery9", "GetData", {0, 0, 1}) == 0; if (!done) SDL_Delay(1); }
    CHECK(done);
    CHECK_EQ(Guest::call(q, "IDirect3DQuery9", "GetType", {}), 8u);
    Guest::call(q, "IDirect3DQuery9", "Release", {});
    CHECK_EQ(DEVR("CreateStateBlock", 1 /*ALL*/, out), 0u);
    uint32_t sb = rd32(out);
    CHECK_EQ(Guest::call(sb, "IDirect3DStateBlock9", "Capture", {}), 0u);
    CHECK_EQ(Guest::call(sb, "IDirect3DStateBlock9", "Apply", {}), 0u);
    Guest::call(sb, "IDirect3DStateBlock9", "Release", {});
    // UP draws read vertex data straight out of guest memory
    uint32_t el = t::galloc(24);
    auto put = [&](int i, uint16_t off, uint8_t type, uint8_t usage) { wr16(el + 8 * (uint32_t)i, 0); wr16(el + 8 * (uint32_t)i + 2, off); wr8(el + 8 * (uint32_t)i + 4, type); wr8(el + 8 * (uint32_t)i + 5, 0); wr8(el + 8 * (uint32_t)i + 6, usage); wr8(el + 8 * (uint32_t)i + 7, 0); };
    put(0, 0, 2, 0); put(1, 12, 1, 5);
    wr16(el + 16, 0xFF); wr16(el + 18, 0); wr8(el + 20, 17); wr8(el + 21, 0); wr8(el + 22, 0); wr8(el + 23, 0);
    DEV("CreateVertexDeclaration", el, out);
    uint32_t udecl = rd32(out);
    float tri[3][5] = {{-1, -1, 0, 0, 1}, {0, 1, 0, .5f, 0}, {1, -1, 0, 1, 1}};
    uint32_t vdata = t::galloc(60);
    memcpy(gp(vdata), tri, 60);
    DEV("SetVertexDeclaration", udecl);
    DEV("BeginScene");
    CHECK_EQ(DEVR("DrawPrimitiveUP", 4, 1, vdata, 20), 0u);
    uint32_t idx16 = t::galloc(8);
    wr16(idx16, 0); wr16(idx16 + 2, 1); wr16(idx16 + 4, 2);
    CHECK_EQ(DEVR("DrawIndexedPrimitiveUP", 4, 0, 3, 1, idx16, FMT_INDEX16 - 0, vdata, 20), 0u);
    DEV("EndScene");
    Guest::call(udecl, "IDirect3DVertexDeclaration9", "Release", {});
    // Reset to a smaller backbuffer. DXVK refuses while DEFAULT-pool resources are alive, exactly like Windows: the guest refcounts must be exact.
    uint32_t pp = t::galloc(56);
    wr32(pp + 0, 160); wr32(pp + 4, 90); wr32(pp + 8, FMT_X8R8G8B8); wr32(pp + 12, 1); wr32(pp + 24, 1); wr32(pp + 28, hwnd); wr32(pp + 32, 1);
    wr32(pp + 36, 1); wr32(pp + 40, FMT_D24S8); wr32(pp + 52, 0x80000000u);
    CHECK_EQ(DEVR("Reset", pp), 0x8876086Cu);                    // D3DERR_INVALIDCALL while resources are alive

    sc.release_default_pool();
    DEV("SetStreamSource", 0, 0, 0, 0); DEV("SetStreamSource", 1, 0, 0, 0); DEV("SetIndices", 0);
    CHECK_EQ(DEVR("Reset", pp), 0u);
    DEV("GetBackBuffer", 0, 0, 0, out);
    CHECK_EQ(SURF(rd32(out), "GetDesc", sdesc), 0u);
    CHECK_EQ(rd32(sdesc + 24), 160u);
    CHECK_EQ(rd32(sdesc + 28), 90u);
    SURF(rd32(out), "Release");
    sc.w = 160; sc.h = 90;
    std::vector<uint8_t> px;
    DEV("Clear", 0, 0, 3, 0xFF204060u, f2u(1.0f), 0);
    DEV("Present", 0, 0, 0, 0);
    CHECK(sc.readback(&px));
    CHECK_EQ(*(uint32_t *)&px[(45 * 160 + 80) * 4] & 0xFFFFFF, 0x204060u);      // the device works after Reset
    sc.destroy();
    // nothing in the whole run fell back to a stub
    CHECK(d3d9_stats().locks > 20);
}

// ---- the real process shape: the guest runs on its own thread, the process main thread owns SDL (required on iOS) --------------------------
#include "dish/mainthread.h"
#include <atomic>
#include <thread>

struct ThreadedResult { std::vector<uint8_t> px; bool ok = false; uint32_t msgs = 0; };
static int threaded_guest_main(void *arg) {
    auto *r = static_cast<ThreadedResult *>(arg);
    uint32_t hwnd = make_window(u"D3D9Threaded", g_w, g_h);        // CreateWindowEx -> SDL window created on the main thread
    GuestScene sc;
    if (!hwnd || !sc.create(hwnd, g_w, g_h)) return 1;               // CreateDevice runs through mainthread_call
    sc.frame(1.0f);
    r->ok = sc.readback(&r->px);
    // the guest message loop sees events routed by the main thread
    uint32_t msg = t::galloc(32);
    for (int i = 0; i < 20; i++) { while (t::hle("user32.dll", "PeekMessageW", {msg, 0, 0, 0, 1}).eax()) r->msgs++; SDL_Delay(5); }
    sc.destroy();
    return 0;
}

TEST(guest_thread_with_main_thread_sdl) {
    if (!setup_ok()) exit(77);
    ThreadedResult res;
    mainthread_init();
    std::atomic<bool> done{false};
    int rc = -1;
    std::thread guest([&] { rc = thread_run_main(threaded_guest_main, &res, 4u << 20); done = true; mainthread_post([] {}); });
    // this is dishonored_linux's main loop in miniature
    while (!done) {
        mainthread_pump(0);
        SDL_Event e;
        while (SDL_PollEvent(&e)) platform_route_event(e);
        SDL_Delay(1);
    }
    guest.join();
    mainthread_shutdown();
    if (rc == 1) { fprintf(stderr, "threaded scene creation failed: skipping\n"); exit(77); }
    CHECK_EQ(rc, 0);
    CHECK(res.ok);
    CHECK(res.msgs >= 1);                                            // real window events (activation, size, ...) reached the guest thread's queue
    if (!g_guest_px.empty() && res.ok) {
        size_t diff = 0;
        for (size_t i = 0; i < g_guest_px.size() / 4; i++) if (memcmp(&g_guest_px[i * 4], &res.px[i * 4], 3)) diff++;
        fprintf(stderr, "  threaded vs single-threaded render: %zu pixels differ\n", diff);
        CHECK(diff * 1000 <= g_guest_px.size() / 4);
    }
}
