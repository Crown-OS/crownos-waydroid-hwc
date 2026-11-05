/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 */

#include "task-window.h"

#include "fence/frame_sync.h"
#include "task/task_window.h"

#include "presentation-time-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <log/log.h>
#include <sync/sync.h>

#include <climits>
#include <unordered_map>

namespace {

/// A toast keeps its distance from the bottom of the atlas, but never more
/// than this fraction of the window height.
constexpr int32_t toast_gap_window_fraction = 3;

void close_fence(int &fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

task::px_rect frame_of(const hwc_layer_1 &layer) {
    return {layer.displayFrame.left, layer.displayFrame.top, layer.displayFrame.right, layer.displayFrame.bottom};
}

task::buffer_view view_of(const hwc_layer_1 &layer, const buffer_metadata &metadata) {
    return {metadata.width,
            metadata.height,
            {layer.sourceCropf.left, layer.sourceCropf.top, layer.sourceCropf.right, layer.sourceCropf.bottom},
            static_cast<int32_t>(hwc_transform_to_buffer_transform(layer.transform))};
}

bool same_placement(const task::placement &a, const task::placement &b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height && a.source.x == b.source.x &&
           a.source.y == b.source.y && a.source.width == b.source.width && a.source.height == b.source.height;
}

/// SurfaceFlinger marks an unchanged buffer with one empty damage rect.
bool content_unchanged(const hwc_layer_1 &layer) {
    const auto &damage = layer.surfaceDamage;
    return damage.numRects == 1 && damage.rects[0].left >= damage.rects[0].right &&
           damage.rects[0].top >= damage.rects[0].bottom;
}

void damage_buffer(const hwc_layer_1 &layer, wl_surface *surface) {
    const auto &damage = layer.surfaceDamage;
    if (damage.numRects == 0 || wl_surface_get_version(surface) < WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION) {
        wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
        return;
    }
    for (size_t i = 0; i < damage.numRects; ++i) {
        const hwc_rect_t &rect = damage.rects[i];
        wl_surface_damage_buffer(surface, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    }
}

void set_accepts_input(struct display *display, window::layer &layer, bool accepts_input) {
    if (layer.accepts_input == accepts_input) {
        return;
    }
    wl_region *nothing = accepts_input ? nullptr : wl_compositor_create_region(display->compositor);
    wl_surface_set_input_region(layer.surface, nothing);
    if (nothing) {
        wl_region_destroy(nothing);
    }
    layer.accepts_input = accepts_input;
}

struct timed_frame {
    waydroid_hwc_composer_device_1 *pdev;
    uint64_t frame;
};

void frame_sync_output(void *, struct wp_presentation_feedback *, wl_output *) {}

void frame_presented(void *data, struct wp_presentation_feedback *feedback, uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                     uint32_t tv_nsec, uint32_t refresh, uint32_t, uint32_t, uint32_t) {
    auto *timed = static_cast<timed_frame *>(data);
    waydroid_hwc_composer_device_1 *pdev = timed->pdev;
    pdev->last_vsync_ns = ((uint64_t(tv_sec_hi) << 32) + tv_sec_lo) * 1000000000ULL + tv_nsec;
    if (refresh > 0) {
        pdev->vsync_period_ns = static_cast<int32_t>(refresh);
    }
    pdev->display->sync->frame_finished(timed->frame);
    wp_presentation_feedback_destroy(feedback);
    delete timed;
}

void frame_discarded(void *data, struct wp_presentation_feedback *feedback) {
    auto *timed = static_cast<timed_frame *>(data);
    timed->pdev->display->sync->frame_finished(timed->frame);
    wp_presentation_feedback_destroy(feedback);
    delete timed;
}

const wp_presentation_feedback_listener frame_feedback_listener = {
    frame_sync_output,
    frame_presented,
    frame_discarded,
};

} // namespace

void task_window_mode::plan_layers(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) {
    struct display *display = pdev->display;
    const size_t count = contents->numHwLayers;
    plans.assign(count, {});
    framebuffer_index = SIZE_MAX;

    std::unordered_map<uint32_t, std::pair<size_t, size_t>> client_ranges;
    for (size_t i = 0; i < count; ++i) {
        hwc_layer_1 &layer = contents->hwLayers[i];
        if (layer.compositionType == HWC_FRAMEBUFFER_TARGET) {
            framebuffer_index = i;
            continue;
        }
        if (layer.flags & HWC_IS_CURSOR_LAYER) {
            continue;
        }
        layer_plan &plan = plans[i];
        const auto name = display->layer_names.find(i);
        plan.identity = task::identify_layer(name != display->layer_names.end() ? name->second : std::string());
        plan.client = (layer.flags & HWC_SKIP_LAYER) ||
                      (layer.handle && pdev->gralloc_handler.is_yuv(display, &layer, i));
        if (plan.client && plan.identity.role == task::layer_role::task) {
            auto [range, added] = client_ranges.try_emplace(plan.identity.task_id, i, i);
            if (!added) {
                range->second.second = i;
            }
        }
    }
    /* SurfaceFlinger composes everything between a task's first and last
     * client layer, so the framebuffer target keeps that task's z-order. */
    for (const auto &[task_id, range] : client_ranges) {
        for (size_t i = range.first; i <= range.second; ++i) {
            if (plans[i].identity.role == task::layer_role::task && plans[i].identity.task_id == task_id) {
                plans[i].client = true;
            }
        }
        plans[range.first].shows_framebuffer = true;
    }
}

int task_window_mode::setup_prepare(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) {
    plan_layers(pdev, contents);
    return 0;
}

int task_window_mode::prepare(hwc_layer_1 *hwc_layer, size_t index) {
    if (index < plans.size()) {
        hwc_layer->compositionType = plans[index].client ? HWC_FRAMEBUFFER : HWC_OVERLAY;
    }
    return 0;
}

int task_window_mode::setup_set(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) {
    if (plans.size() != contents->numHwLayers) {
        plan_layers(pdev, contents);
    }
    framebuffer = framebuffer_index < contents->numHwLayers ? &contents->hwLayers[framebuffer_index] : nullptr;
    framebuffer_acquire = -1;
    if (framebuffer) {
        std::swap(framebuffer_acquire, framebuffer->acquireFenceFd);
        framebuffer->releaseFenceFd = -1;
    }
    input_method_height = 0;
    for (size_t i = 0; i < plans.size(); ++i) {
        const hwc_layer_1 &layer = contents->hwLayers[i];
        if (plans[i].identity.role == task::layer_role::input_method && layer.handle && !plans[i].client) {
            input_method_height = std::max(input_method_height, layer.displayFrame.bottom - layer.displayFrame.top);
        }
    }
    return 0;
}

int task_window_mode::cleanup_stale_windows(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *) {
    struct display *display = pdev->display;
    display->windows.erase_if([display](const auto &entry) {
        const window &window = *entry.second;
        return !window.task || !display->tasks.facts(window.task->task_id);
    });
    for (auto &[key, window] : display->windows) {
        apply_task_facts(display, *window, *display->tasks.facts(window->task->task_id));
    }
    return 0;
}

window *task_window_mode::window_for_task(waydroid_hwc_composer_device_1 *pdev, const task::layer_identity &identity) {
    struct display *display = pdev->display;
    if (window *existing = task_window_of(display, identity.task_id)) {
        return existing;
    }
    if (is_blacklisted(pdev, identity.package, identity.component) ||
        !display->tasks.ready_for_window(identity.task_id)) {
        return nullptr;
    }
    auto created = window::create_for_task(display, identity.task_id, *display->tasks.facts(identity.task_id),
                                           *display->tasks.slot(identity.task_id));
    window *opened = created.get();
    display->windows.add(task_window_key(identity.task_id), std::move(created));
    return opened;
}

window *task_window_mode::focused_window(waydroid_hwc_composer_device_1 *pdev) {
    window *focused = task_window_of(pdev->display, pdev->display->focused_task);
    return focused && focused->task->ready(*focused) ? focused : nullptr;
}

int task_window_mode::show_layer(waydroid_hwc_composer_device_1 *pdev, window &window, hwc_layer_1 *hwc_layer,
                                 size_t index, const task::placement &placement, bool accepts_input) {
    struct display *display = pdev->display;
    window::layer &layer = window.get_next_layer();
    window.task->dirty = true;
    set_accepts_input(display, layer, accepts_input);
    if (accepts_input) {
        display->input_map.set(layer.surface, placement.input);
    } else {
        display->input_map.erase(layer.surface);
    }

    const buffer *cached = cached_layer_buffer(pdev, hwc_layer, index);
    if (cached && cached->id == layer.shown_buffer && same_placement(layer.shown_placement, placement) &&
        content_unchanged(*hwc_layer)) {
        close_fence(hwc_layer->acquireFenceFd);
        hwc_layer->releaseFenceFd =
            layer.sync && display->sync && cached->isDmabuf ? display->sync->release_fence(*layer.sync) : -1;
        return 0;
    }

    const auto default_formats = display->importable_formats();
    const dmabuf::format_set &importable =
        window.task->surface_formats ? *window.task->surface_formats : *default_formats;
    buffer *buf = get_layer_buffer(pdev, hwc_layer, index, importable);
    if (!buf) {
        close_fence(hwc_layer->acquireFenceFd);
        return -1;
    }
    wl_surface_attach(layer.surface, buf->wl_buffer, 0, 0);
    damage_buffer(*hwc_layer, layer.surface);
    wl_surface_set_buffer_transform(layer.surface, static_cast<int32_t>(hwc_transform_to_buffer_transform(hwc_layer->transform)));
    wp_viewport_set_source(layer.viewport, placement.source.x, placement.source.y, placement.source.width,
                           placement.source.height);
    wp_viewport_set_destination(layer.viewport, placement.width, placement.height);
    layer.set_position(placement.x, placement.y);
    commit_with_fences(display, layer, *buf, hwc_layer);
    layer.shown_buffer = buf->id;
    layer.shown_placement = placement;
    return 0;
}

void task_window_mode::show_framebuffer(waydroid_hwc_composer_device_1 *pdev, uint32_t task_id) {
    struct display *display = pdev->display;
    window *window = task_window_of(display, task_id);
    if (!framebuffer || !framebuffer->handle || !window || !window->task->ready(*window)) {
        return;
    }
    const task::window_sizing &sizing = window->task->sizing;
    const task::px_rect &slot = sizing.slot();
    const buffer_metadata metadata = pdev->gralloc_handler.get_buffer_metadata(display, framebuffer, framebuffer_index);
    const task::buffer_view view{metadata.width,
                                 metadata.height,
                                 {float(slot.left), float(slot.top), float(slot.right), float(slot.bottom)},
                                 static_cast<int32_t>(BufferTransform::Normal)};
    const task::placement placement = task::place_in_slot(slot, view, slot, sizing.layer_scale());
    if (!placement.visible) {
        return;
    }
    framebuffer->acquireFenceFd = framebuffer_acquire >= 0 ? dup(framebuffer_acquire) : -1;
    show_layer(pdev, *window, framebuffer, framebuffer_index, placement, false);
    if (framebuffer->releaseFenceFd >= 0) {
        framebuffer_releases.push_back(framebuffer->releaseFenceFd);
        framebuffer->releaseFenceFd = -1;
    }
}

int task_window_mode::handle_layer(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t index) {
    if (index == framebuffer_index || index >= plans.size()) {
        return 0;
    }
    const layer_plan &plan = plans[index];
    if (plan.client) {
        if (plan.shows_framebuffer) {
            show_framebuffer(pdev, plan.identity.task_id);
        }
        close_fence(hwc_layer->acquireFenceFd);
        return 0;
    }
    if (!hwc_layer->handle || plan.identity.role == task::layer_role::other) {
        close_fence(hwc_layer->acquireFenceFd);
        return 0;
    }

    struct display *display = pdev->display;
    const task::px_rect frame = frame_of(*hwc_layer);
    const task::buffer_view view =
        view_of(*hwc_layer, pdev->gralloc_handler.get_buffer_metadata(display, hwc_layer, index));
    window *target;
    task::placement placement;
    if (plan.identity.role == task::layer_role::task) {
        target = window_for_task(pdev, plan.identity);
        if (target && target->task->ready(*target)) {
            const task::window_sizing &sizing = target->task->sizing;
            placement = task::place_in_slot(frame, view, sizing.slot(), sizing.layer_scale());
        }
    } else {
        target = focused_window(pdev);
        if (target) {
            const task::logical_size size = target->task->sizing.size();
            int32_t gap = 0;
            if (plan.identity.role == task::layer_role::toast) {
                const int32_t margin = std::max(0, display->atlas_height - frame.bottom);
                gap = static_cast<int32_t>((input_method_height + margin) / display->scale);
                gap = std::min(gap, size.height / toast_gap_window_fraction);
            }
            placement = task::place_anchored_to_bottom(frame, view, size, display->scale, gap);
        }
    }
    if (!target || !target->task->ready(*target) || !placement.visible) {
        close_fence(hwc_layer->acquireFenceFd);
        return 0;
    }
    return show_layer(pdev, *target, hwc_layer, index, placement, plan.identity.role != task::layer_role::task);
}

int task_window_mode::post_processing(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *) {
    struct display *display = pdev->display;
    for (auto &[key, window] : display->windows) {
        task_window_state *task = window->task.get();
        if (!task || window->lastLayer == 0) {
            continue;
        }
        for (size_t l = window->lastLayer; l < task->layers_last_frame && l < window->layers.size(); ++l) {
            window::layer &unused = window->layers[l];
            wl_surface_attach(unused.surface, nullptr, 0, 0);
            wl_surface_commit(unused.surface);
            unused.shown_buffer = 0;
            display->input_map.erase(unused.surface);
        }
        task->layers_last_frame = window->lastLayer;
    }

    if (framebuffer) {
        int merged = -1;
        for (int release : framebuffer_releases) {
            if (merged < 0) {
                merged = release;
                continue;
            }
            const int both = sync_merge("task-framebuffer", merged, release);
            close(merged);
            close(release);
            merged = both;
        }
        framebuffer->releaseFenceFd = merged;
    }
    framebuffer_releases.clear();
    close_fence(framebuffer_acquire);
    return 0;
}

void task_window_mode::commit_windows(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *) {
    struct display *display = pdev->display;
    auto needs_commit = [](const window &window) {
        const task_window_state &task = *window.task;
        return task.ready(window) && (!task.mapped || task.dirty || task.sizing.size() != task.shown_size);
    };

    window *timed = focused_window(pdev);
    if (!timed || !needs_commit(*timed)) {
        timed = nullptr;
        for (auto &[key, window] : display->windows) {
            if (window->task && needs_commit(*window)) {
                timed = window.get();
                break;
            }
        }
    }

    frame_timed = false;
    for (auto &[key, window] : display->windows) {
        if (!window->task || !needs_commit(*window)) {
            continue;
        }
        task_window_state &task = *window->task;
        const task::logical_size size = task.sizing.size();
        const bool first_map = !task.mapped;
        if (first_map) {
            wl_surface_attach(window->surface, window->bg_buffer, 0, 0);
            wl_surface_damage(window->surface, 0, 0, INT32_MAX, INT32_MAX);
            wp_viewport_set_source(window->viewport, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(1),
                                   wl_fixed_from_int(1));
        }
        if (first_map || size != task.shown_size) {
            xdg_surface_set_window_geometry(window->xdg_surface, 0, 0, size.width, size.height);
            wp_viewport_set_destination(window->viewport, size.width, size.height);
            task.shown_size = size;
        }
        display->input_map.set(window->surface, task::slot_input_mapping(task.sizing.slot(), task.sizing.layer_scale()));
        if (window.get() == timed && display->presentation && display->sync) {
            struct wp_presentation_feedback *feedback = wp_presentation_feedback(display->presentation, window->surface);
            wp_presentation_feedback_add_listener(feedback, &frame_feedback_listener, new timed_frame{pdev, pdev->frame});
            frame_timed = true;
        }
        wl_surface_commit(window->surface);
        task.dirty = false;
        if (first_map) {
            task.mapped = true;
            activate_task_window(display, *window, display->tasks.take_activation_token(task.task_id));
        }
    }
}

int task_window_mode::retire_fence(waydroid_hwc_composer_device_1 *pdev, uint64_t frame) {
    fence::frame_sync *sync = pdev->display->sync.get();
    if (!sync) {
        return -1;
    }
    const int fence = sync->retire_fence(frame);
    /* A frame that is not timed retires the earlier ones at once; a timed
     * one waits for its presentation, but never more than one frame. */
    sync->frame_finished(frame_timed ? frame - 1 : frame);
    return fence;
}
