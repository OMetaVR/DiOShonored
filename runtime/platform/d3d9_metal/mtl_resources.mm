// Metal backend: buffers, textures, surfaces, declarations, shader objects, state blocks.
#include "mtl_device.h"
#include "mtl_shadercache.h"
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace dish {
namespace mtl {

static inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- private data -------------------------------------------------------------------------------------------------------------------
HRESULT PrivateData::set(REFGUID g, const void *d, DWORD size, DWORD flags) {
    if (flags & D3DSPD_IUNKNOWN) {
        // store the pointer and keep it alive
        IUnknown *u = (IUnknown *)d;
        u->AddRef();
    }
    for (Item &it : items) {
        if (it.guid == g) { it.data.assign((const uint8_t *)d, (const uint8_t *)d + size); return D3D_OK; }
    }
    Item it;
    it.guid = g;
    it.data.assign((const uint8_t *)d, (const uint8_t *)d + size);
    items.push_back(std::move(it));
    return D3D_OK;
}
HRESULT PrivateData::get(REFGUID g, void *d, DWORD *size) {
    for (Item &it : items) {
        if (!(it.guid == g)) continue;
        if (!size) return D3DERR_INVALIDCALL;
        if (*size < it.data.size()) { *size = (DWORD)it.data.size(); return D3DERR_MOREDATA; }
        if (d) memcpy(d, it.data.data(), it.data.size());
        *size = (DWORD)it.data.size();
        return D3D_OK;
    }
    return D3DERR_NOTFOUND;
}
HRESULT PrivateData::free(REFGUID g) {
    for (size_t i = 0; i < items.size(); i++) if (items[i].guid == g) { items.erase(items.begin() + (long)i); return D3D_OK; }
    return D3DERR_NOTFOUND;
}

// All resources report the device they were created from
#define DEFINE_GETDEVICE(CLS, DEVMEMBER)                                   \
    HRESULT CLS::GetDevice(IDirect3DDevice9 **pp) {                       \
        if (!pp) return D3DERR_INVALIDCALL;                                \
        *pp = (DEVMEMBER);                                                 \
        (*pp)->AddRef();                                                   \
        return D3D_OK;                                                     \
    }
DEFINE_GETDEVICE(MtlVertexBuffer, core.dev)
DEFINE_GETDEVICE(MtlIndexBuffer, core.dev)
DEFINE_GETDEVICE(MtlSurface, td->dev)
DEFINE_GETDEVICE(MtlTexture2D, td.dev)
DEFINE_GETDEVICE(MtlCubeTexture, td.dev)
DEFINE_GETDEVICE(MtlVolumeTexture, td.dev)
DEFINE_GETDEVICE(MtlVertexDecl, dev)
DEFINE_GETDEVICE(MtlVertexShader, sh.dev)
DEFINE_GETDEVICE(MtlPixelShader, sh.dev)
DEFINE_GETDEVICE(MtlStateBlock, dev)
DEFINE_GETDEVICE(MtlQuery, dev)
HRESULT MtlVolume::GetDevice(IDirect3DDevice9 **pp) { return tex->GetDevice(pp); }

// ---- buffers ------------------------------------------------------------------------------------------------------------------------
bool BufferCore::create(MtlDevice *d, uint32_t sz, DWORD u, D3DPOOL p) {
    dev = d;
    size = sz;
    usage = u;
    pool = p;
    if (sz == 0) return false;
    buf = dev->acquire_buffer(sz);
    if (!buf) return false;
    memacct_gpu_add((int64_t)buf.length);
    stats().gpu_bytes_buffers += buf.length;
    dev->live_inc();
    return true;
}
void BufferCore::destroy() {
    if (buf) {
        memacct_gpu_add(-(int64_t)buf.length);
        stats().gpu_bytes_buffers -= buf.length;
        dev->retire_buffer(buf, last_use);
        buf = nil;
        dev->live_dec();
    }
}
HRESULT BufferCore::lock(uint32_t off, uint32_t len, void **pp, DWORD flags) {
    if (!pp) return D3DERR_INVALIDCALL;
    if (len == 0) len = size > off ? size - off : 0;
    if ((uint64_t)off + len > size) return D3DERR_INVALIDCALL;
    const bool in_flight = last_use > dev->completed_serial();
    if (in_flight && !(flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY))) {
        if (flags & D3DLOCK_DISCARD) {
            // rename: the GPU keeps reading the old storage, the CPU gets fresh memory
            id<MTLBuffer> fresh = dev->acquire_buffer(size);
            if (fresh) {
                dev->retire_buffer(buf, last_use);
                buf = fresh;
                last_use = 0;
            } else dev->wait_serial(last_use);
        } else {
            dev->wait_serial(last_use);
        }
    }
    locked = true;
    *pp = (uint8_t *)buf.contents + off;
    return D3D_OK;
}
HRESULT BufferCore::unlock() {
    if (!locked) return D3DERR_INVALIDCALL;
    locked = false;
    return D3D_OK;
}
HRESULT MtlVertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *d) {
    d->Format = D3DFMT_VERTEXDATA;
    d->Type = D3DRTYPE_VERTEXBUFFER;
    d->Usage = core.usage;
    d->Pool = core.pool;
    d->Size = core.size;
    d->FVF = fvf;
    return D3D_OK;
}
HRESULT MtlIndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *d) {
    d->Format = fmt;
    d->Type = D3DRTYPE_INDEXBUFFER;
    d->Usage = core.usage;
    d->Pool = core.pool;
    d->Size = core.size;
    return D3D_OK;
}

