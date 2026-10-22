# ABI change requests (from the runtime agent)

`include/guest_abi.h` is frozen for the runtime agent. These are the points where the runtime had to pick an interim behaviour. None blocks Phase 1.

## 1. x87 register arguments across a call boundary (needed for `_CI*` helpers)

`docs/guest_abi.md` section 4 defines how an x87 value is *returned* (`GuestThread::st0_ret`) but not how x87 values are *passed in*. The CRT helpers `_CIpow`, `_CIfmod`, `_CIatan2` take two operands in st(1)/st(0); `_CIsqrt`, `_CIsin`, ... take one in st(0). The hot ones are 11.6 M (`_CIfmod`) and 3.2 M (`_CIpow`) calls in the 8-minute trace.

**Interim convention implemented in `runtime/overrides/ovr_crt.cpp` (and tested):**
- st(0) is read from `guest_thread->st0_ret` (so the caller stores its st(0) there before the call, exactly as a float-returning callee would).
- st(1) is read from the first `double` of the runtime-private block: `*(double *)guest_thread->runtime`. The runtime guarantees (static_assert in the tests) that `RtThread::st1_arg` is the first member of the object `GuestThread::runtime` points to.
- The result is written to `st0_ret`.
- Argument meaning: `_CIpow(st1 = base, st0 = exponent)`, `_CIfmod(st1 = x, st0 = y)`, `_CIatan2(st1 = y, st0 = x)`.

**Proposed header change (replaces the hack):** add `double st1_arg;` to `GuestThread` after `st0_ret`, bump `GUEST_ABI_VERSION`, and document in section 4 that x87 arguments are passed in `st0_ret` / `st1_arg`. Alternative that needs no header change: lower these helpers as intrinsics at the call site and call the plain-double host functions `dish_ci_pow`, `dish_ci_fmod`, `dish_ci_atan2` declared in `runtime/include/dish/ovr.h` (for the one-operand helpers use `libm` directly; the generated code is freestanding but links against libm in the final link).

## 2. Data imports cannot be thunks

`msvcr90` exports data (`_iob`, `_acmdln`, `_adjust_fdiv`) and `msvcp90` exports `std::wcerr`. A thunk table entry is code, so an IAT slot that must point at *data* cannot be served by the runtime. With the CRT recompiled these never become HLE imports (the IAT slot gets a `GUEST_IAT_GUEST_ADDR`). If a future configuration binds the CRT as an HLE module, the header needs a third IAT kind (`GUEST_IAT_DATA`: runtime allocates N bytes of guest memory and writes its address).

## 3. `GuestThread::host_tid` is used as the guest thread id

The runtime stores the guest-visible thread id (the value of `fs:[0x24]` and `GetCurrentThreadId`) in `host_tid`. It is not an OS thread id. Please keep it that way or rename the field.

## 4. Module bases must be known before the first guest thread exists

`runtime_init` reserves `[base, base + image_size)` for every module in `guest_program.modules` before it creates the main guest thread (otherwise the first-fit stack allocation lands on 0x400000). Consequence for the lifter: `image_size` in `GuestModuleDesc` must be correct (SizeOfImage), and module ranges must not overlap each other or `[0, 64K)` / `[0xFFEF0000, 4G)`.

## 5. Calls into thunks go through `guest_call` (assumption)

The runtime assumes lifted code reaches every import through `guest_call(target, ...)` with the IAT value (never a direct C call to a host function), and that for stdcall thunks it advances its own esp by `4 + pop_bytes`. Unknown-signature stubs pop 0 bytes (flagged in the log as `[signature unknown, assuming cdecl]`).

## 6. Non-local resumption (docs section 8, TBD)

Still unimplemented. `RaiseException` walks the guest `fs:[0]` chain and calls the handlers with `guest_invoke` (verified in `test_misc`), but a handler that wants to unwind and resume in the middle of a lifted function (about 25 `__except`/`catch` sites) cannot be honoured: the runtime logs and terminates. Proposal when the first site is hit: the lifter emits `setjmp` landing pads per try-region and registers the `jmp_buf` in a per-frame table keyed by the registration address; `RtlUnwind` then `longjmp`s to the matching pad. The runtime side needs only a `guest_register_landing_pad(frame_addr, jmp_buf*)` hook.

## Lifter requests (no header change required)
Everything below is implemented inside generated code or as host symbols; `include/guest_abi.h` is unchanged.
1. `lx_ext_ebx/esi/edi/ebp` (thread-local, defined in `generated/dispatch.c`): inputs of *extended-signature* functions when they are reached through `guest_call` / `guest_lookup`. Lifted code that calls such functions directly passes the registers as parameters. Zero unless a caller sets them. A runtime helper that enters a funclet with a specific `ebp` (the C++ EH unwinder, `_CallSettingFrame`) must set `lx_ext_ebp` before `guest_call`. Not needed until the first unwind runs.
2. Host symbols the generated code references: the `ovr_*` names in `lifter/config/dishonored.toml` (GuestFn signature, `pop = 0`), `dish_ci_fmod/pow/atan2(double, double)`, and `lx_hook_set_thread_name` (weak default in `generated/lx_hooks.c`; see the runtime-side finding in docs/lifter.md section 9). The unary `_CI*` helpers are lowered to compiler builtins (libm), not host symbols.
3. `GuestThread.x87_cw`, `.mxcsr`, `.st0_ret`, `.teb` are used as specified. `fninit` writes `x87_cw = 0x37F`.
