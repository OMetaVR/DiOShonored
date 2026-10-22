// The fake guest D3D9 client shared by test_d3d9 (Linux, DXVK-native) and the on-device acceptance app ios/d3d9test (Metal backend):
// re-expresses tools/spikes/d3d9_test's scene using ONLY guest vtable calls, the way lifted code makes them (guest_call through thunk
// addresses read out of guest memory). Needs testing.h's runtime helpers; include it in exactly one TU per executable.
#pragma once
#include "testing.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "shaders_gen.h"

namespace dish { uint32_t d3d9_slot(const char *iface, const char *method); }

using namespace dish;

// ---- D3D9 constants (the guest client has no headers) ------------------------------------------------------------------------
enum {
    FMT_X8R8G8B8 = 22, FMT_A8R8G8B8 = 21, FMT_D24S8 = 75, FMT_DXT1 = 0x31545844, FMT_DXT5 = 0x35545844, FMT_INDEX16 = 101,
    POOL_DEFAULT = 0, POOL_MANAGED = 1, POOL_SYSTEMMEM = 2, USAGE_RT = 1, USAGE_WRITEONLY = 8,
    RS_ZENABLE = 7, RS_ALPHAREF = 24, RS_ZWRITEENABLE = 14, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHABLENDENABLE = 27, RS_BLENDOP = 171,
    SAMP_ADDRESSU = 1, SAMP_ADDRESSV = 2, SAMP_MAGFILTER = 5, SAMP_MINFILTER = 6, SAMP_MIPFILTER = 7,
};

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

struct Guest {
    static std::map<std::string, uint32_t> &cache() { static std::map<std::string, uint32_t> c; return c; }
    // obj->Method(args...) the way lifted code does it: [[obj] + 4*slot], this pushed first
    static uint32_t call(uint32_t obj, const char *iface, const char *method, std::initializer_list<uint32_t> args, bool expect_ok = false) {
        std::string key = std::string(iface) + "::" + method;
        auto &c = cache();
        auto it = c.find(key);
        if (it == c.end()) it = c.emplace(key, d3d9_slot(iface, method)).first;
        if (it->second == 0xFFFFFFFFu) { fprintf(stderr, "no such method %s\n", key.c_str()); abort(); }
        uint32_t fn = rd32(rd32(obj) + 4 * it->second);
        std::vector<uint32_t> a{obj};
        a.insert(a.end(), args.begin(), args.end());
        uint32_t sp = t::stack_top() & ~15u;
        sp -= 4 * (uint32_t)a.size();
        for (size_t i = 0; i < a.size(); i++) wr32(sp + 4 * (uint32_t)i, a[i]);
        sp -= 4;
        wr32(sp, kReturnSentinel);
        GuestRet r = guest_call(fn, sp, 0, 0, 0);
        if (guest_ret_pop(r) != 4 * a.size()) { fprintf(stderr, "%s popped %u, expected %zu\n", key.c_str(), guest_ret_pop(r), 4 * a.size()); abort(); }
        if (expect_ok && (int32_t)guest_ret_eax(r) < 0) { fprintf(stderr, "%s failed: %08x\n", key.c_str(), guest_ret_eax(r)); abort(); }
        return guest_ret_eax(r);
    }
};
#define DEV(m, ...) Guest::call(dev, "IDirect3DDevice9", m, {__VA_ARGS__}, true)
#define DEVR(m, ...) Guest::call(dev, "IDirect3DDevice9", m, {__VA_ARGS__})

