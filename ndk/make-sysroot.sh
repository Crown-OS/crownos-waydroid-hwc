#!/usr/bin/env bash
# Builds the link sysroot from the pinned Waydroid images without mounting them.
# Starting from the NEEDED list of the stock hwcomposer.waydroid.so, it resolves
# every library and its transitive deps in the order the vendor linker namespace
# searches them (vendor, then the VNDK APEX, then LLNDK in /system, then bionic)
# and copies each file to $SYSROOT/<abi-libdir>/. MANIFEST records where each
# library came from and its sha256.
#
# usage: make-sysroot.sh [x86_64|arm64]
set -euo pipefail

ABI=${1:-x86_64}
HWC_CACHE=${HWC_CACHE:-$HOME/.cache/crownos-android/hwc}
IMAGES=${WAYDROID_IMAGES:-/var/lib/waydroid/images}
NDK=${ANDROID_NDK:-$HOME/Android/Sdk/ndk/27.2.12479018}
READELF=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf
SYSROOT=$HWC_CACHE/sysroot/$ABI
LIBDIR=lib64

case $ABI in
    x86_64 | arm64) ;;
    *) echo "unsupported ABI: $ABI" >&2; exit 1 ;;
esac

VENDOR_IMG=$IMAGES/vendor.img
SYSTEM_IMG=$IMAGES/system.img

# image:directory pairs in vendor-namespace search order.
SEARCH_PATH=(
    "$VENDOR_IMG:/$LIBDIR"
    "$SYSTEM_IMG:/system/apex/com.android.vndk.current/$LIBDIR"
    "$SYSTEM_IMG:/system/$LIBDIR"
    "$SYSTEM_IMG:/system/apex/com.android.runtime/$LIBDIR/bionic"
)

debugfs_run() { debugfs -R "$2" "$1" 2>/dev/null; }

declare -A LISTING
for entry in "${SEARCH_PATH[@]}"; do
    LISTING[$entry]=$(debugfs_run "${entry%%:*}" "ls -p ${entry#*:}" | awk -F/ '$6 != "" {print $3 "/" $6}')
done

# Prints "image:path" of the regular file a library name resolves to, following
# symlinks into /apex (flattened under /system/apex in the system image).
locate_library() {
    local name=$1 entry mode
    for entry in "${SEARCH_PATH[@]}"; do
        mode=$(awk -F/ -v lib="$name" '$2 == lib {print $1; exit}' <<< "${LISTING[$entry]}")
        [[ -z $mode ]] && continue
        local image=${entry%%:*} path=${entry#*:}/$name
        if [[ $mode == 120* ]]; then
            local target
            target=$(debugfs_run "$image" "stat $path" | sed -n 's/^Fast link dest: "\(.*\)"/\1/p')
            path=${target/#\/apex\//\/system\/apex\/}
        fi
        echo "$image:$path"
        return
    done
    return 1
}

rm -rf "$SYSROOT"
mkdir -p "$SYSROOT/$LIBDIR"
STOCK_HWC=$HWC_CACHE/stock/$ABI/hwcomposer.waydroid.so
mkdir -p "$(dirname "$STOCK_HWC")"
[[ -f $STOCK_HWC ]] || debugfs_run "$VENDOR_IMG" "dump /$LIBDIR/hw/hwcomposer.waydroid.so $STOCK_HWC" > /dev/null
declare -A ELF_MACHINE=([x86_64]=X86-64 [arm64]=AArch64)
if ! "$READELF" -h "$STOCK_HWC" | grep -q "Machine:.*${ELF_MACHINE[$ABI]}"; then
    echo "$VENDOR_IMG is not an $ABI image; set WAYDROID_IMAGES" >&2
    exit 1
fi

needed_of() { "$READELF" -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }

declare -A SEEN
QUEUE=($(needed_of "$STOCK_HWC"))
: > "$SYSROOT/MANIFEST"
while ((${#QUEUE[@]})); do
    name=${QUEUE[0]}
    QUEUE=("${QUEUE[@]:1}")
    [[ -n ${SEEN[$name]:-} ]] && continue
    SEEN[$name]=1
    if ! source=$(locate_library "$name"); then
        echo "unresolved: $name" >&2
        exit 1
    fi
    debugfs_run "${source%%:*}" "dump ${source#*:} $SYSROOT/$LIBDIR/$name" > /dev/null
    printf '%s\t%s\t%s\n' "$name" "$(basename "${source%%:*}"):${source#*:}" \
        "$(sha256sum "$SYSROOT/$LIBDIR/$name" | cut -d' ' -f1)" >> "$SYSROOT/MANIFEST"
    QUEUE+=($(needed_of "$SYSROOT/$LIBDIR/$name"))
done

sort -o "$SYSROOT/MANIFEST" "$SYSROOT/MANIFEST"
echo "$(wc -l < "$SYSROOT/MANIFEST") libraries, $(du -sh "$SYSROOT" | cut -f1) in $SYSROOT"
