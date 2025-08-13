/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2018-2025
 *					All rights reserved
 *
 *  This file is part of GPAC / Hardware FFmpeg decode filter
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

#ifdef GPAC_HAS_FFMPEG

#include "ff_common.h"

typedef struct
{
	/* FFmpeg decoder components */
	AVCodecContext *decoder;
	AVFrame *frame;
	const AVCodec *codec;
	
	/* Hardware acceleration configuration */
	char *hwaccel;          /* Hardware acceleration method (vaapi, etc.) */
	char *hwdevice;         /* Hardware device path/name */
	enum AVHWDeviceType hw_type;
	enum AVPixelFormat hw_pix_fmt;
	AVBufferRef *hw_device_ctx;
	Bool hw_accel_enabled;
	
	/* GPAC filter state */
	GF_FilterPid *in_pid, *out_pid;
	u32 width, height;
	u32 codec_id;
	GF_List *src_packets;   /* Source packets for timing correlation */
	u64 last_cts;
	
#if (LIBAVCODEC_VERSION_MAJOR >= 59)
	AVPacket *pkt;
#else
	AVPacket pkt;
#endif
} GF_HWFFDecodeCtx;

/* Hardware pixel format selection callback for FFmpeg decoder */
static enum AVPixelFormat hw_get_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
	GF_HWFFDecodeCtx *s = (GF_HWFFDecodeCtx*)ctx->opaque;
	const enum AVPixelFormat *p;

	for (p = pix_fmts; *p != -1; p++) {
		if (*p == s->hw_pix_fmt) {
			GF_LOG(GF_LOG_INFO, GF_LOG_CODEC, ("[HWFFDec] Selected hardware pixel format %d\n", *p));
			return *p;
		}
	}

	GF_LOG(GF_LOG_WARNING, GF_LOG_CODEC, ("[HWFFDec] Hardware format not available, using software\n"));
	return avcodec_default_get_format(ctx, pix_fmts);
}

/* Initialize hardware acceleration context and device */
static GF_Err hw_init_accel(GF_HWFFDecodeCtx *ctx)
{
	int ret;
	enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
	
	/* Determine hardware acceleration type */
	if (!ctx->hwaccel || !strcmp(ctx->hwaccel, "auto")) {
		/* Auto-detect: prefer VAAPI on Linux systems */
		type = av_hwdevice_find_type_by_name("vaapi");
		if (type == AV_HWDEVICE_TYPE_NONE) {
			GF_LOG(GF_LOG_INFO, GF_LOG_CODEC, ("[HWFFDec] VAAPI not available, hardware acceleration disabled\n"));
			return GF_OK;
		}
	} else {
		type = av_hwdevice_find_type_by_name(ctx->hwaccel);
		if (type == AV_HWDEVICE_TYPE_NONE) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFDec] Hardware acceleration type %s not found\n", ctx->hwaccel));
			return GF_NOT_SUPPORTED;
		}
	}

	ctx->hw_type = type;
	
	/* Create hardware device context */
	ret = av_hwdevice_ctx_create(&ctx->hw_device_ctx, type, ctx->hwdevice, NULL, 0);
	if (ret < 0) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFDec] Failed to create hardware device context: %s\n", av_err2str(ret)));
		return GF_NOT_SUPPORTED;
	}
	
	/* Find compatible hardware pixel format */
	for (int i = 0;; i++) {
		const AVCodecHWConfig *config = avcodec_get_hw_config(ctx->codec, i);
		if (!config) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFDec] No hardware config found for codec\n"));
			av_buffer_unref(&ctx->hw_device_ctx);
			return GF_NOT_SUPPORTED;
		}
		
		if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
			config->device_type == type) {
			ctx->hw_pix_fmt = config->pix_fmt;
			break;
		}
	}
	
	/* Configure decoder for hardware acceleration */
	ctx->decoder->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
	ctx->decoder->get_format = hw_get_format;
	ctx->decoder->opaque = ctx;
	
	ctx->hw_accel_enabled = GF_TRUE;
	GF_LOG(GF_LOG_INFO, GF_LOG_CODEC, ("[HWFFDec] Hardware acceleration enabled: %s\n", av_hwdevice_get_type_name(type)));
	
	return GF_OK;
}

static GF_Err hw_ffdec_initialize(GF_Filter *filter)
{
	GF_HWFFDecodeCtx *ctx = (GF_HWFFDecodeCtx*) gf_filter_get_udta(filter);
	ctx->src_packets = gf_list_new();
	
#if (LIBAVCODEC_VERSION_MAJOR >= 59)
	ctx->pkt = av_packet_alloc();
#endif
	ctx->frame = av_frame_alloc();
	
	return GF_OK;
}

