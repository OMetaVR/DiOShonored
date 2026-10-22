// gdi32: just enough for what the game and Bink use (memory DCs, DIB sections, pixels, device caps). There is no glyph rasteriser:
// text calls report plausible metrics and draw nothing (see docs/runtime.md, known gaps).
#include "win_internal.h"
#include "dish/heap.h"
#include "dish/hle.h"
#include "dish/log.h"
#include "dish/thread.h"
#include "dish/util.h"
#include <cstring>

namespace dish {
using namespace win;
namespace {

enum class GKind { Dc, Bitmap, Font, Brush, Pen, Stock };
struct GObj {
    GKind kind;
    uint32_t bits = 0;               // guest pointer to pixel memory (bitmaps)
    int32_t w = 0, h = 0, bpp = 32, stride = 0;
    bool top_down = false;
    bool owns_bits = false;
    int32_t font_height = 16, font_weight = 400;
    uint32_t selected_bitmap = 0, selected_font = 0;   // for DCs
    uint32_t bk = 0xFFFFFF, text = 0, bkmode = 2, stretch = 1;
};
std::mutex g_gmu;
std::map<uint32_t, GObj> g_objs;
uint32_t g_next = 0x00090010;

uint32_t add(GObj o) {
    std::lock_guard<std::mutex> lk(g_gmu);
    uint32_t h = g_next;
    g_next += 4;
    g_objs[h] = std::move(o);
    return h;
}

uint32_t gdi_CreateCompatibleDC(uint32_t) { GObj o{GKind::Dc}; return add(o); }
uint32_t make_bitmap(int32_t w, int32_t h, int bpp, bool top_down, uint32_t *bits_out) {
    GObj o{GKind::Bitmap};
    o.w = w; o.h = h; o.bpp = bpp; o.top_down = top_down;
    o.stride = (w * bpp + 31) / 32 * 4;
    uint32_t sz = (uint32_t)o.stride * (uint32_t)(h > 0 ? h : 1);
    o.bits = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, sz ? sz : 4);
    o.owns_bits = true;
    if (!o.bits) return 0;
    if (bits_out) *bits_out = o.bits;
    return add(o);
}
uint32_t gdi_CreateCompatibleBitmap(uint32_t, int32_t w, int32_t h) { return make_bitmap(w, h, 32, true, nullptr); }
uint32_t gdi_CreateDIBSection(uint32_t, uint32_t bmi, uint32_t, uint32_t ppbits, uint32_t, uint32_t) {
    int32_t w = (int32_t)rd32(bmi + 4), h = (int32_t)rd32(bmi + 8);
    int bpp = rd16(bmi + 14);
    if (bpp == 0) bpp = 32;
    uint32_t bits = 0;
    uint32_t hb = make_bitmap(w, h < 0 ? -h : h, bpp, h < 0, &bits);
    if (ppbits) wr32(ppbits, bits);
    return hb;
}
uint32_t gdi_DeleteDC(uint32_t h) { std::lock_guard<std::mutex> lk(g_gmu); return g_objs.erase(h) != 0; }
uint32_t gdi_DeleteObject(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_gmu);
    auto it = g_objs.find(h);
    if (it == g_objs.end()) return 0;
    if (it->second.owns_bits && it->second.bits) heap_free(heap_process(), 0, it->second.bits);
    g_objs.erase(it);
    return 1;
}
uint32_t gdi_SelectObject(uint32_t hdc, uint32_t obj) {
    std::lock_guard<std::mutex> lk(g_gmu);
    auto d = g_objs.find(hdc);
    auto o = g_objs.find(obj);
    if (d == g_objs.end() || o == g_objs.end()) return 0;
    uint32_t prev = 0;
    if (o->second.kind == GKind::Bitmap) { prev = d->second.selected_bitmap; d->second.selected_bitmap = obj; }
    else if (o->second.kind == GKind::Font) { prev = d->second.selected_font; d->second.selected_font = obj; }
    else prev = 0x000A0001;
    return prev;
}
uint32_t gdi_GetStockObject(int32_t i) {
    static std::map<int32_t, uint32_t> stock;
    std::lock_guard<std::mutex> lk(g_gmu);
    auto it = stock.find(i);
    if (it != stock.end()) return it->second;
    uint32_t h = g_next; g_next += 4;
    GObj o{i >= 10 && i <= 17 ? GKind::Font : i >= 6 && i <= 8 ? GKind::Pen : GKind::Brush};
    g_objs[h] = o;
    return stock[i] = h;
}
uint32_t gdi_CreateFontIndirectW(uint32_t lf) {
    GObj o{GKind::Font};
    o.font_height = std::abs((int32_t)rd32(lf));
    if (!o.font_height) o.font_height = 16;
    o.font_weight = (int32_t)rd32(lf + 16);
    return add(o);
}
uint32_t gdi_GetObjectW(uint32_t h, int32_t cb, uint32_t buf) {
    std::lock_guard<std::mutex> lk(g_gmu);
    auto it = g_objs.find(h);
    if (it == g_objs.end()) return 0;
    const GObj &o = it->second;
    if (o.kind == GKind::Bitmap) {
        if (!buf) return 24;
        if (cb < 24) return 0;
        wr32(buf, 0); wr32(buf + 4, (uint32_t)o.w); wr32(buf + 8, (uint32_t)o.h); wr32(buf + 12, (uint32_t)o.stride);
        wr16(buf + 16, 1); wr16(buf + 18, (uint16_t)o.bpp); wr32(buf + 20, o.bits);
        return 24;
    }
    if (o.kind == GKind::Font) {
        if (!buf) return 92;
        memset(guest_base + buf, 0, (size_t)std::min(cb, 92));
        wr32(buf, (uint32_t)-o.font_height);
        wr32(buf + 16, (uint32_t)o.font_weight);
        return 92;
    }
    return 0;
}
uint32_t gdi_GetDeviceCaps(uint32_t, int32_t idx) {
    const DisplayInfo &d = primary_display();
    switch (idx) {
        case 0: return 0x0200;                 // DRIVERVERSION
        case 2: return 1;                      // TECHNOLOGY: DT_RASDISPLAY
        case 4: return (uint32_t)(d.w * 254 / 960 / 10);   // HORZSIZE (mm)
        case 6: return (uint32_t)(d.h * 254 / 960 / 10);
        case 8: return (uint32_t)d.w;          // HORZRES
        case 10: return (uint32_t)d.h;         // VERTRES
        case 12: return 32;                    // BITSPIXEL
        case 14: return 1;                     // PLANES
        case 24: return 0xFFFFFFFFu;           // NUMCOLORS
        case 38: return 0x7E99;                // RASTERCAPS
        case 88: case 90: return 96;           // LOGPIXELSX/Y
        case 104: return 0;                    // SIZEPALETTE
        case 108: return 24;                   // COLORRES
        case 116: return (uint32_t)d.hz;       // VREFRESH
        case 117: return (uint32_t)d.h;        // DESKTOPVERTRES
        case 118: return (uint32_t)d.w;        // DESKTOPHORZRES
        default: return 0;
    }
}
template <class F> uint32_t with_dc(uint32_t hdc, F f) {
    std::lock_guard<std::mutex> lk(g_gmu);
    auto d = g_objs.find(hdc);
    if (d == g_objs.end() || d->second.kind != GKind::Dc) return 0;
    return f(d->second);
}
uint32_t gdi_SetBkColor(uint32_t hdc, uint32_t c) { return with_dc(hdc, [&](GObj &d) { uint32_t o = d.bk; d.bk = c; return o; }); }
uint32_t gdi_SetTextColor(uint32_t hdc, uint32_t c) { return with_dc(hdc, [&](GObj &d) { uint32_t o = d.text; d.text = c; return o; }); }
uint32_t gdi_SetBkMode(uint32_t hdc, uint32_t m) { return with_dc(hdc, [&](GObj &d) { uint32_t o = d.bkmode; d.bkmode = m; return o; }); }
uint32_t gdi_SetStretchBltMode(uint32_t hdc, uint32_t m) { return with_dc(hdc, [&](GObj &d) { uint32_t o = d.stretch; d.stretch = m; return o; }); }
GObj *selected(GObj &dc) {
    auto b = g_objs.find(dc.selected_bitmap);
    return b == g_objs.end() ? nullptr : &b->second;
}
uint32_t pixel_addr(const GObj &b, int x, int y) {
    int row = b.top_down ? y : b.h - 1 - y;
    return b.bits + (uint32_t)row * (uint32_t)b.stride + (uint32_t)x * (uint32_t)(b.bpp / 8);
}
uint32_t gdi_GetPixel(uint32_t hdc, int32_t x, int32_t y) {
    return with_dc(hdc, [&](GObj &d) -> uint32_t {
        GObj *b = selected(d);
        if (!b || x < 0 || y < 0 || x >= b->w || y >= b->h || b->bpp < 24) return 0xFFFFFFFFu;   // CLR_INVALID
        uint32_t a = pixel_addr(*b, x, y);
        return (uint32_t)rd8(a + 2) | ((uint32_t)rd8(a + 1) << 8) | ((uint32_t)rd8(a) << 16);
    });
}
uint32_t gdi_SetPixel(uint32_t hdc, int32_t x, int32_t y, uint32_t c) {
    return with_dc(hdc, [&](GObj &d) -> uint32_t {
        GObj *b = selected(d);
        if (!b || x < 0 || y < 0 || x >= b->w || y >= b->h || b->bpp < 24) return 0xFFFFFFFFu;
        uint32_t a = pixel_addr(*b, x, y);
        wr8(a, (uint8_t)(c >> 16)); wr8(a + 1, (uint8_t)(c >> 8)); wr8(a + 2, (uint8_t)c);
        if (b->bpp == 32) wr8(a + 3, 0xFF);
        return c;
    });
}
uint32_t gdi_StretchBlt(uint32_t dst, int32_t dx, int32_t dy, int32_t dw, int32_t dh, uint32_t src, int32_t sx, int32_t sy, int32_t sw, int32_t sh, uint32_t rop) {
    (void)rop;
    std::lock_guard<std::mutex> lk(g_gmu);
    auto d = g_objs.find(dst), s = g_objs.find(src);
    if (d == g_objs.end() || s == g_objs.end()) return 0;
    GObj *db = selected(d->second), *sb = selected(s->second);
    if (!db || !sb || db->bpp < 24 || sb->bpp < 24 || !dw || !dh || !sw || !sh) return 0;
    for (int y = 0; y < std::abs(dh); y++) {
        for (int x = 0; x < std::abs(dw); x++) {
            int ox = dx + x, oy = dy + y;
            int ix = sx + x * std::abs(sw) / std::abs(dw), iy = sy + y * std::abs(sh) / std::abs(dh);
            if (ox < 0 || oy < 0 || ox >= db->w || oy >= db->h || ix < 0 || iy < 0 || ix >= sb->w || iy >= sb->h) continue;
            uint32_t da = pixel_addr(*db, ox, oy), sa = pixel_addr(*sb, ix, iy);
            wr8(da, rd8(sa)); wr8(da + 1, rd8(sa + 1)); wr8(da + 2, rd8(sa + 2));
            if (db->bpp == 32) wr8(da + 3, sb->bpp == 32 ? rd8(sa + 3) : 0xFF);
        }
    }
    return 1;
}
int32_t gdi_GetDIBits(uint32_t, uint32_t hbmp, uint32_t start, uint32_t lines, uint32_t bits, uint32_t bmi, uint32_t) {
    std::lock_guard<std::mutex> lk(g_gmu);
    auto it = g_objs.find(hbmp);
    if (it == g_objs.end() || it->second.kind != GKind::Bitmap) return 0;
    const GObj &b = it->second;
    if (bmi && !bits) {   // query: fill the header
        wr32(bmi, 40); wr32(bmi + 4, (uint32_t)b.w); wr32(bmi + 8, (uint32_t)(b.top_down ? -b.h : b.h)); wr16(bmi + 12, 1); wr16(bmi + 14, (uint16_t)b.bpp);
        wr32(bmi + 16, 0); wr32(bmi + 20, (uint32_t)b.stride * (uint32_t)b.h);
        return (int32_t)b.h;
    }
    if (!bits || !bmi) return 0;
    int32_t want_bpp = rd16(bmi + 14);
    if (want_bpp != b.bpp) return 0;   // no format conversion
    bool want_top_down = (int32_t)rd32(bmi + 8) < 0;
    uint32_t n = std::min<uint32_t>(lines, (uint32_t)b.h - std::min<uint32_t>(start, (uint32_t)b.h));
    for (uint32_t y = 0; y < n; y++) {
        uint32_t src_row = want_top_down == b.top_down ? start + y : (uint32_t)b.h - 1 - (start + y);
        memcpy(guest_base + bits + y * (uint32_t)b.stride, guest_base + b.bits + src_row * (uint32_t)b.stride, (size_t)b.stride);
    }
    return (int32_t)n;
}
uint32_t gdi_GetTextExtentPoint32W(uint32_t hdc, uint32_t str, int32_t n, uint32_t psize) {
    int h = 16;
    {
        std::lock_guard<std::mutex> lk(g_gmu);
        auto d = g_objs.find(hdc);
        if (d != g_objs.end()) { auto f = g_objs.find(d->second.selected_font); if (f != g_objs.end()) h = f->second.font_height; }
    }
    (void)str;
    wr32(psize, (uint32_t)(n * (h / 2 + 1)));
    wr32(psize + 4, (uint32_t)h);
    return 1;
}
uint32_t gdi_TextOutW(uint32_t, int32_t, int32_t, uint32_t, int32_t) { return 1; }

}  // namespace

