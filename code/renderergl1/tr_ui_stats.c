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

#include "tr_local.h"

/* Added in Omaha: per-frame UI GL event counters (ui_perf_hud). */
uiGlStats_t tr_uiStats;
static uiGlStats_t s_last;

/*
 * GPU timer query ring (optional, r_uiPerfGpu).
 * Nested GL_TIME_ELAPSED is illegal — end ui before resolve; layer begin ends ui
 * and restarts on layer end (s_gpuUiOpen). Resolve is timed separately into
 * gpuResolveNs; ui segments (excluding resolve/layer holes) sum into gpuUiNs.
 */
#define UI_GPU_RING_FRAMES 4
#define UI_GPU_MAX_LAYERS  8
#define UI_GPU_MAX_UI_SEGS 16

typedef struct {
	GLuint ui[UI_GPU_MAX_UI_SEGS];
	int    uiCount;
	GLuint resolve;
	GLuint layers[UI_GPU_MAX_LAYERS];
	int    layerCount;
	int    used;
} uiGpuSlot_t;

static uiGpuSlot_t s_gpuRing[UI_GPU_RING_FRAMES];
static int         s_gpuWrite;
static int         s_gpuFrameSerial;
static int         s_gpuQueriesReady;
static int         s_gpuUiOpen;
static int         s_gpuResolveOpen;
static int         s_gpuLayerOpen;
static int         s_gpuWantUi; /* inside UI2D target when not in layer/resolve */

cvar_t *r_uiPerfGpu;

static qboolean RE_UiGpuAvailable(void)
{
	return (qglGenQueries && qglDeleteQueries && qglBeginQuery && qglEndQuery
			&& qglGetQueryObjectuiv && qglGetQueryObjectui64v)
			   ? qtrue
			   : qfalse;
}

static void RE_UiGpuEnsureQueries(void)
{
	int i;
	int j;

	if (s_gpuQueriesReady) {
		return;
	}
	if (!RE_UiGpuAvailable()) {
		return;
	}
	for (i = 0; i < UI_GPU_RING_FRAMES; ++i) {
		qglGenQueries(UI_GPU_MAX_UI_SEGS, s_gpuRing[i].ui);
		qglGenQueries(1, &s_gpuRing[i].resolve);
		qglGenQueries(UI_GPU_MAX_LAYERS, s_gpuRing[i].layers);
		s_gpuRing[i].uiCount = 0;
		s_gpuRing[i].layerCount = 0;
		s_gpuRing[i].used = 0;
	}
	s_gpuQueriesReady = 1;
	(void)j;
}

static qboolean RE_UiGpuActive(void)
{
	if (!r_uiPerfGpu || !r_uiPerfGpu->integer) {
		return qfalse;
	}
	RE_UiGpuEnsureQueries();
	return s_gpuQueriesReady ? qtrue : qfalse;
}

static uiGpuSlot_t *RE_UiGpuCur(void)
{
	return &s_gpuRing[s_gpuWrite];
}

static void RE_UiGpuEndUiQuery(void)
{
	if (!s_gpuUiOpen) {
		return;
	}
	qglEndQuery(GL_TIME_ELAPSED);
	s_gpuUiOpen = 0;
}

static void RE_UiGpuBeginUiQuery(void)
{
	uiGpuSlot_t *slot;

	if (s_gpuUiOpen || s_gpuResolveOpen || s_gpuLayerOpen) {
		return;
	}
	if (!RE_UiGpuActive()) {
		return;
	}
	slot = RE_UiGpuCur();
	if (slot->uiCount >= UI_GPU_MAX_UI_SEGS) {
		return;
	}
	qglBeginQuery(GL_TIME_ELAPSED, slot->ui[slot->uiCount]);
	slot->uiCount++;
	slot->used = 1;
	s_gpuUiOpen = 1;
}

static void RE_UiGpuReadbackOld(void)
{
	int            readIdx;
	uiGpuSlot_t   *slot;
	GLuint         avail;
	GLuint64       ns;
	unsigned long long uiSum;
	unsigned long long layerSum;
	int            i;

	/* Every 60th BeginFrame, read the slot 3 frames old. */
	if ((s_gpuFrameSerial % 60) != 0) {
		return;
	}
	if (!s_gpuQueriesReady || !qglGetQueryObjectuiv || !qglGetQueryObjectui64v) {
		return;
	}

	readIdx = (s_gpuWrite + UI_GPU_RING_FRAMES - 3) % UI_GPU_RING_FRAMES;
	slot = &s_gpuRing[readIdx];
	if (!slot->used) {
		return;
	}

	uiSum = 0;
	for (i = 0; i < slot->uiCount; ++i) {
		avail = 0;
		qglGetQueryObjectuiv(slot->ui[i], GL_QUERY_RESULT_AVAILABLE, &avail);
		tr_uiStats.glQueries++;
		if (!avail) {
			return;
		}
		ns = 0;
		qglGetQueryObjectui64v(slot->ui[i], GL_QUERY_RESULT, &ns);
		tr_uiStats.glQueries++;
		uiSum += (unsigned long long)ns;
	}

	avail = 0;
	qglGetQueryObjectuiv(slot->resolve, GL_QUERY_RESULT_AVAILABLE, &avail);
	tr_uiStats.glQueries++;
	if (!avail) {
		return;
	}
	ns = 0;
	qglGetQueryObjectui64v(slot->resolve, GL_QUERY_RESULT, &ns);
	tr_uiStats.glQueries++;
	s_last.gpuResolveNs = (unsigned long long)ns;

	layerSum = 0;
	for (i = 0; i < slot->layerCount; ++i) {
		avail = 0;
		qglGetQueryObjectuiv(slot->layers[i], GL_QUERY_RESULT_AVAILABLE, &avail);
		tr_uiStats.glQueries++;
		if (!avail) {
			return;
		}
		ns = 0;
		qglGetQueryObjectui64v(slot->layers[i], GL_QUERY_RESULT, &ns);
		tr_uiStats.glQueries++;
		layerSum += (unsigned long long)ns;
	}

	s_last.gpuUiNs = uiSum;
	s_last.gpuLayerNs = layerSum;
	s_last.gpuSamplesValid = 1;
	slot->used = 0;
}

