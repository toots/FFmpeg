/*
 * Pro-MPEG Code of Practice #3 Release 2 FEC
 * Copyright (c) 2016 Mobibase, France (http://www.mobibase.com)
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

/*
 * Reminder:

 [RFC 2733] FEC Packet Structure

   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                         RTP Header                            |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                         FEC Header                            |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                         FEC Payload                           |
   |                                                               |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


 [RFC 3550] RTP header

    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |V=2|P|X|  CC   |M|     PT      |       sequence number         |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                           timestamp                           |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |           synchronization source (SSRC) identifier            |
   +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
   |            contributing source (CSRC) identifiers             |
   |                             ....                              |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

 [RFC 3550] RTP header extension (after CSRC)

    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |      defined by profile       |           length              |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                        header extension                       |
   |                             ....                              |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

 [Pro-MPEG COP3] FEC Header

   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |      SNBase low bits          |        length recovery        |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |E| PT recovery |                 mask                          |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                          TS recovery                          |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |X|D|type |index|    offset     |      NA       |SNBase ext bits|
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

 */

#include "prompeg_utils.h"
#include "config.h"
#include "libavutil/intreadwrite.h"

#include <string.h>

void ff_prompeg_xor_fast(
    const uint8_t *in1, const uint8_t *in2, uint8_t *out, int size)
{
    int i, n, s;

#if HAVE_FAST_64BIT
    uint64_t v1, v2;

    n = size / sizeof(uint64_t);
    s = n * sizeof(uint64_t);

    for (i = 0; i < n; i++) {
        v1 = AV_RN64A(in1);
        v2 = AV_RN64A(in2);
        AV_WN64A(out, v1 ^ v2);
        in1 += 8;
        in2 += 8;
        out += 8;
    }
#else
    uint32_t v1, v2;

    n = size / sizeof(uint32_t);
    s = n * sizeof(uint32_t);

    for (i = 0; i < n; i++) {
        v1 = AV_RN32A(in1);
        v2 = AV_RN32A(in2);
        AV_WN32A(out, v1 ^ v2);
        in1 += 4;
        in2 += 4;
        out += 4;
    }
#endif

    n = size - s;

    for (i = 0; i < n; i++) {
        out[i] = in1[i] ^ in2[i];
    }
}

void ff_prompeg_pack_bitstring(uint8_t *b, const uint8_t *buf, int size)
{
    // P, X, CC
    b[0] = buf[0] & 0x3f;
    // M, PT
    b[1] = buf[1];
    // Timestamp
    b[2] = buf[4];
    b[3] = buf[5];
    b[4] = buf[6];
    b[5] = buf[7];
    AV_WB16(b + 6, size - 12);
    // Payload
    memcpy(b + 8, buf + 12, size - 12);
}

void ff_prompeg_pack_fec_packet(uint8_t *buf, PrompegFec *fec, uint16_t sn,
    uint8_t type, int l, int d, int size)
{
    uint8_t *b = fec->bitstring;

    // V, P, X, CC
    buf[0] = 0x80 | (b[0] & 0x3f);
    // M, PT
    buf[1] = (b[1] & 0x80) | PROMPEG_RTP_PT;
    // SN
    AV_WB16(buf + 2, sn);
    // TS
    AV_WB32(buf + 4, fec->ts);
    // CSRC=0
    // AV_WB32(buf + 8, 0);
    // SNBase low bits
    AV_WB16(buf + 12, fec->sn);
    // Length recovery
    buf[14] = b[6];
    buf[15] = b[7];
    // E=1, PT recovery
    buf[16] = 0x80 | b[1];
    // Mask=0
    // buf[17] = 0x0;
    // buf[18] = 0x0;
    // buf[19] = 0x0;
    // TS recovery
    buf[20] = b[2];
    buf[21] = b[3];
    buf[22] = b[4];
    buf[23] = b[5];
    // X=0, D, type=0, index=0
    buf[24] = type == PROMPEG_FEC_COL ? 0x0 : 0x40;
    // offset
    buf[25] = type == PROMPEG_FEC_COL ? l : 0x1;
    // NA
    buf[26] = type == PROMPEG_FEC_COL ? d : l;
    // SNBase ext bits=0
    // buf[27] = 0x0;
    // Payload
    memcpy(buf + 28, b + 8, size - 8);
}

void ff_prompeg_pack_fec_bitstring(uint8_t *b, const uint8_t *buf, int size)
{
    // P, X, CC
    b[0] = buf[0] & 0x3f;
    // M, PT
    b[1] = buf[16] & 0x3f;
    // Timestamp
    b[2] = buf[20];
    b[3] = buf[21];
    b[4] = buf[22];
    b[5] = buf[23];
    // Length recovery
    b[6] = buf[14];
    b[7] = buf[15];

    memcpy(b + 8, buf + 28, size - 28);
}

void ff_prompeg_restore_packet(uint8_t *packet, const uint8_t *buf, uint8_t m,
    uint32_t ssrc, uint16_t index, int size)
{
    // P, X, CC
    packet[0] = buf[0] | 0x80;
    // M, PT
    packet[1] = buf[1] | (m << 7);
    // Packet index
    AV_WB16(packet + 2, index);
    // Timestamp
    packet[4] = buf[2];
    packet[5] = buf[3];
    packet[6] = buf[4];
    packet[7] = buf[5];
    // SSRC
    memcpy(packet + 8, &ssrc, 4);
    // Payload
    memcpy(packet + 12, buf + 8, size - 8);
}
