#!/bin/sh
# run the Linux host under gdb and print the guest call chain at the first fault: gdbrun.sh [n frames]
R=$(cd "$(dirname "$0")/../.." && pwd)
GD=${GAME_DIR:-$(for d in "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Dishonored" "$HOME/.local/share/Steam/steamapps/common/Dishonored"; do [ -d "$d" ] && echo "$d" && break; done)}
cd $R && timeout 200 gdb -batch -ex "handle SIGSEGV stop nopass" -ex "handle SIGALRM nostop noprint" -ex run -ex "bt ${1:-10}" --args build/lifter_rt/host/linux/dishonored_linux --game-root $GD --save-dir private/save --log info 2>&1 | grep -E "^#" | sed 's/ at \/mnt.*//' | cut -c1-260
