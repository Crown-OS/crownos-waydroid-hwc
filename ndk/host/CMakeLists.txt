cmake_minimum_required(VERSION 3.22)
project(crownos-hwc-host-tools C CXX)

# Host tools built from the same android-13.0.0_r75 sources the image was built
# from: hidl-gen (with the Waydroid host_hwbinder patch) and wayland-scanner.

set(AOSP_DIR "" CACHE PATH "Directory filled by fetch-sources.sh")
if(NOT IS_DIRECTORY "${AOSP_DIR}/system_tools_hidl")
    message(FATAL_ERROR "AOSP_DIR must point at the fetch-sources.sh output")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# AOSP 13 relied on transitive includes that current libstdc++ no longer provides.
set(LEGACY_INCLUDES "SHELL:-include cstdint" "SHELL:-include cstring" "SHELL:-include algorithm")

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
pkg_check_modules(EXPAT REQUIRED IMPORTED_TARGET expat)

set(HIDL ${AOSP_DIR}/system_tools_hidl)
set(LIBBASE ${AOSP_DIR}/system_libbase)
set(LIBLOG ${AOSP_DIR}/system_logging/liblog)
set(CORE ${AOSP_DIR}/system_core)

add_library(host_liblog STATIC
    ${LIBLOG}/log_event_list.cpp
    ${LIBLOG}/log_event_write.cpp
    ${LIBLOG}/logger_name.cpp
    ${LIBLOG}/logger_read.cpp
    ${LIBLOG}/logger_write.cpp
    ${LIBLOG}/logprint.cpp
    ${LIBLOG}/properties.cpp)
target_include_directories(host_liblog
    PUBLIC ${LIBLOG}/include
    PRIVATE ${CORE}/libcutils/include ${LIBBASE}/include)
target_compile_definitions(host_liblog PRIVATE LIBLOG_LOG_TAG=1006 SNET_EVENT_LOG_TAG=1397638484)

add_library(host_libbase STATIC
    ${LIBBASE}/file.cpp
    ${LIBBASE}/hex.cpp
    ${LIBBASE}/logging.cpp
    ${LIBBASE}/mapped_file.cpp
    ${LIBBASE}/parsebool.cpp
    ${LIBBASE}/stringprintf.cpp
    ${LIBBASE}/strings.cpp
    ${LIBBASE}/threads.cpp
    ${LIBBASE}/errors_unix.cpp)
target_include_directories(host_libbase PUBLIC ${LIBBASE}/include)
target_compile_options(host_libbase PRIVATE ${LEGACY_INCLUDES})
add_library(host_libbase_strerror OBJECT ${LIBBASE}/posix_strerror_r.cpp)
target_sources(host_libbase PRIVATE $<TARGET_OBJECTS:host_libbase_strerror>)
target_link_libraries(host_libbase PUBLIC host_liblog)

BISON_TARGET(hidl_parser ${HIDL}/hidl-gen_y.yy ${CMAKE_CURRENT_BINARY_DIR}/hidl-gen_y.cpp
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/hidl-gen_y.h)
FLEX_TARGET(hidl_lexer ${HIDL}/hidl-gen_l.ll ${CMAKE_CURRENT_BINARY_DIR}/hidl-gen_l.cpp)
ADD_FLEX_BISON_DEPENDENCY(hidl_lexer hidl_parser)

file(GLOB HIDL_AST_SOURCES ${HIDL}/*.cpp)
list(REMOVE_ITEM HIDL_AST_SOURCES ${HIDL}/main.cpp)

add_executable(hidl-gen
    ${HIDL}/main.cpp
    ${HIDL_AST_SOURCES}
    ${HIDL}/hashing/Hash.cpp
    ${HIDL}/host_utils/Formatter.cpp
    ${HIDL}/host_utils/StringHelper.cpp
    ${HIDL}/utils/FQName.cpp
    ${HIDL}/utils/FqInstance.cpp
    ${BISON_hidl_parser_OUTPUTS}
    ${FLEX_hidl_lexer_OUTPUTS})
target_include_directories(hidl-gen PRIVATE
    ${HIDL}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${HIDL}/hashing/include
    ${HIDL}/host_utils/include
    ${HIDL}/host_utils/include/hidl-util
    ${HIDL}/utils/include
    ${HIDL}/utils/include/hidl-util
    ${AOSP_DIR}/system_libhwbinder/include
    ${CORE}/libutils/include
    ${CORE}/libcutils/include
    ${CORE}/libsystem/include)
target_compile_options(hidl-gen PRIVATE -Wno-deprecated-declarations ${LEGACY_INCLUDES})
target_link_libraries(hidl-gen PRIVATE host_libbase OpenSSL::Crypto PkgConfig::JSONCPP)

set(WAYLAND ${AOSP_DIR}/external_wayland)
add_executable(wayland-scanner ${WAYLAND}/src/scanner.c ${WAYLAND}/src/wayland-util.c)
target_include_directories(wayland-scanner PRIVATE ${WAYLAND}/src ${WAYLAND}/private ${WAYLAND})
target_link_libraries(wayland-scanner PRIVATE PkgConfig::EXPAT)
