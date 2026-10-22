#include "testing.h"
#include "dish/fault.h"
#include <csetjmp>
#include <cstring>

using namespace dish;

static sigjmp_buf g_jb;
static bool fault_hook(uint32_t addr, int, void *) { siglongjmp(g_jb, 1); (void)addr; return true; }

// Probe a guest address: returns true if the access faulted.
static bool faults_on_read(uint32_t a) {
    fault_set_hook(fault_hook);
    volatile uint8_t sink;
    bool faulted = false;
    if (sigsetjmp(g_jb, 1) == 0) sink = *(volatile uint8_t *)(guest_base + a);
    else faulted = true;
    (void)sink;
    fault_set_hook(nullptr);
    return faulted;
}
static bool faults_on_write(uint32_t a) {
    fault_set_hook(fault_hook);
    bool faulted = false;
    if (sigsetjmp(g_jb, 1) == 0) *(volatile uint8_t *)(guest_base + a) = 1;
    else faulted = true;
    fault_set_hook(nullptr);
    return faulted;
}

TEST(reserve_commit_basic) {
    t::setup_runtime("mem");
    uint32_t err = 0;
    uint32_t base = vm_alloc(0, 0x30000, MEM_RESERVE, PAGE_NOACCESS, &err);
    CHECK(base != 0);
    CHECK_EQ(base & 0xFFFF, 0u);                       // 64 KiB allocation granularity
    CHECK(faults_on_read(base));                       // reserved but not committed
    CHECK_EQ(vm_alloc(base + 0x1000, 0x2000, MEM_COMMIT, PAGE_READWRITE, &err), base + 0x1000);
    CHECK(!faults_on_write(base + 0x1000));
    CHECK(!faults_on_write(base + 0x2FFF));
    CHECK(faults_on_read(base + 0x3000));              // next page still reserved only
    CHECK(faults_on_read(base));
    *(uint32_t *)(guest_base + base + 0x1000) = 0xDEADBEEF;
    // decommit returns zeroed pages on the next commit
    CHECK(vm_free(base + 0x1000, 0x2000, MEM_DECOMMIT));
    CHECK(faults_on_read(base + 0x1000));
    CHECK_EQ(vm_alloc(base + 0x1000, 0x1000, MEM_COMMIT, PAGE_READWRITE, &err), base + 0x1000);
    CHECK_EQ(*(uint32_t *)(guest_base + base + 0x1000), 0u);
    CHECK(vm_free(base, 0, MEM_RELEASE));
    CHECK(faults_on_read(base + 0x1000));
}

