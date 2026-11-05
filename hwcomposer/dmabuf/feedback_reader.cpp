/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "dmabuf/feedback_reader.h"

#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include <cutils/properties.h>
#include <log/log.h>

#include <cstdio>
#include <map>

#include <sys/mman.h>
#include <unistd.h>

namespace dmabuf {

feedback_reader::feedback_reader(zwp_linux_dmabuf_feedback_v1 *proxy, done_callback on_done)
    : proxy(proxy), on_done(std::move(on_done)) {
    static const zwp_linux_dmabuf_feedback_v1_listener listener = {
        handle_done,
        handle_format_table,
        handle_main_device,
        handle_tranche_done,
        handle_tranche_target_device,
        handle_tranche_formats,
        handle_tranche_flags,
    };
    zwp_linux_dmabuf_feedback_v1_add_listener(proxy, &listener, this);
}

feedback_reader::~feedback_reader() {
    zwp_linux_dmabuf_feedback_v1_destroy(proxy);
}

void feedback_reader::handle_done(void *data, zwp_linux_dmabuf_feedback_v1 *) {
    auto *reader = static_cast<feedback_reader *>(data);
    reader->on_done(reader->collector.done());
}

void feedback_reader::handle_format_table(void *data, zwp_linux_dmabuf_feedback_v1 *, int32_t fd, uint32_t size) {
    auto *reader = static_cast<feedback_reader *>(data);
    void *table = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (table == MAP_FAILED) {
        ALOGE("cannot map the dmabuf format table");
        return;
    }
    reader->collector.set_table(static_cast<const uint8_t *>(table), size);
    munmap(table, size);
}

void feedback_reader::handle_main_device(void *, zwp_linux_dmabuf_feedback_v1 *, struct wl_array *) {}

void feedback_reader::handle_tranche_done(void *, zwp_linux_dmabuf_feedback_v1 *) {}

void feedback_reader::handle_tranche_target_device(void *, zwp_linux_dmabuf_feedback_v1 *, struct wl_array *) {}

void feedback_reader::handle_tranche_formats(void *data, zwp_linux_dmabuf_feedback_v1 *, struct wl_array *indices) {
    auto *reader = static_cast<feedback_reader *>(data);
    reader->collector.add_tranche_indices(static_cast<const uint16_t *>(indices->data),
                                          indices->size / sizeof(uint16_t));
}

void feedback_reader::handle_tranche_flags(void *, zwp_linux_dmabuf_feedback_v1 *, uint32_t) {}

void publish_modifier_properties(const std::vector<format_modifier> &preference) {
    std::map<uint32_t, unsigned> published;
    for (const format_modifier &pair : preference) {
        if (pair.modifier == implicit_modifier) {
            continue;
        }
        char name[PROPERTY_KEY_MAX];
        char value[PROPERTY_VALUE_MAX];
        snprintf(name, sizeof(name), "waydroid.modifiers.%x.%u", pair.format, published[pair.format]++);
        snprintf(value, sizeof(value), "%llx", static_cast<unsigned long long>(pair.modifier));
        property_set(name, value);
    }
}

} // namespace dmabuf
