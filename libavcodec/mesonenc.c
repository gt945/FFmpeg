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

#include <stdbool.h>

#include "libavutil/avassert.h"
#include "libavutil/fifo.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_meson.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "internal.h"
#include <mesonenc.h>
#include <IONmem.h>

typedef struct
{
    AVClass *avclass;
    MesonEnc encoder;
    int frame_rate;
    int bitrate;
    int bitrate_factor;
    AVFifoBuffer *timestamp_list;

} MESONEncodeContext;

typedef struct {
    enum AVCodecID id;
    int enc_type;
} MESONCodecMap;

#define CODEC_MAP(id,fmt) \
        {AV_CODEC_ID_##id, MESON_ENC_FMT_##fmt}

static MESONCodecMap meson_codec_map[] = {
    CODEC_MAP(H264,        H264),
    CODEC_MAP(HEVC,        HEVC)
};
static int ff_mesonenc_init(AVCodecContext *avctx);
static int ff_mesonenc_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                          const AVFrame *frame, int *got_packet);
static int ff_mesonenc_close(AVCodecContext *avctx);

av_cold int ff_mesonenc_init(AVCodecContext *avctx)
{
    int ret;
    int i;
    MESONEncodeContext *ctx = (MESONEncodeContext *)avctx->priv_data;

    enum AVPixelFormat pix_fmt;
    if (avctx->pix_fmt == AV_PIX_FMT_MESON) {
        ctx->encoder.buffer_type = MESON_BUFFER_TYPE_ION;
        pix_fmt = avctx->sw_pix_fmt;
    } else {
        ctx->encoder.buffer_type = MESON_BUFFER_TYPE_YUV;
        pix_fmt = avctx->pix_fmt;
    }
    switch (pix_fmt) {
    case AV_PIX_FMT_NV21:
        ctx->encoder.pix_fmt = MESON_PIX_FMT_NV21;
        break;
    case AV_PIX_FMT_NV12:
        ctx->encoder.pix_fmt = MESON_PIX_FMT_NV12;
        break;
    default:
        return AVERROR(EINVAL);
    }

    for (i = 0; i < FF_ARRAY_ELEMS(meson_codec_map); i++) {
       if (meson_codec_map[i].id == avctx->codec->id) {
           ctx->encoder.enc_fmt = meson_codec_map[i].enc_type;
           break;
       }
   }

    ctx->frame_rate = avctx->time_base.den / avctx->time_base.num;
    ctx->bitrate = avctx->width * avctx->height * ctx->frame_rate * ctx->bitrate_factor / 100;

    ctx->encoder.width = avctx->width;
    ctx->encoder.height = avctx->height;
    ctx->encoder.frame_rate = ctx->frame_rate;
    ctx->encoder.bitrate = ctx->bitrate;
    ctx->timestamp_list = av_fifo_alloc(256 * sizeof(int64_t));

    if (!ctx->timestamp_list) {
        return AVERROR(ENOMEM);
    }

    ret = mesonenc_init(&ctx->encoder);
    if (ret < 0) {
        return ret;
    }

    av_log(avctx, AV_LOG_INFO,
        "timebase: %d/%d\n"
        "initQP: %d\n"
        "rate_control: %d\n"
        "auto_scd: %d\n"
        "num_ref_frame: %d\n"
        "num_slice_group: %d\n"
        "fullsearch: %d\n"
        "search_range: %d\n"
        "FreeRun: %d\n"
        "bitrate: %d\n",
        avctx->time_base.den,avctx->time_base.num, ctx->encoder.initQP,
        ctx->encoder.rate_control, ctx->encoder.auto_scd, ctx->encoder.num_ref_frame,
        ctx->encoder.num_slice_group, ctx->encoder.fullsearch, ctx->encoder.search_range,
        ctx->encoder.FreeRun, ctx->bitrate);
    return 0;
}

