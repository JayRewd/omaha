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

#include "uir_svg.h"
#include "uir_path.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

static void uir_skip_ws_comma(const char **p)
{
	while (**p && (isspace((unsigned char)**p) || **p == ',')) {
		(*p)++;
	}
}

uir_status_t UIR_SvgParseFloat(const char **cursor, float *out)
{
	const char *p;
	const char *start;
	int sawDigit = 0;
	int sawDot = 0;
	char buf[64];
	size_t len;
	char *endptr;
	double v;

	if (!cursor || !*cursor || !out) {
		return UIR_ERR_INVALID_ARG;
	}

	p = *cursor;
	uir_skip_ws_comma(&p);
	start = p;

	if (*p == '+' || *p == '-') {
		p++;
	}

	while (*p) {
		if (*p >= '0' && *p <= '9') {
			sawDigit = 1;
			p++;
			continue;
		}
		if (*p == '.' && !sawDot) {
			sawDot = 1;
			p++;
			continue;
		}
		break;
	}

	if ((*p == 'e' || *p == 'E') && sawDigit) {
		const char *e = p + 1;
		if (*e == '+' || *e == '-') {
			e++;
		}
		if (*e >= '0' && *e <= '9') {
			p = e;
			while (*p >= '0' && *p <= '9') {
				p++;
			}
		}
	}

	if (!sawDigit || p == start) {
		return UIR_ERR_PARSE;
	}

	len = (size_t)(p - start);
	if (len >= sizeof(buf)) {
		return UIR_ERR_OVERFLOW;
	}
	memcpy(buf, start, len);
	buf[len] = '\0';

	v = strtod(buf, &endptr);
	if (endptr == buf || *endptr != '\0' || isnan(v) || isinf(v)) {
		return UIR_ERR_PARSE;
	}

	*out = (float)v;
	*cursor = p;
	return UIR_OK;
}

static uir_status_t uir_parse_coord_pair(const char **p, float *x, float *y)
{
	uir_status_t st = UIR_SvgParseFloat(p, x);
	if (st != UIR_OK) {
		return st;
	}
	return UIR_SvgParseFloat(p, y);
}

static float uir_dist2(float x0, float y0, float x1, float y1)
{
	float dx = x1 - x0;
	float dy = y1 - y0;
	return dx * dx + dy * dy;
}

static uir_status_t uir_add_point_unique(uir_contour_t *c, float x, float y)
{
	if (c->count > 0) {
		uir_point_t *last = &c->points[c->count - 1];
		if (uir_dist2(last->x, last->y, x, y) < 1e-12f) {
			return UIR_OK;
		}
	}
	return UIR_ContourAddPoint(c, x, y);
}

static uir_status_t uir_sample_cubic(
	uir_contour_t *c,
	float x0,
	float y0,
	float x1,
	float y1,
	float x2,
	float y2,
	float x3,
	float y3,
	float flatness,
	int depth
)
{
	float mx = 0.125f * x0 + 0.375f * x1 + 0.375f * x2 + 0.125f * x3;
	float my = 0.125f * y0 + 0.375f * y1 + 0.375f * y2 + 0.125f * y3;
	float lx = 0.5f * (x0 + x3);
	float ly = 0.5f * (y0 + y3);
	float err2 = uir_dist2(mx, my, lx, ly);

	if (depth >= 12 || err2 <= flatness * flatness) {
		return uir_add_point_unique(c, x3, y3);
	}

	{
		float x01 = 0.5f * (x0 + x1);
		float y01 = 0.5f * (y0 + y1);
		float x12 = 0.5f * (x1 + x2);
		float y12 = 0.5f * (y1 + y2);
		float x23 = 0.5f * (x2 + x3);
		float y23 = 0.5f * (y2 + y3);
		float x012 = 0.5f * (x01 + x12);
		float y012 = 0.5f * (y01 + y12);
		float x123 = 0.5f * (x12 + x23);
		float y123 = 0.5f * (y12 + y23);
		float x0123 = 0.5f * (x012 + x123);
		float y0123 = 0.5f * (y012 + y123);
		uir_status_t st;

		st = uir_sample_cubic(c, x0, y0, x01, y01, x012, y012, x0123, y0123, flatness, depth + 1);
		if (st != UIR_OK) {
			return st;
		}
		return uir_sample_cubic(c, x0123, y0123, x123, y123, x23, y23, x3, y3, flatness, depth + 1);
	}
}

