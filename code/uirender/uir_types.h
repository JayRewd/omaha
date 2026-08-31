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
#ifndef UIR_TYPES_H
#define UIR_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define UIR_MAX_CONTOUR_POINTS 4096
#define UIR_MAX_CONTOURS       64
#define UIR_MAX_POLYGON_AREA   (4096 * 2160)
#define UIR_COVERAGE_DROP      0.02f
#define UIR_COVERAGE_SOLID     0.98f

typedef enum {
	UIR_OK = 0,
	UIR_ERR_INVALID_ARG,
	UIR_ERR_EMPTY,
	UIR_ERR_OVERFLOW,
	UIR_ERR_PARSE,
	UIR_ERR_UNSUPPORTED,
	UIR_ERR_NOT_READY,
	UIR_ERR_MISSING_ASSET,
	UIR_ERR_WRONG_PHASE
} uir_status_t;

typedef enum {
	UIR_FILL_EVEN_ODD = 0,
	UIR_FILL_NON_ZERO
} uir_fill_rule_t;

typedef enum {
	UIR_FIT_CONTAIN = 0,
	UIR_FIT_STRETCH
} uir_fit_mode_t;

typedef enum {
	UIR_IMAGE_FIT_STRETCH = 0,
	UIR_IMAGE_FIT_REPEAT,
	UIR_IMAGE_FIT_CONTAIN,
	UIR_IMAGE_FIT_COVER
} uir_image_fit_t;

typedef enum {
	UIR_PHASE_IDLE = 0,
	UIR_PHASE_WORLD,
	UIR_PHASE_CHROME,
	UIR_PHASE_PREVIEWS,
	UIR_PHASE_OVERLAY
} uir_frame_phase_t;

typedef struct {
	float x;
	float y;
} uir_point_t;

typedef struct {
	float x;
	float y;
	float w;
	float h;
} uir_rect_t;

typedef struct {
	float r;
	float g;
	float b;
	float a;
} uir_color_t;

/*
 * Top-left origin draw space with a destination framebuffer viewport.
 * scaleX/scaleY map draw units -> FB pixels; invX/invY map FB pixels -> draw units.
 */
typedef struct {
	int   vpX;
	int   vpY;
	int   vpW;
	int   vpH;
	float orthoL;
	float orthoR;
	float orthoT;
	float orthoB;
	float scaleX;
	float scaleY;
	float invX;
	float invY;
} uir_viewport_t;

typedef struct {
	uir_point_t *points;
	int          count;
	int          capacity;
	int          closed;
} uir_contour_t;

typedef struct {
	uir_contour_t  contours[UIR_MAX_CONTOURS];
	int            contourCount;
	uir_fill_rule_t fillRule;
} uir_path_t;

typedef struct {
	float minX;
	float minY;
	float width;
	float height;
} uir_viewbox_t;

typedef struct {
	int sampledPixels;
	int supersamples;
	int emittedRuns;
	int rejectedOversized;
	int drawBoxes;
	int fontRebuilds;
	int previewCount;
	/* Added in OPM: GPU UI batch stats. */
	int batches;
	int batchVerts;
	int batchTris;
	int tessFallbacks;
	/* Added in OPM: last GPU tess failure signature for ui_render_stats. */
	int tessFallbackStatus;
	int tessFallbackContours;
	/* Added in OPM: libtess2 diagnostics. */
	int tessSkippedContours;
	int tessLibFails;
	int tessContoursIn;
	int tessContoursOut;
	/* Added in OPM: clip/scissor apply vs skip counters (Stage 0+). */
	int clipApplies;
	int clipSkips;
	/* Added in OPM: mapped path-cache hit/miss (Stage D). */
	int pathCacheHits;
	int pathCacheMisses;
	/* Added in OPM: tessellated mesh-cache hit/miss (Stage D). */
	int meshCacheHits;
	int meshCacheMisses;
} uir_stats_t;

#ifdef __cplusplus
}
#endif

#endif /* UIR_TYPES_H */
