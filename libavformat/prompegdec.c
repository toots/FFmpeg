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

#include "prompegdec.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "prompeg_utils.h"

#include <memory.h>

static const AVClass prompegdec_class = {
    .class_name = "Prompeg Decoder",
    .item_name = av_default_item_name,
    .version = LIBAVUTIL_VERSION_INT,
};

static float prompeg_restored_ratio(PrompegDecoder *decoder)
{
    double missed_packets = decoder->restored_packets + decoder->failed_packets;

    if (!decoder->restored_packets)
        return -1;

    return ((double)decoder->restored_packets) / missed_packets * 100;
}

static PrompegDecoderPacket *prompegdec_alloc_packet(uint16_t index, int length)
{
    PrompegDecoderPacket *packet = av_mallocz(sizeof(PrompegDecoderPacket));
    if (!packet)
        return NULL;

    packet->index = index;
    packet->bytes = av_malloc(length);
    if (!packet->bytes) {
        av_free(packet->bytes);
        av_free(packet);
        return NULL;
    }

    return packet;
}

PrompegDecoder *ff_prompegdec_alloc_decoder(URLContext *url_context, int l,
    int d, int packet_size, int fec_packet_size, int bitstring_size,
    int min_packets, int max_packets, int max_fec_packets)
{
    PrompegDecoder *decoder = av_mallocz(sizeof(PrompegDecoder));
    if (!decoder)
        return NULL;

    decoder->av_class = &prompegdec_class;
    decoder->l = l;
    decoder->d = d;
    decoder->packet_size = packet_size;
    decoder->fec_packet_size = fec_packet_size;
    decoder->bitstring_size = bitstring_size;
    decoder->url_context = url_context;
    decoder->min_packets = min_packets;
    decoder->max_packets = max_packets;
    decoder->max_fec_packets = max_fec_packets;

    decoder->restore_buffer =
        av_mallocz(sizeof(PrompegDecoderPacket *) * (l < d ? d : l));
    if (!decoder->restore_buffer)
        goto fail;

    decoder->tmp_bitstring = av_malloc(bitstring_size);
    if (!decoder->tmp_bitstring)
        goto fail;

    decoder->bitstring = av_malloc(bitstring_size);
    if (!decoder->bitstring)
        goto fail;

    return decoder;
fail:
    av_free(decoder->tmp_bitstring);
    av_free(decoder->bitstring);
    av_free(decoder->restore_buffer);
    av_free(decoder);
    return NULL;
}

static void prompegdec_free_packet(PrompegDecoderPacket *packet)
{
    if (!packet)
        return;

    av_free(packet->bytes);
    av_free(packet);
}

static int prompegdec_free_tree_elem(void *opaque, void *elem)
{
    prompegdec_free_packet((PrompegDecoderPacket *)elem);
    return 0;
}

void ff_prompegdec_free_decoder(PrompegDecoder *decoder)
{
    if (!decoder)
        return;

    av_tree_enumerate(
        decoder->fec_row_packets, NULL, NULL, prompegdec_free_tree_elem);
    av_tree_destroy(decoder->fec_row_packets);
    av_tree_enumerate(
        decoder->fec_col_packets, NULL, NULL, prompegdec_free_tree_elem);
    av_tree_destroy(decoder->fec_col_packets);
    av_tree_enumerate(decoder->packets, NULL, NULL, prompegdec_free_tree_elem);
    av_tree_destroy(decoder->packets);
    av_free(decoder->restore_buffer);
    av_free(decoder);
}

typedef struct PrompegDecoderPacketEnum {
    uint16_t min_index;
    uint16_t max_index;
    PrompegDecoderPacket *packet;
} PrompegDecoderPacketEnum;

static int prompegdec_get_first_packet_enum(void *opaque, void *elem)
{
    PrompegDecoderPacketEnum *get_packet = (PrompegDecoderPacketEnum *)opaque;
    PrompegDecoderPacket *packet = (PrompegDecoderPacket *)elem;

    if (packet->index <= get_packet->min_index)
        return 0;

    if (get_packet->max_index <= packet->index)
        return 0;

    if (!get_packet->packet) {
        get_packet->packet = packet;
        return 0;
    }

    if (get_packet->packet->index <= packet->index)
        return 0;

    get_packet->packet = packet;
    return 0;
}

