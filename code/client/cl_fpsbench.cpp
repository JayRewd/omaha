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
// FPS bench.
//
// Plays benchmark/fpsbench.dm_8 from the game filesystem with the frame cap
// and vsync off. Every client frame after the load and a short warmup is timed
// in microseconds. Percentiles are taken on frame times (slow side) and
// reported as frame rates. "fpsbench set" applies com_maxfps from p99.

#include "client.h"
#include "cl_fpsbench.h"
#include "cl_ui.h"

#include <SDL.h>

#include <algorithm>
#include <vector>

#define FPSBENCH_DEMO_PATH  "benchmark/fpsbench.dm_8"
#define FPSBENCH_DEMO_NAME  "fpsbench"
#define FPSBENCH_WARMUP_US  1000000
#define FPSBENCH_MIN_FRAMES 100
#define FPSBENCH_MIN_MAXFPS 125

static qboolean             s_active;
static qboolean             s_apply;
static qboolean             s_pendingReport;
static qboolean             s_openConsole;
static int64_t              s_readyUs;
static int64_t              s_lastUs;
static std::vector<int64_t> s_frameUs;

static char s_savedMaxFps[MAX_CVAR_VALUE_STRING];
static char s_savedMaxFpsUnfocused[MAX_CVAR_VALUE_STRING];
static char s_savedMaxFpsMinimized[MAX_CVAR_VALUE_STRING];
static char s_savedTimedemo[MAX_CVAR_VALUE_STRING];
static int  s_savedSwapInterval;

static void FB_BeginBoost(void)
{
	Q_strncpyz(s_savedMaxFps, Cvar_VariableString("com_maxfps"), sizeof(s_savedMaxFps));
	Q_strncpyz(s_savedMaxFpsUnfocused, Cvar_VariableString("com_maxfpsUnfocused"), sizeof(s_savedMaxFpsUnfocused));
	Q_strncpyz(s_savedMaxFpsMinimized, Cvar_VariableString("com_maxfpsMinimized"), sizeof(s_savedMaxFpsMinimized));
	Q_strncpyz(s_savedTimedemo, Cvar_VariableString("timedemo"), sizeof(s_savedTimedemo));
	s_savedSwapInterval = Cvar_VariableIntegerValue("r_swapInterval");

	Cvar_Set("com_maxfps", "0");
	Cvar_Set("com_maxfpsUnfocused", "0");
	Cvar_Set("com_maxfpsMinimized", "0");
	Cvar_Set("timedemo", "0");
	if (SDL_GL_SetSwapInterval(0) != 0) {
		Com_DPrintf("fpsbench: SDL_GL_SetSwapInterval(0) failed: %s\n", SDL_GetError());
	}
}

static void FB_EndBoost(void)
{
	Cvar_Set("com_maxfps", s_savedMaxFps);
	Cvar_Set("com_maxfpsUnfocused", s_savedMaxFpsUnfocused);
	Cvar_Set("com_maxfpsMinimized", s_savedMaxFpsMinimized);
	Cvar_Set("timedemo", s_savedTimedemo);
	if (SDL_GL_SetSwapInterval(s_savedSwapInterval) != 0) {
		Com_DPrintf("fpsbench: restore swap interval failed: %s\n", SDL_GetError());
	}
}

static void FB_Stop(void)
{
	FB_EndBoost();
	s_active = qfalse;
	s_pendingReport = qfalse;
	s_frameUs.clear();
	s_frameUs.shrink_to_fit();
}

// Frame rate at the given percentile of frame time (nearest rank)
static float FB_PercentileFps(const std::vector<int64_t> &sorted, double p)
{
	size_t rank = (size_t)(p * sorted.size() + 0.999999);

	if (rank < 1) {
		rank = 1;
	} else if (rank > sorted.size()) {
		rank = sorted.size();
	}
	return 1000000.0f / (float)std::max<int64_t>(sorted[rank - 1], 1);
}

