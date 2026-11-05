/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "task/geometry.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace task {

/// TaskInfo from the agent.
struct task_facts {
    std::string package;
    std::string label;
    /// 0xAARRGGBB; 0 means no color.
    uint32_t color = 0;
    bool resizable = true;
    uint32_t min_width = 0;
    uint32_t min_height = 0;
};

/// What the agent has said about each task. The agent opens every window
/// with TaskInfo and then SlotApplied, so a task gets its window once both
/// are known, and loses it with TaskRemoved (a parked task comes back the
/// same way it first came). Not thread-safe; the HWC guards it with its
/// windows mutex.
class registry {
  public:
    void set_facts(uint32_t task, task_facts facts);
    void set_slot(uint32_t task, const px_rect &slot);
    /// Activate that came before the window could use it.
    void set_activation_token(uint32_t task, std::string token);
    void remove(uint32_t task);
    void clear() { entries.clear(); }

    const task_facts *facts(uint32_t task) const;
    std::optional<px_rect> slot(uint32_t task) const;
    /// Both TaskInfo and a slot arrived: the task may open its window.
    bool ready_for_window(uint32_t task) const { return facts(task) && slot(task); }
    /// The kept activation token, removed from the registry.
    std::string take_activation_token(uint32_t task);

  private:
    struct entry {
        std::optional<task_facts> facts;
        std::optional<px_rect> slot;
        std::string activation_token;
    };

    std::unordered_map<uint32_t, entry> entries;
};

} // namespace task
