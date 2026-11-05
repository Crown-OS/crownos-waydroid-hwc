/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "task/geometry.h"

#include <array>

namespace input {

/// Distance between the two synthetic fingers when a pinch starts, in
/// Android pixels.
constexpr double pinch_finger_distance_px = 400.0;

struct finger {
    int32_t x = 0;
    int32_t y = 0;
};

/// Turns a touchpad pinch (zwp_pointer_gesture_pinch_v1) into two fingers
/// that Android sees as a pinch on its touchscreen. Fingers stay inside
/// `bounds`, the window the pinch started in, so both touches land on it.
class pinch_emulation {
  public:
    std::array<finger, 2> begin(double center_x, double center_y, const task::px_rect &bounds);
    /// `dx`/`dy` move the centre in Android pixels; `scale` is relative to
    /// the start; `rotation` is the change in degrees, clockwise.
    std::array<finger, 2> update(double dx, double dy, double scale, double rotation);

  private:
    std::array<finger, 2> fingers() const;

    task::px_rect bounds;
    double center_x = 0;
    double center_y = 0;
    double scale = 1;
    double angle_degrees = 0;
};

} // namespace input