void RE_UiStatsFrameBegin(void)
{
	unsigned long long gpuUi;
	unsigned long long gpuResolve;
	unsigned long long gpuLayer;
	int                gpuValid;

	/* Preserve last published GPU samples across the counter roll. */
	gpuUi = s_last.gpuUiNs;
	gpuResolve = s_last.gpuResolveNs;
	gpuLayer = s_last.gpuLayerNs;
	gpuValid = s_last.gpuSamplesValid;

	s_last = tr_uiStats;
	s_last.gpuUiNs = gpuUi;
	s_last.gpuResolveNs = gpuResolve;
	s_last.gpuLayerNs = gpuLayer;
	s_last.gpuSamplesValid = gpuValid;

	memset(&tr_uiStats, 0, sizeof(tr_uiStats));

	s_gpuFrameSerial++;
	if (RE_UiGpuActive()) {
		/* Close any dangling query from a truncated previous frame. */
		RE_UiGpuEndUiQuery();
		if (s_gpuResolveOpen) {
			qglEndQuery(GL_TIME_ELAPSED);
			s_gpuResolveOpen = 0;
		}
		if (s_gpuLayerOpen) {
			qglEndQuery(GL_TIME_ELAPSED);
			s_gpuLayerOpen = 0;
		}
		s_gpuWantUi = 0;
		/* Read 3-frames-old slot before advancing the write index. */
		RE_UiGpuReadbackOld();
		s_gpuWrite = (s_gpuWrite + 1) % UI_GPU_RING_FRAMES;
		s_gpuRing[s_gpuWrite].uiCount = 0;
		s_gpuRing[s_gpuWrite].layerCount = 0;
		s_gpuRing[s_gpuWrite].used = 0;
	}
}

void RE_UiStatsGet(uiGlStats_t *out)
{
	if (!out) {
		return;
	}
	*out = s_last;
}

void RE_UiGpuBeginUi(void)
{
	if (!RE_UiGpuActive()) {
		return;
	}
	s_gpuWantUi = 1;
	RE_UiGpuBeginUiQuery();
}

void RE_UiGpuEndUi(void)
{
	RE_UiGpuEndUiQuery();
	s_gpuWantUi = 0;
}

void RE_UiGpuBeginResolve(void)
{
	uiGpuSlot_t *slot;

	/* Nested TIME_ELAPSED illegal — end ui before resolve. */
	RE_UiGpuEndUiQuery();
	if (!RE_UiGpuActive() || s_gpuResolveOpen || s_gpuLayerOpen) {
		return;
	}
	slot = RE_UiGpuCur();
	qglBeginQuery(GL_TIME_ELAPSED, slot->resolve);
	slot->used = 1;
	s_gpuResolveOpen = 1;
}

void RE_UiGpuEndResolve(void)
{
	if (!s_gpuResolveOpen) {
		return;
	}
	qglEndQuery(GL_TIME_ELAPSED);
	s_gpuResolveOpen = 0;
}

void RE_UiGpuBeginLayer(void)
{
	uiGpuSlot_t *slot;

	/* End ui while a layer query is active. */
	RE_UiGpuEndUiQuery();
	if (!RE_UiGpuActive() || s_gpuLayerOpen || s_gpuResolveOpen) {
		return;
	}
	slot = RE_UiGpuCur();
	if (slot->layerCount >= UI_GPU_MAX_LAYERS) {
		return;
	}
	qglBeginQuery(GL_TIME_ELAPSED, slot->layers[slot->layerCount]);
	slot->layerCount++;
	slot->used = 1;
	s_gpuLayerOpen = 1;
}

void RE_UiGpuEndLayer(void)
{
	if (s_gpuLayerOpen) {
		qglEndQuery(GL_TIME_ELAPSED);
		s_gpuLayerOpen = 0;
	}
	/* Restart ui if still inside the UI2D target. */
	if (s_gpuWantUi) {
		RE_UiGpuBeginUiQuery();
	}
}