static uir_status_t uir_sample_quadratic(
	uir_contour_t *c,
	float x0,
	float y0,
	float x1,
	float y1,
	float x2,
	float y2,
	float flatness
)
{
	float cx1 = x0 + (2.0f / 3.0f) * (x1 - x0);
	float cy1 = y0 + (2.0f / 3.0f) * (y1 - y0);
	float cx2 = x2 + (2.0f / 3.0f) * (x1 - x2);
	float cy2 = y2 + (2.0f / 3.0f) * (y1 - y2);
	return uir_sample_cubic(c, x0, y0, cx1, cy1, cx2, cy2, x2, y2, flatness, 0);
}

static uir_status_t uir_add_arc(
	uir_contour_t *c,
	float x0,
	float y0,
	float rx,
	float ry,
	float xAxisRotDeg,
	int largeArc,
	int sweep,
	float x1,
	float y1,
	float flatness
)
{
	float phi, cosPhi, sinPhi;
	float dx, dy, x1p, y1p;
	float rxSq, rySq, x1pSq, y1pSq, lambda;
	float sq, cxp, cyp, cx, cy;
	float theta1, dTheta;
	float ux, uy, vx, vy, n, p;
	int segments, i;
	float startX, startY;

	rx = fabsf(rx);
	ry = fabsf(ry);
	if (rx < 1e-8f || ry < 1e-8f) {
		return uir_add_point_unique(c, x1, y1);
	}
	if (uir_dist2(x0, y0, x1, y1) < 1e-12f) {
		return UIR_OK;
	}

	phi = xAxisRotDeg * (float)M_PI / 180.0f;
	cosPhi = cosf(phi);
	sinPhi = sinf(phi);

	dx = (x0 - x1) * 0.5f;
	dy = (y0 - y1) * 0.5f;
	x1p = cosPhi * dx + sinPhi * dy;
	y1p = -sinPhi * dx + cosPhi * dy;

	rxSq = rx * rx;
	rySq = ry * ry;
	x1pSq = x1p * x1p;
	y1pSq = y1p * y1p;
	lambda = x1pSq / rxSq + y1pSq / rySq;
	if (lambda > 1.0f) {
		float s = sqrtf(lambda);
		rx *= s;
		ry *= s;
		rxSq = rx * rx;
		rySq = ry * ry;
	}

	sq = (rxSq * rySq - rxSq * y1pSq - rySq * x1pSq) / (rxSq * y1pSq + rySq * x1pSq);
	if (sq < 0.0f) {
		sq = 0.0f;
	}
	sq = sqrtf(sq);
	if (largeArc == sweep) {
		sq = -sq;
	}
	cxp = sq * (rx * y1p / ry);
	cyp = sq * (-(ry * x1p / rx));
	cx = cosPhi * cxp - sinPhi * cyp + (x0 + x1) * 0.5f;
	cy = sinPhi * cxp + cosPhi * cyp + (y0 + y1) * 0.5f;

	ux = (x1p - cxp) / rx;
	uy = (y1p - cyp) / ry;
	vx = (-x1p - cxp) / rx;
	vy = (-y1p - cyp) / ry;
	n = sqrtf(ux * ux + uy * uy);
	p = ux;
	theta1 = acosf(uir_clampf(p / n, -1.0f, 1.0f));
	if (uy < 0.0f) {
		theta1 = -theta1;
	}
	n = sqrtf((ux * ux + uy * uy) * (vx * vx + vy * vy));
	p = ux * vx + uy * vy;
	dTheta = acosf(uir_clampf(p / n, -1.0f, 1.0f));
	if (ux * vy - uy * vx < 0.0f) {
		dTheta = -dTheta;
	}
	if (!sweep && dTheta > 0.0f) {
		dTheta -= 2.0f * (float)M_PI;
	} else if (sweep && dTheta < 0.0f) {
		dTheta += 2.0f * (float)M_PI;
	}

	segments = (int)ceilf(fabsf(dTheta) / ((float)M_PI * 0.5f + 1e-6f));
	if (segments < 1) {
		segments = 1;
	}
	if (segments > 8) {
		segments = 8;
	}

	startX = x0;
	startY = y0;
	for (i = 1; i <= segments; i++) {
		float t0 = theta1 + dTheta * ((float)(i - 1) / (float)segments);
		float t1 = theta1 + dTheta * ((float)i / (float)segments);
		float dt = t1 - t0;
		float alpha = sinf(dt) * (sqrtf(4.0f + 3.0f * tanf(dt * 0.5f) * tanf(dt * 0.5f)) - 1.0f) / 3.0f;
		float cos0 = cosf(t0), sin0 = sinf(t0);
		float cos1 = cosf(t1), sin1 = sinf(t1);
		float e0x = cx + cosPhi * rx * cos0 - sinPhi * ry * sin0;
		float e0y = cy + sinPhi * rx * cos0 + cosPhi * ry * sin0;
		float e1x = cx + cosPhi * rx * cos1 - sinPhi * ry * sin1;
		float e1y = cy + sinPhi * rx * cos1 + cosPhi * ry * sin1;
		float ep0x = -cosPhi * rx * sin0 - sinPhi * ry * cos0;
		float ep0y = -sinPhi * rx * sin0 + cosPhi * ry * cos0;
		float ep1x = -cosPhi * rx * sin1 - sinPhi * ry * cos1;
		float ep1y = -sinPhi * rx * sin1 + cosPhi * ry * cos1;
		float q1x = e0x + alpha * ep0x;
		float q1y = e0y + alpha * ep0y;
		float q2x = e1x - alpha * ep1x;
		float q2y = e1y - alpha * ep1y;
		uir_status_t st = uir_sample_cubic(c, startX, startY, q1x, q1y, q2x, q2y, e1x, e1y, flatness, 0);
		if (st != UIR_OK) {
			return st;
		}
		startX = e1x;
		startY = e1y;
	}
	return UIR_OK;
}

