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

#pragma once

#include "../../qcommon/q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Omaha voice renderer interface (AL-shaped; implemented by snd_render_omaha.cpp). */

typedef int sndVoiceId_t;
typedef int sndBufferId_t;

#define SND_VOICE_NONE  (-1)
#define SND_BUFFER_NONE (-1)

typedef enum {
	SND_STATE_INITIAL = 0,
	SND_STATE_PLAYING = 1,
	SND_STATE_PAUSED  = 2,
	SND_STATE_STOPPED = 3
} sndVoiceState_t;

typedef enum {
	SND_PARAM_POSITION = 0,
	SND_PARAM_VELOCITY,
	SND_PARAM_SOURCE_RELATIVE,
	SND_PARAM_LOOPING,
	SND_PARAM_ROLLOFF_FACTOR,
	SND_PARAM_GAIN,
	SND_PARAM_MAX_GAIN,
	SND_PARAM_REFERENCE_DISTANCE,
	SND_PARAM_MAX_DISTANCE,
	SND_PARAM_DISTANCE_MODEL,
	SND_PARAM_PITCH,
	SND_PARAM_DIRECT_CHANNELS,
	SND_PARAM_RESAMPLER,
	/* Added in Omaha: 0 = clear line of sight, 1 = fully blocked. */
	SND_PARAM_OBSTRUCTION,
	/* Added in Omaha: SND_ROUTE_* for omaha headphone HRTF (ignored otherwise). */
	SND_PARAM_SPATIAL_ROUTE
} sndParam_t;

/* Spatial route: ambisonic bus, per-voice HRTF, or dry stereo (no HRTF). */
#define SND_ROUTE_BUS    0
#define SND_ROUTE_DIRECT 1
#define SND_ROUTE_DRY    2

/* Added in Omaha: bundled HRTF set for omaha headphones (s_hrtf_file default). */
#define SND_HRTF_DEFAULT_FILE "sound/prom/hrtf/SADIE_KU100_DFC_256_order_fir_48000.sofa"

/* Distance model values (match AL enums used by Omaha). */
#define SND_DISTANCE_INVERSE_CLAMPED 0xD002
#define SND_DISTANCE_LINEAR_CLAMPED  0xD004

/* Direct-channels: 0 = spatialize, 1 = remix unmatched (CHAN_LOCAL). */
#define SND_DIRECT_CHANNELS_OFF   0
#define SND_DIRECT_CHANNELS_REMIX 1

typedef enum {
	SND_FMT_MONO8 = 1,
	SND_FMT_MONO16,
	SND_FMT_STEREO8,
	SND_FMT_STEREO16,
	SND_FMT_MONO_IMA4,
	SND_FMT_MONO_MSADPCM,
	SND_FMT_STEREO_IMA4
} sndSampleFormat_t;

typedef struct {
	int         sampleRate;
	const char *deviceName;
	int         speakerType; /* 0 stereo, 1 headphones (HRTF); surround is left to the OpenAL host */
	qboolean    wantCapture;
} sndDeviceConfig_t;

struct sfx_s;

typedef struct sndRenderer_s {
	const char *name;

	qboolean (*Init)(const sndDeviceConfig_t *cfg);
	void (*Shutdown)(void);

	sndBufferId_t (*CreateBufferFromSfx)(struct sfx_s *sfx);
	void (*DestroyBuffer)(sndBufferId_t id);
	/* blockAlign: ADPCM samples per block (AL_UNPACK_BLOCK_ALIGNMENT_SOFT); 0/1 for PCM. */
	qboolean (*UploadBuffer)(
		sndBufferId_t id, sndSampleFormat_t fmt, const void *data, int sizeBytes, int rate, int blockAlign
	);
	int (*BufferFrequency)(sndBufferId_t id);

	sndVoiceId_t (*CreateVoice)(void);
	void (*DestroyVoice)(sndVoiceId_t v);
	void (*SetBuffer)(sndVoiceId_t v, sndBufferId_t b);
	void (*ClearBuffer)(sndVoiceId_t v);
	void (*SetParamf)(sndVoiceId_t v, sndParam_t p, float x);
	void (*SetParam3f)(sndVoiceId_t v, sndParam_t p, float x, float y, float z);
	void (*SetParami)(sndVoiceId_t v, sndParam_t p, int x);
	float (*GetParamf)(sndVoiceId_t v, sndParam_t p);
	void (*Play)(sndVoiceId_t v);
	void (*Pause)(sndVoiceId_t v);
	void (*Stop)(sndVoiceId_t v);
	int (*GetState)(sndVoiceId_t v);
	unsigned (*GetSampleOffset)(sndVoiceId_t v);
	void (*SetSampleOffset)(sndVoiceId_t v, unsigned off);
	/* Partial loop region in sample frames; endSample < 0 means buffer end. Added in Omaha. */
	void (*SetLoopPoints)(sndVoiceId_t v, int startSample, int endSample);

	void (*QueueStreamBuffer)(
		sndVoiceId_t v, const void *pcm, int bytes, sndSampleFormat_t fmt, int rate, int blockAlign
	);
	int (*StreamBuffersQueued)(sndVoiceId_t v);
	int (*StreamBuffersProcessed)(sndVoiceId_t v);
	void (*UnqueueProcessed)(sndVoiceId_t v, int count);

	void (*SetListener)(const float pos[3], const float vel[3], const float at[3], const float up[3]);
	void (*SetListenerGain)(float g);
	void (*GetListenerPosition)(float out[3]);

	qboolean (*BeginCapture)(int sampleRate);
	int (*RenderCapture)(float *interleavedStereo, int frames);
	void (*EndCapture)(void);
	qboolean (*CaptureActive)(void);
	int (*CaptureSampleRate)(void);

	void (*Update)(void);
} sndRenderer_t;

extern const sndRenderer_t *g_sndRenderer;

const sndRenderer_t *S_Renderer_Omaha(void);

/* Init / shut down the Omaha mixer as g_sndRenderer. */
qboolean S_Renderer_Init(const sndDeviceConfig_t *cfg);
void     S_Renderer_Shutdown(void);

/* HF duck HUD meter reads (gain <= 0, sidechain env dB, thresh). */
float S_Omaha_HfDuckGainDb(void);
float S_Omaha_HfDuckMaxCutDb(void);
float S_Omaha_HfDuckSidechainDb(void);
float S_Omaha_HfDuckThreshDb(void);

/* Mixed PCM for AVI / MP4 export (sndHost_t capture entries). */
qboolean S_OMAHA_CaptureBegin(void);
void     S_OMAHA_CaptureEnd(void);
void     S_OMAHA_CaptureFrame(int aviFps);
qboolean S_OMAHA_CaptureActive(void);
int      S_OMAHA_CaptureSampleRate(void);

#ifdef __cplusplus
}
#endif
