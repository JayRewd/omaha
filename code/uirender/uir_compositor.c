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

#include "uir_compositor.h"
#include "uir_menuworld.h"
#include "uir_draw2d.h"
#include "uir_viewport.h"
#include "uir_batch.h"

#include "../uidesign/uid_profile.h"
#include "../qcommon/q_shared.h"

#include <math.h>
#include <string.h>

static uir_viewport_t        g_vp;
static uir_frame_phase_t     g_phase = UIR_PHASE_IDLE;
static int                   g_realtime;
static uir_queued_preview_t  g_previews[UIR_MAX_PREVIEW_QUEUE];
static int                   g_previewCount;
static uir_layer_fn          g_chromeFn;
static void                 *g_chromeUd;
static uir_layer_fn          g_overlayFn;
static void                 *g_overlayUd;
static uir_stats_t           g_stats;
static uir_rect_t            g_clipStack[UIR_MAX_CLIP_DEPTH];
static int                   g_clipDepth;
/* Added in OPM: skip redundant flush+scissor when clip is unchanged. */
static uir_rect_t            g_appliedClip;
static int                   g_appliedClipValid;
static int                   g_clipDedup = 1;
/* Added in OPM: retained chrome cache (idle blit). */
static uir_chrome_cache_backend_t g_chromeCacheBe;
static int                   g_chromeCacheEnabled;
static int                   g_chromeCacheValid;
static int                   g_chromeCacheRebuild;
static uir_viewport_t        g_chromeCacheVp;
static int                   g_chromeCacheHasVp;
static int                   g_chromeCacheKeepPreviews; /* hit path: keep last preview queue */

void UIR_ChromeCacheSetBackend(const uir_chrome_cache_backend_t *backend)
{
	if (backend) {
		g_chromeCacheBe = *backend;
	} else {
		memset(&g_chromeCacheBe, 0, sizeof(g_chromeCacheBe));
	}
	g_chromeCacheValid = 0;
}

void UIR_SetChromeCache(int enable)
{
	g_chromeCacheEnabled = enable ? 1 : 0;
	if (!g_chromeCacheEnabled) {
		g_chromeCacheValid = 0;
		g_chromeCacheKeepPreviews = 0;
		if (g_chromeCacheBe.invalidate) {
			g_chromeCacheBe.invalidate();
		}
	}
}

void UIR_InvalidateChromeCache(void)
{
	g_chromeCacheValid = 0;
	g_chromeCacheRebuild = 1;
	g_chromeCacheKeepPreviews = 0;
	if (g_chromeCacheBe.invalidate) {
		g_chromeCacheBe.invalidate();
	}
}

void UIR_ChromeCacheRequestRebuild(void)
{
	g_chromeCacheRebuild = 1;
	g_chromeCacheValid = 0;
}

static int uir_chrome_cache_ready(void)
{
	return g_chromeCacheEnabled && g_chromeCacheBe.available && g_chromeCacheBe.beginCapture
		&& g_chromeCacheBe.endCapture && g_chromeCacheBe.blit && g_chromeCacheBe.available();
}

static int uir_viewport_equal(const uir_viewport_t *a, const uir_viewport_t *b)
{
	return a->vpX == b->vpX && a->vpY == b->vpY && a->vpW == b->vpW && a->vpH == b->vpH
		&& fabsf(a->orthoL - b->orthoL) < 0.01f && fabsf(a->orthoT - b->orthoT) < 0.01f
		&& fabsf(a->orthoR - b->orthoR) < 0.01f && fabsf(a->orthoB - b->orthoB) < 0.01f;
}

