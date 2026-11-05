#!/usr/bin/env bash
# Generates the HIDL and AIDL C++ headers the HWC and libui need, using hidl-gen
# built from android-13.0.0_r75 (with the Waydroid host_hwbinder patch) and the
# SDK's prebuilt aidl compiler. The vendor.waydroid.* C++ sources are generated
# too, so the build can recompile those libraries and compare them with the image.
#
# usage: generate-interfaces.sh <hidl-gen> <aidl> <out-dir>
set -euo pipefail

HIDL_GEN=$1
AIDL=$2
OUT=$3
S2_CACHE=${S2_CACHE:-$HOME/.cache/crownos-android/s2}
AOSP_DIR=$S2_CACHE/aosp
FORK_DIR=$(cd "$(dirname "$0")/.." && pwd)
INTERFACES=$AOSP_DIR/hardware_interfaces

HIDL_ROOTS=(
    -r "vendor.waydroid:$FORK_DIR/interfaces"
    -r "android.hidl:$AOSP_DIR/system_libhidl/transport"
    -r "android.hardware:$INTERFACES"
)

HIDL_PACKAGES=(
    android.hidl.base@1.0
    android.hidl.manager@1.0
    android.hidl.manager@1.1
    android.hidl.manager@1.2
    android.hardware.graphics.common@1.0
    android.hardware.graphics.common@1.1
    android.hardware.graphics.common@1.2
    android.hardware.graphics.composer@2.1
    android.hardware.graphics.mapper@4.0
    vendor.waydroid.clipboard@1.0
    vendor.waydroid.display@1.0
    vendor.waydroid.display@1.1
    vendor.waydroid.display@1.2
    vendor.waydroid.task@1.0
    vendor.waydroid.window@1.0
    vendor.waydroid.window@1.1
    vendor.waydroid.window@1.2
)

rm -rf "$OUT"
mkdir -p "$OUT/include" "$OUT/sources" "$OUT/aidl-src"

for package in "${HIDL_PACKAGES[@]}"; do
    "$HIDL_GEN" -o "$OUT/include" -L c++-headers "${HIDL_ROOTS[@]}" "$package"
    if [[ $package == vendor.waydroid.* ]]; then
        "$HIDL_GEN" -o "$OUT/sources" -L c++-sources "${HIDL_ROOTS[@]}" "$package"
    fi
done

# generate_aidl_ndk <aidl_api dir> <version> [import dir...]
generate_aidl_ndk() {
    local api_dir=$1 version=$2
    shift 2
    local imports=(-I "$api_dir")
    local dir
    for dir in "$@"; do
        imports+=(-I "$dir")
    done
    mapfile -t sources < <(find "$api_dir" -name '*.aidl' | sort)
    "$AIDL" --lang=ndk --structured --stability=vintf --version="$version" \
        --hash="$(tail -n1 "$api_dir/.hash")" "${imports[@]}" \
        -h "$OUT/include" -o "$OUT/aidl-src" "${sources[@]}"
}

COMMON_V2=$INTERFACES/common/aidl/aidl_api/android.hardware.common/2
GRAPHICS_COMMON_V3=$INTERFACES/graphics/common/aidl/aidl_api/android.hardware.graphics.common/3
generate_aidl_ndk "$COMMON_V2" 2
generate_aidl_ndk "$GRAPHICS_COMMON_V3" 3 "$COMMON_V2"

rm -rf "$OUT/aidl-src"
echo "generated $(find "$OUT/include" -name '*.h' | wc -l) headers in $OUT/include"
