// HLE binding: write host implementations as ordinary C++ functions taking the Win32 arguments; the templates
// below turn them into GuestFn thunks that read the stack arguments, pop the right number of bytes and return
// eax/edx (or st0_ret) per docs/guest_abi.md.
//
//   static uint32_t k32_Sleep(uint32_t ms) { ... return 0; }
//   HLE("kernel32.dll", "Sleep", Stdcall, k32_Sleep);
//
// A first parameter of type `const HleCtx&` receives esp/eax/ecx/edx (needed to call back into guest code).
// Argument types: integral types <= 4 bytes (one stack slot), 64-bit integers / double (two slots), float (one slot).
#pragma once
#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>
#include "guest_abi.h"
#include "dish/log.h"
#include "dish/vm.h"

namespace dish {

struct HleCtx { uint32_t esp, eax, ecx, edx; };
enum class HConv : uint8_t { Stdcall = 'S', Cdecl = 'C', Thiscall = 'T', Varargs = 'V' };

namespace hle_detail {

struct ArgCursor {
    uint32_t off;        // guest address of the next stack slot
    uint32_t ecx;
    bool take_ecx;
    template <class T> T next() {
        static_assert(sizeof(T) <= 8 && (std::is_arithmetic_v<T> || std::is_enum_v<T>), "HLE args must be arithmetic");
        if (take_ecx) {
            take_ecx = false;
            return (T)ecx;
        }
        T v;
        if constexpr (sizeof(T) == 8) {
            memcpy(&v, guest_base + off, 8);
            off += 8;
        } else if constexpr (std::is_floating_point_v<T>) {
            float f;
            memcpy(&f, guest_base + off, 4);
            v = (T)f;
            off += 4;
        } else {
            uint32_t u;
            memcpy(&u, guest_base + off, 4);
            v = static_cast<T>(u);
            off += 4;
        }
        return v;
    }
};

template <class T> constexpr uint32_t slot_bytes() { return sizeof(T) > 4 ? 8 : 4; }

template <class... T> struct TL {};

template <class F> struct Traits;
template <class R, class... A> struct Traits<R (*)(A...)> {
    using Ret = R;
    static constexpr bool ctx = false;
    using Args = TL<A...>;
};
template <class R, class... A> struct Traits<R (*)(const HleCtx &, A...)> {
    using Ret = R;
    static constexpr bool ctx = true;
    using Args = TL<A...>;
};

template <class... A> constexpr uint32_t sum_bytes(TL<A...>) { return (0u + ... + slot_bytes<A>()); }

template <class R> inline GuestRet make_ret(R v, uint32_t pop) {
    if constexpr (std::is_same_v<R, GuestRet>) {
        (void)pop;
        return v;
    } else if constexpr (std::is_floating_point_v<R>) {
        guest_thread->st0_ret = (double)v;
        return guest_ret(0, 0, pop);
    } else if constexpr (sizeof(R) == 8) {
        uint64_t u = (uint64_t)v;
        return guest_ret((uint32_t)u, (uint32_t)(u >> 32), pop);
    } else {
        return guest_ret((uint32_t)v, 0, pop);
    }
}

template <auto Fn, HConv C, class R, bool Ctx, class... B>
inline GuestRet run(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx, TL<B...>) {
    ArgCursor cur{esp + 4, ecx, C == HConv::Thiscall};
    std::tuple<B...> args{cur.template next<B>()...};
    uint32_t pop = (C == HConv::Stdcall || C == HConv::Thiscall) ? cur.off - (esp + 4) : 0;
    HleCtx ctx{esp, eax, ecx, edx};
    auto call = [&](auto &&...a) -> R {
        if constexpr (Ctx) return Fn(ctx, a...);
        else return Fn(a...);
    };
    if constexpr (std::is_void_v<R>) {
        std::apply(call, args);
        return guest_ret(0, 0, pop);
    } else {
        R r = std::apply(call, args);
        return make_ret<R>(r, pop);
    }
}

}  // namespace hle_detail

template <auto Fn, HConv C>
GuestRet hle_thunk(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {
    using Tr = hle_detail::Traits<decltype(Fn)>;
    return hle_detail::run<Fn, C, typename Tr::Ret, Tr::ctx>(esp, eax, ecx, edx, typename Tr::Args{});
}

template <auto Fn, HConv C>
constexpr uint32_t hle_pop() {
    using Tr = hle_detail::Traits<decltype(Fn)>;
    if constexpr (C == HConv::Cdecl || C == HConv::Varargs) return 0;
    else {
        uint32_t b = hle_detail::sum_bytes(typename Tr::Args{});
        return C == HConv::Thiscall ? b - 4 : b;
    }
}

struct HleImpl {
    const char *dll;      // lower-case
    const char *name;
    GuestFn fn;
    uint32_t pop;
    HConv conv;
};

// Registration (static initialisers). Names are matched case-sensitively; dll names lower-case.
void hle_register(const HleImpl &impl);
struct HleRegistrar {
    HleRegistrar(const char *dll, const char *name, GuestFn fn, uint32_t pop, HConv c) { hle_register({dll, name, fn, pop, c}); }
};
const HleImpl *hle_find(const char *dll, const char *name);
size_t hle_impl_count();
void hle_for_each(void (*cb)(const HleImpl &, void *), void *user);

#define DISH_CAT2(a, b) a##b
#define DISH_CAT(a, b) DISH_CAT2(a, b)
#define HLE(dll, name, conv, fn) \
    static ::dish::HleRegistrar DISH_CAT(_hle_reg_, __COUNTER__)(dll, name, ::dish::hle_thunk<fn, ::dish::HConv::conv>, \
                                                                   ::dish::hle_pop<fn, ::dish::HConv::conv>(), ::dish::HConv::conv)

// ---- signature table (generated by runtime/tools/gen_sigs.py) -----------------------------------------------------
struct HleSig {
    const char *dll;
    const char *name;
    uint16_t ordinal;
    char conv;          // S, C, V, T, ? (unknown)
    int32_t pop;        // stack bytes popped by the callee (-1 unknown)
    uint32_t default_ret;
    uint32_t needed;
};
const HleSig *sig_find(const char *dll, const char *name);
const HleSig *sig_find_ordinal(const char *dll, uint32_t ordinal);
const HleSig *sig_table(size_t *n);

// ---- binding --------------------------------------------------------------------------------------------------------
struct StubInfo {
    const char *dll;
    const char *name;
    uint32_t pop;
    uint32_t default_ret;
    bool sig_known;
    uint64_t calls;      // atomic-ish counter (relaxed)
    bool logged;
};
// Resolve (dll, name|ordinal) to a thunk entry: implementation, else auto-generated stub. Returns the thunk index.
// `want_index` >= 0 forces the table slot (import i -> slot i); otherwise a dynamic slot is allocated (cached by name).
uint32_t hle_bind(const char *dll, const char *name, uint32_t ordinal, int64_t want_index);
// GetProcAddress for a system DLL: returns a thunk address, or 0 if the function is neither implemented nor a known import.
uint32_t hle_dynamic_proc(const char *dll, const char *name, uint32_t ordinal);
// Reports
size_t stub_count();
const StubInfo *stub_at(size_t i);
void stub_report(bool only_called);       // logs the unimplemented-import summary at INFO

// Helpers used by implementations
inline uint16_t *gw(uint32_t a) { return (uint16_t *)(guest_base + a); }
inline char *gs(uint32_t a) { return (char *)(guest_base + a); }

}  // namespace dish
