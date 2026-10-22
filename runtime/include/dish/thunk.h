// Host thunk table. Thunk i lives at guest address GUEST_THUNK_BASE + i*16. Indices [0, n_imports) come from
// guest_program.imports; further indices are allocated at run time (GetProcAddress results, COM vtable slots).
#pragma once
#include <cstdint>
#include "guest_abi.h"

namespace dish {

enum : uint32_t {
    TF_NEEDS_AUX = 1,   // guest_call records the thunk index in tl_thunk_idx before dispatch
    TF_STUB = 2,        // auto-generated "unimplemented import" stub
    TF_COM = 4,         // COM method slot (aux = method id)
};

struct ThunkEntry {
    GuestFn fn;
    uint32_t flags;
    uint32_t aux;
    const char *dll;    // static storage
    const char *name;
};

constexpr uint32_t kMaxThunks = (GUEST_THUNK_END - GUEST_THUNK_BASE) / GUEST_THUNK_STRIDE;   // 65536

extern ThunkEntry g_thunks[kMaxThunks];
extern thread_local uint32_t tl_thunk_idx;   // valid inside thunks registered with TF_NEEDS_AUX
extern bool g_hle_trace;                     // log every thunk call at LC_HLE/trace (DISH_TRACE_CALLS=1)

void thunk_reset();                          // test helper: clears the table
uint32_t thunk_alloc(GuestFn fn, uint32_t flags, uint32_t aux, const char *dll, const char *name);   // returns index
void thunk_set(uint32_t idx, GuestFn fn, uint32_t flags, uint32_t aux, const char *dll, const char *name);
uint32_t thunk_count();                      // next free index
inline uint32_t thunk_addr(uint32_t idx) { return guest_thunk_addr(idx); }
inline bool is_thunk_addr(uint32_t a) { return a - GUEST_THUNK_BASE < GUEST_THUNK_END - GUEST_THUNK_BASE; }
inline uint32_t thunk_index(uint32_t a) { return (a - GUEST_THUNK_BASE) / GUEST_THUNK_STRIDE; }
inline uint32_t thunk_aux() { return g_thunks[tl_thunk_idx].aux; }
const ThunkEntry *thunk_lookup(uint32_t guest_addr);   // nullptr if not an allocated thunk

}  // namespace dish
