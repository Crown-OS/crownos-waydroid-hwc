/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "fence/vgem.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace fence {

/// Signals vgem fences when the compositor's release points signal. Each
/// watched point is an eventfd from syncobj_device::point_eventfd; one thread
/// polls them all, so nothing on the composition path ever waits. The same
/// thread runs `on_deadline` once a scheduled deadline passes.
class point_watcher {
  public:
    using clock = std::chrono::steady_clock;

    point_watcher(vgem_fences &fences, std::function<void()> on_deadline);
    ~point_watcher();

    point_watcher(const point_watcher &) = delete;
    point_watcher &operator=(const point_watcher &) = delete;

    /// Takes `eventfd` and signals `fence` once it is readable.
    void watch(int eventfd, const vgem_fence &fence);
    /// Runs `on_deadline` at `deadline`, or earlier if an earlier one is due.
    void schedule(clock::time_point deadline);

  private:
    struct watched {
        int eventfd;
        vgem_fence fence;
    };

    void run();
    void wake();
    int poll_timeout_ms();

    vgem_fences &fences;
    std::function<void()> on_deadline;
    std::optional<clock::time_point> deadline;
    int wake_fd;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::vector<watched> added;
    std::thread thread;
};

} // namespace fence
