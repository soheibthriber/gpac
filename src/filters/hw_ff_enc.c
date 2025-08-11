/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2018-2025
 *					All rights reserved
 *
 *  This file is part of GPAC / Hardware FFmpeg encode filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <gpac/setup.h>
#include <gpac/bitstream.h>
#include <gpac/avparse.h>
#include <gpac/internal/media_dev.h>

#ifdef GPAC_HAS_FFMPEG

#include "ff_common.h"

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>

#if (LIBAVUTIL_VERSION_MAJOR < 59)
#define _avf_dur	pkt_duration
#else
#define _avf_dur	duration
#endif

typedef struct _gf_hw_ffenc_ctx
{
	// Options
	char *codec;           // Codec name (e.g., "h264_vaapi")
	char *device;          // Hardware device path
	u32 bitrate;           // Target bitrate
	u32 gop_size;          // GOP size
	u32 quality;           // Quality level (0-51)
	char *preset;          // Encoding preset
	Bool verbose;          // Verbose logging

	// Internal state
	AVCodecContext *encoder;
	AVBufferRef *hw_device_ctx;
	AVBufferRef *hw_frames_ctx;
	enum AVHWDeviceType hw_device_type;
	
	GF_FilterPid *in_pid, *out_pid;
	u32 width, height;
	u32 fps_num, fps_den;
	u32 timescale;
	
	AVFrame *hw_frame;
	Bool encoder_initialized;
	u32 frame_count;
	
#if (LIBAVCODEC_VERSION_MAJOR >= 59)
	AVPacket *pkt;
#else
	AVPacket pkt;
#endif

} GF_HWFFEncodeCtx;

static const GF_FilterCapability HWFFEncodeCaps[] =
{
	CAP_UINT(GF_CAPS_INPUT_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
	CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
	CAP_BOOL(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),
	CAP_UINT(GF_CAPS_OUTPUT_EXCLUDED, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

static GF_Err hw_ffenc_initialize(GF_Filter *filter)
{
	GF_HWFFEncodeCtx *ctx = (GF_HWFFEncodeCtx *) gf_filter_get_udta(filter);
	
	ffmpeg_setup_logs(GF_LOG_CODEC);

#if (LIBAVCODEC_VERSION_MAJOR >= 59)
	ctx->pkt = av_packet_alloc();
	if (!ctx->pkt) return GF_OUT_OF_MEM;
#endif

	// Set defaults
	if (!ctx->codec) ctx->codec = gf_strdup("h264_vaapi");
	if (!ctx->device) ctx->device = gf_strdup("/dev/dri/renderD128");
	if (!ctx->bitrate) ctx->bitrate = 2000000;
	if (!ctx->gop_size) ctx->gop_size = 50;
	if (!ctx->quality) ctx->quality = 23;
	
	return GF_OK;
}

static void hw_ffenc_finalize(GF_Filter *filter)
{
	GF_HWFFEncodeCtx *ctx = (GF_HWFFEncodeCtx *) gf_filter_get_udta(filter);
	
	if (!ctx) return;
	
	if (ctx->hw_frame) {
		av_frame_free(&ctx->hw_frame);
		ctx->hw_frame = NULL;
	}
	if (ctx->hw_frames_ctx) {
		av_buffer_unref(&ctx->hw_frames_ctx);
		ctx->hw_frames_ctx = NULL;
	}
	if (ctx->hw_device_ctx) {
		av_buffer_unref(&ctx->hw_device_ctx);
		ctx->hw_device_ctx = NULL;
	}

#if (LIBAVCODEC_VERSION_MAJOR >= 59)
	if (ctx->pkt) {
		av_packet_free(&ctx->pkt);
		ctx->pkt = NULL;
	}
#endif

	if (ctx->encoder) {
		avcodec_free_context(&ctx->encoder);
		ctx->encoder = NULL;
	}

	if (ctx->codec) {
		gf_free(ctx->codec);
		ctx->codec = NULL;
	}
	if (ctx->device) {
		gf_free(ctx->device);
		ctx->device = NULL;
	}
	if (ctx->preset) {
		gf_free(ctx->preset);
		ctx->preset = NULL;
	}
}

static GF_Err hw_ffenc_setup_hardware(GF_HWFFEncodeCtx *ctx)
{
	// Find VAAPI device type
	ctx->hw_device_type = av_hwdevice_find_type_by_name("vaapi");
	if (ctx->hw_device_type == AV_HWDEVICE_TYPE_NONE) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] VAAPI not supported\n"));
		return GF_NOT_SUPPORTED;
	}
	
	// Create hardware device context
	if (av_hwdevice_ctx_create(&ctx->hw_device_ctx, ctx->hw_device_type, ctx->device, NULL, 0) < 0) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Failed to create device\n"));
		return GF_NOT_SUPPORTED;
	}
	
	// Create hardware frames context
	ctx->hw_frames_ctx = av_hwframe_ctx_alloc(ctx->hw_device_ctx);
	if (!ctx->hw_frames_ctx) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Failed to allocate frames context\n"));
		return GF_OUT_OF_MEM;
	}
	
	AVHWFramesContext *frames_ctx = (AVHWFramesContext *)ctx->hw_frames_ctx->data;
	frames_ctx->format = AV_PIX_FMT_VAAPI;
	frames_ctx->sw_format = AV_PIX_FMT_NV12;
	frames_ctx->width = ctx->width;
	frames_ctx->height = ctx->height;
	frames_ctx->initial_pool_size = 20;
	
	if (av_hwframe_ctx_init(ctx->hw_frames_ctx) < 0) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Failed to init frames context\n"));
		return GF_NOT_SUPPORTED;
	}
	
	return GF_OK;
}

