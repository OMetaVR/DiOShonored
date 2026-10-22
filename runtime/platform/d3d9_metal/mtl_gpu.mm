// Metal backend: device lifetime, command stream, encoders, uploads/readbacks, queries, presentation (CAMetalLayer + MetalFX).
#include "mtl_device.h"
#include "mtl_shadercache.h"
#include "dish/lifecycle.h"
#include "dish/vfs.h"
#include "dish/mainthread.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sys/stat.h>

namespace dish {
namespace mtl {

static inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// built-in shaders: full-screen triangle blit (present, StretchRect) and clear quad
static const char *kBuiltinMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct BlitOut { float4 pos [[position]]; float2 uv; };
vertex BlitOut dish_blit_vs(uint vid [[vertex_id]], constant float4 &uvrect [[buffer(0)]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    BlitOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = uvrect.xy + float2(p.x, 1.0 - p.y) * uvrect.zw;
    return o;
}
fragment float4 dish_blit_fs(BlitOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return t.sample(s, in.uv);
}
struct ClearU { float4 color; float depth; float3 pad; };
vertex float4 dish_clear_vs(uint vid [[vertex_id]], constant ClearU &u [[buffer(0)]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    return float4(p * 2.0 - 1.0, u.depth, 1.0);
}
struct ClearOut { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; };
fragment ClearOut dish_clear_fs(constant ClearU &u [[buffer(0)]]) {
    ClearOut o;
    o.c0 = u.color; o.c1 = u.color; o.c2 = u.color; o.c3 = u.color;
    return o;
}
)MSL";

size_t PsoKeyHash::operator()(const PsoKey &k) const {
    const uint8_t *p = (const uint8_t *)&k;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof k; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return (size_t)h;
}

static MtlDevice *g_primary = nullptr;   // for the lifecycle hook

MtlDevice::MtlDevice() = default;
MtlDevice::~MtlDevice() = default;

std::string MtlDevice::describe_gpu() const {
    return mdev_ ? std::string([[mdev_ name] UTF8String]) : "(no device)";
}

// ---- init / teardown -------------------------------------------------------------------------------------------------------------
bool MtlDevice::init(IDirect3D9 *parent, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS *pp) {
    parent_ = parent;
    mdev_ = MTLCreateSystemDefaultDevice();
    if (!mdev_) { LOGE(LC_GFX, "metal: no Metal device"); return false; }
    queue_ = [mdev_ newCommandQueue];
    // background shader compiles: a few serial queues used round-robin (a concurrent queue would keep adding threads while compiles
    // block, starving the game's threads and the GPU driver); compiles a draw is waiting for go to a high-priority queue instead
    dispatch_queue_attr_t qa = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
    for (auto &q : compile_qs_) q = dispatch_queue_create("dish.d3d9.shader-compile", qa);
    fill_caps_table(caps_);
    cp_.AdapterOrdinal = adapter;
    cp_.DeviceType = type;
    cp_.hFocusWindow = focus;
    cp_.BehaviorFlags = behavior;
    HWND w = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    window_ = reinterpret_cast<SDL_Window *>(w);
    if (!window_) { LOGE(LC_GFX, "metal: CreateDevice without a window"); return false; }
    view_ = SDL_Metal_CreateView(window_);
    if (!view_) { LOGE(LC_GFX, "metal: SDL_Metal_CreateView failed: %s", SDL_GetError()); return false; }
    layer_ = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view_);
    if (!layer_) { LOGE(LC_GFX, "metal: no CAMetalLayer"); return false; }
    configure_layer();
    pp_ = *pp;
    frame_sem_ = dispatch_semaphore_create(3);
    const size_t ring_size = 8u << 20;
    for (FrameSlot &f : frames_) {
        f.ring = [mdev_ newBufferWithLength:ring_size options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        f.vis = [mdev_ newBufferWithLength:4096 * 8 options:MTLResourceStorageModeShared];
        memset(f.vis.contents, 0, f.vis.length);
        memacct_gpu_add((int64_t)(ring_size + f.vis.length));
    }
    float defattr[4] = {0, 0, 0, 1};
    default_attr_buf_ = [mdev_ newBufferWithBytes:defattr length:sizeof defattr options:MTLResourceStorageModeShared];
    build_builtin_library();
    init_present_path();
    // dummy textures bound when the application leaves a sampler empty (opaque black)
    {
        MTLTextureDescriptor *d2 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
        d2.storageMode = MTLStorageModeShared;
        dummy_tex_[0] = [mdev_ newTextureWithDescriptor:d2];
        const uint8_t black[4] = {0, 0, 0, 255};
        [dummy_tex_[0] replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:black bytesPerRow:4];
        MTLTextureDescriptor *dc = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm size:1 mipmapped:NO];
        dc.storageMode = MTLStorageModeShared;
        dummy_tex_[1] = [mdev_ newTextureWithDescriptor:dc];
        for (int f = 0; f < 6; f++) [dummy_tex_[1] replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:(NSUInteger)f withBytes:black bytesPerRow:4 bytesPerImage:4];
        MTLTextureDescriptor *d3 = [[MTLTextureDescriptor alloc] init];
        d3.textureType = MTLTextureType3D; d3.pixelFormat = MTLPixelFormatBGRA8Unorm; d3.width = d3.height = d3.depth = 1; d3.storageMode = MTLStorageModeShared;
        d3.usage = MTLTextureUsageShaderRead;
        dummy_tex_[2] = [mdev_ newTextureWithDescriptor:d3];
        [dummy_tex_[2] replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:black bytesPerRow:4 bytesPerImage:4];
        MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
        dummy_smp_ = [mdev_ newSamplerStateWithDescriptor:sd];
    }
    st_ = DeviceState();
    if (!create_backbuffer()) return false;
    *pp = pp_;
    swapchain_ = new MtlSwapChain();
    swapchain_->dev = this;
    if (!g_primary) {
        g_primary = this;
        lifecycle_add_hook([](bool paused) { if (paused && g_primary) g_primary->save_archive(); });
    }
    LOGI(LC_GFX, "metal d3d9 device: %s, backbuffer %ux%u, drawable %.0fx%.0f, MetalFX spatial %s, ASTC transcode %s", describe_gpu().c_str(), pp_.BackBufferWidth,
         pp_.BackBufferHeight, layer_.drawableSize.width, layer_.drawableSize.height, metalfx_ok_ ? "available" : "unavailable", options().astc ? "on" : "off");
    return true;
}

void MtlDevice::destroy() {
    if (g_primary == this) { save_archive(); g_primary = nullptr; }
    flush(true);
    for (auto &r : rt_) r.reset();
    ds_.reset();
    backbuffer_.reset();
    auto_ds_.reset();
    st_ = DeviceState();
    SDL_MetalView v = view_;
    view_ = nullptr;
    if (v) mainthread_call([v] { SDL_Metal_DestroyView(v); });
    if (live_.load() > 0) {
        LOGW(LC_GFX, "metal: device released with %d live resources (device storage is leaked)", live_.load());
        return;
    }
    delete swapchain_;
    swapchain_ = nullptr;
    delete this;
}

void MtlDevice::configure_layer() {
    layer_.device = mdev_;
    layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer_.framebufferOnly = YES;
    layer_.maximumDrawableCount = 3;
    layer_.allowsNextDrawableTimeout = YES;
    layer_.opaque = YES;
}

void MtlDevice::build_builtin_library() {
    NSError *err = nil;
    MTLCompileOptions *co = [[MTLCompileOptions alloc] init];
    builtin_ = [mdev_ newLibraryWithSource:[NSString stringWithUTF8String:kBuiltinMsl] options:co error:&err];
    if (!builtin_) LOGE(LC_GFX, "metal: built-in shaders failed to compile: %s", err ? [[err localizedDescription] UTF8String] : "?");
}

void MtlDevice::init_present_path() {
    metalfx_ok_ = false;
    if (@available(iOS 16.0, *)) {
        if (options().upscale && [MTLFXSpatialScalerDescriptor supportsDevice:mdev_]) metalfx_ok_ = true;
    }
    // pipeline archive
    if (@available(iOS 14.0, *)) {
        const std::string &dir = options().cache_dir;
        if (!dir.empty()) {
            [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:dir.c_str()] withIntermediateDirectories:YES attributes:nil error:nil];
            remove((dir + "/pipelines.archive").c_str());   // the v1 archive holds depth-only pipelines that cannot be packed again
            archive_path_ = pipeline_archive_path();
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:archive_path_.c_str()]];
            MTLBinaryArchiveDescriptor *ad = [[MTLBinaryArchiveDescriptor alloc] init];
            NSError *err = nil;
            struct stat st;
            if (stat(archive_path_.c_str(), &st) == 0) {
                ad.url = url;
                archive_ = [mdev_ newBinaryArchiveWithDescriptor:ad error:&err];
                if (!archive_) { LOGW(LC_GFX, "metal: pipeline archive unusable (%s); starting a new one", err ? [[err localizedDescription] UTF8String] : "?"); remove(archive_path_.c_str()); }
            }
            if (!archive_) {
                ad.url = nil;
                archive_ = [mdev_ newBinaryArchiveWithDescriptor:ad error:&err];
            }
        }
    }
}

