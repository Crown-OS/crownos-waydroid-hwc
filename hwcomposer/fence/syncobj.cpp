/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "fence/syncobj.h"

#include "fence/kernel_uapi.h"
#include "util/log.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fence {
namespace {

constexpr int first_render_minor = 128;
constexpr int render_minor_count = 64;

int drm_ioctl(int fd, unsigned long request, void *argument) {
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && (errno == EINTR || errno == EAGAIN));
    return result;
}

bool has_timeline_syncobj(int fd) {
    drm_get_cap cap{};
    cap.capability = DRM_CAP_SYNCOBJ_TIMELINE;
    return drm_ioctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1;
}

uint32_t create_syncobj(int fd, uint32_t flags) {
    drm_syncobj_create create{};
    create.flags = flags;
    return drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) == 0 ? create.handle : 0;
}

} // namespace

std::unique_ptr<syncobj_device> syncobj_device::open_path(const std::string &path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return nullptr;
    }
    const uint32_t scratch = has_timeline_syncobj(fd) ? create_syncobj(fd, 0) : 0;
    if (scratch == 0) {
        close(fd);
        return nullptr;
    }
    return std::unique_ptr<syncobj_device>(new syncobj_device(fd, scratch));
}

std::unique_ptr<syncobj_device> syncobj_device::open(const std::string &preferred) {
    if (!preferred.empty()) {
        if (auto device = open_path(preferred)) {
            return device;
        }
    }
    for (int minor = first_render_minor; minor < first_render_minor + render_minor_count; ++minor) {
        if (auto device = open_path("/dev/dri/renderD" + std::to_string(minor))) {
            return device;
        }
    }
    return nullptr;
}

syncobj_device::syncobj_device(int fd, uint32_t scratch) : fd(fd), scratch(scratch) {}

syncobj_device::~syncobj_device() {
    destroy(scratch);
    close(fd);
}

uint32_t syncobj_device::create_timeline() {
    return create_syncobj(fd, 0);
}

void syncobj_device::destroy(uint32_t handle) {
    drm_syncobj_destroy destroy{};
    destroy.handle = handle;
    drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
}

int syncobj_device::export_timeline(uint32_t handle) {
    drm_syncobj_handle exported{};
    exported.handle = handle;
    exported.fd = -1;
    return drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &exported) == 0 ? exported.fd : -1;
}

bool syncobj_device::import_sync_file(uint32_t handle, uint64_t point, int sync_file) {
    drm_syncobj_handle imported{};
    imported.handle = scratch;
    imported.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    imported.fd = sync_file;
    if (drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &imported) != 0) {
        CROWN_LOGE("cannot import an acquire fence: %s", strerror(errno));
        return false;
    }
    drm_syncobj_transfer transfer{};
    transfer.src_handle = scratch;
    transfer.dst_handle = handle;
    transfer.dst_point = point;
    if (drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &transfer) != 0) {
        CROWN_LOGE("cannot move an acquire fence to its timeline point: %s", strerror(errno));
        return false;
    }
    return true;
}

bool syncobj_device::signal_point(uint32_t handle, uint64_t point) {
    drm_syncobj_timeline_array signal{};
    signal.handles = reinterpret_cast<uintptr_t>(&handle);
    signal.points = reinterpret_cast<uintptr_t>(&point);
    signal.count_handles = 1;
    return drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &signal) == 0;
}

int syncobj_device::point_eventfd(uint32_t handle, uint64_t point) {
    const int event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event < 0) {
        return -1;
    }
    drm_syncobj_eventfd wait{};
    wait.handle = handle;
    wait.point = point;
    wait.fd = event;
    if (drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &wait) != 0) {
        close(event);
        return -1;
    }
    return event;
}

} // namespace fence
