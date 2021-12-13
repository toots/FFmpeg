/*
 * AVFoundation input device
 * Copyright (c) 2014 Thilo Borgmann <thilo.borgmann@mail.de>
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
 * AVFoundation input device
 * @author Thilo Borgmann <thilo.borgmann@mail.de>
 */

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include "libavutil/channel_layout.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "libavformat/internal.h"
#include "libavutil/internal.h"
#include "libavutil/parseutils.h"
#include "libavutil/time.h"
#include "libavutil/imgutils.h"
#include "avdevice.h"

#define CLEANUP_DEVICE_ID(s) [[s stringByReplacingOccurrencesOfString:@":" withString:@"."] UTF8String]

static void av_log_avfoundation(void *s, int lvl, const char *str, OSStatus err) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    av_log(s, lvl, "AVFoundation: %s, %s\n", str,
        [[[NSError errorWithDomain:NSOSStatusErrorDomain code:err userInfo:nil] localizedDescription] UTF8String]);
    [pool release];
}

static const int avf_time_base = 1000000;

static const AVRational avf_time_base_q = {
    .num = 1,
    .den = avf_time_base
};

struct AVFPixelFormatSpec {
    enum AVPixelFormat ff_id;
    OSType avf_id;
};

static const struct AVFPixelFormatSpec avf_pixel_formats[] = {
    { AV_PIX_FMT_MONOBLACK,    kCVPixelFormatType_1Monochrome },
    { AV_PIX_FMT_RGB555BE,     kCVPixelFormatType_16BE555 },
    { AV_PIX_FMT_RGB555LE,     kCVPixelFormatType_16LE555 },
    { AV_PIX_FMT_RGB565BE,     kCVPixelFormatType_16BE565 },
    { AV_PIX_FMT_RGB565LE,     kCVPixelFormatType_16LE565 },
    { AV_PIX_FMT_RGB24,        kCVPixelFormatType_24RGB },
    { AV_PIX_FMT_BGR24,        kCVPixelFormatType_24BGR },
    { AV_PIX_FMT_0RGB,         kCVPixelFormatType_32ARGB },
    { AV_PIX_FMT_BGR0,         kCVPixelFormatType_32BGRA },
    { AV_PIX_FMT_0BGR,         kCVPixelFormatType_32ABGR },
    { AV_PIX_FMT_RGB0,         kCVPixelFormatType_32RGBA },
    { AV_PIX_FMT_BGR48BE,      kCVPixelFormatType_48RGB },
    { AV_PIX_FMT_UYVY422,      kCVPixelFormatType_422YpCbCr8 },
    { AV_PIX_FMT_YUVA444P,     kCVPixelFormatType_4444YpCbCrA8R },
    { AV_PIX_FMT_YUVA444P16LE, kCVPixelFormatType_4444AYpCbCr16 },
    { AV_PIX_FMT_YUV444P,      kCVPixelFormatType_444YpCbCr8 },
    { AV_PIX_FMT_YUV422P16,    kCVPixelFormatType_422YpCbCr16 },
    { AV_PIX_FMT_YUV422P10,    kCVPixelFormatType_422YpCbCr10 },
    { AV_PIX_FMT_YUV444P10,    kCVPixelFormatType_444YpCbCr10 },
    { AV_PIX_FMT_YUV420P,      kCVPixelFormatType_420YpCbCr8Planar },
    { AV_PIX_FMT_NV12,         kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange },
    { AV_PIX_FMT_YUYV422,      kCVPixelFormatType_422YpCbCr8_yuvs },
#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1080
    { AV_PIX_FMT_GRAY8,        kCVPixelFormatType_OneComponent8 },
#endif
    { AV_PIX_FMT_NONE, 0 }
};

typedef struct
{
    AVClass*        class;

    id              avf_delegate;
    id              avf_audio_delegate;

    AVRational      framerate;
    int             width, height;

    int             channels;
    int             big_endian;
    int             sample_rate;
    enum AVSampleFormat sample_format;

    int             capture_cursor;
    int             capture_mouse_clicks;
    int             capture_raw_data;
    int             drop_late_frames;
    int             video_is_muxed;
    int             video_is_screen;

    int             list_devices;
    int             video_device_index;
    int             video_stream_index;
    int             audio_device_index;
    int             audio_stream_index;

    char            *video_filename;
    char            *audio_filename;

    int             num_video_devices;

    enum AVPixelFormat pixel_format;

    AVCaptureSession         *capture_session;
    AVCaptureVideoDataOutput *video_output;
    AVCaptureAudioDataOutput *audio_output;

    CMSimpleQueueRef          frames_queue;
    int                       max_frames;

    AVCaptureDevice          *observed_device;
#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1070
    AVCaptureDeviceTransportControlsPlaybackMode observed_mode;
#endif
    int                      observed_quit;
} AVFContext;

/** FrameReciever class - delegate for AVCaptureSession
 */
@interface AVFFrameReceiver : NSObject
{
    AVFContext* _context;
}

- (id)initWithContext:(AVFContext*)context;

- (void)  captureOutput:(AVCaptureOutput *)captureOutput
  didOutputSampleBuffer:(CMSampleBufferRef)videoFrame
         fromConnection:(AVCaptureConnection *)connection;

@end

@implementation AVFFrameReceiver

