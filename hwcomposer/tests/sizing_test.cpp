/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "task/sizing.h"

#include <gtest/gtest.h>

using namespace task;

TEST(Sizing, FirstSlotGivesTheWindowSize) {
    window_sizing sizing(1.25);
    EXPECT_FALSE(sizing.has_slot());
    sizing.adopt_slot({0, 0, 1280, 800});
    EXPECT_EQ(sizing.size(), (logical_size{1024, 640}));
    EXPECT_DOUBLE_EQ(sizing.layer_scale().x, 0.8);
    EXPECT_FALSE(sizing.resize_pending());
}

TEST(Sizing, ConfigureAsksForPixelsAndStretchesUntilTheSlotArrives) {
    window_sizing sizing(2);
    sizing.adopt_slot({0, 0, 1000, 800});
    const auto request = sizing.configure(600, 500, 17);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->width, 1200u);
    EXPECT_EQ(request->height, 1000u);
    EXPECT_EQ(request->serial, 17u);
    EXPECT_TRUE(sizing.resize_pending());
    EXPECT_EQ(sizing.size(), (logical_size{600, 500}));
    EXPECT_DOUBLE_EQ(sizing.layer_scale().x, 0.6);
    EXPECT_DOUBLE_EQ(sizing.layer_scale().y, 0.625);

    sizing.slot_applied(17, {1000, 0, 2200, 1000});
    EXPECT_FALSE(sizing.resize_pending());
    EXPECT_EQ(sizing.slot(), (px_rect{1000, 0, 2200, 1000}));
    EXPECT_EQ(sizing.size(), (logical_size{600, 500}));
    EXPECT_DOUBLE_EQ(sizing.layer_scale().x, 0.5);
}

TEST(Sizing, OlderSlotKeepsTheNewestRequestedSize) {
    window_sizing sizing(1);
    sizing.adopt_slot({0, 0, 800, 600});
    ASSERT_TRUE(sizing.configure(900, 700, 5));
    ASSERT_TRUE(sizing.configure(1000, 750, 6));
    sizing.slot_applied(5, {0, 0, 900, 700});
    EXPECT_TRUE(sizing.resize_pending());
    EXPECT_EQ(sizing.size(), (logical_size{1000, 750}));
    EXPECT_DOUBLE_EQ(sizing.layer_scale().x, 1000.0 / 900);
    sizing.slot_applied(6, {0, 0, 1000, 750});
    EXPECT_FALSE(sizing.resize_pending());
}

TEST(Sizing, TheAgentMayClampTheSlot) {
    window_sizing sizing(1);
    sizing.adopt_slot({0, 0, 800, 600});
    ASSERT_TRUE(sizing.configure(100, 100, 9));
    sizing.slot_applied(9, {0, 0, 320, 480});
    EXPECT_EQ(sizing.size(), (logical_size{320, 480}));
}

TEST(Sizing, ZeroSizeOrTheSameSizeAsksForNothing) {
    window_sizing sizing(1);
    sizing.adopt_slot({0, 0, 800, 600});
    EXPECT_FALSE(sizing.configure(0, 0, 1));
    EXPECT_FALSE(sizing.configure(800, 600, 2));
}

TEST(Sizing, GoingBackToTheOldSizeStillCancelsAPendingResize) {
    window_sizing sizing(1);
    sizing.adopt_slot({0, 0, 800, 600});
    ASSERT_TRUE(sizing.configure(900, 600, 3));
    const auto back = sizing.configure(800, 600, 4);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->serial, 4u);
}

TEST(Sizing, SerialsWrapAround) {
    window_sizing sizing(1);
    sizing.adopt_slot({0, 0, 800, 600});
    ASSERT_TRUE(sizing.configure(900, 600, 0xFFFFFFFE));
    ASSERT_TRUE(sizing.configure(950, 600, 1));
    sizing.slot_applied(0xFFFFFFFF, {0, 0, 900, 600});
    EXPECT_TRUE(sizing.resize_pending());
    sizing.slot_applied(1, {0, 0, 950, 600});
    EXPECT_FALSE(sizing.resize_pending());
}

TEST(Sizing, SlotsTheAgentMovesOnItsOwnResizeTheWindow) {
    window_sizing sizing(2);
    sizing.adopt_slot({0, 0, 800, 600});
    sizing.slot_applied(0, {0, 0, 1000, 1000});
    EXPECT_EQ(sizing.size(), (logical_size{500, 500}));
}

TEST(Sizing, MaximizedWindowsKeepTheConfiguredSize) {
    window_sizing sizing(1);
    sizing.adopt_slot({0, 0, 800, 600});
    ASSERT_TRUE(sizing.configure(1920, 1080, 4, true));
    sizing.slot_applied(4, {0, 0, 1920, 1200});
    EXPECT_FALSE(sizing.resize_pending());
    EXPECT_EQ(sizing.size(), (logical_size{1920, 1080}));
    EXPECT_DOUBLE_EQ(sizing.layer_scale().y, 0.9);
    ASSERT_TRUE(sizing.configure(800, 600, 5, false));
    sizing.slot_applied(5, {0, 0, 800, 600});
    EXPECT_EQ(sizing.size(), (logical_size{800, 600}));
}
