#!/bin/sh
# Regenerate generated/ from the three original modules.  usage: lifter/regen.sh [extra lifter args, e.g. --trace]
R=$(cd "$(dirname "$0")/.." && pwd)
GD=${GAME_DIR:-$(for d in "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Dishonored" "$HOME/.local/share/Steam/steamapps/common/Dishonored"; do [ -d "$d" ] && echo "$d" && break; done)}
cd "$R/lifter" && cargo build --release -q && rm -rf "$R/generated" && ./target/release/lifter lift --config config/dishonored.toml --game "$GD/" --out "$R/generated" --sigs "$R/runtime/hle/sigs_generated.inc" "$@"