static int prompegdec_get_first_packet_cmp(void *opaque, void *elem)
{
    PrompegDecoderPacketEnum *get_packet = (PrompegDecoderPacketEnum *)opaque;
    PrompegDecoderPacket *packet = (PrompegDecoderPacket *)elem;

    if (packet->index <= get_packet->min_index)
        return -1;

    if (get_packet->max_index <= packet->index)
        return 1;

    return 0;
}

static PrompegDecoderPacket *prompegdec_get_first_packet(
    struct AVTreeNode *t, uint16_t min_index, uint16_t max_index)
{
    PrompegDecoderPacketEnum get_packet;
    get_packet.min_index = min_index;
    get_packet.max_index = max_index;
    get_packet.packet = NULL;

    av_tree_enumerate(t, &get_packet, prompegdec_get_first_packet_cmp,
        prompegdec_get_first_packet_enum);

    return get_packet.packet;
}

static int prompegdec_cmp_packet(const void *left, const void *right)
{
    PrompegDecoderPacket *left_packet = (PrompegDecoderPacket *)left;
    PrompegDecoderPacket *right_packet = (PrompegDecoderPacket *)right;

    if (left_packet->index == right_packet->index)
        return 0;
    if (left_packet->index < right_packet->index)
        return -1;
    return 1;
}

static PrompegDecoderPacket *prompegdec_find_packet(
    struct AVTreeNode *t, uint16_t index)
{
    PrompegDecoderPacket packet_ref;

    packet_ref.index = index;

    return av_tree_find(t, &packet_ref, prompegdec_cmp_packet, NULL);
}

static int prompegdec_remove_packet(
    struct AVTreeNode **t, PrompegDecoderPacket *packet)
{
    struct AVTreeNode *next = NULL;

    av_tree_insert(t, packet, prompegdec_cmp_packet, &next);
    prompegdec_free_packet(packet);
    av_free(next);

    return 0;
}

static int prompegdec_insert_packet(PrompegDecoder *decoder,
    PrompegDecoderPacketType type, PrompegDecoderPacket *packet)
{
    struct AVTreeNode *next = av_tree_node_alloc();
    struct AVTreeNode **packets;

    if (!next)
        return AVERROR(ENOMEM);

    switch (type) {
    case PROMPEGDEC_FEC_ROW_PACKET:
        packets = &decoder->fec_row_packets;
        break;

    case PROMPEGDEC_FEC_COL_PACKET:
        packets = &decoder->fec_col_packets;
        break;

    default:
        packets = &decoder->packets;
    }

    av_tree_insert(packets, packet, prompegdec_cmp_packet, &next);

    if (next) {
        av_free(next);
        av_free(packet);
        AVERROR(EINVAL);
    }

    switch (type) {
    case PROMPEGDEC_FEC_ROW_PACKET:
        decoder->fec_row_packets_count++;
        break;

    case PROMPEGDEC_FEC_COL_PACKET:
        decoder->fec_col_packets_count++;
        break;

    default:
        decoder->packets_count++;
    }

    return 0;
}

typedef struct PrompegDecoderFecEnum {
    int l;
    int d;
    uint16_t packet_index;
    PrompegDecoderPacket *fec_packet;
} PrompegDecoderFecEnum;

static PrompegDecoderPacket *prompegdec_fec_packet(PrompegDecoder *decoder,
    struct AVTreeNode *t, uint16_t packet_index,
    int (*cmp)(void *opaque, void *elem), int (*enu)(void *opaque, void *elem))
{
    PrompegDecoderFecEnum fec_enum;
    fec_enum.l = decoder->l;
    fec_enum.d = decoder->d;
    fec_enum.packet_index = packet_index;
    fec_enum.fec_packet = NULL;

    av_tree_enumerate(t, &fec_enum, cmp, enu);

    return fec_enum.fec_packet;
}

static int prompegdec_fec_row_cmp(void *opaque, void *elem)
{
    PrompegDecoderFecEnum *row_enum = (PrompegDecoderFecEnum *)opaque;
    PrompegDecoderPacket *fec_packet = (PrompegDecoderPacket *)elem;

    if (fec_packet->index <= row_enum->packet_index - row_enum->l)
        return -1;

    if (row_enum->packet_index < fec_packet->index)
        return 1;

    return 0;
}

