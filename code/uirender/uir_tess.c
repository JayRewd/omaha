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

#include "uir_tess.h"
#include "uir_debug.h"
#include "uir_viewport.h"

#include "tesselator.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define UIR_TESS_MAX_POINTS   4096
#define UIR_TESS_MITER_LIMIT  2.0f
#define UIR_TESS_FRINGE_STEPS 2
#define UIR_TESS_FRINGE_EDGE_TILE 64.0f
#define UIR_TESS_MAX_EDGES    8192

typedef struct {
	float x;
	float y;
} uir_xy_t;

static uir_xy_t s_drawPts[UIR_TESS_MAX_POINTS];
static uir_xy_t s_fringeRings[UIR_TESS_FRINGE_STEPS][UIR_TESS_MAX_POINTS];
static float    s_outerX[UIR_TESS_MAX_POINTS + 4];
static float    s_outerY[UIR_TESS_MAX_POINTS + 4];
static float    s_innerX[UIR_TESS_MAX_POINTS + 4];
static float    s_innerY[UIR_TESS_MAX_POINTS + 4];
static float    s_ofringeX[UIR_TESS_FRINGE_STEPS][UIR_TESS_MAX_POINTS + 4];
static float    s_ofringeY[UIR_TESS_FRINGE_STEPS][UIR_TESS_MAX_POINTS + 4];
static float    s_ifringeX[UIR_TESS_FRINGE_STEPS][UIR_TESS_MAX_POINTS + 4];
static float    s_ifringeY[UIR_TESS_FRINGE_STEPS][UIR_TESS_MAX_POINTS + 4];
static uir_stats_t *s_tessStats;
static int          s_inFringeLoop;

static const unsigned char s_fringeAlphas[UIR_TESS_FRINGE_STEPS] = {255, 0};

static float uir_tess_default_fringe_fb_px(float avgScale)
{
	float px;

	if (avgScale < 1e-6f) {
		avgScale = 1.0f;
	}
	px = avgScale * 1.0f;
	if (px < 0.5f) {
		px = 0.5f;
	}
	if (px > 1.0f) {
		px = 1.0f;
	}
	return px;
}

float UIR_TessDefaultFringeFbPx(float avgScale)
{
	return uir_tess_default_fringe_fb_px(avgScale);
}

