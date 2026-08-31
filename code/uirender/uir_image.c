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

#include "uir_image.h"
#include "uir_batch.h"
#include "uir_compositor.h"
#include "uir_gradient.h"
#include "uir_pathcache.h"
#include "uir_stencil.h"
#include "uir_svg.h"
#include "uir_path.h"
#include "uir_viewport.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uir_image_backend_t g_imageBackend;
static uir_color_t         g_imageTint = {1.0f, 1.0f, 1.0f, 1.0f};

#define UIR_IMAGE_REGISTRY_MAX 128

struct uir_image_s {
	char  path[256];
	int   shader;
	int   width;
	int   height;
	int   gpuGeneration;
	int   refCount;
};

static int                g_gpuGeneration = 1;
static uir_image_t       *g_images[UIR_IMAGE_REGISTRY_MAX];
static int                g_imageCount = 0;

void UIR_ImageSetBackend(const uir_image_backend_t *backend)
{
	if (backend) {
		g_imageBackend = *backend;
	} else {
		memset(&g_imageBackend, 0, sizeof(g_imageBackend));
	}
	/* Added in OPM: gradients stretch-draw through the same image backend. */
	UIR_GradientSyncImageBackend(backend);
}

void UIR_ImageInvalidateGpu(void)
{
	g_gpuGeneration++;
}

void UIR_ImageShutdown(void)
{
	int i;
	for (i = 0; i < g_imageCount; i++) {
		g_images[i] = NULL;
	}
	g_imageCount = 0;
	memset(&g_imageBackend, 0, sizeof(g_imageBackend));
}

static uir_image_t *uir_image_find(const char *vfsPath)
{
	int i;
	for (i = 0; i < g_imageCount; i++) {
		if (g_images[i] && !strcmp(g_images[i]->path, vfsPath)) {
			return g_images[i];
		}
	}
	return NULL;
}

uir_image_t *UIR_ImageResolve(const char *vfsPath)
{
	uir_image_t *img;
	int shader;
	int w = 0;
	int h = 0;

	if (!vfsPath || !vfsPath[0] || !g_imageBackend.registerShaderNoMip) {
		return NULL;
	}

	img = uir_image_find(vfsPath);
	if (img) {
		if (img->gpuGeneration != g_gpuGeneration) {
			shader = g_imageBackend.registerShaderNoMip(vfsPath);
			if (!shader) {
				return NULL;
			}
			img->shader = shader;
			if (g_imageBackend.getShaderSize) {
				g_imageBackend.getShaderSize(shader, &w, &h);
				img->width = w;
				img->height = h;
			}
			img->gpuGeneration = g_gpuGeneration;
		}
		img->refCount++;
		return img;
	}

	if (g_imageCount >= UIR_IMAGE_REGISTRY_MAX) {
		return NULL;
	}

	shader = g_imageBackend.registerShaderNoMip(vfsPath);
	if (!shader) {
		return NULL;
	}
	if (g_imageBackend.getShaderSize) {
		g_imageBackend.getShaderSize(shader, &w, &h);
	}

	img = (uir_image_t *)calloc(1, sizeof(*img));
	if (!img) {
		return NULL;
	}
	strncpy(img->path, vfsPath, sizeof(img->path) - 1);
	img->shader = shader;
	img->width = w;
	img->height = h;
	img->gpuGeneration = g_gpuGeneration;
	img->refCount = 1;
	g_images[g_imageCount++] = img;
	return img;
}

void UIR_ImageRelease(uir_image_t *image)
{
	(void)image;
}

int UIR_ImageShader(const uir_image_t *image)
{
	return image ? image->shader : 0;
}

float UIR_ImageWidth(const uir_image_t *image)
{
	return image ? (float)image->width : 0.0f;
}

