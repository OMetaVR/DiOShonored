#include "d3d9_scene.h"
#include "shaders_gen.h"

#include <d3d9.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exception>
#include <string>
#include <vector>

/* Layout/name mirror of DXVK's exception type (src/util/util_error.h) so the message can be read out.
 * Matching across the dylib boundary relies on type_info name comparison (non-unique RTTI). */
namespace dxvk {
  class DxvkError {
  public:
    const std::string& message() const { return m_message; }
  private:
    std::string m_message;
  };
}

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = NULL; } } while (0)

struct Vtx0 { float x, y, z, u, v; };   /* stream 0: position + uv  */
struct Vtx1 { uint32_t color; };        /* stream 1: D3DCOLOR       */

struct d3d9_scene {
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp = {};
    int w = 0, h = 0;

    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DVertexShader9 *vs = NULL;
    IDirect3DPixelShader9 *ps = NULL, *psRtt = NULL;
    IDirect3DVertexBuffer9 *vb0 = NULL, *vb1 = NULL;
    IDirect3DIndexBuffer9 *ib = NULL;
    IDirect3DTexture9 *texDxt5 = NULL, *texDxt1 = NULL, *texRtt = NULL;
    IDirect3DSurface9 *rttSurf = NULL, *rttDepth = NULL;

    void (*log_cb)(const char *, void *) = NULL;
    void *user = NULL;
    float lastT = 0;
    bool usedDxt = true;
};

static void LOGF(d3d9_scene *s, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (s->log_cb) s->log_cb(buf, s->user); else fprintf(stderr, "%s\n", buf);
}

#define CHK(call) do { HRESULT _hr = (call); if (FAILED(_hr)) { \
    snprintf(err, errsz, "%s failed: 0x%08x (line %d)", #call, (unsigned)_hr, __LINE__); return -1; } } while (0)

/* ---------- tiny DXT compressor (range-fit, good enough for a test pattern) ---------- */
static uint16_t to565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
static void from565(uint16_t c, int *r, int *g, int *b) {
    *r = ((c >> 11) & 31) * 255 / 31; *g = ((c >> 5) & 63) * 255 / 63; *b = (c & 31) * 255 / 31;
}
/* px: 16 RGBA pixels. Writes 8-byte DXT1 colour block (4-colour mode, c0 > c1). */
static void encode_color_block(const uint8_t *px, uint8_t *out) {
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) for (int c = 0; c < 3; c++) {
        int v = px[i * 4 + c]; if (v < mn[c]) mn[c] = v; if (v > mx[c]) mx[c] = v;
    }
    uint16_t c0 = to565(mx[0], mx[1], mx[2]), c1 = to565(mn[0], mn[1], mn[2]);
    if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    if (c0 == c1) { /* flat block: force c0 > c1 so we stay in 4 colour mode */
        if (c0 < 0xFFFF) c0++; else c1--;
    }
    int p[4][3];
    from565(c0, &p[0][0], &p[0][1], &p[0][2]); from565(c1, &p[1][0], &p[1][1], &p[1][2]);
    for (int c = 0; c < 3; c++) { p[2][c] = (2 * p[0][c] + p[1][c]) / 3; p[3][c] = (p[0][c] + 2 * p[1][c]) / 3; }
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 4; k++) {
            int d = 0; for (int c = 0; c < 3; c++) { int e = px[i * 4 + c] - p[k][c]; d += e * e; }
            if (d < bd) { bd = d; best = k; }
        }
        idx |= (uint32_t)best << (2 * i);
    }
    out[0] = c0 & 255; out[1] = c0 >> 8; out[2] = c1 & 255; out[3] = c1 >> 8;
    memcpy(out + 4, &idx, 4);
}
static void encode_alpha_block(const uint8_t *px, uint8_t *out) {
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; i++) { int a = px[i * 4 + 3]; if (a < mn) mn = a; if (a > mx) mx = a; }
    int a0 = mx, a1 = mn;               /* a0 > a1 => 8 interpolated values */
    if (a0 == a1) { if (a0 < 255) a0++; else a1--; }
    int tab[8]; tab[0] = a0; tab[1] = a1;
    for (int k = 1; k < 7; k++) tab[1 + k] = ((7 - k) * a0 + k * a1) / 7;
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 8; k++) { int d = abs(px[i * 4 + 3] - tab[k]); if (d < bd) { bd = d; best = k; } }
        bits |= (uint64_t)best << (3 * i);
    }
    out[0] = (uint8_t)a0; out[1] = (uint8_t)a1;
    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(bits >> (8 * i));
}

