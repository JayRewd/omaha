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

#include "uir_path.h"
#include "uir_viewport.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float uir_clampf(float v, float lo, float hi)
{
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

static int uir_finitef(float v)
{
	return !(isnan(v) || isinf(v));
}

void UIR_PathInit(uir_path_t *path)
{
	if (!path) {
		return;
	}
	memset(path, 0, sizeof(*path));
	/* Fixed in OPM: SVG default fill-rule is nonzero. */
	path->fillRule = UIR_FILL_NON_ZERO;
}

void UIR_PathClear(uir_path_t *path)
{
	int i;

	if (!path) {
		return;
	}
	for (i = 0; i < path->contourCount; i++) {
		path->contours[i].count = 0;
		path->contours[i].closed = 0;
	}
	path->contourCount = 0;
}

void UIR_PathFree(uir_path_t *path)
{
	int i;

	if (!path) {
		return;
	}
	for (i = 0; i < UIR_MAX_CONTOURS; i++) {
		free(path->contours[i].points);
		path->contours[i].points = NULL;
		path->contours[i].capacity = 0;
		path->contours[i].count = 0;
		path->contours[i].closed = 0;
	}
	path->contourCount = 0;
}

uir_status_t UIR_PathBeginContour(uir_path_t *path, uir_contour_t **outContour)
{
	uir_contour_t *c;

	if (!path || !outContour) {
		return UIR_ERR_INVALID_ARG;
	}
	if (path->contourCount >= UIR_MAX_CONTOURS) {
		return UIR_ERR_OVERFLOW;
	}

	c = &path->contours[path->contourCount++];
	c->count = 0;
	c->closed = 0;
	if (!c->points) {
		c->capacity = 64;
		c->points = (uir_point_t *)malloc(sizeof(uir_point_t) * (size_t)c->capacity);
		if (!c->points) {
			path->contourCount--;
			c->capacity = 0;
			return UIR_ERR_OVERFLOW;
		}
	}
	*outContour = c;
	return UIR_OK;
}

uir_status_t UIR_ContourAddPoint(uir_contour_t *contour, float x, float y)
{
	uir_point_t *grown;

	if (!contour || !uir_finitef(x) || !uir_finitef(y)) {
		return UIR_ERR_INVALID_ARG;
	}
	if (contour->count >= UIR_MAX_CONTOUR_POINTS) {
		return UIR_ERR_OVERFLOW;
	}
	if (contour->count >= contour->capacity) {
		int newCap = contour->capacity ? contour->capacity * 2 : 64;
		if (newCap > UIR_MAX_CONTOUR_POINTS) {
			newCap = UIR_MAX_CONTOUR_POINTS;
		}
		grown = (uir_point_t *)realloc(contour->points, sizeof(uir_point_t) * (size_t)newCap);
		if (!grown) {
			return UIR_ERR_OVERFLOW;
		}
		contour->points = grown;
		contour->capacity = newCap;
	}
	contour->points[contour->count].x = x;
	contour->points[contour->count].y = y;
	contour->count++;
	return UIR_OK;
}

uir_status_t UIR_ContourClose(uir_contour_t *contour)
{
	if (!contour || contour->count < 3) {
		return UIR_ERR_INVALID_ARG;
	}
	contour->closed = 1;
	return UIR_OK;
}

static float uir_shoelace(const uir_point_t *pts, int count)
{
	double area = 0.0;
	int i;

	for (i = 0; i < count; i++) {
		const uir_point_t *a = &pts[i];
		const uir_point_t *b = &pts[(i + 1) % count];
		area += (double)a->x * (double)b->y - (double)b->x * (double)a->y;
	}
	return (float)(0.5 * area);
}

static int uir_is_convex(const uir_point_t *pts, int count)
{
	int i;
	int sign = 0;

	if (count < 3) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		const uir_point_t *a = &pts[i];
		const uir_point_t *b = &pts[(i + 1) % count];
		const uir_point_t *c = &pts[(i + 2) % count];
		float cross = (b->x - a->x) * (c->y - b->y) - (b->y - a->y) * (c->x - b->x);
		int s;

		if (fabsf(cross) < 1e-6f) {
			continue;
		}
		s = (cross > 0.0f) ? 1 : -1;
		if (sign == 0) {
			sign = s;
		} else if (s != sign) {
			return 0;
		}
	}
	return 1;
}

static float uir_edge_signed_dist(const uir_point_t *a, const uir_point_t *b, float px, float py)
{
	float dx = b->x - a->x;
	float dy = b->y - a->y;
	float len = sqrtf(dx * dx + dy * dy);

	if (len < 1e-8f) {
		return 0.0f;
	}
	return (dx * (py - a->y) - dy * (px - a->x)) / len;
}