// ---- texture storage ----------------------------------------------------------------------------------------------------------------
static MTLPixelFormat srgb_variant(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatBGRA8Unorm: return MTLPixelFormatBGRA8Unorm_sRGB;
        case MTLPixelFormatRGBA8Unorm: return MTLPixelFormatRGBA8Unorm_sRGB;
        case MTLPixelFormatR8Unorm: return MTLPixelFormatR8Unorm_sRGB;
        case MTLPixelFormatRG8Unorm: return MTLPixelFormatRG8Unorm_sRGB;
        case MTLPixelFormatASTC_4x4_LDR: return MTLPixelFormatASTC_4x4_sRGB;
        default: return MTLPixelFormatInvalid;
    }
}

static MTLTextureSwizzleChannels swizzle_for(uint8_t k) {
    MTLTextureSwizzleChannels s = {MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha};
    switch (k) {
        case 1: s.alpha = MTLTextureSwizzleOne; break;
        case 2: s.green = MTLTextureSwizzleRed; s.blue = MTLTextureSwizzleRed; s.alpha = MTLTextureSwizzleOne; break;
        case 3: s.alpha = MTLTextureSwizzleGreen; s.green = MTLTextureSwizzleRed; s.blue = MTLTextureSwizzleRed; break;
    }
    return s;
}

bool TexData::create(MtlDevice *d, D3DFORMAT f, uint32_t W, uint32_t H, uint32_t D, uint32_t L, DWORD u, D3DPOOL p, bool isCube, bool isVol) {
    dev = d;
    fmt = f;
    fi = format_info(f);
    if (!fi) { dev = nullptr; return false; }
    w = W; h = H;
    this->d = D ? D : 1;
    usage = u;
    pool = p;
    cube = isCube;
    volume = isVol;
    faces = isCube ? 6 : 1;
    autogen = (u & D3DUSAGE_AUTOGENMIPMAP) != 0;
    uint32_t maxl = full_mip_count(W, H, isVol ? this->d : 1);
    levels = (L == 0 || autogen) ? maxl : std::min(L, maxl);
    gpu = (p == D3DPOOL_DEFAULT || p == D3DPOOL_MANAGED);
    locks.resize((size_t)faces * levels);
    sys.resize((size_t)faces * levels);
    dev->live_inc();
    if (!gpu) return true;
    if (W > 16384 || H > 16384) return false;
    MTLPixelFormat pf = fi->mtl;
    if (fi->block && options().astc && fi->conv != Conv::Bc4 && fi->conv != Conv::Bc5) { pf = MTLPixelFormatASTC_4x4_LDR; astc = true; }
    MTLTextureDescriptor *td = [[MTLTextureDescriptor alloc] init];
    td.textureType = cube ? MTLTextureTypeCube : volume ? MTLTextureType3D : MTLTextureType2D;
    td.pixelFormat = pf;
    td.width = W;
    td.height = cube ? W : H;
    td.depth = volume ? this->d : 1;
    td.mipmapLevelCount = levels;
    td.storageMode = MTLStorageModePrivate;
    MTLTextureUsage mu = MTLTextureUsageShaderRead;
    if (u & D3DUSAGE_RENDERTARGET) mu |= MTLTextureUsageRenderTarget;
    if (u & D3DUSAGE_DEPTHSTENCIL) mu |= MTLTextureUsageRenderTarget;
    if (srgb_variant(pf) != MTLPixelFormatInvalid) mu |= MTLTextureUsagePixelFormatView;
    td.usage = mu;
    tex = [dev->mdev() newTextureWithDescriptor:td];
    if (!tex) { LOGE(LC_GFX, "metal: newTexture failed (%ux%u fmt %u)", W, H, (unsigned)f); return false; }
    sample_view = tex;
    if (fi->swz) {
        sample_view = [tex newTextureViewWithPixelFormat:pf textureType:td.textureType levels:NSMakeRange(0, levels)
                                                  slices:NSMakeRange(0, faces) swizzle:swizzle_for(fi->swz)];
        if (!sample_view) sample_view = tex;
    }
    gpu_bytes = tex.allocatedSize;
    memacct_gpu_add((int64_t)gpu_bytes);
    stats().gpu_bytes_textures += gpu_bytes;
    return true;
}

