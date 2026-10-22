// Metal backend: D3D format table, options, statistics, small utilities.
#include "mtl_internal.h"
#include <cstdlib>

namespace dish {
namespace mtl {

#define FOURCC(a, b, c, d) ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))
static const D3DFORMAT kINTZ = (D3DFORMAT)FOURCC('I', 'N', 'T', 'Z');
static const D3DFORMAT kRAWZ = (D3DFORMAT)FOURCC('R', 'A', 'W', 'Z');
static const D3DFORMAT kDF16 = (D3DFORMAT)FOURCC('D', 'F', '1', '6');
static const D3DFORMAT kDF24 = (D3DFORMAT)FOURCC('D', 'F', '2', '4');
static const D3DFORMAT kATI1 = (D3DFORMAT)FOURCC('A', 'T', 'I', '1');
static const D3DFORMAT kATI2 = (D3DFORMAT)FOURCC('A', 'T', 'I', '2');
static const D3DFORMAT kDXT2 = (D3DFORMAT)FOURCC('D', 'X', 'T', '2');
static const D3DFORMAT kDXT4 = (D3DFORMAT)FOURCC('D', 'X', 'T', '4');

// swz: 0 none, 1 alpha forced to one, 2 L8, 3 A8L8
static const FormatInfo kFormats[] = {
    // d3d                       mtl                              bpp block  depth stencil conv          swz rt     filter
    {D3DFMT_A8R8G8B8,            MTLPixelFormatBGRA8Unorm,        4,  false, false, false, Conv::None,   0, true,  true},
    {D3DFMT_X8R8G8B8,            MTLPixelFormatBGRA8Unorm,        4,  false, false, false, Conv::None,   1, true,  true},
    {D3DFMT_A8B8G8R8,            MTLPixelFormatRGBA8Unorm,        4,  false, false, false, Conv::None,   0, true,  true},
    {D3DFMT_X8B8G8R8,            MTLPixelFormatRGBA8Unorm,        4,  false, false, false, Conv::None,   1, true,  true},
    {D3DFMT_R5G6B5,              MTLPixelFormatB5G6R5Unorm,       2,  false, false, false, Conv::None,   0, true,  true},
    {D3DFMT_A1R5G5B5,            MTLPixelFormatBGR5A1Unorm,       2,  false, false, false, Conv::None,   0, true,  true},
    {D3DFMT_X1R5G5B5,            MTLPixelFormatBGR5A1Unorm,       2,  false, false, false, Conv::None,   1, true,  true},
    {D3DFMT_A4R4G4B4,            MTLPixelFormatBGRA8Unorm,        2,  false, false, false, Conv::A4R4G4B4, 0, false, true},
    {D3DFMT_X4R4G4B4,            MTLPixelFormatBGRA8Unorm,        2,  false, false, false, Conv::X4R4G4B4, 0, false, true},
    {D3DFMT_A8R3G3B2,            MTLPixelFormatBGRA8Unorm,        2,  false, false, false, Conv::A8R3G3B2, 0, false, true},
    {D3DFMT_R3G3B2,              MTLPixelFormatBGRA8Unorm,        1,  false, false, false, Conv::R3G3B2, 0, false, true},
    {D3DFMT_A8,                  MTLPixelFormatA8Unorm,           1,  false, false, false, Conv::None,   0, false, true},
    {D3DFMT_L8,                  MTLPixelFormatR8Unorm,           1,  false, false, false, Conv::None,   2, false, true},
    {D3DFMT_A8L8,                MTLPixelFormatRG8Unorm,          2,  false, false, false, Conv::None,   3, false, true},
    {D3DFMT_L16,                 MTLPixelFormatR16Unorm,          2,  false, false, false, Conv::None,   2, false, true},
    {D3DFMT_A2B10G10R10,         MTLPixelFormatRGB10A2Unorm,      4,  false, false, false, Conv::None,   0, true,  true},
    {D3DFMT_A2R10G10B10,         MTLPixelFormatBGR10A2Unorm,      4,  false, false, false, Conv::None,   0, true,  true},
    {D3DFMT_G16R16,              MTLPixelFormatRG16Unorm,         4,  false, false, false, Conv::None,   0, true,  true},
    {D3DFMT_A16B16G16R16,        MTLPixelFormatRGBA16Unorm,       8,  false, false, false, Conv::None,   0, true,  true},
    {D3DFMT_V8U8,                MTLPixelFormatRG8Snorm,          2,  false, false, false, Conv::None,   0, false, true},
    {D3DFMT_Q8W8V8U8,           MTLPixelFormatRGBA8Snorm,        4,  false, false, false, Conv::None,   0, false, true},
    {D3DFMT_V16U16,              MTLPixelFormatRG16Snorm,         4,  false, false, false, Conv::None,   0, false, true},
    {D3DFMT_Q16W16V16U16,        MTLPixelFormatRGBA16Snorm,       8,  false, false, false, Conv::None,   0, false, true},
    {D3DFMT_R16F,                MTLPixelFormatR16Float,          2,  false, false, false, Conv::None,   0, true,  true},
    {D3DFMT_G16R16F,             MTLPixelFormatRG16Float,         4,  false, false, false, Conv::None,   0, true,  true},
    {D3DFMT_A16B16G16R16F,       MTLPixelFormatRGBA16Float,       8,  false, false, false, Conv::None,   0, true,  true},
    {D3DFMT_R32F,                MTLPixelFormatR32Float,          4,  false, false, false, Conv::None,   0, true,  false},
    {D3DFMT_G32R32F,             MTLPixelFormatRG32Float,         8,  false, false, false, Conv::None,   0, true,  false},
    {D3DFMT_A32B32G32R32F,       MTLPixelFormatRGBA32Float,       16, false, false, false, Conv::None,   0, true,  false},
    {D3DFMT_DXT1,                MTLPixelFormatRGBA8Unorm,        8,  true,  false, false, Conv::Bc1,    0, false, true},
    {kDXT2,                      MTLPixelFormatRGBA8Unorm,        16, true,  false, false, Conv::Bc2,    0, false, true},
    {D3DFMT_DXT3,                MTLPixelFormatRGBA8Unorm,        16, true,  false, false, Conv::Bc2,    0, false, true},
    {kDXT4,                      MTLPixelFormatRGBA8Unorm,        16, true,  false, false, Conv::Bc3,    0, false, true},
    {D3DFMT_DXT5,                MTLPixelFormatRGBA8Unorm,        16, true,  false, false, Conv::Bc3,    0, false, true},
    {kATI1,                      MTLPixelFormatR8Unorm,           8,  true,  false, false, Conv::Bc4,    0, false, true},
    {kATI2,                      MTLPixelFormatRG8Unorm,          16, true,  false, false, Conv::Bc5,    0, false, true},
    // depth / stencil
    {D3DFMT_D16,                 MTLPixelFormatDepth16Unorm,      2,  false, true,  false, Conv::None,   0, false, true},
    {D3DFMT_D16_LOCKABLE,        MTLPixelFormatDepth16Unorm,      2,  false, true,  false, Conv::None,   0, false, true},
    {D3DFMT_D32,                 MTLPixelFormatDepth32Float,      4,  false, true,  false, Conv::None,   0, false, true},
    {D3DFMT_D32F_LOCKABLE,       MTLPixelFormatDepth32Float,      4,  false, true,  false, Conv::None,   0, false, true},
    {D3DFMT_D24S8,               MTLPixelFormatDepth32Float_Stencil8, 4, false, true, true, Conv::None,  0, false, true},
    {D3DFMT_D24X8,               MTLPixelFormatDepth32Float_Stencil8, 4, false, true, true, Conv::None,  0, false, true},
    {D3DFMT_D24FS8,              MTLPixelFormatDepth32Float_Stencil8, 4, false, true, true, Conv::None,  0, false, true},
    {D3DFMT_D24X4S4,             MTLPixelFormatDepth32Float_Stencil8, 4, false, true, true, Conv::None,  0, false, true},
    {kINTZ,                      MTLPixelFormatDepth32Float_Stencil8, 4, false, true, true, Conv::None,  0, false, true},
    {kDF24,                      MTLPixelFormatDepth32Float_Stencil8, 4, false, true, true, Conv::None,  0, false, true},
    {kRAWZ,                      MTLPixelFormatDepth32Float_Stencil8, 4, false, true, true, Conv::None,  0, false, true},
    {kDF16,                      MTLPixelFormatDepth16Unorm,      2,  false, true,  false, Conv::None,   0, false, true},
};

const FormatInfo *format_info(D3DFORMAT f) {
    for (const FormatInfo &fi : kFormats) if (fi.d3d == f) return &fi;
    return nullptr;
}
bool format_is_bc(D3DFORMAT f) { const FormatInfo *fi = format_info(f); return fi && fi->block; }

uint32_t level_pitch(const FormatInfo *fi, uint32_t w) {
    if (fi->block) return tex::bc_blocks(w) * fi->bpp;
    return (w * fi->bpp + 3) & ~3u;
}
uint32_t level_rows(const FormatInfo *fi, uint32_t h) { return fi->block ? tex::bc_blocks(h) : h; }
uint32_t level_bytes(const FormatInfo *fi, uint32_t w, uint32_t h) { return level_pitch(fi, w) * level_rows(fi, h); }
uint32_t full_mip_count(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t m = std::max(w, std::max(h, d)), n = 1;
    while (m > 1) { m >>= 1; n++; }
    return n;
}

static MtlStats g_stats;
MtlStats &stats() { return g_stats; }

const Options &options() {
    static Options o;
    static bool init = false;
    if (!init) {
        init = true;
        if (const char *e = getenv("DISH_METAL_ASTC")) o.astc = e[0] && e[0] != '0';
        if (const char *e = getenv("DISH_METAL_TEX_SHADOW")) o.tex_shadow = e[0] && e[0] != '0';
        if (const char *e = getenv("DISH_METAL_SYNC_SHADERS")) o.sync_shaders = e[0] && e[0] != '0';
        if (const char *e = getenv("DISH_METAL_UPSCALE")) {
            std::string v = e;
            o.upscale = v == "off" || v == "bilinear" || v == "0" ? 0 : 1;
        }
        if (const char *e = getenv("DISH_METAL_INTERNAL_HEIGHT")) o.internal_height = std::max(240, atoi(e));
        if (const char *e = getenv("DISH_METAL_VALIDATE")) o.verbose = e[0] && e[0] != '0';
        if (const char *e = getenv("DISH_METAL_CACHE_DIR")) o.cache_dir = e;
        if (o.cache_dir.empty()) {
            @autoreleasepool {
                NSArray *dirs = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
                if (dirs.count) o.cache_dir = std::string([dirs[0] UTF8String]) + "/DishMetal";
            }
        }
    }
    return o;
}

HRESULT stub_called(const char *what) {
    static std::mutex mu;
    static std::unordered_map<std::string, int> seen;
    std::lock_guard<std::mutex> lk(mu);
    int n = ++seen[what];
    if (n == 1) LOGE(LC_GFX, "metal d3d9: unimplemented method %s called (D3DERR_NOTAVAILABLE)", what);
    return D3DERR_NOTAVAILABLE;
}

}  // namespace mtl
}  // namespace dish
