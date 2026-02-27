/*
 * Copyright (c) 2025 Romain Beauxis
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

#include "avfilter.h"
#include "filters.h"
#include "hflip.h"
#include "rotate.h"
#include "transpose.h"
#include "vflip.h"
#include "video.h"

#include "libavutil/avstring.h"
#include "libavutil/display.h"
#include "libavutil/internal.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"

enum FilterType { NONE = 0, TRANSPOSE, VFLIP, HFLIP, DOUBLE_FLIP, ROTATE };

typedef struct DisplaymatrixAutorotateContext {
    const AVClass *class;

    char *displaymatrix_str;
    int dynamic;
    int32_t displaymatrix[9];
    int has_displaymatrix;
    int warned_inconsistent;
    int dimensions_configured;

    enum FilterType filter_type;
    TransContext transpose_context;
    VFlipContext vflip_context;
    HFlipContext hflip_context;
    RotContext rotate_context;
} DisplaymatrixAutorotateContext;

static double get_rotation(AVFilterContext *ctx, const int32_t *displaymatrix)
{
    double theta = 0;
    if (displaymatrix)
        theta = -round(av_display_rotation_get(displaymatrix));

    theta -= 360 * floor(theta / 360 + 0.9 / 360);

    if (fabs(theta - 90 * round(theta / 90)) > 2)
        av_log(ctx, AV_LOG_WARNING,
               "Odd rotation angle.\nIf you want to help, upload a sample of "
               "this file to https://streams.videolan.org/upload/ and contact "
               "the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)");

    return theta;
}

static int transformation_changes_dimensions(enum FilterType filter_type, int width, int height)
{
    switch (filter_type) {
    case TRANSPOSE:
    case DOUBLE_FLIP:
        return width != height;
    case ROTATE:
        return 1;
    case VFLIP:
    case HFLIP:
    case NONE:
        return 0;
    }
    return 0;
}

static int parse_displaymatrix(DisplaymatrixAutorotateContext *s, const char *str)
{
    int ret;

    if (!str || !str[0])
        return 0;

    ret = sscanf(str, "%d|%d|%d|%d|%d|%d|%d|%d|%d",
                 &s->displaymatrix[0], &s->displaymatrix[1], &s->displaymatrix[2],
                 &s->displaymatrix[3], &s->displaymatrix[4], &s->displaymatrix[5],
                 &s->displaymatrix[6], &s->displaymatrix[7], &s->displaymatrix[8]);

    if (ret != 9) {
        av_log(s, AV_LOG_ERROR, "Invalid displaymatrix format. Expected a '|'-separated list of 9 integers.\n");
        return AVERROR(EINVAL);
    }

    s->has_displaymatrix = 1;
    return 0;
}

static int setup_rotation(AVFilterContext *ctx)
{
    DisplaymatrixAutorotateContext *s = ctx->priv;
    const int32_t *displaymatrix = s->has_displaymatrix ? s->displaymatrix : NULL;
    double theta;
    const char *filter_name = NULL;

    if (!displaymatrix) {
        s->filter_type = NONE;
        av_log(ctx, AV_LOG_DEBUG, "No displaymatrix detected, no rotation will be applied\n");
        return 0;
    }

    theta = get_rotation(ctx, displaymatrix);

    if (fabs(theta - 90) < 1.0) {
        s->filter_type = TRANSPOSE;
        s->transpose_context.dir =
            displaymatrix[3] > 0 ? TRANSPOSE_CCLOCK_FLIP : TRANSPOSE_CLOCK;
        filter_name = displaymatrix[3] > 0 ? "transpose=cclock_flip" : "transpose=clock";
    } else if (fabs(theta - 180) < 1.0) {
        if (displaymatrix[0] < 0 && displaymatrix[4] < 0) {
            s->filter_type = DOUBLE_FLIP;
            filter_name = "hflip+vflip";
        } else if (displaymatrix[0] < 0) {
            s->filter_type = HFLIP;
            filter_name = "hflip";
        } else if (displaymatrix[4] < 0) {
            s->filter_type = VFLIP;
            filter_name = "vflip";
        } else {
            s->filter_type = NONE;
            filter_name = "none";
        }
    } else if (fabs(theta - 270) < 1.0) {
        s->filter_type = TRANSPOSE;
        s->transpose_context.dir =
            displaymatrix[3] < 0 ? TRANSPOSE_CLOCK_FLIP : TRANSPOSE_CCLOCK;
        filter_name = displaymatrix[3] < 0 ? "transpose=clock_flip" : "transpose=cclock";
    } else if (fabs(theta) > 1.0) {
        char rotate_buf[64];

        snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
        s->filter_type = ROTATE;
        s->rotate_context.angle_expr_str = av_strdup(rotate_buf);
        if (!s->rotate_context.angle_expr_str)
            return AVERROR(ENOMEM);

        s->rotate_context.use_bilinear = 1;
        filter_name = rotate_buf;
    } else {
        /* theta is close to 0 */
        if (displaymatrix && displaymatrix[4] < 0) {
            s->filter_type = VFLIP;
            filter_name = "vflip";
        } else {
            s->filter_type = NONE;
            filter_name = "none";
        }
    }

    av_log(ctx, AV_LOG_DEBUG, "Detected displaymatrix rotation: %.1f degrees -> applying %s\n",
           theta, filter_name);

    return 0;
}