static int prompegdec_fec_row_enum(void *opaque, void *elem)
{
    PrompegDecoderFecEnum *row_enum = (PrompegDecoderFecEnum *)opaque;
    PrompegDecoderPacket *fec_packet = (PrompegDecoderPacket *)elem;

    if (fec_packet->index <= row_enum->packet_index &&
        row_enum->packet_index < fec_packet->index + row_enum->l)
        row_enum->fec_packet = fec_packet;

    return 0;
}

static PrompegDecoderPacket *prompegdec_fec_row_packet(
    PrompegDecoder *decoder, uint16_t packet_index)
{
    return prompegdec_fec_packet(decoder, decoder->fec_row_packets,
        packet_index, prompegdec_fec_row_cmp, prompegdec_fec_row_enum);
}

static int prompegdec_fec_col_cmp(void *opaque, void *elem)
{
    PrompegDecoderFecEnum *col_enum = (PrompegDecoderFecEnum *)opaque;
    PrompegDecoderPacket *fec_packet = (PrompegDecoderPacket *)elem;

    if (fec_packet->index <= col_enum->packet_index - col_enum->l * col_enum->d)
        return -1;

    if (col_enum->packet_index < fec_packet->index)
        return 1;

    return 0;
}

static int prompegdec_fec_col_enum(void *opaque, void *elem)
{
    PrompegDecoderFecEnum *col_enum = (PrompegDecoderFecEnum *)opaque;
    PrompegDecoderPacket *fec_packet = (PrompegDecoderPacket *)elem;
    int col;

    for (col = 0; col < col_enum->d; col++)
        if (fec_packet->index + col * col_enum->l == col_enum->packet_index)
            col_enum->fec_packet = fec_packet;

    return 0;
}

static PrompegDecoderPacket *prompegdec_fec_col_packet(
    PrompegDecoder *decoder, uint16_t packet_index)
{
    return prompegdec_fec_packet(decoder, decoder->fec_col_packets,
        packet_index, prompegdec_fec_col_cmp, prompegdec_fec_col_enum);
}

static int prompeg_restore_packets_buffer(PrompegDecoder *decoder,
    uint16_t index, int buffer_length, PrompegDecoderPacketType type,
    PrompegDecoderPacket *fec_packet)
{
    PrompegDecoderPacket *packet = NULL;
    int i, first = 1;
    uint8_t m;
    uint32_t ssrc;

    ff_prompeg_pack_fec_bitstring(
        decoder->bitstring, fec_packet->bytes, decoder->fec_packet_size);

    for (i = 0; i < buffer_length; i++) {
        if (decoder->restore_buffer[i]) {
            if (first) {
                m = decoder->restore_buffer[i]->bytes[1] >> 7;
                memcpy(&ssrc, decoder->restore_buffer[i]->bytes + 8, 4);
                first = 0;
            }

            ff_prompeg_pack_bitstring(decoder->tmp_bitstring,
                decoder->restore_buffer[i]->bytes, decoder->packet_size);
            ff_prompeg_xor_fast(decoder->bitstring, decoder->tmp_bitstring,
                decoder->bitstring, decoder->bitstring_size);
        }
    }

    packet = prompegdec_alloc_packet(index, decoder->packet_size);
    if (!packet)
        return AVERROR(ENOMEM);

    ff_prompeg_restore_packet(packet->bytes, decoder->bitstring, m, ssrc, index,
        decoder->bitstring_size);

    decoder->restored_packets++;
    av_log(decoder, AV_LOG_INFO,
        "Restored lost packet at index %d using FEC %s.\n", index,
        type == PROMPEGDEC_FEC_ROW_PACKET ? "row" : "col");

    av_log(decoder, AV_LOG_VERBOSE,
        "Restored ratio: %.02f%%, "
        "packets count: %d, FEC row packets count: %d, FEC col packets "
        "count: %d\n",
        prompeg_restored_ratio(decoder), decoder->packets_count,
        decoder->fec_row_packets_count, decoder->fec_col_packets_count);

    decoder->pending_packets++;

    return prompegdec_insert_packet(decoder, PROMPEGDEC_PACKET, packet);
}

