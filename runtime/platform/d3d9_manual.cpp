// d3d9: methods whose arguments cannot be marshalled mechanically (32-bit D3DPRESENT_PARAMETERS, D3DLOCKED_RECT/BOX, HWND-carrying
// structs, void**), plus the guest-memory staging used by every Lock/Unlock.
#include "d3d9_internal.h"
#include "dish/heap.h"
#include "dish/lifecycle.h"
#include "dish/mainthread.h"
#include "dish/memacct.h"
#include "dish/watchdog.h"
#include "dish/platform.h"
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <cstring>
#include <mutex>
#include <vector>

namespace dish {
namespace {

// ---- guest staging buffers ------------------------------------------------------------------------------------------------
std::mutex g_stage_mu;
std::vector<uint32_t> g_stage_free[40];     // power-of-two size classes
std::atomic<uint64_t> g_locks{0}, g_in{0}, g_out{0};

uint32_t size_class(uint32_t size) {
    uint32_t c = 8;   // 256 bytes
    while (c < 31 && (1u << c) < size) c++;
    return c;
}

struct LockRec {
    void *key;          // host resource
    uint32_t sub;       // level / face*64+level / 0
    uint32_t guest;     // staging buffer
    uint32_t cap;       // staging capacity (power of two)
    uint32_t extent;    // bytes mirrored between staging and host
    uint8_t *host;
    uint32_t flags;
};
std::mutex g_lock_mu;
std::vector<LockRec> g_locks_active;

void add_lock(const LockRec &r) {
    std::lock_guard<std::mutex> lk(g_lock_mu);
    g_locks_active.push_back(r);
}
bool take_lock(void *key, uint32_t sub, LockRec *out) {
    std::lock_guard<std::mutex> lk(g_lock_mu);
    for (size_t i = g_locks_active.size(); i-- > 0;)
        if (g_locks_active[i].key == key && g_locks_active[i].sub == sub) {
            *out = g_locks_active[i];
            g_locks_active.erase(g_locks_active.begin() + (long)i);
            return true;
        }
    return false;
}

// bytes per pixel / block description for a D3DFORMAT
struct FmtInfo { uint32_t bpp_or_block; bool block; };
FmtInfo fmt_info(D3DFORMAT f) {
    switch ((uint32_t)f) {
        case D3DFMT_DXT1: return {8, true};
        case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return {16, true};
        case 0x31545441: /* ATI1 */ return {8, true};
        case 0x32495441: /* ATI2 */ return {16, true};
        case D3DFMT_R3G3B2: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A4L4: return {1, false};
        case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8R3G3B2: case D3DFMT_X4R4G4B4:
        case D3DFMT_A8P8: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_L16: case D3DFMT_D16: case D3DFMT_D16_LOCKABLE:
        case D3DFMT_R16F: case D3DFMT_CxV8U8: case D3DFMT_UYVY: case D3DFMT_YUY2: case D3DFMT_INDEX16: case D3DFMT_D15S1: return {2, false};
        case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_Q16W16V16U16: return {8, false};
        case D3DFMT_A32B32G32R32F: return {16, false};
        default: return {4, false};
    }
}
uint32_t extent_of(D3DFORMAT f, uint32_t w, uint32_t h, uint32_t pitch) {
    FmtInfo fi = fmt_info(f);
    uint32_t rows, row_bytes;
    if (fi.block) { rows = (h + 3) / 4; row_bytes = ((w + 3) / 4) * fi.bpp_or_block; }
    else { rows = h; row_bytes = w * fi.bpp_or_block; }
    if (!rows) return row_bytes;
    return pitch * (rows - 1) + row_bytes;
}

bool rect_dims(uint32_t grect, uint32_t full_w, uint32_t full_h, uint32_t *w, uint32_t *h) {
    if (!grect) { *w = full_w; *h = full_h; return true; }
    int32_t l = (int32_t)rd32(grect), t = (int32_t)rd32(grect + 4), r = (int32_t)rd32(grect + 8), b = (int32_t)rd32(grect + 12);
    if (r <= l || b <= t) return false;
    *w = (uint32_t)(r - l);
    *h = (uint32_t)(b - t);
    return true;
}

// Finish a lock: allocate staging, mirror the host memory, record, and hand the guest the staging address.
uint32_t begin_lock(void *key, uint32_t sub, uint8_t *host_ptr, uint32_t extent, uint32_t flags) {
    uint32_t cap = 1u << size_class(extent ? extent : 1);
    uint32_t g = d3d9_stage_alloc(cap);
    if (!g) return 0;
    if (!(flags & D3DLOCK_DISCARD) && extent) { memcpy(guest_base + g, host_ptr, extent); g_in += extent; }
    g_locks++;
    add_lock({key, sub, g, cap, extent, host_ptr, flags});
    return g;
}
// Returns false if there was no matching lock.
bool end_lock(void *key, uint32_t sub) {
    LockRec r;
    if (!take_lock(key, sub, &r)) return false;
    if (!(r.flags & D3DLOCK_READONLY) && r.extent) { memcpy(r.host, guest_base + r.guest, r.extent); g_out += r.extent; }
    d3d9_stage_free(r.guest, r.cap);
    return true;
}

}  // namespace

D3d9Stats d3d9_stats() {
    D3d9Stats s;
    s.locks = g_locks;
    s.lock_bytes_in = g_in;
    s.lock_bytes_out = g_out;
    return s;
}

uint32_t d3d9_stage_alloc(uint32_t size) {
    uint32_t c = size_class(size);
    {
        std::lock_guard<std::mutex> lk(g_stage_mu);
        if (!g_stage_free[c].empty()) { uint32_t g = g_stage_free[c].back(); g_stage_free[c].pop_back(); return g; }
    }
    return heap_alloc(heap_process(), 0, 1u << c);
}
void d3d9_stage_free(uint32_t guest, uint32_t size) {
    uint32_t c = size_class(size);
    std::lock_guard<std::mutex> lk(g_stage_mu);
    if (g_stage_free[c].size() < 16) g_stage_free[c].push_back(guest);
    else heap_free(heap_process(), 0, guest);
}

// ---- D3DPRESENT_PARAMETERS (32-bit guest layout, 56 bytes) ----------------------------------------------------------------
void d3d9_pp_from_guest(uint32_t g, D3DPRESENT_PARAMETERS *h) {
    memset(h, 0, sizeof *h);
    h->BackBufferWidth = rd32(g + 0);
    h->BackBufferHeight = rd32(g + 4);
    h->BackBufferFormat = (D3DFORMAT)rd32(g + 8);
    h->BackBufferCount = rd32(g + 12);
    h->MultiSampleType = (D3DMULTISAMPLE_TYPE)rd32(g + 16);
    h->MultiSampleQuality = rd32(g + 20);
    h->SwapEffect = (D3DSWAPEFFECT)rd32(g + 24);
    h->hDeviceWindow = d3d9_host_hwnd(rd32(g + 28));
    h->Windowed = (BOOL)rd32(g + 32);
    h->EnableAutoDepthStencil = (BOOL)rd32(g + 36);
    h->AutoDepthStencilFormat = (D3DFORMAT)rd32(g + 40);
    h->Flags = rd32(g + 44);
    h->FullScreen_RefreshRateInHz = rd32(g + 48);
    h->PresentationInterval = rd32(g + 52);
}
void d3d9_pp_to_guest(const D3DPRESENT_PARAMETERS &h, uint32_t g) {
    wr32(g + 0, h.BackBufferWidth);
    wr32(g + 4, h.BackBufferHeight);
    wr32(g + 8, (uint32_t)h.BackBufferFormat);
    wr32(g + 12, h.BackBufferCount);
    wr32(g + 16, (uint32_t)h.MultiSampleType);
    wr32(g + 20, h.MultiSampleQuality);
    wr32(g + 24, (uint32_t)h.SwapEffect);
    // hDeviceWindow keeps the guest's value
    wr32(g + 32, (uint32_t)h.Windowed);
    wr32(g + 36, (uint32_t)h.EnableAutoDepthStencil);
    wr32(g + 40, (uint32_t)h.AutoDepthStencilFormat);
    wr32(g + 44, h.Flags);
    wr32(g + 48, h.FullScreen_RefreshRateInHz);
    wr32(g + 52, h.PresentationInterval);
}

namespace {
uint32_t g_focus_hwnd = 0, g_behavior = 0, g_adapter = 0, g_devtype = 1;
}

// IDirect3D9::CreateDevice(this, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface)
GuestRet m_IDirect3D9_CreateDevice(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *self = static_cast<IDirect3D9 *>(com_host_object_raw(A(0)));
    if (!self) return d3d9_bad_this("IDirect3D9::CreateDevice", 28);
    uint32_t adapter = A(1), type = A(2), focus = A(3), flags = A(4), gpp = A(5), gout = A(6);
    if (!gpp || !gout) return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 28);
    D3DPRESENT_PARAMETERS pp;
    d3d9_pp_from_guest(gpp, &pp);
    uint32_t dev_hwnd = rd32(gpp + 28) ? rd32(gpp + 28) : focus;
    HWND hfocus = d3d9_host_hwnd(focus ? focus : dev_hwnd);
    if (!pp.hDeviceWindow) pp.hDeviceWindow = d3d9_host_hwnd(dev_hwnd);
    if (!hfocus) { LOGE(LC_GFX, "CreateDevice: window %08x has no SDL window", focus); return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 28); }
    IDirect3DDevice9 *dev = nullptr;
    HRESULT hr = D3DERR_DRIVERINTERNALERROR;
    mainthread_call([&] {
        try { hr = self->CreateDevice(adapter, (D3DDEVTYPE)type, hfocus, flags, &pp, &dev); } catch (...) { hr = D3DERR_DRIVERINTERNALERROR; }
    });
    LOGI(LC_GFX, "CreateDevice(adapter %u, type %u, flags %08x, %ux%u fmt %u windowed %d) -> %08x", adapter, type, flags, pp.BackBufferWidth, pp.BackBufferHeight,
         (unsigned)pp.BackBufferFormat, (int)pp.Windowed, (unsigned)hr);
    if (FAILED(hr) || !dev) { wr32(gout, 0); return guest_ret((uint32_t)hr, 0, 28); }
    d3d9_pp_to_guest(pp, gpp);
    g_focus_hwnd = focus ? focus : dev_hwnd;
    g_behavior = flags;
    g_adapter = adapter;
    g_devtype = type;
    wr32(gout, d3d9_wrap(dev, K_Device));
    return guest_ret(0, 0, 28);
}
GuestRet m_IDirect3D9_GetAdapterIdentifier(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, Adapter, Flags, D3DADAPTER_IDENTIFIER9*): host struct is 4 bytes longer
    auto *self = static_cast<IDirect3D9 *>(com_host_object_raw(A(0)));
    if (!self) return d3d9_bad_this("IDirect3D9::GetAdapterIdentifier", 16);
    if (!A(3)) return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 16);
    D3DADAPTER_IDENTIFIER9 id;
    memset(&id, 0, sizeof id);
    HRESULT hr = self->GetAdapterIdentifier(A(1), A(2), &id);
    if (SUCCEEDED(hr)) memcpy(guest_base + A(3), &id, 1100);
    return guest_ret((uint32_t)hr, 0, 16);
}
GuestRet m_IDirect3D9_GetAdapterMonitor(uint32_t esp, uint32_t, uint32_t, uint32_t) { return guest_ret(0x00010001, 0, 8); }
GuestRet m_IDirect3D9_RegisterSoftwareDevice(uint32_t esp, uint32_t, uint32_t, uint32_t) { return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 8); }

