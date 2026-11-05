/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "wayland-hwc.h"

extern const struct wl_seat_listener seat_listener;

void send_key_event(struct display *display, uint32_t key, wl_keyboard_key_state state);
void add_tablet_seat(struct display *display);
/// Touchpad pinches become two-finger touches on the touch stream.
void bind_pointer_gestures(struct display *display, struct wl_registry *registry, uint32_t id, uint32_t version);
void destroy_pointer_gestures(struct display *display);
