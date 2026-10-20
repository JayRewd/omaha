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

/*
 * Omaha mixer. Device I/O is miniaudio. Voices resample with an 8-point
 * Blackman sinc, pan with pairwise speaker gains (stereo, quad, 5.1) or the
 * headphone HRTF, then a shared limiter, EQ and HF duck.
 *
 * The game thread enqueues commands and grows the id tables under s_apiMutex.
 * The device callback and capture pull are the only threads that mix, and they
 * take s_mixMutex so they cannot overlap.
 */

#include "../snd_local.h"
#include "snd_adpcm_omaha.h"
#include "snd_hrtf_omaha.h"
#include "snd_renderer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <new>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
#	include <immintrin.h>
#	define OM_HAVE_SSE2 1
#else
#	define OM_HAVE_SSE2 0
#endif

#include "miniaudio.h"

#ifndef M_PI
#	define M_PI 3.14159265358979323846
#endif

#define OM_GUARD       4
#define OM_FRAC_ONE    65536u
#define OM_LINE        1024
#define OM_RAMP        128
#define OM_OUT_MAX     6
#define OM_GAIN_MAX    16.0f
#define OM_SILENCE     0.00001f
#define OM_DIRECT_CAP  12
#define OM_HRTF_MIN_DIST 1.0f

#define OM_LIMITER_LOOKAHEAD_MS 1.5f
#define OM_LIMITER_HOLD_MS      2.0f
#define OM_LIMITER_CEILING_DB   (-0.2f)
#define OM_LIMITER_PRE_TRIM_DB  (-8.7f)
#define OM_LIMITER_THRESHOLD_DB (-3.6f)
#define OM_LIMITER_THRESHOLD_MIN_DB (-12.0f)
#define OM_LIMITER_THRESHOLD_MAX_DB 0.0f
#define OM_LIMITER_RELEASE_MS   20.0f
#define OM_LIMITER_MAX_DELAY    512

#define OM_SPK_EQ_BANDS 3
#define OM_HP_EQ_BANDS  4
#define OM_HP_EQ_TYPE_PEAK   0
#define OM_HP_EQ_TYPE_HSHELF 1

#define OM_HF_DUCK_HP_SECTIONS     4
#define OM_HF_DUCK_SHELF_STAGES    4
#define OM_HF_DUCK_FC_HZ           15500.0f
#define OM_HF_DUCK_SC_BOOST_MAX_DB 24.0f
#define OM_HF_DUCK_THRESH_DEFAULT  (-15.0f)
#define OM_HF_DUCK_RELEASE_MS      300.0f
#define OM_HF_DUCK_SHELF_S         1.0f
#define OM_HF_DUCK_DEFAULT_DB      24.0f
#define OM_HF_DUCK_SC_BOOST_DEFAULT 5.0f
#define OM_HF_DUCK_REDESIGN_DB     0.25f
#define OM_HF_DUCK_BYPASS_DB       0.05f

#define OM_OBSTRUCT_CUTOFF_HZ      7000.0f
#define OM_OBSTRUCT_OPEN_HZ        20000.0f
#define OM_OBSTRUCT_OCC_BLOCKED_HZ 10000.0f
#define OM_OBSTRUCT_SMOOTH_MS      80.0f
#define OM_OBSTRUCT_EPSILON        0.01f
#define OM_OBSTRUCT_DIST_RANGE     3000.0f
#define OM_OBSTRUCT_MIX            0.5f
#define OM_OBSTRUCT_MAKEUP_MAX     2.0f
#define OM_OCCLUDE_CUTOFF_HZ       1800.0f
#define OM_OCCLUDE_GAIN            0.35f

#define OM_REAR_GAIN      0.75f
#define OM_REAR_SHELF_HZ  3000.0f
#define OM_REAR_SHELF_HF  0.5f
#define OM_REAR_SMOOTH_MS 50.0f

/* Added in Omaha: id tables start empty and grow by this many slots. A block is never moved. */
#define OM_BLOCK         32
#define OM_CHUNK_BLOCKS  64
#define OM_ID_NONE       (-1)
#define OM_MAX_STEP      8
#define OM_SINC_TAPS   8
#define OM_SINC_PHASES 256
#define OM_GATHER      (OM_LINE * OM_MAX_STEP + 16)

enum {
	OM_OP_PLAY = 1,
	OM_OP_OPEN,
	OM_OP_UPDATE,
	OM_OP_STOP,
	OM_OP_PAUSE,
	OM_OP_RESUME,
	OM_OP_DESTROY,
	OM_OP_SEEK,
	OM_OP_QUEUE,
	OM_OP_UNQUEUE,
	OM_OP_LISTENER,
	OM_OP_MASTER,
	OM_OP_FREE_SOUND,
	OM_OP_RETIRE_SOUND
};

/* Immutable once published. Only the game thread deletes one, after the mixer let go. */
struct omSound_t {
	int                channels;
	int                frames;
	int                rate;
	std::vector<float> planar[2];
};

struct omChunk_t {
	int                channels;
	int                frames;
	int                rate;
	std::vector<float> planar[2];
};

/* Mixer thread only. */
struct omVoice_t {
	qboolean        used;
	int             state;
	int             handle; /* -1 once the voice no longer reports to a handle */
	int             sound;
	qboolean        isStream;
	qboolean        primed;
	qboolean        ending; /* input is over or stopped: fade, ring out, free */
	qboolean        allowDirect;
	int             tail; /* HRTF ring-out samples left */
	omVoiceParams_t params;
	int             pos;
	unsigned        frac;
	float           dist;
	float           distScale;
	float           dir[3];
	float           spk[2][OM_OUT_MAX];
	float           ambi[OM_AMBI_CHANNELS];
	float           direct;
	float           rear;
	float           rearZ;
	float           lpZ[2];
	float           obstruct;
	float           occlude;
	omHrtfVoice_t  *hrtf;
	std::deque<omChunk_t *> chunks;
	int             self;
	int             nextFree;
};

/* used / queued / pendingUnqueue belong to the game thread; voice to the mixer. */
struct omHandle_t {
	std::atomic<int>      used;
	qboolean              stream;
	int                   voice;
	qboolean              dead; /* mixer: freed, waiting to go back on the free list */
	std::atomic<int>      state;
	std::atomic<unsigned> offset;
	std::atomic<int>      queued;
	std::atomic<int>      processed;
	std::atomic<int>      pendingUnqueue;
	int                   nextFree;
};

struct omSoundSlot_t {
	std::atomic<omSound_t *> sound;
	int                      nextFree;
};

struct omSoundBlock_t {
	omSoundSlot_t slot[OM_BLOCK];
};

struct omHandleBlock_t {
	omHandle_t slot[OM_BLOCK];
};

struct omVoiceBlock_t {
	omVoice_t slot[OM_BLOCK];
};

struct omSoundChunk_t {
	omSoundBlock_t *block[OM_CHUNK_BLOCKS];
	omSoundChunk_t *next;
};

struct omHandleChunk_t {
	omHandleBlock_t *block[OM_CHUNK_BLOCKS];
	omHandleChunk_t *next;
};

struct omVoiceChunk_t {
	omVoiceBlock_t *block[OM_CHUNK_BLOCKS];
	omVoiceChunk_t *next;
};

struct omRankRetired_t {
	int            *rank;
	float          *gain;
	omRankRetired_t *next;
};

struct omCmd_t {
	int             op;
	int             id;
	int             sound;
	int             slot; /* voice reserved on the game thread; PLAY / OPEN only */
	unsigned        offset;
	omVoiceParams_t params;
	omChunk_t      *chunk;
	omSound_t      *retire;
	float           pos[3];
	float           vel[3];
	float           fwd[3];
	float           left[3];
	float           up[3];
	float           gain;
};

struct omEq_t {
	float b0, b1, b2, a1, a2;
	float z1[OM_OUT_MAX], z2[OM_OUT_MAX];
};

struct omHp_t {
	float b0, b1, b2, a1, a2;
	float z1, z2;
};

struct omShelf_t {
	float b0, b1, b2, a1, a2;
	float z1[OM_OUT_MAX], z2[OM_OUT_MAX];
};

static omSoundChunk_t  *s_soundChunks;
static std::atomic<int>  s_soundCap{0};
static int               s_soundFreeHead = OM_ID_NONE;
static int               s_soundRecycleHead = OM_ID_NONE;
static std::vector<omSound_t *> s_soundTrash;
static omHandleChunk_t *s_handleChunks;
static std::atomic<int>  s_handleCap{0};
static int               s_handleFreeHead = OM_ID_NONE;
static int               s_handleRecycleHead = OM_ID_NONE;
static omVoiceChunk_t  *s_voiceChunks;
static std::atomic<int>  s_voiceCap{0};
static int               s_voiceFreeHead = OM_ID_NONE;
static int               s_voiceRecycleHead = OM_ID_NONE;
static std::vector<omCmd_t>     s_cmds;
static std::vector<omCmd_t>     s_drain;
static std::vector<omSound_t *> s_drainTrash;
static std::mutex               s_apiMutex;
static std::mutex               s_mixMutex;

static ma_context s_context;
static qboolean   s_contextReady;
static ma_device  s_device;
static qboolean   s_deviceReady;
static qboolean   s_nullDevice;
static ma_channel s_map[OM_OUT_MAX];

static int      s_sampleRate = 44100;
static int      s_outCh      = 2;
static qboolean s_hrtfOn;
static int      s_layout; /* 0 stereo, 1 hrtf, 2 5.1, 3 quad */
static qboolean s_captureActive;

static float s_lisPos[3];
static float s_lisVel[3];
static float s_lisFwd[3]  = {1, 0, 0};
static float s_lisLeft[3] = {0, 1, 0};
static float s_lisUp[3]   = {0, 0, 1};
static float s_lisGain    = 1.f;
static float s_masterCur  = 1.f;
static qboolean s_masterPrimed;

static float s_bus[OM_OUT_MAX][OM_LINE];
static float s_ambi[OM_AMBI_CHANNELS][OM_LINE];
static float s_dry[2][OM_LINE];
static float s_tmp[OM_LINE];
static float s_gather[OM_GATHER];
static int   s_ambiFlush;
static float s_stash[OM_HRTF_BLOCK * OM_OUT_MAX];
static int   s_stashPos;
static int   s_stashLen;
static std::atomic<int *>   s_rank{NULL};
static std::atomic<float *> s_rankG{NULL};
static omRankRetired_t     *s_rankRetired;

static float s_spkAz[OM_OUT_MAX];
static int   s_spkCh[OM_OUT_MAX];
static int   s_spkN;
static int   s_frontL;
static int   s_frontR;

static cvar_t *s_period_ms;
static cvar_t *s_limiter;
static cvar_t *s_limiter_threshold;
static cvar_t *s_obstruction;
static cvar_t *s_hf_duck;
static cvar_t *s_hf_duck_db;
static cvar_t *s_hf_duck_thresh;
static cvar_t *s_hf_duck_sc_boost;
static cvar_t *s_mix;

static float s_limDelay[OM_LIMITER_MAX_DELAY][OM_OUT_MAX];
static float s_limPeak[OM_LIMITER_MAX_DELAY];
static int   s_limDelayLen, s_limWrite, s_limFilled, s_limHoldLen, s_limHoldLeft;
static float s_limGain = 1.f, s_limReleaseCoeff = 1.f;
static float s_limPreTrim = 1.f, s_limThresh = 1.f, s_limMakeup = 1.f, s_limCeiling = 1.f;
static int   s_limCalibMod = -1;

static omEq_t    s_spkEq[OM_SPK_EQ_BANDS];
static qboolean  s_spkEqReady;
static omEq_t    s_hpEq[OM_HP_EQ_BANDS];
static qboolean  s_hpEqReady;
static omHp_t    s_hfHp[OM_HF_DUCK_HP_SECTIONS];
static omShelf_t s_hfShelf[OM_HF_DUCK_SHELF_STAGES];
static float     s_hfEnv, s_hfGainDb, s_hfMeterDb, s_hfSideDb, s_hfRelease, s_hfBoost;
static qboolean  s_hfReady, s_hfShelfOn;
static float     s_rearA = 1.f;
static float     s_rearStep = 1.f; /* 50 ms rear fade for the current mix line */
static float     s_sinc[OM_SINC_PHASES][OM_SINC_TAPS];
static float     s_pairAmbi[2][OM_AMBI_CHANNELS];

struct OmFtzGuard {
	unsigned old;
	bool     on;
	OmFtzGuard() : old(0), on(false)
	{
#if OM_HAVE_SSE2
		old = _mm_getcsr();
		_mm_setcsr(old | 0x8040);
		on = true;
#elif defined(__aarch64__)
		unsigned long fpcr;
		__asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
		old = (unsigned)fpcr;
		__asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ul << 24)));
		on = true;
#endif
	}
	~OmFtzGuard()
	{
#if OM_HAVE_SSE2
		if (on) {
			_mm_setcsr(old);
		}
#elif defined(__aarch64__)
		if (on) {
			__asm__ __volatile__("msr fpcr, %0" : : "r"((unsigned long)old));
		}
#endif
	}
};

static float Om_Clamp(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float Om_Lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}

static void Om_DefaultParams(omVoiceParams_t *p)
{
	memset(p, 0, sizeof(*p));
	p->pitch    = 1.f;
	p->minDist  = 200.f;
	p->maxDist  = 12800.f;
	p->loopEnd  = -1;
	p->route    = OM_ROUTE_BUS;
}

static void Om_SetupSpeakers(void)
{
	const float d30  = (float)(30.0 * M_PI / 180.0);
	const float d45  = (float)(45.0 * M_PI / 180.0);
	const float d110 = (float)(110.0 * M_PI / 180.0);
	const float d135 = (float)(135.0 * M_PI / 180.0);
	s_frontL = 0;
	s_frontR = 1;
	if (s_layout == 3) {
		s_outCh = 4;
		s_spkN  = 4;
		s_spkAz[0] = -d135; s_spkCh[0] = 3;
		s_spkAz[1] = -d45;  s_spkCh[1] = 1;
		s_spkAz[2] = d45;   s_spkCh[2] = 0;
		s_spkAz[3] = d135;  s_spkCh[3] = 2;
		s_map[0] = MA_CHANNEL_FRONT_LEFT;
		s_map[1] = MA_CHANNEL_FRONT_RIGHT;
		s_map[2] = MA_CHANNEL_BACK_LEFT;
		s_map[3] = MA_CHANNEL_BACK_RIGHT;
	} else if (s_layout == 2) {
		s_outCh = 6;
		s_spkN  = 5;
		s_spkAz[0] = -d110; s_spkCh[0] = 5;
		s_spkAz[1] = -d30;  s_spkCh[1] = 1;
		s_spkAz[2] = 0.f;   s_spkCh[2] = 2;
		s_spkAz[3] = d30;   s_spkCh[3] = 0;
		s_spkAz[4] = d110;  s_spkCh[4] = 4;
		s_map[0] = MA_CHANNEL_FRONT_LEFT;
		s_map[1] = MA_CHANNEL_FRONT_RIGHT;
		s_map[2] = MA_CHANNEL_FRONT_CENTER;
		s_map[3] = MA_CHANNEL_LFE;
		s_map[4] = MA_CHANNEL_SIDE_LEFT;
		s_map[5] = MA_CHANNEL_SIDE_RIGHT;
	} else {
		s_outCh = 2;
		s_spkN  = 2;
		s_spkAz[0] = -d30; s_spkCh[0] = 1;
		s_spkAz[1] = d30;  s_spkCh[1] = 0;
		s_map[0] = MA_CHANNEL_FRONT_LEFT;
		s_map[1] = MA_CHANNEL_FRONT_RIGHT;
	}
}

