# Graphics path: D3D9 -> DXVK -> MoltenVK -> Metal (Spike 0.3)

Status legend used throughout: **[measured]** = I ran it and saw the result on this Linux box;
**[built]** = compiled/linked for arm64-ios on Linux, not yet run on a phone; **[source]** = read from the
DXVK / MoltenVK source trees in `third_party/`; **[inferred]** = reasoning, not measured.

Nothing in this document has been run on the iPhone yet. The phone-side numbers come from the probe app
(`ios/probe`, see `ios_toolchain.md`); the list of what it will tell us is in section 6.

## 1. Verdict so far

1. **Linux side works [measured].** DXVK-native 3.1.1 (SDL3 WSI) renders a scene that exercises what UE3's
   `D3D9Drv` uses: SM3.0 VS+PS bytecode, DXT1/DXT5 with mips, alpha blending, a render-to-texture pass, a
   two-stream vertex declaration, D24S8, bool/float constants. Self-test passes, screenshot in
   `research/platform/d3d9_test_linux.png`.
2. **DXVK d3d9 and SDL3 cross-build for arm64-ios on Linux [built]** with clang + ld64.lld against the Xcode 27.1
   SDK that xtool extracted. MoltenVK 1.4.2 is the prebuilt Khronos iOS framework. All of it is in the IPA.
3. **Stock DXVK cannot create a device on MoltenVK [source].** DXVK 3.1.1 (and 2.6) hard-require device features
   MoltenVK does not expose (section 4). I made an experimental patch that downgrades them to optional. Whether
   the result actually renders on the phone is unknown until the probe runs.
4. **BC/DXT is the likely blocker for the whole path [source + inferred].** MoltenVK removes every BCn format when
   `MTLDevice.supportsBCTextureCompression` is false, and DXVK requires BC. Apple documents BC as an Apple-silicon-Mac
   feature, not an A-series one, so A15 is expected to say `false`. This is unmeasured; the probe reports it.
5. **Performance bar:** I expect DXVK+MoltenVK to be CPU-bound on a draw-call-heavy UE3 game on A15 and I would plan
   for a direct D3D9->Metal backend as the real target (section 7). That is inferred, not measured.

## 2. Linux spike: what was built and what was seen [measured]

### 2.1 DXVK-native build
```
git clone --depth 1 --branch v3.1.1 --recurse-submodules --shallow-submodules https://github.com/doitsujin/dxvk.git third_party/dxvk
cd third_party/dxvk
meson setup build-linux --buildtype=release -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false \
      -Dnative_sdl2=disabled -Dnative_glfw=disabled -Dnative_sdl3=enabled --prefix=$PWD/install-linux
ninja -C build-linux install
```
Outputs `install-linux/lib/libdxvk_d3d9.so.0`, `libdxvk_dxgi.so.0`, headers in `install-linux/include/dxvk`
(`d3d9.h` etc. for native builds). DXVK 3.1.1 was the latest stable release on 2026-10-21.

Gotchas found:
- **`DXVK_WSI_DRIVER=SDL3` must be set in the environment** before `Direct3DCreate9`. DXVK 3.x refuses to guess the
  WSI on non-Windows and throws `DxvkError("DXVK_WSI_DRIVER environment variable unset")`, which is a C++ exception
  escaping out of `Direct3DCreate9` (it is not a `std::exception`, so `catch (const std::exception&)` misses it).
- The HWND you pass is the `SDL_Window*`.
- DXVK dlopens `libSDL3.so.0` itself (function-pointer table in `src/wsi/sdl3/wsi_platform_sdl3_funcs.h`).
  `SDL_GetWindowSizeInPixels` is already used upstream in 3.1.1, so the Generals port's second patch is not needed.

### 2.2 The test: `tools/spikes/d3d9_test`
`d3d9_scene.{h,cpp}` is the shared scene (pure C interface, also compiled into the iOS probe); `main_sdl.cpp` is the
SDL3 desktop driver; `Makefile` builds it. Run:
```
cd tools/spikes/d3d9_test && make
LD_LIBRARY_PATH=../../../third_party/dxvk/install-linux/lib ./d3d9_test --frames 300 --screenshot /tmp/shot.ppm
```
Scene: DXT1 grid floor (opaque), rotating DXT5 quad (alpha cut-out, SRCALPHA/INVSRCALPHA blend) that is modulated in
the pixel shader by a render-to-texture result, plus a small preview of the RTT. Vertex data uses a
`D3DVERTEXELEMENT9` declaration over two streams (stream 0 position+uv, stream 1 D3DCOLOR) like UE3 vertex factories,
`DrawIndexedPrimitive`, D24S8 depth, an A8R8G8B8 render target with its own D24S8.
The DXT textures are compressed by a small encoder in the test (range-fit; fine for a test pattern), with full mip chains.
If the D3D9 layer reports no DXT support the scene falls back to A8R8G8B8 textures and logs a warning, so the phone test
still produces output; the result JSON records `used_real_dxt_textures`.

