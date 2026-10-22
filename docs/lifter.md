# Lifter (Phase 1)

`lifter/` is a Rust workspace (iced-x86, rayon) that turns `Dishonored.exe`, `msvcr90.dll` and `msvcp90.dll` into freestanding C that follows `docs/guest_abi.md`.
"Measured" = produced by a command in section 2; "inferred" = reasoning.

## 1. Architecture
| Crate | Content |
|---|---|
| `ir` | `repr.rs` IR (typed places, lazy-flag producers/consumers, x87/SSE/string ops), `lower*.rs` x86 -> IR (**instruction semantics are defined only here**; vector semantics live in the generated `gen.h`), `util.rs` use/def visitors. |
| `lift` | `image` (PE loader, rebasing), `discover` (entries, jump tables), `func` (function formation + lowering, tail calls, helper inlining), `x87` (static stack-depth resolution), `absint` (stack/register-identity analysis, memory classes), `flags` (reaching definitions, producer classes), `liveness`, `pipeline` (whole-program fixpoint: pops, extended signatures, ecx outputs, noreturn), `emit*` (C emission), `output` (shards, perfect-hash `dispatch.c`, `program.c`), `genh`/`simd.h.in` (`gen.h`, portable SIMD helpers), `driver`. |
| `cli` | `lifter lift`, `stats`, `dump --addr ...` (IR + analysis facts), `lift-blob` (synthetic module for the tests). |

Pipeline: discovery (seeds: entry, exports, config, `.reloc` pointers minus jump-table slots, call targets, prologue scan of gaps) -> per-function CFG (entry + reachable blocks; a jump/fallthrough into another entry is a tail call, unless the stack is not clean there, then the code is duplicated) -> IR -> x87 resolution -> abstract interpretation -> flag classes -> C. Functions run in parallel (rayon); output is deterministic.

Key design points
* Guest registers are C locals; `esp` is a local and the guest stack is real memory. Direct calls use the generic `GuestFn` signature (`g_XXXXXXXX`), the callee's constant `ret N` is folded into the caller.
* **Lazy EFLAGS with reaching-definition fusion**: every flag producer records operands; producers that reach a common consumer share variables ("class"); a class with one producer kind keeps (a, b, r) and consumers become single comparisons (`cmp; jl` -> `(int32_t)a < (int32_t)b`); mixed classes materialise only the consumed bits. Dead producers emit nothing.
* **Memory classes**: accesses whose base is stack-derived (tracked abstractly) or `fs:` are LOCAL, everything else SHARED (acquire/release on arm64). Shared accesses of unknown alignment go through `LX_LD_SU*/LX_ST_SU*` (see 6). Locked ops, `xchg mem`, `cmpxchg`, `xadd` are `__atomic_*` SEQ_CST.
* **x87 -> double**, depth resolved statically per program point (`f0..f7` locals), `st0_ret` handoff at calls (a call pushes `gt->st0_ret` only when later x87 code needs more than the depth held across the call; LTCG code keeps values on the stack across calls to integer helpers) and at returns, `fnstsw`/`fcom` via a status-word local, rounding from `gt->x87_cw`.
* **SSE/MMX -> `v128`** (clang vector extensions + SSE2 intrinsics for movemask), helpers `lx_<name>` in `gen.h`.
* **Register-effect verification** (`absint`): tracks `esp/ebp` as entry-relative offsets and which values are the caller's `ebx/esi/edi/ebp`. A function that reads one of them as input, or returns it modified (or reloaded through a stack it cannot track), gets an **extended signature** `LxExt g_X_x(esp,eax,ecx,edx,<ext regs>)` plus a generic wrapper `g_X` that reads the inputs from `lx_ext_*`. A function that returns a register also takes it as input (so unmodified paths return the caller's value). `ecx` live after a direct call makes the callee return `ecx` (LTCG code relies on this). Calls whose popped size is unknown use the callee's `ret_addr + pop` (dynamic `r0 >> 32`).
* **Helper inlining**: tiny straight-line functions that manipulate the stack (SEH4/EH prolog+epilog style, 61 found) are inlined at their call sites so the caller's stack stays analysable.
* **Overrides** (config): `ovr_<name>` host calls (GuestFn); `_CIpow/_CIfmod/_CIatan2` are call-site intrinsics on the x87 stack (`dish_ci_*`), the other `_CI*` are libm builtins; their lifted bodies still exist for indirect calls. **Hooks**: replace an address by a host symbol.

