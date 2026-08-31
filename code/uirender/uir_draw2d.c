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

#include "uir_draw2d.h"

#include "uir_batch.h"
#include "uir_compositor.h"
#include "uir_debug.h"
#include "uir_meshcache.h"
#include "uir_tess.h"

#include <stdio.h>
#include <string.h>

static uir_draw2d_backend_t g_d2d;
static uir_vert_t           g_d2dVerts[UIR_BATCH_MAX_VERTS];
static unsigned short       g_d2dIdx[UIR_BATCH_MAX_INDEXES];

static void uir_draw2d_warn_fallback(const char *op, uir_status_t st, const uir_path_t *path, uir_stats_t *stats)
{
	static int s_warnFill;
	static int s_warnStroke;
	int *warnFlag = op[0] == 'f' ? &s_warnFill : &s_warnStroke;
	int sig;

	if (!UIR_DebugEnabled()) {
		return;
	}
	sig = (int)st * 1000 + (path ? path->contourCount : 0);
	if (*warnFlag == sig) {
		return;
	}
	*warnFlag = sig;
	UIR_DebugPrintf(
		"UIR: GPU %s tessellation failed (status=%d contours=%d), using CPU fallback\n",
		op,
		(int)st,
		path ? path->contourCount : 0
	);
	if (stats) {
		stats->tessFallbackStatus = (int)st;
		stats->tessFallbackContours = path ? path->contourCount : 0;
	}
}

void UIR_Draw2D_SetBackend(const uir_draw2d_backend_t *backend)
{
	if (backend) {
		g_d2d = *backend;
	} else {
		memset(&g_d2d, 0, sizeof(g_d2d));
	}
}

void UIR_Draw2D_Begin(const uir_viewport_t *vp)
{
	if (!vp || !g_d2d.set2DWindow) {
		return;
	}
	UIR_BatchFlush();
	/* Added in OPM: Begin sets full-viewport scissor; drop applied-clip cache. */
	UIR_InvalidateAppliedClip();
	/* Top-left draw space: orthoT=0 at top, orthoB=height at bottom. */
	g_d2d.set2DWindow(
		vp->vpX,
		vp->vpY,
		vp->vpW,
		vp->vpH,
		vp->orthoL,
		vp->orthoR,
		vp->orthoB,
		vp->orthoT,
		-1.0f,
		1.0f
	);
	if (g_d2d.scissor) {
		g_d2d.scissor(vp->vpX, vp->vpY, vp->vpW, vp->vpH);
	}
}

void UIR_Draw2D_Scissor(int x, int y, int w, int h)
{
	UIR_BatchFlush();
	if (g_d2d.scissor) {
		g_d2d.scissor(x, y, w, h);
	}
}

uir_status_t UIR_Draw2D_Box(float x, float y, float w, float h, const uir_color_t *rgba)
{
	if (!rgba) {
		return UIR_ERR_INVALID_ARG;
	}
	if (UIR_BatchEnabled()) {
		if (UIR_BatchQuad(0, x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, rgba) == UIR_OK) {
			return UIR_OK;
		}
	}
	if (!g_d2d.setColor || !g_d2d.drawBox) {
		return UIR_ERR_INVALID_ARG;
	}
	{
		float c[4];
		c[0] = rgba->r;
		c[1] = rgba->g;
		c[2] = rgba->b;
		c[3] = rgba->a;
		UIR_BatchFlush();
		g_d2d.setColor(c);
		g_d2d.drawBox(x, y, w, h);
		g_d2d.setColor(NULL);
	}
	return UIR_OK;
}

static void uir_d2d_sink_box(float x, float y, float w, float h, const uir_color_t *color, void *userdata)
{
	(void)userdata;
	UIR_Draw2D_Box(x, y, w, h, color);
}

static uir_status_t uir_draw2d_path_gpu(
	const uir_viewport_t *vp,
	const uir_path_t     *path,
	const uir_color_t    *rgba,
	uir_stats_t          *stats,
	int                   crisp,
	int                   noFringe
)
{
	uir_vert_t *verts = g_d2dVerts;
	unsigned short *idx = g_d2dIdx;
	int nv = 0;
	int ni = 0;
	uir_status_t st;
	float fringeFbPx = 0.0f;
	unsigned meshKey;
	const uir_vert_t *cachedVerts = NULL;
	const unsigned short *cachedIdx = NULL;
	int cachedNv = 0;
	int cachedNi = 0;

	if (!UIR_BatchEnabled()) {
		return UIR_ERR_UNSUPPORTED;
	}

	(void)crisp;
	(void)noFringe;

	/* Added in OPM: Stage D mesh cache for GPU fills. */
	meshKey = UIR_MeshCacheKeyFill(path, rgba, crisp, fringeFbPx);
	if (UIR_MeshCacheLookup(meshKey, &cachedVerts, &cachedNv, &cachedIdx, &cachedNi)) {
		if (stats) {
			stats->meshCacheHits++;
		}
		return UIR_BatchTriangles(0, cachedVerts, cachedNv, cachedIdx, cachedNi);
	}
	if (UIR_MeshCacheEnabled() && stats) {
		stats->meshCacheMisses++;
	}

	UIR_TessSetStats(stats);
	st = UIR_TessFillPath(vp, path, rgba, fringeFbPx, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni);
	UIR_TessSetStats(NULL);
	if (st != UIR_OK) {
		if (stats) {
			stats->tessFallbacks++;
		}
		return st;
	}

	UIR_MeshCacheStore(meshKey, verts, nv, idx, ni);
	st = UIR_BatchTriangles(0, verts, nv, idx, ni);
	return st;
}

