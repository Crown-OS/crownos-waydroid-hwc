/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <cstdint>

namespace input {

/// zwp_tablet_tool_v2.pressure range.
constexpr uint32_t wayland_pressure_max = 65535;
/// ABS_PRESSURE range Waydroid's EventHub reports for the wayland_tablet device.
constexpr int32_t android_pressure_max = 255;

/// Maps the whole Wayland pressure range onto Android's, rounding to nearest
/// so full pressure reaches the maximum and the lightest contact never reads
/// as zero.
constexpr int32_t android_pressure(uint32_t wayland_pressure) {
    if (wayland_pressure == 0) {
        return 0;
    }
    const uint64_t clamped = std::min(wayland_pressure, wayland_pressure_max);
    const uint64_t scaled = (clamped * android_pressure_max + wayland_pressure_max / 2) / wayland_pressure_max;
    return std::max<int32_t>(1, static_cast<int32_t>(scaled));
}

} // namespace input
