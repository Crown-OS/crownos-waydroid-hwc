/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "output/outputs.h"

#include "wayland-hwc.h"

#include "xdg-output-unstable-v1-client-protocol.h"

#include <algorithm>

namespace {

constexpr uint32_t highest_output_version = 4;

void output_geometry(void *data, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *,
                     const char *, int32_t transform) {
    static_cast<output_registry::entry *>(data)->info.transform = transform;
}

void output_mode(void *data, wl_output *, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    auto *entry = static_cast<output_registry::entry *>(data);
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) {
        return;
    }
    entry->info.mode_width = width;
    entry->info.mode_height = height;
    entry->info.refresh_mhz = refresh;

    struct display *display = entry->display;
    display->refresh = std::max(display->refresh, refresh);
    display->full_width = width;
    display->full_height = height;
}

void output_done(void *, wl_output *) {}

void output_scale(void *data, wl_output *, int32_t factor) {
    auto *entry = static_cast<output_registry::entry *>(data);
    entry->info.integer_scale = factor;
    struct display *display = entry->display;
    if (!display->fixed_atlas) {
        display->scale = std::max(display->scale, double(factor));
    }
}

void output_name(void *, wl_output *, const char *) {}

void output_description(void *, wl_output *, const char *) {}

const wl_output_listener output_listener = {
    output_geometry, output_mode, output_done, output_scale, output_name, output_description,
};

void logical_position(void *, zxdg_output_v1 *, int32_t, int32_t) {}

void logical_size(void *data, zxdg_output_v1 *, int32_t width, int32_t height) {
    auto *entry = static_cast<output_registry::entry *>(data);
    entry->info.logical_width = width;
    entry->info.logical_height = height;
}

void xdg_output_done(void *, zxdg_output_v1 *) {}

void xdg_output_name(void *, zxdg_output_v1 *, const char *) {}

void xdg_output_description(void *, zxdg_output_v1 *, const char *) {}

const zxdg_output_v1_listener xdg_output_listener = {
    logical_position, logical_size, xdg_output_done, xdg_output_name, xdg_output_description,
};

} // namespace

void output_registry::bind(struct display *display, wl_registry *registry, uint32_t name, uint32_t version) {
    auto entry = std::make_unique<output_registry::entry>();
    entry->display = display;
    entry->global_name = name;
    entry->output = static_cast<wl_output *>(
        wl_registry_bind(registry, name, &wl_output_interface, std::min(version, highest_output_version)));
    wl_output_add_listener(entry->output, &output_listener, entry.get());
    if (!display->output) {
        display->output = entry->output;
    }
    std::lock_guard lock(mutex);
    if (xdg_output_manager) {
        add_xdg_output(*entry, xdg_output_manager);
    }
    entries.push_back(std::move(entry));
}

bool output_registry::remove(uint32_t name) {
    std::lock_guard lock(mutex);
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [name](const auto &entry) { return entry->global_name == name; });
    if (found == entries.end()) {
        return false;
    }
    entry &removed = **found;
    if (removed.display->output == removed.output) {
        removed.display->output = entries.size() > 1 ? (found == entries.begin() ? entries[1] : entries[0])->output
                                                     : nullptr;
    }
    if (removed.xdg_output) {
        zxdg_output_v1_destroy(removed.xdg_output);
    }
    if (wl_output_get_version(removed.output) >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
        wl_output_release(removed.output);
    } else {
        wl_output_destroy(removed.output);
    }
    entries.erase(found);
    return true;
}

void output_registry::add_xdg_output(entry &output, zxdg_output_manager_v1 *manager) {
    output.xdg_output = zxdg_output_manager_v1_get_xdg_output(manager, output.output);
    zxdg_output_v1_add_listener(output.xdg_output, &xdg_output_listener, &output);
}

void output_registry::watch_logical_sizes(zxdg_output_manager_v1 *manager) {
    std::lock_guard lock(mutex);
    xdg_output_manager = manager;
    for (auto &entry : entries) {
        if (!entry->xdg_output) {
            add_xdg_output(*entry, manager);
        }
    }
}

double output_registry::highest_scale() const {
    std::vector<output::output_info> outputs;
    {
        std::lock_guard lock(mutex);
        for (const auto &entry : entries) {
            outputs.push_back(entry->info);
        }
    }
    return output::highest_scale(outputs);
}

void output_registry::destroy_all() {
    std::vector<uint32_t> names;
    {
        std::lock_guard lock(mutex);
        for (const auto &entry : entries) {
            names.push_back(entry->global_name);
        }
    }
    for (uint32_t name : names) {
        remove(name);
    }
}
