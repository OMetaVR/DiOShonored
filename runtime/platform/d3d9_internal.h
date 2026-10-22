// d3d9 thunk layer internals. Include order matters: the DXVK-native headers define Windows type names (DWORD, HRESULT, WAIT_*, S_OK ...),
// so this header must not be combined with dish/sync.h or dish/thread.h in the same translation unit.
#pragma once
#include <d3d9.h>
#include "d3d9_backend.h"
#include <cstdint>
#include "guest_abi.h"
#include "dish/com.h"
#include "dish/log.h"
#include "dish/util.h"

namespace dish {

enum D3d9Kind { K_Unknown, K_D3D9, K_Device, K_SwapChain, K_Surface, K_Volume, K_Texture, K_CubeTexture, K_VolumeTexture, K_VertexBuffer,
                K_IndexBuffer, K_VertexDecl, K_VertexShader, K_PixelShader, K_StateBlock, K_Query, K_COUNT };

// Argument access inside a thunk: `esp` is the thunk's first parameter. A(0) is `this`.
#define A(i) (*reinterpret_cast<const uint32_t *>(guest_base + esp + 4 + 4 * (i)))
#define AF(i) (*reinterpret_cast<const float *>(guest_base + esp + 4 + 4 * (i)))

// One-frame call trace (DISH_D3D9_TRACE_FRAME=<n>): logs every thunk call with its raw guest arguments during frame n.
// DISH_D3D9_BT_ZEROMAT=<n> calls d3d9_debug_hook (a gdb breakpoint target) for the first n vertex-constant uploads of a 4-row matrix with a
// zero 2D scale.
extern bool g_d3d9_trace_on, g_d3d9_watch;
void d3d9_trace(const char *what, uint32_t esp, int nargs);
void d3d9_debug_hook();
#define D3D9_TRACE(what, nargs) do { if (__builtin_expect(g_d3d9_trace_on | g_d3d9_watch, 0)) d3d9_trace(what, esp, nargs); } while (0)

// Wrap a host COM object (taking over one host reference) in a guest-visible object; returns the existing wrapper when there is one
// (the extra host reference is dropped and the guest reference count is incremented).
uint32_t d3d9_wrap(void *host_iface, D3d9Kind kind);
HWND d3d9_host_hwnd(uint32_t guest_hwnd);
uint32_t d3d9_guest_hwnd_of(HWND host);
GuestRet d3d9_unsupported(const char *what, int pop);
GuestRet d3d9_bad_this(const char *what, int pop);
GuestRet d3d9_exception(const char *what, int pop);
GuestRet d3d9_QueryInterface(uint32_t esp, uint32_t, uint32_t, uint32_t);
GuestRet d3d9_AddRef(uint32_t esp, uint32_t, uint32_t, uint32_t);
GuestRet d3d9_Release(uint32_t esp, uint32_t, uint32_t, uint32_t);

// Marshalling helpers shared with d3d9_manual.cpp
void d3d9_pp_from_guest(uint32_t g, D3DPRESENT_PARAMETERS *h);
void d3d9_pp_to_guest(const D3DPRESENT_PARAMETERS &h, uint32_t g);
uint32_t d3d9_stage_alloc(uint32_t size);
void d3d9_stage_free(uint32_t guest, uint32_t size);
IDirect3D9 *d3d9_load_and_create(UINT sdk);   // picks the active backend (see D3d9Backend) and calls its create()

// ---- backend seam (docs/runtime_d3d9_backend.md): see d3d9_backend.h

// Statistics (tests/benchmarks)
struct D3d9Stats { uint64_t locks = 0, lock_bytes_in = 0, lock_bytes_out = 0, unsupported_calls = 0; };
D3d9Stats d3d9_stats();
void d3d9_define_interfaces();                       // idempotent
uint32_t d3d9_slot(const char *iface, const char *method);   // vtable index by name, -1 if unknown
const char *d3d9_dxvk_library();                     // path of the loaded DXVK library ("" if none)

}  // namespace dish
