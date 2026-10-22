#!/bin/sh
# Cross-builds SDL3 (dynamic) for arm64-apple-ios on Linux -> third_party/sdl3-ios-install
# Needs: cmake, ninja, clang, ld64.lld, IOS_SDK, third_party/SDL3-src (git clone --branch release-3.4.14 libsdl-org/SDL).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
: "${IOS_SDK:?set IOS_SDK to an iPhoneOS.sdk directory}"
SRC=${SDL3_SRC:-$root/third_party/SDL3-src}
OUT=${SDL3_IOS:-$root/third_party/sdl3-ios-install}
"$here/build_rt_shim.sh" >/dev/null
rm -rf "$SRC/build-ios"
cmake -S "$SRC" -B "$SRC/build-ios" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$here/ios-arm64.cmake" -DIOS_SDK="$IOS_SDK" \
  -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
  -DSDL_INSTALL_TESTS=OFF -DCMAKE_INSTALL_PREFIX="$OUT"
ninja -C "$SRC/build-ios" install
llvm-otool -l "$OUT/lib/libSDL3.0.dylib" | grep -A4 LC_BUILD_VERSION | grep -E "platform|minos"
llvm-otool -D "$OUT/lib/libSDL3.0.dylib"
