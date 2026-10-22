# Dishonored → iOS (static recompilation) — Project Plan

## 1. What we're working with (measured from the local Steam install)

| Item | Finding | Why it matters |
|---|---|---|
| `Binaries/Win32/Dishonored.exe` | PE32 i386, 18 MB, MSVC 2008 (VC90), `.text` = 12.1 MB (~4M instructions) | One big, conventional binary. |
| DRM wrapper | **None** (no SteamStub `.bind` section) | We can work on the exe as it is. |
| `.reloc` section | Present (1.5 MB) | Lists every absolute address in code and data, so nearly all function pointers, vtables and jump tables can be found statically. This is the biggest help for recompiling. |
| Renderer | **D3D9 only** (`D3D9Drv`, imports `d3d9.dll`) | We need one graphics API: D3D9 → Vulkan (DXVK) → Metal (MoltenVK). |
| Input | **XInput 1.3** + DInput8 | The game already has full gamepad support. We only implement `XInputGetState` on top of GameController.framework (Backbone, DualSense, Xbox and MFi pads all work). |
| Audio | Wwise with a **DirectSound** sink (`CAkSinkDirectSound`) | Only a small DirectSound subset is needed (one streaming buffer). |
| UI | Scaleform GFx, statically linked, draws through D3D9 | No extra work beyond D3D9. |
| Video | `binkw32.dll` (x86) | Recompile it with the same pipeline, or skip movies at first. |
| Physics | PhysX 2.8 + APEX DLLs (x86, loaded dynamically) | Recompile `PhysXLoader/Core/Extensions/Cooking/NxCharacter`. Try turning APEX off in config. |
| FPU | SSE dominates (~270k instructions). x87 (~107k) is mostly `fld`/`fstp` float-return glue | x87 can be emulated as `double` without real precision problems. |
| Atomics | Few `lock` instructions; uses `Interlocked*` imports | Atomics are concentrated in imports we control. |
| Not needed | `wxmsw28*` (editor), `cudart*`, `SteamAPIUpdater`, `dbghelp`, `faultrep` | Stub or drop them. |
| Content | 6.1 GB (`CookedPCConsole` UPKs, Wwise `.pck`, Bink movies) | **Never shipped.** The user imports it from their own copy. |

## 2. Approach: AOT static recompilation plus a Win32 HLE runtime

```
 user's Dishonored.exe + x86 DLLs ──► [lifter] ──► C source ──► clang arm64 ──► libgame.a
                                                                                  │
 iOS shell app (SDL3) ◄── links ── [HLE runtime] ◄────────────────────────────────┘
   ├─ guest memory: one 4 GB reservation; guest ptr = base + u32
   ├─ kernel32/user32/gdi32/advapi32/ole32/winmm shims (files→app Documents, threads, sync, TLS, VM, time, window/messages via SDL3)
   ├─ d3d9 COM thunks (32-bit guest vtables ⇄ 64-bit host) ──► DXVK-native d3d9 ──► MoltenVK ──► Metal
   ├─ XInput ──► SDL3 gamepad ──► GameController.framework
   ├─ DirectSound ──► SDL3 audio
   └─ steam_api / libcurl / wsock32 ──► stubs
```

