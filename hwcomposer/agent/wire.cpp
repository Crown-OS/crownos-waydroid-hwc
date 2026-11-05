/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "agent/wire.h"

#include <limits>
#include <optional>
#include <type_traits>

namespace agent_wire {
namespace {

constexpr size_t len_field_bytes = 4;
constexpr size_t max_u16 = std::numeric_limits<uint16_t>::max();

bool is_utf8(const uint8_t *bytes, size_t size) {
    size_t i = 0;
    while (i < size) {
        const uint8_t lead = bytes[i];
        size_t continuation;
        uint32_t code_point;
        if (lead < 0x80) {
            ++i;
            continue;
        } else if ((lead & 0xE0) == 0xC0) {
            continuation = 1;
            code_point = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            continuation = 2;
            code_point = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            continuation = 3;
            code_point = lead & 0x07;
        } else {
            return false;
        }
        if (size - i <= continuation) {
            return false;
        }
        for (size_t c = 1; c <= continuation; ++c) {
            const uint8_t next = bytes[i + c];
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (next & 0x3F);
        }
        constexpr uint32_t smallest_for_length[] = {0, 0x80, 0x800, 0x10000};
        const bool surrogate = code_point >= 0xD800 && code_point <= 0xDFFF;
        if (code_point < smallest_for_length[continuation] || code_point > 0x10FFFF || surrogate) {
            return false;
        }
        i += continuation + 1;
    }
    return true;
}

class Writer {
  public:
    explicit Writer(std::vector<uint8_t> &out) : out(out) {}

    void put(uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<uint8_t>(value >> shift));
        }
    }
    void put(int32_t value) { put(static_cast<uint32_t>(value)); }
    void put(bool value) { put(static_cast<uint32_t>(value ? 1 : 0)); }
    void put_u16(size_t value) {
        out.push_back(static_cast<uint8_t>(value));
        out.push_back(static_cast<uint8_t>(value >> 8));
    }
    void put(const std::string &value) {
        if (value.size() > max_u16) {
            error = Error::string_too_long;
            return;
        }
        put_u16(value.size());
        out.insert(out.end(), value.begin(), value.end());
    }
    void put(const std::vector<uint32_t> &values) {
        if (values.size() > max_u16) {
            error = Error::list_too_long;
            return;
        }
        put_u16(values.size());
        for (uint32_t value : values) {
            put(value);
        }
    }
    void put(const Rect &rect) { put_fields(rect); }

    template <class R>
    void put_fields(const R &record) {
        std::apply([&](const auto &...fields) { (put(fields), ...); }, record.tie());
    }

    Error error = Error::none;

  private:
    std::vector<uint8_t> &out;
};

class Reader {
  public:
    Reader(const uint8_t *data, size_t size) : data(data), left(size) {}

    bool get(uint32_t &value) {
        if (left < 4) {
            return false;
        }
        value = uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 |
                uint32_t(data[3]) << 24;
        advance(4);
        return true;
    }
    bool get(int32_t &value) {
        uint32_t raw;
        if (!get(raw)) {
            return false;
        }
        value = static_cast<int32_t>(raw);
        return true;
    }
    bool get(bool &value) {
        uint32_t raw;
        if (!get(raw) || raw > 1) {
            return false;
        }
        value = raw == 1;
        return true;
    }
    bool get_u16(size_t &value) {
        if (left < 2) {
            return false;
        }
        value = size_t(data[0]) | size_t(data[1]) << 8;
        advance(2);
        return true;
    }
    bool get(std::string &value) {
        size_t size;
        if (!get_u16(size) || left < size || !is_utf8(data, size)) {
            return false;
        }
        value.assign(reinterpret_cast<const char *>(data), size);
        advance(size);
        return true;
    }
    bool get(std::vector<uint32_t> &values) {
        size_t count;
        if (!get_u16(count) || left < count * 4) {
            return false;
        }
        values.resize(count);
        for (uint32_t &value : values) {
            get(value);
        }
        return true;
    }
    bool get(Rect &rect) { return get_fields(rect); }

