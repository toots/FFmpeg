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
 * Pro-MPEG Code of Practice #3 Release 2 FEC protocol
 * @author Vlad Tarca <vlad.tarca@gmail.com>
 */

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/random_seed.h"
#include "avformat.h"
#include "prompeg_utils.h"
#include "prompegdec.h"
#include "prompeg.h"
#include "config.h"

typedef struct PrompegContext {
    const AVClass *class;
    URLContext *fec_col_hd, *fec_row_hd;
    PrompegFec **fec_arr, **fec_col_tmp, **fec_col, *fec_row;
    int ttl;
    uint8_t l, d;
    uint8_t *rtp_buf;
    uint16_t rtp_col_sn, rtp_row_sn;
    uint16_t length_recovery;
    int packet_size;
    int packet_idx, packet_idx_max;
    int fec_arr_len;
    int bitstring_size;
    int rtp_buf_size;
    int init;
    int first;

    // Decoder only
    PrompegDecoder *decoder;
    int min_buffered_packets;
    int max_buffered_packets;
    int max_packet_gap;
    int max_buffered_fec_packets;
} PrompegContext;

#define OFFSET(x) offsetof(PrompegContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[] = {
    { "ttl",   "Time to live (in milliseconds, multicast only)", OFFSET(ttl), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, INT_MAX, .flags = E },
    { "l", "FEC L", OFFSET(l), AV_OPT_TYPE_INT, { .i64 =  5 }, 4, 20, .flags = E },
    { "d", "FEC D", OFFSET(d), AV_OPT_TYPE_INT, { .i64 =  5 }, 4, 20, .flags = E },
    { "min_buffered_packets", "Min decoder packets", OFFSET(min_buffered_packets), AV_OPT_TYPE_INT, { .i64 =  8 }, 0, INT_MAX, .flags = E },
    { "max_buffered_packets", "Max decoder packets", OFFSET(max_buffered_packets), AV_OPT_TYPE_INT, { .i64 =  50 }, 0, INT_MAX, .flags = E },
    { "max_packet_gap", "Max decoder packet gap.", OFFSET(max_packet_gap), AV_OPT_TYPE_INT, { .i64 =  60 }, 0, INT_MAX, .flags = E },
    { "max_buffered_fec_packets", "Max decoder FEC packets", OFFSET(max_buffered_fec_packets), AV_OPT_TYPE_INT, { .i64 =  60 }, 0, INT_MAX, .flags = E },
    { NULL }
};