- (id)initWithContext:(AVFContext*)context
{
    if (self = [super init]) {
        _context = context;

        // start observing if a device is set for it
#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1070
        if (_context->observed_device) {
            NSString *keyPath = NSStringFromSelector(@selector(transportControlsPlaybackMode));
            NSKeyValueObservingOptions options = NSKeyValueObservingOptionNew;

            [_context->observed_device addObserver: self
                                        forKeyPath: keyPath
                                           options: options
                                           context: _context];
        }
#endif
    }
    return self;
}

- (void)dealloc {
    // stop observing if a device is set for it
#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1070
    if (_context->observed_device) {
        NSString *keyPath = NSStringFromSelector(@selector(transportControlsPlaybackMode));
        [_context->observed_device removeObserver: self forKeyPath: keyPath];
    }
#endif
    [super dealloc];
}

- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary *)change
                       context:(void *)context {
    if (context == _context) {
#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1070
        AVCaptureDeviceTransportControlsPlaybackMode mode =
            [change[NSKeyValueChangeNewKey] integerValue];

        if (mode != _context->observed_mode) {
            if (mode == AVCaptureDeviceTransportControlsNotPlayingMode) {
                _context->observed_quit = 1;
            }
            _context->observed_mode = mode;
        }
#endif
    } else {
        [super observeValueForKeyPath: keyPath
                             ofObject: object
                               change: change
                              context: context];
    }
}

- (void)  captureOutput:(AVCaptureOutput *)captureOutput
  didOutputSampleBuffer:(CMSampleBufferRef)videoFrame
         fromConnection:(AVCaptureConnection *)connection
{
    OSStatus ret = CMSimpleQueueEnqueue(_context->frames_queue, videoFrame);

    if (ret != noErr) {
      av_log_avfoundation(_context, AV_LOG_DEBUG, "Error while queueing video frame", ret);
    }

    CFRetain(videoFrame);
}

@end

/** AudioReciever class - delegate for AVCaptureSession
 */
@interface AVFAudioReceiver : NSObject
{
    AVFContext* _context;
}

- (id)initWithContext:(AVFContext*)context;

- (void)  captureOutput:(AVCaptureOutput *)captureOutput
  didOutputSampleBuffer:(CMSampleBufferRef)audioFrame
         fromConnection:(AVCaptureConnection *)connection;

@end

@implementation AVFAudioReceiver

- (id)initWithContext:(AVFContext*)context
{
    if (self = [super init]) {
        _context = context;
    }
    return self;
}

- (void)  captureOutput:(AVCaptureOutput *)captureOutput
  didOutputSampleBuffer:(CMSampleBufferRef)audioFrame
         fromConnection:(AVCaptureConnection *)connection
{
    OSStatus ret = CMSimpleQueueEnqueue(_context->frames_queue, audioFrame);

    if (ret != noErr) {
      av_log_avfoundation(_context, AV_LOG_DEBUG, "Error while queueing audio frame", ret);
    }

    CFRetain(audioFrame);
}

@end

static void destroy_context(AVFContext* ctx)
{
    [ctx->capture_session stopRunning];

    [ctx->capture_session release];
    [ctx->video_output    release];
    [ctx->audio_output    release];
    [ctx->avf_delegate    release];
    [ctx->avf_audio_delegate release];

    CMSampleBufferRef frame;

    if (ctx->frames_queue) {
        frame = (CMSampleBufferRef)CMSimpleQueueDequeue(ctx->frames_queue);
        while (frame) {
          CFRelease(frame);
          frame = (CMSampleBufferRef)CMSimpleQueueDequeue(ctx->frames_queue);
        }

        CFRelease(ctx->frames_queue);
        ctx->frames_queue = NULL;
    }

    ctx->capture_session = NULL;
    ctx->video_output    = NULL;
    ctx->audio_output    = NULL;
    ctx->avf_delegate    = NULL;
    ctx->avf_audio_delegate = NULL;
}

static void parse_device_name(AVFormatContext *s)
{
    AVFContext *ctx = (AVFContext*)s->priv_data;
    char *tmp = av_strdup(s->url);
    char *save;

    if (tmp[0] != ':') {
        ctx->video_filename = av_strtok(tmp,  ":", &save);
        ctx->audio_filename = av_strtok(NULL, ":", &save);
    } else {
        ctx->audio_filename = av_strtok(tmp,  ":", &save);
    }
}

/**
 * Configure the video device.
 *
 * Configure the video device using a run-time approach to access properties
 * since formats, activeFormat are available since  iOS >= 7.0 or OSX >= 10.7
 * and activeVideoMaxFrameDuration is available since i0S >= 7.0 and OSX >= 10.9.
 *
 * The NSUndefinedKeyException must be handled by the caller of this function.
 *
 */
