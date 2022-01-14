/*
 * AudioToolbox input device
 * Copyright (c) 2022 Romain Beauxis <toots@rastageeks.org>
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
 * AudioToolbox input device
 * @author Romain Beauxis <toots@rastageeks.org>
 */

#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>

#include "libavutil/channel_layout.h"
#include "libavformat/internal.h"
#include "avdevice.h"

typedef struct {
    AVClass             *class;
    CMSimpleQueueRef    frames_queue;
    AudioQueueRef       input_queue;
    uint64_t            position;
    int                 recording_buffers;
    int                 samples_per_buffer;
    int                 stream_index;
    int                 big_endian;
    enum AVSampleFormat sample_format;
    int                 channels;
    int                 list_devices;
} ATContext;

static int check_status(void *ctx, OSStatus status, const char *msg) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSError *error = [NSError errorWithDomain:NSOSStatusErrorDomain code:status userInfo:nil];

    if (status != noErr) {
        av_log(ctx, AV_LOG_ERROR, "Error: %s (%s)\n", msg, [[error localizedDescription] UTF8String]);
        [pool release];
        return 1;
    } else {
        av_log(ctx, AV_LOG_DEBUG, "OK: %s\n", msg);
        [pool release];
        return 0;
    }
}

static void input_callback(void *priv, AudioQueueRef q,
                           AudioQueueBufferRef buffer,
                           const AudioTimeStamp *ts,
                           UInt32 in,
                           const AudioStreamPacketDescription *descr) {
    ATContext *ctx = (ATContext *)priv;
    OSStatus err = CMSimpleQueueEnqueue(ctx->frames_queue, buffer);

    if (check_status(ctx, err, "CMSimpleQueueEnqueue"))
        return;
}

