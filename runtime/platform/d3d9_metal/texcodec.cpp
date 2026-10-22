#include "texcodec.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace dish {
namespace tex {

#include "astc_tables.inc"

namespace {

inline uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
inline void expand565(uint32_t c, int *r, int *g, int *b) {
    int r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

// BC4-style 8-bit channel block (also the DXT5 alpha block): out[16] = values
void decode_bc4_channel(const uint8_t *s, uint8_t out[16]) {
    int a0 = s[0], a1 = s[1];
    int tab[8];
    tab[0] = a0;
    tab[1] = a1;
    if (a0 > a1) {
        for (int k = 1; k < 7; k++) tab[1 + k] = ((7 - k) * a0 + k * a1) / 7;
    } else {
        for (int k = 1; k < 5; k++) tab[1 + k] = ((5 - k) * a0 + k * a1) / 5;
        tab[6] = 0;
        tab[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)s[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)tab[(bits >> (3 * i)) & 7];
}

// BC1 colour block: writes RGB into out (stride 4) and returns whether the block used punch-through alpha (3-colour mode)
// `alpha_out` (optional) receives 0/255 per texel for DXT1.
struct Bc1Info { int c0[3], c1[3]; bool four_color; uint32_t idx; };
void bc1_info(const uint8_t *s, Bc1Info *bi) {
    uint32_t c0 = rd16(s), c1 = rd16(s + 2);
    expand565(c0, &bi->c0[0], &bi->c0[1], &bi->c0[2]);
    expand565(c1, &bi->c1[0], &bi->c1[1], &bi->c1[2]);
    bi->four_color = c0 > c1;
    bi->idx = s[4] | (s[5] << 8) | (s[6] << 16) | ((uint32_t)s[7] << 24);
}
void bc1_palette(const Bc1Info &bi, bool force_four, int pal[4][4]) {
    for (int c = 0; c < 3; c++) {
        pal[0][c] = bi.c0[c];
        pal[1][c] = bi.c1[c];
        if (bi.four_color || force_four) {
            pal[2][c] = (2 * bi.c0[c] + bi.c1[c]) / 3;
            pal[3][c] = (bi.c0[c] + 2 * bi.c1[c]) / 3;
        } else {
            pal[2][c] = (bi.c0[c] + bi.c1[c]) / 2;
            pal[3][c] = 0;
        }
    }
    pal[0][3] = pal[1][3] = pal[2][3] = 255;
    pal[3][3] = (bi.four_color || force_four) ? 255 : 0;
}

}  // namespace

void bc_decode_block(Bc f, const uint8_t *src, uint8_t out[64]) {
    switch (f) {
        case Bc::DXT1: {
            Bc1Info bi;
            bc1_info(src, &bi);
            int pal[4][4];
            bc1_palette(bi, false, pal);
            for (int i = 0; i < 16; i++) {
                int k = (bi.idx >> (2 * i)) & 3;
                for (int c = 0; c < 4; c++) out[i * 4 + c] = (uint8_t)pal[k][c];
            }
            break;
        }
        case Bc::DXT3: {
            Bc1Info bi;
            bc1_info(src + 8, &bi);
            int pal[4][4];
            bc1_palette(bi, true, pal);
            for (int i = 0; i < 16; i++) {
                int k = (bi.idx >> (2 * i)) & 3;
                for (int c = 0; c < 3; c++) out[i * 4 + c] = (uint8_t)pal[k][c];
                int a4 = (src[i >> 1] >> ((i & 1) * 4)) & 15;
                out[i * 4 + 3] = (uint8_t)(a4 * 17);
            }
            break;
        }
        case Bc::DXT5: {
            Bc1Info bi;
            bc1_info(src + 8, &bi);
            int pal[4][4];
            bc1_palette(bi, true, pal);
            uint8_t al[16];
            decode_bc4_channel(src, al);
            for (int i = 0; i < 16; i++) {
                int k = (bi.idx >> (2 * i)) & 3;
                for (int c = 0; c < 3; c++) out[i * 4 + c] = (uint8_t)pal[k][c];
                out[i * 4 + 3] = al[i];
            }
            break;
        }
        case Bc::ATI1: {
            uint8_t r[16];
            decode_bc4_channel(src, r);
            for (int i = 0; i < 16; i++) { out[i * 4] = r[i]; out[i * 4 + 1] = 0; out[i * 4 + 2] = 0; out[i * 4 + 3] = 255; }
            break;
        }
        case Bc::ATI2: {
            uint8_t r[16], g[16];
            decode_bc4_channel(src, r);
            decode_bc4_channel(src + 8, g);
            for (int i = 0; i < 16; i++) { out[i * 4] = r[i]; out[i * 4 + 1] = g[i]; out[i * 4 + 2] = 0; out[i * 4 + 3] = 255; }
            break;
        }
    }
}

void bc_decode_level(Bc f, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, size_t dst_pitch) {
    const uint32_t bw = bc_blocks(w), bh = bc_blocks(h), bb = bc_block_bytes(f), bpp = bc_decoded_bpp(f);
    uint8_t blk[64];
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            bc_decode_block(f, src + ((size_t)by * bw + bx) * bb, blk);
            for (uint32_t j = 0; j < 4; j++) {
                uint32_t y = by * 4 + j;
                if (y >= h) break;
                for (uint32_t i = 0; i < 4; i++) {
                    uint32_t x = bx * 4 + i;
                    if (x >= w) break;
                    uint8_t *d = dst + y * dst_pitch + (size_t)x * bpp;
                    const uint8_t *s = blk + (j * 4 + i) * 4;
                    if (bpp == 4) memcpy(d, s, 4);
                    else if (bpp == 2) { d[0] = s[0]; d[1] = s[1]; }
                    else d[0] = s[0];
                }
            }
        }
    }
}

// ---- ASTC ----------------------------------------------------------------------------------------------------------------------

namespace {

struct BitWriter {
    uint8_t *b;
    void put(uint32_t value, int nbits, int pos) {   // LSB-first at bit position `pos`
        for (int i = 0; i < nbits; i++) {
            int p = pos + i;
            uint8_t m = (uint8_t)(1u << (p & 7));
            if ((value >> i) & 1) b[p >> 3] |= m; else b[p >> 3] &= (uint8_t)~m;
        }
    }
    // weight sequence bit k lives at physical bit 127-k
    void put_rev(uint32_t value, int nbits, int seq_pos) {
        for (int i = 0; i < nbits; i++) {
            int p = 127 - (seq_pos + i);
            uint8_t m = (uint8_t)(1u << (p & 7));
            if ((value >> i) & 1) b[p >> 3] |= m; else b[p >> 3] &= (uint8_t)~m;
        }
    }
};

// Trit ISE of `n` values (each 0..47: trit = v/16, low 4 bits = v&15) starting at bit `pos`. Mirrors astcenc encode_ise for QUANT_48.
int encode_ise_q48(BitWriter &bw, const uint8_t *vals, int n, int pos) {
    int i = 0;
    while (i < n) {
        int cnt = std::min(5, n - i);
        int t[5] = {0, 0, 0, 0, 0};
        for (int k = 0; k < cnt; k++) t[k] = vals[i + k] >> 4;
        uint32_t T = kAstcTritPack[t[0] + 3 * t[1] + 9 * t[2] + 27 * t[3] + 81 * t[4]];
        static const int tbits[5] = {2, 2, 1, 2, 1}, tshift[5] = {0, 2, 4, 5, 7};
        for (int k = 0; k < cnt; k++) {
            uint32_t pack = (vals[i + k] & 15) | (((T >> tshift[k]) & ((1u << tbits[k]) - 1)) << 4);
            bw.put(pack, 4 + tbits[k], pos);
            pos += 4 + tbits[k];
        }
        i += cnt;
    }
    return pos;
}

int q48_index_for(int u8) {
    static uint8_t lut[256];
    static bool init = false;
    if (!init) {
        for (int v = 0; v < 256; v++) {
            int best = 0, bd = 1 << 30;
            for (int q = 0; q < 48; q++) { int d = abs((int)kAstcQ48ToU8[q] - v); if (d < bd) { bd = d; best = q; } }
            lut[v] = (uint8_t)best;
        }
        init = true;
    }
    return lut[u8 < 0 ? 0 : u8 > 255 ? 255 : u8];
}

const uint8_t kBcToAstcLevel[4] = {0, 3, 1, 2};   // BC1 4-colour index -> ASTC 2-bit weight level (0, 1/3, 2/3, 1 of the way c0 -> c1 mapped to levels 0,21,43,64)

void write_constant_block(uint8_t *dst, int r, int g, int b, int a) {
    dst[0] = 0xFC; dst[1] = 0xFD;
    memset(dst + 2, 0xFF, 6);
    uint16_t c[4] = {(uint16_t)(r * 257), (uint16_t)(g * 257), (uint16_t)(b * 257), (uint16_t)(a * 257)};
    for (int i = 0; i < 4; i++) { dst[8 + 2 * i] = (uint8_t)(c[i] & 255); dst[9 + 2 * i] = (uint8_t)(c[i] >> 8); }
}

// Emits a single-plane RGB block (CEM 8, 8-bit endpoints) given endpoints and 16 levels (0..3).
void write_single_plane(uint8_t *dst, const int e0[3], const int e1[3], const uint8_t lvl[16]) {
    memset(dst, 0, 16);
    BitWriter bw{dst};
    bw.put(0x42, 11, 0);     // block mode: 4x4 grid, 2-bit weights, single plane
    bw.put(0, 2, 11);        // one partition
    bw.put(8, 4, 13);        // CEM 8: LDR RGB direct
    const int v[6] = {e0[0], e1[0], e0[1], e1[1], e0[2], e1[2]};
    for (int i = 0; i < 6; i++) bw.put((uint32_t)v[i], 8, 17 + 8 * i);
    for (int i = 0; i < 16; i++) bw.put_rev(lvl[i], 2, 2 * i);
}

// Emits a dual-plane RGBA block: endpoints are final 0..255 values, `lc` colour levels (plane 1) and `la` alpha levels (plane 2).
// The 48-level quantisation, the "no blue contraction" ordering (sum(e0.rgb) <= sum(e1.rgb)) and the weight inversion on a swap are done here.
void write_dual_plane(uint8_t *dst, const int e0in[4], const int e1in[4], const uint8_t lc[16], const uint8_t la[16]) {
    int q0[4], q1[4], u0[4], u1[4];
    for (int c = 0; c < 4; c++) {
        q0[c] = q48_index_for(e0in[c]); q1[c] = q48_index_for(e1in[c]);
        u0[c] = kAstcQ48ToU8[q0[c]]; u1[c] = kAstcQ48ToU8[q1[c]];
    }
    bool swap = (u0[0] + u0[1] + u0[2]) > (u1[0] + u1[1] + u1[2]);
    if (swap) { for (int c = 0; c < 4; c++) { std::swap(q0[c], q1[c]); } }
    memset(dst, 0, 16);
    BitWriter bw{dst};
    bw.put(0x442, 11, 0);    // 4x4 grid, 2-bit weights, dual plane
    bw.put(0, 2, 11);
    bw.put(12, 4, 13);       // CEM 12: LDR RGBA direct
    uint8_t vals[8] = {(uint8_t)q0[0], (uint8_t)q1[0], (uint8_t)q0[1], (uint8_t)q1[1], (uint8_t)q0[2], (uint8_t)q1[2], (uint8_t)q0[3], (uint8_t)q1[3]};
    int end = encode_ise_q48(bw, vals, 8, 17);
    (void)end;   // == 62: 45 bits of endpoints, then the 2-bit colour component selector
    bw.put(3, 2, 62);        // plane 2 carries the alpha channel
    for (int i = 0; i < 16; i++) {
        uint32_t a = lc[i], b = la[i];
        if (swap) { a = 3 - a; b = 3 - b; }
        bw.put_rev(a, 2, 4 * i);
        bw.put_rev(b, 2, 4 * i + 2);
    }
}

int level_for_fraction(int v, int lo, int hi) {   // nearest of 4 levels at 0, 21/64, 43/64, 1 between lo..hi
    if (hi <= lo) return 0;
    int num = (v - lo) * 64;
    int w = (num + (hi - lo) / 2) / (hi - lo);   // 0..64
    static const int lv[4] = {0, 21, 43, 64};
    int best = 0, bd = 1 << 30;
    for (int k = 0; k < 4; k++) { int d = abs(w - lv[k]); if (d < bd) { bd = d; best = k; } }
    return best;
}

}  // namespace

void bc_to_astc_level(Bc f, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, AstcStats *stats) {
    const uint32_t bw = bc_blocks(w), bh = bc_blocks(h), bb = bc_block_bytes(f);
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *s = src + ((size_t)by * bw + bx) * bb;
            uint8_t *d = dst + ((size_t)by * bw + bx) * 16;
            if (stats) stats->blocks++;
            if (f == Bc::ATI1 || f == Bc::ATI2) { memset(d, 0, 16); continue; }   // not transcoded (callers decode to R8/RG8)
            const uint8_t *cblk = f == Bc::DXT1 ? s : s + 8;
            Bc1Info bi;
            bc1_info(cblk, &bi);
            const bool four = f != Bc::DXT1 || bi.four_color;
            int pal[4][4];
            bc1_palette(bi, four, pal);
            // per-texel colour index and alpha
            uint8_t cidx[16];
            int alpha[16];
            bool has_alpha_var = false;
            uint8_t a8[16];
            if (f == Bc::DXT5) decode_bc4_channel(s, a8);
            for (int i = 0; i < 16; i++) {
                cidx[i] = (uint8_t)((bi.idx >> (2 * i)) & 3);
                if (f == Bc::DXT1) alpha[i] = pal[cidx[i]][3];
                else if (f == Bc::DXT3) alpha[i] = ((s[i >> 1] >> ((i & 1) * 4)) & 15) * 17;
                else alpha[i] = a8[i];
                if (alpha[i] != alpha[0]) has_alpha_var = true;
            }
            const bool opaque_const = !has_alpha_var && alpha[0] == 255;
            // constant colour block?
            bool same_col = bi.c0[0] == bi.c1[0] && bi.c0[1] == bi.c1[1] && bi.c0[2] == bi.c1[2];
            bool all_same_idx = true;
            for (int i = 1; i < 16; i++) if (cidx[i] != cidx[0]) all_same_idx = false;
            if ((same_col || all_same_idx) && !has_alpha_var) {
                int k = same_col ? 0 : cidx[0];
                write_constant_block(d, pal[k][0], pal[k][1], pal[k][2], alpha[0]);
                if (stats) stats->constant++;
                continue;
            }
            if (opaque_const) {
                // single plane, exact endpoints
                uint8_t lvl[16];
                for (int i = 0; i < 16; i++) {
                    if (four) lvl[i] = kBcToAstcLevel[cidx[i]];
                    else lvl[i] = cidx[i] == 0 ? 0 : cidx[i] == 1 ? 3 : 2;   // 3-colour mode (idx 3 cannot occur: alpha would vary)
                }
                int e0[3] = {bi.c0[0], bi.c0[1], bi.c0[2]}, e1[3] = {bi.c1[0], bi.c1[1], bi.c1[2]};
                if (e0[0] + e0[1] + e0[2] > e1[0] + e1[1] + e1[2]) {   // avoid the blue-contraction branch of the CEM 8 decoder
                    for (int c = 0; c < 3; c++) std::swap(e0[c], e1[c]);
                    for (int i = 0; i < 16; i++) lvl[i] = (uint8_t)(3 - lvl[i]);
                }
                write_single_plane(d, e0, e1, lvl);
                continue;
            }
            // dual plane: RGB from the colour block, alpha from min/max of the decoded alpha
            int amin = 255, amax = 0;
            for (int i = 0; i < 16; i++) { amin = std::min(amin, alpha[i]); amax = std::max(amax, alpha[i]); }
            uint8_t lc[16], la[16];
            for (int i = 0; i < 16; i++) {
                int k = cidx[i];
                if (four) lc[i] = kBcToAstcLevel[k];
                else lc[i] = k == 0 ? 0 : k == 1 ? 3 : k == 2 ? 2 : 0;
                la[i] = (uint8_t)level_for_fraction(alpha[i], amin, amax);
            }
            int e0[4] = {bi.c0[0], bi.c0[1], bi.c0[2], amin}, e1[4] = {bi.c1[0], bi.c1[1], bi.c1[2], amax};
            write_dual_plane(d, e0, e1, lc, la);
            if (stats) stats->dual_plane++;
        }
    }
}

