# iOS host (Phase 2b)

The runtime on iOS: Mach-exception fault handling, the 4 GiB guest window, the `ios/game` app shell, app lifecycle, memory accounting and packaging.
Companion documents: `docs/runtime.md` (core), `docs/runtime_platform.md` (user32 / d3d9 / dsound ...), `docs/runtime_d3d9_backend.md` (the d3d9 backend seam and the
Metal backend design), `research/platform/ios_toolchain.md`, `research/platform/device_iphone13.md`.

Status: everything below **builds and links for arm64-apple-ios15.0 on Linux**; the Linux suite (19 ctest executables) passes. Nothing could be executed on a device from
here, so the iOS-only code paths (Mach exception handler, UIKit/SDL shell) are verified structurally (layout `static_assert`s against the SDK's MIG structs, symbol and
load-command inspection of the Mach-O) and by design review, not by a run.

## Build and package (Linux, no Mac, no Apple account)

```
source tools/spikes/ios_cross/env.sh                      # SDK from xtool's extracted Darwin SDK, ld64.lld, PATH
ios/game/scripts/build_ipa.sh                             # -> build/Dishonored.ipa (unsigned), placeholder program
ios/game/scripts/build_ipa.sh --with-generated            # same with generated/ linked (the lifter output; a long compile)
```

The script builds SDL 3.4.14 as a static library on demand (`tools/spikes/ios_cross/build_sdl3_ios_static.sh`, Vulkan support off), configures
`ios/game/CMakeLists.txt` with the cross toolchain file (`tools/spikes/ios_cross/ios-arm64.cmake`), builds, assembles `Dishonored.app` (the executable and `Info.plist`, nothing else)
and zips `Payload/Dishonored.app` into `build/Dishonored.ipa` (1.2 MB for the placeholder). It then checks that the executable links only `/usr/lib` and `/System/Library`
libraries. The build uses clang + `ld64.lld` directly rather than SwiftPM/`xtool dev build`: the app is plain C/C++ (SDL supplies the UIKit scene delegate), so the
only thing xtool contributes is the extracted SDK. The Mach-O is not code signed (lld does not sign for iOS); SideStore / LiveContainer sign on install.

Manual configure: `cmake -S ios/game -B build/ios-game -G Ninja -DCMAKE_TOOLCHAIN_FILE=tools/spikes/ios_cross/ios-arm64.cmake -DSDL3_DIR=third_party/sdl3-ios-static-install/lib/cmake/SDL3 -DDISH_USE_GENERATED=OFF`.
`DISH_USE_GENERATED=ON` (script flag `--with-generated`) compiles `generated/*.c` with the guest-ABI flags of `docs/guest_abi.md` section 9; `generated/` currently
holds ~315 MB of C, so that build takes a long time and is separate from the import-screen IPA.

## The app shell (`ios/game/`)

| file | role |
|---|---|
| `Info.plist.in` | `UIApplicationSceneManifest` with `SDLUIKitSceneDelegate` (UIScene lifecycle, mandatory for the iOS 27 SDK), `UIFileSharingEnabled`, `LSSupportsOpeningDocumentsInPlace`, landscape only, full screen, `UILaunchScreen`, `MinimumOSVersion` 15.0, `UIRequiredDeviceCapabilities` arm64 + metal. No entitlements anywhere. |
| `src/main.cpp` | `SDL_main` on the UIKit main thread: paths, import screen, verification worker, guest launch, host main loop |
| `src/screen.cpp` | text screen on `SDL_RenderDebugText` (no assets, no UIKit code of our own) |
| `src/libs.cpp` | library-location policy (below) |
| `CMakeLists.txt`, `scripts/build_ipa.sh` | cross build and packaging |