static int uir_is_command(char ch)
{
	switch (ch) {
	case 'M':
	case 'm':
	case 'L':
	case 'l':
	case 'H':
	case 'h':
	case 'V':
	case 'v':
	case 'C':
	case 'c':
	case 'S':
	case 's':
	case 'Q':
	case 'q':
	case 'T':
	case 't':
	case 'A':
	case 'a':
	case 'Z':
	case 'z':
		return 1;
	default:
		return 0;
	}
}

uir_parse_result_t UIR_SvgParsePathD(const char *d, float flatness, uir_path_t *outPath)
{
	uir_parse_result_t result = {UIR_OK, 0};
	const char *p;
	const char *begin;
	char cmd = 0;
	float cx = 0.0f, cy = 0.0f;
	float startX = 0.0f, startY = 0.0f;
	float lastCx = 0.0f, lastCy = 0.0f;
	float lastQx = 0.0f, lastQy = 0.0f;
	int haveCubicCtrl = 0;
	int haveQuadCtrl = 0;
	uir_contour_t *contour = NULL;
	uir_status_t st;

	if (!d || !outPath) {
		result.status = UIR_ERR_INVALID_ARG;
		return result;
	}
	if (!(flatness > 0.0f)) {
		flatness = 0.25f;
	}

	UIR_PathInit(outPath);
	begin = d;
	p = d;

	while (*p) {
		const char *loopStart;
		uir_skip_ws_comma(&p);
		if (!*p) {
			break;
		}
		loopStart = p;

		if (uir_is_command(*p)) {
			cmd = *p++;
		} else if (!cmd) {
			result.status = UIR_ERR_PARSE;
			result.offset = (int)(p - begin);
			UIR_PathFree(outPath);
			return result;
		} else {
			/* Continuing previous command: next token must be numeric. */
			const char *t = p;
			if (!(*t == '+' || *t == '-' || *t == '.' || (*t >= '0' && *t <= '9'))) {
				result.status = UIR_ERR_PARSE;
				result.offset = (int)(p - begin);
				UIR_PathFree(outPath);
				return result;
			}
		}

		(void)loopStart;

		if (cmd == 'Z' || cmd == 'z') {
			if (contour && contour->count >= 3) {
				UIR_ContourClose(contour);
			}
			cx = startX;
			cy = startY;
			haveCubicCtrl = 0;
			haveQuadCtrl = 0;
			contour = NULL;
			/* Fixed in OPM: clear cmd so trailing garbage is a parse error, not an infinite loop. */
			cmd = 0;
			continue;
		}

		if (cmd == 'M' || cmd == 'm') {
			float x, y;
			int first = 1;
			while (1) {
				const char *save = p;
				st = uir_parse_coord_pair(&p, &x, &y);
				if (st != UIR_OK) {
					if (first) {
						result.status = st;
						result.offset = (int)(save - begin);
						UIR_PathFree(outPath);
						return result;
					}
					p = save;
					break;
				}
				if (cmd == 'm') {
					x += cx;
					y += cy;
				}
				if (first) {
					st = UIR_PathBeginContour(outPath, &contour);
					if (st != UIR_OK) {
						result.status = st;
						result.offset = (int)(save - begin);
						UIR_PathFree(outPath);
						return result;
					}
					st = UIR_ContourAddPoint(contour, x, y);
					if (st != UIR_OK) {
						result.status = st;
						result.offset = (int)(save - begin);
						UIR_PathFree(outPath);
						return result;
					}
					startX = x;
					startY = y;
					first = 0;
					cmd = (cmd == 'M') ? 'L' : 'l';
				} else {
					st = uir_add_point_unique(contour, x, y);
					if (st != UIR_OK) {
						result.status = st;
						result.offset = (int)(save - begin);
						UIR_PathFree(outPath);
						return result;
					}
				}
				cx = x;
				cy = y;
				haveCubicCtrl = 0;
				haveQuadCtrl = 0;
			}
			continue;
		}

		if (!contour) {
			result.status = UIR_ERR_PARSE;
			result.offset = (int)(p - begin);
			UIR_PathFree(outPath);
			return result;
		}

		if (cmd == 'L' || cmd == 'l') {
			while (1) {
				float x, y;
				const char *save = p;
				st = uir_parse_coord_pair(&p, &x, &y);
				if (st != UIR_OK) {
					p = save;
					break;
				}
				if (cmd == 'l') {
					x += cx;
					y += cy;
				}
				st = uir_add_point_unique(contour, x, y);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				cx = x;
				cy = y;
				haveCubicCtrl = 0;
				haveQuadCtrl = 0;
			}
			continue;
		}

		if (cmd == 'H' || cmd == 'h') {
			while (1) {
				float x;
				const char *save = p;
				st = UIR_SvgParseFloat(&p, &x);
				if (st != UIR_OK) {
					p = save;
					break;
				}
				if (cmd == 'h') {
					x += cx;
				}
				st = uir_add_point_unique(contour, x, cy);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				cx = x;
				haveCubicCtrl = 0;
				haveQuadCtrl = 0;
			}
			continue;
		}

		if (cmd == 'V' || cmd == 'v') {
			while (1) {
				float y;
				const char *save = p;
				st = UIR_SvgParseFloat(&p, &y);
				if (st != UIR_OK) {
					p = save;
					break;
				}
				if (cmd == 'v') {
					y += cy;
				}
				st = uir_add_point_unique(contour, cx, y);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				cy = y;
				haveCubicCtrl = 0;
				haveQuadCtrl = 0;
			}
			continue;
		}

		if (cmd == 'C' || cmd == 'c') {
			while (1) {
				float x1, y1, x2, y2, x, y;
				const char *save = p;
				st = uir_parse_coord_pair(&p, &x1, &y1);
				if (st != UIR_OK) {
					p = save;
					break;
				}
				st = uir_parse_coord_pair(&p, &x2, &y2);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				st = uir_parse_coord_pair(&p, &x, &y);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				if (cmd == 'c') {
					x1 += cx;
					y1 += cy;
					x2 += cx;
					y2 += cy;
					x += cx;
					y += cy;
				}
				st = uir_sample_cubic(contour, cx, cy, x1, y1, x2, y2, x, y, flatness, 0);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				lastCx = x2;
				lastCy = y2;
				haveCubicCtrl = 1;
				haveQuadCtrl = 0;
				cx = x;
				cy = y;
			}
			continue;
		}

		if (cmd == 'S' || cmd == 's') {
			while (1) {
				float x2, y2, x, y, x1, y1;
				const char *save = p;
				st = uir_parse_coord_pair(&p, &x2, &y2);
				if (st != UIR_OK) {
					p = save;
					break;
				}
				st = uir_parse_coord_pair(&p, &x, &y);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				if (cmd == 's') {
					x2 += cx;
					y2 += cy;
					x += cx;
					y += cy;
				}
				if (haveCubicCtrl) {
					x1 = 2.0f * cx - lastCx;
					y1 = 2.0f * cy - lastCy;
				} else {
					x1 = cx;
					y1 = cy;
				}
				st = uir_sample_cubic(contour, cx, cy, x1, y1, x2, y2, x, y, flatness, 0);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				lastCx = x2;
				lastCy = y2;
				haveCubicCtrl = 1;
				haveQuadCtrl = 0;
				cx = x;
				cy = y;
			}
			continue;
		}

		if (cmd == 'Q' || cmd == 'q') {
			while (1) {
				float x1, y1, x, y;
				const char *save = p;
				st = uir_parse_coord_pair(&p, &x1, &y1);
				if (st != UIR_OK) {
					p = save;
					break;
				}
				st = uir_parse_coord_pair(&p, &x, &y);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				if (cmd == 'q') {
					x1 += cx;
					y1 += cy;
					x += cx;
					y += cy;
				}
				st = uir_sample_quadratic(contour, cx, cy, x1, y1, x, y, flatness);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				lastQx = x1;
				lastQy = y1;
				haveQuadCtrl = 1;
				haveCubicCtrl = 0;
				cx = x;
				cy = y;
			}
			continue;
		}

		if (cmd == 'T' || cmd == 't') {
			while (1) {
				float x, y, x1, y1;
				const char *save = p;
				st = uir_parse_coord_pair(&p, &x, &y);
				if (st != UIR_OK) {
					p = save;
					break;
				}
				if (cmd == 't') {
					x += cx;
					y += cy;
				}
				if (haveQuadCtrl) {
					x1 = 2.0f * cx - lastQx;
					y1 = 2.0f * cy - lastQy;
				} else {
					x1 = cx;
					y1 = cy;
				}
				st = uir_sample_quadratic(contour, cx, cy, x1, y1, x, y, flatness);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				lastQx = x1;
				lastQy = y1;
				haveQuadCtrl = 1;
				haveCubicCtrl = 0;
				cx = x;
				cy = y;
			}
			continue;
		}

		if (cmd == 'A' || cmd == 'a') {
			while (1) {
				float rx, ry, rot, x, y;
				float largeF, sweepF;
				const char *save = p;
				st = UIR_SvgParseFloat(&p, &rx);
				if (st != UIR_OK) {
					p = save;
					break;
				}
				st = UIR_SvgParseFloat(&p, &ry);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				st = UIR_SvgParseFloat(&p, &rot);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				st = UIR_SvgParseFloat(&p, &largeF);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				st = UIR_SvgParseFloat(&p, &sweepF);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				st = uir_parse_coord_pair(&p, &x, &y);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				if (cmd == 'a') {
					x += cx;
					y += cy;
				}
				st = uir_add_arc(contour, cx, cy, rx, ry, rot, largeF != 0.0f, sweepF != 0.0f, x, y, flatness);
				if (st != UIR_OK) {
					result.status = st;
					result.offset = (int)(save - begin);
					UIR_PathFree(outPath);
					return result;
				}
				cx = x;
				cy = y;
				haveCubicCtrl = 0;
				haveQuadCtrl = 0;
			}
			continue;
		}

		result.status = UIR_ERR_UNSUPPORTED;
		result.offset = (int)(p - begin);
		UIR_PathFree(outPath);
		return result;
	}

	if (contour && contour->count >= 3 && !contour->closed) {
		UIR_ContourClose(contour);
	}

	if (outPath->contourCount == 0) {
		result.status = UIR_ERR_EMPTY;
	}
	return result;
}

