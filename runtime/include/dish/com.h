// COM thunk layer: guest vtable -> thunk -> host object.
//
// A guest COM object is 16 bytes of guest memory: { u32 vtbl; u32 host_id; u32 refcount; u32 cookie }.
// Each interface has ONE shared guest vtable (an array of thunk addresses). Every vtable slot is a normal thunk-table entry whose
// GuestFn is the method's host function, so a call is: lifted code -> guest_call -> thunk table -> method(esp,...). The method reads
// `this` from [esp+4], maps it to the host object with com_host<T>(this) (one array lookup, no locking, no allocation) and pops its own
// arguments (COM methods are stdcall). Nothing on the call path allocates.
//
// d3d9 / dsound / dinput8 implementations (phase 2) register their interfaces through com_define_interface.
#pragma once
#include <cstdint>
#include <cstddef>
#include "guest_abi.h"
#include "dish/vm.h"

namespace dish {

struct ComMethod {
    const char *name;
    GuestFn fn;          // reads args from the guest stack; `this` is the first stack argument
};

// Build (once) the shared guest vtable for `iface`. Returns the guest address of the vtable. Methods are given by the full
// ordered list, including IUnknown's QueryInterface/AddRef/Release in slots 0..2 when the interface derives from it.
uint32_t com_define_interface(const char *iface, const ComMethod *methods, size_t n);
uint32_t com_vtable_of(const char *iface);          // 0 if unknown

// Create a guest object for a host pointer. The returned guest address is what the guest sees as the interface pointer.
uint32_t com_new_object(uint32_t vtbl, void *host_obj, void (*on_destroy)(void *) = nullptr);
// Fixed-capacity object table: lookups are two dependent loads and a compare, no lock (see com.cpp for the writers).
struct ComObjEntry { void *host; void (*destroy)(void *); uint32_t guest; bool live; };
constexpr uint32_t kComMaxObjs = 1u << 16;
extern ComObjEntry g_com_table[kComMaxObjs];
inline void *com_host_object_raw(uint32_t guest_this) {
    const uint32_t *w = reinterpret_cast<const uint32_t *>(guest_base + guest_this);
    if (__builtin_expect(w[3] != 0xC0DEC0DEu, 0)) return nullptr;
    uint32_t id = w[1];
    return id < kComMaxObjs && __atomic_load_n(&g_com_table[id].live, __ATOMIC_ACQUIRE) ? g_com_table[id].host : nullptr;
}
template <class T> inline T *com_host(uint32_t guest_this) { return static_cast<T *>(com_host_object_raw(guest_this)); }
// Reference counting on the guest-visible object; destroys the host object (calls on_destroy) when it reaches zero.
uint32_t com_addref(uint32_t guest_this);
uint32_t com_release(uint32_t guest_this);
uint32_t com_refcount(uint32_t guest_this);
size_t com_live_objects();

// Argument access inside a method: stack slot i (0 = this).
inline uint32_t com_arg(uint32_t esp, unsigned i) { return *(const uint32_t *)(guest_base + esp + 4 + 4 * i); }

}  // namespace dish