float UIR_ImageHeight(const uir_image_t *image)
{
	return image ? (float)image->height : 0.0f;
}

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
)
{
	if (!outX || !outY || !outW || !outH || !s1 || !t1 || !s2 || !t2) {
		return;
	}

	*outX = boxX;
	*outY = boxY;
	*outW = boxW;
	*outH = boxH;
	*s1 = 0.0f;
	*t1 = 0.0f;
	*s2 = 1.0f;
	*t2 = 1.0f;

	if (!(boxW > 0.0f) || !(boxH > 0.0f) || !(imgW > 0.0f) || !(imgH > 0.0f)) {
		return;
	}

	if (fit == UIR_IMAGE_FIT_STRETCH || fit == UIR_IMAGE_FIT_REPEAT) {
		return;
	}

	{
		const float scaleX = boxW / imgW;
		const float scaleY = boxH / imgH;
		float scale;
		float drawW;
		float drawH;

		if (fit == UIR_IMAGE_FIT_CONTAIN) {
			scale = (scaleX < scaleY) ? scaleX : scaleY;
			drawW = imgW * scale;
			drawH = imgH * scale;
			*outX = boxX + (boxW - drawW) * 0.5f;
			*outY = boxY + (boxH - drawH) * 0.5f;
			*outW = drawW;
			*outH = drawH;
		} else if (fit == UIR_IMAGE_FIT_COVER) {
			scale = (scaleX > scaleY) ? scaleX : scaleY;
			drawW = imgW * scale;
			drawH = imgH * scale;
			*outX = boxX + (boxW - drawW) * 0.5f;
			*outY = boxY + (boxH - drawH) * 0.5f;
			*outW = drawW;
			*outH = drawH;
			{
				const float uSpan = boxW / drawW;
				const float vSpan = boxH / drawH;
				const float u0 = (boxX - *outX) / drawW;
				const float v0 = (boxY - *outY) / drawH;
				*s1 = u0;
				*t1 = v0;
				*s2 = u0 + uSpan;
				*t2 = v0 + vSpan;
			}
		}
	}
}

static uir_status_t uir_image_build_clip_path(
	const char *pathD,
	float x,
	float y,
	float w,
	float h,
	float viewW,
	float viewH,
	float rotationDeg,
	const uir_path_t **outPath
)
{
	uir_viewbox_t view;
	uir_rect_t dest;

	if (!pathD || !outPath) {
		return UIR_ERR_INVALID_ARG;
	}

	view.minX = 0.0f;
	view.minY = 0.0f;
	view.width = (viewW > 0.0f) ? viewW : w;
	view.height = (viewH > 0.0f) ? viewH : h;
	dest.x = x;
	dest.y = y;
	dest.w = w;
	dest.h = h;

	/* Added in OPM: shared path cache — *outPath is cache-owned, do not free. */
	return UIR_GetMappedPathCached(pathD, &dest, &view, UIR_FIT_STRETCH, rotationDeg, 0, outPath);
}

static void uir_image_set_tint(const uir_color_t *tintRgba)
{
	float c[4];
	if (!g_imageBackend.setColor) {
		return;
	}
	if (tintRgba) {
		g_imageTint = *tintRgba;
		c[0] = tintRgba->r;
		c[1] = tintRgba->g;
		c[2] = tintRgba->b;
		c[3] = tintRgba->a;
		g_imageBackend.setColor(c);
	} else {
		g_imageTint.r = g_imageTint.g = g_imageTint.b = g_imageTint.a = 1.0f;
		g_imageBackend.setColor(NULL);
	}
}

