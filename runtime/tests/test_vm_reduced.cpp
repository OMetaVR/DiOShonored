// Reduced guest address-space layout (the iOS fallback when a 4 GiB reservation is refused). DISH_GUEST_SPACE_MB
// forces the size, then the usual allocator invariants must hold against the smaller user maximum.
#include "testing.h"
#include "dish/fault.h"
#include <csetjmp>
#include <cstdlib>

using namespace dish;

static sigjmp_buf g_jb;
static bool fault_hook(uint32_t, int, void *) { siglongjmp(g_jb, 1); return true; }
static bool faults_on_read(uint32_t a) {
    fault_set_hook(fault_hook);
    bool faulted = false;
    volatile uint8_t sink;
    if (sigsetjmp(g_jb, 1) == 0) sink = *(volatile uint8_t *)(guest_base + a);
    else faulted = true;
    (void)sink;
    fault_set_hook(nullptr);
    return faulted;
}

TEST(reduced_layout) {
    setenv("DISH_GUEST_SPACE_MB", "3072", 1);
    t::setup_runtime("vmred");
    CHECK_EQ(vm_space_bytes(), 3072ull << 20);
    CHECK_EQ(vm_user_max(), (3072u << 20) - 0x10000u);
    uint32_t err = 0;
    uint32_t a = vm_alloc(0, 0x40000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &err);
    CHECK(a != 0 && a + 0x40000 <= vm_user_max());
    *(volatile uint32_t *)(guest_base + a) = 7;
    uint32_t top = vm_alloc(0, 0x100000, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE, &err);
    CHECK(top != 0 && top + 0x100000 <= vm_user_max());
    CHECK(top > 0x40000000u);                                        // top-down allocation lands near the reduced limit
    CHECK_EQ(vm_alloc(vm_user_max(), 0x1000, MEM_RESERVE, PAGE_READWRITE, &err), 0u);   // top guard is not allocatable
    CHECK(faults_on_read(vm_user_max()));
    CHECK(faults_on_read(0x100));
    VmQuery q;
    CHECK(vm_query(vm_user_max(), &q));
    CHECK_EQ(q.state, (uint32_t)MEM_RESERVE);
    CHECK_EQ(vm_alloc(0x78520000u, 0x10000, MEM_RESERVE, PAGE_READWRITE, &err), 0x78520000u);   // msvcr90 base fits
}
