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

#include "uir_stencil.h"
#include "uir_viewport.h"
#include "uir_batch.h"
#include "uir_compositor.h"
#include "uir_draw2d.h"
#include "uir_pathcache.h"
#include "uir_meshcache.h"
#include "uir_tess.h"
#include "uir_svg.h"
#include "uir_path.h"

#include <math.h>
#include <string.h>
static uir_stencil_backend_t g_stencilBackend;
static int                   g_stencilDepth = 0;
static int                   g_shapeClipAabbActive = 0;
static int                   g_shapeClipStencil = 0; /* ui_shape_clip: default off */
static uir_viewport_t        g_stencilVp;
static uir_rect_t            g_stencilBounds;

static uir_color_t g_stencilWhite = {1.0f, 1.0f, 1.0f, 1.0f};

static uir_vert_t      g_stencilTessVerts[UIR_BATCH_MAX_VERTS];
static unsigned short  g_stencilTessIdx[UIR_BATCH_MAX_INDEXES];


void UIR_StencilSetBackend(const uir_stencil_backend_t *backend)
{
	if (backend) {
		g_stencilBackend = *backend;
	} else {
		memset(&g_stencilBackend, 0, sizeof(g_stencilBackend));
	}
}

void UIR_SetShapeClipStencil(int enabled)
{
	g_shapeClipStencil = enabled ? 1 : 0;
}

int UIR_ShapeClipStencilEnabled(void)
{
	return g_shapeClipStencil;
}

int UIR_StencilAvailable(void)
{
	/* Default off: AABB scissor matches prior Linux behavior and avoids CPU PathFill. */
	if (!g_shapeClipStencil) {
		return 0;
	}
	if (!g_stencilBackend.available) {
		return 0;
	}
	return g_stencilBackend.available() ? 1 : 0;
}

static void uir_stencil_sink_box(float x, float y, float w, float h, const uir_color_t *color, void *userdata)
{
	(void)color;
	(void)userdata;
	if (g_stencilBackend.maskBox) {
		g_stencilBackend.maskBox(x, y, w, h);
	}
}

static void uir_bounds_to_scissor(const uir_viewport_t *vp, const uir_rect_t *bounds, int *sx, int *sy, int *sw, int *sh)
{
	float fx0;
	float fy0;
	float fx1;
	float fy1;

	UIR_ViewportDrawToFb(vp, bounds->x, bounds->y, &fx0, &fy0);
	UIR_ViewportDrawToFb(vp, bounds->x + bounds->w, bounds->y + bounds->h, &fx1, &fy1);
	*sx = (int)floorf(fx0 < fx1 ? fx0 : fx1);
	*sy = (int)floorf(fy0 < fy1 ? fy0 : fy1);
	*sw = (int)ceilf(fx0 > fx1 ? fx0 : fx1) - *sx;
	*sh = (int)ceilf(fy0 > fy1 ? fy0 : fy1) - *sy;
	if (*sw < 0) {
		*sw = 0;
	}
	if (*sh < 0) {
		*sh = 0;
	}
	*sy = vp->vpY + vp->vpH - (*sy + *sh);
}

uir_status_t UIR_BeginShapeClip(const uir_viewport_t *vp, const uir_rect_t *bounds)
{
	int sx;
	int sy;
	int sw;
	int sh;

	if (!vp || !bounds || !g_stencilBackend.beginMask) {
		return UIR_ERR_NOT_READY;
	}
	if (g_stencilDepth > 0 || g_shapeClipAabbActive) {
		return UIR_ERR_WRONG_PHASE;
	}

	UIR_BatchFlush();
	/* Close UI2D session so mask fills do not ride the color batch path. */
	UIR_BatchCloseDrawSession();
	/* Added in Omaha: stencil mask changes GL scissor independently. */
	UIR_InvalidateAppliedClip();
	/* Added in Omaha: Phase 1 — shape clip changes scissor. */
	UIR_Draw2DInvalidate();
	g_stencilVp = *vp;
	g_stencilBounds = *bounds;
	uir_bounds_to_scissor(vp, bounds, &sx, &sy, &sw, &sh);
	g_stencilBackend.beginMask(sx, sy, sw, sh);
	g_stencilDepth = 1;
	return UIR_OK;
}

uir_status_t UIR_StencilWritePath(const uir_viewport_t *vp, const uir_path_t *path)
{
	uir_draw_sink_t sink;
	uir_status_t st;
	float fringeFbPx = 0.0f;
	unsigned meshKey;
	const uir_vert_t *cachedVerts = NULL;
	const unsigned short *cachedIdx = NULL;
	int cachedNv = 0;
	int cachedNi = 0;
	int nv = 0;
	int ni = 0;

	if (!vp || !path || g_stencilDepth <= 0) {
		return UIR_ERR_INVALID_ARG;
	}

	/*
	 * GPU tess + dedicated maskTris draw (colorMask off / ZERO,ONE).
	 * Never UIR_BatchTriangles — that path was flashing white over the
	 * already-painted shape background.
	 */
	if (g_stencilBackend.maskTris && UIR_BatchEnabled()) {
		meshKey = UIR_MeshCacheKeyFill(path, &g_stencilWhite, 1, fringeFbPx);
		if (UIR_MeshCacheLookup(meshKey, &cachedVerts, &cachedNv, &cachedIdx, &cachedNi)) {
			g_stencilBackend.maskTris(cachedVerts, cachedNv, cachedIdx, cachedNi);
			return UIR_OK;
		}
		st = UIR_TessFillPath(
			vp,
			path,
			&g_stencilWhite,
			fringeFbPx,
			g_stencilTessVerts,
			UIR_BATCH_MAX_VERTS,
			&nv,
			g_stencilTessIdx,
			UIR_BATCH_MAX_INDEXES,
			&ni
		);
		if (st == UIR_OK && nv >= 3 && ni >= 3) {
			UIR_MeshCacheStore(meshKey, g_stencilTessVerts, nv, g_stencilTessIdx, ni);
			g_stencilBackend.maskTris(g_stencilTessVerts, nv, g_stencilTessIdx, ni);
			return UIR_OK;
		}
	}

	/* Fallback: CPU scanline via maskBox. */
	memset(&sink, 0, sizeof(sink));
	sink.drawBox = uir_stencil_sink_box;
	return UIR_PathFill(vp, path, &g_stencilWhite, &sink, 0);
}

