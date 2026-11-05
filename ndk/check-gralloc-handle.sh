#!/usr/bin/env bash
# H3: proves the cros_gralloc_handle layout in the fork's minigbm header matches
# what the pinned vendor image's minigbm writes and what the stock HWC reads.
# x86_64 only (it pattern-matches x86_64 disassembly).
#
# usage: check-gralloc-handle.sh <unstripped built hwcomposer.waydroid.so>
set -euo pipefail

BUILT=$1
S2_CACHE=${S2_CACHE:-$HOME/.cache/crownos-android/s2}
IMAGES=${WAYDROID_IMAGES:-/var/lib/waydroid/images}
ANDROID_NDK=${ANDROID_NDK:-$HOME/Android/Sdk/ndk/27.2.12479018}
LLVM_BIN=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
STOCK_DIR=$S2_CACHE/stock/x86_64
WORK_DIR=$S2_CACHE/build/gralloc-probe
MINIGBM=$STOCK_DIR/libminigbm_gralloc_gbm_mesa.so
TOOLS=$(cd "$(dirname "$0")/tools" && pwd)

mkdir -p "$STOCK_DIR" "$WORK_DIR"
[[ -f $MINIGBM ]] || debugfs -R "dump /lib64/libminigbm_gralloc_gbm_mesa.so $MINIGBM" "$IMAGES/vendor.img" 2> /dev/null
[[ -f $STOCK_DIR/hwcomposer.waydroid.so ]] ||
    debugfs -R "dump /lib64/hw/hwcomposer.waydroid.so $STOCK_DIR/hwcomposer.waydroid.so" "$IMAGES/vendor.img" 2> /dev/null

python3 -I "$TOOLS/gralloc_layout.py" "$LLVM_BIN" "$LLVM_BIN/clang++" \
    "$S2_CACHE/aosp/external_minigbm" "$S2_CACHE/aosp/system_core/libcutils/include" \
    "$MINIGBM" "$STOCK_DIR/hwcomposer.waydroid.so" "$BUILT" "$WORK_DIR"
