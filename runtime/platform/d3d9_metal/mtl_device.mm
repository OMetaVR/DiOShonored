// Metal backend: the IDirect3DDevice9 API surface (resource creation, state setters, clears, copies, queries).
#include "mtl_device.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dish {
namespace mtl {

// ---- default device state ---------------------------------------------------------------------------------------------------------------
DeviceState::DeviceState() {
    memset(rs, 0, sizeof rs);
    memset(ss, 0, sizeof ss);
    memset(texdata, 0, sizeof texdata);
    memset(clip, 0, sizeof clip);
    memset(xform, 0, sizeof xform);
    for (auto &m : xform) m._11 = m._22 = m._33 = m._44 = 1.0f;
    rs[D3DRS_ZENABLE] = D3DZB_TRUE;
    rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
    rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    rs[D3DRS_ZWRITEENABLE] = TRUE;
    rs[D3DRS_LASTPIXEL] = TRUE;
    rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILFAIL] = rs[D3DRS_STENCILZFAIL] = rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILMASK] = 0xFFFFFFFFu;
    rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFFu;
    rs[D3DRS_CCW_STENCILFAIL] = rs[D3DRS_CCW_STENCILZFAIL] = rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_COLORWRITEENABLE] = rs[D3DRS_COLORWRITEENABLE1] = rs[D3DRS_COLORWRITEENABLE2] = rs[D3DRS_COLORWRITEENABLE3] = 0xF;
    rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
    rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    rs[D3DRS_BLENDFACTOR] = 0xFFFFFFFFu;
    rs[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
    rs[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFFu;
    rs[D3DRS_POINTSIZE] = 0x3F800000;   // 1.0f
    rs[D3DRS_POINTSIZE_MAX] = 0x42800000;
    for (int s = 0; s < kMaxSamplers; s++) {
        ss[s][D3DSAMP_ADDRESSU] = ss[s][D3DSAMP_ADDRESSV] = ss[s][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        ss[s][D3DSAMP_MAGFILTER] = ss[s][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        ss[s][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        ss[s][D3DSAMP_MAXANISOTROPY] = 1;
    }
    for (int i = 0; i < 16; i++) streams[i].freq = 1;
}

static inline int sampler_slot(DWORD s) {
    if (s < 16) return (int)s;
    if (s >= D3DVERTEXTEXTURESAMPLER0 && s <= D3DVERTEXTEXTURESAMPLER3) return 16 + (int)(s - D3DVERTEXTEXTURESAMPLER0);
    return -1;
}

static TexData *tex_data_of(IDirect3DBaseTexture9 *b) {
    if (!b) return nullptr;
    switch (b->GetType()) {
        case D3DRTYPE_TEXTURE: return &static_cast<MtlTexture2D *>(static_cast<IDirect3DTexture9 *>(b))->td;
        case D3DRTYPE_CUBETEXTURE: return &static_cast<MtlCubeTexture *>(static_cast<IDirect3DCubeTexture9 *>(b))->td;
        case D3DRTYPE_VOLUMETEXTURE: return &static_cast<MtlVolumeTexture *>(static_cast<IDirect3DVolumeTexture9 *>(b))->td;
        default: return nullptr;
    }
}

// ---- device queries -------------------------------------------------------------------------------------------------------------------------
UINT MtlDevice::GetAvailableTextureMem() {
    MemStats ms = memacct_query();
    uint64_t budget = ms.budget ? ms.budget : (1800ull << 20);
    uint64_t used = ms.gpu_reported;
    return (UINT)std::min<uint64_t>(used < budget ? budget - used : 0, 0x7FFFFFFFu);
}
HRESULT MtlDevice::GetDirect3D(IDirect3D9 **pp) {
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = parent_;
    parent_->AddRef();
    return D3D_OK;
}
HRESULT MtlDevice::GetDeviceCaps(D3DCAPS9 *caps) {
    if (!caps) return D3DERR_INVALIDCALL;
    *caps = caps_;
    return D3D_OK;
}
HRESULT MtlDevice::GetDisplayMode(UINT, D3DDISPLAYMODE *m) {
    m->Width = pp_.BackBufferWidth;
    m->Height = pp_.BackBufferHeight;
    m->RefreshRate = 60;
    m->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}
HRESULT MtlDevice::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *p) {
    *p = cp_;
    return D3D_OK;
}
HRESULT MtlDevice::GetSwapChain(UINT n, IDirect3DSwapChain9 **pp) {
    if (n != 0 || !pp) return D3DERR_INVALIDCALL;
    *pp = swapchain_;
    swapchain_->AddRef();
    return D3D_OK;
}
void MtlDevice::GetGammaRamp(UINT, D3DGAMMARAMP *r) {
    for (int i = 0; i < 256; i++) r->red[i] = r->green[i] = r->blue[i] = (WORD)(i * 257);
}

HRESULT MtlDevice::Reset(D3DPRESENT_PARAMETERS *pp) {
    if (!pp) return D3DERR_INVALIDCALL;
    flush(true);
    release_backbuffer();
    pp_ = *pp;
    if (pp->hDeviceWindow && reinterpret_cast<SDL_Window *>(pp->hDeviceWindow) != window_) LOGW(LC_GFX, "metal: Reset with a different window is not supported");
    st_ = DeviceState();
    ap_ = Applied();
    if (!create_backbuffer()) return D3DERR_DRIVERINTERNALERROR;
    *pp = pp_;
    configure_layer();
    LOGI(LC_GFX, "metal: Reset -> %ux%u", pp_.BackBufferWidth, pp_.BackBufferHeight);
    return D3D_OK;
}

HRESULT MtlDevice::Present(const RECT *, const RECT *, HWND, const RGNDATA *) { return do_present(); }

HRESULT MtlDevice::GetBackBuffer(UINT sc, UINT idx, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **pp) {
    if (!pp || sc != 0 || idx != 0 || !backbuffer_) return D3DERR_INVALIDCALL;
    *pp = backbuffer_.get();
    (*pp)->AddRef();
    return D3D_OK;
}

// ---- resource creation ----------------------------------------------------------------------------------------------------------------------
HRESULT MtlDevice::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **pp, HANDLE *) {
    if (!pp || !w || !h) return D3DERR_INVALIDCALL;
    *pp = nullptr;
    if (!format_info(fmt)) { LOGW(LC_GFX, "metal: CreateTexture: unsupported format %u", (unsigned)fmt); return D3DERR_INVALIDCALL; }
    MtlTexture2D *t = new MtlTexture2D();
    if (!t->td.create(this, fmt, w, h, 1, levels, usage, pool, false, false)) { t->td.release(); delete t; return D3DERR_OUTOFVIDEOMEMORY; }
    *pp = t;
    return D3D_OK;
}
HRESULT MtlDevice::CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9 **pp, HANDLE *) {
    if (!pp || !w || !h || !d) return D3DERR_INVALIDCALL;
    *pp = nullptr;
    if (!format_info(fmt)) return D3DERR_INVALIDCALL;
    MtlVolumeTexture *t = new MtlVolumeTexture();
    if (!t->td.create(this, fmt, w, h, d, levels, usage, pool, false, true)) { t->td.release(); delete t; return D3DERR_OUTOFVIDEOMEMORY; }
    *pp = t;
    return D3D_OK;
}
HRESULT MtlDevice::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9 **pp, HANDLE *) {
    if (!pp || !edge) return D3DERR_INVALIDCALL;
    *pp = nullptr;
    if (!format_info(fmt)) return D3DERR_INVALIDCALL;
    MtlCubeTexture *t = new MtlCubeTexture();
    if (!t->td.create(this, fmt, edge, edge, 1, levels, usage, pool, true, false)) { t->td.release(); delete t; return D3DERR_OUTOFVIDEOMEMORY; }
    *pp = t;
    return D3D_OK;
}
HRESULT MtlDevice::CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **pp, HANDLE *) {
    if (!pp || !len) return D3DERR_INVALIDCALL;
    *pp = nullptr;
    MtlVertexBuffer *b = new MtlVertexBuffer();
    if (!b->core.create(this, len, usage, pool)) { delete b; return D3DERR_OUTOFVIDEOMEMORY; }
    b->fvf = fvf;
    *pp = b;
    return D3D_OK;
}
HRESULT MtlDevice::CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **pp, HANDLE *) {
    if (!pp || !len || (fmt != D3DFMT_INDEX16 && fmt != D3DFMT_INDEX32)) return D3DERR_INVALIDCALL;
    *pp = nullptr;
    MtlIndexBuffer *b = new MtlIndexBuffer();
    if (!b->core.create(this, len, usage, pool)) { delete b; return D3DERR_OUTOFVIDEOMEMORY; }
    b->fmt = fmt;
    *pp = b;
    return D3D_OK;
}

