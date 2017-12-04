/*
 * Amlogic Media Driver
 * Copyright (c) 2018 Tao Guo
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"
#include <sys/mman.h>

#include <fcntl.h>
#if HAVE_UNISTD_H
#   include <unistd.h>
#endif


#include "avassert.h"
#include "buffer.h"
#include "common.h"
#include "hwcontext.h"
#include "hwcontext_internal.h"
#include "hwcontext_meson.h"
#include "mem.h"
#include "pixdesc.h"
#include "pixfmt.h"
#include "imgutils.h"


typedef struct MESONDevicePriv {
} MESONDevicePriv;

//typedef struct MESONDeviceContext {
//
//} MESONDeviceContext;

#define MAP(fmt, av) { \
        PIXEL_FORMAT_ ## fmt, \
        AV_PIX_FMT_ ## av \
    }
static MESONFormat meson_formats[] = {
    MAP(RGBA_8888, RGBA),
    MAP(RGBX_8888, RGB0),
    MAP(RGB_888, RGB24),
    MAP(RGB_565, RGB565),
    MAP(BGRA_8888, BGRA),
    MAP(YV12, YUV420P),
    MAP(Y8, GRAY8),
    MAP(YCbCr_422_SP, NV16),
    MAP(YCrCb_420_SP, NV21),
    MAP(YCbCr_422_I, YUYV422)
};
#undef MAP

static int meson_frames_get_constraints(AVHWDeviceContext *hwdev,
                                        const void *hwconfig,
                                        AVHWFramesConstraints *constraints)
{
    AVMESONDeviceContext *hwctx = hwdev->hwctx;
//    MESONDeviceContext *ctx = hwdev->internal->priv;
    enum AVPixelFormat pix_fmt;
    int err, i;

    constraints->valid_sw_formats = av_malloc_array(hwctx->nb_formats + 1,
                                               sizeof(pix_fmt));
    if (!constraints->valid_sw_formats) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    for (i = 0; i < hwctx->nb_formats; i++)
        constraints->valid_sw_formats[i] = hwctx->formats[i].pix_fmt;
    constraints->valid_sw_formats[i] = AV_PIX_FMT_NONE;
    
    constraints->valid_hw_formats = av_malloc_array(2, sizeof(pix_fmt));
    if (!constraints->valid_hw_formats) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    constraints->valid_hw_formats[0] = AV_PIX_FMT_MESON;
    constraints->valid_hw_formats[1] = AV_PIX_FMT_NONE;

    err = 0;
fail:
    return err;
}

static int meson_device_init(AVHWDeviceContext *hwdev)
{
//    MESONDeviceContext *ctx = hwdev->internal->priv;
    AVMESONDeviceContext *hwctx = hwdev->hwctx;
    int err;
    
    err = CMEM_init();
    if (err < 0) {
        av_log(hwctx, AV_LOG_ERROR, "%s error %d\n",
                __FUNCTION__, __LINE__);
        goto fail;
    }
    hwctx->formats  = meson_formats;
    hwctx->nb_formats = sizeof(meson_formats) / sizeof(MESONFormat);

    return 0;
fail:
    return err;
}

static void meson_device_uninit(AVHWDeviceContext *hwdev)
{
//    MESONDeviceContext *ctx = hwdev->internal->priv;
    CMEM_exit();
}

static void meson_buffer_free(void *opaque, uint8_t *data)
{
//    AVHWFramesContext     *hwfc = opaque;
//    AVMESONDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    IONMEM_AllocParams  *ionmem;

    ionmem = (IONMEM_AllocParams *)data;
    CMEM_free(ionmem);
    av_freep(&ionmem);
}

static AVBufferRef *meson_pool_alloc(void *opaque, int size)
{
    AVHWFramesContext     *hwfc = opaque;
//    MESONFramesContext     *ctx = hwfc->internal->priv;
//    AVMESONDeviceContext *hwctx = hwfc->device_ctx->hwctx;
//    AVMESONFrameContext  *avfc = hwfc->hwctx;

    IONMEM_AllocParams  *ionmem;
    AVBufferRef *ref;
    int buff_size;
    unsigned long err;

    buff_size = av_image_get_buffer_size(hwfc->sw_format,
                hwfc->width, hwfc->height, 32);
    
    ionmem = av_mallocz(sizeof(IONMEM_AllocParams));
    if (!ionmem) {
        av_log(hwfc, AV_LOG_ERROR, "%s error %d\n",
                __FUNCTION__, __LINE__);
        goto fail;
    }
    
    err = (int)CMEM_alloc(buff_size, ionmem);
    if (err) {
        av_log(hwfc, AV_LOG_ERROR, "%s error %d\n",
                __FUNCTION__, __LINE__);
        goto fail;
    }
    ionmem->size = buff_size;
    ref = av_buffer_create((uint8_t*)ionmem,
                sizeof(IONMEM_AllocParams), &meson_buffer_free,
                hwfc, AV_BUFFER_FLAG_READONLY);
    if (!ref) {
        av_log(hwfc, AV_LOG_ERROR, "%s error %d\n",
                __FUNCTION__, __LINE__);
        goto fail;
    }

    return ref;
fail:
    
    if (ionmem) {
        if (ionmem->size) {
            CMEM_free(ionmem);
        }
        av_freep(&ionmem);
    }
    return NULL;
}

static int meson_frames_init(AVHWFramesContext *hwfc)
{
//    AVMESONFrameContext  *avfc = hwfc->hwctx;
//    MESONFramesContext     *ctx = hwfc->internal->priv;
//    AVMESONDeviceContext *hwctx = hwfc->device_ctx->hwctx;

    int err;
    if (!hwfc->pool) {
        hwfc->internal->pool_internal =
            av_buffer_pool_init2(sizeof(int), hwfc,
                             &meson_pool_alloc, NULL);
        if (!hwfc->internal->pool_internal) {
            av_log(hwfc, AV_LOG_ERROR, "Failed to create MESON surface pool.\n");
            err = AVERROR(ENOMEM);
            goto fail;
        }
    }
    return 0;

fail:
    return err;
}

static void meson_frames_uninit(AVHWFramesContext *hwfc)
{
//    AVMESONFrameContext *avfc = hwfc->hwctx;
//    MESONFramesContext    *ctx = hwfc->internal->priv;
}

static int meson_get_buffer(AVHWFramesContext *hwfc, AVFrame *frame)
{
    frame->buf[0] = av_buffer_pool_get(hwfc->pool);
    if (!frame->buf[0])
        return AVERROR(ENOMEM);

    frame->data[3] = frame->buf[0]->data;
    frame->format  = AV_PIX_FMT_MESON;
    frame->width   = hwfc->width;
    frame->height  = hwfc->height;

    return 0;
}

static int meson_transfer_get_formats(AVHWFramesContext *hwfc,
                                      enum AVHWFrameTransferDirection dir,
                                      enum AVPixelFormat **formats)
{
//    MESONDeviceContext *ctx = hwfc->device_ctx->internal->priv;
    AVMESONDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    enum AVPixelFormat *pix_fmts, preferred_format;
    int i, k;

    preferred_format = hwfc->sw_format;

    pix_fmts = av_malloc((hwctx->nb_formats + 1) * sizeof(*pix_fmts));
    if (!pix_fmts)
        return AVERROR(ENOMEM);

    pix_fmts[0] = preferred_format;
    k = 1;
    for (i = 0; i < hwctx->nb_formats; i++) {
        if (hwctx->formats[i].pix_fmt == preferred_format)
            continue;
        av_assert0(k < hwctx->nb_formats);
        pix_fmts[k++] = hwctx->formats[i].pix_fmt;
    }
    av_assert0(k == hwctx->nb_formats);
    pix_fmts[k] = AV_PIX_FMT_NONE;

    *formats = pix_fmts;
    return 0;
}

static void meson_unmap_frame(AVHWFramesContext *hwfc,
                              HWMapDescriptor *hwmap)
{
//    AVMESONDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    IONMEM_AllocParams  *ionmem = hwmap->priv;

    munmap(ionmem->usr_ptr, ionmem->size);
}

static int meson_map_frame(AVHWFramesContext *hwfc,
                           AVFrame *dst, const AVFrame *src, int flags)
{
//    AVMESONDeviceContext *hwctx = hwfc->device_ctx->hwctx;
//    MESONFramesContext *ctx = hwfc->internal->priv;
    IONMEM_AllocParams  *ionmem;
    uint8_t *address = NULL;
    int err, i, num_planes, offset;
    int h_shift, v_shift;
    ionmem = (IONMEM_AllocParams *)src->data[3];

    if (dst->format == AV_PIX_FMT_NONE)
        dst->format = hwfc->sw_format;
    if (dst->format != hwfc->sw_format && (flags & AV_HWFRAME_MAP_DIRECT)) {
        // Requested direct mapping but the formats do not match.
        return AVERROR(EINVAL);
    }
    
//    av_log(NULL, AV_LOG_ERROR, "map %p %d\n", ionmem, ionmem->size);
    ionmem->usr_ptr = mmap(NULL, ionmem->size,
            PROT_READ | PROT_WRITE, MAP_SHARED, ionmem->mImageFd, 0);
    if (ionmem->usr_ptr == MAP_FAILED) {
        err = AVERROR(ENOMEM);
        av_log(hwfc, AV_LOG_ERROR, "%s error %d\n",
                __FUNCTION__, __LINE__);
        goto fail;
    }
    address = ionmem->usr_ptr;
    err = ff_hwframe_map_create(src->hw_frames_ctx,
                                dst, src, &meson_unmap_frame, ionmem);
    if (err < 0) {
        av_log(hwfc, AV_LOG_ERROR, "%s error %d\n",
                __FUNCTION__, __LINE__);
        goto fail;
    }

    dst->width  = src->width;
    dst->height = src->height;

    num_planes = av_pix_fmt_count_planes(dst->format);
    av_pix_fmt_get_chroma_sub_sample(dst->format, &h_shift, &v_shift);
    offset = 0;
    for (i = 0; i < num_planes; i++) {
        int h = dst->height;
        if (i == 1 || i == 2) {
            h = AV_CEIL_RSHIFT(dst->height, v_shift);
        }
        dst->data[i] = address + offset;
        dst->linesize[i] = av_image_get_linesize(dst->format, dst->width, i);
        offset += dst->linesize[i] * h;
    }

    return 0;

fail:
    if (address) {
        munmap(address, ionmem->size);
    }
    return err;
}

static int meson_transfer_data_from(AVHWFramesContext *hwfc,
                                    AVFrame *dst, const AVFrame *src)
{
    AVFrame *map;
    int err;

    if (dst->width > hwfc->width || dst->height > hwfc->height)
        return AVERROR(EINVAL);

    map = av_frame_alloc();
    if (!map)
        return AVERROR(ENOMEM);
    map->format = dst->format;

    err = meson_map_frame(hwfc, map, src, AV_HWFRAME_MAP_READ);
    if (err)
        goto fail;

    map->width  = dst->width;
    map->height = dst->height;

    err = av_frame_copy(dst, map);
    if (err)
        goto fail;

    err = 0;
fail:
    av_frame_free(&map);
    return err;
}

static int meson_transfer_data_to(AVHWFramesContext *hwfc,
                                  AVFrame *dst, const AVFrame *src)
{
    AVFrame *map;
    int err;

    if (src->width > hwfc->width || src->height > hwfc->height)
        return AVERROR(EINVAL);

    map = av_frame_alloc();
    if (!map)
        return AVERROR(ENOMEM);
    map->format = src->format;

    err = meson_map_frame(hwfc, map, dst, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE);
    if (err)
        goto fail;

    map->width  = src->width;
    map->height = src->height;

    err = av_frame_copy(map, src);
    if (err)
        goto fail;

    err = 0;
fail:
    av_frame_free(&map);
    return err;
}

static void meson_device_free(AVHWDeviceContext *ctx)
{
//    AVMESONDeviceContext *hwctx = ctx->hwctx;
    MESONDevicePriv      *priv  = ctx->user_opaque;

    av_freep(&priv);
}

static int meson_device_create(AVHWDeviceContext *ctx, const char *device,
                               AVDictionary *opts, int flags)
{
    MESONDevicePriv *priv;
    priv = av_mallocz(sizeof(*priv));
    if (!priv)
        return AVERROR(ENOMEM);

    ctx->user_opaque = priv;
    ctx->free        = meson_device_free;

    return 0;
}

static int meson_device_derive(AVHWDeviceContext *ctx,
                               AVHWDeviceContext *src_ctx, int flags)
{
    av_log(ctx, AV_LOG_ERROR, "%s %d\n", __FILE__, __LINE__);
    return AVERROR(ENOSYS);
}

const HWContextType ff_hwcontext_type_meson = {
    .type                   = AV_HWDEVICE_TYPE_MESON,
    .name                   = "MESON",

    .device_hwctx_size      = sizeof(AVMESONDeviceContext),
//    .device_priv_size       = sizeof(MESONDeviceContext),
//    .device_hwconfig_size   = sizeof(AVMESONHWConfig),
//    .frames_hwctx_size      = sizeof(AVMESONFrameContext),
    .frames_priv_size       = sizeof(MESONFramesContext),

    .device_create          = &meson_device_create,
    .device_derive          = &meson_device_derive,
    .device_init            = &meson_device_init,
    .device_uninit          = &meson_device_uninit,
    .frames_get_constraints = &meson_frames_get_constraints,
    .frames_init            = &meson_frames_init,
    .frames_uninit          = &meson_frames_uninit,
    .frames_get_buffer      = &meson_get_buffer,
    .transfer_get_formats   = &meson_transfer_get_formats,
    .transfer_data_to       = &meson_transfer_data_to,
    .transfer_data_from     = &meson_transfer_data_from,

    .pix_fmts = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_MESON,
        AV_PIX_FMT_NONE
    },
};
