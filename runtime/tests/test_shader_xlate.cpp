// MojoShader Metal-profile translation of D3D9 bytecode: metadata (constant packing, samplers, interpolants) and a type check of the MSL by a C++
// compiler using tests/msl_shim (no Metal compiler is available on Linux).
#include "testing.h"
#include "../platform/d3d9_metal/shader_xlate.h"
#include "shaders_gen.h"
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/wait.h>

using namespace dish::mtl;

#ifndef DISH_MSL_SHIM_DIR
#define DISH_MSL_SHIM_DIR "msl_shim"
#endif

// Returns 0 when the MSL type-checks, 77 when no C++ compiler is available, otherwise the compiler's exit code (diagnostics go to stderr).
static int check_msl(const std::string &name, const std::string &msl) {
    if (getenv("DISH_TEST_DUMP_MSL")) fprintf(stderr, "==== %s\n%s\n", name.c_str(), msl.c_str());
    std::string dir = t::temp_dir();
    std::string path = dir + "/" + name + ".cpp";
    FILE *f = fopen(path.c_str(), "w");
    if (!f) return 77;
    fputs(msl.c_str(), f);
    fclose(f);
    std::string cmd = "clang++ -std=c++17 -x c++ -fsyntax-only -Wno-unknown-attributes -Wno-unused-value -Wno-unused-variable -Wno-unused-parameter -Wno-ignored-attributes -I" DISH_MSL_SHIM_DIR " " + path + " 2>&1";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return 77;
    char buf[512];
    std::string outp;
    while (fgets(buf, sizeof buf, p)) outp += buf;
    int rc = pclose(p);
    if (WIFEXITED(rc) && WEXITSTATUS(rc) == 127) return 77;
    if (!outp.empty()) fprintf(stderr, "%s\n", outp.c_str());
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
}

static bool xlate(const uint32_t *bc, size_t n, ShaderXlate *x) {
    std::string err;
    bool ok = shader_translate(bc, n, x, &err);
    if (!ok) fprintf(stderr, "  translate failed: %s\n", err.c_str());
    return ok;
}

TEST(token_count) {
    CHECK_EQ(shader_token_count(k_scene_vs, 400), 200u);
    CHECK_EQ(shader_token_count(k_scene_ps, 400), 139u);
    CHECK_EQ(shader_token_count(k_rtt_ps, 400), 159u);
    CHECK_EQ(shader_token_count(k_scene_ps, 100), 0u);   // END token not within the limit
}

TEST(scene_vertex_shader) {
    ShaderXlate x;
    CHECK(xlate(k_scene_vs, 200, &x));
    CHECK(x.is_vertex);
    CHECK_EQ(x.major, 3);
    CHECK(x.entry == "VertexShader");
    CHECK_EQ(x.nf, 5u); CHECK_EQ(x.ni, 0u); CHECK_EQ(x.nb, 0u);
    CHECK_EQ(x.ubuf_size, 80u);
    CHECK_EQ(x.uniforms.size(), 5u);
    for (size_t i = 0; i < x.uniforms.size(); i++) { CHECK_EQ(x.uniforms[i].reg, (uint16_t)i); CHECK_EQ(x.uniforms[i].packed, (uint32_t)i); }
    CHECK_EQ(x.inputs.size(), 3u);
    CHECK_EQ(x.inputs[0].usage, 0); CHECK_EQ(x.inputs[1].usage, 5); CHECK_EQ(x.inputs[2].usage, 10);
    CHECK_EQ(x.outputs.size(), 4u);
    CHECK(x.msl.find("vertex VertexShader_Output VertexShader") != std::string::npos);
    CHECK(x.msl.find("[[position, invariant]]") != std::string::npos);   // depth prepass + EQUAL-depth passes need identical z
    int rc = check_msl("scene_vs", x.msl);
    if (rc == 77) fprintf(stderr, "  (no clang++: MSL type check skipped)\n"); else CHECK_EQ(rc, 0);
}

