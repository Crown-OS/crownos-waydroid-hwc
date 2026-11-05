/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 *
 * The HWC's crown_agent link against a fake agent on a Unix socket.
 */

#include "agent/link.h"

#include <gtest/gtest.h>

#include <condition_variable>
#include <cstdlib>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace agent_wire;
using namespace std::chrono_literals;

namespace {

constexpr const char *bundle = "2026.11.0";
constexpr auto patience = 3s;

class fake_agent {
  public:
    fake_agent() {
        const char *temporary = std::getenv("TMPDIR");
        std::string pattern = std::string(temporary ? temporary : "/tmp") + "/crown-agent-XXXXXX";
        directory = mkdtemp(pattern.data());
        path = directory + "/crown_agent";
        listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        EXPECT_EQ(bind(listen_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
        EXPECT_EQ(listen(listen_fd, 4), 0);
    }
    ~fake_agent() {
        if (client_fd >= 0) {
            close(client_fd);
        }
        close(listen_fd);
        unlink(path.c_str());
        rmdir(directory.c_str());
    }

    bool accept_client(std::chrono::milliseconds timeout = patience) {
        pollfd waiter{listen_fd, POLLIN, 0};
        if (poll(&waiter, 1, static_cast<int>(timeout.count())) <= 0) {
            return false;
        }
        client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        return client_fd >= 0;
    }

    void drop_client() {
        close(client_fd);
        client_fd = -1;
        received.clear();
    }

    std::optional<Record> read_record() {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < deadline) {
            const Decoded decoded = decode(received.data(), received.size());
            if (decoded.status == DecodeStatus::record) {
                received.erase(received.begin(), received.begin() + decoded.consumed);
                return decoded.record;
            }
            pollfd waiter{client_fd, POLLIN, 0};
            if (poll(&waiter, 1, 50) <= 0) {
                continue;
            }
            uint8_t chunk[512];
            const ssize_t count = recv(client_fd, chunk, sizeof(chunk), 0);
            if (count <= 0) {
                return std::nullopt;
            }
            received.insert(received.end(), chunk, chunk + count);
        }
        return std::nullopt;
    }

    bool peer_closed() {
        pollfd waiter{client_fd, POLLIN, 0};
        if (poll(&waiter, 1, static_cast<int>(std::chrono::milliseconds(patience).count())) <= 0) {
            return false;
        }
        uint8_t byte;
        return recv(client_fd, &byte, 1, 0) == 0;
    }

    void write_bytes(const std::vector<uint8_t> &bytes) {
        ASSERT_EQ(write(client_fd, bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
    }

    void write_record(const Record &record) {
        std::vector<uint8_t> bytes;
        ASSERT_EQ(encode(record, bytes), Error::none);
        write_bytes(bytes);
    }

    void greet(const std::string &agent_bundle = bundle) {
        ASSERT_TRUE(accept_client());
        auto hello = read_record();
        ASSERT_TRUE(hello);
        EXPECT_TRUE(same(*hello, Hello{wire_version, bundle}));
        write_record(Hello{wire_version, agent_bundle});
    }

    std::string directory;
    std::string path;
    int listen_fd = -1;
    int client_fd = -1;
    std::vector<uint8_t> received;
};

class recording_listener : public agent::link_listener {
  public:
    void on_agent_connected() override { update([this] { ++connects; }); }
    void on_agent_record(const Record &record) override {
        update([&] { records.push_back(record); });
    }
    void on_agent_disconnected() override { update([this] { ++disconnects; }); }

    template <class Predicate>
    bool wait(Predicate predicate) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, patience, predicate);
    }

    std::mutex mutex;
    std::condition_variable changed;
    int connects = 0;
    int disconnects = 0;
    std::vector<Record> records;

  private:
    template <class Change>
    void update(Change change) {
        {
            std::lock_guard lock(mutex);
            change();
        }
        changed.notify_all();
    }
};

agent::link_config config_for(const fake_agent &agent, bool *safe_mode = nullptr) {
    agent::link_config config;
    config.socket_path = agent.path;
    config.bundle = bundle;
    config.safe_mode = [safe_mode] { return safe_mode && *safe_mode; };
    config.first_retry = 20ms;
    config.longest_retry = 200ms;
    config.handshake_timeout = 1s;
    return config;
}

} // namespace

TEST(AgentLink, RecordsFlowBothWaysAfterHello) {
    fake_agent agent;
    recording_listener listener;
    agent::link link(config_for(agent), listener);
    EXPECT_FALSE(link.send(Focus{1}));
    link.start();

    agent.greet();
    ASSERT_TRUE(listener.wait([&] { return listener.connects == 1; }));
    EXPECT_TRUE(link.connected());

    ASSERT_TRUE(link.send(SetSize{7, 1280, 800, 42}));
    auto set_size = agent.read_record();
    ASSERT_TRUE(set_size);
    EXPECT_TRUE(same(*set_size, SetSize{7, 1280, 800, 42}));

    agent.write_record(TaskInfo{7, "com.whatsapp", "WhatsApp", 0xFF075E54, true, 320, 480});
    agent.write_record(SlotApplied{7, 42, Rect{0, 0, 1280, 800}});
    ASSERT_TRUE(listener.wait([&] { return listener.records.size() == 2; }));
    EXPECT_TRUE(same(listener.records[1], SlotApplied{7, 42, Rect{0, 0, 1280, 800}}));
}

TEST(AgentLink, RecordsSplitAcrossWritesStillArrive) {
    fake_agent agent;
    recording_listener listener;
    agent::link link(config_for(agent), listener);
    link.start();
    agent.greet();
    ASSERT_TRUE(listener.wait([&] { return listener.connects == 1; }));

    std::vector<uint8_t> bytes;
    ASSERT_EQ(encode(Activate{7, "tok-1"}, bytes), Error::none);
    const std::vector<uint8_t> head(bytes.begin(), bytes.begin() + 5);
    const std::vector<uint8_t> tail(bytes.begin() + 5, bytes.end());
    agent.write_bytes(head);
    std::this_thread::sleep_for(50ms);
    agent.write_bytes(tail);
    ASSERT_TRUE(listener.wait([&] { return listener.records.size() == 1; }));
    EXPECT_TRUE(same(listener.records[0], Activate{7, "tok-1"}));
}

TEST(AgentLink, BundleMismatchMeansSafeMode) {
    fake_agent agent;
    recording_listener listener;
    agent::link link(config_for(agent), listener);
    link.start();

    agent.greet("2025.01.0");
    EXPECT_TRUE(agent.peer_closed());
    EXPECT_EQ(listener.connects, 0);
    EXPECT_FALSE(link.connected());
    EXPECT_FALSE(link.send(Focus{7}));
}

TEST(AgentLink, WireMismatchMeansSafeMode) {
    fake_agent agent;
    recording_listener listener;
    agent::link link(config_for(agent), listener);
    link.start();

    ASSERT_TRUE(agent.accept_client());
    ASSERT_TRUE(agent.read_record());
    agent.write_record(Hello{wire_version + 1, bundle});
    EXPECT_TRUE(agent.peer_closed());
    EXPECT_FALSE(link.connected());
}

TEST(AgentLink, ReconnectsWhenTheAgentRestarts) {
    fake_agent agent;
    recording_listener listener;
    agent::link link(config_for(agent), listener);
    link.start();

    agent.greet();
    ASSERT_TRUE(listener.wait([&] { return listener.connects == 1; }));
    agent.drop_client();
    ASSERT_TRUE(listener.wait([&] { return listener.disconnects == 1; }));
    EXPECT_FALSE(link.connected());

    agent.greet();
    ASSERT_TRUE(listener.wait([&] { return listener.connects == 2; }));
    EXPECT_TRUE(link.connected());
}

TEST(AgentLink, MalformedRecordDropsTheConnection) {
    fake_agent agent;
    recording_listener listener;
    agent::link link(config_for(agent), listener);
    link.start();
    agent.greet();
    ASSERT_TRUE(listener.wait([&] { return listener.connects == 1; }));

    agent.write_bytes({0x04, 0x00, 0x00, 0x00, 0x99, 0x99, 0x00, 0x00});
    ASSERT_TRUE(listener.wait([&] { return listener.disconnects == 1; }));
    EXPECT_TRUE(agent.peer_closed());
}

TEST(AgentLink, SafeModeKeepsTheLinkDown) {
    fake_agent agent;
    recording_listener listener;
    bool safe_mode = true;
    agent::link link(config_for(agent, &safe_mode), listener);
    link.start();
    EXPECT_FALSE(agent.accept_client(300ms));
    link.stop();
    EXPECT_EQ(listener.connects, 0);
}

TEST(AgentLink, AgentThatNeverAnswersIsLeft) {
    fake_agent agent;
    recording_listener listener;
    agent::link link(config_for(agent), listener);
    link.start();
    ASSERT_TRUE(agent.accept_client());
    ASSERT_TRUE(agent.read_record());
    EXPECT_TRUE(agent.peer_closed());
    EXPECT_EQ(listener.connects, 0);
}