static void uir_run_chrome_phase(void)
{
	int i;
	int useCache;
	int rebuild;
	int captured;

	UID_ProfileBegin(UID_PROF_HOST_CHROME);
	useCache = uir_chrome_cache_ready();
	rebuild = !useCache || !g_chromeCacheValid || g_chromeCacheRebuild || !g_chromeCacheHasVp
		|| !uir_viewport_equal(&g_chromeCacheVp, &g_vp);
	captured = 0;

	if (useCache) {
		if (rebuild) {
			float uiX = g_vp.orthoL;
			float uiY = g_vp.orthoT;
			float uiW = g_vp.orthoR - g_vp.orthoL;
			float uiH = g_vp.orthoB - g_vp.orthoT;
			if (uiW < 0.0f) {
				uiW = -uiW;
			}
			if (uiH < 0.0f) {
				uiH = -uiH;
			}
			g_previewCount = 0;
			g_chromeCacheKeepPreviews = 0;
			if (g_chromeCacheBe.beginCapture(uiX, uiY, uiW, uiH)) {
				UIR_InvalidateAppliedClip();
				UIR_ResetClipStack();
				if (g_chromeFn) {
					g_chromeFn(g_chromeUd);
				}
				UIR_BatchFlush();
				g_chromeCacheBe.endCapture();
				g_chromeCacheValid = 1;
				g_chromeCacheRebuild = 0;
				g_chromeCacheVp = g_vp;
				g_chromeCacheHasVp = 1;
				g_chromeCacheKeepPreviews = (g_previewCount > 0);
				captured = 1;
			} else {
				/* Fallback: paint directly into the UI target. */
				if (g_chromeFn) {
					g_chromeFn(g_chromeUd);
				}
				g_chromeCacheValid = 0;
			}
		} else if (g_chromeCacheKeepPreviews) {
			/* Refresh realtime on retained preview slots; rects stay from last rebuild. */
			for (i = 0; i < g_previewCount; i++) {
				g_previews[i].params.realtime = g_realtime;
			}
		}
		if (captured || (g_chromeCacheValid && !rebuild)) {
			UIR_InvalidateAppliedClip();
			g_chromeCacheBe.blit();
		}
	} else {
		g_previewCount = 0;
		g_chromeCacheKeepPreviews = 0;
		if (g_chromeFn) {
			g_chromeFn(g_chromeUd);
		}
	}
	g_stats.previewCount = g_previewCount;
	UID_ProfileEnd(UID_PROF_HOST_CHROME);
}

void UIR_SetClipDedup(int enable)
{
	g_clipDedup = enable ? 1 : 0;
	if (!g_clipDedup) {
		g_appliedClipValid = 0;
	}
}

void UIR_InvalidateAppliedClip(void)
{
	g_appliedClipValid = 0;
}

