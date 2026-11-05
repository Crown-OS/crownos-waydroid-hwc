/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 *
 * Logging for the modules that also build on the host for tests.
 */

#pragma once

#ifdef __ANDROID__
#include <log/log.h>
#define CROWN_LOGI(...) ALOGI(__VA_ARGS__)
#define CROWN_LOGW(...) ALOGW(__VA_ARGS__)
#define CROWN_LOGE(...) ALOGE(__VA_ARGS__)
#else
#include <cstdio>
#define CROWN_LOG_HOST(level, ...)            \
    do {                                      \
        std::fprintf(stderr, level " " __VA_ARGS__); \
        std::fputc('\n', stderr);             \
    } while (0)
#define CROWN_LOGI(...) CROWN_LOG_HOST("I", __VA_ARGS__)
#define CROWN_LOGW(...) CROWN_LOG_HOST("W", __VA_ARGS__)
#define CROWN_LOGE(...) CROWN_LOG_HOST("E", __VA_ARGS__)
#endif