void MtlDevice::save_archive() {
    if (@available(iOS 14.0, *)) {
        std::lock_guard<std::mutex> lk(archive_mu_);
        if (!archive_ || archive_dirty_ == 0) return;
        NSError *err = nil;
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:archive_path_.c_str()]];
        if (![archive_ serializeToURL:url error:&err]) {
            // a pipeline the archive cannot pack poisons every later save: start a fresh archive (the saved file stays as it was)
            LOGW(LC_GFX, "metal: cannot save the pipeline archive, starting a new one: %s", err ? [[err localizedDescription] UTF8String] : "?");
            MTLBinaryArchiveDescriptor *ad = [[MTLBinaryArchiveDescriptor alloc] init];
            archive_ = [mdev_ newBinaryArchiveWithDescriptor:ad error:nil];
        } else {
            LOGI(LC_GFX, "metal: pipeline archive saved (%u new pipelines)", archive_dirty_);
        }
        archive_dirty_ = 0;
    }
}

// ---- backbuffer -------------------------------------------------------------------------------------------------------------------------
static MtlSurface *new_standalone_surface(MtlDevice *dev, D3DFORMAT fmt, uint32_t w, uint32_t h, DWORD usage, D3DPOOL pool, bool *ok) {
    MtlSurface *s = new MtlSurface();
    s->own.reset(new TexData());
    *ok = s->own->create(dev, fmt, w, h, 1, 1, usage, pool, false, false);
    s->td = s->own.get();
    if (!*ok) { s->own->release(); s->own.reset(); }
    return s;
}

