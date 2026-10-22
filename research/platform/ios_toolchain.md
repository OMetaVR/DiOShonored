# iOS toolchain on Linux, probe app, LiveContainer (Spikes 0.3 phase 2 and 0.4)

Legend: **[done]** = performed on this machine; **[verified structurally]** = built and inspected (`llvm-otool`,
link-time symbol resolution), never executed on a device; **[unverified]** = not run.

Everything below was done on Arch Linux x86_64 without sudo, with no Apple account login.

## 1. What is installed, where, and the exact steps [done]

| Piece | Version | Location | How |
|---|---|---|---|
| Swift toolchain | 6.4.1 | `~/.local/share/swiftly/toolchains/6.4.1` | swiftly |
| xtool | 1.22.2 | `~/.local/bin/xtool` | AppImage from GitHub releases |
| Darwin Swift SDK (iPhoneOS 27.1, iPhoneSimulator 27.1, MacOSX 27.0) | from `Xcode_27.1.xip` | `~/.swiftpm/swift-sdks/darwin.artifactbundle` (3.4 GB, slim) | `xtool sdk install --slim` |
| `.xip` | Xcode 27.1 | `~/Downloads/Xcode_27.1.xip` (left in place) | supplied by the user |

### 1.1 Swift via swiftly (Arch is not an officially supported platform)
```
mkdir -p ~/.local/share/swiftly-bootstrap && cd ~/.local/share/swiftly-bootstrap
curl -LO https://download.swift.org/swiftly/linux/swiftly-$(uname -m).tar.gz && tar xzf swiftly-*.tar.gz
export SWIFTLY_HOME_DIR=$HOME/.local/share/swiftly SWIFTLY_BIN_DIR=$HOME/.local/share/swiftly/bin
./swiftly init --no-modify-profile --skip-install --platform ubuntu24.04 --assume-yes
$SWIFTLY_BIN_DIR/swiftly install 6.4.1 --use --assume-yes
# Arch ships libncursesw.so.6 only; the toolchain wants libncurses.so.6 (RUNPATH is $ORIGIN/../lib/swift/linux):
ln -sf /usr/lib/libncursesw.so.6 $SWIFTLY_HOME_DIR/toolchains/6.4.1/usr/lib/swift/linux/libncurses.so.6
export PATH=$SWIFTLY_BIN_DIR:$PATH ; swift --version     # Swift version 6.4.1
```
Notes: `--platform ubuntu24.04` is what makes swiftly accept Arch. `swiftly install` also wrote `~/.swift-version` in
`$HOME` ("global default"). `swiftly install` printed a harmless permission error for `--post-install-file /dev/null`.
The bundled `lldb` does not start (needs libedit.so.2, libpython3.12, libpanel/libform); not needed for building.
xtool needs **Swift 6.4** to match Xcode 27's SDK; 6.4.1 works.

### 1.2 xtool
```
curl -fL https://github.com/xtool-org/xtool/releases/download/1.22.2/xtool-x86_64.AppImage -o ~/.local/bin/xtool
chmod +x ~/.local/bin/xtool          # FUSE is available here, so the AppImage runs directly
export PATH=$HOME/.local/bin:$HOME/.local/share/swiftly/bin:$PATH
```
**Gotcha:** xtool crashes at start-up (`epoll_ctl ... Operation not permitted`, a NIO `try!`) if **stdin is `/dev/null`**.
Run it with a real stdin (a terminal, a pipe, or a FIFO as `ios/probe/scripts/build_ipa.sh` does).

### 1.3 iOS SDK from Xcode.xip (no Apple login)
```
xtool sdk install --slim ~/Downloads/Xcode_27.1.xip     # progress is printed with \r; log it to a file
swift sdk list                                          # -> darwin
```
This does **not** need an Apple account; `xtool setup` is the command that asks for a login (API key or Apple ID
password), and it is not required for building. `xtool dev build --ipa` (no `--sign`) also needs no login.
Result: `darwin.artifactbundle/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS27.1.sdk`, a copy of the
needed Xcode toolchain parts (clang, swift stdlib for iphoneos), and `toolset/bin/orig/ld64.lld` (LLD 21.1.6). `--slim`
cannot be updated in place; rerun the install with a newer .xip to replace.

