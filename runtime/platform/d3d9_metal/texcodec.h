// Portable texel codecs for the Metal d3d9 backend: BC/DXT decode to uncompressed formats, DXT -> ASTC 4x4 transcode and a few packed
// 16-bit format expanders. Pure C++ (no Metal, no Objective-C) so it is unit-tested on Linux (tests/test_texcodec.cpp).
#pragma once
#include <cstddef>
#include <cstdint>

namespace dish {
namespace tex {

enum class Bc : uint8_t { DXT1, DXT3, DXT5, ATI1, ATI2 };

inline uint32_t bc_block_bytes(Bc f) { return (f == Bc::DXT1 || f == Bc::ATI1) ? 8 : 16; }
// Size in bytes of one (width x height) level of a block compressed format, and its row pitch (bytes per block row).
inline uint32_t bc_blocks(uint32_t v) { return v < 4 ? 1 : (v + 3) / 4; }
inline uint32_t bc_pitch(Bc f, uint32_t w) { return bc_blocks(w) * bc_block_bytes(f); }
inline uint32_t bc_level_bytes(Bc f, uint32_t w, uint32_t h) { return bc_pitch(f, w) * bc_blocks(h); }

// Decode one 4x4 block to RGBA8 (16 texels, 4 bytes each, row major). ATI1 writes the value in R (G=B=0, A=255), ATI2 in R and G.
void bc_decode_block(Bc f, const uint8_t *src, uint8_t out[64]);
// Decode a whole level of w x h texels. Output is RGBA8 for DXT1/3/5 (dst_pitch >= 4*w), R8 for ATI1 (>= w), RG8 for ATI2 (>= 2*w).
// `src` has row pitch bc_pitch(f, w). Texels outside w x h (partial edge blocks) are dropped.
void bc_decode_level(Bc f, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, size_t dst_pitch);
// Bytes per texel of the decoded output.
inline uint32_t bc_decoded_bpp(Bc f) { return f == Bc::ATI1 ? 1 : f == Bc::ATI2 ? 2 : 4; }

// ---- ASTC 4x4 LDR transcode --------------------------------------------------------------------------------------------------
// Every output block is 16 bytes. DXT1 blocks without punch-through alpha become a single-plane block with 2-bit weights (exact up to
// the 4-level weight grid); blocks that need alpha (DXT1 3-colour mode with transparent texels, DXT3, DXT5) become dual-plane blocks
// (RGB weights in plane 1, alpha weights in plane 2, both 2-bit, endpoints in the 48-level trit range). ATI1/ATI2 are not transcoded
// (callers decode them to R8/RG8).
struct AstcStats { uint32_t blocks = 0, dual_plane = 0, constant = 0; };
void bc_to_astc_level(Bc f, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, AstcStats *stats = nullptr);
inline uint32_t astc_level_bytes(uint32_t w, uint32_t h) { return bc_blocks(w) * bc_blocks(h) * 16; }
// Reference decoder for exactly the block types bc_to_astc_level emits (void-extent, single plane / dual plane 4x4 with 2-bit weights).
// Used by tests to measure transcode error; returns false for any other ASTC block.
bool astc_decode_block_subset(const uint8_t blk[16], uint8_t out_rgba[64]);

// ---- small format expanders (D3D layout in -> RGBA8/BGRA8 out), `n` texels ---------------------------------------------------------
void expand_a4r4g4b4_to_bgra8(const uint16_t *src, uint8_t *dst, size_t n);   // D3DFMT_A4R4G4B4
void expand_x4r4g4b4_to_bgra8(const uint16_t *src, uint8_t *dst, size_t n);
void expand_a8r3g3b2_to_bgra8(const uint16_t *src, uint8_t *dst, size_t n);
void expand_r3g3b2_to_bgra8(const uint8_t *src, uint8_t *dst, size_t n);
void expand_a8p8_or_p8(const uint8_t *src, uint8_t *dst, size_t n);   // palettised formats are not supported: opaque white
void expand_l16_to_r16(const uint16_t *src, uint16_t *dst, size_t n);

}  // namespace tex
}  // namespace dish