static GF_Err hw_ffenc_setup_encoder(GF_HWFFEncodeCtx *ctx)
{
	const AVCodec *codec;
	
	codec = avcodec_find_encoder_by_name(ctx->codec);
	if (!codec) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Codec %s not found\n", ctx->codec));
		return GF_NOT_SUPPORTED;
	}
	
	ctx->encoder = avcodec_alloc_context3(codec);
	if (!ctx->encoder) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Failed to allocate encoder context\n"));
		return GF_OUT_OF_MEM;
	}
	
	// Configure encoder
	ctx->encoder->width = ctx->width;
	ctx->encoder->height = ctx->height;
	ctx->encoder->time_base = (AVRational){ctx->fps_den, ctx->fps_num};
	ctx->encoder->framerate = (AVRational){ctx->fps_num, ctx->fps_den};
	ctx->encoder->pix_fmt = AV_PIX_FMT_VAAPI;
	ctx->encoder->bit_rate = ctx->bitrate;
	ctx->encoder->gop_size = ctx->gop_size;
	
	// Set hardware contexts
	ctx->encoder->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
	ctx->encoder->hw_frames_ctx = av_buffer_ref(ctx->hw_frames_ctx);
	
	// VAAPI-specific options
	if (ctx->encoder->priv_data) {
		av_opt_set(ctx->encoder->priv_data, "rc_mode", "CQP", AV_OPT_SEARCH_CHILDREN);
		ctx->encoder->global_quality = ctx->quality;
	}
	
	if (ctx->preset) {
		av_opt_set(ctx->encoder->priv_data, "preset", ctx->preset, AV_OPT_SEARCH_CHILDREN);
	}
	
	if (avcodec_open2(ctx->encoder, codec, NULL) < 0) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Failed to open encoder\n"));
		return GF_NOT_SUPPORTED;
	}
	
	// Allocate hardware frame
	ctx->hw_frame = av_frame_alloc();
	if (!ctx->hw_frame) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Failed to allocate frame\n"));
		return GF_OUT_OF_MEM;
	}
	
	if (av_hwframe_get_buffer(ctx->hw_frames_ctx, ctx->hw_frame, 0) < 0) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Failed to get frame buffer\n"));
		return GF_NOT_SUPPORTED;
	}
	
	ctx->encoder_initialized = GF_TRUE;
	return GF_OK;
}