static float uir_convex_cover(const uir_point_t *pts, int count, float px, float py, float scale)
{
	float winding = uir_shoelace(pts, count) >= 0.0f ? 1.0f : -1.0f;
	float minDist = 1e30f;
	int i;

	for (i = 0; i < count; i++) {
		float d = uir_edge_signed_dist(&pts[i], &pts[(i + 1) % count], px, py) * winding;
		if (d < minDist) {
			minDist = d;
		}
	}
	return uir_clampf(0.5f + minDist * scale, 0.0f, 1.0f);
}

static int uir_point_in_poly_evenodd(const uir_point_t *pts, int count, float px, float py)
{
	int inside = 0;
	int i, j;

	for (i = 0, j = count - 1; i < count; j = i++) {
		float yi = pts[i].y;
		float yj = pts[j].y;
		float xi = pts[i].x;
		float xj = pts[j].x;

		if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / ((yj - yi) + 0.0f) + xi)) {
			inside = !inside;
		}
	}
	return inside;
}

/* Fixed in OPM: nonzero (winding) fill rule for multi-contour SVG coverage. */
static int uir_poly_winding(const uir_point_t *pts, int count, float px, float py)
{
	int wn = 0;
	int i, j;

	for (i = 0, j = count - 1; i < count; j = i++) {
		float yi = pts[i].y;
		float yj = pts[j].y;
		float xi = pts[i].x;
		float xj = pts[j].x;
		float cross;

		if (yj <= py) {
			if (yi > py) {
				cross = (xi - px) * (yj - py) - (xj - px) * (yi - py);
				if (cross > 0.0f) {
					wn++;
				}
			}
		} else if (yi <= py) {
			cross = (xi - px) * (yj - py) - (xj - px) * (yi - py);
			if (cross < 0.0f) {
				wn--;
			}
		}
	}
	return wn;
}

static int uir_point_in_path(const uir_path_t *path, float px, float py)
{
	int c;

	if (path->fillRule == UIR_FILL_NON_ZERO) {
		int wn = 0;
		for (c = 0; c < path->contourCount; c++) {
			const uir_contour_t *contour = &path->contours[c];
			if (contour->count < 3) {
				continue;
			}
			wn += uir_poly_winding(contour->points, contour->count, px, py);
		}
		return wn != 0;
	}

	{
		int inside = 0;
		for (c = 0; c < path->contourCount; c++) {
			const uir_contour_t *contour = &path->contours[c];
			if (contour->count < 3) {
				continue;
			}
			if (uir_point_in_poly_evenodd(contour->points, contour->count, px, py)) {
				inside = !inside;
			}
		}
		return inside;
	}
}

int UIR_PathContainsPoint(const uir_path_t *path, float x, float y)
{
	if (!path) {
		return 0;
	}
	return uir_point_in_path(path, x, y);
}

static float uir_supersample_cover(const uir_path_t *path, float cx, float cy, float invX, float invY)
{
	int inside = 0;
	int sx, sy;

	for (sy = 0; sy < 8; sy++) {
		for (sx = 0; sx < 8; sx++) {
			float px = cx + ((sx + 0.5f) / 8.0f - 0.5f) * invX;
			float py = cy + ((sy + 0.5f) / 8.0f - 0.5f) * invY;
			if (uir_point_in_path(path, px, py)) {
				inside++;
			}
		}
	}
	return (float)inside / 64.0f;
}

static unsigned char uir_quantize_cover(float cover)
{
	int q = (int)(cover * 255.0f + 0.5f);
	if (q < 0) {
		q = 0;
	}
	if (q > 255) {
		q = 255;
	}
	return (unsigned char)q;
}

static void uir_flush_run(
	const uir_draw_sink_t *sink,
	float x0,
	float y,
	float x1,
	float cover,
	const uir_color_t *rgba
)
{
	uir_color_t c;

	if (!sink || !sink->drawBox || cover < UIR_COVERAGE_DROP || x1 <= x0) {
		return;
	}

	c = *rgba;
	if (cover < UIR_COVERAGE_SOLID) {
		c.a = rgba->a * cover;
	}
	sink->drawBox(x0, y, x1 - x0, 1.0f, &c, sink->userdata);
	if (sink->stats) {
		sink->stats->emittedRuns++;
		sink->stats->drawBoxes++;
	}
}

