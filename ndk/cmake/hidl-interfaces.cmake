# Rebuilds the vendor.waydroid.* HIDL libraries from the hidl-gen sources. They
# are never installed: check-hidl-abi.sh compares their exports and vtable
# layouts with the libraries in vendor.img to prove the generated headers the
# HWC compiles against describe the same ABI.

set(HIDL_SOURCES_DIR "${GENERATED_DIR}/sources/vendor/waydroid")
platform_library(composer_2_1 SO android.hardware.graphics.composer@2.1.so)

# hidl_interface_library(<interface@version> <dependency target>...)
function(hidl_interface_library interface)
    string(REPLACE "@" "/" source_subdir "${interface}")
    file(GLOB sources "${HIDL_SOURCES_DIR}/${source_subdir}/*.cpp")
    set(target "hidl_${interface}")
    string(MAKE_C_IDENTIFIER "${target}" target)
    add_library(${target} SHARED ${sources})
    set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "vendor.waydroid.${interface}")
    target_compile_definitions(${target} PRIVATE __ANDROID_DEBUGGABLE__)
    target_compile_options(${target} PRIVATE -Wall -Werror -Wextra-semi)
    target_link_libraries(${target} PRIVATE
        ${ARGN}
        platform::hidlbase
        platform::log
        platform::utils
        platform::cutils
        platform::cxx
        platform::c
        platform::m
        platform::dl)
endfunction()

hidl_interface_library(clipboard@1.0)
hidl_interface_library(display@1.0 platform::composer_2_1)
hidl_interface_library(display@1.1 platform::composer_2_1 platform::waydroid_display_1_0)
hidl_interface_library(display@1.2 platform::composer_2_1 platform::waydroid_display_1_0 platform::waydroid_display_1_1)
hidl_interface_library(task@1.0)
hidl_interface_library(window@1.0)
hidl_interface_library(window@1.1 platform::waydroid_window_1_0)
hidl_interface_library(window@1.2 platform::waydroid_window_1_0 platform::waydroid_window_1_1)
