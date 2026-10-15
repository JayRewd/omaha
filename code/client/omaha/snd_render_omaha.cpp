/*
===========================================================================
Copyright (C) 2026 Project: Omaha

This file is part of Project: Omaha source code.

Project: Omaha builds upon OpenMoHAA / ioquake3 / F.A.K.K. foundations.
The voice math is ported from OpenAL Soft 1.24.3 (alc/alu.cpp, alc/panning.cpp,
core/voice.cpp, core/mixer/mixer_c.cpp, core/cubic_tables.cpp):
  Copyright (C) 1999-2007 by authors.
  Copyright (C) 2011-2024 Chris Robinson and OpenAL Soft contributors.
OpenAL Soft is LGPL-2.0-or-later; this port is distributed under GPL-2+.

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
 * Added in Omaha: OpenAL Soft 1.24.3 stereo voice pipeline on miniaudio device I/O.
 *
 * Scope matches what snd_omaha.cpp asks of OpenAL: stereo output, HRTF off,
 * no effects, inverse-clamped context distance model, "Cubic Spline" resampler.
 * Output is mixed into a first-order 2D ambisonic bus (W, Y, X) and decoded with
 * Soft's StereoConfig, while direct-channel sources go straight to the real L/R
 * outputs, exactly like Soft's Dry / RealOut split. An optional ~1.5 ms lookahead
 * master limiter (s_limiter / s_limiter_threshold) runs after the mix: fixed
 * headroom trim, then GR below a -0.2 dBFS ceiling with opposite makeup so peaks
 * stay at the ceiling (anti-clip fuse; deeper threshold raises the floor).
 * Listener gain (s_volume / fade) is applied after the limiter.
 * Omaha extensions beyond Soft's scope: per-voice obstruction (low-pass +
 * fc-scaled makeup on the direct path) before the master limiter.
 *
 * Added in Omaha: headphones (speaker type 1) load a SOFA HRTF (snd_hrtf_omaha.cpp).
 * SND_ROUTE_DRY is equal-power stereo (own sounds). SND_ROUTE_DIRECT is a per-voice
 * min-phase HRIR + ITD. Everything else spatial goes into a 3rd-order 3D ambisonic
 * bus decoded binaurally once per block. Mixing then runs in whole 128-frame blocks
 * so convolution adds no latency. Stereo mode (speaker type 0) is unchanged.
 */

#include "../snd_local.h"
#include "snd_renderer.h"
#include "snd_hrtf_omaha.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <mutex>
#include <vector>

#include "miniaudio.h"

#ifndef M_PI
#	define M_PI 3.14159265358979323846
#endif

#define OM_FRAC_BITS        16
#define OM_FRAC_ONE         (1u << OM_FRAC_BITS)
#define OM_FRAC_MASK        (OM_FRAC_ONE - 1u)
#define OM_CUBIC_PHASE_BITS 5
#define OM_CUBIC_PHASES     (1 << OM_CUBIC_PHASE_BITS)
#define OM_CUBIC_DIFF_BITS  (OM_FRAC_BITS - OM_CUBIC_PHASE_BITS)
#define OM_CUBIC_DIFF_ONE   (1u << OM_CUBIC_DIFF_BITS)
#define OM_CUBIC_DIFF_MASK  (OM_CUBIC_DIFF_ONE - 1u)
#define OM_MAX_PITCH        10
#define OM_PADDING          48
#define OM_EDGE             (OM_PADDING >> 1)
#define OM_LINE_SIZE        1024
#define OM_FADE_SAMPLES     64
#define OM_GAIN_MIX_MAX     1000.0f
#define OM_GAIN_SILENCE     0.00001f
#define OM_SPEED_OF_SOUND   343.3f
#define OM_SQRT3            1.73205080756887729353f

/*
 * Bus layout: real L, R (Soft RealOut), then the ambisonic Dry bus: W, Y, X in
 * stereo mode, or ACN 0..15 (3rd-order 3D, N3D) in HRTF mode.
 */
#define OM_BUS_L     0
#define OM_BUS_R     1
#define OM_BUS_AMBI  2
#define OM_BUS_W     (OM_BUS_AMBI + 0)
#define OM_BUS_Y     (OM_BUS_AMBI + 1)
#define OM_BUS_X     (OM_BUS_AMBI + 2)
#define OM_BUS_COUNT (OM_BUS_AMBI + OM_AMBI_CHANNELS)
/* Most bus outputs one input channel can feed. */
#define OM_MAX_OUT   OM_AMBI_CHANNELS
/* Added in Omaha: direct HRTF sources closer than this (game units) use the bus. */
#define OM_HRTF_MIN_DIST 1.0f

/* Added in Omaha: master lookahead limiter (post-mix, linked stereo).
 * Fixed -8.7 dB pre-trim (demo peak ~+8.5 → near the -0.2 ceiling at s_volume 1,
 * matching the old clip-free s_volume ≈ 0.7218 cubic). Then s_limiter_threshold
 * (0 .. -12 dB, default -3.6) sets GR depth below the ceiling with opposite
 * makeup so peaks land at -0.2 (anti-clip fuse; deeper threshold raises the
 * floor). Window-peak sidechain. */
#define OM_LIMITER_LOOKAHEAD_MS      1.5f
#define OM_LIMITER_HOLD_MS           2.0f
#define OM_LIMITER_CEILING_DB        (-0.2f)
#define OM_LIMITER_PRE_TRIM_DB       (-8.7f)
#define OM_LIMITER_THRESHOLD_DB      (-3.6f)
#define OM_LIMITER_THRESHOLD_MIN_DB  (-12.0f)
#define OM_LIMITER_THRESHOLD_MAX_DB  (0.0f)
/* Changed in Omaha: 20 ms release (was 100) — less master duck after peaks. */
#define OM_LIMITER_RELEASE_MS        20.0f
#define OM_LIMITER_MAX_DELAY         512

/*
 * Added in Omaha: fixed peaking EQ for two-speaker mode only (not headphones).
 * Bands are compile-time.
 */
#define OM_SPK_EQ_BANDS 3

/*
 * Added in Omaha: fixed EQ for headphones after HRTF decode (on top of SOFA DFC
 * / MagLS DFEQ). Three peaking bands + one high shelf; compile-time; skipped in
 * two-speaker mode.
 */
#define OM_HP_EQ_BANDS       4
#define OM_HP_EQ_TYPE_PEAK   0
#define OM_HP_EQ_TYPE_HSHELF 1

/*
 * Added in Omaha: dynamic high-shelf duck for two-speaker mode (not headphones).
 * Mono 8th-order Butterworth HP sidechain @ 15.5 kHz (s_hf_duck_sc_boost),
 * 0 ms attack / 300 ms release, maps sidechain s_hf_duck_thresh..0 dB → shelf
 * cut 0..−s_hf_duck_db. Audio path: N cascaded RBJ high-shelves (S=1) at the
 * same fc so ~14 kHz stays near unity while the knee sits ~14.5–15.5 kHz.
 * Runs on two-speaker and headphones when s_mix
 * is on (speaker EQ still skips headphones; headphone EQ still runs).
 */
#define OM_HF_DUCK_HP_SECTIONS    4
#define OM_HF_DUCK_SHELF_STAGES   4
#define OM_HF_DUCK_FC_HZ          15500.0f
#define OM_HF_DUCK_SC_BOOST_MAX_DB 24.0f
#define OM_HF_DUCK_THRESH_DEFAULT (-15.0f)
#define OM_HF_DUCK_ENV_MAX_DB     (0.0f)
#define OM_HF_DUCK_RELEASE_MS     300.0f
#define OM_HF_DUCK_SHELF_S        1.0f
#define OM_HF_DUCK_DEFAULT_DB     24.0f
#define OM_HF_DUCK_SC_BOOST_DEFAULT 5.0f
#define OM_HF_DUCK_REDESIGN_DB    0.25f
#define OM_HF_DUCK_BYPASS_DB      0.05f

/* Changed in Omaha: obstruction filter constants (was s_obs_* tune cvars). */
#define OM_OBSTRUCT_CUTOFF_HZ      7000.0f
#define OM_OBSTRUCT_OPEN_HZ        20000.0f
#define OM_OBSTRUCT_OCC_BLOCKED_HZ 10000.0f
#define OM_OBSTRUCT_SMOOTH_MS      80.0f
#define OM_OBSTRUCT_EPSILON        0.01f
#define OM_OBSTRUCT_DIST_RANGE     3000.0f
#define OM_OBSTRUCT_DIST_MUL       1.0f
#define OM_OBSTRUCT_MIX            0.5f
/*
 * Changed in Omaha: makeup tracks 1-pole bandwidth loss (not a flat boost).
 * White-noise variance through y+=a*(x-y) is a/(2-a); full RMS restore is
 * sqrt((2-a)/a). Game content is closer to pink than white, so use the
 * fourth-root of that variance ratio (half the dB of full restore). Cap so
 * extreme low fc cannot explode gain. Not an AGC — closed-form from fc only.
 */
#define OM_OBSTRUCT_MAKEUP_MAX  2.0f

/*
 * Added in Omaha: two-speaker rear cue. Panning is front/back symmetric on L/R,
 * so sources behind get a level drop and a head-shadow high shelf instead.
 */
#define OM_REAR_GAIN      0.75f
#define OM_REAR_SHELF_HZ  3000.0f
#define OM_REAR_SHELF_HF  0.5f
#define OM_REAR_SMOOTH_MS 50.0f

/* alc/panning.cpp StereoConfig (first-order 2D, N3D, order gain 1). */
static const float s_stereoDecoder[2][3] = {
	{5.00000000e-1f, 2.88675135e-1f,  5.52305643e-2f},
	{5.00000000e-1f, -2.88675135e-1f, 5.52305643e-2f},
};

struct omCubic_t {
	float coeffs[4];
	float deltas[4];
};

static omCubic_t s_spline[OM_CUBIC_PHASES];

struct omPcm_t {
	int                channels;
	int                frames;
	int                rate;
	std::vector<float> data; /* interleaved */
};

struct omBuffer_t {
	qboolean used;
	omPcm_t  pcm;
};

struct omTail_t {
	int   channels;
	int   outBase;
	int   outCount;
	float samples[2][OM_FADE_SAMPLES];
	float gains[2][OM_MAX_OUT];
	/* Added in Omaha: a direct-HRTF tail owns the voice's filter until it rings out. */
	omHrtfVoice_t *hrtf;
	float          hrtfDir[3];
	int            hrtfBlocksLeft;
	qboolean       hrtfFaded;
};

struct omVoice_t {
	qboolean used;

	/* API-visible state, mirrors ALsource. */
	int                 state;
	sndBufferId_t       buffer;
	std::deque<omPcm_t> queue;
	qboolean            hasPendingOffset;
	unsigned            pendingOffset;

	float pos[3];
	float vel[3];
	float gain;
	float maxGain;
	float pitch;
	float rolloff;
	float refDist;
	float maxDist;
	int   relative;
	int   looping;
	int   loopStart; /* Added in Omaha: Soft/Miles loop points (sample frames) */
	int   loopEnd;   /* < 0 or 0 with looping → buffer length */
	int   directChannels;
	float obstructTarget; /* Added in Omaha: 0 clear .. 1 blocked */
	float lastDist;       /* Added in Omaha: world distance for distance filter (0 = N/A) */
	int   route;          /* Added in Omaha: SND_ROUTE_* (HRTF mode only) */

	/* Mixer state, mirrors Soft's Voice. */
	qboolean mixing;
	qboolean pausing;
	qboolean fading;
	int      queueCur;
	int      posInt;
	unsigned posFrac;
	unsigned step;
	int      outBase;
	int      outCount;
	float    history[2][OM_PADDING];
	float    curGains[2][OM_MAX_OUT];
	float    obstructCur; /* Added in Omaha: smoothed obstruction */
	float    lpZ[2];      /* Added in Omaha: one-pole low-pass state per channel */
	float    rearCur;     /* Added in Omaha: smoothed rear-cue amount (stereo mode) */
	float    rearZ;       /* Added in Omaha: rear-shelf low-pass state */

	/* Added in Omaha: direct HRTF filter (lazy) and whether the last line used it. */
	omHrtfVoice_t *hrtf;
	qboolean       hrtfDirect;
	float          hrtfDir[3];
};

/* Added in Omaha: Om_CalcSpatial output, consumed by Om_CalcTargets. */
struct omSpatial_t {
	float    base;     /* distance-attenuated gain before panning */
	float    dir[3];   /* listener-space unit direction (front when unknown) */
	qboolean hasDir;   /* mono 3D source with a usable direction */
	qboolean direct;   /* Om_CalcTargets chose the per-voice HRTF this line */
	float    rear;     /* Added in Omaha: stereo-mode rear-cue amount, 0 side/front .. 1 behind */
};

static std::vector<omBuffer_t> s_buffers;
static std::vector<omVoice_t>  s_voices;
static std::vector<omTail_t>   s_tails;
static std::mutex              s_mixMutex;

static ma_context s_context;
static qboolean   s_contextReady;
static ma_device  s_device;
static qboolean   s_deviceReady;
static qboolean   s_captureActive;
static int        s_sampleRate = 44100;
static float      s_listenerPos[3];
static float      s_listenerVel[3];
static float      s_listenerAt[3] = {0, 0, -1};
static float      s_listenerUp[3] = {0, 1, 0};
/* Changed in Omaha: listener gain is the post-limiter master (s_volume / fade), ramped per line. */
static float      s_listenerGain  = 1.f;
static float      s_masterApplied = 1.f;
static qboolean   s_masterPrimed;

