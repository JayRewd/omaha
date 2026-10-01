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

#include "uir_font.h"
#include "uir_batch.h"
#include "uir_debug.h"
#include "uir_viewport.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uir_font_backend_t g_fontBackend;

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(x, u) ((void)(u), g_fontBackend.allocMem ? g_fontBackend.allocMem(x) : malloc(x))
#define STBTT_free(x, u) ((void)(u), g_fontBackend.freeMem ? g_fontBackend.freeMem(x) : free(x))
#include "../thirdparty/stb/stb_truetype.h"

#define UIR_FONT_ATLAS_SIZE 1024
#define UIR_FONT_FIRST_CHAR 32
#define UIR_FONT_NUM_CHARS  95 /* printable ASCII */

#define UIR_FONT_REGISTRY_MAX 64
/* Quantize logical height so ui_scale slider drags reuse atlases. */
#define UIR_FONT_LOGICAL_QUANT 0.5f

struct uir_font_s {
	char            path[256];
	float           logicalHeight; /* layout units */
	float           fbScale;       /* bakeScale = logicalHeight * fbScale */
	float           pixelHeight;   /* bake size in FB pixels */
	float           scale;         /* stb em scale at bake size */
	float           toLogical;     /* 1/fbScale for metric conversion */
	float           ascent;        /* logical */
	float           descent;       /* logical */
	float           lineGap;       /* logical */
	int             atlasW;
	int             atlasH;
	int             shader;
	int             gpuGeneration;
	unsigned char  *atlasRgba;
	stbtt_bakedchar baked[UIR_FONT_NUM_CHARS]; /* advances stored in logical units */
	unsigned char  *ttfData;
	int             ttfSize;
};

static int                g_gpuGeneration = 1;
static uir_font_t        *g_fonts[UIR_FONT_REGISTRY_MAX];
static int                g_fontCount = 0;

/*
===============
UIR_FontQuantizeLogical

Snap layout px so neighboring ui_scale steps share one bake.
===============
*/
static float UIR_FontQuantizeLogical(float logicalPx)
{
	if (!(logicalPx > 0.0f) || logicalPx != logicalPx) {
		return UIR_FONT_LOGICAL_QUANT;
	}
	return floorf(logicalPx / UIR_FONT_LOGICAL_QUANT + 0.5f) * UIR_FONT_LOGICAL_QUANT;
}

/*
===============
UIR_FontTouch

Move registry slot to MRU (end). Index 0 is LRU for eviction.
===============
*/
static void UIR_FontTouch(int index)
{
	uir_font_t *font;
	int         i;

	if (index < 0 || index >= g_fontCount - 1) {
		return;
	}
	font = g_fonts[index];
	for (i = index; i < g_fontCount - 1; i++) {
		g_fonts[i] = g_fonts[i + 1];
	}
	g_fonts[g_fontCount - 1] = font;
}

int UIR_FontRegistryCount(void)
{
	return g_fontCount;
}

int UIR_FontRegistryCapacity(void)
{
	return UIR_FONT_REGISTRY_MAX;
}

void UIR_FontInsetUVs(float *u0, float *v0, float *u1, float *v1, int atlasW, int atlasH)
{
	float iu = 0.5f / (float)atlasW;
	float iv = 0.5f / (float)atlasH;
	if (u0) {
		*u0 += iu;
	}
	if (v0) {
		*v0 += iv;
	}
	if (u1) {
		*u1 -= iu;
	}
	if (v1) {
		*v1 -= iv;
	}
}

float UIR_FontEmScale(float pixelHeight, int unitsPerEm)
{
	if (unitsPerEm <= 0 || !(pixelHeight > 0.0f)) {
		return 0.0f;
	}
	return pixelHeight / (float)unitsPerEm;
}

void UIR_FontSetBackend(const uir_font_backend_t *backend)
{
	if (backend) {
		g_fontBackend = *backend;
	} else {
		memset(&g_fontBackend, 0, sizeof(g_fontBackend));
	}
}

void UIR_FontShutdown(void)
{
	/* Fixed in OPM: release from end so swap-shrink in UIR_FontRelease cannot skip entries. */
	while (g_fontCount > 0) {
		UIR_FontRelease(g_fonts[g_fontCount - 1]);
	}
}

void UIR_FontInvalidateGpu(void)
{
	int i;

	g_gpuGeneration++;
	/* Fixed in OPM: drop stale GPU handles so re-upload always CreateUIAtlas. */
	for (i = 0; i < g_fontCount; i++) {
		if (g_fonts[i]) {
			g_fonts[i]->shader = 0;
			g_fonts[i]->gpuGeneration = 0;
		}
	}
}