static int configure_video_device(AVFormatContext *s, AVCaptureDevice *video_device)
{
    AVFContext *ctx = (AVFContext*)s->priv_data;

    double framerate = av_q2d(ctx->framerate);
    NSObject *range = nil;
    NSObject *format = nil;
    NSObject *selected_range = nil;
    NSObject *selected_format = nil;
    CMFormatDescriptionRef formatDescription;
    CMVideoDimensions dimensions;

    // try to configure format by formats list
    // might raise an exception if no format list is given
    // (then fallback to default, no configuration)
    @try {
        for (format in [video_device valueForKey:@"formats"]) {
            formatDescription = (CMFormatDescriptionRef) [format performSelector:@selector(formatDescription)];
            dimensions = CMVideoFormatDescriptionGetDimensions(formatDescription);

            if ((ctx->width == 0 && ctx->height == 0) ||
                (dimensions.width == ctx->width && dimensions.height == ctx->height)) {

                selected_format = format;

                for (range in [format valueForKey:@"videoSupportedFrameRateRanges"]) {
                    double max_framerate;

                    [[range valueForKey:@"maxFrameRate"] getValue:&max_framerate];
                    if (fabs (framerate - max_framerate) < 0.01) {
                        selected_range = range;
                        break;
                    }
                }
            }
        }

        if (!selected_format) {
            av_log(s, AV_LOG_ERROR, "Selected video size (%dx%d) is not supported by the device.\n",
                ctx->width, ctx->height);
            goto unsupported_format;
        }

        ctx->width  = dimensions.width;
        ctx->height = dimensions.height;

        if (!selected_range) {
            av_log(s, AV_LOG_ERROR, "Selected framerate (%f) is not supported by the device.\n",
                framerate);
            if (ctx->video_is_muxed) {
                av_log(s, AV_LOG_ERROR, "Falling back to default.\n");
            } else {
                goto unsupported_format;
            }
        }

        if ([video_device lockForConfiguration:NULL] == YES) {
            if (selected_format) {
                [video_device setValue:selected_format forKey:@"activeFormat"];
            }
            if (selected_range) {
                NSValue *min_frame_duration = [selected_range valueForKey:@"minFrameDuration"];
                [video_device setValue:min_frame_duration forKey:@"activeVideoMinFrameDuration"];
                [video_device setValue:min_frame_duration forKey:@"activeVideoMaxFrameDuration"];
            }
        } else {
            av_log(s, AV_LOG_ERROR, "Could not lock device for configuration.\n");
            return AVERROR(EINVAL);
        }
    } @catch(NSException *e) {
        av_log(ctx, AV_LOG_WARNING, "Configuration of video device failed, falling back to default.\n");
    }

    return 0;

unsupported_format:

    av_log(s, AV_LOG_ERROR, "Supported modes:\n");
    for (format in [video_device valueForKey:@"formats"]) {
        CMFormatDescriptionRef formatDescription;
        CMVideoDimensions dimensions;

        formatDescription = (CMFormatDescriptionRef) [format performSelector:@selector(formatDescription)];
        dimensions = CMVideoFormatDescriptionGetDimensions(formatDescription);

        for (range in [format valueForKey:@"videoSupportedFrameRateRanges"]) {
            double min_framerate;
            double max_framerate;

            [[range valueForKey:@"minFrameRate"] getValue:&min_framerate];
            [[range valueForKey:@"maxFrameRate"] getValue:&max_framerate];
            av_log(s, AV_LOG_ERROR, "  %dx%d@[%f %f]fps\n",
                dimensions.width, dimensions.height,
                min_framerate, max_framerate);
        }
    }
    return AVERROR(EINVAL);
}

