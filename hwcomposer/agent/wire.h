/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 *
 * The agent-wire v1 record format crown-agent speaks on /dev/socket/crown_agent
 * (HWC) and /data/crownos/agent.sock (daemon). A record is `len: u32`,
 * `type: u16`, `flags: u16`, then the payload, all little-endian; `len` counts
 * every byte after itself. Self-contained: no Android dependencies, so the
 * same code is tested on the host against contracts/agent-wire-vectors.txt.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace agent_wire {

constexpr uint32_t wire_version = 1;
constexpr size_t max_record_len = 64 * 1024;

/// Bytes of `len` plus `type` and `flags`.
constexpr size_t header_len = 8;

#define AGENT_WIRE_FIELDS(...)                                 \
    auto tie() { return std::tie(__VA_ARGS__); }               \
    auto tie() const { return std::tie(__VA_ARGS__); }

/// A rectangle in Android display pixels.
struct Rect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
    AGENT_WIRE_FIELDS(left, top, right, bottom)

    int32_t width() const { return right - left; }
    int32_t height() const { return bottom - top; }
    bool empty() const { return width() <= 0 || height() <= 0; }

    friend bool operator==(const Rect &a, const Rect &b) { return a.tie() == b.tie(); }
    friend bool operator!=(const Rect &a, const Rect &b) { return !(a == b); }
};

struct Hello {
    static constexpr uint16_t type = 0x0001;
    uint32_t wire = 0;
    std::string bundle;
    AGENT_WIRE_FIELDS(wire, bundle)
};

struct SetSize {
    static constexpr uint16_t type = 0x0101;
    uint32_t task = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t serial = 0;
    AGENT_WIRE_FIELDS(task, width, height, serial)
};

struct Focus {
    static constexpr uint16_t type = 0x0102;
    uint32_t task = 0;
    AGENT_WIRE_FIELDS(task)
};

struct Close {
    static constexpr uint16_t type = 0x0103;
    uint32_t task = 0;
    AGENT_WIRE_FIELDS(task)
};

struct Back {
    static constexpr uint16_t type = 0x0104;
    uint32_t task = 0;
    AGENT_WIRE_FIELDS(task)
};

struct Visibility {
    static constexpr uint16_t type = 0x0105;
    uint32_t task = 0;
    bool visible = false;
    AGENT_WIRE_FIELDS(task, visible)
};

struct TaskInfo {
    static constexpr uint16_t type = 0x0201;
    uint32_t task = 0;
    std::string package;
    std::string label;
    uint32_t color = 0;
    bool resizable = false;
    uint32_t min_width = 0;
    uint32_t min_height = 0;
    AGENT_WIRE_FIELDS(task, package, label, color, resizable, min_width, min_height)
};

struct SlotApplied {
    static constexpr uint16_t type = 0x0202;
    uint32_t task = 0;
    uint32_t serial = 0;
    Rect slot;
    AGENT_WIRE_FIELDS(task, serial, slot)
};

struct TaskRemoved {
    static constexpr uint16_t type = 0x0203;
    uint32_t task = 0;
    AGENT_WIRE_FIELDS(task)
};

struct Activate {
    static constexpr uint16_t type = 0x0204;
    uint32_t task = 0;
    std::string token;
    AGENT_WIRE_FIELDS(task, token)
};

struct Launch {
    static constexpr uint16_t type = 0x0301;
    std::string package;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string token;
    AGENT_WIRE_FIELDS(package, width, height, token)
};

struct ListTasks {
    static constexpr uint16_t type = 0x0302;
    AGENT_WIRE_FIELDS()
};

struct QueryKeepAlive {
    static constexpr uint16_t type = 0x0303;
    AGENT_WIRE_FIELDS()
};

struct QueryGsfId {
    static constexpr uint16_t type = 0x0304;
    AGENT_WIRE_FIELDS()
};

struct Shutdown {
    static constexpr uint16_t type = 0x0305;
    AGENT_WIRE_FIELDS()
};

struct OpenUri {
    static constexpr uint16_t type = 0x0306;
    std::string uri;
    AGENT_WIRE_FIELDS(uri)
};

struct InstallPackage {
    static constexpr uint16_t type = 0x0307;
    std::string path;
    AGENT_WIRE_FIELDS(path)
};

struct Ready {
    static constexpr uint16_t type = 0x0401;
    uint32_t boot_ms = 0;
    AGENT_WIRE_FIELDS(boot_ms)
};

struct TaskOpened {
    static constexpr uint16_t type = 0x0402;
    uint32_t task = 0;
    std::string package;
    AGENT_WIRE_FIELDS(task, package)
};

struct TaskClosed {
    static constexpr uint16_t type = 0x0403;
    uint32_t task = 0;
    AGENT_WIRE_FIELDS(task)
};

struct Tasks {
    static constexpr uint16_t type = 0x0404;
    std::vector<uint32_t> tasks;
    AGENT_WIRE_FIELDS(tasks)
};

struct KeepAlive {
    static constexpr uint16_t type = 0x0405;
    static constexpr uint32_t audio = 1u << 0;
    static constexpr uint32_t call = 1u << 1;
    static constexpr uint32_t install = 1u << 2;
    static constexpr uint32_t download = 1u << 3;
    static constexpr uint32_t foreground_service = 1u << 4;
    uint32_t reasons = 0;
    AGENT_WIRE_FIELDS(reasons)
};

struct GsfId {
    static constexpr uint16_t type = 0x0406;
    std::string id;
    AGENT_WIRE_FIELDS(id)
};

struct LaunchFailed {
    static constexpr uint16_t type = 0x0407;
    std::string package;
    std::string reason;
    AGENT_WIRE_FIELDS(package, reason)
};

struct InstallResult {
    static constexpr uint16_t type = 0x0408;
    std::string path;
    bool ok = false;
    std::string message;
    AGENT_WIRE_FIELDS(path, ok, message)
};

#undef AGENT_WIRE_FIELDS

using Record = std::variant<Hello, SetSize, Focus, Close, Back, Visibility, TaskInfo, SlotApplied,
                            TaskRemoved, Activate, Launch, ListTasks, QueryKeepAlive, QueryGsfId,
                            Shutdown, OpenUri, InstallPackage, Ready, TaskOpened, TaskClosed, Tasks,
                            KeepAlive, GsfId, LaunchFailed, InstallResult>;

enum class Error {
    none,
    too_long,
    unknown_type,
    malformed,
    string_too_long,
    list_too_long,
};

const char *error_name(Error error);

/// The wire `type` of a record.
uint16_t record_type(const Record &record);

/// Appends `record` with its header to `out`. On error `out` is unchanged.
Error encode(const Record &record, std::vector<uint8_t> &out);

enum class DecodeStatus {
    record,
    incomplete,
    failed,
};

struct Decoded {
    DecodeStatus status = DecodeStatus::incomplete;
    Record record;
    /// Bytes the record used, valid for DecodeStatus::record.
    size_t consumed = 0;
    /// Why decoding failed, valid for DecodeStatus::failed. A peer that gets
    /// one drops the connection.
    Error error = Error::none;
    uint16_t type = 0;
};

/// Reads one record from the start of `data`.
Decoded decode(const uint8_t *data, size_t size);

/// Field-wise equality of two records.
bool same(const Record &left, const Record &right);

} // namespace agent_wire
