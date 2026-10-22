#!/bin/sh
# Cross-builds DXVK 3.x d3d9 (+dxgi) for arm64-apple-ios on Linux. Output: <dxvk>/install-ios/lib/*.dylib
# Prereqs (see ios_toolchain.md): clang, ld64.lld, llvm-ar, meson, ninja, glslangValidator,
#   IOS_SDK=<iPhoneOS.sdk>, SDL3 built for iOS into $SDL3_IOS (see build_sdl3_ios.sh), third_party/dxvk checkout.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
: "${IOS_SDK:?set IOS_SDK to an iPhoneOS.sdk directory}"
IOS_MIN=${IOS_MIN:-15.0}
DXVK=${DXVK:-$root/third_party/dxvk}
SDL3_IOS=${SDL3_IOS:-$root/third_party/sdl3-ios-install}
"$here/build_rt_shim.sh" >/dev/null
# 1. patches (idempotent). dxvk-3.1.1-apple.patch = Apple support DXVK lacks upstream:
#    Vulkan loader search order (MoltenVK in app bundle / DXVK_VULKAN_LIBRARY), _NSGetExecutablePath, Darwin
#    pthread_setname_np, __APPLE__ in util_win32_compat.h, <cstddef>, no --version-script on Mach-O, and
#    (EXPERIMENTAL) downgrading geometryShader/shaderCullDistance/depthClipEnable/textureCompressionBC from
#    required to optional so vkCreateDevice can succeed on MoltenVK.
patch="$here/patches/dxvk-3.1.1-apple.patch"
if git -C "$DXVK" apply --reverse --check "$patch" 2>/dev/null; then
  echo "dxvk patch already applied"
else
  git -C "$DXVK" apply "$patch"
fi
# 2. cross file
sed -e "s#@IOS_SDK@#$IOS_SDK#g" -e "s#@IOS_MIN@#$IOS_MIN#g" -e "s#@SHIM_DIR@#$here/out#g" \
    -e "s#@SDL3_PKGCONFIG@#$SDL3_IOS/lib/pkgconfig#g" "$here/meson-arm64-ios.ini.in" > "$here/out/meson-arm64-ios.ini"
# 3. configure + build. PKG_CONFIG_LIBDIR must point at the iOS SDL3 .pc, or meson silently picks host SDL2/none.
cd "$DXVK"
rm -rf build-ios
PKG_CONFIG_LIBDIR="$SDL3_IOS/lib/pkgconfig" meson setup build-ios --cross-file "$here/out/meson-arm64-ios.ini" \
  --buildtype=release -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false \
  -Dnative_sdl2=disabled -Dnative_glfw=disabled -Dnative_sdl3=enabled --prefix="$DXVK/install-ios"
ninja -C build-ios install
# 4. verify (exit codes lie: check the artifacts)
for f in "$DXVK"/install-ios/lib/libdxvk_d3d9*.dylib; do
  [ -f "$f" ] && [ ! -L "$f" ] || continue
  echo "== $f"; llvm-otool -l "$f" | grep -A4 LC_BUILD_VERSION | grep -E "platform|minos"
  strings -a "$f" | grep -m1 Sdl3WsiDriver >/dev/null && echo "Sdl3WsiDriver: present" || { echo "ERROR: SDL3 WSI missing"; exit 1; }
done