GuestRet m_IDirect3DDevice9_GetCreationParameters(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    uint32_t out = A(1);
    if (!com_host_object_raw(A(0))) return d3d9_bad_this("GetCreationParameters", 8);
    if (!out) return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 8);
    wr32(out, g_adapter); wr32(out + 4, g_devtype); wr32(out + 8, g_focus_hwnd); wr32(out + 12, g_behavior);
    return guest_ret(0, 0, 8);
}
GuestRet m_IDirect3DDevice9_Reset(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *self = static_cast<IDirect3DDevice9 *>(com_host_object_raw(A(0)));
    if (!self) return d3d9_bad_this("IDirect3DDevice9::Reset", 8);
    uint32_t gpp = A(1);
    if (!gpp) return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 8);
    D3DPRESENT_PARAMETERS pp;
    d3d9_pp_from_guest(gpp, &pp);
    if (!pp.hDeviceWindow) pp.hDeviceWindow = d3d9_host_hwnd(g_focus_hwnd);
    HRESULT hr = D3DERR_DRIVERINTERNALERROR;
    mainthread_call([&] { try { hr = self->Reset(&pp); } catch (...) { hr = D3DERR_DRIVERINTERNALERROR; } });
    LOGI(LC_GFX, "Reset(%ux%u windowed %d) -> %08x", pp.BackBufferWidth, pp.BackBufferHeight, (int)pp.Windowed, (unsigned)hr);
    if (SUCCEEDED(hr)) d3d9_pp_to_guest(pp, gpp);
    return guest_ret((uint32_t)hr, 0, 8);
}
GuestRet m_IDirect3DDevice9_CreateAdditionalSwapChain(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *self = static_cast<IDirect3DDevice9 *>(com_host_object_raw(A(0)));
    if (!self) return d3d9_bad_this("CreateAdditionalSwapChain", 12);
    uint32_t gpp = A(1), gout = A(2);
    D3DPRESENT_PARAMETERS pp;
    d3d9_pp_from_guest(gpp, &pp);
    IDirect3DSwapChain9 *sc = nullptr;
    HRESULT hr = D3DERR_DRIVERINTERNALERROR;
    mainthread_call([&] { try { hr = self->CreateAdditionalSwapChain(&pp, &sc); } catch (...) {} });
    if (FAILED(hr) || !sc) { if (gout) wr32(gout, 0); return guest_ret((uint32_t)hr, 0, 12); }
    d3d9_pp_to_guest(pp, gpp);
    wr32(gout, d3d9_wrap(sc, K_SwapChain));
    return guest_ret(0, 0, 12);
}
GuestRet m_IDirect3DSwapChain9_GetPresentParameters(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *self = static_cast<IDirect3DSwapChain9 *>(com_host_object_raw(A(0)));
    if (!self) return d3d9_bad_this("GetPresentParameters", 8);
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr = self->GetPresentParameters(&pp);
    if (SUCCEEDED(hr) && A(1)) { d3d9_pp_to_guest(pp, A(1)); wr32(A(1) + 28, g_focus_hwnd); }
    return guest_ret((uint32_t)hr, 0, 8);
}