static void Om_LimiterReset(void)
{
	memset(s_limDelay, 0, sizeof(s_limDelay));
	memset(s_limPeak, 0, sizeof(s_limPeak));
	s_limWrite    = 0;
	s_limFilled   = 0;
	s_limGain     = 1.f;
	s_limHoldLeft = 0;
	s_limDelayLen = (int)(OM_LIMITER_LOOKAHEAD_MS * 0.001f * (float)s_sampleRate + 0.5f);
	if (s_limDelayLen < 1) {
		s_limDelayLen = 1;
	}
	if (s_limDelayLen > OM_LIMITER_MAX_DELAY) {
		s_limDelayLen = OM_LIMITER_MAX_DELAY;
	}
	s_limHoldLen = (int)(OM_LIMITER_HOLD_MS * 0.001f * (float)s_sampleRate + 0.5f);
	if (s_limHoldLen < 1) {
		s_limHoldLen = 1;
	}
	{
		const float rel = OM_LIMITER_RELEASE_MS * 0.001f * (float)s_sampleRate;
		s_limReleaseCoeff = rel > 1.f ? (1.f - expf(-1.f / rel)) : 1.f;
	}
}

static void Om_LimiterCalib(void)
{
	int   mod = s_limiter_threshold ? s_limiter_threshold->modificationCount : 0;
	float db;
	if (mod == s_limCalibMod && s_limCalibMod >= 0) {
		return;
	}
	s_limCalibMod = mod;
	db            = s_limiter_threshold ? s_limiter_threshold->value : OM_LIMITER_THRESHOLD_DB;
	db            = Om_Clamp(db, OM_LIMITER_THRESHOLD_MIN_DB, OM_LIMITER_THRESHOLD_MAX_DB);
	s_limPreTrim  = powf(10.f, OM_LIMITER_PRE_TRIM_DB * 0.05f);
	s_limThresh   = powf(10.f, (OM_LIMITER_CEILING_DB + db) * 0.05f);
	s_limMakeup   = db >= 0.f ? 1.f : powf(10.f, -db * 0.05f);
	s_limCeiling  = powf(10.f, OM_LIMITER_CEILING_DB * 0.05f);
}

static float Om_WindowPeak(void)
{
	float m = 0.f;
	int   i;
	for (i = 0; i < s_limFilled && i < s_limDelayLen; i++) {
		if (s_limPeak[i] > m) {
			m = s_limPeak[i];
		}
	}
	return m;
}

static void Om_ApplyLimiter(float *interleaved, int frames, int ch)
{
	int i, c;
	if (!s_limiter || !s_limiter->integer || s_limDelayLen < 1 || ch < 1) {
		return;
	}
	Om_LimiterCalib();
	for (i = 0; i < frames; i++) {
		float in[OM_OUT_MAX];
		float peak = 0.f;
		float window, needed;
		for (c = 0; c < ch; c++) {
			in[c] = interleaved[i * ch + c] * s_limPreTrim;
			if (fabsf(in[c]) > peak) {
				peak = fabsf(in[c]);
			}
		}
		if (s_limFilled < s_limDelayLen) {
			for (c = 0; c < ch; c++) {
				s_limDelay[s_limWrite][c] = in[c];
			}
			s_limPeak[s_limWrite] = peak;
			s_limWrite++;
			if (s_limWrite >= s_limDelayLen) {
				s_limWrite = 0;
			}
			s_limFilled++;
			for (c = 0; c < ch; c++) {
				interleaved[i * ch + c] = 0.f;
			}
			continue;
		}
		for (c = 0; c < ch; c++) {
			s_limDelay[s_limWrite][c] = in[c];
		}
		s_limPeak[s_limWrite] = peak;
		window = Om_WindowPeak();
		needed = window > s_limThresh ? (s_limThresh / window) : 1.f;
		if (needed < s_limGain) {
			s_limGain     = needed;
			s_limHoldLeft = s_limHoldLen;
		} else if (s_limHoldLeft > 0) {
			s_limHoldLeft--;
		} else {
			s_limGain += (1.f - s_limGain) * s_limReleaseCoeff;
			if (s_limGain > needed) {
				s_limGain = needed;
			}
		}
		for (c = 0; c < ch; c++) {
			float o = s_limDelay[s_limWrite][c] * s_limGain * s_limMakeup;
			interleaved[i * ch + c] = Om_Clamp(o, -s_limCeiling, s_limCeiling);
		}
		s_limWrite++;
		if (s_limWrite >= s_limDelayLen) {
			s_limWrite = 0;
		}
	}
}

static float Om_EqQ(float bwOct)
{
	const float a = powf(2.0f, bwOct);
	return sqrtf(a) / (a - 1.0f);
}

static void Om_DesignPeak(omEq_t *b, float fs, float f0, float gainDb, float bwOct)
{
	const float A     = powf(10.0f, gainDb / 40.0f);
	const float w0    = 2.0f * (float)M_PI * f0 / fs;
	const float cosw  = cosf(w0);
	const float sinw  = sinf(w0);
	const float alpha = sinw / (2.0f * Om_EqQ(bwOct));
	const float a0    = 1.0f + alpha / A;
	b->b0 = (1.0f + alpha * A) / a0;
	b->b1 = (-2.0f * cosw) / a0;
	b->b2 = (1.0f - alpha * A) / a0;
	b->a1 = (-2.0f * cosw) / a0;
	b->a2 = (1.0f - alpha / A) / a0;
	memset(b->z1, 0, sizeof(b->z1));
	memset(b->z2, 0, sizeof(b->z2));
}

static void Om_DesignShelf(omEq_t *b, float fs, float f0, float gainDb, float S)
{
	const float A     = powf(10.0f, gainDb / 40.0f);
	const float w0    = 2.0f * (float)M_PI * Om_Clamp(f0, 10.0f, fs * 0.45f) / fs;
	const float cosw  = cosf(w0);
	const float sinw  = sinf(w0);
	float       alpha, two, b0, b1, b2, a0, a1, a2;
	if (S < 0.1f) {
		S = 0.1f;
	}
	alpha = sinw * 0.5f * sqrtf((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
	two   = 2.0f * sqrtf(A) * alpha;
	b0    = A * ((A + 1.0f) + (A - 1.0f) * cosw + two);
	b1    = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw);
	b2    = A * ((A + 1.0f) + (A - 1.0f) * cosw - two);
	a0    = (A + 1.0f) - (A - 1.0f) * cosw + two;
	a1    = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw);
	a2    = (A + 1.0f) - (A - 1.0f) * cosw - two;
	b->b0 = b0 / a0;
	b->b1 = b1 / a0;
	b->b2 = b2 / a0;
	b->a1 = a1 / a0;
	b->a2 = a2 / a0;
	memset(b->z1, 0, sizeof(b->z1));
	memset(b->z2, 0, sizeof(b->z2));
}

static void Om_SpkEqReset(void)
{
	static const float bands[OM_SPK_EQ_BANDS][3] = {
		{101.0f, 5.0f, 1.00f},
		{6000.0f, -2.6f, 0.22f},
		{7000.0f, -2.3f, 0.25f},
	};
	int i;
	if (s_sampleRate <= 0) {
		s_spkEqReady = qfalse;
		return;
	}
	for (i = 0; i < OM_SPK_EQ_BANDS; i++) {
		float f0 = bands[i][0];
		if (f0 >= (float)s_sampleRate * 0.49f) {
			f0 = (float)s_sampleRate * 0.49f;
		}
		Om_DesignPeak(&s_spkEq[i], (float)s_sampleRate, f0, bands[i][1], bands[i][2]);
	}
	s_spkEqReady = qtrue;
}

static void Om_HpEqReset(void)
{
	static const float bands[OM_HP_EQ_BANDS][3] = {
		{100.0f, 3.0f, 1.01f},
		{400.0f, 1.5f, 2.30f},
		{4500.0f, -2.0f, 2.00f},
		{15000.0f, -4.5f, 1.00f},
	};
	static const int types[OM_HP_EQ_BANDS] = {
		OM_HP_EQ_TYPE_PEAK, OM_HP_EQ_TYPE_PEAK, OM_HP_EQ_TYPE_PEAK, OM_HP_EQ_TYPE_HSHELF
	};
	int i;
	if (s_sampleRate <= 0) {
		s_hpEqReady = qfalse;
		return;
	}
	for (i = 0; i < OM_HP_EQ_BANDS; i++) {
		float f0 = bands[i][0];
		if (f0 >= (float)s_sampleRate * 0.49f) {
			f0 = (float)s_sampleRate * 0.49f;
		}
		if (types[i] == OM_HP_EQ_TYPE_HSHELF) {
			Om_DesignShelf(&s_hpEq[i], (float)s_sampleRate, f0, bands[i][1], bands[i][2]);
		} else {
			Om_DesignPeak(&s_hpEq[i], (float)s_sampleRate, f0, bands[i][1], bands[i][2]);
		}
	}
	s_hpEqReady = qtrue;
}

static void Om_RunEq(omEq_t *bands, int nBands, float *interleaved, int frames, int ch)
{
	int i, b, c;
	for (i = 0; i < frames; i++) {
		for (c = 0; c < ch; c++) {
			float x = interleaved[i * ch + c];
			for (b = 0; b < nBands; b++) {
				omEq_t    *band = &bands[b];
				const float y   = band->b0 * x + band->z1[c];
				band->z1[c]     = band->b1 * x - band->a1 * y + band->z2[c];
				band->z2[c]     = band->b2 * x - band->a2 * y;
				x               = y;
			}
			interleaved[i * ch + c] = x;
		}
	}
}

static void Om_HfDesignHp(omHp_t *s, float fs, float fc, float Q)
{
	float w0, cosw, sinw, alpha, a0;
	fc    = Om_Clamp(fc, 10.0f, fs * 0.45f);
	w0    = 2.0f * (float)M_PI * fc / fs;
	cosw  = cosf(w0);
	sinw  = sinf(w0);
	alpha = sinw / (2.0f * Q);
	a0    = 1.0f + alpha;
	s->b0 = ((1.0f + cosw) * 0.5f) / a0;
	s->b1 = (-(1.0f + cosw)) / a0;
	s->b2 = ((1.0f + cosw) * 0.5f) / a0;
	s->a1 = (-2.0f * cosw) / a0;
	s->a2 = (1.0f - alpha) / a0;
	s->z1 = s->z2 = 0.f;
}

static void Om_HfDesignShelf(omShelf_t *s, float fs, float f0, float gainDb)
{
	omEq_t tmp;
	Om_DesignShelf(&tmp, fs, f0, gainDb, OM_HF_DUCK_SHELF_S);
	s->b0 = tmp.b0;
	s->b1 = tmp.b1;
	s->b2 = tmp.b2;
	s->a1 = tmp.a1;
	s->a2 = tmp.a2;
}

static void Om_HfSetShelf(float totalDb)
{
	const float stage = totalDb / (float)OM_HF_DUCK_SHELF_STAGES;
	int         i;
	s_hfGainDb = totalDb;
	if (fabsf(totalDb) < OM_HF_DUCK_BYPASS_DB) {
		s_hfShelfOn = qfalse;
		return;
	}
	s_hfShelfOn = qtrue;
	for (i = 0; i < OM_HF_DUCK_SHELF_STAGES; i++) {
		Om_HfDesignShelf(&s_hfShelf[i], (float)s_sampleRate, OM_HF_DUCK_FC_HZ, stage);
	}
}

static void Om_HfReset(void)
{
	static const float qs[OM_HF_DUCK_HP_SECTIONS] = {0.50979558f, 0.60134489f, 0.89997622f, 2.5629154f};
	int                i;
	s_hfEnv = 0.f;
	s_hfGainDb = 0.f;
	s_hfMeterDb = 0.f;
	s_hfSideDb = -100.f;
	s_hfBoost = 1.f;
	s_hfShelfOn = qfalse;
	for (i = 0; i < OM_HF_DUCK_SHELF_STAGES; i++) {
		memset(s_hfShelf[i].z1, 0, sizeof(s_hfShelf[i].z1));
		memset(s_hfShelf[i].z2, 0, sizeof(s_hfShelf[i].z2));
	}
	if (s_sampleRate <= 0) {
		s_hfReady = qfalse;
		return;
	}
	{
		const float rel = OM_HF_DUCK_RELEASE_MS * 0.001f * (float)s_sampleRate;
		s_hfRelease     = rel > 1.f ? (1.f - expf(-1.f / rel)) : 1.f;
	}
	for (i = 0; i < OM_HF_DUCK_HP_SECTIONS; i++) {
		Om_HfDesignHp(&s_hfHp[i], (float)s_sampleRate, OM_HF_DUCK_FC_HZ, qs[i]);
	}
	Om_HfSetShelf(0.f);
	s_hfReady = qtrue;
}

static float Om_HfHp(float x)
{
	int i;
	for (i = 0; i < OM_HF_DUCK_HP_SECTIONS; i++) {
		omHp_t     *s = &s_hfHp[i];
		const float y = s->b0 * x + s->z1;
		s->z1         = s->b1 * x - s->a1 * y + s->z2;
		s->z2         = s->b2 * x - s->a2 * y;
		x             = y;
	}
	return x;
}

static float Om_HfThresh(void)
{
	float t = s_hf_duck_thresh ? s_hf_duck_thresh->value : OM_HF_DUCK_THRESH_DEFAULT;
	if (t > -1.0f) {
		t = -1.0f;
	}
	if (t < -96.0f) {
		t = -96.0f;
	}
	return t;
}