bool MtlDevice::create_backbuffer() {
    CGSize ds = layer_.drawableSize;
    uint32_t bw = pp_.BackBufferWidth, bh = pp_.BackBufferHeight;
    if (!bw || !bh) {
        bh = (uint32_t)std::min<double>(options().internal_height, ds.height > 0 ? ds.height : options().internal_height);
        bw = (uint32_t)(bh * (ds.height > 0 ? ds.width / ds.height : 16.0 / 9.0) + 1) & ~1u;
    }
    D3DFORMAT fmt = pp_.BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : pp_.BackBufferFormat;
    if (fmt != D3DFMT_A8R8G8B8 && fmt != D3DFMT_X8R8G8B8) fmt = D3DFMT_X8R8G8B8;
    bool ok = false;
    MtlSurface *bb = new_standalone_surface(this, fmt, bw, bh, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, &ok);
    if (!ok) { delete bb; LOGE(LC_GFX, "metal: cannot create the %ux%u back buffer", bw, bh); return false; }
    bb->is_backbuffer = true;
    backbuffer_ = Ref<MtlSurface>::adopt(bb);
    pp_.BackBufferWidth = bw;
    pp_.BackBufferHeight = bh;
    pp_.BackBufferFormat = fmt;
    if (pp_.BackBufferCount == 0) pp_.BackBufferCount = 1;
    auto_ds_.reset();
    if (pp_.EnableAutoDepthStencil) {
        D3DFORMAT dsf = pp_.AutoDepthStencilFormat ? pp_.AutoDepthStencilFormat : D3DFMT_D24S8;
        MtlSurface *dsurf = new_standalone_surface(this, dsf, bw, bh, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, &ok);
        if (!ok) { delete dsurf; LOGE(LC_GFX, "metal: cannot create the auto depth-stencil"); return false; }
        auto_ds_ = Ref<MtlSurface>::adopt(dsurf);
    }
    rt_[0] = backbuffer_;
    for (int i = 1; i < 4; i++) rt_[i].reset();
    ds_ = auto_ds_;
    st_.vp = D3DVIEWPORT9{0, 0, bw, bh, 0.0f, 1.0f};
    st_.scissor = RECT{0, 0, (LONG)bw, (LONG)bh};
    return true;
}

void MtlDevice::release_backbuffer() {
    end_pass();
    for (auto &r : rt_) r.reset();
    ds_.reset();
    backbuffer_.reset();
    auto_ds_.reset();
}

