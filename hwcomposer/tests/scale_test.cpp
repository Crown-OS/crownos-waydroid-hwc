/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "output/scale.h"

#include <gtest/gtest.h>

using namespace output;

TEST(Scale, FractionalScaleComesFromTheLogicalSize) {
    output_info laptop;
    laptop.integer_scale = 2;
    laptop.mode_width = 2880;
    laptop.mode_height = 1800;
    laptop.logical_width = 1646;
    laptop.logical_height = 1029;
    EXPECT_EQ(preferred_scale_120(laptop), 210u);
}

TEST(Scale, RotatedOutputsCompareTheirPhysicalHeight) {
    output_info portrait;
    portrait.mode_width = 2560;
    portrait.mode_height = 1440;
    portrait.logical_width = 1152;
    portrait.logical_height = 2048;
    portrait.transform = 1;
    EXPECT_EQ(preferred_scale_120(portrait), 150u);
}

TEST(Scale, WithoutXdgOutputTheIntegerScaleIsUsed) {
    output_info output;
    output.integer_scale = 2;
    output.mode_width = 3840;
    EXPECT_EQ(preferred_scale_120(output), 240u);
}

TEST(Scale, AndroidUsesTheHighestOutputScale) {
    output_info external;
    external.mode_width = 1920;
    external.logical_width = 1920;
    output_info laptop;
    laptop.mode_width = 2560;
    laptop.logical_width = 2048;
    EXPECT_DOUBLE_EQ(highest_scale({external, laptop}), 1.25);
    EXPECT_DOUBLE_EQ(highest_scale({}), 1.0);
}
