# HWC sources without Android dependencies. The NDK build links them into the
# HAL and ndk/host-tests builds them for the host to run their unit tests.
set(HWC_PORTABLE_SOURCES
    agent/link.cpp
    agent/wire.cpp
    dmabuf/format_set.cpp
    fence/point_watcher.cpp
    fence/syncobj.cpp
    fence/vgem.cpp
    input/keyboard_layout.cpp
    input/pinch.cpp
    input/scroll.cpp
    output/scale.cpp
    task/geometry.cpp
    task/layer_role.cpp
    task/registry.cpp
    task/sizing.cpp)
