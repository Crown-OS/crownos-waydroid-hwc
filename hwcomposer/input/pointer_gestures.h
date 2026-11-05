/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "input/pinch.h"

struct zwp_pointer_gesture_pinch_v1;

/// A touchpad pinch in progress, sent as two fingers on the touch stream.
struct pointer_gestures {
    struct zwp_pointer_gesture_pinch_v1 *pinch = nullptr;
    input::pinch_emulation emulation;
    bool active = false;
    double px_per_logical_x = 1;
    double px_per_logical_y = 1;
};
