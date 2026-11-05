/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "dmabuf/format_set.h"

#include <functional>

struct wl_array;
struct zwp_linux_dmabuf_feedback_v1;

namespace dmabuf {

/// Listens to one zwp_linux_dmabuf_feedback_v1 (the default feedback or a
/// surface's) and reports every complete feedback on the Wayland thread.
class feedback_reader {
  public:
    using done_callback = std::function<void(feedback)>;

    /// Takes `proxy`.
    feedback_reader(zwp_linux_dmabuf_feedback_v1 *proxy, done_callback on_done);
    ~feedback_reader();

    feedback_reader(const feedback_reader &) = delete;
    feedback_reader &operator=(const feedback_reader &) = delete;

  private:
    static void handle_done(void *data, zwp_linux_dmabuf_feedback_v1 *);
    static void handle_format_table(void *data, zwp_linux_dmabuf_feedback_v1 *, int32_t fd, uint32_t size);
    static void handle_main_device(void *, zwp_linux_dmabuf_feedback_v1 *, struct wl_array *);
    static void handle_tranche_done(void *, zwp_linux_dmabuf_feedback_v1 *);
    static void handle_tranche_target_device(void *, zwp_linux_dmabuf_feedback_v1 *, struct wl_array *);
    static void handle_tranche_formats(void *data, zwp_linux_dmabuf_feedback_v1 *, struct wl_array *indices);
    static void handle_tranche_flags(void *, zwp_linux_dmabuf_feedback_v1 *, uint32_t);

    zwp_linux_dmabuf_feedback_v1 *proxy;
    done_callback on_done;
    feedback_collector collector;
};

/// Tells minigbm which modifiers to allocate with, in order of preference:
/// waydroid.modifiers.<format>.<n> = <modifier>, both in hex.
void publish_modifier_properties(const std::vector<format_modifier> &preference);

} // namespace dmabuf