TEST(query_semantics) {
    t::setup_runtime("mem");
    uint32_t base = vm_alloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(base != 0);
    VmQuery q;
    CHECK(vm_query(base + 0x10, &q));
    CHECK_EQ(q.base, base);
    CHECK_EQ(q.alloc_base, base);
    CHECK_EQ(q.state, (uint32_t)MEM_COMMIT);
    CHECK_EQ(q.prot, (uint32_t)PAGE_READWRITE);
    CHECK_EQ(q.size, 0x10000u);
    CHECK_EQ(q.type, (uint32_t)MEM_PRIVATE);
    uint32_t old = 0;
    CHECK(vm_protect(base + 0x2000, 0x1000, PAGE_READONLY, &old));
    CHECK_EQ(old, (uint32_t)PAGE_READWRITE);
    CHECK(vm_query(base, &q));
    CHECK_EQ(q.size, 0x2000u);                          // run ends where protection changes
    CHECK(vm_query(base + 0x2000, &q));
    CHECK_EQ(q.prot, (uint32_t)PAGE_READONLY);
    CHECK_EQ(q.size, 0x1000u);
    CHECK(faults_on_write(base + 0x2000));
    CHECK(!faults_on_read(base + 0x2000));
    CHECK(vm_free(base, 0, MEM_RELEASE));
    CHECK(vm_query(base, &q));
    CHECK_EQ(q.state, (uint32_t)MEM_FREE);
    // a reserved-only region
    uint32_t r = vm_alloc(0, 0x8000, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(vm_query(r, &q));
    CHECK_EQ(q.state, (uint32_t)MEM_RESERVE);
    CHECK_EQ(q.size, 0x8000u);
    vm_free(r, 0, MEM_RELEASE);
}

TEST(fixed_regions_stay_free) {
    t::setup_runtime("mem");
    uint32_t err = 0;
    CHECK_EQ(vm_alloc(0x1000, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &err), 0u);   // null guard
    CHECK_EQ(vm_alloc(0xFFF00000u, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &err), 0u);   // thunk range
    CHECK_EQ(vm_alloc(0xFFFFF000u, 0x1000, MEM_RESERVE, PAGE_READWRITE, &err), 0u);   // sentinel page
    CHECK(faults_on_read(0));
    CHECK(faults_on_read(0xFFFF));
    CHECK(faults_on_read(kReturnSentinel));
    CHECK(faults_on_read(GUEST_THUNK_BASE));
    CHECK(!vm_free(0, 0, MEM_RELEASE));
    // many allocations never land inside the fixed ranges
    std::vector<uint32_t> v;
    for (int i = 0; i < 200; i++) {
        uint32_t a = vm_alloc(0, 0x40000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        CHECK(a >= 0x10000 && a + 0x40000 <= vm_user_max());
        v.push_back(a);
    }
    for (uint32_t a : v) vm_free(a, 0, MEM_RELEASE);
}

TEST(explicit_address_and_overlap) {
    t::setup_runtime("mem");
    uint32_t err = 0;
    uint32_t want = 0x30000000;
    CHECK_EQ(vm_alloc(want + 0x1234, 0x20000, MEM_RESERVE, PAGE_NOACCESS, &err), want);   // rounded down to 64K
    CHECK_EQ(vm_alloc(want + 0x10000, 0x1000, MEM_RESERVE, PAGE_NOACCESS, &err), 0u);       // overlaps
    CHECK_EQ(err, 487u);
    // commit inside the reservation
    CHECK_EQ(vm_alloc(want + 0x4000, 0x1000, MEM_COMMIT, PAGE_READWRITE, &err), want + 0x4000);
    CHECK(!faults_on_write(want + 0x4000));
    // a commit that crosses the end of the reservation fails
    CHECK_EQ(vm_alloc(want + 0x21000, 0x3000, MEM_COMMIT, PAGE_READWRITE, &err), 0u);
    // implicit reserve+commit on free space
    uint32_t w2 = 0x31000000;
    CHECK_EQ(vm_alloc(w2, 0x3000, MEM_COMMIT, PAGE_READWRITE, &err), w2);
    VmQuery q;
    CHECK(vm_query(w2, &q));
    CHECK_EQ(q.alloc_base, w2);
    CHECK(vm_free(w2, 0, MEM_RELEASE));
    CHECK(vm_free(want, 0, MEM_RELEASE));
    CHECK(!vm_free(want, 0, MEM_RELEASE));   // double free rejected
}

TEST(top_down) {
    t::setup_runtime("mem");
    uint32_t a = vm_alloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    uint32_t b = vm_alloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    CHECK(a > 0xF0000000u);
    CHECK(b < a);
    CHECK(a + 0x10000 <= vm_user_max());
    vm_free(a, 0, MEM_RELEASE);
    vm_free(b, 0, MEM_RELEASE);
}

TEST(big_commit) {
    t::setup_runtime("mem");
    uint32_t err = 0;
    uint32_t big = vm_alloc(0, 0x40000000, MEM_RESERVE, PAGE_NOACCESS, &err);   // 1 GiB reserve
    CHECK(big != 0);
    CHECK(vm_alloc(big, 0x100000, MEM_COMMIT, PAGE_READWRITE, &err) == big);
    memset(guest_base + big, 0x5A, 0x100000);
    CHECK(faults_on_read(big + 0x100000));
    CHECK(vm_free(big, 0, MEM_RELEASE));
}

TEST(file_view) {
    t::setup_runtime("mem");
    std::string p = t::temp_dir() + "/view.bin";
    FILE *f = fopen(p.c_str(), "wb");
    for (int i = 0; i < 100000; i++) fputc(i & 0xFF, f);
    fclose(f);
    FILE *r = fopen(p.c_str(), "rb");
    int fd = fileno(r);
    uint32_t err = 0;
    uint32_t v = vm_map_view(fd, 0, 100000, PAGE_READONLY, 0, &err);
    CHECK(v != 0);
    CHECK_EQ(*(uint8_t *)(guest_base + v + 258), (uint8_t)(258 & 0xFF));
    VmQuery q;
    CHECK(vm_query(v, &q));
    CHECK_EQ(q.type, (uint32_t)MEM_MAPPED);
    CHECK(faults_on_write(v));
    CHECK(vm_unmap_view(v));
    CHECK(faults_on_read(v));
    fclose(r);
}

TEST(fault_report_has_address) {
    t::setup_runtime("mem");
    CHECK(faults_on_read(0x1234));
    CHECK_EQ(g_last_fault_addr, 0x1234u);
    CHECK(faults_on_write(0x00020000u + 7));   // low addresses are unmapped unless allocated
}