// ---- reference decoder for the emitted subset ---------------------------------------------------------------------------------

bool astc_decode_block_subset(const uint8_t blk[16], uint8_t out[64]) {
    auto getbits = [&](int pos, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) { int p = pos + i; v |= (uint32_t)((blk[p >> 3] >> (p & 7)) & 1) << i; } return v; };
    uint32_t bm = getbits(0, 11);
    if ((bm & 0x1FF) == 0x1FC) {
        if (((bm >> 9) & 1) != 0) return false;
        for (int i = 0; i < 16; i++)
            for (int c = 0; c < 4; c++) { uint32_t v = blk[8 + 2 * c] | (blk[9 + 2 * c] << 8); out[i * 4 + c] = (uint8_t)((v * 255 + 32767) / 65535); }
        return true;
    }
    bool dual;
    if (bm == 0x42) dual = false; else if (bm == 0x442) dual = true; else return false;
    if (getbits(11, 2) != 0) return false;
    uint32_t cem = getbits(13, 4);
    int e[2][4];
    if (!dual) {
        if (cem != 8) return false;
        for (int c = 0; c < 3; c++) { e[0][c] = (int)getbits(17 + 16 * c, 8); e[1][c] = (int)getbits(17 + 16 * c + 8, 8); }
        e[0][3] = e[1][3] = 255;
        if (e[0][0] + e[0][1] + e[0][2] > e[1][0] + e[1][1] + e[1][2]) return false;   // would need blue contraction: we never emit it
    } else {
        if (cem != 12) return false;
        // trit-packed 8 values
        uint8_t vals[8];
        int pos = 17;
        int i = 0;
        // inverse trit table
        static int8_t inv[256][5];
        static bool init = false;
        if (!init) {
            memset(inv, -1, sizeof inv);
            for (int t4 = 0; t4 < 3; t4++) for (int t3 = 0; t3 < 3; t3++) for (int t2 = 0; t2 < 3; t2++) for (int t1 = 0; t1 < 3; t1++) for (int t0 = 0; t0 < 3; t0++) {
                int T = kAstcTritPack[t0 + 3 * t1 + 9 * t2 + 27 * t3 + 81 * t4];
                inv[T][0] = (int8_t)t0; inv[T][1] = (int8_t)t1; inv[T][2] = (int8_t)t2; inv[T][3] = (int8_t)t3; inv[T][4] = (int8_t)t4;
            }
            init = true;
        }
        while (i < 8) {
            int cnt = std::min(5, 8 - i);
            static const int tbits[5] = {2, 2, 1, 2, 1}, tshift[5] = {0, 2, 4, 5, 7};
            uint32_t T = 0;
            uint32_t low[5];
            for (int k = 0; k < cnt; k++) {
                low[k] = getbits(pos, 4);
                T |= getbits(pos + 4, tbits[k]) << tshift[k];
                pos += 4 + tbits[k];
            }
            if (inv[T & 255][0] < 0) return false;
            for (int k = 0; k < cnt; k++) vals[i + k] = (uint8_t)(inv[T & 255][k] * 16 + (int)low[k]);
            i += cnt;
        }
        for (int c = 0; c < 4; c++) { e[0][c] = kAstcQ48ToU8[vals[2 * c]]; e[1][c] = kAstcQ48ToU8[vals[2 * c + 1]]; }
        if (e[0][0] + e[0][1] + e[0][2] > e[1][0] + e[1][1] + e[1][2]) return false;
        if (getbits(62, 2) != 3) return false;
    }
    static const int unq[4] = {0, 21, 43, 64};
    auto weight = [&](int plane, int i) -> int {
        int base = dual ? (4 * i + 2 * plane) : 2 * i;
        uint32_t v = 0;
        for (int b = 0; b < 2; b++) { int p = 127 - (base + b); v |= (uint32_t)((blk[p >> 3] >> (p & 7)) & 1) << b; }
        return unq[v];
    };
    for (int i = 0; i < 16; i++) {
        int w0 = weight(0, i), w1 = dual ? weight(1, i) : w0;
        for (int c = 0; c < 4; c++) {
            int w = c == 3 ? w1 : w0;
            uint32_t c0 = (uint32_t)e[0][c] * 257, c1 = (uint32_t)e[1][c] * 257;
            uint32_t v = (c0 * (64 - w) + c1 * w + 32) >> 6;
            out[i * 4 + c] = (uint8_t)((v * 255 + 32767) / 65535);
        }
    }
    return true;
}