// ---- tiny DXT compressor (same as the spike's) ---------------------------------------------------------------------------------
static uint16_t to565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
static void from565(uint16_t c, int *r, int *g, int *b) { *r = ((c >> 11) & 31) * 255 / 31; *g = ((c >> 5) & 63) * 255 / 63; *b = (c & 31) * 255 / 31; }
static void encode_color_block(const uint8_t *px, uint8_t *out) {
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) for (int c = 0; c < 3; c++) { int v = px[i * 4 + c]; if (v < mn[c]) mn[c] = v; if (v > mx[c]) mx[c] = v; }
    uint16_t c0 = to565(mx[0], mx[1], mx[2]), c1 = to565(mn[0], mn[1], mn[2]);
    if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    if (c0 == c1) { if (c0 < 0xFFFF) c0++; else c1--; }
    int p[4][3];
    from565(c0, &p[0][0], &p[0][1], &p[0][2]); from565(c1, &p[1][0], &p[1][1], &p[1][2]);
    for (int c = 0; c < 3; c++) { p[2][c] = (2 * p[0][c] + p[1][c]) / 3; p[3][c] = (p[0][c] + 2 * p[1][c]) / 3; }
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 4; k++) { int d = 0; for (int c = 0; c < 3; c++) { int e = px[i * 4 + c] - p[k][c]; d += e * e; } if (d < bd) { bd = d; best = k; } }
        idx |= (uint32_t)best << (2 * i);
    }
    out[0] = c0 & 255; out[1] = c0 >> 8; out[2] = c1 & 255; out[3] = c1 >> 8;
    memcpy(out + 4, &idx, 4);
}
static void encode_alpha_block(const uint8_t *px, uint8_t *out) {
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; i++) { int a = px[i * 4 + 3]; if (a < mn) mn = a; if (a > mx) mx = a; }
    int a0 = mx, a1 = mn;
    if (a0 == a1) { if (a0 < 255) a0++; else a1--; }
    int tab[8]; tab[0] = a0; tab[1] = a1;
    for (int k = 1; k < 7; k++) tab[1 + k] = ((7 - k) * a0 + k * a1) / 7;
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) { int best = 0, bd = 1 << 30; for (int k = 0; k < 8; k++) { int d = abs(px[i * 4 + 3] - tab[k]); if (d < bd) { bd = d; best = k; } } bits |= (uint64_t)best << (3 * i); }
    out[0] = (uint8_t)a0; out[1] = (uint8_t)a1;
    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(bits >> (8 * i));
}
typedef void (*PixelFn)(int x, int y, int w, int h, uint8_t *rgba);
static void pat_dxt5(int x, int y, int w, int h, uint8_t *o) {
    float u = (x + .5f) / w, v = (y + .5f) / h;
    int chk = ((x * 8 / w) + (y * 8 / h)) & 1;
    float dx = u - .5f, dy = v - .5f, d = sqrtf(dx * dx + dy * dy);
    o[0] = (uint8_t)(chk ? 250 : 40 + 160 * u); o[1] = (uint8_t)(chk ? 220 : 80 + 120 * v); o[2] = (uint8_t)(chk ? 40 : 200);
    float a = 1.f - (d - .30f) / .12f; a = a < 0 ? 0 : a > 1 ? 1 : a;
    o[3] = (uint8_t)(a * 255);
}
static void pat_dxt1(int x, int y, int w, int h, uint8_t *o) {
    int line = ((x % (w / 8)) < 2) || ((y % (h / 8)) < 2);
    o[0] = (uint8_t)(line ? 200 : 20 + 40 * x / w); o[1] = (uint8_t)(line ? 200 : 30 + 50 * y / h); o[2] = (uint8_t)(line ? 220 : 90); o[3] = 255;
}