static void Om_ApplyHfDuck(float *interleaved, int frames, int ch)
{
	float maxCut, thresh, range;
	int   i, c, stage;
	if (!s_hfReady || !s_mix || !s_mix->integer || !s_hf_duck || !s_hf_duck->integer || ch < 1) {
		s_hfMeterDb = 0.f;
		s_hfSideDb  = -100.f;
		return;
	}
	maxCut = s_hf_duck_db ? s_hf_duck_db->value : OM_HF_DUCK_DEFAULT_DB;
	if (maxCut < 0.f) {
		maxCut = -maxCut;
	}
	if (maxCut > 24.f) {
		maxCut = 24.f;
	}
	{
		float sc = s_hf_duck_sc_boost ? s_hf_duck_sc_boost->value : OM_HF_DUCK_SC_BOOST_DEFAULT;
		sc       = Om_Clamp(sc, 0.f, OM_HF_DUCK_SC_BOOST_MAX_DB);
		s_hfBoost = powf(10.f, sc / 20.f);
	}
	thresh = Om_HfThresh();
	range  = -thresh;
	if (range < 1.f) {
		range = 1.f;
	}
	for (i = 0; i < frames; i++) {
		float mono = 0.f, sc, envDb, t, gainDb;
		for (c = 0; c < ch; c++) {
			mono += interleaved[i * ch + c];
		}
		mono /= (float)ch;
		sc = fabsf(Om_HfHp(mono) * s_hfBoost);
		if (sc >= s_hfEnv) {
			s_hfEnv = sc;
		} else {
			s_hfEnv += s_hfRelease * (sc - s_hfEnv);
		}
		envDb      = 20.0f * log10f(s_hfEnv + 1.0e-20f);
		s_hfSideDb = envDb;
		t          = Om_Clamp((envDb - thresh) / range, 0.f, 1.f);
		gainDb     = maxCut < OM_HF_DUCK_BYPASS_DB ? 0.f : (-t * maxCut);
		s_hfMeterDb = gainDb;
		if (fabsf(gainDb - s_hfGainDb) >= OM_HF_DUCK_REDESIGN_DB) {
			Om_HfSetShelf(gainDb);
		}
		if (!s_hfShelfOn) {
			continue;
		}
		for (c = 0; c < ch; c++) {
			float sample = interleaved[i * ch + c];
			for (stage = 0; stage < OM_HF_DUCK_SHELF_STAGES; stage++) {
				omShelf_t *s = &s_hfShelf[stage];
				float      y = s->b0 * sample + s->z1[c];
				s->z1[c]     = s->b1 * sample - s->a1 * y + s->z2[c];
				s->z2[c]     = s->b2 * sample - s->a2 * y;
				sample       = y;
			}
			interleaved[i * ch + c] = sample;
		}
	}
}

static void Om_ApplyMaster(float *interleaved, int frames, int ch)
{
	const float target = s_lisGain;
	float       g0     = s_masterPrimed ? s_masterCur : target;
	int         i, c;
	s_masterPrimed = qtrue;
	for (i = 0; i < frames; i++) {
		const float t = frames > 1 ? (float)i / (float)(frames - 1) : 1.f;
		const float g = g0 + (target - g0) * t;
		for (c = 0; c < ch; c++) {
			interleaved[i * ch + c] *= g;
		}
	}
	s_masterCur = target;
}

static void Om_StorePlanar(std::vector<float> planar[2], const float *interleaved, int channels, int frames)
{
	int c, i;
	for (c = 0; c < channels && c < 2; c++) {
		planar[c].assign((size_t)frames + (size_t)OM_GUARD * 2, 0.f);
		for (i = 0; i < frames; i++) {
			planar[c][(size_t)i + OM_GUARD] = interleaved[i * channels + c];
		}
	}
}

static omSound_t *Om_DecodeSound(omSampleFormat_t fmt, const void *data, int sizeBytes, int rate, int blockAlign)
{
	omPcmDecode_t pcm;
	omSound_t    *s;
	if (!OmPcm_Decode(fmt, data, sizeBytes, blockAlign, &pcm)) {
		return NULL;
	}
	if (pcm.frames <= 0 || pcm.channels < 1) {
		OmPcm_Free(&pcm);
		return NULL;
	}
	s = new (std::nothrow) omSound_t();
	if (s) {
		s->channels = pcm.channels > 2 ? 2 : pcm.channels;
		s->frames   = pcm.frames;
		s->rate     = rate > 0 ? rate : 22050;
		Om_StorePlanar(s->planar, pcm.interleaved, pcm.channels, pcm.frames);
	}
	OmPcm_Free(&pcm);
	return s;
}

static omChunk_t *Om_MakeChunk(omSampleFormat_t fmt, const void *data, int sizeBytes, int rate, int blockAlign)
{
	omPcmDecode_t pcm;
	omChunk_t    *chunk;
	if (!OmPcm_Decode(fmt, data, sizeBytes, blockAlign, &pcm)) {
		return NULL;
	}
	chunk = new (std::nothrow) omChunk_t();
	if (chunk) {
		chunk->channels = pcm.channels > 2 ? 2 : (pcm.channels < 1 ? 1 : pcm.channels);
		chunk->frames   = pcm.frames > 0 ? pcm.frames : 0;
		chunk->rate     = rate > 0 ? rate : 22050;
		if (chunk->frames > 0) {
			Om_StorePlanar(chunk->planar, pcm.interleaved, pcm.channels, pcm.frames);
		}
	}
	OmPcm_Free(&pcm);
	return chunk;
}

static unsigned Om_StepFor(int srcRate, float pitch)
{
	float adv;
	long  step;
	if (srcRate <= 0) {
		srcRate = s_sampleRate;
	}
	if (pitch < 0.001f) {
		pitch = 0.001f;
	}
	adv = ((float)srcRate / (float)s_sampleRate) * pitch;
	/* Changed in Omaha: a supersonic Doppler ratio is the step cap, not an infinite pitch. */
	if (adv > (float)OM_MAX_STEP) {
		adv = (float)OM_MAX_STEP;
	}
	step = lrintf(adv * (float)OM_FRAC_ONE);
	if (step < 1) {
		step = 1;
	}
	if (step > (long)(OM_MAX_STEP * OM_FRAC_ONE)) {
		step = (long)(OM_MAX_STEP * OM_FRAC_ONE);
	}
	return (unsigned)step;
}

static float Om_Sinc(float x)
{
	float pix;
	if (fabsf(x) < 1.0e-8f) {
		return 1.f;
	}
	pix = (float)M_PI * x;
	return sinf(pix) / pix;
}

/* Changed in Omaha: 8-point Blackman sinc. Cleaner imaging than a 4-point cubic. */
static void Om_BuildSinc(void)
{
	int p, i;
	for (p = 0; p < OM_SINC_PHASES; p++) {
		const float frac = (float)p / (float)OM_SINC_PHASES;
		float       sum  = 0.f;
		for (i = 0; i < OM_SINC_TAPS; i++) {
			const float offset = (float)i - 3.f - frac;
			const float wpos   = (offset + 4.f) * (1.f / 8.f);
			float       w      = 0.f;
			if (wpos > 0.f && wpos < 1.f) {
				const float twopi = (float)(2.0 * M_PI);
				w = 0.42f - 0.5f * cosf(twopi * wpos) + 0.08f * cosf(2.f * twopi * wpos);
			}
			s_sinc[p][i] = Om_Sinc(offset) * w;
			sum += s_sinc[p][i];
		}
		if (sum != 0.f) {
			for (i = 0; i < OM_SINC_TAPS; i++) {
				s_sinc[p][i] /= sum;
			}
		}
	}
}

static void Om_BuildStereoPair(void)
{
	const float c30 = cosf((float)(M_PI / 6.0));
	const float s30 = sinf((float)(M_PI / 6.0));
	float       dir[3];
	dir[0] = c30;
	dir[1] = s30;
	dir[2] = 0.f;
	OmHrtf_AmbiCoeffs(dir, s_pairAmbi[0]);
	dir[1] = -s30;
	OmHrtf_AmbiCoeffs(dir, s_pairAmbi[1]);
}

static float Om_StaticAt(const float *s, int frames, int i, qboolean loop, int a, int b);

static float Om_SincAt(const float *k, const float *s, int frames, int pos, qboolean loop, int a, int b)
{
	float sum = 0.f;
	int   t;
	for (t = 0; t < OM_SINC_TAPS; t++) {
		sum += k[t] * Om_StaticAt(s, frames, pos - 3 + t, loop, a, b);
	}
	return sum;
}

static void Om_LoopRange(const omVoice_t *v, int frames, int *a, int *b)
{
	*a = v->params.loopStart > 0 ? v->params.loopStart : 0;
	*b = (v->params.loopEnd > 0 && v->params.loopEnd <= frames) ? v->params.loopEnd : frames;
	if (*a >= *b) {
		*a = 0;
		*b = frames;
	}
}

static int Om_Wrap(int i, int a, int b)
{
	int len = b - a;
	int r;
	if (len <= 0) {
		return a;
	}
	r = (i - a) % len;
	if (r < 0) {
		r += len;
	}
	return a + r;
}

static float Om_StaticAt(const float *s, int frames, int i, qboolean loop, int a, int b)
{
	if (loop) {
		i = Om_Wrap(i, a, b);
	} else if (i < 0 || i >= frames) {
		return 0.f;
	}
	return s[i];
}

static void Om_ResamplePtr(const float *s, int frames, int *pos, unsigned *frac, unsigned step, qboolean loop, int a, int b, float *dst, int count)
{
	int i;
	if (step == OM_FRAC_ONE && *frac == 0) {
		qboolean fast = qtrue;
		if (loop) {
			int need = *pos + count;
			fast     = *pos >= a && need <= b;
		}
		if (fast) {
			for (i = 0; i < count; i++) {
				dst[i] = Om_StaticAt(s, frames, *pos + i, loop, a, b);
			}
			*pos += count;
			if (loop) {
				*pos = Om_Wrap(*pos, a, b);
			}
			return;
		}
	}
	for (i = 0; i < count; i++) {
		dst[i] = Om_SincAt(s_sinc[*frac >> 8], s, frames, *pos, loop, a, b);
		*frac += step;
		*pos += (int)(*frac >> 16);
		*frac &= OM_FRAC_ONE - 1;
		if (loop) {
			*pos = Om_Wrap(*pos, a, b);
		}
	}
}

/* dst[k] = stream frame (start + k) across the chunk queue, zero outside it. */
static void Om_StreamGather(const omVoice_t *v, int ch, int start, int n, float *dst)
{
	int    k    = 0;
	int    base = 0;
	size_t i;
	while (k < n && start + k < 0) {
		dst[k++] = 0.f;
	}
	for (i = 0; i < v->chunks.size() && k < n; i++) {
		const omChunk_t *c = v->chunks[i];
		const int        f = start + k - base;
		if (f < c->frames) {
			const int    cc   = ch < c->channels ? ch : 0;
			int          take = c->frames - f;
			if (take > n - k) {
				take = n - k;
			}
			memcpy(dst + k, c->planar[cc].data() + OM_GUARD + f, sizeof(float) * (size_t)take);
			k += take;
		}
		base += c->frames;
	}
	while (k < n) {
		dst[k++] = 0.f;
	}
}

/* g[0] is three frames before the cursor (first sinc tap). Returns how many frames the cursor moved. */
static int Om_ResampleRun(const float *g, unsigned *frac, unsigned step, float *dst, int count)
{
	unsigned f  = *frac;
	int      ip = 0;
	int      i, t;
	if (step == OM_FRAC_ONE && f == 0) {
		memcpy(dst, g + 3, sizeof(float) * (size_t)count);
		return count;
	}
	for (i = 0; i < count; i++) {
		const float *k = s_sinc[f >> 8];
		float        sum = 0.f;
		for (t = 0; t < OM_SINC_TAPS; t++) {
			sum += k[t] * g[ip + t];
		}
		dst[i] = sum;
		f += step;
		ip += (int)(f >> 16);
		f &= OM_FRAC_ONE - 1;
	}
	*frac = f;
	return ip;
}

static int Om_StreamFrames(const omVoice_t *v)
{
	int    n = 0;
	size_t i;
	for (i = 0; i < v->chunks.size(); i++) {
		n += v->chunks[i]->frames;
	}
	return n;
}

/* Added in Omaha: id tables grow by blocks. The callback only reads a published capacity. */

static omSoundSlot_t *Om_SoundSlot(int id)
{
	int cap, blockIndex, slot, chunkIndex, within;
	omSoundChunk_t *c;
	cap = s_soundCap.load(std::memory_order_acquire);
	if (id < 0 || id >= cap) {
		return NULL;
	}
	blockIndex = id / OM_BLOCK;
	slot       = id % OM_BLOCK;
	chunkIndex = blockIndex / OM_CHUNK_BLOCKS;
	within     = blockIndex % OM_CHUNK_BLOCKS;
	c          = s_soundChunks;
	while (chunkIndex-- > 0) {
		if (!c) {
			return NULL;
		}
		c = c->next;
	}
	if (!c || !c->block[within]) {
		return NULL;
	}
	return &c->block[within]->slot[slot];
}

static omHandle_t *Om_HandleAt(int id)
{
	int cap, blockIndex, slot, chunkIndex, within;
	omHandleChunk_t *c;
	cap = s_handleCap.load(std::memory_order_acquire);
	if (id < 0 || id >= cap) {
		return NULL;
	}
	blockIndex = id / OM_BLOCK;
	slot       = id % OM_BLOCK;
	chunkIndex = blockIndex / OM_CHUNK_BLOCKS;
	within     = blockIndex % OM_CHUNK_BLOCKS;
	c          = s_handleChunks;
	while (chunkIndex-- > 0) {
		if (!c) {
			return NULL;
		}
		c = c->next;
	}
	if (!c || !c->block[within]) {
		return NULL;
	}
	return &c->block[within]->slot[slot];
}

static omVoice_t *Om_VoiceAt(int id)
{
	int cap, blockIndex, slot, chunkIndex, within;
	omVoiceChunk_t *c;
	cap = s_voiceCap.load(std::memory_order_acquire);
	if (id < 0 || id >= cap) {
		return NULL;
	}
	blockIndex = id / OM_BLOCK;
	slot       = id % OM_BLOCK;
	chunkIndex = blockIndex / OM_CHUNK_BLOCKS;
	within     = blockIndex % OM_CHUNK_BLOCKS;
	c          = s_voiceChunks;
	while (chunkIndex-- > 0) {
		if (!c) {
			return NULL;
		}
		c = c->next;
	}
	if (!c || !c->block[within]) {
		return NULL;
	}
	return &c->block[within]->slot[slot];
}

template <typename Fn>
static void Om_ForEachVoice(int cap, Fn fn)
{
	int left = cap;
	omVoiceChunk_t *c;
	for (c = s_voiceChunks; c && left > 0; c = c->next) {
		int b;
		for (b = 0; b < OM_CHUNK_BLOCKS && left > 0; b++) {
			omVoiceBlock_t *blk = c->block[b];
			int n = left > OM_BLOCK ? OM_BLOCK : left;
			int s;
			left -= n;
			if (!blk) {
				continue;
			}
			for (s = 0; s < n; s++) {
				fn(&blk->slot[s]);
			}
		}
	}
}

/* Call with s_apiMutex held. Publishes the new capacity only after the block is linked. */
static qboolean Om_GrowSounds(void)
{
	int cap = s_soundCap.load(std::memory_order_relaxed);
	int blockIndex = cap / OM_BLOCK;
	int chunkIndex = blockIndex / OM_CHUNK_BLOCKS;
	int within = blockIndex % OM_CHUNK_BLOCKS;
	int i;
	omSoundChunk_t *c = s_soundChunks;
	omSoundChunk_t *prev = NULL;
	omSoundBlock_t *b;
	qboolean fresh = qfalse;
	for (i = 0; i < chunkIndex; i++) {
		if (!c) {
			break;
		}
		prev = c;
		c = c->next;
	}
	if (!c) {
		c = new (std::nothrow) omSoundChunk_t();
		if (!c) {
			return qfalse;
		}
		memset(c, 0, sizeof(*c));
		fresh = qtrue;
		if (prev) {
			prev->next = c;
		} else {
			s_soundChunks = c;
		}
	}
	b = new (std::nothrow) omSoundBlock_t();
	if (!b) {
		if (fresh) {
			if (prev) {
				prev->next = NULL;
			} else {
				s_soundChunks = NULL;
			}
			delete c;
		}
		return qfalse;
	}
	for (i = 0; i < OM_BLOCK; i++) {
		b->slot[i].sound.store(NULL, std::memory_order_relaxed);
		b->slot[i].nextFree = (i + 1 < OM_BLOCK) ? cap + i + 1 : s_soundFreeHead;
	}
	c->block[within] = b;
	s_soundFreeHead = cap;
	s_soundCap.store(cap + OM_BLOCK, std::memory_order_release);
	return qtrue;
}

