/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

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

} // namespace input