static av_cold int atin_read_header(AVFormatContext *avctx) {
    ATContext *ctx = (ATContext*)avctx->priv_data;
    OSStatus err = noErr;
    CFStringRef device_UID = NULL;
    CFStringRef device_name = NULL;
    AudioDeviceID *devices = NULL;
    AudioChannelLayout *channel_layout = NULL;
    AudioDeviceID device = 0;
    CFStringRef selected_device_UID = NULL;
    int num_devices;
    UInt32 i;

    enum AVCodecID codec_id = av_get_pcm_codec(ctx->sample_format, ctx->big_endian);

    if (codec_id == AV_CODEC_ID_NONE) {
       av_log(ctx, AV_LOG_ERROR, "Error: invalid sample format!\n");
       return AVERROR(EINVAL);
    }

    // get devices
    UInt32 data_size = 0;
    AudioObjectPropertyAddress prop;
    prop.mSelector = kAudioHardwarePropertyDevices;
    prop.mScope    = kAudioObjectPropertyScopeGlobal;
    prop.mElement  = 0;
    err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, NULL, &data_size);
    if (check_status(avctx, err, "AudioObjectGetPropertyDataSize devices"))
        return AVERROR(EINVAL);

    num_devices = data_size / sizeof(AudioDeviceID);
    devices = av_malloc(data_size);

    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, NULL, &data_size, devices);
    if (check_status(avctx, err, "AudioObjectGetPropertyData devices"))
        goto fail;

    if (ctx->list_devices)
        av_log(ctx, AV_LOG_INFO, "CoreAudio input devices:\n");

    for(i = 0; i < num_devices; ++i) {
        prop.mSelector  = kAudioDevicePropertyStreams;
        prop.mScope     = kAudioDevicePropertyScopeInput;
        data_size       = 0;

        err = AudioObjectGetPropertyDataSize(devices[i], &prop, 0, NULL, &data_size);
        if (check_status(avctx, err, "AudioObjectGetPropertyData Streams"))
            continue;

        UInt32 streamCount = data_size / sizeof(AudioStreamID);

        if (streamCount <= 0)
            continue;

        // UID
        data_size = sizeof(device_UID);
        prop.mSelector = kAudioDevicePropertyDeviceUID;
        err = AudioObjectGetPropertyData(devices[i], &prop, 0, NULL, &data_size, &device_UID);
        if (check_status(avctx, err, "AudioObjectGetPropertyData UID"))
            continue;

        if (!strcmp(avctx->url, CFStringGetCStringPtr(device_UID, kCFStringEncodingUTF8))) {
            selected_device_UID = device_UID;
            device = devices[i];
        }

        // name
        data_size = sizeof(device_name);
        prop.mSelector = kAudioDevicePropertyDeviceNameCFString;
        err = AudioObjectGetPropertyData(devices[i], &prop, 0, NULL, &data_size, &device_name);
        if (check_status(avctx, err, "AudioObjecTGetPropertyData name"))
            continue;

        if (!strcmp(avctx->url, CFStringGetCStringPtr(device_name, kCFStringEncodingUTF8))) {
            selected_device_UID = device_name;
            device = devices[i];
        }

        if (ctx->list_devices)
            av_log(ctx, AV_LOG_INFO, "ID: %s, name: %s\n",
                   CFStringGetCStringPtr(device_UID, kCFStringEncodingUTF8),
                   CFStringGetCStringPtr(device_name, kCFStringEncodingUTF8));
    }

    if (!device_UID) {
      av_log(ctx, AV_LOG_ERROR, "No device found!\n");
      goto fail;
    }

    if (ctx->list_devices)
        goto fail;

    Float64 sample_rate;
    prop.mSelector = kAudioDevicePropertyNominalSampleRate;
    prop.mScope    = kAudioObjectPropertyScopeInput;
    data_size      = sizeof(sample_rate);
    err = AudioObjectGetPropertyData(device, &prop, 0, NULL, &data_size, &sample_rate);
    if (check_status(avctx, err, "AudioObjecTGetPropertyData SampleRate"))
        goto fail;

    if (!ctx->channels) {
        prop.mSelector = kAudioDevicePropertyPreferredChannelLayout;
        prop.mScope    = kAudioObjectPropertyScopeInput;
        UInt32 channel_layout_size;

        err = AudioObjectGetPropertyDataSize(device, &prop, 0, NULL, &channel_layout_size);
        if (check_status(avctx, err, "AudioObjectGetPropertyDataSize PreferredChannelLayout"))
            goto fail;

        channel_layout = av_malloc(channel_layout_size);
        err = AudioObjectGetPropertyData(device, &prop, 0, NULL, &channel_layout_size, channel_layout);
        if (check_status(avctx, err, "AudioObjecTGetPropertyData PreferredChannelLayout"))
            goto fail;

        data_size = sizeof(ctx->channels);
        err = AudioFormatGetProperty(kAudioFormatProperty_NumberOfChannelsForLayout, channel_layout_size, channel_layout, &data_size, &ctx->channels);
        if (check_status(avctx, err, "AudioFormatGetProperty NumberOfChannelsForLayout"))
           goto fail;
    }

    AudioStreamBasicDescription record_format;
    memset(&record_format, 0, sizeof(record_format));
    record_format.mFormatID         = kAudioFormatLinearPCM;
    record_format.mChannelsPerFrame = ctx->channels;
    record_format.mFormatFlags      = kAudioFormatFlagIsPacked;
    record_format.mBitsPerChannel   = av_get_bytes_per_sample(ctx->sample_format) << 3;

    if (ctx->big_endian)
        record_format.mFormatFlags |= kAudioFormatFlagIsBigEndian;

    switch (ctx->sample_format) {
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S32:
            record_format.mFormatFlags |= kAudioFormatFlagIsSignedInteger;
            break;
        case AV_SAMPLE_FMT_FLT:
            record_format.mFormatFlags |= kAudioFormatFlagIsFloat;
            break;
        default:
            av_log(ctx, AV_LOG_ERROR, "Error: invalid sample format!\n");
            goto fail;
    }

    av_log(ctx, AV_LOG_DEBUG, "Audio Input: %s\n", CFStringGetCStringPtr(selected_device_UID, kCFStringEncodingUTF8));
    av_log(ctx, AV_LOG_DEBUG, "samplerate: %d\n", (int)sample_rate);
    av_log(ctx, AV_LOG_DEBUG, "channels: %d\n", ctx->channels);
    av_log(ctx, AV_LOG_DEBUG, "Input format: %s\n", avcodec_get_name(codec_id));

    data_size = sizeof(record_format);
    err = AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, NULL, &data_size, &record_format); 
    if (check_status(avctx, err, "AudioFormatGetProperty FormatInfo"))
        goto fail;

    err = AudioQueueNewInput(&record_format, input_callback, ctx, NULL, NULL, 0, &ctx->input_queue);
    if (check_status(avctx, err, "AudioQueueNewInput"))
        goto fail;

    err = AudioQueueSetProperty(ctx->input_queue, kAudioQueueProperty_CurrentDevice, &selected_device_UID, sizeof(selected_device_UID));
    if (check_status(avctx, err, "AudioQueueSetProperty CurrentDevice"))
        goto fail;

    UInt32 buffer_size = ctx->samples_per_buffer * ctx->channels * av_get_bytes_per_sample(ctx->sample_format);
    for(i = 0; i < ctx->recording_buffers; i++) {
        AudioQueueBufferRef buffer;
        err = AudioQueueAllocateBuffer(ctx->input_queue, buffer_size, &buffer);
        if (check_status(avctx, err, "AudioQueueAllocateBuffer"))
            goto fail;

        err = AudioQueueEnqueueBuffer(ctx->input_queue, buffer, 0, NULL);
        if (check_status(avctx, err, "AudioQueueEnqueueBuffer"))
            goto fail;
    }

    err = CMSimpleQueueCreate(kCFAllocatorDefault, ctx->recording_buffers, &ctx->frames_queue);
    if (check_status(avctx, err, "CMSimpleQueueCreate"))
        goto fail;

    CFRetain(ctx->frames_queue);

    err = AudioQueueStart(ctx->input_queue, NULL);
    if (check_status(avctx, err, "AudioQueueStart"))
        goto fail;

    AVStream* stream = avformat_new_stream(avctx, NULL);
    stream->codecpar->codec_type     = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->sample_rate    = sample_rate;
    stream->codecpar->channels       = ctx->channels;
    stream->codecpar->channel_layout = av_get_default_channel_layout(stream->codecpar->channels);
    stream->codecpar->codec_id       = codec_id;

    avpriv_set_pts_info(stream, 64, 1, sample_rate);

    ctx->stream_index = stream->index;
    ctx->position     = 0;

    av_freep(&devices);
    av_freep(&channel_layout);
    return 0;