### Why this route and not the others
- **JIT emulation (Box64/FEX + Wine, like Winlator):** iOS does not allow JIT for sideloaded apps without debugger-attach workarounds, which are fragile and depend on device and iOS version. An interpreter is far too slow for UE3. AOT recompilation needs no JIT and runs with ordinary free-account sideloading.
- **Reimplementing UE3 or using engine source:** years of work, and there is no legal source.
- **Console builds (decided: PC wins):**
  - **Xbox 360:** D3D on the 360 is compiled into the game and writes GPU commands straight to the Xenos chip. That means emulating EDRAM and resolves, tiled big-endian textures, Xenos microcode shaders and XMA audio, or finding and hooking those D3D functions inside a symbol-less binary. The PC exe's `d3d9.dll`, `dsound`, and `xinput` imports are clean replacement boundaries.
  - **PS3:** Cell/SPU code, so no.
  - **PS4/XB1 Definitive Edition:** encrypted binaries, undocumented GNM/D3D11.X, 64-bit, and a bigger memory budget, so worse on every axis.
  - **What the 360 build does better:** a 512 MB memory budget and a weakly ordered CPU like ARM.
    - The memory benefit is reproduced on PC through UE3 texture LOD bias, pool sizes and streaming ini settings.
    - The ordering benefit is handled with the barrier strategy below.
  - **Bring-your-own-copy:** most owners have PC copies, so a PC base fits that model.
  - If someone legitimately has a 360 dump, its ini files are worth reading purely as a reference for console memory and LOD settings.
