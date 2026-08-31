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
#ifndef UID_PROFILE_H
#define UID_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Added in OPM: wall-clock phase profiler (XML load → paint). */

typedef enum uid_prof_phase_e {
	UID_PROF_LOAD_READ = 0,
	UID_PROF_LOAD_PARSE,
	UID_PROF_LOAD_EXPAND,
	UID_PROF_LOAD_COMPILE,
	UID_PROF_LOAD_ADOPT,
	UID_PROF_LEGACY_LOAD, /* Added in OPM: URC UILayout::Load */
	UID_PROF_FRAME_BIND,
	UID_PROF_FRAME_LAYOUT,
	UID_PROF_FRAME_POINTER,
	UID_PROF_FRAME_PAINT_CHROME,
	UID_PROF_FRAME_PAINT_OVERLAY,
	/* Nested under FRAME_BIND (detail only; not summed again into totalUs). */
	UID_PROF_FRAME_FOREACH_WINDOW,   /* Added in OPM: <foreach mode="window"> expand */
	UID_PROF_FRAME_COLLECTION_CULL,  /* Added in OPM: visibility prepass + collection cull walk */
	UID_PROF_HOST_WORLD,
	UID_PROF_HOST_CHROME,
	UID_PROF_HOST_PREVIEWS,
	UID_PROF_HOST_OVERLAY,
	UID_PROF_HOST_BATCH_FLUSH,
	UID_PROF_LEGACY_EVENTS,  /* Added in OPM: uWinMan.ServiceEvents */
	UID_PROF_LEGACY_VIEW3D,  /* Added in OPM: View3D (world + cgame 2D) — not URC */
	UID_PROF_LEGACY_URC,     /* Added in OPM: URC widget Display only */
	UID_PROF_LEGACY_MISC,    /* Added in OPM: UI_Update HUD/menu logic between events and draw */
	UID_PROF_COUNT
} uid_prof_phase_t;

typedef struct uid_prof_timings_s {
	long long us[UID_PROF_COUNT];
	long long totalUs;
	int       layoutRan;
	int       nodeCount;
	char      label[128];
} uid_prof_timings_t;

void        UID_ProfileSetEnabled(int enabled);
int         UID_ProfileEnabled(void);
const char *UID_ProfilePhaseName(uid_prof_phase_t phase);

void UID_ProfileResetLoad(void);
void UID_ProfileResetFrame(void);
void UID_ProfileBegin(uid_prof_phase_t phase);
void UID_ProfileEnd(uid_prof_phase_t phase);

void UID_ProfileSetLoadLabel(const char *label);
void UID_ProfileSetFrameLabel(const char *label);
void UID_ProfileSetFrameMeta(int layoutRan, int nodeCount);

void UID_ProfileCaptureLoad(uid_prof_timings_t *out);
void UID_ProfileCaptureFrame(uid_prof_timings_t *out);

#ifdef __cplusplus
}
#endif

#endif /* UID_PROFILE_H */
