# Guest ABI: lifter ⇄ runtime contract

The binding definitions are in `include/guest_abi.h`. This file explains the rules. If they disagree, the header wins, and this file should be fixed.
Anything here marked **TBD** gets decided when the first real need appears. Neither side may invent its own version of a TBD item.

## 1. Repository layout

| Path | Owner | Content |
|---|---|---|
| `include/guest_abi.h` | lead | The contract. Shared by generated code and the runtime. |
| `lifter/` | lifter agent | Rust workspace with crates `ir`, `lift` and `cli`, plus TOML config in `lifter/config/`. |
| `runtime/` | runtime agent | C++20 runtime library and host executables, built with CMake. |
| `generated/` | build output (gitignored) | Lifter output: `<module>/shard_NNNN.c`, `dispatch.c`, `program.c`, `gen.h`. |
| `private/` | gitignored | Game-derived dumps, traces, Ghidra project. |

**Never commit game bytes or generated code.** The CI and public repo contain only tools.

## 2. Modules, linking and loading

- The lifter treats every recompiled module as one statically linked program:
  - `Dishonored.exe` and `Microsoft.VC90.CRT/msvcr90.dll`, `msvcp90.dll`.
  - Later: `binkw32.dll`, `PhysXLoader.dll`, `PhysXCore.dll`, `PhysXCooking.dll`, `PhysXExtensions.dll`, `NxCharacter.dll`.
  - APEX is optional; Ogg/Vorbis, wx and cudart are not used.
- The lifter chooses non-overlapping bases. The exe always uses its preferred base, 0x400000. Lifted code embeds absolute addresses for the chosen base.
- **CRT decision:** recompile `msvcr90`/`msvcp90`, so semantics are exact. That includes UTF-16 wide strings, MS `printf` formats, locale and file buffering. Hot functions are then overridden with native implementations. This is "scenario A" in `research/binary/api_surface.md`, plus overrides.
  - Overrides are lifter config entries mapping a guest function (by module export or address) to a host symbol with the same `GuestFn` signature, or to an intrinsic lowered at the call site with register arguments.
  - Override candidates: `memcpy`, `memset`, `memmove`, `strlen`, `wcslen`, `_wcsicmp`, `_stricmp`, `_CI*` math, `floor`, `ceil`, `_controlfp_s`.
- **Calls between recompiled modules** through an IAT (`call [imp]`) are resolved by the lifter to direct calls. The runtime still writes every IAT slot, because code also reads IAT values (function-pointer compares, `GetProcAddress` lookalikes).
- **Loader** (runtime, `guest_program`):
  1. Reserve the 4 GiB region.
  2. For each module, verify the SHA-256 of the user's file and map its headers and sections at `base`. Use the section permissions; `.bss` is zero-filled.
  3. If `base != preferred_base`, apply `.reloc`.
  4. Write the `iat[]` entries.
  5. Allocate and initialise PE static TLS if a module has a TLS directory.
  6. Create the main guest thread (§6).
  7. Call DLL entries in array order with `(hinst=base, DLL_PROCESS_ATTACH, 0)` via `guest_invoke`.
  8. Call the exe entry.
- **Game data stays the user's.** Section contents are read from the user's files at run time and are never compiled in.

## 3. Calling convention (see `GuestFn`)