Result on this machine (RTX 3060 Ti, NVIDIA 610.57.4, DXVK reports the adapter as "NVIDIA GeForce RTX 3060 Ti"):
- `caps: VS 0xfffe0300 PS 0xffff0300 maxTex 16384 maxAniso 16 maxVSConstF 256 NumSimultaneousRTs 4`.
- DXT1/DXT3/DXT5, A8R8G8B8/A16B16G16R16F/G16R16F/R32F render targets, D24S8, D24X8, L8, A8, V8U8, A8L8 all "supported".
- Self-test: RTT centre pixel BGRA (153,161,153,255), RTT corner alpha 0, backbuffer has >9000 non-black samples and 64
  distinct colours. `SELFTEST PASS`. The rendered frame is visually correct (screenshot).
- vsync on: ~139-141 fps (display refresh limited). The scene is trivial so this says nothing about performance.

### 2.3 Shader bytecode without fxc
Route chosen: **Wine's `d3dcompiler_47.dll`** (wine 11.16 here), which wraps vkd3d-shader's HLSL compiler and can emit
`vs_3_0`/`ps_3_0` d3dbc. `tools/spikes/hlsl_compile/hlslc.c` is a 40-line mingw program calling `D3DCompile` +
`D3DDisassemble`; `tools/spikes/d3d9_test/shaders/compile.sh` runs it under wine and writes `shaders_gen.h`
(bytecode as `uint32_t` arrays, so neither the test nor the iOS app needs wine or a compiler at run time).
Build the exe: `x86_64-w64-mingw32-gcc -O2 -o hlslc.exe hlslc.c -ld3dcompiler_47 -lole32`.
Notes:
- `wine` refuses to create a prefix under `/tmp` here ("'/tmp' is not owned by you"); the script uses `~/.cache/wine-hlslc`.
- vkd3d's compiler lowered `bool g_useRtt : register(b0)` to a float constant (`if_ne c1.x, -c1.x`), not a real `b0`
  boolean register as fxc would produce. The scene therefore sets both the float constant and `SetPixelShaderConstantB`.
  Real fxc-compiled UE3 shaders use `b#` registers and `i#` loops; nothing in this scene tests those, so SM3 bool/int
  constant handling is not covered by this spike.
- Alternatives considered, not used: `vkd3d-compiler` (not installed, Arch package `vkd3d` would provide it), hand
  assembly.

## 3. iOS cross-build of the stack [built]

All commands and the scripts are in `tools/spikes/ios_cross/` (details and gotchas in `ios_toolchain.md`):
`build_sdl3_ios.sh`, `build_dxvk_ios.sh`, `meson-arm64-ios.ini.in`, `ios-arm64.cmake`, `patches/dxvk-3.1.1-apple.patch`.

| Artifact | Result |
|---|---|
| SDL3 3.4.14 `libSDL3.0.dylib` | arm64, `LC_BUILD_VERSION platform 2 (iOS) minos 15.0`, install name `@rpath/libSDL3.0.dylib`, UIKit video + Metal + MFi/HIDAPI joystick + CoreAudio |
| DXVK 3.1.1 `libdxvk_d3d9.0.dylib` (6.1 MB) | arm64 iOS, exports `_Direct3DCreate9`, links only libSystem + libc++, contains `Sdl3WsiDriver` |
| MoltenVK 1.4.2 `MoltenVK.framework` | Khronos prebuilt `MoltenVK-ios.tar` (`dynamic/MoltenVK.xcframework/ios-arm64`), 4.8 MB, iOS minos 15.0 |

Patches DXVK 3.1.1 needed to build and load on iOS (`dxvk-3.1.1-apple.patch`; upstream DXVK has no Apple support):
1. `vulkan_loader.cpp`: Apple search order. `DXVK_VULKAN_LIBRARY` env override first (an absolute path, because
   LiveContainer rewrites `@executable_path`), then `@executable_path/Frameworks/MoltenVK.framework/MoltenVK`,
   `@rpath/...`, `libMoltenVK.dylib`, `libvulkan.1.dylib`. Upstream only tries `libvulkan.so*`.
