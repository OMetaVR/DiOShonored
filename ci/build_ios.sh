#!/bin/bash
# Builds the iOS app on macOS with Xcode and packs an unsigned IPA (SideStore / LiveContainer sign it on install).
#
#   ci/build_ios.sh                      the app shell: no game code inside, it boots to the "import your game files" screen
#   ci/build_ios.sh --game <Dishonored>  the playable app, recompiled from YOUR copy of the Steam (PC) game. Keep this IPA to
#                                        yourself: it contains code translated from Dishonored.exe.
#
# Needs: Xcode (command line tools), cmake, ninja, git; with --game also a Rust toolchain (cargo).
# Output: build/DiOShonored-shell.ipa or build/DiOShonored.ipa
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
game=""
while [ $# -gt 0 ]; do
  case "$1" in
    --game) game="$2"; shift 2;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
done
jobs=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}
work="$root/build/ci"
mkdir -p "$work"

# ---- SDL3, static for iOS (always linked statically: LiveContainer cannot load an app with a Frameworks/ folder)
SDL_TAG=${SDL_TAG:-release-3.4.14}
sdl_src="$work/SDL-$SDL_TAG"
sdl_inst="$work/sdl3-ios-static"
if [ ! -f "$sdl_inst/lib/libSDL3.a" ]; then
  [ -d "$sdl_src" ] || git clone --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL.git "$sdl_src"
  cmake -S "$sdl_src" -B "$work/sdl-build" -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF \
    -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$sdl_inst"
  cmake --build "$work/sdl-build" -j "$jobs"
  cmake --install "$work/sdl-build"
fi

# ---- the recompiled program (only with --game): lift the user's own Dishonored.exe and its DLLs into generated/
gen_flag=OFF
if [ -n "$game" ]; then
  [ -f "$game/Binaries/Win32/Dishonored.exe" ] || { echo "not a Dishonored (Steam, PC) folder: $game" >&2; exit 1; }
  (cd "$root/lifter" && cargo build --release)
  GAME_DIR="$game" "$root/lifter/regen.sh"
  gen_flag=ON
fi

# ---- the app
b="$root/build/ios-xcode"
cmake -S "$root/ios/game" -B "$b" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$root/ci/ios-macos.cmake" -DCMAKE_BUILD_TYPE=Release \
  -DSDL3_DIR="$sdl_inst/lib/cmake/SDL3" -DDISH_USE_GENERATED=$gen_flag -DDISH_BUILD_D3D9TEST=OFF
cmake --build "$b" -j "$jobs"

name=$([ "$gen_flag" = ON ] && echo DiOShonored || echo DiOShonored-shell)
stage="$work/stage"
rm -rf "$stage" && mkdir -p "$stage/Payload"
cp -R "$b/Dishonored.app" "$stage/Payload/"
ipa="$root/build/$name.ipa"
rm -f "$ipa"
(cd "$stage" && zip -qry "$ipa" Payload)
if otool -L "$stage/Payload/Dishonored.app/Dishonored" | tail -n +2 | grep -vE "/usr/lib/|/System/Library/"; then
  echo "ERROR: the app links a non-system library" >&2
  exit 1
fi
echo "built $ipa ($(du -h "$ipa" | cut -f1))"
