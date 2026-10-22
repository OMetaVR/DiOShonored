#!/bin/bash
# Builds the probe app on Linux with xtool (no Apple account needed) and packs an UNSIGNED .ipa into
# <repo>/build/DIOSProbe.ipa, with MoltenVK + DXVK d3d9 + SDL3 embedded in Frameworks/ for the "Run D3D9 test" button.
# SideStore/LiveContainer sign it on install. Prereqs: see research/platform/ios_toolchain.md
#   usage: scripts/build_ipa.sh [--no-d3d9] [--debug]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
probe=$(cd "$here/.." && pwd)
root=$(cd "$probe/../.." && pwd)
. "$root/tools/spikes/ios_cross/env.sh"          # PATH for swift + xtool, IOS_SDK, IOS_LD
with_d3d9=1; cfg=release
for a in "$@"; do case "$a" in --no-d3d9) with_d3d9=0;; --debug) cfg=debug;; esac; done

mvk="$root/third_party/moltenvk-release/MoltenVK/MoltenVK/dynamic/MoltenVK.xcframework/ios-arm64/MoltenVK.framework"
dxvk="$root/third_party/dxvk/install-ios/lib/libdxvk_d3d9.0.dylib"
sdl="$root/third_party/sdl3-ios-install/lib/libSDL3.0.dylib"
if [ $with_d3d9 = 1 ]; then
  for f in "$mvk/MoltenVK" "$dxvk" "$sdl"; do [ -e "$f" ] || { echo "missing $f (run tools/spikes/ios_cross/build_sdl3_ios.sh and build_dxvk_ios.sh, fetch MoltenVK)"; exit 1; }; done
fi

# 1. build the .app. xtool's own last step (copy the .app into a temp Payload dir) fails when the repo lives on an
#    NTFS/fuseblk mount ("Operation not permitted"), so tolerate that error and pack the .app ourselves.
cd "$probe"
rm -rf xtool/Probe.app
fifo=$(mktemp -u "${TMPDIR:-/tmp}/xtool-stdin.XXXXXX"); mkfifo "$fifo"; exec 3<>"$fifo"; rm -f "$fifo"   # xtool dies if stdin is /dev/null
xtool dev build -c "$cfg" --ipa <&3 || echo "(xtool ipa step failed; packing manually)"
exec 3>&-
[ -f xtool/Probe.app/Probe ] || { echo "xtool did not produce xtool/Probe.app"; exit 1; }

# 2. stage + embed
stage="$root/build/probe-stage"; rm -rf "$stage"; mkdir -p "$stage/Payload"
cp -R xtool/Probe.app "$stage/Payload/Probe.app"
app="$stage/Payload/Probe.app"
if [ $with_d3d9 = 1 ]; then
  mkdir -p "$app/Frameworks"
  cp -R "$mvk" "$app/Frameworks/MoltenVK.framework"
  cp "$dxvk" "$sdl" "$app/Frameworks/"
fi
# 3. zip (ipa = zip with Payload/ at the root)
mkdir -p "$root/build"; ipa="$root/build/DIOSProbe.ipa"; rm -f "$ipa"
(cd "$stage" && zip -qry "$ipa" Payload)
echo "built $ipa ($(du -h "$ipa" | cut -f1))"
llvm-otool -l "$app/Probe" | grep -A4 LC_BUILD_VERSION | grep -E "platform|minos|sdk" | tr '\n' ' '; echo
llvm-otool -L "$app/Probe" | tail -n +2 | awk '{print "  links " $1}'
