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

#include "gralloc_handler.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>

#include <linux/memfd.h>
#include <sys/syscall.h>
#include <drm_fourcc.h>
#include <wayland-client.h>

#include <log/log.h>
#include <ui/Rect.h>
#include <ui/GraphicBufferMapper.h>
#include <gralloc_handle.h>
#include <cros_gralloc/cros_gralloc_handle.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "wayland-android-client-protocol.h"

#include "egl-tools.h"
#include "wayland-hwc.h"

namespace {
    /// Pixels reach the shm buffer as 32-bit BGRA (EGL readback) or swizzled
    /// RGBA/RGBX, so only alpha decides the format.
    uint32_t ConvertHalFormatToShm(uint32_t hal_format) {
        return hal_format == HAL_PIXEL_FORMAT_RGBX_8888 ? WL_SHM_FORMAT_XRGB8888 : WL_SHM_FORMAT_ARGB8888;
    }

    std::atomic<uint64_t> next_buffer_id{1};

    const wl_buffer_listener buffer_listener {
        [](void *, struct wl_buffer *) {}
    };
}
std::unique_ptr<buffer> create_shm_wl_buffer(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    std::unique_ptr<buffer> buf { new buffer() };

    // Assume 4bpp formats or none of this is going to work
    int shm_stride = metadata.width * 4;
    int size = shm_stride * metadata.height;

    buf->id = next_buffer_id++;
    buf->metadata = metadata;
    buf->handle = handle;

    buf->isShm = true;
    buf->size = size;

    auto shm_format = ConvertHalFormatToShm(metadata.format);

    int fd = syscall(SYS_memfd_create, "buffer", MFD_ALLOW_SEALING);
    ftruncate(fd, size);
    buf->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf->shm_data == MAP_FAILED) {
        ALOGE("mmap failed");
        close(fd);
        return nullptr;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(display->shm, fd, size);
    buf->wl_buffer = wl_shm_pool_create_buffer(pool, 0, metadata.width, metadata.height, shm_stride, shm_format);
    wl_buffer_add_listener(buf->wl_buffer, &buffer_listener, nullptr);
    wl_shm_pool_destroy(pool);
    close(fd);

    return buf;
}

namespace {
    /* Only advertised (format, modifier) pairs are imported, so a failure
     * here means the compositor broke its own feedback; the wl_buffer is
     * unusable and the connection is about to die anyway. */
    void create_failed(void *data __unused, zwp_linux_buffer_params_v1 *params __unused)
    {
        ALOGE("%s: zwp_linux_buffer_params.create_immed failed for an advertised format.", __func__);
        abort();
    }

    const struct zwp_linux_buffer_params_v1_listener params_listener = {
            [](void *, zwp_linux_buffer_params_v1 *, wl_buffer *) {},
            create_failed
    };

    bool isFormatSupported(struct display *display, uint32_t format) {
        return display->importable_formats()->has_format(format);
    }

