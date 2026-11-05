# hwcomposer.waydroid.so, mirroring hwcomposer/Android.bp. Shared libraries are
# linked in the same order so DT_NEEDED matches the stock module.

set(HWC_GENERATED_DIR "${CMAKE_BINARY_DIR}/gen/hwcomposer")
file(MAKE_DIRECTORY "${HWC_GENERATED_DIR}")
wayland_scan("${HWC_SOURCE_DIR}/wayland-android.xml" code
    "${HWC_GENERATED_DIR}/wayland-android-client-protocol.c")
wayland_scan("${HWC_SOURCE_DIR}/wayland-android.xml" client-header
    "${HWC_GENERATED_DIR}/wayland-android-client-protocol.h")

set(HWC_SOURCES
    extension.cpp
    gralloc_handler.cpp
    hwcomposer.cpp
    wayland-hwc.cpp
    WaydroidClipboard.cpp
    WaydroidWindow.cpp
    egl-tools.cpp
    modes/waydroid_mode.cpp
    modes/closed.cpp
    modes/full-ui.cpp
    modes/multi-window.cpp
    modes/single-window.cpp)
list(TRANSFORM HWC_SOURCES PREPEND "${HWC_SOURCE_DIR}/")

add_library(hwcomposer.waydroid SHARED
    ${HWC_SOURCES}
    ${HWC_GENERATED_DIR}/wayland-android-client-protocol.c
    ${HWC_GENERATED_DIR}/wayland-android-client-protocol.h)
set_target_properties(hwcomposer.waydroid PROPERTIES PREFIX "" OUTPUT_NAME hwcomposer.waydroid)
target_include_directories(hwcomposer.waydroid PRIVATE
    "${HWC_SOURCE_DIR}"
    "${CORE}"
    "${CORE}/libsync"
    "${CORE}/libsync/include"
    "${HWC_GENERATED_DIR}")
target_compile_definitions(hwcomposer.waydroid PRIVATE "LOG_TAG=\"hwcomposer\"")
target_compile_options(hwcomposer.waydroid PRIVATE -Wall -Werror)

target_link_libraries(hwcomposer.waydroid PRIVATE
    wayland_client_static
    ffi
    wayland_extension_client_protocols
    xkbcommon
    platform::log
    platform::utils
    platform::cutils
    platform::hardware
    platform::hidlbase
    platform::sync
    platform::ui
    platform::drm
    platform::EGL
    platform::GLESv2
    ${WAYDROID_HIDL_TARGETS}
    platform::system_headers
    platform::minigbm_headers
    platform::cxx
    platform::c
    platform::m
    platform::dl)

install(TARGETS hwcomposer.waydroid LIBRARY DESTINATION vendor/lib64/hw)
