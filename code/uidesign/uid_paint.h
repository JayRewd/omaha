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
#ifndef UID_PAINT_H
#define UID_PAINT_H

#include "uid_document.h"
#include "uid_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Added in Omaha: Stage 4 retained paint command list (ui_paint_list).
 * Records batch draws + clip rects during UID_PaintChrome; replays on frames
 * with no PAINT/LAYOUT/STRUCTURE dirt.
 */
void UID_SetPaintList(int enabled);
int  UID_PaintListEnabled(void);

void UID_PaintListInvalidate(uid_document_t *doc);
void UID_PaintListMarkHostDraw(void); /* model/host — list cannot replay */
/* Added in Omaha: soft mask-image uses a layer RT; retained list cannot replay it. */
void UID_PaintListMarkImageMask(void);
void UID_PaintListFree(uid_document_t *doc);

/* Added in Omaha: record SVG shape child-clips so shaped HUD/scoreboard can replay. */
void UID_PaintListRecordShapeClipBegin(
	float x,
	float y,
	float w,
	float h,
	const char *const *pathD,
	int pathCount,
	float viewW,
	float viewH,
	float rotationDeg
);
void UID_PaintListRecordShapeClipEnd(void);

/* Returns 1 if chrome was fully handled by replay (caller should skip tree paint). */
int UID_PaintListTryReplay(uid_document_t *doc);

void UID_PaintListBeginRecord(uid_document_t *doc);
void UID_PaintListEndRecord(uid_document_t *doc);


#ifdef __cplusplus
}
#endif

#endif /* UID_PAINT_H */
