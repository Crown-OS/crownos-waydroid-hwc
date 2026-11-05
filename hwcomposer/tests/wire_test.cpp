/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "agent/wire.h"

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <sstream>

using namespace agent_wire;

namespace {

std::vector<std::pair<std::string, Record>> samples() {
    return {
        {"hello", Hello{1, "2026.11.0"}},
        {"set_size", SetSize{7, 1280, 800, 42}},
        {"focus", Focus{7}},
        {"close", Close{7}},
        {"back", Back{7}},
        {"visibility", Visibility{7, false}},
        {"task_info", TaskInfo{7, "com.whatsapp", "WhatsApp", 0xFF075E54, true, 320, 480}},
        {"slot_applied", SlotApplied{7, 42, Rect{0, 0, 1280, 800}}},
        {"task_removed", TaskRemoved{7}},
        {"activate", Activate{7, "tok-1"}},
        {"launch", Launch{"com.whatsapp", 1280, 800, ""}},
        {"list_tasks", ListTasks{}},
        {"query_keep_alive", QueryKeepAlive{}},
        {"query_gsf_id", QueryGsfId{}},
        {"shutdown", Shutdown{}},
        {"open_uri", OpenUri{"market://details?id=com.whatsapp"}},
        {"install_package", InstallPackage{"app.apk"}},
        {"ready", Ready{9500}},
        {"task_opened", TaskOpened{7, "com.whatsapp"}},
        {"task_closed", TaskClosed{7}},
        {"tasks", Tasks{{7, 9}}},
        {"keep_alive", KeepAlive{KeepAlive::audio | KeepAlive::download}},
        {"gsf_id", GsfId{"3a9f"}},
        {"launch_failed", LaunchFailed{"com.example", "not installed"}},
        {"install_result", InstallResult{"app.apk", true, ""}},
    };
}

std::vector<std::pair<std::string, std::string>> golden_vectors() {
    std::ifstream file(AGENT_WIRE_VECTORS);
    std::vector<std::pair<std::string, std::string>> vectors;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream fields(line);
        std::string name, hex;
        fields >> name >> hex;
        vectors.emplace_back(name, hex);
    }
    return vectors;
}

std::string to_hex(const std::vector<uint8_t> &bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (uint8_t byte : bytes) {
        hex += digits[byte >> 4];
        hex += digits[byte & 0xF];
    }
    return hex;
}

std::vector<uint8_t> from_hex(const std::string &hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

std::vector<uint8_t> encoded(const Record &record) {
    std::vector<uint8_t> bytes;
    EXPECT_EQ(encode(record, bytes), Error::none);
    return bytes;
}

} // namespace

TEST(AgentWire, EverySampleEncodesToItsGoldenVector) {
    const auto vectors = golden_vectors();
    const auto records = samples();
    ASSERT_EQ(vectors.size(), records.size()) << "one vector per record type";
    for (size_t i = 0; i < records.size(); ++i) {
        EXPECT_EQ(vectors[i].first, records[i].first);
        EXPECT_EQ(to_hex(encoded(records[i].second)), vectors[i].second) << records[i].first;
    }
}

TEST(AgentWire, EveryGoldenVectorDecodesToItsSample) {
    const auto vectors = golden_vectors();
    const auto records = samples();
    ASSERT_EQ(vectors.size(), records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        const auto bytes = from_hex(vectors[i].second);
        const Decoded decoded = decode(bytes.data(), bytes.size());
        ASSERT_EQ(decoded.status, DecodeStatus::record) << vectors[i].first;
        EXPECT_EQ(decoded.consumed, bytes.size());
        EXPECT_TRUE(same(decoded.record, records[i].second)) << vectors[i].first;
    }
}

TEST(AgentWire, DecodedFieldsAreTheSampleValues) {
    const auto bytes = encoded(TaskInfo{7, "com.whatsapp", "WhatsApp", 0xFF075E54, true, 320, 480});
    const Decoded decoded = decode(bytes.data(), bytes.size());
    const auto *info = std::get_if<TaskInfo>(&decoded.record);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->label, "WhatsApp");
    EXPECT_EQ(info->color, 0xFF075E54u);
    EXPECT_TRUE(info->resizable);
    EXPECT_EQ(info->min_height, 480u);
}

TEST(AgentWire, PartialRecordsAskForMoreBytes) {
    const auto bytes = encoded(SlotApplied{7, 42, Rect{0, 0, 1280, 800}});
    for (size_t size = 0; size < bytes.size(); ++size) {
        EXPECT_EQ(decode(bytes.data(), size).status, DecodeStatus::incomplete) << size;
    }
}

TEST(AgentWire, BackToBackRecordsDecodeInOrder) {
    std::vector<uint8_t> stream;
    ASSERT_EQ(encode(Focus{3}, stream), Error::none);
    ASSERT_EQ(encode(Back{4}, stream), Error::none);
    const Decoded first = decode(stream.data(), stream.size());
    ASSERT_EQ(first.status, DecodeStatus::record);
    EXPECT_TRUE(same(first.record, Focus{3}));
    const Decoded second = decode(stream.data() + first.consumed, stream.size() - first.consumed);
    ASSERT_EQ(second.status, DecodeStatus::record);
    EXPECT_TRUE(same(second.record, Back{4}));
}

TEST(AgentWire, UnknownTypeIsRejected) {
    const auto bytes = from_hex("0400000099990000");
    const Decoded decoded = decode(bytes.data(), bytes.size());
    EXPECT_EQ(decoded.status, DecodeStatus::failed);
    EXPECT_EQ(decoded.error, Error::unknown_type);
}

TEST(AgentWire, OversizedLengthIsRejectedBeforeTheBodyArrives) {
    const auto bytes = from_hex("01000100");
    EXPECT_EQ(decode(bytes.data(), bytes.size()).error, Error::too_long);
}

TEST(AgentWire, TrailingAndMissingPayloadBytesAreMalformed) {
    const auto trailing = from_hex("0c000000020100000700000000000000");
    EXPECT_EQ(decode(trailing.data(), trailing.size()).error, Error::malformed);
    const auto short_payload = from_hex("060000000201000007000000");
    EXPECT_EQ(decode(short_payload.data(), short_payload.size()).error, Error::malformed);
}

TEST(AgentWire, BooleansOtherThanZeroOrOneAreMalformed) {
    const auto bytes = from_hex("0c000000050100000700000002000000");
    EXPECT_EQ(decode(bytes.data(), bytes.size()).error, Error::malformed);
}

TEST(AgentWire, InvalidUtf8IsMalformed) {
    const auto bytes = from_hex("0a00000006040000040033ff3966");
    EXPECT_EQ(decode(bytes.data(), bytes.size()).error, Error::malformed);
}

TEST(AgentWire, OverlongStringsAreNotEncoded) {
    std::vector<uint8_t> bytes{0xAA};
    EXPECT_EQ(encode(GsfId{std::string(70000, 'x')}, bytes), Error::string_too_long);
    EXPECT_EQ(bytes.size(), 1u);
}

TEST(AgentWire, RecordsOverTheLimitAreNotEncoded) {
    std::vector<uint8_t> bytes;
    EXPECT_EQ(encode(LaunchFailed{std::string(40000, 'a'), std::string(40000, 'b')}, bytes),
              Error::too_long);
    EXPECT_TRUE(bytes.empty());
}
