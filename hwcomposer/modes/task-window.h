/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "waydroid_mode.h"

#include "task/layer_role.h"

#include <vector>

/// Every Android task is its own xdg_toplevel showing the task's slot of the
/// atlas. Used while crown-agent organizes the tasks; without it (or in safe
/// mode) the stock multi-window mode takes over.
class task_window_mode : public waydroid_mode {
  public:
    bool uses_task_windows() const override { return true; }

    int setup_prepare(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) override;
    int prepare(hwc_layer_1 *hwc_layer, size_t hwc_layer_index) override;

    int setup_set(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) override;
    int cleanup_stale_windows(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) override;
    int handle_layer(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t hwc_layer_index) override;
    int post_processing(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) override;
    void commit_windows(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) override;
    int retire_fence(waydroid_hwc_composer_device_1 *pdev, uint64_t frame) override;

  private:
    struct layer_plan {
        task::layer_identity identity;
        /// Composed by SurfaceFlinger into the framebuffer target.
        bool client = false;
        /// The task's first client layer: the framebuffer target goes here.
        bool shows_framebuffer = false;
    };

    void plan_layers(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents);
    window *window_for_task(waydroid_hwc_composer_device_1 *pdev, const task::layer_identity &identity);
    window *focused_window(waydroid_hwc_composer_device_1 *pdev);
    void show_framebuffer(waydroid_hwc_composer_device_1 *pdev, uint32_t task_id);
    int show_layer(waydroid_hwc_composer_device_1 *pdev, window &window, hwc_layer_1 *hwc_layer, size_t index,
                   const task::placement &placement, bool accepts_input);

    std::vector<layer_plan> plans;
    size_t framebuffer_index = SIZE_MAX;
    hwc_layer_1 *framebuffer = nullptr;
    int framebuffer_acquire = -1;
    std::vector<int> framebuffer_releases;
    int32_t input_method_height = 0;
    bool frame_timed = false;
};