// ---- Present: real Present, then the app-lifecycle safe point -------------------------------------------------------------
// When the app is going to the background the host calls lifecycle_pause(); the render thread parks here after finishing
// its frame, having waited for the GPU to go idle (an event query), so no GPU work is submitted while backgrounded.
namespace {
void drain_gpu(void *devp) {
    auto *dev = static_cast<IDirect3DDevice9 *>(devp);
    IDirect3DQuery9 *q = nullptr;
    if (FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &q)) || !q) return;
    q->Issue(D3DISSUE_END);
    for (int i = 0; i < 2000; i++) {   // up to ~2 s
        if (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) != S_FALSE) break;
        SDL_Delay(1);
    }
    q->Release();
}
}  // namespace

// ---- debug: frame-rate log and periodic back-buffer dumps (DISH_FRAME_DUMP=<dir>, DISH_FRAME_DUMP_EVERY=<seconds, default 5>)
// Lets a headless run (or an on-device run) show what the game draws without looking at the screen.
uint32_t png_crc(const uint8_t *p, size_t n, uint32_t c) {
    static uint32_t tab[256];
    if (!tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; tab[i] = v; }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
void png_be32(std::vector<uint8_t> &v, uint32_t x) { for (int sh = 24; sh >= 0; sh -= 8) v.push_back((uint8_t)(x >> sh)); }
void png_chunk(std::vector<uint8_t> &png, const char *type, const std::vector<uint8_t> &data) {
    png_be32(png, (uint32_t)data.size());
    size_t at = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    png_be32(png, png_crc(&png[at], png.size() - at, 0));
}
// BGRA/BGRX rows -> RGB PNG with stored (uncompressed) deflate blocks
bool write_png_bgra(const std::string &path, const uint8_t *bits, uint32_t pitch, uint32_t w, uint32_t h) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (w * 3 + 1));
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);
        const uint8_t *row = bits + (size_t)y * pitch;
        for (uint32_t x = 0; x < w; x++) { raw.push_back(row[4 * x + 2]); raw.push_back(row[4 * x + 1]); raw.push_back(row[4 * x]); }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size();) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8)); z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + (long)off, raw.begin() + (long)(off + n));
        off += n;
    }
    png_be32(z, (b << 16) | a);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'}, ihdr;
    png_be32(ihdr, w); png_be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    png_chunk(png, "IHDR", ihdr);
    png_chunk(png, "IDAT", z);
    png_chunk(png, "IEND", {});
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    return fclose(f) == 0 && ok;
}
void dump_backbuffer(IDirect3DDevice9 *dev, const std::string &path) {
    IDirect3DSurface9 *bb = nullptr, *flat = nullptr, *sys = nullptr;
    D3DSURFACE_DESC d;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    bb->GetDesc(&d);
    IDirect3DSurface9 *src = bb;
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {   // resolve first: GetRenderTargetData needs a single-sample source
        if (SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &flat, nullptr)) &&
            SUCCEEDED(dev->StretchRect(bb, nullptr, flat, nullptr, D3DTEXF_NONE)))
            src = flat;
    }
    if ((d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) &&
        SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(src, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            bool ok = write_png_bgra(path, (const uint8_t *)lr.pBits, (uint32_t)lr.Pitch, d.Width, d.Height);
            sys->UnlockRect();
            LOGI(LC_GFX, "frame dump %s (%ux%u)%s", path.c_str(), d.Width, d.Height, ok ? "" : " FAILED");
        }
    } else {
        LOGW(LC_GFX, "frame dump: back buffer format %d not supported", (int)d.Format);
    }
    if (sys) sys->Release();
    if (flat) flat->Release();
    bb->Release();
}
bool g_d3d9_trace_on = false;
bool g_d3d9_watch = getenv("DISH_D3D9_BT_ZEROMAT") != nullptr;
__attribute__((noinline)) void d3d9_debug_hook() { asm volatile(""); }   // gdb breakpoint target for the debug paths below
void d3d9_trace(const char *what, uint32_t esp, int nargs) {
    if (g_d3d9_watch && !strcmp(what, "IDirect3DDevice9::SetVertexShaderConstantF") && A(3) == 4 && A(2)) {
        static int left = atoi(getenv("DISH_D3D9_BT_ZEROMAT"));
        const float *f = reinterpret_cast<const float *>(guest_base + A(2));
        if (left > 0 && f[0] == 0 && f[1] == 0 && f[4] == 0 && f[5] == 0 && f[10] == 1) {
            left--;
            LOGI(LC_GFX, "zero matrix upload at c%u", A(1));
            d3d9_debug_hook();
        }
    }
    if (!g_d3d9_trace_on) return;
    char buf[160];
    int n = snprintf(buf, sizeof buf, "%s(", what);
    for (int i = 0; i < nargs && i < 8 && n < (int)sizeof buf - 12; i++) n += snprintf(buf + n, sizeof buf - n, i ? ", %x" : "%x", A(i + 1));
    LOGI(LC_GFX, "trace %s) this=%x", buf, A(0));
    auto dump_verts = [](uint32_t g, uint32_t stride, uint32_t n) {
        for (uint32_t v = 0; v < n && v < 8 && g; v++) {
            char line[200];
            int k = 0;
            for (uint32_t w = 0; w < stride / 4 && w < 10; w++) {
                uint32_t x = *reinterpret_cast<const uint32_t *>(guest_base + g + v * stride + 4 * w);
                float f;
                memcpy(&f, &x, 4);
                k += snprintf(line + k, sizeof line - k, " %08x(%g)", x, f);
            }
            LOGI(LC_GFX, "trace   v%u:%s", v, line);
        }
    };
    if (!strcmp(what, "IDirect3DDevice9::DrawPrimitiveUP")) {
        dump_verts(A(3), A(4), A(2) * 3);
    } else if (!strcmp(what, "IDirect3DDevice9::DrawIndexedPrimitiveUP")) {
        dump_verts(A(7), A(8), A(3));
    } else if (!strcmp(what, "IDirect3DDevice9::SetVertexShaderConstantF") || !strcmp(what, "IDirect3DDevice9::SetPixelShaderConstantF")) {
        for (uint32_t r = 0; r < A(3) && r < 8 && A(2); r++) {
            const float *f = reinterpret_cast<const float *>(guest_base + A(2) + 16 * r);
            LOGI(LC_GFX, "trace   c%u: %g %g %g %g", A(1) + r, f[0], f[1], f[2], f[3]);
        }
    } else if (!strcmp(what, "IDirect3DDevice9::SetViewport") && A(1)) {
        const D3DVIEWPORT9 *v = reinterpret_cast<const D3DVIEWPORT9 *>(guest_base + A(1));
        LOGI(LC_GFX, "trace   viewport %lu,%lu %lux%lu z %g..%g", (unsigned long)v->X, (unsigned long)v->Y, (unsigned long)v->Width,
             (unsigned long)v->Height, v->MinZ, v->MaxZ);
    } else if (!strcmp(what, "IDirect3DDevice9::SetScissorRect") && A(1)) {
        const RECT *r = reinterpret_cast<const RECT *>(guest_base + A(1));
        LOGI(LC_GFX, "trace   scissor %ld,%ld-%ld,%ld", (long)r->left, (long)r->top, (long)r->right, (long)r->bottom);
    } else if ((!strcmp(what, "IDirect3DDevice9::SetRenderTarget") && A(2)) || (!strcmp(what, "IDirect3DDevice9::SetDepthStencilSurface") && A(1))) {
        auto *sf = static_cast<IDirect3DSurface9 *>(com_host_object_raw(what[21] == 'R' ? A(2) : A(1)));
        D3DSURFACE_DESC d;
        if (sf && SUCCEEDED(sf->GetDesc(&d))) LOGI(LC_GFX, "trace   surface %ux%u fmt %d usage %x", d.Width, d.Height, (int)d.Format, (unsigned)d.Usage);
    } else if (!strcmp(what, "IDirect3DDevice9::StretchRect")) {
        for (int k : {2, 4}) {
            if (!A(k)) { LOGI(LC_GFX, "trace   rect%d: whole", k); continue; }
            const RECT *r = reinterpret_cast<const RECT *>(guest_base + A(k));
            LOGI(LC_GFX, "trace   rect%d: %ld,%ld-%ld,%ld", k, (long)r->left, (long)r->top, (long)r->right, (long)r->bottom);
        }
    } else if (!strcmp(what, "IDirect3DDevice9::SetTexture") && A(2)) {
        auto *t = static_cast<IDirect3DBaseTexture9 *>(com_host_object_raw(A(2)));
        if (t && t->GetType() == D3DRTYPE_TEXTURE) {
            auto *t2 = static_cast<IDirect3DTexture9 *>(t);
            D3DSURFACE_DESC d;
            if (SUCCEEDED(t2->GetLevelDesc(0, &d))) {
                LOGI(LC_GFX, "trace   tex %x: fmt %d %ux%u levels %u usage %x pool %d", A(2), (int)d.Format, d.Width, d.Height,
                     t2->GetLevelCount(), (unsigned)d.Usage, (int)d.Pool);
                static const char *dir = getenv("DISH_D3D9_TRACE_TEXDIR");
                D3DLOCKED_RECT lr;
                if (dir && d.Format != D3DFMT_DXT1 && d.Format != D3DFMT_DXT3 && d.Format != D3DFMT_DXT5 &&
                    SUCCEEDED(t2->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) {
                    char path[256];
                    snprintf(path, sizeof path, "%s/tex_%x_f%d_%ux%u_p%d.raw", dir, A(2), (int)d.Format, d.Width, d.Height, (int)lr.Pitch);
                    if (FILE *f = fopen(path, "wb")) { fwrite(lr.pBits, 1, (size_t)lr.Pitch * d.Height, f); fclose(f); }
                    t2->UnlockRect(0);
                }
            }
        }
    }
}
void frame_debug(IDirect3DDevice9 *dev) {
    watchdog_note_progress();
    static const long trace_frame = getenv("DISH_D3D9_TRACE_FRAME") ? atol(getenv("DISH_D3D9_TRACE_FRAME")) : -1;
    using Clock = std::chrono::steady_clock;
    static const char *dir = getenv("DISH_FRAME_DUMP");
    static const double every = getenv("DISH_FRAME_DUMP_EVERY") ? atof(getenv("DISH_FRAME_DUMP_EVERY")) : 5.0;
    static Clock::time_point last_log = Clock::now(), last_dump = Clock::now();
    static uint64_t frames = 0, frames_at_log = 0;
    static int dumps = 0;
    frames++;
    g_d3d9_trace_on = (long)frames == trace_frame;
    if (g_d3d9_trace_on) LOGI(LC_GFX, "trace: begin frame %llu", (unsigned long long)frames);
    Clock::time_point now = Clock::now();
    double since_log = std::chrono::duration<double>(now - last_log).count();
    if (since_log >= 5.0) {
        MemStats ms = memacct_query();
        LOGI(LC_GFX, "d3d9: %llu frames presented, %.1f fps; guest commit %llu MiB, footprint %llu MiB, gpu %llu MiB", (unsigned long long)frames,
             (frames - frames_at_log) / since_log, (unsigned long long)(ms.guest_committed >> 20), (unsigned long long)(ms.footprint >> 20),
             (unsigned long long)(ms.gpu_reported >> 20));
        last_log = now;
        frames_at_log = frames;
    }
    if (dir && std::chrono::duration<double>(now - last_dump).count() >= every) {
        last_dump = now;
        char name[64];
        snprintf(name, sizeof name, "/frame_%03d.png", dumps++);
        dump_backbuffer(dev, std::string(dir) + name);
    }
}

GuestRet m_IDirect3DDevice9_Present(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *self = static_cast<IDirect3DDevice9 *>(com_host_object_raw(A(0)));
    if (__builtin_expect(!self, 0)) return d3d9_bad_this("IDirect3DDevice9::Present", 20);
    const RECT *a1 = (const RECT *)(A(1) ? guest_base + A(1) : nullptr);
    const RECT *a2 = (const RECT *)(A(2) ? guest_base + A(2) : nullptr);
    HWND a3 = d3d9_host_hwnd(A(3));
    const RGNDATA *a4 = (const RGNDATA *)(A(4) ? guest_base + A(4) : nullptr);
    uint32_t r = 0;
    frame_debug(self);
    try { r = (uint32_t)self->Present(a1, a2, a3, a4); } catch (...) { return d3d9_exception("IDirect3DDevice9::Present", 20); }
    lifecycle_park(drain_gpu, self);
    return guest_ret(r, 0, 20);
}
GuestRet m_IDirect3DSwapChain9_Present(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *self = static_cast<IDirect3DSwapChain9 *>(com_host_object_raw(A(0)));
    if (__builtin_expect(!self, 0)) return d3d9_bad_this("IDirect3DSwapChain9::Present", 24);
    const RECT *a1 = (const RECT *)(A(1) ? guest_base + A(1) : nullptr);
    const RECT *a2 = (const RECT *)(A(2) ? guest_base + A(2) : nullptr);
    HWND a3 = d3d9_host_hwnd(A(3));
    const RGNDATA *a4 = (const RGNDATA *)(A(4) ? guest_base + A(4) : nullptr);
    DWORD a5 = (DWORD)A(5);
    uint32_t r = 0;
    try { r = (uint32_t)self->Present(a1, a2, a3, a4, a5); } catch (...) { return d3d9_exception("IDirect3DSwapChain9::Present", 24); }
    if (lifecycle_paused()) {
        IDirect3DDevice9 *dev = nullptr;
        if (SUCCEEDED(self->GetDevice(&dev)) && dev) { lifecycle_park(drain_gpu, dev); dev->Release(); }
    } else {
        lifecycle_park(nullptr, nullptr);
    }
    return guest_ret(r, 0, 24);
}

// ---- surfaces ---------------------------------------------------------------------------------------------------------------
GuestRet m_IDirect3DSurface9_LockRect(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, D3DLOCKED_RECT*, const RECT*, DWORD)
    auto *s = static_cast<IDirect3DSurface9 *>(com_host_object_raw(A(0)));
    if (!s) return d3d9_bad_this("Surface9::LockRect", 16);
    D3D9_TRACE("IDirect3DSurface9::LockRect", 3);
    uint32_t gl = A(1), grect = A(2), flags = A(3);
    if (!gl) return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 16);
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    HRESULT hr = s->GetDesc(&d);
    if (SUCCEEDED(hr)) hr = s->LockRect(&lr, grect ? reinterpret_cast<const RECT *>(guest_base + grect) : nullptr, flags);
    if (FAILED(hr)) { wr32(gl, 0); wr32(gl + 4, 0); return guest_ret((uint32_t)hr, 0, 16); }
    uint32_t w, h;
    if (!rect_dims(grect, d.Width, d.Height, &w, &h)) { s->UnlockRect(); return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 16); }
    uint32_t g = begin_lock(s, 0, static_cast<uint8_t *>(lr.pBits), extent_of(d.Format, w, h, (uint32_t)lr.Pitch), flags);
    if (!g) { s->UnlockRect(); return guest_ret((uint32_t)D3DERR_OUTOFVIDEOMEMORY, 0, 16); }
    wr32(gl, (uint32_t)lr.Pitch);
    wr32(gl + 4, g);
    return guest_ret(0, 0, 16);
}
GuestRet m_IDirect3DSurface9_UnlockRect(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *s = static_cast<IDirect3DSurface9 *>(com_host_object_raw(A(0)));
    if (!s) return d3d9_bad_this("Surface9::UnlockRect", 4);
    end_lock(s, 0);
    return guest_ret((uint32_t)s->UnlockRect(), 0, 4);
}
GuestRet m_IDirect3DSurface9_GetContainer(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *s = static_cast<IDirect3DSurface9 *>(com_host_object_raw(A(0)));
    if (!s) return d3d9_bad_this("Surface9::GetContainer", 12);
    void *out = nullptr;
    HRESULT hr = s->GetContainer(*(const GUID *)(guest_base + A(1)), &out);
    if (FAILED(hr) || !out) { wr32(A(2), 0); return guest_ret((uint32_t)(FAILED(hr) ? hr : E_NOINTERFACE), 0, 12); }
    uint32_t g = d3d9_wrap(out, K_Unknown);
    wr32(A(2), g);
    return guest_ret(g ? 0 : (uint32_t)E_NOINTERFACE, 0, 12);
}
GuestRet m_IDirect3DSurface9_GetDC(uint32_t esp, uint32_t, uint32_t, uint32_t) { return d3d9_unsupported("IDirect3DSurface9::GetDC", 8); }
GuestRet m_IDirect3DSurface9_ReleaseDC(uint32_t esp, uint32_t, uint32_t, uint32_t) { return d3d9_unsupported("IDirect3DSurface9::ReleaseDC", 8); }