static uir_status_t uir_draw2d_path_cpu(
	const uir_viewport_t *vp,
	const uir_path_t     *path,
	const uir_color_t    *rgba,
	uir_stats_t          *stats,
	int                   crisp,
	const uir_draw_sink_t *sink
)
{
	return UIR_PathFill(vp, path, rgba, sink, crisp);
}

uir_status_t UIR_Draw2D_Polygon(
	const uir_viewport_t *vp,
	const uir_point_t *pts,
	int count,
	const uir_color_t *rgba,
	uir_stats_t *stats
)
{
	uir_path_t path;
	uir_status_t st;

	if (!vp || !pts || !rgba || count < 3) {
		return UIR_ERR_INVALID_ARG;
	}

	UIR_PathInit(&path);
	st = UIR_PathBeginContour(&path, NULL);
	if (st == UIR_OK) {
		int i;
		for (i = 0; i < count; i++) {
			st = UIR_ContourAddPoint(&path.contours[0], pts[i].x, pts[i].y);
			if (st != UIR_OK) {
				break;
			}
		}
	}
	if (st == UIR_OK) {
		st = UIR_ContourClose(&path.contours[0]);
	}
	if (st == UIR_OK) {
		st = UIR_Draw2D_Path(vp, &path, rgba, stats, 0, 0);
	}
	UIR_PathFree(&path);
	return st;
}

uir_status_t UIR_Draw2D_Path(
	const uir_viewport_t *vp,
	const uir_path_t     *path,
	const uir_color_t    *rgba,
	uir_stats_t          *stats,
	int                   crisp,
	int                   noFringe
)
{
	uir_draw_sink_t sink;
	uir_status_t st;
	int backbuffer = UIR_BatchFringeEnabled();

	if (!vp || !path || !rgba) {
		return UIR_ERR_INVALID_ARG;
	}

	st = uir_draw2d_path_gpu(vp, path, rgba, stats, crisp, noFringe);
	if (st == UIR_OK) {
		return UIR_OK;
	}

	uir_draw2d_warn_fallback("fill", st, path, stats);
	UIR_BatchFlush();
	sink.drawBox = uir_d2d_sink_box;
	sink.userdata = NULL;
	sink.stats = stats;
	return uir_draw2d_path_cpu(vp, path, rgba, stats, backbuffer ? 1 : crisp, &sink);
}

uir_status_t UIR_Draw2D_PathStroke(
	const uir_viewport_t *vp,
	const uir_path_t     *path,
	const uir_color_t    *rgba,
	float                 widthPx,
	uir_stats_t          *stats,
	int                   crisp
)
{
	uir_vert_t *verts = g_d2dVerts;
	unsigned short *idx = g_d2dIdx;
	int nv = 0;
	int ni = 0;
	uir_draw_sink_t sink;
	uir_status_t st;
	int backbuffer = UIR_BatchFringeEnabled();
	int drawCrisp = backbuffer ? 1 : crisp;
	unsigned meshKey;
	const uir_vert_t *cachedVerts = NULL;
	const unsigned short *cachedIdx = NULL;
	int cachedNv = 0;
	int cachedNi = 0;

	if (!vp || !path || !rgba) {
		return UIR_ERR_INVALID_ARG;
	}

	if (UIR_BatchEnabled()) {
		/* Added in OPM: Stage D mesh cache for GPU strokes. */
		meshKey = UIR_MeshCacheKeyStroke(path, rgba, widthPx, drawCrisp);
		if (UIR_MeshCacheLookup(meshKey, &cachedVerts, &cachedNv, &cachedIdx, &cachedNi)) {
			if (stats) {
				stats->meshCacheHits++;
			}
			return UIR_BatchTriangles(0, cachedVerts, cachedNv, cachedIdx, cachedNi);
		}
		if (UIR_MeshCacheEnabled() && stats) {
			stats->meshCacheMisses++;
		}

		st = UIR_TessStrokePath(
			vp,
			path,
			rgba,
			widthPx,
			drawCrisp,
			verts,
			UIR_BATCH_MAX_VERTS,
			&nv,
			idx,
			UIR_BATCH_MAX_INDEXES,
			&ni
		);
		if (st == UIR_OK) {
			UIR_MeshCacheStore(meshKey, verts, nv, idx, ni);
			return UIR_BatchTriangles(0, verts, nv, idx, ni);
		}
		if (stats) {
			stats->tessFallbacks++;
		}
		uir_draw2d_warn_fallback("stroke", st, path, stats);
	}

	UIR_BatchFlush();
	sink.drawBox = uir_d2d_sink_box;
	sink.userdata = NULL;
	sink.stats = stats;
	st = UIR_PathStroke(vp, path, rgba, widthPx, &sink, drawCrisp);
	return st;
}