static int add_video_device(AVFormatContext *s, AVCaptureDevice *video_device)
{
    AVFContext *ctx = (AVFContext*)s->priv_data;
    int ret;
    NSError *error  = nil;
    AVCaptureInput* capture_input = nil;
    struct AVFPixelFormatSpec pxl_fmt_spec;
    NSNumber *pixel_format;
    NSDictionary *capture_dict;
    dispatch_queue_t queue;

    if (ctx->video_device_index < ctx->num_video_devices) {
        capture_input = (AVCaptureInput*) [[[AVCaptureDeviceInput alloc] initWithDevice:video_device error:&error] autorelease];
    } else {
        capture_input = (AVCaptureInput*) video_device;
    }

    if (!capture_input) {
        av_log(s, AV_LOG_ERROR, "Failed to create AV capture input device: %s\n",
               [[error localizedDescription] UTF8String]);
        return 1;
    }

    if ([ctx->capture_session canAddInput:capture_input]) {
        [ctx->capture_session addInput:capture_input];
    } else {
        av_log(s, AV_LOG_ERROR, "can't add video input to capture session\n");
        return 1;
    }

    // Attaching output
    ctx->video_output = [[AVCaptureVideoDataOutput alloc] init];

    if (!ctx->video_output) {
        av_log(s, AV_LOG_ERROR, "Failed to init AV video output\n");
        return 1;
    }

    // Configure device framerate and video size
    @try {
        if ((ret = configure_video_device(s, video_device)) < 0) {
            return ret;
        }
    } @catch (NSException *exception) {
        if (![[exception name] isEqualToString:NSUndefinedKeyException]) {
          av_log (s, AV_LOG_ERROR, "An error occurred: %s", [exception.reason UTF8String]);
          return AVERROR_EXTERNAL;
        }
    }

    // select pixel format
    pxl_fmt_spec.ff_id = AV_PIX_FMT_NONE;

    for (int i = 0; avf_pixel_formats[i].ff_id != AV_PIX_FMT_NONE; i++) {
        if (ctx->pixel_format == avf_pixel_formats[i].ff_id) {
            pxl_fmt_spec = avf_pixel_formats[i];
            break;
        }
    }

    // check if selected pixel format is supported by AVFoundation
    if (pxl_fmt_spec.ff_id == AV_PIX_FMT_NONE) {
        av_log(s, AV_LOG_ERROR, "Selected pixel format (%s) is not supported by AVFoundation.\n",
               av_get_pix_fmt_name(pxl_fmt_spec.ff_id));
        return 1;
    }

    // check if the pixel format is available for this device
    if ([[ctx->video_output availableVideoCVPixelFormatTypes] indexOfObject:[NSNumber numberWithInt:pxl_fmt_spec.avf_id]] == NSNotFound) {
        av_log(s, AV_LOG_ERROR, "Selected pixel format (%s) is not supported by the input device.\n",
               av_get_pix_fmt_name(pxl_fmt_spec.ff_id));

        pxl_fmt_spec.ff_id = AV_PIX_FMT_NONE;

        av_log(s, AV_LOG_ERROR, "Supported pixel formats:\n");
        for (NSNumber *pxl_fmt in [ctx->video_output availableVideoCVPixelFormatTypes]) {
            struct AVFPixelFormatSpec pxl_fmt_dummy;
            pxl_fmt_dummy.ff_id = AV_PIX_FMT_NONE;
            for (int i = 0; avf_pixel_formats[i].ff_id != AV_PIX_FMT_NONE; i++) {
                if ([pxl_fmt intValue] == avf_pixel_formats[i].avf_id) {
                    pxl_fmt_dummy = avf_pixel_formats[i];
                    break;
                }
            }

            if (pxl_fmt_dummy.ff_id != AV_PIX_FMT_NONE) {
                av_log(s, AV_LOG_ERROR, "  %s\n", av_get_pix_fmt_name(pxl_fmt_dummy.ff_id));

                // select first supported pixel format instead of user selected (or default) pixel format
                if (pxl_fmt_spec.ff_id == AV_PIX_FMT_NONE) {
                    pxl_fmt_spec = pxl_fmt_dummy;
                }
            }
        }

        // fail if there is no appropriate pixel format or print a warning about overriding the pixel format
        if (pxl_fmt_spec.ff_id == AV_PIX_FMT_NONE) {
            return 1;
        } else {
            av_log(s, AV_LOG_WARNING, "Overriding selected pixel format to use %s instead.\n",
                   av_get_pix_fmt_name(pxl_fmt_spec.ff_id));
        }
    }

    // set videoSettings to an empty dict for receiving raw data of muxed devices
    if (ctx->capture_raw_data) {
        ctx->pixel_format = pxl_fmt_spec.ff_id;
        ctx->video_output.videoSettings = @{ };
    } else {
        ctx->pixel_format = pxl_fmt_spec.ff_id;
        pixel_format = [NSNumber numberWithUnsignedInt:pxl_fmt_spec.avf_id];
        capture_dict = [NSDictionary dictionaryWithObject:pixel_format
                                                   forKey:(id)kCVPixelBufferPixelFormatTypeKey];

        [ctx->video_output setVideoSettings:capture_dict];
    }
    [ctx->video_output setAlwaysDiscardsLateVideoFrames:ctx->drop_late_frames];

#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1070
    // check for transport control support and set observer device if supported
    if (!ctx->video_is_screen) {
        int trans_ctrl = [video_device transportControlsSupported];
        AVCaptureDeviceTransportControlsPlaybackMode trans_mode = [video_device transportControlsPlaybackMode];

        if (trans_ctrl) {
            ctx->observed_mode   = trans_mode;
            ctx->observed_device = video_device;
        }
    }
#endif

    ctx->avf_delegate = [[AVFFrameReceiver alloc] initWithContext:ctx];

    queue = dispatch_queue_create("avf_queue", NULL);
    [ctx->video_output setSampleBufferDelegate:ctx->avf_delegate queue:queue];
    dispatch_release(queue);

    if ([ctx->capture_session canAddOutput:ctx->video_output]) {
        [ctx->capture_session addOutput:ctx->video_output];
    } else {
        av_log(s, AV_LOG_ERROR, "can't add video output to capture session\n");
        return 1;
    }

    return 0;
}

static int add_audio_device(AVFormatContext *s, AVCaptureDevice *audio_device)
{
    AVFContext *ctx = (AVFContext*)s->priv_data;
    NSError *error  = nil;
    AVCaptureDeviceInput* audio_dev_input = [[[AVCaptureDeviceInput alloc] initWithDevice:audio_device error:&error] autorelease];
    dispatch_queue_t queue;

    if (!audio_dev_input) {
        av_log(s, AV_LOG_ERROR, "Failed to create AV capture input device: %s\n",
               [[error localizedDescription] UTF8String]);
        return 1;
    }

    if ([ctx->capture_session canAddInput:audio_dev_input]) {
        [ctx->capture_session addInput:audio_dev_input];
    } else {
        av_log(s, AV_LOG_ERROR, "can't add audio input to capture session\n");
        return 1;
    }

    // Attaching output
    ctx->audio_output = [[AVCaptureAudioDataOutput alloc] init];

    if (!ctx->audio_output) {
        av_log(s, AV_LOG_ERROR, "Failed to init AV audio output\n");
        return 1;
    }

    ctx->avf_audio_delegate = [[AVFAudioReceiver alloc] initWithContext:ctx];

    queue = dispatch_queue_create("avf_audio_queue", NULL);
    [ctx->audio_output setSampleBufferDelegate:ctx->avf_audio_delegate queue:queue];
    dispatch_release(queue);

    if ([ctx->capture_session canAddOutput:ctx->audio_output]) {
        [ctx->capture_session addOutput:ctx->audio_output];
    } else {
        av_log(s, AV_LOG_ERROR, "adding audio output to capture session failed\n");
        return 1;
    }

    return 0;
}

