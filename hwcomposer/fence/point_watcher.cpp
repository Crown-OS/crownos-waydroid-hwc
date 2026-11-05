/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "fence/point_watcher.h"

#include "util/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace fence {
namespace {

void drain(int fd) {
    uint64_t count;
    while (read(fd, &count, sizeof(count)) > 0) {
    }
}

} // namespace

point_watcher::point_watcher(vgem_fences &fences, std::function<void()> on_deadline)
    : fences(fences), on_deadline(std::move(on_deadline)), wake_fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)),
      thread(&point_watcher::run, this) {}

point_watcher::~point_watcher() {
    stopping = true;
    wake();
    thread.join();
    for (const watched &entry : added) {
        fences.signal(entry.fence);
        close(entry.eventfd);
    }
    close(wake_fd);
}

void point_watcher::wake() {
    const uint64_t one = 1;
    if (write(wake_fd, &one, sizeof(one)) < 0) {
        CROWN_LOGE("cannot wake the fence thread: %s", strerror(errno));
    }
}

void point_watcher::watch(int eventfd, const vgem_fence &fence) {
    {
        std::lock_guard lock(mutex);
        added.push_back({eventfd, fence});
    }
    wake();
}

void point_watcher::schedule(clock::time_point when) {
    {
        std::lock_guard lock(mutex);
        if (deadline && *deadline <= when) {
            return;
        }
        deadline = when;
    }
    wake();
}

int point_watcher::poll_timeout_ms() {
    std::lock_guard lock(mutex);
    if (!deadline) {
        return -1;
    }
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(*deadline - clock::now()).count();
    return static_cast<int>(std::max<int64_t>(0, left));
}

void point_watcher::run() {
    std::vector<watched> pending;
    std::vector<pollfd> fds;
    while (!stopping) {
        {
            std::lock_guard lock(mutex);
            pending.insert(pending.end(), added.begin(), added.end());
            added.clear();
        }
        fds.assign(1, pollfd{wake_fd, POLLIN, 0});
        for (const watched &entry : pending) {
            fds.push_back({entry.eventfd, POLLIN, 0});
        }
        if (poll(fds.data(), fds.size(), poll_timeout_ms()) < 0 && errno != EINTR) {
            CROWN_LOGE("fence thread poll failed: %s", strerror(errno));
            break;
        }
        bool deadline_passed = false;
        {
            std::lock_guard lock(mutex);
            if (deadline && *deadline <= clock::now()) {
                deadline.reset();
                deadline_passed = true;
            }
        }
        if (deadline_passed && on_deadline) {
            on_deadline();
        }
        if (fds[0].revents & POLLIN) {
            drain(wake_fd);
        }
        size_t kept = 0;
        for (size_t i = 0; i < pending.size(); ++i) {
            if (fds[i + 1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
                fences.signal(pending[i].fence);
                close(pending[i].eventfd);
            } else {
                pending[kept++] = pending[i];
            }
        }
        pending.resize(kept);
    }
    std::lock_guard lock(mutex);
    added.insert(added.end(), pending.begin(), pending.end());
}

} // namespace fence
