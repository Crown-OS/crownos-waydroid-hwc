/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <optional>
#include <string_view>

namespace input {

/// The Android 13 keyboard layout (the `keyboard_layout_<name>` resource of
/// com.android.inputdevices, without the prefix) for an xkb layout name such
/// as "English (UK)" or "German (Switzerland)". Unknown layouts have none, and
/// Android keeps its default.
std::optional<std::string_view> android_keyboard_layout(std::string_view xkb_layout_name);

} // namespace input
