// Host -> guest calls, the sentinel return rule, unresolved-call handling.
#pragma once
#include <cstdint>
#include <initializer_list>
#include "guest_abi.h"

namespace dish {

constexpr uint32_t kReturnSentinel = 0xFFFFFFF0u;   // above GUEST_THUNK_END, never mapped

enum class Conv : uint8_t { Cdecl, Stdcall, Thiscall, Fastcall };

// Call guest code with the stack pointer `esp` as the current top of stack (arguments are pushed below it).
// Thunks that call back into guest code MUST use this form with the esp they were called with.
// conv: Thiscall puts args[0] in ecx; Fastcall puts args[0], args[1] in ecx, edx. Returns eax; full result in *out.
uint32_t guest_invoke_at(uint32_t esp, uint32_t target, Conv conv, const uint32_t *args, unsigned nargs,
                         GuestRet *out = nullptr);
// Same, but on the current guest thread's tracked stack pointer. Only valid at the top level of a guest thread
// (loader, thread entry, window/COM callbacks arriving from a foreign host thread) and for calls issued from
// host code that is not inside a thunk.
uint32_t guest_invoke_args(uint32_t target, Conv conv, const uint32_t *args, unsigned nargs, GuestRet *out = nullptr);

template <class... A>
inline uint32_t guest_invoke(uint32_t target, Conv conv, A... a) {
    const uint32_t v[] = {(uint32_t)a..., 0};
    return guest_invoke_args(target, conv, v, sizeof...(A));
}
template <class... A>
inline uint32_t guest_invoke_from(uint32_t esp, uint32_t target, Conv conv, A... a) {
    const uint32_t v[] = {(uint32_t)a..., 0};
    return guest_invoke_at(esp, target, conv, v, sizeof...(A));
}

// Handler for a guest_call to an address nobody knows. Default: log + guest stack dump to unresolved.log + abort.
using UnresolvedHandler = GuestRet (*)(uint32_t target, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
void set_unresolved_handler(UnresolvedHandler h);
void set_unresolved_log_path(const char *path);
uint64_t unresolved_count();

}  // namespace dish
