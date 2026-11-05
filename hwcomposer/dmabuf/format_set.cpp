/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "dmabuf/format_set.h"

#include <algorithm>
#include <cstring>

namespace dmabuf {
namespace {

constexpr size_t table_entry_bytes = 16;

} // namespace

void format_set::add(uint32_t format, uint64_t modifier) {
    const format_modifier pair{format, modifier};
    const auto at = std::lower_bound(pairs.begin(), pairs.end(), pair);
    if (at == pairs.end() || !(*at == pair)) {
        pairs.insert(at, pair);
    }
}

bool format_set::contains(uint32_t format, uint64_t modifier) const {
    return std::binary_search(pairs.begin(), pairs.end(), format_modifier{format, modifier});
}

bool format_set::has_format(uint32_t format) const {
    const auto at = std::lower_bound(pairs.begin(), pairs.end(), format_modifier{format, 0});
    return at != pairs.end() && at->format == format;
}

void feedback_collector::set_table(const uint8_t *bytes, size_t size) {
    table.resize(size / table_entry_bytes);
    for (size_t i = 0; i < table.size(); ++i) {
        const uint8_t *entry = bytes + i * table_entry_bytes;
        std::memcpy(&table[i].format, entry, sizeof(uint32_t));
        std::memcpy(&table[i].modifier, entry + 8, sizeof(uint64_t));
    }
}

void feedback_collector::add_tranche_indices(const uint16_t *indices, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (indices[i] >= table.size()) {
            continue;
        }
        const format_modifier &pair = table[indices[i]];
        if (!collected.formats.contains(pair.format, pair.modifier)) {
            collected.formats.add(pair.format, pair.modifier);
            collected.preference.push_back(pair);
        }
    }
}

feedback feedback_collector::done() {
    feedback result = std::move(collected);
    collected = {};
    return result;
}

} // namespace dmabuf
