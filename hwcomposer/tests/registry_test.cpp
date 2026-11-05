/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "input/scroll.h"
#include "task/registry.h"

#include <gtest/gtest.h>

using input::scroll_axis_id;

TEST(Registry, FactsAndSlotsWaitForTheirWindow) {
    task::registry tasks;
    EXPECT_EQ(tasks.facts(7), nullptr);
    tasks.set_facts(7, {"com.whatsapp", "WhatsApp", 0xFF075E54, false, 320, 480});
    tasks.set_slot(7, {0, 0, 1280, 800});
    ASSERT_NE(tasks.facts(7), nullptr);
    EXPECT_EQ(tasks.facts(7)->label, "WhatsApp");
    EXPECT_EQ(tasks.slot(7), (task::px_rect{0, 0, 1280, 800}));
}

TEST(Registry, AWindowNeedsTaskInfoAndASlot) {
    task::registry tasks;
    tasks.set_facts(7, {});
    EXPECT_FALSE(tasks.ready_for_window(7));
    tasks.set_slot(7, {});
    EXPECT_FALSE(tasks.ready_for_window(7));
    tasks.set_slot(7, {0, 0, 640, 480});
    EXPECT_TRUE(tasks.ready_for_window(7));
}

TEST(Registry, RemovedTasksComeBackOnlyWithNewTaskInfoAndSlot) {
    task::registry tasks;
    tasks.set_facts(3, {});
    tasks.set_slot(3, {0, 0, 640, 480});
    tasks.remove(3);
    EXPECT_FALSE(tasks.ready_for_window(3));
    tasks.set_facts(3, {});
    EXPECT_FALSE(tasks.ready_for_window(3));
    tasks.set_slot(3, {640, 0, 1280, 480});
    EXPECT_TRUE(tasks.ready_for_window(3));
}

TEST(Registry, ActivationTokensWaitForTheWindowAndAreUsedOnce) {
    task::registry tasks;
    tasks.set_activation_token(5, "tok-1");
    EXPECT_EQ(tasks.take_activation_token(5), "tok-1");
    EXPECT_EQ(tasks.take_activation_token(5), "");
    EXPECT_EQ(tasks.take_activation_token(6), "");
}

TEST(ScrollFrame, Value120WinsOverTheAxisValue) {
    input::scroll_frame scroll;
    scroll.source_is_wheel(true);
    scroll.value(scroll_axis_id::vertical, 15);
    scroll.value120(scroll_axis_id::vertical, 120);
    EXPECT_EQ(scroll.finish(), (std::array<int32_t, 2>{1, 0}));
}

TEST(ScrollFrame, DiscreteStepsCountAsWholeDetents) {
    input::scroll_frame scroll;
    scroll.source_is_wheel(true);
    scroll.discrete(scroll_axis_id::horizontal, -2);
    scroll.value(scroll_axis_id::horizontal, -30);
    EXPECT_EQ(scroll.finish(), (std::array<int32_t, 2>{0, -2}));
}

TEST(ScrollFrame, FingerScrollingUsesTheDistance) {
    input::scroll_frame scroll;
    scroll.source_is_wheel(false);
    scroll.value(scroll_axis_id::vertical, 6);
    EXPECT_EQ(scroll.finish(), (std::array<int32_t, 2>{0, 0}));
    scroll.value(scroll_axis_id::vertical, 6);
    EXPECT_EQ(scroll.finish(), (std::array<int32_t, 2>{1, 0}));
    scroll.value(scroll_axis_id::vertical, 9);
    scroll.stop(scroll_axis_id::vertical);
    EXPECT_EQ(scroll.finish(), (std::array<int32_t, 2>{1, 0}));
    scroll.value(scroll_axis_id::vertical, 9);
    EXPECT_EQ(scroll.finish(), (std::array<int32_t, 2>{0, 0}));
}
