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

#ifndef AVUTIL_HWCONTEXT_MESON_H
#define AVUTIL_HWCONTEXT_MESON_H

#include <ge2d_port.h>
#include <ge2d_com.h>
#include <aml_ge2d.h>
#include <IONmem.h>

#define MESON_TRACE(ctx) \
    av_log(ctx, AV_LOG_ERROR, "HERE %s:%d\n", __FUNCTION__, __LINE__);

#define MESON_DUMPVAR(ctx, x) \
    av_log(ctx, AV_LOG_ERROR, #x "=%d\n", x);

#define MESON_DUMPVARX(ctx, x) \
    av_log(ctx, AV_LOG_ERROR, #x "=%x\n", x);

typedef struct {
    pixel_format_t ge2d_fmt;
    enum AVPixelFormat pix_fmt;
} MESONFormat;


typedef struct AVMESONDeviceContext {
    MESONFormat *formats;
    int nb_formats;
} AVMESONDeviceContext;


//typedef struct AVMESONFrameContext {
//} AVMESONFrameContext;


//typedef struct AVMESONHWConfig {
//    int fake;
//} AVMESONHWConfig;

typedef struct MESONFramesContext {

} MESONFramesContext;


#endif /* AVUTIL_HWCONTEXT_MESON_H */
