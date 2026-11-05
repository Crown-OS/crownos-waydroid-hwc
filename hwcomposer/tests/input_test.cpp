/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "input/keyboard_layout.h"
#include "input/pen.h"
#include "input/pinch.h"
#include "input/scroll.h"

#include <gtest/gtest.h>

using namespace input;

TEST(Scroll, WheelDetentsComeFromValue120) {
    scroll_axis axis;
    EXPECT_EQ(axis.wheel(120), 1);
    EXPECT_EQ(axis.wheel(-240), -2);
}

TEST(Scroll, HighResolutionWheelFractionsAddUp) {
    scroll_axis axis;
    EXPECT_EQ(axis.wheel(30), 0);
    EXPECT_EQ(axis.wheel(30), 0);
    EXPECT_EQ(axis.wheel(30), 0);
    EXPECT_EQ(axis.wheel(30), 1);
    EXPECT_EQ(axis.wheel(100), 0);
    EXPECT_EQ(axis.wheel(20), 1);
}

TEST(Scroll, ReversingDropsTheRemainder) {
    scroll_axis axis;
    EXPECT_EQ(axis.wheel(90), 0);
    EXPECT_EQ(axis.wheel(-60), 0);
    EXPECT_EQ(axis.wheel(-60), -1);
}

TEST(Scroll, ContinuousScrollingCarriesFractions) {
    scroll_axis axis;
    EXPECT_EQ(axis.continuous(4.5), 0);
    EXPECT_EQ(axis.continuous(4.5), 0);
    EXPECT_EQ(axis.continuous(1.5), 1);
    EXPECT_EQ(axis.continuous(25), 2);
    axis.stop();
    EXPECT_EQ(axis.continuous(9.9), 0);
}

TEST(Pen, FullRangeMapsOntoAndroidRange) {
    EXPECT_EQ(android_pressure(0), 0);
    EXPECT_EQ(android_pressure(1), 1);
    EXPECT_EQ(android_pressure(65535), android_pressure_max);
    EXPECT_EQ(android_pressure(32768), 128);
    EXPECT_EQ(android_pressure(200), 1);
    EXPECT_EQ(android_pressure(70000), android_pressure_max);
}

TEST(Pinch, FingersSpreadAroundTheCentreAndFollowScale) {
    pinch_emulation pinch;
    auto fingers = pinch.begin(1000, 1000, {0, 0, 4096, 4096});
    EXPECT_EQ(fingers[0].x, 800);
    EXPECT_EQ(fingers[1].x, 1200);
    EXPECT_EQ(fingers[0].y, 1000);
    fingers = pinch.update(10, 0, 2, 0);
    EXPECT_EQ(fingers[0].x, 610);
    EXPECT_EQ(fingers[1].x, 1410);
    fingers = pinch.update(0, 0, 2, 90);
    EXPECT_EQ(fingers[0].x, 1010);
    EXPECT_EQ(fingers[0].y, 600);
    EXPECT_EQ(fingers[1].y, 1400);
}

TEST(Pinch, FingersStayInsideTheWindow) {
    pinch_emulation pinch;
    pinch.begin(110, 50, {100, 0, 500, 300});
    const auto fingers = pinch.update(0, 0, 3, 0);
    EXPECT_EQ(fingers[0].x, 100);
    EXPECT_EQ(fingers[1].x, 499);
}

TEST(KeyboardLayout, XkbNamesMapToAndroidLayouts) {
    EXPECT_EQ(android_keyboard_layout("English (US)"), "english_us");
    EXPECT_EQ(android_keyboard_layout("English (UK)"), "english_uk");
    EXPECT_EQ(android_keyboard_layout("English (UK, extended, Windows)"), "english_uk");
    EXPECT_EQ(android_keyboard_layout("English (US, intl., with dead keys)"), "english_us_intl");
    EXPECT_EQ(android_keyboard_layout("English (Colemak)"), "english_us_colemak");
    EXPECT_EQ(android_keyboard_layout("English (Dvorak)"), "english_us_dvorak");
    EXPECT_EQ(android_keyboard_layout("German"), "german");
    EXPECT_EQ(android_keyboard_layout("German (Austria)"), "german");
    EXPECT_EQ(android_keyboard_layout("German (Switzerland)"), "swiss_german");
    EXPECT_EQ(android_keyboard_layout("French (Switzerland)"), "swiss_french");
    EXPECT_EQ(android_keyboard_layout("French (Canada)"), "french_ca");
    EXPECT_EQ(android_keyboard_layout("Russian (Macintosh)"), "russian_mac");
    EXPECT_EQ(android_keyboard_layout("Portuguese (Brazil)"), "brazilian");
    EXPECT_EQ(android_keyboard_layout("Portuguese"), "portuguese");
    EXPECT_EQ(android_keyboard_layout("Spanish (Latin American)"), "spanish_latin");
    EXPECT_EQ(android_keyboard_layout("Czech (QWERTY)"), "czech_qwerty");
    EXPECT_EQ(android_keyboard_layout("Turkish (F)"), "turkish_f");
    EXPECT_EQ(android_keyboard_layout("Bulgarian (new phonetic)"), "bulgarian_phonetic");
}

TEST(KeyboardLayout, UnknownLayoutsKeepAndroidsDefault) {
    EXPECT_FALSE(android_keyboard_layout("Japanese"));
    EXPECT_FALSE(android_keyboard_layout(""));
    EXPECT_FALSE(android_keyboard_layout("Germanic"));
}
