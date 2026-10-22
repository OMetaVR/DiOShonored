# HLE runtime (Phase 1)

The runtime is the host side of `include/guest_abi.h`: it provides guest memory, the thunk table, threads, a PE loader and a Win32 HLE
layer, and runs the lifter's generated C. The contract is `docs/guest_abi.md`; deviations and open points are in
`docs/abi_change_requests.md`. The list of implemented and stubbed imports is generated into `docs/runtime_imports.md`.

## Layout

| Path | Content |
|---|---|
| `runtime/core/` | `vm.cpp` (guest address space), `fault.cpp` (SIGSEGV/SIGBUS), `thunk.cpp`, `call.cpp` (`guest_call`, `guest_invoke`, `guest_unresolved`), `thread.cpp` (threads, TEB, TLS, FLS, PE static TLS), `sync.cpp` (waits, critical sections, parking lot), `handles.cpp`, `com.cpp`, `log.cpp`, `sha256.cpp`, `mainthread.cpp` |
| `runtime/loader/` | PE loader, module registry, `runtime_init` / `runtime_run` |
| `runtime/hle/` | Win32 implementations (`k32_*.cpp`, `advapi32.cpp`, `misc_dlls.cpp`, `ntdll.cpp`), heap, VFS, binding and stub generation, generated `sigs_generated.inc` |
| `runtime/overrides/` | native `ovr_<name>` replacements for hot CRT functions |
| `runtime/platform/` | Phase 2: user32/gdi32, d3d9 over DXVK-native, dsound, XInput, DirectInput8 over SDL3 (see `docs/runtime_platform.md`) |
| `runtime/host/linux/` | `dishonored_linux` |
| `runtime/tests/` | ctest suite, fake "lifted" modules written to the ABI |
| `runtime/tools/` | `gen_sigs.py` (signature tables), `gen_d3d9.py` (d3d9 guest-vtable thunks), `gen_import_report.py`, `manual_sigs.txt` |
| `third_party/wine-specs/` | Wine `.spec` files, used only as a factual source for names, calling conventions and argument sizes (see `SOURCE.txt`) |

All of `runtime/` is C++20 built with clang (the header declares `_Thread_local` inside `extern "C"`, which g++ rejects; CMake prefers clang and
defines `_Thread_local=thread_local` for other compilers). `runtime/` compiles for `arm64-apple-ios15.0` against the xtool SDK (syntax-checked; not yet linked).
The runtime sources are an OBJECT library on purpose: HLE functions register from static initialisers and a static archive would drop them.

## Build and run

```
cmake -S runtime -B build/runtime -G Ninja            # picks clang; needs SDL3 (pkg-config); -DDISH_GENERATED_DIR=<dir> picks lifter output, -DDISH_USE_GENERATED=OFF ignores it
ninja -C build/runtime
ctest --test-dir build/runtime                         # 17 tests (the 4 platform tests need a display + Vulkan, else they are skipped)
build/runtime/host/linux/dishonored_linux \
    --game-root "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Dishonored" \
    --save-dir "$HOME/.local/share/dishonored-ios" [--log info,file=debug,hle=trace] [--trace-calls] [--no-verify] [-- guest args]
```

If `generated/program.c` exists at configure time, `host/linux/CMakeLists.txt` compiles every `*.c` under it (flags of `docs/guest_abi.md` section 9:
`-std=c11 -ffreestanding -O2 -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -fvisibility=hidden -march=x86-64-v2`) into the executable;
otherwise an empty placeholder program is linked and the binary only supports `--dump-hle` and `--help`. Re-run `cmake` after the lifter writes new shards.

Other options: `--check` verifies the SHA-256 of every module and maps them without running; `--dump-hle` prints registered implementations.
Logging: `DISH_LOG=info,file=debug,hle=trace` (levels error/warn/info/debug/trace; channels core mem loader thread sync file reg hle stub fault exc str gfx audio input win net engine),
`DISH_LOG_FILE`, `DISH_TRACE_CALLS=1`. `OutputDebugString` output goes to the `engine` channel. Faults write `fault.log`, unresolved calls `unresolved.log` (in the working directory).
After the run (or at `ExitProcess`) the runtime prints every unimplemented import that was actually called, with call counts; `g_unimpl_calls` is the total.

## Architecture

**Guest memory** (`vm.cpp`). One `PROT_NONE | MAP_NORESERVE` reservation of 4 GiB + 64 KiB at `guest_base`. A byte per 4 KiB page tracks free/reserved/committed
and a protection value; regions (`VirtualAlloc` allocations, images, mapped views) live in an ordered map. Allocation granularity is 64 KiB, `NULL`-address
allocations are next-fit from 0x10000, `MEM_TOP_DOWN` is honoured, `[0, 64K)` and `[0xFFEF0000, 4G)` (guard, thunk range, return sentinel `0xFFFFFFF0`) are permanently reserved.
Commit and protect are `mprotect` on the host; decommit and release re-`mmap` the range so recommitted pages are zero. Host page size is read at run time: on
16 KiB hosts the 4 KiB guest protections are merged per host page (union), so guard-page semantics are approximate there. Module images are reserved by
`runtime_init` before any thread exists. File mappings are `mmap` views (`MEM_MAPPED`); anonymous mappings use an unlinked temp file.

