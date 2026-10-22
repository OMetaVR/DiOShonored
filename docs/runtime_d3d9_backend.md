# d3d9 backend seam, and the design of the direct Metal backend

Companion to `docs/runtime_platform.md` (d3d9 section) and `PLAN.md` section 8. The guest-facing COM layer is backend independent; a backend is
whatever returns host objects implementing the D3D9 interfaces. DXVK-native is the Linux / reference backend. iOS gets a direct D3D9 -> Metal
backend. This document defines the seam precisely and sketches the Metal backend. Nothing of the Metal backend is implemented yet.

## 1. The seam

```
 lifted guest code
   | guest_call(vtbl[slot])                       32-bit stdcall/thiscall, guest pointers
   v
 COM thunk layer (backend independent)            runtime/platform/d3d9_layer.cpp, d3d9_manual.cpp, d3d9_generated.inc
   - guest object <-> host object identity map, guest refcounts, IUnknown, QueryInterface
   - argument decoding (32-bit structs, guest pointers, HWND -> SDL_Window*), try/catch -> D3DERR_DRIVERINTERNALERROR
   - Lock/Unlock staging in guest memory (host pointer -> guest copy and back)
   - app lifecycle safe point in Present (drain GPU, park while backgrounded)
   |
   |   host-side Windows D3D9 interfaces (64-bit pointers), exactly as declared by d3d9.h
   v
 backend:  "dxvk"  (DXVK-native, Vulkan)        "metal" (to be written)        "null" (a test stub could be added the same way)
```

The seam is the set of host interfaces in `d3d9.h` (the MinGW/DXVK-native header, `third_party/dxvk/install-linux/include/dxvk/d3d9.h`; zlib licensed, so a
backend may reuse it): `IDirect3D9`, `IDirect3DDevice9`, `IDirect3DSwapChain9`, `IDirect3DSurface9`, `IDirect3DVolume9`, `IDirect3DTexture9`,
`IDirect3DCubeTexture9`, `IDirect3DVolumeTexture9`, `IDirect3DVertexBuffer9`, `IDirect3DIndexBuffer9`, `IDirect3DVertexDeclaration9`,
`IDirect3DVertexShader9`, `IDirect3DPixelShader9`, `IDirect3DStateBlock9`, `IDirect3DQuery9` (and the `IDirect3DResource9` / `IDirect3DBaseTexture9` bases).
That is the "set of per-interface vtables": they already exist, are what the thunk generator targets (`runtime/tools/gen_d3d9.py` emits one thunk per
method of the 15 interfaces, 226 generated + 28 hand-written, 0 unsupported), and are what the layer was tested against. A backend therefore needs
**no change in the thunk layer**; it registers itself and returns an `IDirect3D9*`.

Registration (`runtime/platform/d3d9_internal.h`):

```cpp
struct D3d9Backend {
    const char *name;                          // "dxvk", "metal"
    IDirect3D9 *(*create)(UINT sdk_version);   // main thread; nullptr when this backend cannot run here (log the reason)
    void (*shutdown)();                        // optional
    const char *(*describe)();                 // optional: library path / GPU name for logs
};
void d3d9_register_backend(const D3d9Backend *b);   // from a static initialiser; first registered = default
const D3d9Backend *d3d9_active_backend();
```

`d3d9_load_and_create` (called by the `Direct3DCreate9` HLE on the main thread) picks the backend named by `DISH_D3D9_BACKEND`, else the first registered one.
The DXVK backend (`dxvk_create` in `d3d9_layer.cpp`) is registered by a static initialiser in the same file. A Metal backend adds its own
translation unit (`runtime/platform/d3d9_metal/*.mm`, Objective-C++ compiled only for Apple targets) with the same pattern, and the iOS CMake gives it
priority (register it first on `__APPLE__`, or set the default in the host).

### 1.1 What the layer promises the backend

- **Threading.** `Direct3DCreate9`, `CreateDevice`, `Reset` and `CreateAdditionalSwapChain` run on the process main thread (the UIKit thread on iOS), because the
  native window / `CAMetalLayer` must be created there. Every other call runs on the calling guest thread, with no layer-level locking: a game that creates
  the device with `D3DCREATE_MULTITHREADED` may call from several guest threads and the backend must serialise internally in that case, as DXVK does.
  Dishonored (UE3) uses one render thread plus the game thread for resource creation and locks.