TEST(scene_pixel_shaders) {
    ShaderXlate x;
    CHECK(xlate(k_scene_ps, 139, &x));
    CHECK(!x.is_vertex);
    CHECK_EQ(x.samplers.size(), 2u);
    CHECK_EQ(x.nf, 2u);
    CHECK(x.ps_writes_color0);
    CHECK(x.msl.find("constant DishPSExtra &dish_extra [[buffer(1)]]") != std::string::npos);
    CHECK(x.msl.find("fragment FragmentShader_Output FragmentShader") != std::string::npos);
    int rc = check_msl("scene_ps", x.msl);
    if (rc != 77) CHECK_EQ(rc, 0);
    ShaderXlate y;
    CHECK(xlate(k_rtt_ps, 159, &y));
    CHECK_EQ(y.samplers.size(), 0u);
    CHECK_EQ(y.nf, 1u);
    rc = check_msl("rtt_ps", y.msl);
    if (rc != 77) CHECK_EQ(rc, 0);
}

#include "shaders_corpus_gen.h"
#include <vector>

// remove the CTAB comment block (the constant table): what a stripped UE3 shader would look like
static std::vector<uint32_t> strip_ctab(const uint32_t *w, size_t n) {
    std::vector<uint32_t> o;
    o.push_back(w[0]);
    for (size_t i = 1; i < n;) {
        uint32_t t = w[i];
        if ((t & 0xFFFF) == 0xFFFE) {
            size_t c = 1 + ((t >> 16) & 0x7FFF);
            if (i + 1 < n && w[i + 1] == 0x42415443) { i += c; continue; }
            for (size_t k = 0; k < c; k++) o.push_back(w[i + k]);
            i += c;
            continue;
        }
        o.push_back(t);
        i++;
    }
    return o;
}

TEST(corpus_translates_and_typechecks) {
    for (const CorpusShader &c : kCorpus) {
        ShaderXlate x;
        std::string err;
        bool ok = shader_translate(c.code, c.words, &x, &err);
        if (!ok) fprintf(stderr, "  %s: %s\n", c.name, err.c_str());
        CHECK(ok);
        if (!ok) continue;
        int rc = check_msl(c.name, x.msl);
        if (rc == 77) continue;
        if (rc != 0) fprintf(stderr, "  ^ %s\n", c.name);
        CHECK_EQ(rc, 0);
    }
}

TEST(skinning_relative_addressing_with_and_without_ctab) {
    const CorpusShader *skin = nullptr;
    for (const CorpusShader &c : kCorpus) if (strcmp(c.name, "skin_vs") == 0) skin = &c;
    CHECK(skin != nullptr);
    if (!skin) return;
    ShaderXlate a;
    CHECK(xlate(skin->code, skin->words, &a));
    CHECK(!a.synthesized_ctab);
    // g_bones[60] at c8: the array is the first thing in the uniform block, the view-projection matrix registers follow
    CHECK_EQ(a.uniforms[0].reg, 8); CHECK_EQ(a.uniforms[0].count, 60); CHECK_EQ(a.uniforms[0].packed, 0u);
    CHECK_EQ(a.nf, 64u);
    std::vector<uint32_t> stripped = strip_ctab(skin->code, skin->words);
    CHECK(stripped.size() < skin->words);
    ShaderXlate b;
    CHECK(xlate(stripped.data(), stripped.size(), &b));
    CHECK(b.synthesized_ctab);
    CHECK_EQ(b.uniforms[0].reg, 0); CHECK_EQ(b.uniforms[0].count, 256);   // the whole c0..c255 range is treated as one array
    int rc = check_msl("skin_stripped", b.msl);
    if (rc != 77) CHECK_EQ(rc, 0);
}

TEST(texldl_keeps_the_lod_and_vertex_shader_exports_other_semantics) {
    for (const CorpusShader &c : kCorpus) {
        if (strcmp(c.name, "vtf_vs") != 0) continue;
        ShaderXlate x;
        CHECK(xlate(c.code, c.words, &x));
        CHECK(x.is_vertex);
        CHECK(x.msl.find("level(") != std::string::npos);     // vertex texture fetch must carry an explicit LOD
        CHECK(x.samplers.size() == 1);
        bool has_tangent_in = false;
        for (auto &a : x.inputs) if (a.usage == 6) has_tangent_in = true;
        CHECK(has_tangent_in);
    }
}
