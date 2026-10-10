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

// Changed in Omaha: practice markers, chase cam, HUD text and angles node ids are
// server-side (stock entities, Camera, huddraw). This file only keeps listen-host
// extras: cg_practice_nadecam (read by fgame) and the charge-bar release tick.

#include "cg_local.h"

cvar_t *cg_practice_nadecam;
// Added in Omaha: aimed throw cook fraction for HUD release tick (-1 = hidden).
static cvar_t *cg_practice_nade_release;

extern "C" void CG_Practice_Init(void)
{
	// Server camera honors this on a listen server (read by the practice game code).
	cg_practice_nadecam      = cgi.Cvar_Get("cg_practice_nadecam", "1", CVAR_ARCHIVE);
	cg_practice_nade_release = cgi.Cvar_Get("cg_practice_nade_release", "-1", 0);
	cgi.Cvar_Set("cg_practice_nade_release", "-1");
}

extern "C" qboolean CG_Practice_ServerCommand(void)
{
	const char *cmd = cgi.Argv(0);

	if (!cmd || Q_stricmpn(cmd, "omp_", 4)) {
		return qfalse;
	}

	// Added in Omaha: server-selected aimed throw → HUD charge-bar release tick.
	if (!Q_stricmp(cmd, "omp_release")) {
		cgi.Cvar_Set("cg_practice_nade_release", va("%.3f", atof(cgi.Argv(1))));
		return qtrue;
	}

	return qfalse;
}

// Removed in Omaha: floating node labels moved to server iHudDraw (angles edit aim list).
extern "C" void CG_Practice_Draw2D(void) {}
