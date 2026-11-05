/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace fence {

/// Retire fences by frame. HWC1 retires a frame when the next one is on
/// screen, and SurfaceFlinger (through hwc2on1adapter) reads the fence of
/// frame N - 1 as the present fence of frame N. So when the compositor
/// presents (or discards, which also means "superseded") frame N, every
/// fence of an earlier frame is due. A fence is also due at its deadline:
/// a compositor may hold the feedback of a window nobody sees, and
/// SurfaceFlinger must not wait for it.
template <class Fence>
class present_queue {
  public:
    using clock = std::chrono::steady_clock;

    void add(uint64_t frame, const Fence &fence, clock::time_point deadline) {
        std::lock_guard lock(mutex);
        pending.push_back({frame, fence, deadline});
    }

    /// Fences of frames before `frame`, removed from the queue.
    std::vector<Fence> take_before(uint64_t frame) {
        return take_while([frame](const entry &front) { return front.frame < frame; });
    }

    /// Fences whose deadline has passed at `now`.
    std::vector<Fence> take_overdue(clock::time_point now) {
        return take_while([now](const entry &front) { return front.deadline <= now; });
    }

    std::vector<Fence> take_all() {
        return take_while([](const entry &) { return true; });
    }

    std::optional<clock::time_point> next_deadline() const {
        std::lock_guard lock(mutex);
        if (pending.empty()) {
            return std::nullopt;
        }
        return pending.front().deadline;
    }

  private:
    struct entry {
        uint64_t frame;
        Fence fence;
        clock::time_point deadline;
    };

    template <class Due>
    std::vector<Fence> take_while(Due due) {
        std::vector<Fence> taken;
        std::lock_guard lock(mutex);
        while (!pending.empty() && due(pending.front())) {
            taken.push_back(pending.front().fence);
            pending.pop_front();
        }
        return taken;
    }

    mutable std::mutex mutex;
    std::deque<entry> pending;
};

} // namespace fence