uir_status_t UIR_PathFill(
	const uir_viewport_t *vp,
	const uir_path_t     *path,
	const uir_color_t    *rgba,
	const uir_draw_sink_t *sink,
	int                   crisp
)
{
	float minX, minY, maxX, maxY;
	int sx0, sy0, sx1, sy1;
	int area;
	int useConvex = 0;
	float aaScale;
	int c;
	int hasPoints = 0;
	uir_path_t single;
	const uir_path_t *work;

	if (!vp || !path || !rgba || !sink || !sink->drawBox) {
		return UIR_ERR_INVALID_ARG;
	}
	if (path->contourCount <= 0) {
		return UIR_ERR_EMPTY;
	}

	minX = 1e30f;
	minY = 1e30f;
	maxX = -1e30f;
	maxY = -1e30f;

	for (c = 0; c < path->contourCount; c++) {
		const uir_contour_t *contour = &path->contours[c];
		int i;
		if (contour->count < 3) {
			continue;
		}
		for (i = 0; i < contour->count; i++) {
			if (!uir_finitef(contour->points[i].x) || !uir_finitef(contour->points[i].y)) {
				return UIR_ERR_INVALID_ARG;
			}
			if (contour->points[i].x < minX) {
				minX = contour->points[i].x;
			}
			if (contour->points[i].y < minY) {
				minY = contour->points[i].y;
			}
			if (contour->points[i].x > maxX) {
				maxX = contour->points[i].x;
			}
			if (contour->points[i].y > maxY) {
				maxY = contour->points[i].y;
			}
			hasPoints = 1;
		}
	}

	if (!hasPoints) {
		return UIR_ERR_EMPTY;
	}

	/* Expand AABB by one destination pixel in draw space. */
	minX -= vp->invX;
	minY -= vp->invY;
	maxX += vp->invX;
	maxY += vp->invY;

	{
		float fsx0, fsy0, fsx1, fsy1;
		UIR_ViewportDrawToFb(vp, minX, minY, &fsx0, &fsy0);
		UIR_ViewportDrawToFb(vp, maxX, maxY, &fsx1, &fsy1);
		sx0 = (int)floorf(fsx0);
		sy0 = (int)floorf(fsy0);
		sx1 = (int)ceilf(fsx1);
		sy1 = (int)ceilf(fsy1);
	}

	if (sx0 < vp->vpX) {
		sx0 = vp->vpX;
	}
	if (sy0 < vp->vpY) {
		sy0 = vp->vpY;
	}
	if (sx1 > vp->vpX + vp->vpW) {
		sx1 = vp->vpX + vp->vpW;
	}
	if (sy1 > vp->vpY + vp->vpH) {
		sy1 = vp->vpY + vp->vpH;
	}
	if (sx1 <= sx0 || sy1 <= sy0) {
		return UIR_OK;
	}

	area = (sx1 - sx0) * (sy1 - sy0);
	if (area > UIR_MAX_POLYGON_AREA) {
		if (sink->stats) {
			sink->stats->rejectedOversized++;
		}
		return UIR_ERR_OVERFLOW;
	}

	work = path;
	if (path->contourCount == 1 && path->contours[0].count <= 4 && uir_is_convex(path->contours[0].points, path->contours[0].count)) {
		useConvex = 1;
	}

	aaScale = 0.5f * (vp->scaleX + vp->scaleY);
	{
		int sy;
		for (sy = sy0; sy < sy1; sy++) {
			int sx;
			int runStart = -1;
			unsigned char runCover = 0;
			float drawY = vp->orthoT + ((float)sy - (float)vp->vpY) * vp->invY;

			for (sx = sx0; sx < sx1; sx++) {
				float drawX = vp->orthoL + ((float)sx + 0.5f - (float)vp->vpX) * vp->invX;
				float drawYC = vp->orthoT + ((float)sy + 0.5f - (float)vp->vpY) * vp->invY;
				float cover;
				unsigned char q;

				if (sink->stats) {
					sink->stats->sampledPixels++;
				}

				/* Added in OPM: crisp = binary pixel coverage (no soft edge AA). */
				if (crisp) {
					cover = uir_point_in_path(work, drawX, drawYC) ? 1.0f : 0.0f;
				} else if (useConvex) {
					cover = uir_convex_cover(path->contours[0].points, path->contours[0].count, drawX, drawYC, aaScale);
				} else {
					if (sink->stats) {
						sink->stats->supersamples++;
					}
					cover = uir_supersample_cover(work, drawX, drawYC, vp->invX, vp->invY);
				}

				q = uir_quantize_cover(cover);
				if (q < (unsigned char)(UIR_COVERAGE_DROP * 255.0f)) {
					if (runStart >= 0) {
						float x0 = vp->orthoL + ((float)runStart - (float)vp->vpX) * vp->invX;
						float x1 = vp->orthoL + ((float)sx - (float)vp->vpX) * vp->invX;
						uir_flush_run(sink, x0, drawY, x1, runCover / 255.0f, rgba);
						runStart = -1;
					}
					continue;
				}

				if (runStart < 0) {
					runStart = sx;
					runCover = q;
				} else if (q != runCover) {
					float x0 = vp->orthoL + ((float)runStart - (float)vp->vpX) * vp->invX;
					float x1 = vp->orthoL + ((float)sx - (float)vp->vpX) * vp->invX;
					uir_flush_run(sink, x0, drawY, x1, runCover / 255.0f, rgba);
					runStart = sx;
					runCover = q;
				}
			}

			if (runStart >= 0) {
				float x0 = vp->orthoL + ((float)runStart - (float)vp->vpX) * vp->invX;
				float x1 = vp->orthoL + ((float)sx1 - (float)vp->vpX) * vp->invX;
				uir_flush_run(sink, x0, drawY, x1, runCover / 255.0f, rgba);
			}
		}
	}

	(void)single;
	return UIR_OK;
}

