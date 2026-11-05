/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright © 2010 Intel Corporation
 * Copyright © 2014 Collabora Ltd.
 * Copyright © 2021 Waydroid Project.
 * Copyright © 2026 CrownOS
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
 *
 * Wayland seat input turned into the evdev streams Waydroid's EventHub reads
 * from /dev/input/wl_*_events, in Android display coordinates.
 */

#include "input/wayland_input.h"

#include "agent/link.h"
#include "hwcomposer.h"
#include "input/keyboard_layout.h"
#include "input/pen.h"

#include <cerrno>
#include <cmath>
#include <cstring>

#include <fcntl.h>
#include <linux/input.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cutils/properties.h>
#include <log/log.h>
#include <xkbcommon/xkbcommon.h>

#include "pointer-gestures-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "tablet-unstable-v2-client-protocol.h"

/// The last two multi-touch slots belong to the touchpad pinch emulation.
constexpr int first_pinch_slot = MAX_TOUCHPOINTS - 2;
constexpr int touch_pressure = 50;

static void start_pinch_gestures(struct display *display);
static void stop_pinch_gestures(struct display *display);

static void
create_touch_pipe(struct display *d)
{
    d->input_fd[INPUT_TOUCH] = -1;
    mkfifo(INPUT_PIPE_NAME[INPUT_TOUCH], S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    chown(INPUT_PIPE_NAME[INPUT_TOUCH], 1000, 1000);
    for (int i = 0; i < MAX_TOUCHPOINTS; i++)
        d->touch_id[i] = -1;
}

static int
ensure_pipe(struct display* display, int input_type)
{
    if (display->input_fd[input_type] == -1) {
        display->input_fd[input_type] = open(INPUT_PIPE_NAME[input_type], O_WRONLY | O_NONBLOCK);
        if (display->input_fd[input_type] == -1) {
            ALOGE("Failed to open pipe to InputFlinger: %s", strerror(errno));
            return -1;
        }
    }
    return 0;
}

#define ADD_EVENT(type_, code_, value_)            \
    event[n].time.tv_sec = rt.tv_sec;              \
    event[n].time.tv_usec = rt.tv_nsec / 1000;     \
    event[n].type = type_;                         \
    event[n].code = code_;                         \
    event[n].value = value_;                       \
    n++;

void
send_key_event(display *data, uint32_t key, wl_keyboard_key_state state)
{
    struct display* display = (struct display*)data;
    struct input_event event[1];
    struct timespec rt;
    unsigned int res, n = 0;

    if (key >= display->keysDown.size()) {
        ALOGE("Invalid key: %u", key);
        return;
    }

    if (ensure_pipe(display, INPUT_KEYBOARD))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    ADD_EVENT(EV_KEY, key, state);

    res = write(display->input_fd[INPUT_KEYBOARD], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    display->keysDown[(uint8_t)key] = state;
}

static void
keyboard_handle_keymap(void *, struct wl_keyboard *,
               uint32_t format, int fd, uint32_t size)
{
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        char* keymap_shm = (char*)mmap(NULL, size - 1, PROT_READ, MAP_PRIVATE, fd, 0);
        if (keymap_shm == MAP_FAILED) {
            close(fd);
            return;
        }
        auto xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
        auto xkb_keymap = xkb_keymap_new_from_buffer(xkb_ctx, keymap_shm, size - 1,
                            XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        const char* namep = xkb_keymap ? xkb_keymap_layout_get_name(xkb_keymap, 0) : nullptr;
        if (namep) {
            if (const auto layout = input::android_keyboard_layout(namep)) {
                property_set("waydroid.keyboard_layout", std::string(*layout).c_str());
            } else {
                ALOGI("no Android keyboard layout for xkb layout '%s'", namep);
            }
        }

        xkb_keymap_unref(xkb_keymap);
        xkb_context_unref(xkb_ctx);
        munmap(keymap_shm, size - 1);
    }
    close(fd);
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *,
                      uint32_t serial, struct wl_surface *surface,
                      struct wl_array *)
{
    if (!surface) {
        // The surface this enter event corresponds to was already destroyed
        return;
    }

    struct display *display = (struct display *)data;
    display->keyboard_enter_serial = serial;

    // Only the task id is read under windowsMutex; setFocusedTask() is made
    // after it is released. It is a synchronous call that ends in
    // ActivityTaskManager.setFocusedTask(), which waits for the window
    // manager lock - and system_server can be holding that while it waits on
    // SurfaceFlinger, which waits on this HWC in hwc_set() for windowsMutex.
    // Holding the mutex across the call closed that cycle and deadlocked the
    // whole display stack until the watchdog killed system_server.
    int taskID = -1;
    bool task_window = false;
    {
        std::scoped_lock lock(display->windowsMutex);
        auto window = reinterpret_cast<struct window *>(wl_surface_get_user_data(surface));
        if (!window)
            return; // Window was destroyed in hwc_set

        if (window->taskID != "none" && window->taskID != "0")
            taskID = stoi(window->taskID);
        task_window = window->task != nullptr;
    }
    if (taskID < 0)
        return;

    display->focused_task = static_cast<uint32_t>(taskID);
    if (task_window && display->agent_link->send(agent_wire::Focus{static_cast<uint32_t>(taskID)}))
        return;
    if (display->task != nullptr)
        display->task->setFocusedTask(taskID);
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *,
                      uint32_t, struct wl_surface *)
{
    struct display *display = (struct display *)data;
    display->focused_task = 0;

    for (size_t i = 0; i < display->keysDown.size(); i++) {
        if (display->keysDown[i] == WL_KEYBOARD_KEY_STATE_PRESSED) {
            send_key_event(display, i, WL_KEYBOARD_KEY_STATE_RELEASED);
        }
    }
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *,
                    uint32_t, uint32_t, uint32_t key,
                    uint32_t state)
{
    if (key == KEY_POWER)
        return;
    send_key_event((struct display*)data, key, (enum wl_keyboard_key_state)state);
}

static void
keyboard_handle_modifiers(void *, struct wl_keyboard *, uint32_t, uint32_t,
                          uint32_t, uint32_t, uint32_t)
{
}

static void
keyboard_handle_repeat_info(void *, struct wl_keyboard *,
                            int32_t, int32_t)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers,
    keyboard_handle_repeat_info,
};

static void
pointer_handle_enter(void *data, struct wl_pointer *,
                     uint32_t serial, struct wl_surface *surface,
                     wl_fixed_t, wl_fixed_t)
{
    if (!surface) {
        // The surface this enter event corresponds to was already destroyed
        return;
    }

    struct display *display = (struct display *)data;
    display->pointer_surface = surface;
    display->pointer_enter_serial = serial;
    display->cursor_handler->on_cursor_enter(display);
}

static void
pointer_handle_leave(void *data, struct wl_pointer *,
                     uint32_t, struct wl_surface *)
{
    struct display *display = (struct display *)data;
    display->pointer_surface = NULL;
}

static void
pointer_handle_motion(void *data, struct wl_pointer *,
                      uint32_t, wl_fixed_t sx, wl_fixed_t sy)
{
    struct display* display = (struct display*)data;
    struct input_event event[5];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    if (!display->pointer_surface)
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    display->input_map.to_android(display->pointer_surface, wl_fixed_to_double(sx), wl_fixed_to_double(sy),
                                  display->scale, x, y);

    ADD_EVENT(EV_ABS, ABS_X, x);
    ADD_EVENT(EV_ABS, ABS_Y, y);
    ADD_EVENT(EV_REL, REL_X, x - display->ptrPrvX);
    ADD_EVENT(EV_REL, REL_Y, y - display->ptrPrvY);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    display->ptrPrvX = x;
    display->ptrPrvY = y;

    res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

void
handle_relative_motion(void *data, struct zwp_relative_pointer_v1*,
        uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t)
{
    struct display *display = (struct display *)data;
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    static double acc_x = 0;
    static double acc_y = 0;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    task::input_mapping mapping;
    mapping.px_per_logical_x = display->scale;
    mapping.px_per_logical_y = display->scale;
    if (display->pointer_surface)
        display->input_map.find(display->pointer_surface, mapping);
    acc_x += wl_fixed_to_double(dx) * mapping.px_per_logical_x;
    acc_y += wl_fixed_to_double(dy) * mapping.px_per_logical_y;

    if (std::abs(acc_x) < 1 && std::abs(acc_y) < 1)
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_REL, REL_X, (int)acc_x);
    ADD_EVENT(EV_REL, REL_Y, (int)acc_y);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    acc_x -= (int)acc_x;
    acc_y -= (int)acc_y;

    res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
pointer_handle_button(void *data, struct wl_pointer *,
                      uint32_t, uint32_t, uint32_t button,
                      uint32_t state)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    if (!display->pointer_surface)
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    ADD_EVENT(EV_KEY, button, state);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static input::scroll_axis_id
scroll_axis_of(uint32_t axis)
{
    return axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? input::scroll_axis_id::vertical
                                                   : input::scroll_axis_id::horizontal;
}

static void
send_scroll(struct display *display)
{
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    const auto detents = display->scroll.finish();
    if ((detents[0] == 0 && detents[1] == 0) || !display->pointer_surface)
        return;
    if (ensure_pipe(display, INPUT_POINTER))
        return;

    /* Wayland scrolls down or right for positive values; evdev's wheel
     * scrolls up for positive values and its horizontal wheel right */
    const int direction = display->reverseScroll ? -1 : 1;
    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in scroll clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    if (detents[0]) {
        ADD_EVENT(EV_REL, REL_WHEEL, -direction * detents[0]);
    }
    if (detents[1]) {
        ADD_EVENT(EV_REL, REL_HWHEEL, direction * detents[1]);
    }
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_POINTER], &event, n * sizeof(event[0]));
    if (res < n * sizeof(event[0]))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static bool
pointer_has_frames(struct display *display)
{
    return display->seat_version >= WL_POINTER_FRAME_SINCE_VERSION;
}

static void
pointer_handle_axis(void *data, struct wl_pointer *,
                    uint32_t, uint32_t axis, wl_fixed_t value)
{
    struct display* display = (struct display*)data;
    display->scroll.value(scroll_axis_of(axis), wl_fixed_to_double(value));
    if (!pointer_has_frames(display))
        send_scroll(display);
}

static void
pointer_handle_axis_source(void *data, struct wl_pointer *, uint32_t source)
{
    struct display* display = (struct display*)data;
    display->scroll.source_is_wheel(source == WL_POINTER_AXIS_SOURCE_WHEEL ||
                                    source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT);
}

static void
pointer_handle_axis_stop(void *data, struct wl_pointer *, uint32_t, uint32_t axis)
{
    struct display* display = (struct display*)data;
    display->scroll.stop(scroll_axis_of(axis));
}

static void
pointer_handle_axis_discrete(void *data, struct wl_pointer *, uint32_t axis, int32_t discrete)
{
    struct display* display = (struct display*)data;
    display->scroll.discrete(scroll_axis_of(axis), discrete);
}

static void
pointer_handle_axis_value120(void *data, struct wl_pointer *, uint32_t axis, int32_t value120)
{
    struct display* display = (struct display*)data;
    display->scroll.value120(scroll_axis_of(axis), value120);
}

static void
pointer_handle_axis_relative_direction(void *, struct wl_pointer *, uint32_t, uint32_t)
{
}

static void
pointer_handle_frame(void *data, struct wl_pointer *)
{
    send_scroll(static_cast<struct display *>(data));
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
    pointer_handle_frame,
    pointer_handle_axis_source,
    pointer_handle_axis_stop,
    pointer_handle_axis_discrete,
    pointer_handle_axis_value120,
    pointer_handle_axis_relative_direction,
};


static int
get_touch_id(struct display *display, int id)
{
    for (int i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == id)
            return i;
    }
    return -1;
}

