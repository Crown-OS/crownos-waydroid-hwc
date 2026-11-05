/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <vector>

namespace output {

/// What wl_output and xdg_output say about one output.
struct output_info {
    int32_t integer_scale = 1;
    int32_t mode_width = 0;
    int32_t mode_height = 0;
    int32_t logical_width = 0;
    int32_t logical_height = 0;
    /// wl_output_transform; odd values swap width and height.
    int32_t transform = 0;
    int32_t refresh_mhz = 0;
};

/// wp_fractional_scale_v1 units: the scale times 120.
constexpr uint32_t scale_denominator = 120;

/// The preferred scale of one output in 1/120 units: the ratio of its
/// physical to logical size when xdg_output tells it, else its integer scale.
uint32_t preferred_scale_120(const output_info &output);

/// The scale Android renders at: the highest preferred scale of all outputs,
/// so no output ever upscales an Android window. 1 with no outputs.
double highest_scale(const std::vector<output_info> &outputs);

} // namespace output