void TexData::release() {
    if (!dev) return;
    if (gpu_bytes) {
        memacct_gpu_add(-(int64_t)gpu_bytes);
        stats().gpu_bytes_textures -= gpu_bytes;
    }
    size_t sysb = 0;
    for (auto &v : sys) sysb += v.capacity();
    for (auto &v : shadow) sysb += v.capacity();
    shadow.clear();
    if (sysb) memacct_gpu_add(-(int64_t)sysb);
    tex = nil;
    sample_view = nil;
    srgb_view = nil;
    rt_srgb_view = nil;
    sys.clear();
    dev->live_dec();
    dev = nullptr;
}

id<MTLTexture> TexData::sampling_texture(bool srgb) {
    if (!srgb) return sample_view;
    if (srgb_view) return srgb_view;
    MTLPixelFormat sf = srgb_variant(astc ? MTLPixelFormatASTC_4x4_LDR : fi->mtl);
    if (sf == MTLPixelFormatInvalid) return sample_view;
    MTLTextureType tt = cube ? MTLTextureTypeCube : volume ? MTLTextureType3D : MTLTextureType2D;
    if (fi->swz) srgb_view = [tex newTextureViewWithPixelFormat:sf textureType:tt levels:NSMakeRange(0, levels) slices:NSMakeRange(0, faces) swizzle:swizzle_for(fi->swz)];
    else srgb_view = [tex newTextureViewWithPixelFormat:sf textureType:tt levels:NSMakeRange(0, levels) slices:NSMakeRange(0, faces)];
    if (!srgb_view) srgb_view = sample_view;
    return srgb_view;
}

id<MTLTexture> TexData::render_texture(bool srgb) {
    if (!srgb) return tex;
    if (rt_srgb_view) return rt_srgb_view;
    MTLPixelFormat sf = srgb_variant(fi->mtl);
    if (sf == MTLPixelFormatInvalid) return tex;
    rt_srgb_view = [tex newTextureViewWithPixelFormat:sf textureType:(cube ? MTLTextureTypeCube : MTLTextureType2D) levels:NSMakeRange(0, levels) slices:NSMakeRange(0, faces)];
    return rt_srgb_view ? rt_srgb_view : tex;
}

// ---- copy tokens (see CopyToken in mtl_objects.h) -------------------------------------------------------------------------------
namespace {
constexpr uint16_t kTokenMagic = 0xD15A;
std::mutex g_token_mu;
CopyToken g_tokens[256];
uint32_t g_token_next = 0;
uint64_t g_token_serial = 0;

void write_tokens(uint8_t *dst, uint32_t pitch, uint32_t bw, uint32_t bh, uint32_t block_bytes, uint8_t slot) {
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t *b = dst + (size_t)by * pitch + (size_t)bx * block_bytes;
            const uint32_t i = by * bw + bx;
            b[0] = kTokenMagic & 0xFF; b[1] = kTokenMagic >> 8; b[2] = slot; b[3] = (uint8_t)(0xC3 ^ slot);
            memcpy(b + 4, &i, 4);
            if (block_bytes == 16) { const uint32_t n = ~i, m = 0x5EA1C0DEu; memcpy(b + 8, &n, 4); memcpy(b + 12, &m, 4); }
        }
}
// Returns the token slot when the region holds exactly the tokens of one slot, -1 otherwise.
int match_tokens(const uint8_t *src, uint32_t pitch, uint32_t bw, uint32_t bh, uint32_t block_bytes) {
    if (src[0] != (kTokenMagic & 0xFF) || src[1] != (kTokenMagic >> 8) || src[3] != (uint8_t)(0xC3 ^ src[2])) return -1;
    const uint8_t slot = src[2];
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = src + (size_t)by * pitch + (size_t)bx * block_bytes;
            const uint32_t i = by * bw + bx;
            uint32_t v;
            memcpy(&v, b + 4, 4);
            if (b[0] != (kTokenMagic & 0xFF) || b[1] != (kTokenMagic >> 8) || b[2] != slot || b[3] != (uint8_t)(0xC3 ^ slot) || v != i) return -1;
            if (block_bytes == 16) {
                uint32_t n, m;
                memcpy(&n, b + 8, 4); memcpy(&m, b + 12, 4);
                if (n != ~i || m != 0x5EA1C0DEu) return -1;
            }
        }
    return slot;
}
}  // namespace

