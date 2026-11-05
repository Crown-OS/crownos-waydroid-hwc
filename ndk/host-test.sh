#!/usr/bin/env bash
# Builds and runs the HWC host unit tests (ndk/host-tests).
#
# usage: host-test.sh
# env:   HWC_CACHE, CROWNOS_CONTRACTS (directory with agent-wire-vectors.txt),
#        JOBS (default 4), HOST_CXX
set -euo pipefail

NDK_DIR=$(cd "$(dirname "$0")" && pwd)
HWC_CACHE=${HWC_CACHE:-$HOME/.cache/crownos-android/hwc}
CROWNOS_CONTRACTS=${CROWNOS_CONTRACTS:-$NDK_DIR/../../crownos-android/contracts}
JOBS=${JOBS:-4}
BUILD=$HWC_CACHE/build/host-tests

cmake -G Ninja -S "$NDK_DIR/host-tests" -B "$BUILD" \
    -DCMAKE_CXX_COMPILER="${HOST_CXX:-clang++}" -DCMAKE_BUILD_TYPE=Debug \
    -DAGENT_WIRE_VECTORS="$CROWNOS_CONTRACTS/agent-wire-vectors.txt" > /dev/null
cmake --build "$BUILD" -j "$JOBS"
ctest --test-dir "$BUILD" --output-on-failure -j "$JOBS"
