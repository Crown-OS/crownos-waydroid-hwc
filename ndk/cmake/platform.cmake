# Compiler and linker settings that reproduce what Soong uses for a vendor
# module on Android 13: the platform libc++ (std::__1) and the Android 13 bionic
# headers replace the NDK's; only the NDK compiler and crt objects are used.

if(NOT ANDROID OR NOT ANDROID_STL STREQUAL "none")
    message(FATAL_ERROR "configure with the NDK toolchain file and -DANDROID_STL=none")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

set(SYSROOT_LIB_DIR "${SYSROOT_DIR}/lib64")

if(ANDROID_ABI STREQUAL "x86_64")
    set(KERNEL_ARCH x86)
else()
    set(KERNEL_ARCH arm64)
endif()

add_compile_definitions(ANDROID NDEBUG __ANDROID_VNDK__ __ANDROID_VENDOR__)
add_compile_options(
    -fno-strict-aliasing
    -ffunction-sections
    -fdata-sections
    -fno-short-enums
    -funwind-tables
    -fstack-protector-strong
    -ftrivial-auto-var-init=zero
    -Wa,--noexecstack
    -nostdlibinc
    -ffile-prefix-map=${AOSP_DIR}=aosp
    -ffile-prefix-map=${CMAKE_BINARY_DIR}=out
    -ffile-prefix-map=${HWC_SOURCE_DIR}=hardware/waydroid/hwcomposer
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-fvisibility-inlines-hidden;-nostdinc++>")

set(BIONIC "${AOSP_DIR}/bionic/libc")
include_directories(SYSTEM
    "$<$<COMPILE_LANGUAGE:CXX>:${AOSP_DIR}/external_libcxx/include>"
    "$<$<COMPILE_LANGUAGE:CXX>:${AOSP_DIR}/external_libcxxabi/include>"
    "${BIONIC}/include"
    "${BIONIC}/kernel/uapi"
    "${BIONIC}/kernel/uapi/asm-${KERNEL_ARCH}"
    "${BIONIC}/kernel/android/scsi"
    "${BIONIC}/kernel/android/uapi")

add_link_options(
    -nostdlib++
    -L${SYSROOT_LIB_DIR}
    -Wl,-rpath-link,${SYSROOT_LIB_DIR}
    -Wl,--no-undefined
    -Wl,-z,now
    -Wl,-z,relro
    -Wl,-z,noexecstack
    -Wl,--build-id=md5
    -Wl,--hash-style=gnu
    -Wl,--gc-sections
    -Wl,--icf=safe
    -Wl,--pack-dyn-relocs=android+relr)