uir_parse_result_t UIR_SvgParsePolygonPoints(const char *points, uir_path_t *outPath)
{
	uir_parse_result_t result = {UIR_OK, 0};
	const char *p;
	const char *begin;
	uir_contour_t *contour = NULL;
	uir_status_t st;
	int count = 0;

	if (!points || !outPath) {
		result.status = UIR_ERR_INVALID_ARG;
		return result;
	}

	UIR_PathInit(outPath);
	begin = points;
	p = points;
	st = UIR_PathBeginContour(outPath, &contour);
	if (st != UIR_OK) {
		result.status = st;
		return result;
	}

	while (*p) {
		float x, y;
		const char *save = p;
		uir_skip_ws_comma(&p);
		if (!*p) {
			break;
		}
		st = uir_parse_coord_pair(&p, &x, &y);
		if (st != UIR_OK) {
			result.status = st;
			result.offset = (int)(save - begin);
			UIR_PathFree(outPath);
			return result;
		}
		st = UIR_ContourAddPoint(contour, x, y);
		if (st != UIR_OK) {
			result.status = st;
			result.offset = (int)(save - begin);
			UIR_PathFree(outPath);
			return result;
		}
		count++;
	}

	if (count < 3) {
		result.status = UIR_ERR_EMPTY;
		UIR_PathFree(outPath);
		return result;
	}
	UIR_ContourClose(contour);
	return result;
}