static qboolean Om_GrowHandles(void)
{
	int cap = s_handleCap.load(std::memory_order_relaxed);
	int blockIndex = cap / OM_BLOCK;
	int chunkIndex = blockIndex / OM_CHUNK_BLOCKS;
	int within = blockIndex % OM_CHUNK_BLOCKS;
	int i;
	omHandleChunk_t *c = s_handleChunks;
	omHandleChunk_t *prev = NULL;
	omHandleBlock_t *b;
	qboolean fresh = qfalse;
	for (i = 0; i < chunkIndex; i++) {
		if (!c) {
			break;
		}
		prev = c;
		c = c->next;
	}
	if (!c) {
		c = new (std::nothrow) omHandleChunk_t();
		if (!c) {
			return qfalse;
		}
		memset(c, 0, sizeof(*c));
		fresh = qtrue;
		if (prev) {
			prev->next = c;
		} else {
			s_handleChunks = c;
		}
	}
	b = new (std::nothrow) omHandleBlock_t();
	if (!b) {
		if (fresh) {
			if (prev) {
				prev->next = NULL;
			} else {
				s_handleChunks = NULL;
			}
			delete c;
		}
		return qfalse;
	}
	for (i = 0; i < OM_BLOCK; i++) {
		omHandle_t *h = &b->slot[i];
		h->used.store(0, std::memory_order_relaxed);
		h->stream = qfalse;
		h->voice  = -1;
		h->dead   = qfalse;
		h->state.store(OM_VOICE_STOPPED, std::memory_order_relaxed);
		h->offset.store(0, std::memory_order_relaxed);
		h->queued.store(0, std::memory_order_relaxed);
		h->processed.store(0, std::memory_order_relaxed);
		h->pendingUnqueue.store(0, std::memory_order_relaxed);
		h->nextFree = (i + 1 < OM_BLOCK) ? cap + i + 1 : s_handleFreeHead;
	}
	c->block[within] = b;
	s_handleFreeHead = cap;
	s_handleCap.store(cap + OM_BLOCK, std::memory_order_release);
	return qtrue;
}

static qboolean Om_GrowVoices(void)
{
	int cap = s_voiceCap.load(std::memory_order_relaxed);
	int newCap = cap + OM_BLOCK;
	int blockIndex = cap / OM_BLOCK;
	int chunkIndex = blockIndex / OM_CHUNK_BLOCKS;
	int within = blockIndex % OM_CHUNK_BLOCKS;
	int i;
	int *rank;
	float *gain;
	omVoiceChunk_t *c = s_voiceChunks;
	omVoiceChunk_t *prev = NULL;
	omVoiceBlock_t *b;
	omRankRetired_t *old;
	qboolean fresh = qfalse;
	for (i = 0; i < chunkIndex; i++) {
		if (!c) {
			break;
		}
		prev = c;
		c = c->next;
	}
	if (!c) {
		c = new (std::nothrow) omVoiceChunk_t();
		if (!c) {
			return qfalse;
		}
		memset(c, 0, sizeof(*c));
		fresh = qtrue;
		if (prev) {
			prev->next = c;
		} else {
			s_voiceChunks = c;
		}
	}
	b = new (std::nothrow) omVoiceBlock_t();
	rank = new (std::nothrow) int[newCap];
	gain = new (std::nothrow) float[newCap];
	if (!b || !rank || !gain) {
		delete b;
		delete[] rank;
		delete[] gain;
		if (fresh) {
			if (prev) {
				prev->next = NULL;
			} else {
				s_voiceChunks = NULL;
			}
			delete c;
		}
		return qfalse;
	}
	for (i = 0; i < OM_BLOCK; i++) {
		omVoice_t *v = &b->slot[i];
		v->used      = qfalse;
		v->state     = OM_VOICE_STOPPED;
		v->handle    = -1;
		v->sound     = -1;
		v->isStream  = qfalse;
		v->primed    = qfalse;
		v->ending    = qfalse;
		v->allowDirect = qfalse;
		v->tail      = 0;
		v->hrtf      = NULL;
		v->self      = cap + i;
		v->nextFree  = (i + 1 < OM_BLOCK) ? cap + i + 1 : s_voiceFreeHead;
	}
	if (s_rank.load(std::memory_order_relaxed) || s_rankG.load(std::memory_order_relaxed)) {
		old = new (std::nothrow) omRankRetired_t();
		if (!old) {
			delete b;
			delete[] rank;
			delete[] gain;
			if (fresh) {
				if (prev) {
					prev->next = NULL;
				} else {
					s_voiceChunks = NULL;
				}
				delete c;
			}
			return qfalse;
		}
		old->rank = s_rank.load(std::memory_order_relaxed);
		old->gain = s_rankG.load(std::memory_order_relaxed);
		old->next = s_rankRetired;
		s_rankRetired = old;
	}
	c->block[within] = b;
	s_voiceFreeHead = cap;
	s_rank.store(rank, std::memory_order_release);
	s_rankG.store(gain, std::memory_order_release);
	s_voiceCap.store(newCap, std::memory_order_release);
	return qtrue;
}

/* Call with s_apiMutex held. */
static int Om_TakeSound(void)
{
	int id;
	omSoundSlot_t *s;
	if (s_soundFreeHead == OM_ID_NONE && !Om_GrowSounds()) {
		return OM_SOUND_NONE;
	}
	id = s_soundFreeHead;
	s = Om_SoundSlot(id);
	if (!s) {
		return OM_SOUND_NONE;
	}
	s_soundFreeHead = s->nextFree;
	s->nextFree = OM_ID_NONE;
	return id;
}

/* Call with s_apiMutex held. */
static int Om_TakeHandle(void)
{
	int id;
	omHandle_t *h;
	if (s_handleFreeHead == OM_ID_NONE && !Om_GrowHandles()) {
		return OM_VOICE_NONE;
	}
	id = s_handleFreeHead;
	h = Om_HandleAt(id);
	if (!h) {
		return OM_VOICE_NONE;
	}
	s_handleFreeHead = h->nextFree;
	h->nextFree = OM_ID_NONE;
	return id;
}

/* Call with s_apiMutex held. The slot stays unused until the callback binds it. */
static int Om_TakeVoice(void)
{
	int id;
	omVoice_t *v;
	if (s_voiceFreeHead == OM_ID_NONE && !Om_GrowVoices()) {
		return OM_ID_NONE;
	}
	id = s_voiceFreeHead;
	v = Om_VoiceAt(id);
	if (!v) {
		return OM_ID_NONE;
	}
	s_voiceFreeHead = v->nextFree;
	v->nextFree = OM_ID_NONE;
	return id;
}

static void Om_ReturnHandle(int id)
{
	omHandle_t *h = Om_HandleAt(id);
	if (!h) {
		return;
	}
	h->used.store(0, std::memory_order_relaxed);
	h->dead = qfalse;
	h->voice = -1;
	h->nextFree = s_handleFreeHead;
	s_handleFreeHead = id;
}

static const omSound_t *Om_VoiceSound(const omVoice_t *v)
{
	omSoundSlot_t *s;
	if (v->isStream || v->sound < 0) {
		return NULL;
	}
	s = Om_SoundSlot(v->sound);
	return s ? s->sound.load(std::memory_order_acquire) : NULL;
}

static int Om_VoiceChannels(const omVoice_t *v)
{
	const omSound_t *s;
	if (v->isStream) {
		return v->chunks.empty() ? 0 : v->chunks.front()->channels;
	}
	s = Om_VoiceSound(v);
	return s ? s->channels : 0;
}

static void Om_DistanceGain(omVoice_t *v)
{
	const omVoiceParams_t *p = &v->params;
	float                  rel[3], front, left, up, d;
	v->distScale = 1.f;
	v->dist      = 0.f;
	v->dir[0]    = 1.f;
	v->dir[1]    = 0.f;
	v->dir[2]    = 0.f;
	/* Changed in Omaha: Miles pan sits on the unit circle behind the listener (AL +Z). */
	if (p->flags & OM_FLAG_PAN) {
		const float pan = Om_Clamp(p->pan, -1.f, 1.f);
		const float z   = sqrtf(fmaxf(0.f, 1.f - pan * pan));
		v->dir[0]       = -z;
		v->dir[1]       = -pan;
		v->dir[2]       = 0.f;
		v->dist         = 1.f;
		return;
	}
	if (p->flags & OM_FLAG_2D) {
		return;
	}
	rel[0] = p->pos[0] - s_lisPos[0];
	rel[1] = p->pos[1] - s_lisPos[1];
	rel[2] = p->pos[2] - s_lisPos[2];
	front  = rel[0] * s_lisFwd[0] + rel[1] * s_lisFwd[1] + rel[2] * s_lisFwd[2];
	left   = rel[0] * s_lisLeft[0] + rel[1] * s_lisLeft[1] + rel[2] * s_lisLeft[2];
	up     = rel[0] * s_lisUp[0] + rel[1] * s_lisUp[1] + rel[2] * s_lisUp[2];
	d      = sqrtf(front * front + left * left + up * up);
	v->dist = d;
	if (d > 0.0001f) {
		v->dir[0] = front / d;
		v->dir[1] = left / d;
		v->dir[2] = up / d;
	}
	if (p->minDist > 0.f) {
		const float c = Om_Clamp(d, p->minDist, p->maxDist > p->minDist ? p->maxDist : p->minDist);
		v->distScale  = p->minDist / c;
	}
}

/*
 * Lateral image (+1 left .. -1 right), horizontal weight and azimuth. The image
 * is the sine of (pi/2 * lateral share) scaled by the horizontal length, so a
 * source above or below the head collapses to the centre instead of flipping
 * sides on tiny horizontal offsets.
 */
static void Om_Image(const omVoice_t *v, float *img, float *horiz, float *az)
{
	float h, x;
	h = sqrtf(v->dir[0] * v->dir[0] + v->dir[1] * v->dir[1]);
	if (h < 1.0e-4f) {
		*img   = 0.f;
		*horiz = 0.f;
		*az    = 0.f;
		return;
	}
	x      = Om_Clamp(v->dir[1] / h, -1.f, 1.f);
	*img   = sinf((float)(M_PI * 0.5) * x) * h;
	*horiz = h > 1.f ? 1.f : h;
	*az    = atan2f(v->dir[1], v->dir[0]);
}

static void Om_StereoGains(float img, float *gL, float *gR)
{
	const float th = (1.f - Om_Clamp(img, -1.f, 1.f)) * (float)(M_PI * 0.25);
	*gL            = cosf(th);
	*gR            = sinf(th);
}

/*
 * Changed in Omaha: two-speaker image from main. Lateral angle is (pi/2) * left/h,
 * encoded as first-order W/Y/X and decoded with the stereo matrix. h scales the
 * encoded direction, so a source overhead collapses to W instead of flipping sides.
 */
static void Om_FoaStereo(const float dir[3], float *gL, float *gR)
{
	const float sqrt3 = 1.73205080757f;
	const float h     = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
	float       side, front, Y, X;
	if (h < 1.0e-4f) {
		side  = 0.f;
		front = 0.f;
	} else {
		const float x     = Om_Clamp(dir[1] / h, -1.f, 1.f);
		const float angle = (float)(M_PI * 0.5f) * x;
		side              = sinf(angle) * h;
		front             = cosf(angle) * h;
	}
	Y   = sqrt3 * side;
	X   = sqrt3 * front;
	*gL = 0.5f + 0.288675135f * Y + 0.0552305643f * X;
	*gR = 0.5f - 0.288675135f * Y + 0.0552305643f * X;
}

/* Changed in Omaha: main's Doppler. Speeds are world-space and already scaled. */
static float Om_Doppler(const omVoice_t *v)
{
	const float c = 343.3f;
	float       to[3], d, vss, vls;
	int         i;
	to[0] = v->params.pos[0] - s_lisPos[0];
	to[1] = v->params.pos[1] - s_lisPos[1];
	to[2] = v->params.pos[2] - s_lisPos[2];
	d     = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
	if (d < 1.0e-4f) {
		return 1.f;
	}
	for (i = 0; i < 3; i++) {
		to[i] /= d;
	}
	vss = -(v->params.vel[0] * to[0] + v->params.vel[1] * to[1] + v->params.vel[2] * to[2]);
	vls = -(s_lisVel[0] * to[0] + s_lisVel[1] * to[1] + s_lisVel[2] * to[2]);
	if (!(vls < c)) {
		return 0.f;
	}
	if (!(vss < c)) {
		return 1.0e6f;
	}
	return (c - vls) / (c - vss);
}

/* Pairwise gains on the speaker ring, spread evenly as the source goes overhead. */
static void Om_RingGains(float az, float horiz, float g[OM_OUT_MAX])
{
	int i;
	memset(g, 0, sizeof(float) * OM_OUT_MAX);
	for (i = 0; i < s_spkN; i++) {
		const int j   = (i + 1) % s_spkN;
		const float a0 = s_spkAz[i];
		float       a1 = s_spkAz[j];
		float       an = az;
		if (j == 0) {
			a1 += (float)(2.0 * M_PI);
		}
		if (an < a0) {
			an += (float)(2.0 * M_PI);
		}
		if (an >= a0 && an <= a1 && a1 > a0) {
			const float t = (an - a0) / (a1 - a0);
			g[s_spkCh[i]] = cosf(t * (float)(M_PI * 0.5));
			g[s_spkCh[j]] = sinf(t * (float)(M_PI * 0.5));
			break;
		}
	}
	if (horiz < 1.f) {
		const float u = (1.f - horiz) / sqrtf((float)s_spkN);
		float       e = 0.f;
		for (i = 0; i < s_spkN; i++) {
			g[s_spkCh[i]] = g[s_spkCh[i]] * horiz + u;
			e += g[s_spkCh[i]] * g[s_spkCh[i]];
		}
		if (e > 1.0e-6f) {
			e = 1.f / sqrtf(e);
			for (i = 0; i < s_spkN; i++) {
				g[s_spkCh[i]] *= e;
			}
		}
	}
}

/* Mix `in` into `out`, gain moving linearly from `from` to `to` across the block. */
static void Om_AddGain(const float *in, float *out, float from, float to, int count)
{
	int i;
	if (fabsf(from) <= OM_SILENCE && fabsf(to) <= OM_SILENCE) {
		return;
	}
	if (from == to) {
		for (i = 0; i < count; i++) {
			out[i] += in[i] * to;
		}
		return;
	}
	{
		const float step = (to - from) / (float)count;
		float       g    = from;
		for (i = 0; i < count; i++) {
			g += step;
			out[i] += in[i] * g;
		}
	}
}

