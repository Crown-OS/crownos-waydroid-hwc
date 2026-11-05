/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "task/layer_role.h"

#include <algorithm>
#include <cstdint>

namespace task {
namespace {

constexpr std::string_view task_prefix = "TID:";
constexpr std::string_view input_method_prefix = "InputMethod";
constexpr std::string_view toast_name = "Toast";

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

bool parse_task_id(std::string_view digits, uint32_t &task_id) {
    if (digits.empty() || digits.size() > 10) {
        return false;
    }
    uint64_t value = 0;
    for (char digit : digits) {
        if (digit < '0' || digit > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint64_t>(digit - '0');
    }
    if (value > UINT32_MAX) {
        return false;
    }
    task_id = static_cast<uint32_t>(value);
    return true;
}

bool is_toast(std::string_view name) {
    const size_t at = name.find(toast_name);
    if (at == std::string_view::npos) {
        return false;
    }
    const bool starts_word = at == 0 || name[at - 1] == ' ';
    const size_t after = at + toast_name.size();
    const bool ends_word = after == name.size() || name[after] == '#';
    return starts_word && ends_word;
}

} // namespace

layer_identity identify_layer(std::string_view name) {
    layer_identity identity;
    if (starts_with(name, task_prefix)) {
        const std::string_view rest = name.substr(task_prefix.size());
        const size_t hash = rest.find('#');
        if (hash == std::string_view::npos || !parse_task_id(rest.substr(0, hash), identity.task_id)) {
            return identity;
        }
        const std::string_view activity = rest.substr(hash + 1);
        const size_t slash = activity.find('/');
        const std::string_view package = activity.substr(0, std::min(slash, activity.find('#')));
        if (package.empty()) {
            return identity;
        }
        identity.role = layer_role::task;
        identity.package = std::string(package);
        if (slash != std::string_view::npos) {
            const std::string_view component = activity.substr(slash + 1);
            identity.component = std::string(component.substr(0, component.find('#')));
        }
        return identity;
    }
    if (starts_with(name, input_method_prefix)) {
        identity.role = layer_role::input_method;
    } else if (is_toast(name)) {
        identity.role = layer_role::toast;
    }
    return identity;
}

} // namespace task
