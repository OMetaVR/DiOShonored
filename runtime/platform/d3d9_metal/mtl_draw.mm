// Metal backend: draw path (pipeline-state cache keyed by hashed state, constants ring, texture/sampler binding, draw calls, clear quads).
#include "mtl_device.h"
#include "mtl_shadercache.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace dish {
namespace mtl {

static inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static inline uint64_t fnv(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) { h ^= (v >> (8 * i)) & 0xFF; h *= 1099511628211ull; }
    return h;
}

// ---- D3D -> Metal enum translation --------------------------------------------------------------------------------------------------------
static MTLCompareFunction cmp_func(DWORD f) {
    switch (f) {
        case D3DCMP_NEVER: return MTLCompareFunctionNever;
        case D3DCMP_LESS: return MTLCompareFunctionLess;
        case D3DCMP_EQUAL: return MTLCompareFunctionEqual;
        case D3DCMP_LESSEQUAL: return MTLCompareFunctionLessEqual;
        case D3DCMP_GREATER: return MTLCompareFunctionGreater;
        case D3DCMP_NOTEQUAL: return MTLCompareFunctionNotEqual;
        case D3DCMP_GREATEREQUAL: return MTLCompareFunctionGreaterEqual;
        default: return MTLCompareFunctionAlways;
    }
}
static MTLStencilOperation stencil_op(DWORD o) {
    switch (o) {
        case D3DSTENCILOP_ZERO: return MTLStencilOperationZero;
        case D3DSTENCILOP_REPLACE: return MTLStencilOperationReplace;
        case D3DSTENCILOP_INCRSAT: return MTLStencilOperationIncrementClamp;
        case D3DSTENCILOP_DECRSAT: return MTLStencilOperationDecrementClamp;
        case D3DSTENCILOP_INVERT: return MTLStencilOperationInvert;
        case D3DSTENCILOP_INCR: return MTLStencilOperationIncrementWrap;
        case D3DSTENCILOP_DECR: return MTLStencilOperationDecrementWrap;
        default: return MTLStencilOperationKeep;
    }
}
static MTLBlendFactor blend_factor(DWORD b) {
    switch (b) {
        case D3DBLEND_ZERO: return MTLBlendFactorZero;
        case D3DBLEND_ONE: return MTLBlendFactorOne;
        case D3DBLEND_SRCCOLOR: return MTLBlendFactorSourceColor;
        case D3DBLEND_INVSRCCOLOR: return MTLBlendFactorOneMinusSourceColor;
        case D3DBLEND_SRCALPHA: return MTLBlendFactorSourceAlpha;
        case D3DBLEND_INVSRCALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        case D3DBLEND_DESTALPHA: return MTLBlendFactorDestinationAlpha;
        case D3DBLEND_INVDESTALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
        case D3DBLEND_DESTCOLOR: return MTLBlendFactorDestinationColor;
        case D3DBLEND_INVDESTCOLOR: return MTLBlendFactorOneMinusDestinationColor;
        case D3DBLEND_SRCALPHASAT: return MTLBlendFactorSourceAlphaSaturated;
        case D3DBLEND_BLENDFACTOR: return MTLBlendFactorBlendColor;
        case D3DBLEND_INVBLENDFACTOR: return MTLBlendFactorOneMinusBlendColor;
        default: return MTLBlendFactorOne;
    }
}
static MTLBlendOperation blend_op(DWORD o) {
    switch (o) {
        case D3DBLENDOP_SUBTRACT: return MTLBlendOperationSubtract;
        case D3DBLENDOP_REVSUBTRACT: return MTLBlendOperationReverseSubtract;
        case D3DBLENDOP_MIN: return MTLBlendOperationMin;
        case D3DBLENDOP_MAX: return MTLBlendOperationMax;
        default: return MTLBlendOperationAdd;
    }
}
static MTLColorWriteMask write_mask(uint32_t m) {
    MTLColorWriteMask r = MTLColorWriteMaskNone;
    if (m & 1) r |= MTLColorWriteMaskRed;
    if (m & 2) r |= MTLColorWriteMaskGreen;
    if (m & 4) r |= MTLColorWriteMaskBlue;
    if (m & 8) r |= MTLColorWriteMaskAlpha;
    return r;
}
static MTLVertexFormat vertex_format(uint8_t t) {
    switch (t) {
        case D3DDECLTYPE_FLOAT1: return MTLVertexFormatFloat;
        case D3DDECLTYPE_FLOAT2: return MTLVertexFormatFloat2;
        case D3DDECLTYPE_FLOAT3: return MTLVertexFormatFloat3;
        case D3DDECLTYPE_FLOAT4: return MTLVertexFormatFloat4;
        case D3DDECLTYPE_D3DCOLOR: return MTLVertexFormatUChar4Normalized_BGRA;
        case D3DDECLTYPE_UBYTE4: return MTLVertexFormatUChar4;
        case D3DDECLTYPE_SHORT2: return MTLVertexFormatShort2;
        case D3DDECLTYPE_SHORT4: return MTLVertexFormatShort4;
        case D3DDECLTYPE_UBYTE4N: return MTLVertexFormatUChar4Normalized;
        case D3DDECLTYPE_SHORT2N: return MTLVertexFormatShort2Normalized;
        case D3DDECLTYPE_SHORT4N: return MTLVertexFormatShort4Normalized;
        case D3DDECLTYPE_USHORT2N: return MTLVertexFormatUShort2Normalized;
        case D3DDECLTYPE_USHORT4N: return MTLVertexFormatUShort4Normalized;
        case D3DDECLTYPE_UDEC3: return MTLVertexFormatUInt1010102Normalized;   // approximation: Metal only has the normalised form
        case D3DDECLTYPE_DEC3N: return MTLVertexFormatInt1010102Normalized;
        case D3DDECLTYPE_FLOAT16_2: return MTLVertexFormatHalf2;
        case D3DDECLTYPE_FLOAT16_4: return MTLVertexFormatHalf4;
        default: return MTLVertexFormatInvalid;
    }
}

