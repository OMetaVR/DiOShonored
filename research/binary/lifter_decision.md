# Spike 0.5: lifter decision for `Dishonored.exe` (x86-32 MSVC9, 12.1 MB `.text`) to C to arm64

**Recommendation: write our own lifter. Do not fork M-HT/SR. Language: Rust + `iced-x86`. Output: C (one function per guest function) compiled by clang
for `arm64-apple-ios`. Borrow design ideas from XenonRecomp (hashed indirect-call table, TOML config, weak-alias patch hooks, sharded output).**

"Measured" below means measured by me in this session (scripts in `tools/analysis/`, numbers reproducible from `private/proto`).
"Inferred" means reasoning or extrapolation. The prototype was compiled but **not executed**: it measures generated-code size/shape, not correctness.

## 1. Evidence

### 1.1 M-HT/SR (cloned to `third_party/SR`, commit ac690dd, read from source)
- SRW (the Windows-PE recompiler) is about 22.5k lines of C/headers (including the 7.8k-line `llasm` instruction translator and a 3.1k-line helper header), plus the D tool `llasm.d` (5.4k lines).
  It targets **retro 32-bit games**: its only Windows examples are Septerra Core (a 729 KB exe) and three Battle Isle 3 modules (the largest 40 KB).
- **Output is assembler, not C**: NASM x86 / x64, or `.llasm` text that a separate tool written in **D** (`llasm/llasm.d`, 5.4k lines) turns into LLVM IR.
  There is no direct arm64 backend for Windows executables (arm64 comes via LLVM IR).
- **Input is hand-curated**: a per-game directory of `.sci` files (`external_procedures`, `instruction_flags`, `fixup_interpret_as_code`, `noret_procedures`,
  `ignored_areas`, `displaced_labels`, replacements) plus `relocations.csv` (21,588 lines for a 729 KB exe; we have 608,195 relocations).
  `instruction_flags.sci` carries **hand-written flag annotations per jump label**. This does not depend on IDA output, but it depends on a human per binary.
  Decoder: udis86 1.7.
- Handled mnemonics in the llasm translator: about 180 distinct (`case UD_I...`): the integer core, x87 (about 50), string ops, `setcc`, `shld/shrd`, `bsr`, `bswap`.
  **No SSE, MMX, `cmovcc`, `lock`, `cmpxchg`, `xadd`** (grep of `SR_full_llasm_instr.c`). We have 297,180 SSE and 2,863 `cmov` instructions.
- Machine model (`SR/llasm-support/llasm_cpu.h`): a `_cpu` struct in memory with `eax..edi`, **a materialised `eflags` word**, an x87 `double _st[8]` array and
  `_pointer_offset` for 64-bit hosts. My prototype mode N reproduces this style (below): 2.7x more arm64 instructions than lazy flags with register locals.
- No threads, TLS, SEH or exception support anywhere in SRW (grep for "thread" in `SRW/*.c` gives 0 hits); no runtime dispatch fallback for unresolved indirect jumps was found.
- Licence MIT/GPL-compatible; reuse of ideas is fine, but the architecture does not fit.

Verdict: SR is a good existence proof and a useful reference for the PE-loader/fixup handling and the x87-as-`double` choice, but **it would need a new decoder, an SSE/x87-register
backend, automatic flag/ABI/jump-table analysis, threads/SEH, a C backend and a sharded, parallel pipeline**. That is the lifter, not a patch to it.
The scale gap is about 17x in code size (12.1 MB vs 0.73 MB for Septerra Core) and relocations (608k vs 21.6k lines), for a workflow that is manual per binary.

### 1.2 XenonRecomp (cloned to `third_party/XenonRecomp`, README and source skimmed)
Relevant ideas, all adopted or adaptable:
- One C++ function per guest function; CPU state passed as an argument; memory accessed as `base + guest_addr`; PPC registers become locals where the ABI allows (their `*_as_local` options gave the biggest gains).
- **Indirect calls** go through a lookup table built at load time (guest address -> host function, perfect-hash-like "address * 2" direct table).
- **Jump tables** are detected statically (`XenonAnalyse`) and emitted as real `switch`es from a TOML file; function boundaries come from `.pdata` plus analysis. Same idea as our `.reloc`-driven discovery.
- Patch mechanism: every lifted function is a weak alias so a hand-written replacement can override it; "mid-asm hooks" at instruction addresses.
- Known gaps (same as ours): no exception support, MMIO unfinished. `setjmp/longjmp` redirected to native implementations.
- It does not need an EFLAGS model (PPC has CR fields), so our lazy-flags design is new work either way.