int ff_mesonenc_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                          const AVFrame *frame, int *got_packet)
{
    MESONEncodeContext *ctx = (MESONEncodeContext *)avctx->priv_data;
    int buff_size = 0;
    int flag;
    *got_packet = 0;
    if (frame) {
        buff_size = avctx->width * avctx->height * 2;
        av_fifo_generic_write(ctx->timestamp_list, &frame->pts, sizeof(frame->pts), NULL);
        ff_alloc_packet2(avctx, pkt, buff_size, 0);
        mesonenc_encode_frame(&ctx->encoder, frame->data, pkt->data, &pkt->size, got_packet, &flag);
        av_assert0(buff_size > pkt->size);
        if (!*got_packet) {
            av_packet_unref(pkt);
        } else {
            av_fifo_generic_read(ctx->timestamp_list, &pkt->pts, sizeof(pkt->pts), NULL);
            pkt->dts = pkt->pts;
            switch (flag) {
            case MESON_ENC_FRAME_IDR:
                pkt->flags |= AV_PKT_FLAG_KEY;
                break;
            }
        }
    }

    return 0;
}

av_cold int ff_mesonenc_close(AVCodecContext *avctx)
{
    MESONEncodeContext *ctx = (MESONEncodeContext *)avctx->priv_data;
    mesonenc_release(&ctx->encoder);
    if (ctx->timestamp_list) {
        av_fifo_freep(&ctx->timestamp_list);
    }
    return 0;
}

#define OFFSET(x) offsetof(MESONEncodeContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "init_qp",        "initial QP",                                       OFFSET(encoder.initQP),          AV_OPT_TYPE_INT,    { .i64 = 20 }, 15, 50, VE },
    { "rate_ctrl",      "rate control enable, on: RC on, off: constant QP", OFFSET(encoder.rate_control),    AV_OPT_TYPE_BOOL,   { .i64 =  0 }, 0,  1,  VE },
    { "auto_scd",       "scene change detection",                           OFFSET(encoder.auto_scd),        AV_OPT_TYPE_BOOL,   { .i64 =  1 }, 0,  1,  VE },
    { "ref_num",        "number of reference frame used",                   OFFSET(encoder.num_ref_frame),   AV_OPT_TYPE_INT,    { .i64 =  1 }, 1,  16, VE },
    { "slice_num",      "number of slice group",                            OFFSET(encoder.num_slice_group), AV_OPT_TYPE_INT,    { .i64 =  1 }, 1,  16, VE },
    { "full_search",    "full-pel full-search mode",                        OFFSET(encoder.fullsearch),      AV_OPT_TYPE_BOOL,   { .i64 =  1 }, 0,  1,  VE },
    { "search_range",   "search range for motion vector",                   OFFSET(encoder.search_range),    AV_OPT_TYPE_INT,    { .i64 = 16 }, 1,  64, VE },
    { "free_run",       "",                                                 OFFSET(encoder.FreeRun),         AV_OPT_TYPE_BOOL,   { .i64 =  1 }, 0,  1,  VE },
    { "bitrate_factor", "",                                                 OFFSET(bitrate_factor),          AV_OPT_TYPE_INT,    { .i64 =  8 }, 1,  20, VE },
    { NULL },
};

static const AVCodecDefault defaults[] = {
    { NULL },
};

#define MESON_ENC_CLASS(NAME) \
    static const AVClass meson_##NAME##_enc_class = { \
        .class_name = "meson_" #NAME "_enc", \
        .item_name = av_default_item_name, \
        .option = options, \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

#define MESON_ENC(NAME, ID) \
    MESON_ENC_CLASS(NAME) \
    AVCodec ff_##NAME##_meson_encoder = { \
        .name           = #NAME "_meson", \
        .long_name      = NULL_IF_CONFIG_SMALL(#NAME " (Amlogic Encoder)"), \
        .type           = AVMEDIA_TYPE_VIDEO, \
        .id             = AV_CODEC_ID_##ID, \
        .init           = ff_mesonenc_init, \
        .encode2        = ff_mesonenc_encode_frame, \
        .close          = ff_mesonenc_close, \
        .priv_data_size = sizeof(MESONEncodeContext), \
        .priv_class     = &meson_##NAME##_enc_class, \
        .defaults       = defaults, \
        .pix_fmts       = (const enum AVPixelFormat[]) { \
            AV_PIX_FMT_MESON, \
            AV_PIX_FMT_NV21, \
            AV_PIX_FMT_NONE, \
        }, \
        .capabilities   = AV_CODEC_CAP_DELAY, \
        .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,\
    };

MESON_ENC(h264,      H264)
MESON_ENC(hevc,      HEVC)