**Faults** (`fault.cpp`). SIGSEGV/SIGBUS handler on a per-thread `sigaltstack`: if the address is inside the reservation it prints guest address, access type, host pc, symbol
(`g_XXXXXXXX` names appear because the host links with `-rdynamic`), thread, and a host backtrace, then aborts. `fault_set_hook` lets a test (or the later SEH bridge) intercept first.

**Thunks and calls** (`thunk.cpp`, `call.cpp`). Thunk `i` is at `0xFFF00000 + 16*i` (65536 slots). Imports occupy `[0, n_imports)`; `GetProcAddress` results and COM vtable slots are
allocated after them. `guest_call` checks the thunk range first (one compare), then `guest_lookup`, then the sentinel, then `guest_unresolved`. Thunks that need context (auto-stubs, COM
methods) carry a flag and read `thunk_aux()`; plain thunks take no TLS access. `guest_invoke_at(esp, ...)` (use this inside thunks, with the `esp` the thunk received, via `HleCtx`) and `guest_invoke(...)`
(top level only) push arguments right to left, write the sentinel into the return slot, call `guest_call`, and check the popped byte count against the calling convention (cdecl, stdcall, thiscall, fastcall).

**HLE binding** (`hle.h`, `binding.cpp`). Implementations are ordinary C++ functions; `HLE("kernel32.dll", "Sleep", Stdcall, fn)` generates the `GuestFn` thunk by template: it reads stack
arguments (32-bit slots, 64-bit integers and doubles take two), pops `4 * slots` for stdcall/thiscall, returns eax/edx or `st0_ret`. A first parameter `const HleCtx &` receives esp/eax/ecx/edx. At startup every
`guest_program.imports[i]` is bound to slot `i`: an implementation if registered, otherwise a stub generated from the signature table. `runtime/tools/gen_sigs.py` builds that table
(`hle/sigs_generated.inc`, ~5900 entries) from the Wine specs, the 812 imports of `api_surface.json`, and `manual_sigs.txt` for DLLs Wine does not describe (steam_api, libcurl, cudart, dbghelp variants). The
test `all_implementations_match_wine_signatures` checks the pop count of every one of the 500+ implementations against the spec. Stubs log once (`UNIMPLEMENTED dll!name ... pop=N`),
pop correctly, return the table default and count calls. `GetProcAddress` on a simulated system DLL returns a thunk only for implemented functions or functions in the table marked needed; anything else
returns NULL with `ERROR_PROC_NOT_FOUND`, so optional-API probes take their fallback path.