static int config_filters(AVFilterContext *ctx, AVFilterLink *inlink)
{
    DisplaymatrixAutorotateContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int ret = 0;

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    /* Temporarily swap ctx->priv to delegate filter context */
    switch (s->filter_type) {
    case NONE:
        break;
    case DOUBLE_FLIP:
    case HFLIP:
        ctx->priv = &s->hflip_context;
        ret = ff_hflip_config_input(inlink);
        ctx->priv = s;
        if (ret < 0)
            break;
        if (s->filter_type != DOUBLE_FLIP)
            break;
    case VFLIP:
        ctx->priv = &s->vflip_context;
        ret = ff_vflip_config_input(inlink);
        ctx->priv = s;
        break;
    case TRANSPOSE:
        ctx->priv = &s->transpose_context;
        ret = ff_transpose_config_output(outlink);
        ctx->priv = s;
        break;
    case ROTATE:
        ctx->priv = &s->rotate_context;
        if (!strcmp(s->rotate_context.fillcolor_str, "none"))
            s->rotate_context.fillcolor_enable = 0;
        else if (av_parse_color(s->rotate_context.fillcolor, s->rotate_context.fillcolor_str, -1, ctx) >= 0)
            s->rotate_context.fillcolor_enable = 1;
        else {
            ctx->priv = s;
            return AVERROR(EINVAL);
        }
        ret = ff_rotate_config_output(outlink);
        ctx->priv = s;
        break;
    default:
        ret = AVERROR_INVALIDDATA;
    }

    s->dimensions_configured = 1;
    return ret;
}

static int detect_displaymatrix_from_side_data(AVFilterContext *ctx, AVFilterLink *inlink,
                                               AVFrameSideData **side_data, int nb_side_data,
                                               const char *source)
{
    DisplaymatrixAutorotateContext *s = ctx->priv;
    int ret;

    if (s->has_displaymatrix)
        return 0;

    for (int i = 0; i < nb_side_data; i++) {
        if (side_data[i]->type == AV_FRAME_DATA_DISPLAYMATRIX &&
            side_data[i]->size == 9 * sizeof(int32_t)) {
            av_log(ctx, AV_LOG_DEBUG, "Displaymatrix detected from %s\n", source);
            memcpy(s->displaymatrix, side_data[i]->data, sizeof(s->displaymatrix));
            s->has_displaymatrix = 1;

            ret = setup_rotation(ctx);
            if (ret < 0)
                return ret;

            if (s->dimensions_configured && transformation_changes_dimensions(s->filter_type, inlink->w, inlink->h)) {
                av_log(ctx, AV_LOG_WARNING,
                       "Displaymatrix detected from %s after dimensions already configured. "
                       "Dynamic dimension changes are not supported.\n", source);
                return 0;
            }

            return config_filters(ctx, inlink);
        }
    }

    return 0;
}

static AVFrame *get_video_buffer(AVFilterLink *link, int w, int h)
{
    DisplaymatrixAutorotateContext *s = link->dst->priv;

    if (s->filter_type == TRANSPOSE)
        return ff_transpose_get_video_buffer(link, w, h);

    return ff_default_get_video_buffer(link, w, h);
}

