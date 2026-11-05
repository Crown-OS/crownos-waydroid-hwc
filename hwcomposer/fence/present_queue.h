/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace fence {

/// Retire fences by frame. HWC1 retires a frame when the next one is on
/// screen, and SurfaceFlinger (through hwc2on1adapter) reads the fence of
/// frame N - 1 as the present fence of frame N. So when the compositor
/// presents (or discards, which also means "superseded") frame N, every
/// fence of an earlier frame is due.
template <class Fence>
class present_queue {
  public:
    void add(uint64_t frame, const Fence &fence) {
        std::lock_guard lock(mutex);
        pending.push_back({frame, fence});
    }

    /// Fences of frames before `frame`, removed from the queue.
    std::vector<Fence> take_before(uint64_t frame) {
        std::vector<Fence> due;
        std::lock_guard lock(mutex);
        while (!pending.empty() && pending.front().frame < frame) {
            due.push_back(pending.front().fence);
            pending.pop_front();
        }
        return due;
    }

    std::vector<Fence> take_all() {
        std::vector<Fence> due;
        std::lock_guard lock(mutex);
        for (const auto &entry : pending) {
            due.push_back(entry.fence);
        }
        pending.clear();
        return due;
    }

  private:
    struct entry {
        uint64_t frame;
        Fence fence;
    };

    std::mutex mutex;
    std::deque<entry> pending;
};

} // namespace fence