HLE("gdi32.dll", "CreateCompatibleDC", Stdcall, gdi_CreateCompatibleDC);
HLE("gdi32.dll", "CreateCompatibleBitmap", Stdcall, gdi_CreateCompatibleBitmap);
HLE("gdi32.dll", "CreateDIBSection", Stdcall, gdi_CreateDIBSection);
HLE("gdi32.dll", "DeleteDC", Stdcall, gdi_DeleteDC);
HLE("gdi32.dll", "DeleteObject", Stdcall, gdi_DeleteObject);
HLE("gdi32.dll", "SelectObject", Stdcall, gdi_SelectObject);
HLE("gdi32.dll", "GetStockObject", Stdcall, gdi_GetStockObject);
HLE("gdi32.dll", "CreateFontIndirectW", Stdcall, gdi_CreateFontIndirectW);
HLE("gdi32.dll", "GetObjectW", Stdcall, gdi_GetObjectW);
HLE("gdi32.dll", "GetDeviceCaps", Stdcall, gdi_GetDeviceCaps);
HLE("gdi32.dll", "SetBkColor", Stdcall, gdi_SetBkColor);
HLE("gdi32.dll", "SetTextColor", Stdcall, gdi_SetTextColor);
HLE("gdi32.dll", "SetBkMode", Stdcall, gdi_SetBkMode);
HLE("gdi32.dll", "SetStretchBltMode", Stdcall, gdi_SetStretchBltMode);
HLE("gdi32.dll", "GetPixel", Stdcall, gdi_GetPixel);
HLE("gdi32.dll", "SetPixel", Stdcall, gdi_SetPixel);
HLE("gdi32.dll", "StretchBlt", Stdcall, gdi_StretchBlt);
HLE("gdi32.dll", "GetDIBits", Stdcall, gdi_GetDIBits);
HLE("gdi32.dll", "GetTextExtentPoint32W", Stdcall, gdi_GetTextExtentPoint32W);
HLE("gdi32.dll", "TextOutW", Stdcall, gdi_TextOutW);

}  // namespace dish
