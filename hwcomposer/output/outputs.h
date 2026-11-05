/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "output/scale.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

struct display;
struct wl_output;
struct wl_registry;
struct zxdg_output_manager_v1;
struct zxdg_output_v1;

/// Every wl_output (v4) with what xdg_output adds, for the render scale.
struct output_registry {
    struct entry {
        struct display *display = nullptr;
        uint32_t global_name = 0;
        wl_output *output = nullptr;
        zxdg_output_v1 *xdg_output = nullptr;
        output::output_info info;
    };

    void bind(struct display *display, wl_registry *registry, uint32_t name, uint32_t version);
    /// wl_registry.global_remove; false when `name` is not an output.
    bool remove(uint32_t name);
    /// Adds xdg_output to the outputs bound so far.
    void watch_logical_sizes(zxdg_output_manager_v1 *manager);
    /// The highest preferred scale across outputs.
    double highest_scale() const;
    void destroy_all();

  private:
    void add_xdg_output(entry &output, zxdg_output_manager_v1 *manager);

    mutable std::mutex mutex;
    std::vector<std::unique_ptr<entry>> entries;
    zxdg_output_manager_v1 *xdg_output_manager = nullptr;
};
