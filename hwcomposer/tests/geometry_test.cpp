/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "task/geometry.h"

#include <gtest/gtest.h>

using namespace task;

namespace {

buffer_view full_buffer(uint32_t width, uint32_t height, int32_t transform = 0) {
    return {width, height, {0, 0, float(width), float(height)}, transform};
}

constexpr int32_t fixed(int32_t value) {
    return value * 256;
}

} // namespace

TEST(Geometry, LayerAtTheSlotOriginSitsAtTheWindowOrigin) {
    const px_rect slot{2048, 1024, 3328, 1824};
    const auto placed = place_in_slot(slot, full_buffer(1280, 800), slot, {0.5, 0.5});
    ASSERT_TRUE(placed.visible);
    EXPECT_EQ(placed.x, 0);
    EXPECT_EQ(placed.y, 0);
    EXPECT_EQ(placed.width, 640);
    EXPECT_EQ(placed.height, 400);
    EXPECT_EQ(placed.source.x, 0);
    EXPECT_EQ(placed.source.width, fixed(1280));
    EXPECT_EQ(placed.source.height, fixed(800));
}

TEST(Geometry, LayerOffsetIsDisplayFrameMinusSlotOriginOverScale) {
    const px_rect slot{1000, 500, 2000, 1500};
    const px_rect frame{1100, 700, 1300, 900};
    const auto placed = place_in_slot(frame, full_buffer(200, 200), slot, {1 / 1.25, 1 / 1.25});
    EXPECT_EQ(placed.x, 80);
    EXPECT_EQ(placed.y, 160);
    EXPECT_EQ(placed.width, 160);
    EXPECT_EQ(placed.height, 160);
}

TEST(Geometry, LayersAreClippedToTheSlot) {
    const px_rect slot{0, 0, 1000, 1000};
    const px_rect frame{-100, 900, 300, 1300};
    const auto placed = place_in_slot(frame, full_buffer(400, 400), slot, {1, 1});
    ASSERT_TRUE(placed.visible);
    EXPECT_EQ(placed.x, 0);
    EXPECT_EQ(placed.y, 900);
    EXPECT_EQ(placed.width, 300);
    EXPECT_EQ(placed.height, 100);
    EXPECT_EQ(placed.source.x, fixed(100));
    EXPECT_EQ(placed.source.y, 0);
    EXPECT_EQ(placed.source.width, fixed(300));
    EXPECT_EQ(placed.source.height, fixed(100));
}

TEST(Geometry, LayersOutsideTheSlotAreHidden) {
    const auto placed = place_in_slot({5000, 0, 5100, 100}, full_buffer(100, 100), {0, 0, 1000, 1000}, {1, 1});
    EXPECT_FALSE(placed.visible);
}

TEST(Geometry, ClippingFollowsAScaledCrop) {
    buffer_view buffer{1000, 1000, {0, 0, 500, 500}, 0};
    const auto placed = place_in_slot({0, 0, 1000, 1000}, buffer, {500, 0, 1000, 1000}, {1, 1});
    EXPECT_EQ(placed.source.x, fixed(250));
    EXPECT_EQ(placed.source.width, fixed(250));
    EXPECT_EQ(placed.source.height, fixed(500));
}

TEST(Geometry, ClippingARotatedLayerCutsTheMatchingBufferSide) {
    const auto buffer = full_buffer(200, 100, 1);
    const auto placed = place_in_slot({0, 0, 100, 200}, buffer, {0, 0, 100, 100}, {1, 1});
    EXPECT_EQ(placed.source.x, 0);
    EXPECT_EQ(placed.source.y, 0);
    EXPECT_EQ(placed.source.width, fixed(100));
    EXPECT_EQ(placed.source.height, fixed(100));
}

TEST(Geometry, SourceNeverLeavesTheBuffer) {
    buffer_view buffer{100, 100, {-3.3f, 0.2f, 100.7f, 100.4f}, 0};
    const auto placed = place_in_slot({0, 0, 100, 100}, buffer, {0, 0, 100, 100}, {1, 1});
    EXPECT_GE(placed.source.x, 0);
    EXPECT_GE(placed.source.y, 0);
    EXPECT_LE(placed.source.x + placed.source.width, fixed(100));
    EXPECT_LE(placed.source.y + placed.source.height, fixed(100));
    EXPECT_GT(placed.source.width, 0);
}

