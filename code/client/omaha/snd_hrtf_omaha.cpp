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
 * Added in Omaha: headphone HRTF for the omaha mixer. See snd_hrtf_omaha.h.
 */

#include "../snd_local.h"
#include "snd_hrtf_omaha.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "mysofa.h"
#include "pffft.h"
#if defined(__SSE2__)
#include <immintrin.h>
#endif

#ifndef M_PI
#	define M_PI 3.14159265358979323846
#endif

#define OM_HRTF_FFT          (OM_HRTF_BLOCK * 2)
#define OM_HRTF_MAX_PARTS    16
/* Hard cap on per-ear onset delay (samples). Human ITD is ~0.7 ms; SOFA onsets are larger. */
#define OM_HRTF_MAX_DELAY    256
/* Re-query a voice's HRIR once it has turned more than ~1 degree. */
#define OM_HRTF_REQUERY_COS  0.99984770f
/* Front HRIR energy per ear matches the stereo decoder's front gain (0.5957^2). */
#define OM_HRTF_FRONT_ENERGY 0.35488f

/* Decoder / min-phase design (load time + rare voice re-queries). */
#define OM_LEBEDEV_POINTS     50
#define OM_DESIGN_FFT         1024
/* Causal room for MagLS filters; the bus runs this many samples behind direct voices. */
#define OM_DESIGN_DELAY       64
#define OM_DESIGN_TEST_POINTS 240
/*
 * MagLS (Zaunschirm et al.): keep complex LS / ITD below the duplex transition,
 * match HRTF magnitude only above it so order-3 does not comb when the head turns.
 * Changed in Omaha: Hold et al. (DAGA 2023) average phase continuation above the
 * cut so MagLS does not collapse HF group delay into dispersion / hollowness.
 */
#define OM_MAGLS_LO_HZ        1500.0f
#define OM_MAGLS_HI_HZ        2500.0f
#define OM_MAGLS_ITERS        12
#define OM_DFEQ_MIN           0.25f
#define OM_DFEQ_MAX           4.0f
#define OM_ONSET_THRESH       0.05f
#define OM_SOFA_EPS           1.0e-5f
/*
 * Added in Omaha: dense DIRECT bake on a direction lattice.
 */
#define OM_BAKE_STEP_DEG       1.5f
#define OM_BAKE_GATE_DIRS      64
/* Relative residual energy vs live spectra (dB): good matches are largely negative. */
#define OM_BAKE_GATE_MED_DB    (-20.0f)
#define OM_BAKE_GATE_MAX_DB    (-10.0f)
#define OM_BAKE_GATE_MED_DELAY 0.25f

struct omHrtfVoice_s {
	float   *fdl;     /* [parts][FFT] input spectra ring */
	float   *filt[2]; /* two filter slots, each [ear][parts][FFT] */
	float   *inBuf;   /* [FFT]: previous block | current block */
	float   *delayBuf[2]; /* per-ear ring for ITD (length delayLen) */
	int      delayPos[2];
	float    delay[2];    /* current per-ear delay (samples), smoothed */
	float    delayTgt[2]; /* target from last HRIR query */
	int      fdlPos;
	int      cur;
	int      zeroRun;
	unsigned char slotZero[OM_HRTF_MAX_PARTS]; /* Added in Omaha: skip exact-zero FDL slots. */
	qboolean hasFilter;
	float    filtDir[3];
};

static struct {
	qboolean            ready;
	int                 sampleRate;
	struct MYSOFA_EASY *easy;
	int                 irLen;
	int                 parts; /* direct voice filters (time-aligned IR only) */
	int                 maxDelay;
	int                 delayLen; /* ring length: maxDelay + 2 */
	float               scale;
	PFFFT_Setup        *setup;
	PFFFT_Setup        *mpSetup; /* OM_DESIGN_FFT: min-phase + MagLS design */

	/* Scratch, single-threaded (mixer or loader). */
	float *work;
	float *time;
	float *acc;
	float *accB; /* Added in Omaha: second accumulator so both ears convolve in one walk. */
	float *padded;
	float *shifted;
	float *irL;
	float *irR;
	float *mpBuf;
	float *mpSpec;
	float *mpWork;

	/* Ambisonic decoder. */
	int    ambiParts;
	float *ambiFilt; /* [ch][ear][ambiParts][FFT] */
	float *ambiFdl;  /* [ch][ambiParts][FFT] */
	float *ambiIn;   /* [ch][FFT] */
	int    ambiFdlPos;
	int    ambiZeroRun[OM_AMBI_CHANNELS];

	/*
	 * Added in Omaha: DIRECT elev/az bake. Slot layout [dir][ear][parts][FFT].
	 * sofaLive: runtime still queries SOFA (gate fail or s_hrtf_live_query).
	 */
	int      bakeEvCount;
	int      bakeDirCount;
	int     *bakeAzCount;  /* [evCount] */
	int     *bakeIrOffset; /* [evCount] first dir index on that ring */
	float   *bakeFilt;
	float   *bakeDelay; /* [dir][2] */
	qboolean bakeReady;
	qboolean sofaLive;
	float    bakeGateMaxMagDb;
	float    bakeGateMedMagDb;
	float    bakeGateMedDelay;

	std::vector<omHrtfVoice_t *> freeVoices;
} s_hrtf;

static const int s_ambiOrderOfAcn[OM_AMBI_CHANNELS] = {0, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3};

static float *Hrtf_Alloc(int count)
{
	float *p = (float *)pffft_aligned_malloc(sizeof(float) * (size_t)count);
	if (p) {
		memset(p, 0, sizeof(float) * (size_t)count);
	}
	return p;
}

static void Hrtf_Free(float **p)
{
	if (*p) {
		pffft_aligned_free(*p);
		*p = NULL;
	}
}

/* Added in Omaha: workers point these at private scratch; the mixer thread leaves them null. */
static thread_local float *t_work;
static thread_local float *t_acc;
static thread_local float *t_accB;
static thread_local float *t_time;

static float *Hrtf_Work(void)
{
	return t_work ? t_work : s_hrtf.work;
}
static float *Hrtf_Acc(void)
{
	return t_acc ? t_acc : s_hrtf.acc;
}
static float *Hrtf_AccB(void)
{
	return t_accB ? t_accB : s_hrtf.accB;
}
static float *Hrtf_Time(void)
{
	return t_time ? t_time : s_hrtf.time;
}

void OmHrtf_SetThreadScratch(float *work, float *acc, float *accB, float *time)
{
	t_work = work;
	t_acc  = acc;
	t_accB = accB;
	t_time = time;
}

void OmHrtf_AllocScratch(float **work, float **acc, float **accB, float **time)
{
	*work = Hrtf_Alloc(OM_HRTF_FFT);
	*acc  = Hrtf_Alloc(OM_HRTF_FFT);
	*accB = Hrtf_Alloc(OM_HRTF_FFT);
	*time = Hrtf_Alloc(OM_HRTF_FFT);
}

void OmHrtf_FreeScratch(float *work, float *acc, float *accB, float *time)
{
	Hrtf_Free(&work);
	Hrtf_Free(&acc);
	Hrtf_Free(&accB);
	Hrtf_Free(&time);
}

/* Real spherical harmonics, ACN order, N3D (orthonormal). x front, y left, z up. */
static void Hrtf_AmbiCoeffsXYZ(float x, float y, float z, float c[OM_AMBI_CHANNELS])
{
	const float xx = x * x, yy = y * y, zz = z * z, xy = x * y, yz = y * z, xz = x * z;
	const float s3  = sqrtf(3.f);
	const float s5  = sqrtf(5.f);
	const float s15 = sqrtf(15.f);
	const float s35_8 = sqrtf(35.f / 8.f);
	const float s105  = sqrtf(105.f);
	const float s21_8 = sqrtf(21.f / 8.f);

	c[0]  = 1.0f;
	c[1]  = s3 * y;
	c[2]  = s3 * z;
	c[3]  = s3 * x;
	c[4]  = s15 * xy;
	c[5]  = s15 * yz;
	c[6]  = 0.5f * s5 * (3.0f * zz - 1.0f);
	c[7]  = s15 * xz;
	c[8]  = 0.5f * s15 * (xx - yy);
	c[9]  = s35_8 * (y * (3.0f * xx - yy));
	c[10] = s105 * (z * xy);
	c[11] = s21_8 * (y * (5.0f * zz - 1.0f));
	c[12] = 0.5f * sqrtf(7.f) * (z * (5.0f * zz - 3.0f));
	c[13] = s21_8 * (x * (5.0f * zz - 1.0f));
	c[14] = 0.5f * s105 * (z * (xx - yy));
	c[15] = s35_8 * (x * (xx - 3.0f * yy));
}

void OmHrtf_AmbiCoeffs(const float dir[3], float coeffs[OM_AMBI_CHANNELS])
{
	Hrtf_AmbiCoeffsXYZ(dir[0], dir[1], dir[2], coeffs);
}

/* Lebedev degree-11 grid (exact for SH products through order 5); weights sum to 1. */
static int Hrtf_Lebedev50(float pts[OM_LEBEDEV_POINTS][3], float w[OM_LEBEDEV_POINTS])
{
	const float a2 = 0.70710678118654752f;
	const float a3 = 0.57735026918962576f;
	const float bl = 0.30151134457776357f;
	const float bn = 0.90453403373329089f;
	int         n  = 0;
	int         i, s, axis;

	for (axis = 0; axis < 3; axis++) {
		for (s = -1; s <= 1; s += 2) {
			pts[n][0] = pts[n][1] = pts[n][2] = 0.0f;
			pts[n][axis] = (float)s;
			w[n++]       = 4.0f / 315.0f;
		}
	}
	for (axis = 0; axis < 3; axis++) {
		for (i = 0; i < 4; i++) {
			pts[n][axis]           = 0.0f;
			pts[n][(axis + 1) % 3] = (i & 1) ? -a2 : a2;
			pts[n][(axis + 2) % 3] = (i & 2) ? -a2 : a2;
			w[n++]                 = 64.0f / 2835.0f;
		}
	}
	for (i = 0; i < 8; i++) {
		pts[n][0] = (i & 1) ? -a3 : a3;
		pts[n][1] = (i & 2) ? -a3 : a3;
		pts[n][2] = (i & 4) ? -a3 : a3;
		w[n++]    = 27.0f / 1280.0f;
	}
	for (axis = 0; axis < 3; axis++) {
		for (i = 0; i < 8; i++) {
			pts[n][axis]           = (i & 1) ? -bn : bn;
			pts[n][(axis + 1) % 3] = (i & 2) ? -bl : bl;
			pts[n][(axis + 2) % 3] = (i & 4) ? -bl : bl;
			w[n++]                 = 14641.0f / 725760.0f;
		}
	}
	return n;
}

