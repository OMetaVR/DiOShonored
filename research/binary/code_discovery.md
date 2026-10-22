# Spike 0.1: code discovery and characterisation of `Dishonored.exe`

Binary: `Binaries/Win32/Dishonored.exe`, PE32 i386, MSVC 2008 (VC90), image base 0x400000, entry point from the PE header.
Everything below is **measured** by the scripts in `tools/analysis/` unless explicitly marked *inferred*. Raw per-function tables,
the Ghidra project and gap lists are in `private/` (gitignored; they are derived from game bytes).

Tools used:
- Ghidra 12.1 headless auto-analysis (`tools/analysis/ghidra/analyzeHeadless_big.sh`, `ExportFuncs.java`). Project: `private/ghidra`.
- `tools/analysis/discover.py`: independent recursive-descent discovery with iced-x86 (seeds from entry point, exports, call targets,
  `.reloc`-derived code pointers, jump tables, prologue scan of leftover gaps; two rounds so that tail `jmp`s split functions).
- `tools/analysis/compare_ghidra.py`, `eh_natives_scan.py`, `natives_dispatch.py`, `dis.py`, `grep_strings.py`.

## 1. Sections and relocations

| Section | VA | Size | Flags |
|---|---|---|---|
| `.text` | 0x401000 | 0xB9241F (12,133,407 B) | R-X (not writable) |
| `.rdata` | 0xF94000 | 0x2D400D | R |
| `.data` | 0x1269000 | 0x21B3BC | RW |
| `.rsrc` | 0x1485000 | 0x42B8 | R |
| `.reloc` | 0x148A000 | 0x17CA0C | R |

- `.reloc` holds **608,195** HIGHLOW relocations: 239,316 sites in `.text`, 341,607 in `.rdata`, 27,272 in `.data`.
- Relocated dwords that point into `.text`: 18,796 sites in `.text` (code immediates such as `push offset f`), 333,431 in `.rdata`
  (vtables, switch tables, EH tables, function-pointer tables), 13,999 in `.data`.
- Conclusion: every code pointer stored in memory is relocated, so vtables, callbacks, jump tables and EH funclets are discoverable
  without emulation. **Jump tables and index data live inside `.text`** (VC9 emits them right after the function).

## 2. Coverage and function count (spike 0.1 pass criterion: >= 98% of `.text`)

`discover.py` result (bytes of `.text`, exclusive classes):

| Class | Bytes | % of `.text` |
|---|---|---|
| Decoded instructions in discovered functions | 11,572,959 | 95.38% |
| Padding (0xCC / 0x90 / 0x00 between functions) | 542,399 | 4.47% |
| Switch-table / index-table data | 7,568 | 0.06% |
| Unknown (3,527 gaps, none larger than 61 bytes) | 10,481 | 0.09% |

- **99.91% of `.text` is accounted for** (code, padding or recognised data). The pass criterion is met by this pass.
  Caveat: "accounted for" means decoded from a discovered entry or recognised as padding/table; it does not prove every decoded byte is real
  code. 93 sites decoded as invalid instructions; clusters of odd opcodes (`in`/`out`/`hlt`/`bound`/`enter`) look like data swept up after
  non-returning calls and should be treated as decode noise until checked.
