/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "fence/vgem.h"

#include "fence/kernel_uapi.h"
#include "util/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fence {
namespace {

constexpr const char *vgem_by_path = "/dev/dri/by-path/platform-vgem-render";
constexpr const char *dri_directory = "/dev/dri";
constexpr const char *system_heap = "/dev/dma_heap/system";
constexpr size_t fence_buffer_bytes = 4096;
constexpr uint32_t dumb_width = 1024;
constexpr uint32_t dumb_bpp = 32;
/// Fences outstanding at once: frames in flight times layers, with room.
constexpr size_t max_fence_buffers = 256;
/// vgem keeps about 10 bytes per fence id ever made on a file.
constexpr uint64_t fences_per_generation = 1 << 18;

int drm_ioctl(int fd, unsigned long request, void *argument) {
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && (errno == EINTR || errno == EAGAIN));
    return result;
}

bool is_vgem(int fd) {
    char name[16] = {};
    drm_version version{};
    version.name = name;
    version.name_len = sizeof(name) - 1;
    return drm_ioctl(fd, DRM_IOCTL_VERSION, &version) == 0 && std::strcmp(name, "vgem") == 0;
}

/// A one-page dma-buf: from the system dma-heap, or as a vgem dumb buffer
/// where the heap is unavailable.
bool allocate_buffer(int vgem_fd, uint32_t &handle, int &dmabuf) {
    const int heap = ::open(system_heap, O_RDONLY | O_CLOEXEC);
    if (heap >= 0) {
        dma_heap_allocation_data allocation{};
        allocation.len = fence_buffer_bytes;
        allocation.fd_flags = O_RDWR | O_CLOEXEC;
        const bool allocated = drm_ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation) == 0;
        close(heap);
        if (allocated) {
            drm_prime_handle prime{};
            prime.fd = static_cast<int>(allocation.fd);
            if (drm_ioctl(vgem_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) == 0) {
                handle = prime.handle;
                dmabuf = prime.fd;
                return true;
            }
            close(static_cast<int>(allocation.fd));
        }
    }
    drm_mode_create_dumb dumb{};
    dumb.width = dumb_width;
    dumb.height = fence_buffer_bytes / (dumb_width * dumb_bpp / 8);
    dumb.bpp = dumb_bpp;
    if (drm_ioctl(vgem_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) != 0) {
        return false;
    }
    drm_prime_handle prime{};
    prime.handle = dumb.handle;
    prime.flags = DRM_CLOEXEC | DRM_RDWR;
    if (drm_ioctl(vgem_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) {
        drm_gem_close close_handle{};
        close_handle.handle = dumb.handle;
        drm_ioctl(vgem_fd, DRM_IOCTL_GEM_CLOSE, &close_handle);
        return false;
    }
    handle = dumb.handle;
    dmabuf = prime.fd;
    return true;
}

} // namespace

std::unique_ptr<vgem_fences::generation> vgem_fences::open_generation(const std::string &path, uint32_t number) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return nullptr;
    }
    if (!is_vgem(fd)) {
        close(fd);
        return nullptr;
    }
    auto opened = std::make_unique<generation>();
    opened->number = number;
    opened->fd = fd;
    if (!opened->add_buffer()) {
        CROWN_LOGW("vgem at %s cannot make fence buffers", path.c_str());
        return nullptr;
    }
    return opened;
}

std::unique_ptr<vgem_fences> vgem_fences::open_path(const std::string &path) {
    auto first = open_generation(path, 1);
    if (!first) {
        return nullptr;
    }
    return std::unique_ptr<vgem_fences>(new vgem_fences(path, std::move(first)));
}