// ---- GPU buffer pool ---------------------------------------------------------------------------------------------------------------------
id<MTLBuffer> MtlDevice::acquire_buffer(size_t size) {
    size_t cls = (size + 255) & ~(size_t)255;
    {
        std::lock_guard<std::mutex> lk(pool_mu_);
        auto it = buffer_pool_.find(cls);
        if (it != buffer_pool_.end()) {
            auto &v = it->second;
            const uint64_t done = completed_.load(std::memory_order_acquire);
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i].serial <= done) {
                    id<MTLBuffer> b = v[i].buf;
                    v.erase(v.begin() + (long)i);
                    return b;
                }
            }
        }
    }
    return [mdev_ newBufferWithLength:cls options:MTLResourceStorageModeShared];
}

void MtlDevice::retire_buffer(id<MTLBuffer> b, uint64_t serial) {
    if (!b) return;
    size_t cls = b.length;
    std::lock_guard<std::mutex> lk(pool_mu_);
    auto &v = buffer_pool_[cls];
    if (v.size() < 16) v.push_back({b, serial});
}

// ---- command stream ------------------------------------------------------------------------------------------------------------------------
void MtlDevice::begin_cmd() {
    if (cmd_) return;
    cmd_ = [queue_ commandBuffer];
    serial_++;
    stats().command_buffers++;
}

uint64_t MtlDevice::cur_serial() {
    begin_cmd();
    return serial_;
}

void MtlDevice::end_blit() {
    if (blit_) { [blit_ endEncoding]; blit_ = nil; }
}

id<MTLBlitCommandEncoder> MtlDevice::blit_enc() {
    if (blit_) return blit_;
    end_pass();
    begin_cmd();
    blit_ = [cmd_ blitCommandEncoder];
    return blit_;
}

void MtlDevice::flush(bool wait) {
    end_pass();
    end_blit();
    if (cmd_) {
        id<MTLCommandBuffer> cb = cmd_;
        cmd_ = nil;
        const uint64_t s = serial_;
        const bool signal = signal_frame_;
        signal_frame_ = false;
        dispatch_semaphore_t sem = frame_sem_;
        std::atomic<uint64_t> *done = &completed_;
        MtlDevice *self = this;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
            if (c.error) LOGE(LC_GFX, "metal: command buffer %llu failed: %s", (unsigned long long)s, [[c.error localizedDescription] UTF8String]);
            if (c.GPUEndTime > c.GPUStartTime) stats().gpu_busy_ns += (uint64_t)((c.GPUEndTime - c.GPUStartTime) * 1e9);
            uint64_t prev = done->load();
            while (prev < s && !done->compare_exchange_weak(prev, s)) {}
            { std::lock_guard<std::mutex> lk(self->done_mu_); }
            self->done_cv_.notify_all();
            if (signal) dispatch_semaphore_signal(sem);
        }];
        [cb commit];
        last_committed_ = cb;
        if (wait) {
            [cb waitUntilCompleted];
            uint64_t prev = completed_.load();
            while (prev < s && !completed_.compare_exchange_weak(prev, s)) {}
        }
    } else if (wait && last_committed_) {
        [last_committed_ waitUntilCompleted];
        uint64_t prev = completed_.load();
        while (prev < serial_ && !completed_.compare_exchange_weak(prev, serial_)) {}
    }
}

void MtlDevice::wait_serial(uint64_t serial) {
    if (serial <= completed_.load(std::memory_order_acquire)) return;
    flush(true);
}

