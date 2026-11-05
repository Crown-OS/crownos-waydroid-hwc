/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "fence/point_watcher.h"
#include "fence/present_queue.h"
#include "fence/syncobj.h"
#include "fence/vgem.h"

#include <cstdint>
#include <memory>
#include <string>

struct wl_surface;
struct wp_linux_drm_syncobj_manager_v1;
struct wp_linux_drm_syncobj_surface_v1;
struct wp_linux_drm_syncobj_timeline_v1;

namespace fence {

class surface_sync;

/// The HWC's fences without sw_sync:
/// - acquire: SurfaceFlinger's acquire fence goes to the compositor as a
///   wp_linux_drm_syncobj timeline point, so nothing waits on the CPU;
/// - release: a vgem fence that signals when the compositor signals the
///   commit's release point;
/// - retire (present): a vgem fence that signals when the next frame is
///   presented.
/// Each part works alone and degrades to the stock behaviour (CPU wait, or
/// -1 fences) when its kernel or compositor support is missing.
class frame_sync {
  public:
    frame_sync(wp_linux_drm_syncobj_manager_v1 *manager, const std::string &render_node);

    bool explicit_sync() const { return manager && device; }
    std::unique_ptr<surface_sync> sync_surface(wl_surface *surface);

    /// A fence that signals with the release point of the surface's last
    /// commit, or -1.
    int release_fence(const surface_sync &surface);
    /// The retire fence of `frame`, or -1.
    int retire_fence(uint64_t frame);
    /// `frame` was presented or discarded: earlier frames are retired.
    void frame_finished(uint64_t frame);

    syncobj_device &syncobj() { return *device; }

  private:
    friend class surface_sync;

    wp_linux_drm_syncobj_manager_v1 *manager;
    std::unique_ptr<syncobj_device> device;
    std::unique_ptr<vgem_fences> vgem;
    std::unique_ptr<point_watcher> watcher;
    present_queue<vgem_fence> retire_fences;
};

/// wp_linux_drm_syncobj_surface_v1 of one surface with its acquire and
/// release timelines. A surface that has one must set both points on every
/// commit with a buffer, and may only commit dmabufs.
class surface_sync {
  public:
    ~surface_sync();

    surface_sync(const surface_sync &) = delete;
    surface_sync &operator=(const surface_sync &) = delete;

    /// Sets the next commit's points. `acquire_fence` (-1 when the buffer is
    /// ready) is not closed.
    void set_points(int acquire_fence);

    uint32_t release_timeline() const { return release_handle; }
    uint64_t release_point() const { return release_value; }

  private:
    friend class frame_sync;
    surface_sync(frame_sync &sync, wp_linux_drm_syncobj_surface_v1 *proxy);
    bool add_timelines();

    frame_sync &sync;
    wp_linux_drm_syncobj_surface_v1 *proxy;
    uint32_t acquire_handle = 0;
    uint32_t release_handle = 0;
    wp_linux_drm_syncobj_timeline_v1 *acquire_timeline = nullptr;
    wp_linux_drm_syncobj_timeline_v1 *release_timeline_proxy = nullptr;
    uint64_t acquire_value = 0;
    uint64_t release_value = 0;
};

} // namespace fence
