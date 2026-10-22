#!/bin/sh
# print the generated C of a function: fn.sh 78562681
G=$(cd "$(dirname "$0")/../.." && pwd)/generated
f=$(grep -l "^[A-Za-z]* g_$1\(_x\)\?(.*) {" $G/*/shard_*.c | head -1)
n=$(grep -n "^[A-Za-z]* g_$1\(_x\)\?(.*) {" $f | head -1 | cut -d: -f1)
sed -n "${n},\$p" $f | awk '{print} /^}/{exit}'
