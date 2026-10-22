#include "dish/thunk.h"
#include "dish/log.h"
#include <cstring>
#include <mutex>

namespace dish {

ThunkEntry g_thunks[kMaxThunks];
thread_local uint32_t tl_thunk_idx = 0;
bool g_hle_trace = false;
static uint32_t g_next = 0;
static std::mutex g_tmu;

void thunk_reset() {
    std::lock_guard<std::mutex> lk(g_tmu);
    memset(g_thunks, 0, sizeof g_thunks);
    g_next = 0;
}

uint32_t thunk_alloc(GuestFn fn, uint32_t flags, uint32_t aux, const char *dll, const char *name) {
    std::lock_guard<std::mutex> lk(g_tmu);
    if (g_next >= kMaxThunks) fatal("thunk table full (%u entries)", kMaxThunks);
    uint32_t i = g_next++;
    g_thunks[i] = ThunkEntry{fn, flags, aux, dll, name};
    return i;
}

void thunk_set(uint32_t idx, GuestFn fn, uint32_t flags, uint32_t aux, const char *dll, const char *name) {
    std::lock_guard<std::mutex> lk(g_tmu);
    if (idx >= kMaxThunks) fatal("thunk_set: bad index %u", idx);
    g_thunks[idx] = ThunkEntry{fn, flags, aux, dll, name};
    if (idx >= g_next) g_next = idx + 1;
}

uint32_t thunk_count() { return g_next; }

const ThunkEntry *thunk_lookup(uint32_t a) {
    if (!is_thunk_addr(a)) return nullptr;
    uint32_t i = thunk_index(a);
    return g_thunks[i].fn ? &g_thunks[i] : nullptr;
}

}  // namespace dish