uir_status_t UIR_SvgMapPathToRect(
	const uir_path_t    *src,
	const uir_viewbox_t *viewBox,
	const uir_rect_t    *dest,
	uir_fit_mode_t       fit,
	uir_path_t          *out
)
{
	float sx, sy, s, ox, oy;
	int c;

	if (!src || !viewBox || !dest || !out) {
		return UIR_ERR_INVALID_ARG;
	}
	if (!(viewBox->width > 0.0f) || !(viewBox->height > 0.0f) || !(dest->w > 0.0f) || !(dest->h > 0.0f)) {
		return UIR_ERR_INVALID_ARG;
	}

	UIR_PathInit(out);
	out->fillRule = src->fillRule;

	if (fit == UIR_FIT_STRETCH) {
		sx = dest->w / viewBox->width;
		sy = dest->h / viewBox->height;
		ox = dest->x - viewBox->minX * sx;
		oy = dest->y - viewBox->minY * sy;
	} else {
		sx = dest->w / viewBox->width;
		sy = dest->h / viewBox->height;
		s = (sx < sy) ? sx : sy;
		sx = sy = s;
		ox = dest->x + 0.5f * (dest->w - viewBox->width * s) - viewBox->minX * s;
		oy = dest->y + 0.5f * (dest->h - viewBox->height * s) - viewBox->minY * s;
	}

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
			st = UIR_ContourAddPoint(dc, sc->points[i].x * sx + ox, sc->points[i].y * sy + oy);
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

void UIR_SvgMapPointContain(
	float localX,
	float localY,
	const uir_viewbox_t *viewBox,
	float cx,
	float cy,
	float diameter,
	float *outX,
	float *outY
)
{
	float s;
	float midX;
	float midY;

	if (!viewBox || !outX || !outY || !(viewBox->width > 0.0f) || !(diameter > 0.0f)) {
		return;
	}
	s = diameter / viewBox->width;
	midX = viewBox->minX + viewBox->width * 0.5f;
	midY = viewBox->minY + viewBox->height * 0.5f;
	*outX = cx + (localX - midX) * s;
	*outY = cy + (localY - midY) * s;
}
