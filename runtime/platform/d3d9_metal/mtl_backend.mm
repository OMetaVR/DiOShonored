// Metal backend: IDirect3D9 (adapter / format / caps queries, CreateDevice), backend registration, statistics.
#include "mtl_device.h"
#include "../d3d9_backend.h"
#include "dish/mainthread.h"
#include <algorithm>
#include <cstdio>

namespace dish {
namespace mtl {

#include "caps_table.inc"
void fill_caps_table(D3DCAPS9 &c) { fill_caps(c); }

static id<MTLDevice> query_device() {
    static id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    return d;
}

MtlD3D9::MtlD3D9() {}

void MtlD3D9::default_modes(std::vector<std::pair<int, int>> *out) {
    static std::vector<std::pair<int, int>> modes;
    static std::once_flag once;
    std::call_once(once, [] {
        double aspect = 16.0 / 9.0;
        mainthread_call([&] {
            if (!SDL_WasInit(SDL_INIT_VIDEO)) return;
            SDL_DisplayID id = SDL_GetPrimaryDisplay();
            SDL_Rect r;
            if (id && SDL_GetDisplayBounds(id, &r) && r.w > 0 && r.h > 0) aspect = (double)std::max(r.w, r.h) / std::min(r.w, r.h);
        });
        // One mode only: with several, Dishonored first takes the one nearest its INI resolution (1920x1080 -> 1168x540) while
        // laying out the frame for the virtual desktop, so the menu was drawn into a corner until a later Reset fixed the size.
        // The render size is chosen with DISH_METAL_INTERNAL_HEIGHT instead.
        int heights[] = {options().internal_height};
        for (int h : heights) {
            if (h <= 0) continue;
            int w = ((int)(h * aspect + 0.5)) & ~1;
            std::pair<int, int> m(w, h);
            if (std::find(modes.begin(), modes.end(), m) == modes.end()) modes.push_back(m);
        }
    });
    *out = modes;
}

HRESULT MtlD3D9::GetAdapterIdentifier(UINT adapter, DWORD, D3DADAPTER_IDENTIFIER9 *id) {
    if (adapter != 0 || !id) return D3DERR_INVALIDCALL;
    memset(id, 0, sizeof *id);
    snprintf(id->Driver, sizeof id->Driver, "dish_metal.dll");
    snprintf(id->Description, sizeof id->Description, "%s (Metal)", [[query_device().name copy] UTF8String]);
    snprintf(id->DeviceName, sizeof id->DeviceName, "\\\\.\\DISPLAY1");
    id->DriverVersion.QuadPart = 0x0001000000010000ll;
    id->VendorId = 0x106B;    // Apple
    id->DeviceId = 0x0001;
    id->SubSysId = 0;
    id->Revision = 1;
    id->DeviceIdentifier = GUID{0x6D657461, 0x6C64, 0x3964, {0x41, 0x31, 0x35, 0x00, 0x00, 0x00, 0x00, 0x01}};
    id->WHQLLevel = 1;
    return D3D_OK;
}
UINT MtlD3D9::GetAdapterModeCount(UINT adapter, D3DFORMAT fmt) {
    if (adapter != 0 || (fmt != D3DFMT_X8R8G8B8 && fmt != D3DFMT_A8R8G8B8)) return 0;
    std::vector<std::pair<int, int>> m;
    default_modes(&m);
    return (UINT)m.size();
}
HRESULT MtlD3D9::EnumAdapterModes(UINT adapter, D3DFORMAT fmt, UINT idx, D3DDISPLAYMODE *mode) {
    if (adapter != 0 || !mode || (fmt != D3DFMT_X8R8G8B8 && fmt != D3DFMT_A8R8G8B8)) return D3DERR_INVALIDCALL;
    std::vector<std::pair<int, int>> m;
    default_modes(&m);
    if (idx >= m.size()) return D3DERR_INVALIDCALL;
    mode->Width = (UINT)m[idx].first;
    mode->Height = (UINT)m[idx].second;
    mode->RefreshRate = 60;
    mode->Format = fmt;
    return D3D_OK;
}
HRESULT MtlD3D9::GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE *mode) {
    if (adapter != 0 || !mode) return D3DERR_INVALIDCALL;
    return EnumAdapterModes(0, D3DFMT_X8R8G8B8, 0, mode);
}
HRESULT MtlD3D9::CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_fmt, D3DFORMAT back_fmt, WINBOOL) {
    if (adapter != 0 || type != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    bool a = adapter_fmt == D3DFMT_X8R8G8B8 || adapter_fmt == D3DFMT_A8R8G8B8 || adapter_fmt == D3DFMT_UNKNOWN;
    bool b = back_fmt == D3DFMT_X8R8G8B8 || back_fmt == D3DFMT_A8R8G8B8 || back_fmt == D3DFMT_UNKNOWN;
    return a && b ? D3D_OK : D3DERR_NOTAVAILABLE;
}

HRESULT MtlD3D9::CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT, DWORD usage, D3DRESOURCETYPE rtype, D3DFORMAT check) {
    if (adapter != 0 || type != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    if (rtype == D3DRTYPE_VERTEXBUFFER || rtype == D3DRTYPE_INDEXBUFFER) return D3D_OK;
    const FormatInfo *fi = format_info(check);
    if (!fi) return D3DERR_NOTAVAILABLE;
    const bool is_tex = rtype == D3DRTYPE_TEXTURE || rtype == D3DRTYPE_CUBETEXTURE || rtype == D3DRTYPE_VOLUMETEXTURE;
    if (usage & D3DUSAGE_DEPTHSTENCIL) {
        if (!fi->depth) return D3DERR_NOTAVAILABLE;
        if (is_tex && !(check == (D3DFORMAT)0x5A544E49 || check == (D3DFORMAT)0x34324644 || check == (D3DFORMAT)0x36314644)) return D3DERR_NOTAVAILABLE;   // INTZ / DF24 / DF16 only
        return D3D_OK;
    }
    if (fi->depth) {   // depth formats as plain textures: only the readable FourCC variants
        if (is_tex && (check == (D3DFORMAT)0x5A544E49 || check == (D3DFORMAT)0x34324644 || check == (D3DFORMAT)0x36314644) && !(usage & D3DUSAGE_RENDERTARGET)) return D3D_OK;
        return D3DERR_NOTAVAILABLE;
    }
    if (usage & D3DUSAGE_RENDERTARGET) {
        if (!fi->rt_ok || rtype == D3DRTYPE_VOLUMETEXTURE) return D3DERR_NOTAVAILABLE;
    }
    if ((usage & D3DUSAGE_AUTOGENMIPMAP) && (fi->block || !fi->rt_ok)) return D3DERR_NOTAVAILABLE;
    if (rtype == D3DRTYPE_VOLUMETEXTURE && fi->block) return D3DERR_NOTAVAILABLE;
    if (usage & D3DUSAGE_QUERY_FILTER) {
        bool f = fi->filter_ok;
        if (!f) {
            if (@available(iOS 14.0, *)) f = query_device().supports32BitFloatFiltering;
        }
        if (!f) return D3DERR_NOTAVAILABLE;
    }
    if (usage & D3DUSAGE_QUERY_SRGBREAD) {
        if (!(check == D3DFMT_A8R8G8B8 || check == D3DFMT_X8R8G8B8 || check == D3DFMT_A8B8G8R8 || check == D3DFMT_DXT1 || check == D3DFMT_DXT3 || check == D3DFMT_DXT5 || check == D3DFMT_L8)) return D3DERR_NOTAVAILABLE;
    }
    if (usage & D3DUSAGE_QUERY_SRGBWRITE) {
        if (!(check == D3DFMT_A8R8G8B8 || check == D3DFMT_X8R8G8B8)) return D3DERR_NOTAVAILABLE;
    }
    if (usage & D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING) {
        if (!fi->rt_ok || check == D3DFMT_R32F || check == D3DFMT_G32R32F || check == D3DFMT_A32B32G32R32F) return D3DERR_NOTAVAILABLE;
    }
    if (usage & D3DUSAGE_QUERY_VERTEXTEXTURE) {
        if (!(check == D3DFMT_A32B32G32R32F || check == D3DFMT_R32F || check == D3DFMT_G32R32F || check == D3DFMT_A16B16G16R16F || check == D3DFMT_A8R8G8B8)) return D3DERR_NOTAVAILABLE;
    }
    return D3D_OK;
}

HRESULT MtlD3D9::CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE, D3DFORMAT, WINBOOL, D3DMULTISAMPLE_TYPE ms, DWORD *quality) {
    if (adapter != 0) return D3DERR_INVALIDCALL;
    if (ms == D3DMULTISAMPLE_NONE || ms == D3DMULTISAMPLE_NONMASKABLE) { if (quality) *quality = 1; return D3D_OK; }
    return D3DERR_NOTAVAILABLE;
}
HRESULT MtlD3D9::CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE, D3DFORMAT, D3DFORMAT rt_fmt, D3DFORMAT ds_fmt) {
    if (adapter != 0) return D3DERR_INVALIDCALL;
    const FormatInfo *r = format_info(rt_fmt), *d = format_info(ds_fmt);
    return r && d && d->depth ? D3D_OK : D3DERR_NOTAVAILABLE;
}
HRESULT MtlD3D9::GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9 *caps) {
    if (adapter != 0 || !caps) return D3DERR_INVALIDCALL;
    fill_caps_table(*caps);
    caps->DeviceType = type;
    return D3D_OK;
}

