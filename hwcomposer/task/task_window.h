/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 *
 * One Android task as its own xdg_toplevel. The window shows the task's slot
 * of the Android atlas: its size follows the slot, its layers are
 * subsurfaces placed relative to the slot origin, and the compositor learns
 * the task through crownos_android_toplevel_v1.
 */

#pragma once

#include "dmabuf/format_set.h"
#include "task/registry.h"
#include "task/sizing.h"

#include <cstdint>
#include <memory>
#include <string>

struct display;
struct window;
struct crownos_android_toplevel_v1;
struct zwp_linux_dmabuf_feedback_v1;
struct zxdg_toplevel_decoration_v1;

struct task_window_state {
    task_window_state(uint32_t task_id, std::string package, double scale);
    ~task_window_state();

    task_window_state(const task_window_state &) = delete;
    task_window_state &operator=(const task_window_state &) = delete;

    const uint32_t task_id;
    const std::string package;
    /// The agent connection the window was opened under.
    uint32_t agent_session = 0;
    task::window_sizing sizing;

    crownos_android_toplevel_v1 *android_toplevel = nullptr;
    zxdg_toplevel_decoration_v1 *decoration = nullptr;
    zwp_linux_dmabuf_feedback_v1 *feedback = nullptr;
    dmabuf::feedback_collector feedback_collector;
    /// Pairs from the surface's own dmabuf feedback; null until it arrives.
    std::shared_ptr<const dmabuf::format_set> surface_formats;

    /// xdg_toplevel.configure, applied at the xdg_surface.configure after it.
    int32_t pending_width = 0;
    int32_t pending_height = 0;
    bool pending_suspended = false;
    /// Maximized or fullscreen: the configured size must be the geometry.
    bool pending_exact_size = false;
    /// The compositor does not show the window: nothing is committed to it.
    bool suspended = false;

    /// What the window has shown so far (composition thread).
    bool mapped = false;
    /// A layer changed this frame, so the root surface must commit.
    bool dirty = false;
    task::logical_size shown_size;
    size_t layers_last_frame = 0;
    /// TaskInfo as last applied, to resend only what changed.
    task::task_facts applied_facts;

    /// Whether the task can render into the window this frame: configured
    /// and shown.
    bool ready(const window &window) const;
};

/// Applies TaskInfo: title, minimum size, resize lock and title color. They
/// are double-buffered and take effect with the window's next commit.
void apply_task_facts(struct display *display, window &window, const task::task_facts &facts, bool first = false);

/// Raises the window with an xdg_activation token.
void activate_task_window(struct display *display, window &window, const std::string &token);

/// The task window whose root surface, xdg_surface, xdg_toplevel, Android
/// extension, decoration or dmabuf feedback is `proxy`. Call with the windows mutex held.
window *find_task_window(struct display *display, const void *proxy);

/// The window of `task_id`, if it is a task window. Call with the windows
/// mutex held.
window *task_window_of(struct display *display, uint32_t task_id);

std::string task_window_key(uint32_t task_id);
