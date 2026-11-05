/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "input/pinch.h"

#include <algorithm>
#include <cmath>

namespace input {
namespace {

constexpr double pi = 3.14159265358979323846;

int32_t clamp_to(double value, int32_t low, int32_t high) {
    return static_cast<int32_t>(std::lround(std::clamp(value, double(low), double(std::max(low, high - 1)))));
}

} // namespace

std::array<finger, 2> pinch_emulation::begin(double x, double y, const task::px_rect &area) {
    bounds = area;
    center_x = x;
    center_y = y;
    scale = 1;
    angle_degrees = 0;
    return fingers();
}

std::array<finger, 2> pinch_emulation::update(double dx, double dy, double new_scale, double rotation) {
    center_x += dx;
    center_y += dy;
    if (new_scale > 0) {
        scale = new_scale;
    }
    angle_degrees += rotation;
    return fingers();
}

std::array<finger, 2> pinch_emulation::fingers() const {
    const double half = pinch_finger_distance_px * scale / 2;
    const double radians = angle_degrees * pi / 180;
    const double offset_x = half * std::cos(radians);
    const double offset_y = half * std::sin(radians);
    return {finger{clamp_to(center_x - offset_x, bounds.left, bounds.right),
                   clamp_to(center_y - offset_y, bounds.top, bounds.bottom)},
            finger{clamp_to(center_x + offset_x, bounds.left, bounds.right),
                   clamp_to(center_y + offset_y, bounds.top, bounds.bottom)}};
}

} // namespace input
