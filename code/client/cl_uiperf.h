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

#ifndef CL_UIPERF_H
#define CL_UIPERF_H

#include "../qcommon/q_shared.h"
#include "../renderercommon/tr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Added in Omaha: Phase 0 modern UI performance overlay (ui_perf_hud). */

enum {
	UIPERF_FRAME = 0,
	UIPERF_RENDER,
	UIPERF_UI_TOTAL,
	UIPERF_SYNC,
	UIPERF_HUD_CVAR,
	UIPERF_BIND,
	UIPERF_LAYOUT,
	UIPERF_PAINT,
	UIPERF_OVERLAY,
	UIPERF_CG2D,
	UIPERF_ID_COUNT
};

typedef struct UiPerfWindow_s {
	double      frameUs;
	double      renderUs;
	double      uiTotalUs;
	double      syncUs;
	double      hudCvarUs;
	double      bindUs;
	double      layoutUs;
	double      paintUs;
	double      overlayUs;
	double      cg2dUs;
	double      replayHitPct;
	double      maxFrameUs;
	int         layoutRunsPerSec;
	int         frames;
	int         replayHits;
	int         replayTotal;
	int         regionDirty;
	int         regionTotal;
	uiGlStats_t gl;
} UiPerfWindow;

void               CL_UIPerf_Init(void);
void               CL_UIPerf_Shutdown(void);
void               CL_UIPerf_FrameBegin(void);
void               CL_UIPerf_FrameEnd(void);
void               CL_UIPerf_Mark(int id, double us);
void               CL_UIPerf_NoteReplay(int hit);
void               CL_UIPerf_NoteLayoutRan(int mode);
void               CL_UIPerf_NoteRegions(int dirty, int total);
const UiPerfWindow *CL_UIPerf_Window(void);
void               CL_UIPerf_Reset(void);
void               CL_UIPerf_Dump(void);
int                CL_UIPerf_HudInteger(void);

extern cvar_t *ui_perf_hud;
extern cvar_t *ui_perf_gpu;

#ifdef __cplusplus
}

#include <chrono>

/* RAII scope timer — no-op when ui_perf_hud is 0 (one int compare). */
struct UiPerfScope {
	int                                   id;
	std::chrono::steady_clock::time_point t0;

	explicit UiPerfScope(int markId);
	~UiPerfScope();

	UiPerfScope(const UiPerfScope &) = delete;
	UiPerfScope &operator=(const UiPerfScope &) = delete;
};
#endif

#endif /* CL_UIPERF_H */