/* Scratch for one mix line; only touched with s_mixMutex held. */
static float s_bus[OM_BUS_COUNT][OM_LINE_SIZE];
static float s_resampled[2][OM_LINE_SIZE];
static std::vector<float> s_srcData;

/*
 * Added in Omaha: headphone HRTF mode (speaker type 1 with a loaded SOFA). Lines
 * are whole OM_HRTF_BLOCKs; leftover frames wait in s_hrtfStash for the next pull.
 */
static qboolean s_hrtfOn;
static float    s_hrtfMono[OM_LINE_SIZE];
static float    s_hrtfStash[OM_LINE_SIZE * 2];
static int      s_hrtfStashPos;
static int      s_hrtfStashLen;

/*
 * Added in Omaha: master peaking EQ band (two-speaker and headphone cascades).
 * Coeffs rebuilt on sample-rate change; z-state is per band × ear.
 */
typedef struct {
	float b0, b1, b2, a1, a2;
	float z1[2], z2[2];
} omSpkEqBand_t;

static omSpkEqBand_t s_spkEq[OM_SPK_EQ_BANDS];
static qboolean      s_spkEqReady;

/*
 * Added in Omaha: headphone master EQ after HRTF (RBJ peaking + high-shelf).
 */
static omSpkEqBand_t s_hpEq[OM_HP_EQ_BANDS];
static qboolean      s_hpEqReady;

/*
 * Added in Omaha: dynamic HF shelf duck state (two-speaker master).
 * Sidechain HP sections are mono; audio cut is N stereo RBJ high-shelves.
 */
typedef struct {
	float b0, b1, b2, a1, a2;
	float z1, z2;
} omHfDuckHp_t;

typedef struct {
	float b0, b1, b2, a1, a2;
	float z1[2], z2[2];
} omHfDuckShelf_t;

static omHfDuckHp_t    s_hfDuckHp[OM_HF_DUCK_HP_SECTIONS];
static omHfDuckShelf_t s_hfDuckShelf[OM_HF_DUCK_SHELF_STAGES];
static float           s_hfDuckEnv;
static float           s_hfDuckGainDb;
static float           s_hfDuckMeterDb; /* Added in Omaha: instantaneous cut for HUD meter */
static float           s_hfDuckSidechainDb; /* Added in Omaha: HP sidechain env dB for HUD */
static float           s_hfDuckReleaseCoeff;
static float           s_hfDuckScBoostLin;
static qboolean        s_hfDuckReady;
static qboolean        s_hfDuckShelfActive; /* Added in Omaha: false = bypass (gain ~0) */

/* Added in Omaha: post-mix lookahead limiter state (touched under s_mixMutex). */
static cvar_t  *s_limiter;
static cvar_t  *s_limiter_threshold;
static cvar_t  *s_obstruction; /* Added in Omaha: 0 disables distance + occlusion LPF */
static cvar_t  *s_hf_duck;         /* Added in Omaha: 1 enables 15.5 kHz dynamic shelf duck */
static cvar_t  *s_hf_duck_db;      /* Added in Omaha: max shelf cut in dB (positive) */
static cvar_t  *s_hf_duck_thresh;  /* Added in Omaha: sidechain dB where ducking starts */
static cvar_t  *s_hf_duck_sc_boost; /* Added in Omaha: sidechain make-up dB before envelope */
static cvar_t  *s_mix; /* Added in Omaha: host mix-policy cvar; HF duck requires it on */
static float    s_limDelay[OM_LIMITER_MAX_DELAY][2];
static int      s_limDelayLen;
static int      s_limHoldLen;
static int      s_limHoldLeft;
static int      s_limWrite;
static int      s_limFilled;
static float    s_limGain;
static float    s_limReleaseCoeff;

static float Om_Clamp(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float Om_Lerp(float a, float b, float mu)
{
	return a + (b - a) * mu;
}

/* Added in Omaha: reset delay line / GR for current s_sampleRate. */
static void Om_LimiterReset(void)
{
	memset(s_limDelay, 0, sizeof(s_limDelay));
	s_limWrite     = 0;
	s_limFilled    = 0;
	s_limGain      = 1.f;
	s_limHoldLeft  = 0;
	s_limDelayLen =
		(int)(OM_LIMITER_LOOKAHEAD_MS * 0.001f * (float)s_sampleRate + 0.5f);
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
		const float releaseSamples = OM_LIMITER_RELEASE_MS * 0.001f * (float)s_sampleRate;
		s_limReleaseCoeff =
			releaseSamples > 1.f ? (1.f - expf(-1.f / releaseSamples)) : 1.f;
	}
}

/* RBJ peaking: Q from bandwidth in octaves. */
static float Om_SpkEqQFromBw(float bwOct)
{
	const float a = powf(2.0f, bwOct);
	return sqrtf(a) / (a - 1.0f);
}

static void Om_SpkEqDesignBand(omSpkEqBand_t *b, float fs, float f0, float gainDb, float bwOct)
{
	const float A     = powf(10.0f, gainDb / 40.0f);
	const float w0    = 2.0f * (float)M_PI * f0 / fs;
	const float cosw  = cosf(w0);
	const float sinw  = sinf(w0);
	const float Q     = Om_SpkEqQFromBw(bwOct);
	const float alpha = sinw / (2.0f * Q);
	const float b0    = 1.0f + alpha * A;
	const float b1    = -2.0f * cosw;
	const float b2    = 1.0f - alpha * A;
	const float a0    = 1.0f + alpha / A;
	const float a1    = -2.0f * cosw;
	const float a2    = 1.0f - alpha / A;

	b->b0 = b0 / a0;
	b->b1 = b1 / a0;
	b->b2 = b2 / a0;
	b->a1 = a1 / a0;
	b->a2 = a2 / a0;
	b->z1[0] = b->z1[1] = 0.0f;
	b->z2[0] = b->z2[1] = 0.0f;
}

static void Om_SpkEqReset(void)
{
	static const float bands[OM_SPK_EQ_BANDS][3] = {
		/* f0 Hz, gain dB, bandwidth octaves */
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
		Om_SpkEqDesignBand(&s_spkEq[i], (float)s_sampleRate, f0, bands[i][1], bands[i][2]);
	}
	s_spkEqReady = qtrue;
}

/*
 * Added in Omaha: two-speaker peaking cascade on interleaved L/R. Skipped for
 * headphones (headphone path has its own post-HRTF EQ).
 */
static void Om_ApplySpeakerEq(float *interleaved, int frames)
{
	int i, b, c;

	if (s_hrtfOn || !s_spkEqReady) {
		return;
	}
	for (i = 0; i < frames; i++) {
		for (c = 0; c < 2; c++) {
			float x = interleaved[i * 2 + c];
			for (b = 0; b < OM_SPK_EQ_BANDS; b++) {
				omSpkEqBand_t *band = &s_spkEq[b];
				const float    y =
					band->b0 * x + band->z1[c];
				band->z1[c] = band->b1 * x - band->a1 * y + band->z2[c];
				band->z2[c] = band->b2 * x - band->a2 * y;
				x           = y;
			}
			interleaved[i * 2 + c] = x;
		}
	}
}

/*
 * Added in Omaha: RBJ high-shelf into a master-EQ band slot (Audio EQ Cookbook).
 * param S = shelf slope; S=1 is steepest without overshoot.
 */
static void Om_HpEqDesignHshelf(omSpkEqBand_t *b, float fs, float f0, float gainDb, float S)
{
	const float A     = powf(10.0f, gainDb / 40.0f);
	const float w0    = 2.0f * (float)M_PI * Om_Clamp(f0, 10.0f, fs * 0.45f) / fs;
	const float cosw  = cosf(w0);
	const float sinw  = sinf(w0);
	float       alpha;
	float       twoSqrtAAlpha;
	float       b0, b1, b2, a0, a1, a2;

	if (S < 0.1f) {
		S = 0.1f;
	}
	alpha         = sinw * 0.5f * sqrtf((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
	twoSqrtAAlpha = 2.0f * sqrtf(A) * alpha;
	b0            = A * ((A + 1.0f) + (A - 1.0f) * cosw + twoSqrtAAlpha);
	b1            = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw);
	b2            = A * ((A + 1.0f) + (A - 1.0f) * cosw - twoSqrtAAlpha);
	a0            = (A + 1.0f) - (A - 1.0f) * cosw + twoSqrtAAlpha;
	a1            = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw);
	a2            = (A + 1.0f) - (A - 1.0f) * cosw - twoSqrtAAlpha;

	b->b0    = b0 / a0;
	b->b1    = b1 / a0;
	b->b2    = b2 / a0;
	b->a1    = a1 / a0;
	b->a2    = a2 / a0;
	b->z1[0] = b->z1[1] = 0.0f;
	b->z2[0] = b->z2[1] = 0.0f;
}

static void Om_HpEqReset(void)
{
	/* f0 Hz, gain dB, bw-oct (peak) or S (high shelf), type */
	static const float bands[OM_HP_EQ_BANDS][3] = {
		{100.0f, 3.0f, 1.01f},
		{400.0f, 1.5f, 2.30f},
		{4500.0f, -2.0f, 2.00f},
		{15000.0f, -4.5f, 1.00f},
	};
	static const int types[OM_HP_EQ_BANDS] = {
		OM_HP_EQ_TYPE_PEAK,
		OM_HP_EQ_TYPE_PEAK,
		OM_HP_EQ_TYPE_PEAK,
		OM_HP_EQ_TYPE_HSHELF,
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
			Om_HpEqDesignHshelf(&s_hpEq[i], (float)s_sampleRate, f0, bands[i][1], bands[i][2]);
		} else {
			Om_SpkEqDesignBand(&s_hpEq[i], (float)s_sampleRate, f0, bands[i][1], bands[i][2]);
		}
	}
	s_hpEqReady = qtrue;
}

/*
 * Added in Omaha: headphone EQ cascade on interleaved L/R after HRTF.
 * Skipped for two-speaker mode.
 */
static void Om_ApplyHeadphoneEq(float *interleaved, int frames)
{
	int i, b, c;

	if (!s_hrtfOn || !s_hpEqReady) {
		return;
	}
	for (i = 0; i < frames; i++) {
		for (c = 0; c < 2; c++) {
			float x = interleaved[i * 2 + c];
			for (b = 0; b < OM_HP_EQ_BANDS; b++) {
				omSpkEqBand_t *band = &s_hpEq[b];
				const float    y =
					band->b0 * x + band->z1[c];
				band->z1[c] = band->b1 * x - band->a1 * y + band->z2[c];
				band->z2[c] = band->b2 * x - band->a2 * y;
				x           = y;
			}
			interleaved[i * 2 + c] = x;
		}
	}
}

/* Added in Omaha: RBJ high-pass section (Audio EQ Cookbook). */
static void Om_HfDuckDesignHp(omHfDuckHp_t *s, float fs, float fc, float Q)
{
	float w0, cosw, sinw, alpha, a0;

	fc = Om_Clamp(fc, 10.0f, fs * 0.45f);
	w0 = 2.0f * (float)M_PI * fc / fs;
	cosw = cosf(w0);
	sinw = sinf(w0);
	alpha = sinw / (2.0f * Q);
	a0 = 1.0f + alpha;
	s->b0 = ((1.0f + cosw) * 0.5f) / a0;
	s->b1 = (-(1.0f + cosw)) / a0;
	s->b2 = ((1.0f + cosw) * 0.5f) / a0;
	s->a1 = (-2.0f * cosw) / a0;
	s->a2 = (1.0f - alpha) / a0;
	s->z1 = 0.0f;
	s->z2 = 0.0f;
}

/*
 * Added in Omaha: RBJ high-shelf with shelf slope S (Audio EQ Cookbook).
 * S=1 is steepest without overshoot. gainDb negative = cut. Keeps z-state.
 */
static void Om_HfDuckDesignShelf(omHfDuckShelf_t *s, float fs, float f0, float gainDb, float S)
{
	const float A = powf(10.0f, gainDb / 40.0f);
	const float w0 = 2.0f * (float)M_PI * Om_Clamp(f0, 10.0f, fs * 0.45f) / fs;
	const float cosw = cosf(w0);
	const float sinw = sinf(w0);
	float       alpha;
	float       twoSqrtAAlpha;
	float       b0, b1, b2, a0, a1, a2;

	if (S < 0.1f) {
		S = 0.1f;
	}
	alpha         = sinw * 0.5f * sqrtf((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
	twoSqrtAAlpha = 2.0f * sqrtf(A) * alpha;
	b0            = A * ((A + 1.0f) + (A - 1.0f) * cosw + twoSqrtAAlpha);
	b1            = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw);
	b2            = A * ((A + 1.0f) + (A - 1.0f) * cosw - twoSqrtAAlpha);
	a0            = (A + 1.0f) - (A - 1.0f) * cosw + twoSqrtAAlpha;
	a1            = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw);
	a2            = (A + 1.0f) - (A - 1.0f) * cosw - twoSqrtAAlpha;

	s->b0 = b0 / a0;
	s->b1 = b1 / a0;
	s->b2 = b2 / a0;
	s->a1 = a1 / a0;
	s->a2 = a2 / a0;
}

