// d3d9 -> DXVK-native: guest COM objects, wrapper identity map, IUnknown, Direct3DCreate9.
#include "d3d9_internal.h"
#include "dish/config.h"
#include "dish/hle.h"
#include "dish/mainthread.h"
#include "dish/platform.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/stat.h>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

#define D3D9_STATIC_ASSERT_SIZE(T, n) static_assert(sizeof(T) == (n), #T " differs from the 32-bit Windows layout")
D3D9_STATIC_ASSERT_SIZE(D3DCAPS9, 304);
static_assert(offsetof(D3DADAPTER_IDENTIFIER9, WHQLLevel) == 1096, "D3DADAPTER_IDENTIFIER9 prefix layout (the host struct is 4 bytes longer: bounce buffer in GetAdapterIdentifier)");
D3D9_STATIC_ASSERT_SIZE(D3DVIEWPORT9, 24);
D3D9_STATIC_ASSERT_SIZE(D3DSURFACE_DESC, 32);
D3D9_STATIC_ASSERT_SIZE(D3DVOLUME_DESC, 28);
D3D9_STATIC_ASSERT_SIZE(D3DVERTEXBUFFER_DESC, 24);
D3D9_STATIC_ASSERT_SIZE(D3DINDEXBUFFER_DESC, 20);
D3D9_STATIC_ASSERT_SIZE(D3DDISPLAYMODE, 16);
D3D9_STATIC_ASSERT_SIZE(D3DLIGHT9, 104);
D3D9_STATIC_ASSERT_SIZE(D3DMATERIAL9, 68);
D3D9_STATIC_ASSERT_SIZE(D3DVERTEXELEMENT9, 8);
D3D9_STATIC_ASSERT_SIZE(D3DMATRIX, 64);
D3D9_STATIC_ASSERT_SIZE(D3DRASTER_STATUS, 8);
D3D9_STATIC_ASSERT_SIZE(D3DCLIPSTATUS9, 8);
D3D9_STATIC_ASSERT_SIZE(D3DBOX, 24);
D3D9_STATIC_ASSERT_SIZE(D3DRECT, 16);
D3D9_STATIC_ASSERT_SIZE(D3DGAMMARAMP, 1536);
static_assert(sizeof(RECT) == 16 && sizeof(POINT) == 8 && sizeof(PALETTEENTRY) == 4, "RECT/POINT/PALETTEENTRY layout");

namespace dish {
namespace {

std::mutex g_mu;
std::unordered_map<void *, uint32_t> g_wrappers;       // host interface pointer -> guest object
uint32_t g_vtables[K_COUNT] = {};
bool g_defined = false;

void *g_lib = nullptr;
std::string g_lib_path;
IDirect3D9 *(*g_create9)(UINT) = nullptr;
std::atomic<uint64_t> g_unsupported_calls{0};

void destroy_wrapper(void *host) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_wrappers.erase(host);
    }
    static_cast<IUnknown *>(host)->Release();
}

}  // namespace

#include "d3d9_generated.inc"

void d3d9_define_interfaces() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_defined) return;
    g_defined = true;
    for (size_t i = 0; i < sizeof(kD3d9Ifaces) / sizeof(kD3d9Ifaces[0]); i++) {
        uint32_t vt = com_define_interface(kD3d9Ifaces[i].name, kD3d9Ifaces[i].methods, kD3d9Ifaces[i].n);
        g_vtables[kD3d9IfaceKinds[i]] = vt;
    }
    // base interfaces share the vtable of the concrete resource kinds they are asked for through QueryInterface: no objects of their own
    LOGD(LC_GFX, "d3d9: %zu interfaces defined", sizeof(kD3d9Ifaces) / sizeof(kD3d9Ifaces[0]));
}

uint32_t d3d9_slot(const char *iface, const char *method) {
    if (!strcmp(iface, "IUnknown")) return !strcmp(method, "QueryInterface") ? 0 : !strcmp(method, "AddRef") ? 1 : !strcmp(method, "Release") ? 2 : 0xFFFFFFFFu;
    for (auto &d : kD3d9Ifaces)
        if (!strcmp(d.name, iface))
            for (size_t i = 0; i < d.n; i++) if (!strcmp(d.methods[i].name, method)) return (uint32_t)i;
    return 0xFFFFFFFFu;
}