- **Threads.** SDL's `SDL_RunApp` runs `UIApplicationMain`; `SDL_main` (our `main`) executes on the UIKit main thread and is the host main loop: `SDL_PollEvent` ->
  `platform_route_event`, plus `mainthread_pump` (all SDL window / audio-device / Metal-layer work the guest triggers is marshalled here by `mainthread_call`). The guest runs on a
  pthread with a **16 MiB host stack** (`pthread_attr_setstacksize`) that calls `runtime_run`, which in turn starts the guest main thread on its own 8 MiB+ host stack
  (`thread_run_main`); further guest threads use the runtime's `kHostStackSize`. Nothing guest-related runs on the UIKit thread.
- **Game root.** `Documents/Dishonored/` (created on first launch with a `PUT_YOUR_DISHONORED_FILES_HERE.txt`, which also makes the app's folder appear in the Files app). The user copies
  the Steam install into it with Files. The shell searches `Documents` (itself, children, grandchildren, case-insensitive) for `Binaries/Win32/Dishonored.exe`, so a nested
  `Documents/Dishonored/Dishonored/...` or a differently named folder also works. `DISH_GAME_ROOT` overrides. Saves, config, the DXVK cache, `dishonored.log` and `fault.log` go to
  `Documents/DishonoredData/` (visible in Files; writes under the game root are redirected to an overlay there, so the imported files stay pristine).
- **Import screen** (`gamefiles_*` in `loader/gamefiles.cpp`): rescans every second; when the folder is found and the program has modules, a worker hashes every file
  against the SHA-256 table in `guest_program` and the screen lists `relpath  ok / MISSING / WRONG VERSION`. When everything verifies the shell destroys its window (the
  hint `SDL_QUIT_ON_LAST_WINDOW_CLOSE=0` keeps SDL alive), starts the guest and runs the host loop. **With the placeholder program** (`n_modules == 0`) there is nothing to
  verify or run: the screen says so ("placeholder build: no recompiled game code is linked in yet"), shows what it found, and stays up. After the guest exits the shell shows the exit
  code and the log location (the runtime cannot be restarted in-process).
- **Library location (`libs.cpp`).** SDL3 is linked **statically**, so the app has no `Frameworks/` folder at all (the LiveContainer launch crash does not apply). For an
  experiment that needs dynamic libraries (DXVK + MoltenVK dylibs), `dish_ios_configure_libraries()` looks in `<bundle>/<DISH_LIB_DIR>`, `<bundle>/Libs`, then `<bundle>/Frameworks`
  (bundle = `SDL_GetBasePath()`, i.e. `NSBundle` path, so the paths are absolute) and sets `DXVK_VULKAN_LIBRARY`, `SDL_VULKAN_LIBRARY` and `DISH_DXVK_LIB` (`setenv(..., 0)`, so explicit
  environment values always win). That path is untested on device.

## Static DXVK + MoltenVK: result

Both link. `tools/spikes/ios_cross/build_sdl3_ios_static.sh` (with `WITH_VULKAN=1` it applies `patches/sdl3-3.4.14-static-moltenvk.patch`: SDL's UIKit Vulkan loader references
`vkGetInstanceProcAddr` directly instead of `dlsym(RTLD_DEFAULT)`, which cannot see symbols of an executable that LiveContainer has turned into a `RTLD_LOCAL` dylib),
`tools/spikes/ios_cross/build_dxvk_ios_static.sh` (`patches/dxvk-3.1.1-static.patch` on top of the apple patch: meson option `-Dstatic_d3d9=true` builds `libdxvk_d3d9.a`; with `DXVK_STATIC`
the SDL3 WSI binds SDL functions directly instead of `LoadLibrary("libSDL3.0.dylib")` and the Vulkan loader uses the linked `vkGetInstanceProcAddr`), and the app with
`-DDISH_IOS_DXVK=ON` plus MoltenVK's `libMoltenVK.a` produced a single 14.9 MB executable (`Direct3DCreate9` and `vkGetInstanceProcAddr` defined in it) whose only dependencies
are system libraries. So static DXVK + MoltenVK **links and packages without Frameworks/**. The measured outcome on the phone (DXVK rejects MoltenVK 1.4.2 on the A15 for
`robustBufferAccess2` / `nullDescriptor`) made it moot, so the default iOS build does not link DXVK or MoltenVK; the d3d9 backend on iOS will be the direct Metal one
(`docs/runtime_d3d9_backend.md`). `DISH_IOS_DXVK` and the two scripts are kept for experiments.

## Fault handling (`runtime/core/fault.cpp`)

POSIX targets keep the SIGSEGV/SIGBUS handler (sigaltstack, hook, same dump). **Apple arm64 uses Mach exceptions instead, and installs no signal handlers:**

- `fault_install()` allocates a receive port (+ send right) and starts a detached handler thread (256 KiB stack). `fault_thread_init()` (called by every guest thread, including
  the main guest thread) sets a **per-thread** exception port for `EXC_MASK_BAD_ACCESS` with behaviour `EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES` and flavour
  `ARM_THREAD_STATE64`. Per-thread ports take priority over task ports and only cover guest threads, so a debugger's or LiveContainer's task-level handlers stay in effect for
  everything else (SDL, DXVK, system threads).
- The handler receives `mach_exception_raise_state_identity` (id 2407; the SDK has `exc.h` but not `mach_exc.h`, so the 64-bit-code request/reply structs are defined locally and
  `static_assert`ed against the SDK's `__Request__exception_raise_state_identity_t`). A fault whose address lies inside the guest window is a **guest fault**: the handler reads
  `ARM_EXCEPTION_STATE64` (`esr` data-abort `WnR` bit -> read/write), then answers with a modified `ARM_THREAD_STATE64`: `pc` = `dish_fault_trampoline`, `lr` = faulting pc (so the
  backtrace shows the faulting frame), `sp` lowered by 512 bytes. The faulting thread resumes in the trampoline on its own stack and runs the same code as the POSIX handler:
  `g_last_fault_addr`, the fault hook (tests may `longjmp` out), then the shared reporter (guest address, read/write, host pc, symbol, thread tid / name / TEB / guest stack range,
  `backtrace()`, appended to `fault.log` in `Documents/DishonoredData`), then `abort()`.
- Any other `EXC_BAD_ACCESS` (host bug, stack overflow) is answered `KERN_FAILURE`, so the kernel continues to the task-level and host handlers: the normal crash report path,
  untouched. A second guest fault while one is being reported parks that thread. A hook returning "handled" cannot resume the instruction on Mach (state is not restored) and is treated as unhandled.
- Not exercised on a device from here. Verified: compiles and links for the SDK, MIG struct layout asserts, handler logic reviewed against the Mach exception protocol.

## The 4 GiB reservation

**Measured (iPhone 13, iOS 27.0.1, LiveContainer, `research/platform/device_iphone13.md`): a 4 GiB `PROT_NONE` reservation succeeds; 8 GiB and larger fail with `ENOMEM`.** No
extended-virtual-addressing entitlement is needed. The runtime therefore keeps the single 4 GiB (+64 KiB) window.

A fallback exists anyway because it costs little, and the design note for the lifter is below. `vm_init(space)` tries 4096, 3584, 3072, 2560 MiB (`DISH_GUEST_SPACE_MB` caps/forces a size);
`vm_user_max()` / `vm_space_bytes()` report the result (default user max is 0xFFEF0000; reduced layouts use `space - 64 KiB`, with the top 64 KiB a permanent guard). The page tables are sized by the
reservation, allocation limits, `GetSystemInfo`'s `lpMaximumApplicationAddress` and top-down allocation follow `vm_user_max()`. `test_vm_reduced` (3 GiB) covers it. Constraints for a reduced layout:

- Every module base must lie below the user max. Current bases: `Dishonored.exe` 0x00400000-0x01607000, `msvcp90.dll` 0x78480000, `msvcr90.dll` 0x78520000-0x785C3000, so **2.5 GiB is enough**; nothing
  larger than ~1.9 GiB (0x785C3000) is required by the program image layout.
- The thunk table (0xFFF00000..0xFFFF0000) and the `0xFFFFFFF0` sentinel are only ever compared as numbers (call targets, return addresses), never dereferenced as guest memory, so they work
  with a reduced window; any lifted code that *dereferences* an address >= `vm_user_max()` would read unrelated host memory, because `guest_base + addr` escapes the reservation. Reduced layouts are therefore
  only safe if no recompiled code uses absolute data addresses in that range. For the lifter this means: do not hard-code addresses above 0x78600000 (the current output does not), keep the module
  layout in `GuestModuleDesc.base` below 2 GiB, and keep addressing via `guest_base + (u32)` as now. A per-module layout is just the existing `base` fields; the runtime needs no change to relocate modules lower.
- Jetsam (~2.1 GB) is the real limit, not address space: a 3 GiB window is already more than the process may touch.

## App lifecycle (`runtime/core/lifecycle.cpp`)

iOS suspends the app soon after `SDL_EVENT_WILL_ENTER_BACKGROUND` and Metal rejects GPU submissions from the background. The shell installs an SDL **event watch** (invoked synchronously
inside the UIKit callback, which is what the event requires) for `WILL_ENTER_BACKGROUND`, `DID_ENTER_FOREGROUND` and `LOW_MEMORY`, forwarded to `platform_route_event`:

1. `lifecycle_pause(1500)` marks the runtime paused, runs hooks (the dsound mixer pauses its SDL audio stream) and waits up to 1.5 s for the render thread to park.
2. The render thread parks inside `Present` (device and swap-chain `Present` are hand-written thunks): after the backend's `Present` returns it creates an event query, waits for it (the GPU is idle;
   <= 2 s), reports "parked" and blocks. Other guest threads stall behind the render thread through the engine's own fences.
3. `DID_ENTER_FOREGROUND` -> `lifecycle_resume()` wakes the render thread and resumes audio. If the guest has not presented yet, pause returns immediately.

`test_lifecycle` covers the protocol (pause waits for the park, no frames while paused, resume releases, hooks).

## Memory accounting (`runtime/core/memacct.cpp`, `dish/memacct.h`)

Measured budget: ~2.1 GB jetsam ceiling, ≤ ~1.8 GB safe including GPU memory. `memacct_query()` returns guest committed bytes (vm), GPU bytes reported by backends
(`memacct_gpu_add`), the OS footprint (`phys_footprint` via `task_info`) and `os_proc_available_memory()`. `memacct_set_budget(footprint, guest_commit_limit)` records the ceiling and
installs a **guest commit limit** (`vm_set_commit_limit`: `VirtualAlloc(MEM_COMMIT)` fails with `ERROR_NOT_ENOUGH_MEMORY` beyond it instead of the process being jetsammed). The shell sets 1.8 GiB / 1.4 GiB
(`DISH_COMMIT_LIMIT_MB` overrides the second). `SDL_EVENT_LOW_MEMORY` sets `MemStats::low_memory`. The d3d9 backend is the main consumer (see the backend doc, 2.7).

## Files added or changed in the runtime

`core/fault.cpp` (Mach path, shared reporter, `fault_set_log_path`), `core/vm.cpp` (reservation chain, runtime user max, commit limit), `core/lifecycle.cpp`, `core/memacct.cpp`,
`loader/gamefiles.cpp`, `platform/d3d9_*` (backend registry, manual `Present`), `platform/win32_user32.cpp` (lifecycle / low-memory events), `platform/dsound.cpp` (pause hook), `CMakeLists.txt`
(Apple SDL3 via `find_package`, `DISH_STATIC_DXVK`), `tests/test_vm_reduced.cpp`, `tests/test_lifecycle.cpp`.
