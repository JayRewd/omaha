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

#include "uir_batch.h"
#include "uir_compositor.h"
#include "uir_draw2d.h"
#include "uir_viewport.h"

#include "../uidesign/uid_profile.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uir_batch_backend_t g_batchBackend;
static uir_stats_t        *g_batchStats;
static int                 g_batchEnabled = 1;
static int                 g_batchTile = 0; /* Added in Omaha Stage 5: default off */
static int                 g_fringeEnabled = 1;
static int                 g_targetActive = 0;
/* Added in Omaha: TargetBegin requested; bind+clear deferred until first geometry. */
static int                 g_targetPending = 0;
/* Added in Omaha Stage 5: draw-session open (beginDraw/endDraw). */
static int                 g_drawSessionOpen = 0;
/* Added in Omaha: Phase 4.6 — retained target state. */
static int                 g_retainEnabled = 1;
static int                 g_targetRetained = 0;
static int                 g_regionScope = 0;
static int                 g_frameTaint = 0;
static int                 g_retainClaimed = 0;

static uir_vert_t          g_batchVerts[UIR_BATCH_MAX_VERTS];
static unsigned short      g_batchIdx[UIR_BATCH_MAX_INDEXES];
static int                 g_batchVertCount;
static int                 g_batchIdxCount;
static int                 g_batchShader = -1;

/* Added in Omaha Stage 4: retained paint-list recorder. */
static uir_paint_recorder_t g_paintRecorder;
static int                  g_paintRecorderActive;

void UIR_BatchSetPaintRecorder(const uir_paint_recorder_t *recorder)
{
	if (recorder && (recorder->onDraw || recorder->onClip)) {
		g_paintRecorder = *recorder;
		g_paintRecorderActive = 1;
	} else {
		memset(&g_paintRecorder, 0, sizeof(g_paintRecorder));
		g_paintRecorderActive = 0;
	}
}

int UIR_BatchSkipSubmitActive(void)
{
	return (g_paintRecorderActive && g_paintRecorder.skipSubmit) ? 1 : 0;
}

void UIR_BatchNotifyClip(float x, float y, float w, float h)
{
	if (g_paintRecorderActive && g_paintRecorder.onClip) {
		g_paintRecorder.onClip(x, y, w, h, g_paintRecorder.userdata);
	}
}

static unsigned char uir_batch_byte(float v)
{
	int q;

	if (v <= 0.0f) {
		return 0;
	}
	if (v >= 1.0f) {
		return 255;
	}
	q = (int)(v * 255.0f + 0.5f);
	if (q < 0) {
		q = 0;
	}
	if (q > 255) {
		q = 255;
	}
	return (unsigned char)q;
}

void UIR_BatchSetBackend(const uir_batch_backend_t *backend)
{
	if (backend) {
		g_batchBackend = *backend;
	} else {
		memset(&g_batchBackend, 0, sizeof(g_batchBackend));
	}
}

void UIR_BatchSetEnabled(int enabled)
{
	g_batchEnabled = enabled ? 1 : 0;
}

int UIR_BatchEnabled(void)
{
	if (!g_batchBackend.supported || !g_batchBackend.draw) {
		return 0;
	}
	if (!g_batchBackend.supported()) {
		return 0;
	}
	return g_batchEnabled;
}

void UIR_BatchSetTile(int enabled)
{
	g_batchTile = enabled ? 1 : 0;
}

int UIR_BatchTileEnabled(void)
{
	return g_batchTile;
}

void UIR_BatchSetFringe(int enabled)
{
	g_fringeEnabled = enabled ? 1 : 0;
}

int UIR_BatchFringeEnabled(void)
{
	return g_fringeEnabled;
}

void UIR_BatchBeginFrame(uir_stats_t *stats)
{
	if (g_drawSessionOpen && g_batchBackend.endDraw) {
		g_batchBackend.endDraw();
		g_drawSessionOpen = 0;
	}
	g_batchStats = stats;
	g_batchVertCount = 0;
	g_batchIdxCount = 0;
	g_batchShader = -1;
	/* Added in Omaha: Phase 1 — Set2DWindow dedup invalid at frame start. */
	UIR_Draw2DInvalidate();
}

