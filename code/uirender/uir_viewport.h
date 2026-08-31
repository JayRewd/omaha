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
#ifndef UIR_VIEWPORT_H
#define UIR_VIEWPORT_H

#include "uir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Build a top-left-origin draw viewport covering the given framebuffer rect.
 * Draw space matches FB pixels 1:1 by default (orthoL=0, orthoR=w, orthoT=0, orthoB=h).
 */
uir_status_t UIR_ViewportMake(int vpX, int vpY, int vpW, int vpH, uir_viewport_t *out);

/* Custom ortho bounds over the same framebuffer rect. */
uir_status_t UIR_ViewportMakeOrtho(
	int vpX,
	int vpY,
	int vpW,
	int vpH,
	float orthoL,
	float orthoR,
	float orthoT,
	float orthoB,
	uir_viewport_t *out
);

void UIR_ViewportDrawToFb(const uir_viewport_t *vp, float dx, float dy, float *sx, float *sy);
void UIR_ViewportFbToDraw(const uir_viewport_t *vp, float sx, float sy, float *dx, float *dy);

/* Snap a draw-space quad origin/size onto the FB pixel grid. */
void UIR_ViewportSnapQuad(
	const uir_viewport_t *vp,
	float *x,
	float *y,
	float *w,
	float *h
);

/*
 * Added in OPM: reference-resolution scale for authored UI px.
 * min(logicalW/1920, logicalH/1080); returns 1.0 if W/H <= 0.
 */
float UIR_RefPxScale(int logicalW, int logicalH);

#ifdef __cplusplus
}
#endif

#endif /* UIR_VIEWPORT_H */
