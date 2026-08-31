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
#ifndef UIR_IMAGE_H
#define UIR_IMAGE_H

#include "uir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int (*registerShaderNoMip)(const char *path);
	void (*getShaderSize)(int shader, int *width, int *height);
	void (*setColor)(const float *rgba);
	void (*drawStretchPic)(float x, float y, float w, float h, float s1, float t1, float s2, float t2, int shader);
	void (*drawTilePic)(float x, float y, float w, float h, int shader);
	void (*drawTrianglePic)(const float points[3][2], const float texCoords[3][2], int shader);
} uir_image_backend_t;

void UIR_ImageSetBackend(const uir_image_backend_t *backend);
void UIR_ImageShutdown(void);
void UIR_ImageInvalidateGpu(void);

typedef struct uir_image_s uir_image_t;

uir_image_t *UIR_ImageResolve(const char *vfsPath);
void UIR_ImageRelease(uir_image_t *image);

int UIR_ImageShader(const uir_image_t *image);
float UIR_ImageWidth(const uir_image_t *image);
float UIR_ImageHeight(const uir_image_t *image);

/*
 * Compute destination rect and UVs for image fit modes.
 * Box is draw-space (x,y,w,h). UVs are normalized 0..1 in source texture space.
 */
void UIR_ComputeImageRect(
	float imgW,
	float imgH,
	float boxX,
	float boxY,
	float boxW,
	float boxH,
	uir_image_fit_t fit,
	float *outX,
	float *outY,
	float *outW,
	float *outH,
	float *s1,
	float *t1,
	float *s2,
	float *t2
);

/*
 * Draw a registered image into dest, optionally clipped to SVG path contours.
 * clipPathD may be NULL when clipPathCount is 0 (axis-aligned scissor only).
 */
uir_status_t UIR_ImageDrawClipped(
	const char *vfsPath,
	float x,
	float y,
	float w,
	float h,
	const char *const *clipPathD,
	int clipPathCount,
	float viewW,
	float viewH,
	uir_image_fit_t fit,
	float rotationDeg,
	float dipScale,
	float backgroundScale,
	const uir_color_t *tintRgba
);

#ifdef __cplusplus
}
#endif

#endif /* UIR_IMAGE_H */
