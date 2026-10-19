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
 * Omaha mixer. Device I/O is miniaudio. Voices resample with a 4-point Hermite,
 * pan with pairwise speaker gains (stereo, quad, 5.1) or the headphone HRTF,
 * then a shared limiter, EQ and HF duck.
 *
 * The game thread only enqueues commands. The device callback and capture pull
 * are the only threads that mix, and they take s_mixMutex so they cannot overlap.
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

#define OM_GUARD       3
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

#define OM_REAR_GAIN     0.75f
#define OM_REAR_SHELF_HZ 3000.0f
#define OM_REAR_SHELF_HF 0.5f
#define OM_REAR_SMOOTH   0.08f

enum {
	OM_OP_PLAY = 1,
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
	OM_OP_FREE_SOUND
};

struct omSound_t {
	qboolean           used;
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

struct omVoice_t {
	qboolean        used;
	int             state;
	int             handle;
	int             sound;
	qboolean        isStream;
	omVoiceParams_t params;
	int             pos;
	unsigned        frac;
	unsigned        step;
	float           curG[OM_OUT_MAX];
	float           tgtG[OM_OUT_MAX];
	int             ramp;
	int             nOut;
	float           rear;
	float           rearZ;
	float           lpZ[2];
	float           obstruct;
	qboolean        direct;
	float           dir[3];
	omHrtfVoice_t  *hrtf;
	std::deque<omChunk_t> chunks;
	int             chunk;
	qboolean        ending;
};

struct omHandle_t {
	omHandle_t()
		: used(qfalse), voice(-1), state(OM_VOICE_STOPPED), offset(0), queued(0), processed(0), pendingUnqueue(0)
	{
	}
	qboolean              used;
	int                   voice;
	std::atomic<int>      state;
	std::atomic<unsigned> offset;
	std::atomic<int>      queued;
	std::atomic<int>      processed;
	std::atomic<int>      pendingUnqueue;
};

struct omCmd_t {
	int             op;
	int             id;
	int             sound;
	unsigned        offset;
	omVoiceParams_t params;
	omChunk_t      *chunk;
	float           pos[3];
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

static std::vector<omSound_t>  s_sounds;
static std::vector<omVoice_t>  s_voices;
static std::deque<omHandle_t> s_handles;
static std::vector<omCmd_t>    s_cmds;
static std::mutex              s_apiMutex;
static std::mutex              s_mixMutex;

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
static float s_lisFwd[3]  = {1, 0, 0};
static float s_lisLeft[3] = {0, 1, 0};
static float s_lisUp[3]   = {0, 0, 1};
static float s_lisGain    = 1.f;
static float s_masterCur  = 1.f;
static qboolean s_masterPrimed;

static float s_bus[OM_OUT_MAX][OM_LINE];
static float s_ambi[OM_AMBI_CHANNELS][OM_LINE];
static float s_dry[2][OM_LINE];
static int   s_ambiFlush;

static float s_spkAz[OM_OUT_MAX];
static int   s_spkCh[OM_OUT_MAX];
static int   s_spkN;
static int   s_frontL;
static int   s_frontR;

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

static void Om_StorePlanar(omSound_t *s, const float *interleaved, int channels, int frames)
{
	int c, i;
	s->channels = channels;
	s->frames   = frames;
	for (c = 0; c < channels && c < 2; c++) {
		s->planar[c].assign((size_t)frames + (size_t)OM_GUARD * 2, 0.f);
		for (i = 0; i < frames; i++) {
			s->planar[c][(size_t)i + OM_GUARD] = interleaved[i * channels + c];
		}
	}
}

static qboolean Om_UploadNow(omSound_t *s, omSampleFormat_t fmt, const void *data, int sizeBytes, int rate, int blockAlign)
{
	omPcmDecode_t pcm;
	if (!OmPcm_Decode(fmt, data, sizeBytes, blockAlign, &pcm)) {
		return qfalse;
	}
	s->rate = rate > 0 ? rate : 22050;
	Om_StorePlanar(s, pcm.interleaved, pcm.channels, pcm.frames);
	OmPcm_Free(&pcm);
	s->used = qtrue;
	return s->frames > 0;
}

static omChunk_t *Om_MakeChunk(omSampleFormat_t fmt, const void *data, int sizeBytes, int rate, int blockAlign)
{
	omPcmDecode_t pcm;
	omChunk_t    *chunk;
	omSound_t     tmp;
	if (!OmPcm_Decode(fmt, data, sizeBytes, blockAlign, &pcm)) {
		return NULL;
	}
	chunk = new (std::nothrow) omChunk_t();
	if (!chunk) {
		OmPcm_Free(&pcm);
		return NULL;
	}
	memset(&tmp, 0, sizeof(tmp));
	Om_StorePlanar(&tmp, pcm.interleaved, pcm.channels, pcm.frames);
	OmPcm_Free(&pcm);
	chunk->channels = tmp.channels;
	chunk->frames   = tmp.frames;
	chunk->rate     = rate > 0 ? rate : 22050;
	chunk->planar[0].swap(tmp.planar[0]);
	chunk->planar[1].swap(tmp.planar[1]);
	return chunk;
}

static void Om_PatchSeam(omChunk_t *prev, omChunk_t *next)
{
	int c, n, i;
	if (!prev || !next || prev->frames <= 0 || next->frames <= 0) {
		return;
	}
	n = OM_GUARD;
	if (n > prev->frames) {
		n = prev->frames;
	}
	if (n > next->frames) {
		n = next->frames;
	}
	for (c = 0; c < prev->channels && c < next->channels && c < 2; c++) {
		for (i = 0; i < n; i++) {
			prev->planar[c][(size_t)prev->frames + OM_GUARD + i] = next->planar[c][(size_t)OM_GUARD + i];
			next->planar[c][(size_t)OM_GUARD - 1 - i]             = prev->planar[c][(size_t)prev->frames + OM_GUARD - 1 - i];
		}
	}
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
	if (pitch > 8.f) {
		pitch = 8.f;
	}
	adv  = ((float)srcRate / (float)s_sampleRate) * pitch;
	step = lrintf(adv * (float)OM_FRAC_ONE);
	if (step < 1) {
		step = 1;
	}
	return (unsigned)step;
}

static float Om_Hermite(float p0, float p1, float p2, float p3, float t)
{
	const float t2 = t * t;
	const float t3 = t2 * t;
	return 0.5f
		* ((2.f * p1) + (-p0 + p2) * t + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * t2
		   + (-p0 + 3.f * p1 - 3.f * p2 + p3) * t3);
}

static float Om_Half(float p0, float p1, float p2, float p3)
{
	return (-p0 + 9.f * p1 + 9.f * p2 - p3) * (1.f / 16.f);
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

static const float *Om_StaticPtr(const omVoice_t *v, int ch)
{
	const omSound_t *s;
	if (v->sound < 0 || (size_t)v->sound >= s_sounds.size() || !s_sounds[v->sound].used) {
		return NULL;
	}
	s = &s_sounds[v->sound];
	if (ch >= s->channels || s->planar[ch].empty()) {
		return NULL;
	}
	return s->planar[ch].data() + OM_GUARD;
}

static int Om_StaticFrames(const omVoice_t *v)
{
	if (v->sound < 0 || (size_t)v->sound >= s_sounds.size() || !s_sounds[v->sound].used) {
		return 0;
	}
	return s_sounds[v->sound].frames;
}

static int Om_StaticChannels(const omVoice_t *v)
{
	if (v->sound < 0 || (size_t)v->sound >= s_sounds.size() || !s_sounds[v->sound].used) {
		return 0;
	}
	return s_sounds[v->sound].channels;
}

static int Om_StaticRate(const omVoice_t *v)
{
	if (v->sound < 0 || (size_t)v->sound >= s_sounds.size() || !s_sounds[v->sound].used) {
		return s_sampleRate;
	}
	return s_sounds[v->sound].rate > 0 ? s_sounds[v->sound].rate : s_sampleRate;
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
	int i = 0;
	const qboolean ratio1 = step == OM_FRAC_ONE && *frac == 0;
	const qboolean ratio2 = step == (OM_FRAC_ONE / 2) && *frac == 0;
	const qboolean ratio4 = step == (OM_FRAC_ONE / 4) && *frac == 0;
	qboolean       fast   = qtrue;
	if (loop) {
		int need = *pos + (int)(((uint64_t)step * (uint64_t)count) >> 16) + 3;
		fast     = *pos > a && need < b;
	}
	if (fast && ratio1) {
		for (; i < count; i++) {
			int p = *pos + i;
			dst[i] = (p >= 0 && p < frames) || loop ? Om_StaticAt(s, frames, p, loop, a, b) : 0.f;
		}
		*pos += count;
		if (loop) {
			*pos = Om_Wrap(*pos, a, b);
		}
		return;
	}
	if (fast && ratio2) {
		int p = *pos;
		for (; i + 1 < count; i += 2) {
			float p0 = Om_StaticAt(s, frames, p - 1, loop, a, b);
			float p1 = Om_StaticAt(s, frames, p, loop, a, b);
			float p2 = Om_StaticAt(s, frames, p + 1, loop, a, b);
			float p3 = Om_StaticAt(s, frames, p + 2, loop, a, b);
			dst[i]     = p1;
			dst[i + 1] = Om_Half(p0, p1, p2, p3);
			p++;
		}
		*pos = p;
		if (i < count) {
			dst[i] = Om_StaticAt(s, frames, *pos, loop, a, b);
			*frac  = OM_FRAC_ONE / 2;
		}
		if (loop) {
			*pos = Om_Wrap(*pos, a, b);
		}
		return;
	}
	if (fast && ratio4) {
		static const float c0[3] = {-0.0703125f, -0.0625f, -0.0234375f};
		static const float c1[3] = {0.9921875f, 0.5625f, 0.2265625f};
		static const float c2[3] = {0.2265625f, 0.5625f, 0.9921875f};
		static const float c3[3] = {-0.0234375f, -0.0625f, -0.0703125f};
		int                p     = *pos;
		while (i + 3 < count) {
			float p0 = Om_StaticAt(s, frames, p - 1, loop, a, b);
			float p1 = Om_StaticAt(s, frames, p, loop, a, b);
			float p2 = Om_StaticAt(s, frames, p + 1, loop, a, b);
			float p3 = Om_StaticAt(s, frames, p + 2, loop, a, b);
			int   k;
			dst[i++] = p1;
			for (k = 0; k < 3; k++) {
				dst[i++] = c0[k] * p0 + c1[k] * p1 + c2[k] * p2 + c3[k] * p3;
			}
			p++;
		}
		*pos = p;
		while (i < count) {
			float t = (float)(*frac) / (float)OM_FRAC_ONE;
			dst[i++] = Om_Hermite(
				Om_StaticAt(s, frames, *pos - 1, loop, a, b), Om_StaticAt(s, frames, *pos, loop, a, b),
				Om_StaticAt(s, frames, *pos + 1, loop, a, b), Om_StaticAt(s, frames, *pos + 2, loop, a, b), t
			);
			*frac += step;
			*pos += (int)(*frac >> 16);
			*frac &= OM_FRAC_ONE - 1;
		}
		if (loop) {
			*pos = Om_Wrap(*pos, a, b);
		}
		return;
	}
	for (i = 0; i < count; i++) {
		float t = (float)(*frac) / (float)OM_FRAC_ONE;
		dst[i]  = Om_Hermite(
            Om_StaticAt(s, frames, *pos - 1, loop, a, b), Om_StaticAt(s, frames, *pos, loop, a, b),
            Om_StaticAt(s, frames, *pos + 1, loop, a, b), Om_StaticAt(s, frames, *pos + 2, loop, a, b), t
        );
		*frac += step;
		*pos += (int)(*frac >> 16);
		*frac &= OM_FRAC_ONE - 1;
		if (loop) {
			*pos = Om_Wrap(*pos, a, b);
		}
	}
}

static const omChunk_t *Om_ChunkAt(const omVoice_t *v, int *local)
{
	int left = *local;
	int i;
	for (i = 0; i < (int)v->chunks.size(); i++) {
		if (left < v->chunks[i].frames) {
			*local = left;
			return &v->chunks[i];
		}
		left -= v->chunks[i].frames;
	}
	*local = left;
	return NULL;
}

static float Om_StreamAt(const omVoice_t *v, int ch, int frame)
{
	int                local = frame;
	const omChunk_t   *c     = Om_ChunkAt(v, &local);
	if (!c || ch >= c->channels || c->planar[ch].empty()) {
		return 0.f;
	}
	if (local < 0 || local >= c->frames) {
		return 0.f;
	}
	return c->planar[ch][(size_t)local + OM_GUARD];
}

static int Om_StreamFrames(const omVoice_t *v)
{
	int n = 0;
	size_t i;
	for (i = 0; i < v->chunks.size(); i++) {
		n += v->chunks[i].frames;
	}
	return n;
}

static void Om_ResampleStream(omVoice_t *v, int ch, float *dst, int count)
{
	int i;
	for (i = 0; i < count; i++) {
		float t = (float)v->frac / (float)OM_FRAC_ONE;
		dst[i]  = Om_Hermite(
            Om_StreamAt(v, ch, v->pos - 1), Om_StreamAt(v, ch, v->pos), Om_StreamAt(v, ch, v->pos + 1),
            Om_StreamAt(v, ch, v->pos + 2), t
        );
		v->frac += v->step;
		v->pos += (int)(v->frac >> 16);
		v->frac &= OM_FRAC_ONE - 1;
	}
}

static void Om_DistanceGain(const omVoice_t *v, float *dist, float *scale, float dir[3])
{
	const omVoiceParams_t *p = &v->params;
	float                  rel[3], front, left, up, d, g;
	*scale = 1.f;
	*dist  = 0.f;
	dir[0] = 1.f;
	dir[1] = 0.f;
	dir[2] = 0.f;
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
	*dist  = d;
	if (d > 0.0001f) {
		dir[0] = front / d;
		dir[1] = left / d;
		dir[2] = up / d;
	}
	if (p->minDist > 0.f) {
		float c = Om_Clamp(d, p->minDist, p->maxDist > p->minDist ? p->maxDist : p->minDist);
		g       = p->minDist / c;
	} else {
		g = 1.f;
	}
	*scale = g;
}

static void Om_Pair(float front, float left, float dist, int *a, int *b, float *ga, float *gb, float *rear)
{
	const float az = atan2f(left, front);
	int         i;
	(void)dist;
	*a    = s_frontL;
	*b    = -1;
	*ga   = 1.f;
	*gb   = 0.f;
	*rear = 0.f;
	{
		const float horiz = sqrtf(front * front + left * left);
		if (horiz > 0.0001f && front < 0.f) {
			*rear = Om_Clamp(-front / horiz, 0.f, 1.f);
		}
	}
	if (s_layout == 0) {
		const float lim = s_spkAz[1];
		if (az > lim) {
			*a = s_frontL;
			return;
		}
		if (az < -lim) {
			*a = s_frontR;
			return;
		}
		{
			const float t = (az - (-lim)) / (lim * 2.f);
			*a            = s_frontR;
			*b            = s_frontL;
			*ga           = cosf(t * (float)(M_PI * 0.5));
			*gb           = sinf(t * (float)(M_PI * 0.5));
		}
		return;
	}
	for (i = 0; i < s_spkN; i++) {
		int   j   = (i + 1) % s_spkN;
		float a0  = s_spkAz[i];
		float a1  = s_spkAz[j];
		float ang = az;
		if (j == 0) {
			a1 += (float)(2.0 * M_PI);
		}
		if (ang < a0) {
			ang += (float)(2.0 * M_PI);
		}
		if (ang >= a0 && ang <= a1 && a1 > a0) {
			const float t = (ang - a0) / (a1 - a0);
			*a            = s_spkCh[i];
			*b            = s_spkCh[j];
			*ga           = cosf(t * (float)(M_PI * 0.5));
			*gb           = sinf(t * (float)(M_PI * 0.5));
			return;
		}
	}
}

static void Om_AddRamp(const float *in, float *bus, float *cur, float target, int *ramp, int count)
{
	int i = 0;
	if (*ramp > 0) {
		int   n    = *ramp < count ? *ramp : count;
		float from = *cur;
		for (; i < n; i++) {
			float t = (float)(i + 1) / (float)(*ramp);
			bus[i] += in[i] * (from + (target - from) * t);
		}
		*ramp -= n;
		if (*ramp <= 0) {
			*cur = target;
		} else {
			*cur = from + (target - from) * ((float)n / (float)(n + *ramp));
		}
	}
	if (fabsf(target) <= OM_SILENCE) {
		return;
	}
	for (; i < count; i++) {
		bus[i] += in[i] * target;
	}
	*cur = target;
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
}

static void Om_SilenceVoice(omVoice_t *v)
{
	v->used   = qfalse;
	v->state  = OM_VOICE_STOPPED;
	v->handle = -1;
	Om_FreeHrtf(v);
	v->chunks.clear();
}

static unsigned Om_VoiceOffset(const omVoice_t *v)
{
	if (v->pos < 0) {
		return 0;
	}
	return (unsigned)v->pos;
}

static void Om_Publish(omVoice_t *v)
{
	int passed, left;
	size_t i;
	if (v->handle < 0 || (size_t)v->handle >= s_handles.size()) {
		return;
	}
	s_handles[v->handle].offset.store(Om_VoiceOffset(v), std::memory_order_relaxed);
	if (!v->isStream) {
		return;
	}
	passed = 0;
	left   = v->pos;
	for (i = 0; i < v->chunks.size(); i++) {
		if (left < v->chunks[i].frames) {
			break;
		}
		passed++;
		left -= v->chunks[i].frames;
	}
	s_handles[v->handle].processed.store(passed, std::memory_order_relaxed);
}

static void Om_MixVoice(omVoice_t *v, int count, qboolean allowDirect)
{
	int                channels, frames, rate, a, b, c;
	qboolean           loop, ended;
	float              dist, distScale, base, dir[3];
	const float       *ptr;
	omVoiceParams_t   *p = &v->params;

	if (!v->used || v->state == OM_VOICE_PAUSED) {
		return;
	}
	channels = v->isStream ? (v->chunks.empty() ? 0 : v->chunks[v->chunk < (int)v->chunks.size() ? v->chunk : 0].channels) : Om_StaticChannels(v);
	if (channels < 1) {
		if (!v->isStream) {
			Om_SilenceVoice(v);
		}
		return;
	}
	if (channels > 2) {
		channels = 2;
	}
	frames = v->isStream ? Om_StreamFrames(v) : Om_StaticFrames(v);
	rate   = v->isStream ? (v->chunks.empty() ? s_sampleRate : v->chunks.front().rate) : Om_StaticRate(v);
	loop   = (!v->isStream && (p->flags & OM_FLAG_LOOP)) ? qtrue : qfalse;
	Om_LoopRange(v, frames, &a, &b);
	v->step = Om_StepFor(rate, p->pitch);
	Om_DistanceGain(v, &dist, &distScale, dir);
	base = Om_Clamp(p->gain, 0.f, OM_GAIN_MAX) * distScale;
	if (v->state == OM_VOICE_STOPPED && v->ramp <= 0) {
		Om_SilenceVoice(v);
		return;
	}
	if (v->ending) {
		base = 0.f;
	}

	/* Beyond max distance, or silent and not releasing a filter: move the cursor only. */
	if (!v->direct && base <= OM_SILENCE && v->ramp <= 0 && v->obstruct <= OM_OBSTRUCT_EPSILON && p->obstruction <= OM_OBSTRUCT_EPSILON) {
		uint64_t f = (uint64_t)v->frac + (uint64_t)v->step * (uint64_t)count;
		v->pos += (int)(f >> 16);
		v->frac = (unsigned)(f & (OM_FRAC_ONE - 1));
		if (loop) {
			v->pos = Om_Wrap(v->pos, a, b);
		} else if (!v->isStream && v->pos >= frames) {
			Om_SilenceVoice(v);
		}
		Om_Publish(v);
		return;
	}

	ended = qfalse;
	if (!v->isStream && !loop && v->pos >= frames) {
		ended = qtrue;
	}
	for (c = 0; c < channels; c++) {
		if (ended) {
			memset(s_dry[c], 0, sizeof(float) * (size_t)count);
			continue;
		}
		if (v->isStream) {
			int savedPos  = v->pos;
			unsigned savedFrac = v->frac;
			Om_ResampleStream(v, c, s_dry[c], count);
			if (c + 1 < channels) {
				v->pos  = savedPos;
				v->frac = savedFrac;
			}
		} else {
			int      savedPos  = v->pos;
			unsigned savedFrac = v->frac;
			ptr = Om_StaticPtr(v, c);
			if (!ptr) {
				memset(s_dry[c], 0, sizeof(float) * (size_t)count);
				continue;
			}
			Om_ResamplePtr(ptr, frames, &v->pos, &v->frac, v->step, loop, a, b, s_dry[c], count);
			if (c + 1 < channels) {
				v->pos  = savedPos;
				v->frac = savedFrac;
			}
		}
	}
	if (!v->isStream && !loop && v->pos >= frames) {
		v->ending = qtrue;
	}

	{
		qboolean doFilter = qfalse;
		float    lpA      = 1.f;
		float    smooth   = 1.f - expf(-(float)count / (OM_OBSTRUCT_SMOOTH_MS * 0.001f * (float)s_sampleRate));
		if (s_obstruction && s_obstruction->integer && !(p->flags & (OM_FLAG_2D | OM_FLAG_FLAT))) {
			float distT, occ, fc, mix, openHz, farHz, occHz;
			v->obstruct += (p->obstruction - v->obstruct) * smooth;
			mix   = OM_OBSTRUCT_MIX;
			distT = Om_Clamp(dist / OM_OBSTRUCT_DIST_RANGE, 0.f, 1.f);
			occ   = Om_Clamp(v->obstruct, 0.f, 1.f);
			openHz = OM_OBSTRUCT_OPEN_HZ;
			farHz  = OM_OBSTRUCT_CUTOFF_HZ;
			occHz  = OM_OBSTRUCT_OCC_BLOCKED_HZ;
			fc = Om_Lerp(Om_Lerp(openHz, farHz, distT), Om_Lerp(openHz, occHz, occ), mix);
			if ((distT >= OM_OBSTRUCT_EPSILON || occ >= OM_OBSTRUCT_EPSILON) && fc < (float)s_sampleRate * 0.45f) {
				float aSafe = fmaxf(1.f - expf(-2.f * (float)M_PI * fmaxf(fc, 20.f) / (float)s_sampleRate), 1.0e-4f);
				float makeup = sqrtf(sqrtf((2.f - aSafe) / aSafe));
				lpA          = aSafe;
				makeup       = Om_Clamp(makeup, 1.f, OM_OBSTRUCT_MAKEUP_MAX);
				base *= makeup;
				doFilter = qtrue;
			}
		} else {
			v->obstruct = 0.f;
		}
		if (doFilter) {
			for (c = 0; c < channels; c++) {
				Om_Filter(s_dry[c], count, lpA, &v->lpZ[c]);
			}
		}
	}

	if (s_hrtfOn && channels == 1 && !(p->flags & OM_FLAG_FLAT)) {
		float g = base;
		if (p->flags & OM_FLAG_PAN) {
			float pan = Om_Clamp(p->pan, -1.f, 1.f);
			/* pan is unused for HRTF dry; center it */
			(void)pan;
		}
		if ((p->route == OM_ROUTE_DRY) || (p->flags & OM_FLAG_2D)) {
			float x  = (p->flags & OM_FLAG_PAN) ? Om_Clamp(p->pan, -1.f, 1.f) : dir[1];
			float gL = sqrtf(0.5f * (1.f - x));
			float gR = sqrtf(0.5f * (1.f + x));
			/* dir[1] is +left, pan + is right, so flip pan into left-positive */
			if (p->flags & OM_FLAG_PAN) {
				x  = -Om_Clamp(p->pan, -1.f, 1.f);
				gL = sqrtf(0.5f * (1.f + x));
				gR = sqrtf(0.5f * (1.f - x));
			}
			if (v->nOut != 2) {
				v->ramp = OM_RAMP;
				v->nOut = 2;
			}
			v->tgtG[0] = g * gL;
			v->tgtG[1] = g * gR;
			if (v->ramp <= 0) {
				v->curG[0] = v->tgtG[0];
				v->curG[1] = v->tgtG[1];
			}
			Om_AddRamp(s_dry[0], s_bus[0], &v->curG[0], v->tgtG[0], &v->ramp, count);
			{
				int ramp2 = 0;
				v->curG[1] = v->tgtG[1];
				Om_AddRamp(s_dry[0], s_bus[1], &v->curG[1], v->tgtG[1], &ramp2, count);
			}
			Om_FreeHrtf(v);
			v->direct = qfalse;
		} else if (allowDirect && p->route == OM_ROUTE_DIRECT && dist > OM_HRTF_MIN_DIST) {
			if (!v->hrtf) {
				v->hrtf = OmHrtf_VoiceAlloc();
			}
			v->direct = qtrue;
			v->dir[0] = dir[0];
			v->dir[1] = dir[1];
			v->dir[2] = dir[2];
			if (v->ramp <= 0 && fabsf(v->curG[0] - g) > 0.0001f) {
				v->ramp = OM_RAMP;
			}
			v->tgtG[0] = g;
			{
				int i;
				float monoG = v->curG[0];
				int   ramp  = v->ramp;
				for (i = 0; i < count; i++) {
					float gain = g;
					if (ramp > 0) {
						gain = monoG + (g - monoG) * ((float)(i + 1) / (float)(ramp > count ? ramp : count));
					}
					s_dry[0][i] *= gain;
				}
				if (ramp > 0) {
					v->ramp = ramp > count ? ramp - count : 0;
				}
				v->curG[0] = g;
			}
			if (v->hrtf) {
				OmHrtf_VoiceProcess(v->hrtf, v->dir, s_dry[0], s_bus[0], s_bus[1], count);
			}
		} else {
			float coeff[OM_AMBI_CHANNELS];
			int   k;
			OmHrtf_AmbiCoeffs(dir, coeff);
			v->direct = qfalse;
			Om_FreeHrtf(v);
			s_ambiFlush = 8;
			for (k = 0; k < OM_AMBI_CHANNELS; k++) {
				int dummy = 0;
				float cur = coeff[k] * g;
				Om_AddRamp(s_dry[0], s_ambi[k], &cur, cur, &dummy, count);
			}
		}
	} else if (channels >= 2 || (p->flags & OM_FLAG_FLAT)) {
		float gL = base;
		float gR = base;
		if (channels == 1) {
			gL = gR = base * 0.7071f;
		}
		if (v->ramp <= 0) {
			v->curG[s_frontL] = gL;
			v->curG[s_frontR] = gR;
		}
		v->tgtG[s_frontL] = gL;
		v->tgtG[s_frontR] = gR;
		Om_AddRamp(s_dry[0], s_bus[s_frontL], &v->curG[s_frontL], gL, &v->ramp, count);
		{
			int ramp2 = 0;
			const float *src = channels >= 2 ? s_dry[1] : s_dry[0];
			Om_AddRamp(src, s_bus[s_frontR], &v->curG[s_frontR], gR, &ramp2, count);
		}
		Om_FreeHrtf(v);
	} else {
		int   ia, ib;
		float ga, gb, rear;
		float side = (p->flags & OM_FLAG_PAN) ? -p->pan : dir[1];
		float fr   = (p->flags & OM_FLAG_PAN) ? 1.f : dir[0];
		if (p->flags & OM_FLAG_PAN) {
			dist = 1.f;
		}
		Om_Pair(fr, side, dist > 0.f ? dist : 1.f, &ia, &ib, &ga, &gb, &rear);
		if (s_layout == 0) {
			v->rear += (rear - v->rear) * OM_REAR_SMOOTH;
			if (v->rear > 0.001f) {
				Om_Rear(s_dry[0], count, v->rear, &v->rearZ);
				base *= Om_Lerp(1.f, OM_REAR_GAIN, v->rear);
			}
		}
		v->tgtG[0] = base * ga;
		if (v->ramp <= 0) {
			v->curG[0] = v->tgtG[0];
		}
		if (ia >= 0) {
			Om_AddRamp(s_dry[0], s_bus[ia], &v->curG[0], base * ga, &v->ramp, count);
		}
		if (ib >= 0) {
			int ramp2 = 0;
			Om_AddRamp(s_dry[0], s_bus[ib], &v->curG[1], base * gb, &ramp2, count);
			v->curG[1] = base * gb;
		}
		Om_FreeHrtf(v);
		v->direct = qfalse;
	}

	if (v->ending && v->ramp <= 0) {
		Om_SilenceVoice(v);
	}
	Om_Publish(v);
}

static void Om_MixBlock(int count)
{
	size_t i;
	int    c, o;
	int    rank[128];
	float  rankG[128];
	int    nRank = 0;
	qboolean allow[256];

	for (o = 0; o < s_outCh; o++) {
		memset(s_bus[o], 0, sizeof(float) * (size_t)count);
	}
	for (o = 0; o < OM_AMBI_CHANNELS; o++) {
		memset(s_ambi[o], 0, sizeof(float) * (size_t)count);
	}
	memset(allow, 0, sizeof(allow));
	if (s_hrtfOn) {
		for (i = 0; i < s_voices.size(); i++) {
			omVoice_t *v = &s_voices[i];
			float      dist, scale, dir[3], g;
			if (!v->used || v->state == OM_VOICE_PAUSED) {
				continue;
			}
			if (v->params.route != OM_ROUTE_DIRECT || (v->params.flags & (OM_FLAG_2D | OM_FLAG_FLAT))) {
				continue;
			}
			Om_DistanceGain(v, &dist, &scale, dir);
			if (dist <= OM_HRTF_MIN_DIST) {
				continue;
			}
			g = v->params.gain * scale;
			if (nRank < 128) {
				rank[nRank]  = (int)i;
				rankG[nRank] = g;
				nRank++;
			}
		}
		for (c = 0; c < nRank; c++) {
			int   best = c;
			int   k;
			for (k = c + 1; k < nRank; k++) {
				if (rankG[k] > rankG[best]) {
					best = k;
				}
			}
			if (best != c) {
				int   ti = rank[c];
				float tg = rankG[c];
				rank[c]  = rank[best];
				rankG[c] = rankG[best];
				rank[best]  = ti;
				rankG[best] = tg;
			}
		}
		for (c = 0; c < nRank && c < OM_DIRECT_CAP; c++) {
			if (rank[c] >= 0 && rank[c] < 256) {
				allow[rank[c]] = qtrue;
			}
		}
	}
	for (i = 0; i < s_voices.size(); i++) {
		if (!s_voices[i].used) {
			continue;
		}
		Om_MixVoice(&s_voices[i], count, (i < 256 && allow[i]) ? qtrue : qfalse);
	}
	if (s_hrtfOn) {
		if (s_ambiFlush > 0) {
			float *ambi[OM_AMBI_CHANNELS];
			for (o = 0; o < OM_AMBI_CHANNELS; o++) {
				ambi[o] = s_ambi[o];
			}
			OmHrtf_AmbiDecode(ambi, s_bus[0], s_bus[1], count);
			s_ambiFlush--;
		}
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

static void Om_Mix(float *dst, int frames, int ch, qboolean stereoOut)
{
	OmFtzGuard ftz;
	while (frames > 0) {
		int n = frames < OM_LINE ? frames : OM_LINE;
		if (s_hrtfOn && n > OM_HRTF_BLOCK) {
			n = OM_HRTF_BLOCK;
		}
		if (s_hrtfOn && (n % OM_HRTF_BLOCK) != 0) {
			/* Short tail: mix what we can in one HRTF-sized block and keep the rest. */
			n = n < OM_HRTF_BLOCK ? n : OM_HRTF_BLOCK;
		}
		if (s_hrtfOn && n != OM_HRTF_BLOCK && n > 0) {
			/* Convolution wants whole blocks. Pad the mix, keep the remainder unmixed. */
			float pad[OM_HRTF_BLOCK * OM_OUT_MAX];
			int   keep = n;
			Om_MixBlock(OM_HRTF_BLOCK);
			if (stereoOut) {
				Om_Downmix(pad, OM_HRTF_BLOCK);
				Om_Master(pad, OM_HRTF_BLOCK, 2);
				memcpy(dst, pad, sizeof(float) * 2 * (size_t)keep);
				dst += keep * 2;
			} else {
				Om_Interleave(pad, OM_HRTF_BLOCK, ch);
				Om_Master(pad, OM_HRTF_BLOCK, ch);
				memcpy(dst, pad, sizeof(float) * (size_t)ch * (size_t)keep);
				dst += keep * ch;
			}
			frames -= keep;
			break;
		}
		Om_MixBlock(n);
		if (stereoOut) {
			float *block = dst;
			Om_Downmix(block, n);
			Om_Master(block, n, 2);
			dst += n * 2;
		} else {
			Om_Interleave(dst, n, ch);
			Om_Master(dst, n, ch);
			dst += n * ch;
		}
		frames -= n;
	}
}

static int Om_AllocVoiceSlot(void)
{
	size_t i;
	for (i = 0; i < s_voices.size(); i++) {
		if (!s_voices[i].used) {
			Om_FreeHrtf(&s_voices[i]);
			s_voices[i].chunks.clear();
			s_voices[i]        = omVoice_t();
			s_voices[i].used   = qtrue;
			s_voices[i].handle = -1;
			s_voices[i].sound  = -1;
			return (int)i;
		}
	}
	s_voices.push_back(omVoice_t());
	s_voices.back().used   = qtrue;
	s_voices.back().handle = -1;
	s_voices.back().sound  = -1;
	return (int)s_voices.size() - 1;
}

static void Om_Apply(const omCmd_t *c)
{
	omHandle_t *h;
	omVoice_t  *v;
	if (c->op == OM_OP_LISTENER) {
		memcpy(s_lisPos, c->pos, sizeof(s_lisPos));
		memcpy(s_lisFwd, c->fwd, sizeof(s_lisFwd));
		memcpy(s_lisLeft, c->left, sizeof(s_lisLeft));
		memcpy(s_lisUp, c->up, sizeof(s_lisUp));
		return;
	}
	if (c->op == OM_OP_MASTER) {
		s_lisGain = c->gain;
		return;
	}
	if (c->op == OM_OP_FREE_SOUND) {
		if (c->sound >= 0 && (size_t)c->sound < s_sounds.size()) {
			s_sounds[c->sound].used = qfalse;
			s_sounds[c->sound].planar[0].clear();
			s_sounds[c->sound].planar[1].clear();
			s_sounds[c->sound].frames = 0;
		}
		return;
	}
	if (c->id < 0 || (size_t)c->id >= s_handles.size() || !s_handles[c->id].used) {
		if (c->chunk) {
			delete c->chunk;
		}
		return;
	}
	h = &s_handles[c->id];
	v = (h->voice >= 0 && (size_t)h->voice < s_voices.size()) ? &s_voices[h->voice] : NULL;
	switch (c->op) {
	case OM_OP_PLAY:
	case OM_OP_UPDATE:
		if (!v || !v->used) {
			h->voice  = Om_AllocVoiceSlot();
			v         = &s_voices[h->voice];
			v->handle = c->id;
			v->sound  = c->sound;
			v->isStream = c->sound < 0;
			v->state  = OM_VOICE_PLAYING;
			v->pos    = (int)c->offset;
			v->frac   = 0;
			v->ramp   = OM_RAMP;
			memset(v->curG, 0, sizeof(v->curG));
		}
		v->params = c->params;
		v->sound  = c->sound >= 0 ? c->sound : v->sound;
		if (fabsf(v->curG[0] - c->params.gain) > 0.0001f) {
			v->ramp = OM_RAMP;
		}
		h->state.store(OM_VOICE_PLAYING, std::memory_order_relaxed);
		v->state = OM_VOICE_PLAYING;
		break;
	case OM_OP_STOP:
		if (v && v->used) {
			v->ending = qtrue;
			v->ramp   = OM_RAMP;
			v->handle = -1;
		}
		h->voice = -1;
		h->state.store(OM_VOICE_STOPPED, std::memory_order_relaxed);
		break;
	case OM_OP_PAUSE:
		if (v) {
			v->state = OM_VOICE_PAUSED;
		}
		h->state.store(OM_VOICE_PAUSED, std::memory_order_relaxed);
		break;
	case OM_OP_RESUME:
		if (v) {
			v->state = OM_VOICE_PLAYING;
		}
		h->state.store(OM_VOICE_PLAYING, std::memory_order_relaxed);
		break;
	case OM_OP_DESTROY:
		if (v && v->used) {
			Om_SilenceVoice(v);
		}
		h->voice = -1;
		h->used  = qfalse;
		h->state.store(OM_VOICE_STOPPED, std::memory_order_relaxed);
		if (c->chunk) {
			delete c->chunk;
		}
		break;
	case OM_OP_SEEK:
		if (v) {
			v->pos  = (int)c->offset;
			v->frac = 0;
			v->ramp = OM_RAMP;
			memset(v->curG, 0, sizeof(v->curG));
		}
		h->offset.store(c->offset, std::memory_order_relaxed);
		break;
	case OM_OP_QUEUE:
		if (c->chunk) {
			if (!v || !v->used) {
				h->voice    = Om_AllocVoiceSlot();
				v           = &s_voices[h->voice];
				v->handle   = c->id;
				v->isStream = qtrue;
				v->sound    = -1;
				v->state    = h->state.load(std::memory_order_relaxed);
				v->params   = c->params;
			}
			v->isStream = qtrue;
			if (!v->chunks.empty()) {
				Om_PatchSeam(&v->chunks.back(), c->chunk);
			}
			v->chunks.push_back(std::move(*c->chunk));
			delete c->chunk;
		}
		break;
	case OM_OP_UNQUEUE:
		if (v && !v->chunks.empty()) {
			int removed = v->chunks.front().frames;
			v->chunks.pop_front();
			if (v->pos >= removed) {
				v->pos -= removed;
			} else {
				v->pos = 0;
			}
		}
		h->queued.fetch_sub(1, std::memory_order_relaxed);
		h->pendingUnqueue.fetch_sub(1, std::memory_order_relaxed);
		break;
	default:
		if (c->chunk) {
			delete c->chunk;
		}
		break;
	}
}

static void Om_Drain(void)
{
	std::vector<omCmd_t> local;
	{
		std::lock_guard<std::mutex> lock(s_apiMutex);
		local.swap(s_cmds);
	}
	for (size_t i = 0; i < local.size(); i++) {
		Om_Apply(&local[i]);
	}
}

static int Om_AllocHandle(void)
{
	size_t i;
	for (i = 0; i < s_handles.size(); i++) {
		if (!s_handles[i].used) {
			s_handles[i].used = qtrue;
			s_handles[i].voice = -1;
			s_handles[i].state.store(OM_VOICE_STOPPED, std::memory_order_relaxed);
			s_handles[i].offset.store(0, std::memory_order_relaxed);
			s_handles[i].queued.store(0, std::memory_order_relaxed);
			s_handles[i].processed.store(0, std::memory_order_relaxed);
			s_handles[i].pendingUnqueue.store(0, std::memory_order_relaxed);
			return (int)i;
		}
	}
	s_handles.emplace_back();
	s_handles.back().used = qtrue;
	s_handles.back().voice = -1;
	s_handles.back().state.store(OM_VOICE_STOPPED, std::memory_order_relaxed);
	s_handles.back().offset.store(0, std::memory_order_relaxed);
	s_handles.back().queued.store(0, std::memory_order_relaxed);
	s_handles.back().processed.store(0, std::memory_order_relaxed);
	s_handles.back().pendingUnqueue.store(0, std::memory_order_relaxed);
	return (int)s_handles.size() - 1;
}

static void Om_Push(omCmd_t c)
{
	std::lock_guard<std::mutex> lock(s_apiMutex);
	s_cmds.push_back(std::move(c));
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
	dcfg.periodSizeInMilliseconds = 20;
	dcfg.periods            = 3;
	if (s_hrtfOn) {
		const int blocks = (s_sampleRate / 50 + OM_HRTF_BLOCK / 2) / OM_HRTF_BLOCK;
		dcfg.periodSizeInFrames = (ma_uint32)((blocks > 0 ? blocks : 1) * OM_HRTF_BLOCK);
	}
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
	s_lisGain      = 1.f;
	s_masterCur    = 1.f;
	s_masterPrimed = qfalse;
	s_captureActive = qfalse;
	s_ambiFlush    = 0;
	if (!Om_OpenDevice(cfg, qfalse)) {
		Com_Printf("Sound/omaha: no playback device, using a null device.\n");
		if (!Om_OpenDevice(cfg, qtrue)) {
			Com_Printf("Sound/omaha: null device failed\n");
			return qfalse;
		}
	}
	Com_Printf(
		"Sound/omaha: '%s' started (%d Hz, %s, %d ch)\n",
		s_nullDevice ? "null" : s_device.playback.name,
		s_sampleRate,
		s_hrtfOn ? "headphones" : (s_layout == 2 ? "5.1" : (s_layout == 3 ? "quad" : "stereo")),
		s_outCh
	);
	return qtrue;
}

static void Om_Shutdown(void)
{
	size_t i;
	if (s_deviceReady) {
		ma_device_uninit(&s_device);
		s_deviceReady = qfalse;
	}
	if (s_contextReady) {
		ma_context_uninit(&s_context);
		s_contextReady = qfalse;
	}
	for (i = 0; i < s_voices.size(); i++) {
		if (s_voices[i].used) {
			Om_SilenceVoice(&s_voices[i]);
		}
	}
	s_voices.clear();
	s_sounds.clear();
	s_handles.clear();
	s_cmds.clear();
	OmHrtf_Unload();
	s_hrtfOn = qfalse;
}

static omSoundId_t Om_CreateSound(void)
{
	size_t i;
	std::lock_guard<std::mutex> lock(s_apiMutex);
	for (i = 0; i < s_sounds.size(); i++) {
		if (!s_sounds[i].used && s_sounds[i].frames == 0 && s_sounds[i].planar[0].empty()) {
			s_sounds[i].used = qtrue;
			return (omSoundId_t)i;
		}
	}
	s_sounds.push_back(omSound_t());
	s_sounds.back().used = qtrue;
	return (omSoundId_t)(s_sounds.size() - 1);
}

static void Om_DestroySound(omSoundId_t id)
{
	omCmd_t c;
	memset(&c, 0, sizeof(c));
	c.op    = OM_OP_FREE_SOUND;
	c.sound = id;
	Om_Push(c);
}

static qboolean Om_UploadSound(omSoundId_t id, omSampleFormat_t fmt, const void *data, int sizeBytes, int rate, int blockAlign)
{
	std::lock_guard<std::mutex> lock(s_apiMutex);
	if (id < 0 || (size_t)id >= s_sounds.size()) {
		return qfalse;
	}
	return Om_UploadNow(&s_sounds[id], fmt, data, sizeBytes, rate, blockAlign);
}

static int Om_SoundRate(omSoundId_t id)
{
	if (id < 0 || (size_t)id >= s_sounds.size() || !s_sounds[id].used) {
		return 22050;
	}
	return s_sounds[id].rate > 0 ? s_sounds[id].rate : 22050;
}

static omVoiceId_t Om_PlayVoice(omSoundId_t sound, const omVoiceParams_t *params, unsigned offset)
{
	omCmd_t     c;
	omVoiceId_t id = Om_AllocHandle();
	memset(&c, 0, sizeof(c));
	c.op     = OM_OP_PLAY;
	c.id     = id;
	c.sound  = sound;
	c.offset = offset;
	c.params = params ? *params : omVoiceParams_t();
	if (!params) {
		Om_DefaultParams(&c.params);
	}
	s_handles[id].state.store(OM_VOICE_PLAYING, std::memory_order_relaxed);
	s_handles[id].offset.store(offset, std::memory_order_relaxed);
	Om_Push(c);
	return id;
}

static omVoiceId_t Om_OpenStream(const omVoiceParams_t *params)
{
	omCmd_t     c;
	omVoiceId_t id = Om_AllocHandle();
	memset(&c, 0, sizeof(c));
	c.op     = OM_OP_PLAY;
	c.id     = id;
	c.sound  = -1;
	c.params = params ? *params : omVoiceParams_t();
	if (!params) {
		Om_DefaultParams(&c.params);
	}
	c.params.flags |= OM_FLAG_2D;
	s_handles[id].state.store(OM_VOICE_STOPPED, std::memory_order_relaxed);
	Om_Push(c);
	return id;
}

static void Om_UpdateVoice(omVoiceId_t id, const omVoiceParams_t *params)
{
	omCmd_t c;
	if (!params || id < 0) {
		return;
	}
	memset(&c, 0, sizeof(c));
	c.op     = OM_OP_UPDATE;
	c.id     = id;
	c.sound  = -1;
	c.params = *params;
	Om_Push(c);
}

static void Om_StopVoice(omVoiceId_t id)
{
	omCmd_t c;
	memset(&c, 0, sizeof(c));
	c.op = OM_OP_STOP;
	c.id = id;
	if (id >= 0 && (size_t)id < s_handles.size()) {
		s_handles[id].state.store(OM_VOICE_STOPPED, std::memory_order_relaxed);
	}
	Om_Push(c);
}

static void Om_PauseVoice(omVoiceId_t id)
{
	omCmd_t c;
	memset(&c, 0, sizeof(c));
	c.op = OM_OP_PAUSE;
	c.id = id;
	if (id >= 0 && (size_t)id < s_handles.size()) {
		s_handles[id].state.store(OM_VOICE_PAUSED, std::memory_order_relaxed);
	}
	Om_Push(c);
}

static void Om_ResumeVoice(omVoiceId_t id)
{
	omCmd_t c;
	memset(&c, 0, sizeof(c));
	c.op = OM_OP_RESUME;
	c.id = id;
	if (id >= 0 && (size_t)id < s_handles.size()) {
		s_handles[id].state.store(OM_VOICE_PLAYING, std::memory_order_relaxed);
	}
	Om_Push(c);
}

static void Om_DestroyVoice(omVoiceId_t id)
{
	omCmd_t c;
	memset(&c, 0, sizeof(c));
	c.op = OM_OP_DESTROY;
	c.id = id;
	if (id >= 0 && (size_t)id < s_handles.size()) {
		s_handles[id].state.store(OM_VOICE_STOPPED, std::memory_order_relaxed);
	}
	Om_Push(c);
}

static int Om_VoiceState(omVoiceId_t id)
{
	if (id < 0 || (size_t)id >= s_handles.size() || !s_handles[id].used) {
		return OM_VOICE_STOPPED;
	}
	return s_handles[id].state.load(std::memory_order_relaxed);
}

static unsigned Om_GetOffset(omVoiceId_t id)
{
	if (id < 0 || (size_t)id >= s_handles.size() || !s_handles[id].used) {
		return 0;
	}
	return s_handles[id].offset.load(std::memory_order_relaxed);
}

static void Om_SetOffset(omVoiceId_t id, unsigned offset)
{
	omCmd_t c;
	memset(&c, 0, sizeof(c));
	c.op     = OM_OP_SEEK;
	c.id     = id;
	c.offset = offset;
	if (id >= 0 && (size_t)id < s_handles.size()) {
		s_handles[id].offset.store(offset, std::memory_order_relaxed);
	}
	Om_Push(c);
}

static void Om_QueueStream(omVoiceId_t id, const void *pcm, int bytes, omSampleFormat_t fmt, int rate, int blockAlign)
{
	omCmd_t    c;
	omChunk_t *chunk = Om_MakeChunk(fmt, pcm, bytes, rate, blockAlign);
	if (!chunk) {
		return;
	}
	memset(&c, 0, sizeof(c));
	c.op    = OM_OP_QUEUE;
	c.id    = id;
	c.chunk = chunk;
	if (id >= 0 && (size_t)id < s_handles.size()) {
		s_handles[id].queued.fetch_add(1, std::memory_order_relaxed);
	}
	Om_Push(c);
}

static int Om_StreamQueued(omVoiceId_t id)
{
	int q, u;
	if (id < 0 || (size_t)id >= s_handles.size() || !s_handles[id].used) {
		return 0;
	}
	q = s_handles[id].queued.load(std::memory_order_relaxed);
	u = s_handles[id].pendingUnqueue.load(std::memory_order_relaxed);
	q -= u;
	return q > 0 ? q : 0;
}

static int Om_StreamProcessed(omVoiceId_t id)
{
	int p, u;
	if (id < 0 || (size_t)id >= s_handles.size() || !s_handles[id].used) {
		return 0;
	}
	p = s_handles[id].processed.load(std::memory_order_relaxed);
	u = s_handles[id].pendingUnqueue.load(std::memory_order_relaxed);
	p -= u;
	return p > 0 ? p : 0;
}

static void Om_UnqueueStream(omVoiceId_t id, int count)
{
	int i;
	for (i = 0; i < count; i++) {
		omCmd_t c;
		memset(&c, 0, sizeof(c));
		c.op = OM_OP_UNQUEUE;
		c.id = id;
		if (id >= 0 && (size_t)id < s_handles.size()) {
			s_handles[id].pendingUnqueue.fetch_add(1, std::memory_order_relaxed);
		}
		Om_Push(c);
	}
}

static void Om_SetListener(const float pos[3], const float forward[3], const float left[3], const float up[3])
{
	omCmd_t c;
	memset(&c, 0, sizeof(c));
	c.op = OM_OP_LISTENER;
	memcpy(c.pos, pos, sizeof(c.pos));
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
	std::lock_guard<std::mutex> lock(s_apiMutex);
	/* Last command may not have been applied. Fall back to the mixed position. */
	memcpy(out, s_lisPos, sizeof(s_lisPos));
	for (size_t i = 0; i < s_cmds.size(); i++) {
		if (s_cmds[i].op == OM_OP_LISTENER) {
			memcpy(out, s_cmds[i].pos, sizeof(s_lisPos));
		}
	}
}

static qboolean Om_BeginCapture(int sampleRate)
{
	(void)sampleRate;
	std::lock_guard<std::mutex> lock(s_mixMutex);
	if (s_captureActive) {
		return qtrue;
	}
	if (s_deviceReady) {
		ma_device_stop(&s_device);
	}
	Om_LimiterReset();
	s_captureActive = qtrue;
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
	return frames;
}

static void Om_EndCapture(void)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	if (!s_captureActive) {
		return;
	}
	s_captureActive = qfalse;
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

static void Om_Update(void) {}

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
