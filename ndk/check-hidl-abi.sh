#!/usr/bin/env bash
# Compares each vendor.waydroid.* library rebuilt from hidl-gen output with the
# copy in vendor.img (strong exports and vtable slot layout).
#
# usage: check-hidl-abi.sh <cmake build dir> <sysroot-dir>
set -euo pipefail

BUILD_DIR=$1
SYSROOT=$2
ANDROID_NDK=${ANDROID_NDK:-$HOME/Android/Sdk/ndk/27.2.12479018}
READELF=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf
TOOLS=$(cd "$(dirname "$0")/tools" && pwd)

status=0
for rebuilt in "$BUILD_DIR"/vendor.waydroid.*.so; do
    python3 -I "$TOOLS/hidl_abi.py" "$READELF" "$rebuilt" "$SYSROOT/lib64/$(basename "$rebuilt")" || status=1
done
exit $status