/* Added in Omaha: bind+clear UI FBO on first real draw of a pending session. */
static void uir_batch_ensure_target(void)
{
	if (g_targetActive || !g_targetPending) {
		return;
	}
	if (!g_batchBackend.targetAvailable || !g_batchBackend.beginTarget) {
		g_targetPending = 0;
		return;
	}
	if (!g_batchBackend.targetAvailable()) {
		g_targetPending = 0;
		return;
	}
	if (!g_batchBackend.beginTarget()) {
		g_targetPending = 0;
		return;
	}
	g_targetPending = 0;
	g_targetActive = 1;
	if (g_batchBackend.targetSamples && g_batchBackend.targetSamples() > 0) {
		UIR_BatchSetFringe(0);
	}
	/*
	 * Fixed in Omaha: BeginUI2DTarget disables scissor for the clear. Phase 1 clip
	 * dedup would otherwise keep g_appliedClipValid and skip the next re-apply,
	 * leaving GL scissor disabled or stale across scoreboard→HUD.
	 */
	UIR_InvalidateAppliedClip();
	/* Added in Omaha: Phase 1 — FBO begin may change 2D window. */
	UIR_Draw2DInvalidate();
}

void UIR_BatchFlush(void)
{
	if (g_batchVertCount < 3 || g_batchIdxCount < 3 || !g_batchBackend.draw) {
		g_batchVertCount = 0;
		g_batchIdxCount = 0;
		g_batchShader = -1;
		return;
	}

	if (g_paintRecorderActive && g_paintRecorder.onDraw) {
		g_paintRecorder.onDraw(
			g_batchVerts,
			g_batchVertCount,
			g_batchIdx,
			g_batchIdxCount,
			g_batchShader,
			g_paintRecorder.userdata
		);
	}

	/* Changed in Omaha: live-opacity capture records at mul 1.0; skip the framebuffer
	 * submit so a mid-fade recapture does not flash full alpha. */
	if (!(g_paintRecorderActive && g_paintRecorder.skipSubmit)) {
		uir_batch_ensure_target();
		if (!g_drawSessionOpen && g_batchBackend.beginDraw) {
			g_batchBackend.beginDraw();
			g_drawSessionOpen = 1;
		}
		g_batchBackend.draw(g_batchVerts, g_batchVertCount, g_batchIdx, g_batchIdxCount, g_batchShader);

		if (g_batchStats) {
			g_batchStats->batches++;
			g_batchStats->batchVerts += g_batchVertCount;
			g_batchStats->batchTris += g_batchIdxCount / 3;
		}
	}

	g_batchVertCount = 0;
	g_batchIdxCount = 0;
	g_batchShader = -1;
}

void UIR_BatchCloseDrawSession(void)
{
	if (g_drawSessionOpen && g_batchBackend.endDraw) {
		g_batchBackend.endDraw();
		g_drawSessionOpen = 0;
	}
}

static int uir_batch_can_use_shader(int shader)
{
	if (shader == 0) {
		return 1;
	}
	if (!g_batchBackend.canBatchShader) {
		return 0;
	}
	return g_batchBackend.canBatchShader(shader) ? 1 : 0;
}

static void uir_batch_copy_xform(
	uir_vert_t *dst,
	const uir_vert_t *src,
	int n,
	float dx,
	float dy,
	float alphaMul
)
{
	int i;

	for (i = 0; i < n; i++) {
		dst[i] = src[i];
		dst[i].x += dx;
		dst[i].y += dy;
		if (alphaMul != 1.0f) {
			dst[i].a = uir_batch_byte(((float)dst[i].a / 255.0f) * alphaMul);
		}
	}
}

