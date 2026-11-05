/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 *
 * Where an Android layer lands inside a host window. Android draws every task
 * into its own slot of one large display; a task window shows that slot, so a
 * layer sits at its display frame minus the slot origin, scaled to logical
 * units, and is clipped to the slot. Input goes the opposite way.
 */

#pragma once

#include <cstdint>

namespace task {

/// Integer rectangle in Android display pixels.
struct px_rect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;

    int32_t width() const { return right - left; }
    int32_t height() const { return bottom - top; }
    bool empty() const { return width() <= 0 || height() <= 0; }
    bool operator==(const px_rect &other) const {
        return left == other.left && top == other.top && right == other.right && bottom == other.bottom;
    }
    bool operator!=(const px_rect &other) const { return !(*this == other); }
};

px_rect intersect(const px_rect &a, const px_rect &b);
px_rect bounding_box(const px_rect &a, const px_rect &b);

/// Rectangle in buffer or surface coordinates, as HWC source crops are.
struct float_rect {
    float left = 0;
    float top = 0;
    float right = 0;
    float bottom = 0;
};

/// A width and height in logical (compositor) units.
struct logical_size {
    int32_t width = 0;
    int32_t height = 0;
    bool operator==(const logical_size &other) const {
        return width == other.width && height == other.height;
    }
    bool operator!=(const logical_size &other) const { return !(*this == other); }
};

/// wp_viewport source rectangle in wl_fixed units (1/256).
struct fixed_rect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

/// The buffer a layer shows: its size, HWC source crop (buffer coordinates)
/// and wl_output_transform.
struct buffer_view {
    uint32_t width = 0;
    uint32_t height = 0;
    float_rect crop;
    int32_t transform = 0;
};

/// Logical units per Android pixel along each axis. Equal to 1/scale once the
/// window has its slot; differs while a resize waits for its slot.
struct window_scale {
    double x = 1;
    double y = 1;
};

/// Maps surface-local coordinates of one surface to Android display pixels:
/// `origin + local × px_per_logical`.
struct input_mapping {
    double origin_x = 0;
    double origin_y = 0;
    double px_per_logical_x = 1;
    double px_per_logical_y = 1;

    void to_android(double local_x, double local_y, int32_t &android_x, int32_t &android_y) const;
};

struct placement {
    bool visible = false;
    /// Subsurface position inside the window, logical.
    int32_t x = 0;
    int32_t y = 0;
    /// wp_viewport destination, logical, at least 1×1.
    int32_t width = 1;
    int32_t height = 1;
    /// wp_viewport source in surface coordinates, inside the buffer.
    fixed_rect source;
    /// Input mapping for this surface, if it takes input.
    input_mapping input;
};

/// The buffer crop in surface coordinates: after the buffer transform, which
/// is what wp_viewport.set_source takes.
float_rect crop_in_surface_space(const buffer_view &buffer);

/// A task layer: `frame` is its display frame, clipped to `slot` and placed
/// relative to the slot origin.
placement place_in_slot(const px_rect &frame, const buffer_view &buffer, const px_rect &slot,
                        const window_scale &scale);

/// An input method or toast layer inside a window of `window` logical size:
/// centred horizontally, its bottom edge `bottom_gap` logical units above the
/// window's bottom edge, at `scale` Android pixels per logical unit, and
/// clipped to the window.
placement place_anchored_to_bottom(const px_rect &frame, const buffer_view &buffer,
                                   const logical_size &window, double scale, int32_t bottom_gap);

/// Input mapping for a window's root surface.
input_mapping slot_input_mapping(const px_rect &slot, const window_scale &scale);

} // namespace task
