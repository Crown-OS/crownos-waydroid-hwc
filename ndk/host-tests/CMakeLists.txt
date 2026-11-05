cmake_minimum_required(VERSION 3.27)

# Host unit tests for the HWC modules that have no Android dependencies:
# the agent-wire codec against the contract's golden vectors, the agent link
# against a fake agent, and the pure geometry, input and buffer helpers.
project(crownos-hwc-host-tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HWC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../hwcomposer" CACHE PATH "HWC sources to test")
set(AGENT_WIRE_VECTORS "" CACHE FILEPATH "contracts/agent-wire-vectors.txt")
if(NOT EXISTS "${AGENT_WIRE_VECTORS}")
    message(FATAL_ERROR "AGENT_WIRE_VECTORS not found: '${AGENT_WIRE_VECTORS}'")
endif()

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/portable-sources.cmake")
list(TRANSFORM HWC_PORTABLE_SOURCES PREPEND "${HWC_SOURCE_DIR}/")
file(GLOB HWC_TEST_SOURCES CONFIGURE_DEPENDS "${HWC_SOURCE_DIR}/tests/*_test.cpp")

add_library(hwc_portable STATIC ${HWC_PORTABLE_SOURCES})
target_include_directories(hwc_portable PUBLIC "${HWC_SOURCE_DIR}")
target_compile_options(hwc_portable PUBLIC -Wall -Wextra -Werror)
target_link_libraries(hwc_portable PUBLIC Threads::Threads)

add_executable(hwc_host_tests ${HWC_TEST_SOURCES})
target_compile_definitions(hwc_host_tests PRIVATE "AGENT_WIRE_VECTORS=\"${AGENT_WIRE_VECTORS}\"")
target_link_libraries(hwc_host_tests PRIVATE hwc_portable GTest::gtest_main)

enable_testing()
include(GoogleTest)
gtest_discover_tests(hwc_host_tests)