static uir_status_t uir_batch_append(
	int shader,
	const uir_vert_t *verts,
	int vertCount,
	const unsigned short *idx,
	int idxCount,
	float dx,
	float dy,
	float alphaMul
)
{
	int i;
	const int useXform = (dx != 0.0f || dy != 0.0f || alphaMul != 1.0f);

	if (!verts || vertCount <= 0 || !idx || idxCount < 3) {
		return UIR_ERR_INVALID_ARG;
	}
	if (!uir_batch_can_use_shader(shader)) {
		return UIR_ERR_UNSUPPORTED;
	}

	if (g_batchShader != shader && g_batchVertCount > 0) {
		UIR_BatchFlush();
	}
	g_batchShader = shader;

	/* Phase 4.6: geometry not owned by a tracked region cannot be retained. */
	if (g_regionScope == 0 && !(g_paintRecorderActive && g_paintRecorder.skipSubmit)) {
		g_frameTaint = 1;
	}

	if (g_batchVertCount + vertCount > UIR_BATCH_MAX_VERTS ||
	    g_batchIdxCount + idxCount > UIR_BATCH_MAX_INDEXES) {
		UIR_BatchFlush();
		g_batchShader = shader;
		if (vertCount > UIR_BATCH_MAX_VERTS || idxCount > UIR_BATCH_MAX_INDEXES) {
			if (g_batchBackend.draw) {
				const uir_vert_t *drawVerts = verts;
				uir_vert_t *tmp = NULL;
				if (useXform) {
					tmp = (uir_vert_t *)malloc((size_t)vertCount * sizeof(uir_vert_t));
					if (!tmp) {
						return UIR_ERR_UNSUPPORTED;
					}
					uir_batch_copy_xform(tmp, verts, vertCount, dx, dy, alphaMul);
					drawVerts = tmp;
				}
				if (g_paintRecorderActive && g_paintRecorder.onDraw) {
					g_paintRecorder.onDraw(drawVerts, vertCount, idx, idxCount, shader, g_paintRecorder.userdata);
				}
				if (!(g_paintRecorderActive && g_paintRecorder.skipSubmit)) {
					uir_batch_ensure_target();
					if (!g_drawSessionOpen && g_batchBackend.beginDraw) {
						g_batchBackend.beginDraw();
						g_drawSessionOpen = 1;
					}
					g_batchBackend.draw(drawVerts, vertCount, idx, idxCount, shader);
					if (g_batchStats) {
						g_batchStats->batches++;
						g_batchStats->batchVerts += vertCount;
						g_batchStats->batchTris += idxCount / 3;
					}
				}
				free(tmp);
			}
			return UIR_OK;
		}
	}

	if (useXform) {
		uir_batch_copy_xform(&g_batchVerts[g_batchVertCount], verts, vertCount, dx, dy, alphaMul);
	} else {
		memcpy(&g_batchVerts[g_batchVertCount], verts, (size_t)vertCount * sizeof(uir_vert_t));
	}
	for (i = 0; i < idxCount; i++) {
		g_batchIdx[g_batchIdxCount + i] = (unsigned short)(idx[i] + g_batchVertCount);
	}

	g_batchVertCount += vertCount;
	g_batchIdxCount += idxCount;
	return UIR_OK;
}

uir_status_t UIR_BatchTriangles(
	int shader,
	const uir_vert_t *v,
	int nv,
	const unsigned short *idx,
	int ni
)
{
	if (!UIR_BatchEnabled()) {
		return UIR_ERR_UNSUPPORTED;
	}
	return uir_batch_append(shader, v, nv, idx, ni, 0.0f, 0.0f, 1.0f);
}

uir_status_t UIR_BatchTrianglesOffset(
	int shader,
	const uir_vert_t *v,
	int nv,
	const unsigned short *idx,
	int ni,
	float dx,
	float dy
)
{
	if (!UIR_BatchEnabled()) {
		return UIR_ERR_UNSUPPORTED;
	}
	return uir_batch_append(shader, v, nv, idx, ni, dx, dy, 1.0f);
}

