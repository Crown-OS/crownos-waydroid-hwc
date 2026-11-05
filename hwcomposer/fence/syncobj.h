/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace fence {

/// A DRM render node with timeline syncobjs, the kernel side of
/// wp_linux_drm_syncobj_v1. Syncobjs are device-agnostic, so any node that
/// has them serves, but it must be one the compositor's GPU can wait on:
/// the GPU render node Android uses.
class syncobj_device {
  public:
    /// Opens `preferred` if it has timeline syncobjs, else the first render
    /// node that has. Null when there is none.
    static std::unique_ptr<syncobj_device> open(const std::string &preferred);
    static std::unique_ptr<syncobj_device> open_path(const std::string &path);
    ~syncobj_device();

    syncobj_device(const syncobj_device &) = delete;
    syncobj_device &operator=(const syncobj_device &) = delete;

    /// A new timeline syncobj, 0 on failure.
    uint32_t create_timeline();
    void destroy(uint32_t handle);
    /// A new fd for the syncobj, for wp_linux_drm_syncobj_manager_v1.import_timeline.
    int export_timeline(uint32_t handle);
    /// Puts the fence of `sync_file` at `point` of the timeline. The caller
    /// keeps `sync_file`.
    bool import_sync_file(uint32_t handle, uint64_t point, int sync_file);
    /// Signals `point` at once.
    bool signal_point(uint32_t handle, uint64_t point);
    /// A new eventfd that becomes readable when `point` signals; -1 on failure.
    int point_eventfd(uint32_t handle, uint64_t point);

  private:
    explicit syncobj_device(int fd, uint32_t scratch);

    int fd;
    /// Binary syncobj that sync_file imports pass through on their way to a
    /// timeline point.
    uint32_t scratch;
};

} // namespace fence
