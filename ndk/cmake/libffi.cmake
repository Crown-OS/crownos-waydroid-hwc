# libffi as AOSP builds it (external/libffi, static), needed by libwayland-client.

set(LIBFFI "${AOSP_DIR}/external_libffi")
set(LIBFFI_GENERATED_DIR "${CMAKE_BINARY_DIR}/gen/libffi")

file(READ "${LIBFFI}/include/ffi.h.in" ffi_header_template)
foreach(substitution
        "@VERSION@=3.2.1"
        "@TARGET@=FAKE_TARGET_"
        "@HAVE_LONG_DOUBLE@=CONF_HAVE_LONG_DOUBLE"
        "@HAVE_LONG_DOUBLE_VARIANT@=CONF_HAVE_LONG_DOUBLE_VARIANT"
        "@FFI_EXEC_TRAMPOLINE_TABLE@=CONF_FFI_EXEC_TRAMPOLINE_TABLE")
    string(REPLACE "=" ";" pair "${substitution}")
    list(GET pair 0 placeholder)
    list(GET pair 1 value)
    string(REPLACE "${placeholder}" "${value}" ffi_header_template "${ffi_header_template}")
endforeach()
file(CONFIGURE OUTPUT "${LIBFFI_GENERATED_DIR}/ffi_gen.h" CONTENT "${ffi_header_template}" @ONLY)

if(ANDROID_ABI STREQUAL "x86_64")
    set(LIBFFI_ARCH_SOURCES src/x86/ffi64.c src/x86/ffiw64.c src/x86/unix64.S src/x86/win64.S)
    set(LIBFFI_ARCH_INCLUDE linux-x86_64)
    set(LIBFFI_ASM_DEFINITIONS HAVE_AS_X86_PCREL HAVE_AS_ASCII_PSEUDO_OP)
elseif(ANDROID_ABI STREQUAL "arm64-v8a")
    set(LIBFFI_ARCH_SOURCES src/aarch64/sysv.S src/aarch64/ffi.c)
    set(LIBFFI_ARCH_INCLUDE linux-arm64)
    set(LIBFFI_ASM_DEFINITIONS "")
else()
    message(FATAL_ERROR "unsupported ANDROID_ABI ${ANDROID_ABI}")
endif()

list(TRANSFORM LIBFFI_ARCH_SOURCES PREPEND "${LIBFFI}/")
add_library(ffi STATIC
    ${LIBFFI}/src/closures.c
    ${LIBFFI}/src/debug.c
    ${LIBFFI}/src/java_raw_api.c
    ${LIBFFI}/src/prep_cif.c
    ${LIBFFI}/src/raw_api.c
    ${LIBFFI}/src/types.c
    ${LIBFFI_ARCH_SOURCES})
target_include_directories(ffi
    PUBLIC "${LIBFFI}/${LIBFFI_ARCH_INCLUDE}" "${LIBFFI_GENERATED_DIR}"
    PRIVATE "${LIBFFI}/include" "${LIBFFI}")
target_compile_definitions(ffi PRIVATE "$<$<COMPILE_LANGUAGE:ASM>:${LIBFFI_ASM_DEFINITIONS}>")
target_compile_options(ffi PRIVATE
    -Wno-incompatible-pointer-types
    -Wno-deprecated-declarations
    -Wno-missing-field-initializers
    -Wno-null-pointer-arithmetic
    -Wno-pointer-arith
    -Wno-sign-compare
    -Wno-unused-parameter
    -Wno-unused-result)