static MtlSurface *make_standalone(MtlDevice *dev, D3DFORMAT fmt, uint32_t w, uint32_t h, DWORD usage, D3DPOOL pool, D3DMULTISAMPLE_TYPE ms) {
    if (!format_info(fmt)) return nullptr;
    MtlSurface *s = new MtlSurface();
    s->own.reset(new TexData());
    s->td = s->own.get();
    s->ms = ms;
    if (!s->own->create(dev, fmt, w, h, 1, 1, usage, pool, false, false)) { s->own->release(); delete s; return nullptr; }
    return s;
}
HRESULT MtlDevice::CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD, WINBOOL, IDirect3DSurface9 **pp, HANDLE *) {
    if (!pp) return D3DERR_INVALIDCALL;
    if (ms != D3DMULTISAMPLE_NONE) LOGW(LC_GFX, "metal: multisampled render targets are not supported (%d), creating a single-sample target", (int)ms);
    MtlSurface *s = make_standalone(this, fmt, w, h, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, D3DMULTISAMPLE_NONE);
    if (!s) return D3DERR_INVALIDCALL;
    *pp = s;
    return D3D_OK;
}
HRESULT MtlDevice::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD, WINBOOL, IDirect3DSurface9 **pp, HANDLE *) {
    if (!pp) return D3DERR_INVALIDCALL;
    MtlSurface *s = make_standalone(this, fmt, w, h, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, D3DMULTISAMPLE_NONE);
    if (!s) return D3DERR_INVALIDCALL;
    *pp = s;
    return D3D_OK;
}
HRESULT MtlDevice::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool, IDirect3DSurface9 **pp, HANDLE *) {
    if (!pp) return D3DERR_INVALIDCALL;
    MtlSurface *s = make_standalone(this, fmt, w, h, 0, pool, D3DMULTISAMPLE_NONE);
    if (!s) return D3DERR_INVALIDCALL;
    *pp = s;
    return D3D_OK;
}