HRESULT MtlD3D9::CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) {
    if (adapter != 0 || !pp || !out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    MtlDevice *d = new MtlDevice();
    if (!d->init(this, adapter, type, focus, behavior, pp)) {
        LOGE(LC_GFX, "metal: device initialisation failed");
        d->rc_.store(1);
        d->destroy();
        return D3DERR_NOTAVAILABLE;
    }
    *out = d;
    return D3D_OK;
}

// ---- registration ---------------------------------------------------------------------------------------------------------------------------------
static IDirect3D9 *metal_create(UINT) {
    if (!query_device()) { LOGE(LC_GFX, "metal: this device has no Metal GPU"); return nullptr; }
    return new MtlD3D9();
}
static const char *metal_describe() {
    static std::string s;
    s = std::string("Metal: ") + [[query_device().name copy] UTF8String];
    return s.c_str();
}
static const D3d9Backend g_metal_backend = {"metal", metal_create, nullptr, metal_describe};
static struct RegisterMetal {
    RegisterMetal() { d3d9_register_backend(&g_metal_backend); }
} g_register_metal;

// ---- statistics -------------------------------------------------------------------------------------------------------------------------------------
void gpu_stats_json(std::string *out) {
    MtlStats &s = stats();
    MemStats ms = memacct_query();
    char b[2048];
    snprintf(b, sizeof b,
             "{\"draws\":%llu,\"clears\":%llu,\"frames\":%llu,\"command_buffers\":%llu,\"pipelines_created\":%llu,\"pipeline_archive_hits\":%llu,\"pipeline_create_ms\":%.3f,"
             "\"shaders_created\":%llu,\"shader_failures\":%llu,\"shader_translate_ms\":%.3f,\"shader_compile_ms\":%.3f,"
             "\"uploads\":%llu,\"upload_bytes\":%llu,\"readbacks\":%llu,\"ring_bytes\":%llu,"
             "\"decode_in_bytes\":%llu,\"decode_ms\":%.3f,\"transcode_in_bytes\":%llu,\"transcode_blocks\":%llu,\"transcode_ms\":%.3f,"
             "\"gpu_bytes_textures\":%llu,\"gpu_bytes_buffers\":%llu,\"footprint\":%llu,\"os_available\":%llu}",
             (unsigned long long)s.draws.load(), (unsigned long long)s.clears.load(), (unsigned long long)s.frames.load(), (unsigned long long)s.command_buffers.load(),
             (unsigned long long)s.pipelines_created.load(), (unsigned long long)s.pipeline_archive_hits.load(), s.pipeline_create_ns.load() / 1e6,
             (unsigned long long)s.shaders_created.load(), (unsigned long long)s.shader_failures.load(), s.shader_translate_ns.load() / 1e6, s.shader_compile_ns.load() / 1e6,
             (unsigned long long)s.uploads.load(), (unsigned long long)s.upload_bytes.load(), (unsigned long long)s.readbacks.load(), (unsigned long long)s.ring_bytes.load(),
             (unsigned long long)s.decode_in_bytes.load(), s.decode_ns.load() / 1e6, (unsigned long long)s.transcode_in_bytes.load(), (unsigned long long)s.transcode_blocks.load(),
             s.transcode_ns.load() / 1e6, (unsigned long long)s.gpu_bytes_textures.load(), (unsigned long long)s.gpu_bytes_buffers.load(), (unsigned long long)ms.footprint,
             (unsigned long long)ms.os_available);
    *out = b;
}

}  // namespace mtl
}  // namespace dish

extern "C" int dish_metal_stats_json(char *buf, size_t n) {
    std::string s;
    dish::mtl::gpu_stats_json(&s);
    if (buf && n) snprintf(buf, n, "%s", s.c_str());
    return (int)s.size();
}