static int uir_font_upload(uir_font_t *font)
{
	char name[128];

	if (!g_fontBackend.createAtlas || !font || !font->atlasRgba) {
		return 0;
	}
	snprintf(name, sizeof(name), "*uir_font_%p", (void *)font);
	/* Fixed in OPM: Update only when handle is live for the current GPU generation. */
	if (font->shader != 0 && font->gpuGeneration == g_gpuGeneration && g_fontBackend.updateAtlas) {
		if (g_fontBackend.updateAtlas(font->shader, font->atlasRgba, font->atlasW, font->atlasH)) {
			font->gpuGeneration = g_gpuGeneration;
			return 1;
		}
	}
	font->shader = g_fontBackend.createAtlas(name, font->atlasRgba, font->atlasW, font->atlasH);
	font->gpuGeneration = g_gpuGeneration;
	return font->shader != 0;
}

static int uir_font_bake(uir_font_t *font)
{
	unsigned char *bitmap;
	int i;
	stbtt_fontinfo info;
	int ascent, descent, lineGap;

	if (!font || !font->ttfData) {
		return 0;
	}

	if (!stbtt_InitFont(&info, font->ttfData, stbtt_GetFontOffsetForIndex(font->ttfData, 0))) {
		return 0;
	}

	font->scale = stbtt_ScaleForMappingEmToPixels(&info, font->pixelHeight);
	stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
	font->toLogical = (font->fbScale > 1e-6f) ? (1.0f / font->fbScale) : 1.0f;
	font->ascent = (float)ascent * font->scale * font->toLogical;
	font->descent = (float)descent * font->scale * font->toLogical;
	font->lineGap = (float)lineGap * font->scale * font->toLogical;
	font->atlasW = UIR_FONT_ATLAS_SIZE;
	font->atlasH = UIR_FONT_ATLAS_SIZE;

	bitmap = (unsigned char *)(g_fontBackend.allocMem ? g_fontBackend.allocMem((size_t)font->atlasW * font->atlasH)
													 : malloc((size_t)font->atlasW * font->atlasH));
	if (!bitmap) {
		return 0;
	}
	memset(bitmap, 0, (size_t)font->atlasW * font->atlasH);

	/* BakeFontBitmap uses ScaleForPixelHeight; adjust so raster matches em-square. */
	{
		float phScale = stbtt_ScaleForPixelHeight(&info, font->pixelHeight);
		float bakeSize = font->pixelHeight;
		if (phScale > 1e-8f) {
			bakeSize = font->pixelHeight * (font->scale / phScale);
		}
		if (stbtt_BakeFontBitmap(
				font->ttfData,
				0,
				bakeSize,
				bitmap,
				font->atlasW,
				font->atlasH,
				UIR_FONT_FIRST_CHAR,
				UIR_FONT_NUM_CHARS,
				font->baked
			) <= 0) {
			if (g_fontBackend.freeMem) {
				g_fontBackend.freeMem(bitmap);
			} else {
				free(bitmap);
			}
			return 0;
		}
	}

	for (i = 0; i < UIR_FONT_NUM_CHARS; i++) {
		int adv, lsb;
		stbtt_GetCodepointHMetrics(&info, UIR_FONT_FIRST_CHAR + i, &adv, &lsb);
		/* Bake bitmap metrics stay in FB pixels; convert at measure/draw time. */
		font->baked[i].xadvance = (float)adv * font->scale;
	}

	if (font->atlasRgba) {
		if (g_fontBackend.freeMem) {
			g_fontBackend.freeMem(font->atlasRgba);
		} else {
			free(font->atlasRgba);
		}
	}
	font->atlasRgba = (unsigned char *)(g_fontBackend.allocMem
											? g_fontBackend.allocMem((size_t)font->atlasW * font->atlasH * 4)
											: malloc((size_t)font->atlasW * font->atlasH * 4));
	if (!font->atlasRgba) {
		if (g_fontBackend.freeMem) {
			g_fontBackend.freeMem(bitmap);
		} else {
			free(bitmap);
		}
		return 0;
	}

	for (i = 0; i < font->atlasW * font->atlasH; i++) {
		font->atlasRgba[i * 4 + 0] = 255;
		font->atlasRgba[i * 4 + 1] = 255;
		font->atlasRgba[i * 4 + 2] = 255;
		font->atlasRgba[i * 4 + 3] = bitmap[i];
	}

	if (g_fontBackend.freeMem) {
		g_fontBackend.freeMem(bitmap);
	} else {
		free(bitmap);
	}

	font->shader = 0;
	return uir_font_upload(font);
}