    template <class R>
    bool get_fields(R &record) {
        return std::apply([&](auto &...fields) { return (get(fields) && ...); }, record.tie());
    }

    bool empty() const { return left == 0; }

  private:
    void advance(size_t count) {
        data += count;
        left -= count;
    }

    const uint8_t *data;
    size_t left;
};

uint32_t read_u32(const uint8_t *bytes) {
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 |
           uint32_t(bytes[3]) << 24;
}

template <size_t Index = 0>
std::optional<Record> parse_payload(uint16_t type, Reader &reader, bool &known) {
    if constexpr (Index < std::variant_size_v<Record>) {
        using Alternative = std::variant_alternative_t<Index, Record>;
        if (Alternative::type != type) {
            return parse_payload<Index + 1>(type, reader, known);
        }
        known = true;
        Alternative record;
        if (!reader.get_fields(record) || !reader.empty()) {
            return std::nullopt;
        }
        return Record{std::move(record)};
    } else {
        (void)type;
        (void)reader;
        return std::nullopt;
    }
}

Decoded failure(Error error, uint16_t type) {
    Decoded decoded;
    decoded.status = DecodeStatus::failed;
    decoded.error = error;
    decoded.type = type;
    return decoded;
}

} // namespace

const char *error_name(Error error) {
    switch (error) {
        case Error::none:
            return "none";
        case Error::too_long:
            return "record exceeds the size limit";
        case Error::unknown_type:
            return "unknown record type";
        case Error::malformed:
            return "malformed record";
        case Error::string_too_long:
            return "string field longer than 65535 bytes";
        case Error::list_too_long:
            return "list field with more than 65535 items";
    }
    return "unknown error";
}

uint16_t record_type(const Record &record) {
    return std::visit([](const auto &alternative) { return std::decay_t<decltype(alternative)>::type; },
                      record);
}

Error encode(const Record &record, std::vector<uint8_t> &out) {
    const size_t start = out.size();
    Writer writer(out);
    writer.put(uint32_t{0});
    writer.put_u16(record_type(record));
    writer.put_u16(0);
    std::visit([&writer](const auto &alternative) { writer.put_fields(alternative); }, record);

    const size_t len = out.size() - start - len_field_bytes;
    Error error = writer.error;
    if (error == Error::none && len > max_record_len) {
        error = Error::too_long;
    }
    if (error != Error::none) {
        out.resize(start);
        return error;
    }
    for (size_t i = 0; i < len_field_bytes; ++i) {
        out[start + i] = static_cast<uint8_t>(len >> (8 * i));
    }
    return Error::none;
}

Decoded decode(const uint8_t *data, size_t size) {
    if (size < len_field_bytes) {
        return {};
    }
    const size_t len = read_u32(data);
    if (len > max_record_len) {
        return failure(Error::too_long, 0);
    }
    if (len < header_len - len_field_bytes) {
        return failure(Error::malformed, 0);
    }
    if (size - len_field_bytes < len) {
        return {};
    }
    const uint16_t type = static_cast<uint16_t>(data[4] | data[5] << 8);
    Reader reader(data + header_len, len - (header_len - len_field_bytes));
    bool known = false;
    std::optional<Record> record = parse_payload(type, reader, known);
    if (!record) {
        return failure(known ? Error::malformed : Error::unknown_type, type);
    }
    Decoded decoded;
    decoded.status = DecodeStatus::record;
    decoded.record = std::move(*record);
    decoded.consumed = len_field_bytes + len;
    decoded.type = type;
    return decoded;
}

bool same(const Record &left, const Record &right) {
    if (left.index() != right.index()) {
        return false;
    }
    return std::visit(
        [&right](const auto &alternative) {
            using Alternative = std::decay_t<decltype(alternative)>;
            const auto *other = std::get_if<Alternative>(&right);
            return other && alternative.tie() == other->tie();
        },
        left);
}

} // namespace agent_wire