static int get_video_config(AVFormatContext *s)
{
    AVFContext *ctx = (AVFContext*)s->priv_data;
    AVStream* stream = avformat_new_stream(s, NULL);

    if (!stream) {
        return 1;
    }

    ctx->video_stream_index = stream->index;

    avpriv_set_pts_info(stream, 64, 1, avf_time_base);

    stream->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->width      = ctx->width;
    stream->codecpar->height     = ctx->height;
    stream->codecpar->format     = ctx->pixel_format;

    return 0;
}

static int get_audio_config(AVFormatContext *s)
{
    AVFContext *ctx = (AVFContext*)s->priv_data;
    AVStream* stream;
    int bits_per_sample, is_float;

    enum AVCodecID codec_id = av_get_pcm_codec(ctx->sample_format, ctx->big_endian);

    if (codec_id == AV_CODEC_ID_NONE) {
       av_log(ctx, AV_LOG_ERROR, "Error: invalid sample format!\n");
       return AVERROR(EINVAL);
    }

    switch (ctx->sample_format) {
        case AV_SAMPLE_FMT_S16:
            bits_per_sample = 16;
            is_float = 0;
            break;
        case AV_SAMPLE_FMT_S32:
            bits_per_sample = 32;
            is_float = 0;
            break;
        case AV_SAMPLE_FMT_FLT:
            bits_per_sample = 32;
            is_float = 1;
            break;
        default:
            av_log(ctx, AV_LOG_ERROR, "Error: invalid sample format!\n");
            return AVERROR(EINVAL);
    }

    [ctx->audio_output setAudioSettings:@{
        AVFormatIDKey:               @(kAudioFormatLinearPCM),
        AVLinearPCMBitDepthKey:      @(bits_per_sample),
        AVLinearPCMIsFloatKey:       @(is_float),
        AVLinearPCMIsBigEndianKey:   @(ctx->big_endian),
        AVNumberOfChannelsKey:       @(ctx->channels),
        AVLinearPCMIsNonInterleaved: @NO,
        AVSampleRateKey:             @(ctx->sample_rate)
    }];

    stream = avformat_new_stream(s, NULL);
    if (!stream)
        return -1;

    avpriv_set_pts_info(stream, 64, 1, avf_time_base);

    stream->codecpar->codec_type     = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->sample_rate    = ctx->sample_rate;
    stream->codecpar->channels       = ctx->channels;
    stream->codecpar->channel_layout = av_get_default_channel_layout(stream->codecpar->channels);
    stream->codecpar->codec_id       = codec_id;

    ctx->audio_stream_index = stream->index;

    return 0;
}

