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
#include <vector>

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
///   presented, or at its deadline.
/// Each part works alone and degrades to the stock behaviour (CPU wait, or
/// -1 fences) when its kernel or compositor support is missing.
class frame_sync {
  public:
    using clock = std::chrono::steady_clock;

    frame_sync(wp_linux_drm_syncobj_manager_v1 *manager, const std::string &render_node);

    bool explicit_sync() const { return manager && device; }
    std::unique_ptr<surface_sync> sync_surface(wl_surface *surface);

    /// A fence that signals with the release point of the surface's last
    /// commit of `buffer_id`, or -1. Asking again for the same commit hands
    /// out the same fence.
    int release_fence(surface_sync &surface, uint64_t buffer_id);
    /// The retire fence of `frame`, due at the latest at `deadline`; or -1.
    int retire_fence(uint64_t frame, clock::time_point deadline);
    /// `frame` was presented or discarded: earlier frames are retired.
    void frame_finished(uint64_t frame);
    /// Retires every frame, for when nothing will time them any more.
    void retire_all();

  private:
    friend class surface_sync;

    void retire(const std::vector<vgem_fence> &fences);
    void retire_overdue();

    wp_linux_drm_syncobj_manager_v1 *manager;
    std::unique_ptr<syncobj_device> device;
    std::unique_ptr<vgem_fences> vgem;
    present_queue<vgem_fence> retire_fences;
    std::unique_ptr<point_watcher> watcher;
};

/// wp_linux_drm_syncobj_surface_v1 of one surface with its acquire timeline
/// and one release timeline per buffer it shows. Compositors may release
/// buffers out of order (one still on scanout, the next replaced unseen),
/// and a later point on a shared timeline would mark an earlier one done.
/// A surface that has this object must set both points on every commit
/// with a buffer, and may only commit dmabufs.
class surface_sync {
  public:
    ~surface_sync();

    surface_sync(const surface_sync &) = delete;
    surface_sync &operator=(const surface_sync &) = delete;

    /// Sets the next commit's points. `acquire_fence` (-1 when the buffer is
    /// ready) is not closed. False, with nothing set, when the buffer cannot
    /// get a release timeline; the surface must then drop explicit sync.
    [[nodiscard]] bool set_points(int acquire_fence, uint64_t buffer_id);

  private:
    friend class frame_sync;

    struct release_timeline {
        uint64_t buffer_id = 0;
        uint32_t handle = 0;
        wp_linux_drm_syncobj_timeline_v1 *proxy = nullptr;
        uint64_t point = 0;
        /// The release fence handed out for `point`, kept to hand out again.
        uint64_t fenced_point = 0;
        int fence = -1;
        uint64_t last_use = 0;
    };

    surface_sync(frame_sync &sync, wp_linux_drm_syncobj_surface_v1 *proxy);
    bool add_acquire_timeline();
    wp_linux_drm_syncobj_timeline_v1 *import_timeline(uint32_t handle);
    release_timeline *release_for(uint64_t buffer_id);
    release_timeline *add_release_timeline(uint64_t buffer_id);
    void destroy(release_timeline &timeline);

    frame_sync &sync;
    wp_linux_drm_syncobj_surface_v1 *proxy;
    uint32_t acquire_handle = 0;
    wp_linux_drm_syncobj_timeline_v1 *acquire_timeline = nullptr;
    uint64_t acquire_value = 0;
    std::vector<release_timeline> releases;
    uint64_t uses = 0;
};

} // namespace fence