// ---- matrices (row-vector, as D3DX) ---------------------------------------------------------------------------------------------
struct Mat { float m[4][4]; };
static Mat ident() { Mat r = {}; for (int i = 0; i < 4; i++) r.m[i][i] = 1; return r; }
static Mat mul(const Mat &a, const Mat &b) { Mat r = {}; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j]; return r; }
static Mat rotZ(float a) { Mat r = ident(); r.m[0][0] = cosf(a); r.m[0][1] = sinf(a); r.m[1][0] = -sinf(a); r.m[1][1] = cosf(a); return r; }
static Mat rotY(float a) { Mat r = ident(); r.m[0][0] = cosf(a); r.m[0][2] = -sinf(a); r.m[2][0] = sinf(a); r.m[2][2] = cosf(a); return r; }
static Mat trans(float x, float y, float z) { Mat r = ident(); r.m[3][0] = x; r.m[3][1] = y; r.m[3][2] = z; return r; }
static Mat scale(float x, float y, float z) { Mat r = ident(); r.m[0][0] = x; r.m[1][1] = y; r.m[2][2] = z; return r; }
static Mat persp(float fovy, float aspect, float zn, float zf) { float yS = 1.f / tanf(fovy / 2), xS = yS / aspect; Mat r = {}; r.m[0][0] = xS; r.m[1][1] = yS; r.m[2][2] = zf / (zf - zn); r.m[2][3] = 1; r.m[3][2] = -zn * zf / (zf - zn); return r; }
static Mat transpose(const Mat &a) { Mat r; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = a.m[j][i]; return r; }

// ---- the guest-side scene ------------------------------------------------------------------------------------------------------
struct GuestScene {
    uint32_t d3d = 0, dev = 0, decl = 0, vs = 0, ps = 0, psRtt = 0, vb0 = 0, vb1 = 0, ib = 0, texDxt5 = 0, texDxt1 = 0, texRtt = 0, rttSurf = 0, rttDepth = 0;
    int w = 0, h = 0;
    double last_submit_ms = 0;   // guest-side time of the last frame()/stress_frame() up to (not including) Present
    uint32_t scratch = 0;   // guest scratch memory for argument structs

    uint32_t S(uint32_t off) const { return scratch + off; }