static void hw_ffdec_finalize(GF_Filter *filter)
{
	GF_HWFFDecodeCtx *ctx = (GF_HWFFDecodeCtx*) gf_filter_get_udta(filter);
	
	if (ctx->decoder) {
		avcodec_free_context(&ctx->decoder);
	}
	if (ctx->frame) {
		av_frame_free(&ctx->frame);
	}
#if (LIBAVCODEC_VERSION_MAJOR >= 59)
	if (ctx->pkt) {
		av_packet_free(&ctx->pkt);
	}
#endif
	if (ctx->hw_device_ctx) {
		av_buffer_unref(&ctx->hw_device_ctx);
	}
	
	while (gf_list_count(ctx->src_packets)) {
		GF_FilterPacket *pck = gf_list_pop_back(ctx->src_packets);
		gf_filter_pck_unref(pck);
	}
	gf_list_del(ctx->src_packets);
	
	/* Note: hwaccel and hwdevice are filter arguments automatically freed by GPAC */
}

static GF_Err hw_ffdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_HWFFDecodeCtx *ctx = (GF_HWFFDecodeCtx*) gf_filter_get_udta(filter);
	const GF_PropertyValue *prop;
	u32 codec_id, ff_codecid;
	
	if (is_remove) {
		ctx->in_pid = NULL;
		return GF_OK;
	}
	if (ctx->in_pid && (ctx->in_pid != pid)) {
		return GF_REQUIRES_NEW_INSTANCE;
	}
	
	ctx->in_pid = pid;
	
	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_CODECID);
	if (!prop) return GF_NOT_SUPPORTED;
	codec_id = prop->value.uint;
	ctx->codec_id = codec_id;
	
	ff_codecid = ffmpeg_codecid_from_gpac(codec_id, NULL);
	if (!ff_codecid) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFDec] Unsupported codec %s\n", gf_codecid_name(codec_id)));
		return GF_NOT_SUPPORTED;
	}
	
	ctx->codec = avcodec_find_decoder(ff_codecid);
	if (!ctx->codec) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFDec] No decoder found for codec %s\n", gf_codecid_name(codec_id)));
		return GF_NOT_SUPPORTED;
	}
	
	ctx->decoder = avcodec_alloc_context3(ctx->codec);
	if (!ctx->decoder) {
		return GF_OUT_OF_MEM;
	}
	
	/* Configure decoder from stream properties */
	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_WIDTH);
	if (prop) ctx->width = prop->value.uint;
	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_HEIGHT);
	if (prop) ctx->height = prop->value.uint;
	
	/* Set decoder extradata (SPS/PPS for H.264, etc.) */
	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_DECODER_CONFIG);
	if (prop && prop->value.data.ptr) {
		ctx->decoder->extradata = av_malloc(prop->value.data.size + AV_INPUT_BUFFER_PADDING_SIZE);
		if (ctx->decoder->extradata) {
			memcpy(ctx->decoder->extradata, prop->value.data.ptr, prop->value.data.size);
			memset(ctx->decoder->extradata + prop->value.data.size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
			ctx->decoder->extradata_size = prop->value.data.size;
		}
	}
	
	/* Initialize hardware acceleration if available */
	GF_Err e = hw_init_accel(ctx);
	if (e != GF_OK && e != GF_NOT_SUPPORTED) {
		return e;
	}
	
	/* Open the decoder */
	int ret = avcodec_open2(ctx->decoder, ctx->codec, NULL);
	if (ret < 0) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFDec] Failed to open decoder: %s\n", av_err2str(ret)));
		return GF_NOT_SUPPORTED;
	}
	
	/* Create output PID */
	if (!ctx->out_pid) {
		ctx->out_pid = gf_filter_pid_new(filter);
	}
	
	/* Configure output stream properties */
	gf_filter_pid_copy_properties(ctx->out_pid, pid);
	gf_filter_pid_set_property(ctx->out_pid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->out_pid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_YUV));
	
	if (ctx->width && ctx->height) {
		gf_filter_pid_set_property(ctx->out_pid, GF_PROP_PID_WIDTH, &PROP_UINT(ctx->width));
		gf_filter_pid_set_property(ctx->out_pid, GF_PROP_PID_HEIGHT, &PROP_UINT(ctx->height));
	}
	
	GF_LOG(GF_LOG_INFO, GF_LOG_CODEC, ("[HWFFDec] Configured for %s %dx%d, HW accel: %s\n", 
		gf_codecid_name(codec_id), ctx->width, ctx->height, 
		ctx->hw_accel_enabled ? "enabled" : "disabled"));
	
	return GF_OK;
}

