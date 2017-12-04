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

#include "internal.h"
#include "avcodec.h"
#include "decode.h"
#include "h264_parse.h"
#include "hevc_parse.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavutil/buffer.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_meson.h"
#include "libavformat/avformat.h"
#include "bytestream.h"
#include "profiles.h"
#include <fcntl.h>
#include "mesondec.h"


typedef struct {
    int is_eos;
    int is_first;
    MesonDec handle;
    AVBufferRef *frames_ref;
    AVBufferRef *device_ref;
    AVFrame *frames[MESON_BUFFER_SIZE];
} MESONDecoder;

typedef struct {
    const AVClass *class;
    AVBufferRef *decoder_ref;
    AVBufferRef *header_ref;

} MESONDecodeContext;

typedef struct MESONFrameContext {
    int frame_index;
    AVBufferRef *decoder_ref;
} MESONFrameContext;


typedef struct {
    enum AVCodecID id;
    int video_type;
    unsigned int format;
} MESONCodecMap;

typedef struct {
    unsigned int codec_tag;
    unsigned int format;
} MESONVFMTMap;

#define CODEC_MAP(id,type,fmt) \
        {AV_CODEC_ID_##id, VFORMAT_##type, VIDEO_DEC_FORMAT_##fmt}

#define VFMT_MAP(tag, fmt) \
        {CODEC_TAG_##tag, VIDEO_DEC_FORMAT_##fmt}

static MESONCodecMap meson_codec_map[] = {
    CODEC_MAP(H264,        H264,     H264),
    CODEC_MAP(HEVC,        HEVC,     HEVC),
    CODEC_MAP(MPEG1VIDEO,  MPEG12,   UNKNOW),
    CODEC_MAP(MPEG2VIDEO,  MPEG12,   UNKNOW),
    CODEC_MAP(VP9,         VP9,      VP9),
    CODEC_MAP(VC1,         VC1,      WVC1),
    CODEC_MAP(WMV3,        VC1,      WMV3),
    CODEC_MAP(MPEG4,       MPEG4,    UNKNOW),
    CODEC_MAP(H263,        MPEG4,    H263),
    CODEC_MAP(FLV1,        MPEG4,    H263),
};

static MESONVFMTMap meson_vfmt_map[] = {
    VFMT_MAP(MP4V,  MPEG4_5),
    VFMT_MAP(mp4v,  MPEG4_5),
    VFMT_MAP(RMP4,  MPEG4_5),
    VFMT_MAP(MPG4,  MPEG4_5),
    VFMT_MAP(DIV6,  MPEG4_5),
    VFMT_MAP(DIV5,  MPEG4_5),
    VFMT_MAP(DX50,  MPEG4_5),
    VFMT_MAP(M4S2,  MPEG4_5),
    VFMT_MAP(FMP4,  MPEG4_5),
    VFMT_MAP(FVFW,  MPEG4_5),
    VFMT_MAP(XVID,  MPEG4_5),
    VFMT_MAP(xvid,  MPEG4_5),
    VFMT_MAP(XVIX,  MPEG4_5),
    VFMT_MAP(3IV2,  MPEG4_5),
    VFMT_MAP(3iv2,  MPEG4_5),
    VFMT_MAP(DIV4,  MPEG4_4),
    VFMT_MAP(DIVX,  MPEG4_4),
    VFMT_MAP(divx,  MPEG4_4),
    VFMT_MAP(COL1,  MPEG4_3),
    VFMT_MAP(DIV3,  MPEG4_3),
    VFMT_MAP(MP43,  MPEG4_3),
};


static void ff_meson_release_decoder(void *opaque, uint8_t *data)
{
    MESONDecoder *decoder = (MESONDecoder *)data;
    int i;

    mesondec_release(&decoder->handle);
    for (i = 0; i < MESON_BUFFER_SIZE; i++) {
        av_frame_free(&decoder->frames[i]);
    }
    av_buffer_unref(&decoder->frames_ref);
    av_buffer_unref(&decoder->device_ref);
    av_free(decoder);
}

static int ff_meson_decode_extradata(AVCodecContext *avctx)
{
    MESONDecodeContext *h = avctx->priv_data;
    int ret = -1;
    uint8_t *header_data = NULL;
    int header_size = 0;

    header_data = (uint8_t *)av_mallocz(4096 + avctx->extradata_size);
    if (!header_data) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    switch (avctx->codec->id) {
    case AV_CODEC_ID_H264:
        {
            H264ParamSets ps = {0};
            int is_avc, nal_length_size, i;
            uint8_t nal_header[4] = {0, 0, 0, 1};
            ff_h264_decode_extradata(avctx->extradata, avctx->extradata_size,
                                     &ps, &is_avc, &nal_length_size,
                                     avctx->err_recognition, avctx);
            if (is_avc) {
                for (i = 0; i < FF_ARRAY_ELEMS(ps.pps_list) && ps.pps_list[i]; i++) {
                    PPS *pps = (PPS *) ps.pps_list[i]->data;
                    memcpy(header_data + header_size, nal_header, 4);
                    header_size += 4;
                    memcpy(header_data + header_size, pps->data, pps->data_size);
                    header_size += pps->data_size;
                }
                for (i = 0; i < FF_ARRAY_ELEMS(ps.sps_list) && ps.sps_list[i]; i++) {
                    SPS *sps = (SPS *) ps.sps_list[i]->data;
                    memcpy(header_data + header_size, nal_header, 4);
                    header_size += 4;
                    memcpy(header_data + header_size, sps->data, sps->data_size);
                    header_size += sps->data_size;
                }
            }
            ff_h264_ps_uninit(&ps);
        }
        break;
    case AV_CODEC_ID_HEVC:
        {
            HEVCParamSets ps = {0};
            HEVCSEIContext sei = {0};
            int is_nalff, nal_length_size, i;
            uint8_t nal_header[4] = {0, 0, 0, 1};
            int sps_id = -1;
            ff_hevc_decode_extradata(avctx->extradata, avctx->extradata_size,
                                     &ps, &sei, &is_nalff, &nal_length_size,
                                     avctx->err_recognition, 1, avctx);
            for (i = 0; i < FF_ARRAY_ELEMS(ps.pps_list) && ps.pps_list[i]; i++) {
                HEVCPPS *pps = (HEVCPPS *) ps.pps_list[i]->data;
                memcpy(header_data + header_size, nal_header, 4);
                header_size += 4;
                memcpy(header_data + header_size, pps->data, pps->data_size);
                header_size += pps->data_size;
                if (sps_id < 0)
                    sps_id = pps->sps_id;
                av_buffer_unref(&ps.pps_list[i]);
            }
            for (i = 0; i < FF_ARRAY_ELEMS(ps.sps_list) && ps.sps_list[i]; i++) {
                HEVCSPS *sps = (HEVCSPS *) ps.sps_list[i]->data;
                if (i == sps_id && (!avctx->width || !avctx->height)) {
                    av_log(avctx, AV_LOG_ERROR, "size %dx%d\n", sps->width, sps->height);
                    avctx->width = sps->width;
                    avctx->height = sps->height;
                }
                memcpy(header_data + header_size, nal_header, 4);
                header_size += 4;
                memcpy(header_data + header_size, sps->data, sps->data_size);
                header_size += sps->data_size;
                av_buffer_unref(&ps.sps_list[i]);
            }
            for (i = 0; i < FF_ARRAY_ELEMS(ps.vps_list) && ps.vps_list[i]; i++) {
                HEVCVPS *vps = (HEVCVPS *) ps.vps_list[i]->data;
                memcpy(header_data + header_size, nal_header, 4);
                header_size += 4;
                memcpy(header_data + header_size, vps->data, vps->data_size);
                header_size += vps->data_size;
                av_buffer_unref(&ps.vps_list[i]);
            }
        }
        break;
    case AV_CODEC_ID_MPEG4:
        {
            uint32_t size;
            switch(avctx->codec_tag) {
            case CODEC_TAG_COL1:
            case CODEC_TAG_DIV3:
            case CODEC_TAG_MP43:
                size = (avctx->width << 12) | (avctx->height & 0xfff);
                header_data[0] = 0x00;
                header_data[1] = 0x00;
                header_data[2] = 0x00;
                header_data[3] = 0x01;
                header_data[4] = 0x20;
                header_data[5] = (size >> 16) & 0xff;
                header_data[6] = (size >> 8) & 0xff;
                header_data[7] = size & 0xff;
                header_data[8] = 0x00;
                header_data[9] = 0x00;
                header_size = 10;
                break;
            default:
                break;
            }
        }
        break;
    default:
        break;
    }
    av_buffer_unref(&h->header_ref);
    if (header_size > 0) {
        h->header_ref = av_buffer_create(header_data, header_size,
                   NULL, NULL, AV_BUFFER_FLAG_READONLY);
       if (!h->header_ref) {
           ret = AVERROR(ENOMEM);
           goto fail;
       }
    } else {
        av_free(header_data);
    }

    return 0;

fail:
    if (header_data) {
        av_free(header_data);
    }
    return ret;
}

static int ff_meson_context_init(AVCodecContext *avctx)
{
    MESONDecodeContext *h = avctx->priv_data;
    MESONDecoder *decoder = NULL;
    AVHWFramesContext *hwframes = NULL;
    int ret;

    decoder = av_mallocz(sizeof(*decoder));
    if (!decoder) {
        av_log(avctx, AV_LOG_ERROR, "av_mallocz for decoder failed\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    h->decoder_ref = av_buffer_create((uint8_t *) decoder, sizeof(*decoder),
            ff_meson_release_decoder, NULL, AV_BUFFER_FLAG_READONLY);
    if (!h->decoder_ref) {
        av_log(avctx, AV_LOG_ERROR,
                "av_buffer_create for decoder_ref failed\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    decoder->device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MESON);
    if (!decoder->device_ref) {
        av_log(avctx, AV_LOG_ERROR,
                "av_hwdevice_ctx_alloc for device_ref failed\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = av_hwdevice_ctx_init(decoder->device_ref);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "av_hwdevice_ctx_init failed\n");
        goto fail;
    }

    decoder->frames_ref = av_hwframe_ctx_alloc(decoder->device_ref);
    if (!decoder->frames_ref) {
        av_log(avctx, AV_LOG_ERROR,
                "av_hwframe_ctx_alloc for frames_ref failed\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    hwframes = (AVHWFramesContext*)decoder->frames_ref->data;
    hwframes->format            = AV_PIX_FMT_MESON;
    hwframes->sw_format         = AV_PIX_FMT_NV21;
    hwframes->width             = FFALIGN(avctx->width, 32);
    hwframes->height            = avctx->height;
    hwframes->initial_pool_size = MESON_BUFFER_SIZE;

    ret = av_hwframe_ctx_init(decoder->frames_ref);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "av_hwframe_ctx_init failed\n");
        goto fail;
    }

    return 0;
fail:
    return ret;
}

static int ff_meson_driver_init(AVCodecContext *avctx)
{
    MESONDecodeContext *h = avctx->priv_data;
    MESONDecoder *decoder = (MESONDecoder *)h->decoder_ref->data;
    AVHWFramesContext* hwframes = (AVHWFramesContext*)decoder->frames_ref->data;
    int i, ret = -1;

    switch(hwframes->sw_format) {
    case AV_PIX_FMT_NV12:
        decoder->handle.pix_fmt = MESON_PIX_FMT_NV12;
        break;
    case AV_PIX_FMT_NV21:
        decoder->handle.pix_fmt = MESON_PIX_FMT_NV21;
        break;
    default:
        ret = AVERROR(EINVAL);
        goto fail;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(meson_codec_map); i++) {
        if (meson_codec_map[i].id == avctx->codec->id) {
            decoder->handle.video_type = meson_codec_map[i].video_type;
            decoder->handle.format =  meson_codec_map[i].format;
            break;
        }
    }

    if (decoder->handle.format == VIDEO_DEC_FORMAT_UNKNOW) {
        for (i = 0; i < FF_ARRAY_ELEMS(meson_vfmt_map); i++) {
            if (meson_vfmt_map[i].codec_tag == avctx->codec_tag) {
                decoder->handle.format =  meson_vfmt_map[i].format;
                break;
            }
        }
    }

    decoder->handle.width = avctx->width;
    decoder->handle.height = avctx->height;
    decoder->handle.rate = av_rescale(90000, avctx->framerate.den,
                avctx->framerate.num);

    if ((ret = mesondec_init(&decoder->handle, avctx->extradata, avctx->extradata_size))) {
        av_log(avctx, AV_LOG_ERROR, "mesondec_codec_init failed\n");
        goto fail;
    }

    for (i = 0; i < MESON_BUFFER_SIZE; i++) {
        decoder->frames[i] = av_mallocz(sizeof(AVFrame));
        if (!decoder->frames[i]) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ret = av_hwframe_get_buffer(decoder->frames_ref, decoder->frames[i], 0);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "av_hwframe_get_buffer failed\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }

    for (i = 0; i < MESON_BUFFER_SIZE; i++) {
        IONMEM_AllocParams *ionmem;
        ionmem = (IONMEM_AllocParams *)decoder->frames[i]->data[3];
        ret = mesondec_put_buffer(&decoder->handle, ionmem, i);
        if (ret) {
            av_log(NULL, AV_LOG_ERROR, "mesondec_put_buffer failed %d %d\n", __LINE__, ret);
            goto fail;
        }
    }

    av_log(avctx, AV_LOG_ERROR,
            "codec_init %x with type:%d fmt:%d sz:%dx%d rate:%d/%d extra:%d\n",
            avctx->codec_tag, decoder->handle.video_type,
            decoder->handle.format, avctx->width, avctx->height,
            avctx->framerate.num, avctx->framerate.den, avctx->extradata_size);

    decoder->is_eos = 0;
    decoder->is_first = 1;

    return 0;

fail:
    return ret;
}

static int ff_meson_decode_close(AVCodecContext *avctx)
{
    MESONDecodeContext *h = avctx->priv_data;
//    MESONDecoder *decoder = (MESONDecoder *)h->decoder_ref->data;

    av_buffer_unref(&h->decoder_ref);
    av_buffer_unref(&h->header_ref);

    return 0;
}

static int ff_meson_decode_init(AVCodecContext *avctx)
{
    int ret = -1;

    ff_meson_decode_extradata(avctx);
    if (!avctx->width || !avctx->height) {
        av_log(avctx, AV_LOG_ERROR, "aml_codec_init not support size:0x0\n");
        goto fail;
    }

    if ((ret = ff_meson_context_init(avctx))) {
        av_log(avctx, AV_LOG_ERROR, "meson_context_init failed\n");
        goto fail;
    }

    if ((ret = ff_meson_driver_init(avctx))) {
        av_log(avctx, AV_LOG_ERROR, "meson_driver_init failed\n");
        goto fail;
    }

    return 0;
fail:
    ff_meson_decode_close(avctx);
    return ret;
}

static int ff_meson_enqueue_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    MESONDecodeContext *h = avctx->priv_data;
    MESONDecoder *decoder = (MESONDecoder *)h->decoder_ref->data;
    int ret = 0;

    if (pkt->size) {
        if (decoder->is_first && h->header_ref) {
            mesondec_header_write(&decoder->handle);
            decoder->is_first = 0;
        }
        mesondec_checkin_pts(&decoder->handle, pkt->pts * 90LL / 1000LL + 1LL);

        if (pkt) {
            ret = mesondec_packet_write(&decoder->handle, pkt->data, pkt->size);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to write data to decoder (code = %d)\n", ret);
            }
        }
    } else {
        decoder->is_eos = 1;
    }

    return ret;
}

static void ff_meson_release_frame(void *opaque, uint8_t *data)
{
    AVBufferRef *framecontextref = (AVBufferRef *)opaque;
    MESONFrameContext *framecontext = (MESONFrameContext *)framecontextref->data;
    MESONDecoder *decoder = (MESONDecoder *)framecontext->decoder_ref->data;
    int ret, index;
    IONMEM_AllocParams *ionmem;
    index = framecontext->frame_index;
    ionmem = (IONMEM_AllocParams *)decoder->frames[index]->data[3];

    ret = mesondec_put_buffer(&decoder->handle, ionmem, index);
    if (ret) {
        av_log(NULL, AV_LOG_ERROR, "mesondec_put_buffer failed %d %d\n", __LINE__, ret);
    }
    av_buffer_unref(&framecontext->decoder_ref);
    av_buffer_unref(&framecontextref);

}
static int ff_meson_dequeue_frame(AVCodecContext *avctx, AVFrame *frame)
{
    MESONDecodeContext *h = avctx->priv_data;
    MESONDecoder *decoder = (MESONDecoder *)h->decoder_ref->data;
    MESONFrameContext *framecontext = NULL;
    AVBufferRef *framecontextref = NULL;
    IONMEM_AllocParams *ionmem = NULL;
    MesonDecBuffer buffer;
    int ret = -1;
    ret = mesondec_get_buffer(&decoder->handle, &buffer);
    if (ret < 0) {
        if (decoder->is_eos && !mesondec_frame_ready(&decoder->handle)) {
            ret = AVERROR_EOF;
        } else {
            ret = AVERROR(EAGAIN);
        }
        goto fail;
    } else {
//        av_log(avctx, AV_LOG_INFO, "amlv4l_dequeuebuf frame:"
//                "fd %d length %d size %dx%d pts %lld\n", buffer.fd, buffer.length, buffer.width,
//                buffer.height, buffer.pts);
        ionmem = (IONMEM_AllocParams *)decoder->frames[buffer.index]->data[3];
        if (!buffer.width || !buffer.height) {
            ret = mesondec_put_buffer(&decoder->handle, ionmem, buffer.index);
            if (ret) {
                av_log(NULL, AV_LOG_ERROR, "mesondec_put_buffer failed %d %d\n", __LINE__, ret);
            } else {
                ret = AVERROR(EAGAIN);
            }
            goto fail;
        }

        framecontextref = av_buffer_allocz(sizeof(*framecontext));
        if (!framecontextref) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        framecontext = (MESONFrameContext *)framecontextref->data;
        framecontext->decoder_ref = av_buffer_ref(h->decoder_ref);
        framecontext->frame_index = buffer.index;

        frame->format    = AV_PIX_FMT_MESON;
        frame->width     = buffer.width;
        frame->height    = buffer.height;
        frame->pts       = buffer.pts;
        frame->data[3]   = (uint8_t *)ionmem;
        frame->buf[0]    = av_buffer_create((uint8_t *) ionmem, sizeof(*ionmem),
                ff_meson_release_frame, framecontextref, AV_BUFFER_FLAG_READONLY);

        if (!frame->buf[0]) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        frame->hw_frames_ctx = av_buffer_ref(decoder->frames_ref);
        if (!frame->hw_frames_ctx) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }

    return 0;
fail:
    if (framecontext)
        av_buffer_unref(&framecontext->decoder_ref);

    if (framecontextref)
        av_buffer_unref(&framecontextref);

    if (ionmem)
        av_free(ionmem);
    return ret;
}

static int ff_meson_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    MESONDecodeContext *h = avctx->priv_data;
    MESONDecoder *decoder = (MESONDecoder *)h->decoder_ref->data;
    int ret;
    AVPacket pkt = {0};
    if (!decoder->is_eos) {
       ret = ff_decode_get_packet(avctx, &pkt);
       if (ret < 0 && ret != AVERROR_EOF) {
           goto error;
       }
       ret = ff_meson_enqueue_packet(avctx, &pkt);
       av_packet_unref(&pkt);

       if (ret < 0) {
           av_log(avctx, AV_LOG_ERROR, "Failed to send packet to decoder (code = %d)\n", ret);
           return ret;
       }
   }
error:
   return ff_meson_dequeue_frame(avctx, frame);
}

static void ff_meson_decode_flush(AVCodecContext *avctx)
{
    MESONDecodeContext *h = avctx->priv_data;
    MESONDecoder *decoder = (MESONDecoder *)h->decoder_ref->data;
    int ret;
    ret = mesondec_flush(&decoder->handle);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to reset codec (code = %d)\n", ret);
    } else {
        decoder->is_eos = 0;
        decoder->is_first = 1;
    }
}

#define MESON_DEC_CLASS(NAME) \
    static const AVClass meson_##NAME##_dec_class = { \
        .class_name = "meson_" #NAME "_dec", \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

#define MESON_DEC(NAME, ID, BSFS) \
    MESON_DEC_CLASS(NAME) \
    AVCodec ff_##NAME##_meson_decoder = { \
        .name           = #NAME "_meson", \
        .long_name      = NULL_IF_CONFIG_SMALL(#NAME " (Amlogic Decoder)"), \
        .type           = AVMEDIA_TYPE_VIDEO, \
        .id             = AV_CODEC_ID_##ID, \
        .priv_data_size = sizeof(MESONDecodeContext), \
        .init           = ff_meson_decode_init, \
        .close          = ff_meson_decode_close, \
        .receive_frame  = ff_meson_receive_frame, \
        .flush          = ff_meson_decode_flush, \
        .priv_class     = &meson_##NAME##_dec_class, \
        .capabilities   = AV_CODEC_CAP_DELAY \
                            | AV_CODEC_CAP_AVOID_PROBING, \
        .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,\
        .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_MESON, \
                                                         AV_PIX_FMT_NONE}, \
        .bsfs           = BSFS, \
    };

MESON_DEC(h264,      H264,          "h264_mp4toannexb")
MESON_DEC(hevc,      HEVC,          "hevc_mp4toannexb")
MESON_DEC(mpeg1,     MPEG1VIDEO,    NULL)
MESON_DEC(mpeg2,     MPEG2VIDEO,    NULL)
MESON_DEC(vp9,       VP9,           NULL)
MESON_DEC(vc1,       VC1,           NULL)
MESON_DEC(wmv3,      WMV3,          NULL)
MESON_DEC(mpeg4,     MPEG4,         NULL)
MESON_DEC(h263,      H263,          NULL)
MESON_DEC(flv,       FLV1,          NULL)
