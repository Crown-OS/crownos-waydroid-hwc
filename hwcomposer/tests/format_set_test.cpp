/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "dmabuf/format_set.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace dmabuf;

namespace {

constexpr uint32_t argb8888 = 0x34325241;
constexpr uint32_t xrgb8888 = 0x34325258;
constexpr uint32_t nv12 = 0x3231564e;
constexpr uint64_t linear = 0;
constexpr uint64_t nvidia_block_linear = 0x0300000000606014ULL;

std::vector<uint8_t> table_of(const std::vector<format_modifier> &pairs) {
    std::vector<uint8_t> bytes(pairs.size() * 16, 0xEE);
    for (size_t i = 0; i < pairs.size(); ++i) {
        std::memcpy(&bytes[i * 16], &pairs[i].format, 4);
        std::memcpy(&bytes[i * 16 + 8], &pairs[i].modifier, 8);
    }
    return bytes;
}

} // namespace

TEST(FormatSet, OnlyAdvertisedPairsAreImportable) {
    format_set formats;
    formats.add(argb8888, nvidia_block_linear);
    formats.add(xrgb8888, linear);
    formats.add(argb8888, nvidia_block_linear);
    EXPECT_EQ(formats.entries().size(), 2u);
    EXPECT_TRUE(formats.contains(argb8888, nvidia_block_linear));
    EXPECT_FALSE(formats.contains(argb8888, linear));
    EXPECT_FALSE(formats.contains(nv12, linear));
    EXPECT_TRUE(formats.has_format(xrgb8888));
    EXPECT_FALSE(formats.has_format(nv12));
}

TEST(FormatSet, FeedbackTranchesPickPairsFromTheTable) {
    const auto table = table_of({{argb8888, linear}, {argb8888, nvidia_block_linear},
                                 {xrgb8888, linear}, {nv12, linear}});
    feedback_collector collector;
    collector.set_table(table.data(), table.size());
    const uint16_t scanout[] = {1};
    const uint16_t render[] = {0, 2, 9};
    collector.add_tranche_indices(scanout, 1);
    collector.add_tranche_indices(render, 3);
    const format_set formats = collector.done();
    EXPECT_EQ(formats.entries().size(), 3u);
    EXPECT_TRUE(formats.contains(argb8888, nvidia_block_linear));
    EXPECT_TRUE(formats.contains(xrgb8888, linear));
    EXPECT_FALSE(formats.contains(nv12, linear));
}

TEST(FormatSet, EachFeedbackStartsEmptyButKeepsTheTable) {
    const auto table = table_of({{argb8888, linear}, {nv12, linear}});
    feedback_collector collector;
    collector.set_table(table.data(), table.size());
    const uint16_t first[] = {0};
    collector.add_tranche_indices(first, 1);
    EXPECT_TRUE(collector.done().contains(argb8888, linear));
    const uint16_t second[] = {1};
    collector.add_tranche_indices(second, 1);
    const format_set formats = collector.done();
    EXPECT_FALSE(formats.contains(argb8888, linear));
    EXPECT_TRUE(formats.contains(nv12, linear));
}