## 2. How to run
```
lifter/regen.sh                      # regenerate generated/ (cargo build + lift of all three modules; ~1.5 min)
lifter/regen.sh --trace              # also emit generated/trace.c: LX_TRACE=1 prints every function entry
lifter/target/release/lifter stats  --config lifter/config/dishonored.toml --game <Dishonored dir>/
lifter/target/release/lifter dump   --config ... --game ... --addr 428b50      # IR, abs-int facts, signature
lifter/tests/ccall.sh x86|arm64      # compile every shard with the docs 9 flags, error summary + per-shard wall time
lifter/tests/build_difftest.sh       # lift WITHOUT overrides into private/difftest and link the diff driver
tools/.venv/bin/python3 lifter/tests/difftest.py funcs --n 1500 --per 3 --seed 7   # real functions vs Unicorn
tools/.venv/bin/python3 lifter/tests/difftest.py insn  --n 16                      # 1289 synthetic instruction templates
lifter/tests/devrun.sh [gendir]      # compile at -O1, link with build/lifter_rt runtime objects, run the Linux host
```
Outputs: `generated/<module>/shard_NNNN.c` (~40k x86 instructions each), `dispatch.c`, `program.c`, `gen.h`, `lx_hooks.c`, `imports.json`, `report/{functions,iat,traps,unsupported,x87}.tsv`.

## 3. Config (`lifter/config/dishonored.toml`)
Top-level (before any table): `noreturn` (`"module!export"` or `"0xADDR"`), `noreturn_imports` (`"dll!name"`), `shard_instructions`. `[[module]]`: `name, relpath, base, sha256, entries, not_entries, data_ranges`. `[[override]]`: `module, export|addr, host, kind = guestfn|x87_1|x87_2, pop`. `[[hook]]`: `module, addr, host, pop, why`. Unknown keys are rejected. No game bytes; addresses and hashes only. (Bases: exe 0x400000, msvcr90 0x78520000, msvcp90 0x78480000 = preferred bases; ranges are validated against each other, `[0,64K)` and `[0xFFEF0000, 4G)`.)

## 4. Coverage (measured, `lifter stats`)
* 94,970 functions (exe 85,843; msvcr90 3,976; msvcp90 5,150), 4,003,382 x86 instructions, 102 shards, 61 stack helpers inlined, 26 auto-detected noreturn functions.
* **Instruction coverage**: 1,527 instruction sites in 498 functions are not lifted (0.04%) and become `guest_unresolved(addr)` traps; 17,491 trap blocks in 3,741 functions come from undecodable / non-code bytes reached by discovery. Reasons for the 1,527 (`report/unsupported.tsv`): far `jmp/call` operands 631 and `push seg` 331 (decode noise), 16-bit `push/pop` 224, `mov` with segment registers 121, segment overrides 98+47, 16-bit addressing 42, `xlat` 19, `rcl/rcr` by cl 7, `fnstenv` 3. Not lifted by design: `fnsave/frstor/fldenv`, `cmpxchg8b`, `fyl2xp1`, `fxtract`, `ffree`, `fincstp/fdecstp`, BCD, privileged/segment instructions. `rep movs/stos` are loops, not `memcpy`.
* **ABI verification**: stack analysis consistent in 82,421 functions; extended signatures: 52,027 take and 31,724 return callee-saved registers (exe: LTCG code, EH funclets take `ebp`), 4,068 return `ecx`; 16,792 functions have a dynamic pop; 2,913 indirect call sites need `ecx` after the call (cannot be satisfied: reported, none hit yet). 246 bad tail edges.
* `imports.json`: 434 HLE imports, 379 with a known calling convention and argument size (from `runtime/hle/sigs_generated.inc`). No data imports from non-recompiled modules were detected (all IAT slots are used as call/jump targets in the lifted code).

