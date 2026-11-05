/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "fence/frame_sync.h"

#include "linux-drm-syncobj-v1-client-protocol.h"

#include <log/log.h>
#include <sync/sync.h>

#include <algorithm>

#include <unistd.h>

namespace fence {
namespace {

/// How long an acquire fence that cannot become a timeline point may block.
constexpr int acquire_fallback_wait_ms = 100;
/// Release timelines a surface keeps: a few app buffers and the framebuffer
/// target. The least recently used one goes first.
constexpr size_t release_timelines_per_surface = 8;

uint32_t high_bits(uint64_t value) {
    return static_cast<uint32_t>(value >> 32);
}

uint32_t low_bits(uint64_t value) {
    return static_cast<uint32_t>(value);
}

} // namespace

frame_sync::frame_sync(wp_linux_drm_syncobj_manager_v1 *manager, const std::string &render_node)
    : manager(manager), device(syncobj_device::open(render_node)), vgem(vgem_fences::open()) {
    if (vgem) {
        watcher = std::make_unique<point_watcher>(*vgem, [this] { retire_overdue(); });
    }
    ALOGI("fences: explicit sync %s, vgem release and present fences %s",
          explicit_sync() ? "on" : (manager ? "off (no render node with timeline syncobjs)" : "off (no wp_linux_drm_syncobj_manager_v1)"),
          vgem ? "on" : "off (vgem missing, using -1)");
}

std::unique_ptr<surface_sync> frame_sync::sync_surface(wl_surface *surface) {
    if (!explicit_sync()) {
        return nullptr;
    }
    std::unique_ptr<surface_sync> sync(
        new surface_sync(*this, wp_linux_drm_syncobj_manager_v1_get_surface(manager, surface)));
    if (!sync->add_acquire_timeline()) {
        return nullptr;
    }
    return sync;
}

int frame_sync::release_fence(surface_sync &surface, uint64_t buffer_id) {
    surface_sync::release_timeline *timeline = surface.release_for(buffer_id);
    if (!vgem || !timeline || timeline->point == 0) {
        return -1;
    }
    if (timeline->fenced_point != timeline->point) {
        vgem_fence fence;
        const int sync_file = vgem->create(fence);
        if (sync_file < 0) {
            return -1;
        }
        const int release = device->point_eventfd(timeline->handle, timeline->point);
        if (release < 0) {
            vgem->signal(fence);
            close(sync_file);
            return -1;
        }
        watcher->watch(release, fence);
        if (timeline->fence >= 0) {
            close(timeline->fence);
        }
        timeline->fence = sync_file;
        timeline->fenced_point = timeline->point;
    }
    return dup(timeline->fence);
}

int frame_sync::retire_fence(uint64_t frame, clock::time_point deadline) {
    if (!vgem) {
        return -1;
    }
    vgem_fence fence;
    const int sync_file = vgem->create(fence);
    if (sync_file >= 0) {
        retire_fences.add(frame, fence, deadline);
        watcher->schedule(deadline);
    }
    return sync_file;
}

void frame_sync::retire(const std::vector<vgem_fence> &fences) {
    for (const vgem_fence &fence : fences) {
        vgem->signal(fence);
    }
}

void frame_sync::frame_finished(uint64_t frame) {
    if (vgem) {
        retire(retire_fences.take_before(frame));
    }
}

void frame_sync::retire_all() {
    if (vgem) {
        retire(retire_fences.take_all());
    }
}

void frame_sync::retire_overdue() {
    retire(retire_fences.take_overdue(clock::now()));
    if (const auto next = retire_fences.next_deadline()) {
        watcher->schedule(*next);
    }
}

surface_sync::surface_sync(frame_sync &sync, wp_linux_drm_syncobj_surface_v1 *proxy)
    : sync(sync), proxy(proxy) {}

wp_linux_drm_syncobj_timeline_v1 *surface_sync::import_timeline(uint32_t handle) {
    const int fd = sync.device->export_timeline(handle);
    if (fd < 0) {
        ALOGE("cannot export a timeline syncobj");
        return nullptr;
    }
    wp_linux_drm_syncobj_timeline_v1 *timeline = wp_linux_drm_syncobj_manager_v1_import_timeline(sync.manager, fd);
    close(fd);
    return timeline;
}

bool surface_sync::add_acquire_timeline() {
    acquire_handle = sync.device->create_timeline();
    acquire_timeline = acquire_handle ? import_timeline(acquire_handle) : nullptr;
    return acquire_timeline != nullptr;
}

surface_sync::release_timeline *surface_sync::release_for(uint64_t buffer_id) {
    const auto found = std::find_if(releases.begin(), releases.end(),
                                    [buffer_id](const release_timeline &timeline) { return timeline.buffer_id == buffer_id; });
    return found != releases.end() ? &*found : nullptr;
}

surface_sync::release_timeline *surface_sync::add_release_timeline(uint64_t buffer_id) {
    if (releases.size() >= release_timelines_per_surface) {
        const auto oldest = std::min_element(releases.begin(), releases.end(),
            [](const release_timeline &a, const release_timeline &b) { return a.last_use < b.last_use; });
        destroy(*oldest);
        releases.erase(oldest);
    }
    release_timeline timeline;
    timeline.buffer_id = buffer_id;
    timeline.handle = sync.device->create_timeline();
    timeline.proxy = timeline.handle ? import_timeline(timeline.handle) : nullptr;
    if (!timeline.proxy) {
        destroy(timeline);
        return nullptr;
    }
    releases.push_back(timeline);
    return &releases.back();
}

void surface_sync::destroy(release_timeline &timeline) {
    if (timeline.proxy) {
        wp_linux_drm_syncobj_timeline_v1_destroy(timeline.proxy);
    }
    if (timeline.handle) {
        sync.device->destroy(timeline.handle);
    }
    if (timeline.fence >= 0) {
        close(timeline.fence);
    }
}

surface_sync::~surface_sync() {
    wp_linux_drm_syncobj_surface_v1_destroy(proxy);
    if (acquire_timeline) {
        wp_linux_drm_syncobj_timeline_v1_destroy(acquire_timeline);
    }
    if (acquire_handle) {
        sync.device->destroy(acquire_handle);
    }
    for (release_timeline &timeline : releases) {
        destroy(timeline);
    }
}

bool surface_sync::set_points(int acquire_fence, uint64_t buffer_id) {
    release_timeline *release = release_for(buffer_id);
    if (!release) {
        release = add_release_timeline(buffer_id);
    }
    if (!release) {
        return false;
    }

    syncobj_device &device = *sync.device;
    ++acquire_value;
    bool acquired = acquire_fence >= 0 && device.import_sync_file(acquire_handle, acquire_value, acquire_fence);
    if (!acquired) {
        if (acquire_fence >= 0 && sync_wait(acquire_fence, acquire_fallback_wait_ms) < 0) {
            ALOGW("acquire fence %d not signaled after %d ms", acquire_fence, acquire_fallback_wait_ms);
        }
        acquired = device.signal_point(acquire_handle, acquire_value);
    }
    if (!acquired) {
        ALOGE("cannot set acquire point %llu", static_cast<unsigned long long>(acquire_value));
    }
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(proxy, acquire_timeline, high_bits(acquire_value),
                                                      low_bits(acquire_value));
    release->last_use = ++uses;
    ++release->point;
    wp_linux_drm_syncobj_surface_v1_set_release_point(proxy, release->proxy, high_bits(release->point),
                                                      low_bits(release->point));
    return true;
}

} // namespace fence