HRESULT TexData::lock(uint32_t face, uint32_t level, const RECT *rect, const D3DBOX *box, DWORD flags, void **bits, int *pitch_out, int *slice_out) {
    if (level >= levels || face >= faces) return D3DERR_INVALIDCALL;
    if (fi->depth) return D3DERR_INVALIDCALL;
    const uint32_t lw = mip_dim(w, level), lh = mip_dim(h, level), ld = volume ? mip_dim(d, level) : 1;
    uint32_t x = 0, y = 0, z = 0, rw = lw, rh = lh, rd = ld;
    if (rect) {
        if (rect->left < 0 || rect->top < 0 || (uint32_t)rect->right > lw || (uint32_t)rect->bottom > lh || rect->left >= rect->right || rect->top >= rect->bottom) return D3DERR_INVALIDCALL;
        x = (uint32_t)rect->left; y = (uint32_t)rect->top; rw = (uint32_t)(rect->right - rect->left); rh = (uint32_t)(rect->bottom - rect->top);
    }
    if (box) {
        if (box->Right > lw || box->Bottom > lh || box->Back > ld || box->Left >= box->Right || box->Top >= box->Bottom || box->Front >= box->Back) return D3DERR_INVALIDCALL;
        x = box->Left; y = box->Top; z = box->Front; rw = box->Right - box->Left; rh = box->Bottom - box->Top; rd = box->Back - box->Front;
    }
    if (fi->block) { x &= ~3u; y &= ~3u; }
    const uint32_t pitch = level_pitch(fi, lw);
    const uint32_t rows_total = level_rows(fi, lh);
    const uint32_t slice = pitch * rows_total;
    const uint32_t xoff = fi->block ? (x / 4) * fi->bpp : x * fi->bpp;
    const uint32_t yrow = fi->block ? y / 4 : y;
    const size_t idx = index(face, level);
    TexLock &L = locks[idx];
    if (L.active) return D3DERR_INVALIDCALL;
    if (!gpu) {
        std::vector<uint8_t> &st = sys[idx];
        if (st.empty()) {
            st.assign((size_t)slice * (volume ? ld : 1), 0);
            memacct_gpu_add((int64_t)st.capacity());
        }
        L.active = true; L.flags = flags; L.x = x; L.y = y; L.z = z; L.w = rw; L.h = rh; L.d = rd; L.pitch = pitch; L.slice = slice;
        *bits = st.data() + (size_t)z * slice + (size_t)yrow * pitch + xoff;
        *pitch_out = (int)pitch;
        if (slice_out) *slice_out = (int)slice;
        return D3D_OK;
    }
    // GPU texture: stage in the D3D layout
    const uint32_t rrows = fi->block ? (rh + 3) / 4 : rh;
    L.data.assign((size_t)pitch * rrows * rd + xoff + 16, 0);
    if (needs_shadow()) {
        // the region's current contents come from the D3D-layout copy (never written yet: zeros, like a fresh D3D texture)
        if (shadow.empty()) shadow.resize((size_t)faces * levels);
        const std::vector<uint8_t> &sh = shadow[idx];
        if (!sh.empty())
            for (uint32_t zz = 0; zz < rd; zz++)
                for (uint32_t r = 0; r < rrows; r++) {
                    size_t so = (size_t)(z + zz) * slice + (size_t)(yrow + r) * pitch, dof = (size_t)zz * pitch * rrows + (size_t)r * pitch;
                    if (so + pitch <= sh.size()) memcpy(L.data.data() + dof, sh.data() + so, pitch);
                }
    } else if ((flags & D3DLOCK_READONLY) && copy_tokens() && !volume) {
        // no CPU copy of DXT texels: hand out copy tokens (UE3 reads DXT levels back only to copy them into a reallocated texture)
        uint8_t slot;
        {
            std::lock_guard<std::mutex> lk(g_token_mu);
            slot = (uint8_t)(g_token_next++ & 255);
            g_tokens[slot] = CopyToken{tex, level, cube ? face : 0, rw, rh, ++g_token_serial};
        }
        write_tokens(L.data.data() + xoff, pitch, (rw + 3) / 4, rrows, fi->bpp, slot);
        stats().copy_tokens_out++;
    } else if (flags & D3DLOCK_READONLY) {
        if (!readback(face, level, x, y, rw, rh, L.data.data() + xoff, pitch)) {
            LOGW(LC_GFX, "metal: READONLY lock of a %s texture cannot be satisfied (format %u)", fi->block ? "compressed" : "converted", (unsigned)fmt);
            L.data.clear();
            return D3DERR_INVALIDCALL;
        }
    }
    L.active = true; L.flags = flags; L.x = x; L.y = y; L.z = z; L.w = rw; L.h = rh; L.d = rd; L.pitch = pitch; L.slice = pitch * rrows;
    *bits = L.data.data() + xoff;
    *pitch_out = (int)pitch;
    if (slice_out) *slice_out = (int)L.slice;
    return D3D_OK;
}

