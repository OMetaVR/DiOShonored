// Runtime-wide memory accounting. iOS kills the process (jetsam) at roughly 2.1 GB total footprint on a 4 GB phone, and
// that total includes GPU memory (Metal allocations made by MoltenVK/DXVK), so the budget has to be tracked across guest
// commits, GPU allocations and the host heap. Other layers (the d3d9 layer, a future streaming policy) query this.
#pragma once
#include <cstdint>

namespace dish {

struct MemStats {
    uint64_t guest_committed;   // committed guest address space (vm)
    uint64_t gpu_reported;      // bytes of GPU resources reported through memacct_gpu_add (0 until a layer reports)
    uint64_t footprint;         // the OS's view of this process: phys_footprint (Apple) / resident set (Linux)
    uint64_t os_available;      // os_proc_available_memory() on iOS, 0 elsewhere
    uint64_t budget;            // configured ceiling for `footprint` (0 = none)
    bool     low_memory;        // an OS memory warning arrived since the last memacct_clear_warning()
};

MemStats memacct_query();
// Resources whose size the runtime knows but the OS view lags (textures, buffers, render targets).
void memacct_gpu_add(int64_t delta_bytes);
// Ceiling used by memacct_over_budget; defaults to 1.8 GiB on iOS, none elsewhere. Also installs the guest commit limit
// (budget minus `reserve_for_host`) so a runaway guest sees ERROR_NOT_ENOUGH_MEMORY instead of being jetsammed.
void memacct_set_budget(uint64_t footprint_bytes, uint64_t guest_commit_limit = 0);
bool memacct_over_budget();
// Memory-warning plumbing (SDL_EVENT_LOW_MEMORY is routed here by platform_route_event).
void memacct_note_warning();
void memacct_clear_warning();

}  // namespace dish