uir_status_t UIR_BeginShapeClipDraw(void)
{
	if (g_stencilDepth <= 0 || !g_stencilBackend.beginDraw) {
		return UIR_ERR_WRONG_PHASE;
	}
	UIR_BatchFlush();
	g_stencilBackend.beginDraw();
	return UIR_OK;
}

void UIR_EndShapeClip(void)
{
	if (g_shapeClipAabbActive) {
		UIR_BatchFlush();
		UIR_PopClipRect();
		g_shapeClipAabbActive = 0;
		return;
	}
	if (g_stencilDepth <= 0) {
		return;
	}
	UIR_BatchFlush();
	if (g_stencilBackend.end) {
		g_stencilBackend.end();
	}
	g_stencilDepth = 0;
	/* Added in Omaha: stencil end may restore GL scissor. */
	UIR_InvalidateAppliedClip();
	/* Added in Omaha: Phase 1 — shape clip end restores scissor. */
	UIR_Draw2DInvalidate();
}

/* Added in Omaha: map SVG path D into dest for stencil/AABB child clipping. */
static uir_status_t uir_build_svg_clip_path(
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

	/* Added in Omaha: shared path cache — *outPath is cache-owned, do not free. */
	return UIR_GetMappedPathCached(pathD, &dest, &view, UIR_FIT_STRETCH, rotationDeg, 0, outPath);
}

static uir_rect_t uir_stencil_intersect_rect(const uir_rect_t *a, const uir_rect_t *b)
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

static uir_rect_t uir_stencil_union_path_bounds(const uir_path_t *const *paths, int pathCount)
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

uir_status_t UIR_BeginSvgShapeClip(
	float x,
	float y,
	float w,
	float h,
	const char *const *pathD,
	int pathCount,
	float viewW,
	float viewH,
	float rotationDeg
)
{
	const uir_viewport_t *vp = UIR_CompositorViewport();
	uir_rect_t bounds;
	const uir_path_t *builtPaths[UIR_SHAPE_CLIP_MAX_PATHS];
	int builtCount = 0;
	uir_status_t st;
	int i;

	if (!pathD || pathCount <= 0 || !(w > 0.0f) || !(h > 0.0f)) {
		return UIR_ERR_INVALID_ARG;
	}
	if (g_stencilDepth > 0 || g_shapeClipAabbActive) {
		return UIR_ERR_WRONG_PHASE;
	}
	if (pathCount > UIR_SHAPE_CLIP_MAX_PATHS) {
		pathCount = UIR_SHAPE_CLIP_MAX_PATHS;
	}

	bounds.x = x;
	bounds.y = y;
	bounds.w = w;
	bounds.h = h;


	for (i = 0; i < pathCount; i++) {
		builtPaths[i] = NULL;
		st = uir_build_svg_clip_path(pathD[i], x, y, w, h, viewW, viewH, rotationDeg, &builtPaths[i]);
		if (st != UIR_OK || !builtPaths[i]) {
			return st != UIR_OK ? st : UIR_ERR_INVALID_ARG;
		}
		builtCount++;
	}

	if (vp && UIR_StencilAvailable()) {
		st = UIR_BeginShapeClip(vp, &bounds);
		if (st != UIR_OK) {
			return st;
		}
		for (i = 0; i < builtCount; i++) {
			st = UIR_StencilWritePath(vp, builtPaths[i]);
			if (st != UIR_OK) {
				UIR_EndShapeClip();
				return st;
			}
		}
		st = UIR_BeginShapeClipDraw();
		if (st != UIR_OK) {
			UIR_EndShapeClip();
			return st;
		}
		/* Cache-owned paths — do not free. */
		return UIR_OK;
	}

	/*
	 * Fallback: axis-aligned scissor from the union of path bounds
	 * intersected with the node box (matches uir_image / uir_gradient).
	 * Full node box was looser than necessary for skewed clips.
	 */
	{
		uir_rect_t clipAabb = uir_stencil_union_path_bounds(builtPaths, builtCount);
		clipAabb = uir_stencil_intersect_rect(&clipAabb, &bounds);
		if (!(clipAabb.w > 0.0f) || !(clipAabb.h > 0.0f)) {
			clipAabb = bounds;
		}
		st = UIR_PushClipRect(clipAabb.x, clipAabb.y, clipAabb.w, clipAabb.h);
	}
	if (st != UIR_OK) {
		return st;
	}
	g_shapeClipAabbActive = 1;
	return UIR_OK;
}