/* Added in Omaha: split total shelf cut across OM_HF_DUCK_SHELF_STAGES stages. */
static void Om_HfDuckSetShelfGain(float totalGainDb)
{
	const float stageGain = totalGainDb / (float)OM_HF_DUCK_SHELF_STAGES;
	int         i;

	s_hfDuckGainDb = totalGainDb;
	if (fabsf(totalGainDb) < OM_HF_DUCK_BYPASS_DB) {
		s_hfDuckShelfActive = qfalse;
		return;
	}
	s_hfDuckShelfActive = qtrue;
	for (i = 0; i < OM_HF_DUCK_SHELF_STAGES; i++) {
		Om_HfDuckDesignShelf(
			&s_hfDuckShelf[i], (float)s_sampleRate, OM_HF_DUCK_FC_HZ, stageGain, OM_HF_DUCK_SHELF_S
		);
	}
}

static void Om_HfDuckReset(void)
{
	/* 8th-order Butterworth: cascaded 2nd-order Qs */
	static const float qs[OM_HF_DUCK_HP_SECTIONS] = {
		0.50979558f, 0.60134489f, 0.89997622f, 2.5629154f,
	};
	int i;

	s_hfDuckEnv         = 0.0f;
	s_hfDuckGainDb      = 0.0f;
	s_hfDuckMeterDb     = 0.0f;
	s_hfDuckSidechainDb = -100.0f;
	s_hfDuckScBoostLin  = 1.0f;
	s_hfDuckShelfActive = qfalse;
	for (i = 0; i < OM_HF_DUCK_SHELF_STAGES; i++) {
		s_hfDuckShelf[i].z1[0] = s_hfDuckShelf[i].z1[1] = 0.0f;
		s_hfDuckShelf[i].z2[0] = s_hfDuckShelf[i].z2[1] = 0.0f;
	}

	if (s_sampleRate <= 0) {
		s_hfDuckReady = qfalse;
		return;
	}
	{
		const float releaseSamples = OM_HF_DUCK_RELEASE_MS * 0.001f * (float)s_sampleRate;
		s_hfDuckReleaseCoeff =
			releaseSamples > 1.0f ? (1.0f - expf(-1.0f / releaseSamples)) : 1.0f;
	}
	for (i = 0; i < OM_HF_DUCK_HP_SECTIONS; i++) {
		Om_HfDuckDesignHp(&s_hfDuckHp[i], (float)s_sampleRate, OM_HF_DUCK_FC_HZ, qs[i]);
	}
	Om_HfDuckSetShelfGain(0.0f);
	s_hfDuckReady = qtrue;
}

static float Om_HfDuckProcessHp(float x)
{
	int i;

	for (i = 0; i < OM_HF_DUCK_HP_SECTIONS; i++) {
		omHfDuckHp_t *s = &s_hfDuckHp[i];
		const float   y = s->b0 * x + s->z1;
		s->z1           = s->b1 * x - s->a1 * y + s->z2;
		s->z2           = s->b2 * x - s->a2 * y;
		x               = y;
	}
	return x;
}

/*
 * Added in Omaha: ride cascaded 15.5 kHz high shelves from a steep HF sidechain.
 * s_hf_duck 0 = off; requires s_mix 1. Headphones OK.
 */
static float Om_HfDuckThreshDb(void)
{
	float thresh = s_hf_duck_thresh ? s_hf_duck_thresh->value : OM_HF_DUCK_THRESH_DEFAULT;
	/* Must stay below full-scale map top so the duck range has width. */
	if (thresh > OM_HF_DUCK_ENV_MAX_DB - 1.0f) {
		thresh = OM_HF_DUCK_ENV_MAX_DB - 1.0f;
	}
	if (thresh < -96.0f) {
		thresh = -96.0f;
	}
	return thresh;
}

static void Om_ApplyHfDuck(float *interleaved, int frames)
{
	float maxCutDb;
	float threshDb;
	float rangeDb;
	int   i, c, stage;

	if (!s_hfDuckReady) {
		s_hfDuckMeterDb     = 0.0f;
		s_hfDuckSidechainDb = -100.0f;
		return;
	}
	/* Changed in Omaha: HF duck is tuned for s_mix category gains only. */
	if (!s_mix || !s_mix->integer) {
		s_hfDuckMeterDb     = 0.0f;
		s_hfDuckSidechainDb = -100.0f;
		s_hfDuckShelfActive = qfalse;
		return;
	}
	if (!s_hf_duck || !s_hf_duck->integer) {
		s_hfDuckMeterDb     = 0.0f;
		s_hfDuckSidechainDb = -100.0f;
		return;
	}
	maxCutDb = s_hf_duck_db ? s_hf_duck_db->value : OM_HF_DUCK_DEFAULT_DB;
	if (maxCutDb < 0.0f) {
		maxCutDb = -maxCutDb;
	}
	if (maxCutDb > 24.0f) {
		maxCutDb = 24.0f;
	}
	{
		float scBoostDb = s_hf_duck_sc_boost ? s_hf_duck_sc_boost->value : OM_HF_DUCK_SC_BOOST_DEFAULT;
		if (scBoostDb < 0.0f) {
			scBoostDb = 0.0f;
		}
		if (scBoostDb > OM_HF_DUCK_SC_BOOST_MAX_DB) {
			scBoostDb = OM_HF_DUCK_SC_BOOST_MAX_DB;
		}
		s_hfDuckScBoostLin = powf(10.0f, scBoostDb / 20.0f);
	}
	threshDb = Om_HfDuckThreshDb();
	rangeDb  = OM_HF_DUCK_ENV_MAX_DB - threshDb;
	if (rangeDb < 1.0f) {
		rangeDb = 1.0f;
	}

	for (i = 0; i < frames; i++) {
		const float mono = 0.5f * (interleaved[i * 2 + 0] + interleaved[i * 2 + 1]);
		float       sc   = Om_HfDuckProcessHp(mono) * s_hfDuckScBoostLin;
		float       x    = fabsf(sc);
		float       envDb, t, gainDb;

		/* 0 ms attack, release only. */
		if (x >= s_hfDuckEnv) {
			s_hfDuckEnv = x;
		} else {
			s_hfDuckEnv += s_hfDuckReleaseCoeff * (x - s_hfDuckEnv);
		}

		envDb               = 20.0f * log10f(s_hfDuckEnv + 1.0e-20f);
		s_hfDuckSidechainDb = envDb;
		t                   = Om_Clamp((envDb - threshDb) / rangeDb, 0.0f, 1.0f);
		gainDb              = (maxCutDb < OM_HF_DUCK_BYPASS_DB) ? 0.0f : (-t * maxCutDb);
		s_hfDuckMeterDb     = gainDb;

		if (fabsf(gainDb - s_hfDuckGainDb) >= OM_HF_DUCK_REDESIGN_DB) {
			Om_HfDuckSetShelfGain(gainDb);
		}

		if (!s_hfDuckShelfActive) {
			continue;
		}

		for (c = 0; c < 2; c++) {
			float sample = interleaved[i * 2 + c];
			for (stage = 0; stage < OM_HF_DUCK_SHELF_STAGES; stage++) {
				omHfDuckShelf_t *s = &s_hfDuckShelf[stage];
				float            y = s->b0 * sample + s->z1[c];
				s->z1[c]           = s->b1 * sample - s->a1 * y + s->z2[c];
				s->z2[c]           = s->b2 * sample - s->a2 * y;
				sample             = y;
			}
			interleaved[i * 2 + c] = sample;
		}
	}
}

/*
 * Added in Omaha: post-limiter master gain. Linear ramp across the line when the
 * target changed (fades step once per client frame); the first line after a
 * reset snaps to the listener gain.
 */
static void Om_ApplyMaster(float *interleaved, int frames)
{
	const float target = s_listenerGain;
	float       g;
	float       step;
	int         i;

	if (!s_masterPrimed) {
		s_masterApplied = target;
		s_masterPrimed  = qtrue;
	}

	if (s_masterApplied == target) {
		if (target == 1.f) {
			return;
		}
		for (i = 0; i < frames * 2; i++) {
			interleaved[i] *= target;
		}
		return;
	}

	g    = s_masterApplied;
	step = (target - g) / (float)frames;
	for (i = 0; i < frames; i++) {
		g += step;
		interleaved[i * 2 + 0] *= g;
		interleaved[i * 2 + 1] *= g;
	}
	s_masterApplied = target;
}

/*
 * Added in Omaha: map s_limiter_threshold (dB GR depth, 0 .. -12) onto a
 * brickwall level below the fixed ceiling, plus opposite makeup so peaks land
 * at OM_LIMITER_CEILING_DB. Pre-trim is fixed (demo headroom calibration).
 */
static void Om_LimiterCalibrate(float *preTrimLin, float *threshLin, float *makeupLin)
{
	float thrDb = s_limiter_threshold ? s_limiter_threshold->value : OM_LIMITER_THRESHOLD_DB;

	thrDb = Om_Clamp(thrDb, OM_LIMITER_THRESHOLD_MIN_DB, OM_LIMITER_THRESHOLD_MAX_DB);
	*preTrimLin = powf(10.f, OM_LIMITER_PRE_TRIM_DB * 0.05f);
	*threshLin  = powf(10.f, (OM_LIMITER_CEILING_DB + thrDb) * 0.05f);
	*makeupLin  = thrDb >= 0.f ? 1.f : powf(10.f, -thrDb * 0.05f);
}

/*
 * Added in Omaha: ~1.5 ms lookahead brickwall on the stereo master.
 * Changed in Omaha: fixed pre-trim first (s_volume 1 ≈ old clip-free 0.7218),
 * then window-peak GR into the soft knee, then opposite makeup to the -0.2
 * ceiling. Instant attack, Soft-like 2 ms hold, 20 ms release.
 */
static void Om_ApplyLimiter(float *interleaved, int frames)
{
	float preTrimLin;
	float threshLin;
	float makeupLin;
	float ceilingLin;
	int   i;

	if (!s_limiter || !s_limiter->integer || s_limDelayLen < 1) {
		return;
	}

	Om_LimiterCalibrate(&preTrimLin, &threshLin, &makeupLin);
	ceilingLin = powf(10.f, OM_LIMITER_CEILING_DB * 0.05f);

	for (i = 0; i < frames; i++) {
		/* Changed in Omaha: headroom trim before the delay / GR sidechain. */
		const float inL    = interleaved[i * 2 + 0] * preTrimLin;
		const float inR    = interleaved[i * 2 + 1] * preTrimLin;
		const float inPeak = fmaxf(fabsf(inL), fabsf(inR));
		float       outL;
		float       outR;
		float       windowPeak;
		float       needed;
		int         j;

		if (s_limFilled < s_limDelayLen) {
			s_limDelay[s_limWrite][0] = inL;
			s_limDelay[s_limWrite][1] = inR;
			s_limWrite++;
			if (s_limWrite >= s_limDelayLen) {
				s_limWrite = 0;
			}
			s_limFilled++;
			interleaved[i * 2 + 0] = 0.f;
			interleaved[i * 2 + 1] = 0.f;
			continue;
		}

		/* Window = every sample still in the delay + the new one about to enter. */
		windowPeak = inPeak;
		for (j = 0; j < s_limDelayLen; j++) {
			const float p = fmaxf(fabsf(s_limDelay[j][0]), fabsf(s_limDelay[j][1]));
			if (p > windowPeak) {
				windowPeak = p;
			}
		}

		needed = windowPeak > threshLin ? (threshLin / windowPeak) : 1.f;
		if (needed < s_limGain) {
			s_limGain     = needed;
			s_limHoldLeft = s_limHoldLen;
		} else if (s_limHoldLeft > 0) {
			s_limHoldLeft--;
		} else {
			s_limGain += (1.f - s_limGain) * s_limReleaseCoeff;
			/* Changed in Omaha: never release above what the current window allows. */
			if (s_limGain > needed) {
				s_limGain = needed;
			}
		}

		outL = s_limDelay[s_limWrite][0] * s_limGain * makeupLin;
		outR = s_limDelay[s_limWrite][1] * s_limGain * makeupLin;
		/* Added in Omaha: float-safe ceiling after makeup (exact math already lands here). */
		outL = Om_Clamp(outL, -ceilingLin, ceilingLin);
		outR = Om_Clamp(outR, -ceilingLin, ceilingLin);

		s_limDelay[s_limWrite][0] = inL;
		s_limDelay[s_limWrite][1] = inR;
		s_limWrite++;
		if (s_limWrite >= s_limDelayLen) {
			s_limWrite = 0;
		}

		interleaved[i * 2 + 0] = outL;
		interleaved[i * 2 + 1] = outR;
	}
}

/* core/cubic_tables.cpp SplineTable. */
static void Om_InitSplineTable(void)
{
	static const double third = 1.0 / 3.0;
	static const double sixth = 1.0 / 6.0;
	int                 pi;

	for (pi = 0; pi < OM_CUBIC_PHASES; pi++) {
		const double mu  = (double)pi / (double)OM_CUBIC_PHASES;
		const double mu2 = mu * mu;
		const double mu3 = mu * mu2;

		s_spline[pi].coeffs[0] = (float)(-third * mu + 0.5 * mu2 - sixth * mu3);
		s_spline[pi].coeffs[1] = (float)(1.0 - 0.5 * mu - mu2 + 0.5 * mu3);
		s_spline[pi].coeffs[2] = (float)(mu + 0.5 * mu2 - 0.5 * mu3);
		s_spline[pi].coeffs[3] = (float)(-sixth * mu + sixth * mu3);
	}
	for (pi = 0; pi < OM_CUBIC_PHASES - 1; pi++) {
		s_spline[pi].deltas[0] = s_spline[pi + 1].coeffs[0] - s_spline[pi].coeffs[0];
		s_spline[pi].deltas[1] = s_spline[pi + 1].coeffs[1] - s_spline[pi].coeffs[1];
		s_spline[pi].deltas[2] = s_spline[pi + 1].coeffs[2] - s_spline[pi].coeffs[2];
		s_spline[pi].deltas[3] = s_spline[pi + 1].coeffs[3] - s_spline[pi].coeffs[3];
	}
	pi                     = OM_CUBIC_PHASES - 1;
	s_spline[pi].deltas[0] = 0.0f - s_spline[pi].coeffs[0];
	s_spline[pi].deltas[1] = s_spline[0].coeffs[0] - s_spline[pi].coeffs[1];
	s_spline[pi].deltas[2] = s_spline[0].coeffs[1] - s_spline[pi].coeffs[2];
	s_spline[pi].deltas[3] = s_spline[0].coeffs[2] - s_spline[pi].coeffs[3];
}

