/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "task/registry.h"

namespace task {

void registry::set_facts(uint32_t task, task_facts facts) {
    entries[task].facts = std::move(facts);
}

void registry::set_slot(uint32_t task, const px_rect &slot) {
    if (!slot.empty()) {
        entries[task].slot = slot;
    }
}

void registry::set_activation_token(uint32_t task, std::string token) {
    entries[task].activation_token = std::move(token);
}

void registry::remove(uint32_t task) {
    entries.erase(task);
}

const task_facts *registry::facts(uint32_t task) const {
    const auto found = entries.find(task);
    return found != entries.end() && found->second.facts ? &*found->second.facts : nullptr;
}

std::optional<px_rect> registry::slot(uint32_t task) const {
    const auto found = entries.find(task);
    return found != entries.end() ? found->second.slot : std::nullopt;
}

std::string registry::take_activation_token(uint32_t task) {
    const auto found = entries.find(task);
    if (found == entries.end()) {
        return {};
    }
    std::string token = std::move(found->second.activation_token);
    found->second.activation_token.clear();
    return token;
}

} // namespace task
