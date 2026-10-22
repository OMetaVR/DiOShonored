#!/bin/sh
# regenerate (optional), build the runtime with generated/, run the Linux host. usage: runlinux.sh [seconds]
R=$(cd "$(dirname "$0")/../.." && pwd)
GD=${GAME_DIR:-$(for d in "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Dishonored" "$HOME/.local/share/Steam/steamapps/common/Dishonored"; do [ -d "$d" ] && echo "$d" && break; done)}
cd $R && ninja -C build/lifter_rt dishonored_linux 2>&1 | tail -1 | cut -c1-200
timeout ${1:-60} build/lifter_rt/host/linux/dishonored_linux --game-root $GD --save-dir private/save --log info > /tmp/run.log 2>&1; echo "exit $?"
grep -n "GUEST\|guest address\|host symbol\|unresolved\|assert\|abort" /tmp/run.log | head -8 | cut -c1-200
grep -A12 "host backtrace" /tmp/run.log | grep -o "+0x[0-9a-f]*" | head -12 | while read o; do printf "%s " $o; addr2line -f -e build/lifter_rt/host/linux/dishonored_linux $o | head -1; done
tail -3 /tmp/run.log | cut -c1-200