2. `util_env.cpp`: `getExePath()` had no Apple branch (function fell off the end); added `_NSGetExecutablePath`.
   `pthread_setname_np` takes one argument on Darwin.
3. `util_win32_compat.h`: `defined(__unix__)` is false on Darwin, so `LoadLibraryA`/`GetProcAddress` were undeclared.
4. `util_math.h`: missing `<cstddef>`.
5. `meson.build` files: Mach-O `ld` has no `--version-script`; skipped on darwin (all symbols stay exported).
6. **Experimental**: `dxvk_device_info.cpp` downgrades `geometryShader`, `shaderCullDistance`,
   `VK_EXT_depth_clip_enable` and `textureCompressionBC` from required to optional on Apple (section 4).

Build-environment gotchas that cost iterations (all handled in the scripts):
- With `--target=arm64-apple-ios` clang uses the **host LLVM's libc++ headers** unless told otherwise; they reference
  `std::__hash_memory`, which the SDK's `libc++.dylib` does not export -> undefined symbols. Fix: `-nostdinc++ -isystem $SDK/usr/include/c++/v1`.
- Meson probes the linker with link args only; without `--target` in `c_link_args` it detects ELF `ld.lld` and emits GNU
  flags. Put `--target` in both compile and link args.
- Linux clang has no `libclang_rt.ios`, so every `@available`/`API_AVAILABLE` check fails to link on
  `___isPlatformVersionAtLeast`. `tools/spikes/ios_cross/rt_shim/os_version_check.c` provides it via dyld's `__availability_version_check`.
- The Xcode 27 SDK's `.tbd` files use new arch names (`arm64e.x1`) that distro LLD 22.1.8 cannot parse. Use the LLD 21 that xtool ships (`toolset/bin/orig/ld64.lld`) and pass it with `-B <dir containing ld64.lld> -fuse-ld=lld`
  (`--ld-path=` makes clang emit old ld64-style arguments and the link fails).

## 4. Does DXVK even create a device on MoltenVK? [source]

DXVK 3.1.1 `DxvkDeviceCapabilities::checkDeviceCompatibility()` rejects any device missing a feature flagged required
in `getFeatureList()` (`src/dxvk/dxvk_device_info.cpp`). MoltenVK 1.4.2 `MVKPhysicalDevice::initFeatures()` /
`MVKExtensions.def` (`third_party/MoltenVK-src`) were checked against that list:

| DXVK 3.1.1 requires | MoltenVK 1.4.2 | Note |
|---|---|---|
| `geometryShader` | never set | Metal has no geometry shaders. In DXVK 3.1.1 it is only consulted at `dxvk_device.cpp:169`, so dropping the requirement is cheap |
| `shaderCullDistance` | never set | D3D9 shaders do not use it |
| `VK_EXT_depth_clip_enable` (`depthClipEnable`) | extension absent (`EXT_depth_clip_control` only; `vkCmdSetDepthClipEnableEXT` exists via extended_dynamic_state3) | DXVK calls `cmdSetDepthClipState` unconditionally (`dxvk_context.cpp:7995`) and chains `rsDepthClipInfo` into pipelines: removing the requirement may need a real fallback |
| `textureCompressionBC` | only if `supportsBCTextureCompression` | see section 5 |
| `multiViewport` | Apple5+ (A12+) | A15 is Apple8, fine |
| `shaderInt64`, `shaderInt16`, `shaderInt8`, 8/16-bit storage, `bufferDeviceAddress`, `vulkanMemoryModel`, descriptor indexing set, `inlineUniformBlock`, `subgroupSizeControl`, `computeFullSubgroups`, `synchronization2`, `maintenance4`, `dynamicRendering`, `timelineSemaphore`, `scalarBlockLayout` | present (`computeFullSubgroups`/`subgroupSizeControl` need simd/quad permute) | not exhaustive for every flag in the list; the DXVK log on the phone lists the rest |

MoltenVK also lacks `VK_EXT_graphics_pipeline_library`, `VK_EXT_transform_feedback`, `VK_EXT_custom_border_color`,
`VK_EXT_descriptor_buffer` (all optional in DXVK). It does ship `VK_EXT_robustness2`, `VK_KHR_present_wait`,
`VK_KHR_swapchain_maintenance1`, `VK_EXT_extended_dynamic_state3`.