HRESULT TexData::unlock(uint32_t face, uint32_t level) {
    if (level >= levels || face >= faces) return D3DERR_INVALIDCALL;
    TexLock &L = locks[index(face, level)];
    if (!L.active) return D3DERR_INVALIDCALL;
    L.active = false;
    if (gpu && !(L.flags & D3DLOCK_READONLY)) {
        const uint32_t xoff = fi->block ? (L.x / 4) * fi->bpp : L.x * fi->bpp;
        if (copy_tokens() && !volume && L.x == 0 && L.y == 0 && L.w == mip_dim(w, level) && L.h == mip_dim(h, level)) {
            const int slot = match_tokens(L.data.data(), L.pitch, (L.w + 3) / 4, (L.h + 3) / 4, fi->bpp);
            if (slot >= 0) {
                CopyToken t;
                {
                    std::lock_guard<std::mutex> lk(g_token_mu);
                    t = g_tokens[slot];
                }
                if (t.tex && t.w == L.w && t.h == L.h && t.tex.pixelFormat == tex.pixelFormat) {
                    dev->gpu_copy_texture(t.tex, t.level, t.slice, MTLOriginMake(0, 0, 0), MTLSizeMake(L.w, L.h, 1), tex, level, cube ? face : 0,
                                          MTLOriginMake(0, 0, 0));
                    stats().copy_tokens_used++;
                    L.data.clear();
                    L.data.shrink_to_fit();
                    return D3D_OK;
                }
                LOGW(LC_GFX, "metal: copy token for a %ux%u level does not match its source (%ux%u)", L.w, L.h, t.w, t.h);
            }
        }
        upload(face, level, L.x, L.y, L.z, L.w, L.h, L.d, L.data.data() + xoff, L.pitch, L.slice);
        if (autogen && level == 0) generate_mips();
    }
    L.data.clear();
    L.data.shrink_to_fit();
    return D3D_OK;
}

void TexData::upload(uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t rw, uint32_t rh, uint32_t rd, const uint8_t *src, uint32_t pitch, uint32_t slice) {
    if (!gpu || !tex) return;
    const uint32_t lw = mip_dim(w, level), lh = mip_dim(h, level);
    // region size on the GPU texture (clip to the level)
    uint32_t cw = std::min(rw, lw - x), ch = std::min(rh, lh - y);
    MTLOrigin org = MTLOriginMake(x, y, volume ? z : 0);
    MTLSize sz = MTLSizeMake(cw, ch, volume ? rd : 1);
    const uint32_t slice_idx = cube ? face : 0;
    stats().uploads++;
    if (needs_shadow()) {   // keep the D3D-layout copy current (every upload path: unlock, UpdateSurface, UpdateTexture)
        const uint32_t ld = volume ? mip_dim(d, level) : 1;
        const uint32_t lpitch = level_pitch(fi, lw), lslice = lpitch * level_rows(fi, lh);
        if (shadow.empty()) shadow.resize((size_t)faces * levels);
        std::vector<uint8_t> &sh = shadow[index(face, level)];
        if (sh.empty()) {
            sh.assign((size_t)lslice * ld, 0);
            memacct_gpu_add((int64_t)sh.capacity());
        }
        const uint32_t xoff = fi->block ? (x / 4) * fi->bpp : x * fi->bpp;
        const uint32_t yrow = fi->block ? y / 4 : y, rows = fi->block ? (ch + 3) / 4 : ch;
        const uint32_t rowbytes = fi->block ? ((cw + 3) / 4) * fi->bpp : cw * fi->bpp;
        for (uint32_t zz = 0; zz < (volume ? rd : 1); zz++)
            for (uint32_t r = 0; r < rows; r++) {
                size_t so = (size_t)(z + zz) * lslice + (size_t)(yrow + r) * lpitch + xoff;
                if (so + rowbytes <= sh.size()) memcpy(sh.data() + so, src + (size_t)zz * slice + (size_t)r * pitch, rowbytes);
            }
    }
    if (fi->block) {
        const tex::Bc bc = fi->conv == Conv::Bc1 ? tex::Bc::DXT1 : fi->conv == Conv::Bc2 ? tex::Bc::DXT3 : fi->conv == Conv::Bc3 ? tex::Bc::DXT5 : fi->conv == Conv::Bc4 ? tex::Bc::ATI1 : tex::Bc::ATI2;
        const uint32_t bw = tex::bc_blocks(cw), bh = tex::bc_blocks(ch);
        const uint32_t bb = tex::bc_block_bytes(bc);
        // gather the blocks of the region into a tight buffer (the D3D pitch can be larger than the region)
        std::vector<uint8_t> blocks((size_t)bw * bh * bb);
        for (uint32_t r = 0; r < bh; r++) memcpy(&blocks[(size_t)r * bw * bb], src + (size_t)r * pitch, (size_t)bw * bb);
        stats().upload_bytes += blocks.size();
        if (astc) {
            std::vector<uint8_t> out((size_t)bw * bh * 16);
            uint64_t t0 = now_ns();
            tex::AstcStats as;
            tex::bc_to_astc_level(bc, blocks.data(), cw, ch, out.data(), &as);
            stats().transcode_ns += now_ns() - t0;
            stats().transcode_blocks += as.blocks;
            stats().transcode_in_bytes += blocks.size();
            dev->gpu_upload(tex, level, slice_idx, org, sz, out.data(), bw * 16, 0);
        } else {
            const uint32_t bpp = tex::bc_decoded_bpp(bc);
            std::vector<uint8_t> out((size_t)cw * ch * bpp);
            uint64_t t0 = now_ns();
            tex::bc_decode_level(bc, blocks.data(), cw, ch, out.data(), (size_t)cw * bpp);
            stats().decode_ns += now_ns() - t0;
            stats().decode_in_bytes += blocks.size();
            dev->gpu_upload(tex, level, slice_idx, org, sz, out.data(), cw * bpp, 0);
        }
        return;
    }
    if (fi->conv != Conv::None) {
        std::vector<uint8_t> out((size_t)cw * ch * 4);
        for (uint32_t r = 0; r < ch; r++) {
            const uint8_t *s = src + (size_t)r * pitch;
            uint8_t *o = &out[(size_t)r * cw * 4];
            switch (fi->conv) {
                case Conv::A4R4G4B4: tex::expand_a4r4g4b4_to_bgra8((const uint16_t *)s, o, cw); break;
                case Conv::X4R4G4B4: tex::expand_x4r4g4b4_to_bgra8((const uint16_t *)s, o, cw); break;
                case Conv::A8R3G3B2: tex::expand_a8r3g3b2_to_bgra8((const uint16_t *)s, o, cw); break;
                case Conv::R3G3B2: tex::expand_r3g3b2_to_bgra8(s, o, cw); break;
                default: break;
            }
        }
        stats().upload_bytes += out.size();
        dev->gpu_upload(tex, level, slice_idx, org, sz, out.data(), cw * 4, 0);
        return;
    }
    stats().upload_bytes += (size_t)pitch * ch * (volume ? rd : 1);
    dev->gpu_upload(tex, level, slice_idx, org, sz, src, pitch, slice);
}