HRESULT MtlDevice::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **pp) {
    if (!e || !pp) return D3DERR_INVALIDCALL;
    MtlVertexDecl *d = new MtlVertexDecl();
    d->dev = this;
    uint64_t h = 1469598103934665603ull;
    auto size_of = [](uint8_t type) -> uint32_t {
        switch (type) {
            case D3DDECLTYPE_FLOAT1: return 4; case D3DDECLTYPE_FLOAT2: return 8; case D3DDECLTYPE_FLOAT3: return 12; case D3DDECLTYPE_FLOAT4: return 16;
            case D3DDECLTYPE_D3DCOLOR: case D3DDECLTYPE_UBYTE4: case D3DDECLTYPE_SHORT2: case D3DDECLTYPE_UBYTE4N: case D3DDECLTYPE_SHORT2N:
            case D3DDECLTYPE_USHORT2N: case D3DDECLTYPE_UDEC3: case D3DDECLTYPE_DEC3N: case D3DDECLTYPE_FLOAT16_2: return 4;
            case D3DDECLTYPE_SHORT4: case D3DDECLTYPE_SHORT4N: case D3DDECLTYPE_USHORT4N: case D3DDECLTYPE_FLOAT16_4: return 8;
            default: return 16;
        }
    };
    for (int i = 0; i < 64; i++) {
        if (e[i].Stream == 0xFF) break;
        d->elems.push_back(e[i]);
        const uint8_t *p = (const uint8_t *)&e[i];
        for (int k = 0; k < 8; k++) { h ^= p[k]; h *= 1099511628211ull; }
        if (e[i].Stream < 16) d->stream_min_stride[e[i].Stream] = std::max<uint32_t>(d->stream_min_stride[e[i].Stream], e[i].Offset + size_of(e[i].Type));
    }
    d->hash = h;
    *pp = d;
    return D3D_OK;
}
HRESULT MtlDevice::SetVertexDeclaration(IDirect3DVertexDeclaration9 *d) {
    st_.decl = Ref<MtlVertexDecl>(static_cast<MtlVertexDecl *>(d));
    note_state(SC_DECL);
    return D3D_OK;
}
HRESULT MtlDevice::GetVertexDeclaration(IDirect3DVertexDeclaration9 **pp) {
    *pp = st_.decl.get();
    if (*pp) (*pp)->AddRef();
    return D3D_OK;
}

HRESULT MtlDevice::CreateVertexShader(const DWORD *code, IDirect3DVertexShader9 **pp) {
    if (!code || !pp) return D3DERR_INVALIDCALL;
    MtlVertexShader *s = new MtlVertexShader();
    s->sh.start(this, code, false);
    if (s->sh.state.load() == 2) { delete s; return D3DERR_INVALIDCALL; }
    *pp = s;
    return D3D_OK;
}
HRESULT MtlDevice::CreatePixelShader(const DWORD *code, IDirect3DPixelShader9 **pp) {
    if (!code || !pp) return D3DERR_INVALIDCALL;
    MtlPixelShader *s = new MtlPixelShader();
    s->sh.start(this, code, true);
    if (s->sh.state.load() == 2) { delete s; return D3DERR_INVALIDCALL; }
    *pp = s;
    return D3D_OK;
}
HRESULT MtlDevice::SetVertexShader(IDirect3DVertexShader9 *s) {
    st_.vs = Ref<MtlVertexShader>(static_cast<MtlVertexShader *>(s));
    vs_cver_++;
    note_state(SC_VS);
    return D3D_OK;
}
HRESULT MtlDevice::GetVertexShader(IDirect3DVertexShader9 **pp) { *pp = st_.vs.get(); if (*pp) (*pp)->AddRef(); return D3D_OK; }
HRESULT MtlDevice::SetPixelShader(IDirect3DPixelShader9 *s) {
    st_.ps = Ref<MtlPixelShader>(static_cast<MtlPixelShader *>(s));
    ps_cver_++;
    note_state(SC_PS);
    return D3D_OK;
}
HRESULT MtlDevice::GetPixelShader(IDirect3DPixelShader9 **pp) { *pp = st_.ps.get(); if (*pp) (*pp)->AddRef(); return D3D_OK; }

HRESULT MtlDevice::CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **pp) {
    if (type != D3DQUERYTYPE_EVENT && type != D3DQUERYTYPE_OCCLUSION) return D3DERR_NOTAVAILABLE;
    if (!pp) return D3D_OK;   // capability probe
    MtlQuery *q = new MtlQuery();
    q->dev = this;
    q->type = type;
    *pp = q;
    return D3D_OK;
}

DWORD MtlQuery::GetDataSize() { return 4; }
HRESULT MtlQuery::Issue(DWORD flags) {
    if (type == D3DQUERYTYPE_EVENT) {
        if (flags & D3DISSUE_END) { dev->query_issue_end(this); issued = true; }
        return D3D_OK;
    }
    if (flags & D3DISSUE_BEGIN) {
        if (!dev->query_occlusion_slot(this)) return D3D_OK;
        began = true;
        dev->occ_begin(this);
    }
    if (flags & D3DISSUE_END) {
        if (began) dev->occ_end(this);
        dev->query_issue_end(this);
        issued = true;
    }
    return D3D_OK;
}
HRESULT MtlQuery::GetData(void *data, DWORD size, DWORD flags) {
    if (!issued) {
        if (data && size >= 4) *(DWORD *)data = type == D3DQUERYTYPE_EVENT ? TRUE : 0;
        return D3D_OK;
    }
    if (!dev->query_poll(this, (flags & D3DGETDATA_FLUSH) != 0)) return S_FALSE;
    if (data && size >= 4) {
        if (type == D3DQUERYTYPE_EVENT) *(DWORD *)data = TRUE;
        else *(DWORD *)data = (DWORD)std::min<uint64_t>(began ? dev->occ_result(this) : 0, 0xFFFFFFFFu);
    }
    return D3D_OK;
}

