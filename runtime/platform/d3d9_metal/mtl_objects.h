// Metal backend: object model (classes implementing the host D3D9 interfaces).
#pragma once
#include "mtl_internal.h"

namespace dish {
namespace mtl {

#include "iface_stubs.inc"

class MtlDevice;
class MtlSurface;
class MtlVertexShader;
class MtlPixelShader;

// ---- common resource plumbing ---------------------------------------------------------------------------------------------------
struct PrivateData {
    struct Item { GUID guid; std::vector<uint8_t> data; };
    std::vector<Item> items;
    HRESULT set(REFGUID g, const void *d, DWORD size, DWORD flags);
    HRESULT get(REFGUID g, void *d, DWORD *size);
    HRESULT free(REFGUID g);
};

// Methods shared by every resource (IDirect3DResource9). `Base` must be a Com<Stub_I..., ...>.
#define MTL_RESOURCE_METHODS(RESTYPE)                                                                                         \
    STDMETHOD(GetDevice)(IDirect3DDevice9 **pp) override;                                                                     \
    STDMETHOD(SetPrivateData)(REFGUID g, const void *d, DWORD size, DWORD flags) override { return pdata_.set(g, d, size, flags); } \
    STDMETHOD(GetPrivateData)(REFGUID g, void *d, DWORD *size) override { return pdata_.get(g, d, size); }                    \
    STDMETHOD(FreePrivateData)(REFGUID g) override { return pdata_.free(g); }                                                 \
    STDMETHOD_(DWORD, SetPriority)(DWORD p) override { DWORD o = prio_; prio_ = p; return o; }                                \
    STDMETHOD_(DWORD, GetPriority)() override { return prio_; }                                                               \
    STDMETHOD_(void, PreLoad)() override {}                                                                                   \
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return RESTYPE; }

// ---- buffers --------------------------------------------------------------------------------------------------------------------
struct BufferCore {
    MtlDevice *dev = nullptr;
    id<MTLBuffer> buf = nil;
    uint32_t size = 0;
    DWORD usage = 0;
    D3DPOOL pool = D3DPOOL_DEFAULT;
    uint64_t last_use = 0;       // command-buffer serial of the latest GPU use
    bool locked = false;
    bool create(MtlDevice *d, uint32_t sz, DWORD u, D3DPOOL p);
    void destroy();
    HRESULT lock(uint32_t off, uint32_t len, void **pp, DWORD flags);
    HRESULT unlock();
};

class MtlVertexBuffer : public Com<Stub_IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, IDirect3DResource9> {
public:
    BufferCore core;
    DWORD fvf = 0;
    PrivateData pdata_;
    DWORD prio_ = 0;
    MTL_RESOURCE_METHODS(D3DRTYPE_VERTEXBUFFER)
    STDMETHOD(Lock)(UINT off, UINT size, void **pp, DWORD flags) override { return core.lock(off, size, pp, flags); }
    STDMETHOD(Unlock)() override { return core.unlock(); }
    STDMETHOD(GetDesc)(D3DVERTEXBUFFER_DESC *d) override;
    void destroy() override { core.destroy(); delete this; }
};

class MtlIndexBuffer : public Com<Stub_IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, IDirect3DResource9> {
public:
    BufferCore core;
    D3DFORMAT fmt = D3DFMT_INDEX16;
    PrivateData pdata_;
    DWORD prio_ = 0;
    MTL_RESOURCE_METHODS(D3DRTYPE_INDEXBUFFER)
    STDMETHOD(Lock)(UINT off, UINT size, void **pp, DWORD flags) override { return core.lock(off, size, pp, flags); }
    STDMETHOD(Unlock)() override { return core.unlock(); }
    STDMETHOD(GetDesc)(D3DINDEXBUFFER_DESC *d) override;
    void destroy() override { core.destroy(); delete this; }
};

// ---- textures -------------------------------------------------------------------------------------------------------------------
struct TexLock {
    bool active = false;
    DWORD flags = 0;
    uint32_t x = 0, y = 0, z = 0, w = 0, h = 0, d = 0;   // locked region in texels
    std::vector<uint8_t> data;                           // staging in the D3D layout
    uint32_t pitch = 0, slice = 0;
};
// A READONLY lock of a DXT level without a CPU copy fills the staging buffer with copy tokens: every block holds the magic, the
// token slot and the block index. When a whole level of another texture is unlocked holding exactly the tokens of one slot, the
// level is copied on the GPU instead of being transcoded.
struct CopyToken {
    id<MTLTexture> tex = nil;    // strong: keeps the source alive until the copy is encoded
    uint32_t level = 0, slice = 0, w = 0, h = 0;
    uint64_t serial = 0;
};

// Storage of a 2D texture, cube texture, volume texture or standalone surface (render target, depth buffer, offscreen plain).
struct TexData {
    MtlDevice *dev = nullptr;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    const FormatInfo *fi = nullptr;
    D3DPOOL pool = D3DPOOL_DEFAULT;
    DWORD usage = 0;
    uint32_t w = 0, h = 0, d = 1, levels = 1, faces = 1;
    bool cube = false, volume = false;
    bool gpu = false;                 // has a GPU texture (false for SYSTEMMEM / SCRATCH and plain offscreen surfaces)
    bool astc = false;                // block compressed data lives as ASTC on the GPU
    id<MTLTexture> tex = nil;         // base GPU texture
    id<MTLTexture> sample_view = nil; // swizzled view used for sampling when the format needs one (else == tex)
    id<MTLTexture> srgb_view = nil;
    id<MTLTexture> rt_srgb_view = nil;   // unswizzled sRGB view used as a render target when D3DRS_SRGBWRITEENABLE is set
    std::vector<std::vector<uint8_t>> sys;   // CPU copy per (face, level) for pools without a GPU texture
    // D3D-layout copy per (face, level) of GPU textures whose GPU format differs from the D3D one (DXT decoded / transcoded, 16-bit
    // formats expanded): the GPU data cannot be read back as the D3D bytes, and UE3 locks such textures READONLY to copy mips
    std::vector<std::vector<uint8_t>> shadow;
    // A D3D-layout CPU copy is kept for converted formats so READONLY locks can return the texels. DXT levels are not kept (unless
    // DISH_METAL_TEX_SHADOW=1): UE3 only reads them back to copy whole mips into a reallocated texture, which copy tokens turn into a
    // GPU texture copy (see TexData::lock / unlock).
    bool needs_shadow() const { return gpu && (fi->block ? options().tex_shadow : fi->conv != Conv::None); }
    bool copy_tokens() const { return gpu && fi->block && !options().tex_shadow; }
    std::vector<TexLock> locks;       // per (face, level)
    bool autogen = false;
    DWORD lod = 0;
    D3DTEXTUREFILTERTYPE autogen_filter = D3DTEXF_LINEAR;
    uint64_t last_use = 0;
    uint64_t gpu_bytes = 0;
    bool dirty_mips = false;          // level 0 changed since the mip chain was generated (AUTOGENMIPMAP)
    bool tex_written_by_gpu = false;  // rendered to: CPU readbacks have to wait for the GPU
    size_t index(uint32_t face, uint32_t level) const { return (size_t)face * levels + level; }
    bool create(MtlDevice *dev, D3DFORMAT fmt, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, DWORD usage, D3DPOOL pool, bool cube, bool volume);
    void release();
    id<MTLTexture> sampling_texture(bool srgb);
    id<MTLTexture> render_texture(bool srgb);
    HRESULT lock(uint32_t face, uint32_t level, const RECT *rect, const D3DBOX *box, DWORD flags, void **bits, int *pitch, int *slice);
    HRESULT unlock(uint32_t face, uint32_t level);
    // upload a region given in the D3D layout (converted / transcoded to the GPU format)
    void upload(uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t rw, uint32_t rh, uint32_t rd, const uint8_t *src, uint32_t pitch, uint32_t slice);
    // read a region back in the D3D layout (waits for the GPU when needed)
    bool readback(uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t rw, uint32_t rh, uint8_t *dst, uint32_t pitch);
    void generate_mips();
};

// IDirect3DBaseTexture9 methods (same for 2D / cube / volume textures)
#define MTL_BASE_TEXTURE_METHODS                                                                                              \
    STDMETHOD_(DWORD, SetLOD)(DWORD l) override { DWORD o = td.lod; td.lod = l; return o; }                                   \
    STDMETHOD_(DWORD, GetLOD)() override { return td.lod; }                                                                   \
    STDMETHOD_(DWORD, GetLevelCount)() override { return td.autogen ? 1 : td.levels; }                                        \
    STDMETHOD(SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE t) override { td.autogen_filter = t; return D3D_OK; }               \
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() override { return td.autogen_filter; }                          \
    STDMETHOD_(void, GenerateMipSubLevels)() override { td.generate_mips(); }

// A texture the device can bind: common interface for the three kinds
class MtlBaseTextureIf {
public:
    virtual TexData &data() = 0;
    virtual IUnknown *unknown() = 0;
    virtual ~MtlBaseTextureIf() = default;
};

class MtlSurface : public Com<Stub_IDirect3DSurface9, IDirect3DSurface9, IDirect3DResource9> {
public:
    TexData *td = nullptr;
    uint32_t face = 0, level = 0;
    IUnknown *container = nullptr;            // owning texture (weak: it owns this surface); null for standalone surfaces
    std::unique_ptr<TexData> own;             // standalone storage
    PrivateData pdata_;
    DWORD prio_ = 0;
    D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_NONE;
    bool is_backbuffer = false;
    MTL_RESOURCE_METHODS(D3DRTYPE_SURFACE)
    STDMETHOD_(ULONG, AddRef)() override { return container ? container->AddRef() : ++rc_; }
    STDMETHOD_(ULONG, Release)() override { return container ? container->Release() : Com::Release(); }
    STDMETHOD(GetContainer)(REFIID riid, void **pp) override;
    STDMETHOD(GetDesc)(D3DSURFACE_DESC *d) override;
    STDMETHOD(LockRect)(D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) override;
    STDMETHOD(UnlockRect)() override;
    STDMETHOD(GetDC)(HDC *) override { return D3DERR_INVALIDCALL; }
    STDMETHOD(ReleaseDC)(HDC) override { return D3DERR_INVALIDCALL; }
    uint32_t width() const { return mip_dim(td->w, level); }
    uint32_t height() const { return mip_dim(td->h, level); }
    void destroy() override { if (own) own->release(); delete this; }
};

class MtlTexture2D : public Com<Stub_IDirect3DTexture9, IDirect3DTexture9, IDirect3DBaseTexture9, IDirect3DResource9>, public MtlBaseTextureIf {
public:
    TexData td;
    std::vector<MtlSurface *> surfaces;       // one per level, created on demand, owned
    PrivateData pdata_;
    DWORD prio_ = 0;
    MTL_RESOURCE_METHODS(D3DRTYPE_TEXTURE)
    MTL_BASE_TEXTURE_METHODS
    STDMETHOD(GetLevelDesc)(UINT level, D3DSURFACE_DESC *d) override;
    STDMETHOD(GetSurfaceLevel)(UINT level, IDirect3DSurface9 **pp) override;
    STDMETHOD(LockRect)(UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) override;
    STDMETHOD(UnlockRect)(UINT level) override;
    STDMETHOD(AddDirtyRect)(const RECT *) override { return D3D_OK; }
    TexData &data() override { return td; }
    IUnknown *unknown() override { return static_cast<IDirect3DTexture9 *>(this); }
    void destroy() override;
};

class MtlCubeTexture : public Com<Stub_IDirect3DCubeTexture9, IDirect3DCubeTexture9, IDirect3DBaseTexture9, IDirect3DResource9>, public MtlBaseTextureIf {
public:
    TexData td;
    std::vector<MtlSurface *> surfaces;       // faces * levels
    PrivateData pdata_;
    DWORD prio_ = 0;
    MTL_RESOURCE_METHODS(D3DRTYPE_CUBETEXTURE)
    MTL_BASE_TEXTURE_METHODS
    STDMETHOD(GetLevelDesc)(UINT level, D3DSURFACE_DESC *d) override;
    STDMETHOD(GetCubeMapSurface)(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **pp) override;
    STDMETHOD(LockRect)(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) override;
    STDMETHOD(UnlockRect)(D3DCUBEMAP_FACES face, UINT level) override;
    STDMETHOD(AddDirtyRect)(D3DCUBEMAP_FACES, const RECT *) override { return D3D_OK; }
    TexData &data() override { return td; }
    IUnknown *unknown() override { return static_cast<IDirect3DCubeTexture9 *>(this); }
    void destroy() override;
};

class MtlVolume;
class MtlVolumeTexture : public Com<Stub_IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, IDirect3DBaseTexture9, IDirect3DResource9>, public MtlBaseTextureIf {
public:
    TexData td;
    std::vector<MtlVolume *> volumes;
    PrivateData pdata_;
    DWORD prio_ = 0;
    MTL_RESOURCE_METHODS(D3DRTYPE_VOLUMETEXTURE)
    MTL_BASE_TEXTURE_METHODS
    STDMETHOD(GetLevelDesc)(UINT level, D3DVOLUME_DESC *d) override;
    STDMETHOD(GetVolumeLevel)(UINT level, IDirect3DVolume9 **pp) override;
    STDMETHOD(LockBox)(UINT level, D3DLOCKED_BOX *lb, const D3DBOX *box, DWORD flags) override;
    STDMETHOD(UnlockBox)(UINT level) override;
    STDMETHOD(AddDirtyBox)(const D3DBOX *) override { return D3D_OK; }
    TexData &data() override { return td; }
    IUnknown *unknown() override { return static_cast<IDirect3DVolumeTexture9 *>(this); }
    void destroy() override;
};

class MtlVolume : public Com<Stub_IDirect3DVolume9, IDirect3DVolume9> {
public:
    MtlVolumeTexture *tex = nullptr;
    uint32_t level = 0;
    PrivateData pdata_;
    STDMETHOD_(ULONG, AddRef)() override { return tex->AddRef(); }
    STDMETHOD_(ULONG, Release)() override { return tex->Release(); }
    STDMETHOD(GetDevice)(IDirect3DDevice9 **pp) override;
    STDMETHOD(SetPrivateData)(REFGUID g, const void *d, DWORD size, DWORD flags) override { return pdata_.set(g, d, size, flags); }
    STDMETHOD(GetPrivateData)(REFGUID g, void *d, DWORD *size) override { return pdata_.get(g, d, size); }
    STDMETHOD(FreePrivateData)(REFGUID g) override { return pdata_.free(g); }
    STDMETHOD(GetContainer)(REFIID riid, void **pp) override;
    STDMETHOD(GetDesc)(D3DVOLUME_DESC *d) override;
    STDMETHOD(LockBox)(D3DLOCKED_BOX *lb, const D3DBOX *box, DWORD flags) override { return tex->LockBox(level, lb, box, flags); }
    STDMETHOD(UnlockBox)() override { return tex->UnlockBox(level); }
};

// ---- vertex declarations and shaders --------------------------------------------------------------------------------------------
class MtlVertexDecl : public Com<Stub_IDirect3DVertexDeclaration9, IDirect3DVertexDeclaration9> {
public:
    MtlDevice *dev = nullptr;
    std::vector<D3DVERTEXELEMENT9> elems;     // without the D3DDECL_END terminator
    uint64_t hash = 0;
    uint32_t stream_min_stride[16] = {};
    STDMETHOD(GetDevice)(IDirect3DDevice9 **pp) override;
    STDMETHOD(GetDeclaration)(D3DVERTEXELEMENT9 *out, UINT *n) override;
};

struct ShaderCommon {
    MtlDevice *dev = nullptr;
    std::vector<uint32_t> bytecode;
    ShaderXlate x;
    bool is_pixel = false;
    uint64_t sid = 0;
    uint64_t content_hash = 0;      // FNV-1a of the bytecode: names the shader in the persistent record (mtl_shadercache)
    dispatch_group_t group = nil;
    std::atomic<int> state{0};      // 0 queued, 3 compiling, 1 ready, 2 failed
    std::atomic<bool> prioritized{false};
    id<MTLLibrary> lib = nil;
    id<MTLFunction> fn = nil;
    std::string error;
    void start(MtlDevice *d, const uint32_t *code, bool pixel);
    bool wait();                    // blocks until compiled; true when usable
    bool ready() const { return state.load(std::memory_order_acquire) == 1; }
    bool failed() const { return state.load(std::memory_order_acquire) == 2; }
    void prioritize();              // a draw needs it now: compile it ahead of the background queue
    void compile();                 // translate + compile (whoever claims state 0 -> 3 runs it)
};

class MtlVertexShader : public Com<Stub_IDirect3DVertexShader9, IDirect3DVertexShader9> {
public:
    ShaderCommon sh;
    STDMETHOD(GetDevice)(IDirect3DDevice9 **pp) override;
    STDMETHOD(GetFunction)(void *data, UINT *size) override;
};
class MtlPixelShader : public Com<Stub_IDirect3DPixelShader9, IDirect3DPixelShader9> {
public:
    ShaderCommon sh;
    STDMETHOD(GetDevice)(IDirect3DDevice9 **pp) override;
    STDMETHOD(GetFunction)(void *data, UINT *size) override;
};

// ---- device state ---------------------------------------------------------------------------------------------------------------
constexpr int kMaxSamplers = 20;          // 16 pixel + 4 vertex (D3DVERTEXTEXTURESAMPLER0..3 map to 16..19)
constexpr int kNumRS = 256;

struct StreamBinding {
    Ref<MtlVertexBuffer> vb;
    UINT offset = 0, stride = 0;
    UINT freq = 1;
};

struct ConstState {
    float vsf[256 * 4];
    float psf[224 * 4];
    int32_t vsi[16 * 4], psi[16 * 4];
    uint8_t vsb[16], psb[16];
    ConstState() { memset(this, 0, sizeof *this); }
};

struct DeviceState {
    uint32_t rs[kNumRS];
    uint32_t ss[kMaxSamplers][14];
    Ref<IDirect3DBaseTexture9> tex[kMaxSamplers];
    TexData *texdata[kMaxSamplers];          // resolved storage of tex[] (null when empty)
    StreamBinding streams[16];
    Ref<MtlIndexBuffer> ib;
    Ref<MtlVertexDecl> decl;
    DWORD fvf = 0;
    Ref<MtlVertexShader> vs;
    Ref<MtlPixelShader> ps;
    ConstState c;
    D3DVIEWPORT9 vp{};
    RECT scissor{};
    float clip[6][4];
    D3DMATRIX xform[260];
    D3DMATERIAL9 material{};
    DeviceState();
};

class MtlStateBlock : public Com<Stub_IDirect3DStateBlock9, IDirect3DStateBlock9> {
public:
    MtlDevice *dev = nullptr;
    std::unique_ptr<DeviceState> snap;
    uint32_t cats = 0;           // StateCategory bits captured
    STDMETHOD(GetDevice)(IDirect3DDevice9 **pp) override;
    STDMETHOD(Capture)() override;
    STDMETHOD(Apply)() override;
};

// ---- queries --------------------------------------------------------------------------------------------------------------------
class MtlQuery : public Com<Stub_IDirect3DQuery9, IDirect3DQuery9> {
public:
    MtlDevice *dev = nullptr;
    D3DQUERYTYPE type = D3DQUERYTYPE_EVENT;
    uint64_t serial = 0;               // command-buffer serial the END belongs to
    bool issued = false;
    bool began = false;
    int vis_slot = -1;                 // occlusion: index in the visibility buffer of frame slot `vis_frame`
    int vis_frame = -1;
    uint64_t result = 0;
    STDMETHOD(GetDevice)(IDirect3DDevice9 **pp) override;
    STDMETHOD_(D3DQUERYTYPE, GetType)() override { return type; }
    STDMETHOD_(DWORD, GetDataSize)() override;
    STDMETHOD(Issue)(DWORD flags) override;
    STDMETHOD(GetData)(void *data, DWORD size, DWORD flags) override;
};

}  // namespace mtl
}  // namespace dish