typedef void (*PixelFn)(int x, int y, int w, int h, uint8_t *rgba);

static void pat_dxt5(int x, int y, int w, int h, uint8_t *o) {
    float u = (x + .5f) / w, v = (y + .5f) / h;
    int chk = ((x * 8 / w) + (y * 8 / h)) & 1;
    float dx = u - .5f, dy = v - .5f, d = sqrtf(dx * dx + dy * dy);
    o[0] = (uint8_t)(chk ? 250 : 40 + 160 * u);
    o[1] = (uint8_t)(chk ? 220 : 80 + 120 * v);
    o[2] = (uint8_t)(chk ? 40 : 200);
    float a = 1.f - (d - .30f) / .12f; a = a < 0 ? 0 : a > 1 ? 1 : a;   /* soft circular cut-out via alpha */
    o[3] = (uint8_t)(a * 255);
}
static void pat_dxt1(int x, int y, int w, int h, uint8_t *o) {
    int line = ((x % (w / 8)) < 2) || ((y % (h / 8)) < 2);
    o[0] = (uint8_t)(line ? 200 : 20 + 40 * x / w);
    o[1] = (uint8_t)(line ? 200 : 30 + 50 * y / h);
    o[2] = (uint8_t)(line ? 220 : 90);
    o[3] = 255;
}

/* Build full mip chain and compress. */
static int make_dxt_texture(d3d9_scene *s, D3DFORMAT fmt, int size, PixelFn fn, IDirect3DTexture9 **out, char *err, size_t errsz) {
    IDirect3DTexture9 *tex = NULL;
    CHK(s->dev->CreateTexture(size, size, 0, 0, fmt, D3DPOOL_MANAGED, &tex, NULL));
    std::vector<uint8_t> cur(size * size * 4), nxt;
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) fn(x, y, size, size, &cur[(y * size + x) * 4]);
    int lvls = tex->GetLevelCount();
    int dim = size;
    const int blockBytes = (fmt == D3DFMT_DXT1) ? 8 : 16;
    for (int l = 0; l < lvls; l++) {
        D3DLOCKED_RECT lr;
        CHK(tex->LockRect(l, &lr, NULL, 0));
        int bw = (dim + 3) / 4, bh = (dim + 3) / 4;
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
            uint8_t px[64];
            for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
                int sx = bx * 4 + i, sy = by * 4 + j; if (sx >= dim) sx = dim - 1; if (sy >= dim) sy = dim - 1;
                memcpy(px + (j * 4 + i) * 4, &cur[(sy * dim + sx) * 4], 4);
            }
            uint8_t *dst = (uint8_t *)lr.pBits + by * lr.Pitch + bx * blockBytes;
            if (fmt == D3DFMT_DXT5) { encode_alpha_block(px, dst); encode_color_block(px, dst + 8); }
            else encode_color_block(px, dst);
        }
        CHK(tex->UnlockRect(l));
        if (dim > 1) { /* box downsample */
            int nd = dim / 2; nxt.assign(nd * nd * 4, 0);
            for (int y = 0; y < nd; y++) for (int x = 0; x < nd; x++) for (int c = 0; c < 4; c++)
                nxt[(y * nd + x) * 4 + c] = (uint8_t)((cur[((2*y)*dim + 2*x)*4+c] + cur[((2*y)*dim + 2*x+1)*4+c] +
                                                       cur[((2*y+1)*dim + 2*x)*4+c] + cur[((2*y+1)*dim + 2*x+1)*4+c] + 2) / 4);
            cur.swap(nxt); dim = nd;
        }
    }
    *out = tex;
    return 0;
}


