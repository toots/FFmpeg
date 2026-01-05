/*
 * Copyright (c) 2007 Bobby Bingham
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
 * video vertical flip filter
 */

#ifndef AVFILTER_VFLIP_H
#define AVFILTER_VFLIP_H

typedef struct VFlipContext {
    int vsub;   ///< vertical chroma subsampling
    int bayer;
} VFlipContext;

int ff_vflip_config_input(AVFilterLink *link);
AVFrame *ff_vflip_get_video_buffer(AVFilterLink *link, int w, int h);
void ff_vflip_frame_inplace(AVFrame *frame, int vsub, int height);
int ff_vflip_filter_frame(AVFilterLink *link, AVFrame *frame);

#endif