- **Windows.** An `HWND` crossing the seam is `reinterpret_cast<HWND>(SDL_Window*)` (`d3d9_host_hwnd`); a null guest HWND stays null. The backend gets
  the `UIView` / `CAMetalLayer` through `SDL_Metal_CreateView` / `SDL_Metal_GetLayer` (or the window properties).
- **Locks.** The backend's `Lock` / `LockRect` / `LockBox` must return a pointer into host memory that stays valid until the matching `Unlock`. The layer
  allocates a staging buffer in guest memory, copies host -> guest unless `D3DLOCK_DISCARD` (or write-only buffers), hands the guest the staging pointer and pitch,
  and copies guest -> host in `Unlock` unless `D3DLOCK_READONLY`; extents are computed per format (block compressed included) so no copy reads past the allocation.
  Rationale: host memory cannot appear inside the 32-bit guest window. See 2.6 for a no-copy extension.
- **Reference counting.** The guest object owns exactly one host reference; the layer releases it when the guest count reaches zero. `GetBackBuffer`,
  `GetRenderTarget`, `GetContainer` and `QueryInterface` results are mapped through the identity map, so the backend must return the same
  interface pointer for the same underlying object while it lives (or a stable one per call; extra references are dropped by the layer).
- **Exceptions.** C++ exceptions escaping a backend method are caught per call and turned into `D3DERR_DRIVERINTERNALERROR`. Backends built with `-fno-exceptions` are fine.
- **Lifecycle drain.** On `Present` the layer calls `lifecycle_park`; when the app is entering the background it creates a `D3DQUERYTYPE_EVENT` query on the device,
  issues it and polls `GetData(D3DGETDATA_FLUSH)` until the GPU has finished (<= 2 s), then blocks the render thread until the app returns. The backend must
  therefore implement event queries so that "signalled" means all previously submitted work has completed on the GPU, and **must not submit any GPU work from its own
  threads while the layer is parked** (iOS forbids Metal submission from the background).
- **Memory budget.** `dish/memacct.h`: `memacct_gpu_add(+/-bytes)` for every texture / buffer / render target / heap the backend allocates, `memacct_query()` for the
  process footprint (`phys_footprint`), `memacct_over_budget()`, and `SDL_EVENT_LOW_MEMORY` sets `MemStats::low_memory`. The iPhone 13 ceiling is ~2.1 GB total
  with GPU memory counted; the shell sets a 1.8 GiB footprint budget and a 1.4 GiB guest commit limit (`vm_set_commit_limit`).
- **Caps and formats.** `GetDeviceCaps`, `CheckDeviceFormat`, `CheckDeviceType`, `GetAdapterDisplayMode`, `EnumAdapterModes` must describe a
  Shader Model 3.0 HAL device with what Dishonored asks for. The authoritative oracle is the DXVK backend on Linux: `test_d3d9` and a recorded call trace tell which
  formats / caps the game queries; the Metal backend must give identical answers for those, and answer "unsupported" like a real 2012 GPU for the rest.
  DXT1/3/5 must be advertised as supported textures even though the A15 cannot sample BC (see 2.5).

### 1.2 What the backend must implement (device surface by group)

`IDirect3DDevice9` has 119 methods; Dishonored's hot set is small. Grouped for planning (all must exist; a group marked "stub" may return `D3D_OK` / `D3DERR_NOTAVAILABLE` like a real driver without the feature):