static int
create_touch_id(struct display *display, int id) {
    int i = get_touch_id(display, id);
    if (i != -1)
        return i;

    for (i = 0; i < first_pinch_slot; i++) {
        if (display->touch_id[i] == -1) {
            display->touch_id[i] = id;
            return i;
        }
    }

    return -1;
}


static int
flush_touch_id(struct display *display, int id)
{
    for (int i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == id) {
            display->touch_id[i] = -1;
            return i;
        }
    }
    return -1;
}

static void
touch_handle_down(void *data, struct wl_touch *,
          uint32_t, uint32_t, struct wl_surface *surface,
          int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    if (!surface) {
        // The surface this down event corresponds to was already destroyed
        return;
    }

    struct display* display = (struct display*)data;
    struct input_event event[6];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    int touch_id = create_touch_id(display, id);
    if (touch_id < 0)
        return;
    display->touch_surfaces[id] = surface;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }
    display->input_map.to_android(surface, wl_fixed_to_double(x_w), wl_fixed_to_double(y_w), display->scale, x, y);

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, touch_id);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, touch_id);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, x);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, y);
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
touch_handle_up(void *data, struct wl_touch *,
        uint32_t, uint32_t, int32_t id)
{
    struct display* display = (struct display*)data;
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    int touch_id = flush_touch_id(display, id);
    // Might not exist if we discarded the down event due to a NULL surface
    if (touch_id != -1) {
        if (ensure_pipe(display, INPUT_TOUCH))
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }
        display->touch_surfaces[id] = NULL;

        ADD_EVENT(EV_ABS, ABS_MT_SLOT, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

static void
touch_handle_motion(void *data, struct wl_touch *,
            uint32_t, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    struct display* display = (struct display*)data;
    struct input_event event[6];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    int touch_id = get_touch_id(display, id);
    // Might not exist if we discarded the down event due to a NULL surface
    if (touch_id != -1) {
        if (ensure_pipe(display, INPUT_TOUCH))
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }
        display->input_map.to_android(display->touch_surfaces[id], wl_fixed_to_double(x_w),
                                      wl_fixed_to_double(y_w), display->scale, x, y);

        ADD_EVENT(EV_ABS, ABS_MT_SLOT, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, x);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, y);
        ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

static void
touch_handle_frame(void *, struct wl_touch *)
{
}

static void
touch_handle_cancel(void *data, struct wl_touch *)
{
    struct display* display = (struct display*)data;
    struct input_event event[6];
    struct timespec rt;
    unsigned int res, n;
    int i, id;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    // Cancel all touch points.
    for (i = 0; i < first_pinch_slot; i++) {
        if (display->touch_id[i] != -1) {
            id = display->touch_id[i];
            display->touch_id[i] = -1;
            display->touch_surfaces[id] = NULL;

            n = 0;
            // Turn finger into palm.
            ADD_EVENT(EV_ABS, ABS_MT_SLOT, i);
            ADD_EVENT(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM);
            ADD_EVENT(EV_SYN, SYN_REPORT, 0);
            // Lift off.
            ADD_EVENT(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
            ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
            ADD_EVENT(EV_SYN, SYN_REPORT, 0);

            res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
            if (res < sizeof(event))
                ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
        }
    }
}

static void
touch_handle_shape(void *data, struct wl_touch *, int32_t id, wl_fixed_t major, wl_fixed_t minor)
{
    struct display* display = (struct display*)data;
    struct input_event event[5];
    struct timespec rt;
    unsigned int res, n = 0;

    int touch_id = get_touch_id(display, id);
    // Might not exist if we discarded the down event due to a NULL surface
    if (touch_id != -1) {
        if (ensure_pipe(display, INPUT_TOUCH))
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
           ALOGE("%s:%d error in touch clock_gettime: %s",
                __FILE__, __LINE__, strerror(errno));
        }
        ADD_EVENT(EV_ABS, ABS_MT_SLOT, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_TOUCH_MAJOR, wl_fixed_to_int(major));
        ADD_EVENT(EV_ABS, ABS_MT_TOUCH_MINOR, wl_fixed_to_int(minor));
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

static void
touch_handle_orientation(void *, struct wl_touch *, int32_t, wl_fixed_t)
{
}

static const struct wl_touch_listener touch_listener = {
    touch_handle_down,
    touch_handle_up,
    touch_handle_motion,
    touch_handle_frame,
    touch_handle_cancel,
    touch_handle_shape,
    touch_handle_orientation,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t wl_caps)
{
    struct display *d = (struct display*)data;
    enum wl_seat_capability caps = (enum wl_seat_capability) wl_caps;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) {
        d->pointer = wl_seat_get_pointer(seat);
        d->input_fd[INPUT_POINTER] = -1;
        d->ptrPrvX = 0;
        d->ptrPrvY = 0;
        d->reverseScroll = property_get_bool("persist.waydroid.reverse_scrolling", false);
        mkfifo(INPUT_PIPE_NAME[INPUT_POINTER], S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
        chown(INPUT_PIPE_NAME[INPUT_POINTER], 1000, 1000);
        wl_pointer_add_listener(d->pointer, &pointer_listener, d);
        start_pinch_gestures(d);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->pointer) {
        stop_pinch_gestures(d);
        remove(INPUT_PIPE_NAME[INPUT_POINTER]);
        wl_pointer_destroy(d->pointer);
        d->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
        d->keyboard = wl_seat_get_keyboard(seat);
        d->input_fd[INPUT_KEYBOARD] = -1;
        mkfifo(INPUT_PIPE_NAME[INPUT_KEYBOARD], S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
        chown(INPUT_PIPE_NAME[INPUT_KEYBOARD], 1000, 1000);
        wl_keyboard_add_listener(d->keyboard, &keyboard_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard) {
        remove(INPUT_PIPE_NAME[INPUT_KEYBOARD]);
        wl_keyboard_destroy(d->keyboard);
        d->keyboard = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->touch) {
        d->touch = wl_seat_get_touch(seat);
        d->input_fd[INPUT_TOUCH] = -1;
        create_touch_pipe(d);
        wl_touch_set_user_data(d->touch, d);
        wl_touch_add_listener(d->touch, &touch_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && d->touch) {
        if (!d->gestures)
            remove(INPUT_PIPE_NAME[INPUT_TOUCH]);
        wl_touch_destroy(d->touch);
        d->touch = NULL;
    }
}

static void
seat_handle_name(void *, struct wl_seat *, const char *)
{
}

const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
    seat_handle_name,
};

static void tablet_seat_handle_add_tablet(void *, struct zwp_tablet_seat_v2 *,
                                          struct zwp_tablet_v2 *)
{
}

static void tablet_seat_handle_add_pad(void*, struct zwp_tablet_seat_v2 *,
                                       struct zwp_tablet_pad_v2 *)
{
}

static void
tablet_tool_receive_type(void *data, struct zwp_tablet_tool_v2 *tool,
                         uint32_t type)
{
    struct display* display = (struct display*)data;
    uint16_t evt_code;
    switch(type) {
        case ZWP_TABLET_TOOL_V2_TYPE_PEN:
            evt_code = BTN_TOOL_PEN;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_ERASER:
            evt_code = BTN_TOOL_RUBBER;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_BRUSH:
            evt_code = BTN_TOOL_BRUSH;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_PENCIL:
            evt_code = BTN_TOOL_PENCIL;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_AIRBRUSH:
            evt_code = BTN_TOOL_AIRBRUSH;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_FINGER:
            evt_code = BTN_TOOL_FINGER;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_MOUSE:
            evt_code = BTN_TOOL_MOUSE;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_LENS:
            evt_code = BTN_TOOL_LENS;
            break;
        default:
            evt_code = BTN_DIGI;
    }
    display->tablet_tools_evt[tool] = evt_code;
}

static void
tablet_tool_receive_hardware_serial(void *, struct zwp_tablet_tool_v2 *,
                                    uint32_t, uint32_t)
{
}

static void
tablet_tool_receive_hardware_id_wacom(void *, struct zwp_tablet_tool_v2 *,
                                      uint32_t, uint32_t)
{
}

static void
tablet_tool_receive_capability(void *, struct zwp_tablet_tool_v2 *, uint32_t)
{
}

static void
tablet_tool_receive_done(void *, struct zwp_tablet_tool_v2 *)
{
}

static void
tablet_tool_receive_removed(void *, struct zwp_tablet_tool_v2 *)
{
}

static void
tablet_tool_proximity_in(void *data, struct zwp_tablet_tool_v2 *tool,
                         uint32_t, struct zwp_tablet_v2 *,
                         struct wl_surface *surface)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    display->tablet_surface = surface;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, display->tablet_tools_evt[tool], 1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_proximity_out(void *data, struct zwp_tablet_tool_v2 *tool)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    display->tablet_surface = NULL;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, display->tablet_tools_evt[tool], 0);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_down(void *data, struct zwp_tablet_tool_v2 *, uint32_t)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, BTN_TOUCH, 1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_up(void *data, struct zwp_tablet_tool_v2 *)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, BTN_TOUCH, 0);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_motion(void *data, struct zwp_tablet_tool_v2 *,
                   wl_fixed_t x_w, wl_fixed_t y_w)
{
    struct display* display = (struct display*)data;
    struct input_event event[3];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    if (display->tablet_surface) {
        if (ensure_pipe(display, INPUT_TABLET))
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }
        display->input_map.to_android(display->tablet_surface, wl_fixed_to_double(x_w),
                                      wl_fixed_to_double(y_w), display->scale, x, y);

        ADD_EVENT(EV_ABS, ABS_X, x);
        ADD_EVENT(EV_ABS, ABS_Y, y);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

static void
tablet_tool_pressure(void *data, struct zwp_tablet_tool_v2 *,
                     uint32_t pressure)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_PRESSURE, input::android_pressure(pressure));
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_distance(void *data, struct zwp_tablet_tool_v2 *,
                     uint32_t distance_raw)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_DISTANCE, distance_raw);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_tilt(void *data, struct zwp_tablet_tool_v2 *,
                 wl_fixed_t tilt_x, wl_fixed_t tilt_y)
{
    struct display* display = (struct display*)data;
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_TILT_X, wl_fixed_to_int(tilt_x));
    ADD_EVENT(EV_ABS, ABS_TILT_Y, wl_fixed_to_int(tilt_y));
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_rotation(void *, struct zwp_tablet_tool_v2 *, wl_fixed_t)
{
}

static void
tablet_tool_slider(void *, struct zwp_tablet_tool_v2 *, int32_t)
{
}

static void
tablet_tool_wheel(void *, struct zwp_tablet_tool_v2 *, wl_fixed_t, int32_t)
{
}

static void
tablet_tool_button_state(void *data, struct zwp_tablet_tool_v2 *,
                         uint32_t, uint32_t button, uint32_t state)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, button, state);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_frame(void *, struct zwp_tablet_tool_v2 *, uint32_t)
{
}

static const struct zwp_tablet_tool_v2_listener tablet_tool_listener = {
    .type = tablet_tool_receive_type,
    .hardware_serial = tablet_tool_receive_hardware_serial,
    .hardware_id_wacom = tablet_tool_receive_hardware_id_wacom,
    .capability = tablet_tool_receive_capability,
    .done = tablet_tool_receive_done,
    .removed = tablet_tool_receive_removed,
    .proximity_in = tablet_tool_proximity_in,
    .proximity_out = tablet_tool_proximity_out,
    .down = tablet_tool_down,
    .up = tablet_tool_up,
    .motion = tablet_tool_motion,
    .pressure = tablet_tool_pressure,
    .distance = tablet_tool_distance,
    .tilt = tablet_tool_tilt,
    .rotation = tablet_tool_rotation,
    .slider = tablet_tool_slider,
    .wheel = tablet_tool_wheel,
    .button = tablet_tool_button_state,
    .frame = tablet_tool_frame
};

static void tablet_seat_handle_add_tool(void *data, struct zwp_tablet_seat_v2 *,
                            struct zwp_tablet_tool_v2 *tool)
{
    if (!tool) {
        return;
    }

    struct display *d = (struct display*)data;
    d->tablet_tools.push_back(tool);
    zwp_tablet_tool_v2_add_listener(tool, &tablet_tool_listener, d);
    ALOGI("Added tablet tool");
}

static const struct zwp_tablet_seat_v2_listener tablet_seat_listener = {
    tablet_seat_handle_add_tablet,
    tablet_seat_handle_add_tool,
    tablet_seat_handle_add_pad
};

void add_tablet_seat(struct display *d) {
    d->input_fd[INPUT_TABLET] = -1;
    mkfifo(INPUT_PIPE_NAME[INPUT_TABLET], S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    chown(INPUT_PIPE_NAME[INPUT_TABLET], 1000, 1000);

    d->tablet_seat = zwp_tablet_manager_v2_get_tablet_seat(d->tablet_manager, d->seat);
    zwp_tablet_seat_v2_add_listener(d->tablet_seat, &tablet_seat_listener, d);
}

static void
write_pinch_fingers(struct display *display, const std::array<input::finger, 2> *fingers)
{
    struct input_event event[11];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;
    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in pinch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    for (int finger = 0; finger < 2; finger++) {
        const int slot = first_pinch_slot + finger;
        ADD_EVENT(EV_ABS, ABS_MT_SLOT, slot);
        if (fingers) {
            ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, slot);
            ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, (*fingers)[finger].x);
            ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, (*fingers)[finger].y);
            ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, touch_pressure);
        } else {
            ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
        }
    }
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TOUCH], &event, n * sizeof(event[0]));
    if (res < n * sizeof(event[0]))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