fail:
    av_freep(&channel_layout);
    return AVERROR(EIO);
}

static int atin_read_packet(AVFormatContext *avctx, AVPacket *pkt) {
    ATContext *ctx = (ATContext*)avctx->priv_data;

    if (CMSimpleQueueGetCount(ctx->frames_queue) < 1)
        return AVERROR(EAGAIN);

    AudioQueueBufferRef buffer = (AudioQueueBufferRef)CMSimpleQueueDequeue(ctx->frames_queue);

    int status = av_new_packet(pkt, buffer->mAudioDataByteSize);
    if (status < 0) {
        AudioQueueEnqueueBuffer(ctx->input_queue, buffer, 0, NULL);
        return status;
    }

    pkt->stream_index = ctx->stream_index;
    pkt->flags       |= AV_PKT_FLAG_KEY;
    pkt->pts = pkt->dts = ctx->position;

    ctx->position += buffer->mAudioDataByteSize / (ctx->channels * av_get_bytes_per_sample(ctx->sample_format));

    memcpy(pkt->data, buffer->mAudioData, buffer->mAudioDataByteSize);
    AudioQueueEnqueueBuffer(ctx->input_queue, buffer, 0, NULL);

    return 0;
}

static av_cold int atin_close(AVFormatContext *avctx) {
    ATContext *ctx = (ATContext*)avctx->priv_data;

    if (ctx->input_queue) {
        AudioQueueDispose(ctx->input_queue, true);
        ctx->input_queue = NULL;
    }

    if (ctx->frames_queue) {
        CFRelease(ctx->frames_queue);
        ctx->frames_queue = NULL;
    }

    return 0;
}

static const AVOption options[] = {
    { "channels", "number of audio channels", offsetof(ATContext, channels), AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "recording_buffers", "number of recording buffers in the input queue", offsetof(ATContext, recording_buffers), AV_OPT_TYPE_INT, {.i64=3}, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "samples_per_buffer", "size, in samples, of each input buffer in the queue", offsetof(ATContext, samples_per_buffer), AV_OPT_TYPE_INT, {.i64=2048}, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "big_endian", "return big endian samples", offsetof(ATContext, big_endian), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, AV_OPT_FLAG_ENCODING_PARAM },
    { "sample_format", "sample format", offsetof(ATContext, sample_format), AV_OPT_TYPE_INT, {.i64=AV_SAMPLE_FMT_S16}, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "list_devices", "list available audio devices", offsetof(ATContext, list_devices), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, AV_OPT_FLAG_ENCODING_PARAM },
    { NULL },
};

static const AVClass atin_class = {
    .class_name = "AudioToolbox",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
};

const AVInputFormat ff_audiotoolbox_demuxer = {
    .name           = "audiotoolbox",
    .long_name      = NULL_IF_CONFIG_SMALL("AudioToolbox input device"),
    .priv_data_size = sizeof(ATContext),
    .read_header    = atin_read_header,
    .read_packet    = atin_read_packet,
    .read_close     = atin_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &atin_class,
};
