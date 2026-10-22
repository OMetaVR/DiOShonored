#!/bin/sh
# Fast dev loop: compile a generated tree at -O1 (parallel), link it with the runtime objects of build/lifter_rt, run the Linux host.
# usage: devrun.sh [gendir] [seconds] [-gdb]     (default gendir: generated)
R=$(cd "$(dirname "$0")/../.." && pwd)
G=${1:-$R/generated}; SECS=${2:-60}
GD=${GAME_DIR:-$(for d in "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Dishonored" "$HOME/.local/share/Steam/steamapps/common/Dishonored"; do [ -d "$d" ] && echo "$d" && break; done)}
O=${DEVRUN_OBJ:-$R/private/dev/obj}; mkdir -p $O
cd $G
FL="-std=c11 -ffreestanding -O1 -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -fvisibility=hidden -march=x86-64-v2"
ls */shard_*.c dispatch.c program.c trace.c lx_hooks.c 2>/dev/null | nice -n 10 xargs -P 4 -I{} sh -c "o=$O/\$(echo {} | tr / _).o; if [ ! -f \$o ] || [ {} -nt \$o ]; then clang $FL -I. -I$R/include -c {} -o \$o; fi"
cd $R/build/lifter_rt
CMD=$(ninja -t commands dishonored_linux | tail -1 | sed 's/^: && //; s/ && :$//')
CMD=$(echo "$CMD" | tr ' ' '\n' | grep -v "dish_generated.dir" | tr '\n' ' ')
sh -c "$CMD $O/*.o -o $R/private/dev/dishonored_dev" 2>&1 | tail -3
cd $R
if [ "$3" = "-gdb" ]; then
  timeout 300 gdb -batch -ex "handle SIGSEGV stop nopass" -ex "handle SIGALRM nostop noprint" -ex run -ex "bt 12" --args private/dev/dishonored_dev --game-root $GD --save-dir ${DEVRUN_SAVE:-private/save} --log info 2>&1 | grep -E "^#" | sed 's/ at \/mnt.*//' | cut -c1-240
else
  $DEVRUN_ENV timeout $SECS private/dev/dishonored_dev --game-root $GD --save-dir ${DEVRUN_SAVE:-private/save} --log info > /tmp/run.log 2>&1; echo "exit $?"
  grep -v "^\[" /tmp/run.log | head -12 | cut -c1-200
  grep "info\|warn\|UNIMPL" /tmp/run.log | tail -4 | cut -c1-200
fi
grep -o "(+0x[0-9a-f]*)" /tmp/run.log | tr -d '()+' | while read o; do printf "%s " $o; addr2line -f -e $R/private/dev/dishonored_dev $o | head -1; done | grep "g_" | head -8
