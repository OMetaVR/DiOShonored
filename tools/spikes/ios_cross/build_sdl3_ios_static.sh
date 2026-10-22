#!/bin/sh
# Cross-builds SDL3 as a STATIC library for arm64-apple-ios on Linux -> third_party/sdl3-ios-static-install
# (the iOS game app links it into its executable; see ios/game). Patches SDL's UIKit Vulkan loader so a statically linked
# libMoltenVK.a is found by direct symbol reference (dlsym(RTLD_DEFAULT) fails when LiveContainer loads the app as a dylib).
# By default SDL's Vulkan support is OFF (the iOS d3d9 backend is direct Metal; nothing needs libMoltenVK). WITH_VULKAN=1 builds it with the
# static-MoltenVK patch for the DXVK + MoltenVK experiment (then the app must also link libMoltenVK.a).
# Needs: cmake, ninja, clang, ld64.lld, IOS_SDK, third_party/SDL3-src (release-3.4.14).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
: "${IOS_SDK:?set IOS_SDK to an iPhoneOS.sdk directory (source env.sh)}"
SRC=${SDL3_SRC:-$root/third_party/SDL3-src}
OUT=${SDL3_IOS_STATIC:-$root/third_party/sdl3-ios-static-install}
"$here/build_rt_shim.sh" >/dev/null
patch="$here/patches/sdl3-3.4.14-static-moltenvk.patch"
vk=OFF; [ "${WITH_VULKAN:-0}" = 1 ] && vk=ON
if git -C "$SRC" apply --reverse --check "$patch" 2>/dev/null; then echo "sdl patch already applied"; else git -C "$SRC" apply "$patch"; fi
IOS_MIN=${IOS_MIN:-15.0}
# (overriding CMAKE_C_FLAGS replaces the toolchain file's defaults, so repeat them)
flags="--target=arm64-apple-ios$IOS_MIN -isysroot $IOS_SDK -mios-version-min=$IOS_MIN -DSDL_VULKAN_STATIC_MOLTENVK=1"
BUILD=${SDL3_BUILD_DIR:-build-ios-static}
rm -rf "$SRC/$BUILD"
cmake -S "$SRC" -B "$SRC/$BUILD" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$here/ios-arm64.cmake" -DIOS_SDK="$IOS_SDK" \
  -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
  -DSDL_INSTALL_TESTS=OFF -DSDL_VULKAN=$vk -DCMAKE_C_FLAGS="$flags" -DCMAKE_OBJC_FLAGS="$flags" -DCMAKE_INSTALL_PREFIX="$OUT"
ninja -C "$SRC/$BUILD" install
ls -la "$OUT/lib/libSDL3.a"
