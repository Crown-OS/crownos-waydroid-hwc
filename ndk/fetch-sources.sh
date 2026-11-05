#!/usr/bin/env bash
# Fetches the Android 13 (LineageOS 20, android-13.0.0_r75) sources whose headers
# and host tools the out-of-tree HWC build needs, then applies the Waydroid patches
# that change headers or generated code. Each source lands in its own directory
# under $HWC_CACHE/aosp, at the revision sources.lock pins. Safe to re-run.
set -euo pipefail

HWC_CACHE=${HWC_CACHE:-$HOME/.cache/crownos-android/hwc}
AOSP_DIR=$HWC_CACHE/aosp
AOSP_TAG=android-13.0.0_r75
LINEAGE_BRANCH=lineage-20.0
GOOGLESOURCE=https://android.googlesource.com
GITHUB=https://github.com
FREEDESKTOP=https://gitlab.freedesktop.org
LOCK_FILE=$(cd "$(dirname "$0")" && pwd)/sources.lock

mkdir -p "$AOSP_DIR"

# fetch <dir> <url> <ref> [sparse dir...]
fetch() {
    local dir=$AOSP_DIR/$1 url=$2 ref=$3
    shift 3
    if [[ ! -d $dir/.git ]]; then
        local sparse_args=()
        if (($#)); then
            sparse_args=(--filter=blob:none --sparse)
        fi
        git -c advice.detachedHead=false clone -q --depth 1 "${sparse_args[@]}" -b "$ref" "$url" "$dir"
    fi
    if (($#)); then
        git -C "$dir" sparse-checkout set "$@"
    fi
    verify_locked_revision "$(basename "$dir")"
}

# Branch heads move; a revision other than the one in sources.lock means the
# headers may no longer match the pinned images, so stop unless told otherwise.
verify_locked_revision() {
    local name=$1 locked actual
    locked=$(awk -v n="$name" '$1 == n {print $2}' "$LOCK_FILE")
    actual=$(git -C "$AOSP_DIR/$name" rev-parse HEAD)
    if [[ -n $locked && $locked != "$actual" && -z ${HWC_ALLOW_SOURCE_DRIFT:-} ]]; then
        echo "$name is at $actual, sources.lock pins $locked (set HWC_ALLOW_SOURCE_DRIFT=1 to accept)" >&2
        exit 1
    fi
}

# apply_patches <dir> <patch...>: applies the series once, recorded in .crownos-patched
apply_patches() {
    local dir=$AOSP_DIR/$1
    shift
    [[ -f $dir/.crownos-patched ]] && return
    local patch
    for patch in "$@"; do
        git -C "$dir" apply "$patch"
        basename "$patch" >> "$dir/.crownos-patched"
    done
}

fetch vendor_waydroid  $GITHUB/waydroid/android_vendor_waydroid.git lineage-20 waydroid-patches
fetch bionic           $GITHUB/LineageOS/android_bionic.git $LINEAGE_BRANCH libc/include libc/kernel
fetch system_libhidl   $GITHUB/LineageOS/android_system_libhidl.git $LINEAGE_BRANCH
fetch system_libhwbinder $GOOGLESOURCE/platform/system/libhwbinder $AOSP_TAG
fetch system_libfmq    $GITHUB/LineageOS/android_system_libfmq.git $LINEAGE_BRANCH base
fetch system_core      $GITHUB/LineageOS/android_system_core.git $LINEAGE_BRANCH \
    libcutils libutils libsystem libsync include libprocessgroup/include
fetch system_logging   $GITHUB/LineageOS/android_system_logging.git $LINEAGE_BRANCH liblog
fetch system_libbase   $GOOGLESOURCE/platform/system/libbase $AOSP_TAG
fetch hardware_libhardware $GITHUB/LineageOS/android_hardware_libhardware.git $LINEAGE_BRANCH include include_all
fetch hardware_interfaces $GITHUB/LineageOS/android_hardware_interfaces.git $LINEAGE_BRANCH \
    graphics/composer/2.1 graphics/common/1.0 graphics/common/1.1 graphics/common/1.2 \
    graphics/mapper/4.0 graphics/common/aidl/aidl_api common/aidl/aidl_api
fetch frameworks_native $GITHUB/LineageOS/android_frameworks_native.git $LINEAGE_BRANCH \
    include libs/ui libs/nativebase libs/nativewindow libs/arect libs/math opengl/include libs/gralloc/types \
    libs/binder/ndk
fetch system_tools_hidl $GOOGLESOURCE/platform/system/tools/hidl $AOSP_TAG
fetch external_libcxx  $GITHUB/LineageOS/android_external_libcxx.git $LINEAGE_BRANCH include
fetch external_libcxxabi $GOOGLESOURCE/platform/external/libcxxabi $AOSP_TAG include
fetch external_wayland $GOOGLESOURCE/platform/external/wayland $AOSP_TAG
fetch external_wayland_protocols $GOOGLESOURCE/platform/external/wayland-protocols $AOSP_TAG
fetch external_libffi  $GOOGLESOURCE/platform/external/libffi $AOSP_TAG
fetch external_libxkbcommon $GOOGLESOURCE/platform/external/libxkbcommon $AOSP_TAG
fetch external_libdrm  $GITHUB/waydroid/android_external_libdrm.git lineage-18.1 include android
fetch external_minigbm $GITHUB/waydroid/android_external_minigbm.git lineage-18.1 cros_gralloc
# Newer protocol XML than AOSP's: xdg-shell v6, wl_seat v8, wl_output v4,
# linux-dmabuf v4 feedback, linux-drm-syncobj and xdg-activation. The AOSP
# 1.19 scanner generates code that the static libwayland-client 1.19 runs.
fetch upstream_wayland $FREEDESKTOP/wayland/wayland.git 1.24.0 protocol
fetch upstream_wayland_protocols $FREEDESKTOP/wayland/wayland-protocols.git 1.45

WAYDROID_PATCHES=$AOSP_DIR/vendor_waydroid/waydroid-patches/base-patches-33
apply_patches system_libhidl "$WAYDROID_PATCHES"/system/libhidl/*.patch
apply_patches system_libhwbinder "$WAYDROID_PATCHES"/system/libhwbinder/*.patch
apply_patches system_tools_hidl "$WAYDROID_PATCHES"/system/tools/hidl/*.patch
apply_patches external_wayland_protocols "$WAYDROID_PATCHES"/external/wayland-protocols/*.patch

du -sh "$AOSP_DIR"