uir_status_t UIR_FillPolygon(
	const uir_viewport_t *vp,
	const uir_point_t    *points,
	int                   count,
	const uir_color_t    *rgba,
	const uir_draw_sink_t *sink
)
{
	uir_path_t path;
	uir_contour_t *contour;
	uir_status_t st;
	int i;

	if (!points || count < 3) {
		return UIR_ERR_INVALID_ARG;
	}

	UIR_PathInit(&path);
	st = UIR_PathBeginContour(&path, &contour);
	if (st != UIR_OK) {
		return st;
	}
	for (i = 0; i < count; i++) {
		st = UIR_ContourAddPoint(contour, points[i].x, points[i].y);
		if (st != UIR_OK) {
			UIR_PathFree(&path);
			return st;
		}
	}
	UIR_ContourClose(contour);
	st = UIR_PathFill(vp, &path, rgba, sink, 0);
	UIR_PathFree(&path);
	return st;
}

/* Added in OPM: polyline stroke expansion (round caps, round joins; closed = annulus). */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { UIR_STROKE_ARC_STEPS_DEFAULT = 8 };

void UIR_StrokeOptsInit(uir_stroke_opts_t *opts)
{
	if (!opts) {
		return;
	}
	opts->fillRule = UIR_FILL_EVEN_ODD;
	opts->arcSteps = UIR_STROKE_ARC_STEPS_DEFAULT;
	opts->closedStrip = 0;
	/* Outside alignment keeps shape fill visible (no centerline bite / ring flood). */
	opts->alignOutside = 1;
	opts->weldEps = 0.0f;
}

static int uir_stroke_arc_steps(const uir_stroke_opts_t *opts)
{
	int n = opts && opts->arcSteps > 0 ? opts->arcSteps : UIR_STROKE_ARC_STEPS_DEFAULT;
	if (n < 2) {
		n = 2;
	}
	if (n > 64) {
		n = 64;
	}
	return n;
}

static float uir_vlen(float x, float y)
{
	return sqrtf(x * x + y * y);
}

static int uir_vnorm(float x, float y, float *ox, float *oy)
{
	float len = uir_vlen(x, y);
	if (len < 1e-8f) {
		*ox = 0.0f;
		*oy = 0.0f;
		return 0;
	}
	*ox = x / len;
	*oy = y / len;
	return 1;
}

/* Left-hand unit normal of unit direction (dx, dy). */
static void uir_left_normal(float dx, float dy, float *nx, float *ny)
{
	*nx = -dy;
	*ny = dx;
}

static float uir_ang_norm_pi(float a)
{
	while (a <= (float)(-M_PI)) {
		a += 2.0f * (float)M_PI;
	}
	while (a > (float)M_PI) {
		a -= 2.0f * (float)M_PI;
	}
	return a;
}

static int uir_coalesce_points(
	const uir_point_t *in,
	int inCount,
	int closed,
	float weldEps,
	uir_point_t *out,
	int outCap
)
{
	int i;
	int n = 0;
	float eps = weldEps > 1e-4f ? weldEps : 1e-4f;

	if (!in || inCount < 2 || !out || outCap < 2) {
		return 0;
	}
	out[0] = in[0];
	n = 1;
	for (i = 1; i < inCount; i++) {
		float dx = in[i].x - out[n - 1].x;
		float dy = in[i].y - out[n - 1].y;
		if (uir_vlen(dx, dy) < eps) {
			continue;
		}
		if (n >= outCap) {
			return 0;
		}
		out[n++] = in[i];
	}
	if (closed && n >= 2) {
		float dx = out[0].x - out[n - 1].x;
		float dy = out[0].y - out[n - 1].y;
		if (uir_vlen(dx, dy) < eps) {
			n--;
		}
	}
	return n;
}

static uir_status_t uir_contour_add_arc(
	uir_contour_t *c,
	float cx,
	float cy,
	float radius,
	float a0,
	float a1,
	int steps
)
{
	int i;
	float da;
	uir_status_t st;

	if (!c || steps < 1 || !(radius > 0.0f)) {
		return UIR_ERR_INVALID_ARG;
	}
	da = uir_ang_norm_pi(a1 - a0);
	for (i = 0; i <= steps; i++) {
		float t = (float)i / (float)steps;
		float a = a0 + da * t;
		st = UIR_ContourAddPoint(c, cx + cosf(a) * radius, cy + sinf(a) * radius);
		if (st != UIR_OK) {
			return st;
		}
	}
	return UIR_OK;
}