/* Uncompressed fallback with a full mip chain (used when the D3D9 layer reports no DXT support). */
static int make_rgba_texture(d3d9_scene *s, int size, PixelFn fn, IDirect3DTexture9 **out, char *err, size_t errsz) {
    IDirect3DTexture9 *tex = NULL;
    CHK(s->dev->CreateTexture(size, size, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL));
    std::vector<uint8_t> cur(size * size * 4), nxt;
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) fn(x, y, size, size, &cur[(y * size + x) * 4]);
    int lvls = tex->GetLevelCount(), dim = size;
    for (int l = 0; l < lvls; l++) {
        D3DLOCKED_RECT lr; CHK(tex->LockRect(l, &lr, NULL, 0));
        for (int y = 0; y < dim; y++) for (int x = 0; x < dim; x++) {
            uint8_t *p = &cur[(y * dim + x) * 4]; uint8_t *d = (uint8_t *)lr.pBits + y * lr.Pitch + x * 4;
            d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; d[3] = p[3];   /* RGBA -> BGRA */
        }
        CHK(tex->UnlockRect(l));
        if (dim > 1) { int nd = dim / 2; nxt.assign(nd * nd * 4, 0);
            for (int y = 0; y < nd; y++) for (int x = 0; x < nd; x++) for (int c = 0; c < 4; c++)
                nxt[(y * nd + x) * 4 + c] = (uint8_t)((cur[((2*y)*dim + 2*x)*4+c] + cur[((2*y)*dim + 2*x+1)*4+c] + cur[((2*y+1)*dim + 2*x)*4+c] + cur[((2*y+1)*dim + 2*x+1)*4+c] + 2) / 4);
            cur.swap(nxt); dim = nd; }
    }
    *out = tex; return 0;
}

/* ---------- matrices (row-vector convention, as D3DX) ---------- */
struct Mat { float m[4][4]; };
static Mat ident() { Mat r = {}; for (int i = 0; i < 4; i++) r.m[i][i] = 1; return r; }
static Mat mul(const Mat &a, const Mat &b) {
    Mat r = {}; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j]; return r;
}
static Mat rotZ(float a) { Mat r = ident(); r.m[0][0] = cosf(a); r.m[0][1] = sinf(a); r.m[1][0] = -sinf(a); r.m[1][1] = cosf(a); return r; }
static Mat rotY(float a) { Mat r = ident(); r.m[0][0] = cosf(a); r.m[0][2] = -sinf(a); r.m[2][0] = sinf(a); r.m[2][2] = cosf(a); return r; }
static Mat trans(float x, float y, float z) { Mat r = ident(); r.m[3][0] = x; r.m[3][1] = y; r.m[3][2] = z; return r; }
static Mat scale(float x, float y, float z) { Mat r = ident(); r.m[0][0] = x; r.m[1][1] = y; r.m[2][2] = z; return r; }
static Mat persp(float fovy, float aspect, float zn, float zf) {
    float yS = 1.f / tanf(fovy / 2), xS = yS / aspect; Mat r = {};
    r.m[0][0] = xS; r.m[1][1] = yS; r.m[2][2] = zf / (zf - zn); r.m[2][3] = 1; r.m[3][2] = -zn * zf / (zf - zn); return r;
}
static Mat transpose(const Mat &a) { Mat r; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = a.m[j][i]; return r; }

