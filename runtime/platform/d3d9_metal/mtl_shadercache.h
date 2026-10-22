// Persistent shader / pipeline record and the preload ("shader preparation") pass, in the spirit of DXVK's state cache and Fossilize.
//
// While the game runs, every vertex / pixel shader it creates is saved once (D3D9 bytecode, named by content hash) under
// <cache_dir>/shaders/, and every render pipeline the backend builds is appended to <cache_dir>/pipelines.bin with the content
// hashes of its shaders and its vertex declaration. Before the game starts, shader_prep_start() (dish/shader_prep.h) translates and compiles everything
// recorded with exactly the code paths the game later uses: Metal's on-device compiler cache (keyed on the MSL source and the
// compile options) then answers the in-game compiles in a fraction of a millisecond, and in the foreground mode the pipelines are
// also written to the binary archive the device loads. Nothing here is game data shipped with the app: it is derived on the device
// from the user's own copy.
#pragma once
#import <Metal/Metal.h>
#include "mtl_device.h"
#include "shader_xlate.h"
#include "dish/shader_prep.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace dish {
namespace mtl {

uint64_t shader_content_hash(const uint32_t *tokens, size_t n);
std::string pipeline_archive_path();   // the MTLBinaryArchive the device loads and the foreground preparation writes

// Translate D3D9 bytecode to MSL and compile it with the backend's options. Shared by the device and the preload so both produce
// identical sources (the compiler cache hit depends on it).
bool compile_shader(id<MTLDevice> md, const uint32_t *tokens, size_t n, ShaderXlate *x, id<MTLLibrary> __strong *lib,
                    id<MTLFunction> __strong *fn, std::string *err, uint64_t *translate_ns, uint64_t *compile_ns);
// Render pipeline descriptor for a key (PsoKey's vs/ps/decl fields are not read).
MTLRenderPipelineDescriptor *make_pso_desc(const PsoKey &k, id<MTLFunction> vsfn, const std::vector<ShaderAttr> &vs_inputs, id<MTLFunction> psfn,
                                           const std::vector<D3DVERTEXELEMENT9> *elems);

// ---- recording (called by the device; cheap, file writes happen on a background queue) -----------------------------------------
void record_shader(uint64_t hash, bool pixel, const std::vector<uint32_t> &bytecode);
void record_pipeline(const PsoKey &k, uint64_t vs_hash, uint64_t ps_hash, const std::vector<D3DVERTEXELEMENT9> *elems);

}  // namespace mtl
}  // namespace dish