## 5. Differential tests (measured)
Original code in Unicorn (x86-32, CORE2DUO) versus the lifted C (x86-64, `-O2`, fake runtime `tests/diffdrv.c`, fork per case, page-protection write tracking). Same memory snapshot (module images with IAT filled like the loader, deterministic heap/stack/TEB arenas, unmapped pages are created lazily with deterministic content), same registers and stack, HLE imports replaced by identical record/replay stubs. Compared: eax, edx, popped bytes, number of stub calls, st(0) return, every written page (callee-private stack excluded).
* **Synthetic instruction tests** (`difftest.py insn`, 1,289 templates x 16 random cases; flags captured exactly where architecturally defined): **99.7% (20,545 / 20,605 executed cases)**. 100% for ALU (flags), shifts/rotates, mul/div, bit ops, setcc/cmovcc/jcc, string ops, atomics, stack ops, x87 arithmetic/compare/convert with all rounding modes, and every SSE/SSE2/MMX template (arith, logic, compare, convert, shuffle, shift, move). Failures: `x87-trans` 56 (host libm vs x87 microcode for fsin/fcos/fptan/fsincos/fyl2x with random arguments, including |x| > 2^63 where x87 leaves the operand unchanged), `fprem/fprem1` 3 (hardware does partial reduction when the exponent difference is >= 64, the lifted code always reduces fully), `fxam` 1 (NaN class).
* **Real functions** (`difftest.py funcs`, 1,500 sampled functions x 3 cases): 322 ok / 11 fail = 96.7% of the executed cases; only about 7% of the random cases execute to completion (random state rarely satisfies pointer chains), so this is a smoke test, not coverage. Open differences: `0x8ebe50`/`0xac2250` (memory page differs; callee `0x8ebd60` has an inline EH frame, root cause not found), `0x784be329` (control flow diverges after a stub call), `0x99d300` (fault in the lifted code). The instruction tests were what caught the real bugs (dropped `fxch`, flags recorded after the destination was overwritten for `inc/dec/neg/xadd`, MMX pack/unpack layout).
* By category over the real-function sample: int-only 100% (45), tail 100% (15), fs 100% (17), icall 100% (32), sse 100% (27), x87 100% (27), shift 100% (20), string 85% (17+3).

## 6. Code quality (measured, arm64-apple-ios15.0 -mcpu=apple-a15, clang 22.1.8)
* All 102 shards + dispatch.c + program.c compile with **0 errors** for `x86_64-linux (-march=x86-64-v2)` and `arm64-apple-ios15.0` (no SDK needed).
* **Code size**: 58.2 MB of arm64 `__TEXT` for 4.00 M x86 instructions = **3.6 arm64 instructions per x86 instruction** (x86 `.text` of the three modules is about 13 MB). The Phase 0 prototype predicted 2.1 for integer code; the difference is the SHARED access policy, extended-signature struct returns (36 bytes through memory), x87/SSE and flag materialisation. Policy knobs (C compile time, measured on `Dishonored.exe/shard_0038.c`): default (alignment test + LDAPR/STLR + inline slow path) 603,820 B; `-DLX_ASSUME_ALIGNED` 425,064 B (-30%, a misaligned shared access then faults on arm64); `-DGUEST_RELAXED_ORDERING` 343,276 B (-43%). Choosing the policy is the main open performance decision.
* **Compile time**: 12 shards in parallel took 2,692 s of summed wall time for 104 files; the slowest shards run 25 s (`Dishonored.exe/shard_0038.c`, measured alone, arm64 -O2) and 15 s (`msvcr90.dll/shard_0001.c`). The Linux x86-64 build of all shards takes about 4.5 minutes of wall time with `-g` (runtime build, 12 cores).
* **Dedicated base register**: `-ffixed-x27` together with `register uint8_t *lx_gb __asm__("x27")` (`-DLX_BASE_REG`) is accepted by clang for `arm64-apple-ios15.0`; shard_0038 shrinks only 0.4% (601,588 vs 603,820 B). It requires the runtime to load x27 on every entry into lifted code, so it is not enabled. The default is a per-function `uint8_t *const mb = guest_base`.
* Per-TLS cost: functions that use `fs:` or x87 control read `guest_thread` (Apple arm64 TLV access) once per function.

