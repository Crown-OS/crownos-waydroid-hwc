/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright © 2010 Intel Corporation
 * Copyright © 2014 Collabora Ltd.
 * Copyright © 2021 Waydroid Project.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "wayland-hwc.h"

#include "hwcomposer.h"
#include "egl-tools.h"
#include "agent/bridge.h"
#include "agent/link.h"
#include "dmabuf/feedback_reader.h"
#include "fence/frame_sync.h"
#include "input/wayland_input.h"
#include "output/outputs.h"
#include "task/task_window.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/memfd.h>
#include <drm_fourcc.h>
#include <system/graphics.h>
#include <syscall.h>
#include <cmath>
#include <algorithm>

#include <sync/sync.h>
#include <hardware/gralloc.h>
#include <log/log.h>
#include <sys/resource.h>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#include <cutils/properties.h>

#include <xkbcommon/xkbcommon.h>
#include <cinttypes>

#include "gralloc_handler.h"

#include <wayland-client.h>
#include <wayland-android-client-protocol.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "tablet-unstable-v2-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "crownos-android-shell-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

namespace {

constexpr const char *agent_socket = "/dev/socket/crown_agent";
/// Atlas size when the bundle sets no persist.waydroid.width/height.
constexpr int default_atlas_size = 4096;
constexpr uint32_t highest_wm_base_version = 6;
constexpr uint32_t highest_seat_version = 8;
constexpr uint32_t highest_dmabuf_version = 4;
constexpr uint32_t highest_xdg_output_version = 3;

} // namespace

using ::android::hardware::hidl_string;

struct buffer;

buffer::~buffer() {
    wl_buffer_destroy(wl_buffer);
    if (isShm)
        munmap(shm_data, size);
}

// Call me from egl_worker_thread only!
void snapshot_inactive_app_window(struct display *display, struct window *window) {
    if (!window->layers[0].surface || !window->last_layer_buffer
        || window->last_layer_buffer->isShm || window->snapshot_buffer) {
        // Need a surface to draw and a non-SHM buffer to make snapshot from
        return;
    }

    ALOGI("Making inactive window snapshot for %s", window->taskID.c_str());

    struct buffer *old_buf = window->last_layer_buffer;

    window->snapshot_buffer = create_shm_wl_buffer(display, old_buf->metadata, old_buf->handle);
    if (!window->snapshot_buffer) {
        ALOGE("failed to create a wayland buffer for window snapshot");
        return;
    }

    // FIXME won't work as expected if there are multiple surfaces
    struct wl_surface *surface = window->layers[0].surface;
    // wl_shm buffers cannot use explicit sync
    window->layers[0].sync.reset();

    egl_render_to_pixels(display, window->snapshot_buffer.get());

    wl_surface_attach(surface, window->snapshot_buffer->wl_buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface);

    window->last_layer_buffer = nullptr;
}

static void
xdg_surface_handle_configure(void *data, struct xdg_surface *surface,
                 uint32_t serial)
{
    auto window = static_cast<struct window *>(data);

    xdg_surface_ack_configure(surface, serial);

    window->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    xdg_surface_handle_configure,
};

void choose_width_height(struct display* display, int32_t hint_width, int32_t hint_height) {
    char property[PROPERTY_VALUE_MAX];
    int width = hint_width;
    int height = hint_height;

    // The atlas never follows the host: its logical size only mirrors the pixels
    if (display->fixed_atlas) {
        display->isMaximized = false;
        display->width = std::max(1, int(lround(display->atlas_width / display->scale)));
        display->height = std::max(1, int(lround(display->atlas_height / display->scale)));
        return;
    }

    // Ignore hint it requested
    if (property_get("persist.waydroid.width", property, nullptr) > 0) {
        display->isMaximized = false;
        width = atoi(property);
    }

    if (property_get("persist.waydroid.height", property, nullptr) > 0) {
        display->isMaximized = false;
        height = atoi(property);
    }

    display->width = width;
    display->height = height;
}

void
finished_calibrating(struct display *d)
{
    char property[PROPERTY_VALUE_MAX];
    int default_density = 180;
    std::string display_scale = std::to_string(d->scale);
    property_set("waydroid.display_scale", display_scale.c_str());
    if (property_get("ro.sf.lcd_density", property, nullptr) <= 0) {
        std::string lcd_density = std::to_string(int(default_density * d->scale));
        property_set("ro.sf.lcd_density", lcd_density.c_str());
    }

    choose_width_height(d, d->req_width, d->req_height);
}

void
do_hotplug(struct display *display) {
    if (display->touch) {
        char property[PROPERTY_VALUE_MAX];
        int width = floor(display->width * display->scale);
        int height = floor(display->height * display->scale);

        if (property_get("persist.waydroid.width_padding", property, nullptr) > 0)
            width -= atoi(property);
        std::string width_str = std::to_string(width);
        property_set("waydroid.display_width", width_str.c_str());

        if (property_get("persist.waydroid.height_padding", property, nullptr) > 0)
            height -= atoi(property);
        std::string height_str = std::to_string(height);
        property_set("waydroid.display_height", height_str.c_str());

        display->input_fd[INPUT_TOUCH] = -1;
        remove(INPUT_PIPE_NAME[INPUT_TOUCH]);
        mkfifo(INPUT_PIPE_NAME[INPUT_TOUCH], S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
        chown(INPUT_PIPE_NAME[INPUT_TOUCH], 1000, 1000);
    }
    if (display->procs && display->procs->invalidate) {
        display->needHotplug = true;
        display->procs->invalidate(display->procs);
    }
}

static void
xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *,
                              int32_t width, int32_t height,
                              struct wl_array *)
{
    struct window *window = (struct window *)data;
    struct display *display = window->display;

