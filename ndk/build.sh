#!/usr/bin/env bash
# Builds hwcomposer.waydroid.so out of tree:
#   1. fetch the Android 13 sources (headers, hidl-gen, wayland, libffi, xkbcommon)
#   2. extract the link sysroot from the Waydroid images
#   3. build the host tools (hidl-gen, wayland-scanner)
#   4. generate the HIDL/AIDL headers
#   5. cross-compile with the NDK (HWC plus the vendor.waydroid.* HIDL libraries)
#   6. check imports (H2), HIDL ABI against vendor.img, gralloc handle layout (H3)
#
# usage: build.sh [x86_64|arm64]
# env:   ANDROID_NDK, S2_CACHE, WAYDROID_IMAGES (images of the same ABI),
#        JOBS (default 4), AIDL, HOST_CC, HOST_CXX
set -euo pipefail

ABI=${1:-x86_64}
NDK_DIR=$(cd "$(dirname "$0")" && pwd)
S2_CACHE=${S2_CACHE:-$HOME/.cache/crownos-android/s2}
ANDROID_NDK=${ANDROID_NDK:-$HOME/Android/Sdk/ndk/27.2.12479018}
AIDL=${AIDL:-$HOME/Android/Sdk/build-tools/34.0.0/aidl}
JOBS=${JOBS:-4}
API_LEVEL=33
export S2_CACHE ANDROID_NDK

case $ABI in
    x86_64) ANDROID_ABI=x86_64 ;;
    arm64) ANDROID_ABI=arm64-v8a ;;
    *) echo "unsupported ABI: $ABI" >&2; exit 1 ;;
esac

HOST_BUILD=$S2_CACHE/build/host
TARGET_BUILD=$S2_CACHE/build/$ABI
OUTPUT=$S2_CACHE/out/$ABI

"$NDK_DIR/fetch-sources.sh"
"$NDK_DIR/make-sysroot.sh" "$ABI"

cmake -G Ninja -S "$NDK_DIR/host" -B "$HOST_BUILD" \
    -DCMAKE_C_COMPILER="${HOST_CC:-clang}" -DCMAKE_CXX_COMPILER="${HOST_CXX:-clang++}" \
    -DCMAKE_BUILD_TYPE=Release -DAOSP_DIR="$S2_CACHE/aosp"
cmake --build "$HOST_BUILD" -j "$JOBS"

"$NDK_DIR/generate-interfaces.sh" "$HOST_BUILD/hidl-gen" "$AIDL" "$TARGET_BUILD/gen-interfaces"

cmake -G Ninja -S "$NDK_DIR" -B "$TARGET_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ANDROID_ABI" \
    -DANDROID_PLATFORM="android-$API_LEVEL" \
    -DANDROID_STL=none \
    -DCMAKE_BUILD_TYPE=Release \
    -DAOSP_DIR="$S2_CACHE/aosp" \
    -DSYSROOT_DIR="$S2_CACHE/sysroot/$ABI" \
    -DGENERATED_DIR="$TARGET_BUILD/gen-interfaces" \
    -DWAYLAND_SCANNER="$HOST_BUILD/wayland-scanner" \
    -DCMAKE_INSTALL_PREFIX="$OUTPUT"
cmake --build "$TARGET_BUILD" -j "$JOBS"
cmake --install "$TARGET_BUILD" --strip

"$NDK_DIR/check-symbols.sh" "$OUTPUT/vendor/lib64/hw/hwcomposer.waydroid.so" "$S2_CACHE/sysroot/$ABI"
"$NDK_DIR/check-hidl-abi.sh" "$TARGET_BUILD" "$S2_CACHE/sysroot/$ABI"
if [[ $ABI == x86_64 ]]; then
    "$NDK_DIR/check-gralloc-handle.sh" "$TARGET_BUILD/hwcomposer.waydroid.so"
fi
echo "built $OUTPUT/vendor/lib64/hw/hwcomposer.waydroid.so"