static int32_t prompegdec_restore_fec_row(
    PrompegDecoder *decoder, PrompegDecoderPacket *fec_row_packet)
{
    int i;
    uint16_t missing_index, index;
    int ret, packets_count = 0;

    for (i = 0; i < decoder->l; i++) {
        index = fec_row_packet->index + i;
        decoder->restore_buffer[i] =
            prompegdec_find_packet(decoder->packets, index);

        if (decoder->restore_buffer[i]) {
            packets_count++;
        } else {
            missing_index = index;
        }
    }

    if (packets_count != decoder->l - 1)
        return 0;

    ret = prompeg_restore_packets_buffer(decoder, missing_index, decoder->l,
        PROMPEGDEC_FEC_ROW_PACKET, fec_row_packet);

    if (ret < 0)
        return ret;

    return missing_index;
}

static int32_t prompegdec_restore_fec_col(
    PrompegDecoder *decoder, PrompegDecoderPacket *fec_col_packet)
{
    int i;
    uint16_t missing_index, index;
    int ret, packets_count = 0;

    for (i = 0; i < decoder->d; i++) {
        index = fec_col_packet->index + i * decoder->l;
        decoder->restore_buffer[i] =
            prompegdec_find_packet(decoder->packets, index);

        if (decoder->restore_buffer[i]) {
            packets_count++;
        } else {
            missing_index = index;
        }
    }

    if (packets_count != decoder->d - 1)
        return 0;

    ret = prompeg_restore_packets_buffer(decoder, missing_index, decoder->d,
        PROMPEGDEC_FEC_COL_PACKET, fec_col_packet);

    if (ret < 0)
        return ret;

    return missing_index;
}

static int prompegdec_restore_fec_matrix(PrompegDecoder *decoder)
{
    int i, restored_count;
    int32_t restored_index;
    uint16_t packet_index;
    PrompegDecoderPacket *fec_packet;

    do {
        restored_count = 0;

        for (i = 0; i < decoder->d; i++) {
            packet_index = decoder->first_fec_packet_index + i * decoder->l;
            fec_packet = prompegdec_fec_row_packet(decoder, packet_index);

            if (fec_packet) {
                restored_index =
                    prompegdec_restore_fec_row(decoder, fec_packet);

                if (restored_index < 0)
                    return restored_index;

                if (restored_index == decoder->next_index)
                    return 1;

                if (restored_index)
                    restored_count++;
            }
        }

        for (i = 0; i < decoder->l; i++) {
            fec_packet = prompegdec_fec_col_packet(
                decoder, decoder->first_fec_packet_index + i);

            if (fec_packet) {
                restored_index =
                    prompegdec_restore_fec_col(decoder, fec_packet);

                if (restored_index < 0)
                    return restored_index;

                if (restored_index == decoder->next_index)
                    return 1;

                if (restored_index)
                    restored_count++;
            }
        }
    } while (restored_count);

    return 0;
}

static void prompegdec_populate_fec_data(PrompegDecoder *decoder)
{
    if (!decoder->next_fec_row)
        decoder->next_fec_row =
            prompegdec_fec_row_packet(decoder, decoder->next_index);

    if (!decoder->next_fec_col)
        decoder->next_fec_col =
            prompegdec_fec_col_packet(decoder, decoder->next_index);

    if (decoder->next_fec_row && decoder->next_fec_col)
        decoder->first_fec_packet_index = decoder->next_fec_col->index -
                                          decoder->next_index +
                                          decoder->next_fec_row->index;
    else
        decoder->first_fec_packet_index =
            decoder->next_index - decoder->l * decoder->d;
}

static PrompegDecoderPacket *prompegdec_get_next_packet(PrompegDecoder *decoder)
{
    PrompegDecoderPacket *packet =
        prompegdec_find_packet(decoder->packets, decoder->next_index);
    int restored = 0;

    if (packet)
        return packet;

    prompegdec_populate_fec_data(decoder);

    if (decoder->next_fec_row)
        restored = prompegdec_restore_fec_row(decoder, decoder->next_fec_row);

    if (!restored && decoder->next_fec_col)
        restored = prompegdec_restore_fec_col(decoder, decoder->next_fec_col);

    if (!restored && decoder->next_fec_row && decoder->next_fec_col)
        restored = prompegdec_restore_fec_matrix(decoder);

    if (!restored)
        return NULL;

    return prompegdec_find_packet(decoder->packets, decoder->next_index);
}

