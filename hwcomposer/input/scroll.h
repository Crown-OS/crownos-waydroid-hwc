/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>

namespace input {

/// High-resolution wheel units per detent (wl_pointer.axis_value120).
constexpr int32_t value120_per_detent = 120;
/// Logical pixels of finger or continuous scrolling per detent.
constexpr double continuous_units_per_detent = 10.0;

/// Turns one scroll axis into the whole REL_WHEEL detents Android 13 reads,
/// carrying the remainder so fractions of high-resolution wheels and
/// touchpads are never lost or rounded per event.
class scroll_axis {
  public:
    /// A wheel step in 1/120 detents.
    int32_t wheel(int32_t value120);
    /// Finger or continuous scrolling in logical pixels.
    int32_t continuous(double distance);
    /// The scroll sequence ended (axis_stop or a source change).
    void stop() { pending120 = 0; }

  private:
    int32_t add(double value120);
    double pending120 = 0;
};

enum class scroll_axis_id { vertical = 0, horizontal = 1 };

/// Collects one wl_pointer frame of axis events and turns it into detents
/// per axis: axis_value120 (or axis_discrete) when the frame has it, the
/// continuous axis value otherwise.
class scroll_frame {
  public:
    void source_is_wheel(bool wheel);
    void value120(scroll_axis_id axis, int32_t value);
    void discrete(scroll_axis_id axis, int32_t steps);
    void value(scroll_axis_id axis, double distance);
    void stop(scroll_axis_id axis);
    /// Ends the frame: detents to send for {vertical, horizontal}, in the
    /// Wayland sense (positive scrolls down or right).
    std::array<int32_t, 2> finish();

  private:
    struct axis_frame {
        int32_t value120 = 0;
        bool has_value120 = false;
        double distance = 0;
        bool stopped = false;
    };

    std::array<scroll_axis, 2> axes;
    std::array<axis_frame, 2> frame;
    bool wheel = false;
};

} // namespace input
