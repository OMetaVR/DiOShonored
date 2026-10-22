// Metal backend: the device (IDirect3DDevice9), swap chain and the IDirect3D9 factory.
#pragma once
#include "mtl_objects.h"
#include <condition_variable>
#include <mutex>

namespace dish {
namespace mtl {

// pipeline state key: everything that changes a MTLRenderPipelineState
struct PsoKey {
    uint64_t vs = 0, ps = 0, decl = 0;
    uint32_t strides[16] = {};          // per stream: stride (0 = unused)
    uint32_t stream_step[16] = {};      // 0 = per vertex, otherwise instance step rate
    uint16_t color_fmt[4] = {};
    uint16_t depth_fmt = 0, stencil_fmt = 0;
    uint32_t blend_enable = 0;          // bit i: attachment i blends
    uint8_t src_rgb = 0, dst_rgb = 0, op_rgb = 0, src_a = 0, dst_a = 0, op_a = 0;
    uint8_t write_mask[4] = {};
    uint8_t pad[2] = {};
    bool operator==(const PsoKey &o) const { return memcmp(this, &o, sizeof *this) == 0; }
};
struct PsoKeyHash { size_t operator()(const PsoKey &k) const; };

enum StateCat : uint32_t {
    SC_RENDER = 1, SC_SAMPLER = 2, SC_TEXTURE = 4, SC_STREAMS = 8, SC_INDICES = 16, SC_DECL = 32, SC_VS = 64, SC_PS = 128,
    SC_VSCONST = 256, SC_PSCONST = 512, SC_VIEWPORT = 1024, SC_SCISSOR = 2048, SC_ALL = 0xFFFF
};

class MtlSwapChain;

class MtlDevice : public Com<Stub_IDirect3DDevice9, IDirect3DDevice9> {
public:
    MtlDevice();
    ~MtlDevice() override;
    bool init(IDirect3D9 *parent, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS *pp);
    void destroy() override;

