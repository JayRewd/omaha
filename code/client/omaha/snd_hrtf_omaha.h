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

/*
 * Added in Omaha: headphone HRTF for the omaha mixer (snd_render_omaha.cpp).
 *
 * SOFA data via libmysofa, uniform partitioned overlap-save convolution via PFFFT.
 * Two paths share one HRTF set. Modern min-phase + MagLS pipeline so motion does not
 * comb while pinna / ITD cues stay intact:
 *  - per-voice direct: dense elev/az bake of onset-aligned + min-phase FIRs (full IR,
 *    float ITD) with bilinear lookup; live SOFA only if bake gate fails or
 *    s_hrtf_live_query; re-query + crossfade only the min-phase filters, and
 *  - 3rd-order 3D ambisonic bus (ACN / N3D): MagLS Lebedev decoder (complex LS / ITD
 *    below ~2 kHz, magnitude match above with Hold average phase continuation) plus
 *    diffuse-field EQ. Bus runs ~64 samples behind.
 *
 * Directions are unit vectors in OpenAL listener space (x right, y up, -z front).
 * Every process call takes a sample count that is a multiple of OM_HRTF_BLOCK; the
 * mixer renders whole blocks in HRTF mode so filtering adds no latency.
 * Nothing here locks: the mixer calls in with s_mixMutex held.
 */

#include "../../qcommon/q_shared.h"

#define OM_HRTF_BLOCK    128
#define OM_AMBI_ORDER    3
#define OM_AMBI_CHANNELS 16

typedef struct omHrtfVoice_s omHrtfVoice_t;

/* Build the HRTF context from a SOFA file image, resampled to sampleRate. */
qboolean OmHrtf_Load(const void *data, int size, int sampleRate, const char *label);
void     OmHrtf_Unload(void);
qboolean OmHrtf_Active(void);
/* Added in Omaha: DIRECT dense bake is active (SOFA closed). */
qboolean OmHrtf_BakeActive(void);
/* Added in Omaha: last bake-vs-live gate max magnitude error (dB). */
float OmHrtf_BakeGateMaxMagDb(void);

/* Per-voice direct path. Alloc reuses a free list; Free returns it there. */
omHrtfVoice_t *OmHrtf_VoiceAlloc(void);
void           OmHrtf_VoiceFree(omHrtfVoice_t *hv);
void           OmHrtf_VoiceReset(omHrtfVoice_t *hv);
/* Convolve mono `in` and add the binaural result to outL / outR. */
void OmHrtf_VoiceProcess(omHrtfVoice_t *hv, const float dir[3], const float *in, float *outL, float *outR, int count);
/* Blocks of input that still ring out of a voice after its input stops. */
int OmHrtf_TailBlocks(void);

/* 3rd-order 3D ambisonic coefficients (ACN / N3D) for a listener-space direction. */
void OmHrtf_AmbiCoeffs(const float dir[3], float coeffs[OM_AMBI_CHANNELS]);
void OmHrtf_AmbiReset(void);
/* Convolve the bus with the decoder filters and add to outL / outR. */
void OmHrtf_AmbiDecode(float *const ambi[OM_AMBI_CHANNELS], float *outL, float *outR, int count);
/* Added in Omaha: per-worker FFT scratch so direct voices convolve in parallel. */
void OmHrtf_SetThreadScratch(float *work, float *acc, float *accB, float *time);
void OmHrtf_AllocScratch(float **work, float **acc, float **accB, float **time);
void OmHrtf_FreeScratch(float *work, float *acc, float *accB, float *time);
