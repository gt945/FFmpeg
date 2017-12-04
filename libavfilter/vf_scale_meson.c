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

#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_meson.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "scale.h"
#include "video.h"

typedef struct {
    const AVClass *class;

    AVMESONDeviceContext *hwctx;
    AVBufferRef *device_ref;

    AVBufferRef       *input_frames_ref;
    AVHWFramesContext *input_frames;

    AVBufferRef       *output_frames_ref;
    AVHWFramesContext *output_frames;

    char *output_format_string;
    enum AVPixelFormat output_format;

    char *w_expr;      // width expression string
    char *h_expr;      // height expression string

    int output_width;  // computed width
    int output_height; // computed height
} ScaleMesonContext;

static pixel_format_t get_ge2d_pix_fmt(ScaleMesonContext *ctx, enum AVPixelFormat pix_fmt)
{
    AVMESONDeviceContext *hwctx = ctx->hwctx;
    int i;
    for (i = 0; i < hwctx->nb_formats; i++)
        if (hwctx->formats[i].pix_fmt == pix_fmt)
            return hwctx->formats[i].ge2d_fmt;
    return -1;
}
static int scale_meson_query_formats(AVFilterContext *avctx)
{
    enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_MESON, AV_PIX_FMT_NONE,
    };
    int err;

    if ((err = ff_formats_ref(ff_make_format_list(pix_fmts),
                              &avctx->inputs[0]->out_formats)) < 0)
        return err;
    if ((err = ff_formats_ref(ff_make_format_list(pix_fmts),
                              &avctx->outputs[0]->in_formats)) < 0)
        return err;

    return 0;
}

static int scale_meson_config_input(AVFilterLink *inlink)
{
    AVFilterContext *avctx = inlink->dst;
    ScaleMesonContext *ctx = avctx->priv;

    if (!inlink->hw_frames_ctx) {
        av_log(avctx, AV_LOG_ERROR, "A hardware frames reference is "
               "required to associate the processing device.\n");
        return AVERROR(EINVAL);
    }
    ctx->input_frames_ref = av_buffer_ref(inlink->hw_frames_ctx);
    ctx->input_frames = (AVHWFramesContext*)ctx->input_frames_ref->data;
    return 0;
}

static int scale_meson_config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink = outlink->src->inputs[0];
    AVFilterContext *avctx = outlink->src;
    ScaleMesonContext *ctx = avctx->priv;
//    AVMESONHWConfig *hwconfig = NULL;
    AVHWFramesConstraints *constraints = NULL;
//    AVMESONFrameContext *va_frames;
    int err, i;

    ctx->device_ref = av_buffer_ref(ctx->input_frames->device_ref);
    ctx->hwctx = ((AVHWDeviceContext*)ctx->device_ref->data)->hwctx;

//    hwconfig = av_hwdevice_hwconfig_alloc(ctx->device_ref);
//    if (!hwconfig) {
//        err = AVERROR(ENOMEM);
//        goto fail;
//    }
    constraints = av_hwdevice_get_hwframe_constraints(ctx->device_ref,
                                                      NULL/*hwconfig*/);
    if (!constraints) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    if (ctx->output_format == AV_PIX_FMT_NONE)
        ctx->output_format = ctx->input_frames->sw_format;
    if (constraints->valid_sw_formats) {
        for (i = 0; constraints->valid_sw_formats[i] != AV_PIX_FMT_NONE; i++) {
            if (ctx->output_format == constraints->valid_sw_formats[i])
                break;
        }
        if (constraints->valid_sw_formats[i] == AV_PIX_FMT_NONE) {
            av_log(ctx, AV_LOG_ERROR, "Hardware does not support output "
                   "format %s.\n", av_get_pix_fmt_name(ctx->output_format));
            err = AVERROR(EINVAL);
            goto fail;
        }
    }
    if ((err = ff_scale_eval_dimensions(ctx,
                                        ctx->w_expr, ctx->h_expr,
                                        inlink, outlink,
                                        &ctx->output_width, &ctx->output_height)) < 0)
        goto fail;
    if (ctx->output_width  < constraints->min_width  ||
        ctx->output_height < constraints->min_height ||
        ctx->output_width  > constraints->max_width  ||
        ctx->output_height > constraints->max_height) {
        av_log(ctx, AV_LOG_ERROR, "Hardware does not support scaling to "
               "size %dx%d (constraints: width %d-%d height %d-%d).\n",
               ctx->output_width, ctx->output_height,
               constraints->min_width,  constraints->max_width,
               constraints->min_height, constraints->max_height);
        err = AVERROR(EINVAL);
        goto fail;
    }
    ctx->output_frames_ref = av_hwframe_ctx_alloc(ctx->device_ref);
    if (!ctx->output_frames_ref) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create HW frame context "
               "for output.\n");
        err = AVERROR(ENOMEM);
        goto fail;
    }
    ctx->output_frames = (AVHWFramesContext*)ctx->output_frames_ref->data;

    ctx->output_frames->format    = AV_PIX_FMT_MESON;
    ctx->output_frames->sw_format = ctx->output_format;
    ctx->output_frames->width     = ctx->output_width;
    ctx->output_frames->height    = ctx->output_height;

    ctx->output_frames->initial_pool_size = 0;

    err = av_hwframe_ctx_init(ctx->output_frames_ref);
    if (err < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to initialise MESON frame "
               "context for output: %d\n", err);
        goto fail;
    }
//    va_frames = ctx->output_frames->hwctx;

    outlink->w = ctx->output_width;
    outlink->h = ctx->output_height;

    outlink->hw_frames_ctx = av_buffer_ref(ctx->output_frames_ref);
    if (!outlink->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
//    av_freep(&hwconfig);
    av_hwframe_constraints_free(&constraints);
    return 0;

fail:
    av_buffer_unref(&ctx->output_frames_ref);
//    av_freep(&hwconfig);
    av_hwframe_constraints_free(&constraints);
    return err;
}