uint32_t d3d9_wrap(void *host, D3d9Kind kind) {
    if (!host) return 0;
    d3d9_define_interfaces();
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_wrappers.find(host);
    if (it != g_wrappers.end()) {
        com_addref(it->second);
        static_cast<IUnknown *>(host)->Release();   // the wrapper already owns one host reference
        return it->second;
    }
    if (kind == K_Unknown || !g_vtables[kind]) {
        LOGW(LC_GFX, "d3d9: cannot wrap an unknown object kind %d (%p)", (int)kind, host);
        static_cast<IUnknown *>(host)->Release();
        return 0;
    }
    uint32_t g = com_new_object(g_vtables[kind], host, destroy_wrapper);
    if (g) g_wrappers[host] = g;
    return g;
}

HWND d3d9_host_hwnd(uint32_t g) { return g ? reinterpret_cast<HWND>(platform_sdl_window(g)) : nullptr; }

GuestRet d3d9_unsupported(const char *what, int pop) {
    uint64_t n = g_unsupported_calls.fetch_add(1) + 1;
    if (n <= 20) LOGE(LC_GFX, "d3d9: UNSUPPORTED method %s called by the guest (returning D3DERR_NOTAVAILABLE)", what);
    return guest_ret((uint32_t)D3DERR_NOTAVAILABLE, 0, (uint32_t)pop);
}
GuestRet d3d9_bad_this(const char *what, int pop) {
    LOGE(LC_GFX, "d3d9: %s called on an invalid or released object", what);
    return guest_ret((uint32_t)D3DERR_INVALIDCALL, 0, (uint32_t)pop);
}
GuestRet d3d9_exception(const char *what, int pop) {
    LOGE(LC_GFX, "d3d9: C++ exception escaped from the DXVK call in %s", what);
    return guest_ret((uint32_t)D3DERR_DRIVERINTERNALERROR, 0, (uint32_t)pop);
}

// ---- IUnknown (shared by every interface) ------------------------------------------------------------------------------
GuestRet d3d9_QueryInterface(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    uint32_t self = A(0), riid = A(1), ppv = A(2);
    auto *host = static_cast<IUnknown *>(com_host_object_raw(self));
    if (!host) return d3d9_bad_this("QueryInterface", 12);
    if (!riid || !ppv) return guest_ret((uint32_t)E_POINTER, 0, 12);
    void *out = nullptr;
    HRESULT hr;
    try { hr = host->QueryInterface(*(const GUID *)(guest_base + riid), &out); } catch (...) { return d3d9_exception("QueryInterface", 12); }
    if (FAILED(hr) || !out) { wr32(ppv, 0); return guest_ret((uint32_t)(FAILED(hr) ? hr : E_NOINTERFACE), 0, 12); }
    uint32_t g = d3d9_wrap(out, K_Unknown);   // only objects we already wrapped are reachable (same interface family)
    if (!g) { wr32(ppv, 0); return guest_ret((uint32_t)E_NOINTERFACE, 0, 12); }
    wr32(ppv, g);
    return guest_ret(0, 0, 12);
}
GuestRet d3d9_AddRef(uint32_t esp, uint32_t, uint32_t, uint32_t) { return guest_ret(com_addref(A(0)), 0, 4); }
GuestRet d3d9_Release(uint32_t esp, uint32_t, uint32_t, uint32_t) { return guest_ret(com_release(A(0)), 0, 4); }

// ---- backends -----------------------------------------------------------------------------------------------------------
namespace {
std::vector<const D3d9Backend *> &backends() {
    static std::vector<const D3d9Backend *> v;   // function-local: registered from static initialisers
    return v;
}
const D3d9Backend *g_active = nullptr;
}  // namespace

void d3d9_register_backend(const D3d9Backend *b) { backends().push_back(b); }

const D3d9Backend *d3d9_active_backend() { return g_active; }

const char *d3d9_dxvk_library() { return g_lib_path.c_str(); }