static float uir_tess_clampf(float v, float lo, float hi)
{
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

static unsigned char uir_tess_byte(float v)
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

static float uir_tess_shoelace(const uir_xy_t *pts, int count)
{
	float area = 0.0f;
	int i;

	for (i = 0; i < count; i++) {
		const uir_xy_t *a = &pts[i];
		const uir_xy_t *b = &pts[(i + 1) % count];
		area += a->x * b->y - b->x * a->y;
	}
	return area * 0.5f;
}

static void uir_tess_put_vert(
	uir_vert_t *dst,
	float dx,
	float dy,
	const uir_color_t *rgba,
	unsigned char alphaByte
)
{
	dst->x = dx;
	dst->y = dy;
	dst->s = 0.0f;
	dst->t = 0.0f;
	dst->r = uir_tess_byte(rgba->r);
	dst->g = uir_tess_byte(rgba->g);
	dst->b = uir_tess_byte(rgba->b);
	dst->a = alphaByte;
}

void UIR_TessSetStats(uir_stats_t *stats)
{
	s_tessStats = stats;
}

static void *uir_tess_alloc(void *userData, unsigned int size)
{
	(void)userData;
	return malloc(size);
}

static void uir_tess_free(void *userData, void *ptr)
{
	(void)userData;
	free(ptr);
}

static int uir_tess_dedupe_contour_draw(const uir_contour_t *contour, uir_xy_t *out, int maxOut)
{
	int count = contour->count;
	int i;
	int w = 0;

	if (count < 3 || count > maxOut) {
		return -1;
	}
	for (i = 0; i < count; i++) {
		out[i].x = contour->points[i].x;
		out[i].y = contour->points[i].y;
	}
	for (i = 0; i < count; i++) {
		int j = (i + 1) % count;
		if (fabsf(out[i].x - out[j].x) < 1e-4f && fabsf(out[i].y - out[j].y) < 1e-4f) {
			continue;
		}
		out[w++] = out[i];
	}
	return w >= 3 ? w : -1;
}

static int uir_tess_prepare_fill_contour(const uir_contour_t *contour, uir_xy_t *out, int maxOut)
{
	int n = uir_tess_dedupe_contour_draw(contour, out, maxOut);

	if (n < 3) {
		return -1;
	}

	/* SVG fill implicitly closes open subpaths (matches UIR_PathContainsPoint). */
	if (!contour->closed) {
		const uir_xy_t *first = &out[0];
		const uir_xy_t *last = &out[n - 1];

		if (fabsf(first->x - last->x) > 1e-4f || fabsf(first->y - last->y) > 1e-4f) {
			if (n + 1 > maxOut) {
				return -1;
			}
			out[n] = *first;
			n++;
		}
	}

	return n;
}

static uir_status_t uir_tess_append_tri(
	uir_vert_t *verts,
	unsigned short *idx,
	int *nv,
	int *ni,
	int maxVerts,
	int maxIdx,
	float ax,
	float ay,
	float bx,
	float by,
	float cx,
	float cy,
	const uir_color_t *rgba,
	unsigned char alphaByte
)
{
	int b = *nv;

	if (b + 3 > maxVerts || *ni + 3 > maxIdx) {
		return UIR_ERR_OVERFLOW;
	}
	uir_tess_put_vert(&verts[b + 0], ax, ay, rgba, alphaByte);
	uir_tess_put_vert(&verts[b + 1], bx, by, rgba, alphaByte);
	uir_tess_put_vert(&verts[b + 2], cx, cy, rgba, alphaByte);
	idx[*ni + 0] = (unsigned short)(b + 0);
	idx[*ni + 1] = (unsigned short)(b + 1);
	idx[*ni + 2] = (unsigned short)(b + 2);
	*nv = b + 3;
	*ni += 3;
	return UIR_OK;
}

static uir_status_t uir_tess_append_quad(
	uir_vert_t *verts,
	unsigned short *idx,
	int *nv,
	int *ni,
	int maxVerts,
	int maxIdx,
	float x0,
	float y0,
	float x1,
	float y1,
	float x2,
	float y2,
	float x3,
	float y3,
	const uir_color_t *rgba,
	unsigned char a0,
	unsigned char a1,
	unsigned char a2,
	unsigned char a3
)
{
	int b = *nv;
	unsigned char minA;
	unsigned char maxA;

	if (b + 4 > maxVerts || *ni + 6 > maxIdx) {
		return UIR_ERR_OVERFLOW;
	}

	minA = a0;
	maxA = a0;
	if (a1 < minA) {
		minA = a1;
	}
	if (a2 < minA) {
		minA = a2;
	}
	if (a3 < minA) {
		minA = a3;
	}
	if (a1 > maxA) {
		maxA = a1;
	}
	if (a2 > maxA) {
		maxA = a2;
	}
	if (a3 > maxA) {
		maxA = a3;
	}

	uir_tess_put_vert(&verts[b + 0], x0, y0, rgba, a0);
	uir_tess_put_vert(&verts[b + 1], x1, y1, rgba, a1);
	uir_tess_put_vert(&verts[b + 2], x2, y2, rgba, a2);
	uir_tess_put_vert(&verts[b + 3], x3, y3, rgba, a3);
	if (s_inFringeLoop || maxA != minA) {
		/* Perimeter strip: innerStart, outerStart, outerEnd, innerEnd (fringe + stroke). */
		idx[*ni + 0] = (unsigned short)(b + 0);
		idx[*ni + 1] = (unsigned short)(b + 1);
		idx[*ni + 2] = (unsigned short)(b + 2);
		idx[*ni + 3] = (unsigned short)(b + 3);
		idx[*ni + 4] = (unsigned short)(b + 2);
		idx[*ni + 5] = (unsigned short)(b + 1);
	} else if (minA < 255) {
		/* Translucent axis fill: TL, TR, BL, BR — short edge TR–BR (1–3). */
		idx[*ni + 0] = (unsigned short)(b + 0);
		idx[*ni + 1] = (unsigned short)(b + 1);
		idx[*ni + 2] = (unsigned short)(b + 3);
		idx[*ni + 3] = (unsigned short)(b + 1);
		idx[*ni + 4] = (unsigned short)(b + 2);
		idx[*ni + 5] = (unsigned short)(b + 3);
	} else {
		idx[*ni + 0] = (unsigned short)(b + 0);
		idx[*ni + 1] = (unsigned short)(b + 1);
		idx[*ni + 2] = (unsigned short)(b + 2);
		idx[*ni + 3] = (unsigned short)(b + 0);
		idx[*ni + 4] = (unsigned short)(b + 2);
		idx[*ni + 5] = (unsigned short)(b + 3);
	}
	*nv = b + 4;
	*ni += 6;
	return UIR_OK;
}

static void uir_tess_offset_ring(
	const float *bx,
	const float *by,
	int count,
	float orient,
	float dist,
	uir_xy_t *out
)
{
	int i;

	for (i = 0; i < count; i++) {
		float pPrevX = (i > 0) ? bx[i - 1] : bx[count - 1];
		float pPrevY = (i > 0) ? by[i - 1] : by[count - 1];
		float px = bx[i];
		float py = by[i];
		float pNextX = (i + 1 < count) ? bx[i + 1] : bx[0];
		float pNextY = (i + 1 < count) ? by[i + 1] : by[0];
		float d0x = px - pPrevX;
		float d0y = py - pPrevY;
		float d1x = pNextX - px;
		float d1y = pNextY - py;
		float len0 = sqrtf(d0x * d0x + d0y * d0y);
		float len1 = sqrtf(d1x * d1x + d1y * d1y);
		float mx;
		float my;

		if (len0 < 1e-6f || len1 < 1e-6f) {
			out[i].x = bx[i];
			out[i].y = by[i];
			continue;
		}
		d0x /= len0;
		d0y /= len0;
		d1x /= len1;
		d1y /= len1;
		mx = orient * (d0y + d1y);
		my = orient * (-(d0x + d1x));
		{
			float len = sqrtf(mx * mx + my * my);

			if (len > 1e-6f) {
				out[i].x = bx[i] + mx / len * dist;
				out[i].y = by[i] + my / len * dist;
			} else {
				out[i].x = bx[i] + orient * d1y * dist;
				out[i].y = by[i] + orient * (-d1x) * dist;
			}
		}
	}
}

static int uir_tess_fringe_point_count(const float *bx, const float *by, int count)
{
	if (count >= 2 &&
	    fabsf(bx[count - 1] - bx[0]) < 1e-4f &&
	    fabsf(by[count - 1] - by[0]) < 1e-4f) {
		return count - 1;
	}
	return count;
}

static float uir_tess_quad_max_span(
	float x0,
	float y0,
	float x1,
	float y1,
	float x2,
	float y2,
	float x3,
	float y3
)
{
	float xs[4] = {x0, x1, x2, x3};
	float ys[4] = {y0, y1, y2, y3};
	float maxSpan = 0.0f;
	int ei;
	int ej;

	for (ei = 0; ei < 4; ei++) {
		for (ej = ei + 1; ej < 4; ej++) {
			float dx = xs[ej] - xs[ei];
			float dy = ys[ej] - ys[ei];
			float len = sqrtf(dx * dx + dy * dy);

			if (len > maxSpan) {
				maxSpan = len;
			}
		}
	}
	return maxSpan;
}

static float uir_tess_tri_max_edge(
	float x0,
	float y0,
	float x1,
	float y1,
	float x2,
	float y2
)
{
	float e01 = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
	float e12 = sqrtf((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
	float e20 = sqrtf((x0 - x2) * (x0 - x2) + (y0 - y2) * (y0 - y2));
	float maxE = e01;

	if (e12 > maxE) {
		maxE = e12;
	}
	if (e20 > maxE) {
		maxE = e20;
	}
	return maxE;
}

static float uir_tess_quad_tri_max_edge(
	float x0,
	float y0,
	float x1,
	float y1,
	float x2,
	float y2,
	float x3,
	float y3
)
{
	float t0;
	float t1;

	if (s_inFringeLoop) {
		t0 = uir_tess_tri_max_edge(x0, y0, x1, y1, x2, y2);
		t1 = uir_tess_tri_max_edge(x3, y3, x2, y2, x1, y1);
	} else {
		t0 = uir_tess_tri_max_edge(x0, y0, x1, y1, x3, y3);
		t1 = uir_tess_tri_max_edge(x1, y1, x2, y2, x3, y3);
	}
	return (t1 > t0) ? t1 : t0;
}

static float uir_tess_fringe_edge_limit(float edgeLen, float fringeDraw)
{
	float limit = edgeLen * 1.02f + fringeDraw * 4.0f;

	if (limit < fringeDraw * 8.0f) {
		limit = fringeDraw * 8.0f;
	}
	return limit;
}

static void uir_tess_lerp_xy(
	const uir_xy_t *a,
	const uir_xy_t *b,
	float t,
	uir_xy_t *out
)
{
	out->x = a->x + (b->x - a->x) * t;
	out->y = a->y + (b->y - a->y) * t;
}

static uir_status_t uir_tess_emit_fringe_strip_quad(
	const uir_xy_t *innerStart,
	const uir_xy_t *outerStart,
	const uir_xy_t *outerEnd,
	const uir_xy_t *innerEnd,
	unsigned char a0,
	unsigned char a1,
	float quadSpanLimit,
	float edgeLen,
	int contourCount,
	const uir_color_t *rgba,
	uir_vert_t *verts,
	int *nv,
	unsigned short *idx,
	int *ni,
	int maxVerts,
	int maxIdx
)
{
	float quadSpan;
	float triMaxEdge;
	uir_status_t st;

	quadSpan = uir_tess_quad_max_span(
		innerStart->x,
		innerStart->y,
		outerStart->x,
		outerStart->y,
		outerEnd->x,
		outerEnd->y,
		innerEnd->x,
		innerEnd->y
	);
	triMaxEdge = uir_tess_quad_tri_max_edge(
		innerStart->x,
		innerStart->y,
		outerStart->x,
		outerStart->y,
		outerEnd->x,
		outerEnd->y,
		innerEnd->x,
		innerEnd->y
	);
	if (quadSpan > quadSpanLimit || triMaxEdge > quadSpanLimit) {
		return UIR_OK;
	}

	st = uir_tess_append_quad(
		verts,
		idx,
		nv,
		ni,
		maxVerts,
		maxIdx,
		innerStart->x,
		innerStart->y,
		outerStart->x,
		outerStart->y,
		outerEnd->x,
		outerEnd->y,
		innerEnd->x,
		innerEnd->y,
		rgba,
		a0,
		a0,
		a1,
		a1
	);
	return st;
}

static uir_status_t uir_tess_emit_fringe_loop(
	const float *bx,
	const float *by,
	int count,
	float orient,
	float fringeDraw,
	const uir_color_t *rgba,
	unsigned char innerAlpha,
	uir_vert_t *verts,
	int *nv,
	unsigned short *idx,
	int *ni,
	int maxVerts,
	int maxIdx
)
{
	int step;
	int i;
	uir_status_t st;

	if (count < 3 || fringeDraw <= 0.0f) {
		return UIR_OK;
	}

	count = uir_tess_fringe_point_count(bx, by, count);
	if (count < 3) {
		return UIR_OK;
	}

	s_inFringeLoop = 1;

	for (step = 0; step < UIR_TESS_FRINGE_STEPS; step++) {
		float dist = fringeDraw * ((float)step / (float)(UIR_TESS_FRINGE_STEPS - 1));
		uir_tess_offset_ring(
			bx,
			by,
			count,
			orient,
			dist,
			s_fringeRings[step]
		);
	}

	for (i = 0; i < count; i++) {
		int iNext = (i + 1) % count;
		int stepIdx;
		int subSegs;
		int sub;
		float edx = bx[iNext] - bx[i];
		float edy = by[iNext] - by[i];
		float edgeLen;

		if (edx * edx + edy * edy < 1e-8f) {
			continue;
		}

		edgeLen = sqrtf(edx * edx + edy * edy);
		subSegs = 1;
		if (edgeLen > UIR_TESS_FRINGE_EDGE_TILE) {
			subSegs = (int)ceilf(edgeLen / UIR_TESS_FRINGE_EDGE_TILE);
		}

		for (sub = 0; sub < subSegs; sub++) {
			float t0 = (float)sub / (float)subSegs;
			float t1 = (float)(sub + 1) / (float)subSegs;
			float subEdgeLen = edgeLen * (t1 - t0);

			for (stepIdx = 0; stepIdx < UIR_TESS_FRINGE_STEPS - 1; stepIdx++) {
				uir_xy_t innerStart;
				uir_xy_t innerEnd;
				uir_xy_t outerStart;
				uir_xy_t outerEnd;
				unsigned char a0 = (stepIdx == 0) ? innerAlpha : s_fringeAlphas[stepIdx];
				unsigned char a1 = s_fringeAlphas[stepIdx + 1];
				uir_status_t st;

				uir_tess_lerp_xy(&s_fringeRings[stepIdx][i], &s_fringeRings[stepIdx][iNext], t0, &innerStart);
				uir_tess_lerp_xy(&s_fringeRings[stepIdx][i], &s_fringeRings[stepIdx][iNext], t1, &innerEnd);
				uir_tess_lerp_xy(&s_fringeRings[stepIdx + 1][i], &s_fringeRings[stepIdx + 1][iNext], t0, &outerStart);
				uir_tess_lerp_xy(&s_fringeRings[stepIdx + 1][i], &s_fringeRings[stepIdx + 1][iNext], t1, &outerEnd);

				st = uir_tess_emit_fringe_strip_quad(
					&innerStart,
					&outerStart,
					&outerEnd,
					&innerEnd,
					a0,
					a1,
					uir_tess_fringe_edge_limit(subEdgeLen, fringeDraw),
					subEdgeLen,
					count,
					rgba,
					verts,
					nv,
					idx,
					ni,
					maxVerts,
					maxIdx
				);
				if (st != UIR_OK) {
					return st;
				}
			}
		}
	}

	s_inFringeLoop = 0;
	return UIR_OK;
}

static uir_status_t uir_tess_emit_contour_fringes(
	int numContours,
	const int *contourCounts,
	const uir_xy_t *contourPtrs[],
	float fringeDraw,
	const uir_color_t *rgba,
	unsigned char innerAlpha,
	uir_vert_t *verts,
	int *nv,
	unsigned short *idx,
	int *ni,
	int maxVerts,
	int maxIdx
)
{
	int ci;

	if (fringeDraw <= 0.0f || numContours <= 0) {
		return UIR_OK;
	}

	for (ci = 0; ci < numContours; ci++) {
		int n = contourCounts[ci];
		const uir_xy_t *pts = contourPtrs[ci];
		float area;
		float orient;
		int i;
		uir_status_t st;

		if (n < 3) {
			continue;
		}

		for (i = 0; i < n; i++) {
			s_outerX[i] = pts[i].x;
			s_outerY[i] = pts[i].y;
		}

		area = uir_tess_shoelace(pts, n);
		orient = area >= 0.0f ? 1.0f : -1.0f;

		st = uir_tess_emit_fringe_loop(
			s_outerX,
			s_outerY,
			n,
			orient,
			fringeDraw,
			rgba,
			innerAlpha,
			verts,
			nv,
			idx,
			ni,
			maxVerts,
			maxIdx
		);
		if (st != UIR_OK) {
			return st;
		}
	}

	return UIR_OK;
}

static uir_status_t uir_tess_libtess2_fill(
	int numContours,
	const int *contourCounts,
	const uir_xy_t *contourPts[],
	int winding,
	const uir_color_t *rgba,
	uir_vert_t *verts,
	int maxVerts,
	int *outVerts,
	unsigned short *idx,
	int maxIdx,
	int *outIdx
)
{
	TESSalloc alloc;
	TESStesselator *tessHandle;
	const float *tessVerts;
	const TESSindex *elems;
	int elemCount;
	int ci;
	int vertCount = 0;
	int idxCount = 0;
	unsigned char innerAlpha;
	uir_status_t st;
	int warned = 0;

	memset(&alloc, 0, sizeof(alloc));
	alloc.memalloc = uir_tess_alloc;
	alloc.memfree = uir_tess_free;
	alloc.extraVertices = 256;

	tessHandle = tessNewTess(&alloc);
	if (!tessHandle) {
		if (s_tessStats) {
			s_tessStats->tessLibFails++;
		}
		return UIR_ERR_UNSUPPORTED;
	}

	for (ci = 0; ci < numContours; ci++) {
		tessAddContour(tessHandle, 2, contourPts[ci], sizeof(float) * 2, contourCounts[ci]);
		if (tessGetStatus(tessHandle) != TESS_STATUS_OK) {
			tessDeleteTess(tessHandle);
			if (s_tessStats) {
				s_tessStats->tessLibFails++;
			}
			if (UIR_DebugEnabled() && !warned) {
				warned = 1;
				UIR_DebugPrintf("UIR: libtess2 tessAddContour failed (contours=%d ci=%d)\n", numContours, ci);
			}
			return UIR_ERR_UNSUPPORTED;
		}
	}

	if (!tessTesselate(tessHandle, winding, TESS_POLYGONS, 3, 2, NULL)) {
		tessDeleteTess(tessHandle);
		if (s_tessStats) {
			s_tessStats->tessLibFails++;
		}
		if (UIR_DebugEnabled() && !warned) {
			warned = 1;
			UIR_DebugPrintf("UIR: libtess2 tessTesselate failed (contours=%d)\n", numContours);
		}
		return UIR_ERR_UNSUPPORTED;
	}

	tessVerts = tessGetVertices(tessHandle);
	elems = tessGetElements(tessHandle);
	elemCount = tessGetElementCount(tessHandle);
	innerAlpha = uir_tess_byte(rgba->a);

	for (ci = 0; ci < elemCount; ci++) {
		int i0 = elems[ci * 3 + 0];
		int i1 = elems[ci * 3 + 1];
		int i2 = elems[ci * 3 + 2];

		if (i0 == TESS_UNDEF || i1 == TESS_UNDEF || i2 == TESS_UNDEF) {
			continue;
		}
		st = uir_tess_append_tri(
			verts,
			idx,
			&vertCount,
			&idxCount,
			maxVerts,
			maxIdx,
			tessVerts[i0 * 2 + 0],
			tessVerts[i0 * 2 + 1],
			tessVerts[i1 * 2 + 0],
			tessVerts[i1 * 2 + 1],
			tessVerts[i2 * 2 + 0],
			tessVerts[i2 * 2 + 1],
			rgba,
			innerAlpha
		);
		if (st != UIR_OK) {
			tessDeleteTess(tessHandle);
			return st;
		}
	}

	tessDeleteTess(tessHandle);

	if (idxCount == 0) {
		return UIR_ERR_UNSUPPORTED;
	}

	*outVerts = vertCount;
	*outIdx = idxCount;
	return UIR_OK;
}

static int uir_tess_contour_is_axis_rect(
	const uir_xy_t *pts,
	int n,
	float *rx,
	float *ry,
	float *rw,
	float *rh
)
{
	float minX;
	float maxX;
	float minY;
	float maxY;
	int nUse;
	int i;
	const float eps = 1e-3f;

	nUse = n;
	if (nUse >= 2 &&
	    fabsf(pts[nUse - 1].x - pts[0].x) < eps &&
	    fabsf(pts[nUse - 1].y - pts[0].y) < eps) {
		nUse--;
	}
	if (nUse != 4) {
		return 0;
	}

	minX = maxX = pts[0].x;
	minY = maxY = pts[0].y;
	for (i = 1; i < 4; i++) {
		if (pts[i].x < minX) {
			minX = pts[i].x;
		}
		if (pts[i].x > maxX) {
			maxX = pts[i].x;
		}
		if (pts[i].y < minY) {
			minY = pts[i].y;
		}
		if (pts[i].y > maxY) {
			maxY = pts[i].y;
		}
	}
	*rw = maxX - minX;
	*rh = maxY - minY;
	if (*rw < eps || *rh < eps) {
		return 0;
	}
	for (i = 0; i < 4; i++) {
		int onVert = (fabsf(pts[i].x - minX) < eps || fabsf(pts[i].x - maxX) < eps);
		int onHoriz = (fabsf(pts[i].y - minY) < eps || fabsf(pts[i].y - maxY) < eps);

		if (!onVert || !onHoriz) {
			return 0;
		}
	}
	*rx = minX;
	*ry = minY;
	return 1;
}

static uir_status_t uir_tess_emit_axis_rect_fill_tiled(
	float x,
	float y,
	float w,
	float h,
	const uir_color_t *rgba,
	uir_vert_t *verts,
	int maxVerts,
	int *nv,
	unsigned short *idx,
	int maxIdx,
	int *ni
)
{
	float tile = UIR_TESS_FRINGE_EDGE_TILE;
	unsigned char a = uir_tess_byte(rgba->a);
	float yCur = y;
	float yEnd = y + h;

	while (yCur < yEnd - 1e-4f) {
		float th = tile;
		float xCur;
		float xEnd;

		if (yCur + th > yEnd) {
			th = yEnd - yCur;
		}
		xEnd = x + w;
		xCur = x;
		while (xCur < xEnd - 1e-4f) {
			float tw = tile;
			uir_status_t st;

			if (xCur + tw > xEnd) {
				tw = xEnd - xCur;
			}
			st = uir_tess_append_quad(
				verts,
				idx,
				nv,
				ni,
				maxVerts,
				maxIdx,
				xCur,
				yCur,
				xCur + tw,
				yCur,
				xCur,
				yCur + th,
				xCur + tw,
				yCur + th,
				rgba,
				a,
				a,
				a,
				a
			);
			if (st != UIR_OK) {
				return st;
			}
			xCur += tw;
		}
		yCur += th;
	}
	if (*ni <= 0) {
		return UIR_ERR_UNSUPPORTED;
	}
	return UIR_OK;
}

uir_status_t UIR_TessFillPath(
	const uir_viewport_t *vp,
	const uir_path_t     *path,
	const uir_color_t    *rgba,
	float                 fringeFbPx,
	uir_vert_t           *verts,
	int                   maxVerts,
	int                  *outVerts,
	unsigned short       *idx,
	int                   maxIdx,
	int                  *outIdx
)
{
	int contourCounts[UIR_MAX_CONTOURS];
	const uir_xy_t *contourPtrs[UIR_MAX_CONTOURS];
	int numActive = 0;
	int ci;
	int winding;
	float avgScale;
	float fringeDraw;
	int skipped = 0;

	if (!vp || !path || !rgba || !verts || !outVerts || !idx || !outIdx) {
		return UIR_ERR_INVALID_ARG;
	}
	if (path->contourCount <= 0) {
		return UIR_ERR_UNSUPPORTED;
	}

	*outVerts = 0;
	*outIdx = 0;

	if (s_tessStats) {
		s_tessStats->tessContoursIn += path->contourCount;
	}

	{
		int offset = 0;
		for (ci = 0; ci < path->contourCount && numActive < UIR_MAX_CONTOURS; ci++) {
			int n = uir_tess_prepare_fill_contour(&path->contours[ci], &s_drawPts[offset], UIR_TESS_MAX_POINTS - offset);
			if (n < 0) {
				skipped++;
				continue;
			}
			contourCounts[numActive] = n;
			contourPtrs[numActive] = &s_drawPts[offset];
			offset += n;
			numActive++;
		}
	}

	if (s_tessStats) {
		s_tessStats->tessSkippedContours += skipped;
		s_tessStats->tessContoursOut += numActive;
	}

	if (numActive <= 0) {
		return UIR_ERR_UNSUPPORTED;
	}

	winding = (path->fillRule == UIR_FILL_EVEN_ODD) ? TESS_WINDING_ODD : TESS_WINDING_NONZERO;

	avgScale = 0.5f * (vp->scaleX + vp->scaleY);
	if (avgScale < 1e-6f) {
		avgScale = 1.0f;
	}
	if (fringeFbPx > 0.0f) {
		fringeDraw = fringeFbPx / avgScale;
	} else {
		fringeDraw = 0.0f;
	}

	{
		uir_status_t st;

		if (numActive == 1) {
			float rx;
			float ry;
			float rw;
			float rh;

			if (uir_tess_contour_is_axis_rect(contourPtrs[0], contourCounts[0], &rx, &ry, &rw, &rh)) {
				st = uir_tess_emit_axis_rect_fill_tiled(
					rx,
					ry,
					rw,
					rh,
					rgba,
					verts,
					maxVerts,
					outVerts,
					idx,
					maxIdx,
					outIdx
				);
				if (st == UIR_OK) {
					if (fringeDraw > 0.0f) {
						return uir_tess_emit_contour_fringes(
							numActive,
							contourCounts,
							contourPtrs,
							fringeDraw,
							rgba,
							uir_tess_byte(rgba->a),
							verts,
							outVerts,
							idx,
							outIdx,
							maxVerts,
							maxIdx
						);
					}
					return UIR_OK;
				}
			}
		}

		st = uir_tess_libtess2_fill(
			numActive,
			contourCounts,
			contourPtrs,
			winding,
			rgba,
			verts,
			maxVerts,
			outVerts,
			idx,
			maxIdx,
			outIdx
		);
		if (st != UIR_OK) {
			return st;
		}
		if (fringeDraw > 0.0f) {
			return uir_tess_emit_contour_fringes(
				numActive,
				contourCounts,
				contourPtrs,
				fringeDraw,
				rgba,
				uir_tess_byte(rgba->a),
				verts,
				outVerts,
				idx,
				outIdx,
				maxVerts,
				maxIdx
			);
		}
	}

	return UIR_OK;
}

static void uir_tess_segment_dir(const uir_xy_t *a, const uir_xy_t *b, float *dx, float *dy)
{
	*dx = b->x - a->x;
	*dy = b->y - a->y;
	{
		float len = sqrtf((*dx) * (*dx) + (*dy) * (*dy));
		if (len > 1e-6f) {
			*dx /= len;
			*dy /= len;
		} else {
			*dx = 1.0f;
			*dy = 0.0f;
		}
	}
}

static void uir_tess_outward_normal(float dx, float dy, float orient, float *nx, float *ny)
{
	*nx = orient * dy;
	*ny = orient * (-dx);
}

static void uir_tess_stroke_join(
	float px,
	float py,
	float d0x,
	float d0y,
	float d1x,
	float d1y,
	float orient,
	float outerDist,
	float innerDist,
	float fringeDraw,
	float *ox,
	float *oy,
	float *ix,
	float *iy,
	int *bevelSecond,
	float *ox2,
	float *oy2,
	float *ix2,
	float *iy2
)
{
	float n0x, n0y, n1x, n1y;
	float mx, my, dot, scale;

	(void)fringeDraw;

	uir_tess_outward_normal(d0x, d0y, orient, &n0x, &n0y);
	uir_tess_outward_normal(d1x, d1y, orient, &n1x, &n1y);
	mx = n0x + n1x;
	my = n0y + n1y;
	dot = mx * n0x + my * n0y;
	*bevelSecond = 0;

	if (dot < 1e-6f) {
		*ox = px + n0x * outerDist;
		*oy = py + n0y * outerDist;
		*ix = px + n0x * innerDist;
		*iy = py + n0y * innerDist;
		*ox2 = px + n1x * outerDist;
		*oy2 = py + n1y * outerDist;
		*ix2 = px + n1x * innerDist;
		*iy2 = py + n1y * innerDist;
		*bevelSecond = 1;
	} else {
		scale = 1.0f / dot;
		if (scale > UIR_TESS_MITER_LIMIT) {
			*ox = px + n0x * outerDist;
			*oy = py + n0y * outerDist;
			*ix = px + n0x * innerDist;
			*iy = py + n0y * innerDist;
			*ox2 = px + n1x * outerDist;
			*oy2 = py + n1y * outerDist;
			*ix2 = px + n1x * innerDist;
			*iy2 = py + n1y * innerDist;
			*bevelSecond = 1;
		} else {
			*ox = px + mx * scale * outerDist;
			*oy = py + my * scale * outerDist;
			*ix = px + mx * scale * innerDist;
			*iy = py + my * scale * innerDist;
		}
	}
}

static int uir_tess_append_stroke_quad_draw(
	uir_vert_t *verts,
	unsigned short *idx,
	int *nv,
	int *ni,
	int maxVerts,
	int maxIdx,
	float i0x,
	float i0y,
	float o0x,
	float o0y,
	float i1x,
	float i1y,
	float o1x,
	float o1y,
	const uir_color_t *rgba,
	unsigned char innerA,
	unsigned char outerA
)
{
	return (
		       uir_tess_append_quad(
			       verts,
			       idx,
			       nv,
			       ni,
			       maxVerts,
			       maxIdx,
			       i0x,
			       i0y,
			       o0x,
			       o0y,
			       o1x,
			       o1y,
			       i1x,
			       i1y,
			       rgba,
			       innerA,
			       outerA,
			       outerA,
			       innerA
		       ) == UIR_OK
	       )
		       ? 1
		       : 0;
}

static uir_status_t uir_tess_stroke_contour(
	const uir_xy_t *pts,
	int count,
	int closed,
	float outerDist,
	float innerDist,
	float fringeDraw,
	const uir_color_t *rgba,
	unsigned char alphaByte,
	uir_vert_t *verts,
	int vertBase,
	int maxVerts,
	int *outVerts,
	unsigned short *idx,
	int idxBase,
	int maxIdx,
	int *outIdx
)
{
	float orient = 1.0f;
	int n = 0;
	int i;
	int nv;
	int ni;
	unsigned char outerAlpha = alphaByte;
	unsigned char innerAlpha = alphaByte;

	if (count < 2) {
		return UIR_ERR_UNSUPPORTED;
	}

	if (closed && count >= 3) {
		float area = uir_tess_shoelace(pts, count);
		if (fabsf(area) > 1e-6f) {
			orient = area >= 0.0f ? 1.0f : -1.0f;
		}
	}

	for (i = 0; i < count; i++) {
		int iPrev = closed ? ((i + count - 1) % count) : (i > 0 ? i - 1 : i);
		int iNext = closed ? ((i + 1) % count) : (i + 1 < count ? i + 1 : i);
		float d0x, d0y, d1x, d1y;
		float ox, oy, ix, iy;
		float ox2, oy2, ix2, iy2;
		int bevelSecond = 0;
		float n0x, n0y, n1x, n1y;
		int step;

		if (!closed && i == 0) {
			uir_tess_segment_dir(&pts[0], &pts[1], &d1x, &d1y);
			d0x = d1x;
			d0y = d1y;
		} else if (!closed && i == count - 1) {
			uir_tess_segment_dir(&pts[count - 2], &pts[count - 1], &d0x, &d0y);
			d1x = d0x;
			d1y = d0y;
		} else {
			uir_tess_segment_dir(&pts[iPrev], &pts[i], &d0x, &d0y);
			uir_tess_segment_dir(&pts[i], &pts[iNext], &d1x, &d1y);
		}

		uir_tess_stroke_join(
			pts[i].x,
			pts[i].y,
			d0x,
			d0y,
			d1x,
			d1y,
			orient,
			outerDist,
			innerDist,
			fringeDraw,
			&ox,
			&oy,
			&ix,
			&iy,
			&bevelSecond,
			&ox2,
			&oy2,
			&ix2,
			&iy2
		);

		uir_tess_outward_normal(d0x, d0y, orient, &n0x, &n0y);
		uir_tess_outward_normal(d1x, d1y, orient, &n1x, &n1y);

		for (step = 0; step < UIR_TESS_FRINGE_STEPS; step++) {
			float fd = (fringeDraw > 0.0f) ? fringeDraw * ((float)step / (float)(UIR_TESS_FRINGE_STEPS - 1)) : 0.0f;
			s_ofringeX[step][n] = ox + n0x * fd;
			s_ofringeY[step][n] = oy + n0y * fd;
			s_ifringeX[step][n] = ix - n0x * fd;
			s_ifringeY[step][n] = iy - n0y * fd;
		}

		s_outerX[n] = ox;
		s_outerY[n] = oy;
		s_innerX[n] = ix;
		s_innerY[n] = iy;
		n++;

		if (bevelSecond) {
			for (step = 0; step < UIR_TESS_FRINGE_STEPS; step++) {
				float fd = (fringeDraw > 0.0f) ? fringeDraw * ((float)step / (float)(UIR_TESS_FRINGE_STEPS - 1)) : 0.0f;
				s_ofringeX[step][n] = ox2 + n1x * fd;
				s_ofringeY[step][n] = oy2 + n1y * fd;
				s_ifringeX[step][n] = ix2 - n1x * fd;
				s_ifringeY[step][n] = iy2 - n1y * fd;
			}
			s_outerX[n] = ox2;
			s_outerY[n] = oy2;
			s_innerX[n] = ix2;
			s_innerY[n] = iy2;
			n++;
		}
	}

	nv = vertBase;
	ni = idxBase;

	for (i = 0; i < n - 1; i++) {
		if (!uir_tess_append_stroke_quad_draw(
			    verts,
			    idx,
			    &nv,
			    &ni,
			    maxVerts,
			    maxIdx,
			    s_innerX[i],
			    s_innerY[i],
			    s_outerX[i],
			    s_outerY[i],
			    s_innerX[i + 1],
			    s_innerY[i + 1],
			    s_outerX[i + 1],
			    s_outerY[i + 1],
			    rgba,
			    innerAlpha,
			    outerAlpha
		    )) {
			return UIR_ERR_OVERFLOW;
		}
		if (fringeDraw > 0.0f) {
			int step;
			for (step = 0; step < UIR_TESS_FRINGE_STEPS - 1; step++) {
				unsigned char aInner = s_fringeAlphas[step];
				unsigned char aOuter = s_fringeAlphas[step + 1];
				if (!uir_tess_append_stroke_quad_draw(
					    verts,
					    idx,
					    &nv,
					    &ni,
					    maxVerts,
					    maxIdx,
					    s_ifringeX[step][i],
					    s_ifringeY[step][i],
					    s_outerX[i],
					    s_outerY[i],
					    s_ifringeX[step][i + 1],
					    s_ifringeY[step][i + 1],
					    s_outerX[i + 1],
					    s_outerY[i + 1],
					    rgba,
					    aInner,
					    innerAlpha
				    )) {
					return UIR_ERR_OVERFLOW;
				}
				if (!uir_tess_append_stroke_quad_draw(
					    verts,
					    idx,
					    &nv,
					    &ni,
					    maxVerts,
					    maxIdx,
					    s_outerX[i],
					    s_outerY[i],
					    s_ofringeX[step][i],
					    s_ofringeY[step][i],
					    s_outerX[i + 1],
					    s_outerY[i + 1],
					    s_ofringeX[step][i + 1],
					    s_ofringeY[step][i + 1],
					    rgba,
					    outerAlpha,
					    aOuter
				    )) {
					return UIR_ERR_OVERFLOW;
				}
			}
		}
	}

	if (closed && n > 2) {
		if (!uir_tess_append_stroke_quad_draw(
			    verts,
			    idx,
			    &nv,
			    &ni,
			    maxVerts,
			    maxIdx,
			    s_innerX[n - 1],
			    s_innerY[n - 1],
			    s_outerX[n - 1],
			    s_outerY[n - 1],
			    s_innerX[0],
			    s_innerY[0],
			    s_outerX[0],
			    s_outerY[0],
			    rgba,
			    innerAlpha,
			    outerAlpha
		    )) {
			return UIR_ERR_OVERFLOW;
		}
	}

	*outVerts = nv;
	*outIdx = ni;
	return (ni > idxBase) ? UIR_OK : UIR_ERR_UNSUPPORTED;
}

uir_status_t UIR_TessStrokePath(
	const uir_viewport_t *vp,
	const uir_path_t     *path,
	const uir_color_t    *rgba,
	float                 widthPx,
	int                   crisp,
	uir_vert_t           *verts,
	int                   maxVerts,
	int                  *outVerts,
	unsigned short       *idx,
	int                   maxIdx,
	int                  *outIdx
)
{
	uir_color_t strokeColor;
	float avgScale;
	float wDraw;
	float fringeDraw;
	float outerDist;
	float innerDist;
	int ci;
	int vertCount = 0;
	int idxCount = 0;
	uir_status_t st;
	unsigned char alphaByte;

	if (!vp || !path || !rgba || !verts || !outVerts || !idx || !outIdx) {
		return UIR_ERR_INVALID_ARG;
	}

	if (path->contourCount <= 0) {
		return UIR_ERR_UNSUPPORTED;
	}

	strokeColor = *rgba;
	avgScale = 0.5f * (vp->scaleX + vp->scaleY);
	if (avgScale < 1e-6f) {
		avgScale = 1.0f;
	}

	wDraw = widthPx;
	if (widthPx * avgScale < 1.0f) {
		wDraw = 1.0f / avgScale;
		strokeColor.a = rgba->a * uir_tess_clampf(widthPx * avgScale, 0.05f, 1.0f);
	}

	fringeDraw = 0.0f;
	if (!crisp && UIR_BatchFringeEnabled()) {
		fringeDraw = uir_tess_default_fringe_fb_px(avgScale) / avgScale;
	}

	alphaByte = uir_tess_byte(strokeColor.a);

	*outVerts = 0;
	*outIdx = 0;

	for (ci = 0; ci < path->contourCount; ci++) {
		const uir_contour_t *contour = &path->contours[ci];
		int count;
		int closed;

		if (contour->count < 2) {
			continue;
		}

		count = contour->count;
		if (count > UIR_TESS_MAX_POINTS) {
			return UIR_ERR_OVERFLOW;
		}

		{
			int pi;
			for (pi = 0; pi < count; pi++) {
				s_drawPts[pi].x = contour->points[pi].x;
				s_drawPts[pi].y = contour->points[pi].y;
			}
		}

		closed = contour->closed && count >= 3;
		if (closed) {
			outerDist = wDraw;
			innerDist = 0.0f;
		} else {
			outerDist = wDraw * 0.5f;
			innerDist = -wDraw * 0.5f;
		}

		st = uir_tess_stroke_contour(
			s_drawPts,
			count,
			closed,
			outerDist,
			innerDist,
			fringeDraw,
			&strokeColor,
			alphaByte,
			verts,
			vertCount,
			maxVerts,
			&vertCount,
			idx,
			idxCount,
			maxIdx,
			&idxCount
		);
		if (st != UIR_OK) {
			return st;
		}
	}

	if (idxCount == 0) {
		return UIR_ERR_UNSUPPORTED;
	}

	*outVerts = vertCount;
	*outIdx = idxCount;
	return UIR_OK;
}