std::unique_ptr<vgem_fences> vgem_fences::open() {
    if (auto fences = open_path(vgem_by_path)) {
        return fences;
    }
    DIR *directory = opendir(dri_directory);
    if (!directory) {
        return nullptr;
    }
    std::vector<std::string> nodes;
    while (const dirent *entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.rfind("renderD", 0) == 0 || name.rfind("card", 0) == 0) {
            nodes.push_back(std::string(dri_directory) + "/" + name);
        }
    }
    closedir(directory);
    for (const std::string &node : nodes) {
        if (auto fences = open_path(node)) {
            return fences;
        }
    }
    return nullptr;
}

vgem_fences::vgem_fences(std::string path, std::unique_ptr<generation> first) : path(std::move(path)) {
    generations.push_back(std::move(first));
}

vgem_fences::~vgem_fences() = default;

vgem_fences::generation::~generation() {
    for (buffer &entry : buffers) {
        drm_gem_close close_handle{};
        close_handle.handle = entry.handle;
        drm_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_handle);
        close(entry.dmabuf);
    }
    close(fd);
}

bool vgem_fences::generation::add_buffer() {
    buffer entry;
    if (!allocate_buffer(fd, entry.handle, entry.dmabuf)) {
        return false;
    }
    buffers.push_back(entry);
    return true;
}

vgem_fences::generation *vgem_fences::current_generation() {
    generation *current = generations.back().get();
    if (current->fences_made < fences_per_generation) {
        return current;
    }
    if (auto next = open_generation(path, current->number + 1)) {
        generations.push_back(std::move(next));
        if (current->busy == 0) {
            generations.erase(generations.end() - 2);
        }
    }
    return generations.back().get();
}

int vgem_fences::create(vgem_fence &fence) {
    std::lock_guard lock(mutex);
    generation &current = *current_generation();
    size_t slot = 0;
    while (slot < current.buffers.size() && current.buffers[slot].busy) {
        ++slot;
    }
    if (slot == current.buffers.size() && (current.buffers.size() >= max_fence_buffers || !current.add_buffer())) {
        return -1;
    }
    buffer &entry = current.buffers[slot];

    drm_vgem_fence_attach attach{};
    attach.handle = entry.handle;
    attach.flags = VGEM_FENCE_WRITE;
    if (drm_ioctl(current.fd, DRM_IOCTL_VGEM_FENCE_ATTACH, &attach) != 0) {
        CROWN_LOGE("cannot attach a vgem fence: %s", strerror(errno));
        return -1;
    }
    ++current.fences_made;
    dma_buf_export_sync_file exported{};
    exported.flags = DMA_BUF_SYNC_READ;
    exported.fd = -1;
    if (drm_ioctl(entry.dmabuf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported) != 0) {
        CROWN_LOGE("cannot export a vgem fence: %s", strerror(errno));
        drm_vgem_fence_signal signal_now{};
        signal_now.fence = attach.out_fence;
        drm_ioctl(current.fd, DRM_IOCTL_VGEM_FENCE_SIGNAL, &signal_now);
        return -1;
    }
    entry.busy = true;
    ++current.busy;
    fence = {attach.out_fence, static_cast<uint32_t>(slot), current.number};
    return exported.fd;
}

void vgem_fences::signal(const vgem_fence &fence) {
    std::lock_guard lock(mutex);
    const auto owner = std::find_if(generations.begin(), generations.end(),
                                    [&fence](const auto &candidate) { return candidate->number == fence.generation; });
    if (owner == generations.end() || fence.slot >= (*owner)->buffers.size()) {
        return;
    }
    generation &made_by = **owner;
    drm_vgem_fence_signal signal_fence{};
    signal_fence.fence = fence.id;
    if (drm_ioctl(made_by.fd, DRM_IOCTL_VGEM_FENCE_SIGNAL, &signal_fence) != 0 && errno != ETIMEDOUT) {
        CROWN_LOGE("cannot signal vgem fence %u: %s", fence.id, strerror(errno));
    }
    made_by.buffers[fence.slot].busy = false;
    --made_by.busy;
    if (made_by.busy == 0 && owner + 1 != generations.end()) {
        generations.erase(owner);
    }
}

} // namespace fence