static int uir_clip_rects_equal(const uir_rect_t *a, const uir_rect_t *b)
{
	return fabsf(a->x - b->x) < 0.01f && fabsf(a->y - b->y) < 0.01f && fabsf(a->w - b->w) < 0.01f
		&& fabsf(a->h - b->h) < 0.01f;
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

static void uir_apply_clip_scissor(const uir_rect_t *clip)
{
	int sx, sy, sw, sh, syGl;
	float fx0, fy0, fx1, fy1;

	/* Added in OPM: skip flush+scissor when the logical clip is unchanged. */
	if (g_clipDedup && g_appliedClipValid && uir_clip_rects_equal(clip, &g_appliedClip)) {
		g_stats.clipSkips++;
		return;
	}

	UIR_ViewportDrawToFb(&g_vp, clip->x, clip->y, &fx0, &fy0);
	UIR_ViewportDrawToFb(&g_vp, clip->x + clip->w, clip->y + clip->h, &fx1, &fy1);
	sx = (int)floorf(fx0 < fx1 ? fx0 : fx1);
	sy = (int)floorf(fy0 < fy1 ? fy0 : fy1);
	sw = (int)ceilf(fx0 > fx1 ? fx0 : fx1) - sx;
	sh = (int)ceilf(fy0 > fy1 ? fy0 : fy1) - sy;
	if (sw < 0) {
		sw = 0;
	}
	if (sh < 0) {
		sh = 0;
	}
	/* Fixed in OPM: top-left FB → OpenGL bottom-left scissor Y. */
	syGl = g_vp.vpY + g_vp.vpH - (sy + sh);
	/* UIR_Draw2D_Scissor already flushes the batch. */
	UIR_Draw2D_Scissor(sx, syGl, sw, sh);
	g_appliedClip = *clip;
	g_appliedClipValid = 1;
	g_stats.clipApplies++;
}

void UIR_ResetClipStack(void)
{
	uir_rect_t full;
	g_clipDepth = 0;
	/* Added in OPM: force re-apply of full-viewport scissor. */
	g_appliedClipValid = 0;
	full.x = g_vp.orthoL;
	full.y = g_vp.orthoT;
	full.w = g_vp.orthoR - g_vp.orthoL;
	full.h = g_vp.orthoB - g_vp.orthoT;
	if (full.w < 0.0f) {
		full.w = -full.w;
	}
	if (full.h < 0.0f) {
		full.h = -full.h;
	}
	g_clipStack[0] = full;
	g_clipDepth = 1;
	uir_apply_clip_scissor(&g_clipStack[0]);
}

uir_status_t UIR_PushClipRect(float x, float y, float w, float h)
{
	uir_rect_t next;
	uir_rect_t incoming;

	if (g_clipDepth <= 0) {
		UIR_ResetClipStack();
	}
	if (g_clipDepth >= UIR_MAX_CLIP_DEPTH) {
		return UIR_ERR_OVERFLOW;
	}
	incoming.x = x;
	incoming.y = y;
	incoming.w = w;
	incoming.h = h;
	next = uir_intersect_rect(&g_clipStack[g_clipDepth - 1], &incoming);
	g_clipStack[g_clipDepth++] = next;
	uir_apply_clip_scissor(&next);
	return UIR_OK;
}

void UIR_PopClipRect(void)
{
	if (g_clipDepth <= 1) {
		UIR_ResetClipStack();
		return;
	}
	g_clipDepth--;
	uir_apply_clip_scissor(&g_clipStack[g_clipDepth - 1]);
}

void UIR_CompositorReset(void)
{
	g_phase = UIR_PHASE_IDLE;
	g_previewCount = 0;
	g_clipDepth = 0;
	g_appliedClipValid = 0;
	g_chromeCacheValid = 0;
	g_chromeCacheRebuild = 1;
	g_chromeCacheKeepPreviews = 0;
	g_chromeCacheHasVp = 0;
	memset(&g_stats, 0, sizeof(g_stats));
}

uir_frame_phase_t UIR_CompositorPhase(void)
{
	return g_phase;
}

const uir_viewport_t *UIR_CompositorViewport(void)
{
	return &g_vp;
}

uir_stats_t *UIR_CompositorStats(void)
{
	return &g_stats;
}

void UIR_CompositorSetChromeCallback(uir_layer_fn fn, void *userdata)
{
	g_chromeFn = fn;
	g_chromeUd = userdata;
}

void UIR_CompositorSetOverlayCallback(uir_layer_fn fn, void *userdata)
{
	g_overlayFn = fn;
	g_overlayUd = userdata;
}

static void uir_restore_fullscreen_2d(void)
{
	UIR_Draw2D_Begin(&g_vp);
	UIR_ResetClipStack();
}

uir_status_t UIR_BeginDisconnectedFrame(const uir_viewport_t *vp, int realtime)
{
	uir_rect_t dest;

	if (!vp) {
		return UIR_ERR_INVALID_ARG;
	}
	if (g_phase != UIR_PHASE_IDLE) {
		/* Recover from vid_restart / mid-frame renderer shutdown. */
		UIR_CompositorReset();
	}

	g_vp = *vp;
	g_appliedClipValid = 0;
	g_realtime = realtime;
	/* Preview queue cleared inside uir_run_chrome_phase (cache may retain slots). */
	memset(&g_stats, 0, sizeof(g_stats));
	UIR_BatchBeginFrame(&g_stats);

	g_phase = UIR_PHASE_WORLD;
	dest.x = (float)vp->vpX;
	dest.y = (float)vp->vpY;
	dest.w = (float)vp->vpW;
	dest.h = (float)vp->vpH;
	UID_ProfileBegin(UID_PROF_HOST_WORLD);
	UIR_MenuWorldDraw(&dest, realtime);
	UID_ProfileEnd(UID_PROF_HOST_WORLD);
	uir_restore_fullscreen_2d();

	UIR_BatchTargetBegin();
	g_phase = UIR_PHASE_CHROME;
	uir_run_chrome_phase();
	UID_ProfileBegin(UID_PROF_HOST_BATCH_FLUSH);
	UIR_BatchFlush();
	UID_ProfileEnd(UID_PROF_HOST_BATCH_FLUSH);
	UIR_BatchTargetEnd();

	return UIR_OK;
}

uir_status_t UIR_DrawSolidRect(float x, float y, float w, float h, const uir_color_t *rgba)
{
	if (g_phase != UIR_PHASE_CHROME && g_phase != UIR_PHASE_OVERLAY) {
		return UIR_ERR_WRONG_PHASE;
	}
	return UIR_Draw2D_Box(x, y, w, h, rgba);
}

uir_status_t UIR_FillPolygon2D(const uir_point_t *pts, int count, const uir_color_t *rgba)
{
	if (g_phase != UIR_PHASE_CHROME && g_phase != UIR_PHASE_OVERLAY) {
		return UIR_ERR_WRONG_PHASE;
	}
	return UIR_Draw2D_Polygon(&g_vp, pts, count, rgba, &g_stats);
}

uir_status_t UIR_FillPath2D(const uir_path_t *path, const uir_color_t *rgba, int crisp, int noFringe)
{
	if (g_phase != UIR_PHASE_CHROME && g_phase != UIR_PHASE_OVERLAY) {
		return UIR_ERR_WRONG_PHASE;
	}
	return UIR_Draw2D_Path(&g_vp, path, rgba, &g_stats, crisp, noFringe);
}

/* Added in OPM */
uir_status_t UIR_StrokePath2D(const uir_path_t *path, const uir_color_t *rgba, float widthPx, int crisp)
{
	if (g_phase != UIR_PHASE_CHROME && g_phase != UIR_PHASE_OVERLAY) {
		return UIR_ERR_WRONG_PHASE;
	}
	return UIR_Draw2D_PathStroke(&g_vp, path, rgba, widthPx, &g_stats, crisp);
}

uir_status_t UIR_QueueModelPreview(const uir_rect_t *rect, const uir_model_preview_params_t *params)
{
	uir_queued_preview_t *slot;

	if (g_phase != UIR_PHASE_CHROME) {
		return UIR_ERR_WRONG_PHASE;
	}
	if (!rect || !params) {
		return UIR_ERR_INVALID_ARG;
	}
	if (g_previewCount >= UIR_MAX_PREVIEW_QUEUE) {
		return UIR_ERR_OVERFLOW;
	}
	slot = &g_previews[g_previewCount];
	slot->rect = *rect;
	slot->params = *params;
	slot->params.previewSlotId = g_previewCount;
	if (!slot->params.realtime) {
		slot->params.realtime = g_realtime;
	}
	/* Fixed in OPM: copy anim so host stack strings survive until preview phase. */
	if (params->animName && params->animName[0]) {
		Q_strncpyz(slot->animStorage, params->animName, sizeof(slot->animStorage));
		slot->params.animName = slot->animStorage;
	} else {
		slot->animStorage[0] = '\0';
		slot->params.animName = NULL;
	}
	g_previewCount++;
	g_stats.previewCount = g_previewCount;
	return UIR_OK;
}

uir_status_t UIR_EndDisconnectedFrame(void);

/* Added in OPM: chrome-only frame over live gameplay (no menu-map world). */
uir_status_t UIR_BeginOverlayFrame(const uir_viewport_t *vp, int realtime)
{
	if (!vp) {
		return UIR_ERR_INVALID_ARG;
	}
	if (g_phase != UIR_PHASE_IDLE) {
		UIR_CompositorReset();
	}

	g_vp = *vp;
	g_appliedClipValid = 0;
	g_realtime = realtime;
	memset(&g_stats, 0, sizeof(g_stats));
	UIR_BatchBeginFrame(&g_stats);

	uir_restore_fullscreen_2d();
	UIR_BatchTargetBegin();
	g_phase = UIR_PHASE_CHROME;
	uir_run_chrome_phase();
	UID_ProfileBegin(UID_PROF_HOST_BATCH_FLUSH);
	UIR_BatchFlush();
	UID_ProfileEnd(UID_PROF_HOST_BATCH_FLUSH);
	UIR_BatchTargetEnd();
	return UIR_OK;
}

uir_status_t UIR_EndOverlayFrame(void)
{
	int i;

	if (g_phase != UIR_PHASE_CHROME) {
		return UIR_ERR_WRONG_PHASE;
	}

	UID_ProfileBegin(UID_PROF_HOST_BATCH_FLUSH);
	UIR_BatchFlush();
	UID_ProfileEnd(UID_PROF_HOST_BATCH_FLUSH);
	UIR_BatchTargetEnd();

	/*
	 * Fixed in OPM: connected overlays (pause/team menus) queue the same model
	 * previews as disconnected menus.  The old overlay end path discarded that
	 * queue, so SelectTeam rendered its wood backdrop but no player models.
	 */
	uir_restore_fullscreen_2d();
	g_phase = UIR_PHASE_PREVIEWS;
	UID_ProfileBegin(UID_PROF_HOST_PREVIEWS);
	for (i = 0; i < g_previewCount; i++) {
		UIR_ModelPreviewDraw(&g_previews[i].rect, &g_previews[i].params);
	}
	UID_ProfileEnd(UID_PROF_HOST_PREVIEWS);
	uir_restore_fullscreen_2d();

	UIR_BatchTargetBegin();
	g_phase = UIR_PHASE_OVERLAY;
	UID_ProfileBegin(UID_PROF_HOST_OVERLAY);
	if (g_overlayFn) {
		g_overlayFn(g_overlayUd);
	}
	UID_ProfileEnd(UID_PROF_HOST_OVERLAY);
	UID_ProfileBegin(UID_PROF_HOST_BATCH_FLUSH);
	UIR_BatchFlush();
	UID_ProfileEnd(UID_PROF_HOST_BATCH_FLUSH);
	UIR_BatchTargetEnd();
	g_phase = UIR_PHASE_IDLE;
	g_previewCount = 0;
	return UIR_OK;
}

uir_status_t UIR_EndDisconnectedFrame(void)
{
	int i;

	if (g_phase != UIR_PHASE_CHROME) {
		return UIR_ERR_WRONG_PHASE;
	}

	UID_ProfileBegin(UID_PROF_HOST_BATCH_FLUSH);
	UIR_BatchFlush();
	UID_ProfileEnd(UID_PROF_HOST_BATCH_FLUSH);
	UIR_BatchTargetEnd();

	/* Fixed in OPM: clear leftover chrome scissors before 3D preview viewports. */
	uir_restore_fullscreen_2d();

	g_phase = UIR_PHASE_PREVIEWS;

	UID_ProfileBegin(UID_PROF_HOST_PREVIEWS);
	for (i = 0; i < g_previewCount; i++) {
		UIR_ModelPreviewDraw(&g_previews[i].rect, &g_previews[i].params);
	}
	UID_ProfileEnd(UID_PROF_HOST_PREVIEWS);
	uir_restore_fullscreen_2d();

	UIR_BatchTargetBegin();
	g_phase = UIR_PHASE_OVERLAY;
	UID_ProfileBegin(UID_PROF_HOST_OVERLAY);
	if (g_overlayFn) {
		g_overlayFn(g_overlayUd);
	}
	UID_ProfileEnd(UID_PROF_HOST_OVERLAY);
	UID_ProfileBegin(UID_PROF_HOST_BATCH_FLUSH);
	UIR_BatchFlush();
	UID_ProfileEnd(UID_PROF_HOST_BATCH_FLUSH);
	UIR_BatchTargetEnd();

	g_phase = UIR_PHASE_IDLE;
	g_previewCount = 0;
	return UIR_OK;
}
