#include "testing.h"
#include "dish/lifecycle.h"
#include "dish/memacct.h"
#include "dish/vm.h"
#include <atomic>
#include <chrono>
#include <thread>

using namespace dish;

static std::atomic<int> g_drained{0}, g_frames{0};
static std::atomic<bool> g_hook_state{false};

TEST(pause_parks_render_thread_and_resume_releases) {
    t::setup_runtime("life");
    lifecycle_add_hook([](bool p) { g_hook_state = p; });
    std::atomic<bool> stop{false};
    std::thread render([&] {
        while (!stop) {
            g_frames++;
            lifecycle_park([](void *) { g_drained++; }, nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    while (g_frames < 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(lifecycle_pause(2000));
    CHECK(lifecycle_paused());
    CHECK(lifecycle_parked());
    CHECK(g_hook_state.load());
    CHECK_EQ(g_drained.load(), 1);
    int f = g_frames;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(g_frames.load(), f);          // no frames while paused
    lifecycle_resume();
    CHECK(!g_hook_state.load());
    while (g_frames <= f + 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(!lifecycle_parked());
    stop = true;
    render.join();
}

TEST(memory_accounting_and_commit_limit) {
    t::setup_runtime("life");
    MemStats a = memacct_query();
    memacct_gpu_add(1 << 20);
    CHECK_EQ(memacct_query().gpu_reported, a.gpu_reported + (1u << 20));
    memacct_gpu_add(-(1 << 20));
    CHECK(a.footprint > 0);
    uint64_t base = vm_committed_bytes();
    vm_set_commit_limit(base + 0x20000);
    uint32_t err = 0;
    uint32_t p = vm_alloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &err);
    CHECK(p != 0);
    CHECK_EQ(vm_alloc(0, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &err), 0u);
    CHECK_EQ(err, (uint32_t)ERR_NOT_ENOUGH_MEMORY);
    vm_set_commit_limit(0);
    CHECK(vm_alloc(0, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &err) != 0);
}