// ---- packed 16-bit expanders --------------------------------------------------------------------------------------------------

void expand_a4r4g4b4_to_bgra8(const uint16_t *src, uint8_t *dst, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t v = src[i];
        dst[4 * i + 0] = (uint8_t)((v & 15) * 17);
        dst[4 * i + 1] = (uint8_t)(((v >> 4) & 15) * 17);
        dst[4 * i + 2] = (uint8_t)(((v >> 8) & 15) * 17);
        dst[4 * i + 3] = (uint8_t)(((v >> 12) & 15) * 17);
    }
}
void expand_x4r4g4b4_to_bgra8(const uint16_t *src, uint8_t *dst, size_t n) {
    expand_a4r4g4b4_to_bgra8(src, dst, n);
    for (size_t i = 0; i < n; i++) dst[4 * i + 3] = 255;
}
void expand_a8r3g3b2_to_bgra8(const uint16_t *src, uint8_t *dst, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t v = src[i];
        uint32_t b2 = v & 3, g3 = (v >> 2) & 7, r3 = (v >> 5) & 7;
        dst[4 * i + 0] = (uint8_t)(b2 * 85);
        dst[4 * i + 1] = (uint8_t)((g3 * 255 + 3) / 7);
        dst[4 * i + 2] = (uint8_t)((r3 * 255 + 3) / 7);
        dst[4 * i + 3] = (uint8_t)(v >> 8);
    }
}
void expand_r3g3b2_to_bgra8(const uint8_t *src, uint8_t *dst, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t v = src[i];
        dst[4 * i + 0] = (uint8_t)((v & 3) * 85);
        dst[4 * i + 1] = (uint8_t)((((v >> 2) & 7) * 255 + 3) / 7);
        dst[4 * i + 2] = (uint8_t)((((v >> 5) & 7) * 255 + 3) / 7);
        dst[4 * i + 3] = 255;
    }
}
void expand_a8p8_or_p8(const uint8_t *, uint8_t *dst, size_t n) { memset(dst, 255, n * 4); }
void expand_l16_to_r16(const uint16_t *src, uint16_t *dst, size_t n) { memcpy(dst, src, n * 2); }

}  // namespace tex
}  // namespace dish
