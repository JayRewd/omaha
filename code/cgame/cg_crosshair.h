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

#include "q_shared.h"
#include "crosshair_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

void CG_Crosshair_RegisterCvars(void);
void CG_Crosshair_SyncClAliases(void);

qboolean CG_Crosshair_ModeEnabled(void);

void CG_DrawModernCrosshair(qboolean friendTarget);

/* Added in Omaha: fire-mean HUD offset in viewport pixels (currently 0; camera includes kick). */
void CG_Crosshair_FireMeanOffset(float screenW, float screenH, float *dx, float *dy);

/* Added in Omaha: live hipfire config for UIR overlay paint. recoilDx/Dy are overlay px. */
qboolean CG_ModernCrosshairLive(
	xhair_config_t *cfg, float *recoilDx, float *recoilDy, float logicalWidth, float logicalHeight
);

#ifdef __cplusplus
}
#endif