pinch_begin(void *data, struct zwp_pointer_gesture_pinch_v1 *, uint32_t, uint32_t,
            struct wl_surface *surface, uint32_t fingers)
{
    struct display *display = static_cast<struct display *>(data);
    pointer_gestures &gestures = *display->gestures;
    if (!surface || fingers < 2)
        return;

    task::input_mapping mapping;
    if (!display->input_map.find(surface, mapping))
        return;
    task::px_rect bounds = mapping.bounds;
    if (bounds.empty())
        bounds = {0, 0, int(display->width * display->scale), int(display->height * display->scale)};
    gestures.px_per_logical_x = mapping.px_per_logical_x;
    gestures.px_per_logical_y = mapping.px_per_logical_y;
    gestures.active = true;
    const auto positions = gestures.emulation.begin(display->ptrPrvX, display->ptrPrvY, bounds);
    write_pinch_fingers(display, &positions);
}

static void
pinch_update(void *data, struct zwp_pointer_gesture_pinch_v1 *, uint32_t, wl_fixed_t dx, wl_fixed_t dy,
             wl_fixed_t scale, wl_fixed_t rotation)
{
    struct display *display = static_cast<struct display *>(data);
    pointer_gestures &gestures = *display->gestures;
    if (!gestures.active)
        return;
    const auto positions = gestures.emulation.update(wl_fixed_to_double(dx) * gestures.px_per_logical_x,
                                                     wl_fixed_to_double(dy) * gestures.px_per_logical_y,
                                                     wl_fixed_to_double(scale), wl_fixed_to_double(rotation));
    write_pinch_fingers(display, &positions);
}