    uint32_t ConvertHalFormatToDrm(struct display *display, uint32_t hal_format) {
        uint32_t fmt;

        switch (hal_format) {
            case HAL_PIXEL_FORMAT_RGB_888:
                fmt = DRM_FORMAT_BGR888;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_RGB888;
                break;
            case HAL_PIXEL_FORMAT_BGRA_8888:
                fmt = DRM_FORMAT_ARGB8888;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_ABGR8888;
                break;
            case HAL_PIXEL_FORMAT_RGBX_8888:
                fmt = DRM_FORMAT_XBGR8888;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_XRGB8888;
                break;
            case HAL_PIXEL_FORMAT_RGBA_8888:
                fmt = DRM_FORMAT_ABGR8888;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_ARGB8888;
                break;
            case HAL_PIXEL_FORMAT_RGB_565:
                fmt = DRM_FORMAT_BGR565;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_RGB565;
                break;
            case HAL_PIXEL_FORMAT_YV12:
                fmt = DRM_FORMAT_YVU420;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_GR88;
                break;
            default:
                ALOGE("Cannot convert hal format to drm format %u", hal_format);
                return -EINVAL;
        }
        if (!isFormatSupported(display, fmt)) {
            ALOGE("Current wayland display doesn't support hal format %u", hal_format);
            return -EINVAL;
        }
        return fmt;
    }
}
std::unique_ptr<buffer> create_dmabuf_wl_buffer(display *display, const buffer_metadata& metadata,
                                                const dmabuf_plane *planes, size_t plane_count, int drm_format,
                                                uint64_t modifier, buffer_handle_t handle)
{
    assert(plane_count > 0);

    std::unique_ptr<buffer> buf { new buffer() };

    buf->id = next_buffer_id++;
    buf->metadata = metadata;
    buf->handle = handle;
    buf->isDmabuf = true;

    if (drm_format < 0) {
        drm_format = ConvertHalFormatToDrm(display, metadata.format);
    }
    assert(drm_format >= 0);

    zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(display->dmabuf);
    for (size_t plane = 0; plane < plane_count; ++plane) {
        zwp_linux_buffer_params_v1_add(params, planes[plane].fd, plane, planes[plane].offset, planes[plane].stride,
                                       modifier >> 32, modifier & 0xffffffff);
    }
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, nullptr);

    buf->wl_buffer = zwp_linux_buffer_params_v1_create_immed(params, buf->metadata.width, buf->metadata.height, drm_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    wl_buffer_add_listener(buf->wl_buffer, &buffer_listener, nullptr);

    return buf;
}

std::unique_ptr<buffer> create_android_wl_buffer(display *display, const buffer_metadata& metadata, buffer_handle_t handle)
{
    std::unique_ptr<buffer> buf { new buffer() };

    buf->id = next_buffer_id++;
    buf->metadata = metadata;
    buf->handle = handle;

    struct wl_array ints;
    wl_array_init(&ints);
    int *the_ints = (int *)wl_array_add(&ints, handle->numInts * sizeof(int));
    memcpy(the_ints, handle->data + handle->numFds, handle->numInts * sizeof(int));
    android_wlegl_handle *wlegl_handle = android_wlegl_create_handle(display->android_wlegl, handle->numFds, &ints);
    wl_array_release(&ints);

    for (int i = 0; i < handle->numFds; i++) {
        android_wlegl_handle_add_fd(wlegl_handle, handle->data[i]);
    }

    buf->wl_buffer = android_wlegl_create_buffer(display->android_wlegl, buf->metadata.width, buf->metadata.height, buf->metadata.pixel_stride, metadata.format, GRALLOC_USAGE_HW_RENDER, wlegl_handle);
    android_wlegl_handle_destroy(wlegl_handle);

    wl_buffer_add_listener(buf->wl_buffer, &buffer_listener, nullptr);

    return buf;
}