    if (width <= 1 || height <= 1) {
		/* Compositor is deferring to us */
        /* or invalid during calibration */
		return;
	}

    display->req_width = width;
    display->req_height = height;

    if (display->height && display->width) {
        int old_width = display->width;
        int old_height = display->height;
        choose_width_height(display, width, height);
        if (display->wm_base)
            xdg_surface_set_window_geometry(window->xdg_surface, 0, 0, display->width, display->height);
        if (display->width != old_width || display->height != old_height)
            do_hotplug(display);
    }
}

static void
xdg_toplevel_handle_close(void *data, struct xdg_toplevel *)
{
    struct window *window = (struct window *)data;
    struct display *display = window->display;

    // simulate user input to restart idle timeout (TODO: find a better way)
    send_key_event(window->display, 0, WL_KEYBOARD_KEY_STATE_PRESSED);
    send_key_event(window->display, 0, WL_KEYBOARD_KEY_STATE_RELEASED);

    if (display->task != nullptr) {
        if (window->taskID != "none") {
            if (window->taskID == "0") {
                property_set("waydroid.active_apps", "none");
                display->task->removeAllVisibleRecentTasks();
            } else {
                display->task->removeTask(stoi(window->taskID));
            }
        }
    }

    std::scoped_lock lock(window->display->windowsMutex);
    std::string key;
    if (window->taskID != "0" && window->taskID != "none") {
        key = window->taskID;
    } else {
        key = window->appID;
    }
    display->ignored_apps.insert(key);
    display->windows.erase(key);
}

static void
xdg_toplevel_handle_configure_bounds(void *, struct xdg_toplevel *, int32_t, int32_t)
{
}

static void
xdg_toplevel_handle_wm_capabilities(void *, struct xdg_toplevel *, struct wl_array *)
{
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    xdg_toplevel_handle_configure,
    xdg_toplevel_handle_close,
    xdg_toplevel_handle_configure_bounds,
    xdg_toplevel_handle_wm_capabilities,
};