static void Om_Filter(float *buf, int count, float a, float *z)
{
	int   i;
	float s = *z;
	for (i = 0; i < count; i++) {
		s += (buf[i] - s) * a;
		buf[i] = s;
	}
	*z = s;
}

static void Om_Rear(float *buf, int count, float rear, float *z)
{
	int   i;
	float s  = *z;
	float hf = Om_Lerp(1.f, OM_REAR_SHELF_HF, rear);
	if (rear <= 0.001f) {
		*z = count > 0 ? buf[count - 1] : s;
		return;
	}
	for (i = 0; i < count; i++) {
		s += (buf[i] - s) * s_rearA;
		buf[i] = s + hf * (buf[i] - s);
	}
	*z = s;
}

static void Om_FreeHrtf(omVoice_t *v)
{
	if (v->hrtf) {
		OmHrtf_VoiceFree(v->hrtf);
		v->hrtf = NULL;
	}
	v->tail = 0;
}

static void Om_ClearChunks(omVoice_t *v)
{
	size_t i;
	for (i = 0; i < v->chunks.size(); i++) {
		delete v->chunks[i];
	}
	v->chunks.clear();
}

static void Om_ResetVoice(omVoice_t *v)
{
	Om_FreeHrtf(v);
	Om_ClearChunks(v);
	v->used        = qfalse;
	v->state       = OM_VOICE_STOPPED;
	v->handle      = -1;
	v->sound       = -1;
	v->isStream    = qfalse;
	v->primed      = qfalse;
	v->ending      = qfalse;
	v->allowDirect = qfalse;
	v->pos         = 0;
	v->frac        = 0;
	v->dist        = 0.f;
	v->distScale   = 1.f;
	v->dir[0]      = 1.f;
	v->dir[1]      = 0.f;
	v->dir[2]      = 0.f;
	memset(v->spk, 0, sizeof(v->spk));
	memset(v->ambi, 0, sizeof(v->ambi));
	v->direct   = 0.f;
	v->rear     = 0.f;
	v->rearZ    = 0.f;
	v->lpZ[0]   = 0.f;
	v->lpZ[1]   = 0.f;
	v->obstruct = 0.f;
	v->occlude  = 0.f;
}

static qboolean Om_HasGain(const omVoice_t *v)
{
	int k;
	for (k = 0; k < OM_OUT_MAX; k++) {
		if (v->spk[0][k] > OM_SILENCE || v->spk[1][k] > OM_SILENCE) {
			return qtrue;
		}
	}
	for (k = 0; k < OM_AMBI_CHANNELS; k++) {
		if (fabsf(v->ambi[k]) > OM_SILENCE) {
			return qtrue;
		}
	}
	return v->direct > OM_SILENCE ? qtrue : qfalse;
}

static omHandle_t *Om_VoiceHandle(const omVoice_t *v)
{
	if (v->handle < 0) {
		return NULL;
	}
	return Om_HandleAt(v->handle);
}

/* Mixer thread only. The id returns to the game thread after this callback. */
static void Om_RetireVoice(omVoice_t *v)
{
	int id;
	if (!v || !v->used) {
		return;
	}
	id = v->self;
	Om_ResetVoice(v);
	v->self = id;
	v->nextFree = s_voiceRecycleHead;
	s_voiceRecycleHead = id;
}

static void Om_RecycleUnusedVoice(int id)
{
	omVoice_t *v;
	if (id < 0) {
		return;
	}
	v = Om_VoiceAt(id);
	if (!v || v->used) {
		return;
	}
	v->nextFree = s_voiceRecycleHead;
	s_voiceRecycleHead = id;
}

static void Om_Publish(omVoice_t *v)
{
	omHandle_t *h = Om_VoiceHandle(v);
	int         passed, left;
	size_t      i;
	if (!h) {
		return;
	}
	h->offset.store(v->pos > 0 ? (unsigned)v->pos : 0u, std::memory_order_relaxed);
	if (!v->isStream) {
		return;
	}
	passed = 0;
	left   = v->pos;
	for (i = 0; i < v->chunks.size(); i++) {
		if (left < v->chunks[i]->frames) {
			break;
		}
		passed++;
		left -= v->chunks[i]->frames;
	}
	h->processed.store(passed);
}

/* The handle reads STOPPED from now on; the voice keeps fading on its own. */
static void Om_Detach(omVoice_t *v)
{
	omHandle_t *h = Om_VoiceHandle(v);
	if (h) {
		Om_Publish(v);
		h->state.store(OM_VOICE_STOPPED);
		if (h->voice >= 0 && Om_VoiceAt(h->voice) == v) {
			h->voice = -1;
		}
	}
	v->handle = -1;
}

/* Paused or idle voices go at once; audible ones fade over the next block. */
static void Om_Release(omVoice_t *v)
{
	Om_Detach(v);
	if (v->state != OM_VOICE_PLAYING || !v->primed) {
		Om_RetireVoice(v);
		return;
	}
	v->ending = qtrue;
}

static void Om_Targets(
	omVoice_t *v, int channels, float base, float rearStep, float tSpk[2][OM_OUT_MAX], float tAmbi[OM_AMBI_CHANNELS],
	float *tDirect
)
{
	const omVoiceParams_t *p = &v->params;
	float                  img, horiz, az, rear = 0.f;
	int                    k;

	memset(tSpk, 0, sizeof(float) * 2 * OM_OUT_MAX);
	memset(tAmbi, 0, sizeof(float) * OM_AMBI_CHANNELS);
	*tDirect = 0.f;
	if (v->ending) {
		return;
	}
	if (channels >= 2) {
		if (s_hrtfOn) {
			/* ±30° pair on the 3rd-order bus. tSpk[c][0] is that channel's gain. */
			tSpk[0][0] = base;
			tSpk[1][0] = base;
			return;
		}
		if (s_layout == 3) {
			float g[OM_OUT_MAX];
			int   s;
			Om_RingGains((float)(M_PI / 6.0), 1.f, g);
			for (s = 0; s < s_outCh; s++) {
				tSpk[0][s] = base * g[s];
			}
			Om_RingGains((float)(-M_PI / 6.0), 1.f, g);
			for (s = 0; s < s_outCh; s++) {
				tSpk[1][s] = base * g[s];
			}
			return;
		}
		tSpk[0][s_frontL] = base;
		tSpk[1][s_frontR] = base;
		return;
	}
	if (p->flags & OM_FLAG_FLAT) {
		tSpk[0][s_frontL] = base * 0.7071f;
		tSpk[0][s_frontR] = base * 0.7071f;
		return;
	}
	if (s_hrtfOn) {
		/* Plain 2D and dry stay equal-power. A Miles pan is a real behind-the-head direction. */
		if (p->route == OM_ROUTE_DRY || ((p->flags & OM_FLAG_2D) && !(p->flags & OM_FLAG_PAN))) {
			Om_Image(v, &img, &horiz, &az);
			Om_StereoGains(img, &tSpk[0][0], &tSpk[0][1]);
			tSpk[0][0] *= base;
			tSpk[0][1] *= base;
		} else if (v->allowDirect) {
			*tDirect = base;
		} else {
			OmHrtf_AmbiCoeffs(v->dir, tAmbi);
			for (k = 0; k < OM_AMBI_CHANNELS; k++) {
				tAmbi[k] *= base;
			}
		}
		return;
	}
	if (s_layout == 0) {
		float gL, gR;
		/* PAN is behind the listener, so it takes the rear cue. Plain 2D is in front. */
		if (!(p->flags & OM_FLAG_2D) || (p->flags & OM_FLAG_PAN)) {
			rear = Om_Clamp(-v->dir[0], 0.f, 1.f);
		}
		v->rear += (rear - v->rear) * rearStep;
		if (fabsf(rear - v->rear) < 0.0005f) {
			v->rear = rear;
		}
		base *= Om_Lerp(1.f, OM_REAR_GAIN, v->rear);
		Om_FoaStereo(v->dir, &gL, &gR);
		tSpk[0][s_frontL] = base * gL;
		tSpk[0][s_frontR] = base * gR;
		return;
	}
	Om_Image(v, &img, &horiz, &az);
	Om_RingGains(az, horiz, tSpk[0]);
	for (k = 0; k < s_outCh; k++) {
		tSpk[0][k] *= base;
	}
}

static qboolean Om_TargetsSilent(const float tSpk[2][OM_OUT_MAX], const float tAmbi[OM_AMBI_CHANNELS], float tDirect)
{
	int k;
	for (k = 0; k < OM_OUT_MAX; k++) {
		if (tSpk[0][k] > OM_SILENCE || tSpk[1][k] > OM_SILENCE) {
			return qfalse;
		}
	}
	for (k = 0; k < OM_AMBI_CHANNELS; k++) {
		if (fabsf(tAmbi[k]) > OM_SILENCE) {
			return qfalse;
		}
	}
	return tDirect > OM_SILENCE ? qfalse : qtrue;
}

/* Run the direct HRTF filter: ramped input while audible, then zeros until it rings out. */
static void Om_MixDirect(omVoice_t *v, float tDirect, int count)
{
	int i;
	if (tDirect > OM_SILENCE || v->direct > OM_SILENCE) {
		if (!v->hrtf) {
			v->hrtf = OmHrtf_VoiceAlloc();
		}
		if (v->hrtf) {
			const float step = (tDirect - v->direct) / (float)count;
			float       g    = v->direct;
			for (i = 0; i < count; i++) {
				g += step;
				s_tmp[i] = s_dry[0][i] * g;
			}
			OmHrtf_VoiceProcess(v->hrtf, v->dir, s_tmp, s_bus[0], s_bus[1], count);
			v->tail = OmHrtf_TailBlocks() * OM_HRTF_BLOCK;
		}
	} else if (v->hrtf) {
		if (v->tail > 0) {
			memset(s_tmp, 0, sizeof(float) * (size_t)count);
			OmHrtf_VoiceProcess(v->hrtf, v->dir, s_tmp, s_bus[0], s_bus[1], count);
			v->tail -= count;
		} else {
			Om_FreeHrtf(v);
		}
	}
	v->direct = tDirect;
}

static void Om_MixVoice(omVoice_t *v, int count)
{
	omVoiceParams_t *p = &v->params;
	const omSound_t *snd = NULL;
	float            tSpk[2][OM_OUT_MAX], tAmbi[OM_AMBI_CHANNELS], tDirect;
	float            base;
	int              channels, frames, rate, a = 0, b = 0, c, o, k;
	unsigned         step;
	qboolean         loop, finished = qfalse;

	if (v->state != OM_VOICE_PLAYING) {
		return;
	}

	/* Faded out: only the HRTF filter may still ring. */
	if (v->ending && !Om_HasGain(v)) {
		Om_MixDirect(v, 0.f, count);
		if (!v->hrtf) {
			Om_RetireVoice(v);
		}
		return;
	}

	if (v->isStream) {
		frames = Om_StreamFrames(v);
		if (v->pos >= frames) {
			/* Ran dry: wait for more data or a stop. */
			v->pos  = frames;
			v->frac = 0;
			v->state = OM_VOICE_STOPPED;
			Om_Publish(v);
			if (Om_VoiceHandle(v)) {
				Om_VoiceHandle(v)->state.store(OM_VOICE_STOPPED);
			}
			if (v->ending) {
				Om_RetireVoice(v);
			}
			return;
		}
		channels = v->chunks.front()->channels;
		rate     = v->chunks.front()->rate;
		loop     = qfalse;
	} else {
		snd = Om_VoiceSound(v);
		if (!snd) {
			Om_Detach(v);
			Om_RetireVoice(v);
			return;
		}
		channels = snd->channels;
		frames   = snd->frames;
		rate     = snd->rate;
		loop     = (p->flags & OM_FLAG_LOOP) ? qtrue : qfalse;
		Om_LoopRange(v, frames, &a, &b);
	}
	if (channels > 2) {
		channels = 2;
	}
	{
		float pitch = p->pitch;
		if (channels == 1 && !(p->flags & (OM_FLAG_2D | OM_FLAG_PAN | OM_FLAG_FLAT))) {
			pitch *= Om_Doppler(v);
		}
		step = Om_StepFor(rate, pitch);
	}
	base = Om_Clamp(p->gain, 0.f, OM_GAIN_MAX) * v->distScale;

	Om_Targets(v, channels, base, s_rearStep, tSpk, tAmbi, &tDirect);
	if (!v->primed) {
		memcpy(v->spk, tSpk, sizeof(v->spk));
		memcpy(v->ambi, tAmbi, sizeof(v->ambi));
		v->direct = tDirect;
		v->primed = qtrue;
	}

	/* Inaudible (beyond max distance, zero gain): move the cursor only. */
	if (!v->hrtf && !Om_HasGain(v) && Om_TargetsSilent(tSpk, tAmbi, tDirect)) {
		const uint64_t f = (uint64_t)v->frac + (uint64_t)step * (uint64_t)count;
		v->pos += (int)(f >> 16);
		v->frac = (unsigned)(f & (OM_FRAC_ONE - 1));
		if (loop) {
			v->pos = Om_Wrap(v->pos, a, b);
		} else if (v->isStream && v->pos > frames) {
			v->pos  = frames;
			v->frac = 0;
		}
		finished = !loop && !v->isStream && v->pos >= frames;
		Om_Publish(v);
		if (finished || v->ending) {
			Om_Detach(v);
			Om_RetireVoice(v);
		}
		return;
	}

	for (c = 0; c < channels; c++) {
		int      pos  = v->pos;
		unsigned frac = v->frac;
		if (v->isStream) {
			const int need = (int)(((uint64_t)frac + (uint64_t)step * (uint64_t)count) >> 16) + OM_SINC_TAPS;
			Om_StreamGather(v, c, pos - 3, need, s_gather);
			pos += Om_ResampleRun(s_gather, &frac, step, s_dry[c], count);
		} else {
			const float *ptr = snd->planar[c].data() + OM_GUARD;
			Om_ResamplePtr(ptr, frames, &pos, &frac, step, loop, a, b, s_dry[c], count);
		}
		if (c + 1 == channels) {
			v->pos  = pos;
			v->frac = frac;
		}
	}
	if (v->isStream) {
		if (v->pos > frames) {
			v->pos  = frames;
			v->frac = 0;
		}
	} else if (!loop && v->pos >= frames) {
		finished = qtrue;
	}

	if (s_obstruction && s_obstruction->integer && !(p->flags & (OM_FLAG_2D | OM_FLAG_FLAT | OM_FLAG_PAN))) {
		const float smooth = 1.f - expf(-(float)count / (OM_OBSTRUCT_SMOOTH_MS * 0.001f * (float)s_sampleRate));
		float       distT, ray, room, fcRay, fc, dry;
		v->obstruct += (p->obstruction - v->obstruct) * smooth;
		v->occlude += (p->occlusion - v->occlude) * smooth;
		distT = Om_Clamp(v->dist / OM_OBSTRUCT_DIST_RANGE, 0.f, 1.f);
		ray   = Om_Clamp(v->obstruct, 0.f, 1.f);
		room  = Om_Clamp(v->occlude, 0.f, 1.f);
		fcRay = Om_Lerp(
            Om_Lerp(OM_OBSTRUCT_OPEN_HZ, OM_OBSTRUCT_CUTOFF_HZ, distT),
            Om_Lerp(OM_OBSTRUCT_OPEN_HZ, OM_OBSTRUCT_OCC_BLOCKED_HZ, ray),
            OM_OBSTRUCT_MIX
        );
		/* Closed portal replaces the brush lowpass. It does not stack on it. */
		fc  = Om_Lerp(fcRay, OM_OCCLUDE_CUTOFF_HZ, room);
		dry = Om_Lerp(1.f, OM_OCCLUDE_GAIN, room);
		if ((distT >= OM_OBSTRUCT_EPSILON || ray >= OM_OBSTRUCT_EPSILON || room >= OM_OBSTRUCT_EPSILON)
		    && fc < (float)s_sampleRate * 0.45f) {
			const float lpA = fmaxf(1.f - expf(-2.f * (float)M_PI * fmaxf(fc, 20.f) / (float)s_sampleRate), 1.0e-4f);
			const float makeup = Om_Clamp(sqrtf(sqrtf((2.f - lpA) / lpA)), 1.f, OM_OBSTRUCT_MAKEUP_MAX);
			for (c = 0; c < channels; c++) {
				Om_Filter(s_dry[c], count, lpA, &v->lpZ[c]);
				for (k = 0; k < count; k++) {
					s_dry[c][k] *= makeup * dry;
				}
			}
		} else if (dry < 1.f) {
			for (c = 0; c < channels; c++) {
				for (k = 0; k < count; k++) {
					s_dry[c][k] *= dry;
				}
			}
		}
	} else {
		v->obstruct = 0.f;
		v->occlude  = 0.f;
	}
	if (!s_hrtfOn && s_layout == 0 && channels == 1) {
		Om_Rear(s_dry[0], count, v->rear, &v->rearZ);
	}

	if (s_hrtfOn && channels >= 2) {
		qboolean any = qfalse;
		for (c = 0; c < 2; c++) {
			const float from = v->spk[c][0];
			const float to   = tSpk[c][0];
			const float step = (to - from) / (float)count;
			float       g    = from;
			int         i;
			for (i = 0; i < count; i++) {
				g += step;
				s_tmp[i] = s_dry[c][i] * g;
			}
			for (k = 0; k < OM_AMBI_CHANNELS; k++) {
				const float coeff = s_pairAmbi[c][k];
				if (coeff == 0.f) {
					continue;
				}
				for (i = 0; i < count; i++) {
					s_ambi[k][i] += s_tmp[i] * coeff;
				}
				any = qtrue;
			}
		}
		if (any) {
			s_ambiFlush = (OmHrtf_TailBlocks() + 2) * OM_HRTF_BLOCK;
		}
	} else {
		for (c = 0; c < channels; c++) {
			for (o = 0; o < s_outCh; o++) {
				Om_AddGain(s_dry[c], s_bus[o], v->spk[c][o], tSpk[c][o], count);
			}
		}
	}
	memcpy(v->spk, tSpk, sizeof(v->spk));
	if (s_hrtfOn && channels == 1) {
		qboolean any = qfalse;
		for (k = 0; k < OM_AMBI_CHANNELS; k++) {
			if (fabsf(v->ambi[k]) > OM_SILENCE || fabsf(tAmbi[k]) > OM_SILENCE) {
				Om_AddGain(s_dry[0], s_ambi[k], v->ambi[k], tAmbi[k], count);
				any = qtrue;
			}
		}
		if (any) {
			s_ambiFlush = (OmHrtf_TailBlocks() + 2) * OM_HRTF_BLOCK;
		}
		memcpy(v->ambi, tAmbi, sizeof(v->ambi));
		Om_MixDirect(v, tDirect, count);
	}

	Om_Publish(v);
	if (finished) {
		/* Past the end the input is silent, so there is nothing left to fade. */
		Om_Detach(v);
		memset(v->spk, 0, sizeof(v->spk));
		memset(v->ambi, 0, sizeof(v->ambi));
		v->direct = 0.f;
		v->ending = qtrue;
		if (!v->hrtf) {
			Om_RetireVoice(v);
		}
	}
}