**Threads** (`thread.cpp`). Every guest thread is a pthread with a 16 MiB host stack (the main guest thread gets 64 MiB and is not the process main thread), a guest stack (default 1 MiB, lowest
page `PAGE_NOACCESS`), a TEB (`fs:[0]` = -1, `[4]/[8]` stack bounds, `[0x18]` self, `[0x20]/[0x24]` pid/tid, `[0x2C]` PE TLS array, `[0x30]` PEB, `[0x34]` last error, `[0xE10]` 64 TLS slots, `[0xF94]` expansion slots)
and a `GuestThread` reached through the `guest_thread` host TLS pointer. Thread ids are small integers stored in `GuestThread::host_tid`. `TlsAlloc` covers 1088 slots; FLS has destructors run at thread exit; PE static TLS
(`.tls` templates from the user's image, `AddressOfIndex` filled in, per-thread copies) is wired through `fs:[0x2C]`. `ExitThread` unwinds the host stack with `longjmp` to the thread entry. `CREATE_SUSPENDED` works;
`SuspendThread` on a running thread is only counted. DLL `THREAD_ATTACH/DETACH` notifications and PE TLS callbacks are delivered unless `DisableThreadLibraryCalls` was called.
Host threads that are not created by `CreateThread` (audio callbacks, SDL) call `thread_attach(name, stack, true)` before running guest code.
The main-thread queue (`mainthread.h`) lets guest-side HLE run window/SDL work on the process main thread (required on iOS); `dishonored_linux` pumps it while the guest runs on its own thread.

**Synchronisation** (`sync.cpp`, `k32_sync.cpp`). Critical sections are the guest `RTL_CRITICAL_SECTION` bytes themselves (LockCount -1/0/1 futex-style, recursion count, owner), blocking via a hashed parking lot keyed by guest address.
Events, mutexes, semaphores, threads and processes are kernel objects behind one handle table (refcounted, handles are small multiples of 4, pseudo handles -1/-2); waits (`WaitFor*`, wait-all, timeouts, named objects, `PulseEvent`, `SignalObjectAndWait`) use one
sync mutex with a condition variable per waiter, woken per object. Interlocked ops and SLISTs are `__atomic` on guest memory (full barriers). SRW locks, condition variables and `InitOnceExecuteOnce` exist for GetProcAddress consumers.

**Heap** (`heap.cpp`). `HeapCreate`/`HeapAlloc` family, plus `GlobalAlloc`/`LocalAlloc` (moveable memory is not relocated), on size-class free lists over 1 MiB chunks committed from the guest VM; 8-byte header, 16-byte aligned blocks, blocks above 256 KiB get their own VM region.
One mutex per heap. No coalescing (see gaps).

**Loader** (`loader.cpp`). Resolves each module's `relpath` case-insensitively under the game root, verifies SHA-256 against `GuestModuleDesc` (a mismatch refuses with both hashes in the message; `--no-verify` for development), maps headers and sections at `base`, applies `.reloc` when
`base != preferred_base`, writes `iat[]` (imports to thunk addresses, recompiled exports to guest addresses), registers static TLS, then applies section protections (writable sections RW, everything else read-only; guest memory is never executable). `loader_run_entries` runs TLS callbacks and
`DllMain(PROCESS_ATTACH)` for DLLs in array order, then the exe entry through `guest_invoke`. Section bytes come from the user's files at run time only. The module registry backs `GetModuleHandle*`, `GetModuleFileName*`, `LoadLibrary*`, `GetProcAddress` (export table incl. forwarders and ordinals); system DLL names the signature table knows get a fake
header page (readable MZ/PE) and resolve through the HLE table; unknown DLLs (nvcpl, atimgpud, ddraw, nvapi) fail with `ERROR_MOD_NOT_FOUND`.

**Files** (`vfs.cpp`, `k32_file.cpp`). Windows paths (drive letters, either slash, `..`, trailing dots, `\\?\`) map through a mount table: `C:\Program Files (x86)\Steam\steamapps\common\Dishonored` -> `--game-root` (read-only; writes and creates go to `<save-dir>/GameOverlay`, with copy-up for existing files; deleting a pristine game file is refused),
`C:\Users\Player\Documents` -> `<save-dir>/Documents` (this is where UE3 writes `My Games\Dishonored\DishonoredGame\Logs\Launch.log`), `...\AppData\Local|Roaming` (temp under Local), `C:\ProgramData`, `C:\Windows[\Temp]`. Ancestors of mounts are virtual directories. Lookup is case-insensitive over a case-sensitive host through
per-directory lowercase indexes (lazy, refreshed on a miss older than 2 s, invalidated on every change made through the VFS). Implemented: Create/Read/Write/SetFilePointer(Ex)/GetFileSize(Ex)/SetEndOfFile, overlapped reads (completed synchronously, event signalled, `GetOverlappedResult`), Find*, attributes, Delete/Move/Copy,
directories, GetFullPathName/GetLongPathName/cwd/temp paths, file mappings and views (named and anonymous), std handles. The guest game directory and user directory names are configurable (`RuntimeConfig`).

**Registry** (`advapi32.cpp`). In-memory tree (HKCR/HKCU/HKLM/HKU/HKCC), case-insensitive, `A` and `W` variants (string values stored as UTF-16), `Software\Arkane\Dishonored` style create/query/set/enum; a few Windows 7 version and CPU values preset. Not persisted.

**CRT overrides** (`ovr_crt.cpp`, `ovr.h`). `ovr_memcpy memmove memset memcmp memchr strlen wcslen strcmp wcscmp _stricmp _strnicmp _wcsicmp _wcsnicmp floor ceil _controlfp_s _CIpow _CIfmod _CIatan2 _CIsqrt _CIsin _CIcos _CItan _CIasin _CIacos _CIatan _CIlog _CIlog10 _CIexp _CIsinh _CIcosh _CItanh`,
all with the `GuestFn` signature (cdecl: arguments at `esp+4`, pop 0), also registered as `msvcr90.dll`/`msvcr80.dll` exports. `_controlfp_s` maps the MSVC control word to `GuestThread::x87_cw` and `mxcsr` (precision, rounding, exception masks, flush-to-zero) per thread.
The x87 register-argument convention for `_CI*` is interim and, per `docs/guest_abi.md` section 10, only a fallback for indirect calls: the lifter calls `dish_ci_pow/fmod/atan2` and libm directly.

**COM layer** (`com.h`). A guest object is 16 bytes `{vtbl, host_id, refcount, cookie}`; each interface has one shared guest vtable whose slots are thunk-table entries pointing straight at the method's `GuestFn`; `com_host<T>(this)` is one array load (fixed-capacity table, no lock).
Measured with a fake interface: about 9.5 ns per call through `guest_call` (5 M calls in 47 ms), allocation-free. d3d9, dsound and dinput8 will be defined on top of it in phase 2.

## iOS

The iOS host (Mach-exception fault handling, reduced-reservation fallback, UIScene app shell, lifecycle pause, memory accounting, unsigned IPA) is described in `docs/runtime_ios.md`; the d3d9 backend seam and the Metal backend design are in `docs/runtime_d3d9_backend.md`.

## Tests (`ctest`, 19 executables, each its own process)

`test_memory` (reserve/commit/decommit/protect/query, 64 KiB granularity, fixed regions, top-down, file views, fault addresses), `test_heap` (classes, realloc, large blocks, 6-thread stress, HLE wrappers), `test_threads` (CreateThread, ExitThread, suspended start, TLS incl. expansion slots and per-thread isolation, FLS destructors, PE static TLS, 16 MiB host stack, thread naming),
`test_sync` (critical sections under contention, events, named objects, mutex ownership, semaphores, wait any/all, producer/consumer, clocks, Interlocked, SLIST, SRW), `test_files` (case-insensitive paths, save dir, overlay, Find*, overlapped, >4 GiB offsets, mappings), `test_strings` (UTF helpers, code pages, compare/map, ctype, locale, registry),
`test_thunks` (pop counts for every argument shape, **all 500+ implementations against the Wine signatures**, stub behaviour, ordinals, GetProcAddress policy), `test_call` (fake lifted functions in every calling convention, thunks calling back into guest code, deep nesting, unresolved handler and the default `unresolved.log` dump),
`test_loader` (a synthetic PE written to disk: hash check, relocation, IAT, protections, exports, TLS, entry order, thread notifications), `test_misc` (versions, system info, command line, environment, last error in the TEB, SEH-chain walking), `test_overrides`, `test_com`, `test_runtime_run` (`runtime_run` on a fake program, `ExitProcess` code propagates), and the four platform tests `test_window`, `test_d3d9`, `test_dsound`, `test_input` (see `docs/runtime_platform.md`).

## Status against the real program

At the time of writing `generated/` is empty (no `program.c` yet), so the runtime has only been exercised with fake lifted modules. When the lifter output appears: configure with `-DDISH_GENERATED_DIR=generated`, run with `--log info`, and iterate on the "UNIMPLEMENTED" lines and `unresolved.log`.

## Known gaps

- **Non-local resumption into lifted code** (`__except`/`catch`, about 25 sites): `RaiseException` calls the guest SEH handlers but cannot resume; unhandled exceptions terminate. Proposal in `abi_change_requests.md`.
- **Hardware exceptions** (access violation, divide by zero) are diagnostics + abort, not guest exceptions.
- **x87 argument convention** for `_CI*` is interim; `x87_cw`/`mxcsr` are stored per thread but do not change the host FPU state.
- **Platform layer** (user32/gdi32/d3d9/dsound/XInput/DirectInput8) is implemented in Phase 2, see `docs/runtime_platform.md` for what it covers and its gaps. Still stubs: sockets (fail with `WSAEAFNOSUPPORT`), `waveOut*` (`MMSYSERR_NODRIVER`, Bink uses DirectSound), `CoCreateInstance` for CLSIDs other than DirectSound.
- **steam_api**: `SteamAPI_Init` returns 1 but the interface accessors (`SteamUser()`, ...) return NULL; they need guest objects with vtables (the COM layer can host them). **libcurl** `curl_easy_init` returns NULL so HTTP fails fast.
- **Unverified signatures**: the cudart arg sizes in `manual_sigs.txt` are marked unverified; the 30 `nvcuda.dll` imports have no known signature (stubs assume cdecl/0 args, which is wrong for stdcall driver calls; only matters if PhysXCore finds a CUDA device, which the stubs prevent by failing `cudaGetDeviceProperties`).
- **Heap**: no coalescing and no per-thread caches; fragmentation across size classes is possible in long runs, and each heap has one lock.
- **SuspendThread** on a running thread, `TerminateThread`, APCs, alertable waits, mutex abandonment on thread death, `PAGE_GUARD` faults, write watches, 16 KiB-page exactness on iOS, and registry persistence are not implemented.
- **Time**: the guest time zone is UTC; `GetTickCount` starts at an arbitrary 10 minutes of uptime; QPC runs at 10 MHz.
- **iOS**: only syntax-checked against the iOS SDK (core, HLE and platform layer); the host executable and Mach exception handling are not done.
- `CreateProcess`, pipes, the console and `ShellExecute` are refused or no-ops.