uir_font_t *UIR_FontResolve(const char *vfsPath, float logicalPx, float fbScale)
{
	uir_font_t *font;
	void       *buf = NULL;
	long        len;
	int         i;
	float       bakePx;

	if (!vfsPath || !(logicalPx > 0.0f) || !g_fontBackend.readFile) {
		return NULL;
	}
	if (!(fbScale > 0.0f)) {
		fbScale = 1.0f;
	}
	/* Changed in OPM: quantize so ui_scale drag does not create a unique atlas per step. */
	logicalPx = UIR_FontQuantizeLogical(logicalPx);
	bakePx = logicalPx * fbScale;
	/* Fixed in OPM: clamp bake size so atlas pack cannot overflow / thrash. */
	if (bakePx < 4.0f) {
		bakePx = 4.0f;
	} else if (bakePx > 128.0f) {
		bakePx = 128.0f;
	}

	for (i = 0; i < g_fontCount; i++) {
		if (g_fonts[i] && !strcmp(g_fonts[i]->path, vfsPath)
			&& fabsf(g_fonts[i]->logicalHeight - logicalPx) < 0.01f
			&& fabsf(g_fonts[i]->fbScale - fbScale) < 0.001f) {
			UIR_FontTouch(i);
			return g_fonts[g_fontCount - 1];
		}
	}

	len = g_fontBackend.readFile(vfsPath, &buf);
	if (len <= 0 || !buf) {
		return NULL;
	}

	font = (uir_font_t *)(g_fontBackend.allocMem ? g_fontBackend.allocMem(sizeof(*font)) : malloc(sizeof(*font)));
	if (!font) {
		g_fontBackend.freeFile(buf);
		return NULL;
	}
	memset(font, 0, sizeof(*font));
	snprintf(font->path, sizeof(font->path), "%s", vfsPath);
	font->logicalHeight = logicalPx;
	font->fbScale = fbScale;
	font->pixelHeight = bakePx;
	font->ttfSize = (int)len;
	font->ttfData = (unsigned char *)(g_fontBackend.allocMem ? g_fontBackend.allocMem((size_t)len) : malloc((size_t)len));
	if (!font->ttfData) {
		g_fontBackend.freeFile(buf);
		if (g_fontBackend.freeMem) {
			g_fontBackend.freeMem(font);
		} else {
			free(font);
		}
		return NULL;
	}
	memcpy(font->ttfData, buf, (size_t)len);
	g_fontBackend.freeFile(buf);

	if (!uir_font_bake(font)) {
		UIR_FontRelease(font);
		return NULL;
	}

	/* Fixed in OPM: always track fonts; evict LRU when full (was leaking atlases). */
	if (g_fontCount >= UIR_FONT_REGISTRY_MAX) {
		if (UIR_DebugEnabled()) {
			fprintf(
				stderr,
				"UIR font: evict LRU '%s' logical=%.2f (registry full %d)\n",
				g_fonts[0] ? g_fonts[0]->path : "?",
				g_fonts[0] ? g_fonts[0]->logicalHeight : 0.0f,
				UIR_FONT_REGISTRY_MAX
			);
		}
		UIR_FontRelease(g_fonts[0]);
	}
	if (g_fontCount < UIR_FONT_REGISTRY_MAX) {
		g_fonts[g_fontCount++] = font;
	} else {
		/* Should be unreachable after eviction; never return an untracked atlas. */
		UIR_FontRelease(font);
		return NULL;
	}

	if (UIR_DebugEnabled()) {
		fprintf(
			stderr,
			"UIR font: bake '%s' logical=%.2f bakePx=%.1f registry=%d/%d\n",
			vfsPath,
			logicalPx,
			bakePx,
			g_fontCount,
			UIR_FONT_REGISTRY_MAX
		);
	}
	return font;
}

uir_font_t *UIR_FontLoad(const char *vfsPath, float pixelHeight)
{
	return UIR_FontResolve(vfsPath, pixelHeight, 1.0f);
}