static GF_Err hw_ffdec_process(GF_Filter *filter)
{
	GF_HWFFDecodeCtx *ctx = (GF_HWFFDecodeCtx *) gf_filter_get_udta(filter);
	AVPacket *pkt;
	AVFrame *frame;
	Bool is_eos = GF_FALSE;
	s32 res, gotpic;
	const char *data = NULL;
	u32 size = 0;
	GF_FilterPacket *pck_src;
	GF_FilterPacket *dst_pck;
	GF_FilterPacket *pck = gf_filter_pid_get_packet(ctx->in_pid);

	if (!pck) {
		is_eos = gf_filter_pid_is_eos(ctx->in_pid);
		if (!is_eos) return GF_OK;
	}

	if (!ctx->decoder) return GF_OK;

	frame = ctx->frame;

#if (LIBAVCODEC_VERSION_MAJOR >= 59)
	pkt = ctx->pkt;
	av_packet_unref(pkt);
#else
	pkt = &ctx->pkt;
	av_init_packet(pkt);
#endif

	pck_src = NULL;
	if (pck) {
		data = gf_filter_pck_get_data(pck, &size);
		if (!size) {
			gf_filter_pid_drop_packet(ctx->in_pid);
			return GF_OK;
		}

		pck_src = pck;
		gf_filter_pck_ref_props(&pck_src);
		if (pck_src) gf_list_add(ctx->src_packets, pck_src);

		pkt->dts = gf_filter_pck_get_dts(pck);
		pkt->pts = gf_filter_pck_get_cts(pck);
		pkt->duration = gf_filter_pck_get_duration(pck);
		if (gf_filter_pck_get_sap(pck) > 0)
			pkt->flags = AV_PKT_FLAG_KEY;
	}
	pkt->data = (uint8_t*)data;
	pkt->size = size;

	gotpic = 0;
#if (LIBAVCODEC_VERSION_MAJOR < 59)
	res = avcodec_decode_video2(ctx->decoder, frame, &gotpic, pkt);
#else
	res = avcodec_send_packet(ctx->decoder, pkt);
	if (res == 0 || res == AVERROR_EOF) {
		gotpic = 0;
		res = avcodec_receive_frame(ctx->decoder, frame);
		if (res == 0) {
			gotpic = 1;
		} else if (res == AVERROR(EAGAIN) || res == AVERROR_EOF) {
			res = 0;
		}
	}
#endif

	if (pck) gf_filter_pid_drop_packet(ctx->in_pid);

	if (!gotpic) {
		if (is_eos) {
			gf_filter_pid_set_eos(ctx->out_pid);
			return GF_EOS;
		}
		return GF_OK;
	}

	if (res < 0) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFDec] Failed to decode frame: %s\n", av_err2str(res)));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	/* Handle hardware-to-software frame transfer for output */
	if (gotpic && ctx->hw_accel_enabled && frame->format == ctx->hw_pix_fmt) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[HWFFDec] Transferring hardware frame to system memory\n"));
		
		/* Allocate software frame for CPU-accessible data */
		AVFrame *sw_frame = av_frame_alloc();
		if (!sw_frame) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFDec] Failed to allocate SW frame\n"));
			return GF_OUT_OF_MEM;
		}
		
		/* Configure software frame properties */
		av_frame_copy_props(sw_frame, frame);
		sw_frame->width = frame->width;
		sw_frame->height = frame->height;
		sw_frame->format = AV_PIX_FMT_YUV420P;
		
		/* Allocate CPU-accessible buffers */
		int ret = av_frame_get_buffer(sw_frame, 16);
		if (ret < 0) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFDec] Failed to allocate SW frame buffers: %s\n", 
				av_err2str(ret)));
			av_frame_free(&sw_frame);
			return GF_IO_ERR;
		}
		
		/* Transfer data from hardware frame to software frame */
		ret = av_hwframe_transfer_data(sw_frame, frame, 0);
		if (ret < 0) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HWFFDec] Failed to transfer HW frame: %s\n", 
				av_err2str(ret)));
			av_frame_free(&sw_frame);
			return GF_IO_ERR;
		}
		
		/* Replace hardware frame with transferred software frame */
		av_frame_unref(frame);
		av_frame_move_ref(frame, sw_frame);
		av_frame_free(&sw_frame);
	}

	/* Update output properties if frame dimensions changed */
	if (ctx->width != frame->width || ctx->height != frame->height) {
		ctx->width = frame->width;
		ctx->height = frame->height;
		gf_filter_pid_set_property(ctx->out_pid, GF_PROP_PID_WIDTH, &PROP_UINT(ctx->width));
		gf_filter_pid_set_property(ctx->out_pid, GF_PROP_PID_HEIGHT, &PROP_UINT(ctx->height));
	}

	/* Find source packet for timestamp correlation */
	pck_src = NULL;
	u32 count = gf_list_count(ctx->src_packets);
	for (u32 i = 0; i < count; i++) {
		u64 cts;
		pck_src = gf_list_get(ctx->src_packets, i);
		cts = gf_filter_pck_get_cts(pck_src);
		if (cts == frame->pts) break;
#if (LIBAVCODEC_VERSION_MAJOR < 59)
		if (cts == frame->pkt_pts) break;
#endif
		pck_src = NULL;
	}

	/* Determine output timestamp */
	u64 out_cts;
	if (pck_src) {
		out_cts = gf_filter_pck_get_cts(pck_src);
	} else {
		if (frame->pts == AV_NOPTS_VALUE)
			out_cts = ctx->last_cts + 1;
		else
			out_cts = frame->pts;
	}
	ctx->last_cts = out_cts;

	/* Create output packet with decoded frame data */
	u32 output_size = ctx->width * ctx->height * 3 / 2; /* YUV420P format */
	u8 *output_data;
	dst_pck = gf_filter_pck_new_alloc(ctx->out_pid, output_size, &output_data);
	if (!dst_pck) return GF_OUT_OF_MEM;

	/* Copy YUV420P frame data to output packet */
	if (frame->format == AV_PIX_FMT_YUV420P) {
		memcpy(output_data, frame->data[0], ctx->width * ctx->height);
		memcpy(output_data + ctx->width * ctx->height, frame->data[1], ctx->width * ctx->height / 4);
		memcpy(output_data + ctx->width * ctx->height * 5 / 4, frame->data[2], ctx->width * ctx->height / 4);
	}

	if (pck_src) {
		gf_filter_pck_merge_properties(pck_src, dst_pck);
		gf_filter_pck_set_dependency_flags(dst_pck, 0);
		gf_list_del_item(ctx->src_packets, pck_src);
		gf_filter_pck_unref(pck_src);
	} else {
		gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	}

	gf_filter_pck_set_dts(dst_pck, out_cts);
	gf_filter_pck_set_cts(dst_pck, out_cts);
	gf_filter_pck_send(dst_pck);

	return GF_OK;
}