### 1.3 Other options (not evaluated hands-on)
Generic LLVM-IR lifters (remill/McSema class) model the whole CPU state in memory and rely on LLVM to clean it up; 3.7 M instructions with a `State` struct per function is the same
shape as SR's mode N and would be a research project to optimise. A Ghidra p-code route (decompiler output) is not meant for 85k functions of exact-semantics code. Not recommended.

### 1.4 Facts about the binary that drive the design (from `code_discovery.md`)
85,755 functions (median 38 bytes, 21,949 <= 16 bytes); 36,093 `call reg` (virtual) and 662 switch tables (all in `.text`, all resolvable through `.reloc`); about 59% of functions have no `ebp` frame;
`ecx`-in (thiscall) in 56%; 7,888 EH-framed functions and 8 SEH4 functions; SSE 8.1% and x87 2.9% of instructions; no self-modifying code seen; GNatives is a 4,096-entry table filled at static-init time.

## 2. Prototype measurements (mode comparison on real code)

`tools/analysis/proto_lift.py` lifts a random sample of real functions (3-120 instructions) that use only an integer subset (no SSE/x87/partial regs other than `al/cl/dl/bl`, no `fs:`, no indirect `jmp`;
62.9% of candidates qualified, the rest are listed by reason in `tools/analysis/out/proto_lift_stats.json`) in several styles and compiles each with `clang --target=aarch64-apple-ios -O2 -S`.
Sample: **2,000 functions, 39,759 x86 instructions**. All styles use the same guest-memory model, faithful guest stack, and a register-passing call convention for lifted functions.

| Mode | Style | arm64 instructions | per x86 instruction | loads / stores |
|---|---|---|---|---|
| N | SR-like: registers in a `cpu` struct, `EFLAGS` materialised after every flag-writing instruction | 227,166 | **5.71** | 45,941 / 45,510 |
| L0 | locals + lazy flags, but `g_base` global reloaded after each store, 4x32-bit `Ret` struct, return-address store at each call | 104,602 | 2.63 | 26,715 / 17,414 |
| **L** | locals + lazy flags with reaching-definition fusion; `base` hoisted into a local; `Ret` = two 64-bit values (`eax | pop<<32`, `edx`) so no packing; callee pop count known statically; no return-address store | 84,632 | **2.13** | 15,993 / 14,652 |
| P | L + stack-slot promotion (locals and spills become C locals) + callee-saved push/pop elision | 83,566 | 2.10 | 15,955 / 14,118 |
| A | P + stack arguments of direct calls passed as C parameters (register ABI) when the callee only reads them | 82,259 | 2.07 | 15,622 / 13,553 |

Reading:
- **Lazy flags + register locals + a clean call ABI give 2.7x fewer instructions than the SR style** (5.71 vs 2.13). This is the main reason not to reuse SR.
- Hoisting `base` out of the global (L0 -> L) is worth about 20%: a store through `uint8_t*` forces reloading any global the compiler cannot prove unaliased. The final design puts the guest base in a dedicated register (see 3.3).
- Stack-slot promotion and register arguments are worth only 1.2% and 1.6% in *instruction count* here (6.4% and 7.5% of stores). The subset where the analysis applied (1,343 of 2,000 functions): L 50,907, P 49,803, A 48,322.
  Reason (measured over the whole binary): of 543,455 `push` instructions only about 67.5k (12%) are prologue saves of callee-saved registers; about 466k (86%) are **outgoing call arguments**.
  Promoting locals cannot remove those; only calling-convention recovery can, and that needs interprocedural summaries. It is a later optimisation, not a day-one requirement, and its benefit is mostly fewer memory operations (L1 traffic) rather than fewer instructions.
- Code size: the L-style object had 338,560 bytes of `__text` for those 2,000 functions (N-style 908,736). Extrapolated to 3.67 M x86 instructions this is about **31 MB of arm64 text at 2.1 arm64 instructions per x86 instruction for the integer subset; SSE/x87 and the larger ratio for complex instructions will add to it** (inferred). The original `.text` is 12.1 MB.
- Compile throughput (single clang process, `-O2`, no LTO): L-style source 39,759 x86 instructions in 7.4 s (about 5.4k x86 instructions per CPU-second); N-style 17.1 s. Whole-binary compile cost therefore scales linearly with
  instruction count (3.67 M x86 instructions, more once SSE/x87 are lifted) and matters for the CI-minutes budget in PLAN section 3; it must be sharded (section 3.10), and the L style compiles about 2.3x faster than the SR style.