static uir_rect_t uir_intersect_rect(const uir_rect_t *a, const uir_rect_t *b)
{
	uir_rect_t r;
	float x1 = (a->x > b->x) ? a->x : b->x;
	float y1 = (a->y > b->y) ? a->y : b->y;
	float x2 = (a->x + a->w < b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
	float y2 = (a->y + a->h < b->y + b->h) ? (a->y + a->h) : (b->y + b->h);
	r.x = x1;
	r.y = y1;
	r.w = (x2 > x1) ? (x2 - x1) : 0.0f;
	r.h = (y2 > y1) ? (y2 - y1) : 0.0f;
	return r;
}

static uir_rect_t uir_union_path_bounds(const uir_path_t *const *paths, int pathCount)
{
	uir_rect_t unionBounds;
	uir_rect_t bounds;
	int i;

	unionBounds.x = 0.0f;
	unionBounds.y = 0.0f;
	unionBounds.w = 0.0f;
	unionBounds.h = 0.0f;

	if (!paths || pathCount <= 0 || !paths[0]) {
		return unionBounds;
	}

	if (UIR_PathBounds(paths[0], &unionBounds) != UIR_OK) {
		unionBounds.w = 0.0f;
		unionBounds.h = 0.0f;
		return unionBounds;
	}

	for (i = 1; i < pathCount; i++) {
		if (!paths[i] || UIR_PathBounds(paths[i], &bounds) != UIR_OK) {
			continue;
		}
		{
			const float minX = (unionBounds.x < bounds.x) ? unionBounds.x : bounds.x;
			const float minY = (unionBounds.y < bounds.y) ? unionBounds.y : bounds.y;
			const float maxX = (unionBounds.x + unionBounds.w > bounds.x + bounds.w)
				? (unionBounds.x + unionBounds.w)
				: (bounds.x + bounds.w);
			const float maxY = (unionBounds.y + unionBounds.h > bounds.y + bounds.h)
				? (unionBounds.y + unionBounds.h)
				: (bounds.y + bounds.h);
			unionBounds.x = minX;
			unionBounds.y = minY;
			unionBounds.w = maxX - minX;
			unionBounds.h = maxY - minY;
		}
	}
	return unionBounds;
}

static int uir_path_is_axis_aligned_rect(const uir_path_t *path)
{
	int i;
	const uir_contour_t *c;

	if (!path || path->contourCount != 1) {
		return 0;
	}
	c = &path->contours[0];
	if (!c->closed || c->count < 4 || c->count > 5) {
		return 0;
	}
	for (i = 0; i < c->count - 1; i++) {
		const float dx = c->points[i + 1].x - c->points[i].x;
		const float dy = c->points[i + 1].y - c->points[i].y;
		if (fabsf(dx) >= 0.01f && fabsf(dy) >= 0.01f) {
			return 0;
		}
	}
	return 1;
}

static void uir_emit_quad(
	uir_image_t *image,
	float qx,
	float qy,
	float qw,
	float qh,
	float s1,
	float t1,
	float s2,
	float t2,
	float pivotX,
	float pivotY,
	float cosr,
	float sinr,
	int rotated
)
{
	float px[4];
	float py[4];
	float tri[3][2];
	float uv[3][2];
	int i;

	if (!image || !g_imageBackend.drawStretchPic) {
		return;
	}

	if (!rotated && UIR_BatchEnabled()) {
		if (UIR_BatchQuad(image->shader, qx, qy, qw, qh, s1, t1, s2, t2, &g_imageTint) == UIR_OK) {
			return;
		}
	}

	UIR_BatchFlush();

	if (!rotated || !g_imageBackend.drawTrianglePic) {
		g_imageBackend.drawStretchPic(qx, qy, qw, qh, s1, t1, s2, t2, image->shader);
		return;
	}

	px[0] = qx;
	py[0] = qy;
	px[1] = qx + qw;
	py[1] = qy;
	px[2] = qx;
	py[2] = qy + qh;
	px[3] = qx + qw;
	py[3] = qy + qh;

	for (i = 0; i < 4; i++) {
		const float dx = px[i] - pivotX;
		const float dy = py[i] - pivotY;
		px[i] = pivotX + cosr * dx - sinr * dy;
		py[i] = pivotY + sinr * dx + cosr * dy;
	}

	tri[0][0] = px[0];
	tri[0][1] = py[0];
	uv[0][0] = s1;
	uv[0][1] = t1;
	tri[1][0] = px[1];
	tri[1][1] = py[1];
	uv[1][0] = s2;
	uv[1][1] = t1;
	tri[2][0] = px[2];
	tri[2][1] = py[2];
	uv[2][0] = s1;
	uv[2][1] = t2;
	g_imageBackend.drawTrianglePic(tri, uv, image->shader);

	tri[0][0] = px[2];
	tri[0][1] = py[2];
	uv[0][0] = s1;
	uv[0][1] = t2;
	tri[1][0] = px[1];
	tri[1][1] = py[1];
	uv[1][0] = s2;
	uv[1][1] = t1;
	tri[2][0] = px[3];
	tri[2][1] = py[3];
	uv[2][0] = s2;
	uv[2][1] = t2;
	g_imageBackend.drawTrianglePic(tri, uv, image->shader);
}

static uir_status_t uir_image_draw_resolved(
	uir_image_t *image,
	float x,
	float y,
	float w,
	float h,
	uir_image_fit_t fit,
	float rotationDeg,
	float dipScale,
	float backgroundScale,
	const uir_color_t *tintRgba
)
{
	float imgW;
	float imgH;
	float drawX;
	float drawY;
	float drawW;
	float drawH;
	float s1;
	float t1;
	float s2;
	float t2;
	float pivotX;
	float pivotY;
	float cosr;
	float sinr;
	int rotated;

	if (!image || !g_imageBackend.drawStretchPic) {
		return UIR_ERR_NOT_READY;
	}

	imgW = (float)image->width;
	imgH = (float)image->height;
	pivotX = x + w * 0.5f;
	pivotY = y + h * 0.5f;
	rotated = (rotationDeg != 0.0f) ? 1 : 0;
	if (rotated) {
		const float rad = rotationDeg * (3.14159265358979323846f / 180.0f);
		cosr = cosf(rad);
		sinr = sinf(rad);
	} else {
		cosr = 1.0f;
		sinr = 0.0f;
	}

	uir_image_set_tint(tintRgba);

	if (dipScale <= 0.0f || dipScale != dipScale) {
		dipScale = 1.0f;
	}
	if (backgroundScale <= 0.0f || backgroundScale != backgroundScale) {
		backgroundScale = 1.0f;
	}

	if (fit == UIR_IMAGE_FIT_REPEAT) {
		int cols;
		int rows;
		int c;
		int r;
		int tileCount;

		if (!(imgW > 0.0f) || !(imgH > 0.0f)) {
			UIR_ComputeImageRect(
				imgW, imgH, x, y, w, h, UIR_IMAGE_FIT_STRETCH,
				&drawX, &drawY, &drawW, &drawH, &s1, &t1, &s2, &t2
			);
			uir_emit_quad(image, drawX, drawY, drawW, drawH, s1, t1, s2, t2,
				pivotX, pivotY, cosr, sinr, rotated);
		} else if (!rotated) {
			/*
			 * Retail parity: authored layout px are DIP-scaled (uiPxScale) while
			 * shader sizes are native texels. Tile UVs by logical size / texel size,
			 * matching legacy UIFakkLabel statbar DrawStretchPic fvWidth/fvHeight.
			 */
			s1 = 0.0f;
			t1 = 0.0f;
			s2 = w / (dipScale * imgW * backgroundScale);
			t2 = h / (dipScale * imgH * backgroundScale);
			uir_emit_quad(image, x, y, w, h, s1, t1, s2, t2, pivotX, pivotY, cosr, sinr, 0);
		} else {
			const float tileW = imgW * dipScale * backgroundScale;
			const float tileH = imgH * dipScale * backgroundScale;
			cols = (int)ceilf(w / tileW);
			rows = (int)ceilf(h / tileH);
			if (cols < 1) {
				cols = 1;
			}
			if (rows < 1) {
				rows = 1;
			}
			tileCount = cols * rows;
			if (tileCount > 1024) {
				tileCount = 1024;
				rows = tileCount / cols;
				if (rows < 1) {
					rows = 1;
				}
			}
			for (r = 0; r < rows; r++) {
				for (c = 0; c < cols; c++) {
					const float tileX = x + (float)c * tileW;
					const float tileY = y + (float)r * tileH;
					const float remW = w - (float)c * tileW;
					const float remH = h - (float)r * tileH;
					const float quadW = (remW < tileW) ? remW : tileW;
					const float quadH = (remH < tileH) ? remH : tileH;

					if (quadW <= 0.0f || quadH <= 0.0f) {
						continue;
					}
					uir_emit_quad(
						image,
						tileX,
						tileY,
						quadW,
						quadH,
						0.0f,
						0.0f,
						quadW / imgW,
						quadH / imgH,
						pivotX,
						pivotY,
						cosr,
						sinr,
						rotated
					);
				}
			}
		}
	} else {
		UIR_ComputeImageRect(imgW, imgH, x, y, w, h, fit, &drawX, &drawY, &drawW, &drawH, &s1, &t1, &s2, &t2);
		/* Added in OPM: background-scale shrinks/grows contain/cover/stretch about the fitted center. */
		if (backgroundScale > 0.0f && backgroundScale != 1.0f) {
			const float cx = drawX + drawW * 0.5f;
			const float cy = drawY + drawH * 0.5f;
			drawW *= backgroundScale;
			drawH *= backgroundScale;
			drawX = cx - drawW * 0.5f;
			drawY = cy - drawH * 0.5f;
		}
		uir_emit_quad(image, drawX, drawY, drawW, drawH, s1, t1, s2, t2,
			pivotX, pivotY, cosr, sinr, rotated);
	}

	g_imageBackend.setColor(NULL);
	return UIR_OK;
}

#define UIR_IMAGE_MAX_CLIP_PATHS 8

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
)
{
	uir_image_t *image;
	const uir_viewport_t *vp = UIR_CompositorViewport();
	uir_rect_t bounds;
	uir_rect_t clipAabb;
	const uir_path_t *builtPaths[UIR_IMAGE_MAX_CLIP_PATHS];
	int builtCount = 0;
	uir_status_t st = UIR_OK;
	int useStencil = 0;
	int useAxisScissor = 0;
	int pushedClip = 0;
	int i;

	if (!vfsPath || !vp || !(w > 0.0f) || !(h > 0.0f)) {
		return UIR_ERR_INVALID_ARG;
	}

	image = UIR_ImageResolve(vfsPath);
	if (!image) {
		return UIR_ERR_MISSING_ASSET;
	}

	bounds.x = x;
	bounds.y = y;
	bounds.w = w;
	bounds.h = h;

	if (clipPathCount > UIR_IMAGE_MAX_CLIP_PATHS) {
		clipPathCount = UIR_IMAGE_MAX_CLIP_PATHS;
	}

	for (i = 0; i < clipPathCount; i++) {
		builtPaths[i] = NULL;
		st = uir_image_build_clip_path(clipPathD[i], x, y, w, h, viewW, viewH, rotationDeg, &builtPaths[i]);
		if (st != UIR_OK || !builtPaths[i]) {
			UIR_ImageRelease(image);
			return st != UIR_OK ? st : UIR_ERR_INVALID_ARG;
		}
		builtCount++;
	}

	if (builtCount > 0) {
		clipAabb = uir_union_path_bounds(builtPaths, builtCount);
		clipAabb = uir_intersect_rect(&clipAabb, &bounds);

		if (builtCount == 1 && uir_path_is_axis_aligned_rect(builtPaths[0])) {
			useAxisScissor = 1;
		} else if (UIR_StencilAvailable()) {
			useStencil = 1;
		} else {
			useAxisScissor = 1;
		}
	}

	if (useStencil) {
		st = UIR_BeginShapeClip(vp, &bounds);
		if (st != UIR_OK) {
			UIR_ImageRelease(image);
			return st;
		}
		for (i = 0; i < builtCount; i++) {
			st = UIR_StencilWritePath(vp, builtPaths[i]);
			if (st != UIR_OK) {
				UIR_EndShapeClip();
				UIR_ImageRelease(image);
				return st;
			}
		}
		st = UIR_BeginShapeClipDraw();
		if (st != UIR_OK) {
			UIR_EndShapeClip();
			UIR_ImageRelease(image);
			return st;
		}
	} else if (useAxisScissor) {
		UIR_PushClipRect(clipAabb.x, clipAabb.y, clipAabb.w, clipAabb.h);
		pushedClip = 1;
	} else {
		UIR_PushClipRect(x, y, w, h);
		pushedClip = 1;
	}

	st = uir_image_draw_resolved(image, x, y, w, h, fit, rotationDeg, dipScale, backgroundScale, tintRgba);

	if (useStencil) {
		UIR_EndShapeClip();
	} else if (pushedClip) {
		UIR_PopClipRect();
	}

	/* Cache-owned paths — do not free. */
	UIR_ImageRelease(image);
	return st;
}
