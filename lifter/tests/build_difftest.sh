#!/bin/sh
# Lift the three modules WITHOUT native overrides into private/difftest/gen and link the differential-test driver.
# usage: build_difftest.sh [relink]   (relink: only rebuild diffdrv.c against the existing objects)
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
GD=${GAME_DIR:-$(for d in "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Dishonored" "$HOME/.local/share/Steam/steamapps/common/Dishonored"; do [ -d "$d" ] && echo "$d" && break; done)}
OUT=$R/private/difftest; GEN=$OUT/gen; mkdir -p $OUT $GEN $OUT/obj
if [ "$1" != relink ]; then
  cd $R/lifter
  ./target/release/lifter lift --config config/difftest.toml --game $GD/ --out $GEN 2>&1 | tail -2
  cd $GEN
  rm -f $OUT/obj/*.o
  FL="-std=c11 -ffreestanding -O2 -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -fvisibility=hidden -march=x86-64-v2"
  ls */shard_*.c dispatch.c program.c | xargs -P 12 -I{} sh -c "clang $FL -I. -I$R/include -c {} -o $OUT/obj/\$(echo {} | tr / _).o"
fi
clang -O1 -g -std=gnu11 -march=x86-64-v2 -I$GEN -I$R/include -no-pie -o $OUT/diffdrv $R/lifter/tests/diffdrv.c $OUT/obj/*.o -lm
echo built $OUT/diffdrv
