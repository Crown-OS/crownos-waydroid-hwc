/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 *
 * The syncobj and vgem tests run against the host's DRM nodes and skip
 * themselves where a node is missing.
 */

#include "fence/kernel_uapi.h"
#include "fence/point_watcher.h"
#include "fence/present_queue.h"
#include "fence/syncobj.h"
#include "fence/vgem.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace fence;
using namespace std::chrono_literals;

namespace {

bool readable(int fd, int timeout_ms = 0) {
    pollfd waiter{fd, POLLIN, 0};
    return poll(&waiter, 1, timeout_ms) == 1;
}

/// A sync_file that is already signaled, made from a signaled binary syncobj.
int signaled_sync_file(const std::string &node) {
    const int fd = open(node.c_str(), O_RDWR | O_CLOEXEC);
    drm_syncobj_create create{};
    create.flags = DRM_SYNCOBJ_CREATE_SIGNALED;
    ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create);
    drm_syncobj_handle exported{};
    exported.handle = create.handle;
    exported.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    exported.fd = -1;
    ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &exported);
    close(fd);
    return exported.fd;
}

std::unique_ptr<syncobj_device> host_device(std::string &node) {
    for (int minor = 128; minor < 192; ++minor) {
        node = "/dev/dri/renderD" + std::to_string(minor);
        if (auto device = syncobj_device::open_path(node)) {
            return device;
        }
    }
    return nullptr;
}

} // namespace

TEST(PresentQueue, PresentingAFrameRetiresEveryEarlierFrame) {
    present_queue<int> queue;
    queue.add(1, 10);
    queue.add(2, 20);
    queue.add(3, 30);
    EXPECT_TRUE(queue.take_before(1).empty());
    EXPECT_EQ(queue.take_before(3), (std::vector<int>{10, 20}));
    EXPECT_TRUE(queue.take_before(3).empty());
    EXPECT_EQ(queue.take_all(), (std::vector<int>{30}));
}

TEST(Syncobj, SignaledPointWakesItsEventfd) {
    std::string node;
    auto device = host_device(node);
    if (!device) {
        GTEST_SKIP() << "no render node with timeline syncobjs";
    }
    const uint32_t timeline = device->create_timeline();
    ASSERT_NE(timeline, 0u);
    const int first = device->point_eventfd(timeline, 1);
    if (first < 0) {
        device->destroy(timeline);
        GTEST_SKIP() << "kernel without DRM_IOCTL_SYNCOBJ_EVENTFD";
    }
    const int second = device->point_eventfd(timeline, 2);
    EXPECT_FALSE(readable(first));
    ASSERT_TRUE(device->signal_point(timeline, 1));
    EXPECT_TRUE(readable(first, 1000));
    EXPECT_FALSE(readable(second));
    ASSERT_TRUE(device->signal_point(timeline, 2));
    EXPECT_TRUE(readable(second, 1000));
    close(first);
    close(second);
    device->destroy(timeline);
}

TEST(Syncobj, ImportedSyncFileLandsOnItsPoint) {
    std::string node;
    auto device = host_device(node);
    if (!device) {
        GTEST_SKIP() << "no render node with timeline syncobjs";
    }
    const uint32_t timeline = device->create_timeline();
    const int sync_file = signaled_sync_file(node);
    ASSERT_GE(sync_file, 0);
    const int waiter = device->point_eventfd(timeline, 5);
    if (waiter < 0) {
        close(sync_file);
        device->destroy(timeline);
        GTEST_SKIP() << "kernel without DRM_IOCTL_SYNCOBJ_EVENTFD";
    }
    EXPECT_FALSE(readable(waiter));
    ASSERT_TRUE(device->import_sync_file(timeline, 5, sync_file));
    EXPECT_TRUE(readable(waiter, 1000));
    const int exported = device->export_timeline(timeline);
    EXPECT_GE(exported, 0);
    close(exported);
    close(sync_file);
    close(waiter);
    device->destroy(timeline);
}

TEST(Vgem, FencesSignalThroughTheWatcher) {
    auto fences = vgem_fences::open();
    if (!fences) {
        GTEST_SKIP() << "vgem is not loaded";
    }
    std::string node;
    auto device = host_device(node);
    if (!device) {
        GTEST_SKIP() << "no render node with timeline syncobjs";
    }
    vgem_fence fence;
    const int sync_file = fences->create(fence);
    ASSERT_GE(sync_file, 0);
    EXPECT_FALSE(readable(sync_file));

    const uint32_t timeline = device->create_timeline();
    const int release = device->point_eventfd(timeline, 1);
    ASSERT_GE(release, 0);
    {
        point_watcher watcher(*fences);
        watcher.watch(release, fence);
        std::this_thread::sleep_for(20ms);
        EXPECT_FALSE(readable(sync_file));
        ASSERT_TRUE(device->signal_point(timeline, 1));
        EXPECT_TRUE(readable(sync_file, 1000));
    }
    close(sync_file);
    device->destroy(timeline);
}
