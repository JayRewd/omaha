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
#ifndef UIR_FONT_H
#define UIR_FONT_H

#include "uir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uir_font_s uir_font_t;

typedef struct {
	/* Provide RGBA upload without linking the full client when unit-testing. */
	int (*createAtlas)(const char *name, const unsigned char *rgba, int width, int height);
	int (*updateAtlas)(int h, const unsigned char *rgba, int width, int height);
	void (*setColor)(const float *rgba);
	void (*drawPic)(float x, float y, float w, float h, float s1, float t1, float s2, float t2, int shader);
	/* Added in Omaha: non-axis-aligned glyph fallback (same contract as image backend). */
	void (*drawTrianglePic)(const float points[3][2], const float texCoords[3][2], int shader);
	long (*readFile)(const char *path, void **buffer);
	void (*freeFile)(void *buffer);
	void *(*allocMem)(size_t size);
	void (*freeMem)(void *ptr);
} uir_font_backend_t;

void UIR_FontSetBackend(const uir_font_backend_t *backend);
void UIR_FontShutdown(void);
void UIR_FontInvalidateGpu(void);

/* Legacy 1:1 load (framebuffer pixels == logical). Prefer UIR_FontResolve. */
uir_font_t *UIR_FontLoad(const char *vfsPath, float pixelHeight);

/*
 * Named static-TTF registry keyed by (path, logicalPx, framebufferScale).
 * Glyphs bake at logicalPx * fbScale; returned metrics are in logical units.
 */
uir_font_t *UIR_FontResolve(const char *vfsPath, float logicalPx, float fbScale);
void UIR_FontRelease(uir_font_t *font);

/* Added in OPM: registry pressure while ui_scale / DIP changes thrash bakes. */
int UIR_FontRegistryCount(void);
int UIR_FontRegistryCapacity(void);

float UIR_FontAscent(const uir_font_t *font);
float UIR_FontLineHeight(const uir_font_t *font);
float UIR_FontMeasure(const uir_font_t *font, const char *text, float tracking);

uir_status_t UIR_FontDraw(
	const uir_viewport_t *vp,
	uir_font_t           *font,
	float                 x,
	float                 y,
	const char           *text,
	const uir_color_t    *rgba,
	float                 tracking
);

/*
 * CSS skewX-style glyph shear: x' = x + (y - originY) * skewTan.
 * Implemented as 1px atlas strips so drawPic stays axis-aligned.
 */
uir_status_t UIR_FontDrawSkewed(
	const uir_viewport_t *vp,
	uir_font_t           *font,
	float                 x,
	float                 y,
	const char           *text,
	const uir_color_t    *rgba,
	float                 tracking,
	float                 skewTan,
	float                 originY
);

/*
 * Added in Omaha: rotate each glyph quad around a shared pivot (clockwise degrees).
 * Layout uses the unrotated baseline; when rotationDeg ~= 0, delegates to UIR_FontDraw.
 */
uir_status_t UIR_FontDrawRotated(
	const uir_viewport_t *vp,
	uir_font_t           *font,
	float                 x,
	float                 y,
	const char           *text,
	const uir_color_t    *rgba,
	float                 tracking,
	float                 rotationDeg,
	float                 pivotX,
	float                 pivotY
);

void UIR_FontInsetUVs(float *u0, float *v0, float *u1, float *v1, int atlasW, int atlasH);
float UIR_FontEmScale(float pixelHeight, int unitsPerEm);

#ifdef __cplusplus
}
#endif

#endif /* UIR_FONT_H */
