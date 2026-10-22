#!/bin/sh
# Compile every generated shard for a target; prints error summary.  usage: ccall.sh <x86|arm64> [outdir]
T=${1:-x86}; GEN=${GEN:-$(cd "$(dirname "$0")/../.." && pwd)/generated}; OUT=${2:-/tmp/cc_$T}
mkdir -p $OUT; rm -f $OUT/*
if [ "$T" = x86 ]; then FL="-march=x86-64-v2"; else FL="--target=arm64-apple-ios15.0 -mcpu=apple-a15"; fi
cd $GEN
ls */shard_*.c dispatch.c program.c | xargs -P 12 -I{} sh -c "s=\$(date +%s%N); clang -std=c11 -ffreestanding -O2 -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -fvisibility=hidden $FL -I. -I$GEN/../include -c {} -o $OUT/\$(echo {} | tr / _).o 2> $OUT/\$(echo {} | tr / _).err; e=\$(date +%s%N); echo \"{} \$(( (e - s) / 1000000 ))\" >> $OUT/times.txt"
echo "errors: $(cat $OUT/*.err | grep -c 'error:')"
cat $OUT/*.err | grep "error:" | sed 's/.*error: //' | sed "s/'[A-Za-z0-9_]*'/X/g" | sort | uniq -c | sort -rn | head -${TOPN:-15}
