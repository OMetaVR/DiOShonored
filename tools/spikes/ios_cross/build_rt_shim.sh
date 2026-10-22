#!/bin/sh
# Builds out/libclang_rt_shim.a (arm64 iOS). Needs: clang, llvm-ar, IOS_SDK.
set -e
here=$(cd "$(dirname "$0")" && pwd)
: "${IOS_SDK:?set IOS_SDK to an iPhoneOS.sdk directory}"
mkdir -p "$here/out/ldbin"
ln -sf "${IOS_LD:-$(command -v ld64.lld)}" "$here/out/ldbin/ld64.lld"
clang --target=arm64-apple-ios15.0 -isysroot "$IOS_SDK" -O2 -c "$here/rt_shim/os_version_check.c" -o "$here/out/os_version_check.o"
rm -f "$here/out/libclang_rt_shim.a"
llvm-ar rcs "$here/out/libclang_rt_shim.a" "$here/out/os_version_check.o"
echo "built $here/out/libclang_rt_shim.a"