void UIR_FontRelease(uir_font_t *font)
{
	int i;
	if (!font) {
		return;
	}
	for (i = 0; i < g_fontCount; i++) {
		if (g_fonts[i] == font) {
			g_fonts[i] = g_fonts[--g_fontCount];
			break;
		}
	}
	if (font->atlasRgba) {
		if (g_fontBackend.freeMem) {
			g_fontBackend.freeMem(font->atlasRgba);
		} else {
			free(font->atlasRgba);
		}
	}
	if (font->ttfData) {
		if (g_fontBackend.freeMem) {
			g_fontBackend.freeMem(font->ttfData);
		} else {
			free(font->ttfData);
		}
	}
	if (g_fontBackend.freeMem) {
		g_fontBackend.freeMem(font);
	} else {
		free(font);
	}
}

float UIR_FontAscent(const uir_font_t *font)
{
	return font ? font->ascent : 0.0f;
}

/* Full typographic line box: ascent - descent + lineGap (descent is ≤ 0 in stbtt). */
float UIR_FontLineHeight(const uir_font_t *font)
{
	if (!font) {
		return 0.0f;
	}
	return font->ascent - font->descent + font->lineGap;
}

float UIR_FontMeasure(const uir_font_t *font, const char *text, float tracking)
{
	float w = 0.0f;
	float inv;
	if (!font || !text) {
		return 0.0f;
	}
	inv = font->toLogical > 0.0f ? font->toLogical : 1.0f;
	while (*text) {
		unsigned char ch = (unsigned char)*text++;
		if (ch < UIR_FONT_FIRST_CHAR || ch >= UIR_FONT_FIRST_CHAR + UIR_FONT_NUM_CHARS) {
			ch = (unsigned char)'?';
		}
		w += font->baked[ch - UIR_FONT_FIRST_CHAR].xadvance * inv;
		if (*text) {
			w += tracking;
		}
	}
	return w;
}

