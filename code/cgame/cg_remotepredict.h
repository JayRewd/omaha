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

#include "../qcommon/q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

struct centity_s;

void     CG_RP_RegisterCvars(void);
void     CG_RP_Init(void);
void     CG_RP_BeginFrame(void);
void     CG_RP_UpdateEntity(struct centity_s *cent);
qboolean CG_RP_GetPredictedBounds(int entityNum, vec3_t origin, vec3_t mins, vec3_t maxs);
int      CG_RP_GetLeadMsec(void);
// No-op unless CG_RP_DEBUG_DRAW is built (see cg_remotepredict.cpp).
void     CG_RP_DebugDraw(void);

#ifdef __cplusplus
}
#endif