void MtlDevice::occ_begin(MtlQuery *q) {
    active_occ_ = q;
    if (enc_) [enc_ setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(NSUInteger)q->vis_slot * 8];
}
void MtlDevice::occ_end(MtlQuery *q) {
    if (active_occ_ == q) active_occ_ = nullptr;
    if (enc_) [enc_ setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
}
uint64_t MtlDevice::occ_result(MtlQuery *q) {
    if (q->vis_slot < 0 || q->vis_frame < 0) return 0;
    id<MTLBuffer> v = frames_[q->vis_frame].vis;
    return ((const uint64_t *)v.contents)[q->vis_slot];
}

// ---- render targets / viewport -----------------------------------------------------------------------------------------------------------------
HRESULT MtlDevice::SetRenderTarget(DWORD idx, IDirect3DSurface9 *rt) {
    if (idx >= 4) return D3DERR_INVALIDCALL;
    MtlSurface *s = static_cast<MtlSurface *>(rt);
    if (idx == 0 && !s) return D3DERR_INVALIDCALL;
    if (s && !s->td->gpu) return D3DERR_INVALIDCALL;
    if (rt_[idx].get() == s) return D3D_OK;
    end_pass();
    rt_[idx] = Ref<MtlSurface>(s);
    if (idx == 0 && s) {
        st_.vp = D3DVIEWPORT9{0, 0, s->width(), s->height(), 0.0f, 1.0f};
        st_.scissor = RECT{0, 0, (LONG)s->width(), (LONG)s->height()};
    }
    return D3D_OK;
}
HRESULT MtlDevice::GetRenderTarget(DWORD idx, IDirect3DSurface9 **pp) {
    if (idx >= 4 || !pp) return D3DERR_INVALIDCALL;
    *pp = rt_[idx].get();
    if (!*pp) return D3DERR_NOTFOUND;
    (*pp)->AddRef();
    return D3D_OK;
}
HRESULT MtlDevice::SetDepthStencilSurface(IDirect3DSurface9 *ds) {
    MtlSurface *s = static_cast<MtlSurface *>(ds);
    if (ds_.get() == s) return D3D_OK;
    end_pass();
    ds_ = Ref<MtlSurface>(s);
    return D3D_OK;
}
HRESULT MtlDevice::GetDepthStencilSurface(IDirect3DSurface9 **pp) {
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = ds_.get();
    if (!*pp) return D3DERR_NOTFOUND;
    (*pp)->AddRef();
    return D3D_OK;
}
HRESULT MtlDevice::SetViewport(const D3DVIEWPORT9 *vp) {
    st_.vp = *vp;
    note_state(SC_VIEWPORT);
    return D3D_OK;
}

// ---- clears -----------------------------------------------------------------------------------------------------------------------------------------
static void color_to_float(D3DCOLOR c, float out[4]) {
    out[0] = ((c >> 16) & 255) / 255.0f;
    out[1] = ((c >> 8) & 255) / 255.0f;
    out[2] = (c & 255) / 255.0f;
    out[3] = ((c >> 24) & 255) / 255.0f;
}

HRESULT MtlDevice::Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    stats().clears++;
    MtlSurface *ref = rt_[0].get() ? rt_[0].get() : ds_.get();
    if (!ref) return D3D_OK;
    const bool wantC = (flags & D3DCLEAR_TARGET) && rt_[0];
    const bool wantZ = (flags & D3DCLEAR_ZBUFFER) && ds_;
    const bool wantS = (flags & D3DCLEAR_STENCIL) && ds_ && ds_->td->fi->stencil;
    if (!wantC && !wantZ && !wantS) return D3D_OK;
    const uint32_t W = ref->width(), H = ref->height();
    bool full = (count == 0 || !rects);
    if (full) {
        const D3DVIEWPORT9 &v = st_.vp;
        full = v.X == 0 && v.Y == 0 && v.Width >= W && v.Height >= H;
        if (full && st_.rs[D3DRS_SCISSORTESTENABLE]) {
            const RECT &r = st_.scissor;
            full = r.left <= 0 && r.top <= 0 && (uint32_t)r.right >= W && (uint32_t)r.bottom >= H;
        }
    }
    if (full) {
        // a full-surface clear becomes the load action of the next render pass
        if (enc_) end_pass();
        if (wantC) { pend_.color = true; color_to_float(color, pend_.c); }
        if (wantZ) { pend_.depth = true; pend_.z = std::min(1.0f, std::max(0.0f, z)); }
        if (wantS) { pend_.stencil = true; pend_.s = stencil & 0xFF; }
        return D3D_OK;
    }
    if (count == 0 || !rects) {
        RECT r{(LONG)st_.vp.X, (LONG)st_.vp.Y, (LONG)(st_.vp.X + st_.vp.Width), (LONG)(st_.vp.Y + st_.vp.Height)};
        draw_clear_quad(flags, color, z, stencil, &r);
    } else {
        for (DWORD i = 0; i < count; i++) {
            RECT r{rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2};
            draw_clear_quad(flags, color, z, stencil, &r);
        }
    }
    return D3D_OK;
}