static int avf_read_header(AVFormatContext *s)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    uint32_t num_screens    = 0;
    AVFContext *ctx         = (AVFContext*)s->priv_data;
    AVCaptureDevice *video_device = nil;
    AVCaptureDevice *audio_device = nil;
    // Find capture device
    NSArray *devices = [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
    NSArray *devices_muxed = [AVCaptureDevice devicesWithMediaType:AVMediaTypeMuxed];

    ctx->num_video_devices = [devices count] + [devices_muxed count];

#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1070
    CGGetActiveDisplayList(0, NULL, &num_screens);
#endif

    // List devices if requested
    if (ctx->list_devices) {
        int index = 0;
        av_log(ctx, AV_LOG_INFO, "AVFoundation video devices:\n");
        for (AVCaptureDevice *device in devices) {
            const char *name     = [[device localizedName] UTF8String];
            const char *uniqueId = CLEANUP_DEVICE_ID([device uniqueID]);
            index                = [devices indexOfObject:device];
            av_log(ctx, AV_LOG_INFO, "[%d] %s (ID: %s)\n", index, name, uniqueId);
        }
        for (AVCaptureDevice *device in devices_muxed) {
            const char *name     = [[device localizedName] UTF8String];
            const char *uniqueId = CLEANUP_DEVICE_ID([device uniqueID]);
            index                = [devices count] + [devices_muxed indexOfObject:device];
            av_log(ctx, AV_LOG_INFO, "[%d] %s (ID: %s)\n", index, name, uniqueId);
        }
#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1070
        if (num_screens > 0) {
            CGDirectDisplayID screens[num_screens];
            CGGetActiveDisplayList(num_screens, screens, &num_screens);
            for (int i = 0; i < num_screens; i++) {
                av_log(ctx, AV_LOG_INFO, "[%d] Capture screen %d (ID: AvfilterAvfoundationCaptureScreen%d)\n", ctx->num_video_devices + i, i, screens[i]);
            }
        }
#endif

        av_log(ctx, AV_LOG_INFO, "AVFoundation audio devices:\n");
        devices = [AVCaptureDevice devicesWithMediaType:AVMediaTypeAudio];
        for (AVCaptureDevice *device in devices) {
            const char *name     = [[device localizedName] UTF8String];
            const char *uniqueId = CLEANUP_DEVICE_ID([device uniqueID]);
            int index            = [devices indexOfObject:device];
            av_log(ctx, AV_LOG_INFO, "[%d] %s (ID: %s)\n", index, name, uniqueId);
        }
         goto fail;
    }

    // parse input filename for video and audio device
    parse_device_name(s);

    // check for device index given in filename
    if (ctx->video_device_index == -1 && ctx->video_filename) {
        sscanf(ctx->video_filename, "%d", &ctx->video_device_index);
    }
    if (ctx->audio_device_index == -1 && ctx->audio_filename) {
        sscanf(ctx->audio_filename, "%d", &ctx->audio_device_index);
    }

    if (ctx->video_device_index >= 0) {
        if (ctx->video_device_index < ctx->num_video_devices) {
            if (ctx->video_device_index < [devices count]) {
                video_device = [devices objectAtIndex:ctx->video_device_index];
            } else {
                video_device = [devices_muxed objectAtIndex:(ctx->video_device_index - [devices count])];
                ctx->video_is_muxed = 1;
            }
        } else if (ctx->video_device_index < ctx->num_video_devices + num_screens) {
#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1070
            CGDirectDisplayID screens[num_screens];
            CGGetActiveDisplayList(num_screens, screens, &num_screens);
            AVCaptureScreenInput* capture_screen_input = [[[AVCaptureScreenInput alloc] initWithDisplayID:screens[ctx->video_device_index - ctx->num_video_devices]] autorelease];

            if (ctx->framerate.num > 0) {
                capture_screen_input.minFrameDuration = CMTimeMake(ctx->framerate.den, ctx->framerate.num);
            }

#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1080
            if (ctx->capture_cursor) {
                capture_screen_input.capturesCursor = YES;
            } else {
                capture_screen_input.capturesCursor = NO;
            }
#endif

            if (ctx->capture_mouse_clicks) {
                capture_screen_input.capturesMouseClicks = YES;
            } else {
                capture_screen_input.capturesMouseClicks = NO;
            }

            video_device = (AVCaptureDevice*) capture_screen_input;
            ctx->video_is_screen = 1;
#endif
         } else {
            av_log(ctx, AV_LOG_ERROR, "Invalid device index\n");
            goto fail;
        }
    } else if (ctx->video_filename &&
               strncmp(ctx->video_filename, "none", 4)) {
        if (!strncmp(ctx->video_filename, "default", 7)) {
            video_device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        } else {
        // looking for video inputs
        for (AVCaptureDevice *device in devices) {
            const char *name = [[device localizedName] UTF8String];
            if (!strncmp(ctx->video_filename, name, strlen(ctx->video_filename))) {
                video_device = device;
                break;
            }

            const char *uniqueId = CLEANUP_DEVICE_ID([device uniqueID]);
            if (!strncmp(ctx->video_filename, uniqueId, strlen(ctx->video_filename))) {
                video_device = device;
                break;
            }
        }
        // looking for muxed inputs
        for (AVCaptureDevice *device in devices_muxed) {
            const char *name = [[device localizedName] UTF8String];
            if (!strncmp(ctx->video_filename, name, strlen(ctx->video_filename))) {
                video_device = device;
                ctx->video_is_muxed = 1;
                break;
            }

            const char *uniqueId = CLEANUP_DEVICE_ID([device uniqueID]);
            if (!strncmp(ctx->video_filename, uniqueId, strlen(ctx->video_filename))) {
                video_device = device;
                ctx->video_is_muxed = 1;
                break;
            }
        }

#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1070
        // looking for screen inputs
        if (!video_device) {
            int idx;
            CGDirectDisplayID screens[num_screens];
            CGGetActiveDisplayList(num_screens, screens, &num_screens);
            AVCaptureScreenInput* capture_screen_input = NULL;

            if(sscanf(ctx->video_filename, "Capture screen %d", &idx) && idx < num_screens) {
                capture_screen_input = [[[AVCaptureScreenInput alloc] initWithDisplayID:screens[idx]] autorelease];
            }

            if(sscanf(ctx->video_filename, "AvfilterAvfoundationCaptureScreen%d", &idx)) {
                for (int i = 0; i < num_screens; i++) {
                    if (screens[i] == idx) {
                        capture_screen_input = [[[AVCaptureScreenInput alloc] initWithDisplayID:idx] autorelease];
                    }
                }
            }

            if (capture_screen_input) {
                video_device = (AVCaptureDevice*) capture_screen_input;
                ctx->video_device_index = ctx->num_video_devices + idx;
                ctx->video_is_screen = 1;

                if (ctx->framerate.num > 0) {
                    capture_screen_input.minFrameDuration = CMTimeMake(ctx->framerate.den, ctx->framerate.num);
                }

#if !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1080
                if (ctx->capture_cursor) {
                    capture_screen_input.capturesCursor = YES;
                } else {
                    capture_screen_input.capturesCursor = NO;
                }
#endif

                if (ctx->capture_mouse_clicks) {
                    capture_screen_input.capturesMouseClicks = YES;
                } else {
                    capture_screen_input.capturesMouseClicks = NO;
                }
            }
        }
#endif
        }

        if (!video_device) {
            av_log(ctx, AV_LOG_ERROR, "Video device not found\n");
            goto fail;
        }
    }

    // get audio device
    if (ctx->audio_device_index >= 0) {
        NSArray *devices = [AVCaptureDevice devicesWithMediaType:AVMediaTypeAudio];

        if (ctx->audio_device_index >= [devices count]) {
            av_log(ctx, AV_LOG_ERROR, "Invalid audio device index\n");
            goto fail;
        }

        audio_device = [devices objectAtIndex:ctx->audio_device_index];
    } else if (ctx->audio_filename &&
               strncmp(ctx->audio_filename, "none", 4)) {
        if (!strncmp(ctx->audio_filename, "default", 7)) {
            audio_device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
        } else {
        NSArray *devices = [AVCaptureDevice devicesWithMediaType:AVMediaTypeAudio];

        for (AVCaptureDevice *device in devices) {
            const char *name = [[device localizedName] UTF8String];
            if (!strncmp(ctx->audio_filename, name, strlen(ctx->audio_filename))) {
                audio_device = device;
                break;
            }

            const char *uniqueId = CLEANUP_DEVICE_ID([device uniqueID]);
            if (!strncmp(ctx->audio_filename, uniqueId, strlen(ctx->audio_filename))) {
                audio_device = device;
                break;
            }
        }
        }

        if (!audio_device) {
            av_log(ctx, AV_LOG_ERROR, "Audio device not found\n");
             goto fail;
        }
    }

    // Video nor Audio capture device not found, looking for AVMediaTypeVideo/Audio
    if (!video_device && !audio_device) {
        av_log(s, AV_LOG_ERROR, "No AV capture device found\n");
        goto fail;
    }

    if (video_device) {
        if (ctx->video_device_index < ctx->num_video_devices) {
            av_log(s, AV_LOG_DEBUG, "'%s' opened\n", [[video_device localizedName] UTF8String]);
        } else {
            av_log(s, AV_LOG_DEBUG, "'%s' opened\n", [[video_device description] UTF8String]);
        }
    }
    if (audio_device) {
        av_log(s, AV_LOG_DEBUG, "audio device '%s' opened\n", [[audio_device localizedName] UTF8String]);
    }

    // Initialize capture session
    ctx->capture_session = [[AVCaptureSession alloc] init];

    OSStatus ret;
    ret = CMSimpleQueueCreate(kCFAllocatorDefault, ctx->max_frames, &ctx->frames_queue);

    if (ret != noErr) {
        av_log_avfoundation(s, AV_LOG_ERROR, "error while creating frame queue", ret);
        goto fail;
    }

    if (video_device && add_video_device(s, video_device)) {
        goto fail;
    }
    if (audio_device && add_audio_device(s, audio_device)) {
        goto fail;
    }

    [ctx->capture_session startRunning];

    /* Unlock device configuration only after the session is started so it
     * does not reset the capture formats */
    if (!ctx->video_is_screen) {
        [video_device unlockForConfiguration];
    }

    if (video_device && get_video_config(s)) {
        goto fail;
    }

    // set audio stream
    if (audio_device && get_audio_config(s)) {
        goto fail;
    }

    [pool release];
    return 0;

fail:
    [pool release];
    destroy_context(ctx);
    av_log(s, AV_LOG_ERROR, "Error while opening AVfoundation capture session\n");
    return AVERROR_EXTERNAL;
}

