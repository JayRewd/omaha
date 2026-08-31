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
#ifndef UIR_DRAW2D_H
#define UIR_DRAW2D_H

#include "uir_types.h"
#include "uir_path.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	void (*setColor)(const float *rgba);
	void (*drawBox)(float x, float y, float w, float h);
	void (*set2DWindow)(int x, int y, int w, int h, float left, float right, float bottom, float top, float n, float f);
	void (*scissor)(int x, int y, int w, int h);
} uir_draw2d_backend_t;

void UIR_Draw2D_SetBackend(const uir_draw2d_backend_t *backend);
void UIR_Draw2D_Begin(const uir_viewport_t *vp);
void UIR_Draw2D_Scissor(int x, int y, int w, int h);
uir_status_t UIR_Draw2D_Box(float x, float y, float w, float h, const uir_color_t *rgba);
uir_status_t UIR_Draw2D_Polygon(
	const uir_viewport_t *vp,
	const uir_point_t *pts,
	int count,
	const uir_color_t *rgba,
	uir_stats_t *stats
);
uir_status_t UIR_Draw2D_Path(
	const uir_viewport_t *vp,
	const uir_path_t *path,
	const uir_color_t *rgba,
	uir_stats_t *stats,
	int crisp,
	int noFringe
);

/* Added in OPM */
uir_status_t UIR_Draw2D_PathStroke(
	const uir_viewport_t *vp,
	const uir_path_t *path,
	const uir_color_t *rgba,
	float widthPx,
	uir_stats_t *stats,
	int crisp
);

#ifdef __cplusplus
}
#endif

#endif /* UIR_DRAW2D_H */