static void Om_MixBlock(int count)
{
	int i, o, n = 0;

	/* Changed in Omaha: one 50 ms step for this line, shared by every voice. */
	if (s_sampleRate > 0 && count > 0) {
		s_rearStep = 1.f - expf(-(float)count / (OM_REAR_SMOOTH_MS * 0.001f * (float)s_sampleRate));
	} else {
		s_rearStep = 1.f;
	}

	for (o = 0; o < s_outCh; o++) {
		memset(s_bus[o], 0, sizeof(float) * (size_t)count);
	}
	if (s_hrtfOn) {
		for (o = 0; o < OM_AMBI_CHANNELS; o++) {
			memset(s_ambi[o], 0, sizeof(float) * (size_t)count);
		}
	}
	{
		const int cap = s_voiceCap.load(std::memory_order_acquire);
		int      *rank = s_rank.load(std::memory_order_acquire);
		float    *rankG = s_rankG.load(std::memory_order_acquire);
		if (rank && rankG && cap > 0) {
			Om_ForEachVoice(cap, [&](omVoice_t *v) {
				if (!v->used) {
					return;
				}
				Om_DistanceGain(v);
				v->allowDirect = qfalse;
				if (n >= cap) {
					return;
				}
				if (s_hrtfOn && v->state == OM_VOICE_PLAYING && !v->ending && v->params.route == OM_ROUTE_DIRECT
					&& !(v->params.flags & (OM_FLAG_2D | OM_FLAG_FLAT | OM_FLAG_PAN)) && v->dist > OM_HRTF_MIN_DIST
					&& Om_VoiceChannels(v) == 1) {
					rank[n]  = v->self;
					rankG[n] = v->params.gain * v->distScale;
					n++;
				}
			});
			/* The loudest few direct voices get their own HRTF filter; the rest share the bus. */
			for (i = 0; i < n && i < OM_DIRECT_CAP; i++) {
				int best = i, k;
				for (k = i + 1; k < n; k++) {
					if (rankG[k] > rankG[best]) {
						best = k;
					}
				}
				if (best != i) {
					const int   ti = rank[i];
					const float tg = rankG[i];
					rank[i]      = rank[best];
					rankG[i]     = rankG[best];
					rank[best]   = ti;
					rankG[best]  = tg;
				}
				{
					omVoice_t *chosen = Om_VoiceAt(rank[i]);
					if (chosen) {
						chosen->allowDirect = qtrue;
					}
				}
			}
			Om_ForEachVoice(cap, [&](omVoice_t *v) {
				if (v->used) {
					Om_MixVoice(v, count);
				}
			});
		}
	}
	if (s_hrtfOn && s_ambiFlush > 0) {
		float *ambi[OM_AMBI_CHANNELS];
		for (o = 0; o < OM_AMBI_CHANNELS; o++) {
			ambi[o] = s_ambi[o];
		}
		OmHrtf_AmbiDecode(ambi, s_bus[0], s_bus[1], count);
		s_ambiFlush -= count;
	}
}

static void Om_Interleave(float *dst, int frames, int ch)
{
	int i, c;
	for (i = 0; i < frames; i++) {
		for (c = 0; c < ch; c++) {
			dst[i * ch + c] = s_bus[c][i];
		}
	}
}

static void Om_Downmix(float *stereo, int frames)
{
	int i;
	for (i = 0; i < frames; i++) {
		float l, r;
		if (s_layout == 3) {
			l = s_bus[0][i] + s_bus[2][i];
			r = s_bus[1][i] + s_bus[3][i];
			l *= 0.7071f;
			r *= 0.7071f;
		} else if (s_layout == 2) {
			l = s_bus[0][i] + 0.7071f * s_bus[2][i] + s_bus[4][i];
			r = s_bus[1][i] + 0.7071f * s_bus[2][i] + s_bus[5][i];
			l *= 0.7071f;
			r *= 0.7071f;
		} else {
			l = s_bus[0][i];
			r = s_bus[1][i];
		}
		stereo[i * 2 + 0] = l;
		stereo[i * 2 + 1] = r;
	}
}

static void Om_Master(float *interleaved, int frames, int ch)
{
	if (!s_hrtfOn && s_layout == 0 && s_spkEqReady) {
		Om_RunEq(s_spkEq, OM_SPK_EQ_BANDS, interleaved, frames, ch);
	}
	if (s_hrtfOn && s_hpEqReady && ch == 2) {
		Om_RunEq(s_hpEq, OM_HP_EQ_BANDS, interleaved, frames, ch);
	}
	Om_ApplyHfDuck(interleaved, frames, ch);
	Om_ApplyLimiter(interleaved, frames, ch);
	Om_ApplyMaster(interleaved, frames, ch);
}

static void Om_RenderLine(float *dst, int frames, int ch, qboolean stereoOut)
{
	Om_MixBlock(frames);
	if (stereoOut) {
		Om_Downmix(dst, frames);
		Om_Master(dst, frames, 2);
	} else {
		Om_Interleave(dst, frames, ch);
		Om_Master(dst, frames, ch);
	}
}

/* HRTF filters take whole blocks; a short request is served from one stashed block. */
static void Om_Mix(float *dst, int frames, int ch, qboolean stereoOut)
{
	OmFtzGuard ftz;
	const int  oc = stereoOut ? 2 : ch;
	while (frames > 0) {
		int n;
		if (!s_hrtfOn) {
			n = frames < OM_LINE ? frames : OM_LINE;
			Om_RenderLine(dst, n, ch, stereoOut);
		} else if (s_stashLen > 0) {
			n = frames < s_stashLen ? frames : s_stashLen;
			memcpy(dst, s_stash + (size_t)s_stashPos * oc, sizeof(float) * (size_t)n * oc);
			s_stashPos += n;
			s_stashLen -= n;
		} else if (frames >= OM_HRTF_BLOCK) {
			n = frames - frames % OM_HRTF_BLOCK;
			if (n > OM_LINE) {
				n = OM_LINE;
			}
			Om_RenderLine(dst, n, ch, stereoOut);
		} else {
			Om_RenderLine(s_stash, OM_HRTF_BLOCK, ch, stereoOut);
			s_stashPos = 0;
			s_stashLen = OM_HRTF_BLOCK;
			continue;
		}
		dst += (size_t)n * oc;
		frames -= n;
	}
}

static void Om_FreeHandle(int id)
{
	omHandle_t *h = Om_HandleAt(id);
	if (!h || h->dead) {
		return;
	}
	h->voice = -1;
	h->dead  = qtrue;
	h->state.store(OM_VOICE_STOPPED);
	h->nextFree = s_handleRecycleHead;
	s_handleRecycleHead = id;
}

static void Om_RecycleSound(int id)
{
	omSoundSlot_t *s = Om_SoundSlot(id);
	if (!s) {
		return;
	}
	s->nextFree = s_soundRecycleHead;
	s_soundRecycleHead = id;
}

/* Mixer thread, after mix. Splices recycled ids onto the free chains. */
static void Om_PublishRecycle(void)
{
	std::lock_guard<std::mutex> lock(s_apiMutex);
	if (s_handleRecycleHead != OM_ID_NONE) {
		int id = s_handleRecycleHead;
		int tail = id;
		for (;;) {
			omHandle_t *h = Om_HandleAt(tail);
			h->used.store(0, std::memory_order_relaxed);
			if (h->nextFree == OM_ID_NONE) {
				break;
			}
			tail = h->nextFree;
		}
		Om_HandleAt(tail)->nextFree = s_handleFreeHead;
		s_handleFreeHead = id;
		s_handleRecycleHead = OM_ID_NONE;
	}
	if (s_soundRecycleHead != OM_ID_NONE) {
		int id = s_soundRecycleHead;
		int tail = id;
		for (;;) {
			omSoundSlot_t *s = Om_SoundSlot(tail);
			if (s->nextFree == OM_ID_NONE) {
				s->nextFree = s_soundFreeHead;
				break;
			}
			tail = s->nextFree;
		}
		s_soundFreeHead = id;
		s_soundRecycleHead = OM_ID_NONE;
	}
	if (s_voiceRecycleHead != OM_ID_NONE) {
		int id = s_voiceRecycleHead;
		int tail = id;
		for (;;) {
			omVoice_t *v = Om_VoiceAt(tail);
			if (v->nextFree == OM_ID_NONE) {
				v->nextFree = s_voiceFreeHead;
				break;
			}
			tail = v->nextFree;
		}
		s_voiceFreeHead = id;
		s_voiceRecycleHead = OM_ID_NONE;
	}
	if (!s_drainTrash.empty()) {
		s_soundTrash.insert(s_soundTrash.end(), s_drainTrash.begin(), s_drainTrash.end());
		s_drainTrash.clear();
	}
}