/* core/mixer/mixer_c.cpp do_cubic + DoResample; src[0] is the sample before the position. */
static void Om_ResampleCubic(const float *src, unsigned frac, unsigned increment, float *dst, int count)
{
	size_t pos = 0;
	int    i;

	for (i = 0; i < count; i++) {
		const unsigned   pi  = frac >> OM_CUBIC_DIFF_BITS;
		const float      pf  = (float)(frac & OM_CUBIC_DIFF_MASK) * (1.0f / OM_CUBIC_DIFF_ONE);
		const omCubic_t *fil = &s_spline[pi];
		const float     *v   = &src[pos];

		dst[i] = (fil->coeffs[0] + pf * fil->deltas[0]) * v[0] + (fil->coeffs[1] + pf * fil->deltas[1]) * v[1]
			   + (fil->coeffs[2] + pf * fil->deltas[2]) * v[2] + (fil->coeffs[3] + pf * fil->deltas[3]) * v[3];

		frac += increment;
		pos += frac >> OM_FRAC_BITS;
		frac &= OM_FRAC_MASK;
	}
}

/* core/mixer/mixer_c.cpp MixLine; out is one bus line. */
static void Om_MixLine(const float *in, float *out, float *curGain, float target, int counter, int count)
{
	const float delta   = counter > 0 ? 1.0f / (float)counter : 0.0f;
	const int   fadeLen = counter < count ? counter : count;
	const float step    = (target - *curGain) * delta;
	int         i       = 0;

	if (fabsf(step) > FLT_EPSILON) {
		const float gain      = *curGain;
		float       stepCount = 0.0f;

		for (; i < fadeLen; i++) {
			out[i] += in[i] * (gain + step * stepCount);
			stepCount += 1.0f;
		}
		if (fadeLen < counter) {
			*curGain = gain + step * stepCount;
			return;
		}
	}
	*curGain = target;

	if (!(fabsf(target) > OM_GAIN_SILENCE)) {
		return;
	}
	for (; i < count; i++) {
		out[i] += in[i] * target;
	}
}

/* core/ambidefs.h CalcAmbiCoeffs via core/mixer.h CalcDirectionCoeffs, 2D first order (W, Y, X). */
static void Om_DirectionCoeffs(const float dir[3], float coeffs[3])
{
	coeffs[0] = 1.0f;
	coeffs[1] = OM_SQRT3 * -dir[0];
	coeffs[2] = OM_SQRT3 * -dir[2];
}

/* Equal-power L/R pan from a listener-space unit direction (x right, -z front). */
static void Om_StereoPan(const float dir[3], qboolean hasDir, float *gainL, float *gainR)
{
	float pan = hasDir ? Om_Clamp(dir[0], -1.0f, 1.0f) : 0.0f;
	*gainL    = sqrtf(0.5f * (1.0f - pan));
	*gainR    = sqrtf(0.5f * (1.0f + pan));
}

/* alc/alu.cpp ScaleAzimuthFront3. */
static void Om_ScaleAzimuthFront3(float pos[3])
{
	if (pos[2] < 0.0f) {
		const float len2d = sqrtf(pos[0] * pos[0] + pos[2] * pos[2]);
		float       x     = pos[0] / len2d;
		float       z     = -pos[2] / len2d;

		if (z > 0.866025403785f) {
			x      = x * 3.0f - x * x * x * 4.0f;
			z      = z * z * z * 4.0f - z * 3.0f;
			pos[0] = x * len2d;
			pos[2] = -z * len2d;
		} else {
			pos[0] = copysignf(len2d, pos[0]);
			pos[2] = 0.0f;
		}
	}
}

/* alc/alu.cpp ScaleAzimuthFront3_2. */
static void Om_ScaleAzimuthFront3_2(float pos[3])
{
	if (pos[2] < 0.0f) {
		const float len2d = sqrtf(pos[0] * pos[0] + pos[2] * pos[2]);
		float       x     = pos[0] / len2d;
		float       z     = -pos[2] / len2d;

		if (z > 0.5f) {
			x      = copysignf(sqrtf((1.0f - z) * 0.5f), x);
			z      = sqrtf((1.0f + z) * 0.5f);
			x      = x * 3.0f - x * x * x * 4.0f;
			z      = z * z * z * 4.0f - z * 3.0f;
			pos[0] = x * len2d;
			pos[2] = -z * len2d;
		} else {
			pos[0] = copysignf(len2d, pos[0]);
			pos[2] = 0.0f;
		}
	}
}

/*
 * Added in Omaha: two-speaker image, replacing ScaleAzimuthFront3_2 for mono 3D
 * sources. Pan angle is (pi/2) * lateral sine, so 30° lands where Soft put it
 * (45°) but 60–90° no longer collapses onto the hard side, and a source behind
 * pans like its front mirror instead of drifting centre. Returns the rear-cue
 * amount; elevated sources fade toward centre and get less of it.
 */
static float Om_StereoImage(const float dir[3], float pos[3])
{
	const float len2d = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
	float       x, angle;

	pos[1] = 0.0f;
	if (len2d < 1.0e-4f) {
		pos[0] = 0.0f;
		pos[2] = -len2d;
		return 0.0f;
	}
	x      = Om_Clamp(dir[0] / len2d, -1.0f, 1.0f);
	angle  = (float)(M_PI * 0.5) * x;
	pos[0] = sinf(angle) * len2d;
	pos[2] = -cosf(angle) * len2d;
	return Om_Clamp(dir[2], 0.0f, 1.0f);
}

/* common/vecmat.h Vector::normalize. */
static float Om_Normalize(float v[3])
{
	const float lengthSqr = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	if (lengthSqr > FLT_EPSILON) {
		const float length    = sqrtf(lengthSqr);
		const float invLength = 1.0f / length;
		v[0] *= invLength;
		v[1] *= invLength;
		v[2] *= invLength;
		return length;
	}
	return 0.0f;
}

/* alc/alu.cpp CalcContextParams listener matrix. Rows U, V, -N applied as dot products. */
static void Om_ListenerMatrix(float U[3], float V[3], float N[3])
{
	N[0] = s_listenerAt[0];
	N[1] = s_listenerAt[1];
	N[2] = s_listenerAt[2];
	Om_Normalize(N);
	V[0] = s_listenerUp[0];
	V[1] = s_listenerUp[1];
	V[2] = s_listenerUp[2];
	Om_Normalize(V);
	U[0] = N[1] * V[2] - N[2] * V[1];
	U[1] = N[2] * V[0] - N[0] * V[2];
	U[2] = N[0] * V[1] - N[1] * V[0];
	Om_Normalize(U);
}

static void Om_Transform(const float U[3], const float V[3], const float N[3], const float in[3], float out[3])
{
	out[0] = in[0] * U[0] + in[1] * U[1] + in[2] * U[2];
	out[1] = in[0] * V[0] + in[1] * V[1] + in[2] * V[2];
	out[2] = in[0] * -N[0] + in[1] * -N[1] + in[2] * -N[2];
}

static unsigned Om_PitchToStep(float pitch)
{
	if (pitch > (float)OM_MAX_PITCH) {
		return OM_MAX_PITCH << OM_FRAC_BITS;
	}
	/* fastf2u: round to nearest, like _mm_cvt_ss2si. */
	long step = lrintf(pitch * (float)OM_FRAC_ONE);
	return step < 1 ? 1u : (unsigned)step;
}

static const omPcm_t *Om_VoiceFirstPcm(const omVoice_t *v)
{
	if (!v->queue.empty()) {
		return &v->queue.front();
	}
	if (v->buffer >= 0 && (size_t)v->buffer < s_buffers.size() && s_buffers[v->buffer].used
		&& s_buffers[v->buffer].pcm.frames > 0) {
		return &s_buffers[v->buffer].pcm;
	}
	return NULL;
}

static qboolean Om_VoiceIsStatic(const omVoice_t *v)
{
	return v->queue.empty() ? qtrue : qfalse;
}

/*
 * CalcNonAttnSourceParams / CalcAttnSourceParams for the subset Omaha uses.
 * Changed in Omaha: split from panning so HRTF can use the direction and base
 * gain directly. Sets the fixed-point step and lastDist; fills base gain and the
 * unscaled listener-space direction (mono 3D sources only).
 */
static void Om_CalcSpatial(omVoice_t *v, const omPcm_t *pcm, omSpatial_t *sp)
{
	const float freqRatio = (float)pcm->rate / (float)s_sampleRate;

	sp->dir[0] = 0.0f;
	sp->dir[1] = 0.0f;
	sp->dir[2] = -1.0f;
	sp->hasDir = qfalse;
	sp->direct = qfalse;
	sp->rear   = 0.0f;

	if (pcm->channels >= 2) {
		sp->base    = fminf(Om_Clamp(v->gain, 0.0f, v->maxGain), OM_GAIN_MIX_MAX);
		v->step     = Om_PitchToStep(freqRatio * v->pitch);
		v->lastDist = 0.0f;
		return;
	}

	{
		float U[3], V[3], N[3];
		float position[3], velocity[3], toSource[3];
		float distance;
		float dryGainBase = v->gain;
		float pitch = v->pitch;

		Om_ListenerMatrix(U, V, N);
		if (!v->relative) {
			float rel[3];
			rel[0] = v->pos[0] - s_listenerPos[0];
			rel[1] = v->pos[1] - s_listenerPos[1];
			rel[2] = v->pos[2] - s_listenerPos[2];
			Om_Transform(U, V, N, rel, position);
			Om_Transform(U, V, N, v->vel, velocity);
		} else {
			float lvel[3];
			Om_Transform(U, V, N, s_listenerVel, lvel);
			position[0] = v->pos[0];
			position[1] = v->pos[1];
			position[2] = v->pos[2];
			velocity[0] = v->vel[0] + lvel[0];
			velocity[1] = v->vel[1] + lvel[1];
			velocity[2] = v->vel[2] + lvel[2];
		}

		toSource[0] = position[0];
		toSource[1] = position[1];
		toSource[2] = position[2];
		distance    = Om_Normalize(toSource);
		v->lastDist = distance; /* Added in Omaha: for distance-filter axis */

		/* Context model: AL_INVERSE_DISTANCE_CLAMPED (AL_SOURCE_DISTANCE_MODEL is never enabled). */
		if (!(v->maxDist < v->refDist)) {
			const float clampedDist = Om_Clamp(distance, v->refDist, v->maxDist);
			if (v->refDist > 0.0f) {
				const float dist = Om_Lerp(v->refDist, clampedDist, v->rolloff);
				if (dist > 0.0f) {
					dryGainBase *= v->refDist / dist;
				}
			}
		}

		dryGainBase = Om_Clamp(dryGainBase, 0.0f, v->maxGain);
		sp->base    = fminf(dryGainBase * 1.0f, OM_GAIN_MIX_MAX);

		{
			float lvel[3];
			float vss, vls;

			Om_Transform(U, V, N, s_listenerVel, lvel);
			vss = (velocity[0] * toSource[0] + velocity[1] * toSource[1] + velocity[2] * toSource[2]) * -1.0f;
			vls = (lvel[0] * toSource[0] + lvel[1] * toSource[1] + lvel[2] * toSource[2]) * -1.0f;
			if (!(vls < OM_SPEED_OF_SOUND)) {
				pitch = 0.0f;
			} else if (!(vss < OM_SPEED_OF_SOUND)) {
				pitch = INFINITY;
			} else {
				pitch *= (OM_SPEED_OF_SOUND - vls) / (OM_SPEED_OF_SOUND - vss);
			}
		}
		v->step = Om_PitchToStep(pitch * freqRatio);

		if (distance > FLT_EPSILON) {
			sp->dir[0] = toSource[0];
			sp->dir[1] = toSource[1];
			sp->dir[2] = toSource[2];
			sp->hasDir = qtrue;
		}
	}
}

/*
 * CalcPanningAndFilters for the subset Omaha uses. Writes target gains per input
 * channel into the voice's outputs: real L/R for direct channels, the 2D W/Y/X bus
 * (stereo mode), the 3rd-order bus or a per-voice HRTF (HRTF mode). allowDirect
 * means the voice is routed SND_ROUTE_DIRECT and has a filter available.
 */