/* ---------- creation ---------- */
int d3d9_scene_create(const d3d9_scene_desc *d, d3d9_scene **out, char *err, size_t errsz,
                      void (*log_cb)(const char *, void *), void *user) {
    d3d9_scene *s = new d3d9_scene();
    s->log_cb = log_cb; s->user = user; s->w = d->width; s->h = d->height;
    *out = s;
    try { s->d3d = (IDirect3D9 *)d->create9(D3D_SDK_VERSION); }
    catch (const dxvk::DxvkError &e) { snprintf(err, errsz, "Direct3DCreate9 threw DxvkError: %s", e.message().c_str()); return -1; }
    catch (const std::exception &e) { snprintf(err, errsz, "Direct3DCreate9 threw std::exception: %s", e.what()); return -1; }
    catch (...) { snprintf(err, errsz, "Direct3DCreate9 threw an unknown C++ exception; see dxvk_stderr.log / Probe_d3d9.log"); return -1; }
    if (!s->d3d) { snprintf(err, errsz, "Direct3DCreate9 returned NULL"); return -1; }

    D3DADAPTER_IDENTIFIER9 id; s->d3d->GetAdapterIdentifier(0, 0, &id);
    LOGF(s, "adapter: %s | driver: %s", id.Description, id.Driver);
    D3DCAPS9 caps; CHK(s->d3d->GetDeviceCaps(0, D3DDEVTYPE_HAL, &caps));
    LOGF(s, "caps: VS 0x%08x PS 0x%08x maxTex %ux%u maxAniso %u maxVSConstF %u NumSimultaneousRTs %u",
         (unsigned)caps.VertexShaderVersion, (unsigned)caps.PixelShaderVersion, (unsigned)caps.MaxTextureWidth,
         (unsigned)caps.MaxTextureHeight, (unsigned)caps.MaxAnisotropy, (unsigned)caps.MaxVertexShaderConst,
         (unsigned)caps.NumSimultaneousRTs);
    if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0)) {
        snprintf(err, errsz, "SM3.0 not reported by device"); return -1;
    }
    struct { D3DFORMAT f; const char *n; DWORD usage; } fmts[] = {
        { D3DFMT_DXT1, "DXT1", 0 }, { D3DFMT_DXT3, "DXT3", 0 }, { D3DFMT_DXT5, "DXT5", 0 },
        { D3DFMT_A8R8G8B8, "A8R8G8B8 RT", D3DUSAGE_RENDERTARGET }, { D3DFMT_A16B16G16R16F, "A16B16G16R16F RT", D3DUSAGE_RENDERTARGET },
        { D3DFMT_G16R16F, "G16R16F RT", D3DUSAGE_RENDERTARGET }, { D3DFMT_R32F, "R32F RT", D3DUSAGE_RENDERTARGET },
        { D3DFMT_D24S8, "D24S8 DS", D3DUSAGE_DEPTHSTENCIL }, { D3DFMT_D24X8, "D24X8 DS", D3DUSAGE_DEPTHSTENCIL },
        { D3DFMT_L8, "L8", 0 }, { D3DFMT_A8, "A8", 0 }, { D3DFMT_V8U8, "V8U8", 0 }, { D3DFMT_A8L8, "A8L8", 0 },
    };
    for (auto &f : fmts) {
        HRESULT hr = s->d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, f.usage, D3DRTYPE_TEXTURE, f.f);
        if (f.usage == D3DUSAGE_DEPTHSTENCIL) hr = s->d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, f.usage, D3DRTYPE_SURFACE, f.f);
        LOGF(s, "format %-18s: %s", f.n, SUCCEEDED(hr) ? "supported" : "NOT supported");
    }
    s->usedDxt = SUCCEEDED(s->d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT1)) &&
                 SUCCEEDED(s->d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT5));
    if (!s->usedDxt) LOGF(s, "WARNING: DXT1/DXT5 NOT supported by the D3D9 layer -> falling back to uncompressed A8R8G8B8 textures");

    s->pp.BackBufferWidth = d->width; s->pp.BackBufferHeight = d->height;
    s->pp.BackBufferFormat = D3DFMT_X8R8G8B8; s->pp.BackBufferCount = 1;
    s->pp.SwapEffect = D3DSWAPEFFECT_DISCARD; s->pp.hDeviceWindow = (HWND)d->window; s->pp.Windowed = TRUE;
    s->pp.EnableAutoDepthStencil = TRUE; s->pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    s->pp.PresentationInterval = d->vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    CHK(s->d3d->CreateDevice(0, D3DDEVTYPE_HAL, (HWND)d->window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &s->pp, &s->dev));

    /* vertex declaration over two streams (what UE3 vertex factories do) */
    D3DVERTEXELEMENT9 el[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT3,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT2,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 1, 0,  D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        D3DDECL_END()
    };
    CHK(s->dev->CreateVertexDeclaration(el, &s->decl));
    CHK(s->dev->CreateVertexShader((const DWORD *)k_scene_vs, &s->vs));
    CHK(s->dev->CreatePixelShader((const DWORD *)k_scene_ps, &s->ps));
    CHK(s->dev->CreatePixelShader((const DWORD *)k_rtt_ps, &s->psRtt));

    /* unit quad, 4 verts, 6 indices */
    Vtx0 v0[4] = { {-1, -1, 0, 0, 1}, {-1, 1, 0, 0, 0}, {1, 1, 0, 1, 0}, {1, -1, 0, 1, 1} };
    Vtx1 v1[4] = { {0xFFFFFFFFu}, {0xFFFFE0E0u}, {0xFFE0E0FFu}, {0xFFE0FFE0u} };
    uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
    void *p;
    CHK(s->dev->CreateVertexBuffer(sizeof v0, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &s->vb0, NULL));
    CHK(s->vb0->Lock(0, sizeof v0, &p, 0)); memcpy(p, v0, sizeof v0); s->vb0->Unlock();
    CHK(s->dev->CreateVertexBuffer(sizeof v1, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &s->vb1, NULL));
    CHK(s->vb1->Lock(0, sizeof v1, &p, 0)); memcpy(p, v1, sizeof v1); s->vb1->Unlock();
    CHK(s->dev->CreateIndexBuffer(sizeof idx, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &s->ib, NULL));
    CHK(s->ib->Lock(0, sizeof idx, &p, 0)); memcpy(p, idx, sizeof idx); s->ib->Unlock();

    if (s->usedDxt) {
        if (make_dxt_texture(s, D3DFMT_DXT5, 256, pat_dxt5, &s->texDxt5, err, errsz)) return -1;
        if (make_dxt_texture(s, D3DFMT_DXT1, 256, pat_dxt1, &s->texDxt1, err, errsz)) return -1;
    } else {
        if (make_rgba_texture(s, 256, pat_dxt5, &s->texDxt5, err, errsz)) return -1;
        if (make_rgba_texture(s, 256, pat_dxt1, &s->texDxt1, err, errsz)) return -1;
    }
    LOGF(s, "%s textures: %u mip levels (alpha-cutout texture), %u (opaque grid)", s->usedDxt ? "DXT5/DXT1" : "A8R8G8B8 fallback", s->texDxt5->GetLevelCount(), s->texDxt1->GetLevelCount());

    CHK(s->dev->CreateTexture(256, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s->texRtt, NULL));
    CHK(s->texRtt->GetSurfaceLevel(0, &s->rttSurf));
    CHK(s->dev->CreateDepthStencilSurface(256, 256, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &s->rttDepth, NULL));
    LOGF(s, "scene created %dx%d", d->width, d->height);
    return 0;
}