### 1.4 What the user must download themselves (already done for Xcode 27.1)
Only `Xcode.xip`, from https://developer.apple.com/download/all/?q=Xcode after logging in with an Apple ID (browser only).
Nothing else needs Apple credentials for this workflow. The Apple ID is used later by SideStore on the phone, not here.

## 2. Two ways to build iOS arm64 on this box

### 2.1 xtool SwiftPM (the intended path) [done for ios/probe]
`ios/probe` is a SwiftPM package: `Package.swift`, `xtool.yml` (`bundleID`, `infoPath: Info.plist`), a Swift
`@main` shim and two C-family targets. Build:
```
ios/probe/scripts/build_ipa.sh          # [--no-d3d9] [--debug]
```
which runs `xtool dev build -c release --ipa`, adds MoltenVK + DXVK + SDL3 to `Frameworks/`, and zips an **unsigned**
`build/DIOSProbe.ipa` (build/ is git-ignored). Findings:
- xtool compiles Swift, Objective-C and C++ targets in one SwiftPM build against the extracted SDK. Linkage:
  `LC_BUILD_VERSION platform 2 minos 15.0`; the Swift shim pulls in `/usr/lib/swift/*.dylib` (present on iOS 27).
- xtool passes `-platform_version ios 15.0 15.0` to the linker, i.e. the "SDK version" in the binary equals the minimum
  OS. I override it with `-Xlinker -platform_version -Xlinker ios -Xlinker 15.0 -Xlinker 27.1` in `Package.swift` so iOS 27
  does not apply old-SDK compatibility behaviour (the binary now reports `sdk 27.1`).
- xtool's final step ("copy `.app` into a temp `Payload/` and zip") **fails on this repo** because it lives on an NTFS
  `fuseblk` mount: `NSCocoaErrorDomain 513 ... Operation not permitted` while copying to `~/.cache/xtool`. The `.app` is
  already complete in `ios/probe/xtool/Probe.app` at that point, so `build_ipa.sh` tolerates the error and zips it itself.
- The IPA is unsigned: SideStore/LiveContainer sign it on install.
- `xtool dev` (build, sign, install over USB) was not exercised; it needs the Apple login (`xtool setup`) and `usbmuxd`
  (installed here). For this project the intended install route is SideStore + LiveContainer, not `xtool install`.
- Swift is only used for a 3-line `@main final class AppDelegate: PBAppDelegate {}`; the rest is Objective-C/C/C++ on purpose
  (see 2.2: it can be syntax-checked against an SDK without the Swift module machinery, and it keeps the shared C++ scene).

### 2.2 clang + ld64.lld + SDK sysroot (no Swift, no xtool) [verified structurally]
A real arm64 iOS Mach-O builds on Linux with plain clang:
```
clang --target=arm64-apple-ios15.0 -isysroot $IOS_SDK -fobjc-arc -c foo.m
clang --target=arm64-apple-ios15.0 -isysroot $IOS_SDK -B <dir with ld64.lld> -fuse-ld=lld foo.o -framework UIKit ...
```
This is what `tools/spikes/ios_cross/` uses for SDL3 and DXVK (CMake toolchain `ios-arm64.cmake`, meson cross file
`meson-arm64-ios.ini.in`). It is the path for the static-recompilation output, which is freestanding C and needs no Swift.
Source the environment with `. tools/spikes/ios_cross/env.sh` (sets `IOS_SDK`, `IOS_LD`, `PATH`). Required workarounds:
- **Linker:** the Xcode 27 SDK's `.tbd` stubs use arch names (`arm64e.x1`) that distro LLD 22.1.8 rejects ("unknown
  architecture"). Use xtool's `toolset/bin/orig/ld64.lld` (21.1.6). `build_rt_shim.sh` symlinks it to `out/ldbin/ld64.lld`
  and the scripts pass `-B out/ldbin -fuse-ld=lld`. (`--ld-path=` is not equivalent: clang then emits `ld64`-classic
  arguments and LLD errors with "must specify -platform_version".) `xtool`'s `toolset/bin/ld64.lld` is a wrapper script
  that only adds `-r` handling.
