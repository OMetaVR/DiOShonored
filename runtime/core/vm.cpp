#include "dish/vm.h"
#include "dish/log.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
uint8_t *guest_base = nullptr;
}

namespace dish {
namespace {

enum : uint8_t { PS_FREE = 0, PS_RESERVED = 1, PS_COMMITTED = 2 };
// The reservation size is chosen at vm_init: 4 GiB when the OS grants it, otherwise a smaller fallback (iOS).
uint64_t kSpace = 1ull << 32;                       // guest bytes covered by the reservation
uint64_t kReserveBytes = (1ull << 32) + 0x10000;    // slack so a 4-byte access at the last address stays in the mapping
uint32_t kNumPages = (uint32_t)((1ull << 32) >> 12);
uint32_t kUserMax = GUEST_THUNK_BASE - 0x10000u;    // exclusive upper bound of allocatable space

struct Region {
    uint32_t size;        // bytes, page-rounded
    uint32_t type;        // MEM_PRIVATE / MEM_MAPPED / MEM_IMAGE
    uint32_t alloc_prot;  // AllocationProtect
    bool sys;             // permanently reserved (null guard, thunk range)
};

std::mutex g_mu;
std::map<uint32_t, Region> g_regions;
uint8_t *g_pstate = nullptr;     // kNumPages bytes
uint16_t *g_pprot = nullptr;     // kNumPages
uint32_t g_host_page = 4096;
uint32_t g_cursor = 0x10000;
uint64_t g_committed = 0;
uint64_t g_commit_limit = 0;   // 0 = unlimited
void *g_map = nullptr;

inline uint32_t rup(uint32_t x, uint32_t a) { return (x + a - 1) & ~(a - 1); }
inline uint64_t rup64(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }
inline uint64_t span_end(uint32_t base, const Region &r) { return rup64((uint64_t)base + r.size, kAllocGranularity); }

uint32_t canon_prot(uint32_t p) {
    return p & 0x7FF;
}
// Host protection bits for a guest protection value: bit0 = read, bit1 = write
int host_perm(uint32_t p) {
    if (p & PAGE_GUARD) return 0;
    switch (p & 0xFF) {
        case PAGE_NOACCESS: return 0;
        case PAGE_READONLY: case PAGE_EXECUTE: case PAGE_EXECUTE_READ: return 1;
        case PAGE_READWRITE: case PAGE_WRITECOPY: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return 3;
        default: return 0;
    }
}
bool valid_prot(uint32_t p) {
    switch (p & 0xFF) {
        case PAGE_NOACCESS: case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY: case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    }
    return false;
}
inline int perm_to_prot(int perm) { return (perm & 1 ? PROT_READ : 0) | (perm & 2 ? PROT_WRITE : 0); }

inline int page_perm(uint32_t pg) { return g_pstate[pg] == PS_COMMITTED ? host_perm(g_pprot[pg]) : 0; }

// Re-apply host protections for guest pages [first, first+n), using the union over each host page.
void apply_host(uint32_t first, uint32_t n) {
    if (!n) return;
    uint32_t hp = g_host_page >> 12;
    uint64_t a = first & ~(uint64_t)(hp - 1);
    uint64_t b = rup64((uint64_t)first + n, hp);
    if (b > kNumPages) b = kNumPages;
    uint64_t run_start = a;
    int run_perm = -1;
    auto flush = [&](uint64_t end) {
        if (run_perm >= 0 && end > run_start) {
            if (mprotect(guest_base + (run_start << 12), (end - run_start) << 12, perm_to_prot(run_perm)) != 0)
                LOGE(LC_MEM, "mprotect(%08llx, %llx, %d) failed", (unsigned long long)(run_start << 12),
                     (unsigned long long)((end - run_start) << 12), run_perm);
        }
    };
    for (uint64_t hpg = a; hpg < b; hpg += hp) {
        int perm = 0;
        for (uint32_t k = 0; k < hp && hpg + k < kNumPages; k++) perm |= page_perm((uint32_t)(hpg + k));
        if (perm != run_perm) { flush(hpg); run_start = hpg; run_perm = perm; }
    }
    flush(b);
}

// Return the (guest) range [lo,hi) of whole host pages inside [addr, addr+size) to a pristine zero PROT_NONE state.
void wipe_host(uint32_t addr, uint32_t size) {
    uint64_t lo = rup64(addr, g_host_page), hi = ((uint64_t)addr + size) & ~(uint64_t)(g_host_page - 1);
    if (hi > lo) {
        void *r = mmap(guest_base + lo, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
        if (r == MAP_FAILED) LOGE(LC_MEM, "wipe mmap failed at %08llx", (unsigned long long)lo);
    }
}

bool overlaps(uint64_t base, uint64_t end) {  // [base,end) in 64K-granular spans against existing regions
    auto it = g_regions.upper_bound((uint32_t)(end ? end - 1 : 0));
    if (it != g_regions.begin()) {
        --it;
        if (it->first < end && span_end(it->first, it->second) > base) return true;
    }
    return false;
}

uint32_t find_gap(uint32_t size, bool top_down) {
    uint64_t span = rup64(size, kAllocGranularity);
    if (span == 0 || span > kUserMax) return 0;
    if (top_down) {
        uint64_t hi = kUserMax;  // exclusive
        while (hi >= 0x10000 + span) {
            uint64_t cand = (hi - span) & ~(uint64_t)(kAllocGranularity - 1);
            auto it = g_regions.lower_bound((uint32_t)(cand + span));
            if (it != g_regions.begin()) {
                auto pv = std::prev(it);
                if (span_end(pv->first, pv->second) > cand) { hi = pv->first; continue; }
            }
            return (uint32_t)cand;
        }
        return 0;
    }
    auto scan = [&](uint64_t start, uint64_t limit) -> uint32_t {
        uint64_t cand = rup64(start, kAllocGranularity);
        if (cand < 0x10000) cand = 0x10000;
        while (cand + span <= limit) {
            auto it = g_regions.lower_bound((uint32_t)(cand + span));
            if (it == g_regions.begin()) return (uint32_t)cand;
            auto pv = std::prev(it);
            uint64_t e = span_end(pv->first, pv->second);
            if (e <= cand) return (uint32_t)cand;
            cand = e;
        }
        return 0;
    };
    uint32_t r = scan(g_cursor, kUserMax);
    if (!r) r = scan(0x10000, kUserMax);
    return r;
}

void set_state(uint32_t first, uint32_t n, uint8_t st, uint16_t prot) {
    for (uint32_t i = 0; i < n; i++) { g_pstate[first + i] = st; g_pprot[first + i] = prot; }
}

}  // namespace

bool vm_ready() { return guest_base != nullptr; }
uint32_t vm_host_page_size() { return g_host_page; }

uint32_t vm_user_max() { return kUserMax; }
uint64_t vm_space_bytes() { return kSpace; }

bool vm_init(uint64_t space_bytes) {
    if (guest_base) return true;
    g_host_page = (uint32_t)sysconf(_SC_PAGESIZE);
    if (g_host_page < 4096) g_host_page = 4096;
    // Reservation fallback chain. DISH_GUEST_SPACE_MB (env) forces a size for testing the reduced layouts.
    static const uint64_t kChain[] = {4096ull << 20, 3584ull << 20, 3072ull << 20, 2560ull << 20};
    uint64_t want = space_bytes;
    if (const char *e = getenv("DISH_GUEST_SPACE_MB")) want = strtoull(e, nullptr, 10) << 20;
    void *m = MAP_FAILED;
    uint64_t got = 0;
    for (uint64_t cand : kChain) {
        if (want && cand > want) continue;
        void *r = mmap(nullptr, cand + 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (r != MAP_FAILED) { m = r; got = cand; break; }
        LOGW(LC_MEM, "cannot reserve %llu MiB of address space (errno %d), trying smaller", (unsigned long long)(cand >> 20), errno);
    }
    if (m == MAP_FAILED) { LOGE(LC_MEM, "cannot reserve guest address space"); return false; }
    kSpace = got;
    kReserveBytes = got + 0x10000;
    kNumPages = (uint32_t)std::min<uint64_t>(got >> 12, 1ull << 20);
    kUserMax = got >= (1ull << 32) ? GUEST_THUNK_BASE - 0x10000u : (uint32_t)(got - 0x10000);
    g_map = m;
    guest_base = (uint8_t *)m;
    g_pstate = (uint8_t *)mmap(nullptr, kNumPages, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    g_pprot = (uint16_t *)mmap(nullptr, kNumPages * 2ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_pstate == (void *)MAP_FAILED || g_pprot == (void *)MAP_FAILED) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    g_regions.clear();
    g_regions[0] = Region{GUEST_NULL_GUARD_END, MEM_PRIVATE, PAGE_NOACCESS, true};
    set_state(0, GUEST_NULL_GUARD_END >> 12, PS_RESERVED, 0);
    // [kUserMax, 4G): guard + thunk range + sentinel page, permanently reserved
    g_regions[kUserMax] = Region{(uint32_t)(kSpace - kUserMax), MEM_PRIVATE, PAGE_NOACCESS, true};
    set_state(kUserMax >> 12, (uint32_t)((kSpace - kUserMax) >> 12), PS_RESERVED, 0);
    LOGI(LC_MEM, "guest address space: base=%p size=%llu MiB user_max=%08x host_page=%u", (void *)guest_base,
         (unsigned long long)(kSpace >> 20), kUserMax, g_host_page);
    return true;
}

void vm_shutdown() {
    if (!guest_base) return;
    munmap(g_map, kReserveBytes);
    munmap(g_pstate, kNumPages);
    munmap(g_pprot, kNumPages * 2ull);
    guest_base = nullptr;
    g_regions.clear();
    g_committed = 0;
    g_cursor = 0x10000;
}

uint32_t vm_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t prot, uint32_t *err) {
    auto fail = [&](uint32_t e) -> uint32_t { if (err) *err = e; return 0; };
    if (!size) return fail(ERR_INVALID_PARAMETER);
    if (!(type & (MEM_COMMIT | MEM_RESERVE | MEM_RESET))) return fail(ERR_INVALID_PARAMETER);
    if (type & MEM_RESET) return addr ? addr : fail(ERR_INVALID_PARAMETER);
    bool commit = type & MEM_COMMIT;
    if (commit && !valid_prot(prot)) return fail(ERR_INVALID_PARAMETER);
    std::lock_guard<std::mutex> lk(g_mu);
    if ((type & MEM_RESERVE) || addr == 0) {
        uint32_t base;
        uint64_t sz;
        if (addr == 0) {
            sz = rup64(size, kGuestPage);
            if (sz > kUserMax) return fail(ERR_NOT_ENOUGH_MEMORY);
            base = find_gap((uint32_t)sz, type & MEM_TOP_DOWN);
            if (!base) return fail(ERR_NOT_ENOUGH_MEMORY);
        } else {
            base = addr & ~(kAllocGranularity - 1);
            sz = rup64((uint64_t)(addr - base) + size, kGuestPage);
            if (base < 0x10000 || base + sz > kUserMax) return fail(ERR_INVALID_ADDRESS);
            if (overlaps(base, rup64(base + sz, kAllocGranularity))) return fail(ERR_INVALID_ADDRESS);
        }
        if (commit && g_commit_limit && g_committed + sz > g_commit_limit) return fail(ERR_NOT_ENOUGH_MEMORY);
        g_regions[base] = Region{(uint32_t)sz, MEM_PRIVATE, prot ? prot : PAGE_NOACCESS, false};
        set_state(base >> 12, (uint32_t)(sz >> 12), PS_RESERVED, 0);
        if (addr == 0) g_cursor = (uint32_t)std::min<uint64_t>(rup64(base + sz, kAllocGranularity), kUserMax - 1);
        if (commit) {
            uint32_t pg = base >> 12, n = (uint32_t)(sz >> 12);
            set_state(pg, n, PS_COMMITTED, canon_prot(prot));
            g_committed += sz;
            apply_host(pg, n);
        }
        return base;
    }
    // MEM_COMMIT only, at a fixed address
    uint64_t start = addr & ~(kGuestPage - 1);
    uint64_t end = rup64((uint64_t)addr + size, kGuestPage);
    if (start < 0x10000 || end > kUserMax) return fail(ERR_INVALID_ADDRESS);
    auto it = g_regions.upper_bound((uint32_t)start);
    bool in_region = false;
    if (it != g_regions.begin()) {
        auto pv = std::prev(it);
        if (pv->first + (uint64_t)pv->second.size > start) {
            if (pv->second.sys) return fail(ERR_INVALID_ADDRESS);
            if (pv->first + (uint64_t)pv->second.size < end) return fail(ERR_INVALID_ADDRESS);
            in_region = true;
        }
    }
    if (g_commit_limit && g_committed + (end - start) > g_commit_limit) {   // conservative: counts already-committed pages too
        uint64_t add = 0;
        for (uint64_t pg = start >> 12; pg < (end >> 12); pg++) if (g_pstate[pg] != PS_COMMITTED) add += kGuestPage;
        if (g_committed + add > g_commit_limit) return fail(ERR_NOT_ENOUGH_MEMORY);
    }
    if (!in_region) {  // implicit reserve + commit of free space
        uint32_t base = (uint32_t)(start & ~(uint64_t)(kAllocGranularity - 1));
        uint64_t sz = rup64(end - base, kGuestPage);
        if (overlaps(base, rup64(base + sz, kAllocGranularity))) return fail(ERR_INVALID_ADDRESS);
        g_regions[base] = Region{(uint32_t)sz, MEM_PRIVATE, prot, false};
        set_state(base >> 12, (uint32_t)(sz >> 12), PS_RESERVED, 0);
    }
    uint32_t pg = (uint32_t)(start >> 12), n = (uint32_t)((end - start) >> 12);
    for (uint32_t i = 0; i < n; i++)
        if (g_pstate[pg + i] != PS_COMMITTED) g_committed += kGuestPage;
    set_state(pg, n, PS_COMMITTED, canon_prot(prot));
    apply_host(pg, n);
    return (uint32_t)start;
}

bool vm_free(uint32_t addr, uint32_t size, uint32_t type, uint32_t *err) {
    auto fail = [&](uint32_t e) { if (err) *err = e; return false; };
    std::lock_guard<std::mutex> lk(g_mu);
    if (type & MEM_RELEASE) {
        if (size != 0) return fail(ERR_INVALID_PARAMETER);
        auto it = g_regions.find(addr);
        if (it == g_regions.end() || it->second.sys) return fail(ERR_INVALID_ADDRESS);
        uint32_t sz = it->second.size, pg = addr >> 12, n = sz >> 12;
        for (uint32_t i = 0; i < n; i++)
            if (g_pstate[pg + i] == PS_COMMITTED) g_committed -= kGuestPage;
        set_state(pg, n, PS_FREE, 0);
        void *r = mmap(guest_base + addr, rup(sz, g_host_page), PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
        if (r == MAP_FAILED) LOGE(LC_MEM, "release remap failed at %08x", addr);
        g_regions.erase(it);
        if (addr < g_cursor) g_cursor = addr;
        return true;
    }
    if (type & MEM_DECOMMIT) {
        uint64_t start = addr & ~(kGuestPage - 1);
        uint64_t end = size ? rup64((uint64_t)addr + size, kGuestPage) : 0;
        auto it = g_regions.upper_bound(addr);
        if (it == g_regions.begin()) return fail(ERR_INVALID_ADDRESS);
        --it;
        if (it->second.sys || it->first + (uint64_t)it->second.size <= start) return fail(ERR_INVALID_ADDRESS);
        if (size == 0) {
            if (addr != it->first) return fail(ERR_INVALID_PARAMETER);
            end = it->first + (uint64_t)it->second.size;
        }
        if (end > it->first + (uint64_t)it->second.size) return fail(ERR_INVALID_ADDRESS);
        uint32_t pg = (uint32_t)(start >> 12), n = (uint32_t)((end - start) >> 12);
        for (uint32_t i = 0; i < n; i++)
            if (g_pstate[pg + i] == PS_COMMITTED) g_committed -= kGuestPage;
        set_state(pg, n, PS_RESERVED, 0);
        wipe_host((uint32_t)start, (uint32_t)(end - start));
        apply_host(pg, n);
        return true;
    }
    return fail(ERR_INVALID_PARAMETER);
}

bool vm_protect(uint32_t addr, uint32_t size, uint32_t prot, uint32_t *old_prot, uint32_t *err) {
    auto fail = [&](uint32_t e) { if (err) *err = e; return false; };
    if (!size || !valid_prot(prot)) return fail(ERR_INVALID_PARAMETER);
    std::lock_guard<std::mutex> lk(g_mu);
    uint64_t start = addr & ~(kGuestPage - 1), end = rup64((uint64_t)addr + size, kGuestPage);
    if (end > kSpace) return fail(ERR_INVALID_ADDRESS);
    uint32_t pg = (uint32_t)(start >> 12), n = (uint32_t)((end - start) >> 12);
    for (uint32_t i = 0; i < n; i++)
        if (g_pstate[pg + i] != PS_COMMITTED) return fail(ERR_INVALID_ADDRESS);
    if (old_prot) *old_prot = g_pprot[pg];
    for (uint32_t i = 0; i < n; i++) g_pprot[pg + i] = (uint16_t)canon_prot(prot);
    apply_host(pg, n);
    return true;
}

bool vm_query(uint32_t addr, VmQuery *q) {
    std::lock_guard<std::mutex> lk(g_mu);
    uint32_t pg = addr >> 12;
    q->base = addr & ~(kGuestPage - 1);
    if (g_pstate[pg] == PS_FREE) {
        auto it = g_regions.upper_bound(addr);
        uint64_t next = it == g_regions.end() ? kSpace : it->first;
        q->alloc_base = 0; q->alloc_prot = 0; q->state = MEM_FREE; q->prot = PAGE_NOACCESS; q->type = 0;
        q->size = (uint32_t)(next - q->base);
        return true;
    }
    auto it = g_regions.upper_bound(addr);
    if (it == g_regions.begin()) return false;
    --it;
    uint64_t rend = it->first + (uint64_t)it->second.size;
    if (addr >= rend) {
        q->alloc_base = 0; q->alloc_prot = 0; q->state = MEM_FREE; q->prot = PAGE_NOACCESS; q->type = 0;
        auto nx = std::next(it);
        q->size = (uint32_t)((nx == g_regions.end() ? kSpace : nx->first) - q->base);
        return true;
    }
    q->alloc_base = it->first;
    q->alloc_prot = it->second.alloc_prot;
    q->type = it->second.type;
    uint8_t st = g_pstate[pg];
    uint16_t pr = g_pprot[pg];
    q->state = st == PS_COMMITTED ? MEM_COMMIT : MEM_RESERVE;
    q->prot = st == PS_COMMITTED ? pr : 0;
    uint64_t endpg = rend >> 12, p = pg + 1;
    while (p < endpg && g_pstate[p] == st && (st != PS_COMMITTED || g_pprot[p] == pr)) p++;
    q->size = (uint32_t)(((p << 12) - ((uint64_t)pg << 12)));
    if (p == kNumPages) q->size = (uint32_t)(kSpace - q->base);
    return true;
}

bool vm_reserve_image(uint32_t base, uint32_t size, uint32_t *err) {
    auto fail = [&](uint32_t e) { if (err) *err = e; return false; };
    if (base & (kAllocGranularity - 1) || !size) return fail(ERR_INVALID_PARAMETER);
    std::lock_guard<std::mutex> lk(g_mu);
    uint64_t sz = rup64(size, kGuestPage);
    if (base < 0x10000 || base + sz > kUserMax) return fail(ERR_INVALID_ADDRESS);
    auto ex = g_regions.find(base);
    if (ex != g_regions.end() && ex->second.type == MEM_IMAGE && ex->second.size >= sz) return true;   // idempotent
    if (overlaps(base, rup64(base + sz, kAllocGranularity))) return fail(ERR_INVALID_ADDRESS);
    g_regions[base] = Region{(uint32_t)sz, MEM_IMAGE, PAGE_EXECUTE_WRITECOPY, false};
    set_state(base >> 12, (uint32_t)(sz >> 12), PS_RESERVED, 0);
    return true;
}

bool vm_map_image(uint32_t base, uint32_t size, uint32_t *err) {
    auto fail = [&](uint32_t e) { if (err) *err = e; return false; };
    if (base & (kAllocGranularity - 1) || !size) return fail(ERR_INVALID_PARAMETER);
    std::lock_guard<std::mutex> lk(g_mu);
    uint64_t sz = rup64(size, kGuestPage);
    if (base < 0x10000 || base + sz > kUserMax) return fail(ERR_INVALID_ADDRESS);
    auto ex = g_regions.find(base);
    if (ex != g_regions.end() && ex->second.type == MEM_IMAGE && !ex->second.sys && ex->second.size >= sz) {
        sz = ex->second.size;   // previously reserved by vm_reserve_image
    } else {
        if (overlaps(base, rup64(base + sz, kAllocGranularity))) return fail(ERR_INVALID_ADDRESS);
        g_regions[base] = Region{(uint32_t)sz, MEM_IMAGE, PAGE_EXECUTE_WRITECOPY, false};
    }
    uint32_t pg = base >> 12, n = (uint32_t)(sz >> 12);
    set_state(pg, n, PS_COMMITTED, PAGE_READWRITE);
    g_committed += sz;
    apply_host(pg, n);
    return true;
}

uint32_t vm_map_view(int fd, uint64_t offset, uint32_t size, uint32_t prot, uint32_t hint, uint32_t *err) {
    auto fail = [&](uint32_t e) -> uint32_t { if (err) *err = e; return 0; };
    if (!size) return fail(ERR_INVALID_PARAMETER);
    std::lock_guard<std::mutex> lk(g_mu);
    uint64_t sz = rup64(size, kGuestPage);
    uint32_t base;
    if (hint) {
        base = hint & ~(kAllocGranularity - 1);
        if (base != hint || overlaps(base, rup64(base + sz, kAllocGranularity)) || base + sz > kUserMax)
            return fail(ERR_INVALID_ADDRESS);
    } else {
        base = find_gap((uint32_t)sz, false);
        if (!base) return fail(ERR_NOT_ENOUGH_MEMORY);
    }
    int hperm = host_perm(prot);
    bool priv = (prot & 0xFF) == PAGE_WRITECOPY || (prot & 0xFF) == PAGE_EXECUTE_WRITECOPY;
    void *r = mmap(guest_base + base, rup((uint32_t)sz, g_host_page), perm_to_prot(hperm),
                   (priv ? MAP_PRIVATE : MAP_SHARED) | MAP_FIXED, fd, (off_t)offset);
    if (r == MAP_FAILED) { LOGE(LC_MEM, "view mmap failed (errno %d)", errno); return fail(ERR_NOT_ENOUGH_MEMORY); }
    g_regions[base] = Region{(uint32_t)sz, MEM_MAPPED, prot, false};
    set_state(base >> 12, (uint32_t)(sz >> 12), PS_COMMITTED, canon_prot(prot));
    g_committed += sz;
    return base;
}

bool vm_unmap_view(uint32_t base) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_regions.find(base);
    if (it == g_regions.end() || it->second.type != MEM_MAPPED) return false;
    uint32_t sz = it->second.size, pg = base >> 12, n = sz >> 12;
    g_committed -= sz;
    set_state(pg, n, PS_FREE, 0);
    mmap(guest_base + base, rup(sz, g_host_page), PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
    g_regions.erase(it);
    if (base < g_cursor) g_cursor = base;
    return true;
}

bool vm_is_committed(uint32_t addr, uint32_t size) {
    if (!size) return true;
    uint64_t end = (uint64_t)addr + size;
    if (end > kSpace) return false;
    for (uint64_t p = addr >> 12; p <= (end - 1) >> 12; p++)
        if (g_pstate[p] != PS_COMMITTED || host_perm(g_pprot[p]) == 0) return false;
    return true;
}

uint64_t vm_committed_bytes() { return g_committed; }
void vm_set_commit_limit(uint64_t bytes) { std::lock_guard<std::mutex> lk(g_mu); g_commit_limit = bytes; }

}  // namespace dish