uir_status_t UIR_BatchTrianglesScaledAlpha(
	int shader,
	const uir_vert_t *v,
	int nv,
	const unsigned short *idx,
	int ni,
	float alphaMul
)
{
	if (!UIR_BatchEnabled()) {
		return UIR_ERR_UNSUPPORTED;
	}
	return uir_batch_append(shader, v, nv, idx, ni, 0.0f, 0.0f, alphaMul);
}

static void uir_batch_assign_vert(
	uir_vert_t *v,
	float x,
	float y,
	float s,
	float t,
	unsigned char r,
	unsigned char g,
	unsigned char b,
	unsigned char a
)
{
	v->x = x;
	v->y = y;
	v->s = s;
	v->t = t;
	v->r = r;
	v->g = g;
	v->b = b;
	v->a = a;
}

static uir_status_t uir_batch_append_quad_tris(
	int shader,
	uir_vert_t *corners,
	unsigned char alpha,
	float s0,
	float t0,
	float s1,
	float t1
)
{
	unsigned short idx[6];

	(void)s0;
	(void)t0;
	(void)s1;
	(void)t1;
	(void)alpha;

	idx[0] = 0;
	idx[1] = 1;
	idx[2] = 2;
	idx[3] = 2;
	idx[4] = 1;
	idx[5] = 3;
	return uir_batch_append(shader, corners, 4, idx, 6, 0.0f, 0.0f, 1.0f);
}

static uir_status_t uir_batch_quad_single(
	int shader,
	float x,
	float y,
	float w,
	float h,
	float s0,
	float t0,
	float s1,
	float t1,
	const uir_color_t *rgba
)
{
	uir_vert_t v[4];
	unsigned char r;
	unsigned char g;
	unsigned char b;
	unsigned char a;

	r = uir_batch_byte(rgba->r);
	g = uir_batch_byte(rgba->g);
	b = uir_batch_byte(rgba->b);
	a = uir_batch_byte(rgba->a);

	uir_batch_assign_vert(&v[0], x, y, s0, t0, r, g, b, a);
	uir_batch_assign_vert(&v[1], x + w, y, s1, t0, r, g, b, a);
	uir_batch_assign_vert(&v[2], x, y + h, s0, t1, r, g, b, a);
	uir_batch_assign_vert(&v[3], x + w, y + h, s1, t1, r, g, b, a);

	return uir_batch_append_quad_tris(shader, v, a, s0, t0, s1, t1);
}

uir_status_t UIR_BatchQuad(
	int shader,
	float x,
	float y,
	float w,
	float h,
	float s0,
	float t0,
	float s1,
	float t1,
	const uir_color_t *rgba
)
{
	unsigned char a;
	uir_status_t st;
	float tileSize;
	float yCur;
	float yEnd;
	int isTileCandidate;

	if (!rgba || !UIR_BatchEnabled()) {
		return UIR_ERR_UNSUPPORTED;
	}
	if (!uir_batch_can_use_shader(shader)) {
		return UIR_ERR_UNSUPPORTED;
	}

	a = uir_batch_byte(rgba->a);
	isTileCandidate = (a < 255 && (w > 64.0f || h > 64.0f)) ? 1 : 0;

	/* Stage 5: tiling off by default — one quad matches the tiled math. */
	if (!g_batchTile || !isTileCandidate) {
		return uir_batch_quad_single(shader, x, y, w, h, s0, t0, s1, t1, rgba);
	}

	tileSize = 64.0f;
	yEnd = y + h;
	yCur = y;
	while (yCur < yEnd - 1e-4f) {
		float th = tileSize;
		float xCur;
		float xEnd;
		float ty0;
		float ty1;

		if (yCur + th > yEnd) {
			th = yEnd - yCur;
		}
		ty0 = t0 + ((yCur - y) / h) * (t1 - t0);
		ty1 = t0 + ((yCur + th - y) / h) * (t1 - t0);

		xEnd = x + w;
		xCur = x;
		while (xCur < xEnd - 1e-4f) {
			float tw = tileSize;
			float tx0;
			float tx1;

			if (xCur + tw > xEnd) {
				tw = xEnd - xCur;
			}
			tx0 = s0 + ((xCur - x) / w) * (s1 - s0);
			tx1 = s0 + ((xCur + tw - x) / w) * (s1 - s0);

			st = uir_batch_quad_single(shader, xCur, yCur, tw, th, tx0, ty0, tx1, ty1, rgba);
			if (st != UIR_OK) {
				return st;
			}
			xCur += tw;
		}
		yCur += th;
	}

	return UIR_OK;
}

