#include "shader_xlate.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "mojoshader.h"
}

namespace dish {
namespace mtl {

namespace {

const char kPsExtraDecl[] =
    "struct DishPSExtra { float alpha_ref; uint alpha_func; uint alpha_enable; uint pad; };\n"
    "inline bool dish_alpha_pass(float a, float ref, uint func) {\n"
    "    switch (func) {\n"
    "        case 1u: return false;\n"
    "        case 2u: return a < ref;\n"
    "        case 3u: return a == ref;\n"
    "        case 4u: return a <= ref;\n"
    "        case 5u: return a > ref;\n"
    "        case 6u: return a != ref;\n"
    "        case 7u: return a >= ref;\n"
    "        default: return true;\n"
    "    }\n"
    "}\n";

bool is_end_token(uint32_t t) { return t == 0x0000FFFFu; }

}  // namespace

const char *shader_ps_extra_decl() { return kPsExtraDecl; }

size_t shader_token_count(const uint32_t *tokens, size_t max_tokens) {
    if (max_tokens < 2) return 0;
    const uint32_t ver = tokens[0];
    const uint32_t type = ver >> 16;
    if (type != 0xFFFE && type != 0xFFFF) return 0;
    const bool sm2plus = ((ver >> 8) & 0xFF) >= 2;
    size_t i = 1;
    while (i < max_tokens) {
        uint32_t t = tokens[i];
        if ((t & 0xFFFF) == 0xFFFE) {   // comment: length in dwords in bits 16..30
            i += 1 + ((t >> 16) & 0x7FFF);
            continue;
        }
        if (is_end_token(t)) return i + 1;
        if (sm2plus) {
            i += 1 + ((t >> 24) & 0xF);   // instruction length field (excludes the opcode token)
        } else {
            // SM1: no length field. Conservative scan: consume tokens until the next END token that can only be an opcode token.
            i++;
        }
    }
    return 0;
}

namespace {

// Builds a synthetic constant table declaring one float4 array c0..c(count-1): lets MojoShader translate relative constant addressing for
// shaders whose CTAB was stripped. The array is then bound as `count` contiguous registers.
std::vector<uint32_t> with_synthetic_ctab(const uint32_t *tokens, size_t n, uint32_t count) {
    const uint32_t version = tokens[0];
    const bool pixel = (version >> 16) == 0xFFFF;
    char target[8];
    snprintf(target, sizeof target, "%cs_%u_%u", pixel ? 'p' : 'v', (version >> 8) & 0xFF, version & 0xFF);
    std::vector<uint8_t> t(64);
    auto w32 = [&](size_t o, uint32_t v) { memcpy(&t[o], &v, 4); };
    auto w16 = [&](size_t o, uint16_t v) { memcpy(&t[o], &v, 2); };
    const char creator[] = "dish";
    const char cname[] = "dish_c";
    const size_t creator_off = 64, cname_off = creator_off + sizeof creator, target_off = cname_off + sizeof cname;
    // header
    w32(0, 28); w32(4, (uint32_t)creator_off); w32(8, version); w32(12, 1); w32(16, 28); w32(20, 0); w32(24, (uint32_t)target_off);
    // constant info @28 (20 bytes)
    w32(28, (uint32_t)cname_off); w16(32, 2); w16(34, 0); w16(36, (uint16_t)count); w16(38, 0); w32(40, 48); w32(44, 0);
    // type info @48 (16 bytes): class VECTOR(1), type FLOAT(3), rows 1, columns 4, elements count
    w16(48, 1); w16(50, 3); w16(52, 1); w16(54, 4); w16(56, (uint16_t)count); w16(58, 0); w32(60, 0);
    t.insert(t.end(), creator, creator + sizeof creator);
    t.insert(t.end(), cname, cname + sizeof cname);
    t.insert(t.end(), target, target + strlen(target) + 1);
    while (t.size() % 4) t.push_back(0);
    const uint32_t body_dwords = (uint32_t)(t.size() / 4) + 1;   // + the 'CTAB' fourcc
    std::vector<uint32_t> out;
    out.push_back(version);
    out.push_back(0xFFFEu | (body_dwords << 16));
    out.push_back(0x42415443u);
    for (size_t i = 0; i < t.size(); i += 4) { uint32_t v; memcpy(&v, &t[i], 4); out.push_back(v); }
    out.insert(out.end(), tokens + 1, tokens + n);
    return out;
}

// MojoShader's D3D parser insists that `texkill` writes .xyzw and its Metal emitter then tests .xyz < 0, while fxc's clip() produces `texkill r4.x`
// (kill when the masked components are negative). Rewrite every partially masked ps_2_0+ texkill as
//     mov rT.xyzw, rN.<masked components repeated>     texkill rT
// where rT is a temp register the shader does not otherwise use. Returns false when no scratch temp is free.
bool rewrite_texkill(const uint32_t *tokens, size_t n, std::vector<uint32_t> *out, bool *changed) {
    *changed = false;
    const uint32_t ver = tokens[0];
    if ((ver >> 16) != 0xFFFF || ((ver >> 8) & 0xFF) < 2) return true;   // only pixel shaders >= 2.0 have maskable texkill
    // pass 1: find texkills with partial masks and the highest temp register used
    int max_temp = -1;
    bool any = false;
    for (size_t i = 1; i < n;) {
        uint32_t t = tokens[i];
        if ((t & 0xFFFF) == 0xFFFE) { i += 1 + ((t >> 16) & 0x7FFF); continue; }
        if (t == 0xFFFF) break;
        uint32_t op = t & 0xFFFF, len = (t >> 24) & 0xF;
        if (op == 0x41 && len == 1 && i + 1 < n) {
            uint32_t d = tokens[i + 1];
            uint32_t type = ((d >> 28) & 7) | ((d >> 8) & 0x18);
            uint32_t mask = (d >> 16) & 0xF;
            if (type == 0 && mask != 0xF) any = true;
        }
        if (op != 0x51 /*def*/ && op != 0x30 /*defi*/ && op != 0x53 /*defb*/) {
            for (uint32_t k = 1; k <= len && i + k < n; k++) {
                uint32_t p = tokens[i + k];
                if (!(p & 0x80000000u)) continue;
                uint32_t type = ((p >> 28) & 7) | ((p >> 8) & 0x18);
                if (type == 0) max_temp = std::max<int>(max_temp, (int)(p & 0x7FF));
            }
        }
        i += 1 + len;
    }
    if (!any) return true;
    if (max_temp >= 31) return false;
    const uint32_t scratch = (uint32_t)(max_temp + 1);
    out->reserve(n + 8);
    out->push_back(tokens[0]);
    for (size_t i = 1; i < n;) {
        uint32_t t = tokens[i];
        if ((t & 0xFFFF) == 0xFFFE) {
            size_t cnt = 1 + ((t >> 16) & 0x7FFF);
            for (size_t k = 0; k < cnt && i + k < n; k++) out->push_back(tokens[i + k]);
            i += cnt;
            continue;
        }
        if (t == 0xFFFF) { out->push_back(t); i++; break; }
        uint32_t op = t & 0xFFFF, len = (t >> 24) & 0xF;
        bool rewritten = false;
        if (op == 0x41 && len == 1 && i + 1 < n) {
            uint32_t d = tokens[i + 1];
            uint32_t type = ((d >> 28) & 7) | ((d >> 8) & 0x18);
            uint32_t mask = (d >> 16) & 0xF;
            if (type == 0 && mask != 0xF && mask != 0) {
                int comps[4], nc = 0;
                for (int c = 0; c < 4; c++) if (mask & (1u << c)) comps[nc++] = c;
                uint32_t swz = 0;
                for (int c = 0; c < 4; c++) swz |= (uint32_t)comps[c < nc ? c : nc - 1] << (2 * c);
                out->push_back(0x00000001u | (2u << 24));                       // mov
                out->push_back(0x800F0000u | scratch);                          // dest rT.xyzw
                out->push_back(0x80000000u | (d & 0x7FF) | (swz << 16));         // src rN.swizzle
                out->push_back(0x00000041u | (1u << 24));                       // texkill
                out->push_back(0x800F0000u | scratch);
                rewritten = true;
                *changed = true;
            }
        }
        if (!rewritten) for (uint32_t k = 0; k <= len && i + k < n; k++) out->push_back(tokens[i + k]);
        i += 1 + len;
    }
    return true;
}

void fix_msl(std::string &s, bool pixel, ShaderXlate *x) {
    // 1. one include instead of the per-feature headers
    std::string out = "#include <metal_stdlib>\n";
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(pos, e - pos);
        if (line.rfind("#include <metal_", 0) != 0) { out += line; if (e < s.size()) out += '\n'; }
        pos = e + 1;
    }
    s.swap(out);
    // 2. helper declarations after `using namespace metal;`
    if (pixel) {
        size_t u = s.find("using namespace metal;");
        if (u != std::string::npos) {
            u = s.find('\n', u);
            s.insert(u == std::string::npos ? s.size() : u + 1, std::string("\n") + kPsExtraDecl + "\n");
        }
    }
    // 2b. the position output is invariant: UE3 draws a depth prepass and then adds lights / decals with an EQUAL depth test, each
    // with its own vertex shader. Without invariance (MTLCompileOptions.preserveInvariance is set) fast-math may contract and
    // reorder the scalarised dp4s differently per shader, and the later passes fail the depth test in blocky patches.
    if (!pixel) {
        size_t q = s.find("[[position]]");
        if (q != std::string::npos) s.replace(q, 12, "[[position, invariant]]");
    }
    // 3. entry signature: drop the dangling comma MojoShader leaves after the last sampler, append the alpha-test constants
    size_t close = s.find("\n) {");
    if (close != std::string::npos) {
        size_t p = close;
        while (p > 0 && isspace((unsigned char)s[p - 1])) p--;
        if (p > 0 && s[p - 1] == ',') { s.erase(p - 1, 1); p--; }
        if (pixel && s.find("oC0 [[color(0)]]") != std::string::npos) {
            std::string add = (p > 0 && s[p - 1] == '(' ? "" : ",") + std::string("\n\tconstant DishPSExtra &dish_extra [[buffer(1)]]");
            s.insert(p, add);
            // 4. alpha test hook before the final return
            size_t r = s.rfind("return output;");
            if (r != std::string::npos) {
                s.insert(r, "if (dish_extra.alpha_enable != 0u && !dish_alpha_pass(output.oC0.a, dish_extra.alpha_ref, dish_extra.alpha_func)) discard_fragment();\n\t");
                x->ps_writes_color0 = true;
            }
        }
    }
}

}  // namespace

bool shader_translate(const uint32_t *tokens, size_t ntokens, ShaderXlate *x, std::string *err) {
    if (ntokens < 2) { if (err) *err = "empty shader"; return false; }
    const bool pixel = (tokens[0] >> 16) == 0xFFFF;
    const char *mainfn = pixel ? "FragmentShader" : "VertexShader";
    std::vector<uint32_t> patched, killfix;
    const uint32_t *src = tokens;
    size_t n = ntokens;
    {
        bool changed = false;
        if (!rewrite_texkill(tokens, ntokens, &killfix, &changed)) { if (err) *err = "texkill with a partial write mask and no free temp register"; return false; }
        if (changed) { src = killfix.data(); n = killfix.size(); x->notes += "partial-mask texkill rewritten; "; }
    }
    const MOJOSHADER_parseData *pd = MOJOSHADER_parse("metal", mainfn, (const unsigned char *)src, (unsigned)(n * 4), nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr);
    if (pd->error_count > 0) {
        bool need_ctab = false;
        for (int i = 0; i < pd->error_count; i++) if (pd->errors[i].error && strstr(pd->errors[i].error, "without a CTAB")) need_ctab = true;
        if (need_ctab) {
            MOJOSHADER_freeParseData(pd);
            patched = with_synthetic_ctab(src, n, pixel ? 224 : 256);
            src = patched.data();
            n = patched.size();
            pd = MOJOSHADER_parse("metal", mainfn, (const unsigned char *)src, (unsigned)(n * 4), nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr);
            x->synthesized_ctab = true;
        }
    }
    if (pd->error_count > 0 || !pd->output) {
        if (err) {
            *err = "MojoShader: ";
            for (int i = 0; i < pd->error_count; i++) { *err += pd->errors[i].error ? pd->errors[i].error : "?"; *err += "; "; }
        }
        MOJOSHADER_freeParseData(pd);
        return false;
    }
    x->is_vertex = pd->shader_type == MOJOSHADER_TYPE_VERTEX;
    x->major = pd->major_ver;
    x->minor = pd->minor_ver;
    x->entry = pd->mainfn ? pd->mainfn : mainfn;
    // constants: arrays first (they are FLOAT, array_count > 0), then single registers, in list order: packed index = running count per type
    uint32_t nf = 0, ni = 0, nb = 0;
    for (int i = 0; i < pd->uniform_count; i++) {
        const MOJOSHADER_uniform &u = pd->uniforms[i];
        ShaderUniform su;
        su.reg = (uint16_t)u.index;
        su.count = (uint16_t)(u.array_count > 0 ? u.array_count : 1);
        if (u.type == MOJOSHADER_UNIFORM_FLOAT) {
            su.type = SU_FLOAT;
            if (u.constant) continue;   // shader-embedded constant array: nothing to upload
            su.packed = nf; nf += su.count;
        } else if (u.type == MOJOSHADER_UNIFORM_INT) {
            su.type = SU_INT; su.packed = ni; ni += su.count;
        } else if (u.type == MOJOSHADER_UNIFORM_BOOL) {
            su.type = SU_BOOL; su.packed = nb; nb += su.count;
        } else continue;
        x->uniforms.push_back(su);
    }
    x->nf = nf; x->ni = ni; x->nb = nb;
    x->off_int = nf * 16;
    x->off_bool = x->off_int + ni * 16;
    uint32_t sz = x->off_bool + nb;
    x->ubuf_size = (nf + ni + nb) ? ((sz + 15) & ~15u) : 0;
    for (int i = 0; i < pd->sampler_count; i++) {
        ShaderSampler s;
        s.index = (uint8_t)pd->samplers[i].index;
        s.type = pd->samplers[i].type == MOJOSHADER_SAMPLER_CUBE ? ST_CUBE : pd->samplers[i].type == MOJOSHADER_SAMPLER_VOLUME ? ST_VOLUME : ST_2D;
        x->samplers.push_back(s);
    }
    auto regnum = [](const char *name) { return (uint8_t)(name && name[0] && isdigit((unsigned char)name[1]) ? atoi(name + 1) : 0); };
    for (int i = 0; i < pd->attribute_count; i++) x->inputs.push_back({(uint8_t)pd->attributes[i].usage, (uint8_t)pd->attributes[i].index, regnum(pd->attributes[i].name)});
    for (int i = 0; i < pd->output_count; i++) x->outputs.push_back({(uint8_t)pd->outputs[i].usage, (uint8_t)pd->outputs[i].index, regnum(pd->outputs[i].name)});
    x->msl = pd->output;
    MOJOSHADER_freeParseData(pd);
    fix_msl(x->msl, pixel, x);
    return true;
}

}  // namespace mtl
}  // namespace dish
