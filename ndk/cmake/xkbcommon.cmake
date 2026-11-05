# libxkbcommon as AOSP builds it (external/libxkbcommon, static). registry.c is
# left out: it needs libxml2 and the HWC never references the rxkb_* API, so the
# archive member would not be linked anyway.

set(XKBCOMMON "${AOSP_DIR}/external_libxkbcommon")

set(XKBCOMMON_SOURCES
    config/libxkbcommon.so.0.0.0.p/parser.c
    src/compose/parser.c
    src/compose/paths.c
    src/compose/state.c
    src/compose/table.c
    src/xkbcomp/action.c
    src/xkbcomp/ast-build.c
    src/xkbcomp/compat.c
    src/xkbcomp/expr.c
    src/xkbcomp/include.c
    src/xkbcomp/keycodes.c
    src/xkbcomp/keymap.c
    src/xkbcomp/keymap-dump.c
    src/xkbcomp/keywords.c
    src/xkbcomp/rules.c
    src/xkbcomp/scanner.c
    src/xkbcomp/symbols.c
    src/xkbcomp/types.c
    src/xkbcomp/vmod.c
    src/xkbcomp/xkbcomp.c
    src/atom.c
    src/context.c
    src/context-priv.c
    src/keymap.c
    src/keymap-priv.c
    src/keysym.c
    src/keysym-utf.c
    src/state.c
    src/text.c
    src/utf8.c
    src/util-list.c
    src/utils.c)
list(TRANSFORM XKBCOMMON_SOURCES PREPEND "${XKBCOMMON}/")

add_library(xkbcommon STATIC ${XKBCOMMON_SOURCES})
target_include_directories(xkbcommon
    PUBLIC "${XKBCOMMON}/include"
    PRIVATE
        "${XKBCOMMON}"
        "${XKBCOMMON}/src"
        "${XKBCOMMON}/config"
        "${XKBCOMMON}/config/libxkbcommon.so.0.0.0.p")
target_compile_definitions(xkbcommon PRIVATE _GNU_SOURCE)
target_compile_options(xkbcommon PRIVATE
    -fno-strict-aliasing
    -Wno-format
    -Wno-unused-parameter
    -Wno-missing-field-initializers)
