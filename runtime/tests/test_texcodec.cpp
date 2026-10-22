// Texel codecs of the Metal backend (platform/d3d9_metal/texcodec.*): BC decode, DXT -> ASTC transcode error, packed expanders.
#include "testing.h"
#include "../platform/d3d9_metal/texcodec.h"
#include <cmath>
#include <cstring>
#include <vector>

using namespace dish::tex;

// ---- small DXT encoders (same idea as the scene's) ----
static uint16_t to565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
static void from565(uint16_t c, int *r, int *g, int *b) { *r = ((c >> 11) & 31) * 255 / 31; *g = ((c >> 5) & 63) * 255 / 63; *b = (c & 31) * 255 / 31; }
static void enc_color(const uint8_t *px, uint8_t *out, bool allow3 = false) {
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) for (int c = 0; c < 3; c++) { int v = px[i * 4 + c]; mn[c] = std::min(mn[c], v); mx[c] = std::max(mx[c], v); }
    uint16_t c0 = to565(mx[0], mx[1], mx[2]), c1 = to565(mn[0], mn[1], mn[2]);
    if (c0 < c1) std::swap(c0, c1);
    if (c0 == c1) { if (c0 < 0xFFFF) c0++; else c1--; }
    int p[4][3];
    from565(c0, &p[0][0], &p[0][1], &p[0][2]); from565(c1, &p[1][0], &p[1][1], &p[1][2]);
    for (int c = 0; c < 3; c++) { p[2][c] = (2 * p[0][c] + p[1][c]) / 3; p[3][c] = (p[0][c] + 2 * p[1][c]) / 3; }
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 4; k++) { int d = 0; for (int c = 0; c < 3; c++) { int e = px[i * 4 + c] - p[k][c]; d += e * e; } if (d < bd) { bd = d; best = k; } }
        idx |= (uint32_t)best << (2 * i);
    }
    out[0] = c0 & 255; out[1] = c0 >> 8; out[2] = c1 & 255; out[3] = c1 >> 8;
    memcpy(out + 4, &idx, 4);
    (void)allow3;
}
static void enc_alpha(const uint8_t *px, uint8_t *out) {
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; i++) { int a = px[i * 4 + 3]; mn = std::min(mn, a); mx = std::max(mx, a); }
    int a0 = mx, a1 = mn;
    if (a0 == a1) { if (a0 < 255) a0++; else a1--; }
    int tab[8]; tab[0] = a0; tab[1] = a1;
    for (int k = 1; k < 7; k++) tab[1 + k] = ((7 - k) * a0 + k * a1) / 7;
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) { int best = 0, bd = 1 << 30; for (int k = 0; k < 8; k++) { int d = abs(px[i * 4 + 3] - tab[k]); if (d < bd) { bd = d; best = k; } } bits |= (uint64_t)best << (3 * i); }
    out[0] = (uint8_t)a0; out[1] = (uint8_t)a1;
    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(bits >> (8 * i));
}

static void make_image(int w, int h, std::vector<uint8_t> *px, bool alpha) {
    px->resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t *o = &(*px)[((size_t)y * w + x) * 4];
        float u = (x + .5f) / w, v = (y + .5f) / h;
        int chk = ((x / 8) + (y / 8)) & 1;
        o[0] = (uint8_t)(chk ? 230 : 30 + 180 * u);
        o[1] = (uint8_t)(40 + 150 * v + 40 * sinf(u * 12));
        o[2] = (uint8_t)(120 + 100 * cosf(v * 9) * u);
        o[3] = alpha ? (uint8_t)(255 * (0.5f + 0.5f * sinf((u + v) * 8))) : 255;
    }
}
static std::vector<uint8_t> compress(Bc f, const std::vector<uint8_t> &px, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    std::vector<uint8_t> out((size_t)bw * bh * bc_block_bytes(f));
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        uint8_t blk[64];
        for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
            int sx = std::min(bx * 4 + i, w - 1), sy = std::min(by * 4 + j, h - 1);
            memcpy(blk + (j * 4 + i) * 4, &px[((size_t)sy * w + sx) * 4], 4);
        }
        uint8_t *d = &out[((size_t)by * bw + bx) * bc_block_bytes(f)];
        if (f == Bc::DXT5) { enc_alpha(blk, d); enc_color(blk, d + 8); }
        else if (f == Bc::DXT1) enc_color(blk, d);
        else if (f == Bc::DXT3) {
            for (int i = 0; i < 8; i++) d[i] = (uint8_t)(((blk[(2 * i) * 4 + 3] + 8) / 17) | (((blk[(2 * i + 1) * 4 + 3] + 8) / 17) << 4));
            enc_color(blk, d + 8);
        }
    }
    return out;
}
static double psnr(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, int channels_mask, double *maxerr) {
    double se = 0; size_t n = 0; int mx = 0;
    for (size_t i = 0; i < a.size(); i++) {
        if (!((channels_mask >> (i & 3)) & 1)) continue;
        int d = (int)a[i] - (int)b[i];
        se += (double)d * d; n++; mx = std::max(mx, abs(d));
    }
    if (maxerr) *maxerr = mx;
    if (se == 0) return 99;
    return 10 * log10(255.0 * 255.0 / (se / n));
}