uir_status_t UIR_FontDraw(
	const uir_viewport_t *vp,
	uir_font_t           *font,
	float                 x,
	float                 y,
	const char           *text,
	const uir_color_t    *rgba,
	float                 tracking
)
{
	float penX;
	float baseline;

	if (!vp || !font || !text || !rgba || !g_fontBackend.drawPic || !g_fontBackend.setColor) {
		return UIR_ERR_INVALID_ARG;
	}

	if (font->gpuGeneration != g_gpuGeneration) {
		if (!uir_font_upload(font)) {
			return UIR_ERR_NOT_READY;
		}
	}

	penX = x;
	baseline = y + font->ascent;

	while (*text) {
		unsigned char ch = (unsigned char)*text++;
		float gx, gy, gw, gh;
		float u0, v0, u1, v1;
		float inv = font->toLogical > 0.0f ? font->toLogical : 1.0f;
		uir_color_t glyphColor;

		if (ch < UIR_FONT_FIRST_CHAR || ch >= UIR_FONT_FIRST_CHAR + UIR_FONT_NUM_CHARS) {
			ch = (unsigned char)'?';
		}

		{
			const stbtt_bakedchar *b = &font->baked[ch - UIR_FONT_FIRST_CHAR];
			gx = penX + b->xoff * inv;
			gy = baseline + b->yoff * inv;
			gw = (float)(b->x1 - b->x0) * inv;
			gh = (float)(b->y1 - b->y0) * inv;
			u0 = (float)b->x0 / (float)font->atlasW;
			v0 = (float)b->y0 / (float)font->atlasH;
			u1 = (float)b->x1 / (float)font->atlasW;
			v1 = (float)b->y1 / (float)font->atlasH;
		}

		UIR_FontInsetUVs(&u0, &v0, &u1, &v1, font->atlasW, font->atlasH);
		UIR_ViewportSnapQuad(vp, &gx, &gy, &gw, &gh);
		glyphColor = *rgba;
		if (UIR_BatchEnabled()) {
			if (UIR_BatchQuad(font->shader, gx, gy, gw, gh, u0, v0, u1, v1, &glyphColor) != UIR_OK) {
				float color[4];
				color[0] = rgba->r;
				color[1] = rgba->g;
				color[2] = rgba->b;
				color[3] = rgba->a;
				g_fontBackend.setColor(color);
				g_fontBackend.drawPic(gx, gy, gw, gh, u0, v0, u1, v1, font->shader);
				color[0] = color[1] = color[2] = color[3] = 1.0f;
				g_fontBackend.setColor(color);
			}
		} else {
			float color[4];
			color[0] = rgba->r;
			color[1] = rgba->g;
			color[2] = rgba->b;
			color[3] = rgba->a;
			g_fontBackend.setColor(color);
			g_fontBackend.drawPic(gx, gy, gw, gh, u0, v0, u1, v1, font->shader);
			color[0] = color[1] = color[2] = color[3] = 1.0f;
			g_fontBackend.setColor(color);
		}

		penX += font->baked[ch - UIR_FONT_FIRST_CHAR].xadvance * inv;
		if (*text) {
			penX += tracking;
		}
	}

	return UIR_OK;
}

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
)
{
	float penX;
	float baseline;

	if (!vp || !font || !text || !rgba || !g_fontBackend.drawPic || !g_fontBackend.setColor) {
		return UIR_ERR_INVALID_ARG;
	}
	if (skewTan == 0.0f) {
		return UIR_FontDraw(vp, font, x, y, text, rgba, tracking);
	}

	if (font->gpuGeneration != g_gpuGeneration) {
		if (!uir_font_upload(font)) {
			return UIR_ERR_NOT_READY;
		}
	}

	penX = x;
	baseline = y + font->ascent;

	while (*text) {
		unsigned char ch = (unsigned char)*text++;
		float gx, gy, gw, gh;
		float u0, v0, u1, v1;
		float inv = font->toLogical > 0.0f ? font->toLogical : 1.0f;
		uir_color_t glyphColor;

		if (ch < UIR_FONT_FIRST_CHAR || ch >= UIR_FONT_FIRST_CHAR + UIR_FONT_NUM_CHARS) {
			ch = (unsigned char)'?';
		}

		{
			const stbtt_bakedchar *b = &font->baked[ch - UIR_FONT_FIRST_CHAR];
			gx = penX + b->xoff * inv;
			gy = baseline + b->yoff * inv;
			gw = (float)(b->x1 - b->x0) * inv;
			gh = (float)(b->y1 - b->y0) * inv;
			u0 = (float)b->x0 / (float)font->atlasW;
			v0 = (float)b->y0 / (float)font->atlasH;
			u1 = (float)b->x1 / (float)font->atlasW;
			v1 = (float)b->y1 / (float)font->atlasH;
		}

		UIR_FontInsetUVs(&u0, &v0, &u1, &v1, font->atlasW, font->atlasH);
		if (!(gw > 0.0f) || !(gh > 0.0f)) {
			penX += font->baked[ch - UIR_FONT_FIRST_CHAR].xadvance * inv;
			if (*text) {
				penX += tracking;
			}
			continue;
		}

		glyphColor = *rgba;
		if (UIR_BatchEnabled()) {
			if (UIR_BatchQuadSkewed(
			        font->shader, gx, gy, gw, gh, u0, v0, u1, v1, &glyphColor, skewTan, originY) != UIR_OK) {
				float color[4];
				color[0] = rgba->r;
				color[1] = rgba->g;
				color[2] = rgba->b;
				color[3] = rgba->a;
				g_fontBackend.setColor(color);
				g_fontBackend.drawPic(gx, gy, gw, gh, u0, v0, u1, v1, font->shader);
				color[0] = color[1] = color[2] = color[3] = 1.0f;
				g_fontBackend.setColor(color);
			}
		} else {
			float color[4];
			color[0] = rgba->r;
			color[1] = rgba->g;
			color[2] = rgba->b;
			color[3] = rgba->a;
			g_fontBackend.setColor(color);
			g_fontBackend.drawPic(gx, gy, gw, gh, u0, v0, u1, v1, font->shader);
			color[0] = color[1] = color[2] = color[3] = 1.0f;
			g_fontBackend.setColor(color);
		}

		penX += font->baked[ch - UIR_FONT_FIRST_CHAR].xadvance * inv;
		if (*text) {
			penX += tracking;
		}
	}

	return UIR_OK;
}

