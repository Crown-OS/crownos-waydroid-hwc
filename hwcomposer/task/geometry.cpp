/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "task/geometry.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace task {
namespace {

constexpr double fixed_units = 256.0;

enum wl_transform : int32_t {
    normal = 0,
    rotate_90 = 1,
    rotate_180 = 2,
    rotate_270 = 3,
    flipped = 4,
    flipped_90 = 5,
    flipped_180 = 6,
    flipped_270 = 7,
};

bool swaps_axes(int32_t transform) {
    return transform & 1;
}

void buffer_to_surface(int32_t transform, float width, float height, float bx, float by, float &sx,
                       float &sy) {
    switch (transform) {
        case rotate_90:
            sx = height - by;
            sy = bx;
            break;
        case rotate_180:
            sx = width - bx;
            sy = height - by;
            break;
        case rotate_270:
            sx = by;
            sy = width - bx;
            break;
        case flipped:
            sx = width - bx;
            sy = by;
            break;
        case flipped_90:
            sx = by;
            sy = bx;
            break;
        case flipped_180:
            sx = bx;
            sy = height - by;
            break;
        case flipped_270:
            sx = height - by;
            sy = width - bx;
            break;
        default:
            sx = bx;
            sy = by;
            break;
    }
}

/// [start, end) of a source span, in fixed units, kept inside [0, limit).
std::pair<int32_t, int32_t> fixed_span(double start, double end, double limit) {
    const int32_t bound = static_cast<int32_t>(std::floor(limit * fixed_units));
    int32_t first = static_cast<int32_t>(std::lround(start * fixed_units));
    int32_t last = static_cast<int32_t>(std::lround(end * fixed_units));
    first = std::clamp(first, 0, std::max(0, bound - 1));
    last = std::clamp(last, first + 1, std::max(first + 1, bound));
    return {first, last - first};
}

/// Places the part `visible` of a layer whose display frame is `frame`.
/// `origin_x/y` is the Android pixel that maps to the window's logical (0,0).
placement place(const px_rect &frame, const px_rect &visible, const buffer_view &buffer,
                double origin_x, double origin_y, const window_scale &scale) {
    placement result;
    if (visible.empty() || frame.empty() || buffer.width == 0 || buffer.height == 0) {
        return result;
    }
    const int32_t x0 = static_cast<int32_t>(std::lround((visible.left - origin_x) * scale.x));
    const int32_t y0 = static_cast<int32_t>(std::lround((visible.top - origin_y) * scale.y));
    const int32_t x1 = static_cast<int32_t>(std::lround((visible.right - origin_x) * scale.x));
    const int32_t y1 = static_cast<int32_t>(std::lround((visible.bottom - origin_y) * scale.y));
    result.visible = true;
    result.x = x0;
    result.y = y0;
    result.width = std::max(1, x1 - x0);
    result.height = std::max(1, y1 - y0);

    const float_rect crop = crop_in_surface_space(buffer);
    const double crop_width = crop.right - crop.left;
    const double crop_height = crop.bottom - crop.top;
    const double first_x = double(visible.left - frame.left) / frame.width();
    const double last_x = double(visible.right - frame.left) / frame.width();
    const double first_y = double(visible.top - frame.top) / frame.height();
    const double last_y = double(visible.bottom - frame.top) / frame.height();
    const bool swapped = swaps_axes(buffer.transform);
    const double surface_width = swapped ? buffer.height : buffer.width;
    const double surface_height = swapped ? buffer.width : buffer.height;
    const auto [source_x, source_width] = fixed_span(crop.left + first_x * crop_width,
                                                     crop.left + last_x * crop_width, surface_width);
    const auto [source_y, source_height] = fixed_span(
        crop.top + first_y * crop_height, crop.top + last_y * crop_height, surface_height);
    result.source = {source_x, source_y, source_width, source_height};

    result.input.origin_x = visible.left;
    result.input.origin_y = visible.top;
    result.input.bounds = visible;
    result.input.px_per_logical_x = double(visible.width()) / result.width;
    result.input.px_per_logical_y = double(visible.height()) / result.height;
    return result;
}

} // namespace

px_rect intersect(const px_rect &a, const px_rect &b) {
    px_rect result{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
                   std::min(a.bottom, b.bottom)};
    if (result.empty()) {
        return {};
    }
    return result;
}

px_rect bounding_box(const px_rect &a, const px_rect &b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right),
            std::max(a.bottom, b.bottom)};
}

void input_mapping::to_android(double local_x, double local_y, int32_t &android_x,
                               int32_t &android_y) const {
    android_x = static_cast<int32_t>(std::lround(origin_x + local_x * px_per_logical_x));
    android_y = static_cast<int32_t>(std::lround(origin_y + local_y * px_per_logical_y));
}

float_rect crop_in_surface_space(const buffer_view &buffer) {
    const float width = static_cast<float>(buffer.width);
    const float height = static_cast<float>(buffer.height);
    float x0, y0, x1, y1;
    buffer_to_surface(buffer.transform, width, height, buffer.crop.left, buffer.crop.top, x0, y0);
    buffer_to_surface(buffer.transform, width, height, buffer.crop.right, buffer.crop.bottom, x1, y1);
    return {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
}

placement place_in_slot(const px_rect &frame, const buffer_view &buffer, const px_rect &slot,
                        const window_scale &scale) {
    return place(frame, intersect(frame, slot), buffer, slot.left, slot.top, scale);
}

placement place_anchored_to_bottom(const px_rect &frame, const buffer_view &buffer,
                                   const logical_size &window, double scale, int32_t bottom_gap) {
    if (scale <= 0 || window.width <= 0 || window.height <= 0) {
        return {};
    }
    const double width = frame.width() / scale;
    const double height = frame.height() / scale;
    const double x = (window.width - width) / 2;
    const double y = window.height - bottom_gap - height;
    const double origin_x = frame.left - x * scale;
    const double origin_y = frame.top - y * scale;
    const px_rect window_px{static_cast<int32_t>(std::lround(origin_x)),
                            static_cast<int32_t>(std::lround(origin_y)),
                            static_cast<int32_t>(std::lround(origin_x + window.width * scale)),
                            static_cast<int32_t>(std::lround(origin_y + window.height * scale))};
    const double logical_per_px = 1 / scale;
    return place(frame, intersect(frame, window_px), buffer, origin_x, origin_y,
                 {logical_per_px, logical_per_px});
}

input_mapping slot_input_mapping(const px_rect &slot, const window_scale &scale) {
    input_mapping mapping;
    mapping.origin_x = slot.left;
    mapping.origin_y = slot.top;
    mapping.bounds = slot;
    mapping.px_per_logical_x = scale.x > 0 ? 1 / scale.x : 1;
    mapping.px_per_logical_y = scale.y > 0 ? 1 / scale.y : 1;
    return mapping;
}

} // namespace task
