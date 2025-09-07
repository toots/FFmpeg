/*
 * Pro-MPEG Code of Practice #3 Release 2 FEC
 * Copyright (c) 2025 Radio France (https://radiofrance.fr)
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
 * Pro-MPEG Code of Practice #3 Release 2 FEC protocol -- Decoder logic
 * @author Romain Beauxis <romain.beauxis@gmail.com>
 */

#ifndef AVFORMAT_PROMPEGDEC_H
#define AVFORMAT_PROMPEGDEC_H

#include "libavformat/url.h"
#include "libavutil/tree.h"

typedef struct PrompegDecoderPacket {
    uint16_t index;
    uint8_t *bytes;
} PrompegDecoderPacket;

typedef struct PrompegDecoder {
    const AVClass *av_class;
    int l, d, packet_size, fec_packet_size, bitstring_size;
    URLContext *url_context;
    PrompegDecoderPacket **restore_buffer;
    uint8_t *tmp_bitstring, *bitstring;
    uint64_t restored_packets;
    uint64_t failed_packets;

    uint8_t pending_packets;

    struct AVTreeNode *packets;
    uint16_t next_index;
    PrompegDecoderPacket *next_fec_col;
    PrompegDecoderPacket *next_fec_row;
    uint16_t first_fec_packet_index;
    int packets_count;
    int min_packets;
    int max_packets;
    int max_fec_packets;

    struct AVTreeNode *fec_col_packets;
    int fec_row_packets_count;

    struct AVTreeNode *fec_row_packets;
    int fec_col_packets_count;
} PrompegDecoder;

typedef enum PrompegDecoderPacketType {
    PROMPEGDEC_PACKET,
    PROMPEGDEC_FEC_ROW_PACKET,
    PROMPEGDEC_FEC_COL_PACKET,
} PrompegDecoderPacketType;

PrompegDecoder *ff_prompegdec_alloc_decoder(URLContext *url_context, int l,
    int d, int packet_size, int fec_packet_size, int bitstring_size,
    int min_packets, int max_packets, int max_fec_packets);

void ff_prompegdec_free_decoder(PrompegDecoder *decoder);

int ff_prompegdec_add_packet(PrompegDecoder *decoder,
    PrompegDecoderPacketType type, uint16_t index, const uint8_t *bytes,
    int length);

int ff_prompegdec_read_packet(
    PrompegDecoder *decoder, uint8_t *bytes, int length);

#endif