uir_status_t UIR_BatchQuadSkewed(
	int shader,
	float x,
	float y,
	float w,
	float h,
	float s0,
	float t0,
	float s1,
	float t1,
	const uir_color_t *rgba,
	float skewTan,
	float originY
)
{
	uir_vert_t v[4];
	unsigned char a;
	uir_status_t st;
	float tileSize;
	float yCur;
	float yEnd;

	if (!rgba || !UIR_BatchEnabled()) {
		return UIR_ERR_UNSUPPORTED;
	}
	if (!uir_batch_can_use_shader(shader)) {
		return UIR_ERR_UNSUPPORTED;
	}

	a = uir_batch_byte(rgba->a);
	if (!g_batchTile || a >= 255 || (w <= 64.0f && h <= 64.0f)) {
		float corners[4][2];
		int i;

		corners[0][0] = x;
		corners[0][1] = y;
		corners[1][0] = x + w;
		corners[1][1] = y;
		corners[2][0] = x;
		corners[2][1] = y + h;
		corners[3][0] = x + w;
		corners[3][1] = y + h;

		for (i = 0; i < 4; i++) {
			v[i].x = corners[i][0] + (corners[i][1] - originY) * skewTan;
			v[i].y = corners[i][1];
			v[i].r = uir_batch_byte(rgba->r);
			v[i].g = uir_batch_byte(rgba->g);
			v[i].b = uir_batch_byte(rgba->b);
			v[i].a = a;
		}
		v[0].s = s0;
		v[0].t = t0;
		v[1].s = s1;
		v[1].t = t0;
		v[2].s = s0;
		v[2].t = t1;
		v[3].s = s1;
		v[3].t = t1;

		return uir_batch_append_quad_tris(shader, v, a, s0, t0, s1, t1);
	}

	tileSize = 64.0f;
	yEnd = y + h;
	yCur = y;
	while (yCur < yEnd - 1e-4f) {
		float th = tileSize;
		float xCur;
		float xEnd;
		float ty0;
		float ty1;

		if (yCur + th > yEnd) {
			th = yEnd - yCur;
		}
		ty0 = t0 + ((yCur - y) / h) * (t1 - t0);
		ty1 = t0 + ((yCur + th - y) / h) * (t1 - t0);

		xEnd = x + w;
		xCur = x;
		while (xCur < xEnd - 1e-4f) {
			float tw = tileSize;
			float tx0;
			float tx1;

			if (xCur + tw > xEnd) {
				tw = xEnd - xCur;
			}
			tx0 = s0 + ((xCur - x) / w) * (s1 - s0);
			tx1 = s0 + ((xCur + tw - x) / w) * (s1 - s0);

			st = UIR_BatchQuadSkewed(
				shader,
				xCur,
				yCur,
				tw,
				th,
				tx0,
				ty0,
				tx1,
				ty1,
				rgba,
				skewTan,
				originY
			);
			if (st != UIR_OK) {
				return st;
			}
			xCur += tw;
		}
		yCur += th;
	}

	return UIR_OK;
}