static int copy_cvpixelbuffer(AVFormatContext *s,
                               CVPixelBufferRef image_buffer,
                               AVPacket *pkt)
{
    AVFContext *ctx = s->priv_data;
    int src_linesize[4];
    const uint8_t *src_data[4];
    int width  = CVPixelBufferGetWidth(image_buffer);
    int height = CVPixelBufferGetHeight(image_buffer);
    int status;

    memset(src_linesize, 0, sizeof(src_linesize));
    memset(src_data, 0, sizeof(src_data));

    status = CVPixelBufferLockBaseAddress(image_buffer, 0);
    if (status != kCVReturnSuccess) {
        av_log(s, AV_LOG_ERROR, "Could not lock base address: %d (%dx%d)\n", status, width, height);
        return AVERROR_EXTERNAL;
    }

    if (CVPixelBufferIsPlanar(image_buffer)) {
        size_t plane_count = CVPixelBufferGetPlaneCount(image_buffer);
        int i;
        for(i = 0; i < plane_count; i++){
            src_linesize[i] = CVPixelBufferGetBytesPerRowOfPlane(image_buffer, i);
            src_data[i] = CVPixelBufferGetBaseAddressOfPlane(image_buffer, i);
        }
    } else {
        src_linesize[0] = CVPixelBufferGetBytesPerRow(image_buffer);
        src_data[0] = CVPixelBufferGetBaseAddress(image_buffer);
    }

    status = av_image_copy_to_buffer(pkt->data, pkt->size,
                                     src_data, src_linesize,
                                     ctx->pixel_format, width, height, 1);



    CVPixelBufferUnlockBaseAddress(image_buffer, 0);

    return status;
}