static void Om_CalcTargets(
	omVoice_t *v, const omPcm_t *pcm, omSpatial_t *sp, qboolean allowDirect, float targets[2][OM_MAX_OUT]
)
{
	const float base = sp->base;
	int         c, k;

	memset(targets, 0, sizeof(float) * 2 * OM_MAX_OUT);

	if (pcm->channels >= 2) {
		if (v->directChannels != SND_DIRECT_CHANNELS_OFF) {
			v->outBase    = OM_BUS_L;
			v->outCount   = 2;
			targets[0][0] = base;
			targets[1][1] = base;
			return;
		}

		v->outBase  = OM_BUS_AMBI;
		v->outCount = s_hrtfOn ? OM_AMBI_CHANNELS : 3;
		for (c = 0; c < 2; c++) {
			/* StereoMap at +/-30 degrees (StereoPan default), counter-clockwise. */
			const float a = (c == 0 ? 1.0f : -1.0f) * (float)(M_PI / 6.0);
			float       chanPos[3];
			float       coeffs[OM_MAX_OUT];

			chanPos[0] = -sinf(a);
			chanPos[1] = 0.0f;
			chanPos[2] = -cosf(a);
			if (s_hrtfOn) {
				OmHrtf_AmbiCoeffs(chanPos, coeffs);
			} else {
				Om_ScaleAzimuthFront3(chanPos);
				Om_DirectionCoeffs(chanPos, coeffs);
			}
			for (k = 0; k < v->outCount; k++) {
				targets[c][k] = 1.0f * coeffs[k] * base;
			}
		}
		return;
	}

	if (s_hrtfOn) {
		float coeffs[OM_AMBI_CHANNELS];
		float gL, gR;

		/* Added in Omaha: own / head-locked sounds stay dry (equal-power pan). */
		if (v->route == SND_ROUTE_DRY) {
			Om_StereoPan(sp->dir, sp->hasDir, &gL, &gR);
			v->outBase    = OM_BUS_L;
			v->outCount   = 2;
			targets[0][0] = base * gL;
			targets[0][1] = base * gR;
			return;
		}

		/* Added in Omaha: inside-the-head sources stay on the bus (no stable direction). */
		if (allowDirect && sp->hasDir && v->lastDist > OM_HRTF_MIN_DIST) {
			sp->direct    = qtrue;
			v->outBase    = OM_BUS_L;
			v->outCount   = 0;
			targets[0][0] = base;
			return;
		}
		OmHrtf_AmbiCoeffs(sp->dir, coeffs);
		v->outBase  = OM_BUS_AMBI;
		v->outCount = OM_AMBI_CHANNELS;
		for (k = 0; k < OM_AMBI_CHANNELS; k++) {
			targets[0][k] = coeffs[k] * base;
		}
		return;
	}

	{
		float dir[3];
		float coeffs[3];
		float gain = base;

		v->outBase  = OM_BUS_W;
		v->outCount = 3;
		if (sp->hasDir) {
			sp->rear = Om_StereoImage(sp->dir, dir);
			gain *= Om_Lerp(1.0f, OM_REAR_GAIN, sp->rear);
			Om_DirectionCoeffs(dir, coeffs);
		} else if (sp->hasDir) {
			dir[0] = sp->dir[0];
			dir[1] = sp->dir[1];
			dir[2] = sp->dir[2];
			Om_ScaleAzimuthFront3_2(dir);
			Om_DirectionCoeffs(dir, coeffs);
		} else {
			float front[3] = {0.0f, 0.0f, -1.0f};
			Om_ScaleAzimuthFront3(front);
			Om_DirectionCoeffs(front, coeffs);
		}
		for (k = 0; k < 3; k++) {
			targets[0][k] = 1.0f * coeffs[k] * gain;
		}
	}
}

/* Soft loop region for a static buffer (core/voice.cpp LoadBufferStatic). */
static void Om_VoiceLoopRange(const omVoice_t *v, int frames, size_t *loopStart, size_t *loopEnd)
{
	size_t start = v->loopStart > 0 ? (size_t)v->loopStart : 0;
	size_t end   = (v->loopEnd > 0 && v->loopEnd <= frames) ? (size_t)v->loopEnd : (size_t)frames;

	if (start >= end) {
		start = 0;
		end   = (size_t)frames;
	}
	*loopStart = start;
	*loopEnd   = end;
}

/* core/voice.cpp LoadBufferStatic / LoadBufferQueue for one channel. */
static void Om_LoadSamples(const omVoice_t *v, int ch, int posInt, int queueCur, float *dst, size_t count)
{
	float  lastSample = 0.0f;
	size_t done       = 0;

	if (Om_VoiceIsStatic(v)) {
		const omPcm_t *pcm = &s_buffers[v->buffer].pcm;
		const size_t   len = (size_t)pcm->frames;
		const int      nch = pcm->channels;
		size_t         p   = (size_t)(posInt > 0 ? posInt : 0);
		size_t         loopStart, loopEnd, loopSize;

		if (!v->looping) {
			if (len > p) {
				const size_t remaining = (count < len - p) ? count : len - p;
				for (; done < remaining; done++) {
					dst[done] = pcm->data[(p + done) * nch + ch];
				}
				lastSample = dst[remaining - 1];
			}
			for (; done < count; done++) {
				dst[done] = lastSample;
			}
			return;
		}

		Om_VoiceLoopRange(v, pcm->frames, &loopStart, &loopEnd);
		loopSize = loopEnd - loopStart;
		if (p >= loopEnd) {
			p = ((p - loopStart) % loopSize) + loopStart;
		}
		while (done < count) {
			const size_t n = (count - done < loopEnd - p) ? count - done : loopEnd - p;
			size_t       i;
			for (i = 0; i < n; i++) {
				dst[done + i] = pcm->data[(p + i) * nch + ch];
			}
			done += n;
			p = loopStart;
		}
		return;
	}

	{
		size_t p   = (size_t)(posInt > 0 ? posInt : 0);
		int    idx = queueCur;

		while (idx >= 0 && idx < (int)v->queue.size() && done < count) {
			const omPcm_t *pcm = &v->queue[idx];
			const size_t   len = (size_t)pcm->frames;

			if (p >= len) {
				p -= len;
			} else {
				const size_t remaining = (count - done < len - p) ? count - done : len - p;
				size_t       i;
				for (i = 0; i < remaining; i++) {
					dst[done + i] = pcm->data[(p + i) * pcm->channels + ch];
				}
				done += remaining;
				lastSample = dst[done - 1];
				p          = 0;
			}
			idx++;
			if (idx >= (int)v->queue.size() && v->looping) {
				idx = 0;
			}
		}
		for (; done < count; done++) {
			dst[done] = lastSample;
		}
	}
}

static size_t Om_SourceCount(unsigned frac, unsigned increment, int dstCount)
{
	const unsigned ext  = increment <= OM_FRAC_ONE ? 1u : 0u;
	uint64_t       size = (uint64_t)(dstCount - (int)ext);
	size                = ((size * increment + frac) >> OM_FRAC_BITS) + ext + OM_EDGE;
	return (size_t)size;
}

/*
 * One channel of Voice::mix's load + resample: history in front, then source
 * samples from the voice position (or, for an ended voice, the sample closest to
 * zero held constant). Updates the history when storeHistory is set.
 */
static void Om_ResampleChannel(omVoice_t *v, int ch, qboolean ended, qboolean storeHistory, float *out, int count)
{
	const size_t srcCount = Om_SourceCount(v->posFrac, v->step, count);
	float       *data;
	float       *buf;

	if (s_srcData.size() < OM_EDGE + srcCount + OM_PADDING) {
		s_srcData.resize(OM_EDGE + srcCount + OM_PADDING);
	}
	data = s_srcData.data();
	memcpy(data, v->history[ch], sizeof(float) * OM_PADDING);
	buf = data + OM_EDGE;

	if (ended) {
		const size_t avail  = srcCount < OM_EDGE ? srcCount : OM_EDGE;
		const size_t toFill = srcCount > OM_EDGE ? srcCount : OM_EDGE;
		size_t       best   = 0;
		size_t       i;

		for (i = 1; i < avail; i++) {
			if (fabsf(buf[i]) < fabsf(buf[best])) {
				best = i;
			}
		}
		for (i = best + 1; i < toFill; i++) {
			buf[i] = buf[best];
		}
	} else {
		Om_LoadSamples(v, ch, v->posInt, v->queueCur, buf, srcCount);
	}

	if (v->step == OM_FRAC_ONE && v->posFrac == 0) {
		memcpy(out, buf, sizeof(float) * count);
	} else {
		Om_ResampleCubic(data + OM_EDGE - 1, v->posFrac, v->step, out, count);
	}

	if (storeHistory) {
		const size_t srcOffset = (size_t)(((uint64_t)count * v->step + v->posFrac) >> OM_FRAC_BITS);
		memcpy(v->history[ch], data + srcOffset, sizeof(float) * OM_PADDING);
	}
}

/*
 * Soft keeps the old voice for one more mix fading to silence (stop, restart, end of data).
 * Changed in Omaha: takes the outputs the voice last mixed to; a direct-HRTF voice hands
 * its filter to the tail, which keeps convolving until the HRIR has rung out.
 */
static void Om_QueueTailTo(omVoice_t *v, int channels, int outBase, int outCount, qboolean hrtfDirect)
{
	omTail_t tail;
	int      c;

	if (!v->fading || channels <= 0) {
		return;
	}
	if (hrtfDirect && !v->hrtf) {
		return;
	}
	memset(&tail, 0, sizeof(tail));
	tail.channels = channels > 2 ? 2 : channels;
	tail.outBase  = outBase;
	tail.outCount = outCount;
	for (c = 0; c < tail.channels; c++) {
		Om_ResampleChannel(v, c, qtrue, qfalse, tail.samples[c], OM_FADE_SAMPLES);
		memcpy(tail.gains[c], v->curGains[c], sizeof(tail.gains[c]));
	}
	if (hrtfDirect) {
		tail.channels       = 1;
		tail.outCount       = 0;
		tail.hrtf           = v->hrtf;
		tail.hrtfBlocksLeft = OmHrtf_TailBlocks() + (OM_FADE_SAMPLES + OM_HRTF_BLOCK - 1) / OM_HRTF_BLOCK;
		VectorCopy(v->hrtfDir, tail.hrtfDir);
		v->hrtf = NULL;
	}
	s_tails.push_back(tail);
}

static void Om_QueueTail(omVoice_t *v, int channels)
{
	Om_QueueTailTo(v, channels, v->outBase, v->outCount, v->hrtfDirect);
}

static int Om_VoiceChannels(const omVoice_t *v)
{
	const omPcm_t *pcm = Om_VoiceFirstPcm(v);
	return pcm ? (pcm->channels > 2 ? 2 : pcm->channels) : 0;
}

static void Om_MixTails(int count)
{
	const int counter = count < OM_FADE_SAMPLES ? count : OM_FADE_SAMPLES;
	size_t    t;

	size_t    keep    = 0;

	for (t = 0; t < s_tails.size(); t++) {
		omTail_t *tail = &s_tails[t];
		int       c, k;

		if (tail->hrtf) {
			/* Added in Omaha: fade the last input into the filter, then let it ring out. */
			memset(s_hrtfMono, 0, sizeof(float) * count);
			if (!tail->hrtfFaded) {
				Om_MixLine(tail->samples[0], s_hrtfMono, &tail->gains[0][0], 0.0f, counter, counter);
				tail->hrtfFaded = qtrue;
			}
			OmHrtf_VoiceProcess(tail->hrtf, tail->hrtfDir, s_hrtfMono, s_bus[OM_BUS_L], s_bus[OM_BUS_R], count);
			tail->hrtfBlocksLeft -= count / OM_HRTF_BLOCK;
			if (tail->hrtfBlocksLeft > 0) {
				s_tails[keep++] = *tail;
			} else {
				OmHrtf_VoiceFree(tail->hrtf);
			}
			continue;
		}
		for (c = 0; c < tail->channels; c++) {
			for (k = 0; k < tail->outCount; k++) {
				Om_MixLine(tail->samples[c], s_bus[tail->outBase + k], &tail->gains[c][k], 0.0f, counter, counter);
			}
		}
	}
	s_tails.resize(keep);
}

/* Added in Omaha: return every HRTF filter held by voices and tails (before unload). */
static void Om_ReleaseHrtfLocked(void)
{
	size_t i;

	for (i = 0; i < s_voices.size(); i++) {
		OmHrtf_VoiceFree(s_voices[i].hrtf);
		s_voices[i].hrtf       = NULL;
		s_voices[i].hrtfDirect = qfalse;
	}
	for (i = 0; i < s_tails.size(); i++) {
		OmHrtf_VoiceFree(s_tails[i].hrtf);
		s_tails[i].hrtf = NULL;
	}
	s_tails.clear();
	s_hrtfStashPos = 0;
	s_hrtfStashLen = 0;
}