void
shell_surface_ping(void *, struct wl_shell_surface *shell_surface, uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

void
shell_surface_configure(void *data, struct wl_shell_surface *, uint32_t, int32_t width, int32_t height)
{
    struct window *window = (struct window *)data;
    struct display *display = window->display;

    if (width > 1 && height > 1) {
        display->req_width = width;
        display->req_height = height;
	}

    window->configured = true;

    if (display->height && display->width && width > 1 && height > 1) {
        int old_width = display->width;
        int old_height = display->height;
        choose_width_height(display, width, height);
        if (display->width != old_width || display->height != old_height)
            do_hotplug(display);
    }
}

void
shell_surface_popup_done(void *, struct wl_shell_surface *)
{
}

struct wl_shell_surface_listener shell_surface_listener = {
	&shell_surface_ping,
	&shell_surface_configure,
	&shell_surface_popup_done
};


BufferTransform hwc_transform_to_buffer_transform(uint32_t hwc_transform) {
    switch (hwc_transform) {
        case 0:
            return BufferTransform::Normal;
        case HWC_TRANSFORM_FLIP_H:
            return BufferTransform::Flip_Rot_180;
        case HWC_TRANSFORM_FLIP_V:
            return BufferTransform::Flip;
        case HWC_TRANSFORM_ROT_90:
            return BufferTransform::Rot_90;
        case HWC_TRANSFORM_ROT_180:
            return BufferTransform::Rot_180;
        case HWC_TRANSFORM_ROT_270:
            return BufferTransform::Rot_270;
        case HWC_TRANSFORM_FLIP_H_ROT_90:
            return BufferTransform::Flip_Rot_270;
        case HWC_TRANSFORM_FLIP_V_ROT_90:
            return BufferTransform::Flip_Rot_90;
        default:
            ALOGE("Invalid hwc_transform: %" PRIu32, hwc_transform);
            abort();
    }
}

void surface_context::attach_buffer(buffer& buf) {
    wl_surface_attach(surface, buf.wl_buffer, 0, 0);
}

void surface_context::damage_surface(int32_t x, int32_t y, int32_t width, int32_t height) {
    wl_surface_damage(surface, x, y, width, height);
}

void surface_context::set_buffer_transform(BufferTransform transform) {
    wl_surface_set_buffer_transform(surface, static_cast<int32_t>(transform));
}

void surface_context::set_buffer_scale(double d_scale) {
    assert(!viewport);

    // Usually with no viewporter the scale is guaranteed to be integer
    // When supports_cursor_viewport == false, this might not be the case
    // thus use ceil anyway
    int32_t scale = static_cast<int32_t>(ceil(d_scale));
    wl_surface_set_buffer_scale(surface, scale);
}

void surface_context::set_crop(hwc_frect_t crop) {
    assert(viewport);

    float x = fmax(0, crop.left);
    float y = fmax(0, crop.top);
    float width = fmax(1, crop.right - crop.left);
    float height = fmax(1, crop.bottom - crop.top);

    wp_viewport_set_source(viewport,
                           wl_fixed_from_double(x),
                           wl_fixed_from_double(y),
                           wl_fixed_from_double(width),
                           wl_fixed_from_double(height));
}

void surface_context::set_display_frame(hwc_rect_t rect, double scale) {
    assert(viewport);

    int width = static_cast<int>(ceil((rect.right - rect.left) / scale));
    int height = static_cast<int>(ceil((rect.bottom - rect.top) / scale));

    wp_viewport_set_destination(viewport,
                                std::max(1, width),
                                std::max(1, height));
}

void window::reset_per_set_state() {
    lastLayer = 0;
    last_layer_buffer = nullptr;
    if (input_region) {
        wl_region_subtract(input_region, 0, 0, INT_MAX, INT_MAX);
    }
}

void window::set_maximize(bool enabled) {
    if (xdg_toplevel) {
        if (enabled) {
            xdg_toplevel_set_maximized(xdg_toplevel);
        } else {
            xdg_toplevel_unset_maximized(xdg_toplevel);
        }
    } else {
        assert(shell_surface);
        if (enabled) {
            wl_shell_surface_set_maximized(shell_surface, display->output);
        } else {
            ALOGW("wl_shell_surface: does not support un-maximizing");
        }
    }
}

void window::set_title(const char* title) {
    if (xdg_toplevel) {
        xdg_toplevel_set_title(xdg_toplevel, title);
    } else {
        assert(shell_surface);
        wl_shell_surface_set_title(shell_surface, title);
    }
}

void window::set_app_id(std::string appID) {
    this->appID = std::move(appID);
    if (xdg_toplevel) {
        const std::string &wayland_app_id = [&](){
            if (this->appID != "Waydroid") {
                return "waydroid." + this->appID;
            } else {
                return this->appID;
            }
        }();
        xdg_toplevel_set_app_id(xdg_toplevel, wayland_app_id.c_str());
    }
}

void window::minimize() {
    if (xdg_toplevel) {
        xdg_toplevel_set_minimized(xdg_toplevel);
        wl_surface_commit(surface); // unclear if this is required
        wl_display_flush(display->display);
    }
}

window::~window() {
    task.reset();
    display->input_map.erase(surface);
    for (const auto &layer : layers)
        display->input_map.erase(layer.surface);
    if (xdg_toplevel)
        xdg_toplevel_destroy(xdg_toplevel);
    if (xdg_surface)
        xdg_surface_destroy(xdg_surface);
    if (shell_surface)
        wl_shell_surface_destroy(shell_surface);
    if (bg_buffer)
        wl_buffer_destroy(bg_buffer);
    if (input_region)
        wl_region_destroy(input_region);
    if (idle_inhibitor)
        zwp_idle_inhibitor_v1_destroy(idle_inhibitor);

    layers.clear();
    if (dedicated_background_surface) {
        if (viewport)
            wp_viewport_destroy(viewport);
        if (surface) {
            wl_surface_set_user_data(surface, nullptr);
            wl_surface_destroy(surface);
        }
        if (locked_pointer)
            zwp_locked_pointer_v1_destroy(locked_pointer);
    }

    wl_display_flush(display->display);
}

static void fractional_scale_handle_preferred_scale(void *data, struct wp_fractional_scale_v1 *,
            uint32_t scale_times_120) {
    struct display *display = (struct display *)data;
    if (!display->viewporter) {
        // We should always have the viewporter if we have the fractional scale manager
        // but for debugging purpuses we may decide to disable one
        return;
    }
    display->scale = scale_times_120 / 120.0;
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred_scale
};

std::unique_ptr<window>
window::create(struct display *display, bool use_subsurfaces, std::string appID, std::string taskID, hwc_color_t color)
{
    std::unique_ptr<window> window { new struct window() };
    if (!window)
        return nullptr;

    window->display = display;
    window->surface = wl_compositor_create_surface(display->compositor);
    wl_surface_set_user_data(window->surface, window.get());
    if (display->viewporter)
        window->viewport = wp_viewporter_get_viewport(display->viewporter, window->surface);
    window->taskID = std::move(taskID);
    window->dedicated_background_surface = true;
    window->bg_buffer = nullptr;

    int fd = syscall(SYS_memfd_create, "buffer", 0);
    ftruncate(fd, 4);
    void *shm_data = mmap(NULL, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm_data == MAP_FAILED) {
        ALOGE("mmap failed");
        close(fd);
        exit(1);
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(display->shm, fd, 4);
    close(fd);

    // Is this the first window created?
    bool calibrating = !display->height || !display->width;

    if (display->wm_base) {
        window->xdg_surface =
                xdg_wm_base_get_xdg_surface(display->wm_base, window->surface);
        assert(window->xdg_surface);
        xdg_surface_add_listener(window->xdg_surface,
                                     &xdg_surface_listener, window.get());

        window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
        assert(window->xdg_toplevel);
        xdg_toplevel_add_listener(window->xdg_toplevel, &xdg_toplevel_listener, window.get());
    } else if (display->shell) {
        window->shell_surface =
            wl_shell_get_shell_surface(display->shell, window->surface);
        assert(window->shell_surface);
        wl_shell_surface_add_listener(window->shell_surface, &shell_surface_listener, window.get());
        wl_shell_surface_set_toplevel(window->shell_surface);
    } else {
        abort();
    }

    if (display->isMaximized || calibrating) {
        window->set_maximize(true);
    }
    // Set title
    if (appID != "Waydroid" && display->task) {
        const hidl_string appID_hidl(appID);
        display->task->getAppName(appID_hidl, [&](const hidl_string &value){
            window->set_title(value.c_str());
        });
    } else {
        window->set_title(appID.c_str());
    }
    // Set appID
    window->set_app_id(std::move(appID));

    wl_surface_commit(window->surface);
    // Wait for first configure event
    do {
        wl_display_roundtrip(display->display);
    } while (!window->configured);

    if (calibrating) {
        wp_fractional_scale_v1* fs = nullptr;
        if (display->fractional_scale_manager) {
            // We only support one global scale
            fs = wp_fractional_scale_manager_v1_get_fractional_scale(
                    display->fractional_scale_manager, window->surface);
            wp_fractional_scale_v1_add_listener(fs, &fractional_scale_listener, display);
        }

        // Some compositors fail to give us a window size or scale without a buffer attached
        // See: https://github.com/swaywm/sway/issues/2176
        // Some other compositors may refine the window size after a buffer is attached (Hyprland)
        // Try second configure event, with buffer attached
        struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
        wl_surface_attach(window->surface, buf, 0, 0);
        if (window->viewport && display->req_width && display->req_height)
            wp_viewport_set_destination(window->viewport, display->req_width, display->req_height);
        wl_surface_commit(window->surface);
        wl_display_roundtrip(display->display);

        if (fs) {
            wp_fractional_scale_v1_destroy(fs);
        }

        // If after all of this we still did not receive a proper configure event,
        // fallback to using the full output size.
        // NOTICE: full_width and full_heigth should be in compositor logical size!
        if (!display->req_height)
            display->req_height = display->full_height / display->scale;
        if (!display->req_width)
            display->req_width = display->full_width / display->scale;

        finished_calibrating(display);
        if (!display->isMaximized)
            window->set_maximize(false);
    }

    if (display->wm_base)
        xdg_surface_set_window_geometry(window->xdg_surface, 0, 0, display->width, display->height);

    struct wl_region *region = wl_compositor_create_region(display->compositor);
    if (color.a == 0) {
        wl_surface_set_input_region(window->surface, region);
        if (display->system_version >= 33)
            window->input_region = region;
        else
            wl_region_destroy(region);
    }
    if (color.a == 255) {
        wl_region_add(region, 0, 0, display->width, display->height);
        wl_surface_set_opaque_region(window->surface, region);
        wl_region_destroy(region);
    }

    // TODO: Fix background when viewport is not supported
    // No subsurface background for us!
    if (!display->subcompositor ||
        !display->viewporter ||
        property_get_bool("persist.waydroid.no_background_subsurface", false)) {
        window->dedicated_background_surface = false;
        window->layers.emplace_back(window->surface, window->viewport);
        wl_shm_pool_destroy(pool);
        wl_surface_commit(window->surface);
        return window;
    } else if (!use_subsurfaces) {
        /* If we do not use subsurfaces for compositing create at least one for the window content
         * This surface should be desync so that we don't need to send commits for the background surface.
         * Since the surface's position will always be (0,0) committing the parent surface is not required then*/
        window->create_new_layer();
        wl_subsurface_set_desync(window->layers[0].subsurface);
    }

    uint32_t *buf = (uint32_t*)shm_data;
    *buf = color.a << 24 | color.r << 16 | color.g << 8 | color.b;
    window->bg_buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    wl_surface_attach(window->surface, window->bg_buffer, 0, 0);
    wl_surface_damage(window->surface, 0, 0, INT32_MAX, INT32_MAX);
    wp_viewport_set_source(window->viewport, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(1), wl_fixed_from_int(1));
    wp_viewport_set_destination(window->viewport, display->width, display->height);
    wl_surface_commit(window->surface);

    return window;
}

surface_context::surface_context(wl_surface *surface, wp_viewport *viewport) : surface(surface), viewport(viewport) {}
surface_context::~surface_context() {
    sync.reset();
    if (viewport)
        wp_viewport_destroy(viewport);
    if (surface)
        wl_surface_destroy(surface);
}
surface_context::surface_context(surface_context&& other) : surface(other.surface), viewport(other.viewport), sync(std::move(other.sync)) {
    other.surface = nullptr;
    other.viewport = nullptr;
}
surface_context& surface_context::operator=(surface_context&& rhs) {
    sync.reset();
    if (viewport)
        wp_viewport_destroy(viewport);
    if (surface)
        wl_surface_destroy(surface);

    surface = rhs.surface;
    viewport = rhs.viewport;
    sync = std::move(rhs.sync);

    rhs.surface = nullptr;
    rhs.viewport = nullptr;

    return *this;
}

window::layer::layer(wl_surface *surface, wp_viewport *viewport, wl_subsurface *subsurface) : surface_context(surface, viewport), subsurface(subsurface) {}
window::layer::~layer() {
    if (subsurface)
        wl_subsurface_destroy(subsurface);
    if (locked_pointer)
        zwp_locked_pointer_v1_destroy(locked_pointer);
}
window::layer::layer(window::layer&& other) :
        surface_context(std::move(static_cast<surface_context&>(other))),
        subsurface(other.subsurface),
        locked_pointer(other.locked_pointer),
        accepts_input(other.accepts_input),
        shown_buffer(other.shown_buffer),
        shown_placement(other.shown_placement) {
    other.subsurface = nullptr;
    other.locked_pointer = nullptr;
}
window::layer& window::layer::operator=(window::layer&& rhs) {
    if (subsurface)
        wl_subsurface_destroy(subsurface);
    if (locked_pointer)
        zwp_locked_pointer_v1_destroy(locked_pointer);

    subsurface = rhs.subsurface;
    locked_pointer = rhs.locked_pointer;
    accepts_input = rhs.accepts_input;
    shown_buffer = rhs.shown_buffer;
    shown_placement = rhs.shown_placement;

    rhs.subsurface = nullptr;
    rhs.locked_pointer = nullptr;

    surface_context::operator=(std::move(rhs));

    return *this;
}

void window::layer::set_position(int32_t x, int32_t y) {
    assert(subsurface || (x == 0 && y == 0));
    if (subsurface) {
        wl_subsurface_set_position(subsurface, x, y);
    }
}

window::layer& window::get_next_layer() {
    if (lastLayer >= layers.size()) {
        assert(lastLayer == layers.size());
        create_new_layer();
    }
    return layers[lastLayer++];
}

window::layer& window::create_new_layer() {
    wl_surface *surface = wl_compositor_create_surface(display->compositor);
    wl_subsurface *subsurface = wl_subcompositor_get_subsurface(display->subcompositor, surface, this->surface);
    wp_viewport *viewport = nullptr;
    if (display->viewporter)
        viewport = wp_viewporter_get_viewport(display->viewporter, surface);

    return layers.emplace_back(surface, viewport, subsurface);
}

static void
xdg_wm_base_ping(void *, struct xdg_wm_base *wm_base, uint32_t serial)
{
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    xdg_wm_base_ping,
};

static void
presentation_clock_id(void *, struct wp_presentation *,
              uint32_t clk_id)
{
    ALOGE("*** %s: clk_id %d CLOCK_MONOTONIC %d", __func__, clk_id, CLOCK_MONOTONIC);
}

static const struct wp_presentation_listener presentation_listener = {
    presentation_clock_id
};

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *, uint32_t format)
{
    struct display *d = (struct display*)data;
    if (!d->legacy_formats.formats.contains(format, dmabuf::implicit_modifier)) {
        d->legacy_formats.formats.add(format, dmabuf::implicit_modifier);
        d->legacy_formats.preference.push_back({format, dmabuf::implicit_modifier});
    }
}

static void
dmabuf_modifiers(void *data, struct zwp_linux_dmabuf_v1 *,
         uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
    struct display *d = (struct display*)data;
    const uint64_t modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
    if (!d->legacy_formats.formats.contains(format, modifier)) {
        d->legacy_formats.formats.add(format, modifier);
        d->legacy_formats.preference.push_back({format, modifier});
    }
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    dmabuf_format,
    dmabuf_modifiers
};

static void
bind_dmabuf(struct display *d, struct wl_registry *registry, uint32_t id, uint32_t version)
{
    d->dmabuf_version = std::min(version, highest_dmabuf_version);
    d->dmabuf = (struct zwp_linux_dmabuf_v1*)wl_registry_bind(registry, id,
            &zwp_linux_dmabuf_v1_interface, d->dmabuf_version);
    if (d->dmabuf_version < ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) {
        zwp_linux_dmabuf_v1_add_listener(d->dmabuf, &dmabuf_listener, d);
        return;
    }
    d->default_feedback = std::make_unique<dmabuf::feedback_reader>(
        zwp_linux_dmabuf_v1_get_default_feedback(d->dmabuf), [d](dmabuf::feedback feedback) {
            dmabuf::publish_modifier_properties(feedback.preference);
            d->set_importable_formats(std::move(feedback.formats));
        });
}

static void
registry_handle_global(void *data, struct wl_registry *registry,
               uint32_t id, const char *interface, uint32_t version)
{
    struct display *d = (struct display*)data;

    if (strcmp(interface, "wl_compositor") == 0) {
        d->compositor =
            (struct wl_compositor*)wl_registry_bind(registry,
                id, &wl_compositor_interface, std::min(version, 5U));
    } else if (strcmp(interface, "wl_subcompositor") == 0) {
        d->subcompositor =
        (struct wl_subcompositor*)wl_registry_bind(registry,
                id, &wl_subcompositor_interface, 1);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        d->wm_base_version = std::min(version, highest_wm_base_version);
        d->wm_base = (struct xdg_wm_base*)wl_registry_bind(registry,
                id, &xdg_wm_base_interface, d->wm_base_version);
        xdg_wm_base_add_listener(d->wm_base, &xdg_wm_base_listener, d);
    } else if(strcmp(interface, "wl_shell") == 0) {
        d->shell = (struct wl_shell *)wl_registry_bind(
                registry, id, &wl_shell_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0) {
        d->seat_version = std::min(version, highest_seat_version);
        d->seat = (struct wl_seat*)wl_registry_bind(registry, id,
                &wl_seat_interface, d->seat_version);
        wl_seat_add_listener(d->seat, &seat_listener, d);
        if (d->tablet_manager && !d->tablet_seat)
            add_tablet_seat(d);
        if (d->data_device_manager && !d->data_device)
            d->data_device = wl_data_device_manager_get_data_device(d->data_device_manager, d->seat);
    } else if (strcmp(interface, "wl_shm") == 0) {
		d->shm = (struct wl_shm *)wl_registry_bind(registry, id,
                &wl_shm_interface, 1);
    } else if (strcmp(interface, "wl_output") == 0) {
        d->outputs->bind(d, registry, id, version);
        wl_display_roundtrip(d->display);
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        d->xdg_output_manager = (struct zxdg_output_manager_v1 *)wl_registry_bind(registry, id,
                &zxdg_output_manager_v1_interface, std::min(version, highest_xdg_output_version));
        d->outputs->watch_logical_sizes(d->xdg_output_manager);
    } else if (strcmp(interface, "wp_presentation") == 0) {
        bool no_presentation = property_get_bool("persist.waydroid.no_presentation", false);
        if (!no_presentation) {
            d->presentation = (struct wp_presentation*)wl_registry_bind(registry, id,
                    &wp_presentation_interface, 1);
            wp_presentation_add_listener(d->presentation,
                    &presentation_listener, d);
        }
    } else if (strcmp(interface, "wp_viewporter") == 0) {
        d->viewporter = (struct wp_viewporter*)wl_registry_bind(registry, id,
                &wp_viewporter_interface, 1);
    } else if ((d->gtype == GrallocType::GRALLOC_ANDROID) &&
               (strcmp(interface, "android_wlegl") == 0)) {
        d->android_wlegl = (struct android_wlegl*)wl_registry_bind(registry, id,
                &android_wlegl_interface, 1);
    } else if ((d->gtype == GrallocType::GRALLOC_GBM || d->gtype == GrallocType::GRALLOC_CROS) &&
               (strcmp(interface, "zwp_linux_dmabuf_v1") == 0)) {
        if (version < 3)
            return;
        bind_dmabuf(d, registry, id, version);
    } else if (strcmp(interface, wp_linux_drm_syncobj_manager_v1_interface.name) == 0) {
        d->syncobj_manager = (struct wp_linux_drm_syncobj_manager_v1 *)wl_registry_bind(registry, id,
                &wp_linux_drm_syncobj_manager_v1_interface, 1);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        d->decoration_manager = (struct zxdg_decoration_manager_v1 *)wl_registry_bind(registry, id,
                &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, xdg_activation_v1_interface.name) == 0) {
        d->activation = (struct xdg_activation_v1 *)wl_registry_bind(registry, id,
                &xdg_activation_v1_interface, 1);
    } else if (strcmp(interface, crownos_android_shell_manager_v1_interface.name) == 0) {
        d->android_shell = (struct crownos_android_shell_manager_v1 *)wl_registry_bind(registry, id,
                &crownos_android_shell_manager_v1_interface, 1);
    } else if (strcmp(interface, "zwp_pointer_gestures_v1") == 0) {
        bind_pointer_gestures(d, registry, id, version);
    } else if (strcmp(interface, "zwp_tablet_manager_v2") == 0) {
        d->tablet_manager = (struct zwp_tablet_manager_v2 *)wl_registry_bind(registry, id,
                &zwp_tablet_manager_v2_interface, 1);
        if (d->tablet_manager && d->seat)
            add_tablet_seat(d);
    } else if (strcmp(interface, "zwp_pointer_constraints_v1") == 0) {
        d->pointer_constraints = (struct zwp_pointer_constraints_v1 *)wl_registry_bind(
                registry, id, &zwp_pointer_constraints_v1_interface, 1);
    } else if (strcmp(interface, "zwp_relative_pointer_manager_v1") == 0) {
        d->relative_pointer_manager = (struct zwp_relative_pointer_manager_v1 *)wl_registry_bind(
                registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
    } else if (strcmp(interface, "zwp_idle_inhibit_manager_v1") == 0) {
        d->idle_manager = (struct zwp_idle_inhibit_manager_v1 *)wl_registry_bind(
                registry, id, &zwp_idle_inhibit_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        d->fractional_scale_manager = (struct wp_fractional_scale_manager_v1*)wl_registry_bind(registry, id,
                &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        d->data_device_manager = (struct wl_data_device_manager *)wl_registry_bind(registry, id,
                &wl_data_device_manager_interface, std::min(version,  3U));
        if (d->data_device_manager && d->seat)
            d->data_device = wl_data_device_manager_get_data_device(d->data_device_manager, d->seat);
    } else if (strcmp(interface, "gtk_shell1") == 0) {
        if (version < 6)
            d->supports_cursor_viewport = false;
    } else if (strcmp(interface, "mir_shell_v1") == 0) {
        d->supports_cursor_hw_buffer = false;
    }
}

static void
registry_handle_global_remove(void *data, struct wl_registry *, uint32_t name)
{
    struct display *d = (struct display*)data;
    d->outputs->remove(name);
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

static int
str_starts_with(const char *a, const char *b)
{
    return strncmp(a, b, strlen(b));
}

GrallocType
get_gralloc_type(const char *gralloc)
{
    if (strcmp(gralloc, "default") == 0) {
        return GrallocType::GRALLOC_DEFAULT;
    } else if (strcmp(gralloc, "gbm") == 0) {
        return GrallocType::GRALLOC_GBM;
    } else if (str_starts_with(gralloc, "minigbm") == 0) {
        return GrallocType::GRALLOC_CROS;
    } else {
        return GrallocType::GRALLOC_ANDROID;
    }
}

static void
wayland_log_handler (const char *format, va_list args)
{
    LOG_PRI_VA (ANDROID_LOG_ERROR, "wayland-hwc", format, args);
}

window *open_windows::add(waydroid_hwc_composer_device_1 *pdev, const std::string& key, const std::string& aid, const std::string& tid, hwc_color_t color) {
    window *window = nullptr;
    update([&](){
        auto res = windows.emplace(
            key,
            window::create(pdev->display, pdev->should_compose, aid, tid, color)
        );
        assert(res.second);
        window = res.first->second.get();
    });
    return window;
}

void open_windows::add(const std::string& key, std::unique_ptr<window> window) {
    update([&](){
        auto res = windows.emplace(
            key,
            std::move(window)
        );
        assert(res.second); (void)res;
    });
}

void open_windows::clear() {
    windows.clear();
    property_set("waydroid.open_windows", "0");
}

void open_windows::erase(const_iterator pos) {
    update([&](){
        windows.erase(pos);
    });
}

void open_windows::erase(const key_type& key) {
    if (windows.erase(key) > 0) {
        std::string windows_size_str = std::to_string(windows.size());
        property_set("waydroid.open_windows", windows_size_str.c_str());
    }
}

static void* hwc_wayland_thread(void* data) {
    auto* display = static_cast<struct wl_display*>(data);
    int ret = 0;

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    while (ret != -1)
        ret = wl_display_dispatch(display);

    /* Report why the connection died, not strerror(-1): for a protocol
     * error name the interface/object the compositor rejected, otherwise
     * the socket errno. Also persist it under /data (bind-mounted from
     * the host) so it survives the container for post-mortem debugging. */
    int err = wl_display_get_error(display);
    char msg[256];
    if (err == EPROTO) {
        const struct wl_interface *intf = NULL;
        uint32_t id = 0;
        uint32_t code = wl_display_get_protocol_error(display, &intf, &id);
        snprintf(msg, sizeof(msg), "protocol error: interface %s, object id %u, code %u",
                 intf ? intf->name : "?", id, code);
    } else {
        snprintf(msg, sizeof(msg), "socket error: %s", strerror(err));
    }
    ALOGE("*** %s: Wayland client was disconnected: %s", __PRETTY_FUNCTION__, msg);

    FILE *f = fopen("/data/waydroid_hwc_wayland_error.txt", "a");
    if (f) {
        struct timespec rt;
        clock_gettime(CLOCK_REALTIME, &rt);
        fprintf(f, "[%lld] %s\n", (long long)rt.tv_sec, msg);
        fclose(f);
    }

    abort();
}

std::shared_ptr<const dmabuf::format_set> display::importable_formats() const {
    std::lock_guard lock(formats_mutex);
    return formats ? formats : std::make_shared<const dmabuf::format_set>();
}

void display::set_importable_formats(dmabuf::format_set importable) {
    auto shared = std::make_shared<const dmabuf::format_set>(std::move(importable));
    std::lock_guard lock(formats_mutex);
    formats = std::move(shared);
}

void display::request_frame() const {
    if (procs && procs->invalidate)
        procs->invalidate(procs);
}

static int
atlas_dimension(const char *property_name)
{
    const int32_t value = property_get_int32(property_name, default_atlas_size);
    return value > 0 ? value : default_atlas_size;
}

/* With a fixed atlas, Android's scale is the highest preferred scale of the
 * outputs, read once at start: no window has to be shown to learn it. */
static void
calibrate_fixed_atlas(struct display *display)
{
    if (display->xdg_output_manager)
        wl_display_roundtrip(display->display);
    display->scale = display->outputs->highest_scale();
    display->atlas_width = atlas_dimension("persist.waydroid.width");
    display->atlas_height = atlas_dimension("persist.waydroid.height");
    finished_calibrating(display);
    ALOGI("fixed %dx%d atlas at scale %.3f", display->atlas_width, display->atlas_height, display->scale);
}

static void
create_agent_link(struct display *display)
{
    char bundle[PROPERTY_VALUE_MAX];
    property_get("ro.crownos.bundle", bundle, "");
    agent::link_config config;
    config.socket_path = agent_socket;
    config.bundle = bundle;
    config.safe_mode = [] { return property_get_bool("persist.crownos.safe_mode", false); };
    display->bridge = std::make_unique<agent_bridge>(display);
    display->agent_link = std::make_unique<agent::link>(std::move(config), *display->bridge);
}

struct display *
create_display(const char *gralloc)
{
    struct display *display = new struct display();
    if (display == NULL) {
        ALOGE("out of memory");
        return NULL;
    }
    char bundle[PROPERTY_VALUE_MAX];
    display->fixed_atlas = property_get("ro.crownos.bundle", bundle, nullptr) > 0;
    display->outputs = std::make_unique<output_registry>();
    create_agent_link(display);
    wl_log_set_handler_client(wayland_log_handler);
    display->system_version = property_get_int32("ro.system.build.version.sdk", 0);
    display->gtype = get_gralloc_type(gralloc);
    display->refresh = 0;
    display->isMaximized = true;
    display->supports_cursor_viewport = true;
    display->supports_cursor_hw_buffer = property_get_bool("persist.waydroid.cursor_force_shm", false);
    display->display = wl_display_connect(NULL);
    ALOGI("WAYLAND_DISPLAY: %s", getenv("WAYLAND_DISPLAY"));
    ALOGI("XDG_RUNTIME_DIR: %s", getenv("XDG_RUNTIME_DIR"));
    if (!display->display) {
        ALOGE("Couldn't open Wayland display.");
        return NULL;
    }
    sem_init(&display->egl_go, 0, 0);
    sem_init(&display->egl_done, 0, 0);

    umask(0);
    mkdir("/dev/input", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    chown("/dev/input", 1000, 1000);

    display->registry = wl_display_get_registry(display->display);
    wl_registry_add_listener(display->registry,
                 &registry_listener, display);
    wl_display_roundtrip(display->display);
    if (display->dmabuf_version >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) {
        wl_display_roundtrip(display->display);
    } else {
        dmabuf::publish_modifier_properties(display->legacy_formats.preference);
        display->set_importable_formats(display->legacy_formats.formats);
    }
    if (display->fixed_atlas)
        calibrate_fixed_atlas(display);

    char render_node[PROPERTY_VALUE_MAX];
    property_get("gralloc.gbm.device", render_node, "");
    display->sync = std::make_unique<fence::frame_sync>(display->syncobj_manager, render_node);

    if (pthread_create(&display->wayland_thread, nullptr, hwc_wayland_thread, display->display) != 0) {
        ALOGE("Couldn't create wayland thread");
        wl_display_disconnect(display->display);
        sem_destroy(&display->egl_go);
        sem_destroy(&display->egl_done);
        return nullptr;
    }

    display->task = IWaydroidTask::getService();
    return display;
}

void
start_task_windows(struct display *display)
{
    if (display->fixed_atlas)
        display->agent_link->start();
}

void
destroy_display(struct display *display)
{
    display->agent_link->stop();
    pthread_kill(display->wayland_thread, SIGTERM);
    pthread_join(display->wayland_thread, nullptr);

    {
        std::scoped_lock lock(display->windowsMutex);
        display->windows.clear();
    }
    display->buffer_map.clear();
    display->default_feedback.reset();
    destroy_pointer_gestures(display);
    display->outputs->destroy_all();

    if (display->wm_base)
        xdg_wm_base_destroy(display->wm_base);

    if (display->shell)
        wl_shell_destroy(display->shell);

    if (display->compositor)
        wl_compositor_destroy(display->compositor);

    if (display->tablet_manager) {
        for (struct zwp_tablet_tool_v2 *t : display->tablet_tools) {
            zwp_tablet_tool_v2_destroy(t);
        }
        zwp_tablet_seat_v2_destroy(display->tablet_seat);
        zwp_tablet_manager_v2_destroy(display->tablet_manager);
    }

    if (display->relative_pointer_manager)
        zwp_relative_pointer_manager_v1_destroy(display->relative_pointer_manager);

    if (display->pointer_constraints)
        zwp_pointer_constraints_v1_destroy(display->pointer_constraints);

    if (display->xdg_output_manager)
        zxdg_output_manager_v1_destroy(display->xdg_output_manager);
    if (display->decoration_manager)
        zxdg_decoration_manager_v1_destroy(display->decoration_manager);
    if (display->activation)
        xdg_activation_v1_destroy(display->activation);
    if (display->android_shell)
        crownos_android_shell_manager_v1_destroy(display->android_shell);
    display->sync.reset();
    if (display->syncobj_manager)
        wp_linux_drm_syncobj_manager_v1_destroy(display->syncobj_manager);

    wl_registry_destroy(display->registry);
    wl_display_flush(display->display);
    wl_display_disconnect(display->display);

    delete display;
}
