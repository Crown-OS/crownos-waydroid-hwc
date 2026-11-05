/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "agent/link.h"

#include "util/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace agent {
namespace {

constexpr size_t max_queued_bytes = 1 << 20;
constexpr size_t read_chunk_bytes = 4096;
constexpr int safe_mode_check_ms = 1000;

int connect_unix(const std::string &path) {
    sockaddr_un address{};
    if (path.size() >= sizeof(address.sun_path)) {
        return -1;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int remaining_ms(std::chrono::steady_clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return static_cast<int>(std::max<int64_t>(0, left.count()));
}

/// Appends what the socket has to `received`. False on EOF or a socket error.
bool read_available(int socket_fd, std::vector<uint8_t> &received) {
    uint8_t chunk[read_chunk_bytes];
    while (true) {
        const ssize_t count = recv(socket_fd, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (count > 0) {
            received.insert(received.end(), chunk, chunk + count);
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

/// Writes as much of `bytes` as the socket takes and erases it. False on a
/// socket error.
bool write_available(int socket_fd, std::vector<uint8_t> &bytes) {
    size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::send(socket_fd, bytes.data() + written, bytes.size() - written,
                                     MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        return false;
    }
    bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(written));
    return true;
}

} // namespace

link::link(link_config config, link_listener &listener)
    : config(std::move(config)), listener(listener),
      wake_fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

link::~link() {
    stop();
    if (wake_fd >= 0) {
        close(wake_fd);
    }
}

void link::start() {
    if (thread.joinable() || wake_fd < 0) {
        return;
    }
    stopping = false;
    thread = std::thread(&link::run, this);
}

void link::stop() {
    stopping = true;
    wake();
    if (thread.joinable()) {
        thread.join();
    }
}

bool link::send(const agent_wire::Record &record) {
    if (!connected()) {
        return false;
    }
    {
        std::lock_guard lock(outgoing_mutex);
        if (outgoing.size() > max_queued_bytes) {
            CROWN_LOGW("crown_agent is not reading; dropping record 0x%04x",
                       agent_wire::record_type(record));
            return false;
        }
        const agent_wire::Error error = agent_wire::encode(record, outgoing);
        if (error != agent_wire::Error::none) {
            CROWN_LOGE("cannot encode record 0x%04x: %s", agent_wire::record_type(record),
                       agent_wire::error_name(error));
            return false;
        }
    }
    wake();
    return true;
}

void link::wake() {
    const uint64_t one = 1;
    if (wake_fd >= 0 && write(wake_fd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
        CROWN_LOGE("cannot wake the crown_agent link: %s", strerror(errno));
    }
}

void link::wait_for(std::chrono::milliseconds delay) {
    pollfd waker{wake_fd, POLLIN, 0};
    if (poll(&waker, 1, static_cast<int>(delay.count())) > 0) {
        uint64_t count;
        while (read(wake_fd, &count, sizeof(count)) > 0) {
        }
    }
}

void link::run() {
    auto retry = config.first_retry;
    while (!stopping) {
        if (config.safe_mode && config.safe_mode()) {
            wait_for(config.longest_retry);
            continue;
        }
        const int socket_fd = connect_unix(config.socket_path);
        if (socket_fd >= 0) {
            const session_end end = session(socket_fd);
            close(socket_fd);
            if (active.exchange(false)) {
                listener.on_agent_disconnected();
                retry = config.first_retry;
            }
            if (end == session_end::stopped) {
                break;
            }
            if (end == session_end::refused) {
                retry = config.longest_retry;
            }
        }
        wait_for(retry);
        retry = std::min(retry * 2, config.longest_retry);
    }
}

bool link::handshake(int socket_fd, std::vector<uint8_t> &received) {
    std::vector<uint8_t> hello;
    if (agent_wire::encode(agent_wire::Hello{agent_wire::wire_version, config.bundle}, hello) !=
        agent_wire::Error::none) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + config.handshake_timeout;
    while (!stopping) {
        if (!hello.empty() && !write_available(socket_fd, hello)) {
            return false;
        }
        const agent_wire::Decoded decoded = agent_wire::decode(received.data(), received.size());
        if (decoded.status == agent_wire::DecodeStatus::failed) {
            CROWN_LOGE("crown_agent sent a bad first record: %s", agent_wire::error_name(decoded.error));
            return false;
        }
        if (decoded.status == agent_wire::DecodeStatus::record) {
            received.erase(received.begin(), received.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));
            const auto *peer = std::get_if<agent_wire::Hello>(&decoded.record);
            if (!peer) {
                CROWN_LOGE("crown_agent did not start with Hello");
                return false;
            }
            if (peer->wire != agent_wire::wire_version || peer->bundle != config.bundle) {
                CROWN_LOGW("crown_agent speaks wire %u bundle '%s', we are wire %u bundle '%s': safe mode",
                           peer->wire, peer->bundle.c_str(), agent_wire::wire_version, config.bundle.c_str());
                return false;
            }
            return hello.empty() || write_available(socket_fd, hello);
        }
        const int timeout = remaining_ms(deadline);
        if (timeout == 0) {
            CROWN_LOGE("crown_agent did not answer Hello in time");
            return false;
        }
        pollfd fds[] = {{socket_fd, static_cast<short>(POLLIN | (hello.empty() ? 0 : POLLOUT)), 0},
                        {wake_fd, POLLIN, 0}};
        if (poll(fds, 2, timeout) < 0 && errno != EINTR) {
            return false;
        }
        if ((fds[0].revents & POLLIN) && !read_available(socket_fd, received)) {
            return false;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL) && !(fds[0].revents & POLLIN)) {
            return false;
        }
        if (fds[1].revents & POLLIN) {
            uint64_t count;
            while (read(wake_fd, &count, sizeof(count)) > 0) {
            }
        }
    }
    return false;
}

bool link::pump(std::vector<uint8_t> &received) {
    size_t offset = 0;
    while (offset < received.size()) {
        const agent_wire::Decoded decoded =
            agent_wire::decode(received.data() + offset, received.size() - offset);
        if (decoded.status == agent_wire::DecodeStatus::incomplete) {
            break;
        }
        if (decoded.status == agent_wire::DecodeStatus::failed) {
            CROWN_LOGE("dropping crown_agent: record 0x%04x: %s", decoded.type,
                       agent_wire::error_name(decoded.error));
            return false;
        }
        offset += decoded.consumed;
        listener.on_agent_record(decoded.record);
    }
    received.erase(received.begin(), received.begin() + static_cast<std::ptrdiff_t>(offset));
    return true;
}

bool link::flush_outgoing(int socket_fd) {
    std::lock_guard lock(outgoing_mutex);
    return outgoing.empty() || write_available(socket_fd, outgoing);
}

link::session_end link::session(int socket_fd) {
    std::vector<uint8_t> received;
    {
        std::lock_guard lock(outgoing_mutex);
        outgoing.clear();
    }
    if (!handshake(socket_fd, received)) {
        return stopping ? session_end::stopped : session_end::refused;
    }

    active.store(true, std::memory_order_release);
    CROWN_LOGI("crown_agent connected (wire %u, bundle '%s')", agent_wire::wire_version,
               config.bundle.c_str());
    listener.on_agent_connected();

    while (!stopping) {
        if (!pump(received) || !flush_outgoing(socket_fd)) {
            return session_end::closed;
        }
        if (config.safe_mode && config.safe_mode()) {
            CROWN_LOGI("safe mode turned on; leaving crown_agent");
            return session_end::closed;
        }
        bool has_outgoing;
        {
            std::lock_guard lock(outgoing_mutex);
            has_outgoing = !outgoing.empty();
        }
        pollfd fds[] = {{socket_fd, static_cast<short>(POLLIN | (has_outgoing ? POLLOUT : 0)), 0},
                        {wake_fd, POLLIN, 0}};
        if (poll(fds, 2, safe_mode_check_ms) < 0 && errno != EINTR) {
            return session_end::closed;
        }
        if (fds[1].revents & POLLIN) {
            uint64_t count;
            while (read(wake_fd, &count, sizeof(count)) > 0) {
            }
        }
        if (fds[0].revents & POLLIN) {
            if (!read_available(socket_fd, received)) {
                pump(received);
                CROWN_LOGI("crown_agent closed the connection");
                return session_end::closed;
            }
        } else if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            CROWN_LOGI("crown_agent connection lost");
            return session_end::closed;
        }
    }
    return session_end::stopped;
}

} // namespace agent
