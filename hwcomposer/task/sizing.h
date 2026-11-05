/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "task/geometry.h"

#include <cstdint>
#include <optional>

namespace task {

/// SetSize the agent must apply, in Android pixels.
struct size_request {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t serial = 0;
};

/// Tracks a task window's size through the configure flow: the compositor
/// configures a logical size, the agent resizes the task's slot and answers
/// with the configure serial. Until that answer the window keeps its new size
/// and stretches the old slot's buffers to fill it.
class window_sizing {
  public:
    explicit window_sizing(double scale) : scale(scale) {}

    /// The first slot known for the task, from the agent or from its layers.
    void adopt_slot(const px_rect &slot);

    /// xdg_toplevel configure. Returns the SetSize to send, if any.
    std::optional<size_request> configure(int32_t width, int32_t height, uint32_t serial);

    /// SlotApplied from the agent.
    void slot_applied(uint32_t serial, const px_rect &slot);

    bool has_slot() const { return !current_slot.empty(); }
    const px_rect &slot() const { return current_slot; }
    /// The window size in logical units: the window geometry.
    logical_size size() const { return current_size; }
    bool resize_pending() const { return pending_serial.has_value(); }
    /// Logical units per Android pixel of the current slot.
    window_scale layer_scale() const;

  private:
    logical_size size_of(const px_rect &slot) const;

    double scale;
    px_rect current_slot;
    logical_size current_size;
    std::optional<uint32_t> pending_serial;
};

} // namespace task
