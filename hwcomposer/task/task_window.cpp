/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "task/task_window.h"

#include "agent/link.h"
#include "input/wayland_input.h"
#include "wayland-hwc.h"

#include "crownos-android-shell-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <log/log.h>

#include <cmath>
#include <cstring>

#include <linux/input.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

constexpr int32_t background_pixel_bytes = 4;

/// Every listener below gets the display and looks its window up by proxy
/// under the windows mutex, so an event can never reach a window that the
/// composition or agent thread has just destroyed.
struct display *display_of(void *data) {
    return static_cast<struct display *>(data);
}

bool has_state(const wl_array *states, uint32_t wanted) {
    const auto *state = static_cast<const uint32_t *>(states->data);
    for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i) {
        if (state[i] == wanted) {
            return true;
        }
    }
    return false;
}

void handle_toplevel_configure(void *data, xdg_toplevel *toplevel, int32_t width, int32_t height,
                               wl_array *states) {
    struct display *display = display_of(data);
    std::scoped_lock lock(display->windowsMutex);
    window *window = find_task_window(display, toplevel);
    if (!window) {
        return;
    }
    window->task->pending_width = width;
    window->task->pending_height = height;
    window->task->pending_suspended = has_state(states, XDG_TOPLEVEL_STATE_SUSPENDED);
}

void handle_toplevel_close(void *data, xdg_toplevel *toplevel) {
    struct display *display = display_of(data);
    uint32_t task_id;
    {
        std::scoped_lock lock(display->windowsMutex);
        window *window = find_task_window(display, toplevel);
        if (!window) {
            return;
        }
        task_id = window->task->task_id;
    }
    // simulate user input to restart Android's idle timeout
    send_key_event(display, 0, WL_KEYBOARD_KEY_STATE_PRESSED);
    send_key_event(display, 0, WL_KEYBOARD_KEY_STATE_RELEASED);
    if (display->agent_link->send(agent_wire::Close{task_id})) {
        return;
    }
    // Without the agent, close the task the stock way. This is a blocking
    // binder call, so it runs without the windows mutex.
    if (display->task != nullptr) {
        display->task->removeTask(static_cast<int32_t>(task_id));
    }
}

void handle_toplevel_configure_bounds(void *, xdg_toplevel *, int32_t, int32_t) {}

void handle_toplevel_wm_capabilities(void *, xdg_toplevel *, wl_array *) {}

const xdg_toplevel_listener toplevel_listener = {
    handle_toplevel_configure,
    handle_toplevel_close,
    handle_toplevel_configure_bounds,
    handle_toplevel_wm_capabilities,
};

void handle_surface_configure(void *data, xdg_surface *surface, uint32_t serial) {
    struct display *display = display_of(data);
    std::scoped_lock lock(display->windowsMutex);
    window *window = find_task_window(display, surface);
    if (!window) {
        return;
    }
    xdg_surface_ack_configure(surface, serial);

    task_window_state &task = *window->task;
    const auto request = task.sizing.configure(task.pending_width, task.pending_height, serial);
    if (request) {
        display->agent_link->send(
            agent_wire::SetSize{task.task_id, request->width, request->height, request->serial});
    }
    if (task.pending_suspended != task.suspended) {
        task.suspended = task.pending_suspended;
        display->agent_link->send(agent_wire::Visibility{task.task_id, !task.suspended});
    }
    window->configured = true;
    display->request_frame();
}

const xdg_surface_listener surface_listener = {
    handle_surface_configure,
};

void handle_decoration_configure(void *, zxdg_toplevel_decoration_v1 *, uint32_t) {}

const zxdg_toplevel_decoration_v1_listener decoration_listener = {
    handle_decoration_configure,
};

void handle_back(void *data, crownos_android_toplevel_v1 *android_toplevel) {
    struct display *display = display_of(data);
    uint32_t task_id;
    {
        std::scoped_lock lock(display->windowsMutex);
        window *window = find_task_window(display, android_toplevel);
        if (!window) {
            return;
        }
        task_id = window->task->task_id;
    }
    if (!display->agent_link->send(agent_wire::Back{task_id})) {
        send_key_event(display, KEY_BACK, WL_KEYBOARD_KEY_STATE_PRESSED);
        send_key_event(display, KEY_BACK, WL_KEYBOARD_KEY_STATE_RELEASED);
    }
}