static GF_Err hw_ffenc_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_HWFFEncodeCtx *ctx = (GF_HWFFEncodeCtx *) gf_filter_get_udta(filter);
	const GF_PropertyValue *p;
	
	if (is_remove) {
		if (ctx->out_pid) {
			gf_filter_pid_remove(ctx->out_pid);
			ctx->out_pid = NULL;
		}
		return GF_OK;
	}
	
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;
	
	if (!ctx->out_pid) {
		ctx->out_pid = gf_filter_pid_new(filter);
	}
	ctx->in_pid = pid;
	
	// Get input properties
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_WIDTH);
	if (p) ctx->width = p->value.uint;
	
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_HEIGHT);
	if (p) ctx->height = p->value.uint;
	
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_FPS);
	if (p) {
		ctx->fps_num = p->value.frac.num;
		ctx->fps_den = p->value.frac.den;
	} else {
		ctx->fps_num = 25;
		ctx->fps_den = 1;
	}
	
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_TIMESCALE);
	if (p) {
		ctx->timescale = p->value.uint;
	} else {
		ctx->timescale = 1000;
	}
	
	if (!ctx->width || !ctx->height) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Invalid dimensions: %dx%d\n", ctx->width, ctx->height));
		return GF_NOT_SUPPORTED;
	}
	
	// Setup hardware acceleration
	GF_Err e = hw_ffenc_setup_hardware(ctx);
	if (e != GF_OK) return e;
	
	// Setup encoder
	e = hw_ffenc_setup_encoder(ctx);
	if (e != GF_OK) return e;
	
	// Configure output PID
	gf_filter_pid_copy_properties(ctx->out_pid, ctx->in_pid);
	gf_filter_pid_set_property(ctx->out_pid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_AVC));
	gf_filter_pid_set_property(ctx->out_pid, GF_PROP_PID_BITRATE, &PROP_UINT(ctx->bitrate));
	gf_filter_pid_set_property(ctx->out_pid, GF_PROP_PID_DECODER_CONFIG, NULL);
	gf_filter_pid_set_property(ctx->out_pid, GF_PROP_PID_UNFRAMED, &PROP_BOOL(GF_TRUE));
	
	return GF_OK;
}

static GF_Err hw_ffenc_process(GF_Filter *filter)
{
	GF_HWFFEncodeCtx *ctx = (GF_HWFFEncodeCtx *) gf_filter_get_udta(filter);
	GF_FilterPacket *pck;
	const char *data;
	u32 size;
	int ret;
	
	if (!ctx->encoder_initialized) {
		return GF_OK;
	}
	
	pck = gf_filter_pid_get_packet(ctx->in_pid);
	if (!pck) {
		if (gf_filter_pid_is_eos(ctx->in_pid)) {
			// Flush encoder
			if (avcodec_send_frame(ctx->encoder, NULL) < 0) {
				GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Error flushing encoder\n"));
			}
			gf_filter_pid_set_eos(ctx->out_pid);
			return GF_EOS;
		}
		return GF_OK;
	}
	
	data = gf_filter_pck_get_data(pck, &size);
	if (!data) {
		gf_filter_pid_drop_packet(ctx->in_pid);
		return GF_OK;
	}
	
	// Create software frame for CPU→GPU transfer
	AVFrame *sw_frame = av_frame_alloc();
	if (!sw_frame) {
		gf_filter_pid_drop_packet(ctx->in_pid);
		return GF_OUT_OF_MEM;
	}
	
	sw_frame->format = AV_PIX_FMT_NV12;
	sw_frame->width = ctx->width;
	sw_frame->height = ctx->height;
	
	if (av_frame_get_buffer(sw_frame, 0) < 0) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Failed to allocate software frame\n"));
		av_frame_free(&sw_frame);
		gf_filter_pid_drop_packet(ctx->in_pid);
		return GF_NOT_SUPPORTED;
	}
	
	// Copy input data (assumes NV12 format)
	memcpy(sw_frame->data[0], data, ctx->width * ctx->height);
	memcpy(sw_frame->data[1], data + ctx->width * ctx->height, ctx->width * ctx->height / 2);
	
	// Transfer to hardware
	if (av_hwframe_transfer_data(ctx->hw_frame, sw_frame, 0) < 0) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Hardware transfer failed\n"));
		av_frame_free(&sw_frame);
		gf_filter_pid_drop_packet(ctx->in_pid);
		return GF_NOT_SUPPORTED;
	}
	
	// Set timing and encode
	ctx->hw_frame->pts = ctx->frame_count++;
	
	if (avcodec_send_frame(ctx->encoder, ctx->hw_frame) < 0) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Encode failed\n"));
		av_frame_free(&sw_frame);
		gf_filter_pid_drop_packet(ctx->in_pid);
		return GF_NOT_SUPPORTED;
	}
	
	// Receive encoded packets
	while (ret >= 0) {
#if (LIBAVCODEC_VERSION_MAJOR >= 59)
		AVPacket *out_pkt = ctx->pkt;
#else
		AVPacket *out_pkt = &ctx->pkt;
		av_init_packet(out_pkt);
#endif
		
		ret = avcodec_receive_packet(ctx->encoder, out_pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			break;
		}
		if (ret < 0) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFEnc] Receive packet failed\n"));
			break;
		}
		
		// Create output packet
		GF_FilterPacket *dst_pck = gf_filter_pck_new_alloc(ctx->out_pid, out_pkt->size, (u8 **)&data);
		if (dst_pck) {
			memcpy((u8 *)data, out_pkt->data, out_pkt->size);
			
			// Copy timing from input
			gf_filter_pck_set_cts(dst_pck, gf_filter_pck_get_cts(pck));
			gf_filter_pck_set_dts(dst_pck, gf_filter_pck_get_dts(pck));
			gf_filter_pck_set_duration(dst_pck, gf_filter_pck_get_duration(pck));
			
			if (out_pkt->flags & AV_PKT_FLAG_KEY) {
				gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
			}
			
			gf_filter_pck_send(dst_pck);
		}
		
		av_packet_unref(out_pkt);
	}
	
	av_frame_free(&sw_frame);
	gf_filter_pid_drop_packet(ctx->in_pid);
	
	return GF_OK;
}

