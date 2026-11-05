/*
 * Copyright © 2025 Waydroid Project.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <cstddef>
#include <memory>

#include "wayland-hwc.h"

/// One plane of a dmabuf, as zwp_linux_buffer_params_v1.add takes it.
struct dmabuf_plane {
    int fd;
    uint32_t offset;
    uint32_t stride;
};

std::unique_ptr<buffer> create_shm_wl_buffer(display *display, const buffer_metadata& metadata, buffer_handle_t handle);
/// Imports every plane. `drm_format` < 0 means "derive it from the HAL format".
std::unique_ptr<buffer> create_dmabuf_wl_buffer(display *display, const buffer_metadata& metadata,
                                                const dmabuf_plane *planes, size_t plane_count, int drm_format,
                                                uint64_t modifier, buffer_handle_t handle);
std::unique_ptr<buffer> create_android_wl_buffer(display *display, const buffer_metadata& metadata, buffer_handle_t target);


class gralloc_handler {
    using get_buffer_metadata_func = buffer_metadata (*)(display *display, hwc_layer_1_t *layer, size_t pos);
    using create_buffer_func = std::unique_ptr<buffer> (*)(display *display, const buffer_metadata& metadata, buffer_handle_t handle);
    using update_shm_buffer_func = void (*)(display *display, buffer *buffer);
    using importable_func = bool (*)(display *display, const buffer_metadata &metadata, buffer_handle_t handle,
                                     const dmabuf::format_set &formats);
    using is_yuv_func = bool (*)(display *display, hwc_layer_1_t *layer, size_t pos);

    get_buffer_metadata_func get_buffer_metadata_impl;
    create_buffer_func create_buffer_impl;
    update_shm_buffer_func update_shm_buffer_impl;
    importable_func importable_impl;
    is_yuv_func is_yuv_impl;

    static get_buffer_metadata_func select_get_buffer_metadata_impl(GrallocType gralloc_type);
    static create_buffer_func select_create_buffer_impl(display *display, GrallocType gralloc_type);
    static update_shm_buffer_func select_update_shm_buffer_impl(GrallocType gralloc_type);
    static importable_func select_importable_impl(display *display, GrallocType gralloc_type);
    static is_yuv_func select_is_yuv_impl(GrallocType gralloc_type);

  public:
    gralloc_handler() = default;
    gralloc_handler(display *display);

    buffer_metadata get_buffer_metadata(display *display, hwc_layer_1_t *layer, size_t pos) const {
        return get_buffer_metadata_impl(display, layer, pos);
    }
    std::unique_ptr<buffer> create_buffer(display *display, const buffer_metadata& metadata, buffer_handle_t handle) const {
        return create_buffer_impl(display, metadata, handle);
    }
    void update_shm_buffer(display *display, buffer *buffer) const {
        update_shm_buffer_impl(display, buffer);
    }
    /// Whether create_buffer would make an import the compositor
    /// advertised; anything else goes through wl_shm, because an
    /// unadvertised import kills the connection.
    bool importable(display *display, const buffer_metadata &metadata, buffer_handle_t handle,
                    const dmabuf::format_set &formats) const {
        return importable_impl(display, metadata, handle, formats);
    }
    /// YUV layers are composed by SurfaceFlinger.
    bool is_yuv(display *display, hwc_layer_1_t *layer, size_t pos) const {
        return is_yuv_impl(display, layer, pos);
    }
};