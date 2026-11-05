/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "task/geometry.h"

#include <mutex>
#include <unordered_map>

struct wl_surface;

namespace input {

/// Where each input-receiving surface sits on the Android display. Written
/// by the composition thread, read by the Wayland thread for every event.
class surface_input_map {
  public:
    void set(wl_surface *surface, const task::input_mapping &mapping) {
        std::lock_guard lock(mutex);
        mappings[surface] = mapping;
    }

    void erase(wl_surface *surface) {
        std::lock_guard lock(mutex);
        mappings.erase(surface);
    }

    bool find(wl_surface *surface, task::input_mapping &mapping) const {
        std::lock_guard lock(mutex);
        const auto found = mappings.find(surface);
        if (found == mappings.end()) {
            return false;
        }
        mapping = found->second;
        return true;
    }

    /// Android display coordinates of a surface-local point. Surfaces the
    /// HWC has not placed yet map with `fallback_scale` from the origin.
    void to_android(wl_surface *surface, double local_x, double local_y, double fallback_scale,
                    int32_t &android_x, int32_t &android_y) const {
        task::input_mapping mapping;
        mapping.px_per_logical_x = fallback_scale;
        mapping.px_per_logical_y = fallback_scale;
        {
            std::lock_guard lock(mutex);
            const auto found = mappings.find(surface);
            if (found != mappings.end()) {
                mapping = found->second;
            }
        }
        mapping.to_android(local_x, local_y, android_x, android_y);
    }

  private:
    mutable std::mutex mutex;
    std::unordered_map<wl_surface *, task::input_mapping> mappings;
};

} // namespace input
