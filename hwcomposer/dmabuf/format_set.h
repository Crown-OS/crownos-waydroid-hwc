/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dmabuf {

/// DRM_FORMAT_MOD_INVALID: the buffer uses an implicit modifier.
constexpr uint64_t implicit_modifier = 0x00ffffffffffffffULL;

struct format_modifier {
    uint32_t format = 0;
    uint64_t modifier = 0;

    bool operator<(const format_modifier &other) const {
        return format != other.format ? format < other.format : modifier < other.modifier;
    }
    bool operator==(const format_modifier &other) const {
        return format == other.format && modifier == other.modifier;
    }
};

/// The (format, modifier) pairs a compositor can import.
class format_set {
  public:
    void add(uint32_t format, uint64_t modifier);
    bool contains(uint32_t format, uint64_t modifier) const;
    bool has_format(uint32_t format) const;
    bool empty() const { return pairs.empty(); }
    const std::vector<format_modifier> &entries() const { return pairs; }

  private:
    std::vector<format_modifier> pairs;
};

/// One complete linux-dmabuf feedback.
struct feedback {
    format_set formats;
    /// Every pair once, in the compositor's order of preference.
    std::vector<format_modifier> preference;
};

/// Builds a feedback from zwp_linux_dmabuf_feedback_v1 events: the format
/// table maps tranche indices to pairs, every tranche adds its pairs, and
/// `done` hands out the result.
class feedback_collector {
  public:
    /// The mapped format table: 16-byte entries of u32 format, u32 padding,
    /// u64 modifier.
    void set_table(const uint8_t *table, size_t size);
    void add_tranche_indices(const uint16_t *indices, size_t count);
    feedback done();

  private:
    std::vector<format_modifier> table;
    feedback collected;
};

} // namespace dmabuf
