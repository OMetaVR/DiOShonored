#include "dish/memacct.h"
#include "dish/vm.h"
#include "dish/log.h"
#include <atomic>
#include <cstdio>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <os/proc.h>
#else
#include <unistd.h>
#endif

namespace dish {

static std::atomic<int64_t> g_gpu{0};
static std::atomic<uint64_t> g_budget{0};
static std::atomic<bool> g_warn{false};

void memacct_gpu_add(int64_t d) { g_gpu.fetch_add(d, std::memory_order_relaxed); }

static uint64_t os_footprint() {
#if defined(__APPLE__)
    task_vm_info_data_t vi;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) == KERN_SUCCESS) return vi.phys_footprint;
    return 0;
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long total = 0, res = 0;
    if (fscanf(f, "%lu %lu", &total, &res) != 2) res = 0;
    fclose(f);
    return (uint64_t)res * (uint64_t)sysconf(_SC_PAGESIZE);
#endif
}

MemStats memacct_query() {
    MemStats s{};
    s.guest_committed = vm_committed_bytes();
    int64_t g = g_gpu.load(std::memory_order_relaxed);
    s.gpu_reported = g > 0 ? (uint64_t)g : 0;
    s.footprint = os_footprint();
    s.os_available =
#if defined(__APPLE__)
        (uint64_t)os_proc_available_memory();
#else
        0;
#endif
    s.budget = g_budget.load();
    s.low_memory = g_warn.load();
    return s;
}

void memacct_set_budget(uint64_t footprint_bytes, uint64_t guest_commit_limit) {
    g_budget = footprint_bytes;
    vm_set_commit_limit(guest_commit_limit);
    LOGI(LC_MEM, "memory budget: footprint %llu MiB, guest commit limit %llu MiB", (unsigned long long)(footprint_bytes >> 20),
         (unsigned long long)(guest_commit_limit >> 20));
}

bool memacct_over_budget() {
    uint64_t b = g_budget.load();
    return b && os_footprint() > b;
}

void memacct_note_warning() { g_warn = true; LOGW(LC_MEM, "OS memory warning"); }
void memacct_clear_warning() { g_warn = false; }

}  // namespace dish
