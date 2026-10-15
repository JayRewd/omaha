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

// DESCRIPTION:
// Omaha renderer lifetime and mixed-PCM capture for AVI / MP4 export.

#include "../snd_local.h"
#include "snd_renderer.h"
#include "../client.h"

#include <string.h>

const sndRenderer_t *g_sndRenderer = NULL;

qboolean S_Renderer_Init(const sndDeviceConfig_t *cfg)
{
	const sndRenderer_t *r = S_Renderer_Omaha();

	if (!r || !r->Init) {
		g_sndRenderer = NULL;
		return qfalse;
	}

	g_sndRenderer = r;
	if (!r->Init(cfg)) {
		g_sndRenderer = NULL;
		return qfalse;
	}
	return qtrue;
}

void S_Renderer_Shutdown(void)
{
	if (g_sndRenderer && g_sndRenderer->Shutdown) {
		g_sndRenderer->Shutdown();
	}
	g_sndRenderer = NULL;
}

qboolean S_OMAHA_CaptureBegin(void)
{
	int rate;

	if (!g_sndRenderer || !g_sndRenderer->BeginCapture) {
		return qfalse;
	}
	if (g_sndRenderer->CaptureActive && g_sndRenderer->CaptureActive()) {
		return qtrue;
	}

	rate = g_sndRenderer->CaptureSampleRate ? g_sndRenderer->CaptureSampleRate() : 44100;
	return g_sndRenderer->BeginCapture(rate > 0 ? rate : 44100);
}

void S_OMAHA_CaptureEnd(void)
{
	if (g_sndRenderer && g_sndRenderer->EndCapture) {
		g_sndRenderer->EndCapture();
	}
}

qboolean S_OMAHA_CaptureActive(void)
{
	if (!g_sndRenderer || !g_sndRenderer->CaptureActive) {
		return qfalse;
	}
	return g_sndRenderer->CaptureActive();
}

int S_OMAHA_CaptureSampleRate(void)
{
	if (!g_sndRenderer || !g_sndRenderer->CaptureSampleRate) {
		return 44100;
	}
	return g_sndRenderer->CaptureSampleRate();
}

void S_OMAHA_CaptureFrame(int aviFps)
{
	static float s_captureScratch[8192 * 2];
	short        s16[8192 * 2];
	float        frameDuration;
	int          samples;
	int          frames;
	int          rate;
	int          i;
	const int    frameRate = (aviFps > 0) ? aviFps : 25;

	if (!g_sndRenderer || !g_sndRenderer->RenderCapture) {
		return;
	}
	if (!S_OMAHA_CaptureActive() || !CL_VideoRecording()) {
		return;
	}

	rate = S_OMAHA_CaptureSampleRate();
	if (rate <= 0) {
		rate = 44100;
	}

	frameDuration              = ((float)rate / (float)frameRate) + clc.aviSoundFrameRemainder;
	samples                    = (int)frameDuration;
	clc.aviSoundFrameRemainder = frameDuration - (float)samples;
	if (samples < 1) {
		samples = 1;
	}
	if (samples > 8192) {
		samples = 8192;
	}

	frames = g_sndRenderer->RenderCapture(s_captureScratch, samples);
	if (frames <= 0) {
		return;
	}

	/* The AVI / encoder path expects s16 PCM. */
	for (i = 0; i < frames * 2; i++) {
		float s = s_captureScratch[i];
		if (s > 1.f) {
			s = 1.f;
		} else if (s < -1.f) {
			s = -1.f;
		}
		s16[i] = (short)(s * 32767.f);
	}
	CL_WriteAVIAudioFrame((byte *)s16, frames * 4);
}