#define OFFS(_n)	#_n, offsetof(GF_HWFFEncodeCtx, _n)
static GF_FilterArgs HWFFEncArgs[] =
{
	{ OFFS(codec), "Hardware codec name", GF_PROP_STRING, "h264_vaapi", NULL, 0},
	{ OFFS(device), "Hardware device path", GF_PROP_STRING, "/dev/dri/renderD128", NULL, 0},
	{ OFFS(bitrate), "Target bitrate", GF_PROP_UINT, "2000000", NULL, 0},
	{ OFFS(gop_size), "GOP size", GF_PROP_UINT, "50", NULL, 0},
	{ OFFS(quality), "Quality level (0-51)", GF_PROP_UINT, "23", NULL, 0},
	{ OFFS(preset), "Encoding preset", GF_PROP_STRING, NULL, NULL, 0},
	{ OFFS(verbose), "Verbose logging", GF_PROP_BOOL, "false", NULL, 0},
	{0}
};

GF_FilterRegister HWFFEncRegister = {
	.name = "hw_ffenc",
	.version = "1.0",
	GF_FS_SET_DESCRIPTION("Hardware-accelerated FFmpeg video encoder (VAAPI)")
	GF_FS_SET_HELP("Hardware-accelerated video encoding using FFmpeg and VAAPI.\n"
	"Simplified VAAPI encoder for development and testing.\n"
	"\n"
	"Supported Hardware:\n"
	"- Intel GPUs with VAAPI support\n"
	"- Linux systems with /dev/dri/renderD128 device\n"
	"\n"
	"Usage Examples:\n"
	"- Basic encoding: hw_ffenc:codec=h264_vaapi:bitrate=5000000\n"
	"- High quality: hw_ffenc:quality=18:gop_size=25\n"
	"- Custom device: hw_ffenc:device=/dev/dri/renderD129\n"
	)
	.private_size = sizeof(GF_HWFFEncodeCtx),
	.args = HWFFEncArgs,
	.configure_pid = hw_ffenc_configure_pid,
	SETCAPS(HWFFEncodeCaps),
	.initialize = hw_ffenc_initialize,
	.finalize = hw_ffenc_finalize,
	.process = hw_ffenc_process,
	.flags = GF_FS_REG_MAIN_THREAD,
};

const GF_FilterRegister *hw_ffenc_register(GF_FilterSession *session)
{
	return &HWFFEncRegister;
}

#else

const GF_FilterRegister *hw_ffenc_register(GF_FilterSession *session)
{
	return NULL;
}

#endif