    // ---- IDirect3DDevice9 -----------------------------------------------------------------------------------------------------
    STDMETHOD(TestCooperativeLevel)() override { return D3D_OK; }
    STDMETHOD_(UINT, GetAvailableTextureMem)() override;
    STDMETHOD(EvictManagedResources)() override { return D3D_OK; }
    STDMETHOD(GetDirect3D)(IDirect3D9 **pp) override;
    STDMETHOD(GetDeviceCaps)(D3DCAPS9 *caps) override;
    STDMETHOD(GetDisplayMode)(UINT sc, D3DDISPLAYMODE *m) override;
    STDMETHOD(GetCreationParameters)(D3DDEVICE_CREATION_PARAMETERS *p) override;
    STDMETHOD(SetCursorProperties)(UINT, UINT, IDirect3DSurface9 *) override { return D3D_OK; }
    STDMETHOD_(void, SetCursorPosition)(int, int, DWORD) override {}
    STDMETHOD_(WINBOOL, ShowCursor)(WINBOOL) override { return FALSE; }
    STDMETHOD(CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS *, IDirect3DSwapChain9 **) override { return D3DERR_NOTAVAILABLE; }
    STDMETHOD(GetSwapChain)(UINT n, IDirect3DSwapChain9 **pp) override;
    STDMETHOD_(UINT, GetNumberOfSwapChains)() override { return 1; }
    STDMETHOD(Reset)(D3DPRESENT_PARAMETERS *pp) override;
    STDMETHOD(Present)(const RECT *, const RECT *, HWND, const RGNDATA *) override;
    STDMETHOD(GetBackBuffer)(UINT sc, UINT idx, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **pp) override;
    STDMETHOD(GetRasterStatus)(UINT, D3DRASTER_STATUS *rs) override { memset(rs, 0, sizeof *rs); return D3D_OK; }
    STDMETHOD(SetDialogBoxMode)(WINBOOL) override { return D3D_OK; }
    STDMETHOD_(void, SetGammaRamp)(UINT, DWORD, const D3DGAMMARAMP *) override {}
    STDMETHOD_(void, GetGammaRamp)(UINT, D3DGAMMARAMP *r) override;
    STDMETHOD(CreateTexture)(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **pp, HANDLE *shared) override;
    STDMETHOD(CreateVolumeTexture)(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9 **pp, HANDLE *shared) override;
    STDMETHOD(CreateCubeTexture)(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9 **pp, HANDLE *shared) override;
    STDMETHOD(CreateVertexBuffer)(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **pp, HANDLE *shared) override;
    STDMETHOD(CreateIndexBuffer)(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **pp, HANDLE *shared) override;
    STDMETHOD(CreateRenderTarget)(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD msq, WINBOOL lockable, IDirect3DSurface9 **pp, HANDLE *shared) override;
    STDMETHOD(CreateDepthStencilSurface)(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD msq, WINBOOL discard, IDirect3DSurface9 **pp, HANDLE *shared) override;
    STDMETHOD(UpdateSurface)(IDirect3DSurface9 *src, const RECT *srect, IDirect3DSurface9 *dst, const POINT *dpt) override;
    STDMETHOD(UpdateTexture)(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst) override;
    STDMETHOD(GetRenderTargetData)(IDirect3DSurface9 *rt, IDirect3DSurface9 *dst) override;
    STDMETHOD(GetFrontBufferData)(UINT sc, IDirect3DSurface9 *dst) override;
    STDMETHOD(StretchRect)(IDirect3DSurface9 *src, const RECT *srect, IDirect3DSurface9 *dst, const RECT *drect, D3DTEXTUREFILTERTYPE filter) override;
    STDMETHOD(ColorFill)(IDirect3DSurface9 *surf, const RECT *rect, D3DCOLOR color) override;
    STDMETHOD(CreateOffscreenPlainSurface)(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool, IDirect3DSurface9 **pp, HANDLE *shared) override;
    STDMETHOD(SetRenderTarget)(DWORD idx, IDirect3DSurface9 *rt) override;
    STDMETHOD(GetRenderTarget)(DWORD idx, IDirect3DSurface9 **pp) override;
    STDMETHOD(SetDepthStencilSurface)(IDirect3DSurface9 *ds) override;
    STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface9 **pp) override;
    STDMETHOD(BeginScene)() override { in_scene_ = true; return D3D_OK; }
    STDMETHOD(EndScene)() override { in_scene_ = false; return D3D_OK; }
    STDMETHOD(Clear)(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override;
    STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) override;
    STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE t, D3DMATRIX *m) override;
    STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE, const D3DMATRIX *) override { return D3D_OK; }
    STDMETHOD(SetViewport)(const D3DVIEWPORT9 *vp) override;
    STDMETHOD(GetViewport)(D3DVIEWPORT9 *vp) override { *vp = st_.vp; return D3D_OK; }
    STDMETHOD(SetMaterial)(const D3DMATERIAL9 *m) override { st_.material = *m; return D3D_OK; }
    STDMETHOD(GetMaterial)(D3DMATERIAL9 *m) override { *m = st_.material; return D3D_OK; }
    STDMETHOD(SetLight)(DWORD, const D3DLIGHT9 *) override { return D3D_OK; }
    STDMETHOD(LightEnable)(DWORD, WINBOOL) override { return D3D_OK; }
    STDMETHOD(SetClipPlane)(DWORD i, const float *p) override;
    STDMETHOD(GetClipPlane)(DWORD i, float *p) override;
    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE s, DWORD v) override;
    STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE s, DWORD *v) override;
    STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9 **pp) override;
    STDMETHOD(BeginStateBlock)() override;
    STDMETHOD(EndStateBlock)(IDirect3DStateBlock9 **pp) override;
    STDMETHOD(SetClipStatus)(const D3DCLIPSTATUS9 *) override { return D3D_OK; }
    STDMETHOD(GetClipStatus)(D3DCLIPSTATUS9 *cs) override { memset(cs, 0, sizeof *cs); return D3D_OK; }
    STDMETHOD(GetTexture)(DWORD stage, IDirect3DBaseTexture9 **pp) override;
    STDMETHOD(SetTexture)(DWORD stage, IDirect3DBaseTexture9 *tex) override;
    STDMETHOD(GetTextureStageState)(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD *v) override { *v = 0; return D3D_OK; }
    STDMETHOD(SetTextureStageState)(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) override { return D3D_OK; }   // fixed function is not supported
    STDMETHOD(GetSamplerState)(DWORD s, D3DSAMPLERSTATETYPE t, DWORD *v) override;
    STDMETHOD(SetSamplerState)(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) override;
    STDMETHOD(ValidateDevice)(DWORD *passes) override { if (passes) *passes = 1; return D3D_OK; }
    STDMETHOD(SetScissorRect)(const RECT *r) override { st_.scissor = *r; note_state(SC_SCISSOR); return D3D_OK; }
    STDMETHOD(GetScissorRect)(RECT *r) override { *r = st_.scissor; return D3D_OK; }
    STDMETHOD(SetSoftwareVertexProcessing)(WINBOOL) override { return D3D_OK; }
    STDMETHOD_(WINBOOL, GetSoftwareVertexProcessing)() override { return FALSE; }
    STDMETHOD(SetNPatchMode)(float) override { return D3D_OK; }
    STDMETHOD_(float, GetNPatchMode)() override { return 0.0f; }
    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE type, UINT start, UINT count) override;
    STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE type, INT base, UINT minv, UINT nverts, UINT start, UINT count) override;
    STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE type, UINT count, const void *data, UINT stride) override;
    STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE type, UINT minv, UINT nverts, UINT count, const void *idx, D3DFORMAT ifmt, const void *data, UINT stride) override;
    STDMETHOD(CreateVertexDeclaration)(const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **pp) override;
    STDMETHOD(SetVertexDeclaration)(IDirect3DVertexDeclaration9 *d) override;
    STDMETHOD(GetVertexDeclaration)(IDirect3DVertexDeclaration9 **pp) override;
    STDMETHOD(SetFVF)(DWORD fvf) override { st_.fvf = fvf; note_state(SC_DECL); return D3D_OK; }
    STDMETHOD(GetFVF)(DWORD *fvf) override { *fvf = st_.fvf; return D3D_OK; }
    STDMETHOD(CreateVertexShader)(const DWORD *code, IDirect3DVertexShader9 **pp) override;
    STDMETHOD(SetVertexShader)(IDirect3DVertexShader9 *s) override;
    STDMETHOD(GetVertexShader)(IDirect3DVertexShader9 **pp) override;
    STDMETHOD(SetVertexShaderConstantF)(UINT start, const float *data, UINT count) override;
    STDMETHOD(GetVertexShaderConstantF)(UINT start, float *data, UINT count) override;
    STDMETHOD(SetVertexShaderConstantI)(UINT start, const int *data, UINT count) override;
    STDMETHOD(GetVertexShaderConstantI)(UINT start, int *data, UINT count) override;
    STDMETHOD(SetVertexShaderConstantB)(UINT start, const WINBOOL *data, UINT count) override;
    STDMETHOD(GetVertexShaderConstantB)(UINT start, WINBOOL *data, UINT count) override;
    STDMETHOD(SetStreamSource)(UINT stream, IDirect3DVertexBuffer9 *vb, UINT offset, UINT stride) override;
    STDMETHOD(GetStreamSource)(UINT stream, IDirect3DVertexBuffer9 **pp, UINT *offset, UINT *stride) override;
    STDMETHOD(SetStreamSourceFreq)(UINT stream, UINT freq) override;
    STDMETHOD(GetStreamSourceFreq)(UINT stream, UINT *freq) override;
    STDMETHOD(SetIndices)(IDirect3DIndexBuffer9 *ib) override;
    STDMETHOD(GetIndices)(IDirect3DIndexBuffer9 **pp) override;
    STDMETHOD(CreatePixelShader)(const DWORD *code, IDirect3DPixelShader9 **pp) override;
    STDMETHOD(SetPixelShader)(IDirect3DPixelShader9 *s) override;
    STDMETHOD(GetPixelShader)(IDirect3DPixelShader9 **pp) override;
    STDMETHOD(SetPixelShaderConstantF)(UINT start, const float *data, UINT count) override;
    STDMETHOD(GetPixelShaderConstantF)(UINT start, float *data, UINT count) override;
    STDMETHOD(SetPixelShaderConstantI)(UINT start, const int *data, UINT count) override;
    STDMETHOD(GetPixelShaderConstantI)(UINT start, int *data, UINT count) override;
    STDMETHOD(SetPixelShaderConstantB)(UINT start, const WINBOOL *data, UINT count) override;
    STDMETHOD(GetPixelShaderConstantB)(UINT start, WINBOOL *data, UINT count) override;
    STDMETHOD(CreateQuery)(D3DQUERYTYPE type, IDirect3DQuery9 **pp) override;

    // ---- services used by resources / shaders / queries ---------------------------------------------------------------------------
    id<MTLDevice> mdev() const { return mdev_; }
    id<MTLCommandQueue> queue() const { return queue_; }
    dispatch_queue_t compile_queue() { return compile_qs_[compile_rr_++ % 4]; }
    uint64_t next_shader_id() { return ++shader_ids_; }
    uint64_t cur_serial();                        // serial of the command buffer commands are being recorded into (opens one if needed)
    uint64_t completed_serial() const { return completed_.load(std::memory_order_acquire); }
    void flush(bool wait);                        // end encoders, commit; optionally wait for the GPU
    void wait_serial(uint64_t serial);            // GPU work of that serial (and earlier) has finished
    void end_pass();                              // close the current render encoder (materialising a pending clear)
    // upload `bytes` to a level/slice region of a private texture through a staging buffer
    void gpu_upload(id<MTLTexture> t, uint32_t level, uint32_t slice, MTLOrigin o, MTLSize sz, const void *bytes, uint32_t row_pitch, uint32_t img_pitch);
    // copy a texture region to CPU memory (waits for the GPU)
    bool gpu_readback(id<MTLTexture> t, uint32_t level, uint32_t slice, MTLOrigin o, MTLSize sz, void *dst, uint32_t row_pitch);
    void gpu_generate_mips(id<MTLTexture> t);
    void gpu_copy_texture(id<MTLTexture> src, uint32_t slevel, uint32_t sslice, MTLOrigin so, MTLSize sz, id<MTLTexture> dst, uint32_t dlevel, uint32_t dslice, MTLOrigin dorg);
    // shared-storage buffer pool for dynamic data (vertex / index renames, upload staging): buffers come back once the GPU is done with them
    id<MTLBuffer> acquire_buffer(size_t size);
    void retire_buffer(id<MTLBuffer> b, uint64_t last_use_serial);
    bool query_occlusion_slot(MtlQuery *q);
    void occ_begin(MtlQuery *q);
    void occ_end(MtlQuery *q);
    uint64_t occ_result(MtlQuery *q);
    void query_issue_end(MtlQuery *q);
    bool query_poll(MtlQuery *q, bool flush_if_needed);
    id<MTLBuffer> visibility_buffer() const { return frames_[frame_idx_].vis; }
    IDirect3D9 *parent() const { return parent_; }
    SDL_Window *window() const { return window_; }
    CAMetalLayer *layer() const { return layer_; }
    const D3DPRESENT_PARAMETERS &present_params() const { return pp_; }
    D3DDEVICE_CREATION_PARAMETERS creation_params() const { return cp_; }
    MtlSurface *backbuffer() const { return backbuffer_.get(); }
    void live_inc() { live_++; }
    void live_dec() { live_--; }
    void note_state(uint32_t cat) { if (recording_) rec_cats_ |= cat; }
    void st_capture(DeviceState *dst, uint32_t cats);
    void st_apply(const DeviceState &src, uint32_t cats);
    // present (also called by the swap chain)
    HRESULT do_present();
    void pump_thermal();
    std::string describe_gpu() const;