/* Added in Omaha: rotate axis-aligned quad around pivot (clockwise degrees). */
uir_status_t UIR_BatchQuadRotated(
	int shader,
	float x,
	float y,
	float w,
	float h,
	float s0,
	float t0,
	float s1,
	float t1,
	const uir_color_t *rgba,
	float rotationDeg,
	float pivotX,
	float pivotY
)
{
	uir_vert_t v[4];
	unsigned char a;
	float corners[4][2];
	float rad;
	float cosr;
	float sinr;
	int i;

	if (!rgba || !UIR_BatchEnabled()) {
		return UIR_ERR_UNSUPPORTED;
	}
	if (!uir_batch_can_use_shader(shader)) {
		return UIR_ERR_UNSUPPORTED;
	}
	if (rotationDeg == 0.0f) {
		return UIR_BatchQuad(shader, x, y, w, h, s0, t0, s1, t1, rgba);
	}

	a = uir_batch_byte(rgba->a);
	rad = rotationDeg * (3.14159265358979323846f / 180.0f);
	cosr = cosf(rad);
	sinr = sinf(rad);

	corners[0][0] = x;
	corners[0][1] = y;
	corners[1][0] = x + w;
	corners[1][1] = y;
	corners[2][0] = x;
	corners[2][1] = y + h;
	corners[3][0] = x + w;
	corners[3][1] = y + h;

	for (i = 0; i < 4; i++) {
		const float dx = corners[i][0] - pivotX;
		const float dy = corners[i][1] - pivotY;
		v[i].x = pivotX + cosr * dx - sinr * dy;
		v[i].y = pivotY + sinr * dx + cosr * dy;
		v[i].r = uir_batch_byte(rgba->r);
		v[i].g = uir_batch_byte(rgba->g);
		v[i].b = uir_batch_byte(rgba->b);
		v[i].a = a;
	}
	v[0].s = s0;
	v[0].t = t0;
	v[1].s = s1;
	v[1].t = t0;
	v[2].s = s0;
	v[2].t = t1;
	v[3].s = s1;
	v[3].t = t1;

	return uir_batch_append_quad_tris(shader, v, a, s0, t0, s1, t1);
}

void UIR_BatchTargetBegin(void)
{
	UIR_BatchFlush();
	/* Added in Omaha: FBO switch may reset GL scissor. */
	UIR_InvalidateAppliedClip();
	if (g_targetActive || g_targetPending) {
		return;
	}
	if (!g_batchBackend.targetAvailable || !g_batchBackend.beginTarget) {
		return;
	}
	if (!g_batchBackend.targetAvailable()) {
		return;
	}
	/*
	 * Added in Omaha: defer bind+clear until first flush with geometry so empty
	 * overlay/chrome sessions skip a full-res clear+resolve. Immediate begin when
	 * batching is off (draws bypass the batcher).
	 */
	if (UIR_BatchEnabled()) {
		g_targetPending = 1;
		return;
	}
	if (!g_batchBackend.beginTarget()) {
		return;
	}
	g_targetActive = 1;
	if (g_batchBackend.targetSamples && g_batchBackend.targetSamples() > 0) {
		UIR_BatchSetFringe(0);
	}
	/* Added in Omaha: Phase 1 — FBO begin may change 2D window. */
	UIR_Draw2DInvalidate();
}

/* Added in Omaha: Phase 4.6 — retained target. */
void UIR_BatchSetRetain(int enabled)
{
	g_retainEnabled = enabled ? 1 : 0;
}

int UIR_BatchRetainEnabled(void)
{
	return (g_retainEnabled && g_batchBackend.beginTargetKeep && g_batchBackend.clearRectFb) ? 1 : 0;
}