/* Added in Omaha: emit a rotated glyph quad as two triangles. */
static void uir_font_emit_rotated_quad(
	float gx,
	float gy,
	float gw,
	float gh,
	float u0,
	float v0,
	float u1,
	float v1,
	float pivotX,
	float pivotY,
	float cosr,
	float sinr,
	int shader
)
{
	float px[4];
	float py[4];
	float tri[3][2];
	float uv[3][2];
	int i;

	if (!g_fontBackend.drawTrianglePic) {
		g_fontBackend.drawPic(gx, gy, gw, gh, u0, v0, u1, v1, shader);
		return;
	}

	px[0] = gx;
	py[0] = gy;
	px[1] = gx + gw;
	py[1] = gy;
	px[2] = gx;
	py[2] = gy + gh;
	px[3] = gx + gw;
	py[3] = gy + gh;

	for (i = 0; i < 4; i++) {
		const float dx = px[i] - pivotX;
		const float dy = py[i] - pivotY;
		px[i] = pivotX + cosr * dx - sinr * dy;
		py[i] = pivotY + sinr * dx + cosr * dy;
	}

	tri[0][0] = px[0];
	tri[0][1] = py[0];
	uv[0][0] = u0;
	uv[0][1] = v0;
	tri[1][0] = px[1];
	tri[1][1] = py[1];
	uv[1][0] = u1;
	uv[1][1] = v0;
	tri[2][0] = px[2];
	tri[2][1] = py[2];
	uv[2][0] = u0;
	uv[2][1] = v1;
	g_fontBackend.drawTrianglePic(tri, uv, shader);

	tri[0][0] = px[2];
	tri[0][1] = py[2];
	uv[0][0] = u0;
	uv[0][1] = v1;
	tri[1][0] = px[1];
	tri[1][1] = py[1];
	uv[1][0] = u1;
	uv[1][1] = v0;
	tri[2][0] = px[3];
	tri[2][1] = py[3];
	uv[2][0] = u1;
	uv[2][1] = v1;
	g_fontBackend.drawTrianglePic(tri, uv, shader);
}

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
)
{
	float penX;
	float baseline;
	float rad;
	float cosr;
	float sinr;

	if (!vp || !font || !text || !rgba || !g_fontBackend.drawPic || !g_fontBackend.setColor) {
		return UIR_ERR_INVALID_ARG;
	}
	if (rotationDeg == 0.0f || fabsf(rotationDeg) <= 1e-6f) {
		return UIR_FontDraw(vp, font, x, y, text, rgba, tracking);
	}

	if (font->gpuGeneration != g_gpuGeneration) {
		if (!uir_font_upload(font)) {
			return UIR_ERR_NOT_READY;
		}
	}

	rad = rotationDeg * (3.14159265358979323846f / 180.0f);
	cosr = cosf(rad);
	sinr = sinf(rad);

	penX = x;
	baseline = y + font->ascent;

	while (*text) {
		unsigned char ch = (unsigned char)*text++;
		float gx, gy, gw, gh;
		float u0, v0, u1, v1;
		float inv = font->toLogical > 0.0f ? font->toLogical : 1.0f;
		uir_color_t glyphColor;

		if (ch < UIR_FONT_FIRST_CHAR || ch >= UIR_FONT_FIRST_CHAR + UIR_FONT_NUM_CHARS) {
			ch = (unsigned char)'?';
		}

		{
			const stbtt_bakedchar *b = &font->baked[ch - UIR_FONT_FIRST_CHAR];
			gx = penX + b->xoff * inv;
			gy = baseline + b->yoff * inv;
			gw = (float)(b->x1 - b->x0) * inv;
			gh = (float)(b->y1 - b->y0) * inv;
			u0 = (float)b->x0 / (float)font->atlasW;
			v0 = (float)b->y0 / (float)font->atlasH;
			u1 = (float)b->x1 / (float)font->atlasW;
			v1 = (float)b->y1 / (float)font->atlasH;
		}

		UIR_FontInsetUVs(&u0, &v0, &u1, &v1, font->atlasW, font->atlasH);
		/* Skip FB snap when rotating — snapping then rotating fights AA. */
		if (!(gw > 0.0f) || !(gh > 0.0f)) {
			penX += font->baked[ch - UIR_FONT_FIRST_CHAR].xadvance * inv;
			if (*text) {
				penX += tracking;
			}
			continue;
		}

		glyphColor = *rgba;
		if (UIR_BatchEnabled() &&
			UIR_BatchQuadRotated(
				font->shader, gx, gy, gw, gh, u0, v0, u1, v1, &glyphColor, rotationDeg, pivotX, pivotY
			) == UIR_OK) {
			/* batched */
		} else {
			float color[4];
			color[0] = rgba->r;
			color[1] = rgba->g;
			color[2] = rgba->b;
			color[3] = rgba->a;
			g_fontBackend.setColor(color);
			uir_font_emit_rotated_quad(gx, gy, gw, gh, u0, v0, u1, v1, pivotX, pivotY, cosr, sinr, font->shader);
			color[0] = color[1] = color[2] = color[3] = 1.0f;
			g_fontBackend.setColor(color);
		}

		penX += font->baked[ch - UIR_FONT_FIRST_CHAR].xadvance * inv;
		if (*text) {
			penX += tracking;
		}
	}

	return UIR_OK;
}