static void
pinch_end(void *data, struct zwp_pointer_gesture_pinch_v1 *, uint32_t, uint32_t, int32_t)
{
    struct display *display = static_cast<struct display *>(data);
    pointer_gestures &gestures = *display->gestures;
    if (!gestures.active)
        return;
    gestures.active = false;
    write_pinch_fingers(display, nullptr);
}

static const struct zwp_pointer_gesture_pinch_v1_listener pinch_listener = {
    pinch_begin,
    pinch_update,
    pinch_end,
};

static void
start_pinch_gestures(struct display *d)
{
    if (!d->pointer_gestures_manager || !d->pointer || d->gestures)
        return;
    d->gestures = std::make_unique<pointer_gestures>();
    d->gestures->pinch = zwp_pointer_gestures_v1_get_pinch_gesture(d->pointer_gestures_manager, d->pointer);
    zwp_pointer_gesture_pinch_v1_add_listener(d->gestures->pinch, &pinch_listener, d);
    if (!d->touch)
        create_touch_pipe(d);
}

static void
stop_pinch_gestures(struct display *d)
{
    if (!d->gestures)
        return;
    if (d->gestures->active)
        write_pinch_fingers(d, nullptr);
    zwp_pointer_gesture_pinch_v1_destroy(d->gestures->pinch);
    d->gestures.reset();
    if (!d->touch)
        remove(INPUT_PIPE_NAME[INPUT_TOUCH]);
}

void
bind_pointer_gestures(struct display *d, struct wl_registry *registry, uint32_t id, uint32_t version)
{
    d->pointer_gestures_manager = static_cast<struct zwp_pointer_gestures_v1 *>(
        wl_registry_bind(registry, id, &zwp_pointer_gestures_v1_interface, std::min(version, 1U)));
    start_pinch_gestures(d);
}

void
destroy_pointer_gestures(struct display *d)
{
    stop_pinch_gestures(d);
    if (d->pointer_gestures_manager)
        zwp_pointer_gestures_v1_destroy(d->pointer_gestures_manager);
}
