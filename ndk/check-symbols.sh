#!/usr/bin/env bash
# Checks that every dynamic import of the built HWC resolves against the image
# sysroot (bionic lookup order, GNU versions, std::__1 only) and, given the
# stock module, prints how the import lists differ.
#
# usage: check-symbols.sh <hwcomposer.waydroid.so> <sysroot-dir> [stock.so]
set -euo pipefail

BUILT=$1
SYSROOT=$2
STOCK=${3:-${HWC_CACHE:-$HOME/.cache/crownos-android/hwc}/stock/$(basename "$SYSROOT")/hwcomposer.waydroid.so}
TOOLS=$(cd "$(dirname "$0")/tools" && pwd)

stock_args=()
[[ -f $STOCK ]] && stock_args=("$STOCK")
python3 -I "$TOOLS/elf_symbols.py" "$BUILT" "$SYSROOT/lib64" "${stock_args[@]}"