// ---- state setters ---------------------------------------------------------------------------------------------------------------------------------
HRESULT MtlDevice::SetRenderState(D3DRENDERSTATETYPE s, DWORD v) {
    if ((unsigned)s >= kNumRS) return D3DERR_INVALIDCALL;
    st_.rs[s] = v;
    if (s == D3DRS_ALPHAREF || s == D3DRS_ALPHAFUNC || s == D3DRS_ALPHATESTENABLE) ps_cver_++;
    note_state(SC_RENDER);
    return D3D_OK;
}
HRESULT MtlDevice::GetRenderState(D3DRENDERSTATETYPE s, DWORD *v) {
    if ((unsigned)s >= kNumRS) return D3DERR_INVALIDCALL;
    *v = st_.rs[s];
    return D3D_OK;
}
HRESULT MtlDevice::SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) {
    int slot = sampler_slot(s);
    if (slot < 0 || (unsigned)t >= 14) return D3D_OK;
    st_.ss[slot][t] = v;
    note_state(SC_SAMPLER);
    return D3D_OK;
}
HRESULT MtlDevice::GetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD *v) {
    int slot = sampler_slot(s);
    if (slot < 0 || (unsigned)t >= 14) return D3DERR_INVALIDCALL;
    *v = st_.ss[slot][t];
    return D3D_OK;
}
HRESULT MtlDevice::SetTexture(DWORD s, IDirect3DBaseTexture9 *tex) {
    int slot = sampler_slot(s);
    if (slot < 0) return D3D_OK;
    st_.tex[slot] = Ref<IDirect3DBaseTexture9>(tex);
    st_.texdata[slot] = tex_data_of(tex);
    note_state(SC_TEXTURE);
    return D3D_OK;
}
HRESULT MtlDevice::GetTexture(DWORD s, IDirect3DBaseTexture9 **pp) {
    int slot = sampler_slot(s);
    if (slot < 0 || !pp) return D3DERR_INVALIDCALL;
    *pp = st_.tex[slot].get();
    if (*pp) (*pp)->AddRef();
    return D3D_OK;
}
HRESULT MtlDevice::SetTransform(D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) {
    unsigned i = (unsigned)t;
    if (i < 260) st_.xform[i] = *m;
    return D3D_OK;
}
HRESULT MtlDevice::GetTransform(D3DTRANSFORMSTATETYPE t, D3DMATRIX *m) {
    unsigned i = (unsigned)t;
    if (i >= 260) return D3DERR_INVALIDCALL;
    *m = st_.xform[i];
    return D3D_OK;
}
HRESULT MtlDevice::SetClipPlane(DWORD i, const float *p) {
    if (i >= 6) return D3DERR_INVALIDCALL;
    memcpy(st_.clip[i], p, 16);
    return D3D_OK;
}
HRESULT MtlDevice::GetClipPlane(DWORD i, float *p) {
    if (i >= 6) return D3DERR_INVALIDCALL;
    memcpy(p, st_.clip[i], 16);
    return D3D_OK;
}

#define SET_CONST(NAME, STAGE_ARR, MAXREG, TYPE, CVER, CAT, ELEMS)                                                \
    HRESULT MtlDevice::NAME(UINT start, const TYPE *data, UINT count) {                                           \
        if (!data || start + count > (MAXREG)) return D3DERR_INVALIDCALL;                                         \
        memcpy(&st_.c.STAGE_ARR[(size_t)start * (ELEMS)], data, (size_t)count * (ELEMS) * sizeof(TYPE));          \
        CVER++;                                                                                                   \
        note_state(CAT);                                                                                          \
        return D3D_OK;                                                                                            \
    }
#define GET_CONST(NAME, STAGE_ARR, MAXREG, TYPE, ELEMS)                                                           \
    HRESULT MtlDevice::NAME(UINT start, TYPE *data, UINT count) {                                                 \
        if (!data || start + count > (MAXREG)) return D3DERR_INVALIDCALL;                                         \
        memcpy(data, &st_.c.STAGE_ARR[(size_t)start * (ELEMS)], (size_t)count * (ELEMS) * sizeof(TYPE));          \
        return D3D_OK;                                                                                            \
    }
SET_CONST(SetVertexShaderConstantF, vsf, 256, float, vs_cver_, SC_VSCONST, 4)
SET_CONST(SetVertexShaderConstantI, vsi, 16, int, vs_cver_, SC_VSCONST, 4)
SET_CONST(SetPixelShaderConstantF, psf, 224, float, ps_cver_, SC_PSCONST, 4)
SET_CONST(SetPixelShaderConstantI, psi, 16, int, ps_cver_, SC_PSCONST, 4)
GET_CONST(GetVertexShaderConstantF, vsf, 256, float, 4)
GET_CONST(GetVertexShaderConstantI, vsi, 16, int, 4)
GET_CONST(GetPixelShaderConstantF, psf, 224, float, 4)
GET_CONST(GetPixelShaderConstantI, psi, 16, int, 4)

HRESULT MtlDevice::SetVertexShaderConstantB(UINT start, const WINBOOL *data, UINT count) {
    if (!data || start + count > 16) return D3DERR_INVALIDCALL;
    for (UINT i = 0; i < count; i++) st_.c.vsb[start + i] = data[i] ? 1 : 0;
    vs_cver_++;
    note_state(SC_VSCONST);
    return D3D_OK;
}
HRESULT MtlDevice::GetVertexShaderConstantB(UINT start, WINBOOL *data, UINT count) {
    if (!data || start + count > 16) return D3DERR_INVALIDCALL;
    for (UINT i = 0; i < count; i++) data[i] = st_.c.vsb[start + i];
    return D3D_OK;
}
HRESULT MtlDevice::SetPixelShaderConstantB(UINT start, const WINBOOL *data, UINT count) {
    if (!data || start + count > 16) return D3DERR_INVALIDCALL;
    for (UINT i = 0; i < count; i++) st_.c.psb[start + i] = data[i] ? 1 : 0;
    ps_cver_++;
    note_state(SC_PSCONST);
    return D3D_OK;
}
HRESULT MtlDevice::GetPixelShaderConstantB(UINT start, WINBOOL *data, UINT count) {
    if (!data || start + count > 16) return D3DERR_INVALIDCALL;
    for (UINT i = 0; i < count; i++) data[i] = st_.c.psb[start + i];
    return D3D_OK;
}