namespace {
// DXVK-native: Linux reference backend. dlopen'ed (libdxvk_d3d9.so.0), or linked in when DISH_STATIC_DXVK.
IDirect3D9 *dxvk_create(UINT sdk) {
    static std::once_flag once;
    std::call_once(once, [] {
        setenv("DXVK_WSI_DRIVER", "SDL3", 0);   // DXVK >= 3 refuses to guess the window-system integration on non-Windows
        setenv("DXVK_LOG_LEVEL", "warn", 0);    // DXVK logs to stdout at "info" by default
        if (!config().save_dir.empty()) {       // pipeline cache lives with the game's other writable state
            std::string dir = config().save_dir + "/DXVK";
            mkdir(dir.c_str(), 0755);
            setenv("DXVK_STATE_CACHE_PATH", dir.c_str(), 0);
            setenv("DXVK_LOG_PATH", dir.c_str(), 0);
        }
#ifdef DISH_STATIC_DXVK
        // DXVK, SDL3 and MoltenVK are linked into the executable: no dlopen. (DISH_DXVK_LIB still forces a dynamic library.)
        if (!getenv("DISH_DXVK_LIB")) {
            g_create9 = &Direct3DCreate9;
            g_lib_path = "(static)";
            LOGI(LC_GFX, "using the statically linked DXVK d3d9");
            return;
        }
#endif
        std::vector<std::string> cand;
        if (const char *e = getenv("DISH_DXVK_LIB")) cand.push_back(e);
#ifdef __APPLE__
        cand.push_back("@executable_path/Frameworks/libdxvk_d3d9.0.dylib");
        cand.push_back("libdxvk_d3d9.0.dylib");
#else
        cand.push_back("libdxvk_d3d9.so.0");
#endif
#ifdef DISH_DXVK_DEFAULT_LIB
        cand.push_back(DISH_DXVK_DEFAULT_LIB);
#endif
        for (auto &c : cand) {
            g_lib = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (g_lib) { g_lib_path = c; break; }
        }
        if (!g_lib) { LOGE(LC_GFX, "cannot load DXVK-native d3d9 (%s); set DISH_DXVK_LIB", dlerror()); return; }
        g_create9 = reinterpret_cast<IDirect3D9 *(*)(UINT)>(dlsym(g_lib, "Direct3DCreate9"));
        if (!g_create9) LOGE(LC_GFX, "%s does not export Direct3DCreate9", g_lib_path.c_str());
        else LOGI(LC_GFX, "loaded %s", g_lib_path.c_str());
    });
    if (!g_create9) return nullptr;
    try { return g_create9(sdk); } catch (...) { LOGE(LC_GFX, "Direct3DCreate9 threw a C++ exception (check DXVK_WSI_DRIVER and the DXVK log)"); return nullptr; }
}
const char *dxvk_describe() { return g_lib_path.c_str(); }
const D3d9Backend g_dxvk_backend = {"dxvk", dxvk_create, nullptr, dxvk_describe};
struct RegisterDxvk { RegisterDxvk() { d3d9_register_backend(&g_dxvk_backend); } } g_register_dxvk;
}  // namespace

IDirect3D9 *d3d9_load_and_create(UINT sdk) {
    if (!g_active) {
        const char *want = getenv("DISH_D3D9_BACKEND");
#ifdef __APPLE__
        if (!want) want = "metal";   // iOS default; falls back to the first registered backend below
#endif
        for (const D3d9Backend *b : backends())
            if (!want || strcmp(want, b->name) == 0) { g_active = b; break; }
        if (!g_active && !getenv("DISH_D3D9_BACKEND") && !backends().empty()) g_active = backends().front();
        if (!g_active) { LOGE(LC_GFX, "no d3d9 backend named '%s' is built into this runtime", want ? want : "(default)"); return nullptr; }
        LOGI(LC_GFX, "d3d9 backend: %s", g_active->name);
    }
    return g_active->create(sdk);
}

namespace {
uint32_t d3d9_Direct3DCreate9(uint32_t sdk) {
    d3d9_define_interfaces();
    IDirect3D9 *d = nullptr;
    mainthread_call([&] { d = d3d9_load_and_create(sdk); });
    if (!d) return 0;
    return d3d9_wrap(d, K_D3D9);
}
int32_t d3d9_BeginEvent(uint32_t, uint32_t) { return -1; }
int32_t d3d9_EndEvent() { return -1; }
void d3d9_SetOptions(uint32_t) {}
void d3d9_SetMarker(uint32_t, uint32_t) {}
void d3d9_SetRegion(uint32_t, uint32_t) {}
uint32_t d3d9_GetStatus() { return 0; }
uint32_t d3d9_QueryRepeatFrame() { return 0; }
}  // namespace

D3d9Stats d3d9_stats();   // defined in d3d9_manual.cpp

HLE("d3d9.dll", "Direct3DCreate9", Stdcall, d3d9_Direct3DCreate9);
HLE("d3d9.dll", "D3DPERF_BeginEvent", Stdcall, d3d9_BeginEvent);
HLE("d3d9.dll", "D3DPERF_EndEvent", Stdcall, d3d9_EndEvent);
HLE("d3d9.dll", "D3DPERF_SetOptions", Stdcall, d3d9_SetOptions);
HLE("d3d9.dll", "D3DPERF_SetMarker", Stdcall, d3d9_SetMarker);
HLE("d3d9.dll", "D3DPERF_SetRegion", Stdcall, d3d9_SetRegion);
HLE("d3d9.dll", "D3DPERF_GetStatus", Stdcall, d3d9_GetStatus);
HLE("d3d9.dll", "D3DPERF_QueryRepeatFrame", Stdcall, d3d9_QueryRepeatFrame);

}  // namespace dish