bool TexData::readback(uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t rw, uint32_t rh, uint8_t *dst, uint32_t pitch) {
    if (!gpu || !tex || fi->block || fi->conv != Conv::None) return false;
    stats().readbacks++;
    return dev->gpu_readback(tex, level, cube ? face : 0, MTLOriginMake(x, y, 0), MTLSizeMake(rw, rh, 1), dst, pitch);
}

void TexData::generate_mips() {
    if (gpu && tex && levels > 1) dev->gpu_generate_mips(tex);
}

// ---- surfaces -----------------------------------------------------------------------------------------------------------------------
HRESULT MtlSurface::GetContainer(REFIID riid, void **pp) {
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = nullptr;
    if (!container) return E_NOINTERFACE;
    return container->QueryInterface(riid, pp);
}
HRESULT MtlSurface::GetDesc(D3DSURFACE_DESC *d) {
    d->Format = td->fmt;
    d->Type = D3DRTYPE_SURFACE;
    d->Usage = td->usage;
    d->Pool = td->pool;
    d->MultiSampleType = ms;
    d->MultiSampleQuality = 0;
    d->Width = width();
    d->Height = height();
    return D3D_OK;
}
HRESULT MtlSurface::LockRect(D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) {
    if (!lr) return D3DERR_INVALIDCALL;
    void *bits = nullptr;
    int pitch = 0;
    HRESULT hr = td->lock(face, level, rect, nullptr, flags, &bits, &pitch, nullptr);
    if (FAILED(hr)) { lr->pBits = nullptr; lr->Pitch = 0; return hr; }
    lr->pBits = bits;
    lr->Pitch = pitch;
    return D3D_OK;
}
HRESULT MtlSurface::UnlockRect() { return td->unlock(face, level); }

static MtlSurface *make_level_surface(TexData *td, IUnknown *container, uint32_t face, uint32_t level) {
    MtlSurface *s = new MtlSurface();
    s->td = td;
    s->container = container;
    s->face = face;
    s->level = level;
    return s;
}

// ---- 2D texture ---------------------------------------------------------------------------------------------------------------------
HRESULT MtlTexture2D::GetLevelDesc(UINT level, D3DSURFACE_DESC *d) {
    if (level >= td.levels) return D3DERR_INVALIDCALL;
    d->Format = td.fmt; d->Type = D3DRTYPE_SURFACE; d->Usage = td.usage; d->Pool = td.pool;
    d->MultiSampleType = D3DMULTISAMPLE_NONE; d->MultiSampleQuality = 0;
    d->Width = mip_dim(td.w, level); d->Height = mip_dim(td.h, level);
    return D3D_OK;
}
HRESULT MtlTexture2D::GetSurfaceLevel(UINT level, IDirect3DSurface9 **pp) {
    if (!pp || level >= td.levels) return D3DERR_INVALIDCALL;
    if (surfaces.empty()) surfaces.assign(td.levels, nullptr);
    if (!surfaces[level]) surfaces[level] = make_level_surface(&td, static_cast<IDirect3DTexture9 *>(this), 0, level);
    *pp = surfaces[level];
    (*pp)->AddRef();
    return D3D_OK;
}
HRESULT MtlTexture2D::LockRect(UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) {
    if (!lr) return D3DERR_INVALIDCALL;
    void *bits = nullptr; int pitch = 0;
    HRESULT hr = td.lock(0, level, rect, nullptr, flags, &bits, &pitch, nullptr);
    if (FAILED(hr)) { lr->pBits = nullptr; lr->Pitch = 0; return hr; }
    lr->pBits = bits; lr->Pitch = pitch;
    return D3D_OK;
}
HRESULT MtlTexture2D::UnlockRect(UINT level) { return td.unlock(0, level); }
void MtlTexture2D::destroy() {
    for (MtlSurface *s : surfaces) if (s) delete s;
    surfaces.clear();
    td.release();
    delete this;
}