| group | methods | notes |
|---|---|---|
| lifecycle | `TestCooperativeLevel`, `Reset`, `GetCreationParameters`, `GetDeviceCaps`, `GetDisplayMode`, `EvictManagedResources`, `GetAvailableTextureMem`, `SetCursor*`/`ShowCursor` (stub) | `Reset` is called on resolution / vsync change; it must succeed only when all `D3DPOOL_DEFAULT` resources are released (DXVK's rule; the layer's refcounts make it exact) |
| resources | `CreateTexture/CubeTexture/VolumeTexture/VertexBuffer/IndexBuffer/RenderTarget/DepthStencilSurface/OffscreenPlainSurface`, `CreateQuery`, `CreateVertexDeclaration`, `CreateVertexShader/PixelShader` | pools DEFAULT / MANAGED / SYSTEMMEM / SCRATCH; managed = backend keeps a system-memory copy and uploads on demand |
| copy | `StretchRect`, `UpdateSurface`, `UpdateTexture`, `GetRenderTargetData`, `GetFrontBufferData`, `ColorFill` | `GetRenderTargetData` is the screenshot / readback path |
| bind | `SetRenderTarget/GetRenderTarget`, `SetDepthStencilSurface`, `SetTexture`, `SetStreamSource(Freq)`, `SetIndices`, `SetVertexDeclaration`, `SetFVF` (fixed function: needs an FVF -> shader path, UE3 mostly uses shaders but the engine does issue fixed-function UI draws), `SetVertexShader/PixelShader` | |
| state | `SetRenderState` (~100 states), `SetSamplerState`, `SetTextureStageState` (fixed function), `SetViewport`, `SetScissorRect`, `SetClipPlane`, `SetTransform`, `SetMaterial`, `SetLight/LightEnable`, state blocks (`CreateStateBlock`, `BeginStateBlock`, `EndStateBlock`, `Capture`, `Apply`), `SetNPatchMode` (stub) | |
| constants | `Set{Vertex,Pixel}ShaderConstant{F,I,B}` and the getters | the hottest calls (14 M `SetPixelShaderConstantF` in 5 minutes of play): keep them to a memcpy into a shadow array plus a dirty range |
| draw | `Clear`, `BeginScene/EndScene`, `DrawPrimitive`, `DrawIndexedPrimitive`, `DrawPrimitiveUP`, `DrawIndexedPrimitiveUP`, `ProcessVertices` (stub is acceptable), patches (not supported by a real mid-2010s driver either) | |
| present | `Present`, `GetBackBuffer`, `SwapChain9::Present/GetFrontBufferData/GetBackBuffer/GetRasterStatus/GetDisplayMode/GetPresentParameters`, `GetRasterStatus`, `SetGammaRamp/GetGammaRamp`, `CreateAdditionalSwapChain` | |
| debug | `D3DPERF_*` are handled in the layer | |

`IDirect3D9Ex` / `IDirect3DDevice9Ex` are not exposed to the guest.

## 2. Design of the Metal backend (iOS)

Target: iPhone 13 (A15, Apple8 family, Metal 4 class OS), measured in `research/platform/device_iphone13.md`: no BC/DXT sampling, ASTC / ETC2 / RGBA16F fine, MetalFX spatial and temporal
available, argument buffers Tier 2, ~2.1 GB jetsam budget. Written in Objective-C++ (`.mm`), compiled with the rest of the app by the Linux cross toolchain
(clang, iPhoneOS SDK; Metal and MetalKit link as system frameworks, `MTLDevice` APIs need no entitlements). It never touches Vulkan, so MoltenVK and DXVK drop out of the iOS build.

### 2.1 Objects

One C++ class per D3D9 interface (`MtlDevice : IDirect3DDevice9`, `MtlTexture`, `MtlSurface` ...), each holding the `id<MTL...>` objects it owns and a COM refcount (the layer already
keeps guest counts; the host count only has to keep objects alive across `Release` while the GPU still reads them: retire lists keyed by command-buffer completion).
`MtlDevice` owns the `MTLDevice`, one `MTLCommandQueue`, the swap chain (`CAMetalLayer` via SDL), the state tracker, the pipeline cache, the constant ring, and the upload / transcode machinery.

### 2.2 State tracking and the pipeline cache

D3D9 state is mutable and sticky; Metal is immutable pipeline objects. `MtlDevice` keeps a shadow of all D3D9 state and a dirty mask. At each draw it hashes only what affects PSO / encoder state:

