/*
 * Copyright (c) 2007 Benoit Fouet
 * Copyright (c) 2010 Stefano Sabatini
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

#ifndef AVFILTER_HFLIP_H
#define AVFILTER_HFLIP_H

#include <stdint.h>

typedef struct HFlipContext {
    int max_step[4];    ///< max pixel step for each plane, expressed as a number of bytes
    int bayer_plus1;    ///< 1 .. not a Bayer input format, 2 .. Bayer input format
    int planewidth[4];  ///< width of each plane
    int planeheight[4]; ///< height of each plane

    void (*flip_line[4])(const uint8_t *src, uint8_t *dst, int w);
} HFlipContext;

void ff_hflip_init_x86(HFlipContext *s, int step[4], int nb_planes);

int ff_hflip_config_input(AVFilterLink *inlink);
void ff_hflip_frame(AVFilterContext *ctx, AVFrame *out, const AVFrame *in);
int ff_hflip_filter_frame(AVFilterLink *inlink, AVFrame *in);

#endif /* AVFILTER_HFLIP_H */