- What the prototype does not show: speed at run time, correctness, indirect-call overhead, SSE/x87 cost. Those are the first things to measure in Phase 1.

Whole-binary memory-access mix (static, `tools/analysis/memclass.py`, about 3.8 M instructions scanned; explicit operands plus implicit push/pop/call/ret):

| | Loads | Stores |
|---|---|---|
| esp/ebp-based or implicit stack | 264,181 explicit + 255,873 pop/ret = 520,054 (49.7%) | 224,951 explicit + 796,642 push/call = 1,021,593 (83.2%) |
| through other registers (heap, `this`, arrays) | 425,349 (40.7%) | 165,945 (13.5%) |
| absolute addresses (globals, IAT) | 91,450 (8.7%) | 19,619 (1.6%) |
| `fs:` (TEB) | 8,361 (0.8%) | 18,535 (1.5%) |
| **non-stack total** | **525,160** | **204,099** |

## 3. Design

### 3.1 Language and structure
- **Rust + `iced-x86`** (complete SSE/x87 decoding and instruction-info API; I used it from Python for the discovery pass and it decoded the whole binary correctly). Python was fast enough for analysis (about 100 s for the full discovery pass) but not for a
  multi-pass lifter with SSA-style analyses and emission over 3.7 M instructions; C++/Zydis would work equally but gives up Rust's enums for the IR and safe parallelism (`rayon` over functions).
- The lifter is a library plus a CLI: `decode -> discover -> function formation -> analyses -> IR -> C emit`. Config in TOML (hooks, replacements, extra entries, noreturn list).
- **One mid-level IR** (basic blocks of simple typed ops: load/store/arith/flag-producing ops/call/branch/switch) with two backends: the C emitter, and an **IR interpreter** used for the fallback (3.7). The IR makes instruction semantics defined once.

### 3.2 Discovery and function formation (already prototyped in `discover.py`)
- Seeds: entry point, exports, call targets, `.reloc` code pointers from `.text`/`.rdata`/`.data` (minus switch-table slots), prologue scan of gaps, plus interpreter-miss feedback (profile-guided coverage via config).
- Functions are "entry + reachable blocks"; a `jmp` to another entry is a tail call (16,381 of them); 8,044 functions share blocks with others, so emit shared blocks once as an internal label-function or duplicate them.
- **EH funclets (14k entries Ghidra misses) are emitted as ordinary lifted functions** with a special entry convention (ebp-based frame passed in `ecx`/`edx` as the unwinder does).
- Switch tables come from the `.reloc` runs: emit C `switch`; the 130 byte-index tables are decoded from the preceding `cmp`.

### 3.3 Register model, ABI and guest memory
- Guest GPRs are `uint32_t` C locals (SSA in clang; AArch64 has enough registers for all eight plus temporaries). `esp` is also a local; **the guest stack is real memory** (faithful mode) so address-taken locals, `alloca`, SEH frames and unwinding keep working.
- Lifted function ABI (measured as mode L): `Ret f(esp, eax, ecx, edx)` returning `{ eax | (popped_bytes << 32), edx }` in `x0/x1` with no packing; `ebx/esi/edi/ebp` are **callee-saved by verification**:
  the lifter checks that each function restores them on every path (push/pop symmetric); functions that violate the convention (hand-written asm, some CRT helpers) get an extended signature that passes and returns the registers.
  `ecx` is treated as clobbered by calls (verified against the 56% thiscall population; functions that return a value in `ecx/edx` get `edx`/extra return slots from a per-function register-effect summary).