void MtlDevice::end_pass() {
    if (enc_) {
        if (active_occ_) [enc_ setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        [enc_ endEncoding];
        enc_ = nil;
        enc_draws_ = 0;
    } else if (pend_.color || pend_.depth || pend_.stencil) {
        flush_clear();
    }
    pend_ = PendingClear();
}

void MtlDevice::flush_clear() {
    if (!(pend_.color || pend_.depth || pend_.stencil)) return;
    if (render_enc()) {
        [enc_ endEncoding];
        enc_ = nil;
        enc_draws_ = 0;
    }
    pend_ = PendingClear();
}

MTLRenderPassDescriptor *MtlDevice::make_pass(bool use_pending_clear) {
    const bool srgb = st_.rs[D3DRS_SRGBWRITEENABLE] != 0;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    bool any = false;
    for (int i = 0; i < 4; i++) {
        MtlSurface *s = rt_[i].get();
        if (!s || !s->td->tex) continue;
        MTLRenderPassColorAttachmentDescriptor *ca = rp.colorAttachments[i];
        ca.texture = s->td->render_texture(srgb);
        ca.level = s->level;
        ca.slice = s->td->cube ? s->face : 0;
        ca.storeAction = MTLStoreActionStore;
        if (use_pending_clear && pend_.color) {
            ca.loadAction = MTLLoadActionClear;
            ca.clearColor = MTLClearColorMake(pend_.c[0], pend_.c[1], pend_.c[2], pend_.c[3]);
        } else {
            ca.loadAction = MTLLoadActionLoad;
        }
        s->td->tex_written_by_gpu = true;
        any = true;
    }
    MtlSurface *d = ds_.get();
    if (d && d->td->tex) {
        const FormatInfo *fi = d->td->fi;
        MTLRenderPassDepthAttachmentDescriptor *da = rp.depthAttachment;
        da.texture = d->td->tex;
        da.level = d->level;
        da.storeAction = MTLStoreActionStore;
        if (use_pending_clear && pend_.depth) { da.loadAction = MTLLoadActionClear; da.clearDepth = pend_.z; }
        else da.loadAction = MTLLoadActionLoad;
        if (fi->stencil) {
            MTLRenderPassStencilAttachmentDescriptor *sa = rp.stencilAttachment;
            sa.texture = d->td->tex;
            sa.level = d->level;
            sa.storeAction = MTLStoreActionStore;
            if (use_pending_clear && pend_.stencil) { sa.loadAction = MTLLoadActionClear; sa.clearStencil = pend_.s; }
            else sa.loadAction = MTLLoadActionLoad;
        }
        any = true;
    }
    if (!any) return nil;
    rp.visibilityResultBuffer = frames_[frame_idx_].vis;
    return rp;
}

id<MTLRenderCommandEncoder> MtlDevice::render_enc() {
    if (enc_) return enc_;
    end_blit();
    MTLRenderPassDescriptor *rp = make_pass(true);
    if (!rp) return nil;
    begin_cmd();
    enc_ = [cmd_ renderCommandEncoderWithDescriptor:rp];
    enc_srgb_ = st_.rs[D3DRS_SRGBWRITEENABLE] != 0;
    pend_ = PendingClear();
    ap_ = Applied();
    enc_draws_ = 0;
    if (active_occ_) [enc_ setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(NSUInteger)active_occ_->vis_slot * 8];
    // GPU use bookkeeping for readbacks / locks of the attachments
    const uint64_t s = serial_;
    for (int i = 0; i < 4; i++) if (rt_[i]) rt_[i]->td->last_use = s;
    if (ds_) ds_->td->last_use = s;
    return enc_;
}

// ---- ring allocator for constants / transient vertex data --------------------------------------------------------------------------------------
MtlDevice::RingAlloc MtlDevice::ring_alloc(size_t size, size_t align) {
    FrameSlot &f = frames_[frame_idx_];
    size_t off = (f.used + align - 1) & ~(align - 1);
    stats().ring_bytes += size;
    if (off + size <= f.ring.length) {
        f.used = off + size;
        return {f.ring, off, (uint8_t *)f.ring.contents + off};
    }
    // frame ring exhausted: a one-off buffer that lives as long as the commands referencing it
    id<MTLBuffer> b = [mdev_ newBufferWithLength:std::max<size_t>(size, 256) options:MTLResourceStorageModeShared];
    return {b, 0, (uint8_t *)b.contents};
}

// ---- uploads / readbacks -----------------------------------------------------------------------------------------------------------------------
void MtlDevice::gpu_upload(id<MTLTexture> t, uint32_t level, uint32_t slice, MTLOrigin o, MTLSize sz, const void *bytes, uint32_t row_pitch, uint32_t img_pitch) {
    const bool compressed = t.pixelFormat == MTLPixelFormatASTC_4x4_LDR || t.pixelFormat == MTLPixelFormatASTC_4x4_sRGB;
    const size_t rows = compressed ? (sz.height + 3) / 4 : sz.height;
    if (img_pitch == 0) img_pitch = (uint32_t)(row_pitch * rows);
    const size_t len = (size_t)img_pitch * sz.depth;
    id<MTLBuffer> stage = acquire_buffer(len);
    memcpy(stage.contents, bytes, std::min(len, (size_t)row_pitch * rows * sz.depth));
    id<MTLBlitCommandEncoder> b = blit_enc();
    [b copyFromBuffer:stage sourceOffset:0 sourceBytesPerRow:row_pitch sourceBytesPerImage:img_pitch sourceSize:sz toTexture:t destinationSlice:slice destinationLevel:level destinationOrigin:o];
    retire_buffer(stage, serial_);
}

bool MtlDevice::gpu_readback(id<MTLTexture> t, uint32_t level, uint32_t slice, MTLOrigin o, MTLSize sz, void *dst, uint32_t row_pitch) {
    const MTLPixelFormat pf = t.pixelFormat;
    // bytes per texel of the GPU format for the formats we can read back
    uint32_t bpp = 0;
    switch (pf) {
        case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatBGR10A2Unorm:
        case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG16Float: case MTLPixelFormatR32Float: case MTLPixelFormatRG16Snorm: case MTLPixelFormatRGBA8Snorm: bpp = 4; break;
        case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Float: case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA16Snorm: bpp = 8; break;
        case MTLPixelFormatRGBA32Float: bpp = 16; break;
        case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatBGR5A1Unorm: case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Snorm: case MTLPixelFormatR16Unorm:
        case MTLPixelFormatR16Float: bpp = 2; break;
        case MTLPixelFormatR8Unorm: case MTLPixelFormatA8Unorm: bpp = 1; break;
        default: return false;
    }
    const size_t row = (size_t)sz.width * bpp;
    const size_t tight = ((row + 15) & ~(size_t)15);   // Metal wants aligned row pitches for texture -> buffer copies
    const size_t len = tight * sz.height * sz.depth;
    id<MTLBuffer> stage = acquire_buffer(len);
    id<MTLBlitCommandEncoder> b = blit_enc();
    [b copyFromTexture:t sourceSlice:slice sourceLevel:level sourceOrigin:o sourceSize:sz toBuffer:stage destinationOffset:0 destinationBytesPerRow:tight destinationBytesPerImage:tight * sz.height];
    flush(true);
    for (NSUInteger r = 0; r < sz.height * sz.depth; r++) memcpy((uint8_t *)dst + (size_t)r * row_pitch, (uint8_t *)stage.contents + r * tight, row);
    retire_buffer(stage, serial_);
    return true;
}

void MtlDevice::gpu_generate_mips(id<MTLTexture> t) {
    if (t.pixelFormat == MTLPixelFormatASTC_4x4_LDR) return;
    id<MTLBlitCommandEncoder> b = blit_enc();
    [b generateMipmapsForTexture:t];
}

void MtlDevice::gpu_copy_texture(id<MTLTexture> src, uint32_t sl, uint32_t ss, MTLOrigin so, MTLSize sz, id<MTLTexture> dst, uint32_t dl, uint32_t ds, MTLOrigin dorg) {
    id<MTLBlitCommandEncoder> b = blit_enc();
    [b copyFromTexture:src sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:sz toTexture:dst destinationSlice:ds destinationLevel:dl destinationOrigin:dorg];
}

// ---- queries ----------------------------------------------------------------------------------------------------------------------------------
bool MtlDevice::query_occlusion_slot(MtlQuery *q) {
    FrameSlot &f = frames_[frame_idx_];
    if (f.vis_used >= (int)(f.vis.length / 8)) return false;
    q->vis_slot = f.vis_used++;
    q->vis_frame = frame_idx_;
    q->result = 0;
    return true;
}

void MtlDevice::query_issue_end(MtlQuery *q) { q->serial = cur_serial(); }

bool MtlDevice::query_poll(MtlQuery *q, bool flush_if_needed) {
    if (q->serial <= completed_.load(std::memory_order_acquire)) return true;
    // Real D3D9 drivers submit pending work when a query is polled even without D3DGETDATA_FLUSH, and UE3 polls with flags 0: without
    // this the query waited in the open command buffer until Present, the GPU started each frame late and the render thread spun.
    // Only event queries (the frame-pacing waits): occlusion results are polled without needing an answer, and flushing or waiting
    // for each of those would split render passes and stall.
    const bool event = q->type == D3DQUERYTYPE_EVENT;
    if (cmd_ && serial_ == q->serial && (event || flush_if_needed)) flush(false);
    if (!event) return q->serial <= completed_.load(std::memory_order_acquire);
    // Not done yet: sleep until a command buffer completes (at most 0.5 ms) instead of returning at once. UE3 calls GetData in a
    // loop that also reads the clock; without the wait that loop kept a CPU core busy for nothing.
    std::unique_lock<std::mutex> lk(done_mu_);
    done_cv_.wait_for(lk, std::chrono::microseconds(500), [&] { return q->serial <= completed_.load(std::memory_order_acquire); });
    return q->serial <= completed_.load(std::memory_order_acquire);
}

// ---- presentation -------------------------------------------------------------------------------------------------------------------------------
void MtlDevice::pump_thermal() {
    static NSProcessInfoThermalState state = NSProcessInfoThermalStateNominal;
    state = [[NSProcessInfo processInfo] thermalState];
    thermal_serious_ = state >= NSProcessInfoThermalStateSerious;
}

id<MTLRenderPipelineState> MtlDevice::blit_pso(MTLPixelFormat fmt, bool) {
    uint64_t key = 0xB11700000000ull | (uint64_t)fmt;
    auto it = misc_pso_.find(key);
    if (it != misc_pso_.end()) return it->second;
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = [builtin_ newFunctionWithName:@"dish_blit_vs"];
    d.fragmentFunction = [builtin_ newFunctionWithName:@"dish_blit_fs"];
    d.colorAttachments[0].pixelFormat = fmt;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [mdev_ newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) LOGE(LC_GFX, "metal: blit pipeline failed: %s", err ? [[err localizedDescription] UTF8String] : "?");
    misc_pso_[key] = p;
    return p;
}

HRESULT MtlDevice::do_present() {
    pump_thermal();
    end_pass();
    MtlSurface *bb = backbuffer_.get();
    if (!bb || !bb->td->tex) return D3DERR_INVALIDCALL;
    @autoreleasepool {
        id<CAMetalDrawable> dr = [layer_ nextDrawable];
        if (!dr) { LOGW(LC_GFX, "metal: no drawable (skipping a present)"); signal_frame_ = true; flush(false); dispatch_semaphore_wait(frame_sem_, DISPATCH_TIME_FOREVER); return D3D_OK; }
        end_blit();
        begin_cmd();
        const double dw = (double)dr.texture.width, dh = (double)dr.texture.height;
        const double sw = bb->td->w, sh = bb->td->h;
        const double scale = std::min(dw / sw, dh / sh);
        const double fw = std::floor(sw * scale), fh = std::floor(sh * scale);
        id<MTLTexture> src = bb->td->tex;
        id<MTLTexture> sampled = bb->td->sample_view;
        bool used_fx = false;
        if (metalfx_ok_ && scale > 1.15) {
            if (@available(iOS 16.0, *)) {
                const uint32_t ow = (uint32_t)fw, oh = (uint32_t)fh;
                if (!spatial_scaler_ || scaler_in_w_ != sw || scaler_in_h_ != sh || scaler_out_w_ != ow || scaler_out_h_ != oh) {
                    MTLFXSpatialScalerDescriptor *sd = [[MTLFXSpatialScalerDescriptor alloc] init];
                    sd.colorTextureFormat = src.pixelFormat;
                    sd.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
                    sd.inputWidth = (NSUInteger)sw; sd.inputHeight = (NSUInteger)sh;
                    sd.outputWidth = ow; sd.outputHeight = oh;
                    sd.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
                    spatial_scaler_ = [sd newSpatialScalerWithDevice:mdev_];
                    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:ow height:oh mipmapped:NO];
                    td.storageMode = MTLStorageModePrivate;
                    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                    upscale_out_ = [mdev_ newTextureWithDescriptor:td];
                    scaler_in_w_ = (uint32_t)sw; scaler_in_h_ = (uint32_t)sh; scaler_out_w_ = ow; scaler_out_h_ = oh;
                    if (!spatial_scaler_ || !upscale_out_) { LOGW(LC_GFX, "metal: MetalFX spatial scaler unavailable, using the bilinear blit"); metalfx_ok_ = false; spatial_scaler_ = nil; upscale_out_ = nil; }
                }
                if (spatial_scaler_) {
                    id<MTLFXSpatialScaler> sc = (id<MTLFXSpatialScaler>)spatial_scaler_;
                    sc.colorTexture = src;
                    sc.outputTexture = upscale_out_;
                    sc.inputContentWidth = (NSUInteger)sw;
                    sc.inputContentHeight = (NSUInteger)sh;
                    [sc encodeToCommandBuffer:cmd_];
                    sampled = upscale_out_;
                    used_fx = true;
                }
            }
        }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dr.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:rp];
        id<MTLRenderPipelineState> pso = blit_pso(dr.texture.pixelFormat, true);
        if (pso) {
            [e setRenderPipelineState:pso];
            MTLViewport vp = {(dw - fw) * 0.5, (dh - fh) * 0.5, fw, fh, 0, 1};
            [e setViewport:vp];
            float uvr[4] = {0, 0, 1, 1};
            [e setVertexBytes:uvr length:sizeof uvr atIndex:0];
            [e setFragmentTexture:sampled atIndex:0];
            MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
            sd.minFilter = sd.magFilter = (used_fx || scale == 1.0) ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
            if (!present_smp_[used_fx || scale == 1.0 ? 0 : 1]) present_smp_[used_fx || scale == 1.0 ? 0 : 1] = [mdev_ newSamplerStateWithDescriptor:sd];
            [e setFragmentSamplerState:present_smp_[used_fx || scale == 1.0 ? 0 : 1] atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [e endEncoding];
        // frame pacing: 60 Hz, or 30 Hz when the device is thermally constrained
        double interval = thermal_serious_ ? 1.0 / 30.0 : 1.0 / 60.0;
        if (pp_.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE && !thermal_serious_) interval = 0.0;
        if (interval > 0.0) [cmd_ presentDrawable:dr afterMinimumDuration:interval];
        else [cmd_ presentDrawable:dr];
        signal_frame_ = true;
        flush(false);
        // next frame slot: 3 frames in flight at most
        dispatch_semaphore_wait(frame_sem_, DISPATCH_TIME_FOREVER);
        frame_idx_ = (frame_idx_ + 1) % 3;
        FrameSlot &nf = frames_[frame_idx_];
        nf.used = 0;
        nf.vis_used = 0;
        memset(nf.vis.contents, 0, nf.vis.length);
        ring_epoch_++;
        stats().frames++;
        static uint64_t last_report_ns = 0;
        const uint64_t tnow = now_ns();
        {
            // hitch log: a frame over 50 ms says what happened during it (first-use shaders / pipelines, texture conversion, file reads)
            struct Snap { uint64_t t, shaders, pipes, archive, blocks, xcode_ns, upload, read; };
            static Snap prev{};
            MtlStats &s = stats();
            Snap cur{tnow, s.shaders_created.load(), s.pipelines_created.load(), s.pipeline_archive_hits.load(), s.transcode_blocks.load(),
                     s.transcode_ns.load() + s.decode_ns.load(), s.upload_bytes.load(), file_read_bytes_total()};
            if (prev.t && cur.t - prev.t > 50000000ull) {
                LOGI(LC_GFX, "hitch: %.0f ms frame; %llu new shaders (%lld compiling), %llu new pipelines (%llu from archive), %.1f MB textures "
                     "uploaded (%.0f ms converting), %.1f MB read from files",
                     (cur.t - prev.t) / 1e6, (unsigned long long)(cur.shaders - prev.shaders), (long long)s.shaders_pending.load(),
                     (unsigned long long)(cur.pipes - prev.pipes), (unsigned long long)(cur.archive - prev.archive), (cur.upload - prev.upload) / 1048576.0,
                     (cur.xcode_ns - prev.xcode_ns) / 1e6, (cur.read - prev.read) / 1048576.0);
            }
            prev = cur;
        }
        if (tnow - last_report_ns > 5000000000ull) {
            last_report_ns = tnow;
            MtlStats &s = stats();
            LOGI(LC_GFX, "metal: shaders %llu created, %lld compiling, %llu failed (translate %.0f ms, compile %.0f ms total); pipelines %llu (%llu from archive, %.0f ms); draws skipped for compiling shaders %llu",
                 (unsigned long long)s.shaders_created.load(), (long long)s.shaders_pending.load(), (unsigned long long)s.shader_failures.load(),
                 s.shader_translate_ns.load() / 1e6, s.shader_compile_ns.load() / 1e6, (unsigned long long)s.pipelines_created.load(),
                 (unsigned long long)s.pipeline_archive_hits.load(), s.pipeline_create_ns.load() / 1e6, (unsigned long long)s.draws_skipped_shader.load());
            if (s.draws_skipped_pso.load()) LOGI(LC_GFX, "metal: draws skipped for pipelines being built %llu", (unsigned long long)s.draws_skipped_pso.load());
            {
                static uint64_t last_busy = 0, last_frames = 0;
                const uint64_t busy = s.gpu_busy_ns.load(), fr = s.frames.load();
                if (fr > last_frames)
                    LOGI(LC_GFX, "metal: GPU busy %.1f ms per frame over the last %llu frames (%.0f%% of the interval)", (busy - last_busy) / 1e6 / (fr - last_frames),
                         (unsigned long long)(fr - last_frames), (busy - last_busy) / 50000000.0);
                last_busy = busy; last_frames = fr;
            }
            LOGI(LC_GFX, "metal: textures %.0f MiB on the GPU; DXT %s: %llu blocks in %.0f ms; copy tokens %llu out, %llu used",
                 s.gpu_bytes_textures.load() / 1048576.0, options().astc ? "-> ASTC" : "-> RGBA8 decode",
                 (unsigned long long)s.transcode_blocks.load(), (options().astc ? s.transcode_ns.load() : s.decode_ns.load()) / 1e6,
                 (unsigned long long)s.copy_tokens_out.load(), (unsigned long long)s.copy_tokens_used.load());
        }
    }
    return D3D_OK;
}

}  // namespace mtl
}  // namespace dish
