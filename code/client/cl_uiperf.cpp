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
#include "cl_uiperf.h"
#include "cl_uirender.h"
#include "../uidesign/uid_paint.h"
#include "../uidesign/uid_profile.h"

#include <chrono>
#include <cstring>

/* Added in Omaha: Phase 0 zero-I/O UI performance accumulators (ui_perf_hud). */

cvar_t *ui_perf_hud;
cvar_t *ui_perf_gpu;

namespace {

using clock = std::chrono::steady_clock;

struct UiPerfAcc {
	double sumUs;
	double maxUs;
	int    n;
};

struct UiPerfGlAcc {
	long long fboBinds;
	long long scissorCalls;
	long long scissorLayer;
	long long scissorClip;
	long long scissorSet2d;
	long long scissorStencil;
	long long scissorOther;
	long long glQueries;
	long long drawElements;
	long long drawVerts;
	long long immediateQuads;
	long long set2DWindow;
	long long issuePending;
	long long targetBegins;
	long long targetEnds;
	long long layerBegins;
	long long stencilBegins;
	long long resolvePixels;
	unsigned long long gpuUiNs;
	unsigned long long gpuResolveNs;
	unsigned long long gpuLayerNs;
	int                gpuSamples;
	int                n;
};

static UiPerfAcc    s_acc[UIPERF_ID_COUNT];
static UiPerfGlAcc  s_glAcc;
static UiPerfWindow s_window;
static int          s_replayHits;
static int          s_replayTotal;
static int          s_regionDirty;
static int          s_regionTotal;
static int          s_regionSamples;
static int          s_layoutRuns;
static clock::time_point s_windowStart;
static clock::time_point s_lastFrameBegin;
static clock::time_point s_renderStart;
static int          s_haveLastFrame;
static int          s_renderOpen;

static void AccAdd(UiPerfAcc *a, double us)
{
	if (!a) {
		return;
	}
	a->sumUs += us;
	if (a->n == 0 || us > a->maxUs) {
		a->maxUs = us;
	}
	a->n++;
}

static double AccAvg(const UiPerfAcc *a)
{
	if (!a || a->n <= 0) {
		return 0.0;
	}
	return a->sumUs / (double)a->n;
}

static void AccReset(UiPerfAcc *a)
{
	if (a) {
		a->sumUs = 0.0;
		a->maxUs = 0.0;
		a->n = 0;
	}
}

static void GlAccAdd(const uiGlStats_t *gl)
{
	if (!gl) {
		return;
	}
	s_glAcc.fboBinds += gl->fboBinds;
	s_glAcc.scissorCalls += gl->scissorCalls;
	s_glAcc.scissorLayer += gl->scissorLayer;
	s_glAcc.scissorClip += gl->scissorClip;
	s_glAcc.scissorSet2d += gl->scissorSet2d;
	s_glAcc.scissorStencil += gl->scissorStencil;
	s_glAcc.scissorOther += gl->scissorOther;
	s_glAcc.glQueries += gl->glQueries;
	s_glAcc.drawElements += gl->drawElements;
	s_glAcc.drawVerts += gl->drawVerts;
	s_glAcc.immediateQuads += gl->immediateQuads;
	s_glAcc.set2DWindow += gl->set2DWindow;
	s_glAcc.issuePending += gl->issuePending;
	s_glAcc.targetBegins += gl->targetBegins;
	s_glAcc.targetEnds += gl->targetEnds;
	s_glAcc.layerBegins += gl->layerBegins;
	s_glAcc.stencilBegins += gl->stencilBegins;
	s_glAcc.resolvePixels += gl->resolvePixels;
	if (gl->gpuSamplesValid) {
		s_glAcc.gpuUiNs += gl->gpuUiNs;
		s_glAcc.gpuResolveNs += gl->gpuResolveNs;
		s_glAcc.gpuLayerNs += gl->gpuLayerNs;
		s_glAcc.gpuSamples++;
	}
	s_glAcc.n++;
}

static int GlAvgI(long long sum, int n)
{
	if (n <= 0) {
		return 0;
	}
	return (int)((sum + (n / 2)) / n);
}

static void RollWindow(void)
{
	const int n = s_acc[UIPERF_FRAME].n > 0 ? s_acc[UIPERF_FRAME].n : s_glAcc.n;
	UiPerfWindow w;

	std::memset(&w, 0, sizeof(w));
	w.frames = n > 0 ? n : s_acc[UIPERF_RENDER].n;
	w.frameUs = AccAvg(&s_acc[UIPERF_FRAME]);
	w.renderUs = AccAvg(&s_acc[UIPERF_RENDER]);
	w.uiTotalUs = AccAvg(&s_acc[UIPERF_UI_TOTAL]);
	w.syncUs = AccAvg(&s_acc[UIPERF_SYNC]);
	w.hudCvarUs = AccAvg(&s_acc[UIPERF_HUD_CVAR]);
	w.bindUs = AccAvg(&s_acc[UIPERF_BIND]);
	w.layoutUs = AccAvg(&s_acc[UIPERF_LAYOUT]);
	w.paintUs = AccAvg(&s_acc[UIPERF_PAINT]);
	w.overlayUs = AccAvg(&s_acc[UIPERF_OVERLAY]);
	w.cg2dUs = AccAvg(&s_acc[UIPERF_CG2D]);
	w.maxFrameUs = s_acc[UIPERF_FRAME].maxUs;
	w.layoutRunsPerSec = s_layoutRuns;
	w.replayHits = s_replayHits;
	w.replayTotal = s_replayTotal;
	if (s_replayTotal > 0) {
		w.replayHitPct = 100.0 * (double)s_replayHits / (double)s_replayTotal;
	}
	if (s_regionSamples > 0) {
		w.regionDirty = (s_regionDirty + (s_regionSamples / 2)) / s_regionSamples;
		w.regionTotal = (s_regionTotal + (s_regionSamples / 2)) / s_regionSamples;
	}

	if (s_glAcc.n > 0) {
		const int gn = s_glAcc.n;
		w.gl.fboBinds = GlAvgI(s_glAcc.fboBinds, gn);
		w.gl.scissorCalls = GlAvgI(s_glAcc.scissorCalls, gn);
		w.gl.scissorLayer = GlAvgI(s_glAcc.scissorLayer, gn);
		w.gl.scissorClip = GlAvgI(s_glAcc.scissorClip, gn);
		w.gl.scissorSet2d = GlAvgI(s_glAcc.scissorSet2d, gn);
		w.gl.scissorStencil = GlAvgI(s_glAcc.scissorStencil, gn);
		w.gl.scissorOther = GlAvgI(s_glAcc.scissorOther, gn);
		w.gl.glQueries = GlAvgI(s_glAcc.glQueries, gn);
		w.gl.drawElements = GlAvgI(s_glAcc.drawElements, gn);
		w.gl.drawVerts = GlAvgI(s_glAcc.drawVerts, gn);
		w.gl.immediateQuads = GlAvgI(s_glAcc.immediateQuads, gn);
		w.gl.set2DWindow = GlAvgI(s_glAcc.set2DWindow, gn);
		w.gl.issuePending = GlAvgI(s_glAcc.issuePending, gn);
		w.gl.targetBegins = GlAvgI(s_glAcc.targetBegins, gn);
		w.gl.targetEnds = GlAvgI(s_glAcc.targetEnds, gn);
		w.gl.layerBegins = GlAvgI(s_glAcc.layerBegins, gn);
		w.gl.stencilBegins = GlAvgI(s_glAcc.stencilBegins, gn);
		w.gl.resolvePixels = GlAvgI(s_glAcc.resolvePixels, gn);
		if (s_glAcc.gpuSamples > 0) {
			w.gl.gpuUiNs = s_glAcc.gpuUiNs / (unsigned long long)s_glAcc.gpuSamples;
			w.gl.gpuResolveNs = s_glAcc.gpuResolveNs / (unsigned long long)s_glAcc.gpuSamples;
			w.gl.gpuLayerNs = s_glAcc.gpuLayerNs / (unsigned long long)s_glAcc.gpuSamples;
			w.gl.gpuSamplesValid = 1;
		}
	}

	s_window = w;

	for (int i = 0; i < UIPERF_ID_COUNT; ++i) {
		AccReset(&s_acc[i]);
	}
	std::memset(&s_glAcc, 0, sizeof(s_glAcc));
	s_replayHits = 0;
	s_replayTotal = 0;
	s_regionDirty = 0;
	s_regionTotal = 0;
	s_regionSamples = 0;
	s_layoutRuns = 0;
	s_windowStart = clock::now();
}

static double ElapsedUs(clock::time_point t0, clock::time_point t1)
{
	return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

} // namespace

UiPerfScope::UiPerfScope(int markId)
	: id(-1)
	, t0()
{
	if (!ui_perf_hud || !ui_perf_hud->integer) {
		return;
	}
	id = markId;
	t0 = clock::now();
}

UiPerfScope::~UiPerfScope()
{
	if (id < 0) {
		return;
	}
	CL_UIPerf_Mark(id, ElapsedUs(t0, clock::now()));
}

void CL_UIPerf_Init(void)
{
	ui_perf_hud = Cvar_Get("ui_perf_hud", "0", CVAR_TEMP);
	ui_perf_gpu = Cvar_Get("ui_perf_gpu", "0", CVAR_TEMP);
	CL_UIPerf_Reset();
}

void CL_UIPerf_Shutdown(void)
{
	UID_ProfileSetExternalEnable(0);
}

void CL_UIPerf_Reset(void)
{
	for (int i = 0; i < UIPERF_ID_COUNT; ++i) {
		AccReset(&s_acc[i]);
	}
	std::memset(&s_glAcc, 0, sizeof(s_glAcc));
	std::memset(&s_window, 0, sizeof(s_window));
	s_replayHits = 0;
	s_replayTotal = 0;
	s_regionDirty = 0;
	s_regionTotal = 0;
	s_regionSamples = 0;
	s_layoutRuns = 0;
	s_haveLastFrame = 0;
	s_renderOpen = 0;
	s_windowStart = clock::now();
}

void CL_UIPerf_FrameBegin(void)
{
	const clock::time_point now = clock::now();
	uiGlStats_t             gl;

	if (!ui_perf_hud || !ui_perf_hud->integer) {
		UID_ProfileSetExternalEnable(0);
		return;
	}

	UID_ProfileSetExternalEnable(1);

	/* Mirror client ui_perf_gpu into the renderer cvar. */
	if (ui_perf_gpu) {
		Cvar_Set("r_uiPerfGpu", va("%d", ui_perf_gpu->integer));
	}

	if (s_haveLastFrame) {
		const double frameUs = ElapsedUs(s_lastFrameBegin, now);
		AccAdd(&s_acc[UIPERF_FRAME], frameUs);
	}
	s_lastFrameBegin = now;
	s_haveLastFrame = 1;
	s_renderStart = now;
	s_renderOpen = 1;

	if (re.UiStatsGet) {
		std::memset(&gl, 0, sizeof(gl));
		re.UiStatsGet(&gl);
		GlAccAdd(&gl);
	}

	if (ElapsedUs(s_windowStart, now) >= 1000000.0) {
		RollWindow();
	}
}

void CL_UIPerf_FrameEnd(void)
{
	if (!ui_perf_hud || !ui_perf_hud->integer || !s_renderOpen) {
		return;
	}
	AccAdd(&s_acc[UIPERF_RENDER], ElapsedUs(s_renderStart, clock::now()));
	s_renderOpen = 0;
}

void CL_UIPerf_Mark(int id, double us)
{
	if (!ui_perf_hud || !ui_perf_hud->integer) {
		return;
	}
	if (id < 0 || id >= UIPERF_ID_COUNT) {
		return;
	}
	AccAdd(&s_acc[id], us);
}

void CL_UIPerf_NoteReplay(int hit)
{
	if (!ui_perf_hud || !ui_perf_hud->integer) {
		return;
	}
	s_replayTotal++;
	if (hit) {
		s_replayHits++;
	}
}

void CL_UIPerf_NoteLayoutRan(int mode)
{
	if (!ui_perf_hud || !ui_perf_hud->integer) {
		return;
	}
	if (mode > 0) {
		s_layoutRuns++;
	}
}

void CL_UIPerf_NoteRegions(int dirty, int total)
{
	if (!ui_perf_hud || !ui_perf_hud->integer) {
		return;
	}
	s_regionDirty += dirty;
	s_regionTotal += total;
	s_regionSamples++;
}

const UiPerfWindow *CL_UIPerf_Window(void)
{
	if (!ui_perf_hud || !ui_perf_hud->integer) {
		return nullptr;
	}
	return &s_window;
}

int CL_UIPerf_HudInteger(void)
{
	return (ui_perf_hud && ui_perf_hud->integer) ? ui_perf_hud->integer : 0;
}

void CL_UIPerf_Dump(void)
{
	const UiPerfWindow *w = &s_window;
	char                line[1024];
	char                ospath[MAX_OSPATH];

	Com_sprintf(
		line,
		sizeof(line),
		"UIPERF dump: frames=%d frame=%.0fus render=%.0fus ui=%.0fus sync=%.0fus "
		"bind=%.0fus layout=%.0fus paint=%.0fus cg2d=%.0fus replay=%.0f%% "
		"draws=%d scissor=%d fbo=%d gets=%d set2d=%d issue=%d\n",
		w->frames,
		w->frameUs,
		w->renderUs,
		w->uiTotalUs,
		w->syncUs,
		w->bindUs,
		w->layoutUs,
		w->paintUs,
		w->cg2dUs,
		w->replayHitPct,
		w->gl.drawElements,
		w->gl.scissorCalls,
		w->gl.fboBinds,
		w->gl.glQueries,
		w->gl.set2DWindow,
		w->gl.issuePending
	);
	Com_Printf("%s", line);

	/*
	 * Always persist under fs_homepath/main (install dir for Omaha defaults),
	 * independent of logfile — so agents/tools can read the dump without console scrape.
	 */
	if (FS_WriteFile("ui_perf_dump.txt", line, (int)strlen(line)) > 0) {
		Com_sprintf(
			ospath,
			sizeof(ospath),
			"%s",
			FS_BuildOSPath(Cvar_VariableString("fs_homedatapath"), FS_GetCurrentGameDir(), "ui_perf_dump.txt")
		);
		Com_Printf("UIPERF dump written to %s\n", ospath);
	} else {
		Com_Printf("UIPERF dump: failed to write ui_perf_dump.txt\n");
	}
}