static int scale_meson_filter_frame(AVFilterLink *inlink, AVFrame *input_frame)
{
    AVFilterContext *avctx = inlink->dst;
    AVFilterLink *outlink = avctx->outputs[0];
    ScaleMesonContext *ctx = avctx->priv;
    AVFrame *output_frame = NULL;
    IONMEM_AllocParams  *ionmem_in, *ionmem_out;
    aml_ge2d_info_t pge2d;
    int err;
//    static int color = 0;
    av_log(ctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input_frame->format),
           input_frame->width, input_frame->height, input_frame->pts);

    ionmem_in = (IONMEM_AllocParams *)input_frame->data[3];

    output_frame = ff_get_video_buffer(outlink, ctx->output_width,
                                       ctx->output_height);
    if (!output_frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ionmem_out = (IONMEM_AllocParams *)output_frame->data[3];

    memset(&pge2d, 0, sizeof(pge2d));

    pge2d.src_info[0].memtype = GE2D_CANVAS_ALLOC;
    pge2d.src_info[0].shared_fd = ionmem_in->mImageFd;
    pge2d.src_info[0].canvas_w = input_frame->width;
    pge2d.src_info[0].canvas_h = input_frame->height;
    pge2d.src_info[0].format = 17; //get_ge2d_pix_fmt(ctx, input_frame->format);
    pge2d.src_info[0].rect.x = 0;
    pge2d.src_info[0].rect.y = 0;
    pge2d.src_info[0].rect.w = input_frame->width;
    pge2d.src_info[0].rect.h = input_frame->height;

    pge2d.dst_info.memtype = GE2D_CANVAS_ALLOC;
    pge2d.dst_info.shared_fd = ionmem_out->mImageFd;
    pge2d.dst_info.canvas_w = output_frame->width;
    pge2d.dst_info.canvas_h = output_frame->height;
    pge2d.dst_info.format = 17; //get_ge2d_pix_fmt(ctx, output_frame->format);
    pge2d.dst_info.rect.x = 0;
    pge2d.dst_info.rect.y = 0;
    pge2d.dst_info.rect.w = output_frame->width;
    pge2d.dst_info.rect.h = output_frame->height;
    pge2d.dst_info.rotation = GE2D_ROTATION_0;

    pge2d.offset = 0;
    pge2d.ge2d_op = AML_GE2D_STRETCHBLIT;
    //pge2d.blend_mode = BLEND_MODE_PREMULTIPLIED;


    err = aml_ge2d_process(&pge2d);
    if (err != ge2d_success) {
        av_log(avctx, AV_LOG_ERROR, "%s error %d\n",
                        __FUNCTION__, __LINE__);
        goto fail;
    }

    av_frame_copy_props(output_frame, input_frame);
    av_frame_free(&input_frame);

    av_log(ctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(output_frame->format),
           output_frame->width, output_frame->height, output_frame->pts);

    return ff_filter_frame(outlink, output_frame);


fail:
    av_frame_free(&input_frame);
    av_frame_free(&output_frame);
    return err;
}

static av_cold int scale_meson_init(AVFilterContext *avctx)
{
    ScaleMesonContext *ctx = avctx->priv;
    int err;

    err = aml_ge2d_init();
    if (err != ge2d_success) {
        av_log(avctx, AV_LOG_ERROR, "%s error %d\n",
                __FUNCTION__, __LINE__);
        return AVERROR(EBUSY);
    }
    if (ctx->output_format_string) {
        ctx->output_format = av_get_pix_fmt(ctx->output_format_string);
        if (ctx->output_format == AV_PIX_FMT_NONE) {
            av_log(ctx, AV_LOG_ERROR, "Invalid output format.\n");
            return AVERROR(EINVAL);
        }
    } else {
        // Use the input format once that is configured.
        ctx->output_format = AV_PIX_FMT_NONE;
    }

    
    return 0;
}

static av_cold void scale_meson_uninit(AVFilterContext *avctx)
{
    ScaleMesonContext *ctx = avctx->priv;

    aml_ge2d_exit();

    av_buffer_unref(&ctx->input_frames_ref);
    av_buffer_unref(&ctx->output_frames_ref);
    av_buffer_unref(&ctx->device_ref);
}


#define OFFSET(x) offsetof(ScaleMesonContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption scale_meson_options[] = {
    { "w", "Output video width",
      OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, .flags = FLAGS },
    { "h", "Output video height",
      OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, .flags = FLAGS },
    { "format", "Output video format (software format of hardware frames)",
      OFFSET(output_format_string), AV_OPT_TYPE_STRING, .flags = FLAGS },
    { NULL },
};

static const AVClass scale_meson_class = {
    .class_name = "scale_meson",
    .item_name  = av_default_item_name,
    .option     = scale_meson_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad scale_meson_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &scale_meson_filter_frame,
        .config_props = &scale_meson_config_input,
    },
    { NULL }
};

static const AVFilterPad scale_meson_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = &scale_meson_config_output,
    },
    { NULL }
};

AVFilter ff_vf_scale_meson = {
    .name          = "scale_meson",
    .description   = NULL_IF_CONFIG_SMALL("Scale to/from MESON surfaces."),
    .priv_size     = sizeof(ScaleMesonContext),
    .init          = &scale_meson_init,
    .uninit        = &scale_meson_uninit,
    .query_formats = &scale_meson_query_formats,
    .inputs        = scale_meson_inputs,
    .outputs       = scale_meson_outputs,
    .priv_class    = &scale_meson_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
