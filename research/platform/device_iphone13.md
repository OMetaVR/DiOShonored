# Measured on the user's iPhone 13 (iPhone14,5), iOS 27.0.1, under LiveContainer + SideStore

Source: `private/probe_results/run1/probe_results.json`, from the DIOS Probe NoFW build.

- **RAM:** 3661 MB physical, 6 CPU cores, 16 KiB pages. The screen is 1170x2532 native at 3x scale, 60 Hz.
- **Jetsam budget:** about 2098 MB. That is `os_proc_available_memory` at start (2087 MB) plus the 10 MB footprint. The probe committed 1728 MB before stopping at its own safety threshold (356 MB left) and got one memory warning. **Plan for a total footprint, GPU included, of ≤ ~1.8 GB.**
- **Reservations:** a 4 GiB PROT_NONE reservation succeeds (errno 0). 8, 16, 32 and 64 GiB reservations fail with ENOMEM, so the 4 GiB guest window is viable without extra entitlements.
- **Metal:** Apple A15 GPU, families Apple1–8, Common1–3, Metal3 and Metal4, MSL 4.0, argument buffers Tier 2, read-write textures Tier 2. `maxBufferLength` is 2048 MB and `recommendedMaxWorkingSetSize` 2730 MB, but the jetsam budget above is what binds.
  - **No native BC/DXT** (`supportsBCTextureCompression` = false). ASTC 4x4, 6x6 and 8x8 LDR, ETC2, RGBA8, RGB10A2, RG11B10F and RGBA16F all create.
  - **MetalFX spatial, temporal, temporal-denoised and frame interpolation are all supported** on the A15.
  - MSAA 4x is supported; 8x is not. 32-bit float filtering is not supported. (`isLowPower` is macOS-only and threw an exception, now fixed.)
- **Controller:** the Backbone One shows up as an MFi `GCExtendedGamepad` with every XInput-equivalent control: A/B/X/Y, both shoulders and triggers, both sticks with L3/R3, dpad, and Menu, Options, Home and Share. A full XInput 1.3 mapping is possible.
- **DXVK 3.1.1 + MoltenVK 1.4.2** (Vulkan 1.4.357, 153 device extensions, log in `private/probe_results/run4/dxvk_stderr.log`): DXVK rejects the adapter for exactly two missing required features, both from VK_EXT_robustness2, which MoltenVK exposes but without these features:
  - `robustBufferAccess2`
  - `nullDescriptor`

  Everything else DXVK 3.1.1 requires is satisfied once the existing apple patch is applied.
- **Packaging:** a guest app whose `Frameworks/` holds MoltenVK.framework and the SDL3/DXVK dylibs crashes before any of its code runs. The identical build without `Frameworks/` runs. The `Libs/` variant (libraries loaded from `Libs/` by absolute path) **launches fine**.

## D3D9 → Metal backend acceptance (d3d9test v1–v3, 2026-10-21)

Fake guest client (runtime/tests/guest_scene.h) through the thunk layer into the Metal backend, iPhone14,5, iOS 27.0.1.
Same program on Linux/DXVK-native is the reference (`build/rt-lead/tests/d3d9_bench`).

| | iPhone 13 (Metal) | Linux (DXVK, desktop) |
|---|---|---|
| reference scene 640x360 / 1280x720 | 58 / 61.6 fps (vsync), 0.27 ms CPU submit | — |
| stress 1000 draws/frame | 60 fps, 5.0 ms submit | 1.3 ms |
| stress 3000 draws/frame | 60 fps, 5.6 ms submit (1.85 µs/draw, ~5 thunk calls per draw) | 3.8 ms |
| cold first frame (3 shaders, 3 pipelines) | 181–191 ms (shader compile 112 ms, pipelines 53 ms) | 17 ms |
| device re-create (pipeline archive warm) | 10 ms | 354 ms |
| footprint | ~120 MB, ~2 GB available | |

- Image parity: after the D3D9 half-pixel viewport offset fix, the scene matches DXVK within 5/255 everywhere (v1 without it: ~4% of pixels off by >32 along every edge).
- The per-draw cost is flat across the frame (250-draw segments). It is ~2.6x higher at 1000 draws than at 3000 because the CPU
  stays at low clocks when the frame is mostly idle (DVFS), not because of a stall. Benchmarks must use enough load to ramp the clocks.
- Guest threads now carry QoS classes (main: USER_INTERACTIVE, CreateThread: USER_INITIATED).