/*
 * Round join on one offset side: short arc from nIn*hw to nOut*hw around vertex.
 * sideSign +1 = left normals of travel dirs; -1 = right.
 */
static uir_status_t uir_append_round_join(
	uir_contour_t *c,
	float px,
	float py,
	float inDx,
	float inDy,
	float outDx,
	float outDy,
	float hw,
	float sideSign,
	int arcSteps
)
{
	float n1x, n1y, n2x, n2y;
	float a0, a1, da;
	int steps;

	uir_left_normal(inDx, inDy, &n1x, &n1y);
	uir_left_normal(outDx, outDy, &n2x, &n2y);
	n1x *= sideSign;
	n1y *= sideSign;
	n2x *= sideSign;
	n2y *= sideSign;
	a0 = atan2f(n1y, n1x);
	a1 = atan2f(n2y, n2x);
	da = fabsf(uir_ang_norm_pi(a1 - a0));
	if (da < 1e-3f) {
		return UIR_ContourAddPoint(c, px + n2x * hw, py + n2y * hw);
	}
	steps = 1 + (int)(da / (float)M_PI * (float)arcSteps);
	if (steps < 2) {
		steps = 2;
	}
	if (steps > arcSteps) {
		steps = arcSteps;
	}
	return uir_contour_add_arc(c, px, py, hw, a0, a1, steps);
}

/* Half-circle cap; travelDx/Dy is the outward direction (away from the segment). */
static uir_status_t uir_append_round_cap(
	uir_contour_t *c,
	float px,
	float py,
	float outwardDx,
	float outwardDy,
	float hw,
	int fromRightToLeft,
	int arcSteps
)
{
	float aOut;
	int i;
	uir_status_t st;

	if (!uir_vnorm(outwardDx, outwardDy, &outwardDx, &outwardDy)) {
		outwardDx = 1.0f;
		outwardDy = 0.0f;
	}
	aOut = atan2f(outwardDy, outwardDx);
	for (i = 0; i <= arcSteps; i++) {
		float t = (float)i / (float)arcSteps;
		float a;
		if (fromRightToLeft) {
			/* aOut - pi/2 → aOut + pi/2 through aOut */
			a = aOut + (float)M_PI * (t - 0.5f);
		} else {
			/* aOut + pi/2 → aOut - pi/2 through aOut */
			a = aOut + (float)M_PI * (0.5f - t);
		}
		st = UIR_ContourAddPoint(c, px + cosf(a) * hw, py + sinf(a) * hw);
		if (st != UIR_OK) {
			return st;
		}
	}
	return UIR_OK;
}

/*
 * Single-contour outside stroke band for GPU tessellation: outer edge at +width,
 * inner edge on the original centerline. Fill drawn on top stays inside the path.
 */
static uir_status_t uir_stroke_contour_outside_band(
	const uir_point_t *pts,
	int count,
	float widthPx,
	int arcSteps,
	uir_path_t *out
)
{
	uir_contour_t *c;
	uir_status_t st;
	float dirs[UIR_MAX_CONTOUR_POINTS][2];
	int i;

	if (count < 3 || !(widthPx > 0.0f)) {
		return UIR_ERR_INVALID_ARG;
	}

	for (i = 0; i < count; i++) {
		int j = (i + 1) % count;
		float dx = pts[j].x - pts[i].x;
		float dy = pts[j].y - pts[i].y;
		if (!uir_vnorm(dx, dy, &dirs[i][0], &dirs[i][1])) {
			dirs[i][0] = 1.0f;
			dirs[i][1] = 0.0f;
		}
	}

	st = UIR_PathBeginContour(out, &c);
	if (st != UIR_OK) {
		return st;
	}

	for (i = 0; i < count; i++) {
		int prevSeg = (i + count - 1) % count;
		st = uir_append_round_join(
			c,
			pts[i].x,
			pts[i].y,
			dirs[prevSeg][0],
			dirs[prevSeg][1],
			dirs[i][0],
			dirs[i][1],
			widthPx,
			-1.0f,
			arcSteps
		);
		if (st != UIR_OK) {
			return st;
		}
	}

	for (i = count - 1; i >= 0; i--) {
		st = UIR_ContourAddPoint(c, pts[i].x, pts[i].y);
		if (st != UIR_OK) {
			return st;
		}
	}

	return UIR_ContourClose(c);
}