- **MTLRenderPipelineState** key: vertex declaration (vertex descriptor), vertex shader id + its permutation (see 2.3), pixel shader id, color / depth / stencil attachment formats, blend enable + factors + ops + write mask (per render target), alpha-to-coverage, sample count. Created lazily with `newRenderPipelineStateWithDescriptor:` on the render thread for the first draw, and prewarmed from the cache on startup.
- **MTLDepthStencilState** key: depth enable / write / compare, stencil enable, both-sided, ops, masks. **Rasterizer state** (cull, fill mode, depth bias, scissor, viewport, clip) is set directly on the encoder (`setCullMode`, `setDepthBias`, ...), not part of the PSO.
- **Samplers** (`MTLSamplerState` keyed by address modes, filters, anisotropy, LOD bias, border colour (Metal has fixed border colours; arbitrary D3D9 border colours are approximated or emulated in the shader), sRGB read is a texture view format).
- **Render pass management**: a render encoder lives while the same render targets are bound; `SetRenderTarget` changes or `StretchRect` / `Present` end it (load action = load unless a `Clear` is the first thing recorded for the target, then `clear`; store action = store, or `dontCare` for depth that is never read back, known from `D3DPOOL_DEFAULT` discard flags and later reads).
- The cache persists with `MTLBinaryArchive` under `<save>/MetalCache/` (2.3), so a second run does no compilation on the render thread.

### 2.3 Shaders: SM1-3 bytecode to MSL

- Translation: **MojoShader's Metal profile** is the base (zlib licensed; it parses SM1.x-3.0 bytecode including `ps_3_0` / `vs_3_0` dynamic flow control and emits MSL). It is to be vendored under `third_party/mojoshader` (not present yet) and extended where Dishonored's shaders need it (the lifter-side trace of all shader bytecode, plus `D3DXCompileShader`-free offline set, gives the regression corpus).
- Permutations: D3D9 shader behaviour depends on state the MSL cannot see at compile time: vertex declaration -> attribute layout (handled by the vertex descriptor, with `D3DDECLTYPE` -> `MTLVertexFormat` mapping and software expansion for formats Metal lacks (e.g. `D3DDECLTYPE_UDEC3`, `DEC3N`, `USHORT4N` exists, `FLOAT16_2` ok)), pixel shader fog / alpha-test / clip planes (D3D9 has fixed-function alpha test: emitted as a `discard_fragment` on a constant-buffer compare, via a function constant), `D3DRS_SRGBWRITEENABLE`, shadow-map compares (`D3DFMT_D24X8` / INTZ sampled as depth with hardware PCF: map to `sampler(compare_func::less)` when bound with the compare filter), and boolean / integer constant registers (`b#`, `i#` -> function constants for `if`/`loop`, or uniform buffer loads). These become **function constants / specialisations of one MSL library per D3D9 shader**, so the permutation count stays low.
- Compilation: `newLibraryWithSource:options:` on a worker queue (never the render thread; the first draw using a not-yet-compiled shader waits or, for non-critical draws, is skipped with a one-frame delay), then `newFunctionWithName:constantValues:`. Source -> `MTLLibrary` is slow (tens of ms); the persistent store is keyed by `SHA-256(bytecode) + translator version` and holds the generated MSL text; the compiled GPU binaries live in an `MTLBinaryArchive` (`newBinaryArchiveWithDescriptor:`, `addRenderPipelineFunctionsWithDescriptor:`, `serializeToURL:`), serialised after a quiet period and on `WILL_ENTER_BACKGROUND`.
- Fixed function (`SetFVF` without shaders, texture stage states, lights): generated shader variants from the TSS / light state, as in the D3D9->GL translators; UE3 uses it for little (canvas / debug draws) so a compact implementation suffices.

### 2.4 Constants and bindings

- **Constant ring buffer.** One large `MTLBuffer` (shared storage, e.g. 8 MiB x 3 frames in flight) is sub-allocated per draw: `c0..c255` float4 (vs), `c0..c223` (ps), `i#`, `b#`. The shadow arrays are updated by `Set*ShaderConstantF/I/B` (memcpy + dirty range); at draw time only the dirty ranges are copied into the ring (or the whole used range when it is < 4 KiB, which beats tracking), and the buffer offset is bound with `setVertexBufferOffset:atIndex:` (cheap, no new binding). A shader's constant usage mask (from the bytecode `def`/usage analysis) limits the copy to registers it can read.
- **Argument buffers (Tier 2).** Textures and samplers for a draw are encoded into an `MTLArgumentEncoder` buffer allocated from the same ring (up to 16 PS + 4 VS samplers), so a texture change is a re-encode of one slot rather than 20 `setFragmentTexture:` calls; `useResource:usage:` marks the resident textures per encoder (or the heap approach: all textures from a few `MTLHeap`s, `useHeap:` once per pass).
- **Vertex / index data**: `D3DPOOL_DEFAULT` buffers are `MTLBuffer`s in shared storage (unified memory on A-series, so lock = pointer into the buffer; `DISCARD` renames: allocate a new buffer from a pool and retire the old after the frame; `NOOVERWRITE` writes into unused ranges directly). `DrawPrimitiveUP` copies into a transient ring.
- Primitive types: triangle fans are converted to indexed lists (cached per count); `D3DPT_LINELIST` / strips map directly; point sprites need a vertex shader expansion (`point_size` is supported natively on Metal for points, and `D3DRS_POINTSPRITEENABLE` maps to it).

