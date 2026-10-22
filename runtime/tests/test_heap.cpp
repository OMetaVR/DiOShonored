#include "testing.h"
#include "dish/heap.h"
#include <cstring>
#include <random>
#include <thread>

using namespace dish;

TEST(basic) {
    t::setup_runtime("heap");
    uint32_t h = heap_process();
    uint32_t a = heap_alloc(h, 0, 100);
    CHECK(a != 0);
    CHECK_EQ(a & 15, 0u);
    CHECK_EQ(heap_size(h, 0, a), 100u);
    memset(gp(a), 0xAB, 100);
    uint32_t b = heap_alloc(h, HEAP_ZERO_MEMORY, 100);
    CHECK(b != a);
    for (int i = 0; i < 100; i++) CHECK_EQ(gp(b)[i], 0);
    CHECK(heap_validate(h, a));
    CHECK(heap_free(h, 0, a));
    uint32_t c = heap_alloc(h, 0, 100);
    CHECK_EQ(c, a);   // LIFO reuse from the class free list
    heap_free(h, 0, b);
    heap_free(h, 0, c);
}

TEST(realloc_preserves) {
    t::setup_runtime("heap");
    uint32_t h = heap_process();
    uint32_t a = heap_alloc(h, 0, 40);
    for (int i = 0; i < 40; i++) gp(a)[i] = (uint8_t)i;
    uint32_t b = heap_realloc(h, 0, a, 5000);
    CHECK(b != 0);
    for (int i = 0; i < 40; i++) CHECK_EQ(gp(b)[i], (uint8_t)i);
    CHECK_EQ(heap_size(h, 0, b), 5000u);
    uint32_t c = heap_realloc(h, HEAP_ZERO_MEMORY, b, 6000);
    CHECK_EQ(gp(c)[5500], 0);
    uint32_t d = heap_realloc(h, 0, c, 20);
    for (int i = 0; i < 20; i++) CHECK_EQ(gp(d)[i], (uint8_t)i);
    CHECK_EQ(heap_realloc(h, HEAP_REALLOC_IN_PLACE_ONLY, d, 100000), 0u);
    heap_free(h, 0, d);
}

TEST(large_blocks) {
    t::setup_runtime("heap");
    uint32_t h = heap_create(0, 0, 0);
    uint32_t a = heap_alloc(h, 0, 3u << 20);
    CHECK(a != 0);
    CHECK_EQ(a & 15, 0u);
    gp(a)[(3u << 20) - 1] = 7;
    CHECK_EQ(heap_size(h, 0, a), 3u << 20);
    CHECK(heap_free(h, 0, a));
    CHECK(vm_is_committed(a, 1) == false);   // large blocks go straight back to the VM
    CHECK(heap_destroy(h));
    CHECK_EQ(heap_alloc(h, 0, 16), 0u);      // destroyed heap rejects
}

TEST(many_sizes_and_alignment) {
    t::setup_runtime("heap");
    uint32_t h = heap_create(0, 0, 0);
    std::vector<uint32_t> v;
    for (uint32_t s = 1; s < 70000; s = s * 3 / 2 + 1) {
        uint32_t p = heap_alloc(h, 0, s);
        CHECK(p != 0);
        CHECK_EQ(p & 15, 0u);
        CHECK(heap_size(h, 0, p) == s);
        memset(gp(p), 0x11, s);
        v.push_back(p);
    }
    for (uint32_t p : v) CHECK(heap_free(h, 0, p));
}

TEST(multithreaded_stress) {
    t::setup_runtime("heap");
    uint32_t h = heap_process();
    std::atomic<int> bad{0};
    auto worker = [&](int seed) {
        RtThread *th = thread_attach("heapw", 0x20000, true);
        std::mt19937 rng(seed);
        std::vector<std::pair<uint32_t, uint32_t>> live;
        for (int i = 0; i < 20000; i++) {
            if (live.size() < 200 && (rng() & 1)) {
                uint32_t s = 1 + rng() % 3000;
                uint32_t p = heap_alloc(h, 0, s);
                if (!p) { bad++; continue; }
                memset(gp(p), (int)(p & 0xFF), s);
                live.push_back({p, s});
            } else if (!live.empty()) {
                size_t k = rng() % live.size();
                auto [p, s] = live[k];
                for (uint32_t j = 0; j < s; j += 97) if (gp(p)[j] != (uint8_t)(p & 0xFF)) bad++;
                heap_free(h, 0, p);
                live[k] = live.back();
                live.pop_back();
            }
        }
        for (auto &[p, s] : live) heap_free(h, 0, p);
        thread_detach(th);
    };
    std::vector<std::thread> ts;
    for (int i = 0; i < 6; i++) ts.emplace_back(worker, 100 + i);
    for (auto &x : ts) x.join();
    CHECK_EQ(bad.load(), 0);
}

TEST(hle_wrappers) {
    t::setup_runtime("heap");
    auto h = t::hle("kernel32.dll", "GetProcessHeap", {});
    CHECK_EQ(h.pop(), 0u);
    auto a = t::hle("kernel32.dll", "HeapAlloc", {h.eax(), 8, 64});
    CHECK_EQ(a.pop(), 12u);
    CHECK(a.eax() != 0);
    for (int i = 0; i < 64; i++) CHECK_EQ(gp(a.eax())[i], 0);
    auto s = t::hle("kernel32.dll", "HeapSize", {h.eax(), 0, a.eax()});
    CHECK_EQ(s.eax(), 64u);
    auto f = t::hle("kernel32.dll", "HeapFree", {h.eax(), 0, a.eax()});
    CHECK_EQ(f.eax(), 1u);
    auto ga = t::hle("kernel32.dll", "GlobalAlloc", {0x40, 32});
    CHECK(ga.eax() != 0);
    CHECK_EQ(t::hle("kernel32.dll", "GlobalSize", {ga.eax()}).eax(), 32u);
    CHECK_EQ(t::hle("kernel32.dll", "GlobalFree", {ga.eax()}).eax(), 0u);
}
