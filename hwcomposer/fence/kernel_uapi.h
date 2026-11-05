/*
 * Copyright © 2026 CrownOS
 * SPDX-License-Identifier: MIT
 *
 * Kernel interfaces newer than the Android 13 bionic headers, with the
 * layouts of Linux 6.6+ (syncobj eventfd) and 6.0+ (dma-buf sync_file export).
 */

#pragma once

#include <drm/drm.h>
#include <drm/vgem_drm.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#ifndef DRM_IOCTL_SYNCOBJ_EVENTFD
struct drm_syncobj_eventfd {
    __u32 handle;
    __u32 flags;
    __u64 point;
    __s32 fd;
    __u32 pad;
};
#define DRM_IOCTL_SYNCOBJ_EVENTFD DRM_IOWR(0xCF, struct drm_syncobj_eventfd)
#endif

#ifndef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
struct dma_buf_export_sync_file {
    __u32 flags;
    __s32 fd;
};
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR(DMA_BUF_BASE, 2, struct dma_buf_export_sync_file)
#endif