const crownos_android_toplevel_v1_listener android_toplevel_listener = {
    handle_back,
};

void handle_feedback_done(void *data, zwp_linux_dmabuf_feedback_v1 *feedback) {
    struct display *display = display_of(data);
    std::scoped_lock lock(display->windowsMutex);
    if (window *window = find_task_window(display, feedback)) {
        window->task->surface_formats = std::make_shared<const dmabuf::format_set>(
            window->task->feedback_collector.done().formats);
    }
}

void handle_feedback_format_table(void *data, zwp_linux_dmabuf_feedback_v1 *feedback, int32_t fd, uint32_t size) {
    struct display *display = display_of(data);
    void *table = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (table == MAP_FAILED) {
        return;
    }
    {
        std::scoped_lock lock(display->windowsMutex);
        if (window *window = find_task_window(display, feedback)) {
            window->task->feedback_collector.set_table(static_cast<const uint8_t *>(table), size);
        }
    }
    munmap(table, size);
}

void handle_feedback_tranche_formats(void *data, zwp_linux_dmabuf_feedback_v1 *feedback, wl_array *indices) {
    struct display *display = display_of(data);
    std::scoped_lock lock(display->windowsMutex);
    if (window *window = find_task_window(display, feedback)) {
        window->task->feedback_collector.add_tranche_indices(static_cast<const uint16_t *>(indices->data),
                                                             indices->size / sizeof(uint16_t));
    }
}

void handle_feedback_device(void *, zwp_linux_dmabuf_feedback_v1 *, wl_array *) {}

void handle_feedback_tranche_done(void *, zwp_linux_dmabuf_feedback_v1 *) {}

void handle_feedback_tranche_flags(void *, zwp_linux_dmabuf_feedback_v1 *, uint32_t) {}

const zwp_linux_dmabuf_feedback_v1_listener feedback_listener = {
    handle_feedback_done,
    handle_feedback_format_table,
    handle_feedback_device,
    handle_feedback_tranche_done,
    handle_feedback_device,
    handle_feedback_tranche_formats,
    handle_feedback_tranche_flags,
};

wl_buffer *transparent_pixel(struct display *display) {
    const int fd = static_cast<int>(syscall(SYS_memfd_create, "task-window-background", 0));
    if (fd < 0) {
        return nullptr;
    }
    wl_buffer *buffer = nullptr;
    if (ftruncate(fd, background_pixel_bytes) == 0) {
        wl_shm_pool *pool = wl_shm_create_pool(display->shm, fd, background_pixel_bytes);
        buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, background_pixel_bytes, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
    }
    close(fd);
    return buffer;
}

int32_t to_logical(uint32_t pixels, double scale) {
    return static_cast<int32_t>(std::ceil(pixels / scale));
}

} // namespace

task_window_state::task_window_state(uint32_t task_id, std::string package, double scale)
    : task_id(task_id), package(std::move(package)), sizing(scale) {}

task_window_state::~task_window_state() {
    if (feedback) {
        zwp_linux_dmabuf_feedback_v1_destroy(feedback);
    }
    if (decoration) {
        zxdg_toplevel_decoration_v1_destroy(decoration);
    }
    if (android_toplevel) {
        crownos_android_toplevel_v1_destroy(android_toplevel);
    }
}

bool task_window_state::ready(const window &window) const {
    return window.configured && !suspended;
}

std::string task_window_key(uint32_t task_id) {
    return std::to_string(task_id);
}

window *task_window_of(struct display *display, uint32_t task_id) {
    const auto found = display->windows.find(task_window_key(task_id));
    return found != display->windows.end() && found->second->task ? found->second.get() : nullptr;
}

window *find_task_window(struct display *display, const void *proxy) {
    for (auto &[key, window] : display->windows) {
        const task_window_state *task = window->task.get();
        if (!task) {
            continue;
        }
        if (proxy == window->surface || proxy == window->xdg_surface || proxy == window->xdg_toplevel ||
            proxy == task->android_toplevel || proxy == task->decoration || proxy == task->feedback) {
            return window.get();
        }
    }
    return nullptr;
}