- **Precedents that exist:** [M-HT/SR](https://github.com/M-HT/SR) statically recompiles Windows PE games to Linux and arm64. The [Generals iOS port](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad) runs DXVK 2.6 d3d9 → MoltenVK 1.4.1 → Metal on iPhone. N64Recomp, Unleashed Recompiled and DK64 Rekompiled show the "bring your own game" distribution model.

The phone's raw power is not the problem; a modern iPhone is far above Dishonored's 2012 PC minimum spec. The hard parts are architecture (x86 Windows → ARM iOS) and Apple's no-JIT rule. Static recompilation addresses both.

### Key technical decisions
1. **Lifter:** our own x86-32 → C lifter, XenonRecomp style (one C function per guest function, registers as locals, lazy EFLAGS). Built on `iced-x86` or Zydis for decoding. Function discovery comes from entry point + exports + `.reloc` targets + call targets + jump tables, cross-checked against Ghidra. Evaluate M-HT/SR first; reuse it if it scales to an 18 MB binary.
2. **Missed code safety net:** an indirect call to an unknown address drops into a slow x86 interpreter and logs the address. The address goes into `config/dishonored.toml` for the next build (profile-guided coverage).
3. **Memory ordering:** x86 is TSO and ARM is weakly ordered. UE3 is multithreaded (render thread, streaming, Wwise). Start conservative: non-stack loads and stores emitted as `LDAPR`/`STLR`. Relax this later with profiling.
4. **SEH / C++ exceptions:** emulate the `fs:[0]` chain in a fake TEB. Try-blocks get host `setjmp` landing pads so `__CxxFrameHandler3` continuations can resume mid-function.
5. **COM thunks:** generate the d3d9/dsound guest↔host thunks from the headers with a script. Do not hand-write them.
6. **One codebase for Linux and iOS:** SDL3 for window, gamepad and audio, and DXVK-native for d3d9. **All development happens on your Linux PC first**; iOS is a port of a working Linux build.

## 3. Bring-your-own-copy and piracy: yes, this works

- **Public repo:** lifter, runtime, iOS shell, and a config keyed to the **SHA-256 of the Steam `Dishonored.exe`**. It contains no game bytes and no generated code.
- **Recompiled code** is generated from the **user's own exe** at build time. A built IPA contains derived game code, so **IPAs are never published**. Every user builds their own.
- **Assets** are never in the IPA. On first launch the app asks the user to import their `Dishonored/` folder (Files app or Finder file sharing) and checks a few file hashes.
- This is the Zelda64Recomp / Unleashed Recompiled model.

### Build pipeline (fits a free GitHub account)
- **Daily iteration, local on Linux:** [xtool](https://github.com/xtool-org/xtool) builds, signs with a free Apple ID and installs from Linux. It needs the iOS SDK extracted from `Xcode.xip`; Apple's license for that is a grey area, so use it personally only.
- **Public-repo CI** builds and tests the runtime and shell only. Standard runners are free for public repos, including macOS.
- **Private full-IPA workflow:**
  - The game exe and DLLs (~25 MB) live as a private release asset.
  - A **Linux runner** generates the C and compiles `libgame.a` for `arm64-apple-ios`. The generated code is freestanding, so it needs no Apple SDK; this is the expensive step and it runs at the 1× minute rate.
  - A **macOS runner** only links and packages an unsigned IPA, in about 5 minutes. On private repos macOS minutes count 10×, which leaves ~200 macOS minutes/month on the free plan, enough for ~30–40 builds.
- **Install:** sideload the unsigned IPA with SideStore, AltStore, Sideloadly or Impactor — our path: SideStore + LiveContainer (see §7).

## 4. Phases, ordered so the riskiest parts fail early

### Phase 0: Spikes. Each one can kill or redirect the plan.
| # | Spike | Pass criteria |
|---|---|---|
| 0.1 | Static analysis: Ghidra or REA over the exe, function discovery from `.reloc` and calls, SEH usage, jump tables | ≥98% of `.text` assigned to functions; SEH/C++ EH pattern catalogued |
| 0.2 | API surface: run under Proton with `WINEDEBUG=+relay` (filtered) through menu → first mission | Exact list of Win32 / COM methods actually called |
| 0.3 | Graphics: DXVK-native d3d9 sample on Linux, then the same on iPhone via MoltenVK | Textured, shaded D3D9 scene on the phone |
| 0.4 | iOS shell: SDL3 app via xtool, Backbone input readout, reserve 4 GB of VA, commit 2.5 GB | Controller works; memory doesn't get jetsam-killed on **your** iPhone |
| 0.5 | Lifter choice: try M-HT/SR on the exe vs. a prototype iced-x86→C lifter on 1,000 functions | Decision recorded |

### Phase 1: Recompiler MVP (Linux x86_64)
Lift the whole exe and compile it. The runtime logs unimplemented imports, so execution reaches `WinMain` and the engine init log (`Launch.log` equivalent).

### Phase 2: Main menu on Linux
kernel32/user32 core, d3d9 thunks → DXVK, DirectSound → SDL, Scaleform rendering. **Milestone: main menu with sound, navigable by gamepad.**

### Phase 3: Playable on Linux
PhysX DLLs recompiled, Bink (or skipped), saves, streaming, and stability over hours of play. Use the Proton build as ground truth for comparisons. **Milestone: finish the first mission on a controller.**

### Phase 4: iOS
- Build for arm64 and turn on the memory-ordering mode.
- File import UI.
- App lifecycle: pause on background, and save before iOS kills the app.
- Render-resolution scaling and a 30/60 fps cap.
- DXVK state-cache pre-warm to reduce shader stutter.

**Milestone: plays on iPhone with a Backbone.**

### Phase 5: Release tooling
- CI split.
- BYO-copy build docs.
- Hash check of the user's exe.
- Optional asset trimming (English only, ~5.5 GB).
- Performance pass that relaxes the memory barriers.

## 5. Top risks and fallbacks
1. **DXVK d3d9 on MoltenVK** has rendering bugs or is too slow. Fallback: our own D3D9→Metal layer, using MojoShader to translate SM2/3 bytecode to MSL.
2. **Free-account memory ceiling:** the increased-memory-limit entitlement is probably unavailable with free signing. Mitigation: the game is 32-bit, so it is already designed for <4 GB. Lower texture pool settings. Target 8 GB iPhones (15 Pro or newer).
3. **Memory-ordering bugs** (rare hangs or corruption on ARM only). Mitigation: conservative barriers first; test on an ARM Linux box or a Mac if one is available.
4. **SEH / C++ exception edge cases.** Mitigation: Phase 0.1 catalogue plus targeted handling.
5. **Missed indirect targets.** Mitigation: interpreter fallback with logging.
6. **Thunk volume (COM, 32↔64-bit).** Mitigation: generated thunks, limited to the methods the 0.2 trace shows are used.

## 6. Working split (two Sonnet sub-agents, ≤ high effort)
- **Agent A, "binary":** spikes 0.1, 0.2 and 0.5, then the lifter.
- **Agent B, "platform":** spikes 0.3 and 0.4, then the runtime, SDL3, DXVK and the iOS shell.
- **Main thread:** integration, design decisions, review.

## 7. Target device and environment (confirmed)
- **Target: iPhone 13, regular model** (A15, **4 GB RAM** confirmed), **iOS 27.0.1**. Optimize for this; newer phones get headroom for free.
  - Memory is the #1 constraint. Unified memory means GPU textures count against the app's jetsam limit (expect roughly 2–2.8 GB usable). The exe is Large-Address-Aware, so the PC build may use up to 4 GB; we cap texture pools and streaming via ini and runtime hooks.
  - Check whether the A15 samples BC/DXT textures natively (`supportsBCTextureCompression`). If not, an import-time DXT→ASTC transcode is required. Decompressing to RGBA8 would multiply texture memory 4–8×.
  - Frame rate goal: stable 60 fps (30 fps fallback on thermal throttle), rendered at ~720p–900p and upscaled (check MetalFX spatial on A15), with dynamic resolution if needed. The reference bar is the Bully Anniversary Edition port.
  - If profiling shows DXVK + MoltenVK CPU overhead is the bottleneck (UE3 D3D9 issues many draw calls), replace that path with a direct D3D9→Metal backend. Plan for this as a likely late-phase optimization, not only a fallback.
- **Apple account: free.** Sideloading goes through **SideStore + LiveContainer**, which avoids the 7-day re-sign and the 3-app limit.
  - The app must work as a LiveContainer guest: it is loaded via `dlopen` into LiveContainer's process, runs under LiveContainer's entitlements, and has no special entitlements of its own.
  - Must not rely on the increased-memory-limit or extended-virtual-addressing entitlements.
- **Mac: ~2015 Intel**, capped at an old macOS/Xcode. This is a last-resort packaging fallback only. The primary path is xtool on Linux.
- **Dev box:** Arch Linux, 12 threads, 62 GB RAM. Available: clang, gcc, rust, meson, ninja, wine, Proton 10/11, Ghidra, Vulkan, SDL3 3.4.


## 8. Graphics decision (from iPhone 13 measurements)

Measurements are in `research/platform/device_iphone13.md`.

- **iOS uses a direct D3D9 → Metal backend.** It plugs in behind the runtime's d3d9 COM thunk layer (the seam), and **DXVK-native remains the Linux and reference backend**. Reasons:
  1. On MoltenVK, DXVK 3.1.1 is missing `robustBufferAccess2` and `nullDescriptor`, which would need deep patches: dummy-descriptor substitution throughout the descriptor code.
  2. The A15 has **no BC/DXT sampling**, and Dishonored's textures are DXT. We need control of the texture upload path to transcode DXT → ASTC (with a persistent on-device cache keyed by content hash), which DXVK doesn't give us.
  3. Performance: one translation layer instead of two on a draw-call-heavy UE3 renderer is the main CPU win for the iPhone 13 target.
- **Backend outline:**
  - D3D9 state tracking feeds a pipeline-state cache (MTLRenderPipelineState / MTLDepthStencilState keyed by hashed state).
  - SM1–3 bytecode is translated to MSL using MojoShader's Metal profile as the base, compiled at runtime with `newLibraryWithSource` and cached with `MTLBinaryArchive`.
  - Argument buffers (Tier 2) carry textures; constants go in a ring buffer with per-draw offsets.
  - Presentation goes to a CAMetalLayer, with **MetalFX** upscaling (spatial first, temporal later) from a ~720p internal resolution.
  - Memory accounting stays within the ~1.8 GB budget.
- **Testing:** the d3d9_test scene through the same thunks on the phone via a probe-style app, with pixel comparison against the Linux DXVK render. The user's Intel Mac (Metal-capable) is an optional faster loop.