`MVKDevice::enableFeatures` returns `VK_ERROR_FEATURE_NOT_PRESENT` if a requested feature is unavailable, so DXVK 2.6
(`D3D9DeviceEx::GetDeviceFeatures` enables `geometryShader`, `textureCompressionBC`, `multiViewport` unconditionally)
cannot create a device on stock MoltenVK either. **Open question:** the Generals iOS port reports DXVK 2.6 -> MoltenVK
1.4.1 working on an iPhone, and its `Patches/dxvk-ios.patch` only touches the Vulkan loader and the SDL3 window-size call.
Its build uses a DXVK fork (`generalsx-project/dxvk`) "with macOS fixes" that I could not identify. Either that fork has
undocumented changes or something in that setup differs from what I read in source. This is worth one question to
whoever can ask, or reading the fork's history before relying on the port as a precedent.

## 5. Texture formats: BC/DXT

[source] MoltenVK `MVKPixelFormats.mm:1464`: `noBC = !gpuCaps.supportsBCTextureCompression` then every BC1..BC7 format
is set to no capability. DXVK d3d9 maps `D3D9Format::DXT1/3/5` to `VK_FORMAT_BC1/2/3_*` and asks Vulkan in
`CheckDeviceFormat`, so on an A-series phone `CheckDeviceFormat(DXT*)` returns `D3DERR_NOTAVAILABLE` and UE3 gets no DXT.

[inferred] `supportsBCTextureCompression == false` on iPhone 13. The probe app reports it and also tries to create BC1-BC7,
ASTC and ETC2 textures, which is the behavioural check. If it is false the options are:

| Option | Memory | Notes |
|---|---|---|
| Decompress DXT -> RGBA8 at `UnlockRect`/upload | 8x (BC1) / 4x (BC3) of the compressed size | simplest, kills the memory budget (see below) |
| Transcode BC1/BC3/BC5 -> ASTC 4x4 at upload (CPU, on a worker thread) | BC1 doubles (4 -> 8 bpp), BC3 unchanged (8 bpp) | ASTC 4x4 is native on every Apple GPU; needs a real-time encoder; quality loss vs source is small but nonzero |
| Offline transcode at import time | same as above | the game reads textures out of UPK packages through `CreateTexture`+`LockRect`, so the hook belongs in the D3D9 layer, not in a file importer |

In DXVK, the natural hook is the same mechanism d3d9 already uses for exotic formats (`ConversionFormatInfo` in
`d3d9_format.cpp`); in a direct Metal backend it is one function in the texture upload path.

## 6. What the phone will tell us (probe app)

`ios/probe` (see `ios_toolchain.md` for build/install) records to `Documents/probe_results.json`:
- `metal`: GPU family flags (Apple1..10, Common1-3, Metal3/4), `supportsBCTextureCompression`, texture-creation test for
  BC1-7/ASTC/ETC2/RGBA formats, `maxBufferLength`, `recommendedMaxWorkingSetSize` (the header marks it unavailable on iOS;
  the probe asks the runtime), argument-buffer tier, MetalFX spatial/temporal/denoised/frame-interpolation support.
- `memory`: real jetsam budget under LiveContainer (section 8 of `ios_toolchain.md`).
- `controllers`: Backbone element list and which elements were ever pressed.
- `d3d9`: DXVK -> MoltenVK result of the same scene as on Linux, avg fps and CPU ms/frame, a screenshot
  (`Documents/d3d9_test.png`) and the DXVK log (`Documents/d3d9_d3d9.log`). If DXVK rejects the device the log says which
  required feature was missing.

## 7. Performance assessment: will D3D9 -> DXVK -> MoltenVK hit the bar? [inferred]

The bar: near-1:1 visuals at a good frame rate on iPhone 13, comparable to the official Bully iOS port.
Nothing below is measured on A15; use it to decide what to measure.

**Where the time goes.** UE3 on D3D9 is CPU-heavy on the render thread: thousands of small draws with frequent
state, shader-constant and render-target changes, `Lock`/`Unlock` of dynamic buffers, occlusion queries. Two translation
layers sit between that and the GPU:
1. *DXVK*: every D3D9 call is recorded as a command-stream packet and replayed on DXVK's CS thread (state tracking,
   constant buffer uploads, pipeline-variant lookup/hash, descriptor writes, barrier tracking). Pipelines are built
   from SM3 bytecode via DXVK's dxbc-spirv path. The CS thread and the submission thread add cross-thread traffic.