private:
    friend class MtlSwapChain;
    friend class MtlStateBlock;
    // ---- command stream ---------------------------------------------------------------------------------------------------------
    struct FrameSlot { id<MTLBuffer> ring = nil; size_t used = 0; id<MTLBuffer> vis = nil; int vis_used = 0; };
    struct RingAlloc { id<MTLBuffer> buf; size_t off; uint8_t *ptr; };
    RingAlloc ring_alloc(size_t size, size_t align = 256);
    void begin_cmd();
    void end_blit();
    id<MTLBlitCommandEncoder> blit_enc();
    id<MTLRenderCommandEncoder> render_enc();
    void flush_clear();
    MTLRenderPassDescriptor *make_pass(bool use_pending_clear);
    bool pass_matches() const;
    // ---- state application --------------------------------------------------------------------------------------------------------
    id<MTLRenderPipelineState> get_pso(const PsoKey &k, ShaderCommon &vs, ShaderCommon *ps, MtlVertexDecl *decl, bool *ok);
    id<MTLDepthStencilState> get_dss();
    id<MTLSamplerState> get_sampler(int s, bool *srgb);
    bool prepare_draw(D3DPRIMITIVETYPE type, bool indexed);
    void apply_constants(ShaderCommon &sh, bool vertex, ConstState &cs);
    void apply_textures(ShaderCommon &sh, bool vertex);
    void apply_streams(MtlVertexDecl *decl);
    id<MTLRenderPipelineState> pso_for_pass(id<MTLFunction> vfn, id<MTLFunction> ffn, const PsoKey &k, MTLVertexDescriptor *vd, const char *label);
    void draw_clear_quad(DWORD flags, D3DCOLOR color, float z, DWORD stencil, const RECT *rect);
    void fill_default_state();
    void build_builtin_library();
    id<MTLRenderPipelineState> blit_pso(MTLPixelFormat fmt, bool filter_float);
    id<MTLRenderPipelineState> clear_pso(const PsoKey &k);
    void init_present_path();
    bool create_backbuffer();
    void release_backbuffer();
    void configure_layer();
    void save_archive();
    void archive_add(MTLRenderPipelineDescriptor *d);   // any thread
    struct PrimInfo { MTLPrimitiveType prim; uint32_t verts; bool fan; };
    bool prim_info(D3DPRIMITIVETYPE t, UINT count, PrimInfo *out);
    HRESULT draw_impl(D3DPRIMITIVETYPE type, INT base_vertex, UINT start, UINT count, bool indexed);
    HRESULT draw_up(D3DPRIMITIVETYPE type, UINT count, const void *idx, D3DFORMAT ifmt, const void *data, UINT stride, UINT nverts, UINT min_index);
    void set_rt_dirty();
    bool bind_sampler_texture(int slot, IDirect3DBaseTexture9 *tex);

    // ---- members ------------------------------------------------------------------------------------------------------------------
    IDirect3D9 *parent_ = nullptr;
    id<MTLDevice> mdev_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    dispatch_queue_t compile_qs_[4] = {nil, nil, nil, nil};
    std::atomic<uint32_t> compile_rr_{0};
    SDL_Window *window_ = nullptr;
    SDL_MetalView view_ = nullptr;
    CAMetalLayer *layer_ = nil;
    D3DPRESENT_PARAMETERS pp_{};
    D3DDEVICE_CREATION_PARAMETERS cp_{};
    DeviceState st_;
    Ref<MtlSurface> backbuffer_, auto_ds_;
    Ref<MtlSurface> rt_[4], ds_;
    MtlSwapChain *swapchain_ = nullptr;
    bool in_scene_ = false;
    std::atomic<int> live_{0};
    uint64_t shader_ids_ = 0;
    // state block recording
    bool recording_ = false;
    uint32_t rec_cats_ = 0;
    std::unique_ptr<DeviceState> rec_pre_;
    // GPU timeline
    FrameSlot frames_[3];
    int frame_idx_ = 0;
    dispatch_semaphore_t frame_sem_ = nil;
    id<MTLCommandBuffer> cmd_ = nil;
    id<MTLCommandBuffer> last_committed_ = nil;
    uint64_t serial_ = 0;                       // serial of cmd_ (or of the last one when cmd_ is nil)
    std::atomic<uint64_t> completed_{0};
    std::mutex done_mu_;                         // query waits sleep on done_cv_ until a command buffer completes
    std::condition_variable done_cv_;
    id<MTLRenderCommandEncoder> enc_ = nil;
    id<MTLBlitCommandEncoder> blit_ = nil;
    uint32_t enc_draws_ = 0;
    struct EncKey { void *rt[4]; uint32_t rt_level[4]; uint32_t rt_slice[4]; void *ds; } enc_key_{};
    struct PendingClear { bool color = false, depth = false, stencil = false; float c[4] = {0, 0, 0, 0}; double z = 1; uint32_t s = 0; } pend_;
    // applied encoder state (reset whenever a render encoder is created)
    struct Applied {
        void *pso = nullptr, *dss = nullptr;
        uint32_t stencil_ref = 0xFFFFFFFF;
        int cull = -1, fill = -1;
        float bias[3] = {1e30f, 0, 0};
        MTLViewport vp{-1, -1, -1, -1, -1, -1};
        MTLScissorRect sc{0xFFFFFFFFu, 0, 0, 0};
        float blend[4] = {-1, -1, -1, -1};
        void *vbuf[16] = {};
        uint32_t voff[16] = {};
        void *tex_ps[16] = {}, *tex_vs[4] = {};
        void *smp_ps[16] = {}, *smp_vs[4] = {};
        size_t vs_coff = 0, ps_coff = 0;
        void *vs_cbuf_raw = nullptr, *ps_cbuf_raw = nullptr;
        uint32_t ps_extra[4] = {~0u, ~0u, ~0u, ~0u};
        bool default_attr = false;
    } ap_;
    struct ConstCache { uint64_t sid = 0, ver = 0, epoch = 0; id<MTLBuffer> buf = nil; size_t off = 0; };
    ConstCache cc_[2];                           // last gathered constant block per stage (0 = vertex, 1 = pixel)
    struct UpData { id<MTLBuffer> vbuf = nil; size_t voff = 0; uint32_t vstride = 0; bool vactive = false; id<MTLBuffer> ibuf = nil; size_t ioff = 0; MTLIndexType itype = MTLIndexTypeUInt16; bool iactive = false; } up_;
    uint64_t vs_cver_ = 1, ps_cver_ = 1;        // bumped by every constant write
    uint64_t ring_epoch_ = 1;                   // bumped when the frame ring slot changes
    struct Retired { id<MTLBuffer> buf; uint64_t serial; };
    std::unordered_map<size_t, std::vector<Retired>> buffer_pool_;
    std::mutex pool_mu_;
    // caches
    // pipelines: archive hits are created inline; misses are built asynchronously by Metal and the draws that need them are
    // skipped until ready (like draws whose shaders are compiling), so a new material does not stall the render thread
    struct PsoEntry { id<MTLRenderPipelineState> pso = nil; std::atomic<int> state{0}; };   // 0 building, 1 ready, 2 failed
    std::unordered_map<PsoKey, std::shared_ptr<PsoEntry>, PsoKeyHash> pso_cache_;
    std::mutex archive_mu_;                      // archive_ is fed from Metal's completion threads
    std::unordered_map<uint64_t, id<MTLDepthStencilState>> dss_cache_;
    std::unordered_map<uint64_t, id<MTLSamplerState>> smp_cache_;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> misc_pso_;
    id<MTLLibrary> builtin_ = nil;
    id<MTLBinaryArchive> archive_ = nil;
    uint32_t archive_dirty_ = 0;
    std::string archive_path_;
    id<MTLTexture> dummy_tex_[3] = {nil, nil, nil};
    id<MTLSamplerState> dummy_smp_ = nil;
    id<MTLBuffer> default_attr_buf_ = nil;
    // present path
    id<MTLTexture> upscale_out_ = nil;
    id<NSObject> spatial_scaler_ = nil;
    uint32_t scaler_in_w_ = 0, scaler_in_h_ = 0, scaler_out_w_ = 0, scaler_out_h_ = 0;
    bool metalfx_ok_ = false;
    int64_t last_present_ns_ = 0;
    bool signal_frame_ = false;
    bool enc_srgb_ = false;
    bool thermal_serious_ = false;
    id<MTLSamplerState> present_smp_[2] = {nil, nil};
    MtlQuery *active_occ_ = nullptr;
    uint64_t draw_unsupported_ = 0;
    // D3D host objects
    D3DCAPS9 caps_{};
};

