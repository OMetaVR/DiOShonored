// D3D9 shader bytecode (vs/ps 1.x-3.0) -> Metal Shading Language, through MojoShader's Metal profile (third_party/mojoshader, zlib).
// Pure C++: no Metal framework dependency, so the translation and its constant layout are unit-tested on Linux.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dish {
namespace mtl {

enum : uint8_t { SU_FLOAT = 0, SU_INT = 1, SU_BOOL = 2 };
enum : uint8_t { ST_2D = 0, ST_CUBE = 1, ST_VOLUME = 2 };

// One constant range the shader reads. The shader's uniform block ("constant Name_Uniforms &uniforms [[buffer(0)]]") stores all float4 constants
// first (nf entries), then int4 constants (ni), then bools (nb, one byte each); `packed` is the index inside the array of its type.
struct ShaderUniform {
    uint8_t type;       // SU_*
    uint16_t reg;       // first D3D register (c#, i#, b#)
    uint16_t count;     // registers in the range (arrays); 1 for a single register
    uint32_t packed;
};
struct ShaderSampler { uint8_t index; uint8_t type; };   // sampler stage (s#; vertex samplers use 0..3), ST_*
struct ShaderAttr { uint8_t usage; uint8_t index; uint8_t reg; };   // D3DDECLUSAGE numbering; reg = input register number (the MSL attribute(N) of a vertex shader input)

struct ShaderXlate {
    std::string msl;                  // complete MSL source
    std::string entry;                // entry point name
    bool is_vertex = false;
    int major = 0, minor = 0;
    std::vector<ShaderUniform> uniforms;
    uint32_t nf = 0, ni = 0, nb = 0;  // packed element counts
    uint32_t ubuf_size = 0;           // bytes of the uniform block (16-byte multiple); 0 when the shader has no uniforms
    uint32_t off_int = 0, off_bool = 0;   // byte offsets of the int4 / bool arrays inside the block
    std::vector<ShaderSampler> samplers;
    std::vector<ShaderAttr> inputs;   // vertex shader: inputs[i] is stage_in attribute(inputs[i].reg); pixel shader: interpolants it reads ([[user(...)]])
    std::vector<ShaderAttr> outputs;  // vertex shader: interpolants it writes; pixel shader: colour outputs (usage COLOR, index = target)
    bool ps_writes_color0 = false;    // pixel shader contains the alpha-test hook (constant "DishPSExtra" in buffer(1))
    bool synthesized_ctab = false;    // relative addressing without a constant table: a c0..cN array was assumed
    std::string notes;                // translator warnings (informational)
};

// Number of 32-bit tokens of the shader at `tokens` (through the END token), or 0 when no END token is found within `max_tokens`.
size_t shader_token_count(const uint32_t *tokens, size_t max_tokens);

// Translate; returns false and fills *err on failure.
bool shader_translate(const uint32_t *tokens, size_t ntokens, ShaderXlate *out, std::string *err);

// Text of the helper structs/functions every translated pixel shader refers to (also useful to tests).
const char *shader_ps_extra_decl();

}  // namespace mtl
}  // namespace dish