- Every lifted function is a C function named `g_XXXXXXXX`, where `XXXXXXXX` is its guest address in upper-case hex. Addresses are unique across modules because the bases are fixed.
- At entry, `[esp]` is the return slot and the arguments are at `esp+4`.
- `ebx`, `esi`, `edi` and `ebp` are callee-saved and verified by the lifter. Functions that violate this use an extended signature that only the lifter knows about, plus a generic wrapper used for dispatch.
- **Indirect calls hand over the callee-saved registers:** before an indirect call, lifted code stores the `ebx`/`esi`/`edi`/`ebp` it holds in `GuestThread.ext_regs`; generic entries of extended-signature functions read their extra inputs from there (EH funclets entered with the parent frame's `ebp`). An indirect call counts as a read of `ebp`, so funclet trampolines like `_NLG_Call` pass it through.
- `eax`, `ecx` and `edx` are passed in. On return the caller gets `eax`, `edx` and `pop_bytes`. The caller computes its own `esp` as `entry_esp + 4 + pop_bytes`.
- Direct calls may use a lifter-private faster signature (for example, register arguments) if the generic `GuestFn` entry still exists for dispatch.
- **Host thunks**, meaning HLE imports and COM methods, have exactly the `GuestFn` signature. They read arguments from guest memory at `esp+4` and return with `guest_ret(eax, edx, pop)`. A stdcall thunk pops `4*nargs`; cdecl pops 0.
- **Host → guest calls** (WndProc, thread start routines, DllMain, SEH handlers, `qsort` comparators, CRT callbacks) go through the runtime helper `guest_invoke(target, conv, nargs, args...)`. It:
  1. pushes the arguments right-to-left on the current guest thread's stack;
  2. reserves the return slot and writes the sentinel `0xFFFFFFF0` (above `GUEST_THUNK_END`, never mapped);
  3. calls `guest_call`, and checks `pop_bytes` against `conv`.

## 4. x87 and SSE state

- **x87 → `double`:** the lifter keeps the x87 register stack in C locals.
- **Float returns:** a function returning in `st(0)` writes `guest_thread->st0_ret` before returning, and the caller reads it after the call. Host thunks returning float or double do the same.
  - The lifter may give known float-returning functions a private `double`-returning direct-call signature. The `st0_ret` write is still required on the generic entry.
- **`x87_cw` and `mxcsr`** live in `GuestThread`. Both are per thread, as on Windows. `_controlfp_s` and `fldcw` must be cheap, plain stores.

## 5. Memory model

- **Pointers:** a guest pointer is a `uint32_t`. Host code converts with `G(addr)` and converts back with `(uint32_t)(hostptr - guest_base)`. Host pointers are never stored in guest memory.
- **Wide strings:** guest `WCHAR` is `uint16_t` (UTF-16). Never use host `wchar_t` (32-bit on Apple and Linux). The runtime provides UTF-16 ⇄ UTF-8 helpers.
- **Ordering:**
  - Shared accesses use `GUEST_LD_SHARED`/`GUEST_ST_SHARED`, which are acquire/release on arm64.
  - Stack and `fs:` accesses use the `LOCAL` forms.
  - `lock`, `xchg`, `cmpxchg` and `xadd` use `__atomic_*` with `__ATOMIC_SEQ_CST`.
  - `mfence` is `__atomic_thread_fence(__ATOMIC_SEQ_CST)`.
  - A shared access with unknown alignment must use the lifter's unaligned slow path. Atomics on unaligned addresses are undefined behaviour.
  - The runtime's sync primitives (critical sections, events, `Interlocked*`, waits) imply full barriers.
- **Guest address-space allocation** (`VirtualAlloc`, heaps, guest stacks, TEBs, thunks) belongs entirely to the runtime. The lifter never allocates guest memory.
- **Low 64 KiB** is never mapped, so NULL dereferences fault.

## 6. Threads, TEB and TLS

- Each guest thread has:
  - a guest stack in guest memory: default 1 MiB, or as `CreateThread` requests;
  - a TEB page with `fs:[0]` (SEH chain), `fs:[4]`/`fs:[8]` (stack base and limit), `fs:[0x18]` (self), `fs:[0x2C]` (TLS array pointer), `fs:[0x34]` (LastError) and the `TlsSlots` area;
  - a `GuestThread` struct reached through `guest_thread`, which is host TLS.
- Lifted code reads `fs:` as `GUEST_LD_LOCAL(T, guest_thread->teb + off)`. The lifter hoists `guest_thread->teb` per function.
- **Host stack size matters.** Lifted code recurses on the host stack, and each guest frame uses host stack too. Every host thread that runs guest code must get at least **16 MiB** of host stack. On iOS, **guest code never runs on the UIKit main thread**; the guest "main thread" is its own pthread.

## 7. Dispatch and unknown code

- The lifter generates `guest_lookup` (a perfect hash over every entry), including EH funclets and `.reloc`-discovered stubs.
- `guest_call` is implemented by the runtime:
  - thunk range: index the thunk table;
  - the sentinel return address means "return to host";
  - otherwise use `guest_lookup`;
  - on a miss, call `guest_unresolved`.
- **Phase 1:** `guest_unresolved` logs the target, `esp`, the caller if known and a guest stack dump to `unresolved.log`, then aborts. **Later:** the IR interpreter (a Rust staticlib from `lifter/ir`) runs until it reaches a known entry. The logged addresses are fed back into the lifter config.

## 8. Exceptions

- The `fs:[0]` chain lives in guest memory and is maintained by lifted code itself.
- `RaiseException`, `RtlUnwind` and the hardware-fault path are implemented by the runtime. They walk the guest chain and call the lifted handlers with `guest_invoke` (cdecl, four arguments: `ExceptionRecord*`, `EstablisherFrame`, `ContextRecord*`, `DispatcherContext`). All structures live in guest memory.
- `RaiseException(0x406D1388)` (thread naming) is handled directly: set the host thread name and continue.
- **Dispatcher frames:** while a handler runs, the TEB head is a dispatcher frame `{Prev, Handler, establisher}` pushed by the runtime (Wine's `EXC_CallHandler`). MSVC's `_UnwindNestedFrames` re-links the saved head in front of the unwind target, so without it the target frame would point at itself. Its handler reports `ExceptionNestedException` / `ExceptionCollidedUnwind` as on Windows.
- **Non-local resumption (landing pads), implemented:** the lifter finds every catch continuation (FuncInfo catch funclets' `mov eax, imm; ret`) and `__except` body (SEH4 scope tables) and makes them roots of the owning function, not functions of their own. After the owner's prolog saves esp (`[ebp-10h]` C++ / `[ebp-18h]` SEH4) it registers a `GuestPad` (`guest_pad_push`, `_setjmp` in the lifted frame, keyed by ebp). When guest code jumps to an address that is no function entry (`_JumpToContinuation`, `_EH4_TransferToHandler`) and a registered frame matches the ebp handed over in `GuestThread.ext_regs`, the runtime `longjmp`s into that function, which reloads esp from the slot and continues at the continuation. Known gap: msvcp90's catch handler stubs are not referenced by immediates, so its continuations are not found yet.

## 9. Build flags for generated code

`-std=c11 -ffreestanding -O2 -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -fvisibility=hidden`, plus:
- arm64 iOS: `--target=arm64-apple-ios15.0 -mcpu=apple-a15`
- x86-64 Linux dev build: `-march=x86-64-v2`

Generated code is freestanding. The only include is `guest_abi.h` (plus `<arm_neon.h>`/`<immintrin.h>`-style compiler headers). No libc headers are allowed, so it compiles without an Apple SDK.

## 10. Resolved decisions (from `docs/abi_change_requests.md`)
1. **x87 operands of `_CI*` helpers:** the lifter lowers direct calls to `_CIpow`, `_CIfmod`, `_CIatan2`, `_CIsqrt`, `_CIsin`, `_CIcos`, `_CItan`, `_CIasin`, `_CIacos`, `_CIatan`, `_CIexp`, `_CIlog`, `_CIlog10` as **call-site intrinsics**. They call plain-double host functions (`dish_ci_pow(double base, double exp)` etc., declared in `runtime/include/dish/ovr.h`) with the x87 stack locals, so there is no memory round-trip. The runtime's interim convention stays as a fallback for indirect calls only: st(0) in `st0_ret`, st(1) in the runtime block. The header is unchanged.
2. **Data imports:** only possible from HLE modules. The CRT is recompiled, so they do not occur. If one appears, the lifter reports it and the header gets a `GUEST_IAT_DATA` kind.
3. **`host_tid`** is the guest-visible thread id, as the runtime implemented it. It is not an OS tid.
4. **Module ranges:** `image_size` must equal SizeOfImage, and ranges must not overlap each other or the reserved regions. The lifter validates this at generation time.
5. **Import calls:** lifted code reaches HLE imports through `guest_call(iat_value, ...)` or via a direct call to the thunk's host function. Both are allowed; the direct form is preferred for hot imports. `ovr_*` overrides are always called directly.
6. **Non-local resumption:** adopted as the runtime proposed. The lifter emits per-try-region `setjmp` landing pads and registers them with `guest_register_landing_pad(frame_addr, jmp_buf *)`, and `RtlUnwind` `longjmp`s to the pad. This is implemented when the first site is hit.
