/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "task/sizing.h"

#include <algorithm>
#include <cmath>

namespace task {
namespace {

/// xdg configure serials only grow; compare them modulo 2^32.
bool at_or_after(uint32_t serial, uint32_t reference) {
    return static_cast<int32_t>(serial - reference) >= 0;
}

} // namespace

logical_size window_sizing::size_of(const px_rect &slot) const {
    return {std::max(1, static_cast<int32_t>(std::lround(slot.width() / scale))),
            std::max(1, static_cast<int32_t>(std::lround(slot.height() / scale)))};
}

void window_sizing::adopt_slot(const px_rect &slot) {
    if (slot.empty()) {
        return;
    }
    current_slot = slot;
    if (!pending_serial) {
        current_size = size_of(slot);
    }
}

std::optional<size_request> window_sizing::configure(int32_t width, int32_t height, uint32_t serial,
                                                     bool exact_size) {
    if (width <= 0 || height <= 0) {
        exact = false;
        return std::nullopt;
    }
    exact = exact_size;
    const logical_size requested{width, height};
    if (requested == current_size && !pending_serial) {
        return std::nullopt;
    }
    current_size = requested;
    pending_serial = serial;
    return size_request{static_cast<uint32_t>(std::lround(width * scale)),
                        static_cast<uint32_t>(std::lround(height * scale)), serial};
}

void window_sizing::slot_applied(uint32_t serial, const px_rect &slot) {
    if (slot.empty()) {
        return;
    }
    current_slot = slot;
    if (pending_serial && !at_or_after(serial, *pending_serial)) {
        return;
    }
    pending_serial.reset();
    if (!exact) {
        current_size = size_of(slot);
    }
}

window_scale window_sizing::layer_scale() const {
    if (current_slot.empty() || current_size.width <= 0 || current_size.height <= 0) {
        return {1 / scale, 1 / scale};
    }
    return {double(current_size.width) / current_slot.width(),
            double(current_size.height) / current_slot.height()};
}

} // namespace task