    bool create(uint32_t hwnd, int width, int height) {
        w = width; h = height;
        scratch = t::galloc(4096);
        d3d = t::hle("d3d9.dll", "Direct3DCreate9", {32}).eax();
        if (!d3d) return false;
        // adapter checks like UE3's D3D9Drv startup
        Guest::call(d3d, "IDirect3D9", "GetAdapterIdentifier", {0, 0, S(0)}, true);
        if (Guest::call(d3d, "IDirect3D9", "GetAdapterCount", {}) < 1) return false;
        uint32_t caps = t::galloc(304);
        Guest::call(d3d, "IDirect3D9", "GetDeviceCaps", {0, 1, caps}, true);
        for (uint32_t f : {FMT_DXT1, FMT_DXT5}) {
            if ((int32_t)Guest::call(d3d, "IDirect3D9", "CheckDeviceFormat", {0, 1, FMT_X8R8G8B8, 0, 3 /*TEXTURE*/, f}) < 0) { fprintf(stderr, "no DXT\n"); return false; }
        }
        // presentation parameters (32-bit layout)
        uint32_t pp = t::galloc(56);
        wr32(pp + 0, (uint32_t)w); wr32(pp + 4, (uint32_t)h); wr32(pp + 8, FMT_X8R8G8B8); wr32(pp + 12, 1); wr32(pp + 24, 1 /*DISCARD*/);
        wr32(pp + 28, hwnd); wr32(pp + 32, 1 /*Windowed*/); wr32(pp + 36, 1); wr32(pp + 40, FMT_D24S8); wr32(pp + 52, 0x80000000u /*IMMEDIATE*/);
        uint32_t pdev = t::galloc(4);
        uint32_t hr = Guest::call(d3d, "IDirect3D9", "CreateDevice", {0, 1, hwnd, 0x40, pp, pdev});
        if ((int32_t)hr < 0) { fprintf(stderr, "CreateDevice failed %08x\n", hr); return false; }
        dev = rd32(pdev);
        // vertex declaration over two streams
        uint32_t el = t::galloc(32);
        auto put_el = [&](int i, uint16_t stream, uint16_t off, uint8_t type, uint8_t method, uint8_t usage, uint8_t idx) {
            wr16(el + 8 * (uint32_t)i, stream); wr16(el + 8 * (uint32_t)i + 2, off); wr8(el + 8 * (uint32_t)i + 4, type); wr8(el + 8 * (uint32_t)i + 5, method);
            wr8(el + 8 * (uint32_t)i + 6, usage); wr8(el + 8 * (uint32_t)i + 7, idx);
        };
        put_el(0, 0, 0, 2 /*FLOAT3*/, 0, 0 /*POSITION*/, 0);
        put_el(1, 0, 12, 1 /*FLOAT2*/, 0, 5 /*TEXCOORD*/, 0);
        put_el(2, 1, 0, 4 /*D3DCOLOR*/, 0, 10 /*COLOR*/, 0);
        put_el(3, 0xFF, 0, 17 /*UNUSED*/, 0, 0, 0);
        uint32_t out = t::galloc(4);
        DEV("CreateVertexDeclaration", el, out); decl = rd32(out);
        uint32_t code = t::galloc(1024);
        memcpy(gp(code), k_scene_vs, sizeof k_scene_vs); DEV("CreateVertexShader", code, out); vs = rd32(out);
        memcpy(gp(code), k_scene_ps, sizeof k_scene_ps); DEV("CreatePixelShader", code, out); ps = rd32(out);
        memcpy(gp(code), k_rtt_ps, sizeof k_rtt_ps); DEV("CreatePixelShader", code, out); psRtt = rd32(out);
        // geometry
        float v0[4][5] = {{-1, -1, 0, 0, 1}, {-1, 1, 0, 0, 0}, {1, 1, 0, 1, 0}, {1, -1, 0, 1, 1}};
        uint32_t v1[4] = {0xFFFFFFFFu, 0xFFFFE0E0u, 0xFFE0E0FFu, 0xFFE0FFE0u};
        uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
        uint32_t pp_data = t::galloc(4);
        DEV("CreateVertexBuffer", sizeof v0, USAGE_WRITEONLY, 0, POOL_DEFAULT, out, 0); vb0 = rd32(out);
        Guest::call(vb0, "IDirect3DVertexBuffer9", "Lock", {0, sizeof v0, pp_data, 0}, true); memcpy(gp(rd32(pp_data)), v0, sizeof v0);
        Guest::call(vb0, "IDirect3DVertexBuffer9", "Unlock", {}, true);
        DEV("CreateVertexBuffer", sizeof v1, USAGE_WRITEONLY, 0, POOL_DEFAULT, out, 0); vb1 = rd32(out);
        Guest::call(vb1, "IDirect3DVertexBuffer9", "Lock", {0, sizeof v1, pp_data, 0}, true); memcpy(gp(rd32(pp_data)), v1, sizeof v1);
        Guest::call(vb1, "IDirect3DVertexBuffer9", "Unlock", {}, true);
        DEV("CreateIndexBuffer", sizeof idx, USAGE_WRITEONLY, FMT_INDEX16, POOL_DEFAULT, out, 0); ib = rd32(out);
        Guest::call(ib, "IDirect3DIndexBuffer9", "Lock", {0, sizeof idx, pp_data, 0}, true); memcpy(gp(rd32(pp_data)), idx, sizeof idx);
        Guest::call(ib, "IDirect3DIndexBuffer9", "Unlock", {}, true);
        make_dxt(FMT_DXT5, 256, pat_dxt5, &texDxt5);
        make_dxt(FMT_DXT1, 256, pat_dxt1, &texDxt1);
        DEV("CreateTexture", 256, 256, 1, USAGE_RT, FMT_A8R8G8B8, POOL_DEFAULT, out, 0); texRtt = rd32(out);
        Guest::call(texRtt, "IDirect3DTexture9", "GetSurfaceLevel", {0, out}, true); rttSurf = rd32(out);
        DEV("CreateDepthStencilSurface", 256, 256, FMT_D24S8, 0, 0, 1, out, 0); rttDepth = rd32(out);
        return true;
    }

