#!/bin/bash
# Cross-builds the Dishonored iOS app on Linux (clang + ld64.lld + the iPhoneOS SDK that xtool extracts) and packs an UNSIGNED
# .ipa into <repo>/build/Dishonored.ipa. SideStore / LiveContainer sign it on install. There is no Frameworks/ folder.
#
#   ios/game/scripts/build_ipa.sh [--with-generated] [--debug] [--skip-deps]
#
# Default links the empty placeholder program, so the app boots to the "import your game files" screen. --with-generated links
# generated/ (the lifter's output; compiling it takes a long time). Prereqs: tools/spikes/ios_cross (env.sh, build_rt_shim.sh),
# cmake, ninja, clang. SDL3 is built statically on demand by tools/spikes/ios_cross/build_sdl3_ios_static.sh.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
. "$root/tools/spikes/ios_cross/env.sh"
gen=OFF; type=Release; deps=1
for a in "$@"; do case "$a" in --with-generated) gen=ON;; --debug) type=RelWithDebInfo;; --skip-deps) deps=0;; esac; done

sdl="$root/third_party/sdl3-ios-static-install"
if [ $deps = 1 ] && [ ! -f "$sdl/lib/libSDL3.a" ]; then "$root/tools/spikes/ios_cross/build_sdl3_ios_static.sh"; fi
"$root/tools/spikes/ios_cross/build_rt_shim.sh" >/dev/null

b="$root/build/ios-game"
cmake -S "$root/ios/game" -B "$b" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$root/tools/spikes/ios_cross/ios-arm64.cmake" \
  -DCMAKE_BUILD_TYPE=$type -DSDL3_DIR="$sdl/lib/cmake/SDL3" -DDISH_USE_GENERATED=$gen
cmake --build "$b"

stage="$root/build/ios-game-stage"; rm -rf "$stage"; mkdir -p "$stage/Payload"
cp -R "$b/Dishonored.app" "$stage/Payload/Dishonored.app"
ipa="$root/build/Dishonored.ipa"; rm -f "$ipa"
(cd "$stage" && zip -qry "$ipa" Payload)
echo "built $ipa ($(du -h "$ipa" | cut -f1))"
app="$stage/Payload/Dishonored.app"
llvm-otool -l "$app/Dishonored" | grep -A4 LC_BUILD_VERSION | grep -E "platform|minos|sdk" | tr '\n' ' '; echo
echo "app bundle contents:"; (cd "$app" && find . -type f | sort)
if llvm-otool -L "$app/Dishonored" | tail -n +2 | grep -vE "/usr/lib/|/System/Library/"; then echo "ERROR: non-system dylib dependency"; exit 1; fi
echo "links only system libraries (SDL3 static, no Frameworks/)"

# the d3d9 acceptance app (DISH_BUILD_D3D9TEST, on by default)
if [ -d "$b/D3D9Test.app" ]; then
  tstage="$root/build/ios-d3d9test-stage"; rm -rf "$tstage"; mkdir -p "$tstage/Payload"
  cp -R "$b/D3D9Test.app" "$tstage/Payload/D3D9Test.app"
  tipa="$root/build/d3d9test.ipa"; rm -f "$tipa"
  (cd "$tstage" && zip -qry "$tipa" Payload)
  if llvm-otool -L "$tstage/Payload/D3D9Test.app/D3D9Test" | tail -n +2 | grep -vE "/usr/lib/|/System/Library/"; then echo "ERROR: d3d9test links a non-system dylib"; exit 1; fi
  echo "built $tipa ($(du -h "$tipa" | cut -f1))"
fi
