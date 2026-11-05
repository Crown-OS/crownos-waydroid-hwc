/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "input/scroll.h"

#include <cmath>

namespace input {

int32_t scroll_axis::add(double value120) {
    if (value120 * pending120 < 0) {
        pending120 = 0;
    }
    pending120 += value120;
    const double detents = std::trunc(pending120 / value120_per_detent);
    pending120 -= detents * value120_per_detent;
    return static_cast<int32_t>(detents);
}

int32_t scroll_axis::wheel(int32_t value120) {
    return add(value120);
}

int32_t scroll_axis::continuous(double distance) {
    return add(distance * value120_per_detent / continuous_units_per_detent);
}

void scroll_frame::source_is_wheel(bool is_wheel) {
    if (is_wheel != wheel) {
        axes[0].stop();
        axes[1].stop();
    }
    wheel = is_wheel;
}

void scroll_frame::value120(scroll_axis_id axis, int32_t value) {
    auto &entry = frame[static_cast<size_t>(axis)];
    entry.value120 += value;
    entry.has_value120 = true;
}

void scroll_frame::discrete(scroll_axis_id axis, int32_t steps) {
    value120(axis, steps * value120_per_detent);
}

void scroll_frame::value(scroll_axis_id axis, double distance) {
    frame[static_cast<size_t>(axis)].distance += distance;
}

void scroll_frame::stop(scroll_axis_id axis) {
    frame[static_cast<size_t>(axis)].stopped = true;
}

std::array<int32_t, 2> scroll_frame::finish() {
    std::array<int32_t, 2> detents{};
    for (size_t i = 0; i < axes.size(); ++i) {
        const axis_frame &entry = frame[i];
        if (entry.has_value120) {
            detents[i] = axes[i].wheel(entry.value120);
        } else if (entry.distance != 0 && !wheel) {
            detents[i] = axes[i].continuous(entry.distance);
        }
        if (entry.stopped) {
            axes[i].stop();
        }
    }
    frame = {};
    return detents;
}

} // namespace input
