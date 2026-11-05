/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "agent/wire.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agent {

struct link_config {
    std::string socket_path;
    /// `ro.crownos.bundle`, sent in Hello; the agent must report the same one.
    std::string bundle;
    /// `persist.crownos.safe_mode == 1`; checked before every connect and
    /// while connected. Safe mode keeps the HWC on stock multi-window.
    std::function<bool()> safe_mode;
    std::chrono::milliseconds first_retry{100};
    std::chrono::milliseconds longest_retry{5000};
    std::chrono::milliseconds handshake_timeout{2000};
};

/// Receives what the agent says. Every call comes from the link thread.
class link_listener {
  public:
    virtual ~link_listener() = default;
    virtual void on_agent_connected() = 0;
    virtual void on_agent_record(const agent_wire::Record &record) = 0;
    virtual void on_agent_disconnected() = 0;
};

/// The HWC side of /dev/socket/crown_agent. All socket IO happens on its own
/// thread; send() only queues bytes, so the composition thread never blocks.
class link {
  public:
    link(link_config config, link_listener &listener);
    ~link();

    link(const link &) = delete;
    link &operator=(const link &) = delete;

    void start();
    void stop();

    /// Queues a record for the agent. False while no agent is connected, so
    /// callers can take their fallback path.
    bool send(const agent_wire::Record &record);

    /// True between a matching Hello and the end of that connection.
    bool connected() const { return active.load(std::memory_order_acquire); }

  private:
    enum class session_end { stopped, closed, refused };

    void run();
    session_end session(int socket_fd);
    bool handshake(int socket_fd, std::vector<uint8_t> &received);
    bool pump(std::vector<uint8_t> &received);
    bool flush_outgoing(int socket_fd);
    void wait_for(std::chrono::milliseconds delay);
    void wake();

    link_config config;
    link_listener &listener;
    std::thread thread;
    std::atomic<bool> stopping{false};
    std::atomic<bool> active{false};
    int wake_fd = -1;

    std::mutex outgoing_mutex;
    std::vector<uint8_t> outgoing;
};

} // namespace agent