namespace {
buffer_metadata get_buffer_metadata_generic(display *display, hwc_layer_1_t *layer, size_t pos) {
    uint32_t format, pixel_stride, width, height;
    if (layer->compositionType == HWC_FRAMEBUFFER_TARGET) {
        format = display->target_layer_handle_ext.format;
        pixel_stride = display->target_layer_handle_ext.stride;
        width = display->target_layer_handle_ext.width;
        height = display->target_layer_handle_ext.height;
    } else {
        format = display->layer_handles_ext[pos].format;
        pixel_stride = display->layer_handles_ext[pos].stride;
        width = display->layer_handles_ext[pos].width;
        height = display->layer_handles_ext[pos].height;
    }

    if (!width)
        width = layer->displayFrame.right - layer->displayFrame.left;
    if (!height)
        height = layer->displayFrame.bottom - layer->displayFrame.top;

    return {height, width, pixel_stride, format};
}
buffer_metadata get_buffer_metadata_gbm(display *, hwc_layer_1_t *layer, size_t) {
    auto handle = reinterpret_cast<const gralloc_handle_t *>(layer->handle);
    return {
            handle->height,
            handle->width,
            handle->stride,
            handle->format
    };
}
buffer_metadata get_buffer_metadata_cros(display *, hwc_layer_1_t *layer, size_t) {
    auto handle = reinterpret_cast<const cros_gralloc_handle *>(layer->handle);
    return {
            handle->height,
            handle->width,
            handle->pixel_stride,
            static_cast<uint32_t>(handle->droid_format)
    };
}

std::unique_ptr<buffer> create_buffer_generic(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    return create_shm_wl_buffer(display, metadata, handle);
}
std::unique_ptr<buffer> create_buffer_gbm(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    auto *drm_handle = reinterpret_cast<const gralloc_handle_t *>(handle);
    const dmabuf_plane plane{drm_handle->prime_fd, 0, drm_handle->stride};
    return create_dmabuf_wl_buffer(display, metadata, &plane, 1, -1 /* compute drm format */,
                                   drm_handle->modifier, handle);
}
std::unique_ptr<buffer> create_buffer_cros(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    auto *cros_handle = reinterpret_cast<const cros_gralloc_handle *>(handle);
    const uint32_t plane_count = std::clamp<uint32_t>(cros_handle->num_planes, 1, DRV_MAX_PLANES);
    dmabuf_plane planes[DRV_MAX_PLANES];
    for (uint32_t plane = 0; plane < plane_count; ++plane) {
        planes[plane] = {cros_handle->fds[plane], cros_handle->offsets[plane], cros_handle->strides[plane]};
    }
    return create_dmabuf_wl_buffer(display, metadata, planes, plane_count, cros_handle->format,
                                   cros_handle->format_modifier, handle);
}
std::unique_ptr<buffer> create_buffer_android(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    return create_android_wl_buffer(display, metadata, handle);
}

bool importable_gbm(display *display, const buffer_metadata &metadata, buffer_handle_t handle,
                    const dmabuf::format_set &formats) {
    auto *drm_handle = reinterpret_cast<const gralloc_handle_t *>(handle);
    const uint32_t drm_format = ConvertHalFormatToDrm(display, metadata.format);
    return drm_format != static_cast<uint32_t>(-EINVAL) && formats.contains(drm_format, drm_handle->modifier);
}
bool importable_cros(display *, const buffer_metadata &, buffer_handle_t handle, const dmabuf::format_set &formats) {
    auto *cros_handle = reinterpret_cast<const cros_gralloc_handle *>(handle);
    return formats.contains(cros_handle->format, cros_handle->format_modifier);
}
bool importable_always(display *, const buffer_metadata &, buffer_handle_t, const dmabuf::format_set &) {
    return true;
}

bool is_yuv_hal_format(uint32_t hal_format) {
    switch (hal_format) {
        case HAL_PIXEL_FORMAT_YV12:
        case HAL_PIXEL_FORMAT_YCBCR_420_888:
        case HAL_PIXEL_FORMAT_YCBCR_422_SP:
        case HAL_PIXEL_FORMAT_YCRCB_420_SP:
        case HAL_PIXEL_FORMAT_YCBCR_422_I:
        case HAL_PIXEL_FORMAT_YCBCR_P010:
            return true;
        default:
            return false;
    }
}
bool is_yuv_generic(display *display, hwc_layer_1_t *layer, size_t pos) {
    return is_yuv_hal_format(get_buffer_metadata_generic(display, layer, pos).format);
}
bool is_yuv_gbm(display *, hwc_layer_1_t *layer, size_t) {
    return is_yuv_hal_format(reinterpret_cast<const gralloc_handle_t *>(layer->handle)->format);
}
bool is_yuv_cros(display *, hwc_layer_1_t *layer, size_t) {
    switch (reinterpret_cast<const cros_gralloc_handle *>(layer->handle)->format) {
        case DRM_FORMAT_NV12:
        case DRM_FORMAT_NV21:
        case DRM_FORMAT_NV16:
        case DRM_FORMAT_NV61:
        case DRM_FORMAT_YUV420:
        case DRM_FORMAT_YVU420:
        case DRM_FORMAT_YUYV:
        case DRM_FORMAT_P010:
            return true;
        default:
            return false;
    }
}

void update_shm_buffer_generic(display *display, buffer *buffer) {
    display->egl_work_queue.emplace_back(std::bind(egl_render_to_pixels, display, buffer));
    sem_post(&display->egl_go);
    sem_wait(&display->egl_done);
}
void update_shm_buffer_default(display *, buffer *buffer) {
    void *data;
    uint32_t shm_stride, src_stride;
    android::Rect bounds(buffer->metadata.width, buffer->metadata.height);
    if (android::GraphicBufferMapper::get().lock(buffer->handle, GRALLOC_USAGE_SW_READ_OFTEN, bounds, &data) == 0) {
        src_stride = buffer->metadata.pixel_stride;
        shm_stride = buffer->metadata.width;
        for (int i = 0; i < buffer->metadata.height; i++) {
            uint32_t* source = (uint32_t*)data + (i * src_stride);
            uint32_t* dist = (uint32_t*)buffer->shm_data + (i * shm_stride);
            uint32_t* end = dist + shm_stride;

            while (dist < end) {
                uint32_t c = *source;
                *dist = (c & 0xFF00FF00) | ((c & 0xFF0000) >> 16) | ((c & 0xFF) << 16);
                source++;
                dist++;
            }
        }
        android::GraphicBufferMapper::get().unlock(buffer->handle);
    }
}
}