static void Om_Apply(const omCmd_t *c)
{
	omHandle_t *h;
	omVoice_t  *v = NULL;
	int         vi;

	switch (c->op) {
	case OM_OP_LISTENER:
		memcpy(s_lisPos, c->pos, sizeof(s_lisPos));
		memcpy(s_lisVel, c->vel, sizeof(s_lisVel));
		memcpy(s_lisFwd, c->fwd, sizeof(s_lisFwd));
		memcpy(s_lisLeft, c->left, sizeof(s_lisLeft));
		memcpy(s_lisUp, c->up, sizeof(s_lisUp));
		return;
	case OM_OP_MASTER:
		s_lisGain = c->gain;
		return;
	case OM_OP_RETIRE_SOUND:
		if (c->retire) {
			s_drainTrash.push_back(c->retire);
		}
		return;
	case OM_OP_FREE_SOUND:
		{
			omSoundSlot_t *slot = Om_SoundSlot(c->sound);
			if (slot) {
				omSound_t *old = slot->sound.exchange(NULL);
				const int cap = s_voiceCap.load(std::memory_order_acquire);
				Om_ForEachVoice(cap, [&](omVoice_t *voice) {
					if (voice->used && !voice->isStream && voice->sound == c->sound) {
						Om_Detach(voice);
						Om_RetireVoice(voice);
					}
				});
				if (old) {
					s_drainTrash.push_back(old);
				}
				Om_RecycleSound(c->sound);
			}
		}
		return;
	default:
		break;
	}

	h = Om_HandleAt(c->id);
	if (!h) {
		if (c->op == OM_OP_PLAY || c->op == OM_OP_OPEN) {
			Om_RecycleUnusedVoice(c->slot);
		}
		delete c->chunk;
		return;
	}
	if (h->dead) {
		/* Freed earlier in this or a past drain: only a fresh owner may PLAY / OPEN it. */
		if (c->op != OM_OP_PLAY && c->op != OM_OP_OPEN) {
			delete c->chunk;
			return;
		}
		h->dead = qfalse;
	}
	if (h->voice >= 0) {
		omVoice_t *live = Om_VoiceAt(h->voice);
		if (live && live->used && live->handle == c->id) {
			v = live;
		}
	}

	switch (c->op) {
	case OM_OP_PLAY:
	case OM_OP_OPEN:
		if (v && v->self == c->slot) {
			v->params   = c->params;
			v->isStream = c->op == OM_OP_OPEN ? qtrue : qfalse;
			v->sound    = v->isStream ? -1 : c->sound;
			v->pos      = (int)c->offset;
			v->state    = v->isStream ? OM_VOICE_INITIAL : OM_VOICE_PLAYING;
			h->voice    = v->self;
			h->state.store(v->state);
			break;
		}
		if (v) {
			Om_Release(v);
		}
		vi = c->slot;
		v = Om_VoiceAt(vi);
		if (!v || v->used) {
			h->voice = -1;
			h->state.store(OM_VOICE_STOPPED);
			break;
		}
		Om_ResetVoice(v);
		v->self     = vi;
		v->used     = qtrue;
		v->handle   = c->id;
		v->params   = c->params;
		v->isStream = c->op == OM_OP_OPEN ? qtrue : qfalse;
		v->sound    = v->isStream ? -1 : c->sound;
		v->pos      = (int)c->offset;
		v->state    = v->isStream ? OM_VOICE_INITIAL : OM_VOICE_PLAYING;
		h->voice    = vi;
		h->state.store(v->state);
		break;
	case OM_OP_UPDATE:
		if (v) {
			v->params = c->params;
		}
		break;
	case OM_OP_STOP:
		if (v && v->isStream) {
			/* Queued chunks all count as processed until unqueued, like an AL source. */
			v->pos   = Om_StreamFrames(v);
			v->frac  = 0;
			v->state = OM_VOICE_STOPPED;
			Om_Publish(v);
			h->state.store(OM_VOICE_STOPPED);
			break;
		}
		if (v) {
			Om_Release(v);
		}
		if (!h->stream) {
			Om_FreeHandle(c->id);
		}
		break;
	case OM_OP_PAUSE:
		if (v && v->state != OM_VOICE_STOPPED) {
			v->state = OM_VOICE_PAUSED;
		}
		h->state.store(v ? v->state : OM_VOICE_STOPPED);
		break;
	case OM_OP_RESUME:
		if (v) {
			v->state = OM_VOICE_PLAYING;
		}
		h->state.store(v ? OM_VOICE_PLAYING : OM_VOICE_STOPPED);
		break;
	case OM_OP_DESTROY:
		if (v) {
			Om_Release(v);
		}
		Om_FreeHandle(c->id);
		break;
	case OM_OP_SEEK:
		if (v) {
			v->pos  = (int)c->offset;
			v->frac = 0;
			Om_Publish(v);
		}
		break;
	case OM_OP_QUEUE:
		if (v && v->isStream && c->chunk && c->chunk->frames > 0) {
			v->chunks.push_back(c->chunk);
		} else {
			delete c->chunk;
		}
		break;
	case OM_OP_UNQUEUE:
		if (v && !v->chunks.empty()) {
			const int removed = v->chunks.front()->frames;
			delete v->chunks.front();
			v->chunks.pop_front();
			v->pos = v->pos >= removed ? v->pos - removed : 0;
			Om_Publish(v);
		}
		h->queued.fetch_sub(1);
		h->pendingUnqueue.fetch_sub(1);
		break;
	default:
		delete c->chunk;
		break;
	}
}

static void Om_Drain(void)
{
	size_t i;
	{
		std::lock_guard<std::mutex> lock(s_apiMutex);
		s_drain.swap(s_cmds);
	}
	if (s_drain.empty()) {
		return;
	}
	for (i = 0; i < s_drain.size(); i++) {
		Om_Apply(&s_drain[i]);
	}
	s_drain.clear();
}

static void Om_Push(const omCmd_t &c)
{
	std::lock_guard<std::mutex> lock(s_apiMutex);
	s_cmds.push_back(c);
}

static void Om_DataCallback(ma_device *dev, void *output, const void *input, ma_uint32 frameCount)
{
	(void)dev;
	(void)input;
	std::lock_guard<std::mutex> lock(s_mixMutex);
	if (s_captureActive) {
		memset(output, 0, sizeof(float) * s_outCh * (size_t)frameCount);
		return;
	}
	Om_Drain();
	Om_Mix((float *)output, (int)frameCount, s_outCh, qfalse);
	Om_PublishRecycle();
}

static void Om_LogDevices(void)
{
	ma_device_info *playback = NULL;
	ma_uint32       count    = 0, i;
	if (!s_contextReady || ma_context_get_devices(&s_context, &playback, &count, NULL, NULL) != MA_SUCCESS) {
		return;
	}
	Com_Printf("Sound/omaha: %u playback device(s):\n", (unsigned)count);
	for (i = 0; i < count; i++) {
		Com_Printf("  %s%s\n", playback[i].name, playback[i].isDefault ? " (default)" : "");
	}
}

static qboolean Om_FindDevice(const char *name, ma_device_id *id)
{
	ma_device_info *playback = NULL;
	ma_uint32       count    = 0, i;
	if (!name || !name[0] || !s_contextReady) {
		return qfalse;
	}
	if (ma_context_get_devices(&s_context, &playback, &count, NULL, NULL) != MA_SUCCESS) {
		return qfalse;
	}
	for (i = 0; i < count; i++) {
		if (!Q_stricmp(playback[i].name, name)) {
			*id = playback[i].id;
			return qtrue;
		}
	}
	Com_Printf("Sound/omaha: device '%s' not found, using default.\n", name);
	return qfalse;
}

static qboolean Om_LoadHrtf(void)
{
	cvar_t  *file = Cvar_Get("s_hrtf_file", SND_HRTF_DEFAULT_FILE, CVAR_ARCHIVE | CVAR_SOUND_LATCH);
	void    *data = NULL;
	long     size;
	qboolean ok;
	size = FS_ReadFile(file->string, &data);
	if (size <= 0 || !data) {
		Com_Printf("Sound/omaha: HRTF file '%s' not found; headphones fall back to stereo.\n", file->string);
		return qfalse;
	}
	ok = OmHrtf_Load(data, (int)size, s_sampleRate, file->string);
	FS_FreeFile(data);
	if (!ok) {
		Com_Printf("Sound/omaha: headphones fall back to stereo.\n");
	}
	return ok;
}

/* Added in Omaha: device callback length in ms (s_period_ms, 1..20). */
static int Om_PeriodMs(void)
{
	int ms = s_period_ms ? s_period_ms->integer : 5;
	if (ms < 1) {
		ms = 1;
	} else if (ms > 20) {
		ms = 20;
	}
	return ms;
}

static void Om_LogStarted(void)
{
	const ma_uint32 frames  = s_device.playback.internalPeriodSizeInFrames;
	const ma_uint32 periods = s_device.playback.internalPeriods;
	const ma_uint32 devRate = s_device.playback.internalSampleRate;
	const int       rate    = s_sampleRate > 0 ? s_sampleRate : (int)devRate;
	const float     ms      = rate > 0 ? (1000.f * (float)frames / (float)rate) : 0.f;
	const char     *backend = "unknown";

	if (s_nullDevice) {
		backend = "null";
	} else if (s_device.pContext) {
		backend = ma_get_backend_name(s_device.pContext->backend);
	}
	Com_Printf(
		"Sound/omaha: '%s' started (%d Hz, %s, %d ch, %s, requested %d ms x 3, granted %u frames / %.1f ms x %u)\n",
		s_nullDevice ? "null" : s_device.playback.name,
		s_sampleRate,
		s_hrtfOn ? "headphones" : (s_layout == 2 ? "5.1" : (s_layout == 3 ? "quad" : "stereo")),
		s_outCh,
		backend,
		Om_PeriodMs(),
		(unsigned)frames,
		ms,
		(unsigned)periods
	);
	if (devRate > 0 && (int)devRate != s_sampleRate) {
		Com_Printf("Sound/omaha: device rate %u Hz, mixer %d Hz\n", (unsigned)devRate, s_sampleRate);
	}
#ifdef MA_SUPPORT_WASAPI
	if (!s_nullDevice && s_device.pContext && s_device.pContext->backend == ma_backend_wasapi) {
		Com_Printf(
			"Sound/omaha: WASAPI buffer %u frames\n",
			(unsigned)s_device.wasapi.actualBufferSizeInFramesPlayback
		);
	}
#endif
}

static qboolean Om_OpenDevice(const sndDeviceConfig_t *cfg, qboolean nullBackend)
{
	ma_device_config dcfg;
	ma_device_id     devId;
	qboolean         haveId = qfalse;
	ma_result        res;
	ma_backend       nulls[1];

	if (s_contextReady) {
		ma_context_uninit(&s_context);
		s_contextReady = qfalse;
	}
	if (nullBackend) {
		nulls[0] = ma_backend_null;
		res       = ma_context_init(nulls, 1, NULL, &s_context);
	} else {
		res = ma_context_init(NULL, 0, NULL, &s_context);
	}
	if (res != MA_SUCCESS) {
		return qfalse;
	}
	s_contextReady = qtrue;
	if (!nullBackend && developer && developer->integer) {
		Om_LogDevices();
	}
	haveId = nullBackend ? qfalse : Om_FindDevice(cfg ? cfg->deviceName : NULL, &devId);
	dcfg                    = ma_device_config_init(ma_device_type_playback);
	dcfg.playback.format    = ma_format_f32;
	dcfg.playback.channels  = (ma_uint32)s_outCh;
	dcfg.playback.pChannelMap = s_map;
	dcfg.playback.pDeviceID = haveId ? &devId : NULL;
	dcfg.sampleRate         = (ma_uint32)s_sampleRate;
	dcfg.dataCallback       = Om_DataCallback;
	/* Changed in Omaha: s_period_ms x 3. HRTF stays on 128-sample blocks via the mix stash. */
	dcfg.periodSizeInMilliseconds = (ma_uint32)Om_PeriodMs();
	dcfg.periods            = 3;
	res = ma_device_init(&s_context, &dcfg, &s_device);
	if (res != MA_SUCCESS) {
		ma_context_uninit(&s_context);
		s_contextReady = qfalse;
		return qfalse;
	}
	res = ma_device_start(&s_device);
	if (res != MA_SUCCESS) {
		ma_device_uninit(&s_device);
		ma_context_uninit(&s_context);
		s_contextReady = qfalse;
		return qfalse;
	}
	s_deviceReady = qtrue;
	s_nullDevice  = nullBackend;
	return qtrue;
}

static void Om_FreeSoundTables(void)
{
	omSoundChunk_t *c = s_soundChunks;
	while (c) {
		omSoundChunk_t *next = c->next;
		int b;
		for (b = 0; b < OM_CHUNK_BLOCKS; b++) {
			omSoundBlock_t *blk = c->block[b];
			int s;
			if (!blk) {
				continue;
			}
			for (s = 0; s < OM_BLOCK; s++) {
				delete blk->slot[s].sound.exchange(NULL, std::memory_order_relaxed);
			}
			delete blk;
		}
		delete c;
		c = next;
	}
	s_soundChunks = NULL;
	s_soundCap.store(0, std::memory_order_relaxed);
	s_soundFreeHead = OM_ID_NONE;
	s_soundRecycleHead = OM_ID_NONE;
}

static void Om_FreeHandleTables(void)
{
	omHandleChunk_t *c = s_handleChunks;
	while (c) {
		omHandleChunk_t *next = c->next;
		int b;
		for (b = 0; b < OM_CHUNK_BLOCKS; b++) {
			delete c->block[b];
		}
		delete c;
		c = next;
	}
	s_handleChunks = NULL;
	s_handleCap.store(0, std::memory_order_relaxed);
	s_handleFreeHead = OM_ID_NONE;
	s_handleRecycleHead = OM_ID_NONE;
}

static void Om_FreeVoiceTables(void)
{
	omVoiceChunk_t *c = s_voiceChunks;
	omRankRetired_t *old;
	while (c) {
		omVoiceChunk_t *next = c->next;
		int b;
		for (b = 0; b < OM_CHUNK_BLOCKS; b++) {
			omVoiceBlock_t *blk = c->block[b];
			int s;
			if (!blk) {
				continue;
			}
			for (s = 0; s < OM_BLOCK; s++) {
				Om_ResetVoice(&blk->slot[s]);
			}
			delete blk;
		}
		delete c;
		c = next;
	}
	s_voiceChunks = NULL;
	s_voiceCap.store(0, std::memory_order_relaxed);
	s_voiceFreeHead = OM_ID_NONE;
	s_voiceRecycleHead = OM_ID_NONE;
	delete[] s_rank.exchange(NULL, std::memory_order_relaxed);
	delete[] s_rankG.exchange(NULL, std::memory_order_relaxed);
	old = s_rankRetired;
	s_rankRetired = NULL;
	while (old) {
		omRankRetired_t *next = old->next;
		delete[] old->rank;
		delete[] old->gain;
		delete old;
		old = next;
	}
}

static void Om_ResetState(void)
{
	int i;
	Om_FreeVoiceTables();
	Om_FreeSoundTables();
	for (i = 0; i < (int)s_soundTrash.size(); i++) {
		delete s_soundTrash[i];
	}
	s_soundTrash.clear();
	Om_FreeHandleTables();
	for (i = 0; i < (int)s_cmds.size(); i++) {
		delete s_cmds[i].chunk;
		delete s_cmds[i].retire;
	}
	s_cmds.clear();
	s_drain.clear();
	for (i = 0; i < (int)s_drainTrash.size(); i++) {
		delete s_drainTrash[i];
	}
	s_drainTrash.clear();
	s_stashPos  = 0;
	s_stashLen  = 0;
	s_ambiFlush = 0;
}