- **`___isPlatformVersionAtLeast`:** missing without Apple's compiler-rt. `rt_shim/os_version_check.c` implements it with
  `__availability_version_check` and is linked into every dylib/exe the scripts build.
- **libc++:** use the SDK's headers (`-nostdinc++ -isystem $SDK/usr/include/c++/v1`) for C++ code, see `graphics_path.md` section 3.
- A distro `llvm-otool`, `llvm-nm`, `llvm-lipo`, `llvm-ar` are enough to inspect results.
- As a stop-gap before the Xcode SDK existed I used `theos/sdks` `iPhoneOS16.5.sdk` (headers + `.tbd`, ~260 MB sparse
  checkout in `third_party/theos-sdks`). It was enough to compile and link SDL3/DXVK and to syntax-check the probe. These are
  Apple headers redistributed by a third party: local verification only, not a dependency of the project.

## 3. Meson cross file for DXVK -> arm64-apple-ios [built]

`tools/spikes/ios_cross/meson-arm64-ios.ini.in` (templated by `build_dxvk_ios.sh`). Essentials:
```
[binaries]      c='clang' cpp='clang++' ar='llvm-ar' strip='llvm-strip' pkg-config='pkg-config'
[properties]    needs_exe_wrapper=true ; pkg_config_libdir='<sdl3-ios-install>/lib/pkgconfig'
[built-in options]
c_args   = ['--target=arm64-apple-ios15.0','-isysroot','<SDK>','-mios-version-min=15.0']
cpp_args = [... same ..., '-nostdinc++','-isystem','<SDK>/usr/include/c++/v1']
c_link_args/cpp_link_args = ['--target=arm64-apple-ios15.0','-B<out/ldbin>','-fuse-ld=lld','-isysroot','<SDK>',
                             '-miphoneos-version-min=15.0','-L<out>','-lclang_rt_shim']
[host_machine]  system='darwin' subsystem='ios' kernel='xnu' cpu_family='aarch64' cpu='arm64' endian='little'
```
Run: `. tools/spikes/ios_cross/env.sh && tools/spikes/ios_cross/build_sdl3_ios.sh && tools/spikes/ios_cross/build_dxvk_ios.sh`.

Points from the Generals playbook, and how they came out here:
- *Loader looks for MoltenVK in the app's Frameworks:* needed, plus an env override (patch 1 in `graphics_path.md`).
- *`SDL_GetWindowSizeInPixels`:* already used by upstream DXVK 3.1.1 (the 2.6-era patch is obsolete).
- *`sdl3.pc`:* not hand-written; SDL3's own CMake install generates `lib/pkgconfig/sdl3.pc` for the iOS build, and the
  cross file points `pkg_config_libdir` at it. Without that, meson silently builds without SDL3 WSI / picks a host `.pc`.
  `build_dxvk_ios.sh` checks the result: `strings libdxvk_d3d9.0.dylib | grep Sdl3WsiDriver`.
- *Verify `LC_BUILD_VERSION platform 2`:* done by the script (`llvm-otool -l`).
- *Silent failures:* the script verifies artifacts rather than trusting exit codes, as the playbook recommends.

**Can it be built without the SDK?** No: compiling needs the SDK headers (libc++, `mach-o/dyld.h`, UIKit for SDL3) and
linking needs the `.tbd` stubs. With *an* SDK it builds entirely on Linux; no Mac, Xcode.app, or Swift is needed for DXVK
and SDL3. MoltenVK itself is **not** built here: building it needs Xcode (Metal shader compilation, SPIRV-Cross build
phases), so the prebuilt Khronos `MoltenVK-ios.tar` (v1.4.2, `third_party/moltenvk-release`) is used. A patched MoltenVK
would need the Mac or a CI macOS runner.

## 4. The probe app: `ios/probe` [verified structurally, unverified on device]

