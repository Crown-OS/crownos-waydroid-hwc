# HWC sources without Android dependencies. The NDK build links them into the
# HAL and ndk/host-tests builds them for the host to run their unit tests.
set(HWC_PORTABLE_SOURCES
    agent/wire.cpp
    agent/link.cpp)
