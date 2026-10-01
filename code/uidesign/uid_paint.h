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
/* Added in Omaha: Phase 3 — replay clip dedup / EndRecord CLIP collapse. */
void UID_SetReplayClipDedup(int enabled);
/* Added in Omaha: Phase 4.1 — per-region retained paint chunks. */
void UID_SetPaintRegions(int enabled);
int  UID_PaintRegionsEnabled(void);
/* Added in Omaha: Phase 4.2 — cached geometry for bound translate subtrees. */
void UID_SetLiveTranslateCache(int enabled);
int  UID_LiveTranslateCacheEnabled(void);
int  UID_PaintLiveCaptureNoCull(void);
int  UID_PaintListBeginLiveCapture(
	uid_node_id_t nodeId,
	float originX,
	float originY,
	float clipX,
	float clipY,
	float clipW,
	float clipH
);
/* Added in Omaha: Phase 4.3 — foreach lifetime fade cache. */
void UID_SetLiveOpacityCache(int enabled);
int  UID_LiveOpacityCacheEnabled(void);
int  UID_PaintLiveOpacityRowsCached(const uid_document_t *doc, uid_node_id_t foreachId);
int  UID_PaintListBeginLiveOpacityCapture(
	uid_document_t *doc,
	uid_node_id_t nodeId,
	float originX,
	float originY,
	float clipX,
	float clipY,
	float clipW,
	float clipH
);
void UID_PaintListEndLiveCapture(void);
void UID_PaintListRecordLiveOpacity(uid_node_id_t nodeId);
void UID_RebuildPaintRegions(uid_document_t *doc);
void UID_PaintRegionsInvalidateAll(uid_document_t *doc);
void UID_PaintRegionsInvalidateNode(uid_document_t *doc, uid_node_id_t nodeId);
int  UID_PaintRegionsPrepare(uid_document_t *doc);
int  UID_PaintListFlag(void);
int  UID_PaintRegionsFlag(void);
int  UID_PaintRegionsRawUsable(const uid_document_t *doc);
int  UID_PaintRegionsUsable(const uid_document_t *doc);
int  UID_PaintRegionsCount(const uid_document_t *doc);
uid_node_id_t UID_PaintChromeRootId(const uid_document_t *doc);
uid_node_id_t UID_PaintRegionRootId(const uid_document_t *doc, int region);
int  UID_PaintRegionCanReplay(const uid_document_t *doc, int region);
int  UID_PaintRegionReplay(uid_document_t *doc, int region, const uid_backend_t *backend);
void UID_PaintRegionBeginRecord(uid_document_t *doc, int region);
void UID_PaintRegionEndRecord(uid_document_t *doc, int region);
void UID_PaintRegionMarkInvalid(uid_document_t *doc, int region);

/*
 * Added in Omaha: Phase 4.6 — retained UI target / partial redraw.
 * RetainPlan (root clip in draw space) returns 1 when only dirty regions need
 * painting; RetainSkip says whether a region is clean this frame; NoteDrawn
 * records the box a painted region now owns; RetainEnd finishes the frame.
 * RetainDrop forces a cleared target (root hidden / non-region paint path).
 */
int  UID_PaintRegionsRetainPlan(
	uid_document_t *doc,
	const uid_backend_t *backend,
	float clipX,
	float clipY,
	float clipW,
	float clipH
);
int  UID_PaintRegionRetainSkip(const uid_document_t *doc, int region);
int  UID_PaintRegionRetainLiveOnly(const uid_document_t *doc, int region);
int  UID_PaintRegionReplayLives(uid_document_t *doc, int region, const uid_backend_t *backend);
void UID_PaintRegionNoteDrawn(uid_document_t *doc, int region);
void UID_PaintRegionsRetainEnd(uid_document_t *doc);
void UID_PaintRegionsRetainDrop(uid_document_t *doc);

void UID_PaintListInvalidate(uid_document_t *doc);
void UID_PaintListMarkHostDraw(void); /* model/host — list cannot replay */
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

/* Added in Omaha: record soft mask-image layer begin/end so retained list can replay. */
void UID_PaintListRecordImageMaskBegin(
	float x,
	float y,
	float w,
	float h,
	const char *maskSpec,
	int fit
);
void UID_PaintListRecordImageMaskEnd(void);

/* Returns 1 if chrome was fully handled by replay (caller should skip tree paint). */
int UID_PaintListTryReplay(uid_document_t *doc, const uid_backend_t *backend);

void UID_PaintListBeginRecord(uid_document_t *doc);
void UID_PaintListEndRecord(uid_document_t *doc);

/*
 * Added in Omaha: bound translate-x/y nodes are recorded as live subtrees so the
 * retained list stays valid while any HUD element scrolls/offsets via translate.
 */
int  UID_PaintListIsActivelyRecording(void);
void UID_PaintListRecordLiveSubtree(uid_node_id_t nodeId);
void UID_PaintListPauseRecord(void);
void UID_PaintListResumeRecord(void);

#ifdef __cplusplus
}
#endif

#endif /* UID_PAINT_H */
