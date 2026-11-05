/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "fence/frame_sync.h"

#include "linux-drm-syncobj-v1-client-protocol.h"

#include <log/log.h>
#include <sync/sync.h>

#include <unistd.h>

namespace fence {
namespace {

/// How long an acquire fence that cannot become a timeline point may block.
constexpr int acquire_fallback_wait_ms = 100;

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
        watcher = std::make_unique<point_watcher>(*vgem);
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
    if (!sync->add_timelines()) {
        return nullptr;
    }
    return sync;
}

int frame_sync::release_fence(const surface_sync &surface) {
    if (!vgem || !device || surface.release_point() == 0) {
        return -1;
    }
    vgem_fence fence;
    const int sync_file = vgem->create(fence);
    if (sync_file < 0) {
        return -1;
    }
    const int release = device->point_eventfd(surface.release_timeline(), surface.release_point());
    if (release < 0) {
        vgem->signal(fence);
        close(sync_file);
        return -1;
    }
    watcher->watch(release, fence);
    return sync_file;
}

int frame_sync::retire_fence(uint64_t frame) {
    if (!vgem) {
        return -1;
    }
    vgem_fence fence;
    const int sync_file = vgem->create(fence);
    if (sync_file >= 0) {
        retire_fences.add(frame, fence);
    }
    return sync_file;
}

void frame_sync::frame_finished(uint64_t frame) {
    if (!vgem) {
        return;
    }
    for (const vgem_fence &fence : retire_fences.take_before(frame)) {
        vgem->signal(fence);
    }
}

surface_sync::surface_sync(frame_sync &sync, wp_linux_drm_syncobj_surface_v1 *proxy)
    : sync(sync), proxy(proxy) {}

bool surface_sync::add_timelines() {
    syncobj_device &device = *sync.device;
    acquire_handle = device.create_timeline();
    release_handle = device.create_timeline();
    const int acquire_fd = acquire_handle ? device.export_timeline(acquire_handle) : -1;
    const int release_fd = release_handle ? device.export_timeline(release_handle) : -1;
    const bool exported = acquire_fd >= 0 && release_fd >= 0;
    if (exported) {
        acquire_timeline = wp_linux_drm_syncobj_manager_v1_import_timeline(sync.manager, acquire_fd);
        release_timeline_proxy = wp_linux_drm_syncobj_manager_v1_import_timeline(sync.manager, release_fd);
    } else {
        ALOGE("cannot export timeline syncobjs; this surface uses implicit sync");
    }
    if (acquire_fd >= 0) {
        close(acquire_fd);
    }
    if (release_fd >= 0) {
        close(release_fd);
    }
    return exported;
}

surface_sync::~surface_sync() {
    wp_linux_drm_syncobj_surface_v1_destroy(proxy);
    if (acquire_timeline) {
        wp_linux_drm_syncobj_timeline_v1_destroy(acquire_timeline);
    }
    if (release_timeline_proxy) {
        wp_linux_drm_syncobj_timeline_v1_destroy(release_timeline_proxy);
    }
    if (acquire_handle) {
        sync.device->destroy(acquire_handle);
    }
    if (release_handle) {
        sync.device->destroy(release_handle);
    }
}

void surface_sync::set_points(int acquire_fence) {
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
    ++release_value;
    wp_linux_drm_syncobj_surface_v1_set_release_point(proxy, release_timeline_proxy, high_bits(release_value),
                                                      low_bits(release_value));
}

} // namespace fence
