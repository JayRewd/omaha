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
// FPS bench: plays benchmark/fpsbench.dm_8 uncapped and reports frame rate
// percentiles, optionally applying them to com_maxfps.

#pragma once

#include "../qcommon/q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

void     CL_FpsBench_Init(void);
void     CL_FpsBench_Shutdown(void);

qboolean CL_FpsBench_Active(void);

void     CL_FpsBench_Frame(void);
// End of demo: restore boost cvars and keep samples for a deferred report.
// Call before CL_Disconnect so Abort is a no-op.
void     CL_FpsBench_DemoCompleted(void);
// Print the deferred report after disconnect noise has settled
void     CL_FpsBench_PrintResults(void);
// Called on disconnect; drops an unfinished run
void     CL_FpsBench_Abort(void);

#ifdef __cplusplus
}
#endif