static void draw_quad(d3d9_scene *s) {
    s->dev->SetStreamSource(0, s->vb0, 0, sizeof(Vtx0));
    s->dev->SetStreamSource(1, s->vb1, 0, sizeof(Vtx1));
    s->dev->SetIndices(s->ib);
    s->dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
}
static void set_wvp(d3d9_scene *s, const Mat &wvp) { Mat t = transpose(wvp); s->dev->SetVertexShaderConstantF(0, &t.m[0][0], 4); }

int d3d9_scene_frame(d3d9_scene *s, float t) {
    s->lastT = t;
    IDirect3DDevice9 *dev = s->dev;
    HRESULT hr = dev->TestCooperativeLevel();
    if (FAILED(hr)) return -2;

    dev->SetVertexDeclaration(s->decl);
    dev->SetVertexShader(s->vs);
    for (int i = 0; i < 2; i++) {
        dev->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); dev->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(i, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR); dev->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        dev->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    }
    float vsParams[4] = { t, 0.25f * t, 0, 0 };
    dev->SetVertexShaderConstantF(4, vsParams, 1);

    /* ---- pass 1: render to texture ---- */
    IDirect3DSurface9 *oldRT = NULL, *oldDS = NULL;
    dev->GetRenderTarget(0, &oldRT); dev->GetDepthStencilSurface(&oldDS);
    D3DVIEWPORT9 oldVp; dev->GetViewport(&oldVp);
    dev->SetRenderTarget(0, s->rttSurf); dev->SetDepthStencilSurface(s->rttDepth);
    D3DVIEWPORT9 vp = { 0, 0, 256, 256, 0.f, 1.f }; dev->SetViewport(&vp);
    dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0x00000000, 1.0f, 0);
    dev->BeginScene();
    dev->SetRenderState(D3DRS_ZENABLE, FALSE); dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    set_wvp(s, ident());
    dev->SetPixelShader(s->psRtt);
    float rp[4] = { t, 0, 0, 0 }; dev->SetPixelShaderConstantF(0, rp, 1);
    dev->SetTexture(0, NULL);
    draw_quad(s);
    dev->EndScene();
    dev->SetRenderTarget(0, oldRT); dev->SetDepthStencilSurface(oldDS); dev->SetViewport(&oldVp);
    SAFE_RELEASE(oldRT); SAFE_RELEASE(oldDS);

    /* ---- pass 2: backbuffer ---- */
    dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF101830, 1.0f, 0);
    dev->BeginScene();
    dev->SetPixelShader(s->ps);
    float aspect = (float)s->w / (float)s->h;
    Mat proj = persp(1.0f, aspect, 0.1f, 50.f);
    float tint[4] = { 1, 1, 1, 1 }; float useRtt[4] = { 0, 0, 0, 0 };

    /* background: DXT1 floor, opaque */
    dev->SetRenderState(D3DRS_ZENABLE, TRUE); dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL); dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetTexture(0, s->texDxt1);
    dev->SetPixelShaderConstantF(0, tint, 1); dev->SetPixelShaderConstantF(1, useRtt, 1);
    BOOL b0 = FALSE; dev->SetPixelShaderConstantB(0, &b0, 1);
    set_wvp(s, mul(mul(scale(3.5f * aspect, 3.5f, 1), trans(0, 0, 4.5f)), proj));
    draw_quad(s);

    /* foreground: rotating DXT5 quad, alpha-blended, modulated by RTT in the pixel shader */
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetTexture(0, s->texDxt5); dev->SetTexture(1, s->texRtt);
    useRtt[0] = 1; dev->SetPixelShaderConstantF(1, useRtt, 1); b0 = TRUE; dev->SetPixelShaderConstantB(0, &b0, 1);
    Mat world = mul(mul(rotZ(t * 0.6f), rotY(sinf(t) * 0.9f)), trans(0, 0, 3.0f));
    set_wvp(s, mul(world, proj));
    draw_quad(s);

    /* small RTT preview in the corner (alpha blended; shows the RTT result directly) */
    useRtt[0] = 0; dev->SetPixelShaderConstantF(1, useRtt, 1); b0 = FALSE; dev->SetPixelShaderConstantB(0, &b0, 1);
    dev->SetTexture(0, s->texRtt);
    set_wvp(s, mul(mul(scale(0.55f, 0.55f, 1), trans(-1.9f * aspect / 1.6f, -1.25f, 3.0f)), proj));
    draw_quad(s);
    dev->EndScene();

    hr = dev->Present(NULL, NULL, NULL, NULL);
    if (hr == D3DERR_DEVICELOST) return -2;
    return FAILED(hr) ? -1 : 0;
}