// ---- cube texture -------------------------------------------------------------------------------------------------------------------
HRESULT MtlCubeTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC *d) {
    if (level >= td.levels) return D3DERR_INVALIDCALL;
    d->Format = td.fmt; d->Type = D3DRTYPE_SURFACE; d->Usage = td.usage; d->Pool = td.pool;
    d->MultiSampleType = D3DMULTISAMPLE_NONE; d->MultiSampleQuality = 0;
    d->Width = d->Height = mip_dim(td.w, level);
    return D3D_OK;
}
HRESULT MtlCubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **pp) {
    if (!pp || level >= td.levels || (UINT)face >= 6) return D3DERR_INVALIDCALL;
    if (surfaces.empty()) surfaces.assign((size_t)6 * td.levels, nullptr);
    MtlSurface *&s = surfaces[td.index((uint32_t)face, level)];
    if (!s) s = make_level_surface(&td, static_cast<IDirect3DCubeTexture9 *>(this), (uint32_t)face, level);
    *pp = s;
    s->AddRef();
    return D3D_OK;
}
HRESULT MtlCubeTexture::LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) {
    if (!lr || (UINT)face >= 6) return D3DERR_INVALIDCALL;
    void *bits = nullptr; int pitch = 0;
    HRESULT hr = td.lock((uint32_t)face, level, rect, nullptr, flags, &bits, &pitch, nullptr);
    if (FAILED(hr)) { lr->pBits = nullptr; lr->Pitch = 0; return hr; }
    lr->pBits = bits; lr->Pitch = pitch;
    return D3D_OK;
}
HRESULT MtlCubeTexture::UnlockRect(D3DCUBEMAP_FACES face, UINT level) { return td.unlock((uint32_t)face, level); }
void MtlCubeTexture::destroy() {
    for (MtlSurface *s : surfaces) if (s) delete s;
    surfaces.clear();
    td.release();
    delete this;
}

// ---- volume texture -----------------------------------------------------------------------------------------------------------------
HRESULT MtlVolumeTexture::GetLevelDesc(UINT level, D3DVOLUME_DESC *d) {
    if (level >= td.levels) return D3DERR_INVALIDCALL;
    d->Format = td.fmt; d->Type = D3DRTYPE_VOLUME; d->Usage = td.usage; d->Pool = td.pool;
    d->Width = mip_dim(td.w, level); d->Height = mip_dim(td.h, level); d->Depth = mip_dim(td.d, level);
    return D3D_OK;
}
HRESULT MtlVolumeTexture::GetVolumeLevel(UINT level, IDirect3DVolume9 **pp) {
    if (!pp || level >= td.levels) return D3DERR_INVALIDCALL;
    if (volumes.empty()) volumes.assign(td.levels, nullptr);
    if (!volumes[level]) { MtlVolume *v = new MtlVolume(); v->tex = this; v->level = level; volumes[level] = v; }
    *pp = volumes[level];
    (*pp)->AddRef();
    return D3D_OK;
}
HRESULT MtlVolumeTexture::LockBox(UINT level, D3DLOCKED_BOX *lb, const D3DBOX *box, DWORD flags) {
    if (!lb) return D3DERR_INVALIDCALL;
    void *bits = nullptr; int pitch = 0, slice = 0;
    HRESULT hr = td.lock(0, level, nullptr, box, flags, &bits, &pitch, &slice);
    if (FAILED(hr)) { lb->pBits = nullptr; lb->RowPitch = lb->SlicePitch = 0; return hr; }
    lb->pBits = bits; lb->RowPitch = pitch; lb->SlicePitch = slice;
    return D3D_OK;
}
HRESULT MtlVolumeTexture::UnlockBox(UINT level) { return td.unlock(0, level); }
void MtlVolumeTexture::destroy() {
    for (MtlVolume *v : volumes) if (v) delete v;
    volumes.clear();
    td.release();
    delete this;
}
HRESULT MtlVolume::GetContainer(REFIID riid, void **pp) { return tex->QueryInterface(riid, pp); }
HRESULT MtlVolume::GetDesc(D3DVOLUME_DESC *d) { return tex->GetLevelDesc(level, d); }

// ---- vertex declarations ------------------------------------------------------------------------------------------------------------
HRESULT MtlVertexDecl::GetDeclaration(D3DVERTEXELEMENT9 *out, UINT *n) {
    if (!n) return D3DERR_INVALIDCALL;
    if (!out) { *n = (UINT)elems.size() + 1; return D3D_OK; }
    if (*n < elems.size() + 1) { *n = (UINT)elems.size() + 1; return D3DERR_INVALIDCALL; }
    for (size_t i = 0; i < elems.size(); i++) out[i] = elems[i];
    out[elems.size()] = D3DDECL_END();
    *n = (UINT)elems.size() + 1;
    return D3D_OK;
}