### 2.5 Textures: DXT -> ASTC runtime transcode and caching

The A15 cannot sample BC1/2/3. Dishonored's textures are DXT1/3/5 (and some uncompressed). Policy:

- At `CreateTexture` with a DXT format the backend allocates an **ASTC 4x4** (8 bpp, the same footprint as DXT5; DXT1 expands from 4 to 8 bpp, a 2x memory cost for DXT1 that the budget has to include) texture, `MTLPixelFormatASTC_4x4_LDR` (or `_sRGB` when sampled sRGB). The `Lock` / `UnlockRect` staging buffer on the guest side is DXT data as the game wrote it; at `Unlock` (or `UpdateSurface` / `UpdateTexture`) the level is **transcoded DXT -> ASTC on the CPU**, on a worker pool, with NEON: BC1/3 decode to RGBA8 blocks (trivial) and an ASTC encoder that does not search (void-extent for constant blocks, otherwise a single-partition 4x4 LDR block whose endpoints are the BC endpoints: a BC1/BC3 colour block is already two endpoints plus 2-bit weights, so the transcode is intended to be a re-quantisation of the weights onto ASTC's weight grid rather than a real encode; BC3 alpha is folded into RGBA endpoints, with a dual-plane mode when alpha varies independently). This mapping is a design hypothesis to be validated against `astcenc` before committing to it. Quality target: visually indistinguishable at 720p after MetalFX; verified by PSNR against the DXVK frames (test scene and captures).
- **Persistent cache.** Key = SHA-256 of (format, width, height, mip count, level bytes). Value = the finished ASTC mip chain, stored as one file per texture in `<save>/TexCache/xx/<hash>` (or a few packed files with an index, to keep inode count down; Files-app-visible folder, deletable). On a hit the transcode is skipped entirely and the file is read straight into the texture with `replaceRegion` (or `MTLIOCommandQueue` where available for async loads). UE3 streams mips: each streamed level is keyed individually, and level data identical across runs is the norm, so after the first pass the cache removes the CPU cost. Budget: a cap (e.g. 1.5 GB of flash, LRU by access time) since Dishonored's texture set is ~2-3 GB of DXT.
- **Fallback**: RGBA8 decode (BC -> RGBA8) when ASTC transcoding is disabled or fails (4x memory of DXT1 x2 more: only for small or critical textures), and always for formats ASTC cannot express (BC4/BC5 -> `R8Unorm` / `RG8Unorm`, which Dishonored uses for normal maps with DXN: decode to RG8 plus Z reconstruction in the shader).
- Other formats: `A8R8G8B8` -> `BGRA8Unorm`, `X8R8G8B8` -> `BGRA8Unorm` (alpha forced to one in the sampler swizzle), `A16B16G16R16F` -> `RGBA16Float`, `R32F`, `A2B10G10R10` ..., `D24S8` -> `Depth32Float_Stencil8` (D24 has no Apple equivalent, depth precision is a superset), `INTZ` / `DF24` -> sampled depth. `D3DPOOL_MANAGED` textures keep a system-memory copy (counted in the budget) and re-upload after eviction; with the 1.8 GB budget the backend may drop the system copy of textures whose upload is complete and re-read from the TexCache instead.
- Mip level memory is allocated from `MTLHeap`s sized by pool, to avoid fragmentation and to make `memacct_gpu_add` exact (`heap.currentAllocatedSize`).

### 2.6 Presentation and MetalFX

- `Present` blits / resolves the D3D9 back buffer into the drawable of a `CAMetalLayer` (framebufferOnly = YES for the scaled path; `displaySyncEnabled`, `maximumDrawableCount = 3`; `D3DPRESENT_INTERVAL_*` maps to vsync on/off). The D3D9 back buffer is a private `MTLTexture` at the **internal resolution** (default ~1280x720, i.e. what the game's `D3DPRESENT_PARAMETERS` asks for, which the layer can be told to clamp); the drawable is at the screen's native size.
- **MetalFX**: `MTLFXSpatialScaler` first (input colour only, no engine integration needed) from the internal resolution to native; `MTLFXTemporalScaler` later, which needs motion vectors and a depth buffer the engine would not supply, so it would need either UE3 shader modifications at the lifter level (jitter the projection, write velocity) or a screen-space motion estimate; keep it behind a setting. Fallback is a plain bilinear blit.
- Frame pacing: triple-buffered constant rings and `dispatch_semaphore` on command buffer completion limit CPU run-ahead to two frames; `commandBuffer presentDrawable:afterMinimumDuration:` for the refresh interval.
- **Lifecycle**: `lifecycle_park` already runs after `Present`; the backend's event query completes after `waitUntilCompleted` of the latest command buffer. The backend stops its worker queues (shader compile, transcode) in `shutdown()` and on background (they only touch CPU, but any `commandBuffer commit` from them must be avoided).
- **No-copy locks (optional extension, not needed for correctness).** Metal can wrap page-aligned CPU memory as an `MTLBuffer` (`newBufferWithBytesNoCopy`). If the layer reserved the staging buffers inside a dedicated region of the guest window (they already come from the guest heap), the backend could wrap that guest memory directly and skip the host <-> guest copies for dynamic vertex / index buffers. This requires an added virtual `guest_backed_storage` capability query on the backend, so it is deferred until measurements show Lock copies matter.

### 2.7 Memory accounting

Texture / buffer / heap sizes are reported with `memacct_gpu_add`; `MemStats::footprint` (`phys_footprint` from `task_info(TASK_VM_INFO)`) is the ground truth. Policy: below ~1.4 GB footprint do nothing; at 1.4-1.8 GB shrink in this order: drop managed-pool system copies already uploaded, evict transcode caches, reduce `MTLHeap` slack, lower the internal resolution one step, refuse new `D3DPOOL_DEFAULT` textures with `D3DERR_OUTOFVIDEOMEMORY` (the engine reacts by lowering its streaming pool). `SDL_EVENT_LOW_MEMORY` triggers the same ladder immediately. The guest's own commit limit (`vm_set_commit_limit`) is the other half of the budget.

### 2.8 Testing

- `test_d3d9` already drives the backend only through guest vtables: it must pass unchanged with `DISH_D3D9_BACKEND=metal` on a Mac (the user's Intel Mac is Metal-capable and an optional fast loop), and with 0 differing pixels against the DXVK image *except* for the DXT -> ASTC path, where the comparison is a PSNR threshold. The same scene runs on the phone from a probe-style app.
- Differential testing against DXVK on Linux over a recorded D3D9 call trace (`DISH_TRACE_CALLS` already logs every HLE thunk; a d3d9-only recorder would serialise the stream with resource contents) replayed on both backends, comparing the back buffer per frame.
- Unit tests for the DXT -> ASTC transcoder run on Linux against a reference ASTC decoder (the Khronos `astcenc` decoder as the oracle).

## 3. Files touched by the seam work

- `runtime/platform/d3d9_internal.h`: `D3d9Backend`, `d3d9_register_backend`, `d3d9_active_backend`.
- `runtime/platform/d3d9_layer.cpp`: registry, DXVK backend, selection (`DISH_D3D9_BACKEND`).
- `runtime/platform/d3d9_manual.cpp` + `tools/gen_d3d9.py`: `Present` thunks (Device and SwapChain) call `lifecycle_park` after the backend's `Present`.
- `runtime/include/dish/lifecycle.h`, `core/lifecycle.cpp`, `include/dish/memacct.h`, `core/memacct.cpp`: the pause protocol and the budget accounting the backend uses.
