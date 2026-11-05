/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "task/layer_role.h"

#include <gtest/gtest.h>

using task::identify_layer;
using task::layer_role;

TEST(LayerRole, TaskLayersCarryTaskPackageAndComponent) {
    const auto identity = identify_layer("TID:42#com.whatsapp/com.whatsapp.HomeActivity#0");
    EXPECT_EQ(identity.role, layer_role::task);
    EXPECT_EQ(identity.task_id, 42u);
    EXPECT_EQ(identity.package, "com.whatsapp");
    EXPECT_EQ(identity.component, "com.whatsapp.HomeActivity");
}

TEST(LayerRole, TaskLayerWithoutComponentKeepsThePackage) {
    const auto identity = identify_layer("TID:7#org.example#3");
    EXPECT_EQ(identity.role, layer_role::task);
    EXPECT_EQ(identity.package, "org.example");
    EXPECT_TRUE(identity.component.empty());
}

TEST(LayerRole, BrokenTaskTagsAreNotTasks) {
    EXPECT_EQ(identify_layer("TID:#com.app/x#0").role, layer_role::other);
    EXPECT_EQ(identify_layer("TID:12x#com.app/x#0").role, layer_role::other);
    EXPECT_EQ(identify_layer("TID:99999999999#com.app/x#0").role, layer_role::other);
    EXPECT_EQ(identify_layer("TID:5#/x#0").role, layer_role::other);
    EXPECT_EQ(identify_layer("TID:5").role, layer_role::other);
}

TEST(LayerRole, InputMethodAndToastsAreRecognised) {
    EXPECT_EQ(identify_layer("InputMethod#0").role, layer_role::input_method);
    EXPECT_EQ(identify_layer("Toast#12").role, layer_role::toast);
    EXPECT_EQ(identify_layer("4f2a1c Toast#3").role, layer_role::toast);
    EXPECT_EQ(identify_layer("Toast").role, layer_role::toast);
}

TEST(LayerRole, EverythingElseIsIgnored) {
    EXPECT_EQ(identify_layer("").role, layer_role::other);
    EXPECT_EQ(identify_layer("StatusBar#0").role, layer_role::other);
    EXPECT_EQ(identify_layer("com.example/ToastActivity#0").role, layer_role::other);
    EXPECT_EQ(identify_layer("ToastyApp#1").role, layer_role::other);
}
