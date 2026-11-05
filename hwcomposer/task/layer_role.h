/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace task {

enum class layer_role {
    /// `TID:<task>#<package>/<component>`: belongs to a task window.
    task,
    /// The input method window, shown inside the focused task window.
    input_method,
    /// A toast, shown inside the focused task window above the input method.
    toast,
    /// Anything else SurfaceFlinger names: never shown.
    other,
};

struct layer_identity {
    layer_role role = layer_role::other;
    uint32_t task_id = 0;
    std::string package;
    std::string component;
};

/// What a SurfaceFlinger layer name says about where the layer belongs.
layer_identity identify_layer(std::string_view name);

} // namespace task