static qboolean Om_Init(const sndDeviceConfig_t *cfg)
{
	int speaker = cfg ? cfg->speakerType : 0;
	s_sampleRate = cfg && cfg->sampleRate > 0 ? cfg->sampleRate : 44100;
	s_rearA      = 1.0f - expf(-2.0f * (float)M_PI * OM_REAR_SHELF_HZ / (float)s_sampleRate);
	s_hrtfOn     = qfalse;
	s_layout     = 0;
	OmHrtf_Unload();
	if (speaker == 1) {
		s_hrtfOn = Om_LoadHrtf();
		s_layout = s_hrtfOn ? 1 : 0;
	} else if (speaker == 2) {
		s_layout = 2;
	} else if (speaker == 3) {
		s_layout = 3;
	}
	Om_SetupSpeakers();
	Om_BuildSinc();
	Om_BuildStereoPair();
	/* Added in Omaha: playback callback length in ms. Takes effect on snd_restart. */
	s_period_ms         = Cvar_Get("s_period_ms", "5", CVAR_ARCHIVE | CVAR_SOUND_LATCH);
	Cvar_CheckRange(s_period_ms, 1, 20, qtrue);
	s_limiter           = Cvar_Get("s_limiter", "1", CVAR_ARCHIVE);
	s_limiter_threshold = Cvar_Get("s_limiter_threshold", "-3.6", CVAR_ARCHIVE);
	s_obstruction       = Cvar_Get("s_obstruction", "1", CVAR_ARCHIVE);
	s_hf_duck           = Cvar_Get("s_hf_duck", "1", CVAR_ARCHIVE);
	s_hf_duck_db        = Cvar_Get("s_hf_duck_db", "24", CVAR_ARCHIVE);
	s_hf_duck_thresh    = Cvar_Get("s_hf_duck_thresh", "-15", CVAR_ARCHIVE);
	s_hf_duck_sc_boost  = Cvar_Get("s_hf_duck_sc_boost", "5", CVAR_ARCHIVE);
	s_mix               = Cvar_Get("s_mix", "1", CVAR_ARCHIVE);
	Om_LimiterReset();
	Om_SpkEqReset();
	Om_HpEqReset();
	Om_HfReset();
	Om_ResetState();
	/* Command queue only. Id tables grow on the game thread, not from this reserve. */
	s_cmds.reserve(1024);
	s_drain.reserve(1024);
	s_soundTrash.reserve(256);
	s_drainTrash.reserve(256);
	s_lisGain       = 1.f;
	s_masterCur     = 1.f;
	s_masterPrimed  = qfalse;
	s_captureActive = qfalse;
	if (!Om_OpenDevice(cfg, qfalse)) {
		Com_Printf("Sound/omaha: no playback device, using a null device.\n");
		if (!Om_OpenDevice(cfg, qtrue)) {
			Com_Printf("Sound/omaha: null device failed\n");
			return qfalse;
		}
	}
	Om_LogStarted();
	return qtrue;
}

static void Om_Shutdown(void)
{
	if (s_deviceReady) {
		ma_device_uninit(&s_device);
		s_deviceReady = qfalse;
	}
	if (s_contextReady) {
		ma_context_uninit(&s_context);
		s_contextReady = qfalse;
	}
	{
		std::lock_guard<std::mutex> mix(s_mixMutex);
		std::lock_guard<std::mutex> api(s_apiMutex);
		Om_ResetState();
	}
	OmHrtf_Unload();
	s_hrtfOn = qfalse;
}

/* Game thread: delete sounds the mixer has let go of. */
static void Om_EmptyTrash(void)
{
	std::vector<omSound_t *> trash;
	size_t                   i;
	{
		std::lock_guard<std::mutex> lock(s_apiMutex);
		if (s_soundTrash.empty()) {
			return;
		}
		trash.swap(s_soundTrash);
		s_soundTrash.reserve(256);
	}
	for (i = 0; i < trash.size(); i++) {
		delete trash[i];
	}
}

static omSoundId_t Om_CreateSound(void)
{
	omSoundId_t id = OM_SOUND_NONE;
	Om_EmptyTrash();
	std::lock_guard<std::mutex> lock(s_apiMutex);
	id = Om_TakeSound();
	return id;
}

static void Om_DestroySound(omSoundId_t id)
{
	omCmd_t c;
	if (!Om_SoundSlot(id)) {
		return;
	}
	memset(&c, 0, sizeof(c));
	c.op    = OM_OP_FREE_SOUND;
	c.sound = id;
	Om_Push(c);
}

static qboolean Om_UploadSound(omSoundId_t id, omSampleFormat_t fmt, const void *data, int sizeBytes, int rate, int blockAlign)
{
	omSound_t *s, *old;
	omSoundSlot_t *slot = Om_SoundSlot(id);
	if (!slot) {
		return qfalse;
	}
	s = Om_DecodeSound(fmt, data, sizeBytes, rate, blockAlign);
	if (!s) {
		return qfalse;
	}
	old = slot->sound.exchange(s, std::memory_order_acq_rel);
	if (old) {
		omCmd_t c;
		memset(&c, 0, sizeof(c));
		c.op     = OM_OP_RETIRE_SOUND;
		c.retire = old;
		Om_Push(c);
	}
	return qtrue;
}

static int Om_SoundRate(omSoundId_t id)
{
	const omSound_t *s;
	omSoundSlot_t *slot = Om_SoundSlot(id);
	if (!slot) {
		return 22050;
	}
	s = slot->sound.load(std::memory_order_acquire);
	return s && s->rate > 0 ? s->rate : 22050;
}

/* Call with s_apiMutex held. */
static int Om_AllocHandleLocked(qboolean stream)
{
	omHandle_t *h;
	int         id = Om_TakeHandle();
	if (id == OM_VOICE_NONE) {
		return OM_VOICE_NONE;
	}
	h         = Om_HandleAt(id);
	h->stream = stream;
	h->voice  = -1;
	h->dead   = qfalse;
	h->state.store(stream ? OM_VOICE_INITIAL : OM_VOICE_PLAYING);
	h->offset.store(0);
	h->queued.store(0);
	h->processed.store(0);
	h->pendingUnqueue.store(0);
	h->used.store(1);
	return id;
}

static qboolean Om_HandleLive(omVoiceId_t id)
{
	omHandle_t *h = Om_HandleAt(id);
	return h && h->used.load() ? qtrue : qfalse;
}

static omVoiceId_t Om_StartVoice(int op, omSoundId_t sound, const omVoiceParams_t *params, unsigned offset)
{
	omCmd_t c;
	int     id;
	memset(&c, 0, sizeof(c));
	c.op     = op;
	c.sound  = sound;
	c.offset = offset;
	if (params) {
		c.params = *params;
	} else {
		Om_DefaultParams(&c.params);
	}
	std::lock_guard<std::mutex> lock(s_apiMutex);
	id = Om_AllocHandleLocked(op == OM_OP_OPEN ? qtrue : qfalse);
	if (id == OM_VOICE_NONE) {
		return OM_VOICE_NONE;
	}
	c.slot = Om_TakeVoice();
	if (c.slot < 0) {
		Om_ReturnHandle(id);
		return OM_VOICE_NONE;
	}
	Om_HandleAt(id)->offset.store(offset);
	c.id = id;
	s_cmds.push_back(c);
	return id;
}

static omVoiceId_t Om_PlayVoice(omSoundId_t sound, const omVoiceParams_t *params, unsigned offset)
{
	if (!Om_SoundSlot(sound)) {
		return OM_VOICE_NONE;
	}
	return Om_StartVoice(OM_OP_PLAY, sound, params, offset);
}

static omVoiceId_t Om_OpenStream(const omVoiceParams_t *params)
{
	return Om_StartVoice(OM_OP_OPEN, OM_SOUND_NONE, params, 0);
}

static void Om_VoiceCmd(int op, omVoiceId_t id, int state)
{
	omCmd_t c;
	omHandle_t *h;
	if (!Om_HandleLive(id)) {
		return;
	}
	h = Om_HandleAt(id);
	memset(&c, 0, sizeof(c));
	c.op = op;
	c.id = id;
	c.slot = OM_ID_NONE;
	if (state >= 0 && h) {
		h->state.store(state);
	}
	Om_Push(c);
}

static void Om_UpdateVoice(omVoiceId_t id, const omVoiceParams_t *params)
{
	omCmd_t c;
	if (!params || !Om_HandleLive(id)) {
		return;
	}
	memset(&c, 0, sizeof(c));
	c.op     = OM_OP_UPDATE;
	c.id     = id;
	c.slot   = OM_ID_NONE;
	c.params = *params;
	Om_Push(c);
}

static void Om_StopVoice(omVoiceId_t id)
{
	Om_VoiceCmd(OM_OP_STOP, id, OM_VOICE_STOPPED);
}

static void Om_PauseVoice(omVoiceId_t id)
{
	Om_VoiceCmd(OM_OP_PAUSE, id, OM_VOICE_PAUSED);
}

static void Om_ResumeVoice(omVoiceId_t id)
{
	Om_VoiceCmd(OM_OP_RESUME, id, OM_VOICE_PLAYING);
}

static void Om_DestroyVoice(omVoiceId_t id)
{
	Om_VoiceCmd(OM_OP_DESTROY, id, OM_VOICE_STOPPED);
}

static int Om_VoiceState(omVoiceId_t id)
{
	omHandle_t *h = Om_HandleAt(id);
	if (!h || !h->used.load()) {
		return OM_VOICE_STOPPED;
	}
	return h->state.load();
}

static unsigned Om_GetOffset(omVoiceId_t id)
{
	omHandle_t *h = Om_HandleAt(id);
	if (!h || !h->used.load()) {
		return 0;
	}
	return h->offset.load(std::memory_order_relaxed);
}

static void Om_SetOffset(omVoiceId_t id, unsigned offset)
{
	omCmd_t c;
	if (!Om_HandleLive(id)) {
		return;
	}
	memset(&c, 0, sizeof(c));
	c.op     = OM_OP_SEEK;
	c.id     = id;
	c.slot   = OM_ID_NONE;
	c.offset = offset;
	Om_HandleAt(id)->offset.store(offset, std::memory_order_relaxed);
	Om_Push(c);
}

static void Om_QueueStream(omVoiceId_t id, const void *pcm, int bytes, omSampleFormat_t fmt, int rate, int blockAlign)
{
	omCmd_t    c;
	omChunk_t *chunk;
	if (!Om_HandleLive(id)) {
		return;
	}
	chunk = Om_MakeChunk(fmt, pcm, bytes, rate, blockAlign);
	if (!chunk) {
		return;
	}
	if (chunk->frames <= 0) {
		delete chunk;
		return;
	}
	memset(&c, 0, sizeof(c));
	c.op    = OM_OP_QUEUE;
	c.id    = id;
	c.slot  = OM_ID_NONE;
	c.chunk = chunk;
	Om_HandleAt(id)->queued.fetch_add(1);
	Om_Push(c);
}

static int Om_StreamQueued(omVoiceId_t id)
{
	int q;
	omHandle_t *h = Om_HandleAt(id);
	if (!h || !h->used.load()) {
		return 0;
	}
	q = h->queued.load() - h->pendingUnqueue.load();
	return q > 0 ? q : 0;
}

static int Om_StreamProcessed(omVoiceId_t id)
{
	int p;
	omHandle_t *h = Om_HandleAt(id);
	if (!h || !h->used.load()) {
		return 0;
	}
	p = h->processed.load() - h->pendingUnqueue.load();
	return p > 0 ? p : 0;
}

static void Om_UnqueueStream(omVoiceId_t id, int count)
{
	int i;
	if (!Om_HandleLive(id)) {
		return;
	}
	for (i = 0; i < count; i++) {
		omCmd_t c;
		memset(&c, 0, sizeof(c));
		c.op = OM_OP_UNQUEUE;
		c.id = id;
		c.slot = OM_ID_NONE;
		Om_HandleAt(id)->pendingUnqueue.fetch_add(1);
		Om_Push(c);
	}
}

static void Om_SetListener(
	const float pos[3], const float vel[3], const float forward[3], const float left[3], const float up[3]
)
{
	omCmd_t c;
	memset(&c, 0, sizeof(c));
	c.op = OM_OP_LISTENER;
	memcpy(c.pos, pos, sizeof(c.pos));
	if (vel) {
		memcpy(c.vel, vel, sizeof(c.vel));
	}
	memcpy(c.fwd, forward, sizeof(c.fwd));
	memcpy(c.left, left, sizeof(c.left));
	memcpy(c.up, up, sizeof(c.up));
	Om_Push(c);
}

static void Om_SetListenerGain(float g)
{
	omCmd_t c;
	memset(&c, 0, sizeof(c));
	c.op   = OM_OP_MASTER;
	c.gain = g;
	Om_Push(c);
}

static void Om_GetListenerPosition(float out[3])
{
	size_t i;
	std::lock_guard<std::mutex> lock(s_apiMutex);
	/* The newest queued listener wins over the one the mixer last applied. */
	memcpy(out, s_lisPos, sizeof(s_lisPos));
	for (i = 0; i < s_cmds.size(); i++) {
		if (s_cmds[i].op == OM_OP_LISTENER) {
			memcpy(out, s_cmds[i].pos, sizeof(s_lisPos));
		}
	}
}

static qboolean Om_BeginCapture(int sampleRate)
{
	(void)sampleRate;
	{
		std::lock_guard<std::mutex> lock(s_mixMutex);
		if (s_captureActive) {
			return qtrue;
		}
		Om_LimiterReset();
		s_stashPos      = 0;
		s_stashLen      = 0;
		s_captureActive = qtrue;
	}
	/* Outside the lock: stopping waits for the callback, which takes s_mixMutex. */
	if (s_deviceReady) {
		ma_device_stop(&s_device);
	}
	return qtrue;
}

static int Om_RenderCapture(float *interleavedStereo, int frames)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	if (!s_captureActive || !interleavedStereo || frames <= 0) {
		return 0;
	}
	Om_Drain();
	Om_Mix(interleavedStereo, frames, 2, qtrue);
	Om_PublishRecycle();
	return frames;
}

static void Om_EndCapture(void)
{
	{
		std::lock_guard<std::mutex> lock(s_mixMutex);
		if (!s_captureActive) {
			return;
		}
		s_stashPos      = 0;
		s_stashLen      = 0;
		s_captureActive = qfalse;
	}
	if (s_deviceReady) {
		ma_device_start(&s_device);
	}
}

static qboolean Om_CaptureActive(void)
{
	return s_captureActive;
}

static int Om_CaptureSampleRate(void)
{
	return s_sampleRate;
}

static void Om_Update(void)
{
	Om_EmptyTrash();
}

static const sndRenderer_t s_omahaRenderer = {
	"omaha",
	Om_Init,
	Om_Shutdown,
	Om_CreateSound,
	Om_DestroySound,
	Om_UploadSound,
	Om_SoundRate,
	Om_PlayVoice,
	Om_OpenStream,
	Om_UpdateVoice,
	Om_StopVoice,
	Om_PauseVoice,
	Om_ResumeVoice,
	Om_DestroyVoice,
	Om_VoiceState,
	Om_GetOffset,
	Om_SetOffset,
	Om_QueueStream,
	Om_StreamQueued,
	Om_StreamProcessed,
	Om_UnqueueStream,
	Om_SetListener,
	Om_SetListenerGain,
	Om_GetListenerPosition,
	Om_BeginCapture,
	Om_RenderCapture,
	Om_EndCapture,
	Om_CaptureActive,
	Om_CaptureSampleRate,
	Om_Update,
};

extern "C" const sndRenderer_t *S_Renderer_Omaha(void)
{
	return &s_omahaRenderer;
}

extern "C" float S_Omaha_HfDuckGainDb(void)
{
	return s_hfMeterDb;
}

extern "C" float S_Omaha_HfDuckMaxCutDb(void)
{
	float maxCut = s_hf_duck_db ? s_hf_duck_db->value : OM_HF_DUCK_DEFAULT_DB;
	if (maxCut < 0.f) {
		maxCut = -maxCut;
	}
	if (maxCut > 24.f) {
		maxCut = 24.f;
	}
	return maxCut;
}

extern "C" float S_Omaha_HfDuckSidechainDb(void)
{
	return s_hfSideDb;
}

extern "C" float S_Omaha_HfDuckThreshDb(void)
{
	return s_hf_duck_thresh ? s_hf_duck_thresh->value : OM_HF_DUCK_THRESH_DEFAULT;
}