// ---- shaders ------------------------------------------------------------------------------------------------------------------------
HRESULT MtlVertexShader::GetFunction(void *data, UINT *size) {
    if (!size) return D3DERR_INVALIDCALL;
    UINT need = (UINT)(sh.bytecode.size() * 4);
    if (data) { if (*size < need) return D3DERR_INVALIDCALL; memcpy(data, sh.bytecode.data(), need); }
    *size = need;
    return D3D_OK;
}
HRESULT MtlPixelShader::GetFunction(void *data, UINT *size) {
    if (!size) return D3DERR_INVALIDCALL;
    UINT need = (UINT)(sh.bytecode.size() * 4);
    if (data) { if (*size < need) return D3DERR_INVALIDCALL; memcpy(data, sh.bytecode.data(), need); }
    *size = need;
    return D3D_OK;
}

void ShaderCommon::start(MtlDevice *d, const uint32_t *code, bool pixel) {
    dev = d;
    is_pixel = pixel;
    sid = d->next_shader_id();
    size_t n = shader_token_count(code, 1 << 16);
    if (n == 0) { error = "no END token"; state = 2; return; }
    bytecode.assign(code, code + n);
    content_hash = shader_content_hash(bytecode.data(), bytecode.size());
    record_shader(content_hash, pixel, bytecode);
    group = dispatch_group_create();
    dispatch_group_enter(group);
    stats().shaders_created++;
    stats().shaders_pending++;
    dispatch_async(d->compile_queue(), ^{
        int expect = 0;
        if (state.compare_exchange_strong(expect, 3)) compile();
        dispatch_group_leave(group);
    });
}

void ShaderCommon::prioritize() {
    if (state.load(std::memory_order_acquire) != 0 || prioritized.exchange(true)) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        int expect = 0;
        if (state.compare_exchange_strong(expect, 3)) compile();
    });
}

void ShaderCommon::compile() {
    id<MTLDevice> md = dev->mdev();
    const std::string cache_dir = options().cache_dir;
    std::string err;
    uint64_t translate_ns = 0, compile_ns = 0;
    id<MTLLibrary> l = nil;
    id<MTLFunction> fnc = nil;
    bool ok = compile_shader(md, bytecode.data(), bytecode.size(), &x, &l, &fnc, &err, &translate_ns, &compile_ns);
    lib = l;
    fn = fnc;
    stats().shader_translate_ns += translate_ns;
    stats().shader_compile_ns += compile_ns;
    if (translate_ns + compile_ns > 400000000ull)
        LOGW(LC_GFX, "metal: slow %s shader %llu: translate %.0f ms, MSL compile %.0f ms (%zu tokens, %zu MSL bytes)", is_pixel ? "pixel" : "vertex",
             (unsigned long long)sid, translate_ns / 1e6, compile_ns / 1e6, bytecode.size(), x.msl.size());
    if (!ok) {
        error = err;
        stats().shader_failures++;
        LOGE(LC_GFX, "metal: %s shader %llu failed: %s", is_pixel ? "pixel" : "vertex", (unsigned long long)sid, err.c_str());
        if (!cache_dir.empty() && !x.msl.empty()) {   // keep the source for debugging on the device
            [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:cache_dir.c_str()] withIntermediateDirectories:YES attributes:nil error:nil];
            char path[1024];
            snprintf(path, sizeof path, "%s/failed_%s_%llu.metal", cache_dir.c_str(), is_pixel ? "ps" : "vs", (unsigned long long)sid);
            if (FILE *f = fopen(path, "w")) { fputs(x.msl.c_str(), f); fclose(f); }
        }
    }
    stats().shaders_pending--;
    state.store(ok ? 1 : 2, std::memory_order_release);
}

bool ShaderCommon::wait() {
    int s = state.load(std::memory_order_acquire);
    if (s == 0) {   // not started yet: compile it here rather than queue behind the others
        int expect = 0;
        if (state.compare_exchange_strong(expect, 3)) compile();
        s = state.load(std::memory_order_acquire);
    }
    if (s == 3 && group) {
        dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
        s = state.load(std::memory_order_acquire);
    }
    while (s == 3) { usleep(200); s = state.load(std::memory_order_acquire); }   // a prioritized compile outside the group
    return s == 1;
}

// ---- state blocks -------------------------------------------------------------------------------------------------------------------
HRESULT MtlStateBlock::Capture() {
    if (!dev) return D3DERR_INVALIDCALL;
    dev->st_capture(snap.get(), cats);
    return D3D_OK;
}
HRESULT MtlStateBlock::Apply() {
    if (!dev) return D3DERR_INVALIDCALL;
    dev->st_apply(*snap, cats);
    return D3D_OK;
}

}  // namespace mtl
}  // namespace dish