static uir_status_t uir_stroke_contour_strip(
	const uir_point_t *pts,
	int count,
	int closed,
	float hw,
	int arcSteps,
	uir_path_t *out
)
{
	uir_contour_t *c;
	uir_status_t st;
	float dirs[UIR_MAX_CONTOUR_POINTS][2];
	int i;
	int segCount;

	if (closed) {
		segCount = count;
	} else {
		segCount = count - 1;
	}
	for (i = 0; i < segCount; i++) {
		int j = (i + 1) % count;
		float dx = pts[j].x - pts[i].x;
		float dy = pts[j].y - pts[i].y;
		if (!uir_vnorm(dx, dy, &dirs[i][0], &dirs[i][1])) {
			dirs[i][0] = 1.0f;
			dirs[i][1] = 0.0f;
		}
	}

	st = UIR_PathBeginContour(out, &c);
	if (st != UIR_OK) {
		return st;
	}

	if (closed) {
		for (i = 0; i < count; i++) {
			int prevSeg = (i + count - 1) % count;
			st = uir_append_round_join(
				c, pts[i].x, pts[i].y, dirs[prevSeg][0], dirs[prevSeg][1], dirs[i][0], dirs[i][1], hw, 1.0f,
				arcSteps
			);
			if (st != UIR_OK) {
				return st;
			}
		}
		for (i = count - 1; i >= 0; i--) {
			int prevSeg = (i + count - 1) % count;
			st = uir_append_round_join(
				c, pts[i].x, pts[i].y, dirs[prevSeg][0], dirs[prevSeg][1], dirs[i][0], dirs[i][1], hw, -1.0f,
				arcSteps
			);
			if (st != UIR_OK) {
				return st;
			}
		}
		return UIR_ContourClose(c);
	}

	{
		float nlx, nly;
		uir_left_normal(dirs[0][0], dirs[0][1], &nlx, &nly);
		st = UIR_ContourAddPoint(c, pts[0].x + nlx * hw, pts[0].y + nly * hw);
		if (st != UIR_OK) {
			return st;
		}
	}
	for (i = 1; i < count - 1; i++) {
		st = uir_append_round_join(
			c, pts[i].x, pts[i].y, dirs[i - 1][0], dirs[i - 1][1], dirs[i][0], dirs[i][1], hw, 1.0f, arcSteps
		);
		if (st != UIR_OK) {
			return st;
		}
	}
	{
		float nlx, nly;
		uir_left_normal(dirs[count - 2][0], dirs[count - 2][1], &nlx, &nly);
		st = UIR_ContourAddPoint(c, pts[count - 1].x + nlx * hw, pts[count - 1].y + nly * hw);
		if (st != UIR_OK) {
			return st;
		}
		st = uir_append_round_cap(
			c, pts[count - 1].x, pts[count - 1].y, dirs[count - 2][0], dirs[count - 2][1], hw, 0, arcSteps
		);
		if (st != UIR_OK) {
			return st;
		}
	}
	for (i = count - 2; i >= 1; i--) {
		st = uir_append_round_join(
			c, pts[i].x, pts[i].y, dirs[i][0], dirs[i][1], dirs[i - 1][0], dirs[i - 1][1], hw, -1.0f, arcSteps
		);
		if (st != UIR_OK) {
			return st;
		}
	}
	{
		float nlx, nly;
		uir_left_normal(dirs[0][0], dirs[0][1], &nlx, &nly);
		st = UIR_ContourAddPoint(c, pts[0].x - nlx * hw, pts[0].y - nly * hw);
		if (st != UIR_OK) {
			return st;
		}
		st = uir_append_round_cap(c, pts[0].x, pts[0].y, -dirs[0][0], -dirs[0][1], hw, 1, arcSteps);
		if (st != UIR_OK) {
			return st;
		}
	}
	return UIR_ContourClose(c);
}

