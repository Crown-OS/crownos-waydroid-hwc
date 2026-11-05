/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fence {

/// One vgem fence: signal it once, through vgem_fences::signal.
struct vgem_fence {
    uint32_t id = 0;
    uint32_t slot = 0;
    uint32_t generation = 0;
};

/// Sync fences that the HWC signals itself, the replacement for sw_sync
/// (which the CrownOS kernel does not have). Each fence sits on its own
/// one-page buffer, so a buffer's exported sync_file holds exactly that
/// fence; buffers return to the pool once their fence signals. vgem signals
/// a forgotten fence by itself after 10 s, so a lost compositor event can
/// never wedge SurfaceFlinger.
///
/// vgem keeps every fence id of an open file until the file closes, so
/// fences come from generations: after a while a new file takes over, and
/// the old one closes once its last fence has signaled.
class vgem_fences {
  public:
    /// Opens the vgem node; null when vgem is not loaded or not reachable.
    static std::unique_ptr<vgem_fences> open();
    static std::unique_ptr<vgem_fences> open_path(const std::string &path);
    ~vgem_fences();

    vgem_fences(const vgem_fences &) = delete;
    vgem_fences &operator=(const vgem_fences &) = delete;

    /// A new unsignaled fence and a sync_file for it, or -1 when none can be
    /// made (the caller then hands out -1, which means "already signaled").
    int create(vgem_fence &fence);
    void signal(const vgem_fence &fence);

  private:
    struct buffer {
        uint32_t handle = 0;
        int dmabuf = -1;
        bool busy = false;
    };
    struct generation {
        ~generation();
        uint32_t number = 0;
        int fd = -1;
        std::vector<buffer> buffers;
        uint64_t fences_made = 0;
        size_t busy = 0;

        bool add_buffer();
    };

    vgem_fences(std::string path, std::unique_ptr<generation> first);
    static std::unique_ptr<generation> open_generation(const std::string &path, uint32_t number);
    generation *current_generation();

    const std::string path;
    std::mutex mutex;
    /// The last one makes new fences; older ones only finish theirs.
    std::vector<std::unique_ptr<generation>> generations;
};

} // namespace fence