/* ---------- readback ---------- */
static int readback(d3d9_scene *s, IDirect3DSurface9 *src, std::vector<uint8_t> &bgra, int &w, int &h) {
    D3DSURFACE_DESC d; src->GetDesc(&d); w = d.Width; h = d.Height;
    IDirect3DSurface9 *sys = NULL;
    if (FAILED(s->dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL))) return -1;
    if (FAILED(s->dev->GetRenderTargetData(src, sys))) { sys->Release(); return -1; }
    D3DLOCKED_RECT lr; if (FAILED(sys->LockRect(&lr, NULL, D3DLOCK_READONLY))) { sys->Release(); return -1; }
    bgra.resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++) memcpy(&bgra[(size_t)y * w * 4], (uint8_t *)lr.pBits + (size_t)y * lr.Pitch, (size_t)w * 4);
    sys->UnlockRect(); sys->Release();
    return 0;
}

int d3d9_scene_read_backbuffer(d3d9_scene *s, uint8_t **out, int *w, int *h) {
    IDirect3DSurface9 *bb = NULL; if (FAILED(s->dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return -1;
    std::vector<uint8_t> px; int r = readback(s, bb, px, *w, *h); bb->Release(); if (r) return -1;
    *out = (uint8_t *)malloc(px.size()); memcpy(*out, px.data(), px.size()); return 0;
}

int d3d9_scene_screenshot_ppm(d3d9_scene *s, const char *path) {
    IDirect3DSurface9 *bb = NULL; if (FAILED(s->dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return -1;
    std::vector<uint8_t> px; int w, h; int r = readback(s, bb, px, w, h); bb->Release(); if (r) return -1;
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint8_t rgb[3] = { px[i * 4 + 2], px[i * 4 + 1], px[i * 4 + 0] }; fwrite(rgb, 1, 3, f); }
    fclose(f); return 0;
}

int d3d9_scene_selftest(d3d9_scene *s, char *report, size_t n) {
    int ok = 1; size_t o = 0;
    auto P = [&](const char *fmt, ...) { va_list ap; va_start(ap, fmt); if (o < n) o += vsnprintf(report + o, n - o, fmt, ap); va_end(ap); };
    /* RTT centre pixel: d=0 -> alpha=1, colour = (0.2+0.8*.5, 0.3+0.7*rings, 1-0.8*.5) = (0.6, ?, 0.6) -> ~153,?,153 */
    std::vector<uint8_t> px; int w, h;
    if (readback(s, s->rttSurf, px, w, h)) { P("RTT readback FAILED\n"); return -1; }
    int cx = (h / 2) * w + (w / 2);
    int B = px[cx * 4 + 0], G = px[cx * 4 + 1], R = px[cx * 4 + 2], A = px[cx * 4 + 3];
    float rings = 0.5f + 0.5f * sinf(sqrtf(((w / 2 + .5f) / w - .5f) * ((w / 2 + .5f) / w - .5f) * 2) * 40.f - s->lastT * 3.f);
    int expG = (int)((0.3f + 0.7f * rings) * 255 + .5f);
    P("RTT centre  BGRA=(%d,%d,%d,%d) expected R~153 G~%d B~153 A~250+\n", B, G, R, A, expG);
    if (abs(R - 153) > 8 || abs(B - 153) > 8 || abs(G - expG) > 16 || A < 240) ok = 0;
    /* RTT corner must be transparent (alpha 0, clear colour) */
    A = px[3];
    P("RTT corner  A=%d (expected 0)\n", A); if (A > 8) ok = 0;
    /* backbuffer: top-right corner shows the DXT1 floor or clear colour: must not be pure black / must differ from clear */
    IDirect3DSurface9 *bb = NULL; s->dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (readback(s, bb, px, w, h)) { bb->Release(); P("backbuffer readback FAILED\n"); return -1; }
    bb->Release();
    int cnt = 0, distinct = 0; uint32_t seen[64] = {};
    for (int i = 0; i < w * h; i += 97) { uint32_t c = *(uint32_t *)&px[i * 4] & 0xFFFFFF; if (c) cnt++;
        int f = 0; for (int k = 0; k < distinct; k++) if (seen[k] == (c >> 3)) f = 1; if (!f && distinct < 64) seen[distinct++] = c >> 3; }
    P("backbuffer  non-black samples=%d distinct colours (sampled, >>3)=%d\n", cnt, distinct);
    if (cnt < 100 || distinct < 20) ok = 0;
    int mid = (h / 2) * w + w / 2;
    P("backbuffer centre BGRA=(%d,%d,%d,%d)\n", px[mid * 4], px[mid * 4 + 1], px[mid * 4 + 2], px[mid * 4 + 3]);
    P("SELFTEST %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int d3d9_scene_used_dxt(d3d9_scene *s) { return s->usedDxt ? 1 : 0; }

void d3d9_scene_destroy(d3d9_scene *s) {
    if (!s) return;
    SAFE_RELEASE(s->rttSurf); SAFE_RELEASE(s->rttDepth); SAFE_RELEASE(s->texRtt); SAFE_RELEASE(s->texDxt5); SAFE_RELEASE(s->texDxt1);
    SAFE_RELEASE(s->vb0); SAFE_RELEASE(s->vb1); SAFE_RELEASE(s->ib); SAFE_RELEASE(s->decl);
    SAFE_RELEASE(s->vs); SAFE_RELEASE(s->ps); SAFE_RELEASE(s->psRtt); SAFE_RELEASE(s->dev); SAFE_RELEASE(s->d3d);
    delete s;
}