static uir_status_t uir_stroke_contour(
	const uir_point_t *pts,
	int count,
	int closed,
	float widthPx,
	const uir_stroke_opts_t *opts,
	uir_path_t *out
)
{
	uir_contour_t *c;
	uir_status_t st;
	float dirs[UIR_MAX_CONTOUR_POINTS][2];
	int i;
	int segCount;
	int arcSteps = uir_stroke_arc_steps(opts);
	float hw = widthPx * 0.5f;
	int useStrip = !closed || (opts && opts->closedStrip);
	int alignOutside = closed && (!opts || opts->alignOutside);

	if (count < 2 || !(widthPx > 0.0f)) {
		return UIR_ERR_INVALID_ARG;
	}

	if (useStrip) {
		return uir_stroke_contour_strip(pts, count, closed, hw, arcSteps, out);
	}

	segCount = count;
	for (i = 0; i < segCount; i++) {
		int j = (i + 1) % count;
		float dx = pts[j].x - pts[i].x;
		float dy = pts[j].y - pts[i].y;
		if (!uir_vnorm(dx, dy, &dirs[i][0], &dirs[i][1])) {
			dirs[i][0] = 1.0f;
			dirs[i][1] = 0.0f;
		}
	}

	/*
	 * Closed stroke band via even-odd:
	 * - centerline (alignOutside=0): outer at +hw (right), inner at -hw (left)
	 * - outside (alignOutside=1): outer at +width, inner along the path — fill untouched
	 */
	{
		float outerDist = alignOutside ? widthPx : hw;

		st = UIR_PathBeginContour(out, &c);
		if (st != UIR_OK) {
			return st;
		}
		for (i = 0; i < count; i++) {
			int prevSeg = (i + count - 1) % count;
			st = uir_append_round_join(
				c,
				pts[i].x,
				pts[i].y,
				dirs[prevSeg][0],
				dirs[prevSeg][1],
				dirs[i][0],
				dirs[i][1],
				outerDist,
				-1.0f,
				arcSteps
			);
			if (st != UIR_OK) {
				return st;
			}
		}
		st = UIR_ContourClose(c);
		if (st != UIR_OK) {
			return st;
		}

		st = UIR_PathBeginContour(out, &c);
		if (st != UIR_OK) {
			return st;
		}
		if (alignOutside) {
			/* Inner edge = original centerline (reverse winding). */
			for (i = count - 1; i >= 0; i--) {
				st = UIR_ContourAddPoint(c, pts[i].x, pts[i].y);
				if (st != UIR_OK) {
					return st;
				}
			}
		} else {
			for (i = count - 1; i >= 0; i--) {
				int prevSeg = (i + count - 1) % count;
				st = uir_append_round_join(
					c,
					pts[i].x,
					pts[i].y,
					dirs[prevSeg][0],
					dirs[prevSeg][1],
					dirs[i][0],
					dirs[i][1],
					hw,
					1.0f,
					arcSteps
				);
				if (st != UIR_OK) {
					return st;
				}
			}
		}
		return UIR_ContourClose(c);
	}
}

uir_status_t UIR_BuildStrokePathOpts(
	const uir_path_t *src,
	float widthPx,
	const uir_stroke_opts_t *opts,
	uir_path_t *out
)
{
	uir_point_t coalesced[UIR_MAX_CONTOUR_POINTS];
	uir_stroke_opts_t local;
	int ci;
	float weldEps;

	if (!src || !out || !(widthPx > 0.0f) || !uir_finitef(widthPx)) {
		return UIR_ERR_INVALID_ARG;
	}

	if (!opts) {
		UIR_StrokeOptsInit(&local);
		opts = &local;
	}

	weldEps = opts->weldEps > 0.0f ? opts->weldEps : 0.0f;
	UIR_PathInit(out);

	for (ci = 0; ci < src->contourCount; ci++) {
		const uir_contour_t *sc = &src->contours[ci];
		int n;
		uir_status_t st;

		if (sc->count < 2 || !sc->points) {
			continue;
		}
		n = uir_coalesce_points(sc->points, sc->count, sc->closed, weldEps, coalesced, UIR_MAX_CONTOUR_POINTS);
		if (n < 2) {
			continue;
		}
		st = uir_stroke_contour(coalesced, n, sc->closed ? 1 : 0, widthPx, opts, out);
		if (st != UIR_OK) {
			UIR_PathFree(out);
			return st;
		}
	}

	if (out->contourCount == 0) {
		UIR_PathFree(out);
		return UIR_ERR_INVALID_ARG;
	}
	out->fillRule = opts->fillRule;
	return UIR_OK;
}

uir_status_t UIR_BuildStrokePath(const uir_path_t *src, float widthPx, uir_path_t *out)
{
	uir_stroke_opts_t opts;
	UIR_StrokeOptsInit(&opts);
	return UIR_BuildStrokePathOpts(src, widthPx, &opts, out);
}

/*
 * Added in OPM: outside-only stroke outline (single contour) for GPU tessellation.
 * Matches even-odd alignOutside CPU strokes without a fill/stroke seam.
 */
