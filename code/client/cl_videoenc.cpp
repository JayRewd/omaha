/*
===========================================================================
Copyright (C) 2026 Project: Omaha

This file is part of Project: Omaha source code.

Project: Omaha builds upon OpenMoHAA / ioquake3 / F.A.K.K. foundations.
Project: Omaha source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Project: Omaha source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Project: Omaha source code; if not, see COPYING.txt in the
source tree, or write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "client.h"
#include "cl_videoenc.h"

#ifndef USE_VIDEO_H264

qboolean CL_VideoEnc_Available(void)
{
	return qfalse;
}

qboolean CL_VideoEnc_Active(void)
{
	return qfalse;
}

qboolean CL_VideoEnc_Open(const char *osPath, int width, int height, int frameRate, int sampleRate)
{
	(void)osPath;
	(void)width;
	(void)height;
	(void)frameRate;
	(void)sampleRate;
	return qfalse;
}

void CL_VideoEnc_PushVideo(const byte *bgr, int size)
{
	(void)bgr;
	(void)size;
}

void CL_VideoEnc_PushAudio(const byte *pcm, int size)
{
	(void)pcm;
	(void)size;
}

qboolean CL_VideoEnc_Close(void)
{
	return qfalse;
}

#else /* USE_VIDEO_H264 */

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

/* Added in Omaha: async H.264/AAC MP4 writer for demo export. */

cvar_t *cl_videoPreset;
cvar_t *cl_videoCrf;

namespace {

constexpr int kRingSlots = 8;
constexpr int kAudioRingBytes = 256 * 1024;
constexpr int kAacFrameSamples = 1024;

enum class JobKind : uint8_t {
	Video,
	Audio,
	Eos
};

struct VideoJob {
	JobKind              kind;
	std::vector<uint8_t> data;
	int                  samples; /* audio: sample frames (stereo pairs) */
};

struct EncoderState {
	std::mutex              mutex;
	std::condition_variable cvNotEmpty;
	std::condition_variable cvNotFull;
	std::vector<VideoJob>   queue;
	bool                    running = false;
	bool                    fatal = false;
	std::thread             worker;

	AVFormatContext  *fmt = nullptr;
	AVStream         *vst = nullptr;
	AVStream         *ast = nullptr;
	AVCodecContext   *vctx = nullptr;
	AVCodecContext   *actx = nullptr;
	SwsContext       *sws = nullptr;
	AVFrame          *vframe = nullptr;
	AVFrame          *aframe = nullptr;
	AVPacket         *pkt = nullptr;

	int width = 0;
	int height = 0;
	int stride = 0; /* BGR bytes per row including AVI pad */
	int fps = 25;
	int sampleRate = 44100;
	int64_t nextVideoPts = 0;
	int64_t nextAudioPts = 0;