2. *MoltenVK*: each Vulkan call is re-encoded into Metal. `vkCreateGraphicsPipelines` goes SPIR-V -> SPIRV-Cross ->
   MSL -> Metal compile (synchronous in the calling thread, here DXVK's compile threads). MoltenVK has no graphics
   pipeline libraries, so DXVK cannot hide pipeline creation behind partial pipelines: first-use shader/pipeline
   stalls are likely, worse than desktop DXVK. The DXVK state cache stores DXVK-level state, not Metal binaries.
   Descriptor sets become Metal argument buffers or per-draw bindings; barriers become fences/events.

A15 has two performance cores and four efficiency cores, so a design with three busy threads (game render thread, DXVK
CS thread, MoltenVK/Metal encoding) competes for the performance cores. Thermal throttling on a phone is a further
unknown. On desktop, DXVK's per-draw overhead is a small fraction of a fast core; on a phone core it is
proportionally larger. I have no number for how many draws per frame Dishonored issues; **measure it** by running the
game under Proton with `DXVK_HUD=drawcalls,pipelines,fps` (Spike 0.2 is already tracing it) and compare against
the CPU ms/frame the probe reports for the trivial scene (that figure is the per-frame floor of the stack, not a draw cost).

**What a direct D3D9 -> Metal backend replaces.** DXVK's whole d3d9 layer (state machine, constant management, format
conversion, fixed-function emulation) plus MoltenVK. It still has to provide: SM1-3 -> MSL translation, vertex
declarations -> `MTLVertexDescriptor`, state -> `MTLRenderPipelineState` + depth/stencil state objects, render-target
and surface semantics, textures/samplers, query/occlusion, `Lock` semantics, and fixed-function (UE3 uses programmable
shaders almost exclusively, which shrinks this a lot). It removes: SPIR-V generation + SPIRV-Cross + two sets of
command encoding, a thread boundary, and the "required Vulkan feature" mismatch of section 4. It makes BC->ASTC
transcoding and a Metal-native present/dynamic-resolution/MetalFX path straightforward.

**Shader translation reference: MojoShader.** `icculus/mojoshader` (zlib licence) parses D3D9 SM1-3 bytecode and has a
**Metal profile** (`profiles/mojoshader_profile_metal.c`, ~83 KB; it emits MSL source) plus GLSL, HLSL, ARB1 and SPIR-V
profiles. FNA uses it for exactly this: D3D9-era effect bytecode on non-D3D platforms, including Metal. Reuse it as the
translator and verify it against the real shader set from the game (SM3 coverage: loops/`rep`, `if`, `texldl`, `dsx/dsy`,
`vPos`, `vFace`, `sincos`, relative addressing). Both approaches compile MSL at run time on the device
(`newLibraryWithSource`, which does not need JIT entitlements); a Metal binary archive can cache the result.

**Recommendation.** Treat DXVK+MoltenVK as the Phase-2 bring-up path (it exists, it gives correct D3D9 semantics to compare
a direct backend against, and the iOS build now works), and plan the direct Metal backend as the performance
path, as PLAN.md already anticipates. Decide with data from the phone: (a) BC support result, (b) whether DXVK creates a
device at all after the feature downgrade, (c) CPU ms/frame of the trivial scene, (d) Proton draw-call counts.

## 8. Risks

| Risk | Basis |
|---|---|
| DXVK feature requirements make device creation fail on MoltenVK; downgrade patch is untested and `depthClipEnable` may need a real fallback | source |
| No BC on A15 -> every DXT texture needs transcoding or 4-8x memory | source (MoltenVK) + Apple docs; confirm with probe |
| Pipeline-creation hitches (no GPL in MoltenVK) | source (MoltenVK extension list) |
| SDL3 window created inside an app that is not SDL-owned (`SDL_SetMainReady` + `SDL_CreateWindow` under our UIKit app, then DXVK's WSI) | untested; SDL's iOS video driver is normally started by `SDL_main`; the probe does it manually |
| LiveContainer: `dlopen` of embedded dylibs depends on LC re-signing and rewriting `@executable_path`; absolute paths are used to avoid that | LiveContainer docs; untested |
| DXVK 3.1.1 vs the Generals port's 2.6: 3.x raises requirements and I could not find how the precedent solved section 4 | section 4 |
| Linux-built iOS binaries were verified only structurally (`otool`, symbol resolution at link time), never executed | no device |
| SM3 bool/int constants and loops not covered by the Linux test because vkd3d lowered the bool to a float constant | section 2.3 |
