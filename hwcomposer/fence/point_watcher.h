/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "fence/vgem.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace fence {

/// Signals vgem fences when the compositor's release points signal. Each
/// watched point is an eventfd from syncobj_device::point_eventfd; one thread
/// polls them all, so nothing on the composition path ever waits.
class point_watcher {
  public:
    explicit point_watcher(vgem_fences &fences);
    ~point_watcher();

    point_watcher(const point_watcher &) = delete;
    point_watcher &operator=(const point_watcher &) = delete;

    /// Takes `eventfd` and signals `fence` once it is readable.
    void watch(int eventfd, const vgem_fence &fence);

  private:
    struct watched {
        int eventfd;
        vgem_fence fence;
    };

    void run();

    vgem_fences &fences;
    int wake_fd;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::vector<watched> added;
    std::thread thread;
};

} // namespace fence