/* First sample past OM_ONSET_THRESH of the peak — residual onset left in the FIR. */
static int Hrtf_FindOnset(const float *ir, int len)
{
	float peak = 0.0f;
	int   i;

	for (i = 0; i < len; i++) {
		const float a = fabsf(ir[i]);
		if (a > peak) {
			peak = a;
		}
	}
	if (peak < 1.0e-12f) {
		return 0;
	}
	{
		const float thr = peak * OM_ONSET_THRESH;
		for (i = 0; i < len; i++) {
			if (fabsf(ir[i]) >= thr) {
				return i;
			}
		}
	}
	return 0;
}

static float Hrtf_CartDist(const float *a, const float *b)
{
	const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

static float Hrtf_SofaDelay(struct MYSOFA_HRTF *hrtf, int index, int ear)
{
	if (hrtf->DataDelay.elements > hrtf->R) {
		return hrtf->DataDelay.values[(size_t)index * hrtf->R + ear];
	}
	return hrtf->DataDelay.values[ear];
}

/*
 * Added in Omaha: same neighbor pick as mysofa_interpolate, but onset-align each
 * HRIR before the weighted sum and fold onset into the returned delay. Unaligned
 * time-domain blends comb; min-phase would then bake those notches into |H|.
 */
static void Hrtf_SofaGet(float x, float y, float z, float *irL, float *irR, float delays[2])
{
	struct MYSOFA_EASY *easy = s_hrtf.easy;
	struct MYSOFA_HRTF *hrtf = easy->hrtf;
	float               c[3];
	float               accL[OM_DESIGN_FFT], accR[OM_DESIGN_FFT];
	float               d6[6];
	int                 use[6];
	int                 nearest, *neighborhood, N, size, i;
	float               d, weight;

	c[0] = x;
	c[1] = y;
	c[2] = z;
	nearest      = mysofa_lookup(easy->lookup, c);
	neighborhood = mysofa_neighborhood(easy->neighborhood, nearest);
	N            = (int)hrtf->N;
	size         = N * (int)hrtf->R;
	if (N > OM_DESIGN_FFT) {
		N = OM_DESIGN_FFT;
	}

	d = Hrtf_CartDist(c, hrtf->SourcePosition.values + nearest * hrtf->C);
	if (d < OM_SOFA_EPS) {
		const float *src = hrtf->DataIR.values + (size_t)nearest * size;
		float       *outs[2];
		int          e, onset, keep, t;

		outs[0] = irL;
		outs[1] = irR;
		for (e = 0; e < 2; e++) {
			const float *in = src + e * hrtf->N;
			memcpy(outs[e], in, sizeof(float) * N);
			onset = Hrtf_FindOnset(outs[e], N);
			delays[e] = Hrtf_SofaDelay(hrtf, nearest, e) + (float)onset;
			if (delays[e] < 0.0f) {
				delays[e] = 0.0f;
			}
			keep = N - onset;
			if (keep < 1) {
				keep = 1;
			}
			if (onset > 0) {
				memmove(outs[e], outs[e] + onset, sizeof(float) * keep);
				memset(outs[e] + keep, 0, sizeof(float) * (N - keep));
			}
			for (t = N; t < s_hrtf.irLen; t++) {
				outs[e][t] = 0.0f;
			}
		}
		return;
	}

	for (i = 0; i < 6; i++) {
		use[i] = 0;
		d6[i]  = 1.0f;
	}
	if (neighborhood[0] >= 0 && neighborhood[1] >= 0) {
		d6[0] = Hrtf_CartDist(c, hrtf->SourcePosition.values + neighborhood[0] * hrtf->C);
		d6[1] = Hrtf_CartDist(c, hrtf->SourcePosition.values + neighborhood[1] * hrtf->C);
		if (fabsf(d6[0] - d6[1]) >= OM_SOFA_EPS) {
			use[d6[0] < d6[1] ? 0 : 1] = 1;
		}
	} else if (neighborhood[0] >= 0) {
		d6[0]  = Hrtf_CartDist(c, hrtf->SourcePosition.values + neighborhood[0] * hrtf->C);
		use[0] = 1;
	} else if (neighborhood[1] >= 0) {
		d6[1]  = Hrtf_CartDist(c, hrtf->SourcePosition.values + neighborhood[1] * hrtf->C);
		use[1] = 1;
	}
	if (neighborhood[2] >= 0 && neighborhood[3] >= 0) {
		d6[2] = Hrtf_CartDist(c, hrtf->SourcePosition.values + neighborhood[2] * hrtf->C);
		d6[3] = Hrtf_CartDist(c, hrtf->SourcePosition.values + neighborhood[3] * hrtf->C);
		if (fabsf(d6[2] - d6[3]) >= OM_SOFA_EPS) {
			use[d6[2] < d6[3] ? 2 : 3] = 1;
		}
	} else if (neighborhood[2] >= 0) {
		d6[2]  = Hrtf_CartDist(c, hrtf->SourcePosition.values + neighborhood[2] * hrtf->C);
		use[2] = 1;
	} else if (neighborhood[3] >= 0) {
		d6[3]  = Hrtf_CartDist(c, hrtf->SourcePosition.values + neighborhood[3] * hrtf->C);
		use[3] = 1;
	}
	if (neighborhood[4] >= 0 && neighborhood[5] >= 0) {
		d6[4] = Hrtf_CartDist(c, hrtf->SourcePosition.values + neighborhood[4] * hrtf->C);
		d6[5] = Hrtf_CartDist(c, hrtf->SourcePosition.values + neighborhood[5] * hrtf->C);
		if (fabsf(d6[4] - d6[5]) >= OM_SOFA_EPS) {
			use[d6[4] < d6[5] ? 4 : 5] = 1;
		}
	} else if (neighborhood[4] >= 0) {
		d6[4]  = Hrtf_CartDist(c, hrtf->SourcePosition.values + neighborhood[4] * hrtf->C);
		use[4] = 1;
	} else if (neighborhood[5] >= 0) {
		d6[5]  = Hrtf_CartDist(c, hrtf->SourcePosition.values + neighborhood[5] * hrtf->C);
		use[5] = 1;
	}

	memset(accL, 0, sizeof(float) * N);
	memset(accR, 0, sizeof(float) * N);
	delays[0] = delays[1] = 0.0f;
	weight                = 0.0f;

	{
		const int indices[7] = {
			nearest,
			use[0] ? neighborhood[0] : -1,
			use[1] ? neighborhood[1] : -1,
			use[2] ? neighborhood[2] : -1,
			use[3] ? neighborhood[3] : -1,
			use[4] ? neighborhood[4] : -1,
			use[5] ? neighborhood[5] : -1
		};
		const float dists[7] = {d, d6[0], d6[1], d6[2], d6[3], d6[4], d6[5]};

		for (i = 0; i < 7; i++) {
			const int   idx = indices[i];
			const float w   = (idx >= 0 && dists[i] > OM_SOFA_EPS) ? (1.0f / dists[i]) : (idx >= 0 ? 1.0f : 0.0f);
			const float *src;
			int          onset[2], t, e;

			if (idx < 0 || w <= 0.0f) {
				continue;
			}
			src = hrtf->DataIR.values + (size_t)idx * size;
			for (e = 0; e < 2; e++) {
				const float *ir = src + e * hrtf->N;
				onset[e]        = Hrtf_FindOnset(ir, N);
				delays[e] += (Hrtf_SofaDelay(hrtf, idx, e) + (float)onset[e]) * w;
			}
			for (t = 0; t < N; t++) {
				const int iL = t + onset[0];
				const int iR = t + onset[1];
				if (iL < N) {
					accL[t] += src[iL] * w;
				}
				if (iR < N) {
					accR[t] += src[hrtf->N + iR] * w;
				}
			}
			weight += w;
		}
	}

	if (weight < 1.0e-12f) {
		const float *src = hrtf->DataIR.values + (size_t)nearest * size;
		float       *outs[2];
		int          e, onset, keep, t;

		outs[0] = irL;
		outs[1] = irR;
		for (e = 0; e < 2; e++) {
			memcpy(outs[e], src + e * hrtf->N, sizeof(float) * N);
			onset     = Hrtf_FindOnset(outs[e], N);
			delays[e] = Hrtf_SofaDelay(hrtf, nearest, e) + (float)onset;
			if (delays[e] < 0.0f) {
				delays[e] = 0.0f;
			}
			keep = N - onset;
			if (keep < 1) {
				keep = 1;
			}
			if (onset > 0) {
				memmove(outs[e], outs[e] + onset, sizeof(float) * keep);
				memset(outs[e] + keep, 0, sizeof(float) * (N - keep));
			}
			for (t = N; t < s_hrtf.irLen; t++) {
				outs[e][t] = 0.0f;
			}
		}
		return;
	}
	weight = 1.0f / weight;
	for (i = 0; i < N; i++) {
		irL[i] = accL[i] * weight;
		irR[i] = accR[i] * weight;
	}
	for (; i < s_hrtf.irLen; i++) {
		irL[i] = 0.0f;
		irR[i] = 0.0f;
	}
	delays[0] *= weight;
	delays[1] *= weight;
	if (delays[0] < 0.0f) {
		delays[0] = 0.0f;
	}
	if (delays[1] < 0.0f) {
		delays[1] = 0.0f;
	}
}

/*
 * Cepstral minimum-phase reconstruction of `ir` (length irLen <= OM_DESIGN_FFT).
 * Preserves |H(f)| (pinna notches / ILD) while putting all excess phase into the
 * separate delay line — crossfades then do not comb.
 */
static void Hrtf_MinPhase(float *ir, int irLen)
{
	const int   N    = OM_DESIGN_FFT;
	float      *buf  = s_hrtf.mpBuf;
	float      *spec = s_hrtf.mpSpec;
	float      *work = s_hrtf.mpWork;
	int         k, i;

	memset(buf, 0, sizeof(float) * N);
	memcpy(buf, ir, sizeof(float) * irLen);
	pffft_transform_ordered(s_hrtf.mpSetup, buf, spec, work, PFFFT_FORWARD);

	/* Real log-magnitude spectrum. */
	spec[0] = logf(fabsf(spec[0]) + 1.0e-20f);
	spec[1] = logf(fabsf(spec[1]) + 1.0e-20f);
	for (k = 1; k < N / 2; k++) {
		const float re = spec[2 * k], im = spec[2 * k + 1];
		spec[2 * k]     = 0.5f * logf(re * re + im * im + 1.0e-20f);
		spec[2 * k + 1] = 0.0f;
	}
	pffft_transform_ordered(s_hrtf.mpSetup, spec, buf, work, PFFFT_BACKWARD);

	/* Causal cepstrum window → minimum phase. */
	buf[0] *= 1.0f / (float)N;
	for (i = 1; i < N / 2; i++) {
		buf[i] *= 2.0f / (float)N;
	}
	buf[N / 2] *= 1.0f / (float)N;
	for (i = N / 2 + 1; i < N; i++) {
		buf[i] = 0.0f;
	}
	pffft_transform_ordered(s_hrtf.mpSetup, buf, spec, work, PFFFT_FORWARD);

	/* exp(real + j imag) */
	spec[0] = expf(spec[0]);
	spec[1] = expf(spec[1]);
	for (k = 1; k < N / 2; k++) {
		const float e = expf(spec[2 * k]);
		const float re = e * cosf(spec[2 * k + 1]);
		const float im = e * sinf(spec[2 * k + 1]);
		spec[2 * k]     = re;
		spec[2 * k + 1] = im;
	}
	pffft_transform_ordered(s_hrtf.mpSetup, spec, buf, work, PFFFT_BACKWARD);
	for (i = 0; i < irLen; i++) {
		ir[i] = buf[i] / (float)N;
	}
}

/* Fractional-delay bake of a measured IR into outLen samples (MagLS LF / |H|). */
static void Hrtf_BakeDelay(const float *ir, int irLen, float delay, float *out, int outLen)
{
	const float d    = delay > 0.0f ? delay : 0.0f;
	const int   di   = (int)d;
	const float frac = d - (float)di;
	int         t;

	for (t = 0; t < outLen; t++) {
		const int   k  = t - di;
		const float s0 = (k >= 0 && k < irLen) ? ir[k] : 0.0f;
		const float s1 = (k - 1 >= 0 && k - 1 < irLen) ? ir[k - 1] : 0.0f;
		out[t]         = ((1.0f - frac) * s0 + frac * s1) * s_hrtf.scale;
	}
}

/*
 * Direct-path HRIR: minimum-phase magnitude + separate onset/ITD delay.
 * SOFA-space direction (x front, y left, z up).
 * Changed in Omaha: Hrtf_SofaGet already onset-aligns; delays include TOA.
 */
static void Hrtf_QueryIr(float x, float y, float z, float *outL, float *outR, float delays[2])
{
	const int total = s_hrtf.parts * OM_HRTF_BLOCK;
	float    *outs[2];
	float    *irs[2];
	int       ear;

	Hrtf_SofaGet(x, y, z, s_hrtf.irL, s_hrtf.irR, delays);

	outs[0] = outL;
	outs[1] = outR;
	irs[0]  = s_hrtf.irL;
	irs[1]  = s_hrtf.irR;
	for (ear = 0; ear < 2; ear++) {
		float *out = outs[ear];
		float *ir  = irs[ear];
		int    t;

		if (delays[ear] < 0.0f) {
			delays[ear] = 0.0f;
		}
		if (delays[ear] > (float)s_hrtf.maxDelay) {
			delays[ear] = (float)s_hrtf.maxDelay;
		}
		Hrtf_MinPhase(ir, s_hrtf.irLen);
		for (t = 0; t < s_hrtf.irLen; t++) {
			out[t] = ir[t] * s_hrtf.scale;
		}
		for (; t < total; t++) {
			out[t] = 0.0f;
		}
	}
}

/* Measured HRIR with onset baked in (for MagLS design spectra). */
static void Hrtf_QueryMeasured(float x, float y, float z, float *outL, float *outR, int outLen)
{
	float delays[2];

	/* Changed in Omaha: onset-aligned mix, then bake total TOA/ITD. */
	Hrtf_SofaGet(x, y, z, s_hrtf.irL, s_hrtf.irR, delays);
	Hrtf_BakeDelay(s_hrtf.irL, s_hrtf.irLen, delays[0], outL, outLen);
	Hrtf_BakeDelay(s_hrtf.irR, s_hrtf.irLen, delays[1], outR, outLen);
}

/* Partition a time-domain filter of parts * BLOCK samples into FFT spectra. */
static void Hrtf_Partition(const float *ir, int parts, float *spectra)
{
	int p;

	for (p = 0; p < parts; p++) {
		memcpy(s_hrtf.padded, ir + p * OM_HRTF_BLOCK, sizeof(float) * OM_HRTF_BLOCK);
		memset(s_hrtf.padded + OM_HRTF_BLOCK, 0, sizeof(float) * OM_HRTF_BLOCK);
		pffft_transform(s_hrtf.setup, s_hrtf.padded, spectra + p * OM_HRTF_FFT, s_hrtf.work, PFFFT_FORWARD);
	}
}

static int Hrtf_BakeEarStride(void)
{
	return s_hrtf.parts * OM_HRTF_FFT;
}

static int Hrtf_BakeDirStride(void)
{
	return 2 * Hrtf_BakeEarStride();
}

static float *Hrtf_BakeSlot(int dir, int ear)
{
	return s_hrtf.bakeFilt + ((size_t)dir * 2 + ear) * (size_t)Hrtf_BakeEarStride();
}

static void Hrtf_BakeFree(void)
{
	if (s_hrtf.bakeAzCount) {
		free(s_hrtf.bakeAzCount);
		s_hrtf.bakeAzCount = NULL;
	}
	if (s_hrtf.bakeIrOffset) {
		free(s_hrtf.bakeIrOffset);
		s_hrtf.bakeIrOffset = NULL;
	}
	Hrtf_Free(&s_hrtf.bakeFilt);
	Hrtf_Free(&s_hrtf.bakeDelay);
	s_hrtf.bakeEvCount       = 0;
	s_hrtf.bakeDirCount      = 0;
	s_hrtf.bakeReady         = qfalse;
	s_hrtf.bakeGateMaxMagDb  = 0.0f;
	s_hrtf.bakeGateMedMagDb  = 0.0f;
	s_hrtf.bakeGateMedDelay  = 0.0f;
}

/* SOFA XYZ from elevation / azimuth (radians): x front, y left, z up. */
static void Hrtf_BakeElevAzToXYZ(float elev, float az, float *x, float *y, float *z)
{
	const float ce = cosf(elev), se = sinf(elev);
	*x = ce * cosf(az);
	*y = ce * sinf(az);
	*z = se;
}

static void Hrtf_BakeDirToElevAz(const float dir[3], float *elev, float *az)
{
	const float x = dir[0], y = dir[1], z = dir[2];
	const float r = sqrtf(x * x + y * y);

	*elev = atan2f(z, r > 1.0e-12f ? r : 1.0e-12f);
	*az   = atan2f(y, x);
}

static void Hrtf_BakeAzIndices(int azCount, float az, int *a0, int *a1, float *af)
{
	float       u, iu;
	const float twoPi = (float)(2.0 * M_PI);

	if (azCount <= 1) {
		*a0 = *a1 = 0;
		*af       = 0.0f;
		return;
	}
	u = az / twoPi;
	u -= floorf(u);
	u *= (float)azCount;
	iu  = floorf(u);
	*af = u - iu;
	*a0 = (int)iu % azCount;
	if (*a0 < 0) {
		*a0 += azCount;
	}
	*a1 = (*a0 + 1) % azCount;
}

/*
 * Added in Omaha: precompute DIRECT filters on a dense elev/az lattice using the
 * same onset-aligned + min-phase path as live query (full IR, float delays).
 */
static qboolean Hrtf_BakeDirectGrid(void)
{
	const float step   = OM_BAKE_STEP_DEG * (float)M_PI / 180.0f;
	const int total = s_hrtf.parts * OM_HRTF_BLOCK;
	int       ev, az, dirCount, e;
	size_t    filtCount;

	Hrtf_BakeFree();

	s_hrtf.bakeEvCount = (int)lroundf((float)M_PI / step) + 1;
	if (s_hrtf.bakeEvCount < 3) {
		s_hrtf.bakeEvCount = 3;
	}
	s_hrtf.bakeAzCount  = (int *)calloc((size_t)s_hrtf.bakeEvCount, sizeof(int));
	s_hrtf.bakeIrOffset = (int *)calloc((size_t)s_hrtf.bakeEvCount, sizeof(int));
	if (!s_hrtf.bakeAzCount || !s_hrtf.bakeIrOffset) {
		Hrtf_BakeFree();
		return qfalse;
	}

	dirCount = 0;
	for (ev = 0; ev < s_hrtf.bakeEvCount; ev++) {
		const float elev = -0.5f * (float)M_PI + (float)ev * step;
		const float ce   = fabsf(cosf(elev));
		int         naz  = (int)lroundf(2.0f * (float)M_PI * fmaxf(ce, 0.05f) / step);

		if (naz < 1) {
			naz = 1;
		}
		s_hrtf.bakeAzCount[ev]  = naz;
		s_hrtf.bakeIrOffset[ev] = dirCount;
		dirCount += naz;
	}
	s_hrtf.bakeDirCount = dirCount;

	filtCount         = (size_t)dirCount * (size_t)Hrtf_BakeDirStride();
	s_hrtf.bakeFilt   = Hrtf_Alloc((int)filtCount);
	s_hrtf.bakeDelay  = Hrtf_Alloc(dirCount * 2);
	if (!s_hrtf.bakeFilt || !s_hrtf.bakeDelay) {
		Hrtf_BakeFree();
		return qfalse;
	}

	for (ev = 0; ev < s_hrtf.bakeEvCount; ev++) {
		const float elev = -0.5f * (float)M_PI + (float)ev * step;
		const int   naz  = s_hrtf.bakeAzCount[ev];
		const int   base = s_hrtf.bakeIrOffset[ev];

		for (az = 0; az < naz; az++) {
			const float azim = (float)(2.0 * M_PI) * (float)az / (float)naz;
			float       x, y, z;
			float       delays[2];
			const int   dir = base + az;

			Hrtf_BakeElevAzToXYZ(elev, azim, &x, &y, &z);
			Hrtf_QueryIr(x, y, z, s_hrtf.shifted, s_hrtf.shifted + total, delays);
			for (e = 0; e < 2; e++) {
				Hrtf_Partition(
					s_hrtf.shifted + e * total, s_hrtf.parts, Hrtf_BakeSlot(dir, e)
				);
				s_hrtf.bakeDelay[dir * 2 + e] = delays[e];
			}
		}
	}

	s_hrtf.bakeReady = qtrue;
	Com_Printf(
		"Sound/omaha: HRTF DIRECT bake %d dirs (%d elev x ~%.1f deg, %.1f MiB)\n",
		dirCount,
		s_hrtf.bakeEvCount,
		OM_BAKE_STEP_DEG,
		(float)(filtCount * sizeof(float)) / (1024.0f * 1024.0f)
	);
	return qtrue;
}

/* Bilinear elev/az lerp of baked spectra + delays into a voice filter slot. */
static void Hrtf_LookupBaked(const float dir[3], float *slot, float delays[2])
{
	float       elev, az, ef, elevBlend, af0, af1;
	int         ev0, ev1, a00, a01, a10, a11, e, i;
	const int   earStr = Hrtf_BakeEarStride();
	const int   lastEv = s_hrtf.bakeEvCount - 1;
	float       w[4];
	const float *s0, *s1, *s2, *s3;

	Hrtf_BakeDirToElevAz(dir, &elev, &az);
	ef = (elev + 0.5f * (float)M_PI) / (float)M_PI * (float)lastEv;
	if (ef < 0.0f) {
		ef = 0.0f;
	} else if (ef > (float)lastEv) {
		ef = (float)lastEv;
	}
	ev0       = (int)ef;
	elevBlend = ef - (float)ev0;
	if (ev0 >= lastEv) {
		ev0       = lastEv - 1;
		elevBlend = 1.0f;
	}
	ev1 = ev0 + 1;

	Hrtf_BakeAzIndices(s_hrtf.bakeAzCount[ev0], az, &a00, &a01, &af0);
	Hrtf_BakeAzIndices(s_hrtf.bakeAzCount[ev1], az, &a10, &a11, &af1);

	w[0] = (1.0f - elevBlend) * (1.0f - af0);
	w[1] = (1.0f - elevBlend) * af0;
	w[2] = elevBlend * (1.0f - af1);
	w[3] = elevBlend * af1;

	{
		const int d0 = s_hrtf.bakeIrOffset[ev0] + a00;
		const int d1 = s_hrtf.bakeIrOffset[ev0] + a01;
		const int d2 = s_hrtf.bakeIrOffset[ev1] + a10;
		const int d3 = s_hrtf.bakeIrOffset[ev1] + a11;

		for (e = 0; e < 2; e++) {
			s0 = Hrtf_BakeSlot(d0, e);
			s1 = Hrtf_BakeSlot(d1, e);
			s2 = Hrtf_BakeSlot(d2, e);
			s3 = Hrtf_BakeSlot(d3, e);
			i = 0;
#if defined(__SSE2__)
			{
				const __m128 w0 = _mm_set1_ps(w[0]);
				const __m128 w1 = _mm_set1_ps(w[1]);
				const __m128 w2 = _mm_set1_ps(w[2]);
				const __m128 w3 = _mm_set1_ps(w[3]);
				for (; i + 4 <= earStr; i += 4) {
					__m128 a = _mm_mul_ps(_mm_loadu_ps(s0 + i), w0);
					a        = _mm_add_ps(a, _mm_mul_ps(_mm_loadu_ps(s1 + i), w1));
					a        = _mm_add_ps(a, _mm_mul_ps(_mm_loadu_ps(s2 + i), w2));
					a        = _mm_add_ps(a, _mm_mul_ps(_mm_loadu_ps(s3 + i), w3));
					_mm_storeu_ps(slot + e * earStr + i, a);
				}
			}
#endif
			for (; i < earStr; i++) {
				slot[e * earStr + i] =
					s0[i] * w[0] + s1[i] * w[1] + s2[i] * w[2] + s3[i] * w[3];
			}
			delays[e] = s_hrtf.bakeDelay[d0 * 2 + e] * w[0] + s_hrtf.bakeDelay[d1 * 2 + e] * w[1]
						+ s_hrtf.bakeDelay[d2 * 2 + e] * w[2] + s_hrtf.bakeDelay[d3 * 2 + e] * w[3];
		}
	}
}

/*
 * Relative residual energy of packed partition spectra (bake vs live), in dB.
 * Identical → −∞ (clamped by epsilon); −20 dB ≈ 1% residual power.
 */
static float Hrtf_FiltMagErrDb(const float *live, const float *baked, int n)
{
	double num = 0.0, den = 0.0;
	int    i;

	for (i = 0; i < n; i++) {
		const double d = (double)live[i] - (double)baked[i];
		num += d * d;
		den += (double)live[i] * (double)live[i];
	}
	if (den < 1.0e-30) {
		return -90.0f;
	}
	return (float)(10.0 * log10(num / den + 1.0e-30));
}

static int Hrtf_FloatCmp(const void *a, const void *b)
{
	const float fa = *(const float *)a, fb = *(const float *)b;
	return (fa > fb) - (fa < fb);
}

/*
 * Added in Omaha: bake must match live Hrtf_QueryIr within thresholds, else keep SOFA.
 */
static qboolean Hrtf_BakeQualityGate(void)
{
	const int   total  = s_hrtf.parts * OM_HRTF_BLOCK;
	const int   earStr = Hrtf_BakeEarStride();
	const int   slotN  = Hrtf_BakeDirStride();
	float       magErr[OM_BAKE_GATE_DIRS];
	float       delErr[OM_BAKE_GATE_DIRS];
	float      *liveSlot;
	float      *bakeSlot;
	int         s, e;
	unsigned    seed = 0xC0FFEEu;

	if (!s_hrtf.bakeReady || !s_hrtf.easy) {
		return qfalse;
	}

	liveSlot = Hrtf_Alloc(slotN);
	bakeSlot = Hrtf_Alloc(slotN);
	if (!liveSlot || !bakeSlot) {
		Hrtf_Free(&liveSlot);
		Hrtf_Free(&bakeSlot);
		return qfalse;
	}

	for (s = 0; s < OM_BAKE_GATE_DIRS; s++) {
		float dir[3], delaysLive[2], delaysBake[2];
		float u1, u2, z, r;
		float maxDel = 0.0f, maxMag;

		/* Deterministic unit directions (Marsaglia-ish). */
		seed = seed * 1664525u + 1013904223u;
		u1   = (float)(seed & 0xffff) / 65535.0f;
		seed = seed * 1664525u + 1013904223u;
		u2   = (float)(seed & 0xffff) / 65535.0f;
		z    = 2.0f * u1 - 1.0f;
		r    = sqrtf(fmaxf(0.0f, 1.0f - z * z));
		dir[0] = r * cosf(2.0f * (float)M_PI * u2);
		dir[1] = r * sinf(2.0f * (float)M_PI * u2);
		dir[2] = z;

		Hrtf_QueryIr(dir[0], dir[1], dir[2], s_hrtf.shifted, s_hrtf.shifted + total, delaysLive);
		Hrtf_Partition(s_hrtf.shifted, s_hrtf.parts, liveSlot);
		Hrtf_Partition(s_hrtf.shifted + total, s_hrtf.parts, liveSlot + earStr);
		Hrtf_LookupBaked(dir, bakeSlot, delaysBake);

		maxMag = Hrtf_FiltMagErrDb(liveSlot, bakeSlot, slotN);
		for (e = 0; e < 2; e++) {
			const float d = fabsf(delaysLive[e] - delaysBake[e]);
			if (d > maxDel) {
				maxDel = d;
			}
		}
		magErr[s] = maxMag;
		delErr[s] = maxDel;
	}

	Hrtf_Free(&liveSlot);
	Hrtf_Free(&bakeSlot);

	qsort(magErr, OM_BAKE_GATE_DIRS, sizeof(float), Hrtf_FloatCmp);
	qsort(delErr, OM_BAKE_GATE_DIRS, sizeof(float), Hrtf_FloatCmp);
	s_hrtf.bakeGateMedMagDb = magErr[OM_BAKE_GATE_DIRS / 2];
	s_hrtf.bakeGateMaxMagDb = magErr[OM_BAKE_GATE_DIRS - 1];
	s_hrtf.bakeGateMedDelay = delErr[OM_BAKE_GATE_DIRS / 2];

	/* Residual dB must stay below (more negative than) the thresholds. */
	if (s_hrtf.bakeGateMedMagDb > OM_BAKE_GATE_MED_DB || s_hrtf.bakeGateMaxMagDb > OM_BAKE_GATE_MAX_DB
		|| s_hrtf.bakeGateMedDelay > OM_BAKE_GATE_MED_DELAY) {
		Com_Printf(
			"Sound/omaha: HRTF bake gate FAIL (med %.2f dB / max %.2f dB / delay med %.3f) — keeping live SOFA\n",
			s_hrtf.bakeGateMedMagDb,
			s_hrtf.bakeGateMaxMagDb,
			s_hrtf.bakeGateMedDelay
		);
		return qfalse;
	}
	Com_Printf(
		"Sound/omaha: HRTF bake gate OK (med %.2f dB / max %.2f dB / delay med %.3f)\n",
		s_hrtf.bakeGateMedMagDb,
		s_hrtf.bakeGateMaxMagDb,
		s_hrtf.bakeGateMedDelay
	);
	return qtrue;
}

/* Voice filter slot from a listener-space direction; delays are ITD/onset in samples. */
static void Hrtf_BuildVoiceFilter(const float dir[3], float *slot, float delays[2])
{
	const int total = s_hrtf.parts * OM_HRTF_BLOCK;

	/* Changed in Omaha: prefer dense bake; live SOFA only when sofaLive. */
	if (s_hrtf.bakeReady && !s_hrtf.sofaLive) {
		Hrtf_LookupBaked(dir, slot, delays);
		return;
	}
	Hrtf_QueryIr(dir[0], dir[1], dir[2], s_hrtf.shifted, s_hrtf.shifted + total, delays);
	Hrtf_Partition(s_hrtf.shifted, s_hrtf.parts, slot);
	Hrtf_Partition(s_hrtf.shifted + total, s_hrtf.parts, slot + s_hrtf.parts * OM_HRTF_FFT);
}

/* Push a wet block through the per-ear fractional delay line and add to out. */
static void Hrtf_DelayBlock(omHrtfVoice_t *hv, int ear, const float *wet, float *out)
{
	float      *buf = hv->delayBuf[ear];
	const int   len = s_hrtf.delayLen;
	int         pos = hv->delayPos[ear];
	const float d0  = hv->delay[ear];
	const float d1  = hv->delayTgt[ear];
	const float maxd = (float)s_hrtf.maxDelay;
	int         i;

	for (i = 0; i < OM_HRTF_BLOCK; i++) {
		float d, frac;
		int   di, i0, i1;

		buf[pos] = wet[i];
		d        = d0 + (d1 - d0) * ((float)(i + 1) / (float)OM_HRTF_BLOCK);
		if (d < 0.0f) {
			d = 0.0f;
		} else if (d > maxd) {
			d = maxd;
		}
		di   = (int)d;
		frac = d - (float)di;
		i0   = pos - di;
		i1   = i0 - 1;
		if (i0 < 0) {
			i0 += len;
		}
		if (i1 < 0) {
			i1 += len;
		}
		out[i] += buf[i0] + (buf[i1] - buf[i0]) * frac;
		pos = pos + 1;
		if (pos >= len) {
			pos = 0;
		}
	}
	hv->delayPos[ear] = pos;
	hv->delay[ear]    = d1;
}

static qboolean Hrtf_BlockIsZero(const float *in)
{
	int i;
	for (i = 0; i < OM_HRTF_BLOCK; i++) {
		if (in[i] != 0.0f) {
			return qfalse;
		}
	}
	return qtrue;
}

/*
 * Ordered real spectra at OM_DESIGN_FFT: [DC, Nyquist, re1, im1, ...]. Bin k of
 * the complex array is (spec[2k], spec[2k+1]) for k >= 1; DC and Nyquist are real.
 */
static float Hrtf_BinEnergy(const float *spec, int k)
{
	if (k == 0) {
		return spec[0] * spec[0];
	}
	if (k == OM_DESIGN_FFT / 2) {
		return spec[1] * spec[1];
	}
	return spec[2 * k] * spec[2 * k] + spec[2 * k + 1] * spec[2 * k + 1];
}

/* Ordered-FFT bin phase (Nyquist is real → 0). */
static float Hrtf_BinPhase(const float *fc, int b, int M)
{
	if (b <= 0 || b >= M / 2) {
		return 0.0f;
	}
	return atan2f(fc[2 * b + 1], fc[2 * b]);
}

static float Hrtf_WrapPi(float a)
{
	while (a > (float)M_PI) {
		a -= (float)(2.0 * M_PI);
	}
	while (a < (float)-M_PI) {
		a += (float)(2.0 * M_PI);
	}
	return a;
}

/*
 * MagLS binaural decoder. Speaker k gets g_k = w_k * sum_n Y_n(k) a_n, so each
 * ear is sum_n a_n * F_n with F_n = sum_k w_k Y_n(k) H_k. Below ~2 kHz, H keeps
 * measured phase (ITD). Above, iterative MagLS matches |H| only so order-3 does
 * not comb. Changed in Omaha: Hold et al. average phase continuation (W-channel
 * LF phase slope) so MagLS does not disperse the HRIR. Diffuse-field EQ matches
 * direct (min-phase) HRIR energy; a fixed design delay makes filters causal.
 */
static qboolean Hrtf_BuildAmbiDecoder(void)
{
	const int          M     = OM_DESIGN_FFT;
	const int          bins  = M / 2 + 1;
	const int          total = s_hrtf.parts * OM_HRTF_BLOCK;
	float              pts[OM_LEBEDEV_POINTS][3];
	float              w[OM_LEBEDEV_POINTS];
	const int          count = Hrtf_Lebedev50(pts, w);
	float             *buf   = s_hrtf.mpBuf;
	float             *spec  = s_hrtf.mpSpec;
	float             *work  = s_hrtf.mpWork;
	std::vector<float> y((size_t)count * OM_AMBI_CHANNELS);
	std::vector<float> href((size_t)count * 2 * M);
	std::vector<float> chanSpec((size_t)OM_AMBI_CHANNELS * 2 * M, 0.0f);
	std::vector<float> chanComplex((size_t)OM_AMBI_CHANNELS * 2 * M, 0.0f);
	std::vector<float> hmag((size_t)count * 2 * bins);
	std::vector<double> numE(bins, 0.0), denE(bins, 0.0);
	std::vector<double> numC(bins + 1, 0.0), denC(bins + 1, 0.0);
	std::vector<float>  eq(bins);
	int                 k, n, ear, b, s, iter;

	if (!s_hrtf.mpSetup || !buf || !spec || !work) {
		return qfalse;
	}

	std::vector<float> measured((size_t)M * 2);
	std::vector<float> fnew((size_t)OM_AMBI_CHANNELS * 2 * 2);

	/* Measured HRTFs (ITD baked) and SH coefficients per Lebedev direction. */
	for (k = 0; k < count; k++) {
		float *yl = &y[(size_t)k * OM_AMBI_CHANNELS];

		Hrtf_AmbiCoeffsXYZ(pts[k][0], pts[k][1], pts[k][2], yl);
		Hrtf_QueryMeasured(pts[k][0], pts[k][1], pts[k][2], measured.data(), measured.data() + M, M);
		for (ear = 0; ear < 2; ear++) {
			float *dst = &href[((size_t)k * 2 + ear) * M];
			memcpy(buf, measured.data() + ear * M, sizeof(float) * M);
			pffft_transform_ordered(s_hrtf.mpSetup, buf, dst, work, PFFFT_FORWARD);
			hmag[((size_t)k * 2 + ear) * bins] = fabsf(dst[0]);
			hmag[((size_t)k * 2 + ear) * bins + bins - 1] = fabsf(dst[1]);
			for (b = 1; b < bins - 1; b++) {
				const float re = dst[2 * b], im = dst[2 * b + 1];
				hmag[((size_t)k * 2 + ear) * bins + b] = sqrtf(re * re + im * im);
			}
			for (n = 0; n < OM_AMBI_CHANNELS; n++) {
				const float g   = w[k] * yl[n];
				float      *out = &chanSpec[((size_t)n * 2 + ear) * M];
				int         i;
				for (i = 0; i < M; i++) {
					out[i] += g * dst[i];
				}
			}
		}
	}
	chanComplex = chanSpec;

	/*
	 * MagLS above the duplex transition with Hold average phase continuation.
	 * Walk frequency in order: seed each bin from the previous bin's reconstructed
	 * spatial phase plus the W-channel LF phase step (group delay), then iterate
	 * magnitude match. Blend with complex LS through the crossover.
	 */
	{
		int   blo = 1;
		float dphi[2] = {0.0f, 0.0f};
		int   prevBin;

		for (b = 1; b < bins; b++) {
			const float f = (float)b * (float)s_hrtf.sampleRate / (float)M;
			if (f <= OM_MAGLS_LO_HZ) {
				blo = b;
			} else {
				break;
			}
		}
		for (ear = 0; ear < 2; ear++) {
			const float *fc = &chanComplex[((size_t)0 * 2 + ear) * M];
			float        prev = Hrtf_BinPhase(fc, 1, M);
			double       sum  = 0.0;
			int          steps = 0;

			for (b = 2; b <= blo; b++) {
				const float ang  = Hrtf_BinPhase(fc, b, M);
				const float diff = Hrtf_WrapPi(ang - prev);
				sum += (double)diff;
				prev = ang;
				steps++;
			}
			dphi[ear] = steps > 0 ? (float)(sum / (double)steps) : 0.0f;
		}

		prevBin = blo;
		for (b = blo + 1; b < bins; b++) {
			const float f = (float)b * (float)s_hrtf.sampleRate / (float)M;
			float       mu;

			if (f >= OM_MAGLS_HI_HZ) {
				mu = 1.0f;
			} else {
				mu = 0.5f - 0.5f * cosf((float)M_PI * (f - OM_MAGLS_LO_HZ) / (OM_MAGLS_HI_HZ - OM_MAGLS_LO_HZ));
			}

			for (iter = 0; iter < OM_MAGLS_ITERS; iter++) {
				memset(fnew.data(), 0, fnew.size() * sizeof(float));

				for (k = 0; k < count; k++) {
					const float *yl = &y[(size_t)k * OM_AMBI_CHANNELS];
					for (ear = 0; ear < 2; ear++) {
						double re = 0.0, im = 0.0;
						float  mag, ang, cr, ci;
						int    ni;

						/* Phase from previous frequency bin (+ continuation on first iter). */
						for (n = 0; n < OM_AMBI_CHANNELS; n++) {
							const float *fc = &chanSpec[((size_t)n * 2 + ear) * M];
							if (iter == 0) {
								if (prevBin == M / 2) {
									re += (double)yl[n] * fc[1];
								} else {
									re += (double)yl[n] * fc[2 * prevBin];
									im += (double)yl[n] * fc[2 * prevBin + 1];
								}
							} else if (b == M / 2) {
								re += (double)yl[n] * fc[1];
							} else {
								re += (double)yl[n] * fc[2 * b];
								im += (double)yl[n] * fc[2 * b + 1];
							}
						}
						mag = hmag[((size_t)k * 2 + ear) * bins + b];
						if (b == M / 2) {
							ang = 0.0f;
						} else {
							ang = atan2f((float)im, (float)re);
							if (iter == 0) {
								ang = Hrtf_WrapPi(ang + dphi[ear]);
							}
						}
						cr = mag * cosf(ang);
						ci = (b == M / 2) ? 0.0f : mag * sinf(ang);
						for (ni = 0; ni < OM_AMBI_CHANNELS; ni++) {
							const float g = w[k] * yl[ni];
							fnew[(size_t)ni * 4 + ear * 2] += g * cr;
							fnew[(size_t)ni * 4 + ear * 2 + 1] += g * ci;
						}
					}
				}
				for (n = 0; n < OM_AMBI_CHANNELS; n++) {
					for (ear = 0; ear < 2; ear++) {
						float *fc = &chanSpec[((size_t)n * 2 + ear) * M];
						if (b == M / 2) {
							fc[1] = fnew[(size_t)n * 4 + ear * 2];
						} else {
							fc[2 * b]     = fnew[(size_t)n * 4 + ear * 2];
							fc[2 * b + 1] = fnew[(size_t)n * 4 + ear * 2 + 1];
						}
					}
				}
			}

			for (n = 0; n < OM_AMBI_CHANNELS; n++) {
				for (ear = 0; ear < 2; ear++) {
					float       *fc = &chanSpec[((size_t)n * 2 + ear) * M];
					const float *cc = &chanComplex[((size_t)n * 2 + ear) * M];
					if (b == M / 2) {
						fc[1] = cc[1] + (fc[1] - cc[1]) * mu;
					} else {
						fc[2 * b]     = cc[2 * b] + (fc[2 * b] - cc[2 * b]) * mu;
						fc[2 * b + 1] = cc[2 * b + 1] + (fc[2 * b + 1] - cc[2 * b + 1]) * mu;
					}
				}
			}
			prevBin = b;
		}
	}

	/* Diffuse-field EQ vs direct min-phase HRIRs (same energy the voices use). */
	for (s = 0; s < OM_DESIGN_TEST_POINTS; s++) {
		const float zc  = 1.0f - 2.0f * ((float)s + 0.5f) / (float)OM_DESIGN_TEST_POINTS;
		const float rc  = sqrtf(fmaxf(0.0f, 1.0f - zc * zc));
		const float phi = (float)s * 2.39996323f;
		const float px = rc * cosf(phi), py = rc * sinf(phi), pz = zc;
		float       ys[OM_AMBI_CHANNELS];
		float       delays[2];

		Hrtf_QueryIr(px, py, pz, s_hrtf.shifted, s_hrtf.shifted + total, delays);
		Hrtf_AmbiCoeffsXYZ(px, py, pz, ys);
		for (ear = 0; ear < 2; ear++) {
			memset(buf, 0, sizeof(float) * M);
			memcpy(buf, s_hrtf.shifted + ear * total, sizeof(float) * total);
			pffft_transform_ordered(s_hrtf.mpSetup, buf, spec, work, PFFFT_FORWARD);
			for (b = 0; b < bins; b++) {
				numE[b] += Hrtf_BinEnergy(spec, b);
			}
			for (b = 0; b < bins; b++) {
				double re = 0.0, im = 0.0;
				for (n = 0; n < OM_AMBI_CHANNELS; n++) {
					const float *f = &chanSpec[((size_t)n * 2 + ear) * M];
					const double g = (double)ys[n];
					if (b == 0) {
						re += g * f[0];
					} else if (b == M / 2) {
						re += g * f[1];
					} else {
						re += g * f[2 * b];
						im += g * f[2 * b + 1];
					}
				}
				denE[b] += re * re + im * im;
			}
		}
	}

	for (b = 0; b < bins; b++) {
		numC[b + 1] = numC[b] + numE[b];
		denC[b + 1] = denC[b] + denE[b];
	}
	for (b = 0; b < bins; b++) {
		int    half = (int)((float)b * 0.12f);
		int    lo, hi;
		double num, den;
		if (half < 4) {
			half = 4;
		}
		lo    = b - half < 0 ? 0 : b - half;
		hi    = b + half > bins - 1 ? bins - 1 : b + half;
		num   = numC[hi + 1] - numC[lo];
		den   = denC[hi + 1] - denC[lo];
		eq[b] = den > 1.0e-20 ? (float)sqrt(num / den) : 1.0f;
		if (eq[b] < OM_DFEQ_MIN) {
			eq[b] = OM_DFEQ_MIN;
		} else if (eq[b] > OM_DFEQ_MAX) {
			eq[b] = OM_DFEQ_MAX;
		}
	}

	/* Design delay + truncate into partitioned filters. */
	s_hrtf.ambiParts =
		(s_hrtf.irLen + s_hrtf.maxDelay + 2 * OM_DESIGN_DELAY + OM_HRTF_BLOCK - 1) / OM_HRTF_BLOCK;
	if (s_hrtf.ambiParts > OM_HRTF_MAX_PARTS) {
		s_hrtf.ambiParts = OM_HRTF_MAX_PARTS;
	}
	s_hrtf.ambiFilt = Hrtf_Alloc(OM_AMBI_CHANNELS * 2 * s_hrtf.ambiParts * OM_HRTF_FFT);
	s_hrtf.ambiFdl  = Hrtf_Alloc(OM_AMBI_CHANNELS * s_hrtf.ambiParts * OM_HRTF_FFT);
	s_hrtf.ambiIn   = Hrtf_Alloc(OM_AMBI_CHANNELS * OM_HRTF_FFT);
	if (s_hrtf.ambiFilt && s_hrtf.ambiFdl && s_hrtf.ambiIn) {
		const int          len  = s_hrtf.ambiParts * OM_HRTF_BLOCK;
		const int          fade = OM_HRTF_BLOCK / 4;
		std::vector<float> ir((size_t)len);
		double             sumSq = 0.0;
		double             tailSq = 0.0;

		for (n = 0; n < OM_AMBI_CHANNELS; n++) {
			for (ear = 0; ear < 2; ear++) {
				const float *src = &chanSpec[((size_t)n * 2 + ear) * M];
				int          i;

				spec[0] = src[0] * eq[0];
				spec[1] = src[1] * eq[M / 2] * ((OM_DESIGN_DELAY & 1) ? -1.0f : 1.0f);
				for (b = 1; b < M / 2; b++) {
					const float  g  = eq[b];
					const double ph = -2.0 * M_PI * (double)b * OM_DESIGN_DELAY / (double)M;
					const float  c  = (float)cos(ph), sn = (float)sin(ph);
					const float  re = src[2 * b] * g, im = src[2 * b + 1] * g;
					spec[2 * b]     = re * c - im * sn;
					spec[2 * b + 1] = re * sn + im * c;
				}
				pffft_transform_ordered(s_hrtf.mpSetup, spec, buf, work, PFFFT_BACKWARD);
				for (i = 0; i < len; i++) {
					float v = buf[i] / (float)M;
					if (i >= len - fade) {
						v *= 0.5f + 0.5f * cosf((float)M_PI * (float)(i - (len - fade) + 1) / (float)fade);
					}
					ir[i] = v;
					sumSq += (double)v * (double)v;
					if (i >= len - OM_HRTF_BLOCK) {
						tailSq += (double)v * (double)v;
					}
				}
				Hrtf_Partition(
					ir.data(),
					s_hrtf.ambiParts,
					s_hrtf.ambiFilt + ((size_t)n * 2 + ear) * s_hrtf.ambiParts * OM_HRTF_FFT
				);
			}
		}
		/* Added in Omaha: drop a trailing decoder partition below -100 dB (Tier C, usually a no-op). */
		if (s_hrtf.ambiParts > 1 && sumSq > 0.0 && tailSq < sumSq * 1.0e-10) {
			s_hrtf.ambiParts--;
		}
	}

	return (s_hrtf.ambiFilt && s_hrtf.ambiFdl && s_hrtf.ambiIn) ? qtrue : qfalse;
}

void OmHrtf_AmbiReset(void)
{
	if (!s_hrtf.ready) {
		return;
	}
	memset(s_hrtf.ambiFdl, 0, sizeof(float) * OM_AMBI_CHANNELS * s_hrtf.ambiParts * OM_HRTF_FFT);
	memset(s_hrtf.ambiIn, 0, sizeof(float) * OM_AMBI_CHANNELS * OM_HRTF_FFT);
	s_hrtf.ambiFdlPos = 0;
	/* A window spans two blocks, so parts + 1 zero blocks clear every spectrum. */
	for (int n = 0; n < OM_AMBI_CHANNELS; n++) {
		s_hrtf.ambiZeroRun[n] = s_hrtf.ambiParts + 1;
	}
}

void OmHrtf_AmbiDecode(float *const ambi[OM_AMBI_CHANNELS], float *outL, float *outR, int count)
{
	const float scaling = 1.0f / (float)OM_HRTF_FFT;
	const int   parts   = s_hrtf.ambiParts;
	float      *outs[2];
	int         n, b, i, ear, p;

	if (!s_hrtf.ready) {
		return;
	}
	outs[0] = outL;
	outs[1] = outR;

	for (b = 0; b + OM_HRTF_BLOCK <= count; b += OM_HRTF_BLOCK) {
		qboolean live[OM_AMBI_CHANNELS];
		qboolean any = qfalse;

		for (n = 0; n < OM_AMBI_CHANNELS; n++) {
			float *inBuf = s_hrtf.ambiIn + (size_t)n * OM_HRTF_FFT;
			float *slot  = s_hrtf.ambiFdl + ((size_t)n * parts + s_hrtf.ambiFdlPos) * OM_HRTF_FFT;

			if (Hrtf_BlockIsZero(ambi[n] + b)) {
				if (s_hrtf.ambiZeroRun[n] > parts) {
					live[n] = qfalse;
					continue;
				}
				s_hrtf.ambiZeroRun[n]++;
			} else {
				s_hrtf.ambiZeroRun[n] = 0;
			}
			memcpy(inBuf + OM_HRTF_BLOCK, ambi[n] + b, sizeof(float) * OM_HRTF_BLOCK);
			pffft_transform(s_hrtf.setup, inBuf, slot, Hrtf_Work(), PFFFT_FORWARD);
			memcpy(inBuf, inBuf + OM_HRTF_BLOCK, sizeof(float) * OM_HRTF_BLOCK);
			live[n] = qtrue;
			any     = qtrue;
		}

		if (any) {
			for (ear = 0; ear < 2; ear++) {
				memset(Hrtf_Acc(), 0, sizeof(float) * OM_HRTF_FFT);
				for (n = 0; n < OM_AMBI_CHANNELS; n++) {
					const float *fdl  = s_hrtf.ambiFdl + (size_t)n * parts * OM_HRTF_FFT;
					const float *filt = s_hrtf.ambiFilt + ((size_t)n * 2 + ear) * parts * OM_HRTF_FFT;
					if (!live[n]) {
						continue;
					}
					for (p = 0; p < parts; p++) {
						const int slot = (s_hrtf.ambiFdlPos - p + parts) % parts;
						pffft_zconvolve_accumulate(
							s_hrtf.setup, fdl + slot * OM_HRTF_FFT, filt + p * OM_HRTF_FFT, Hrtf_Acc(), scaling
						);
					}
				}
				pffft_transform(s_hrtf.setup, Hrtf_Acc(), Hrtf_Time(), Hrtf_Work(), PFFFT_BACKWARD);
				for (i = 0; i < OM_HRTF_BLOCK; i++) {
					outs[ear][b + i] += Hrtf_Time()[OM_HRTF_BLOCK + i];
				}
			}
		}
		/* Dead channels hold zero spectra in every slot, so the shared ring position can advance. */
		s_hrtf.ambiFdlPos = (s_hrtf.ambiFdlPos + 1) % parts;
	}
}

/* One output block: sum over partitions of fdl * filter, inverse FFT, keep the valid half.
 * Both ears share the delay line, so one walk accumulates two filters. Added in Omaha. */
static void Hrtf_ConvolveBlockStereo(
	const float *fdl, int fdlPos, const unsigned char *slotZero, const float *filtL, const float *filtR, float *outL,
	float *outR
)
{
	const float scaling = 1.0f / (float)OM_HRTF_FFT;
	float      *accL    = Hrtf_Acc();
	float      *accR    = Hrtf_AccB();
	int         p;

	memset(accL, 0, sizeof(float) * OM_HRTF_FFT);
	memset(accR, 0, sizeof(float) * OM_HRTF_FFT);
	for (p = 0; p < s_hrtf.parts; p++) {
		const int slot = (fdlPos - p + s_hrtf.parts) % s_hrtf.parts;
		if (slotZero && slotZero[slot]) {
			continue;
		}
		pffft_zconvolve_accumulate(s_hrtf.setup, fdl + slot * OM_HRTF_FFT, filtL + p * OM_HRTF_FFT, accL, scaling);
		pffft_zconvolve_accumulate(s_hrtf.setup, fdl + slot * OM_HRTF_FFT, filtR + p * OM_HRTF_FFT, accR, scaling);
	}
	pffft_transform(s_hrtf.setup, accL, Hrtf_Time(), Hrtf_Work(), PFFFT_BACKWARD);
	memcpy(outL, Hrtf_Time() + OM_HRTF_BLOCK, sizeof(float) * OM_HRTF_BLOCK);
	pffft_transform(s_hrtf.setup, accR, Hrtf_Time(), Hrtf_Work(), PFFFT_BACKWARD);
	memcpy(outR, Hrtf_Time() + OM_HRTF_BLOCK, sizeof(float) * OM_HRTF_BLOCK);
}

omHrtfVoice_t *OmHrtf_VoiceAlloc(void)
{
	omHrtfVoice_t *hv;
	const int      specs = s_hrtf.parts * OM_HRTF_FFT;

	if (!s_hrtf.ready) {
		return NULL;
	}
	if (!s_hrtf.freeVoices.empty()) {
		hv = s_hrtf.freeVoices.back();
		s_hrtf.freeVoices.pop_back();
		OmHrtf_VoiceReset(hv);
		return hv;
	}

	hv               = new omHrtfVoice_t;
	hv->fdl          = Hrtf_Alloc(specs);
	hv->filt[0]      = Hrtf_Alloc(specs * 2);
	hv->filt[1]      = Hrtf_Alloc(specs * 2);
	hv->inBuf        = Hrtf_Alloc(OM_HRTF_FFT);
	hv->delayBuf[0]  = Hrtf_Alloc(s_hrtf.delayLen);
	hv->delayBuf[1]  = Hrtf_Alloc(s_hrtf.delayLen);
	if (!hv->fdl || !hv->filt[0] || !hv->filt[1] || !hv->inBuf || !hv->delayBuf[0] || !hv->delayBuf[1]) {
		Hrtf_Free(&hv->fdl);
		Hrtf_Free(&hv->filt[0]);
		Hrtf_Free(&hv->filt[1]);
		Hrtf_Free(&hv->inBuf);
		Hrtf_Free(&hv->delayBuf[0]);
		Hrtf_Free(&hv->delayBuf[1]);
		delete hv;
		return NULL;
	}
	OmHrtf_VoiceReset(hv);
	return hv;
}

static void Hrtf_VoiceDestroy(omHrtfVoice_t *hv)
{
	Hrtf_Free(&hv->fdl);
	Hrtf_Free(&hv->filt[0]);
	Hrtf_Free(&hv->filt[1]);
	Hrtf_Free(&hv->inBuf);
	Hrtf_Free(&hv->delayBuf[0]);
	Hrtf_Free(&hv->delayBuf[1]);
	delete hv;
}

void OmHrtf_VoiceFree(omHrtfVoice_t *hv)
{
	if (!hv) {
		return;
	}
	if (!s_hrtf.ready) {
		Hrtf_VoiceDestroy(hv);
		return;
	}
	s_hrtf.freeVoices.push_back(hv);
}

void OmHrtf_VoiceReset(omHrtfVoice_t *hv)
{
	if (!hv || !s_hrtf.ready) {
		return;
	}
	memset(hv->fdl, 0, sizeof(float) * s_hrtf.parts * OM_HRTF_FFT);
	memset(hv->inBuf, 0, sizeof(float) * OM_HRTF_FFT);
	memset(hv->delayBuf[0], 0, sizeof(float) * s_hrtf.delayLen);
	memset(hv->delayBuf[1], 0, sizeof(float) * s_hrtf.delayLen);
	hv->delayPos[0] = hv->delayPos[1] = 0;
	hv->delay[0] = hv->delay[1] = 0.0f;
	hv->delayTgt[0] = hv->delayTgt[1] = 0.0f;
	hv->fdlPos     = 0;
	hv->cur        = 0;
	hv->zeroRun    = s_hrtf.parts + 1;
	memset(hv->slotZero, 1, sizeof(hv->slotZero));
	hv->hasFilter  = qfalse;
	hv->filtDir[0] = 0.0f;
	hv->filtDir[1] = 0.0f;
	hv->filtDir[2] = -1.0f;
}

int OmHrtf_TailBlocks(void)
{
	if (!s_hrtf.ready) {
		return 0;
	}
	/* Convolution ring-out plus the ITD delay line. */
	return s_hrtf.parts + 1 + (s_hrtf.maxDelay + OM_HRTF_BLOCK - 1) / OM_HRTF_BLOCK;
}

void OmHrtf_VoiceProcess(omHrtfVoice_t *hv, const float dir[3], const float *in, float *outL, float *outR, int count)
{
	const int specs = s_hrtf.parts * OM_HRTF_FFT;
	float     blockOld[OM_HRTF_BLOCK];
	float     blockNew[OM_HRTF_BLOCK];
	float     wet[OM_HRTF_BLOCK];
	qboolean  fade = qfalse;
	float    *outs[2];
	int       b, i, ear;

	if (!hv || !s_hrtf.ready) {
		return;
	}
	outs[0] = outL;
	outs[1] = outR;

	/* Rate-limited per line: re-query only after ~1 degree of movement. */
	if (!hv->hasFilter) {
		float delays[2];
		Hrtf_BuildVoiceFilter(dir, hv->filt[hv->cur], delays);
		hv->delay[0] = hv->delayTgt[0] = delays[0];
		hv->delay[1] = hv->delayTgt[1] = delays[1];
		VectorCopy(dir, hv->filtDir);
		hv->hasFilter = qtrue;
	} else if (DotProduct(dir, hv->filtDir) < OM_HRTF_REQUERY_COS) {
		float delays[2];
		Hrtf_BuildVoiceFilter(dir, hv->filt[hv->cur ^ 1], delays);
		hv->delayTgt[0] = delays[0];
		hv->delayTgt[1] = delays[1];
		VectorCopy(dir, hv->filtDir);
		fade = qtrue;
	}

	for (b = 0; b + OM_HRTF_BLOCK <= count; b += OM_HRTF_BLOCK) {
		qboolean skipConvolve = qfalse;

		if (Hrtf_BlockIsZero(in + b)) {
			if (hv->zeroRun > s_hrtf.parts) {
				/* FDL is clear; still run the delay line so ITD tails drain. */
				skipConvolve = qtrue;
			} else {
				hv->zeroRun++;
			}
		} else {
			hv->zeroRun = 0;
		}

		if (!skipConvolve) {
			float *slot = hv->fdl + hv->fdlPos * OM_HRTF_FFT;

			memcpy(hv->inBuf + OM_HRTF_BLOCK, in + b, sizeof(float) * OM_HRTF_BLOCK);
			pffft_transform(s_hrtf.setup, hv->inBuf, slot, Hrtf_Work(), PFFFT_FORWARD);
			memcpy(hv->inBuf, hv->inBuf + OM_HRTF_BLOCK, sizeof(float) * OM_HRTF_BLOCK);
			hv->slotZero[hv->fdlPos] = Hrtf_BlockIsZero(in + b) ? 1 : 0;
		}

		if (skipConvolve) {
			for (ear = 0; ear < 2; ear++) {
				memset(wet, 0, sizeof(wet));
				Hrtf_DelayBlock(hv, ear, wet, outs[ear] + b);
			}
		} else if (!fade) {
			Hrtf_ConvolveBlockStereo(
				hv->fdl, hv->fdlPos, hv->slotZero, hv->filt[hv->cur], hv->filt[hv->cur] + specs, blockOld, blockNew
			);
			Hrtf_DelayBlock(hv, 0, blockOld, outs[0] + b);
			Hrtf_DelayBlock(hv, 1, blockNew, outs[1] + b);
		} else {
			float newL[OM_HRTF_BLOCK];
			float newR[OM_HRTF_BLOCK];
			const float step = 1.0f / (float)OM_HRTF_BLOCK;
			Hrtf_ConvolveBlockStereo(
				hv->fdl, hv->fdlPos, hv->slotZero, hv->filt[hv->cur], hv->filt[hv->cur] + specs, blockOld, blockNew
			);
			Hrtf_ConvolveBlockStereo(
				hv->fdl, hv->fdlPos, hv->slotZero, hv->filt[hv->cur ^ 1], hv->filt[hv->cur ^ 1] + specs, newL, newR
			);
			for (i = 0; i < OM_HRTF_BLOCK; i++) {
				const float mu = (float)(i + 1) * step;
				wet[i]         = blockOld[i] + (newL[i] - blockOld[i]) * mu;
			}
			Hrtf_DelayBlock(hv, 0, wet, outs[0] + b);
			for (i = 0; i < OM_HRTF_BLOCK; i++) {
				const float mu = (float)(i + 1) * step;
				wet[i]         = blockNew[i] + (newR[i] - blockNew[i]) * mu;
			}
			Hrtf_DelayBlock(hv, 1, wet, outs[1] + b);
		}
		if (fade) {
			hv->cur ^= 1;
			fade = qfalse;
		}
		if (!skipConvolve) {
			hv->fdlPos = (hv->fdlPos + 1) % s_hrtf.parts;
		}
	}
}

qboolean OmHrtf_Active(void)
{
	return s_hrtf.ready;
}

/* Drop SOFA + design scratch once DIRECT bake owns runtime lookups. */
static void Hrtf_ReleaseSofa(void)
{
	if (s_hrtf.easy) {
		mysofa_close(s_hrtf.easy);
		s_hrtf.easy = NULL;
	}
	Hrtf_Free(&s_hrtf.irL);
	Hrtf_Free(&s_hrtf.irR);
	Hrtf_Free(&s_hrtf.mpBuf);
	Hrtf_Free(&s_hrtf.mpSpec);
	Hrtf_Free(&s_hrtf.mpWork);
	if (s_hrtf.mpSetup) {
		pffft_destroy_setup(s_hrtf.mpSetup);
		s_hrtf.mpSetup = NULL;
	}
}

void OmHrtf_Unload(void)
{
	size_t i;

	for (i = 0; i < s_hrtf.freeVoices.size(); i++) {
		Hrtf_VoiceDestroy(s_hrtf.freeVoices[i]);
	}
	s_hrtf.freeVoices.clear();

	Hrtf_BakeFree();
	s_hrtf.sofaLive = qfalse;

	if (s_hrtf.easy) {
		mysofa_close(s_hrtf.easy);
		s_hrtf.easy = NULL;
	}
	if (s_hrtf.setup) {
		pffft_destroy_setup(s_hrtf.setup);
		s_hrtf.setup = NULL;
	}
	if (s_hrtf.mpSetup) {
		pffft_destroy_setup(s_hrtf.mpSetup);
		s_hrtf.mpSetup = NULL;
	}
	Hrtf_Free(&s_hrtf.work);
	Hrtf_Free(&s_hrtf.time);
	Hrtf_Free(&s_hrtf.acc);
	Hrtf_Free(&s_hrtf.accB);
	Hrtf_Free(&s_hrtf.padded);
	Hrtf_Free(&s_hrtf.shifted);
	Hrtf_Free(&s_hrtf.irL);
	Hrtf_Free(&s_hrtf.irR);
	Hrtf_Free(&s_hrtf.mpBuf);
	Hrtf_Free(&s_hrtf.mpSpec);
	Hrtf_Free(&s_hrtf.mpWork);
	Hrtf_Free(&s_hrtf.ambiFilt);
	Hrtf_Free(&s_hrtf.ambiFdl);
	Hrtf_Free(&s_hrtf.ambiIn);
	s_hrtf.ambiParts = 0;
	s_hrtf.maxDelay  = 0;
	s_hrtf.delayLen  = 0;
	s_hrtf.ready     = qfalse;
}

qboolean OmHrtf_Load(const void *data, int size, int sampleRate, const char *label)
{
	int      filterLength = 0;
	int      err          = 0;
	int      maxDelay     = 0;
	int      total;
	float    frontEnergy;
	float    delays[2];
	unsigned i;

	OmHrtf_Unload();
	if (!data || size <= 0 || sampleRate <= 0) {
		return qfalse;
	}

	s_hrtf.easy = mysofa_open_data((const char *)data, size, (float)sampleRate, &filterLength, &err);
	if (!s_hrtf.easy || err != MYSOFA_OK || filterLength <= 0) {
		Com_Printf("Sound/omaha: HRTF '%s' could not be read (libmysofa error %d)\n", label, err);
		OmHrtf_Unload();
		return qfalse;
	}

	/* SOFA Data.Delay is in samples (libmysofa rescales it when resampling). */
	for (i = 0; i < s_hrtf.easy->hrtf->DataDelay.elements; i++) {
		const int d = (int)ceilf(s_hrtf.easy->hrtf->DataDelay.values[i]) + 1;
		if (d > maxDelay) {
			maxDelay = d;
		}
	}
	/* Room for residual onset detection on top of SOFA Data.Delay. */
	maxDelay += 64;
	if (maxDelay < 1) {
		maxDelay = 1;
	}
	if (maxDelay > OM_HRTF_MAX_DELAY) {
		Com_Printf("Sound/omaha: HRTF '%s' onset delay %d exceeds max %d\n", label, maxDelay, OM_HRTF_MAX_DELAY);
		maxDelay = OM_HRTF_MAX_DELAY;
	}

	s_hrtf.sampleRate = sampleRate;
	s_hrtf.irLen      = filterLength;
	s_hrtf.maxDelay   = maxDelay;
	s_hrtf.delayLen   = maxDelay + 2;
	/* Partitions cover the min-phase FIR only; ITD is a separate delay line. */
	s_hrtf.parts = (filterLength + OM_HRTF_BLOCK - 1) / OM_HRTF_BLOCK;
	if (s_hrtf.parts > OM_HRTF_MAX_PARTS / 2) {
		Com_Printf(
			"Sound/omaha: HRTF '%s' is too long (%d taps); max %d\n",
			label,
			filterLength,
			OM_HRTF_MAX_PARTS / 2 * OM_HRTF_BLOCK
		);
		OmHrtf_Unload();
		return qfalse;
	}
	total = s_hrtf.parts * OM_HRTF_BLOCK;

	s_hrtf.setup   = pffft_new_setup(OM_HRTF_FFT, PFFFT_REAL);
	s_hrtf.mpSetup = pffft_new_setup(OM_DESIGN_FFT, PFFFT_REAL);
	s_hrtf.work    = Hrtf_Alloc(OM_HRTF_FFT);
	s_hrtf.time    = Hrtf_Alloc(OM_HRTF_FFT);
	s_hrtf.acc     = Hrtf_Alloc(OM_HRTF_FFT);
	s_hrtf.accB    = Hrtf_Alloc(OM_HRTF_FFT);
	s_hrtf.padded  = Hrtf_Alloc(OM_HRTF_FFT);
	s_hrtf.shifted = Hrtf_Alloc(total * 2);
	s_hrtf.irL     = Hrtf_Alloc(filterLength);
	s_hrtf.irR     = Hrtf_Alloc(filterLength);
	s_hrtf.mpBuf   = Hrtf_Alloc(OM_DESIGN_FFT);
	s_hrtf.mpSpec  = Hrtf_Alloc(OM_DESIGN_FFT);
	s_hrtf.mpWork  = Hrtf_Alloc(OM_DESIGN_FFT);
	if (!s_hrtf.setup || !s_hrtf.mpSetup || !s_hrtf.work || !s_hrtf.time || !s_hrtf.acc || !s_hrtf.accB || !s_hrtf.padded
		|| !s_hrtf.shifted || !s_hrtf.irL || !s_hrtf.irR || !s_hrtf.mpBuf || !s_hrtf.mpSpec || !s_hrtf.mpWork) {
		Com_Printf("Sound/omaha: HRTF setup failed (out of memory)\n");
		OmHrtf_Unload();
		return qfalse;
	}

	/* Global level: front HRIR energy per ear equals the stereo decoder's front gain. */
	s_hrtf.scale = 1.0f;
	Hrtf_QueryIr(1.0f, 0.0f, 0.0f, s_hrtf.shifted, s_hrtf.shifted + total, delays);
	frontEnergy = 0.0f;
	for (int t = 0; t < total * 2; t++) {
		frontEnergy += s_hrtf.shifted[t] * s_hrtf.shifted[t];
	}
	frontEnergy *= 0.5f;
	s_hrtf.scale = frontEnergy > 1.0e-12f ? sqrtf(OM_HRTF_FRONT_ENERGY / frontEnergy) : 1.0f;

	if (!Hrtf_BuildAmbiDecoder()) {
		Com_Printf("Sound/omaha: HRTF decoder setup failed (out of memory)\n");
		OmHrtf_Unload();
		return qfalse;
	}

	{
		/* Added in Omaha: dense DIRECT bake + quality gate; SOFA optional after. */
		cvar_t        *liveCv = Cvar_Get("s_hrtf_live_query", "0", CVAR_SOUND_LATCH);
		const qboolean forceLive = (liveCv && liveCv->integer) ? qtrue : qfalse;
		const unsigned  sofaM     = s_hrtf.easy->hrtf->M;
		qboolean        gateOk;

		s_hrtf.sofaLive = qtrue;
		if (!Hrtf_BakeDirectGrid()) {
			Com_Printf("Sound/omaha: HRTF DIRECT bake failed (OOM) — using live SOFA\n");
			s_hrtf.sofaLive = qtrue;
		} else {
			gateOk = Hrtf_BakeQualityGate();
			if (!gateOk) {
				const float medM = s_hrtf.bakeGateMedMagDb;
				const float maxM = s_hrtf.bakeGateMaxMagDb;
				const float medD = s_hrtf.bakeGateMedDelay;
				Hrtf_BakeFree();
				/* Keep gate numbers for tests / logs after freeing the table. */
				s_hrtf.bakeGateMedMagDb = medM;
				s_hrtf.bakeGateMaxMagDb = maxM;
				s_hrtf.bakeGateMedDelay = medD;
				s_hrtf.sofaLive         = qtrue;
			} else if (forceLive) {
				/* Keep bake for metrics / optional later use; runtime stays live. */
				s_hrtf.sofaLive = qtrue;
				Com_Printf("Sound/omaha: s_hrtf_live_query 1 — DIRECT uses live SOFA\n");
			} else {
				Hrtf_ReleaseSofa();
				s_hrtf.sofaLive = qfalse;
			}
		}

		s_hrtf.ready = qtrue;
		OmHrtf_AmbiReset();

		Com_Printf(
			"Sound/omaha: HRTF '%s' loaded (%u measurements, %d taps at %d Hz, %d/%d x %d partitions%s)\n",
			label,
			sofaM,
			filterLength,
			sampleRate,
			s_hrtf.parts,
			s_hrtf.ambiParts,
			OM_HRTF_BLOCK,
			s_hrtf.bakeReady && !s_hrtf.sofaLive ? ", baked DIRECT" : ", live DIRECT"
		);
	}
	return qtrue;
}

qboolean OmHrtf_BakeActive(void)
{
	return (s_hrtf.ready && s_hrtf.bakeReady && !s_hrtf.sofaLive) ? qtrue : qfalse;
}

float OmHrtf_BakeGateMaxMagDb(void)
{
	return s_hrtf.bakeGateMaxMagDb;
}