## 7. Running the real program (Linux host, `-O1` objects)
`lifter/tests/devrun.sh`: the three modules load, `DllMain(msvcr90)` and `DllMain(msvcp90)` complete, the exe entry runs the CRT startup and reaches UE3 engine initialisation (log lines `Init: Version: 9403`, `Init: Compiled (32-bit): Feb 17 2022`, base directory, DXVK d3d9 backend loaded, `LoadLibrary` probes of nvapi/ddraw), then stops with the game's own message box "Failed to Initialize PhysX Hardware Acceleration" because `PhysXLoader.dll` (delay-loaded, not a recompiled module yet) is not available: `NxGetCookingLib` is an unimplemented stub (runtime-side / next module). Bugs found on the way and fixed in the lifter: top-level TOML keys silently swallowed by the last `[[module]]` (noreturn lists were ignored), callee-saved registers lost across SEH4 prolog/epilog helpers (now inlined, and "reloaded through an untracked stack" counts as modified), extended-return registers not initialised on unmodified paths, direct calls to non-code targets, dropped `fxch`.

## 8. Known gaps
* Not lifted: see section 4. Landing pads for non-local EH resumption (about 25 sites) are not emitted: the `__except`/`catch` bodies are separate lifted functions that return a continuation address; resumption by the CRT's `_JumpToContinuation` ends in `guest_unresolved`.
* Indirect calls to functions that need extended inputs get them only through `lx_ext_*` (zero unless the caller sets them). `_CallSettingFrame` must set `lx_ext_ebp` (config key `ext_pass` is parsed but not implemented).
* xmm registers are not part of any signature. LTCG code may keep xmm values live across calls to leaf helpers; not detected yet (only `ecx` is).
* `rcpps/rsqrtps` are exact (not the 12-bit estimate); `cvtps2dq` assumes round-to-nearest (MXCSR rounding mode is stored but not honoured); `fsin/fcos/...` use libm; `fprem` reduces fully; 80-bit values are narrowed to double.
* Stack accesses in functions whose frame cannot be tracked (11,850 functions) are classed SHARED (slower, still correct). Pops of 16,792 functions are dynamic.
* NEON versions: only portable vector-extension code is generated for arm64 (it compiles and clang maps it to NEON); the SSE2 intrinsics are used for movemask on x86-64. The arm64 code is compile-tested only.
* String instructions use plain accesses (no ordering). Atomics on unaligned addresses are undefined (per the ABI).

## 9. Runtime-side findings (not fixed here)
1. `runtime/hle/k32_process.cpp raise_exception`: for the thread-naming exception 0x406D1388 `lpArguments` already points at the THREADNAME_INFO (dwType, szName, dwThreadID, dwFlags); the code dereferences `args[0]` as a pointer first and faults at guest address 0x1000. The config hook `lx_hook_set_thread_name` (weak no-op in `generated/lx_hooks.c`, `[[hook]]` in the config) works around it for 0x416580; remove the hook when fixed.
2. `build/runtime` was reconfigured by another process with `DISH_USE_GENERATED=OFF` while the lifter ran; the lifter uses `build/lifter_rt` and `lifter/tests/devrun.sh`. `-DDISH_GENERATED_DIR` must be an absolute path.
3. Early in the session `runtime/platform/d3d9_layer.cpp` failed a `static_assert` (`D3DVOLUME_DESC` 28 vs 32); it built later without changes on our side.
4. `PhysXLoader.dll` is not available (see section 7). `steam_api!SteamAPI_Init/SteamApps` are unimplemented stubs.