/* core/voice.cpp Voice::mix for one line of at most OM_LINE_SIZE samples. */
static void Om_MixVoice(omVoice_t *v, int count)
{
	const omPcm_t *pcm = Om_VoiceFirstPcm(v);
	float          targets[2][OM_MAX_OUT];
	omSpatial_t    sp;
	int            channels, counter, c, k;

	if (!pcm) {
		v->mixing = qfalse;
		v->state  = SND_STATE_STOPPED;
		return;
	}
	channels = pcm->channels > 2 ? 2 : pcm->channels;

	Om_CalcSpatial(v, pcm, &sp);
	if (!s_hrtfOn) {
		Om_CalcTargets(v, pcm, &sp, qfalse, targets);
	} else {
		/* Added in Omaha: the direct filter is allocated on the first DIRECT mix. */
		const int      prevBase   = v->outBase;
		const int      prevCount  = v->outCount;
		const qboolean prevDirect = v->hrtfDirect;
		qboolean       allowDirect;

		if (v->route == SND_ROUTE_DIRECT && channels == 1 && !v->hrtf) {
			v->hrtf = OmHrtf_VoiceAlloc();
		}
		allowDirect = (v->route == SND_ROUTE_DIRECT && v->hrtf) ? qtrue : qfalse;
		Om_CalcTargets(v, pcm, &sp, allowDirect, targets);

		/* Route changed mid-sound: fade the old outputs out as a tail, fade the new ones in. */
		if (v->fading && (prevDirect != sp.direct || prevBase != v->outBase || prevCount != v->outCount)) {
			Om_QueueTailTo(v, channels, prevBase, prevCount, prevDirect);
			memset(v->curGains, 0, sizeof(v->curGains));
		}
		v->hrtfDirect = sp.direct;
	}

	/*
	 * Added in Omaha: distance + occlusion filter on the direct path.
	 * Changed in Omaha: s_obstruction 0 turns both
	 * axes off — distance darkening used to keep running when occlusion was cleared.
	 */
	{
		const qboolean spatial = v->directChannels == SND_DIRECT_CHANNELS_OFF ? qtrue : qfalse;
		const qboolean allowFilter =
			s_obstruction && s_obstruction->integer ? qtrue : qfalse;
		float          lpA     = 1.0f;
		qboolean       filter  = qfalse;

		if (!allowFilter) {
			v->obstructCur = 0.0f;
		} else {
			const float smoothSamples = OM_OBSTRUCT_SMOOTH_MS * 0.001f * (float)s_sampleRate;
			v->obstructCur += (v->obstructTarget - v->obstructCur) * (1.0f - expf(-(float)count / smoothSamples));
			if (fabsf(v->obstructTarget - v->obstructCur) < 0.0005f) {
				v->obstructCur = v->obstructTarget;
			}
		}

		if (allowFilter && spatial) {
			const float mix   = Om_Clamp(OM_OBSTRUCT_MIX, 0.0f, 1.0f);
			const float range = OM_OBSTRUCT_DIST_RANGE;
			const float mul   = OM_OBSTRUCT_DIST_MUL;
			const float distT = Om_Clamp(v->lastDist / (range * mul), 0.0f, 1.0f);
			const float occ   = v->obstructCur;

			const float distCutoff = Om_Lerp(OM_OBSTRUCT_OPEN_HZ, OM_OBSTRUCT_CUTOFF_HZ, distT);
			const float occCutoff  = Om_Lerp(OM_OBSTRUCT_OPEN_HZ, OM_OBSTRUCT_OCC_BLOCKED_HZ, occ);
			const float fc         = Om_Lerp(distCutoff, occCutoff, mix);
			const float needFilter =
				(mix < 0.001f) ? (distT >= OM_OBSTRUCT_EPSILON ? 1.0f : 0.0f)
							   : ((mix > 0.999f) ? (occ >= OM_OBSTRUCT_EPSILON ? 1.0f : 0.0f)
												 : ((distT >= OM_OBSTRUCT_EPSILON || occ >= OM_OBSTRUCT_EPSILON) ? 1.0f
																											  : 0.0f));

			/*
			 * Changed in Omaha: LPF is timbre-only. Distance loudness stays in
			 * Om_CalcSpatial. Makeup scales with pole coefficient a(fc) so a
			 * barely-engaged filter does not get a full boost (no AGC).
			 */
			if (needFilter > 0.0f && fc < (float)s_sampleRate * 0.45f) {
				int         kk;
				float       makeup;
				const float aSafe = fmaxf(lpA = 1.0f - expf(-2.0f * (float)M_PI * fmaxf(fc, 20.0f) / (float)s_sampleRate),
										  1.0e-4f);
				/* Tempered white-noise restore: ((2-a)/a)^0.25 */
				makeup = sqrtf(sqrtf((2.0f - aSafe) / aSafe));
				makeup = Om_Clamp(makeup, 1.0f, OM_OBSTRUCT_MAKEUP_MAX);
				filter = qtrue;
				for (c = 0; c < 2; c++) {
					for (kk = 0; kk < OM_MAX_OUT; kk++) {
						targets[c][kk] *= makeup;
					}
				}
			}
		}

		for (c = 0; c < channels; c++) {
			float *buf = s_resampled[c];

			Om_ResampleChannel(v, c, qfalse, v->pausing ? qfalse : qtrue, buf, count);

			if (filter) {
				float z = v->lpZ[c];
				for (k = 0; k < count; k++) {
					z += (buf[k] - z) * lpA;
					buf[k] = z;
				}
				v->lpZ[c] = z;
			} else {
				v->lpZ[c] = buf[count - 1];
			}
		}
	}

	/* Added in Omaha: rear head-shadow shelf (stereo mode, mono 3D sources only). */
	if (channels == 1) {
		const float smoothSamples = OM_REAR_SMOOTH_MS * 0.001f * (float)s_sampleRate;
		float      *buf           = s_resampled[0];

		v->rearCur += (sp.rear - v->rearCur) * (1.0f - expf(-(float)count / smoothSamples));
		if (fabsf(sp.rear - v->rearCur) < 0.0005f) {
			v->rearCur = sp.rear;
		}
		if (v->rearCur > 0.001f) {
			const float a  = 1.0f - expf(-2.0f * (float)M_PI * OM_REAR_SHELF_HZ / (float)s_sampleRate);
			const float hf = Om_Lerp(1.0f, OM_REAR_SHELF_HF, v->rearCur);
			float       z  = v->rearZ;
			for (k = 0; k < count; k++) {
				z += (buf[k] - z) * a;
				buf[k] = z + hf * (buf[k] - z);
			}
			v->rearZ = z;
		} else {
			v->rearZ = buf[count - 1];
		}
	}

	counter = v->fading ? (count < OM_FADE_SAMPLES ? count : OM_FADE_SAMPLES) : 0;
	if (!counter) {
		memcpy(v->curGains, targets, sizeof(v->curGains));
	}
	if (sp.direct) {
		/*
		 * Added in Omaha: gain ramp into a mono line, then this voice's own HRIR
		 * pair. Removed in Omaha: proximity dry/wet blend (combed with HRTF).
		 */
		const float target = v->pausing ? 0.0f : targets[0][0];

		memset(s_hrtfMono, 0, sizeof(float) * count);
		Om_MixLine(s_resampled[0], s_hrtfMono, &v->curGains[0][0], target, counter, count);
		OmHrtf_VoiceProcess(v->hrtf, sp.dir, s_hrtfMono, s_bus[OM_BUS_L], s_bus[OM_BUS_R], count);
		VectorCopy(sp.dir, v->hrtfDir);
	} else {
		for (c = 0; c < channels; c++) {
			for (k = 0; k < v->outCount; k++) {
				const float target = v->pausing ? 0.0f : targets[c][k];
				Om_MixLine(s_resampled[c], s_bus[v->outBase + k], &v->curGains[c][k], target, counter, count);
			}
		}
	}
	v->fading = qtrue;

	if (v->pausing) {
		/* Added in Omaha: a paused direct voice lets its filter ring out on a tail. */
		if (sp.direct) {
			Om_QueueTail(v, channels);
		}
		v->pausing = qfalse;
		v->mixing  = qfalse;
		return;
	}

	{
		uint64_t frac = (uint64_t)v->posFrac + (uint64_t)v->step * (uint64_t)count;
		v->posInt += (int)(frac >> OM_FRAC_BITS);
		v->posFrac = (unsigned)(frac & OM_FRAC_MASK);
	}

	if (Om_VoiceIsStatic(v)) {
		const int len = pcm->frames;
		if (v->looping) {
			size_t loopStart, loopEnd, loopSize;
			Om_VoiceLoopRange(v, len, &loopStart, &loopEnd);
			loopSize = loopEnd - loopStart;
			if (v->posInt >= (int)loopEnd) {
				v->posInt =
					(int)((((size_t)v->posInt - loopStart) % loopSize) + loopStart);
			}
			return;
		}
		if (v->posInt < len) {
			return;
		}
	} else {
		while (v->queueCur < (int)v->queue.size()) {
			const int len = v->queue[v->queueCur].frames;
			if (len > v->posInt) {
				return;
			}
			v->posInt -= len;
			v->queueCur++;
			if (v->queueCur >= (int)v->queue.size() && v->looping) {
				v->queueCur = 0;
			}
		}
	}

	/* Out of data: the source stops now, the voice fades out next line. */
	v->state  = SND_STATE_STOPPED;
	v->mixing = qfalse;
	Om_QueueTail(v, channels);
}

static void Om_MixLineBlock(float *out, int count)
{
	const int busCount = OM_BUS_AMBI + (s_hrtfOn ? OM_AMBI_CHANNELS : 3);
	size_t    i;
	int       o, j;

	for (o = 0; o < busCount; o++) {
		memset(s_bus[o], 0, sizeof(float) * count);
	}

	Om_MixTails(count);
	for (i = 0; i < s_voices.size(); i++) {
		if (s_voices[i].used && s_voices[i].mixing) {
			Om_MixVoice(&s_voices[i], count);
		}
	}

	if (s_hrtfOn) {
		/* Added in Omaha: binaural decode of the 3rd-order bus, on top of direct HRTF and dry L/R. */
		float *ambi[OM_AMBI_CHANNELS];
		for (o = 0; o < OM_AMBI_CHANNELS; o++) {
			ambi[o] = s_bus[OM_BUS_AMBI + o];
		}
		OmHrtf_AmbiDecode(ambi, s_bus[OM_BUS_L], s_bus[OM_BUS_R], count);
	} else
	/* BFormatDec single-band: add each ambisonic input into the real outputs. */
	for (o = 0; o < 2; o++) {
		float *real = s_bus[OM_BUS_L + o];
		int    a;
		for (a = 0; a < 3; a++) {
			const float  g  = s_stereoDecoder[o][a];
			const float *in = s_bus[OM_BUS_W + a];
			if (!(fabsf(g) > OM_GAIN_SILENCE)) {
				continue;
			}
			for (j = 0; j < count; j++) {
				real[j] += in[j] * g;
			}
		}
	}

	for (j = 0; j < count; j++) {
		out[j * 2 + 0] = s_bus[OM_BUS_L][j];
		out[j * 2 + 1] = s_bus[OM_BUS_R][j];
	}

	/* Added in Omaha: two-speaker timbre EQ before the limiter (headphones skip). */
	Om_ApplySpeakerEq(out, count);
	/* Added in Omaha: headphone timbre EQ after HRTF (two-speaker skip). */
	Om_ApplyHeadphoneEq(out, count);

	/* Added in Omaha: dynamic 15 kHz shelf duck after fixed EQ. */
	Om_ApplyHfDuck(out, count);

	/* Added in Omaha: master limiter after bus decode (pre-trim + threshold + makeup). */
	Om_ApplyLimiter(out, count);

	/* Changed in Omaha: master volume after the limiter, so it never changes how hard it works. */
	Om_ApplyMaster(out, count);
}

static void Om_Mix(float *out, int frames)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	if (s_hrtfOn) {
		/* Added in Omaha: whole HRTF blocks per line; the remainder waits in the stash. */
		while (frames > 0) {
			int n;
			if (s_hrtfStashLen <= 0) {
				int want = ((frames + OM_HRTF_BLOCK - 1) / OM_HRTF_BLOCK) * OM_HRTF_BLOCK;
				if (want > OM_LINE_SIZE) {
					want = OM_LINE_SIZE;
				}
				Om_MixLineBlock(s_hrtfStash, want);
				s_hrtfStashPos = 0;
				s_hrtfStashLen = want;
			}
			n = frames < s_hrtfStashLen ? frames : s_hrtfStashLen;
			memcpy(out, s_hrtfStash + s_hrtfStashPos * 2, sizeof(float) * 2 * n);
			s_hrtfStashPos += n;
			s_hrtfStashLen -= n;
			out += n * 2;
			frames -= n;
		}
		return;
	}
	while (frames > 0) {
		const int count = frames < OM_LINE_SIZE ? frames : OM_LINE_SIZE;
		Om_MixLineBlock(out, count);
		out += count * 2;
		frames -= count;
	}
}

static void Om_DataCallback(ma_device *dev, void *output, const void *input, ma_uint32 frameCount)
{
	(void)dev;
	(void)input;
	Om_Mix((float *)output, (int)frameCount);
}

/* core/voice.cpp LoadSamples<FmtIMA4> for the whole buffer. */
static const int s_imaStep[89] = {
	7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
	31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
	130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
	544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
	2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
	9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22358, 24633, 27086, 29794, 32767
};
static const int s_imaCodeword[16]     = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
static const int s_imaIndexAdjust[16]  = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static qboolean Om_DecodeIma4(const unsigned char *src, int sizeBytes, int channels, int samplesPerBlock, omPcm_t *out)
{
	const int blockBytes = ((samplesPerBlock - 1) / 2 + 4) * channels;
	int       blocks, b, ch;

	if (samplesPerBlock <= 1 || channels < 1 || channels > 2 || blockBytes <= 0) {
		return qfalse;
	}
	/* alBufferData rejects sizes that are not a whole number of blocks. */
	if (sizeBytes % blockBytes) {
		Com_DPrintf("Sound/omaha: IMA4 data is not block aligned (%d %% %d).\n", sizeBytes, blockBytes);
		return qfalse;
	}
	blocks       = sizeBytes / blockBytes;
	out->channels = channels;
	out->frames   = blocks * samplesPerBlock;
	out->data.assign((size_t)out->frames * channels, 0.0f);

	for (b = 0; b < blocks; b++) {
		const unsigned char *block = src + (size_t)b * blockBytes;
		for (ch = 0; ch < channels; ch++) {
			int                  sample  = block[ch * 4 + 0] | (block[ch * 4 + 1] << 8);
			int                  index   = block[ch * 4 + 2] | (block[ch * 4 + 3] << 8);
			const unsigned char *nibbles = block + (channels + ch) * 4;
			float               *dst     = &out->data[((size_t)b * samplesPerBlock) * channels + ch];
			int                  n;

			sample = (sample ^ 0x8000) - 32768;
			index  = (index ^ 0x8000) - 32768;
			index  = index < 0 ? 0 : (index > 88 ? 88 : index);

			dst[0] = (float)sample / 32768.0f;
			for (n = 0; n < samplesPerBlock - 1; n++) {
				const int byteShift  = (n & 1) * 4;
				const int wordOffset = (n >> 1) & ~3;
				const int byteOffset = wordOffset * channels + ((n >> 1) & 3);
				const int nibble     = (nibbles[byteOffset] >> byteShift) & 0xf;

				sample += s_imaCodeword[nibble] * s_imaStep[index] / 8;
				sample = sample < -32768 ? -32768 : (sample > 32767 ? 32767 : sample);
				index += s_imaIndexAdjust[nibble];
				index = index < 0 ? 0 : (index > 88 ? 88 : index);

				dst[(size_t)(n + 1) * channels] = (float)sample / 32768.0f;
			}
		}
	}
	return qtrue;
}