Layout: `Package.swift`, `xtool.yml`, `Info.plist`, `Sources/Probe/AppDelegate.swift` (Swift shim),
`Sources/ProbeCore/*.m|.c` (all logic and UI), `Sources/D3D9Scene` (symlinks to `tools/spikes/d3d9_test` sources and to
DXVK's native headers), `scripts/build_ipa.sh`, `tests/mem_test.c`.

| Probe | Implementation |
|---|---|
| Device/LiveContainer environment | `hw.machine`, OS, RAM, bundle path, executable path, `LC_*` env vars, loaded images of interest; recorded under `device` |
| Metal | `PBDeviceProbe.m`: GPU families by raw enum (Apple1-10, Common, Metal3/4), `supportsBCTextureCompression`, behavioural texture-creation test for BC1-7/ASTC/ETC2/RGBA/depth formats, `maxBufferLength`, `recommendedMaxWorkingSetSize` via runtime selector lookup (marked unavailable in the iOS headers), argument buffer tier, MetalFX spatial/temporal/denoised/frame-interpolator `supportsDevice:` via `NSClassFromString`; runs automatically at launch |
| Controllers | `PBControllers.m`: connect/disconnect notifications, live dump of `extendedGamepad` (A/B/X/Y, shoulders, triggers, sticks, dpad, menu, options, home, L3/R3) and generically of every element in `physicalInputProfile` (buttons, dpads, axes), plus a set of "every element ever pressed" (so Backbone extras show up) saved to JSON; Controllers tab |
| Memory | `PBMemory.c` + `PBMemoryRunner.m`: `os_proc_available_memory()` and `task_vm_info.phys_footprint` at start; `mmap` 4 GiB `PROT_NONE` (success/errno) plus 8/16/32/64 GiB VA reservation attempts; then `mprotect` RW + touch with incompressible data in 64 MB steps, logging available, footprint and implied limit (footprint + available) after each step; stops before a step that would leave `< 150 MB` available, on a memory warning, or at the 4 GiB reservation cap; records `max_safely_committed_mb`. Results are flushed after every step; if iOS kills the app mid-probe, the next launch reports "previous run ended in jetsam at ~N MB". The stepping logic was exercised on Linux with a fake limit (`PB_FAKE_LIMIT_MB=1000 tests/mem_test`: stopped at 832 MB with 166 MB left) |
| D3D9 test | `PBD3D9Runner.m`: `dlopen`s SDL3, MoltenVK and `libdxvk_d3d9.0.dylib` from `<bundle>/Frameworks` by absolute path (`DXVK_VULKAN_LIBRARY`, `SDL_VULKAN_LIBRARY`, `DXVK_WSI_DRIVER=SDL3`, `DXVK_LOG_PATH=Documents`), creates an SDL window (fullscreen, Vulkan, high-DPI), runs the shared scene for 8 s on a `CADisplayLink` (tap to stop), self-tests, saves `d3d9_test.png`, writes fps and CPU ms/frame to JSON. Everything is optional: the app runs if the libraries are absent |

UI: four buttons (memory, D3D9, Metal, Share), tabs Log / Controllers / Results JSON. Output files in the app's
`Documents`: `probe_results.json`, `probe_results_previous.json`, `probe_log.txt`, `d3d9_test.png`, `d3d9_d3d9.log`.
`Info.plist` has `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace`.

## 5. LiveContainer guest-app constraints (from LiveContainer's README/docs, v3.8.0) and how the probe fits

LiveContainer (LC) runs the guest by patching its main executable and `dlopen`ing it into LC's own process.

| LC behaviour / limitation | Consequence for us |
|---|---|
| Guest `MH_EXECUTE` is rewritten to `MH_DYLIB`, `__PAGEZERO` changed to `vmaddr 0xFFFFC000 size 0x4000`, a `TweakLoader.dylib` load command injected | Build a normal executable with `UIApplicationMain`; no custom entry. Static executables or fixed-address tricks will not survive. The guest cannot reuse the low 4 GB of the address space (LC's own `__PAGEZERO` still occupies it), which is consistent with the plan's "guest pointer = base + u32" design. |
| `@executable_path` is faked by hooking dyld's `_NSGetExecutablePath`; `NSBundle.mainBundle` is replaced | Use `NSBundle`/absolute paths for bundled dylibs; the probe and the DXVK patch take absolute paths. |
| Entitlements of the guest are **not applied**; the process has LC's entitlements (app group, keychain groups, `get-task-allow` from the dev cert, no increased-memory-limit, no extended VA) | The app needs none and declares none. The memory probe therefore measures the budget the game will really get. |
| App extensions are unsupported | None are built. |
| JIT-less mode (the default; JIT on iOS 26+ needs StikDebug plus a per-app script on A15+) | Fine: AOT recompilation needs no JIT; no `MAP_JIT`, no `mprotect(PROT_EXEC)` is used. The memory probe only uses RW anonymous memory. |
| All embedded executables must be signed; LC re-signs the guest with its own certificate via bundled zsign when JIT is not available | Embedded `Frameworks/*.dylib` and `MoltenVK.framework` are signed by LC; ship the framework as a proper `.framework` with `Info.plist` (done, as in the MoltenVK release). Unsigned IPA is expected. |
| Guest containers are not sandboxed from each other; "Fix File Picker", "Fix Local Notifications", orientation lock (`landscape`) and "Spoof SDK Version" are per-app settings | File sharing: the guest's `Documents` lives under LC's data (`LiveContainer/Data/Application/<uuid>/Documents`); reach it via the Files app (LC has file sharing enabled) or use the in-app "Share results JSON" button. Landscape-only games may need the orientation lock setting. The binary reports SDK 27.1 so "Spoof SDK Version" should not be needed. |
| Documented game crashes are "among the most difficult"; maintainers say keep expectations low | We are an unusual guest (Metal, GameController, 2.5+ GB of memory); the memory and D3D9 probes exist partly to find LC-specific failures early. |
| Permissions are global to LC | No special permissions are required. |

LiveContainer+SideStore 3.8.0 is the current release (`LiveContainer+SideStore.ipa`); the LC process itself already holds
some memory, which the probe's `avail_at_start_mb` shows.

## 6. User steps (what remains for a human)

Already done: Xcode download. Nothing else needs an Apple login on the Linux side.

1. Get `build/DIOSProbe.ipa` (4.4 MB, unsigned) onto the phone (AirDrop, iCloud Drive, a USB file manager, or `python3 -m http.server` from a folder containing only the .ipa).
2. LiveContainer -> "+" -> choose the .ipa -> install (SideStore signs LC itself; LC re-signs guests; no new Apple ID step).
3. Launch "DIOS Probe". The Metal probe and device info run automatically. Open the **Controllers** tab with the Backbone
   attached and press every button, stick and trigger once (also any extra Backbone button). Then tap **Run D3D9 test**
   (an SDL window covers the screen for ~8 s) and finally **Run memory probe** *last*: it deliberately pushes toward the limit and
   iOS may kill the app (relaunch: the log explains how far it got).
4. Retrieve results: tap **Share results JSON** (AirDrop/Files) or copy `probe_results.json`, `probe_log.txt`, `d3d9_test.png`
   and `d3d9_d3d9.log` from the guest's Documents via the Files app. Send them back.
5. To rebuild after changes: `ios/probe/scripts/build_ipa.sh` (Linux, needs the SDK installed as in section 1).

## 7. Reproducibility checklist
```
. tools/spikes/ios_cross/env.sh                   # PATH, IOS_SDK, IOS_LD
tools/spikes/ios_cross/build_sdl3_ios.sh          # third_party/sdl3-ios-install
tools/spikes/ios_cross/build_dxvk_ios.sh          # third_party/dxvk/install-ios (applies patches/dxvk-3.1.1-apple.patch)
ios/probe/scripts/build_ipa.sh                    # build/DIOSProbe.ipa
```
Third-party checkouts (all git-ignored under `third_party/`): `dxvk` (v3.1.1 + patch), `SDL3-src` (release-3.4.14),
`moltenvk-release` (v1.4.2 iOS tarball), `MoltenVK-src` (v1.4.2 source, for reading only), `theos-sdks` (optional).

## 8. Risks
- Nothing has run on a device. Highest uncertainty: the SDL3-window-inside-our-UIKit-app approach, DXVK device creation on
  MoltenVK, and dyld resolution of embedded dylibs under LC's patched `@executable_path`.
- xtool packaging on an NTFS mount needs the manual zip workaround; moving the repo or a build copy to a native Linux
  filesystem would make `xtool dev build --ipa` work unmodified.
- The Linux-built binaries depend on xtool's LLD 21 and on the SDK's `.tbd` files; a future Xcode SDK may need a newer
  LLD again. Keep xtool updated (`xtool sdk` will warn).
- `theos-sdks` is only for local fallback verification.
