#include "dish/heap.h"
#include "dish/log.h"
#include "dish/thread.h"
#include "dish/util.h"
#include "dish/vm.h"
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace dish {
namespace {

constexpr uint32_t kChunk = 1u << 20;
constexpr uint32_t kMaxSmall = 256u << 10;     // user sizes above this get their own VM region
constexpr uint32_t kHdr = 8;
constexpr uint16_t kLargeCls = 0xFFFF;
constexpr uint32_t kHeapHandleBase = 0x00150000;
constexpr uint32_t kHeapHandleStep = 0x1000;

struct Hdr { uint32_t req; uint16_t cls; uint16_t heap; };

std::vector<uint32_t> g_class_total;   // total block size (header included), multiples of 16

void build_classes() {
    if (!g_class_total.empty()) return;
    for (uint32_t s = 32; s <= 128; s += 16) g_class_total.push_back(s);
    uint32_t base = 128;
    while (base < kMaxSmall + 64) {
        for (int k = 1; k <= 4; k++) {
            uint32_t s = base + base / 4 * k;
            g_class_total.push_back((s + 15) & ~15u);
        }
        base *= 2;
    }
}

uint16_t class_for(uint32_t req) {
    uint32_t total = req + kHdr;
    // binary search
    size_t lo = 0, hi = g_class_total.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_class_total[mid] < total) lo = mid + 1; else hi = mid;
    }
    return (uint16_t)lo;
}

struct Heap {
    uint16_t id;
    uint32_t flags;
    std::mutex mu;
    std::vector<uint32_t> free_head;    // per class: user pointer or 0
    uint32_t bump = 0, bump_end = 0;
    std::vector<uint32_t> chunks;
    std::vector<uint32_t> large;        // live large blocks (region bases)
    bool dead = false;
    Heap() : id(0), flags(0) { free_head.assign(g_class_total.size(), 0); }
};

std::mutex g_hmu;
std::vector<std::unique_ptr<Heap>> g_heaps;     // index = id - 1
std::atomic<uint64_t> g_in_use{0};
uint32_t g_process_heap = 0;

Heap *heap_from_handle(uint32_t h) {
    if (h < kHeapHandleBase || (h - kHeapHandleBase) % kHeapHandleStep) return nullptr;
    uint32_t idx = (h - kHeapHandleBase) / kHeapHandleStep;
    std::lock_guard<std::mutex> lk(g_hmu);
    if (idx >= g_heaps.size() || !g_heaps[idx] || g_heaps[idx]->dead) return nullptr;
    return g_heaps[idx].get();
}

inline Hdr &hdr_of(uint32_t user) { return *(Hdr *)(guest_base + user - kHdr); }

bool new_chunk(Heap *h, uint32_t need) {
    uint32_t sz = need > kChunk ? need : kChunk;
    uint32_t c = vm_alloc(0, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!c) return false;
    h->chunks.push_back(c);
    // block starts must be 8 mod 16 so that user pointers are 16-aligned
    h->bump = c + 8;
    h->bump_end = c + ((sz + 0xFFFF) & ~0xFFFFu);
    if (h->bump_end > c + sz) h->bump_end = c + sz;
    return true;
}

uint32_t cap_of(uint16_t cls) { return g_class_total[cls] - kHdr; }

}  // namespace

uint32_t heap_create(uint32_t flags, uint32_t, uint32_t) {
    std::lock_guard<std::mutex> lk(g_hmu);
    build_classes();
    auto h = std::make_unique<Heap>();
    h->flags = flags;
    uint32_t idx = (uint32_t)g_heaps.size();
    h->id = (uint16_t)(idx + 1);
    g_heaps.push_back(std::move(h));
    return kHeapHandleBase + idx * kHeapHandleStep;
}

bool heap_destroy(uint32_t handle) {
    Heap *h = heap_from_handle(handle);
    if (!h || handle == g_process_heap) return false;
    std::lock_guard<std::mutex> lk(h->mu);
    for (uint32_t c : h->chunks) vm_free(c, 0, MEM_RELEASE);
    for (uint32_t l : h->large) vm_free(l, 0, MEM_RELEASE);
    h->chunks.clear();
    h->large.clear();
    h->dead = true;
    return true;
}

uint32_t heap_process() {
    std::lock_guard<std::mutex> lk(g_hmu);
    if (!g_process_heap) {
        build_classes();
        auto h = std::make_unique<Heap>();
        h->id = (uint16_t)(g_heaps.size() + 1);
        g_process_heap = kHeapHandleBase + (uint32_t)g_heaps.size() * kHeapHandleStep;
        g_heaps.push_back(std::move(h));
    }
    return g_process_heap;
}