    void make_dxt(uint32_t fmt, int size, PixelFn fn, uint32_t *tex_out) {
        uint32_t out = t::galloc(4);
        DEV("CreateTexture", (uint32_t)size, (uint32_t)size, 0, 0, fmt, POOL_MANAGED, out, 0);
        uint32_t tex = rd32(out);
        std::vector<uint8_t> cur((size_t)size * size * 4), nxt;
        for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) fn(x, y, size, size, &cur[((size_t)y * size + x) * 4]);
        uint32_t lvls = Guest::call(tex, "IDirect3DTexture9", "GetLevelCount", {});
        int dim = size;
        const int blockBytes = fmt == FMT_DXT1 ? 8 : 16;
        uint32_t lr = t::galloc(8);
        for (uint32_t l = 0; l < lvls; l++) {
            Guest::call(tex, "IDirect3DTexture9", "LockRect", {l, lr, 0, 0}, true);
            uint32_t pitch = rd32(lr), bits = rd32(lr + 4);
            int bw = (dim + 3) / 4, bh = (dim + 3) / 4;
            for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
                uint8_t px[64];
                for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
                    int sx = bx * 4 + i, sy = by * 4 + j; if (sx >= dim) sx = dim - 1; if (sy >= dim) sy = dim - 1;
                    memcpy(px + (j * 4 + i) * 4, &cur[((size_t)sy * dim + sx) * 4], 4);
                }
                uint8_t *dst = gp(bits) + (size_t)by * pitch + (size_t)bx * blockBytes;
                if (fmt == FMT_DXT5) { encode_alpha_block(px, dst); encode_color_block(px, dst + 8); } else encode_color_block(px, dst);
            }
            Guest::call(tex, "IDirect3DTexture9", "UnlockRect", {l}, true);
            if (dim > 1) {
                int nd = dim / 2; nxt.assign((size_t)nd * nd * 4, 0);
                for (int y = 0; y < nd; y++) for (int x = 0; x < nd; x++) for (int c = 0; c < 4; c++)
                    nxt[((size_t)y * nd + x) * 4 + c] = (uint8_t)((cur[((size_t)(2 * y) * dim + 2 * x) * 4 + c] + cur[((size_t)(2 * y) * dim + 2 * x + 1) * 4 + c] +
                                                                   cur[((size_t)(2 * y + 1) * dim + 2 * x) * 4 + c] + cur[((size_t)(2 * y + 1) * dim + 2 * x + 1) * 4 + c] + 2) / 4);
                cur.swap(nxt); dim = nd;
            }
        }
        *tex_out = tex;
    }

    void draw_quad() {
        DEV("SetStreamSource", 0, vb0, 0, 20);
        DEV("SetStreamSource", 1, vb1, 0, 4);
        DEV("SetIndices", ib);
        DEV("DrawIndexedPrimitive", 4 /*TRIANGLELIST*/, 0, 0, 4, 0, 2);
    }
    void set_wvp(const Mat &wvp) {
        Mat tm = transpose(wvp);
        memcpy(gp(S(256)), &tm.m[0][0], 64);
        DEV("SetVertexShaderConstantF", 0, S(256), 4);
    }
    void vec4_ps(uint32_t reg, float a, float b, float c, float d) {
        float v[4] = {a, b, c, d};
        memcpy(gp(S(512)), v, 16);
        DEV("SetPixelShaderConstantF", reg, S(512), 1);
    }

    void present(std::chrono::steady_clock::time_point t0) {
        last_submit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        DEV("Present", 0, 0, 0, 0);
    }

    void frame(float t_) {
        auto t0 = std::chrono::steady_clock::now();
        DEV("TestCooperativeLevel");
        DEV("SetVertexDeclaration", decl);
        DEV("SetVertexShader", vs);
        for (uint32_t i = 0; i < 2; i++) {
            DEV("SetSamplerState", i, SAMP_MINFILTER, 2); DEV("SetSamplerState", i, SAMP_MAGFILTER, 2); DEV("SetSamplerState", i, SAMP_MIPFILTER, 2);
            DEV("SetSamplerState", i, SAMP_ADDRESSU, 1); DEV("SetSamplerState", i, SAMP_ADDRESSV, 1);
        }
        float vsp[4] = {t_, 0.25f * t_, 0, 0};
        memcpy(gp(S(600)), vsp, 16);
        DEV("SetVertexShaderConstantF", 4, S(600), 1);
        // pass 1: render to texture
        uint32_t pRT = S(700), pDS = S(704), vp = S(720), vp_old = S(760);
        DEV("GetRenderTarget", 0, pRT);
        DEV("GetDepthStencilSurface", pDS);
        DEV("GetViewport", vp_old);
        DEV("SetRenderTarget", 0, rttSurf);
        DEV("SetDepthStencilSurface", rttDepth);
        wr32(vp, 0); wr32(vp + 4, 0); wr32(vp + 8, 256); wr32(vp + 12, 256); wr32(vp + 16, f2u(0.f)); wr32(vp + 20, f2u(1.f));
        DEV("SetViewport", vp);
        DEV("Clear", 0, 0, 7, 0x00000000, f2u(1.0f), 0);
        DEV("BeginScene");
        DEV("SetRenderState", RS_ZENABLE, 0); DEV("SetRenderState", RS_CULLMODE, 1); DEV("SetRenderState", RS_ALPHABLENDENABLE, 0);
        set_wvp(ident());
        DEV("SetPixelShader", psRtt);
        vec4_ps(0, t_, 0, 0, 0);
        DEV("SetTexture", 0, 0);
        draw_quad();
        DEV("EndScene");
        DEV("SetRenderTarget", 0, rd32(pRT));
        DEV("SetDepthStencilSurface", rd32(pDS));
        DEV("SetViewport", vp_old);
        Guest::call(rd32(pRT), "IDirect3DSurface9", "Release", {});
        Guest::call(rd32(pDS), "IDirect3DSurface9", "Release", {});
        // pass 2: backbuffer
        DEV("Clear", 0, 0, 7, 0xFF101830, f2u(1.0f), 0);
        DEV("BeginScene");
        DEV("SetPixelShader", ps);
        float aspect = (float)w / (float)h;
        Mat proj = persp(1.0f, aspect, 0.1f, 50.f);
        DEV("SetRenderState", RS_ZENABLE, 1); DEV("SetRenderState", RS_ZFUNC, 4); DEV("SetRenderState", RS_ZWRITEENABLE, 1);
        DEV("SetRenderState", RS_ALPHABLENDENABLE, 0);
        DEV("SetTexture", 0, texDxt1);
        vec4_ps(0, 1, 1, 1, 1); vec4_ps(1, 0, 0, 0, 0);
        wr32(S(800), 0);
        DEV("SetPixelShaderConstantB", 0, S(800), 1);
        set_wvp(mul(mul(scale(3.5f * aspect, 3.5f, 1), trans(0, 0, 4.5f)), proj));
        draw_quad();
        DEV("SetRenderState", RS_ALPHABLENDENABLE, 1);
        DEV("SetRenderState", RS_SRCBLEND, 5); DEV("SetRenderState", RS_DESTBLEND, 6); DEV("SetRenderState", RS_BLENDOP, 1);
        DEV("SetRenderState", RS_ZWRITEENABLE, 0);
        DEV("SetTexture", 0, texDxt5); DEV("SetTexture", 1, texRtt);
        vec4_ps(1, 1, 0, 0, 0); wr32(S(800), 1); DEV("SetPixelShaderConstantB", 0, S(800), 1);
        Mat world = mul(mul(rotZ(t_ * 0.6f), rotY(sinf(t_) * 0.9f)), trans(0, 0, 3.0f));
        set_wvp(mul(world, proj));
        draw_quad();
        vec4_ps(1, 0, 0, 0, 0); wr32(S(800), 0); DEV("SetPixelShaderConstantB", 0, S(800), 1);
        DEV("SetTexture", 0, texRtt);
        set_wvp(mul(mul(scale(0.55f, 0.55f, 1), trans(-1.9f * aspect / 1.6f, -1.25f, 3.0f)), proj));
        draw_quad();
        DEV("EndScene");
        present(t0);
    }

    // UE3-like submission pressure: `draws` small quads in a grid, each with its own WVP (SetVertexShaderConstantF), a PS constant and
    // a texture switch every other draw, alternating opaque/blended state. Measures the per-draw cost of the thunk + backend path.
    // `marks` (optional) receives the elapsed ms at the first draw and after every 250 draws, to locate stalls inside a frame
    void stress_frame(float t_, int draws, std::vector<double> *marks = nullptr) {
        auto t0 = std::chrono::steady_clock::now();
        auto mark = [&] { if (marks) marks->push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); };
        DEV("TestCooperativeLevel");
        DEV("Clear", 0, 0, 7, 0xFF181010, f2u(1.0f), 0);
        DEV("BeginScene");
        DEV("SetVertexDeclaration", decl);
        DEV("SetVertexShader", vs);
        DEV("SetPixelShader", ps);
        DEV("SetRenderState", RS_ZENABLE, 1); DEV("SetRenderState", RS_ZFUNC, 4); DEV("SetRenderState", RS_ZWRITEENABLE, 1);
        DEV("SetRenderState", RS_CULLMODE, 1);
        DEV("SetRenderState", RS_SRCBLEND, 5); DEV("SetRenderState", RS_DESTBLEND, 6); DEV("SetRenderState", RS_BLENDOP, 1);
        wr32(S(800), 0);
        DEV("SetPixelShaderConstantB", 0, S(800), 1);
        vec4_ps(1, 0, 0, 0, 0);
        DEV("SetStreamSource", 0, vb0, 0, 20);
        DEV("SetStreamSource", 1, vb1, 0, 4);
        DEV("SetIndices", ib);
        float aspect = (float)w / (float)h;
        Mat proj = persp(1.0f, aspect, 0.1f, 50.f);
        int cols = (int)ceilf(sqrtf((float)draws * aspect)), rows = (draws + cols - 1) / cols;
        float cw = 2.0f * 2.2f * aspect / (float)cols, ch = 2.0f * 2.2f / (float)rows;
        const uint32_t texs[3] = {texDxt1, texDxt5, texRtt};
        for (int i = 0; i < draws; i++) {
            if (i % 250 == 0) mark();
            int cx = i % cols, cy = i / cols;
            float x = -2.2f * aspect + cw * (cx + 0.5f), y = 2.2f - ch * (cy + 0.5f);
            float s = 0.45f * (cw < ch ? cw : ch) * (1.0f + 0.2f * sinf(t_ * 2.0f + i * 0.37f));
            set_wvp(mul(mul(mul(scale(s, s, 1), rotZ(t_ + i * 0.05f)), trans(x, y, 3.0f)), proj));
            vec4_ps(0, 0.6f + 0.4f * sinf(i * 0.11f), 0.6f + 0.4f * sinf(i * 0.07f + 1), 0.6f + 0.4f * sinf(i * 0.05f + 2), 1);
            if ((i & 1) == 0) DEV("SetTexture", 0, texs[(i >> 1) % 3]);
            if ((i & 63) == 0) DEV("SetRenderState", RS_ALPHABLENDENABLE, (uint32_t)((i >> 6) & 1));
            DEV("DrawIndexedPrimitive", 4 /*TRIANGLELIST*/, 0, 0, 4, 0, 2);
        }
        mark();
        DEV("EndScene");
        present(t0);
    }

    // backbuffer readback through GetRenderTargetData into a SYSTEMMEM surface and a guest-side LockRect
    bool readback(std::vector<uint8_t> *bgra) {
        uint32_t out = t::galloc(4), sys = t::galloc(4);
        if ((int32_t)DEVR("GetBackBuffer", 0, 0, 0, out) < 0) return false;
        uint32_t bb = rd32(out);
        if ((int32_t)DEVR("CreateOffscreenPlainSurface", (uint32_t)w, (uint32_t)h, FMT_X8R8G8B8, POOL_SYSTEMMEM, sys, 0) < 0) return false;
        uint32_t ss = rd32(sys);
        if ((int32_t)DEVR("GetRenderTargetData", bb, ss) < 0) return false;
        uint32_t lr = t::galloc(8);
        if ((int32_t)Guest::call(ss, "IDirect3DSurface9", "LockRect", {lr, 0, 0x10 /*READONLY*/}) < 0) return false;
        uint32_t pitch = rd32(lr), bits = rd32(lr + 4);
        bgra->resize((size_t)w * h * 4);
        for (int y = 0; y < h; y++) memcpy(&(*bgra)[(size_t)y * w * 4], gp(bits) + (size_t)y * pitch, (size_t)w * 4);
        Guest::call(ss, "IDirect3DSurface9", "UnlockRect", {}, true);
        Guest::call(ss, "IDirect3DSurface9", "Release", {});
        Guest::call(bb, "IDirect3DSurface9", "Release", {});
        return true;
    }
    // D3D9 rule (enforced by DXVK): Reset needs every D3DPOOL_DEFAULT resource released first
    void release_default_pool() {
        for (uint32_t *o : {&rttSurf, &rttDepth, &texRtt, &vb0, &vb1, &ib})
            if (*o) { Guest::call(*o, "IUnknown", "Release", {}); *o = 0; }
    }
    void destroy() {
        for (uint32_t* o : {&rttSurf, &rttDepth, &texRtt, &texDxt5, &texDxt1, &vb0, &vb1, &ib, &decl, &vs, &ps, &psRtt, &dev, &d3d})
            if (*o) { Guest::call(*o, "IUnknown", "Release", {}); *o = 0; }
    }
};