- **85,755 functions** found (3,668,201 instructions).
- Ghidra 12.1 auto-analysis: **72,437 functions**, whose bodies cover 85.38% of `.text`.
- Cross-check: 71,313 entries are common (98.4% of Ghidra's entries).
  - Ghidra-only: 1,124 (1,025 lie inside a discovered function span, i.e. Ghidra split a function at an internal label/shared block; 99 lie
    outside any discovered span and need a manual look).
  - Python-only: **14,442 entries covering 1.27 MB (about 10% of `.text`)** that Ghidra did not cover at all. 8,010 are targets of relocated
    pointers in `.rdata` (C++ EH unwind/catch funclets and vtable slots) and 6,184 are referenced only through code immediates
    (mostly the 4,301 `__ehhandler` stubs pushed into frames). Only 86 come from call targets/gap scanning. 6,102 are <= 24 bytes.
  - Implication: **Ghidra alone would leave 10% of the code unlifted**; entry discovery must use `.reloc` pointers. The lifter needs the
    EH funclets because they are reached only through the EH runtime.
- Tail calls: **16,381** `jmp` to another function's entry (plus 7 conditional jumps to another entry), from 16,327 functions.
  8,044 functions run into code owned by another function (shared tails / outlined blocks), so function bodies are not strictly disjoint.

Function size (bytes of owned code): see `private/analysis/stats.json` (`func_size_hist`) and the figures below.

### 2a. Function size distribution (owned code bytes)
Median 38 B; 90th percentile 291 B; 99th percentile 1,502 B; largest 24,682 B. 21,949 functions are <= 16 B (thunks, getters, stubs), 1,613 are >= 1 KiB, 115 are >= 4 KiB.
Many tiny functions means call overhead and inlining of lifted code (`static inline`/LTO) matter for generated-code quality.

## 3. Indirect control flow

From the discovered functions (counts of instruction sites):

| Kind | Sites | Notes |
|---|---|---|
| direct `call rel32` | 213,082 | |
| `call reg` | 36,093 | almost all virtual calls: `mov eax,[ecx]; mov edx,[eax+N]; mov ecx,esi; call edx` |
| `call [IAT]` | 3,417 | imports; most imports also go through `jmp [IAT]` thunks (127 thunks in a block near 0xEC3C00) |
| `call [abs data]` | 2,109 | function-pointer globals |
| `call [reg+disp]` | 51 | |
| `call [esp+..]`, `call [idx*4+tbl]` | 42, 2 | |
| `jmp reg` | 1,003 | all inspected samples are tail virtual dispatch thunks (`mov eax,[ecx]; mov eax,[eax+N]; jmp eax`), not computed switches |
| `jmp [idx*4+tbl]` (switch) | 662 | every one resolved via `.reloc` |
| `jmp [IAT]` (import thunk) | 127 | |

Raw linear decoding of every function range gives 64,548 `call reg`, but that double-counts shared/outlined code; use the table above.

### Switch jump tables
- 662 distinct tables, 6,190 entries, all located **in `.text`** right after the function. Encoding is `jmp dword ptr [reg*4 + imm32]` with the
  absolute table address as a relocated imm32 and every entry a relocated absolute VA (so the table length is "run of relocated dwords").
- Size: median 5 entries; 505 tables have <= 8 entries, 20 have >= 32; the largest has 768 entries (next largest 135).
- 130 sites use a byte index table (`movzx eax, byte [eax+tbl2]` then `jmp [eax*4+tbl]`), also in `.text`.
- No computed (non-table) switch jumps were found in the 1,003 unresolved `jmp reg` sites that were sampled.
- The lifter can emit real `switch` statements from these tables.

## 4. Prologues, calling conventions

First-instruction shapes of the 85,755 functions:

| Shape | Functions |
|---|---|
| `push ebp; mov ebp,esp` frame | 34,889 |
| `mov ...` first (leaf getters, `this`-based) | 14,625 |
| `push reg` (frameless, FPO) | 8,226 |
| `sub esp,N` (frameless) | 1,900 |
| `mov eax,imm` (alloca probe / return constant) | 472 |
| `jmp` thunk | 256 |
| other (leaf FPO starting with arbitrary instr., funclets, stubs) | about 25,000 |

- Roughly 60% of functions are frameless (FPO), so **ESP-relative argument access is the norm and stack-depth tracking is required**.
- Return kinds: `ret` (cdecl-like) 38,179 sites, `ret 4` 20,714, `ret 8` 11,494, `ret 12` 4,503, `ret 16` 1,975 ... (callee-pops).
- Heuristic classification (entry-block liveness of `ecx`/`edx`, *inferred*):
  - `ecx` live-in at entry in about 47,700 functions (56%): `__thiscall` is the dominant convention.
  - `ecx`+`edx` both live-in in about 12,300: some `__fastcall`, or functions that merely read `edx` as a scratch register before writing it (the heuristic cannot tell). Treat as "unknown register args" and keep `eax/ecx/edx` as potential args in the lifter.
  - callee-pops (`ret N`) about 29,700; plain-`ret` about 31,700; about 24,400 have no `ret` in their owned body (thunks/tail-only/funclets).
- The lifter should not trust a convention per function. Model registers + stack faithfully and let the C compiler eliminate dead args
  (a "register-args" ABI for lifted functions: pass `eax,ecx,edx,esp-relative stack` explicitly; see `lifter_decision.md`).

## 5. SEH / C++ exceptions

- **Inline frame setup, no helper**: functions begin with `push ebp; mov ebp,esp; push -1; push <handler>; mov eax,fs:[0]; push eax; ...; lea eax,[ebp-0Ch]; mov fs:[0],eax`
  (VC9 with `/GS`: `mov eax,[__security_cookie]; xor eax,ebp`). There is no `__SEH_prolog4`/`__EH_prolog3` in this exe.
- **7,888 functions (9.2%)** contain this inline frame setup; all use initial state `-1` (C++ EH, not `__try` SEH4's `-2`).
  `fs:[0]` accessed at 27,220 sites (18,759 writes).
- Handler stubs: 4,301 distinct `__ehhandler$` stubs (`mov edx,[esp+8]; ... call __security_check_cookie; mov eax,<FuncInfo>; jmp __CxxFrameHandler3`);
  4,247 end in the single `jmp [__CxxFrameHandler3]` thunk (the other 54 have a different tail shape).
- **4,742 `FuncInfo` structures** (magic 0x19930522) found in `.rdata`; **only 17 contain try blocks (17 try blocks in total)**.
  So C++ EH is used almost exclusively for **destructor unwinding**, not for `catch`.
- `_CxxThrowException`: **3 call sites** (0x4033A2, 0x41F5E4, 0x4A4EC9) and none was hit during the startup trace (see api_surface.md).
- C `__try/__except`: **8 functions** have an inline SEH4 frame (`push -2; push <scope table>; push <thunk to __except_handler4_common>; mov eax,fs:[0]`):
  0x403D80, 0x416580 (the thread-naming function below), 0x548870, 0x9B5EF0, 0x9B5F80, 0x9B60B0 (CPU feature detection around `cpuid`), 0x9E3B90, 0x9E3C60.
  The SEH4 handler thunk is at 0xEC3DCB (-> `jmp [_except_handler4_common]`); `_except_handler3` is referenced only from the CRT startup region.
  The `__except` handler block resumes in the same function after restoring `esp` from `[ebp-18h]`.
- A **thread-naming `RaiseException(0x406D1388)`** is called 19 times at startup from 0x4165E8 and is swallowed by a handler (measured at runtime).
  The HLE `RaiseException` must at least support "unhandled exception from debugger-style thread naming -> return".
- `fs:` use: `fs:[0]` (SEH chain) and one `fs:[0x18]` (TEB self) only. **No inline TLS access** (`fs:[0x2C]`); TLS goes through `TlsGetValue` (imports).
- `setjmp`: `_setjmp3` has **4 call sites** and `longjmp` **1** (all in the statically linked CRT region 0xEC4E0E-0xEC97C1). No application use.
- Landing pads that need non-local resumption: the 8 SEH4 functions plus the 17 try blocks, about 25 sites in total.
  The 7.9k C++ frames only need *cleanup* (calling the unwind funclets), which does not resume inside the function.
- Consequence for the lifter (design in `lifter_decision.md`): maintain the `fs:[0]` chain faithfully in guest memory (it is ordinary loads/stores),
  implement `RaiseException` and the unwinder as HLE that calls lifted filter/unwind funclets, and give only the ~25 resumption functions a `setjmp` landing pad.

## 6. Instruction mix (3,668,201 decoded instructions, 282 distinct mnemonics)

| Class | Instructions | Share |
|---|---|---|
| Base integer / control | 3,258,554 | 88.8% |
| SSE (scalar+packed single) | 297,180 | 8.1% |
| x87 | 104,590 | 2.85% |
| SSE2 | 7,535 | 0.2% |
| CMOV | 2,863 | 0.08% |
| MMX | 142 | |
| 3DNow | 1 (`femms`) | |
| SSE3 / SSSE3 / SSE4.x / AVX | 0 | |

Top mnemonics: MOV 1,060,460; PUSH 547,334; CALL 254,813; POP 178,033; LEA 159,832; TEST 142,783; MOVSS 131,641; CMP 125,711; JE 124,265; ADD 120,592.
SSE: MOVSS 131,641; MOVAPS 43,325; MULSS 35,048; ADDSS 19,304; SUBSS 12,484; XORPS 11,500; MULPS 9,296; SHUFPS 8,799; COMISS 8,679; ADDPS 5,876;
MOVDQA 2,926; CVTSI2SS 2,058; UCOMISS 2,017; DIVSS 1,793; CVTTSS2SI 985; PSHUFD 494. The vector code is small and regular: it maps to NEON /
`sse2neon`-style intrinsics or to plain C vector extensions.
x87: FSTP 34,324 and FLD 33,537 dominate (return-value glue), FMUL 5,117, FST 3,780, FXCH 3,736, FADD 2,688, FILD 2,107, FSUB 2,065, FCOMIP 1,580, FNSTSW 1,431, FSQRT 757.

### Awkward-to-lift instructions (static sites)

| Item | Sites | Comment |
|---|---|---|
| `fldcw` / `fnstcw` | 602 / 300 | classic float->int truncation (`fnstcw; or 0xC00; fldcw; fistp; fldcw`). **Runtime: `_controlfp_s` called 548,521 times in 8 minutes**, so rounding mode must be modelled |
| `fnstsw` | 1,431 | x87 compare results (`fnstsw ax; test ah,..`); pattern-match with `fcomi` forms |
| `lahf` / `sahf` | 2,101 / 17 | flags to AH; needs full EFLAGS materialisation at those points |
| `cpuid` | 19 | all at 0x9B5F2E-0x9BA24A (CPU feature detection); return fixed values |
| `rdtsc` | 0 | timing uses `QueryPerformanceCounter`/`timeGetTime` |
| `pushfd` / `popfd` | 41 / 25 | clustered (CPU feature probe) |
| `ldmxcsr` / `stmxcsr` | 2 / 3 | at 0xD1DC4C-0xD1DCDC (MXCSR denormal/rounding control) |
| `int` (non-`int3`) | 9 | at 0x72DF20, 0x917DDF, 0xA89524, 0xCA6BB4, 0xD77EBD, 0xDC24DC, 0xDFABE4, 0xE1EA44, 0xE2B278: inspect; most likely decode noise |
| `int3` | 410 | padding / `__debugbreak` |
| `lock` prefix | 1 | plus 38 `xchg` with a memory operand (implicit lock) -> map to atomic exchange |
| `rep movsd` | 1,768 | `rep stosd` 467, other rep forms < 50 each; map to `memcpy`/`memset` |
| `emms` | 14 | around x87-pow code (0x411D92...), no-op for us |
| `movntq` | 4 | at 0xD57F04 (MMX streaming store) |
| `prefetch*` | 67 | no-op / `__builtin_prefetch` |
| `fnsave` / `frstor` | 1 / 1 | at 0xE33A54 / 0x6A220C |
| `in` / `out` / `hlt` / `bound` / `enter` / `arpl` / `daa` | 170/129/16/12/24/16/22 | clustered; **likely decode noise**, not real code |
| `xlat`, `aam`, `lds`, `les` | 14, 30, 8, 15 | likewise |

x87 CRT math entry points used by the exe (`_CIsin`, `_CIcos`, `_CIpow`, `_CIfmod`, `_CIasin`, `_CIsqrt`...) take their arguments in `st(0)`/`st(1)`.
They are imports, so the HLE supplies them; at runtime `_CIfmod` was called 11.6 million and `_CIpow` 3.2 million times in 8 minutes.

## 7. Self-modifying / generated code

- `.text` is `R-X` and not writable. No instruction with an absolute memory destination inside `.text` was found (0 hits).
- Trace (startup, about 8 minutes): `VirtualAlloc` was called 5,674 times, all `MEM_COMMIT|PAGE_READWRITE`; no `VirtualProtect`, no
  executable allocation, no `FlushInstructionCache`.
- Conclusion: **no runtime-generated or self-modifying code observed.** A full gameplay trace is still needed to rule out a rarely used path
  (the interpreter fallback in the plan covers any surprise).

## 8. UnrealScript native dispatch (GNatives)

- `execUndefined` (the default native, logs "Unknown code token %02X") is at **0x425E60**.
- `GRegisterNative(int index, Native fn)` is at **0x426C60**. On first use it fills the table with `rep stosd` (0x1000 dwords of 0x425E60 at
  runtime), then stores `GNatives[index] = fn`, with a check against re-registration.
- **`GNatives` is a 0x1000-entry (4096) dword array at 0x14068F0 in `.data`, filled at runtime** by static initialisers.
  There are **4,082 `mov reg,[idx*4+0x14068F0]` reads** and about 1,670 functions that inline the registration (411 inline copies of the
  "initialise to execUndefined" sequence carry the 0x425E60 immediate). The largest registration function (0x9EA260) registers about 520 natives.
- Each registered native is stored from a **relocated immediate** (`push/mov offset UClass::execFoo`), so **all ~4k native entry points are
  discoverable statically via `.reloc`** and are already in the entry set. The runtime dispatch (`FFrame::Step`/`ProcessInternal`) is an
  indirect call through this table, i.e. a `call reg` with a 32-bit guest address. This is the main "big indirect-call table" the dispatcher
  (guest address -> host function) has to serve. The dispatch call sites themselves were not isolated in this pass (needs decompilation); the table
  address, size, initialiser and registration function are the load-bearing facts.
- Recommended handling: the indirect-call dispatcher maps a guest address to its lifted function through a perfect hash or a direct-mapped table keyed by
  `(addr - 0x401000)`; GNatives needs nothing special.

## 9. Other measurements

- Functions calling hot helper `0x428B50` (17,754 call sites) and others: ordinary heavily-called functions that set up their own EH frame, not prolog helpers.
- Import call sites: 3,544 (`call [IAT]` and `jmp [IAT]`), 3,417 + 127.
- Exports: the exe exports 216 symbols, almost all Wwise (`AK::SoundEngine::*`, `AK::MemoryMgr::*`) so that Wwise plugins can link to it.
- The exe has no `dsound`/`xaudio` strings: DirectSound is only opened by `binkw32.dll` at runtime (see `api_surface.md`); the Wwise sink is initialised through
  other code paths that were not reached in the startup trace.

## Risks surfaced by this pass
1. About 10% of code (EH funclets, handler stubs) is invisible to Ghidra and found only through `.reloc`; the lifter must seed from `.reloc`.
2. Frameless functions with ESP-relative args and thiscall dominate, so the stack-pointer delta must be tracked exactly per instruction.
3. x87 rounding-mode flipping (`fldcw` 602 sites, `_controlfp_s` 548k runtime calls) and hot x87 CRT math (`_CIfmod`, `_CIpow`) need native fast paths and correct rounding semantics.
4. 7,888 EH-framed functions: even if throws are rare, the `fs:[0]` chain stores must remain valid or be removed consistently.
5. 24k functions have no `ret` and 8k share code with another function: function = "entry + reachable blocks" is not a clean partition. Emit shared blocks as separate (or duplicated) lifted units.