HRESULT MtlDevice::SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *vb, UINT offset, UINT stride) {
    if (stream >= 16) return D3DERR_INVALIDCALL;
    StreamBinding &b = st_.streams[stream];
    b.vb = Ref<MtlVertexBuffer>(static_cast<MtlVertexBuffer *>(vb));
    b.offset = offset;
    b.stride = stride;
    note_state(SC_STREAMS);
    return D3D_OK;
}
HRESULT MtlDevice::GetStreamSource(UINT stream, IDirect3DVertexBuffer9 **pp, UINT *offset, UINT *stride) {
    if (stream >= 16 || !pp) return D3DERR_INVALIDCALL;
    *pp = st_.streams[stream].vb.get();
    if (*pp) (*pp)->AddRef();
    if (offset) *offset = st_.streams[stream].offset;
    if (stride) *stride = st_.streams[stream].stride;
    return D3D_OK;
}
HRESULT MtlDevice::SetStreamSourceFreq(UINT stream, UINT freq) {
    if (stream >= 16) return D3DERR_INVALIDCALL;
    st_.streams[stream].freq = freq;
    note_state(SC_STREAMS);
    return D3D_OK;
}
HRESULT MtlDevice::GetStreamSourceFreq(UINT stream, UINT *freq) {
    if (stream >= 16 || !freq) return D3DERR_INVALIDCALL;
    *freq = st_.streams[stream].freq;
    return D3D_OK;
}
HRESULT MtlDevice::SetIndices(IDirect3DIndexBuffer9 *ib) {
    st_.ib = Ref<MtlIndexBuffer>(static_cast<MtlIndexBuffer *>(ib));
    note_state(SC_INDICES);
    return D3D_OK;
}
HRESULT MtlDevice::GetIndices(IDirect3DIndexBuffer9 **pp) {
    *pp = st_.ib.get();
    if (*pp) (*pp)->AddRef();
    return D3D_OK;
}

// ---- state blocks ----------------------------------------------------------------------------------------------------------------------------------
void MtlDevice::st_capture(DeviceState *dst, uint32_t cats) {
    if (cats & SC_RENDER) memcpy(dst->rs, st_.rs, sizeof dst->rs);
    if (cats & SC_SAMPLER) memcpy(dst->ss, st_.ss, sizeof dst->ss);
    if (cats & SC_TEXTURE) { for (int i = 0; i < kMaxSamplers; i++) { dst->tex[i] = st_.tex[i]; dst->texdata[i] = st_.texdata[i]; } }
    if (cats & SC_STREAMS) for (int i = 0; i < 16; i++) dst->streams[i] = st_.streams[i];
    if (cats & SC_INDICES) dst->ib = st_.ib;
    if (cats & SC_DECL) { dst->decl = st_.decl; dst->fvf = st_.fvf; }
    if (cats & SC_VS) dst->vs = st_.vs;
    if (cats & SC_PS) dst->ps = st_.ps;
    if (cats & SC_VSCONST) { memcpy(dst->c.vsf, st_.c.vsf, sizeof dst->c.vsf); memcpy(dst->c.vsi, st_.c.vsi, sizeof dst->c.vsi); memcpy(dst->c.vsb, st_.c.vsb, sizeof dst->c.vsb); }
    if (cats & SC_PSCONST) { memcpy(dst->c.psf, st_.c.psf, sizeof dst->c.psf); memcpy(dst->c.psi, st_.c.psi, sizeof dst->c.psi); memcpy(dst->c.psb, st_.c.psb, sizeof dst->c.psb); }
    if (cats & SC_VIEWPORT) dst->vp = st_.vp;
    if (cats & SC_SCISSOR) dst->scissor = st_.scissor;
}
void MtlDevice::st_apply(const DeviceState &src, uint32_t cats) {
    if (cats & SC_RENDER) memcpy(st_.rs, src.rs, sizeof st_.rs);
    if (cats & SC_SAMPLER) memcpy(st_.ss, src.ss, sizeof st_.ss);
    if (cats & SC_TEXTURE) { for (int i = 0; i < kMaxSamplers; i++) { st_.tex[i] = src.tex[i]; st_.texdata[i] = src.texdata[i]; } }
    if (cats & SC_STREAMS) for (int i = 0; i < 16; i++) st_.streams[i] = src.streams[i];
    if (cats & SC_INDICES) st_.ib = src.ib;
    if (cats & SC_DECL) { st_.decl = src.decl; st_.fvf = src.fvf; }
    if (cats & SC_VS) { st_.vs = src.vs; vs_cver_++; }
    if (cats & SC_PS) { st_.ps = src.ps; ps_cver_++; }
    if (cats & SC_VSCONST) { memcpy(st_.c.vsf, src.c.vsf, sizeof st_.c.vsf); memcpy(st_.c.vsi, src.c.vsi, sizeof st_.c.vsi); memcpy(st_.c.vsb, src.c.vsb, sizeof st_.c.vsb); vs_cver_++; }
    if (cats & SC_PSCONST) { memcpy(st_.c.psf, src.c.psf, sizeof st_.c.psf); memcpy(st_.c.psi, src.c.psi, sizeof st_.c.psi); memcpy(st_.c.psb, src.c.psb, sizeof st_.c.psb); ps_cver_++; }
    if (cats & SC_VIEWPORT) st_.vp = src.vp;
    if (cats & SC_SCISSOR) st_.scissor = src.scissor;
}

HRESULT MtlDevice::CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9 **pp) {
    if (!pp) return D3DERR_INVALIDCALL;
    MtlStateBlock *b = new MtlStateBlock();
    b->dev = this;
    b->snap.reset(new DeviceState());
    switch (type) {
        case D3DSBT_PIXELSTATE: b->cats = SC_RENDER | SC_SAMPLER | SC_TEXTURE | SC_PS | SC_PSCONST; break;
        case D3DSBT_VERTEXSTATE: b->cats = SC_RENDER | SC_STREAMS | SC_INDICES | SC_DECL | SC_VS | SC_VSCONST; break;
        default: b->cats = SC_ALL; break;
    }
    st_capture(b->snap.get(), b->cats);
    *pp = b;
    return D3D_OK;
}
HRESULT MtlDevice::BeginStateBlock() {
    if (recording_) return D3DERR_INVALIDCALL;
    recording_ = true;
    rec_cats_ = 0;
    rec_pre_.reset(new DeviceState(st_));
    return D3D_OK;
}
HRESULT MtlDevice::EndStateBlock(IDirect3DStateBlock9 **pp) {
    if (!recording_ || !pp) return D3DERR_INVALIDCALL;
    recording_ = false;
    MtlStateBlock *b = new MtlStateBlock();
    b->dev = this;
    b->snap.reset(new DeviceState());
    b->cats = rec_cats_;
    st_capture(b->snap.get(), b->cats);
    // D3D records without applying: put the device back to what it was before Begin
    st_apply(*rec_pre_, rec_cats_);
    rec_pre_.reset();
    *pp = b;
    return D3D_OK;
}