/* core/fmt_traits.h sample conversion. */
static qboolean Om_ConvertToFloat(
	sndSampleFormat_t fmt, const void *data, int sizeBytes, int rate, int blockAlign, omPcm_t *out
)
{
	int i, n;

	out->data.clear();
	out->rate     = rate;
	out->frames   = 0;
	out->channels = 0;

	switch (fmt) {
	case SND_FMT_MONO16:
	case SND_FMT_STEREO16:
		{
			const short *s = (const short *)data;
			out->channels  = fmt == SND_FMT_STEREO16 ? 2 : 1;
			n              = sizeBytes / (2 * out->channels);
			out->frames    = n;
			out->data.resize((size_t)n * out->channels);
			for (i = 0; i < n * out->channels; i++) {
				out->data[i] = (float)s[i] * (1.0f / 32768.0f);
			}
			return qtrue;
		}
	case SND_FMT_MONO8:
	case SND_FMT_STEREO8:
		{
			const unsigned char *s = (const unsigned char *)data;
			out->channels          = fmt == SND_FMT_STEREO8 ? 2 : 1;
			n                      = sizeBytes / out->channels;
			out->frames            = n;
			out->data.resize((size_t)n * out->channels);
			for (i = 0; i < n * out->channels; i++) {
				out->data[i] = (float)s[i] * (1.0f / 128.0f) - 1.0f;
			}
			return qtrue;
		}
	case SND_FMT_MONO_IMA4:
		return Om_DecodeIma4((const unsigned char *)data, sizeBytes, 1, blockAlign > 1 ? blockAlign : 65, out);
	case SND_FMT_STEREO_IMA4:
		return Om_DecodeIma4((const unsigned char *)data, sizeBytes, 2, blockAlign > 1 ? blockAlign : 65, out);
	default:
		return qfalse;
	}
}

static void Om_LogDevices(void)
{
	ma_device_info *playback = NULL;
	ma_uint32       count    = 0;
	ma_uint32       i;

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
	ma_uint32       count    = 0;
	ma_uint32       i;

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

/* Added in Omaha: s_hrtf_file through the engine FS (pk3 / homepath), resampled to the mix rate. */
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

static qboolean Om_Init(const sndDeviceConfig_t *cfg)
{
	ma_device_config dcfg;
	ma_device_id     devId;
	qboolean         haveId;
	ma_result        res;

	Om_InitSplineTable();

	s_sampleRate = cfg && cfg->sampleRate > 0 ? cfg->sampleRate : 44100;
	s_hrtfOn     = qfalse;
	OmHrtf_Unload();
	if (cfg && cfg->speakerType == 1) {
		/* Added in Omaha: headphones load the SOFA set; failure keeps omaha stereo. */
		s_hrtfOn = Om_LoadHrtf();
	} else if (cfg && cfg->speakerType != 0) {
		/* Selection should have routed surround layouts to Soft already. */
		Com_Printf("Sound/omaha: speaker_type %i unexpected; mixing stereo.\n", cfg->speakerType);
	}

	/* Added in Omaha: master limiter (1 = on). */
	s_limiter            = Cvar_Get("s_limiter", "1", CVAR_ARCHIVE);
	/* Added in Omaha: GR depth below the -0.2 dBFS ceiling (0 = fuse only, -12 = heavy). */
	s_limiter_threshold  = Cvar_Get("s_limiter_threshold", "-3.6", CVAR_ARCHIVE);
	/* Same cvar as the host: 0 disables distance + occlusion LPF in the mixer. */
	s_obstruction        = Cvar_Get("s_obstruction", "1", CVAR_ARCHIVE);
	/* Added in Omaha: two-speaker dynamic HF shelf duck (A/B with 0). */
	s_hf_duck            = Cvar_Get("s_hf_duck", "1", CVAR_ARCHIVE);
	s_hf_duck_db         = Cvar_Get("s_hf_duck_db", "24", CVAR_ARCHIVE);
	/* Added in Omaha: sidechain dB where shelf cut begins (full cut at 0 dB). */
	s_hf_duck_thresh     = Cvar_Get("s_hf_duck_thresh", "-15", CVAR_ARCHIVE);
	/* Added in Omaha: make-up on HP sidechain before envelope (0…24 dB). */
	s_hf_duck_sc_boost   = Cvar_Get("s_hf_duck_sc_boost", "5", CVAR_ARCHIVE);
	/* Same host cvar: HF duck only runs with Omaha mix-policy gains. */
	s_mix                = Cvar_Get("s_mix", "1", CVAR_ARCHIVE);
	Om_LimiterReset();
	Om_SpkEqReset();
	Om_HpEqReset();
	Om_HfDuckReset();

	s_buffers.clear();
	s_voices.clear();
	s_tails.clear();
	s_hrtfStashPos = 0;
	s_hrtfStashLen = 0;
	memset(s_listenerPos, 0, sizeof(s_listenerPos));
	memset(s_listenerVel, 0, sizeof(s_listenerVel));
	s_listenerAt[0] = 0;
	s_listenerAt[1] = 0;
	s_listenerAt[2] = -1;
	s_listenerUp[0] = 0;
	s_listenerUp[1] = 1;
	s_listenerUp[2] = 0;
	s_listenerGain  = 1.f;
	s_masterPrimed  = qfalse;
	s_captureActive = qfalse;

	s_contextReady = ma_context_init(NULL, 0, NULL, &s_context) == MA_SUCCESS ? qtrue : qfalse;
	if (s_contextReady && developer && developer->integer) {
		Om_LogDevices();
	}
	haveId = Om_FindDevice(cfg ? cfg->deviceName : NULL, &devId);

	dcfg                          = ma_device_config_init(ma_device_type_playback);
	dcfg.playback.format          = ma_format_f32;
	dcfg.playback.channels        = 2;
	dcfg.playback.pDeviceID       = haveId ? &devId : NULL;
	dcfg.sampleRate               = (ma_uint32)s_sampleRate;
	dcfg.dataCallback             = Om_DataCallback;
	dcfg.periodSizeInMilliseconds = 20;
	dcfg.periods                  = 3;
	if (s_hrtfOn) {
		/* Added in Omaha: ~20 ms in whole HRTF blocks, so pulls rarely leave a remainder. */
		const int blocks = (s_sampleRate / 50 + OM_HRTF_BLOCK / 2) / OM_HRTF_BLOCK;
		dcfg.periodSizeInFrames = (ma_uint32)((blocks > 0 ? blocks : 1) * OM_HRTF_BLOCK);
	}

	res = ma_device_init(s_contextReady ? &s_context : NULL, &dcfg, &s_device);
	if (res != MA_SUCCESS) {
		Com_Printf("Sound/omaha: ma_device_init failed (%d)\n", (int)res);
		if (s_contextReady) {
			ma_context_uninit(&s_context);
			s_contextReady = qfalse;
		}
		s_deviceReady = qfalse;
		return qfalse;
	}

	res = ma_device_start(&s_device);
	if (res != MA_SUCCESS) {
		Com_Printf("Sound/omaha: ma_device_start failed (%d)\n", (int)res);
		ma_device_uninit(&s_device);
		if (s_contextReady) {
			ma_context_uninit(&s_context);
			s_contextReady = qfalse;
		}
		s_deviceReady = qfalse;
		return qfalse;
	}

	s_deviceReady = qtrue;
	Com_Printf(
		"Sound/omaha: '%s' started (%d Hz %s f32, limiter %s thr %.1f dB, lookahead %.1f ms)\n",
		s_device.playback.name,
		s_sampleRate,
		s_hrtfOn ? "headphone HRTF" : "stereo",
		(s_limiter && s_limiter->integer) ? "on" : "off",
		s_limiter_threshold ? s_limiter_threshold->value : OM_LIMITER_THRESHOLD_DB,
		OM_LIMITER_LOOKAHEAD_MS
	);
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
	std::lock_guard<std::mutex> lock(s_mixMutex);
	Om_ReleaseHrtfLocked();
	OmHrtf_Unload();
	s_hrtfOn = qfalse;
	s_voices.clear();
	s_buffers.clear();
	s_tails.clear();
	s_captureActive = qfalse;
}

static sndBufferId_t Om_CreateBufferFromSfx(sfx_t *sfx)
{
	omBuffer_t b;
	(void)sfx;
	b.used         = qtrue;
	b.pcm.channels = 0;
	b.pcm.frames   = 0;
	b.pcm.rate     = 0;
	std::lock_guard<std::mutex> lock(s_mixMutex);
	s_buffers.push_back(std::move(b));
	return (sndBufferId_t)(s_buffers.size() - 1);
}

static void Om_DestroyBuffer(sndBufferId_t id)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	if (id < 0 || (size_t)id >= s_buffers.size()) {
		return;
	}
	s_buffers[id].used = qfalse;
	s_buffers[id].pcm.data.clear();
	s_buffers[id].pcm.frames = 0;
}

static qboolean Om_UploadBuffer(
	sndBufferId_t id, sndSampleFormat_t fmt, const void *data, int sizeBytes, int rate, int blockAlign
)
{
	omPcm_t pcm;

	if (!Om_ConvertToFloat(fmt, data, sizeBytes, rate, blockAlign, &pcm)) {
		return qfalse;
	}
	std::lock_guard<std::mutex> lock(s_mixMutex);
	if (id < 0 || (size_t)id >= s_buffers.size()) {
		return qfalse;
	}
	s_buffers[id].pcm = std::move(pcm);
	return qtrue;
}

static int Om_BufferFrequency(sndBufferId_t id)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	if (id < 0 || (size_t)id >= s_buffers.size()) {
		return 22050;
	}
	return s_buffers[id].pcm.rate > 0 ? s_buffers[id].pcm.rate : 22050;
}

static void Om_ResetVoiceProps(omVoice_t *v)
{
	/* OpenAL source defaults. */
	memset(v->pos, 0, sizeof(v->pos));
	memset(v->vel, 0, sizeof(v->vel));
	v->gain           = 1.f;
	v->maxGain        = 1.f;
	v->pitch          = 1.f;
	v->rolloff        = 1.f;
	v->refDist        = 1.f;
	v->maxDist        = FLT_MAX;
	v->relative       = 0;
	v->looping        = 0;
	v->loopStart      = 0;
	v->loopEnd        = -1;
	v->directChannels = SND_DIRECT_CHANNELS_OFF;
	v->obstructTarget = 0.f;
	v->lastDist       = 0.f;
	v->route          = SND_ROUTE_BUS;
}

static sndVoiceId_t Om_CreateVoice(void)
{
	omVoice_t v;

	v.used             = qtrue;
	v.state            = SND_STATE_INITIAL;
	v.buffer           = SND_BUFFER_NONE;
	v.hasPendingOffset = qfalse;
	v.pendingOffset    = 0;
	Om_ResetVoiceProps(&v);
	v.mixing   = qfalse;
	v.pausing  = qfalse;
	v.fading   = qfalse;
	v.queueCur = 0;
	v.posInt   = 0;
	v.posFrac  = 0;
	v.step     = OM_FRAC_ONE;
	v.outBase  = OM_BUS_W;
	v.outCount = 3;
	memset(v.history, 0, sizeof(v.history));
	memset(v.curGains, 0, sizeof(v.curGains));
	v.obstructCur = 0.f;
	memset(v.lpZ, 0, sizeof(v.lpZ));
	v.rearCur = 0.f;
	v.rearZ   = 0.f;
	v.hrtf       = NULL;
	v.hrtfDirect = qfalse;
	v.hrtfDir[0] = 0.f;
	v.hrtfDir[1] = 0.f;
	v.hrtfDir[2] = -1.f;
	std::lock_guard<std::mutex> lock(s_mixMutex);
	s_voices.push_back(std::move(v));
	return (sndVoiceId_t)(s_voices.size() - 1);
}

static omVoice_t *Om_Voice(sndVoiceId_t id)
{
	if (id < 0 || (size_t)id >= s_voices.size() || !s_voices[id].used) {
		return NULL;
	}
	return &s_voices[id];
}

/* Stop semantics shared by Stop / SetBuffer / Destroy: fade the old voice, rewind. */
static void Om_StopLocked(omVoice_t *v)
{
	if (v->mixing) {
		Om_QueueTail(v, Om_VoiceChannels(v));
	}
	v->mixing           = qfalse;
	v->pausing          = qfalse;
	v->posInt           = 0;
	v->posFrac          = 0;
	v->hasPendingOffset = qfalse;
	if (v->state != SND_STATE_INITIAL) {
		v->state = SND_STATE_STOPPED;
	}
	/* A stopped source reports every queued buffer as processed. */
	v->queueCur = (int)v->queue.size();
}