static uint32_t make_window(const char16_t *cls_name, int w, int h) {
    static uint32_t wndproc_addr = 0x00406000;
    static bool registered = false;
    if (!registered) {
        registered = true;
        t::register_guest_fn(wndproc_addr, [](uint32_t esp, uint32_t, uint32_t, uint32_t) -> GuestRet {
            uint32_t sp = esp - 32;
            wr32(sp, kReturnSentinel); wr32(sp + 4, rd32(esp + 4)); wr32(sp + 8, rd32(esp + 8)); wr32(sp + 12, rd32(esp + 12)); wr32(sp + 16, rd32(esp + 16));
            GuestRet r = guest_call(thunk_addr(hle_bind("user32.dll", "DefWindowProcW", 0, -1)), sp, 0, 0, 0);
            return guest_ret(guest_ret_eax(r), 0, 16);
        });
    }
    uint32_t cls = t::gwstr(cls_name);
    uint32_t wc = t::galloc(48);
    wr32(wc, 48); wr32(wc + 8, wndproc_addr); wr32(wc + 40, cls);
    t::hle("user32.dll", "RegisterClassExW", {wc});
    return t::hle("user32.dll", "CreateWindowExW", {0, cls, t::gwstr(u"d3d9 test"), 0x10CA0000 /*WS_VISIBLE|WS_OVERLAPPED|CAPTION|SYSMENU*/, 50, 50, (uint32_t)w + 16, (uint32_t)h + 39, 0, 0, 0, 0}).eax();
}