class MtlSwapChain : public Com<Stub_IDirect3DSwapChain9, IDirect3DSwapChain9> {
public:
    MtlDevice *dev = nullptr;
    STDMETHOD_(ULONG, AddRef)() override { return dev->AddRef(); }
    STDMETHOD_(ULONG, Release)() override { return dev->Release(); }
    STDMETHOD(Present)(const RECT *a, const RECT *b, HWND c, const RGNDATA *d, DWORD) override { return dev->Present(a, b, c, d); }
    STDMETHOD(GetFrontBufferData)(IDirect3DSurface9 *s) override { return dev->GetFrontBufferData(0, s); }
    STDMETHOD(GetBackBuffer)(UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9 **pp) override { return dev->GetBackBuffer(0, i, t, pp); }
    STDMETHOD(GetRasterStatus)(D3DRASTER_STATUS *rs) override { return dev->GetRasterStatus(0, rs); }
    STDMETHOD(GetDisplayMode)(D3DDISPLAYMODE *m) override { return dev->GetDisplayMode(0, m); }
    STDMETHOD(GetDevice)(IDirect3DDevice9 **pp) override { *pp = dev; dev->AddRef(); return D3D_OK; }
    STDMETHOD(GetPresentParameters)(D3DPRESENT_PARAMETERS *pp) override { *pp = dev->present_params(); return D3D_OK; }
    void destroy() override {}   // owned by the device
};

