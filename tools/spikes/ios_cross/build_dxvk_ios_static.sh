#!/bin/sh
# Cross-builds DXVK 3.x d3d9 as STATIC archives for arm64-apple-ios on Linux (the iOS game app links them into its
# executable together with libSDL3.a and libMoltenVK.a). Output: <dxvk>/install-ios-static/lib/*.a
# Applies patches/dxvk-3.1.1-apple.patch (shared with the dynamic build) and patches/dxvk-3.1.1-static.patch:
#   -Dstatic_d3d9=true builds libdxvk_d3d9.a and defines DXVK_STATIC, which makes the SDL3 WSI call SDL directly
#   (no LoadLibrary("libSDL3.0.dylib")) and makes the Vulkan loader use the linked vkGetInstanceProcAddr (MoltenVK).
# Prereqs: build_sdl3_ios_static.sh has run (headers + pkg-config), IOS_SDK set (source env.sh), meson, ninja, glslangValidator.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
: "${IOS_SDK:?set IOS_SDK to an iPhoneOS.sdk directory (source env.sh)}"
IOS_MIN=${IOS_MIN:-15.0}
DXVK=${DXVK:-$root/third_party/dxvk}
SDL3_IOS=${SDL3_IOS_STATIC:-$root/third_party/sdl3-ios-static-install}
"$here/build_rt_shim.sh" >/dev/null
apple="$here/patches/dxvk-3.1.1-apple.patch"; static="$here/patches/dxvk-3.1.1-static.patch"
if git -C "$DXVK" apply --reverse --check "$static" 2>/dev/null; then
  echo "dxvk apple+static patches already applied"          # (the static patch builds on the apple one)
else
  git -C "$DXVK" apply --reverse --check "$apple" 2>/dev/null && echo "dxvk apple patch already applied" || git -C "$DXVK" apply "$apple"
  git -C "$DXVK" apply "$static"
fi
mkdir -p "$here/out"
sed -e "s#@IOS_SDK@#$IOS_SDK#g" -e "s#@IOS_MIN@#$IOS_MIN#g" -e "s#@SHIM_DIR@#$here/out#g" \
    -e "s#@SDL3_PKGCONFIG@#$SDL3_IOS/lib/pkgconfig#g" "$here/meson-arm64-ios.ini.in" > "$here/out/meson-arm64-ios-static.ini"
cd "$DXVK"
rm -rf build-ios-static install-ios-static
PKG_CONFIG_LIBDIR="$SDL3_IOS/lib/pkgconfig" meson setup build-ios-static --cross-file "$here/out/meson-arm64-ios-static.ini" \
  --buildtype=release -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=true \
  -Dnative_sdl2=disabled -Dnative_glfw=disabled -Dnative_sdl3=enabled -Dstatic_d3d9=true --prefix="$DXVK/install-ios-static"
# only the d3d9 archive and its static dependencies (the dxgi shared library would fail to link without SDL; not needed)
ninja -C build-ios-static src/d3d9/libdxvk_d3d9.a src/dxvk/libdxvk.a src/util/libutil.a src/spirv/libspirv.a src/wsi/libwsi.a \
  subprojects/libdisplay-info/libdisplay-info.a subprojects/dxbc-spirv/libdxbc_spv.a src/vulkan/libvkcommon.a
mkdir -p install-ios-static/lib
# every archive the d3d9 link needs: d3d9 itself and its static dependencies
find build-ios-static -name '*.a' -exec cp {} install-ios-static/lib/ \;
ls -la install-ios-static/lib
llvm-nm install-ios-static/lib/libdxvk_d3d9.a 2>/dev/null | grep -E " T _?Direct3DCreate9$" || { echo "ERROR: Direct3DCreate9 missing"; exit 1; }
