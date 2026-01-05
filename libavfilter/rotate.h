/*
 * Copyright (c) 2013 Stefano Sabatini
 * Copyright (c) 2008 Vitor Sessak
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

/**
 * @file
 * rotation filter, partially based on the tests/rotozoom.c program
*/

#ifndef AVFILTER_ROTATE_H
#define AVFILTER_ROTATE_H

#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/opt.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "drawutils.h"
#include "filters.h"
#include "video.h"

#include <float.h>

enum var_name {
    VAR_IN_W , VAR_IW,
    VAR_IN_H , VAR_IH,
    VAR_OUT_W, VAR_OW,
    VAR_OUT_H, VAR_OH,
    VAR_HSUB, VAR_VSUB,
    VAR_N,
    VAR_T,
    VAR_A,
    VAR_VARS_NB
};

typedef struct RotContext {
    const AVClass *class;
    double angle;
    char *angle_expr_str;   ///< expression for the angle
    AVExpr *angle_expr;     ///< parsed expression for the angle
    char *outw_expr_str, *outh_expr_str;
    int outh, outw;
    uint8_t fillcolor[4];   ///< color expressed either in YUVA or RGBA colorspace for the padding area
    char *fillcolor_str;
    int fillcolor_enable;
    int hsub, vsub;
    int nb_planes;
    int use_bilinear;
    float sinx, cosx;
    double var_values[VAR_VARS_NB];
    FFDrawContext draw;
    FFDrawColor color;
    uint8_t *(*interpolate_bilinear)(uint8_t *dst_color,
                                    const uint8_t *src, int src_linesize, int src_linestep,
                                    int x, int y, int max_x, int max_y);
} RotContext;

int ff_rotate_config_output(AVFilterLink *outlink);
int ff_rotate_filter_frame(AVFilterLink *inlink, AVFrame *in);

static const enum AVPixelFormat ff_rotate_pix_fmts[] = {
    AV_PIX_FMT_GBRP,   AV_PIX_FMT_GBRAP,
    AV_PIX_FMT_ARGB,   AV_PIX_FMT_RGBA,
    AV_PIX_FMT_ABGR,   AV_PIX_FMT_BGRA,
    AV_PIX_FMT_0RGB,   AV_PIX_FMT_RGB0,
    AV_PIX_FMT_0BGR,   AV_PIX_FMT_BGR0,
    AV_PIX_FMT_RGB24,  AV_PIX_FMT_BGR24,
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_YUV410P,
    AV_PIX_FMT_YUV444P,  AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_YUV420P,  AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUVA444P, AV_PIX_FMT_YUVA420P,
    AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUVA420P10LE,
    AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_YUVA444P10LE,
    AV_PIX_FMT_YUV420P12LE,
    AV_PIX_FMT_YUV444P12LE,
    AV_PIX_FMT_YUV444P16LE, AV_PIX_FMT_YUVA444P16LE,
    AV_PIX_FMT_YUV420P16LE, AV_PIX_FMT_YUVA420P16LE,
    AV_PIX_FMT_YUV444P9LE, AV_PIX_FMT_YUVA444P9LE,
    AV_PIX_FMT_YUV420P9LE, AV_PIX_FMT_YUVA420P9LE,
    AV_PIX_FMT_NONE
};

#endif