static int avf_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    OSStatus ret;
    int status, length;
    CMBlockBufferRef block_buffer;
    CMSampleBufferRef frame;
    CMFormatDescriptionRef format;
    AVFContext* ctx = (AVFContext*)s->priv_data;
    CMItemCount count;
    CMSampleTimingInfo timing_info;
    CVImageBufferRef image_buffer;
    size_t buffer_size;
    AVRational timebase_q;

    if (CMSimpleQueueGetCount(ctx->frames_queue) < 1)
        return AVERROR(EAGAIN);

    frame = (CMSampleBufferRef)CMSimpleQueueDequeue(ctx->frames_queue);
    format = CMSampleBufferGetFormatDescription(frame);

    switch (CMFormatDescriptionGetMediaType(format)) {
        case kCMMediaType_Video:
            length = 0;
            image_buffer = CMSampleBufferGetImageBuffer(frame);
            block_buffer = CMSampleBufferGetDataBuffer(frame);

            if (image_buffer != nil) {
                length = (int)CVPixelBufferGetDataSize(image_buffer);
            } else if (block_buffer != nil) {
                length = (int)CMBlockBufferGetDataLength(block_buffer);
            } else  {
                CFRelease(frame);
                return AVERROR(EINVAL);
            }

            status = av_new_packet(pkt, length);
            if (status < 0) {
                CFRelease(frame);
                return status;
            }

            if (CMSampleBufferGetOutputSampleTimingInfoArray(frame, 1, &timing_info, &count) == noErr) {
                timebase_q = av_make_q(1, timing_info.presentationTimeStamp.timescale);
                pkt->pts = pkt->dts = av_rescale_q(timing_info.presentationTimeStamp.value, timebase_q, avf_time_base_q);
            }

            pkt->stream_index  = ctx->video_stream_index;
            pkt->flags        |= AV_PKT_FLAG_KEY;

            if (image_buffer) {
                status = copy_cvpixelbuffer(s, image_buffer, pkt);
            } else {
                status = 0;
                ret = CMBlockBufferCopyDataBytes(block_buffer, 0, pkt->size, pkt->data);
                if (ret != kCMBlockBufferNoErr) {
                    av_log_avfoundation(s, AV_LOG_ERROR, "error while copying buffer data", ret);
                    status = AVERROR_EXTERNAL;
                }
            }
            CFRelease(frame);

            return status;

        case kCMMediaType_Audio:
            block_buffer = CMSampleBufferGetDataBuffer(frame);
            buffer_size = CMBlockBufferGetDataLength(block_buffer);

            status = av_new_packet(pkt, buffer_size);
            if (status < 0) {
                CFRelease(frame);
                return status;
            }

            ret = CMBlockBufferCopyDataBytes(block_buffer, 0, pkt->size, pkt->data);
            if (ret != kCMBlockBufferNoErr) {
                CFRelease(frame);
                av_log_avfoundation(s, AV_LOG_ERROR, "error while copying audio data", ret);
                return AVERROR_EXTERNAL;
            }

            if (CMSampleBufferGetOutputSampleTimingInfoArray(frame, 1, &timing_info, &count) == noErr) {
                timebase_q = av_make_q(1, timing_info.presentationTimeStamp.timescale);
                pkt->pts = pkt->dts = av_rescale_q(timing_info.presentationTimeStamp.value, timebase_q, avf_time_base_q);
            }

            pkt->stream_index  = ctx->audio_stream_index;
            pkt->flags        |= AV_PKT_FLAG_KEY;

            CFRelease(frame);
            return 0;
        default:
            pkt->data = NULL;
            if (ctx->observed_quit)
                return AVERROR_EOF;

            return AVERROR(EAGAIN);
    }

    return AVERROR_BUG;
}

static int avf_close(AVFormatContext *s)
{
    AVFContext* ctx = (AVFContext*)s->priv_data;
    destroy_context(ctx);
    return 0;
}

static const AVOption options[] = {
    { "channels", "number of audio channels", offsetof(AVFContext, channels), AV_OPT_TYPE_INT, {.i64=2}, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "sample_rate", "audio sample rate", offsetof(AVFContext, sample_rate), AV_OPT_TYPE_INT, {.i64=44100}, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "big_endian", "return big endian samples for audio data", offsetof(AVFContext, big_endian), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, AV_OPT_FLAG_ENCODING_PARAM },
    { "sample_format", "audio sample format", offsetof(AVFContext, sample_format), AV_OPT_TYPE_INT, {.i64=AV_SAMPLE_FMT_S16}, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "list_devices", "list available devices", offsetof(AVFContext, list_devices), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    { "video_device_index", "select video device by index for devices with same name (starts at 0)", offsetof(AVFContext, video_device_index), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { "audio_device_index", "select audio device by index for devices with same name (starts at 0)", offsetof(AVFContext, audio_device_index), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { "pixel_format", "set pixel format", offsetof(AVFContext, pixel_format), AV_OPT_TYPE_PIXEL_FMT, {.i64 = AV_PIX_FMT_YUV420P}, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM},
    { "framerate", "set frame rate", offsetof(AVFContext, framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "ntsc"}, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { "video_size", "set video size", offsetof(AVFContext, width), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    { "capture_cursor", "capture the screen cursor", offsetof(AVFContext, capture_cursor), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    { "capture_mouse_clicks", "capture the screen mouse clicks", offsetof(AVFContext, capture_mouse_clicks), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    { "capture_raw_data", "capture the raw data from device connection", offsetof(AVFContext, capture_raw_data), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    { "drop_late_frames", "drop frames that are available later than expected", offsetof(AVFContext, drop_late_frames), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    { "max_frames", "Maximun length of the queue of pending frames", offsetof(AVFContext, max_frames), AV_OPT_TYPE_INT, {.i64=10}, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },

    { NULL },
};

static const AVClass avf_class = {
    .class_name = "AVFoundation indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT,
};

const AVInputFormat ff_avfoundation_demuxer = {
    .name           = "avfoundation",
    .long_name      = NULL_IF_CONFIG_SMALL("AVFoundation input device"),
    .priv_data_size = sizeof(AVFContext),
    .read_header    = avf_read_header,
    .read_packet    = avf_read_packet,
    .read_close     = avf_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &avf_class,
};
