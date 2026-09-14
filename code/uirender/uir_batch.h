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
#ifndef UIR_BATCH_H
#define UIR_BATCH_H

#include "uir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UIR_BATCH_MAX_VERTS   16384
#define UIR_BATCH_MAX_INDEXES 49152

typedef struct {
	float x;
	float y;
	float s;
	float t;
	unsigned char r;
	unsigned char g;
	unsigned char b;
	unsigned char a;
} uir_vert_t;

typedef struct {
	int  (*supported)(void);
	int  (*canBatchShader)(int shader);
	void (*draw)(const uir_vert_t *v, int nv, const unsigned short *idx, int ni, int shader);
	/* Added in Omaha Stage 5: hoist IssuePending/MSAA/client-state around multi-draw. */
	void (*beginDraw)(void);
	void (*endDraw)(void);
	int  (*targetAvailable)(void);
	int  (*targetSamples)(void);
	int  (*beginTarget)(void);
	void (*endTarget)(void);
	/* Added in Omaha: Phase 4.6 — retained target. Optional (NULL = no retention). */
	int  (*beginTargetKeep)(int keep);              /* 0 fail, 1 cleared, 2 kept */
	void (*clearRectFb)(int x, int y, int w, int h); /* GL window rect; w/h<=0 = whole target */
} uir_batch_backend_t;

void UIR_BatchSetBackend(const uir_batch_backend_t *backend);
void UIR_BatchSetEnabled(int enabled);
int  UIR_BatchEnabled(void);
/* Added in Omaha Stage 5: 64px translucent quad tiling (default off). */
void UIR_BatchSetTile(int enabled);
int  UIR_BatchTileEnabled(void);
void UIR_BatchSetFringe(int enabled);
int  UIR_BatchFringeEnabled(void);
void UIR_BatchBeginFrame(uir_stats_t *stats);
void UIR_BatchFlush(void);
/* Close an open UI2D draw session without submitting geometry. */
void UIR_BatchCloseDrawSession(void);
void UIR_BatchTargetBegin(void);
void UIR_BatchTargetEnd(void);

/*
 * Added in Omaha: Phase 4.6 — retained UI target.
 *
 * A retainable session is begun immediately (no deferral) so an all-clean
 * frame still composites the retained image. When keep!=0 and the backend
 * still holds last frame's retainable image, the clear is skipped and
 * UIR_BatchTargetRetained() reports 1: callers then clear + redraw only the
 * regions that changed (UIR_BatchClearRect) inside UIR_BatchRegionScope
 * brackets. Any geometry appended outside a region scope, any immediate/host
 * draw (UIR_BatchNoteExternalDraw) or a DropRetained call taints the frame so
 * the next session starts from a clear.
 */
void UIR_BatchSetRetain(int enabled);
int  UIR_BatchRetainEnabled(void);
int  UIR_BatchTargetBeginKeep(int keep); /* returns 1 when previous contents were kept */
int  UIR_BatchTargetRetained(void);
void UIR_BatchTargetDropRetained(void);  /* full clear now (if kept) + taint */
void UIR_BatchRegionScopeBegin(void);
void UIR_BatchRegionScopeEnd(void);
void UIR_BatchClearRect(float x, float y, float w, float h); /* draw-space rect */
void UIR_BatchNoteExternalDraw(void);
void UIR_BatchRetainClaim(void);         /* a document painted with retained regions this frame */
int  UIR_BatchRetainClaimed(void);
int  UIR_BatchFrameTainted(void);
void UIR_BatchResetTaint(void);

/*
 * Added in Omaha Stage 4: optional recorder for retained paint lists.
 * onDraw is invoked from flush (and oversized direct draws) with the geometry
 * about to be submitted. onClip is invoked when a scissor is actually applied.
 */
typedef struct uir_paint_recorder_s {
	void (*onDraw)(const uir_vert_t *v, int nv, const unsigned short *idx, int ni, int shader, void *userdata);
	void (*onClip)(float x, float y, float w, float h, void *userdata);
	void *userdata;
	/* Added in Omaha: Phase 4.3 — record live-opacity verts without a full-alpha submit. */
	int skipSubmit;
} uir_paint_recorder_t;

void UIR_BatchSetPaintRecorder(const uir_paint_recorder_t *recorder);
int  UIR_BatchSkipSubmitActive(void);

/* Added in Omaha Stage 4: compositor notifies recorder of applied clips. */
void UIR_BatchNotifyClip(float x, float y, float w, float h);

uir_status_t UIR_BatchQuad(
	int shader,
	float x,
	float y,
	float w,
	float h,
	float s0,
	float t0,
	float s1,
	float t1,
	const uir_color_t *rgba
);

uir_status_t UIR_BatchQuadSkewed(
	int shader,
	float x,
	float y,
	float w,
	float h,
	float s0,
	float t0,
	float s1,
	float t1,
	const uir_color_t *rgba,
	float skewTan,
	float originY
);

/* Added in Omaha: rotate axis-aligned quad around pivot (clockwise degrees). */
uir_status_t UIR_BatchQuadRotated(
	int shader,
	float x,
	float y,
	float w,
	float h,
	float s0,
	float t0,
	float s1,
	float t1,
	const uir_color_t *rgba,
	float rotationDeg,
	float pivotX,
	float pivotY
);

uir_status_t UIR_BatchTriangles(
	int shader,
	const uir_vert_t *v,
	int nv,
	const unsigned short *idx,
	int ni
);

/* Added in Omaha: Phase 4.2 — copy verts with a translate offset during append. */
uir_status_t UIR_BatchTrianglesOffset(
	int shader,
	const uir_vert_t *v,
	int nv,
	const unsigned short *idx,
	int ni,
	float dx,
	float dy
);

/* Added in Omaha: Phase 4.3 — copy verts scaling vertex alpha by mul. */
uir_status_t UIR_BatchTrianglesScaledAlpha(
	int shader,
	const uir_vert_t *v,
	int nv,
	const unsigned short *idx,
	int ni,
	float alphaMul
);

#ifdef __cplusplus
}
#endif

#endif /* UIR_BATCH_H */