// ---- textures ---------------------------------------------------------------------------------------------------------------
GuestRet m_IDirect3DTexture9_LockRect(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, Level, D3DLOCKED_RECT*, const RECT*, DWORD)
    auto *t = static_cast<IDirect3DTexture9 *>(com_host_object_raw(A(0)));
    if (!t) return d3d9_bad_this("Texture9::LockRect", 20);
    D3D9_TRACE("IDirect3DTexture9::LockRect", 4);
    if (static const char *bt = getenv("DISH_D3D9_BT_LOCK"); __builtin_expect(bt != nullptr, 0) && A(3)) {
        // host backtrace of sub-rect texture locks (glyph cache updates); resolve g_ frames with addr2line
        static int n = 0;
        if (n++ < atoi(bt)) {
            const RECT *r = reinterpret_cast<const RECT *>(guest_base + A(3));
            d3d9_debug_hook();
            LOGI(LC_GFX, "bt lock tex %x level %u rect %ld,%ld-%ld,%ld", A(0), A(1), (long)r->left, (long)r->top, (long)r->right, (long)r->bottom);
            void *frames[24];
            int nf = backtrace(frames, 24);
            for (int i = 0; i < nf; i++) {
                Dl_info di;
                if (dladdr(frames[i], &di) && di.dli_fbase)
                    LOGI(LC_GFX, "bt   #%d +0x%lx", i, (unsigned long)((uintptr_t)frames[i] - (uintptr_t)di.dli_fbase));
            }
        }
    }
    uint32_t level = A(1), gl = A(2), grect = A(3), flags = A(4);
    if (!gl) return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 20);
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    HRESULT hr = t->GetLevelDesc(level, &d);
    if (static const bool lg = getenv("DISH_D3D9_LOG_LOCKS") != nullptr; lg && SUCCEEDED(hr) && (d.Format == D3DFMT_DXT1 || d.Format == D3DFMT_DXT5))
        LOGI(LC_GFX, "lock tex %x level %u %ux%u fmt %d flags %x rect %x", A(0), level, d.Width, d.Height, (int)d.Format, flags, grect);
    if (SUCCEEDED(hr)) hr = t->LockRect(level, &lr, grect ? reinterpret_cast<const RECT *>(guest_base + grect) : nullptr, flags);
    if (FAILED(hr)) { wr32(gl, 0); wr32(gl + 4, 0); return guest_ret((uint32_t)hr, 0, 20); }
    uint32_t w, h;
    if (!rect_dims(grect, d.Width, d.Height, &w, &h)) { t->UnlockRect(level); return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 20); }
    uint32_t g = begin_lock(t, level, static_cast<uint8_t *>(lr.pBits), extent_of(d.Format, w, h, (uint32_t)lr.Pitch), flags);
    if (!g) { t->UnlockRect(level); return guest_ret((uint32_t)D3DERR_OUTOFVIDEOMEMORY, 0, 20); }
    wr32(gl, (uint32_t)lr.Pitch);
    wr32(gl + 4, g);
    return guest_ret(0, 0, 20);
}
GuestRet m_IDirect3DTexture9_UnlockRect(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *t = static_cast<IDirect3DTexture9 *>(com_host_object_raw(A(0)));
    if (!t) return d3d9_bad_this("Texture9::UnlockRect", 8);
    D3D9_TRACE("IDirect3DTexture9::UnlockRect", 1);
    end_lock(t, A(1));
    HRESULT hr = t->UnlockRect(A(1));
    // DISH_D3D9_DUMP_DXT=<dir>[:<max>]: save DXT levels the game uploads (raw blocks, row pitch = blocks) to validate the ASTC transcode
    if (static const char *dump = getenv("DISH_D3D9_DUMP_DXT"); __builtin_expect(dump != nullptr, 0) && SUCCEEDED(hr)) {
        static std::atomic<int> n{0};
        static const int max_n = strchr(dump, ':') ? atoi(strchr(dump, ':') + 1) : 400;
        D3DSURFACE_DESC d;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(t->GetLevelDesc(A(1), &d)) && (d.Format == D3DFMT_DXT1 || d.Format == D3DFMT_DXT3 || d.Format == D3DFMT_DXT5) && d.Width >= 64 &&
            d.Height >= 64 && n.load() < max_n && SUCCEEDED(t->LockRect(A(1), &lr, nullptr, D3DLOCK_READONLY))) {
            int k = n++;
            const uint32_t bs = d.Format == D3DFMT_DXT1 ? 8 : 16, row = (d.Width + 3) / 4 * bs, rows = (d.Height + 3) / 4;
            char path[512];
            std::string dir(dump, strchr(dump, ':') ? (size_t)(strchr(dump, ':') - dump) : strlen(dump));
            snprintf(path, sizeof path, "%s/dxt%d_%ux%u_%04d.bin", dir.c_str(), d.Format == D3DFMT_DXT1 ? 1 : d.Format == D3DFMT_DXT3 ? 3 : 5,
                     d.Width, d.Height, k);
            if (FILE *f = fopen(path, "wb")) {
                for (uint32_t y = 0; y < rows; y++) fwrite((const uint8_t *)lr.pBits + (size_t)y * lr.Pitch, 1, row, f);
                fclose(f);
            }
            t->UnlockRect(A(1));
        }
    }
    return guest_ret((uint32_t)hr, 0, 8);
}
GuestRet m_IDirect3DCubeTexture9_LockRect(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, Face, Level, D3DLOCKED_RECT*, const RECT*, DWORD)
    auto *t = static_cast<IDirect3DCubeTexture9 *>(com_host_object_raw(A(0)));
    if (!t) return d3d9_bad_this("CubeTexture9::LockRect", 24);
    uint32_t face = A(1), level = A(2), gl = A(3), grect = A(4), flags = A(5);
    if (!gl) return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 24);
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    HRESULT hr = t->GetLevelDesc(level, &d);
    if (SUCCEEDED(hr)) hr = t->LockRect((D3DCUBEMAP_FACES)face, level, &lr, grect ? reinterpret_cast<const RECT *>(guest_base + grect) : nullptr, flags);
    if (FAILED(hr)) { wr32(gl, 0); wr32(gl + 4, 0); return guest_ret((uint32_t)hr, 0, 24); }
    uint32_t w, h;
    if (!rect_dims(grect, d.Width, d.Height, &w, &h)) { t->UnlockRect((D3DCUBEMAP_FACES)face, level); return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 24); }
    uint32_t g = begin_lock(t, face * 64 + level, static_cast<uint8_t *>(lr.pBits), extent_of(d.Format, w, h, (uint32_t)lr.Pitch), flags);
    if (!g) { t->UnlockRect((D3DCUBEMAP_FACES)face, level); return guest_ret((uint32_t)D3DERR_OUTOFVIDEOMEMORY, 0, 24); }
    wr32(gl, (uint32_t)lr.Pitch);
    wr32(gl + 4, g);
    return guest_ret(0, 0, 24);
}
GuestRet m_IDirect3DCubeTexture9_UnlockRect(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *t = static_cast<IDirect3DCubeTexture9 *>(com_host_object_raw(A(0)));
    if (!t) return d3d9_bad_this("CubeTexture9::UnlockRect", 12);
    end_lock(t, A(1) * 64 + A(2));
    return guest_ret((uint32_t)t->UnlockRect((D3DCUBEMAP_FACES)A(1), A(2)), 0, 12);
}

