/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "agent/bridge.h"

#include "task/task_window.h"
#include "wayland-hwc.h"

#include <log/log.h>

#include <type_traits>

namespace {

task::px_rect to_px_rect(const agent_wire::Rect &rect) {
    return {rect.left, rect.top, rect.right, rect.bottom};
}

} // namespace

void agent_bridge::on_agent_connected() {
    {
        std::scoped_lock lock(display->windowsMutex);
        display->tasks.clear();
    }
    display->request_frame();
}

void agent_bridge::on_agent_disconnected() {
    {
        std::scoped_lock lock(display->windowsMutex);
        display->tasks.clear();
    }
    ALOGW("crown_agent is gone; task windows fall back to stock multi-window");
    display->request_frame();
}

void agent_bridge::on_agent_record(const agent_wire::Record &record) {
    std::visit(
        [this](const auto &received) {
            using Received = std::decay_t<decltype(received)>;
            if constexpr (std::is_same_v<Received, agent_wire::TaskInfo> ||
                          std::is_same_v<Received, agent_wire::SlotApplied> ||
                          std::is_same_v<Received, agent_wire::TaskRemoved> ||
                          std::is_same_v<Received, agent_wire::Activate>) {
                handle(received);
            } else {
                ALOGW("crown_agent sent record 0x%04x, which is not for the HWC", Received::type);
            }
        },
        record);
}

void agent_bridge::handle(const agent_wire::TaskInfo &info) {
    {
        std::scoped_lock lock(display->windowsMutex);
        display->tasks.set_facts(info.task, {info.package, info.label, info.color, info.resizable,
                                             info.min_width, info.min_height});
    }
    display->request_frame();
}

void agent_bridge::handle(const agent_wire::SlotApplied &applied) {
    const task::px_rect slot = to_px_rect(applied.slot);
    {
        std::scoped_lock lock(display->windowsMutex);
        display->tasks.set_slot(applied.task, slot);
        if (window *window = task_window_of(display, applied.task)) {
            window->task->sizing.slot_applied(applied.serial, slot);
        }
    }
    display->request_frame();
}

void agent_bridge::handle(const agent_wire::TaskRemoved &removed) {
    {
        std::scoped_lock lock(display->windowsMutex);
        display->tasks.remove(removed.task);
    }
    display->request_frame();
}

void agent_bridge::handle(const agent_wire::Activate &activate) {
    std::scoped_lock lock(display->windowsMutex);
    window *window = task_window_of(display, activate.task);
    if (window && window->task->mapped) {
        activate_task_window(display, *window, activate.token);
    } else {
        display->tasks.set_activation_token(activate.task, activate.token);
    }
}