void apply_task_facts(struct display *display, window &window, const task::task_facts &facts, bool first) {
    task_window_state &task = *window.task;
    task::task_facts &applied = task.applied_facts;
    if (first || facts.label != applied.label) {
        window.set_title(facts.label.empty() ? task.package.c_str() : facts.label.c_str());
    }
    if (first || facts.min_width != applied.min_width || facts.min_height != applied.min_height) {
        xdg_toplevel_set_min_size(window.xdg_toplevel, to_logical(facts.min_width, display->scale),
                                  to_logical(facts.min_height, display->scale));
    }
    if (task.android_toplevel && (first || facts.resizable != applied.resizable)) {
        crownos_android_toplevel_v1_set_resize_lock(
            task.android_toplevel, facts.resizable ? CROWNOS_ANDROID_TOPLEVEL_V1_RESIZE_LOCK_NONE
                                                   : CROWNOS_ANDROID_TOPLEVEL_V1_RESIZE_LOCK_FIXED);
    }
    if (task.android_toplevel && (first || facts.color != applied.color)) {
        crownos_android_toplevel_v1_set_title_color(task.android_toplevel, facts.color);
    }
    applied = facts;
}

void activate_task_window(struct display *display, window &window, const std::string &token) {
    if (!display->activation || token.empty()) {
        return;
    }
    xdg_activation_v1_activate(display->activation, token.c_str(), window.surface);
    wl_display_flush(display->display);
}

std::unique_ptr<window> window::create_for_task(struct display *display, uint32_t task_id,
                                                const task::task_facts &facts, const task::px_rect &slot) {
    std::unique_ptr<window> created{new window()};
    window &task_window = *created;
    task_window.display = display;
    task_window.dedicated_background_surface = true;
    task_window.taskID = task_window_key(task_id);
    task_window.surface = wl_compositor_create_surface(display->compositor);
    wl_surface_set_user_data(task_window.surface, created.get());
    task_window.viewport = wp_viewporter_get_viewport(display->viewporter, task_window.surface);
    task_window.bg_buffer = transparent_pixel(display);
    task_window.task = std::make_unique<task_window_state>(task_id, facts.package, display->scale);
    task_window_state &task = *task_window.task;
    task.sizing.adopt_slot(slot);

    task_window.xdg_surface = xdg_wm_base_get_xdg_surface(display->wm_base, task_window.surface);
    xdg_surface_add_listener(task_window.xdg_surface, &surface_listener, display);
    task_window.xdg_toplevel = xdg_surface_get_toplevel(task_window.xdg_surface);
    xdg_toplevel_add_listener(task_window.xdg_toplevel, &toplevel_listener, display);
    task_window.set_app_id(facts.package);

    if (display->decoration_manager) {
        task.decoration =
            zxdg_decoration_manager_v1_get_toplevel_decoration(display->decoration_manager, task_window.xdg_toplevel);
        zxdg_toplevel_decoration_v1_add_listener(task.decoration, &decoration_listener, display);
        zxdg_toplevel_decoration_v1_set_mode(task.decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    if (display->android_shell) {
        task.android_toplevel =
            crownos_android_shell_manager_v1_get_android_toplevel(display->android_shell, task_window.xdg_toplevel);
        crownos_android_toplevel_v1_add_listener(task.android_toplevel, &android_toplevel_listener, display);
        crownos_android_toplevel_v1_set_task(task.android_toplevel, task_id, facts.package.c_str());
    }
    apply_task_facts(display, task_window, facts, true);

    if (display->dmabuf && display->dmabuf_version >= ZWP_LINUX_DMABUF_V1_GET_SURFACE_FEEDBACK_SINCE_VERSION) {
        task.feedback = zwp_linux_dmabuf_v1_get_surface_feedback(display->dmabuf, task_window.surface);
        zwp_linux_dmabuf_feedback_v1_add_listener(task.feedback, &feedback_listener, display);
    }

    wl_surface_commit(task_window.surface);
    wl_display_flush(display->display);
    ALOGI("task %u (%s) opened its window", task_id, facts.package.c_str());
    return created;
}