// volumes: D3DLOCKED_BOX { INT RowPitch; INT SlicePitch; void *pBits } is 12 bytes in the guest, 16 on the host
static uint32_t lock_box_common(void *key, uint32_t sub, uint32_t depth_hint, D3DFORMAT fmt, uint32_t w, uint32_t h, uint32_t gl, D3DLOCKED_BOX &lb, uint32_t flags) {
    (void)depth_hint;
    uint32_t extent = lb.SlicePitch * (depth_hint ? depth_hint - 1 : 0) + extent_of(fmt, w, h, (uint32_t)lb.RowPitch);
    uint32_t g = begin_lock(key, sub, static_cast<uint8_t *>(lb.pBits), extent, flags);
    if (!g) return 0;
    wr32(gl, (uint32_t)lb.RowPitch);
    wr32(gl + 4, (uint32_t)lb.SlicePitch);
    wr32(gl + 8, g);
    return g;
}
static bool box_dims(uint32_t gbox, uint32_t fw, uint32_t fh, uint32_t fd, uint32_t *w, uint32_t *h, uint32_t *d) {
    if (!gbox) { *w = fw; *h = fh; *d = fd; return true; }
    uint32_t l = rd32(gbox), t = rd32(gbox + 4), r = rd32(gbox + 8), b = rd32(gbox + 12), f = rd32(gbox + 16), bk = rd32(gbox + 20);
    if (r <= l || b <= t || bk <= f) return false;
    *w = r - l; *h = b - t; *d = bk - f;
    return true;
}
GuestRet m_IDirect3DVolumeTexture9_LockBox(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, Level, D3DLOCKED_BOX*, const D3DBOX*, DWORD)
    auto *t = static_cast<IDirect3DVolumeTexture9 *>(com_host_object_raw(A(0)));
    if (!t) return d3d9_bad_this("VolumeTexture9::LockBox", 20);
    uint32_t level = A(1), gl = A(2), gbox = A(3), flags = A(4);
    D3DVOLUME_DESC vd;
    D3DLOCKED_BOX lb;
    HRESULT hr = t->GetLevelDesc(level, &vd);
    if (SUCCEEDED(hr)) hr = t->LockBox(level, &lb, gbox ? reinterpret_cast<const D3DBOX *>(guest_base + gbox) : nullptr, flags);
    if (FAILED(hr)) return guest_ret((uint32_t)hr, 0, 20);
    uint32_t w, h, d;
    if (!box_dims(gbox, vd.Width, vd.Height, vd.Depth, &w, &h, &d) || !lock_box_common(t, level, d, vd.Format, w, h, gl, lb, flags)) {
        t->UnlockBox(level);
        return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 20);
    }
    return guest_ret(0, 0, 20);
}
GuestRet m_IDirect3DVolumeTexture9_UnlockBox(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *t = static_cast<IDirect3DVolumeTexture9 *>(com_host_object_raw(A(0)));
    if (!t) return d3d9_bad_this("VolumeTexture9::UnlockBox", 8);
    end_lock(t, A(1));
    return guest_ret((uint32_t)t->UnlockBox(A(1)), 0, 8);
}
GuestRet m_IDirect3DVolume9_LockBox(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, D3DLOCKED_BOX*, const D3DBOX*, DWORD)
    auto *v = static_cast<IDirect3DVolume9 *>(com_host_object_raw(A(0)));
    if (!v) return d3d9_bad_this("Volume9::LockBox", 16);
    uint32_t gl = A(1), gbox = A(2), flags = A(3);
    D3DVOLUME_DESC vd;
    D3DLOCKED_BOX lb;
    HRESULT hr = v->GetDesc(&vd);
    if (SUCCEEDED(hr)) hr = v->LockBox(&lb, gbox ? reinterpret_cast<const D3DBOX *>(guest_base + gbox) : nullptr, flags);
    if (FAILED(hr)) return guest_ret((uint32_t)hr, 0, 16);
    uint32_t w, h, d;
    if (!box_dims(gbox, vd.Width, vd.Height, vd.Depth, &w, &h, &d) || !lock_box_common(v, 0, d, vd.Format, w, h, gl, lb, flags)) {
        v->UnlockBox();
        return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 16);
    }
    return guest_ret(0, 0, 16);
}
GuestRet m_IDirect3DVolume9_UnlockBox(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *v = static_cast<IDirect3DVolume9 *>(com_host_object_raw(A(0)));
    if (!v) return d3d9_bad_this("Volume9::UnlockBox", 4);
    end_lock(v, 0);
    return guest_ret((uint32_t)v->UnlockBox(), 0, 4);
}
GuestRet m_IDirect3DVolume9_GetContainer(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *v = static_cast<IDirect3DVolume9 *>(com_host_object_raw(A(0)));
    if (!v) return d3d9_bad_this("Volume9::GetContainer", 12);
    void *out = nullptr;
    HRESULT hr = v->GetContainer(*(const GUID *)(guest_base + A(1)), &out);
    if (FAILED(hr) || !out) { wr32(A(2), 0); return guest_ret((uint32_t)(FAILED(hr) ? hr : E_NOINTERFACE), 0, 12); }
    uint32_t g = d3d9_wrap(out, K_Unknown);
    wr32(A(2), g);
    return guest_ret(g ? 0 : (uint32_t)E_NOINTERFACE, 0, 12);
}