uir_status_t UIR_BuildOutsideStrokePath(const uir_path_t *src, float widthPx, uir_path_t *out)
{
	uir_point_t coalesced[UIR_MAX_CONTOUR_POINTS];
	uir_stroke_opts_t opts;
	int ci;

	if (!src || !out || !(widthPx > 0.0f) || !uir_finitef(widthPx)) {
		return UIR_ERR_INVALID_ARG;
	}

	UIR_StrokeOptsInit(&opts);
	UIR_PathInit(out);

	for (ci = 0; ci < src->contourCount; ci++) {
		const uir_contour_t *sc = &src->contours[ci];
		int n;
		uir_status_t st;

		if (sc->count < 2 || !sc->points) {
			continue;
		}
		n = uir_coalesce_points(sc->points, sc->count, sc->closed, 0.0f, coalesced, UIR_MAX_CONTOUR_POINTS);
		if (n < 2) {
			continue;
		}
		if (sc->closed && n >= 3) {
			st = uir_stroke_contour_outside_band(coalesced, n, widthPx, uir_stroke_arc_steps(&opts), out);
		} else {
			st = uir_stroke_contour(coalesced, n, 0, widthPx, &opts, out);
		}
		if (st != UIR_OK) {
			UIR_PathFree(out);
			return st;
		}
	}

	if (out->contourCount == 0) {
		UIR_PathFree(out);
		return UIR_ERR_INVALID_ARG;
	}
	out->fillRule = UIR_FILL_NON_ZERO;
	return UIR_OK;
}

uir_status_t UIR_PathStroke(
	const uir_viewport_t *vp,
	const uir_path_t     *path,
	const uir_color_t    *rgba,
	float                 widthPx,
	const uir_draw_sink_t *sink,
	int                   crisp
)
{
	uir_path_t stroke;
	uir_status_t st;

	if (!vp || !path || !rgba || !sink || !(widthPx > 0.0f)) {
		return UIR_ERR_INVALID_ARG;
	}

	st = UIR_BuildStrokePath(path, widthPx, &stroke);
	if (st != UIR_OK) {
		return st;
	}
	st = UIR_PathFill(vp, &stroke, rgba, sink, crisp);
	UIR_PathFree(&stroke);
	return st;
}

uir_status_t UIR_PathRotate(const uir_path_t *src, float cx, float cy, float degrees, uir_path_t *out)
{
	int c;

	if (!src || !out) {
		return UIR_ERR_INVALID_ARG;
	}

	UIR_PathInit(out);
	out->fillRule = src->fillRule;

	if (degrees == 0.0f) {
		for (c = 0; c < src->contourCount; c++) {
			const uir_contour_t *sc = &src->contours[c];
			uir_contour_t *dc;
			uir_status_t st;
			int i;

			st = UIR_PathBeginContour(out, &dc);
			if (st != UIR_OK) {
				UIR_PathFree(out);
				return st;
			}
			for (i = 0; i < sc->count; i++) {
				st = UIR_ContourAddPoint(dc, sc->points[i].x, sc->points[i].y);
				if (st != UIR_OK) {
					UIR_PathFree(out);
					return st;
				}
			}
			if (sc->closed) {
				UIR_ContourClose(dc);
			}
		}
		return UIR_OK;
	}

	{
		const float rad = degrees * (3.14159265358979323846f / 180.0f);
		const float cosr = cosf(rad);
		const float sinr = sinf(rad);

		for (c = 0; c < src->contourCount; c++) {
			const uir_contour_t *sc = &src->contours[c];
			uir_contour_t *dc;
			uir_status_t st;
			int i;

			st = UIR_PathBeginContour(out, &dc);
			if (st != UIR_OK) {
				UIR_PathFree(out);
				return st;
			}
			for (i = 0; i < sc->count; i++) {
				const float dx = sc->points[i].x - cx;
				const float dy = sc->points[i].y - cy;
				const float rx = cx + cosr * dx - sinr * dy;
				const float ry = cy + sinr * dx + cosr * dy;
				st = UIR_ContourAddPoint(dc, rx, ry);
				if (st != UIR_OK) {
					UIR_PathFree(out);
					return st;
				}
			}
			if (sc->closed) {
				UIR_ContourClose(dc);
			}
		}
	}
	return UIR_OK;
}

uir_status_t UIR_PathBounds(const uir_path_t *path, uir_rect_t *out)
{
	int c;
	int i;

	if (!path || !out) {
		return UIR_ERR_INVALID_ARG;
	}

	if (path->contourCount <= 0) {
		out->x = 0.0f;
		out->y = 0.0f;
		out->w = 0.0f;
		out->h = 0.0f;
		return UIR_OK;
	}

	float minX = path->contours[0].points[0].x;
	float minY = path->contours[0].points[0].y;
	float maxX = minX;
	float maxY = minY;

	for (c = 0; c < path->contourCount; c++) {
		const uir_contour_t *contour = &path->contours[c];
		for (i = 0; i < contour->count; i++) {
			const float px = contour->points[i].x;
			const float py = contour->points[i].y;
			if (px < minX) {
				minX = px;
			}
			if (py < minY) {
				minY = py;
			}
			if (px > maxX) {
				maxX = px;
			}
			if (py > maxY) {
				maxY = py;
			}
		}
	}

	out->x = minX;
	out->y = minY;
	out->w = maxX - minX;
	out->h = maxY - minY;
	return UIR_OK;
}