static void Om_DestroyVoice(sndVoiceId_t id)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (!v) {
		return;
	}
	Om_StopLocked(v);
	v->queue.clear();
	OmHrtf_VoiceFree(v->hrtf);
	v->hrtf       = NULL;
	v->hrtfDirect = qfalse;
	v->used       = qfalse;
}

static void Om_SetBuffer(sndVoiceId_t id, sndBufferId_t b)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (!v) {
		return;
	}
	/* alSourcei(AL_BUFFER) is only legal on stopped/initial sources; callers stop first. */
	if (v->mixing) {
		Om_StopLocked(v);
	}
	v->queue.clear();
	v->queueCur = 0;
	v->buffer   = b;
}

static void Om_ClearBuffer(sndVoiceId_t v)
{
	Om_SetBuffer(v, SND_BUFFER_NONE);
}

static void Om_SetParamf(sndVoiceId_t id, sndParam_t p, float x)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (!v) {
		return;
	}
	switch (p) {
	case SND_PARAM_GAIN:
		v->gain = x;
		break;
	case SND_PARAM_MAX_GAIN:
		v->maxGain = x;
		break;
	case SND_PARAM_PITCH:
		v->pitch = x;
		break;
	case SND_PARAM_ROLLOFF_FACTOR:
		v->rolloff = x;
		break;
	case SND_PARAM_REFERENCE_DISTANCE:
		v->refDist = x;
		break;
	case SND_PARAM_MAX_DISTANCE:
		v->maxDist = x;
		break;
	case SND_PARAM_OBSTRUCTION:
		v->obstructTarget = Om_Clamp(x, 0.f, 1.f);
		break;
	default:
		break;
	}
}

static void Om_SetParam3f(sndVoiceId_t id, sndParam_t p, float x, float y, float z)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (!v) {
		return;
	}
	if (p == SND_PARAM_POSITION) {
		v->pos[0] = x;
		v->pos[1] = y;
		v->pos[2] = z;
	} else if (p == SND_PARAM_VELOCITY) {
		v->vel[0] = x;
		v->vel[1] = y;
		v->vel[2] = z;
	}
}

static void Om_SetParami(sndVoiceId_t id, sndParam_t p, int x)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (!v) {
		return;
	}
	switch (p) {
	case SND_PARAM_SOURCE_RELATIVE:
		v->relative = x ? 1 : 0;
		break;
	case SND_PARAM_LOOPING:
		v->looping = x ? 1 : 0;
		break;
	case SND_PARAM_DIRECT_CHANNELS:
		v->directChannels = x;
		break;
	case SND_PARAM_ROLLOFF_FACTOR:
		v->rolloff = (float)x;
		break;
	case SND_PARAM_SPATIAL_ROUTE:
		/* Changed in Omaha: DRY = own sounds; DIRECT = per-voice HRTF; else bus. */
		if (x == SND_ROUTE_DIRECT) {
			v->route = SND_ROUTE_DIRECT;
		} else if (x == SND_ROUTE_DRY) {
			v->route = SND_ROUTE_DRY;
		} else {
			v->route = SND_ROUTE_BUS;
		}
		break;
	case SND_PARAM_DISTANCE_MODEL:
	case SND_PARAM_RESAMPLER:
		/* Per-source model is ignored without AL_SOURCE_DISTANCE_MODEL; resampler is fixed to spline. */
		break;
	default:
		break;
	}
}

static float Om_GetParamf(sndVoiceId_t id, sndParam_t p)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (!v) {
		return 0;
	}
	if (p == SND_PARAM_GAIN) {
		return v->gain;
	}
	if (p == SND_PARAM_PITCH) {
		return v->pitch;
	}
	return 0;
}

/* Added in Omaha: Miles/Soft partial loop region (sample frames). */
static void Om_SetLoopPoints(sndVoiceId_t id, int startSample, int endSample)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (!v) {
		return;
	}
	v->loopStart = startSample > 0 ? startSample : 0;
	v->loopEnd   = endSample;
}

/* Position a fresh voice at a sample offset; returns qfalse if past the data (al/source.cpp GetSampleOffset). */
static qboolean Om_SeekLocked(omVoice_t *v, unsigned offset)
{
	if (Om_VoiceIsStatic(v)) {
		const omPcm_t *pcm = Om_VoiceFirstPcm(v);
		if (!pcm || offset >= (unsigned)pcm->frames) {
			return qfalse;
		}
		v->posInt   = (int)offset;
		v->queueCur = 0;
	} else {
		unsigned total = 0;
		int      i;
		for (i = 0; i < (int)v->queue.size(); i++) {
			const unsigned len = (unsigned)v->queue[i].frames;
			if (offset < total + len) {
				v->posInt   = (int)(offset - total);
				v->queueCur = i;
				break;
			}
			total += len;
		}
		if (i >= (int)v->queue.size()) {
			return qfalse;
		}
	}
	v->posFrac = 0;
	return qtrue;
}

static void Om_StartFreshVoice(omVoice_t *v, qboolean useOffset, unsigned offset)
{
	v->posInt   = 0;
	v->posFrac  = 0;
	v->queueCur = 0;
	v->fading   = qfalse;
	v->pausing  = qfalse;
	memset(v->history, 0, sizeof(v->history));
	memset(v->curGains, 0, sizeof(v->curGains));
	/* Added in Omaha: a new sound starts at its obstruction target, filter cleared. */
	v->obstructCur = v->obstructTarget;
	memset(v->lpZ, 0, sizeof(v->lpZ));
	v->rearCur = 0.f;
	v->rearZ   = 0.f;
	/* Added in Omaha: a kept direct filter starts empty (no tail took it). */
	if (v->hrtf) {
		OmHrtf_VoiceReset(v->hrtf);
	}
	v->hrtfDirect = qfalse;
	if (useOffset && Om_SeekLocked(v, offset)) {
		if (v->posInt > 0 || v->queueCur > 0) {
			v->fading = qtrue;
		}
	}
	v->mixing = qtrue;
	v->state  = SND_STATE_PLAYING;
}

/* al/source.cpp StartSources. */
static void Om_Play(sndVoiceId_t id)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (!v) {
		return;
	}

	if (!Om_VoiceFirstPcm(v)) {
		v->state = SND_STATE_STOPPED;
		return;
	}

	if (v->state == SND_STATE_PAUSED) {
		/* Same voice resumes; it fades back in from the paused (silent) gains. */
		v->pausing = qfalse;
		v->mixing  = qtrue;
		v->state   = SND_STATE_PLAYING;
		return;
	}

	if (v->state == SND_STATE_PLAYING && v->mixing) {
		Om_QueueTail(v, Om_VoiceChannels(v));
	}
	Om_StartFreshVoice(v, v->hasPendingOffset, v->pendingOffset);
	v->hasPendingOffset = qfalse;
	v->pendingOffset    = 0;
}

static void Om_Pause(sndVoiceId_t id)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (v && v->state == SND_STATE_PLAYING) {
		v->state = SND_STATE_PAUSED;
		if (v->mixing) {
			v->pausing = qtrue;
		}
	}
}

static void Om_Stop(sndVoiceId_t id)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (!v) {
		return;
	}
	if (v->state == SND_STATE_INITIAL) {
		/* alSourceStop on an initial source moves it to stopped. */
		v->state = SND_STATE_STOPPED;
	}
	Om_StopLocked(v);
}

static int Om_GetState(sndVoiceId_t id)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (!v) {
		return SND_STATE_STOPPED;
	}
	return v->state;
}

/* AL_SAMPLE_OFFSET: frames from the start of the queue, including processed buffers. */
static unsigned Om_GetSampleOffset(sndVoiceId_t id)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	unsigned                    offset = 0;
	int                         i;

	if (!v || (v->state != SND_STATE_PLAYING && v->state != SND_STATE_PAUSED)) {
		return 0;
	}
	if (!Om_VoiceIsStatic(v)) {
		for (i = 0; i < v->queueCur && i < (int)v->queue.size(); i++) {
			offset += (unsigned)v->queue[i].frames;
		}
	}
	return offset + (unsigned)(v->posInt > 0 ? v->posInt : 0);
}

static void Om_SetSampleOffset(sndVoiceId_t id, unsigned off)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (!v) {
		return;
	}
	if (v->state == SND_STATE_PLAYING && v->mixing) {
		/* Seeking a playing source restarts it on a new voice; the old one fades out. */
		omVoice_t probe = *v;
		if (!Om_SeekLocked(&probe, off)) {
			return;
		}
		Om_QueueTail(v, Om_VoiceChannels(v));
		Om_StartFreshVoice(v, qtrue, off);
		return;
	}
	if (v->state == SND_STATE_PAUSED) {
		if (Om_SeekLocked(v, off)) {
			memset(v->history, 0, sizeof(v->history));
			v->fading = (v->posInt > 0 || v->queueCur > 0) ? qtrue : qfalse;
		}
		return;
	}
	v->hasPendingOffset = qtrue;
	v->pendingOffset    = off;
}

static void Om_QueueStreamBuffer(
	sndVoiceId_t id, const void *data, int bytes, sndSampleFormat_t fmt, int rate, int blockAlign
)
{
	omPcm_t pcm;

	if (!data || bytes <= 0 || !Om_ConvertToFloat(fmt, data, bytes, rate, blockAlign, &pcm) || pcm.frames <= 0) {
		return;
	}

	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (!v) {
		return;
	}
	if (v->queue.empty()) {
		v->buffer = SND_BUFFER_NONE;
	}
	v->queue.push_back(std::move(pcm));
}

static int Om_StreamBuffersQueued(sndVoiceId_t id)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	return v ? (int)v->queue.size() : 0;
}

static int Om_StreamBuffersProcessed(sndVoiceId_t id)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	if (!v) {
		return 0;
	}
	if (v->state == SND_STATE_STOPPED) {
		return (int)v->queue.size();
	}
	if (v->state == SND_STATE_INITIAL) {
		return 0;
	}
	return v->queueCur < (int)v->queue.size() ? v->queueCur : (int)v->queue.size();
}

static void Om_UnqueueProcessed(sndVoiceId_t id, int count)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	omVoice_t                  *v = Om_Voice(id);
	int                         processed;
	if (!v || count <= 0) {
		return;
	}
	processed = v->state == SND_STATE_STOPPED ? (int)v->queue.size()
			  : (v->state == SND_STATE_INITIAL ? 0 : v->queueCur);
	if (count > processed) {
		count = processed;
	}
	while (count-- > 0 && !v->queue.empty()) {
		v->queue.pop_front();
		if (v->queueCur > 0) {
			v->queueCur--;
		}
	}
}

static void Om_SetListener(const float pos[3], const float vel[3], const float at[3], const float up[3])
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	if (pos) {
		memcpy(s_listenerPos, pos, sizeof(s_listenerPos));
	}
	if (vel) {
		memcpy(s_listenerVel, vel, sizeof(s_listenerVel));
	}
	if (at && up) {
		memcpy(s_listenerAt, at, sizeof(s_listenerAt));
		memcpy(s_listenerUp, up, sizeof(s_listenerUp));
	}
}

static void Om_SetListenerGain(float g)
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	s_listenerGain = g;
}

static void Om_GetListenerPosition(float out[3])
{
	std::lock_guard<std::mutex> lock(s_mixMutex);
	if (out) {
		memcpy(out, s_listenerPos, sizeof(s_listenerPos));
	}
}

/* Pull mode: the live device stops and the caller renders exact block sizes. */
static qboolean Om_BeginCapture(int sampleRate)
{
	(void)sampleRate;
	if (s_captureActive) {
		return qtrue;
	}
	if (s_deviceReady) {
		ma_device_stop(&s_device);
	}
	s_captureActive = qtrue;
	return qtrue;
}

static int Om_RenderCapture(float *interleavedStereo, int frames)
{
	if (!s_captureActive || !interleavedStereo || frames <= 0) {
		return 0;
	}
	Om_Mix(interleavedStereo, frames);
	return frames;
}

static void Om_EndCapture(void)
{
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
	Om_CreateBufferFromSfx,
	Om_DestroyBuffer,
	Om_UploadBuffer,
	Om_BufferFrequency,
	Om_CreateVoice,
	Om_DestroyVoice,
	Om_SetBuffer,
	Om_ClearBuffer,
	Om_SetParamf,
	Om_SetParam3f,
	Om_SetParami,
	Om_GetParamf,
	Om_Play,
	Om_Pause,
	Om_Stop,
	Om_GetState,
	Om_GetSampleOffset,
	Om_SetSampleOffset,
	Om_SetLoopPoints,
	Om_QueueStreamBuffer,
	Om_StreamBuffersQueued,
	Om_StreamBuffersProcessed,
	Om_UnqueueProcessed,
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

/* Added in Omaha: HUD meter reads (mixer thread writes). */
extern "C" float S_Omaha_HfDuckGainDb(void)
{
	return s_hfDuckMeterDb;
}

extern "C" float S_Omaha_HfDuckMaxCutDb(void)
{
	float maxCutDb = s_hf_duck_db ? s_hf_duck_db->value : OM_HF_DUCK_DEFAULT_DB;
	if (maxCutDb < 0.0f) {
		maxCutDb = -maxCutDb;
	}
	if (maxCutDb > 24.0f) {
		maxCutDb = 24.0f;
	}
	return maxCutDb;
}

extern "C" float S_Omaha_HfDuckSidechainDb(void)
{
	return s_hfDuckSidechainDb;
}

extern "C" float S_Omaha_HfDuckThreshDb(void)
{
	return Om_HfDuckThreshDb();
}