TEST(Geometry, CropTransformsMapIntoSurfaceSpace) {
    const buffer_view buffer{400, 200, {10, 20, 110, 70}, 0};
    auto with = [&](int32_t transform) {
        buffer_view transformed = buffer;
        transformed.transform = transform;
        return crop_in_surface_space(transformed);
    };
    const auto rotated_90 = with(1);
    EXPECT_FLOAT_EQ(rotated_90.left, 130);
    EXPECT_FLOAT_EQ(rotated_90.top, 10);
    EXPECT_FLOAT_EQ(rotated_90.right, 180);
    EXPECT_FLOAT_EQ(rotated_90.bottom, 110);
    const auto rotated_180 = with(2);
    EXPECT_FLOAT_EQ(rotated_180.left, 290);
    EXPECT_FLOAT_EQ(rotated_180.top, 130);
    const auto rotated_270 = with(3);
    EXPECT_FLOAT_EQ(rotated_270.left, 20);
    EXPECT_FLOAT_EQ(rotated_270.top, 290);
    const auto mirrored = with(4);
    EXPECT_FLOAT_EQ(mirrored.left, 290);
    EXPECT_FLOAT_EQ(mirrored.top, 20);
    const auto transposed = with(5);
    EXPECT_FLOAT_EQ(transposed.left, 20);
    EXPECT_FLOAT_EQ(transposed.top, 10);
    const auto upside_down = with(6);
    EXPECT_FLOAT_EQ(upside_down.left, 10);
    EXPECT_FLOAT_EQ(upside_down.top, 130);
    const auto anti_transposed = with(7);
    EXPECT_FLOAT_EQ(anti_transposed.left, 130);
    EXPECT_FLOAT_EQ(anti_transposed.top, 290);
}

TEST(Geometry, InputMapsLocalTimesScalePlusSlotOrigin) {
    const px_rect slot{2048, 1024, 3328, 1824};
    const auto mapping = slot_input_mapping(slot, {0.5, 0.5});
    int32_t x, y;
    mapping.to_android(10.5, 20, x, y);
    EXPECT_EQ(x, 2069);
    EXPECT_EQ(y, 1064);
}

TEST(Geometry, InputWhileAResizeWaitsUsesTheStretchedScale) {
    const auto mapping = slot_input_mapping({0, 0, 1000, 500}, {0.8, 0.8});
    int32_t x, y;
    mapping.to_android(800, 400, x, y);
    EXPECT_EQ(x, 1000);
    EXPECT_EQ(y, 500);
}

TEST(Geometry, InputMethodIsCentredAtTheBottom) {
    const px_rect frame{0, 3296, 4096, 4096};
    const auto placed = place_anchored_to_bottom(frame, full_buffer(4096, 800), {1024, 768}, 2, 0);
    ASSERT_TRUE(placed.visible);
    EXPECT_EQ(placed.x, 0);
    EXPECT_EQ(placed.y, 368);
    EXPECT_EQ(placed.width, 1024);
    EXPECT_EQ(placed.height, 400);
    EXPECT_EQ(placed.source.x, fixed(1024));
    EXPECT_EQ(placed.source.width, fixed(2048));
    int32_t x, y;
    placed.input.to_android(0, 0, x, y);
    EXPECT_EQ(x, 1024);
    EXPECT_EQ(y, 3296);
}

TEST(Geometry, ToastSitsAboveTheGap) {
    const px_rect frame{1800, 3600, 2296, 3700};
    const auto placed = place_anchored_to_bottom(frame, full_buffer(496, 100), {1000, 800}, 2, 300);
    ASSERT_TRUE(placed.visible);
    EXPECT_EQ(placed.x, 376);
    EXPECT_EQ(placed.y, 450);
    EXPECT_EQ(placed.width, 248);
    EXPECT_EQ(placed.height, 50);
}

TEST(Geometry, BoundingBoxAndIntersection) {
    EXPECT_EQ(bounding_box({}, {1, 2, 3, 4}), (px_rect{1, 2, 3, 4}));
    EXPECT_EQ(bounding_box({0, 0, 10, 10}, {5, 5, 20, 8}), (px_rect{0, 0, 20, 10}));
    EXPECT_TRUE(intersect({0, 0, 10, 10}, {10, 0, 20, 10}).empty());
}