TEST(dxt_decode_known_blocks) {
    // DXT1: c0 = pure red (f800), c1 = pure blue (001f), indices 0,1,2,3 repeating
    uint8_t b1[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};   // 0b11100100 repeated: texel0=0, 1=1, 2=2, 3=3
    uint8_t out[64];
    bc_decode_block(Bc::DXT1, b1, out);
    CHECK_EQ(out[0], 255); CHECK_EQ(out[1], 0); CHECK_EQ(out[2], 0); CHECK_EQ(out[3], 255);                // red
    CHECK_EQ(out[4], 0);   CHECK_EQ(out[5], 0); CHECK_EQ(out[6], 255);                                      // blue
    CHECK_EQ(out[8], 170); CHECK_EQ(out[10], 85);                                                           // (2r+b)/3, (2r+b)/3 on blue
    CHECK_EQ(out[12], 85); CHECK_EQ(out[14], 170);
    // DXT1 3-colour mode: c0 <= c1, index 3 is transparent black
    uint8_t b3[8] = {0x1F, 0x00, 0x00, 0xF8, 0xE4, 0xE4, 0xE4, 0xE4};
    bc_decode_block(Bc::DXT1, b3, out);
    CHECK_EQ(out[11], 255);      // texel 2 is the midpoint, opaque
    CHECK_EQ(out[10], 127);
    CHECK_EQ(out[15], 0);        // texel 3 alpha 0
    // DXT3 explicit alpha
    uint8_t b5[16] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
    bc_decode_block(Bc::DXT3, b5, out);
    CHECK_EQ(out[3], 0); CHECK_EQ(out[7], 17); CHECK_EQ(out[63], 255);
    // DXT5 alpha: a0=255, a1=0 -> 8-step ramp; all indices 1 => alpha 0, all 0 => 255
    uint8_t b6[16] = {255, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
    bc_decode_block(Bc::DXT5, b6, out);
    CHECK_EQ(out[3], 255);
    b6[2] = 0x09;   // texel0 = 1, texel1 = 1 (bits 001 001)
    bc_decode_block(Bc::DXT5, b6, out);
    CHECK_EQ(out[3], 0); CHECK_EQ(out[7], 0); CHECK_EQ(out[11], 255);
}

TEST(dxt_level_decode_partial_blocks) {
    // 6x2 DXT1 level = 2 blocks wide, 1 block high; output must be exactly 6x2 texels
    uint8_t blk[16] = {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0, 0x1F, 0x00, 0x00, 0xF8, 0, 0, 0, 0};
    std::vector<uint8_t> out(6 * 2 * 4, 0xCC);
    bc_decode_level(Bc::DXT1, blk, 6, 2, out.data(), 6 * 4);
    CHECK_EQ(out[0], 255);            // first texel red
    CHECK_EQ(out[(6 * 1 + 5) * 4 + 2], 255 * 1 / 1);   // last texel of the second row is from the second block: c0=blue
}

static void roundtrip(Bc f, bool alpha, double min_psnr_rgb, double min_psnr_a) {
    const int w = 128, h = 64;
    std::vector<uint8_t> px;
    make_image(w, h, &px, alpha);
    std::vector<uint8_t> bc = compress(f, px, w, h);
    std::vector<uint8_t> ref((size_t)w * h * 4);
    bc_decode_level(f, bc.data(), w, h, ref.data(), w * 4);
    std::vector<uint8_t> astc(astc_level_bytes(w, h));
    AstcStats st;
    bc_to_astc_level(f, bc.data(), w, h, astc.data(), &st);
    std::vector<uint8_t> dec((size_t)w * h * 4);
    int bad = 0;
    for (int by = 0; by < h / 4; by++) for (int bx = 0; bx < w / 4; bx++) {
        uint8_t blk[64];
        if (!astc_decode_block_subset(&astc[((size_t)by * (w / 4) + bx) * 16], blk)) { bad++; continue; }
        for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) memcpy(&dec[((size_t)(by * 4 + j) * w + bx * 4 + i) * 4], blk + (j * 4 + i) * 4, 4);
    }
    CHECK_EQ(bad, 0);
    double mx_rgb, mx_a;
    double p_rgb = psnr(ref, dec, 7, &mx_rgb), p_a = psnr(ref, dec, 8, &mx_a);
    fprintf(stderr, "  fmt=%d: blocks=%u dual=%u const=%u  PSNR rgb=%.1f dB (max err %.0f) alpha=%.1f dB (max err %.0f)\n", (int)f, st.blocks, st.dual_plane, st.constant, p_rgb, mx_rgb, p_a, mx_a);
    CHECK(p_rgb >= min_psnr_rgb);
    CHECK(p_a >= min_psnr_a);
}

TEST(dxt1_to_astc_error) { roundtrip(Bc::DXT1, false, 40.0, 90.0); }
TEST(dxt3_to_astc_error) { roundtrip(Bc::DXT3, true, 33.0, 28.0); }
TEST(dxt5_to_astc_error) { roundtrip(Bc::DXT5, true, 33.0, 30.0); }

TEST(astc_constant_blocks) {
    uint8_t b[16];
    // a DXT1 block with c0 == c1 is a flat colour
    uint8_t dxt[8] = {0x1F, 0x00, 0x1F, 0x00, 0, 0, 0, 0};   // blue, but c0 == c1 => 3-colour mode with idx 0 only => opaque blue
    bc_to_astc_level(Bc::DXT1, dxt, 4, 4, b);
    uint8_t out[64];
    CHECK(astc_decode_block_subset(b, out));
    CHECK_EQ(out[0], 0); CHECK_EQ(out[2], 255); CHECK_EQ(out[3], 255); CHECK_EQ(out[60], 0); CHECK_EQ(out[62], 255);
}

TEST(expanders) {
    uint16_t a[2] = {0xF0F0, 0x0F0F};
    uint8_t out[8];
    expand_a4r4g4b4_to_bgra8(a, out, 2);
    CHECK_EQ(out[3], 255); CHECK_EQ(out[2], 0); CHECK_EQ(out[1], 255); CHECK_EQ(out[0], 0);
    CHECK_EQ(out[7], 0); CHECK_EQ(out[6], 255); CHECK_EQ(out[5], 0); CHECK_EQ(out[4], 255);
}
