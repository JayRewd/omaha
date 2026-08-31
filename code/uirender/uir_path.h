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
#ifndef UIR_PATH_H
#define UIR_PATH_H

#include "uir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*uir_draw_box_fn)(float x, float y, float w, float h, const uir_color_t *color, void *userdata);

typedef struct {
	uir_draw_box_fn drawBox;
	void           *userdata;
	uir_stats_t    *stats; /* optional */
} uir_draw_sink_t;

void UIR_PathInit(uir_path_t *path);
void UIR_PathClear(uir_path_t *path);
void UIR_PathFree(uir_path_t *path);

uir_status_t UIR_PathBeginContour(uir_path_t *path, uir_contour_t **outContour);
uir_status_t UIR_ContourAddPoint(uir_contour_t *contour, float x, float y);
uir_status_t UIR_ContourClose(uir_contour_t *contour);

/*
 * Framebuffer-pixel coverage fill. Emits horizontal DrawBox runs through sink.
 * Single-contour wrapper for UIR_PathFill.
 */
uir_status_t UIR_FillPolygon(
	const uir_viewport_t *vp,
	const uir_point_t    *points,
	int                   count,
	const uir_color_t    *rgba,
	const uir_draw_sink_t *sink
);

/* crisp != 0: binary in/out (no soft AA) for pixel HUD marks such as crosshairs. */
uir_status_t UIR_PathFill(
	const uir_viewport_t *vp,
	const uir_path_t     *path,
	const uir_color_t    *rgba,
	const uir_draw_sink_t *sink,
	int                   crisp
);

/* Added in OPM: expand centerline to a filled strip (round caps/joins; closed = annulus). */
typedef struct uir_stroke_opts_s {
	uir_fill_rule_t fillRule;    /* default EVEN_ODD (annulus hole) */
	int             arcSteps;    /* 0 → 8 */
	int             closedStrip; /* 1 → single left/right strip (no outer+inner) */
	int             alignOutside; /* 1 → closed stroke lies outside fill (0..width), default */
	float           weldEps;     /* 0 off; else coalesce/snap threshold in px */
} uir_stroke_opts_t;

void UIR_StrokeOptsInit(uir_stroke_opts_t *opts);
uir_status_t UIR_BuildStrokePath(const uir_path_t *src, float widthPx, uir_path_t *out);
/* Added in OPM: single-contour outside band for GPU stroke tessellation. */
uir_status_t UIR_BuildOutsideStrokePath(const uir_path_t *src, float widthPx, uir_path_t *out);
uir_status_t UIR_BuildStrokePathOpts(
	const uir_path_t *src,
	float widthPx,
	const uir_stroke_opts_t *opts,
	uir_path_t *out
);
uir_status_t UIR_PathStroke(
	const uir_viewport_t *vp,
	const uir_path_t     *path,
	const uir_color_t    *rgba,
	float                 widthPx,
	const uir_draw_sink_t *sink,
	int                   crisp
);

/* Added in OPM: rotate path points around cx/cy; degrees clockwise in screen Y-down. */
uir_status_t UIR_PathRotate(const uir_path_t *src, float cx, float cy, float degrees, uir_path_t *out);

/* Axis-aligned bounding box of all path points. */
uir_status_t UIR_PathBounds(const uir_path_t *path, uir_rect_t *out);

/* Added in OPM: golden reference for GPU fill coverage tests. */
int UIR_PathContainsPoint(const uir_path_t *path, float x, float y);

#ifdef __cplusplus
}
#endif

#endif /* UIR_PATH_H */
