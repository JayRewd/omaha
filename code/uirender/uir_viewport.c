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

#include "uir_viewport.h"

#include <math.h>

uir_status_t UIR_ViewportMake(int vpX, int vpY, int vpW, int vpH, uir_viewport_t *out)
{
	return UIR_ViewportMakeOrtho(vpX, vpY, vpW, vpH, 0.0f, (float)vpW, 0.0f, (float)vpH, out);
}

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
)
{
	float drawW;
	float drawH;

	if (!out || vpW <= 0 || vpH <= 0) {
		return UIR_ERR_INVALID_ARG;
	}

	drawW = orthoR - orthoL;
	drawH = orthoB - orthoT;
	if (!(fabsf(drawW) > 1e-8f) || !(fabsf(drawH) > 1e-8f)) {
		return UIR_ERR_INVALID_ARG;
	}

	out->vpX = vpX;
	out->vpY = vpY;
	out->vpW = vpW;
	out->vpH = vpH;
	out->orthoL = orthoL;
	out->orthoR = orthoR;
	out->orthoT = orthoT;
	out->orthoB = orthoB;
	out->scaleX = (float)vpW / drawW;
	out->scaleY = (float)vpH / drawH;
	out->invX = 1.0f / out->scaleX;
	out->invY = 1.0f / out->scaleY;
	return UIR_OK;
}

void UIR_ViewportDrawToFb(const uir_viewport_t *vp, float dx, float dy, float *sx, float *sy)
{
	if (!vp) {
		return;
	}
	if (sx) {
		*sx = (float)vp->vpX + (dx - vp->orthoL) * vp->scaleX;
	}
	if (sy) {
		*sy = (float)vp->vpY + (dy - vp->orthoT) * vp->scaleY;
	}
}

void UIR_ViewportFbToDraw(const uir_viewport_t *vp, float sx, float sy, float *dx, float *dy)
{
	if (!vp) {
		return;
	}
	if (dx) {
		*dx = vp->orthoL + (sx - (float)vp->vpX) * vp->invX;
	}
	if (dy) {
		*dy = vp->orthoT + (sy - (float)vp->vpY) * vp->invY;
	}
}

void UIR_ViewportSnapQuad(const uir_viewport_t *vp, float *x, float *y, float *w, float *h)
{
	float sx0, sy0, sx1, sy1;
	float dx0, dy0, dx1, dy1;

	if (!vp || !x || !y || !w || !h) {
		return;
	}

	UIR_ViewportDrawToFb(vp, *x, *y, &sx0, &sy0);
	UIR_ViewportDrawToFb(vp, *x + *w, *y + *h, &sx1, &sy1);

	sx0 = floorf(sx0 + 0.5f);
	sy0 = floorf(sy0 + 0.5f);
	sx1 = floorf(sx1 + 0.5f);
	sy1 = floorf(sy1 + 0.5f);

	UIR_ViewportFbToDraw(vp, sx0, sy0, &dx0, &dy0);
	UIR_ViewportFbToDraw(vp, sx1, sy1, &dx1, &dy1);

	*x = dx0;
	*y = dy0;
	*w = dx1 - dx0;
	*h = dy1 - dy0;
}

/*
===============
UIR_RefPxScale

Added in OPM: uniform contain scale vs 1920x1080 design reference.
===============
*/
float UIR_RefPxScale(int logicalW, int logicalH)
{
	const float refW = 1920.0f;
	const float refH = 1080.0f;
	float       sx;
	float       sy;

	if (logicalW <= 0 || logicalH <= 0) {
		return 1.0f;
	}

	sx = (float)logicalW / refW;
	sy = (float)logicalH / refH;
	return (sx < sy) ? sx : sy;
}