static const AVClass prompeg_class = {
    .class_name = "prompeg",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int prompeg_create_bitstring(URLContext *h, const uint8_t *buf, int size,
        uint8_t **bitstring) {
    PrompegContext *s = h->priv_data;

    if (size < 12 || (buf[0] & 0xc0) != 0x80 || (buf[1] & 0x7f) != 0x21) {
        av_log(h, AV_LOG_ERROR, "Unsupported stream format (expected MPEG-TS over RTP)\n");
        return AVERROR(EINVAL);
    }
    if (size != s->packet_size) {
        av_log(h, AV_LOG_ERROR, "The RTP packet size must be constant (set pkt_size)\n");
        return AVERROR(EINVAL);
    }

    *bitstring = av_malloc(s->bitstring_size);
    if (!*bitstring) {
        av_log(h, AV_LOG_ERROR, "Failed to allocate the bitstring buffer\n");
        return AVERROR(ENOMEM);
    }

    ff_prompeg_pack_bitstring(*bitstring, buf, size);

    return 0;
}

static int prompeg_write_fec(URLContext *h, PrompegFec *fec, uint8_t type) {
    PrompegContext *s = h->priv_data;
    URLContext *hd;
    uint8_t *buf = s->rtp_buf; // zero-filled
    uint16_t sn;
    int ret;

    sn = type == PROMPEG_FEC_COL ? ++s->rtp_col_sn : ++s->rtp_row_sn;

    ff_prompeg_pack_fec_packet(buf, fec, sn, type, s->l, s->d, s->rtp_buf_size);

    hd = type == PROMPEG_FEC_COL ? s->fec_col_hd : s->fec_row_hd;
    ret = ffurl_write(hd, buf, s->rtp_buf_size);
    return ret;
}

static int prompeg_open(URLContext *h, const char *uri, int flags) {
    PrompegContext *s = h->priv_data;
    AVDictionary *udp_opts = NULL;
    int rtp_port;
    char hostname[256];
    char buf[1024];

    s->fec_col_hd = NULL;
    s->fec_row_hd = NULL;

    if (s->l * s->d > 100) {
        av_log(h, AV_LOG_ERROR, "L * D must be <= 100\n");
        return AVERROR(EINVAL);
    }

    av_url_split(NULL, 0, NULL, 0, hostname, sizeof (hostname), &rtp_port,
            NULL, 0, uri);

    if (rtp_port < 1 || rtp_port > UINT16_MAX - 4) {
        av_log(h, AV_LOG_ERROR, "Invalid RTP base port %d\n", rtp_port);
        return AVERROR(EINVAL);
    }

    if (s->ttl > 0) {
        av_dict_set_int(&udp_opts, "ttl", s->ttl, 0);
    }

    if (h->flags & AVIO_FLAG_READ)
        flags |= AVIO_FLAG_NONBLOCK;

    ff_url_join(buf, sizeof (buf), "udp", NULL, hostname, rtp_port + 2, NULL);
    if (ffurl_open_whitelist(&s->fec_col_hd, buf, flags, &h->interrupt_callback,
            &udp_opts, h->protocol_whitelist, h->protocol_blacklist, h) < 0)
        goto fail;
    ff_url_join(buf, sizeof (buf), "udp", NULL, hostname, rtp_port + 4, NULL);
    if (ffurl_open_whitelist(&s->fec_row_hd, buf, flags, &h->interrupt_callback,
            &udp_opts, h->protocol_whitelist, h->protocol_blacklist, h) < 0)
        goto fail;

    h->max_packet_size = s->fec_col_hd->max_packet_size;
    s->init = 1;

    av_dict_free(&udp_opts);
    av_log(h, AV_LOG_INFO, "ProMPEG CoP#3-R2 FEC L=%d D=%d\n", s->l, s->d);
    return 0;

fail:
    ffurl_closep(&s->fec_col_hd);
    ffurl_closep(&s->fec_row_hd);
    av_dict_free(&udp_opts);
    return AVERROR(EIO);
}

static int prompeg_init(URLContext *h, const uint8_t *buf, int size) {
    PrompegContext *s = h->priv_data;
    uint32_t seed;
    int i;

    s->fec_arr = NULL;
    s->rtp_buf = NULL;

    if (size < 12 || size > UINT16_MAX + 12) {
        av_log(h, AV_LOG_ERROR, "Invalid RTP packet size\n");
        return AVERROR_INVALIDDATA;
    }

    s->packet_idx = 0;
    s->packet_idx_max = s->l * s->d;
    s->packet_size = size;
    s->length_recovery = size - 12;
    s->rtp_buf_size = 28 + s->length_recovery; // 12 + 16: RTP + FEC headers
    s->bitstring_size = 8 + s->length_recovery; // 8: P, X, CC, M, PT, SN, TS
    s->fec_arr_len = 1 + 2 * s->l; // row + column tmp + column out

    if (h->flags & AVFMT_FLAG_BITEXACT) {
        s->rtp_col_sn = 0;
        s->rtp_row_sn = 0;
    } else {
        seed = av_get_random_seed();
        s->rtp_col_sn = seed & 0x0fff;
        s->rtp_row_sn = (seed >> 16) & 0x0fff;
    }

    s->fec_arr = av_malloc_array(s->fec_arr_len, sizeof (PrompegFec*));
    if (!s->fec_arr) {
        goto fail;
    }
    for (i = 0; i < s->fec_arr_len; i++) {
        s->fec_arr[i] = av_malloc(sizeof (PrompegFec));
        if (!s->fec_arr[i]) {
            goto fail;
        }
        s->fec_arr[i]->bitstring = av_malloc_array(s->bitstring_size, sizeof (uint8_t));
        if (!s->fec_arr[i]->bitstring) {
            av_freep(&s->fec_arr[i]);
            goto fail;
        }
    }
    s->fec_row = *s->fec_arr;
    s->fec_col = s->fec_arr + 1;
    s->fec_col_tmp = s->fec_arr + 1 + s->l;

    s->rtp_buf = av_malloc_array(s->rtp_buf_size, sizeof (uint8_t));
    if (!s->rtp_buf) {
        goto fail;
    }
    memset(s->rtp_buf, 0, s->rtp_buf_size);

    s->init = 0;
    s->first = 1;

    return 0;

fail:
    av_log(h, AV_LOG_ERROR, "Failed to allocate the FEC buffer\n");
    return AVERROR(ENOMEM);
}

int ff_prompeg_add_packet(URLContext *h, const uint8_t *buf, int size) {
    PrompegContext *s = h->priv_data;
    PrompegDecoderConfig config;
    int ret;

    if (s->init) {
        ret = prompeg_init(h, buf, size);
        if (ret < 0)
            return ret;

        h->flags |= AVIO_FLAG_NONBLOCK;

        config.l = s->l;
        config.d = s->d;
        config.packet_size = s->packet_size;
        config.fec_packet_size = s->rtp_buf_size;
        config.bitstring_size = s->bitstring_size;
        config.min_packets = s->min_buffered_packets;
        config.max_packets = s->max_buffered_packets;
        config.max_packet_gap = s->max_packet_gap;
        config.max_fec_packets = s->max_buffered_fec_packets;

        s->decoder = ff_prompegdec_alloc_decoder(h, &config);

        if (!s->decoder)
            return AVERROR(ENOMEM);
    }

    av_log(h, AV_LOG_DEBUG, "Packet add, index: %d\n", AV_RB16(buf + 2));
    return ff_prompegdec_add_packet(s->decoder, PROMPEGDEC_PACKET, AV_RB16(buf + 2), buf, size);
}

static int prompeg_write(URLContext *h, const uint8_t *buf, int size) {
    PrompegContext *s = h->priv_data;
    PrompegFec *fec_tmp;
    uint8_t *bitstring = NULL;
    int col_idx, col_out_idx, row_idx;
    int ret = 0;

    if (s->init && ((ret = prompeg_init(h, buf, size)) < 0))
        goto end;

    if ((ret = prompeg_create_bitstring(h, buf, size, &bitstring)) < 0)
        goto end;

    col_idx = s->packet_idx % s->l;
    row_idx = s->packet_idx / s->l % s->d;

    // FEC' (row) send block-aligned, xor
    if (col_idx == 0) {
        if (!s->first || s->packet_idx > 0) {
            if ((ret = prompeg_write_fec(h, s->fec_row, PROMPEG_FEC_ROW)) < 0)
                goto end;
        }
        memcpy(s->fec_row->bitstring, bitstring, s->bitstring_size);
        s->fec_row->sn = AV_RB16(buf + 2);
        s->fec_row->ts = AV_RB32(buf + 4);
    } else {
        ff_prompeg_xor_fast(s->fec_row->bitstring, bitstring, s->fec_row->bitstring,
                s->bitstring_size);
    }

    // FEC (column) xor
    if (row_idx == 0) {
        if (!s->first) {
            // swap fec_col and fec_col_tmp
            fec_tmp = s->fec_col[col_idx];
            s->fec_col[col_idx] = s->fec_col_tmp[col_idx];
            s->fec_col_tmp[col_idx] = fec_tmp;
        }
        memcpy(s->fec_col_tmp[col_idx]->bitstring, bitstring, s->bitstring_size);
        s->fec_col_tmp[col_idx]->sn = AV_RB16(buf + 2);
        s->fec_col_tmp[col_idx]->ts = AV_RB32(buf + 4);
    } else {
        ff_prompeg_xor_fast(s->fec_col_tmp[col_idx]->bitstring, bitstring,
                s->fec_col_tmp[col_idx]->bitstring, s->bitstring_size);
    }

    // FEC (column) send block-aligned
    if (!s->first && s->packet_idx % s->d == 0) {
        col_out_idx = s->packet_idx / s->d;
        if ((ret = prompeg_write_fec(h, s->fec_col[col_out_idx], PROMPEG_FEC_COL)) < 0)
            goto end;
    }

    if (++s->packet_idx >= s->packet_idx_max) {
        s->packet_idx = 0;
        if (s->first)
            s->first = 0;
    }

    ret = size;

end:
    av_free(bitstring);
    return ret;
}

static int prompeg_read_fec_packets(URLContext *h)
{
    PrompegContext *s = h->priv_data;
    int i, ret;
    PrompegDecoderPacketType packet_type;
    URLContext *url_context;

    if (ff_check_interrupt(&h->interrupt_callback))
        return AVERROR_EXIT;

    for (i = 0; i < 2; i++) {
        url_context = i == 0 ? s->fec_row_hd : s->fec_col_hd;
        packet_type = i == 0
            ? PROMPEGDEC_FEC_ROW_PACKET
            : PROMPEGDEC_FEC_COL_PACKET;

        for (;;) {
            ret = ffurl_read(url_context, s->rtp_buf, s->rtp_buf_size);
            av_log(h, AV_LOG_DEBUG, "FEC %s read %d\n",
                i == 0 ? "row" : "col", ret);

            if (ret == AVERROR(EAGAIN))
                break;

            if (ret < 0)
                return ret;

            if (ret != s->rtp_buf_size)
                return AVERROR(EINVAL); 

            av_log(h, AV_LOG_DEBUG, "FEC packet add: type: %s, index: %d\n",
                    packet_type == PROMPEGDEC_FEC_ROW_PACKET ? "row" : "col",
                    AV_RB16(s->rtp_buf + 12));

            ret = ff_prompegdec_add_packet(s->decoder, packet_type,
                    AV_RB16(s->rtp_buf + 12), s->rtp_buf, s->rtp_buf_size);

            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static int prompeg_read(URLContext *h, uint8_t *buf, int size)
{
    PrompegContext *s = h->priv_data;
    int ret;

    if (s->init)
        return AVERROR(EAGAIN);

    prompeg_read_fec_packets(h);

    ret = ff_prompegdec_read_packet(s->decoder, buf, size);

    if (4 < ret)
        av_log(h, AV_LOG_DEBUG, "Got packet %d from FEC decoder\n", AV_RB16(buf + 2));

    return ret;
}

static int prompeg_close(URLContext *h) {
    PrompegContext *s = h->priv_data;
    int i;

    ffurl_closep(&s->fec_col_hd);
    ffurl_closep(&s->fec_row_hd);

    if (s->fec_arr) {
        for (i = 0; i < s->fec_arr_len; i++) {
            av_free(s->fec_arr[i]->bitstring);
            av_freep(&s->fec_arr[i]);
        }
        av_freep(&s->fec_arr);
    }
    av_freep(&s->rtp_buf);

    ff_prompegdec_free_decoder(s->decoder);

    return 0;
}

const URLProtocol ff_prompeg_protocol = {
    .name                      = "prompeg",
    .url_open                  = prompeg_open,
    .url_write                 = prompeg_write,
    .url_read                  = prompeg_read,
    .url_close                 = prompeg_close,
    .priv_data_size            = sizeof(PrompegContext),
    .flags                     = URL_PROTOCOL_FLAG_NETWORK,
    .priv_data_class           = &prompeg_class,
};