static av_cold int init(AVFilterContext *ctx)
{
    DisplaymatrixAutorotateContext *s = ctx->priv;
    int ret;

    ret = parse_displaymatrix(s, s->displaymatrix_str);
    if (ret < 0)
        return ret;

    return setup_rotation(ctx);
}

static void cleanup_filter_context(DisplaymatrixAutorotateContext *s, enum FilterType filter_type)
{
    if (filter_type == ROTATE) {
        av_expr_free(s->rotate_context.angle_expr);
        s->rotate_context.angle_expr = NULL;
        av_freep(&s->rotate_context.angle_expr_str);
    }
}

static int handle_displaymatrix_change(AVFilterContext *ctx, AVFilterLink *inlink,
                                       const int32_t *new_displaymatrix)
{
    DisplaymatrixAutorotateContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int32_t old_displaymatrix[9];
    enum FilterType old_filter_type;
    int old_w, old_h;
    int ret;

    memcpy(old_displaymatrix, s->displaymatrix, sizeof(old_displaymatrix));
    old_filter_type = s->filter_type;
    old_w = outlink->w;
    old_h = outlink->h;

    cleanup_filter_context(s, old_filter_type);

    memcpy(s->displaymatrix, new_displaymatrix, sizeof(s->displaymatrix));

    ret = setup_rotation(ctx);
    if (ret < 0)
        goto restore;

    ret = config_filters(ctx, inlink);
    if (ret < 0)
        goto restore;

    if (outlink->w != old_w || outlink->h != old_h) {
        if (!s->warned_inconsistent) {
            av_log(ctx, AV_LOG_WARNING,
                   "Frame displaymatrix change would alter dimensions (%dx%d -> %dx%d). "
                   "Keeping current transformation as dynamic dimension changes are not supported.\n",
                   old_w, old_h, outlink->w, outlink->h);
            s->warned_inconsistent = 1;
        }
        ret = 0;
        goto restore;
    }

    av_log(ctx, AV_LOG_DEBUG,
           "Frame displaymatrix changed without affecting dimensions, reconfiguring\n");

    return 0;

restore:
    cleanup_filter_context(s, s->filter_type);
    memcpy(s->displaymatrix, old_displaymatrix, sizeof(s->displaymatrix));
    s->filter_type = old_filter_type;
    outlink->w = old_w;
    outlink->h = old_h;
    return ret;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    DisplaymatrixAutorotateContext *s = ctx->priv;
    cleanup_filter_context(s, s->filter_type);
}

