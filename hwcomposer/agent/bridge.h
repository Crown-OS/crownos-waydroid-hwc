/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "agent/link.h"

struct display;

/// Applies what crown-agent says to the task registry and the task windows.
/// Runs on the link thread; window changes that need a commit are left to
/// the next frame, which it asks SurfaceFlinger for.
struct agent_bridge : agent::link_listener {
    explicit agent_bridge(struct display *display) : display(display) {}

    void on_agent_connected() override;
    void on_agent_record(const agent_wire::Record &record) override;
    void on_agent_disconnected() override;

  private:
    void handle(const agent_wire::TaskInfo &info);
    void handle(const agent_wire::SlotApplied &applied);
    void handle(const agent_wire::TaskRemoved &removed);
    void handle(const agent_wire::Activate &activate);

    struct display *display;
};
