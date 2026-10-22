// Whole-process sampling profiler (runtime/core/profiler.cpp): DISH_PROFILE=<start s>:<duration s>[:<interval ms>].
#pragma once
#include <string>

namespace dish {
// Starts the sampler thread when DISH_PROFILE is set; the report goes to <out_dir>/profile.txt (or DISH_PROFILE_OUT).
void profiler_start(const std::string &out_dir);
}  // namespace dish
