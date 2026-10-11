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
// Demo playback overlay: timeline index, pause / rate, seek and crop export.

#pragma once

#include "../qcommon/q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	DEMOBITS_COMMANDS,       // after the reliable acknowledge
	DEMOBITS_GAMESTATE,      // before svc_gamestate
	DEMOBITS_SNAPSHOT_START, // before svc_snapshot
	DEMOBITS_SNAPSHOT_END,   // after the snapshot sounds
	DEMOBITS_END             // after svc_EOF
} demoBits_t;

void     CL_DemoPlayback_Init(void);
void     CL_DemoPlayback_Shutdown(void);

// Called by CL_PlayDemo_f once the file is open; path is the opened file
void     CL_DemoPlayback_OnDemoStart(const char *path, const char *arg);
void     CL_DemoPlayback_BeforeMessage(void);
void     CL_DemoPlayback_AfterMessage(const byte *data, int size);
void     CL_DemoPlayback_NoteBits(int which, int bit);
// Returns qtrue when the end of the file should hold instead of leaving the demo
qboolean CL_DemoPlayback_HoldAtEnd(void);
qboolean CL_DemoPlayback_AtEnd(void);

void     CL_DemoPlayback_AdjustTimeDelta(void);
qboolean CL_DemoPlayback_FastForwarding(void);
int      CL_DemoPlayback_FastForwardBatch(void);
qboolean CL_DemoPlayback_CanReadAhead(void);
qboolean CL_DemoPlayback_MuteEventSounds(void);
qboolean CL_DemoPlayback_MuteLoopingSounds(void);

void     CL_DemoPlayback_Frame(void);

// UI actions
void     CL_DemoPlayback_TogglePause(void);
void     CL_DemoPlayback_RateReset(void);
void     CL_DemoPlayback_RateStep(void);
void     CL_DemoPlayback_SeekToUIPosition(void);
void     CL_DemoPlayback_Export(void);
void     CL_DemoPlayback_ToggleOverlay(void);

// Enemy frags by the recording player, as 0..1 timeline fractions
int      CL_DemoPlayback_FragCount(void);
float    CL_DemoPlayback_FragPos(int index);
unsigned CL_DemoPlayback_FragRevision(void);

#ifdef __cplusplus
}
#endif