// ---- copies ------------------------------------------------------------------------------------------------------------------------------------------
HRESULT MtlDevice::UpdateSurface(IDirect3DSurface9 *src, const RECT *srect, IDirect3DSurface9 *dst, const POINT *dpt) {
    MtlSurface *s = static_cast<MtlSurface *>(src), *d = static_cast<MtlSurface *>(dst);
    if (!s || !d || s->td->fmt != d->td->fmt) return D3DERR_INVALIDCALL;
    const uint32_t sw = s->width(), sh = s->height();
    RECT r = srect ? *srect : RECT{0, 0, (LONG)sw, (LONG)sh};
    uint32_t dx = dpt ? (uint32_t)dpt->x : 0, dy = dpt ? (uint32_t)dpt->y : 0;
    uint32_t rw = (uint32_t)(r.right - r.left), rh = (uint32_t)(r.bottom - r.top);
    const FormatInfo *fi = s->td->fi;
    std::vector<uint8_t> &ss = s->td->sys[s->td->index(s->face, s->level)];
    const uint32_t spitch = level_pitch(fi, sw);
    if (s->td->gpu || ss.empty()) {
        // source without CPU data: read it back (only possible for plain formats)
        std::vector<uint8_t> tmp((size_t)spitch * level_rows(fi, sh));
        if (!s->td->readback(s->face, s->level, 0, 0, sw, sh, tmp.data(), spitch)) return D3DERR_INVALIDCALL;
        ss.swap(tmp);
    }
    const uint32_t xoff = fi->block ? (r.left / 4) * fi->bpp : (uint32_t)r.left * fi->bpp;
    const uint32_t yrow = fi->block ? (uint32_t)r.top / 4 : (uint32_t)r.top;
    const uint8_t *sp = ss.data() + (size_t)yrow * spitch + xoff;
    if (d->td->gpu) {
        d->td->upload(d->face, d->level, dx, dy, 0, rw, rh, 1, sp, spitch, 0);
    } else {
        std::vector<uint8_t> &ds = d->td->sys[d->td->index(d->face, d->level)];
        const uint32_t dpitch = level_pitch(fi, d->width());
        if (ds.empty()) ds.assign((size_t)dpitch * level_rows(fi, d->height()), 0);
        const uint32_t rows = fi->block ? (rh + 3) / 4 : rh;
        const uint32_t rowbytes = fi->block ? ((rw + 3) / 4) * fi->bpp : rw * fi->bpp;
        const uint32_t dxo = fi->block ? (dx / 4) * fi->bpp : dx * fi->bpp, dyr = fi->block ? dy / 4 : dy;
        for (uint32_t y = 0; y < rows; y++) memcpy(&ds[(size_t)(dyr + y) * dpitch + dxo], sp + (size_t)y * spitch, rowbytes);
    }
    return D3D_OK;
}

HRESULT MtlDevice::UpdateTexture(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst) {
    TexData *s = tex_data_of(src), *d = tex_data_of(dst);
    if (!s || !d || s->fmt != d->fmt || s->cube != d->cube || s->volume != d->volume) return D3DERR_INVALIDCALL;
    // the source's top level maps to the destination level with the same size
    uint32_t base = 0;
    while (base < d->levels && (mip_dim(d->w, base) != s->w || mip_dim(d->h, base) != s->h)) base++;
    if (base >= d->levels) return D3DERR_INVALIDCALL;
    const FormatInfo *fi = s->fi;
    const uint32_t nlev = d->autogen ? 1 : std::min(s->levels, d->levels - base);
    for (uint32_t f = 0; f < s->faces; f++) {
        for (uint32_t l = 0; l < nlev; l++) {
            std::vector<uint8_t> &data = s->sys[s->index(f, l)];
            if (data.empty()) continue;
            const uint32_t lw = mip_dim(s->w, l), lh = mip_dim(s->h, l), ld = s->volume ? mip_dim(s->d, l) : 1;
            const uint32_t pitch = level_pitch(fi, lw), slice = pitch * level_rows(fi, lh);
            d->upload(f, base + l, 0, 0, 0, lw, lh, ld, data.data(), pitch, slice);
        }
    }
    if (d->autogen) d->generate_mips();
    return D3D_OK;
}

HRESULT MtlDevice::GetRenderTargetData(IDirect3DSurface9 *rt, IDirect3DSurface9 *dst) {
    MtlSurface *s = static_cast<MtlSurface *>(rt), *d = static_cast<MtlSurface *>(dst);
    if (!s || !d || d->td->gpu) return D3DERR_INVALIDCALL;
    if (s->width() != d->width() || s->height() != d->height()) return D3DERR_INVALIDCALL;
    const FormatInfo *fi = s->td->fi;
    const uint32_t pitch = level_pitch(fi, s->width());
    std::vector<uint8_t> &out = d->td->sys[d->td->index(d->face, d->level)];
    out.assign((size_t)pitch * s->height(), 0);
    if (!s->td->readback(s->face, s->level, 0, 0, s->width(), s->height(), out.data(), pitch)) return D3DERR_INVALIDCALL;
    return D3D_OK;
}
HRESULT MtlDevice::GetFrontBufferData(UINT, IDirect3DSurface9 *dst) { return GetRenderTargetData(backbuffer_.get(), dst); }

