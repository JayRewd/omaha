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
#ifndef UIR_GRADIENT_H
#define UIR_GRADIENT_H

#include "uir_types.h"
#include "uir_image.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UIR_GRADIENT_MAX_STOPS 8

typedef enum {
	UIR_GRADIENT_LINEAR = 0,
	UIR_GRADIENT_RADIAL
} uir_gradient_kind_t;

typedef struct {
	uir_color_t color;
	float       offset; /* 0..1 */
} uir_gradient_stop_t;

typedef struct {
	uir_gradient_kind_t kind;
	float               angleDeg; /* linear: CSS-like (0=to top, 90=to right) */
	float               centerX;  /* radial: 0..1 */
	float               centerY;
	int                 stopCount;
	uir_gradient_stop_t stops[UIR_GRADIENT_MAX_STOPS];
} uir_gradient_t;

/*
 * Atlas upload hooks (same CreateUIAtlas / UpdateUIAtlas as fonts).
 * Draw hooks reuse the image backend after UIR_ImageSetBackend.
 */
typedef struct {
	int (*createAtlas)(const char *name, const unsigned char *rgba, int width, int height);
	int (*updateAtlas)(int h, const unsigned char *rgba, int width, int height);
} uir_gradient_backend_t;

void UIR_GradientSetBackend(const uir_gradient_backend_t *backend);
/* Called from UIR_ImageSetBackend so stretch/triangle draw hooks stay in sync. */
void UIR_GradientSyncImageBackend(const uir_image_backend_t *backend);
void UIR_GradientShutdown(void);
void UIR_GradientInvalidateGpu(void);

/* True if text looks like linear(...) or radial(...) (leading whitespace ok). */
int UIR_GradientIsBrush(const char *text);

/* Parse brush string into out. Returns UIR_OK or UIR_ERR_PARSE / INVALID_ARG. */
uir_status_t UIR_GradientParse(const char *text, uir_gradient_t *out);

/*
 * CPU rasterize into caller buffer (row-major RGBA8, size w*h*4).
 * Used by tests and the atlas bake path.
 */
uir_status_t UIR_GradientRasterize(const uir_gradient_t *grad, int w, int h, unsigned char *rgba);

/*
 * Bake (or reuse cached) atlas for brush at dest size.
 * outShader is a CreateUIAtlas handle usable with stretch/mask draws.
 */
uir_status_t UIR_GradientEnsureShader(
	const char *brush,
	float destW,
	float destH,
	int *outShader,
	int *outBakeW,
	int *outBakeH
);

/*
 * Bake (or reuse cached) atlas and stretch-draw into dest, optionally clipped
 * to SVG path contours. tintRgba NULL = opaque white modulate.
 */
uir_status_t UIR_GradientDrawClipped(
	const char *brush,
	float x,
	float y,
	float w,
	float h,
	const char *const *clipPathD,
	int clipPathCount,
	float viewW,
	float viewH,
	float rotationDeg,
	const uir_color_t *tintRgba
);

#ifdef __cplusplus
}
#endif

#endif /* UIR_GRADIENT_H */
