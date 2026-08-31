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
#ifndef UIR_STENCIL_H
#define UIR_STENCIL_H

#include "uir_types.h"
#include "uir_path.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int (*available)(void);
	void (*beginMask)(int scissorX, int scissorY, int scissorW, int scissorH);
	void (*maskBox)(float x, float y, float w, float h);
	void (*beginDraw)(void);
	void (*end)(void);
} uir_stencil_backend_t;

void UIR_StencilSetBackend(const uir_stencil_backend_t *backend);
int UIR_StencilAvailable(void);

uir_status_t UIR_BeginShapeClip(const uir_viewport_t *vp, const uir_rect_t *bounds);
uir_status_t UIR_StencilWritePath(const uir_viewport_t *vp, const uir_path_t *path);
uir_status_t UIR_BeginShapeClipDraw(void);
void UIR_EndShapeClip(void);

/*
 * Added in OPM: begin clipping subsequent draws to SVG path(s) mapped into dest.
 * Uses stencil when available; otherwise axis-aligned scissor of dest (or path AABB).
 * Nested calls fail with UIR_ERR_WRONG_PHASE — caller should skip.
 * Pair with UIR_EndShapeClip.
 */
#define UIR_SHAPE_CLIP_MAX_PATHS 8
uir_status_t UIR_BeginSvgShapeClip(
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

#ifdef __cplusplus
}
#endif

#endif /* UIR_STENCIL_H */
