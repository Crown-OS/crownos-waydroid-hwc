/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "output/scale.h"

#include <algorithm>
#include <cmath>

namespace output {

uint32_t preferred_scale_120(const output_info &output) {
    const bool rotated = output.transform & 1;
    const int32_t physical = rotated ? output.mode_height : output.mode_width;
    if (output.logical_width > 0 && physical > 0) {
        return static_cast<uint32_t>(std::lround(double(physical) * scale_denominator / output.logical_width));
    }
    return static_cast<uint32_t>(std::max(1, output.integer_scale)) * scale_denominator;
}

double highest_scale(const std::vector<output_info> &outputs) {
    uint32_t highest = scale_denominator;
    bool any = false;
    for (const output_info &output : outputs) {
        const uint32_t scale = preferred_scale_120(output);
        highest = any ? std::max(highest, scale) : scale;
        any = true;
    }
    return std::max<uint32_t>(highest, 1) / double(scale_denominator);
}

} // namespace output