	/* Accumulated s16 interleaved stereo pending AAC framing. */
	std::vector<int16_t> pcmPending;
};

EncoderState g_enc;

static const char *AvErr(int err)
{
	static char buf[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(err, buf, sizeof(buf));
	return buf;
}

static int WritePacket(AVFormatContext *fmt, AVCodecContext *ctx, AVStream *st, AVPacket *pkt)
{
	av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
	pkt->stream_index = st->index;
	return av_interleaved_write_frame(fmt, pkt);
}

static int EncodeFrame(AVFormatContext *fmt, AVCodecContext *ctx, AVStream *st, AVFrame *frame, AVPacket *pkt)
{
	int ret = avcodec_send_frame(ctx, frame);
	if (ret < 0) {
		return ret;
	}
	for (;;) {
		ret = avcodec_receive_packet(ctx, pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			return 0;
		}
		if (ret < 0) {
			return ret;
		}
		ret = WritePacket(fmt, ctx, st, pkt);
		av_packet_unref(pkt);
		if (ret < 0) {
			return ret;
		}
	}
}

static bool DrainPcmToAac(EncoderState &e)
{
	if (!e.actx || !e.aframe) {
		e.pcmPending.clear();
		return true;
	}

	const int channels = 2;
	while ((int)e.pcmPending.size() >= kAacFrameSamples * channels) {
		float *left = (float *)e.aframe->data[0];
		float *right = (float *)e.aframe->data[1];
		for (int i = 0; i < kAacFrameSamples; i++) {
			left[i] = e.pcmPending[i * 2] / 32768.0f;
			right[i] = e.pcmPending[i * 2 + 1] / 32768.0f;
		}
		e.pcmPending.erase(e.pcmPending.begin(), e.pcmPending.begin() + kAacFrameSamples * channels);

		e.aframe->nb_samples = kAacFrameSamples;
		e.aframe->pts = e.nextAudioPts;
		e.nextAudioPts += kAacFrameSamples;

		const int ret = EncodeFrame(e.fmt, e.actx, e.ast, e.aframe, e.pkt);
		if (ret < 0) {
			Com_Printf(S_COLOR_RED "VideoEnc: AAC encode failed: %s\n", AvErr(ret));
			return false;
		}
	}
	return true;
}

static bool ProcessVideoJob(EncoderState &e, const VideoJob &job)
{
	const uint8_t *src = job.data.data();
	/* Bottom-up BGR24 from the GL1 capture path — flip via negative stride. */
	const uint8_t *srcSlice[4] = {src + (size_t)(e.height - 1) * (size_t)e.stride, nullptr, nullptr, nullptr};
	int            srcStride[4] = {-e.stride, 0, 0, 0};

	sws_scale(e.sws, srcSlice, srcStride, 0, e.height, e.vframe->data, e.vframe->linesize);
	e.vframe->pts = e.nextVideoPts++;

	const int ret = EncodeFrame(e.fmt, e.vctx, e.vst, e.vframe, e.pkt);
	if (ret < 0) {
		Com_Printf(S_COLOR_RED "VideoEnc: H.264 encode failed: %s\n", AvErr(ret));
		return false;
	}
	return true;
}

static bool ProcessAudioJob(EncoderState &e, const VideoJob &job)
{
	const int samples = job.samples;
	if (samples <= 0) {
		return true;
	}
	const int16_t *pcm = (const int16_t *)job.data.data();
	e.pcmPending.insert(e.pcmPending.end(), pcm, pcm + samples * 2);
	return DrainPcmToAac(e);
}

static bool FlushEncoders(EncoderState &e)
{
	if (!DrainPcmToAac(e)) {
		return false;
	}
	/* Flush remaining partial AAC frame with silence. */
	if (e.actx && !e.pcmPending.empty()) {
		const size_t need = (size_t)kAacFrameSamples * 2;
		e.pcmPending.resize(need, 0);
		if (!DrainPcmToAac(e)) {
			return false;
		}
	}

	int ret = EncodeFrame(e.fmt, e.vctx, e.vst, nullptr, e.pkt);
	if (ret < 0) {
		Com_Printf(S_COLOR_RED "VideoEnc: video flush failed: %s\n", AvErr(ret));
		return false;
	}
	if (e.actx) {
		ret = EncodeFrame(e.fmt, e.actx, e.ast, nullptr, e.pkt);
		if (ret < 0) {
			Com_Printf(S_COLOR_RED "VideoEnc: audio flush failed: %s\n", AvErr(ret));
			return false;
		}
	}
	ret = av_write_trailer(e.fmt);
	if (ret < 0) {
		Com_Printf(S_COLOR_RED "VideoEnc: trailer failed: %s\n", AvErr(ret));
		return false;
	}
	return true;
}

static void DestroyEncoderUnlocked(EncoderState &e)
{
	if (e.sws) {
		sws_freeContext(e.sws);
		e.sws = nullptr;
	}
	if (e.vframe) {
		av_frame_free(&e.vframe);
	}
	if (e.aframe) {
		av_frame_free(&e.aframe);
	}
	if (e.pkt) {
		av_packet_free(&e.pkt);
	}
	if (e.vctx) {
		avcodec_free_context(&e.vctx);
	}
	if (e.actx) {
		avcodec_free_context(&e.actx);
	}
	if (e.fmt) {
		if (!(e.fmt->oformat->flags & AVFMT_NOFILE) && e.fmt->pb) {
			avio_closep(&e.fmt->pb);
		}
		avformat_free_context(e.fmt);
		e.fmt = nullptr;
	}
	e.vst = nullptr;
	e.ast = nullptr;
	e.pcmPending.clear();
	e.queue.clear();
}

static void WorkerMain()
{
	EncoderState &e = g_enc;
	for (;;) {
		VideoJob job;
		{
			std::unique_lock<std::mutex> lock(e.mutex);
			e.cvNotEmpty.wait(lock, [&] { return !e.queue.empty() || !e.running; });
			if (e.queue.empty()) {
				break;
			}
			job = std::move(e.queue.front());
			e.queue.erase(e.queue.begin());
			e.cvNotFull.notify_one();
		}

		if (job.kind == JobKind::Eos) {
			if (!FlushEncoders(e)) {
				std::lock_guard<std::mutex> lock(e.mutex);
				e.fatal = true;
			}
			break;
		}

		bool ok = true;
		if (job.kind == JobKind::Video) {
			ok = ProcessVideoJob(e, job);
		} else if (job.kind == JobKind::Audio) {
			ok = ProcessAudioJob(e, job);
		}
		if (!ok) {
			std::lock_guard<std::mutex> lock(e.mutex);
			e.fatal = true;
			break;
		}
	}

	{
		std::lock_guard<std::mutex> lock(e.mutex);
		e.running = false;
		e.cvNotFull.notify_all();
		e.cvNotEmpty.notify_all();
	}
}

static bool PushJob(VideoJob &&job)
{
	EncoderState &e = g_enc;
	std::unique_lock<std::mutex> lock(e.mutex);
	if (!e.running || e.fatal) {
		return false;
	}
	e.cvNotFull.wait(lock, [&] { return e.queue.size() < (size_t)kRingSlots || !e.running || e.fatal; });
	if (!e.running || e.fatal) {
		return false;
	}
	e.queue.push_back(std::move(job));
	e.cvNotEmpty.notify_one();
	return true;
}

static bool OpenCodecs(EncoderState &e, const char *osPath, int width, int height, int frameRate, int sampleRate)
{
	int ret;

	e.width = width & ~1;
	e.height = height & ~1;
	if (e.width < 2 || e.height < 2) {
		Com_Printf(S_COLOR_RED "VideoEnc: resolution too small\n");
		return false;
	}
	e.fps = frameRate > 0 ? frameRate : 25;
	e.sampleRate = sampleRate > 0 ? sampleRate : 44100;
	e.stride = PAD(width * 3, AVI_LINE_PADDING);
	e.nextVideoPts = 0;
	e.nextAudioPts = 0;
	e.pcmPending.clear();
	e.fatal = false;

	ret = avformat_alloc_output_context2(&e.fmt, nullptr, "mp4", osPath);
	if (ret < 0 || !e.fmt) {
		Com_Printf(S_COLOR_RED "VideoEnc: could not create mp4 context: %s\n", AvErr(ret));
		return false;
	}

	const AVCodec *vcodec = avcodec_find_encoder(AV_CODEC_ID_H264);
	if (!vcodec) {
		Com_Printf(S_COLOR_RED "VideoEnc: libx264 encoder not found in this build\n");
		return false;
	}
	e.vst = avformat_new_stream(e.fmt, nullptr);
	if (!e.vst) {
		return false;
	}
	e.vctx = avcodec_alloc_context3(vcodec);
	if (!e.vctx) {
		return false;
	}
	e.vctx->codec_id = AV_CODEC_ID_H264;
	e.vctx->width = e.width;
	e.vctx->height = e.height;
	e.vctx->time_base = AVRational{1, e.fps};
	e.vctx->framerate = AVRational{e.fps, 1};
	e.vctx->pix_fmt = AV_PIX_FMT_YUV420P;
	e.vctx->gop_size = e.fps * 2;
	e.vctx->max_b_frames = 2;
	if (e.fmt->oformat->flags & AVFMT_GLOBALHEADER) {
		e.vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	AVDictionary *opts = nullptr;
	const char   *preset = (cl_videoPreset && cl_videoPreset->string[0]) ? cl_videoPreset->string : "veryfast";
	const char   *crf = (cl_videoCrf && cl_videoCrf->string[0]) ? cl_videoCrf->string : "15";
	av_dict_set(&opts, "preset", preset, 0);
	av_dict_set(&opts, "crf", crf, 0);
	av_dict_set(&opts, "threads", "0", 0);
	av_dict_set(&opts, "profile", "high", 0);
	ret = avcodec_open2(e.vctx, vcodec, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		Com_Printf(S_COLOR_RED "VideoEnc: avcodec_open2(h264) failed: %s\n", AvErr(ret));
		return false;
	}
	ret = avcodec_parameters_from_context(e.vst->codecpar, e.vctx);
	if (ret < 0) {
		return false;
	}
	e.vst->time_base = e.vctx->time_base;

	e.sws = sws_getContext(
		e.width,
		e.height,
		AV_PIX_FMT_BGR24,
		e.width,
		e.height,
		AV_PIX_FMT_YUV420P,
		SWS_BILINEAR,
		nullptr,
		nullptr,
		nullptr
	);
	if (!e.sws) {
		Com_Printf(S_COLOR_RED "VideoEnc: sws_getContext failed\n");
		return false;
	}

	e.vframe = av_frame_alloc();
	if (!e.vframe) {
		return false;
	}
	e.vframe->format = AV_PIX_FMT_YUV420P;
	e.vframe->width = e.width;
	e.vframe->height = e.height;
	ret = av_frame_get_buffer(e.vframe, 32);
	if (ret < 0) {
		return false;
	}

	/* Optional AAC audio — open codec before adding the muxer stream. */
	const AVCodec *acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (acodec && sampleRate > 0) {
		AVCodecContext *actx = avcodec_alloc_context3(acodec);
		if (!actx) {
			return false;
		}
		actx->sample_rate = e.sampleRate;
		actx->bit_rate = 192000;
		ret = av_channel_layout_from_mask(&actx->ch_layout, AV_CH_LAYOUT_STEREO);
		if (ret < 0) {
			avcodec_free_context(&actx);
			return false;
		}
		actx->sample_fmt = AV_SAMPLE_FMT_FLTP;
		actx->time_base = AVRational{1, e.sampleRate};
		if (e.fmt->oformat->flags & AVFMT_GLOBALHEADER) {
			actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}
		ret = avcodec_open2(actx, acodec, nullptr);
		if (ret < 0) {
			Com_Printf(S_COLOR_YELLOW "VideoEnc: AAC open failed (%s), writing video only\n", AvErr(ret));
			avcodec_free_context(&actx);
		} else {
			e.ast = avformat_new_stream(e.fmt, nullptr);
			if (!e.ast) {
				avcodec_free_context(&actx);
				return false;
			}
			e.actx = actx;
			ret = avcodec_parameters_from_context(e.ast->codecpar, e.actx);
			if (ret < 0) {
				return false;
			}
			e.ast->time_base = e.actx->time_base;

			e.aframe = av_frame_alloc();
			if (!e.aframe) {
				return false;
			}
			e.aframe->format = AV_SAMPLE_FMT_FLTP;
			e.aframe->nb_samples = kAacFrameSamples;
			e.aframe->sample_rate = e.sampleRate;
			ret = av_channel_layout_copy(&e.aframe->ch_layout, &e.actx->ch_layout);
			if (ret < 0) {
				return false;
			}
			ret = av_frame_get_buffer(e.aframe, 0);
			if (ret < 0) {
				return false;
			}
		}
	} else if (sampleRate > 0) {
		Com_Printf(S_COLOR_YELLOW "VideoEnc: AAC encoder unavailable, writing video only\n");
	}

	e.pkt = av_packet_alloc();
	if (!e.pkt) {
		return false;
	}

	if (!(e.fmt->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&e.fmt->pb, osPath, AVIO_FLAG_WRITE);
		if (ret < 0) {
			Com_Printf(S_COLOR_RED "VideoEnc: could not open %s: %s\n", osPath, AvErr(ret));
			return false;
		}
	}

	AVDictionary *muxOpts = nullptr;
	av_dict_set(&muxOpts, "movflags", "+faststart", 0);
	ret = avformat_write_header(e.fmt, &muxOpts);
	av_dict_free(&muxOpts);
	if (ret < 0) {
		Com_Printf(S_COLOR_RED "VideoEnc: write_header failed: %s\n", AvErr(ret));
		return false;
	}

	return true;
}

} // namespace

qboolean CL_VideoEnc_Available(void)
{
	return qtrue;
}

qboolean CL_VideoEnc_Active(void)
{
	std::lock_guard<std::mutex> lock(g_enc.mutex);
	return g_enc.running ? qtrue : qfalse;
}

qboolean CL_VideoEnc_Open(const char *osPath, int width, int height, int frameRate, int sampleRate)
{
	if (!osPath || !osPath[0]) {
		return qfalse;
	}
	if (CL_VideoEnc_Active()) {
		Com_Printf(S_COLOR_YELLOW "VideoEnc: already recording\n");
		return qfalse;
	}

	if (!cl_videoPreset) {
		cl_videoPreset = Cvar_Get("cl_videoPreset", "veryfast", CVAR_ARCHIVE);
	}
	if (!cl_videoCrf) {
		cl_videoCrf = Cvar_Get("cl_videoCrf", "15", CVAR_ARCHIVE);
	}

	EncoderState &e = g_enc;
	{
		std::lock_guard<std::mutex> lock(e.mutex);
		DestroyEncoderUnlocked(e);
	}

	if (!OpenCodecs(e, osPath, width, height, frameRate, sampleRate)) {
		DestroyEncoderUnlocked(e);
		return qfalse;
	}

	{
		std::lock_guard<std::mutex> lock(e.mutex);
		e.running = true;
		e.fatal = false;
		e.queue.clear();
	}
	e.worker = std::thread(WorkerMain);

	Com_Printf(
		"VideoEnc: recording %dx%d @ %d fps (libx264 crf %s preset %s) -> %s\n",
		e.width,
		e.height,
		e.fps,
		cl_videoCrf->string,
		cl_videoPreset->string,
		osPath
	);
	return qtrue;
}

void CL_VideoEnc_PushVideo(const byte *bgr, int size)
{
	if (!bgr || size <= 0) {
		return;
	}
	EncoderState &e = g_enc;

	/* Capture may be full GL size; encoder uses even height/width with matching stride. */
	const int copyBytes = e.stride * e.height;
	if (size < copyBytes) {
		Com_Printf(S_COLOR_YELLOW "VideoEnc: short video frame (%d < %d)\n", size, copyBytes);
		return;
	}

	VideoJob job;
	job.kind = JobKind::Video;
	job.samples = 0;
	job.data.assign(bgr, bgr + copyBytes);
	PushJob(std::move(job));
}

void CL_VideoEnc_PushAudio(const byte *pcm, int size)
{
	if (!pcm || size < 4) {
		return;
	}
	if (!g_enc.actx) {
		return;
	}
	/* Cap burst to avoid unbounded queue growth if encode stalls. */
	int use = size;
	if (use > kAudioRingBytes) {
		use = kAudioRingBytes;
	}
	use &= ~3; /* whole stereo s16 frames */

	VideoJob job;
	job.kind = JobKind::Audio;
	job.samples = use / 4;
	job.data.assign(pcm, pcm + use);
	PushJob(std::move(job));
}

qboolean CL_VideoEnc_Close(void)
{
	EncoderState &e = g_enc;
	bool          wasRunning = false;
	{
		std::lock_guard<std::mutex> lock(e.mutex);
		wasRunning = e.running;
		if (wasRunning) {
			VideoJob eos;
			eos.kind = JobKind::Eos;
			eos.samples = 0;
			e.queue.push_back(std::move(eos));
			e.cvNotEmpty.notify_one();
		}
	}

	if (e.worker.joinable()) {
		e.worker.join();
	}

	const bool fatal = e.fatal;
	DestroyEncoderUnlocked(e);
	e.running = false;

	if (!wasRunning) {
		return qfalse;
	}
	if (fatal) {
		Com_Printf(S_COLOR_RED "VideoEnc: finished with errors\n");
		return qfalse;
	}
	Com_Printf("VideoEnc: closed\n");
	return qtrue;
}

#endif /* USE_VIDEO_H264 */