// ---- clear quad ------------------------------------------------------------------------------------------------------------------------------------
id<MTLRenderPipelineState> MtlDevice::clear_pso(const PsoKey &k) {
    uint64_t key = 0xC1EA0000ull;
    for (int i = 0; i < 4; i++) key = fnv(key, k.color_fmt[i] | ((uint64_t)k.write_mask[i] << 16));
    key = fnv(key, k.depth_fmt | ((uint64_t)k.stencil_fmt << 16));
    auto it = misc_pso_.find(key);
    if (it != misc_pso_.end()) return it->second;
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = [builtin_ newFunctionWithName:@"dish_clear_vs"];
    d.fragmentFunction = [builtin_ newFunctionWithName:@"dish_clear_fs"];
    for (int i = 0; i < 4; i++) {
        d.colorAttachments[i].pixelFormat = (MTLPixelFormat)k.color_fmt[i];
        d.colorAttachments[i].writeMask = write_mask(k.write_mask[i]);
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k.depth_fmt;
    d.stencilAttachmentPixelFormat = (MTLPixelFormat)k.stencil_fmt;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [mdev_ newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) LOGE(LC_GFX, "metal: clear pipeline failed: %s", err ? [[err localizedDescription] UTF8String] : "?");
    misc_pso_[key] = p;
    return p;
}

void MtlDevice::draw_clear_quad(DWORD flags, D3DCOLOR color, float z, DWORD stencil, const RECT *rect) {
    id<MTLRenderCommandEncoder> e = render_enc();
    if (!e) return;
    MtlSurface *ref = rt_[0].get() ? rt_[0].get() : ds_.get();
    const uint32_t W = ref->width(), H = ref->height();
    PsoKey k;
    const bool wantC = (flags & D3DCLEAR_TARGET) != 0;
    for (int i = 0; i < 4; i++) {
        if (!rt_[i]) continue;
        k.color_fmt[i] = (uint16_t)rt_[i]->td->render_texture(enc_srgb_).pixelFormat;
        k.write_mask[i] = wantC ? 0xF : 0;
    }
    bool has_stencil = false;
    if (ds_) {
        k.depth_fmt = (uint16_t)ds_->td->tex.pixelFormat;
        if (ds_->td->fi->stencil) { k.stencil_fmt = k.depth_fmt; has_stencil = true; }
    }
    [e setRenderPipelineState:clear_pso(k)];
    // depth-stencil state: always pass, write what the flags ask for
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = (flags & D3DCLEAR_ZBUFFER) != 0;
    if (has_stencil && (flags & D3DCLEAR_STENCIL)) {
        MTLStencilDescriptor *sd = [[MTLStencilDescriptor alloc] init];
        sd.stencilCompareFunction = MTLCompareFunctionAlways;
        sd.stencilFailureOperation = sd.depthFailureOperation = sd.depthStencilPassOperation = MTLStencilOperationReplace;
        sd.readMask = 0xFF; sd.writeMask = 0xFF;
        dd.frontFaceStencil = sd; dd.backFaceStencil = sd;
    }
    if (ds_) [e setDepthStencilState:[mdev_ newDepthStencilStateWithDescriptor:dd]];
    [e setStencilReferenceValue:stencil & 0xFF];
    [e setCullMode:MTLCullModeNone];
    [e setTriangleFillMode:MTLTriangleFillModeFill];
    MTLViewport vp = {0, 0, (double)W, (double)H, 0, 1};
    [e setViewport:vp];
    NSInteger l = std::max<LONG>(rect->left, 0), t = std::max<LONG>(rect->top, 0), r = std::min<LONG>(rect->right, (LONG)W), b = std::min<LONG>(rect->bottom, (LONG)H);
    if (r <= l || b <= t) { ap_ = Applied(); return; }
    MTLScissorRect sc = {(NSUInteger)l, (NSUInteger)t, (NSUInteger)(r - l), (NSUInteger)(b - t)};
    [e setScissorRect:sc];
    float c[4];
    c[0] = ((color >> 16) & 255) / 255.0f; c[1] = ((color >> 8) & 255) / 255.0f; c[2] = (color & 255) / 255.0f; c[3] = ((color >> 24) & 255) / 255.0f;
    float u[8] = {c[0], c[1], c[2], c[3], std::min(1.0f, std::max(0.0f, z)), 0, 0, 0};
    [e setVertexBytes:u length:sizeof u atIndex:0];
    [e setFragmentBytes:u length:sizeof u atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    enc_draws_++;
    ap_ = Applied();   // the clear quad changed every piece of encoder state
}

// ---- pipeline creation -------------------------------------------------------------------------------------------------------------------------------
static MTLVertexDescriptor *make_vertex_desc(const std::vector<ShaderAttr> &inputs, const std::vector<D3DVERTEXELEMENT9> *elems, const PsoKey &k) {
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    bool need_default = false;
    uint32_t used_streams = 0;
    for (const ShaderAttr &in : inputs) {
        const D3DVERTEXELEMENT9 *el = nullptr;
        if (elems) for (const D3DVERTEXELEMENT9 &e : *elems) if (e.Usage == in.usage && e.UsageIndex == in.index) { el = &e; break; }
        MTLVertexFormat vf = el ? vertex_format(el->Type) : MTLVertexFormatInvalid;
        if (!el || vf == MTLVertexFormatInvalid || el->Stream >= 16) {
            if (el && vf == MTLVertexFormatInvalid) LOGW(LC_GFX, "metal: vertex element type %u is not supported", (unsigned)el->Type);
            vd.attributes[in.reg].format = MTLVertexFormatFloat4;
            vd.attributes[in.reg].offset = 0;
            vd.attributes[in.reg].bufferIndex = 30;
            need_default = true;
            continue;
        }
        vd.attributes[in.reg].format = vf;
        vd.attributes[in.reg].offset = el->Offset;
        vd.attributes[in.reg].bufferIndex = 10 + el->Stream;
        used_streams |= 1u << el->Stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(used_streams & (1u << s))) continue;
        MTLVertexBufferLayoutDescriptor *l = vd.layouts[10 + s];
        l.stride = k.strides[s] ? k.strides[s] : 4;
        if (k.stream_step[s]) { l.stepFunction = MTLVertexStepFunctionPerInstance; l.stepRate = k.stream_step[s]; }
        else l.stepFunction = MTLVertexStepFunctionPerVertex;
    }
    if (need_default) {
        vd.layouts[30].stride = 0;
        vd.layouts[30].stepFunction = MTLVertexStepFunctionConstant;
        vd.layouts[30].stepRate = 0;
    }
    return vd;
}

MTLRenderPipelineDescriptor *make_pso_desc(const PsoKey &k, id<MTLFunction> vsfn, const std::vector<ShaderAttr> &vs_inputs, id<MTLFunction> psfn,
                                           const std::vector<D3DVERTEXELEMENT9> *elems) {
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = vsfn;
    d.fragmentFunction = psfn;
    d.vertexDescriptor = make_vertex_desc(vs_inputs, elems, k);
    for (int i = 0; i < 4; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[i];
        ca.pixelFormat = (MTLPixelFormat)k.color_fmt[i];
        if (!k.color_fmt[i]) continue;
        ca.writeMask = write_mask(k.write_mask[i]);
        if (k.blend_enable & (1u << i)) {
            ca.blendingEnabled = YES;
            ca.sourceRGBBlendFactor = blend_factor(k.src_rgb);
            ca.destinationRGBBlendFactor = blend_factor(k.dst_rgb);
            ca.rgbBlendOperation = blend_op(k.op_rgb);
            ca.sourceAlphaBlendFactor = blend_factor(k.src_a);
            ca.destinationAlphaBlendFactor = blend_factor(k.dst_a);
            ca.alphaBlendOperation = blend_op(k.op_a);
        }
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k.depth_fmt;
    d.stencilAttachmentPixelFormat = (MTLPixelFormat)k.stencil_fmt;
    return d;
}

id<MTLRenderPipelineState> MtlDevice::get_pso(const PsoKey &k, ShaderCommon &vs, ShaderCommon *ps, MtlVertexDecl *decl, bool *ok) {
    auto it = pso_cache_.find(k);
    if (it != pso_cache_.end()) {
        const int st = it->second->state.load(std::memory_order_acquire);
        if (st == 0) stats().draws_skipped_pso++;
        *ok = st == 1;
        return st == 1 ? it->second->pso : nil;
    }
    uint64_t t0 = now_ns();
    MTLRenderPipelineDescriptor *d = make_pso_desc(k, vs.fn, vs.x.inputs, ps ? ps->fn : nil, decl ? &decl->elems : nullptr);
    record_pipeline(k, vs.content_hash, ps ? ps->content_hash : 0, decl ? &decl->elems : nullptr);
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = nil;
    id<MTLBinaryArchive> arch = nil;
    {
        std::lock_guard<std::mutex> lk(archive_mu_);   // save_archive may replace it from a completion thread
        arch = archive_;
    }
    if (@available(iOS 14.0, *)) {
        if (arch) {
            d.binaryArchives = @[arch];
            pso = [mdev_ newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&err];
            if (pso) stats().pipeline_archive_hits++;
            err = nil;
        }
    }
    auto entry = std::make_shared<PsoEntry>();
    pso_cache_[k] = entry;
    // Metal cannot pack pipelines without a fragment function into a binary archive ("expecting 'fragment' stage"), and one such
    // pipeline makes every later save fail: depth-only pipelines are not archived
    const bool archivable = arch != nil && d.fragmentFunction != nil;
    if (pso) {
        entry->pso = pso;
        entry->state.store(1, std::memory_order_release);
    } else if (options().sync_shaders) {
        pso = [mdev_ newRenderPipelineStateWithDescriptor:d error:&err];
        if (!pso) LOGE(LC_GFX, "metal: pipeline creation failed (vs %llu ps %llu): %s", (unsigned long long)k.vs, (unsigned long long)k.ps, err ? [[err localizedDescription] UTF8String] : "?");
        else if (archivable) archive_add(d);
        entry->pso = pso;
        entry->state.store(pso ? 1 : 2, std::memory_order_release);
    } else {
        const uint64_t vsid = k.vs, psid = k.ps;
        MtlDevice *self = this;
        [mdev_ newRenderPipelineStateWithDescriptor:d completionHandler:^(id<MTLRenderPipelineState> p, NSError *e) {
            if (!p) LOGE(LC_GFX, "metal: pipeline creation failed (vs %llu ps %llu): %s", (unsigned long long)vsid, (unsigned long long)psid, e ? [[e localizedDescription] UTF8String] : "?");
            else if (archivable) self->archive_add(d);
            stats().pipeline_create_ns += now_ns() - t0;
            entry->pso = p;
            entry->state.store(p ? 1 : 2, std::memory_order_release);
        }];
        stats().pipelines_created++;
        stats().draws_skipped_pso++;
        *ok = false;
        return nil;
    }
    stats().pipelines_created++;
    stats().pipeline_create_ns += now_ns() - t0;
    *ok = pso != nil;
    return pso;
}

void MtlDevice::archive_add(MTLRenderPipelineDescriptor *d) {
    if (@available(iOS 14.0, *)) {
        bool save = false;
        {
            std::lock_guard<std::mutex> lk(archive_mu_);
            NSError *aerr = nil;
            if (archive_ && [archive_ addRenderPipelineFunctionsWithDescriptor:d error:&aerr]) archive_dirty_++;
            save = archive_dirty_ >= 64;
        }
        if (save) save_archive();
    }
}

id<MTLDepthStencilState> MtlDevice::get_dss() {
    const uint32_t *rs = st_.rs;
    MtlSurface *dsurf = ds_.get();
    const bool has_depth = dsurf != nullptr;
    const bool has_stencil = dsurf && dsurf->td->fi->stencil;
    uint64_t key = 0xD55D0000ull;
    key = fnv(key, (has_depth ? 1 : 0) | (has_stencil ? 2 : 0) | (rs[D3DRS_ZENABLE] ? 4 : 0) | (rs[D3DRS_ZWRITEENABLE] ? 8 : 0) | ((uint64_t)rs[D3DRS_ZFUNC] << 8));
    if (has_stencil && rs[D3DRS_STENCILENABLE]) {
        key = fnv(key, rs[D3DRS_STENCILFAIL] | ((uint64_t)rs[D3DRS_STENCILZFAIL] << 8) | ((uint64_t)rs[D3DRS_STENCILPASS] << 16) | ((uint64_t)rs[D3DRS_STENCILFUNC] << 24) |
                           ((uint64_t)(rs[D3DRS_STENCILMASK] & 0xFF) << 32) | ((uint64_t)(rs[D3DRS_STENCILWRITEMASK] & 0xFF) << 40) | ((uint64_t)(rs[D3DRS_TWOSIDEDSTENCILMODE] ? 1 : 0) << 48));
        if (rs[D3DRS_TWOSIDEDSTENCILMODE])
            key = fnv(key, rs[D3DRS_CCW_STENCILFAIL] | ((uint64_t)rs[D3DRS_CCW_STENCILZFAIL] << 8) | ((uint64_t)rs[D3DRS_CCW_STENCILPASS] << 16) | ((uint64_t)rs[D3DRS_CCW_STENCILFUNC] << 24));
    }
    auto it = dss_cache_.find(key);
    if (it != dss_cache_.end()) return it->second;
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = rs[D3DRS_ZENABLE] ? cmp_func(rs[D3DRS_ZFUNC]) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = rs[D3DRS_ZENABLE] && rs[D3DRS_ZWRITEENABLE];
    if (has_stencil && rs[D3DRS_STENCILENABLE]) {
        MTLStencilDescriptor *f = [[MTLStencilDescriptor alloc] init];
        f.stencilFailureOperation = stencil_op(rs[D3DRS_STENCILFAIL]);
        f.depthFailureOperation = stencil_op(rs[D3DRS_STENCILZFAIL]);
        f.depthStencilPassOperation = stencil_op(rs[D3DRS_STENCILPASS]);
        f.stencilCompareFunction = cmp_func(rs[D3DRS_STENCILFUNC]);
        f.readMask = rs[D3DRS_STENCILMASK] & 0xFF;
        f.writeMask = rs[D3DRS_STENCILWRITEMASK] & 0xFF;
        dd.frontFaceStencil = f;
        if (rs[D3DRS_TWOSIDEDSTENCILMODE]) {
            MTLStencilDescriptor *b = [[MTLStencilDescriptor alloc] init];
            b.stencilFailureOperation = stencil_op(rs[D3DRS_CCW_STENCILFAIL]);
            b.depthFailureOperation = stencil_op(rs[D3DRS_CCW_STENCILZFAIL]);
            b.depthStencilPassOperation = stencil_op(rs[D3DRS_CCW_STENCILPASS]);
            b.stencilCompareFunction = cmp_func(rs[D3DRS_CCW_STENCILFUNC]);
            b.readMask = rs[D3DRS_STENCILMASK] & 0xFF;
            b.writeMask = rs[D3DRS_STENCILWRITEMASK] & 0xFF;
            dd.backFaceStencil = b;
        } else dd.backFaceStencil = f;
    }
    id<MTLDepthStencilState> s = [mdev_ newDepthStencilStateWithDescriptor:dd];
    dss_cache_[key] = s;
    return s;
}

id<MTLSamplerState> MtlDevice::get_sampler(int slot, bool *) {
    const uint32_t *s = st_.ss[slot];
    uint64_t key = 0x5A4D0000ull;
    key = fnv(key, s[D3DSAMP_ADDRESSU] | (s[D3DSAMP_ADDRESSV] << 4) | (s[D3DSAMP_ADDRESSW] << 8) | (s[D3DSAMP_MAGFILTER] << 12) | (s[D3DSAMP_MINFILTER] << 16) | (s[D3DSAMP_MIPFILTER] << 20) |
                       ((uint64_t)std::min<uint32_t>(s[D3DSAMP_MAXANISOTROPY], 16) << 24) | ((uint64_t)std::min<uint32_t>(s[D3DSAMP_MAXMIPLEVEL], 31) << 32));
    uint32_t bc = s[D3DSAMP_BORDERCOLOR];
    int bclass = (bc >> 24) == 0 ? 0 : (((bc >> 16) & 255) >= 128 && ((bc >> 8) & 255) >= 128 && (bc & 255) >= 128) ? 2 : 1;
    key = fnv(key, (uint64_t)bclass);
    auto it = smp_cache_.find(key);
    if (it != smp_cache_.end()) return it->second;
    MTLSamplerDescriptor *d = [[MTLSamplerDescriptor alloc] init];
    auto addr = [&](DWORD m) -> MTLSamplerAddressMode {
        switch (m) {
            case D3DTADDRESS_MIRROR: return MTLSamplerAddressModeMirrorRepeat;
            case D3DTADDRESS_CLAMP: return MTLSamplerAddressModeClampToEdge;
            case D3DTADDRESS_BORDER:
                if (@available(iOS 14.0, *)) return MTLSamplerAddressModeClampToBorderColor;
                return MTLSamplerAddressModeClampToEdge;
            case D3DTADDRESS_MIRRORONCE: return MTLSamplerAddressModeMirrorClampToEdge;
            default: return MTLSamplerAddressModeRepeat;
        }
    };
    d.sAddressMode = addr(s[D3DSAMP_ADDRESSU]);
    d.tAddressMode = addr(s[D3DSAMP_ADDRESSV]);
    d.rAddressMode = addr(s[D3DSAMP_ADDRESSW]);
    if (@available(iOS 14.0, *)) d.borderColor = bclass == 0 ? MTLSamplerBorderColorTransparentBlack : bclass == 2 ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack;
    const bool aniso = s[D3DSAMP_MINFILTER] == D3DTEXF_ANISOTROPIC || s[D3DSAMP_MAGFILTER] == D3DTEXF_ANISOTROPIC;
    d.magFilter = (s[D3DSAMP_MAGFILTER] >= D3DTEXF_LINEAR) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = (s[D3DSAMP_MINFILTER] >= D3DTEXF_LINEAR) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = s[D3DSAMP_MIPFILTER] == D3DTEXF_NONE ? MTLSamplerMipFilterNotMipmapped : s[D3DSAMP_MIPFILTER] == D3DTEXF_POINT ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    d.maxAnisotropy = aniso ? std::max<uint32_t>(1, std::min<uint32_t>(s[D3DSAMP_MAXANISOTROPY], 16)) : 1;
    d.lodMinClamp = (float)std::min<uint32_t>(s[D3DSAMP_MAXMIPLEVEL], 31);
    d.lodMaxClamp = 1000.0f;
    id<MTLSamplerState> st = [mdev_ newSamplerStateWithDescriptor:d];
    smp_cache_[key] = st;
    return st;
}

// ---- per-draw state application ---------------------------------------------------------------------------------------------------------------------
void MtlDevice::apply_constants(ShaderCommon &sh, bool vertex, ConstState &cs) {
    if (sh.x.ubuf_size == 0) return;
    ConstCache &cc = cc_[vertex ? 0 : 1];
    const uint64_t cver = vertex ? vs_cver_ : ps_cver_;
    if (!(cc.sid == sh.sid && cc.ver == cver && cc.epoch == ring_epoch_ && cc.buf)) {
        RingAlloc r = ring_alloc(sh.x.ubuf_size, 256);
        uint8_t *dst = r.ptr;
        const float *f = vertex ? cs.vsf : cs.psf;
        const int32_t *iv = vertex ? cs.vsi : cs.psi;
        const uint8_t *bv = vertex ? cs.vsb : cs.psb;
        const uint32_t maxf = vertex ? 256 : 224;
        for (const ShaderUniform &u : sh.x.uniforms) {
            switch (u.type) {
                case SU_FLOAT: { uint32_t n = std::min<uint32_t>(u.count, maxf > u.reg ? maxf - u.reg : 0); memcpy(dst + 16 * (size_t)u.packed, f + (size_t)u.reg * 4, 16 * (size_t)n); break; }
                case SU_INT: { uint32_t n = std::min<uint32_t>(u.count, 16 > u.reg ? 16 - u.reg : 0); memcpy(dst + sh.x.off_int + 16 * (size_t)u.packed, iv + (size_t)u.reg * 4, 16 * (size_t)n); break; }
                case SU_BOOL: { for (uint32_t k = 0; k < u.count && u.reg + k < 16; k++) dst[sh.x.off_bool + u.packed + k] = bv[u.reg + k]; break; }
            }
        }
        cc.sid = sh.sid; cc.ver = cver; cc.epoch = ring_epoch_; cc.buf = r.buf; cc.off = r.off;
    }
    void *&bound = vertex ? (void *&)ap_.vs_cbuf_raw : (void *&)ap_.ps_cbuf_raw;
    size_t &boundoff = vertex ? ap_.vs_coff : ap_.ps_coff;
    if (bound != (__bridge void *)cc.buf || boundoff != cc.off) {
        if (vertex) [enc_ setVertexBuffer:cc.buf offset:cc.off atIndex:0];
        else [enc_ setFragmentBuffer:cc.buf offset:cc.off atIndex:0];
        bound = (__bridge void *)cc.buf;
        boundoff = cc.off;
    }
}

void MtlDevice::apply_textures(ShaderCommon &sh, bool vertex) {
    for (const ShaderSampler &sm : sh.x.samplers) {
        const int slot = vertex ? 16 + sm.index : sm.index;
        if (slot >= kMaxSamplers) continue;
        TexData *td = st_.texdata[slot];
        bool type_ok = td && td->gpu && td->tex && ((sm.type == ST_2D && !td->cube && !td->volume) || (sm.type == ST_CUBE && td->cube) || (sm.type == ST_VOLUME && td->volume));
        id<MTLTexture> t;
        if (type_ok) {
            t = td->sampling_texture(st_.ss[slot][D3DSAMP_SRGBTEXTURE] != 0);
            td->last_use = serial_;
        } else t = dummy_tex_[sm.type > 2 ? 0 : sm.type];
        id<MTLSamplerState> smp = type_ok ? get_sampler(slot, nullptr) : dummy_smp_;
        void **tb = vertex ? &ap_.tex_vs[sm.index & 3] : &ap_.tex_ps[sm.index & 15];
        void **sb = vertex ? &ap_.smp_vs[sm.index & 3] : &ap_.smp_ps[sm.index & 15];
        if (*tb != (__bridge void *)t) {
            if (vertex) [enc_ setVertexTexture:t atIndex:sm.index]; else [enc_ setFragmentTexture:t atIndex:sm.index];
            *tb = (__bridge void *)t;
        }
        if (*sb != (__bridge void *)smp) {
            if (vertex) [enc_ setVertexSamplerState:smp atIndex:sm.index]; else [enc_ setFragmentSamplerState:smp atIndex:sm.index];
            *sb = (__bridge void *)smp;
        }
    }
}

bool MtlDevice::prim_info(D3DPRIMITIVETYPE t, UINT count, PrimInfo *o) {
    switch (t) {
        case D3DPT_POINTLIST: *o = {MTLPrimitiveTypePoint, count, false}; return true;
        case D3DPT_LINELIST: *o = {MTLPrimitiveTypeLine, count * 2, false}; return true;
        case D3DPT_LINESTRIP: *o = {MTLPrimitiveTypeLineStrip, count + 1, false}; return true;
        case D3DPT_TRIANGLELIST: *o = {MTLPrimitiveTypeTriangle, count * 3, false}; return true;
        case D3DPT_TRIANGLESTRIP: *o = {MTLPrimitiveTypeTriangleStrip, count + 2, false}; return true;
        case D3DPT_TRIANGLEFAN: *o = {MTLPrimitiveTypeTriangle, count * 3, true}; return true;
        default: return false;
    }
}

bool MtlDevice::prepare_draw(D3DPRIMITIVETYPE, bool) {
    if (!st_.vs) {
        if (draw_unsupported_++ < 5) LOGE(LC_GFX, "metal: draw without a vertex shader: the fixed-function pipeline is not implemented (draw skipped)");
        return false;
    }
    ShaderCommon &vs = st_.vs->sh;
    ShaderCommon *psp = st_.ps ? &st_.ps->sh : nullptr;   // no pixel shader: depth-only pipeline (no fragment function)
    if (options().sync_shaders) {
        if (!vs.wait() || (psp && !psp->wait())) return false;
    } else if (!vs.ready() || (psp && !psp->ready())) {
        // asynchronous shaders: a draw whose shaders are still compiling is skipped (it appears a few frames later) instead of
        // stalling the render thread behind the compile queue; the shaders it needs are compiled ahead of the others
        if (vs.failed() || (psp && psp->failed())) return false;
        vs.prioritize();
        if (psp) psp->prioritize();
        stats().draws_skipped_shader++;
        return false;
    }
    if (!rt_[0] && !ds_) return false;
    const bool srgb = st_.rs[D3DRS_SRGBWRITEENABLE] != 0;
    if (enc_ && enc_srgb_ != srgb) end_pass();
    id<MTLRenderCommandEncoder> e = render_enc();
    if (!e) return false;
    MtlVertexDecl *decl = st_.decl.get();
    if (!decl) {
        if (draw_unsupported_++ < 5) LOGE(LC_GFX, "metal: draw without a vertex declaration (FVF path not implemented)");
        return false;
    }

    // ---- pipeline
    PsoKey k;
    k.vs = vs.sid;
    k.ps = psp ? psp->sid : 0;
    k.decl = decl->hash;
    for (int s = 0; s < 16; s++) {
        if (!decl->stream_min_stride[s]) continue;
        const StreamBinding &b = st_.streams[s];
        uint32_t stride = (s == 0 && up_.vactive) ? up_.vstride : b.stride;
        k.strides[s] = stride ? stride : decl->stream_min_stride[s];
        if (b.freq & D3DSTREAMSOURCE_INSTANCEDATA) k.stream_step[s] = std::max<uint32_t>(1, b.freq & 0x3FFFFFFF);
    }
    for (int i = 0; i < 4; i++) {
        if (!rt_[i]) continue;
        k.color_fmt[i] = (uint16_t)rt_[i]->td->render_texture(srgb).pixelFormat;
        k.write_mask[i] = (uint8_t)(st_.rs[i == 0 ? D3DRS_COLORWRITEENABLE : D3DRS_COLORWRITEENABLE1 + (i - 1)] & 0xF);
    }
    if (ds_) {
        k.depth_fmt = (uint16_t)ds_->td->tex.pixelFormat;
        if (ds_->td->fi->stencil) k.stencil_fmt = k.depth_fmt;
    }
    if (st_.rs[D3DRS_ALPHABLENDENABLE]) {
        k.blend_enable = 0xF;
        DWORD sb = st_.rs[D3DRS_SRCBLEND], db = st_.rs[D3DRS_DESTBLEND];
        if (sb == D3DBLEND_BOTHSRCALPHA) { sb = D3DBLEND_SRCALPHA; db = D3DBLEND_INVSRCALPHA; }
        else if (sb == D3DBLEND_BOTHINVSRCALPHA) { sb = D3DBLEND_INVSRCALPHA; db = D3DBLEND_SRCALPHA; }
        k.src_rgb = (uint8_t)sb; k.dst_rgb = (uint8_t)db; k.op_rgb = (uint8_t)st_.rs[D3DRS_BLENDOP];
        if (st_.rs[D3DRS_SEPARATEALPHABLENDENABLE]) {
            k.src_a = (uint8_t)st_.rs[D3DRS_SRCBLENDALPHA]; k.dst_a = (uint8_t)st_.rs[D3DRS_DESTBLENDALPHA]; k.op_a = (uint8_t)st_.rs[D3DRS_BLENDOPALPHA];
        } else { k.src_a = k.src_rgb; k.dst_a = k.dst_rgb; k.op_a = k.op_rgb; }
    }
    bool ok = false;
    id<MTLRenderPipelineState> pso = get_pso(k, vs, psp, decl, &ok);
    if (!ok) return false;
    if (ap_.pso != (__bridge void *)pso) { [e setRenderPipelineState:pso]; ap_.pso = (__bridge void *)pso; }

    // ---- depth / stencil / rasteriser
    id<MTLDepthStencilState> dss = get_dss();
    if (ap_.dss != (__bridge void *)dss) { [e setDepthStencilState:dss]; ap_.dss = (__bridge void *)dss; }
    const uint32_t sref = st_.rs[D3DRS_STENCILREF] & 0xFF;
    if (ap_.stencil_ref != sref) { [e setStencilReferenceValue:sref]; ap_.stencil_ref = sref; }
    int cull = st_.rs[D3DRS_CULLMODE] == D3DCULL_NONE ? 0 : st_.rs[D3DRS_CULLMODE] == D3DCULL_CW ? 1 : 2;
    if (ap_.cull != cull) { [e setCullMode:cull == 0 ? MTLCullModeNone : cull == 1 ? MTLCullModeFront : MTLCullModeBack]; ap_.cull = cull; }
    int fill = st_.rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME ? 1 : 0;
    if (ap_.fill != fill) { [e setTriangleFillMode:fill ? MTLTriangleFillModeLines : MTLTriangleFillModeFill]; ap_.fill = fill; }
    float bias[3];
    memcpy(&bias[0], &st_.rs[D3DRS_DEPTHBIAS], 4);
    memcpy(&bias[1], &st_.rs[D3DRS_SLOPESCALEDEPTHBIAS], 4);
    bias[2] = 0;
    if (bias[0] != ap_.bias[0] || bias[1] != ap_.bias[1]) {
        [e setDepthBias:bias[0] * 16777216.0f slopeScale:bias[1] clamp:0.0f];
        ap_.bias[0] = bias[0]; ap_.bias[1] = bias[1];
    }
    MtlSurface *ref = rt_[0].get() ? rt_[0].get() : ds_.get();
    const uint32_t W = ref->width(), H = ref->height();
    {
        // D3D9 samples pixels at integer coordinates, Metal at +0.5: shifting the viewport origin by half a pixel makes pixel-aligned
        // geometry (UE3's full-screen passes, UI) land exactly as on Windows. The MinZ bias avoids a degenerate depth range (as DXVK).
        const float minz = std::clamp(st_.vp.MinZ, 0.0f, 1.0f);
        const float maxz = std::clamp(std::max(st_.vp.MaxZ, st_.vp.MinZ + (st_.vp.MinZ >= 0.5f ? 0.0f : 0.001f)), 0.0f, 1.0f);
        MTLViewport vp = {st_.vp.X + 0.5, st_.vp.Y + 0.5, (double)st_.vp.Width, (double)st_.vp.Height, (double)minz, (double)maxz};
        if (memcmp(&vp, &ap_.vp, sizeof vp) != 0) { [e setViewport:vp]; ap_.vp = vp; }
        MTLScissorRect sc;
        if (st_.rs[D3DRS_SCISSORTESTENABLE]) {
            LONG l = std::max<LONG>(st_.scissor.left, 0), t = std::max<LONG>(st_.scissor.top, 0), r = std::min<LONG>(st_.scissor.right, (LONG)W), b = std::min<LONG>(st_.scissor.bottom, (LONG)H);
            if (r <= l || b <= t) return false;
            sc = {(NSUInteger)l, (NSUInteger)t, (NSUInteger)(r - l), (NSUInteger)(b - t)};
        } else sc = {0, 0, W, H};
        if (memcmp(&sc, &ap_.sc, sizeof sc) != 0) { [e setScissorRect:sc]; ap_.sc = sc; }
    }
    {
        const uint32_t bf = st_.rs[D3DRS_BLENDFACTOR];
        float c[4] = {((bf >> 16) & 255) / 255.0f, ((bf >> 8) & 255) / 255.0f, (bf & 255) / 255.0f, ((bf >> 24) & 255) / 255.0f};
        if (memcmp(c, ap_.blend, sizeof c) != 0) { [e setBlendColorRed:c[0] green:c[1] blue:c[2] alpha:c[3]]; memcpy(ap_.blend, c, sizeof c); }
    }

    // ---- shader inputs
    apply_constants(vs, true, st_.c);
    if (psp) apply_constants(*psp, false, st_.c);
    apply_textures(vs, true);
    if (psp) apply_textures(*psp, false);
    if (psp && psp->x.ps_writes_color0) {
        uint32_t extra[4];
        const float aref = (float)(st_.rs[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
        memcpy(&extra[0], &aref, 4);
        extra[1] = st_.rs[D3DRS_ALPHAFUNC];
        extra[2] = st_.rs[D3DRS_ALPHATESTENABLE] ? 1 : 0;
        extra[3] = 0;
        if (memcmp(extra, ap_.ps_extra, 16) != 0) { [e setFragmentBytes:extra length:16 atIndex:1]; memcpy(ap_.ps_extra, extra, 16); }
    }
    // ---- vertex streams
    bool needs_default = false;
    for (const ShaderAttr &in : vs.x.inputs) {
        bool found = false;
        for (const D3DVERTEXELEMENT9 &el : decl->elems) if (el.Usage == in.usage && el.UsageIndex == in.index && vertex_format(el.Type) != MTLVertexFormatInvalid) found = true;
        if (!found) needs_default = true;
    }
    if (needs_default && !ap_.default_attr) { [e setVertexBuffer:default_attr_buf_ offset:0 atIndex:30]; ap_.default_attr = true; }
    for (int s = 0; s < 16; s++) {
        if (!decl->stream_min_stride[s]) continue;
        id<MTLBuffer> buf = nil;
        size_t off = 0;
        if (s == 0 && up_.vactive) { buf = up_.vbuf; off = up_.voff; }
        else {
            const StreamBinding &b = st_.streams[s];
            if (!b.vb) continue;
            buf = b.vb->core.buf;
            off = b.offset;
            b.vb->core.last_use = serial_;
        }
        if (ap_.vbuf[s] != (__bridge void *)buf || ap_.voff[s] != off) {
            [e setVertexBuffer:buf offset:off atIndex:10 + s];
            ap_.vbuf[s] = (__bridge void *)buf;
            ap_.voff[s] = (uint32_t)off;
        }
    }
    return true;
}

HRESULT MtlDevice::draw_impl(D3DPRIMITIVETYPE type, INT base_vertex, UINT start, UINT count, bool indexed) {
    PrimInfo pi;
    if (!prim_info(type, count, &pi)) return D3DERR_INVALIDCALL;
    if (count == 0) return D3D_OK;
    if (!prepare_draw(type, indexed)) return D3D_OK;
    stats().draws++;
    enc_draws_++;
    id<MTLRenderCommandEncoder> e = enc_;
    // instancing (stream frequency): INDEXEDDATA on stream 0 carries the instance count
    NSUInteger instances = 1;
    if (st_.streams[0].freq & D3DSTREAMSOURCE_INDEXEDDATA) instances = std::max<UINT>(1, st_.streams[0].freq & 0x3FFFFFFF);
    if (!indexed) {
        if (!pi.fan) {
            [e drawPrimitives:pi.prim vertexStart:start vertexCount:pi.verts instanceCount:instances];
        } else {
            // triangle fan -> indexed triangle list
            const uint32_t ntri = count;
            RingAlloc r = ring_alloc((size_t)ntri * 3 * 4, 16);
            uint32_t *ix = (uint32_t *)r.ptr;
            for (uint32_t t = 0; t < ntri; t++) { ix[3 * t] = 0; ix[3 * t + 1] = t + 1; ix[3 * t + 2] = t + 2; }
            [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:ntri * 3 indexType:MTLIndexTypeUInt32 indexBuffer:r.buf indexBufferOffset:r.off instanceCount:instances baseVertex:(NSInteger)start baseInstance:0];
        }
        return D3D_OK;
    }
    id<MTLBuffer> ibuf = nil;
    size_t ioff = 0;
    MTLIndexType itype = MTLIndexTypeUInt16;
    const uint8_t *host_idx = nullptr;
    if (up_.iactive) { ibuf = up_.ibuf; ioff = up_.ioff; itype = up_.itype; host_idx = (const uint8_t *)up_.ibuf.contents + up_.ioff; }
    else {
        MtlIndexBuffer *ib = st_.ib.get();
        if (!ib) return D3DERR_INVALIDCALL;
        ibuf = ib->core.buf;
        itype = ib->fmt == D3DFMT_INDEX32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
        ioff = (size_t)start * (itype == MTLIndexTypeUInt32 ? 4 : 2);
        host_idx = (const uint8_t *)ibuf.contents + ioff;
        ib->core.last_use = serial_;
        start = 0;
    }
    if (!pi.fan) {
        const size_t isz = itype == MTLIndexTypeUInt32 ? 4 : 2;
        [e drawIndexedPrimitives:pi.prim indexCount:pi.verts indexType:itype indexBuffer:ibuf indexBufferOffset:ioff + (up_.iactive ? (size_t)start * isz : 0) instanceCount:instances baseVertex:base_vertex baseInstance:0];
        return D3D_OK;
    }
    // indexed triangle fan: rewrite the indices on the CPU
    const uint32_t ntri = count;
    const size_t isz = itype == MTLIndexTypeUInt32 ? 4 : 2;
    const uint8_t *src = host_idx + (up_.iactive ? (size_t)start * isz : 0);
    RingAlloc r = ring_alloc((size_t)ntri * 3 * 4, 16);
    uint32_t *ix = (uint32_t *)r.ptr;
    auto rd = [&](uint32_t i) -> uint32_t { return isz == 4 ? ((const uint32_t *)src)[i] : ((const uint16_t *)src)[i]; };
    for (uint32_t t = 0; t < ntri; t++) { ix[3 * t] = rd(0); ix[3 * t + 1] = rd(t + 1); ix[3 * t + 2] = rd(t + 2); }
    [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:ntri * 3 indexType:MTLIndexTypeUInt32 indexBuffer:r.buf indexBufferOffset:r.off instanceCount:instances baseVertex:base_vertex baseInstance:0];
    return D3D_OK;
}

HRESULT MtlDevice::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) { return draw_impl(type, 0, start, count, false); }
HRESULT MtlDevice::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base, UINT, UINT, UINT start, UINT count) { return draw_impl(type, base, start, count, true); }

HRESULT MtlDevice::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void *data, UINT stride) {
    PrimInfo pi;
    if (!data || !prim_info(type, count, &pi)) return D3DERR_INVALIDCALL;
    const size_t bytes = (size_t)stride * pi.verts;
    RingAlloc r = ring_alloc(bytes, 16);
    memcpy(r.ptr, data, bytes);
    up_.vbuf = r.buf; up_.voff = r.off; up_.vstride = stride; up_.vactive = true;
    HRESULT hr = draw_impl(type, 0, 0, count, false);
    up_.vactive = false;
    up_.vbuf = nil;
    return hr;
}

HRESULT MtlDevice::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minv, UINT nverts, UINT count, const void *idx, D3DFORMAT ifmt, const void *data, UINT stride) {
    PrimInfo pi;
    if (!data || !idx || !prim_info(type, count, &pi)) return D3DERR_INVALIDCALL;
    const size_t isz = ifmt == D3DFMT_INDEX32 ? 4 : 2;
    RingAlloc vr = ring_alloc((size_t)stride * (minv + nverts), 16);
    memcpy(vr.ptr + (size_t)stride * minv, (const uint8_t *)data + (size_t)stride * minv, (size_t)stride * nverts);
    RingAlloc ir = ring_alloc(isz * pi.verts, 16);
    memcpy(ir.ptr, idx, isz * pi.verts);
    up_.vbuf = vr.buf; up_.voff = vr.off; up_.vstride = stride; up_.vactive = true;
    up_.ibuf = ir.buf; up_.ioff = ir.off; up_.itype = isz == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16; up_.iactive = true;
    HRESULT hr = draw_impl(type, 0, 0, count, true);
    up_.vactive = up_.iactive = false;
    up_.vbuf = up_.ibuf = nil;
    return hr;
}

}  // namespace mtl
}  // namespace dish