// ---- vertex / index buffers: Lock(this, OffsetToLock, SizeToLock, void **ppbData, Flags) -----------------------------------
template <class Buf, class Desc>
static GuestRet lock_buffer(uint32_t esp, const char *what) {
    auto *b = static_cast<Buf *>(com_host_object_raw(A(0)));
    if (!b) return d3d9_bad_this(what, 20);
    D3D9_TRACE(what, 4);
    uint32_t off = A(1), size = A(2), gp = A(3), flags = A(4);
    if (!gp) return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 20);
    Desc d;
    HRESULT hr = b->GetDesc(&d);
    if (FAILED(hr)) return guest_ret((uint32_t)hr, 0, 20);
    if (off > d.Size) return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, 20);
    uint32_t region = size ? size : d.Size - off;
    if (off + region > d.Size) region = d.Size - off;
    void *p = nullptr;
    hr = b->Lock(off, size, &p, flags);
    if (FAILED(hr)) { wr32(gp, 0); return guest_ret((uint32_t)hr, 0, 20); }
    uint32_t g = begin_lock(b, 0, static_cast<uint8_t *>(p), region, flags);
    if (!g) { b->Unlock(); return guest_ret((uint32_t)D3DERR_OUTOFVIDEOMEMORY, 0, 20); }
    wr32(gp, g);
    return guest_ret(0, 0, 20);
}
template <class Buf>
static GuestRet unlock_buffer(uint32_t esp, const char *what) {
    auto *b = static_cast<Buf *>(com_host_object_raw(A(0)));
    if (!b) return d3d9_bad_this(what, 4);
    end_lock(b, 0);
    return guest_ret((uint32_t)b->Unlock(), 0, 4);
}
GuestRet m_IDirect3DVertexBuffer9_Lock(uint32_t esp, uint32_t, uint32_t, uint32_t) { return lock_buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>(esp, "VertexBuffer9::Lock"); }
GuestRet m_IDirect3DVertexBuffer9_Unlock(uint32_t esp, uint32_t, uint32_t, uint32_t) { return unlock_buffer<IDirect3DVertexBuffer9>(esp, "VertexBuffer9::Unlock"); }
GuestRet m_IDirect3DIndexBuffer9_Lock(uint32_t esp, uint32_t, uint32_t, uint32_t) { return lock_buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>(esp, "IndexBuffer9::Lock"); }
GuestRet m_IDirect3DIndexBuffer9_Unlock(uint32_t esp, uint32_t, uint32_t, uint32_t) { return unlock_buffer<IDirect3DIndexBuffer9>(esp, "IndexBuffer9::Unlock"); }

}  // namespace dish