static int prompegdec_return_packet(
    PrompegDecoder *decoder, PrompegDecoderPacket *packet, uint8_t *bytes)
{
    PrompegDecoderPacket *old_packet;

    memcpy(bytes, packet->bytes, decoder->packet_size);
    decoder->next_index = packet->index + 1;
    decoder->next_fec_row = decoder->next_fec_col = NULL;
    decoder->pending_packets--;
    prompegdec_populate_fec_data(decoder);

    do {
        old_packet = prompegdec_get_first_packet(
            decoder->packets, 0, decoder->first_fec_packet_index);

        if (old_packet) {
            prompegdec_remove_packet(&decoder->packets, old_packet);
            decoder->packets_count--;
        }
    } while (old_packet);

    do {
        old_packet = prompegdec_get_first_packet(
            decoder->fec_col_packets, 0, decoder->first_fec_packet_index);

        if (old_packet) {
            prompegdec_remove_packet(&decoder->fec_col_packets, old_packet);
            decoder->fec_col_packets_count--;
        }
    } while (old_packet);

    do {
        old_packet = prompegdec_get_first_packet(
            decoder->fec_row_packets, 0, decoder->first_fec_packet_index);

        if (old_packet) {
            prompegdec_remove_packet(&decoder->fec_row_packets, old_packet);
            decoder->fec_row_packets_count--;
        }
    } while (old_packet);

    return decoder->packet_size;
}

int ff_prompegdec_add_packet(PrompegDecoder *decoder,
    PrompegDecoderPacketType type, uint16_t index, const uint8_t *bytes,
    int length)
{
    PrompegDecoderPacket *packet;
    int expected_length = type == PROMPEGDEC_PACKET ? decoder->packet_size
                                                    : decoder->fec_packet_size;

    if (length != expected_length)
        return AVERROR(EINVAL);

    if (index <= decoder->first_fec_packet_index)
        return 0;

    switch (type) {
    case PROMPEGDEC_FEC_ROW_PACKET:
        if (decoder->max_fec_packets <= decoder->fec_row_packets_count) {
            av_log(decoder, AV_LOG_ERROR,
                "Reached maximum of FEC row packets, dropping new packet..\n");
            return 0;
        }
        break;

    case PROMPEGDEC_FEC_COL_PACKET:
        if (decoder->max_fec_packets <= decoder->fec_col_packets_count) {
            av_log(decoder, AV_LOG_ERROR,
                "Reached maximum of FEC col packets, dropping new packet..\n");
            return 0;
        }
        break;

    default:
        if (!decoder->pending_packets)
            decoder->pending_packets++;
        if (!decoder->next_index ||
            (decoder->packets_count < decoder->min_packets &&
                decoder->next_index <= index))
            decoder->next_index = index;
    }

    packet = prompegdec_alloc_packet(index, length);
    if (!packet)
        return AVERROR(ENOMEM);

    memcpy(packet->bytes, bytes, length);

    return prompegdec_insert_packet(decoder, type, packet);
}

int ff_prompegdec_read_packet(
    PrompegDecoder *decoder, uint8_t *bytes, int length)
{
    PrompegDecoderPacket *packet;

    if (length < decoder->packet_size)
        return AVERROR(EINVAL);

    if (!decoder->pending_packets ||
        decoder->packets_count < decoder->min_packets)
        return AVERROR(EAGAIN);

    packet = prompegdec_get_next_packet(decoder);

    if (packet)
        return prompegdec_return_packet(decoder, packet, bytes);

    if (decoder->packets_count < decoder->max_packets)
        return AVERROR(EAGAIN);

    packet = prompegdec_get_first_packet(
        decoder->packets, decoder->next_index, UINT16_MAX);

    if (!packet)
        return AVERROR(EAGAIN);

    decoder->failed_packets++;

    av_log(decoder, AV_LOG_ERROR,
        "Could not restore lost packet at index %d.\n", decoder->next_index);

    av_log(decoder, AV_LOG_VERBOSE,
        "Restored ratio: %.02f%%, "
        "packets count: %d, FEC row packets count: %d, FEC col packets "
        "count: %d.\n",
        prompeg_restored_ratio(decoder), decoder->packets_count,
        decoder->fec_row_packets_count, decoder->fec_col_packets_count);

    return prompegdec_return_packet(decoder, packet, bytes);
}