static void FB_Report(void)
{
	std::vector<int64_t> sorted = s_frameUs;
	int64_t              total = 0;
	float                avg, p95, p99, p999, best, worst;
	int                  maxfps;

	if (sorted.size() < FPSBENCH_MIN_FRAMES) {
		Com_Printf(S_COLOR_YELLOW "fpsbench: not enough frames measured (%d)\n", (int)sorted.size());
		return;
	}

	std::sort(sorted.begin(), sorted.end());
	for (int64_t us : sorted) {
		total += us;
	}

	avg = (float)((double)sorted.size() * 1000000.0 / (double)std::max<int64_t>(total, 1));
	p95 = FB_PercentileFps(sorted, 0.95);
	p99 = FB_PercentileFps(sorted, 0.99);
	p999 = FB_PercentileFps(sorted, 0.999);
	best = 1000000.0f / (float)std::max<int64_t>(sorted.front(), 1);
	worst = 1000000.0f / (float)std::max<int64_t>(sorted.back(), 1);

	Com_Printf("avg   %.1f fps\n", avg);
	Com_Printf("p95   %.1f fps\n", p95);
	Com_Printf("p99   %.1f fps\n", p99);
	Com_Printf("p99.9 %.1f fps\n", p999);
	Com_Printf("max   %.1f fps\n", best);
	Com_Printf("min   %.1f fps\n", worst);

	if (s_apply) {
		// p99 minus 100 headroom, floored to the nearest hundred, at least 125
		maxfps = ((int)(p99 - 100.0f) / 100) * 100;
		if (maxfps < FPSBENCH_MIN_MAXFPS) {
			maxfps = FPSBENCH_MIN_MAXFPS;
		}
		Cvar_Set("com_maxfps", va("%d", maxfps));
		Com_Printf("com_maxfps set to %d - Nearest stable value\n", maxfps);
	}
}

/*
====================
CL_FpsBench_f

fpsbench [set]
====================
*/
static void CL_FpsBench_f(void)
{
	qboolean apply = qfalse;

	if (Cmd_Argc() > 2 || (Cmd_Argc() == 2 && Q_stricmp(Cmd_Argv(1), "set"))) {
		Com_Printf("usage: fpsbench [set]\n");
		Com_Printf("  fpsbench      play the benchmark demo and print frame rates\n");
		Com_Printf("  fpsbench set  same, then set com_maxfps from p99 - 100 (floor 100s, min %d)\n", FPSBENCH_MIN_MAXFPS);
		return;
	}
	if (Cmd_Argc() == 2) {
		apply = qtrue;
	}

	// make sure a local server is killed
	// 2 means don't force disconnect of local client
	Cvar_Set("sv_killserver", "2");

	CL_Disconnect();

	FS_FOpenFileRead(FPSBENCH_DEMO_PATH, &clc.demofile, qtrue, qtrue);
	if (!clc.demofile) {
		Com_Printf(S_COLOR_YELLOW "fpsbench: couldn't open %s\n", FPSBENCH_DEMO_PATH);
		return;
	}

	FB_BeginBoost();
	s_active = qtrue;
	s_apply = apply;
	s_pendingReport = qfalse;
	s_openConsole = qfalse;
	s_readyUs = 0;
	s_lastUs = 0;
	s_frameUs.clear();
	s_frameUs.reserve(65536);

	Com_Printf("fpsbench: running, results print when the demo ends\n");
	CL_BeginDemoPlayback(FPSBENCH_DEMO_PATH, FPSBENCH_DEMO_NAME);
}

void CL_FpsBench_Init(void)
{
	Cmd_AddCommand("fpsbench", CL_FpsBench_f);
}

void CL_FpsBench_Shutdown(void)
{
	Cmd_RemoveCommand("fpsbench");
	if (s_active) {
		FB_Stop();
	}
}

qboolean CL_FpsBench_Active(void)
{
	return s_active;
}

void CL_FpsBench_Frame(void)
{
	int64_t now;

	if (!s_active) {
		if (s_openConsole && clc.state <= CA_DISCONNECTED) {
			s_openConsole = qfalse;
			UI_OpenConsole();
		}
		return;
	}

	now = Sys_Microseconds();
	if (clc.state != CA_ACTIVE || !clc.demoplaying || server_loading) {
		s_readyUs = 0;
		return;
	}
	if (!s_readyUs) {
		s_readyUs = now;
		s_lastUs = now;
		return;
	}
	if (now - s_readyUs >= FPSBENCH_WARMUP_US) {
		s_frameUs.push_back(now - s_lastUs);
	}
	s_lastUs = now;
}

void CL_FpsBench_DemoCompleted(void)
{
	if (!s_active) {
		return;
	}
	// Restore boost cvars now; keep samples so PrintResults can run after
	// CL_Disconnect / CL_NextDemo have finished dumping to the console.
	FB_EndBoost();
	s_active = qfalse;
	s_pendingReport = qtrue;
}

void CL_FpsBench_PrintResults(void)
{
	if (!s_pendingReport) {
		return;
	}
	FB_Report();
	s_pendingReport = qfalse;
	s_frameUs.clear();
	s_frameUs.shrink_to_fit();
	s_openConsole = qtrue;
}

void CL_FpsBench_Abort(void)
{
	if (!s_active) {
		return;
	}
	Com_Printf("fpsbench: aborted\n");
	FB_Stop();
}
