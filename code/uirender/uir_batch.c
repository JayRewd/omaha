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

#include <math.h>
#include <string.h>

static uir_batch_backend_t g_batchBackend;
static uir_stats_t        *g_batchStats;
static int                 g_batchEnabled = 1;
static int                 g_fringeEnabled = 1;
static int                 g_targetActive = 0;

static uir_vert_t          g_batchVerts[UIR_BATCH_MAX_VERTS];
static unsigned short      g_batchIdx[UIR_BATCH_MAX_INDEXES];
static int                 g_batchVertCount;
static int                 g_batchIdxCount;
static int                 g_batchShader = -1;

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
	g_batchStats = stats;
	g_batchVertCount = 0;
	g_batchIdxCount = 0;
	g_batchShader = -1;
}

void UIR_BatchFlush(void)
{
	if (g_batchVertCount < 3 || g_batchIdxCount < 3 || !g_batchBackend.draw) {
		g_batchVertCount = 0;
		g_batchIdxCount = 0;
		g_batchShader = -1;
		return;
	}

	g_batchBackend.draw(g_batchVerts, g_batchVertCount, g_batchIdx, g_batchIdxCount, g_batchShader);

	if (g_batchStats) {
		g_batchStats->batches++;
		g_batchStats->batchVerts += g_batchVertCount;
		g_batchStats->batchTris += g_batchIdxCount / 3;
	}

	g_batchVertCount = 0;
	g_batchIdxCount = 0;
	g_batchShader = -1;
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

static uir_status_t uir_batch_append(
	int shader,
	const uir_vert_t *verts,
	int vertCount,
	const unsigned short *idx,
	int idxCount
)
{
	int i;

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

	if (g_batchVertCount + vertCount > UIR_BATCH_MAX_VERTS ||
	    g_batchIdxCount + idxCount > UIR_BATCH_MAX_INDEXES) {
		UIR_BatchFlush();
		g_batchShader = shader;
		if (vertCount > UIR_BATCH_MAX_VERTS || idxCount > UIR_BATCH_MAX_INDEXES) {
			if (g_batchBackend.draw) {
				g_batchBackend.draw(verts, vertCount, idx, idxCount, shader);
				if (g_batchStats) {
					g_batchStats->batches++;
					g_batchStats->batchVerts += vertCount;
					g_batchStats->batchTris += idxCount / 3;
				}
			}
			return UIR_OK;
		}
	}

	for (i = 0; i < vertCount; i++) {
		g_batchVerts[g_batchVertCount + i] = verts[i];
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
	return uir_batch_append(shader, v, nv, idx, ni);
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
	return uir_batch_append(shader, corners, 4, idx, 6);
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

	if (!rgba || !UIR_BatchEnabled()) {
		return UIR_ERR_UNSUPPORTED;
	}
	if (!uir_batch_can_use_shader(shader)) {
		return UIR_ERR_UNSUPPORTED;
	}

	a = uir_batch_byte(rgba->a);
	if (a >= 255 || (w <= 64.0f && h <= 64.0f)) {
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
	if (a >= 255 || (w <= 64.0f && h <= 64.0f)) {
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
	/* Added in OPM: FBO switch may reset GL scissor. */
	UIR_InvalidateAppliedClip();
	if (g_targetActive) {
		return;
	}
	if (!g_batchBackend.targetAvailable || !g_batchBackend.beginTarget) {
		return;
	}
	if (!g_batchBackend.targetAvailable()) {
		return;
	}
	if (!g_batchBackend.beginTarget()) {
		return;
	}
	g_targetActive = 1;
	if (g_batchBackend.targetSamples && g_batchBackend.targetSamples() > 0) {
		UIR_BatchSetFringe(0);
	}
}

void UIR_BatchTargetEnd(void)
{
	if (!g_targetActive) {
		return;
	}
	UIR_BatchFlush();
	if (g_batchBackend.endTarget) {
		g_batchBackend.endTarget();
	}
	g_targetActive = 0;
	UIR_BatchSetFringe(1);
	/* Added in OPM: leaving FBO may reset GL scissor. */
	UIR_InvalidateAppliedClip();
}
