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

} // namespace input