// IDirect3D9
class MtlD3D9 : public Com<Stub_IDirect3D9, IDirect3D9> {
public:
    MtlD3D9();
    STDMETHOD_(UINT, GetAdapterCount)() override { return 1; }
    STDMETHOD(GetAdapterIdentifier)(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9 *id) override;
    STDMETHOD_(UINT, GetAdapterModeCount)(UINT adapter, D3DFORMAT fmt) override;
    STDMETHOD(EnumAdapterModes)(UINT adapter, D3DFORMAT fmt, UINT mode, D3DDISPLAYMODE *m) override;
    STDMETHOD(GetAdapterDisplayMode)(UINT adapter, D3DDISPLAYMODE *m) override;
    STDMETHOD(CheckDeviceType)(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_fmt, D3DFORMAT back_fmt, WINBOOL windowed) override;
    STDMETHOD(CheckDeviceFormat)(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_fmt, DWORD usage, D3DRESOURCETYPE rtype, D3DFORMAT check) override;
    STDMETHOD(CheckDeviceMultiSampleType)(UINT adapter, D3DDEVTYPE type, D3DFORMAT fmt, WINBOOL windowed, D3DMULTISAMPLE_TYPE ms, DWORD *quality) override;
    STDMETHOD(CheckDepthStencilMatch)(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_fmt, D3DFORMAT rt_fmt, D3DFORMAT ds_fmt) override;
    STDMETHOD(CheckDeviceFormatConversion)(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT) override { return D3D_OK; }
    STDMETHOD(GetDeviceCaps)(UINT adapter, D3DDEVTYPE type, D3DCAPS9 *caps) override;
    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT) override { return (HMONITOR)(uintptr_t)0x10001; }
    STDMETHOD(CreateDevice)(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) override;
    // display modes the game is offered (internal resolutions)
    static void default_modes(std::vector<std::pair<int, int>> *out);
};

void fill_caps_table(D3DCAPS9 &c);
void gpu_stats_json(std::string *out);   // for the on-device acceptance app

}  // namespace mtl
}  // namespace dish