- Direct calls know the callee's `ret N` statically. Indirect calls get the popped byte count from the high half of `r0`.
- **Two ABIs per function when promoting arguments (phase 2):** an internal register-argument entry for direct calls and a memory-ABI wrapper entered through the dispatch table. Only when escape analysis proves the callee only reads its stack arguments (the prototype's mode A conditions).
- **Guest pointer = `base + (uint32_t)addr`.** Reserve a 4 GB region; `base` lives in a callee-saved host register for the whole program (clang's `-ffixed-x27` plus an inline-asm/global-register mechanism, to be validated in Phase 1 because it affects every access). Fallback: pass `base` as a hoisted local (the L-mode result above, one `ldr` per function entry).
  32-bit address arithmetic is done in `uint32_t` and zero-extended (`uxtw` addressing), which AArch64 supports directly in loads/stores. Every u32 is in-range, so no bounds checks; unmapped pages fault and the fault handler converts them to an access violation.
- TEB/`fs:` is a real guest page at a fixed guest address; `fs:[0]` (SEH chain) and `fs:[0x18]` (self) are ordinary loads/stores from `base + teb`.
- Unaligned accesses: plain loads/stores may be unaligned on arm64 normal memory; **acquire/release, exclusive and atomic accesses must be naturally aligned**, so ordered accesses to addresses not provably aligned use a checked slow path (rare).
- Hot imports (`memcpy`, `memset`, `_wcsicmp`, string functions, `_CIfmod/_CIpow/_CIasin/_CIsqrt/_CIlog`, `floor`, `_controlfp_s`...) are lowered by the lifter at the call site to host intrinsics with register arguments instead of being called through the generic stack-argument import shim
  (measured hot counts in `api_surface.md`: `memcpy` about 2 M and `_wcsicmp` 1.3 M calls per minute, `_CIfmod` 11.6 M per 8 minutes).

### 3.4 Lazy EFLAGS (prototype: modes L/P/A)
- Flag state is not stored. Each flag-writing instruction records `(kind, size, operands)` where `kind` in `{SUB/CMP, ADD, LOGIC(res), INC, DEC, SHIFT(res), ...}`.
  Consumers (`jcc`, `setcc`, `cmovcc`, `adc/sbb`, `lahf`, `pushfd`) rebuild what they need from those values.
- A forward **reaching-definition analysis** per function finds, for each consumer, the producer kinds that can reach it. With one kind (the overwhelming case: `cmp/test` followed immediately by `jcc`) the condition is emitted as a direct comparison (`a < b`, `(int32_t)a < (int32_t)b`, `r == 0`) and clang deletes the dead recordings.
  In the prototype 62.9% of sampled functions lifted with only these constructs; unsupported consumers/producers (CF after `inc/dec`, flags from `imul`, `sbb` etc.) are the cases to implement next.
- Cases needing a full materialised `EFLAGS` (`pushfd`, `lahf` 2,101 sites, `adc/sbb` chains, flag-merging joins of different producer kinds) get an explicit slow materialisation at the producer. PF is computed from the low byte with `__builtin_parity`, AF only when consumed (`daa`/`aaa` do not appear outside decode noise).
- Shifts/rotates with count 0 leave flags unchanged: the producer is conditional when the count is not constant.

### 3.5 x87 -> `double`
- x87 is 2.9% of instructions, mostly `fld/fstp` return-value glue (34k each) and some arithmetic (`fmul` 5.1k, `fadd` 2.7k, `fsub` 2.1k, `fcomi(p)` 2.6k, `fsqrt` 757).
- Track the register stack depth statically per program point (MSVC output is balanced at block boundaries except the `st(0)` return value) and map `st(i)` to named `double` locals. Across calls, `st(0)` return values go through a **dedicated double return** (`d0`, as a second return struct `{u64,u64}` plus a `double` global in a per-thread context, or a lifted-ABI `double` return for the ~functions that return floats; decided by the register-effect summary).
- Precision: the MSVC CRT startup sets 53-bit precision control (`_controlfp_s` is imported and called), so `double` arithmetic reproduces `fadd/fmul/fdiv/fsqrt` exactly. `fld tbyte`/`fstp tbyte` (80-bit) occur rarely; emulate by software extended-to-double conversion or `long double` on the host where available (Apple arm64 `long double` is 64-bit: conversion is lossy, acceptable if rare; to be counted in Phase 1).
- Rounding mode: `fldcw` (602 sites) and `fnstcw` (300) mostly appear in the `fnstcw; or 0xC00; fldcw; fistp; fldcw` truncation idiom. Pattern-match it to a C `(int32_t)` truncating conversion; otherwise keep a control-word variable in the context and select the rounding for `fist*/frndint` accordingly. `_controlfp_s` runs 548k times in 8 minutes, so the HLE must implement it as a store to that variable, not as a syscall.
- `fnstsw ax; test ah,..; jcc/sahf` after `fcom/fucom` (1,431 sites) is fused with the comparison into a C floating-point compare; `fcomi/fcomip` map straight to compare + flags kind `FCMP`.
- CRT x87 math (`_CIsin`, `_CIcos`, `_CIpow`, `_CIfmod`, `_CIasin`...): host `libm` calls with `st(0)/st(1)` operands, results are not bit-exact with x87 `fsin`; acceptable (the PC game did not rely on bit-exact trig).

### 3.6 SSE -> NEON
- 297k SSE instructions are scalar-single and 4-wide float: `movss`, `movaps`, `mulss/ps`, `addss/ps`, `subss`, `xorps`, `shufps`, `comiss`, `cvtsi2ss`, `cvttss2si`, `movdqa`, `pshufd`, a few integer packed ops. No SSE3/SSSE3/SSE4/AVX.
- Map `xmm` registers to `float32x4_t`/`int32x4_t` locals (or `__m128` via `simde`/`sse2neon` headers on arm64) and lower each opcode by table. Scalar ops use only lane 0 and rely on the compiler to scalarise.
- Needs: `MXCSR` model (2 `ldmxcsr`, 3 `stmxcsr` sites: flush-to-zero/denormals-are-zero and rounding at one place), `movaps` alignment faults are ignored (arm64 allows it), `cvttss2si` saturation (`0x80000000` on overflow) emulated, `shufps` immediates -> `__builtin_shufflevector`.

### 3.7 Indirect calls and the interpreter fallback
- Every `call reg`, `call [mem]`, virtual-call thunk (`jmp reg`) and the **GNatives** dispatch goes through `dispatch(guest_addr, ...)`. 85,844 entry points: use a **perfect hash (CHD/PTHash) built by the lifter** keyed by 32-bit guest address (a few hundred KB, two multiplies and two loads),
  with a verification of the stored key; the cheaper direct table `(addr - 0x401000) >> 4` (758k slots) only works for 16-byte-aligned entries and the 14k funclets/stubs are not aligned.
- Per-site inline caches are not needed initially. Many virtual calls can be devirtualised later by vtable-slot analysis from `.rdata` relocations.
- On a miss (address inside the image but not in the table, or a call into data): **fall back to the IR interpreter** (the same IR crate compiled for iOS as a static library, lifting basic blocks on the fly and interpreting them, no JIT needed), log the address, and resynchronise when execution reaches a known entry. The log feeds the config for the next lift (PLAN section 2, item 2).
  The interpreter must support the full instruction set used by the binary, but only needs to be correct, not fast.

### 3.8 SEH / C++ EH (measured scope in `code_discovery.md`)
- Keep the `fs:[0]` chain faithful in guest memory (7,888 C++ frames, 8 SEH4 frames; it is only loads/stores).
- Throw paths: `RaiseException` (19 calls at startup, all `0x406D1388` thread naming), `_CxxThrowException` (3 call sites, none hit), hardware faults. HLE implements `RaiseException`/`RtlUnwind` by walking the guest chain and **calling the lifted handler functions** (`__CxxFrameHandler3` stub -> FuncInfo, SEH4 scope tables), which in turn call the lifted unwind funclets.
- Only the **~25 functions with non-local resumption** (8 SEH4 `__except` bodies, 17 C++ `catch` blocks) need a landing pad: lift them in a "registers in a frame struct" mode with `setjmp` at try entry (or `__builtin_setjmp`), and keep their locals volatile. All other EH frames only need their cleanup funclets to be callable.
- `setjmp/longjmp`: 4 call sites of `_setjmp3` and 1 of `longjmp`, all in the statically linked CRT startup region. Implement natively like XenonRecomp does.

### 3.9 x86-TSO to ARM memory ordering
x86-TSO allows only store-load reordering. A correct, cheap mapping on ARMv8 is **every shared load = `LDAPR` (acquire, RCpc), every shared store = `STLR` (release)**; `lock`-prefixed ops, `xchg mem`, `cmpxchg` and `xadd` become sequentially-consistent
`__atomic_*`/`LDAXR/STLXR` or LSE `CAS/SWP/LDADD` (`-mcpu=apple-a15` has LSE); `mfence` becomes `dmb ish`. (Apple's hardware TSO mode used by Rosetta is not available to iOS apps.)
- **Classification (static, in the lifter):** accesses with base `esp`, any access whose base is `ebp` inside a function where `ebp` is a verified frame pointer (`mov ebp,esp` dominates, never reassigned), and implicit push/pop/call/ret are **thread-private stack** -> plain `LDR/STR`. `fs:` accesses are thread-private -> plain.
  Everything else (through `eax`, `ecx`, absolute globals, `[reg*scale+disp]`) is **shared by default** -> `LDAPR/STLR`.
  Pointers into the stack passed to callees (`lea eax,[ebp-8]; push eax`) are accessed through registers in the callee, so they stay classified as shared: conservative but correct.
- Measured mix (section 2): about **83% of stores and 50% of loads are stack-class**, so only 204k stores and 525k loads in the whole binary are emitted with ordering. UE3 communicates between its game, render and streaming threads through events, critical sections and `Interlocked*`
  (all imports, which the HLE implements with full barriers); the remaining ordering matters only for lock-free flags and spin loops.
- Later relaxation (Phase 5): data-race-free code needs ordering only at synchronisation points, so non-stack loads/stores can become plain with a barrier at each import call that synchronises and at identified volatile/spin patterns. Profile first, relax with tests (PLAN risk 3).
- Not solvable by classification: split-line unaligned accesses, `xchg` with misaligned operands. Fall back to a lock-protected slow path and count occurrences in the interpreter log.

### 3.10 Output structure and parallel build
- Emit **many C files**: shard by address range so that locality of calls is preserved, about 500-2,000 functions or about 40k x86 instructions per shard (the measured compile unit above took 7.4 s), which gives on the order of 100 shards for 3.67 M instructions, compiled in parallel (12 threads locally, matrix jobs in CI). One shared header (`guest.h`: types, `ld/st` macros, `Ret`, flag helpers, dispatch declarations).
- Tiny functions (21,949 <= 16 bytes) and thunks are emitted `static inline` into a shared inline header or merged by the lifter (IR-level inlining of leaf functions with no stack frame) because they are called from many shards.
- Deterministic output, content-hashed shards for incremental builds, generated dispatch table and import-stub table as separate files.
- Flags: `-O2 -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -mcpu=apple-a15 -fvisibility=hidden`; huge functions (largest 24,682 bytes) get `-O1` or splitting if compile time explodes. Thin LTO across shards only if profile shows cross-shard call overhead.
- The arm64 text is about 2.5x the size of the x86 text (inferred above); i-cache and iOS page-in behaviour need measuring on device (A15 L1i is 192 KB). Hot/cold ordering from interpreter and runtime profiles can drive function placement.

### 3.11 Interaction with the HLE
- Lifted code calls imports through stubs generated from a signature database (name -> arg count/types/conv, stdcall pops N). Each stub reads its arguments from the guest stack (or from registers for lowered hot imports) and returns in `eax`/`st0`.
- The d3d9/dsound COM objects are guest-memory vtables whose slots point at generated host thunks (the PLAN's item 5); the call-volume measured in the trace (about 14 M `SetPixelShaderConstantF`, 13 M `SetVertexShaderConstantF`, 7.5 M `SetTexture` in 5 minutes) says the thunks must be allocation-free and branch-light.

## 4. Open risks and what decides them (no schedule implied)
1. **Dedicated base register**: confirm clang for `arm64-apple-ios` can keep a global register (otherwise per-function reload). Micro-benchmark decides.
2. **Register-effect/ABI verification** across 85k functions (callee-saved preservation, `ecx/edx` returns, funclet conventions) is the main correctness engine; wrong summaries are the likeliest source of silent bugs. A differential harness (run original under Wine/Proton with logging vs the lifted build) is the safety net.
3. **x87 corner cases** (80-bit loads, rounding modes, `fprem`, `fsin` family, denormals) and **SSE denormal/MXCSR** behaviour.
4. **EH funclet/unwinder fidelity** for the 25 resumption points.
5. **Code size and i-cache pressure** from about 31+ MB of arm64 text.
6. **Memory-ordering bugs** only visible under load: keep the conservative mapping until profiling and tests justify relaxing.
7. **Interpreter coverage** (needs every opcode in the binary, including the "decode noise" sets).

## 5. Next measurements for Phase 1 (to validate before committing the full pipeline)
- Extend the prototype to SSE and x87 for the hot loops (`movss/mulss/shufps`, `fld/fstp` glue) and compare arm64 instruction count and a micro-benchmark with `LDAPR/STLR` versus plain accesses on an A-series device or an Apple-silicon Mac.
- Lift one real hot function end-to-end and run it against the original under Wine for equal results (register state at return).
- Measure dispatch cost of the perfect-hash lookup versus a direct table.
