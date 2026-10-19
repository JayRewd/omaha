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

/*
 * Omaha mixer voice API. One sound is immutable PCM. One voice is a playing
 * instance. The game thread sends a whole omVoiceParams_t when something
 * changed; it does not poke individual properties.
 */

typedef int omSoundId_t;
typedef int omVoiceId_t;

#define OM_SOUND_NONE (-1)
#define OM_VOICE_NONE (-1)

#define OM_VOICE_STOPPED 0
#define OM_VOICE_PLAYING 1
#define OM_VOICE_PAUSED  2

#define OM_FLAG_2D   1 /* no distance attenuation */
#define OM_FLAG_LOOP 2
#define OM_FLAG_FLAT 4 /* speakers get the channels directly; no position */
#define OM_FLAG_PAN  8 /* cur.pan is the stereo image, -1 left .. +1 right */

#define OM_ROUTE_BUS    0
#define OM_ROUTE_DIRECT 1
#define OM_ROUTE_DRY    2

typedef enum {
	OM_FMT_MONO8 = 1,
	OM_FMT_MONO16,
	OM_FMT_STEREO8,
	OM_FMT_STEREO16,
	OM_FMT_MONO_F32,
	OM_FMT_STEREO_F32,
	OM_FMT_MONO_IMA4,
	OM_FMT_STEREO_IMA4,
	OM_FMT_MONO_MSADPCM,
	OM_FMT_STEREO_MSADPCM
} omSampleFormat_t;

typedef struct omVoiceParams_s {
	float pos[3];
	float gain;
	float pitch;
	float minDist;
	float maxDist;
	float obstruction; /* 0 clear, 1 blocked */
	float pan;         /* used when OM_FLAG_PAN */
	int   flags;
	int   route;
	int   loopStart; /* source frames */
	int   loopEnd;   /* < 0 means the end of the sound */
} omVoiceParams_t;

typedef struct {
	int         sampleRate;
	const char *deviceName;
	int         speakerType; /* 0 stereo, 1 headphones, 2 5.1, 3 quad */
	qboolean    wantCapture;
} sndDeviceConfig_t;

/* Bundled HRTF set for headphones. */
#define SND_HRTF_DEFAULT_FILE "sound/prom/hrtf/SADIE_KU100_DFC_256_order_fir_48000.sofa"

typedef struct sndRenderer_s {
	const char *name;

	qboolean (*Init)(const sndDeviceConfig_t *cfg);
	void (*Shutdown)(void);

	omSoundId_t (*CreateSound)(void);
	void (*DestroySound)(omSoundId_t id);
	/* blockAlign: ADPCM samples per block; 0 or 1 selects the usual 65. */
	qboolean (*UploadSound)(
		omSoundId_t id, omSampleFormat_t fmt, const void *data, int sizeBytes, int rate, int blockAlign
	);
	int (*SoundRate)(omSoundId_t id);

	omVoiceId_t (*PlayVoice)(omSoundId_t sound, const omVoiceParams_t *params, unsigned offset);
	omVoiceId_t (*OpenStream)(const omVoiceParams_t *params);
	void (*UpdateVoice)(omVoiceId_t id, const omVoiceParams_t *params);
	void (*StopVoice)(omVoiceId_t id);
	void (*PauseVoice)(omVoiceId_t id);
	void (*ResumeVoice)(omVoiceId_t id);
	void (*DestroyVoice)(omVoiceId_t id);
	int (*VoiceState)(omVoiceId_t id);
	unsigned (*VoiceOffset)(omVoiceId_t id);
	void (*SetVoiceOffset)(omVoiceId_t id, unsigned offset);

	void (*QueueStream)(omVoiceId_t id, const void *pcm, int bytes, omSampleFormat_t fmt, int rate, int blockAlign);
	int (*StreamQueued)(omVoiceId_t id);
	int (*StreamProcessed)(omVoiceId_t id);
	void (*UnqueueStream)(omVoiceId_t id, int count);

	/* forward / left / up are world axes. There is no listener velocity. */
	void (*SetListener)(const float pos[3], const float forward[3], const float left[3], const float up[3]);
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

qboolean S_Renderer_Init(const sndDeviceConfig_t *cfg);
void     S_Renderer_Shutdown(void);

float S_Omaha_HfDuckGainDb(void);
float S_Omaha_HfDuckMaxCutDb(void);
float S_Omaha_HfDuckSidechainDb(void);
float S_Omaha_HfDuckThreshDb(void);

qboolean S_OMAHA_CaptureBegin(void);
void     S_OMAHA_CaptureEnd(void);
void     S_OMAHA_CaptureFrame(int aviFps);
qboolean S_OMAHA_CaptureActive(void);
int      S_OMAHA_CaptureSampleRate(void);

#ifdef __cplusplus
}
#endif