#define OFFS(_n)	#_n, offsetof(GF_HWFFDecodeCtx, _n)

static const GF_FilterArgs HWFFDecodeArgs[] =
{
	{ OFFS(hwaccel), "Hardware acceleration type (auto, vaapi, etc.)", GF_PROP_NAME, "auto", NULL, GF_FS_ARG_HINT_ADVANCED},
	{ OFFS(hwdevice), "Hardware device identifier", GF_PROP_NAME, NULL, NULL, GF_FS_ARG_HINT_ADVANCED},
	{0}
};

const int HWFFDEC_STATIC_ARGS = (sizeof (HWFFDecodeArgs) / sizeof (GF_FilterArgs)) - 1;

static const GF_FilterCapability HWFFDecodeCaps[] =
{
	CAP_UINT(GF_CAPS_INPUT,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
	CAP_UINT(GF_CAPS_INPUT,  GF_PROP_PID_CODECID, GF_CODECID_AVC),
	CAP_UINT(GF_CAPS_INPUT,  GF_PROP_PID_CODECID, GF_CODECID_HEVC),
	CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
	CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister HWFFDecodeRegister = {
	.name = "hw_ffdec",
	.description = "FFmpeg hardware-accelerated video decoder",
	.private_size = sizeof(GF_HWFFDecodeCtx),
	.args = HWFFDecodeArgs,
	.initialize = hw_ffdec_initialize,
	.finalize = hw_ffdec_finalize,
	.configure_pid = hw_ffdec_configure_pid,
	.process = hw_ffdec_process,
	.probe_data = NULL,
	SETCAPS(HWFFDecodeCaps),
	.flags = GF_FS_REG_MAIN_THREAD,
	.version = "1.0",
	.author = "GPAC-licensing"
};

const GF_FilterRegister *hw_ffdec_register(GF_FilterSession *session)
{
	return ffmpeg_build_register(session, &HWFFDecodeRegister, HWFFDecodeArgs, HWFFDEC_STATIC_ARGS, FF_REG_TYPE_DECODE);
}

#else
const GF_FilterRegister *hw_ffdec_register(GF_FilterSession *session)
{
	return NULL;
}
#endif // GPAC_HAS_FFMPEG