static int filter_double_flip(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    DisplaymatrixAutorotateContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    /* copy palette if required */
    if (av_pix_fmt_desc_get(inlink->format)->flags & AV_PIX_FMT_FLAG_PAL)
        memcpy(out->data[1], in->data[1], AVPALETTE_SIZE);

    ctx->priv = &s->hflip_context;
    ff_hflip_frame(ctx, out, in);
    ctx->priv = s;

    ff_vflip_frame_inplace(out, s->vflip_context.vsub, outlink->h);

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static int config_props(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    DisplaymatrixAutorotateContext *s = ctx->priv;
    int ret;

    av_log(ctx, AV_LOG_DEBUG, "Configuring displaymatrix_autorotate filter\n");

    if (!s->has_displaymatrix) {
        ret = detect_displaymatrix_from_side_data(ctx, inlink,
                                                  inlink->side_data,
                                                  inlink->nb_side_data,
                                                  "link side data");
        if (ret < 0)
            return ret;
    }

    if (s->has_displaymatrix)
        return config_filters(ctx, inlink);

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    DisplaymatrixAutorotateContext *s = ctx->priv;
    AVFrameSideData *sidedata;
    int err;

    /* If no displaymatrix yet, try to detect from frame side data */
    if (!s->has_displaymatrix) {
        err = detect_displaymatrix_from_side_data(ctx, inlink,
                                                  in->side_data,
                                                  in->nb_side_data,
                                                  "frame side data");
        if (err < 0)
            return err;
    } else {
        sidedata = av_frame_get_side_data(in, AV_FRAME_DATA_DISPLAYMATRIX);
        if (sidedata && sidedata->size == 9 * sizeof(int32_t)) {
            if (memcmp(s->displaymatrix, sidedata->data, sizeof(s->displaymatrix)) != 0) {
                if (s->dynamic) {
                    err = handle_displaymatrix_change(ctx, inlink, (const int32_t *)sidedata->data);
                    if (err < 0)
                        return err;
                } else if (!s->warned_inconsistent) {
                    av_log(ctx, AV_LOG_WARNING,
                           "Frame displaymatrix differs from initialization displaymatrix. "
                           "Using initialization value (dynamic reconfiguration disabled).\n");
                    s->warned_inconsistent = 1;
                }
            }
        }
    }

    /* Temporarily swap ctx->priv to delegate filter context
     * so that delegated filter functions receive their expected context */
    switch (s->filter_type) {
    case NONE:
        err = ff_filter_frame(ctx->outputs[0], in);
        break;
    case TRANSPOSE:
        ctx->priv = &s->transpose_context;
        err = ff_transpose_filter_frame(inlink, in);
        ctx->priv = s;
        break;
    case HFLIP:
        ctx->priv = &s->hflip_context;
        err = ff_hflip_filter_frame(inlink, in);
        ctx->priv = s;
        break;
    case VFLIP:
        ctx->priv = &s->vflip_context;
        err = ff_vflip_filter_frame(inlink, in);
        ctx->priv = s;
        break;
    case DOUBLE_FLIP:
        err = filter_double_flip(inlink, in);
        break;
    case ROTATE:
        ctx->priv = &s->rotate_context;
        err = ff_rotate_filter_frame(inlink, in);
        ctx->priv = s;
        break;
    default:
        err = AVERROR_INVALIDDATA;
    }

    return err;
}

#define OFFSET(x) offsetof(DisplaymatrixAutorotateContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption displaymatrix_autorotate_options[] = {
    { "matrix", "A '|'-separated list of 9 integers for the displaymatrix", OFFSET(displaymatrix_str), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    { "fillcolor", "set background fill color for rotation", OFFSET(rotate_context.fillcolor_str), AV_OPT_TYPE_STRING, {.str="black"}, 0, 0, FLAGS },
    { "c", "set background fill color for rotation", OFFSET(rotate_context.fillcolor_str), AV_OPT_TYPE_STRING, {.str="black"}, 0, 0, FLAGS },
    { "out_w", "set output width expression for rotation", OFFSET(rotate_context.outw_expr_str), AV_OPT_TYPE_STRING, {.str="rotw(a)"}, 0, 0, FLAGS },
    { "ow", "set output width expression for rotation", OFFSET(rotate_context.outw_expr_str), AV_OPT_TYPE_STRING, {.str="rotw(a)"}, 0, 0, FLAGS },
    { "out_h", "set output height expression for rotation", OFFSET(rotate_context.outh_expr_str), AV_OPT_TYPE_STRING, {.str="roth(a)"}, 0, 0, FLAGS },
    { "oh", "set output height expression for rotation", OFFSET(rotate_context.outh_expr_str), AV_OPT_TYPE_STRING, {.str="roth(a)"}, 0, 0, FLAGS },
    { "dynamic", "allow dynamic displaymatrix reconfiguration (limited to dimension-preserving changes)", OFFSET(dynamic), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(displaymatrix_autorotate);

static const AVFilterPad avfilter_vf_displaymatrix_autorotate_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .get_buffer.video = get_video_buffer,
        .filter_frame = filter_frame,
        .config_props = config_props,
    },
};

static const AVFilterPad avfilter_vf_displaymatrix_autorotate_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

const FFFilter ff_vf_displaymatrix_autorotate = {
    .p.name = "displaymatrix_autorotate",
    .p.description =
        NULL_IF_CONFIG_SMALL("Automatically rotate frames according to the "
                             "displaymatrix side data."),
    .p.flags =
        AVFILTER_FLAG_SLICE_THREADS | AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .p.priv_class = &displaymatrix_autorotate_class,
    .priv_size = sizeof(DisplaymatrixAutorotateContext),
    .init = init,
    .uninit = uninit,
    FILTER_INPUTS(avfilter_vf_displaymatrix_autorotate_inputs),
    FILTER_OUTPUTS(avfilter_vf_displaymatrix_autorotate_outputs),
    FILTER_PIXFMTS_ARRAY(ff_rotate_pix_fmts),
};
