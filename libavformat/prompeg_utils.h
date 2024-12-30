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
 * Pro-MPEG Code of Practice #3 Release 2 FEC protocol -- Utils file
 * @author Romain Beauxis <romain.beauxis@gmail.com>
 */

#ifndef AVFORMAT_PROMPEG_UTILS_H
#define AVFORMAT_PROMPEG_UTILS_H

#include <stdint.h>

typedef struct PrompegFec {
    uint16_t sn;
    uint32_t ts;
    uint8_t *bitstring;
} PrompegFec;

#define PROMPEG_RTP_PT 0x60
#define PROMPEG_FEC_COL 0x0
#define PROMPEG_FEC_ROW 0x1

void ff_prompeg_xor_fast(
    const uint8_t *in1, const uint8_t *in2, uint8_t *out, int size);

void ff_prompeg_pack_bitstring(
    uint8_t *bitstring, const uint8_t *packet, int packet_size);

void ff_prompeg_pack_fec_bitstring(
    uint8_t *bitstring, const uint8_t *fec_packet, int fec_packet_size);

void ff_prompeg_pack_fec_packet(uint8_t *fec_packet, PrompegFec *fec,
    uint16_t sn, uint8_t type, int l, int d, int bitstring_size);

void ff_prompeg_restore_packet(uint8_t *packet, const uint8_t *bitstring,
    uint8_t m, uint32_t ssrc, uint16_t index, int bitstring_size);

#endif
