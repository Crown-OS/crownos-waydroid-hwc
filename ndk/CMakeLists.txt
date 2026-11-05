cmake_minimum_required(VERSION 3.27)

# Out-of-tree build of hwcomposer.waydroid.so with the NDK compiler against the
# platform ABI of the pinned Waydroid images. Configure through build.sh, which
# passes the NDK toolchain file, ANDROID_ABI and the directories below.
project(crownos-waydroid-hwc C CXX ASM)

set(AOSP_DIR "" CACHE PATH "fetch-sources.sh output")
set(SYSROOT_DIR "" CACHE PATH "make-sysroot.sh output for ANDROID_ABI")
set(GENERATED_DIR "" CACHE PATH "generate-interfaces.sh output")
set(WAYLAND_SCANNER "" CACHE FILEPATH "wayland-scanner built from AOSP external/wayland")
set(HWC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../hwcomposer" CACHE PATH "HWC sources to build")

foreach(required_dir AOSP_DIR SYSROOT_DIR GENERATED_DIR HWC_SOURCE_DIR)
    if(NOT IS_DIRECTORY "${${required_dir}}")
        message(FATAL_ERROR "${required_dir} is not a directory: '${${required_dir}}'")
    endif()
endforeach()
if(NOT EXISTS "${WAYLAND_SCANNER}")
    message(FATAL_ERROR "WAYLAND_SCANNER not found: '${WAYLAND_SCANNER}'")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(platform)
include(platform-libraries)
include(libffi)
include(wayland)
include(xkbcommon)
include(hwcomposer)
include(hidl-interfaces)