HRESULT MtlDevice::ColorFill(IDirect3DSurface9 *surf, const RECT *rect, D3DCOLOR color) {
    MtlSurface *s = static_cast<MtlSurface *>(surf);
    if (!s) return D3DERR_INVALIDCALL;
    TexData *td = s->td;
    const uint32_t w = s->width(), h = s->height();
    if (!td->gpu) {   // CPU surface (X8R8G8B8 / A8R8G8B8 only)
        if (td->fi->bpp != 4 || td->fi->block) return D3DERR_INVALIDCALL;
        std::vector<uint8_t> &data = td->sys[td->index(s->face, s->level)];
        const uint32_t pitch = level_pitch(td->fi, w);
        if (data.empty()) data.assign((size_t)pitch * h, 0);
        RECT r = rect ? *rect : RECT{0, 0, (LONG)w, (LONG)h};
        for (LONG y = r.top; y < r.bottom; y++) for (LONG x = r.left; x < r.right; x++) memcpy(&data[(size_t)y * pitch + (size_t)x * 4], &color, 4);
        return D3D_OK;
    }
    if (!td->tex || !td->fi->rt_ok) return D3DERR_INVALIDCALL;
    end_pass();
    begin_cmd();
    end_blit();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    const bool full = !rect || (rect->left <= 0 && rect->top <= 0 && (uint32_t)rect->right >= w && (uint32_t)rect->bottom >= h);
    float c[4];
    color_to_float(color, c);
    rp.colorAttachments[0].texture = td->tex;
    rp.colorAttachments[0].level = s->level;
    rp.colorAttachments[0].slice = td->cube ? s->face : 0;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (full) {
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
        id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:rp];
        [e endEncoding];
    } else {
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:rp];
        PsoKey k;
        k.color_fmt[0] = (uint16_t)td->tex.pixelFormat;
        k.write_mask[0] = 0xF;
        [e setRenderPipelineState:clear_pso(k)];
        float u[8] = {c[0], c[1], c[2], c[3], 0.0f, 0, 0, 0};
        [e setVertexBytes:u length:sizeof u atIndex:0];
        [e setFragmentBytes:u length:sizeof u atIndex:0];
        MTLViewport vp = {0, 0, (double)w, (double)h, 0, 1};
        [e setViewport:vp];
        MTLScissorRect sc = {(NSUInteger)std::max<LONG>(rect->left, 0), (NSUInteger)std::max<LONG>(rect->top, 0), (NSUInteger)std::min<LONG>(rect->right, w) - (NSUInteger)std::max<LONG>(rect->left, 0),
                             (NSUInteger)std::min<LONG>(rect->bottom, h) - (NSUInteger)std::max<LONG>(rect->top, 0)};
        [e setScissorRect:sc];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
    td->last_use = serial_;
    return D3D_OK;
}

HRESULT MtlDevice::StretchRect(IDirect3DSurface9 *src, const RECT *srect, IDirect3DSurface9 *dst, const RECT *drect, D3DTEXTUREFILTERTYPE filter) {
    MtlSurface *s = static_cast<MtlSurface *>(src), *d = static_cast<MtlSurface *>(dst);
    if (!s || !d || !s->td->gpu || !d->td->gpu) return D3DERR_INVALIDCALL;
    const uint32_t sw = s->width(), sh = s->height(), dw = d->width(), dh = d->height();
    RECT sr = srect ? *srect : RECT{0, 0, (LONG)sw, (LONG)sh}, dr = drect ? *drect : RECT{0, 0, (LONG)dw, (LONG)dh};
    const uint32_t srw = sr.right - sr.left, srh = sr.bottom - sr.top, drw = dr.right - dr.left, drh = dr.bottom - dr.top;
    end_pass();
    s->td->last_use = d->td->last_use = cur_serial();
    if (s->td->fi->depth || d->td->fi->depth) {
        if (srw != drw || srh != drh || s->td->tex.pixelFormat != d->td->tex.pixelFormat) return D3DERR_INVALIDCALL;
        gpu_copy_texture(s->td->tex, s->level, 0, MTLOriginMake(sr.left, sr.top, 0), MTLSizeMake(srw, srh, 1), d->td->tex, d->level, 0, MTLOriginMake(dr.left, dr.top, 0));
        return D3D_OK;
    }
    if (srw == drw && srh == drh && s->td->tex.pixelFormat == d->td->tex.pixelFormat && s->td != d->td) {
        gpu_copy_texture(s->td->tex, s->level, s->td->cube ? s->face : 0, MTLOriginMake(sr.left, sr.top, 0), MTLSizeMake(srw, srh, 1), d->td->tex, d->level, d->td->cube ? d->face : 0,
                         MTLOriginMake(dr.left, dr.top, 0));
        return D3D_OK;
    }
    if (!d->td->fi->rt_ok) return D3DERR_INVALIDCALL;
    end_blit();
    begin_cmd();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = d->td->tex;
    rp.colorAttachments[0].level = d->level;
    rp.colorAttachments[0].slice = d->td->cube ? d->face : 0;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:blit_pso(d->td->tex.pixelFormat, true)];
    MTLViewport vp = {(double)dr.left, (double)dr.top, (double)drw, (double)drh, 0, 1};
    [e setViewport:vp];
    float uvr[4] = {(float)sr.left / sw, (float)sr.top / sh, (float)srw / sw, (float)srh / sh};
    [e setVertexBytes:uvr length:sizeof uvr atIndex:0];
    // sample level `s->level` of the source: use a one-level view when the surface is not level 0
    id<MTLTexture> st = s->td->sample_view;
    if (s->level != 0 || s->td->faces > 1)
        st = [s->td->tex newTextureViewWithPixelFormat:s->td->tex.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(s->level, 1) slices:NSMakeRange(s->td->cube ? s->face : 0, 1)];
    [e setFragmentTexture:st atIndex:0];
    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = sd.magFilter = filter == D3DTEXF_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    [e setFragmentSamplerState:[mdev_ newSamplerStateWithDescriptor:sd] atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    return D3D_OK;
}

}  // namespace mtl
}  // namespace dish