int UIR_BatchTargetBeginKeep(int keep)
{
	int rc;

	UIR_BatchFlush();
	UIR_InvalidateAppliedClip();
	if (g_targetActive || g_targetPending) {
		return g_targetRetained;
	}
	g_targetRetained = 0;
	if (!UIR_BatchRetainEnabled()) {
		UIR_BatchTargetBegin();
		return 0;
	}
	if (!g_batchBackend.targetAvailable || !g_batchBackend.targetAvailable()) {
		return 0;
	}
	rc = g_batchBackend.beginTargetKeep(keep ? 1 : 0);
	if (rc == 0) {
		return 0;
	}
	g_targetActive = 1;
	g_targetRetained = (rc == 2) ? 1 : 0;
	if (g_batchBackend.targetSamples && g_batchBackend.targetSamples() > 0) {
		UIR_BatchSetFringe(0);
	}
	UIR_Draw2DInvalidate();
	return g_targetRetained;
}

int UIR_BatchTargetRetained(void)
{
	return (g_targetActive && g_targetRetained) ? 1 : 0;
}

void UIR_BatchTargetDropRetained(void)
{
	g_frameTaint = 1;
	if (!g_targetActive || !g_targetRetained) {
		return;
	}
	UIR_BatchFlush();
	if (g_batchBackend.clearRectFb) {
		g_batchBackend.clearRectFb(0, 0, 0, 0);
	}
	g_targetRetained = 0;
}

void UIR_BatchRegionScopeBegin(void)
{
	g_regionScope++;
}

void UIR_BatchRegionScopeEnd(void)
{
	if (g_regionScope > 0) {
		g_regionScope--;
	}
}

void UIR_BatchClearRect(float x, float y, float w, float h)
{
	const uir_viewport_t *vp = UIR_CompositorViewport();
	float fx0, fy0, fx1, fy1;
	int   sx, sy, sw, sh, syGl;

	if (!g_targetActive || !g_batchBackend.clearRectFb || !vp || w <= 0.0f || h <= 0.0f) {
		return;
	}
	UIR_BatchFlush();
	UIR_ViewportDrawToFb(vp, x, y, &fx0, &fy0);
	UIR_ViewportDrawToFb(vp, x + w, y + h, &fx1, &fy1);
	sx = (int)floorf(fx0 < fx1 ? fx0 : fx1);
	sy = (int)floorf(fy0 < fy1 ? fy0 : fy1);
	sw = (int)ceilf(fx0 > fx1 ? fx0 : fx1) - sx;
	sh = (int)ceilf(fy0 > fy1 ? fy0 : fy1) - sy;
	if (sw <= 0 || sh <= 0) {
		return;
	}
	/* Top-left FB → OpenGL bottom-left window Y. */
	syGl = vp->vpY + vp->vpH - (sy + sh);
	g_batchBackend.clearRectFb(sx, syGl, sw, sh);
}

void UIR_BatchNoteExternalDraw(void)
{
	g_frameTaint = 1;
}

void UIR_BatchRetainClaim(void)
{
	g_retainClaimed = 1;
}

int UIR_BatchRetainClaimed(void)
{
	return g_retainClaimed;
}

int UIR_BatchFrameTainted(void)
{
	return g_frameTaint;
}

void UIR_BatchResetTaint(void)
{
	g_frameTaint = 0;
	g_retainClaimed = 0;
}

void UIR_BatchTargetEnd(void)
{
	UIR_BatchFlush();
	if (g_targetActive) {
		/* Added in Omaha debug: time MSAA resolve / FBO blit (was outside profile). */
		UID_ProfileBegin(UID_PROF_HOST_BATCH_FLUSH);
		if (g_batchBackend.endTarget) {
			g_batchBackend.endTarget();
		}
		UID_ProfileEnd(UID_PROF_HOST_BATCH_FLUSH);
		g_targetActive = 0;
		g_targetRetained = 0;
		UIR_BatchSetFringe(1);
	/* Added in Omaha: leaving FBO may reset GL scissor. */
		UIR_InvalidateAppliedClip();
		/* Added in Omaha: Phase 1 — EndUI2DTarget calls Set2DWindow. */
		UIR_Draw2DInvalidate();
	}
	g_targetPending = 0;
	if (g_drawSessionOpen && g_batchBackend.endDraw) {
		g_batchBackend.endDraw();
		g_drawSessionOpen = 0;
	}
}