gralloc_handler::get_buffer_metadata_func gralloc_handler::select_get_buffer_metadata_impl(GrallocType gralloc_type) {
    switch (gralloc_type) {
        case GrallocType::GRALLOC_GBM:
            return get_buffer_metadata_gbm;
        case GrallocType::GRALLOC_CROS:
            return get_buffer_metadata_cros;
        case GrallocType::GRALLOC_ANDROID:
        case GrallocType::GRALLOC_DEFAULT:
            return get_buffer_metadata_generic;
        default:
            abort();
    }
}

gralloc_handler::create_buffer_func gralloc_handler::select_create_buffer_impl(display *display, GrallocType gralloc_type) {
    if (gralloc_type == GrallocType::GRALLOC_GBM && display->dmabuf) {
        return create_buffer_gbm;
    } else if (gralloc_type == GrallocType::GRALLOC_CROS && display->dmabuf) {
        return create_buffer_cros;
    } else if (gralloc_type == GrallocType::GRALLOC_ANDROID && display->android_wlegl) {
        return create_buffer_android;
    } else {
        return create_buffer_generic;
    }
}

gralloc_handler::importable_func gralloc_handler::select_importable_impl(display *display, GrallocType gralloc_type) {
    if (gralloc_type == GrallocType::GRALLOC_GBM && display->dmabuf) {
        return importable_gbm;
    } else if (gralloc_type == GrallocType::GRALLOC_CROS && display->dmabuf) {
        return importable_cros;
    } else {
        return importable_always;
    }
}

gralloc_handler::is_yuv_func gralloc_handler::select_is_yuv_impl(GrallocType gralloc_type) {
    switch (gralloc_type) {
        case GrallocType::GRALLOC_GBM:
            return is_yuv_gbm;
        case GrallocType::GRALLOC_CROS:
            return is_yuv_cros;
        default:
            return is_yuv_generic;
    }
}

gralloc_handler::update_shm_buffer_func gralloc_handler::select_update_shm_buffer_impl(GrallocType gralloc_type) {
    if (gralloc_type == GrallocType::GRALLOC_DEFAULT) {
        return update_shm_buffer_default;
    } else {
        return update_shm_buffer_generic;
    }
}

gralloc_handler::gralloc_handler(display *display)
    : get_buffer_metadata_impl(select_get_buffer_metadata_impl(display->gtype))
    , create_buffer_impl(select_create_buffer_impl(display, display->gtype))
    , update_shm_buffer_impl(select_update_shm_buffer_impl(display->gtype))
    , importable_impl(select_importable_impl(display, display->gtype))
    , is_yuv_impl(select_is_yuv_impl(display->gtype)) { }