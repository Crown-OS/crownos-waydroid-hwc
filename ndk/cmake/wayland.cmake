# libwayland_client_static (AOSP external/wayland 1.19 with its Android patches)
# and the protocols the HWC speaks. The protocol XML is newer than AOSP's
# (wayland 1.24, wayland-protocols 1.45, pinned in sources.lock); the 1.19
# scanner turns it into code that the 1.19 library runs.

set(WAYLAND "${AOSP_DIR}/external_wayland")
set(WAYLAND_CORE_XML "${AOSP_DIR}/upstream_wayland/protocol/wayland.xml")
set(WAYLAND_PROTOCOLS "${AOSP_DIR}/upstream_wayland_protocols")
set(WAYLAND_GENERATED_DIR "${CMAKE_BINARY_DIR}/gen/wayland")

# wayland_scan(<xml> <scanner mode> <output file>)
function(wayland_scan xml mode output)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${WAYLAND_SCANNER}" ${mode} < "${xml}" > "${output}"
        DEPENDS "${xml}" "${WAYLAND_SCANNER}"
        VERBATIM)
endfunction()

set(WAYLAND_COMPILE_OPTIONS
    -fvisibility=hidden
    -Wno-cast-qual
    -Wno-pointer-arith
    -Wno-unused-parameter
    -Wno-unused-variable)

file(MAKE_DIRECTORY "${WAYLAND_GENERATED_DIR}")
wayland_scan("${WAYLAND_CORE_XML}" "-s;private-code" "${WAYLAND_GENERATED_DIR}/wayland-protocol.c")
wayland_scan("${WAYLAND_CORE_XML}" "-s;client-header" "${WAYLAND_GENERATED_DIR}/wayland-client-protocol.h")

add_library(wayland_client_static STATIC
    ${WAYLAND}/src/connection.c
    ${WAYLAND}/src/wayland-client.c
    ${WAYLAND}/src/wayland-os.c
    ${WAYLAND}/src/wayland-util.c
    ${WAYLAND_GENERATED_DIR}/wayland-protocol.c
    ${WAYLAND_GENERATED_DIR}/wayland-client-protocol.h)
target_include_directories(wayland_client_static
    PUBLIC "${WAYLAND}/src" "${WAYLAND}/egl" "${WAYLAND_GENERATED_DIR}"
    PRIVATE "${WAYLAND}")
target_compile_options(wayland_client_static PRIVATE ${WAYLAND_COMPILE_OPTIONS})
target_link_libraries(wayland_client_static PUBLIC ffi)

set(HWC_EXTENSION_PROTOCOLS
    "${WAYLAND_PROTOCOLS}/stable/presentation-time/presentation-time.xml"
    "${WAYLAND_PROTOCOLS}/stable/viewporter/viewporter.xml"
    "${WAYLAND_PROTOCOLS}/stable/xdg-shell/xdg-shell.xml"
    "${WAYLAND_PROTOCOLS}/staging/fractional-scale/fractional-scale-v1.xml"
    "${WAYLAND_PROTOCOLS}/staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml"
    "${WAYLAND_PROTOCOLS}/staging/xdg-activation/xdg-activation-v1.xml"
    "${WAYLAND_PROTOCOLS}/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml"
    "${WAYLAND_PROTOCOLS}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml"
    "${WAYLAND_PROTOCOLS}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml"
    "${WAYLAND_PROTOCOLS}/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml"
    "${WAYLAND_PROTOCOLS}/unstable/relative-pointer/relative-pointer-unstable-v1.xml"
    "${WAYLAND_PROTOCOLS}/unstable/tablet/tablet-unstable-v2.xml"
    "${WAYLAND_PROTOCOLS}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
    "${WAYLAND_PROTOCOLS}/unstable/xdg-output/xdg-output-unstable-v1.xml"
    "${CROWNOS_PROTOCOLS_DIR}/crownos-android-shell-v1.xml")

set(EXTENSION_PROTOCOL_FILES "")
foreach(xml IN LISTS HWC_EXTENSION_PROTOCOLS)
    if(NOT EXISTS "${xml}")
        message(FATAL_ERROR "protocol XML missing: ${xml}")
    endif()
    get_filename_component(protocol_name "${xml}" NAME_WE)
    set(source "${WAYLAND_GENERATED_DIR}/${protocol_name}.c")
    set(header "${WAYLAND_GENERATED_DIR}/${protocol_name}-client-protocol.h")
    wayland_scan("${xml}" private-code "${source}")
    wayland_scan("${xml}" client-header "${header}")
    list(APPEND EXTENSION_PROTOCOL_FILES "${source}" "${header}")
endforeach()

add_library(wayland_extension_client_protocols STATIC ${EXTENSION_PROTOCOL_FILES})
target_link_libraries(wayland_extension_client_protocols PUBLIC wayland_client_static)
