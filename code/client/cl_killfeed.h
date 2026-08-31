/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#pragma once

#include "../qcommon/q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Added in OPM: parse TA printdeathmsg into structured kill-feed host row. */
void CL_KillFeed_HandlePrintDeathMsg(void);

/* Added in OPM: Base MOH death lines arrive as print → dmbox; parse into kill-feed. */
void CL_KillFeed_HandleDeathPrint(const char *text, int friendly);

/* Testable classifier — match raw English s1/s2 before LV_ConvertString. */
void CL_KillFeed_Classify(
	const char *s1,
	const char *s2,
	char        typeChar,
	char       *weaponClassOut,
	size_t      weaponClassSize,
	char       *killKindOut,
	size_t      killKindSize,
	int        *headshotOut,
	int        *friendlyOut
);

/* Added in OPM: reverse-parse G_PrintDeathMessageEmulated English line. */
qboolean CL_KillFeed_ParseDeathPrint(
	const char *text,
	char       *victimOut,
	size_t      victimSize,
	char       *attackerOut,
	size_t      attackerSize,
	char       *s1Out,
	size_t      s1Size,
	char       *s2Out,
	size_t      s2Size,
	char       *typeCharOut
);

#ifdef __cplusplus
}
#endif
