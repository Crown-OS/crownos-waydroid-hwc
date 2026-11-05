# One target per library the HWC links: the .so comes from the image sysroot,
# the headers from the include directories its vendor variant exports in AOSP.
# DEPS only forward headers ($<COMPILE_ONLY>), so the link line, and with it
# DT_NEEDED, keeps the order the HWC lists its libraries in.

set(GENERATED_INCLUDE_DIR "${GENERATED_DIR}/include")
set(CORE "${AOSP_DIR}/system_core")
set(NATIVE "${AOSP_DIR}/frameworks_native")

# platform_library(<name> [SO <file>] [INCLUDES <dir>...] [DEPS <target>...])
function(platform_library name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "SO" "INCLUDES;DEPS")
    add_library(platform::${name} INTERFACE IMPORTED)
    if(ARG_SO)
        set(library_file "${SYSROOT_LIB_DIR}/${ARG_SO}")
        if(NOT EXISTS "${library_file}")
            message(FATAL_ERROR "${library_file} missing: run make-sysroot.sh")
        endif()
        target_link_libraries(platform::${name} INTERFACE "${library_file}")
    endif()
    target_include_directories(platform::${name} SYSTEM INTERFACE ${ARG_INCLUDES})
    list(TRANSFORM ARG_DEPS REPLACE "^(.+)$" "$<COMPILE_ONLY:\\1>")
    target_link_libraries(platform::${name} INTERFACE ${ARG_DEPS})
endfunction()

platform_library(base_headers INCLUDES "${AOSP_DIR}/system_libbase/include")
platform_library(system_headers INCLUDES "${CORE}/libsystem/include")
platform_library(log SO liblog.so INCLUDES "${AOSP_DIR}/system_logging/liblog/include_vndk")
platform_library(cutils SO libcutils.so
    INCLUDES "${CORE}/libcutils/include_outside_system"
    DEPS platform::log)
platform_library(utils SO libutils.so
    INCLUDES "${CORE}/libutils/include"
    DEPS platform::cutils platform::system_headers platform::base_headers)
platform_library(hardware SO libhardware.so
    INCLUDES "${AOSP_DIR}/hardware_libhardware/include"
    DEPS platform::cutils platform::system_headers)
platform_library(hidlbase SO libhidlbase.so
    INCLUDES
        "${AOSP_DIR}/system_libhidl/base/include"
        "${AOSP_DIR}/system_libhidl/transport/include"
        "${AOSP_DIR}/system_libfmq/base"
        "${AOSP_DIR}/system_libhwbinder/include"
        "${GENERATED_INCLUDE_DIR}"
    DEPS platform::utils platform::cutils)
platform_library(sync SO libsync.so INCLUDES "${CORE}/libsync/include")
platform_library(binder_ndk_headers
    INCLUDES
        "${NATIVE}/libs/binder/ndk/include_cpp"
        "${NATIVE}/libs/binder/ndk/include_ndk"
        "${NATIVE}/libs/binder/ndk/include_platform")
platform_library(ui SO libui.so
    INCLUDES
        "${NATIVE}/libs/ui/include"
        "${NATIVE}/libs/ui/include_private"
        "${NATIVE}/libs/nativebase/include"
        "${NATIVE}/libs/nativewindow/include"
        "${NATIVE}/libs/ui/include_vndk"
        "${NATIVE}/libs/arect/include"
        "${NATIVE}/libs/math/include"
        "${NATIVE}/libs/gralloc/types/include"
        "${GENERATED_INCLUDE_DIR}"
    DEPS platform::hardware platform::hidlbase platform::utils platform::base_headers
        platform::binder_ndk_headers)
platform_library(drm SO libdrm.so
    INCLUDES "${AOSP_DIR}/external_libdrm/include/drm" "${AOSP_DIR}/external_libdrm/android")
platform_library(opengl_headers INCLUDES "${NATIVE}/opengl/include")
platform_library(EGL SO libEGL.so DEPS platform::opengl_headers)
platform_library(GLESv2 SO libGLESv2.so DEPS platform::opengl_headers)
platform_library(minigbm_headers INCLUDES "${AOSP_DIR}/external_minigbm")

set(WAYDROID_HIDL_LIBRARIES
    clipboard@1.0 display@1.0 display@1.1 display@1.2 task@1.0 window@1.0 window@1.1 window@1.2)
set(WAYDROID_HIDL_TARGETS "")
foreach(interface IN LISTS WAYDROID_HIDL_LIBRARIES)
    string(MAKE_C_IDENTIFIER "waydroid_${interface}" target_name)
    platform_library(${target_name} SO vendor.waydroid.${interface}.so DEPS platform::hidlbase)
    list(APPEND WAYDROID_HIDL_TARGETS platform::${target_name})
endforeach()

platform_library(cxx SO libc++.so)
platform_library(c SO libc.so)
platform_library(m SO libm.so)
platform_library(dl SO libdl.so)
