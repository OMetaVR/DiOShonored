// Shader preparation (preload of recorded shaders and pipelines) for hosts: plain C++, implemented by the Metal backend
// (platform/d3d9_metal/mtl_shadercache.mm). Non-Apple builds get no-op versions.
#pragma once
#include <atomic>
#include <cstdint>

namespace dish {

struct ShaderPrepStatus {
    std::atomic<uint32_t> shaders_total{0}, shaders_done{0}, pipelines_total{0}, pipelines_done{0}, failures{0};
    std::atomic<bool> finished{false};
    std::atomic<bool> cancel{false};        // stop after the work in flight
    std::atomic<bool> background{false};    // lower priority, do not write the pipeline archive (the game's device owns it)
};
struct ShaderPrepSummary {
    uint32_t shaders = 0, pipelines = 0;    // recorded so far
    bool all_prepared = false;              // the last completed preparation covered all of it
};

ShaderPrepSummary shader_prep_summary();
// Starts preparing on worker threads; `st` must stay alive until st->finished. May be switched to background while running.
void shader_prep_start(ShaderPrepStatus *st);

}  // namespace dish