uint32_t heap_alloc(uint32_t handle, uint32_t flags, uint32_t size) {
    Heap *h = heap_from_handle(handle);
    if (!h) { set_last_error(6); return 0; }
    if (size > 0x7FFF0000u) { set_last_error(8); return 0; }
    if (size == 0) size = 1;
    uint32_t user;
    if (size > kMaxSmall) {
        uint32_t total = (size + 16 + 0xFFF) & ~0xFFFu;
        uint32_t base = vm_alloc(0, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!base) { set_last_error(8); return 0; }
        user = base + 16;
        Hdr &hd = hdr_of(user);
        hd.req = size; hd.cls = kLargeCls; hd.heap = h->id;
        std::lock_guard<std::mutex> lk(h->mu);
        h->large.push_back(base);
        g_in_use += size;
        return user;   // fresh VM is already zero
    }
    uint16_t cls = class_for(size);
    {
        std::lock_guard<std::mutex> lk(h->mu);
        uint32_t u = h->free_head[cls];
        if (u) {
            h->free_head[cls] = rd32(u);
            user = u;
        } else {
            uint32_t tot = g_class_total[cls];
            if (h->bump + tot > h->bump_end) {
                if (!new_chunk(h, tot + 16)) { set_last_error(8); return 0; }
            }
            user = h->bump + kHdr;
            h->bump += tot;
        }
        Hdr &hd = hdr_of(user);
        hd.req = size; hd.cls = cls; hd.heap = h->id;
    }
    g_in_use += size;
    if (flags & HEAP_ZERO_MEMORY) memset(guest_base + user, 0, size);
    return user;
}

bool heap_free(uint32_t handle, uint32_t, uint32_t ptr) {
    if (!ptr) return true;
    Heap *h = heap_from_handle(handle);
    if (!h || !vm_is_committed(ptr - kHdr, kHdr)) { set_last_error(87); return false; }
    Hdr hd = hdr_of(ptr);
    if (hd.req == 0 && hd.cls != kLargeCls) { LOGW(LC_HLE, "HeapFree: double free or foreign pointer %08x", ptr); return true; }
    Heap *owner = hd.heap && hd.heap == h->id ? h : nullptr;
    if (!owner) {
        // pointer from another heap: honour the header (Windows would corrupt; we are lenient)
        std::lock_guard<std::mutex> lk(g_hmu);
        if (hd.heap && hd.heap <= g_heaps.size()) owner = g_heaps[hd.heap - 1].get();
        if (!owner) { set_last_error(87); return false; }
    }
    g_in_use -= hd.req;
    if (hd.cls == kLargeCls) {
        std::lock_guard<std::mutex> lk(owner->mu);
        uint32_t base = ptr - 16;
        for (size_t i = 0; i < owner->large.size(); i++)
            if (owner->large[i] == base) { owner->large[i] = owner->large.back(); owner->large.pop_back(); break; }
        vm_free(base, 0, MEM_RELEASE);
        return true;
    }
    if (hd.cls >= g_class_total.size()) { set_last_error(87); return false; }
    std::lock_guard<std::mutex> lk(owner->mu);
    wr32(ptr, owner->free_head[hd.cls]);
    owner->free_head[hd.cls] = ptr;
    hdr_of(ptr).req = 0;
    return true;
}

uint32_t heap_size(uint32_t handle, uint32_t, uint32_t ptr) {
    if (!heap_from_handle(handle) || !ptr || !vm_is_committed(ptr - kHdr, kHdr)) { set_last_error(87); return 0xFFFFFFFFu; }
    return hdr_of(ptr).req;
}

bool heap_validate(uint32_t handle, uint32_t ptr) {
    if (!heap_from_handle(handle)) return false;
    if (!ptr) return true;
    if (!vm_is_committed(ptr - kHdr, kHdr)) return false;
    Hdr hd = hdr_of(ptr);
    return hd.heap != 0 && (hd.cls == kLargeCls || hd.cls < g_class_total.size());
}

uint32_t heap_realloc(uint32_t handle, uint32_t flags, uint32_t ptr, uint32_t size) {
    if (!ptr) return heap_alloc(handle, flags, size);
    Heap *h = heap_from_handle(handle);
    if (!h || !vm_is_committed(ptr - kHdr, kHdr)) { set_last_error(87); return 0; }
    Hdr hd = hdr_of(ptr);
    uint32_t old = hd.req;
    if (size == 0) size = 1;
    if (hd.cls != kLargeCls && hd.cls < g_class_total.size() && size <= cap_of(hd.cls) && (size > cap_of(hd.cls) / 2 || cap_of(hd.cls) < 256)) {
        // fits in the existing block
        hdr_of(ptr).req = size;
        g_in_use += (uint64_t)size - old;
        if ((flags & HEAP_ZERO_MEMORY) && size > old) memset(guest_base + ptr + old, 0, size - old);
        return ptr;
    }
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) { set_last_error(8); return 0; }
    uint32_t np = heap_alloc(handle, flags & ~HEAP_ZERO_MEMORY, size);
    if (!np) return 0;
    uint32_t n = old < size ? old : size;
    memcpy(guest_base + np, guest_base + ptr, n);
    if ((flags & HEAP_ZERO_MEMORY) && size > old) memset(guest_base + np + old, 0, size - old);
    heap_free(handle, 0, ptr);
    return np;
}

uint64_t heap_bytes_in_use() { return g_in_use.load(); }

void heap_reset() {
    std::lock_guard<std::mutex> lk(g_hmu);
    g_heaps.clear();
    g_process_heap = 0;
    g_in_use = 0;
}

}  // namespace dish
