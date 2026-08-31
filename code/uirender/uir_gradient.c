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
/* Added in OPM: design-format gradient fills baked into UI atlas textures. */

#include "uir_gradient.h"
#include "uir_batch.h"
#include "uir_compositor.h"
#include "uir_image.h"
#include "uir_path.h"
#include "uir_stencil.h"
#include "uir_svg.h"
#include "uir_viewport.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UIR_GRAD_CACHE_MAX     64
#define UIR_GRAD_MAX_CLIP      8
#define UIR_GRAD_STRIP_LEN     256
#define UIR_GRAD_PI            3.14159265358979323846f

typedef struct {
	unsigned int    fingerprint;
	int             bakeW;
	int             bakeH;
	int             shader;
	int             gpuGeneration;
	int             lruTick;
} uir_grad_cache_entry_t;

static uir_gradient_backend_t g_gradBackend;
static uir_grad_cache_entry_t g_gradCache[UIR_GRAD_CACHE_MAX];
static int                    g_gradCacheCount = 0;
static int                    g_gradGpuGeneration = 1;
static int                    g_gradLruTick = 1;
static uir_color_t            g_gradTint = {1.0f, 1.0f, 1.0f, 1.0f};

/* Image backend draw entry points — synced via UIR_GradientSyncImageBackend. */
static uir_image_backend_t g_gradImageDraw;

void UIR_GradientSyncImageBackend(const uir_image_backend_t *backend)
{
	if (backend) {
		g_gradImageDraw = *backend;
	} else {
		memset(&g_gradImageDraw, 0, sizeof(g_gradImageDraw));
	}
}

void UIR_GradientSetBackend(const uir_gradient_backend_t *backend)
{
	if (backend) {
		g_gradBackend = *backend;
	} else {
		memset(&g_gradBackend, 0, sizeof(g_gradBackend));
	}
}

void UIR_GradientShutdown(void)
{
	g_gradCacheCount = 0;
	memset(g_gradCache, 0, sizeof(g_gradCache));
	memset(&g_gradBackend, 0, sizeof(g_gradBackend));
	memset(&g_gradImageDraw, 0, sizeof(g_gradImageDraw));
}

void UIR_GradientInvalidateGpu(void)
{
	g_gradGpuGeneration++;
	g_gradCacheCount = 0;
	memset(g_gradCache, 0, sizeof(g_gradCache));
}

static const char *uir_grad_skip_ws(const char *p)
{
	while (p && *p && isspace((unsigned char)*p)) {
		++p;
	}
	return p;
}

int UIR_GradientIsBrush(const char *text)
{
	const char *p = uir_grad_skip_ws(text);
	if (!p || !*p) {
		return 0;
	}
	if (!strncmp(p, "linear(", 7) || !strncmp(p, "radial(", 7)) {
		return 1;
	}
	return 0;
}

static int uir_grad_hex_nibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return 10 + (c - 'a');
	}
	if (c >= 'A' && c <= 'F') {
		return 10 + (c - 'A');
	}
	return -1;
}

static int uir_grad_hex_byte(const char *p, int *out)
{
	const int hi = uir_grad_hex_nibble(p[0]);
	const int lo = uir_grad_hex_nibble(p[1]);
	if (hi < 0 || lo < 0) {
		return 0;
	}
	*out = (hi << 4) | lo;
	return 1;
}

static int uir_grad_parse_color(const char *text, const char **endOut, uir_color_t *out)
{
	const char *p = uir_grad_skip_ws(text);
	size_t hexLen = 0;
	int r = 0, g = 0, b = 0, a = 255;

	if (!p || *p != '#') {
		return 0;
	}
	++p;
	while (p[hexLen] && !isspace((unsigned char)p[hexLen]) && p[hexLen] != ',' && p[hexLen] != ')') {
		++hexLen;
	}
	if (hexLen != 6 && hexLen != 8) {
		return 0;
	}
	if (!uir_grad_hex_byte(p + 0, &r) || !uir_grad_hex_byte(p + 2, &g) || !uir_grad_hex_byte(p + 4, &b)) {
		return 0;
	}
	if (hexLen == 8 && !uir_grad_hex_byte(p + 6, &a)) {
		return 0;
	}
	out->r = (float)r / 255.0f;
	out->g = (float)g / 255.0f;
	out->b = (float)b / 255.0f;
	out->a = (float)a / 255.0f;
	if (endOut) {
		*endOut = p + hexLen;
	}
	return 1;
}

static int uir_grad_parse_float(const char *text, const char **endOut, float *out)
{
	char *end = NULL;
	float v;

	text = uir_grad_skip_ws(text);
	if (!text || !*text) {
		return 0;
	}
	v = strtof(text, &end);
	if (end == text) {
		return 0;
	}
	*out = v;
	if (endOut) {
		*endOut = end;
	}
	return 1;
}

static void uir_grad_normalize_stops(uir_gradient_t *g)
{
	int i;
	int missing = 0;

	if (!g || g->stopCount <= 0) {
		return;
	}
	for (i = 0; i < g->stopCount; i++) {
		if (g->stops[i].offset < 0.0f) {
			missing++;
		}
	}
	if (missing == g->stopCount) {
		if (g->stopCount == 1) {
			g->stops[0].offset = 0.0f;
		} else {
			for (i = 0; i < g->stopCount; i++) {
				g->stops[i].offset = (float)i / (float)(g->stopCount - 1);
			}
		}
	} else {
		/* Fill unspecified offsets by even spacing between neighbors. */
		float prev = 0.0f;
		int prevSet = 0;
		for (i = 0; i < g->stopCount; i++) {
			if (g->stops[i].offset >= 0.0f) {
				if (g->stops[i].offset < prev && prevSet) {
					g->stops[i].offset = prev;
				}
				prev = g->stops[i].offset;
				prevSet = 1;
			}
		}
		if (g->stops[0].offset < 0.0f) {
			g->stops[0].offset = 0.0f;
		}
		if (g->stops[g->stopCount - 1].offset < 0.0f) {
			g->stops[g->stopCount - 1].offset = 1.0f;
		}
		for (i = 1; i < g->stopCount - 1; i++) {
			if (g->stops[i].offset < 0.0f) {
				int j = i + 1;
				float t0 = g->stops[i - 1].offset;
				float t1;
				int span;
				while (j < g->stopCount && g->stops[j].offset < 0.0f) {
					++j;
				}
				t1 = g->stops[j].offset;
				span = j - (i - 1);
				{
					int k;
					for (k = i; k < j; k++) {
						const float u = (float)(k - (i - 1)) / (float)span;
						g->stops[k].offset = t0 + (t1 - t0) * u;
					}
				}
				i = j - 1;
			}
		}
	}
	for (i = 0; i < g->stopCount; i++) {
		if (g->stops[i].offset < 0.0f) {
			g->stops[i].offset = 0.0f;
		}
		if (g->stops[i].offset > 1.0f) {
			g->stops[i].offset = 1.0f;
		}
	}
}

static int uir_grad_parse_stop(const char **pp, uir_gradient_stop_t *stop)
{
	const char *p = uir_grad_skip_ws(*pp);
	const char *end = NULL;
	float pct;

	stop->offset = -1.0f;
	if (!uir_grad_parse_color(p, &end, &stop->color)) {
		return 0;
	}
	p = uir_grad_skip_ws(end);
	if (uir_grad_parse_float(p, &end, &pct)) {
		const char *q = uir_grad_skip_ws(end);
		if (*q == '%') {
			stop->offset = pct / 100.0f;
			p = q + 1;
		} else {
			/* Bare number after color is not a valid stop offset in v1. */
			return 0;
		}
	} else {
		p = end;
	}
	*pp = p;
	return 1;
}

uir_status_t UIR_GradientParse(const char *text, uir_gradient_t *out)
{
	const char *p;
	uir_gradient_t g;
	int isLinear;

	if (!text || !out) {
		return UIR_ERR_INVALID_ARG;
	}
	memset(&g, 0, sizeof(g));
	g.centerX = 0.5f;
	g.centerY = 0.5f;
	g.angleDeg = 180.0f;

	p = uir_grad_skip_ws(text);
	if (!strncmp(p, "linear(", 7)) {
		isLinear = 1;
		g.kind = UIR_GRADIENT_LINEAR;
		p += 7;
	} else if (!strncmp(p, "radial(", 7)) {
		isLinear = 0;
		g.kind = UIR_GRADIENT_RADIAL;
		p += 7;
	} else {
		return UIR_ERR_PARSE;
	}

	p = uir_grad_skip_ws(p);
	if (isLinear) {
		float angle = 0.0f;
		const char *end = NULL;
		if (!uir_grad_parse_float(p, &end, &angle)) {
			return UIR_ERR_PARSE;
		}
		p = uir_grad_skip_ws(end);
		if (strncmp(p, "deg", 3) != 0) {
			return UIR_ERR_PARSE;
		}
		p = uir_grad_skip_ws(p + 3);
		g.angleDeg = angle;
		if (*p != ',') {
			return UIR_ERR_PARSE;
		}
		++p;
	} else {
		/* Optional "cx% cy%," before first color. */
		const char *save = p;
		float cx = 0.0f;
		float cy = 0.0f;
		const char *end = NULL;
		int hasCenter = 0;

		if (uir_grad_parse_float(p, &end, &cx)) {
			const char *q = uir_grad_skip_ws(end);
			if (*q == '%') {
				++q;
				q = uir_grad_skip_ws(q);
				if (uir_grad_parse_float(q, &end, &cy)) {
					q = uir_grad_skip_ws(end);
					if (*q == '%') {
						++q;
						q = uir_grad_skip_ws(q);
						if (*q == ',') {
							g.centerX = cx / 100.0f;
							g.centerY = cy / 100.0f;
							p = q + 1;
							hasCenter = 1;
						}
					}
				}
			}
		}
		if (!hasCenter) {
			p = save;
		}
	}

	while (g.stopCount < UIR_GRADIENT_MAX_STOPS) {
		uir_gradient_stop_t stop;
		p = uir_grad_skip_ws(p);
		if (*p == ')') {
			break;
		}
		if (!uir_grad_parse_stop(&p, &stop)) {
			return UIR_ERR_PARSE;
		}
		g.stops[g.stopCount++] = stop;
		p = uir_grad_skip_ws(p);
		if (*p == ',') {
			++p;
			continue;
		}
		if (*p == ')') {
			break;
		}
		return UIR_ERR_PARSE;
	}

	p = uir_grad_skip_ws(p);
	if (*p != ')') {
		return UIR_ERR_PARSE;
	}
	++p;
	p = uir_grad_skip_ws(p);
	if (*p != '\0') {
		return UIR_ERR_PARSE;
	}
	if (g.stopCount < 1) {
		return UIR_ERR_PARSE;
	}

	uir_grad_normalize_stops(&g);
	*out = g;
	return UIR_OK;
}

static void uir_grad_lerp_color(const uir_color_t *a, const uir_color_t *b, float t, uir_color_t *out)
{
	out->r = a->r + (b->r - a->r) * t;
	out->g = a->g + (b->g - a->g) * t;
	out->b = a->b + (b->b - a->b) * t;
	out->a = a->a + (b->a - a->a) * t;
}

static void uir_grad_sample(const uir_gradient_t *g, float t, uir_color_t *out)
{
	int i;

	if (!g || g->stopCount <= 0 || !out) {
		if (out) {
			out->r = out->g = out->b = out->a = 0.0f;
		}
		return;
	}
	if (t <= g->stops[0].offset || g->stopCount == 1) {
		*out = g->stops[0].color;
		return;
	}
	if (t >= g->stops[g->stopCount - 1].offset) {
		*out = g->stops[g->stopCount - 1].color;
		return;
	}
	for (i = 0; i < g->stopCount - 1; i++) {
		const float o0 = g->stops[i].offset;
		const float o1 = g->stops[i + 1].offset;
		if (t >= o0 && t <= o1) {
			const float denom = o1 - o0;
			const float u = (denom > 1e-6f) ? ((t - o0) / denom) : 0.0f;
			uir_grad_lerp_color(&g->stops[i].color, &g->stops[i + 1].color, u, out);
			return;
		}
	}
	*out = g->stops[g->stopCount - 1].color;
}

static void uir_grad_put_px(unsigned char *rgba, int w, int x, int y, const uir_color_t *c)
{
	unsigned char *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4u;
	float r = c->r;
	float g = c->g;
	float b = c->b;
	float a = c->a;
	if (r < 0.0f) {
		r = 0.0f;
	} else if (r > 1.0f) {
		r = 1.0f;
	}
	if (g < 0.0f) {
		g = 0.0f;
	} else if (g > 1.0f) {
		g = 1.0f;
	}
	if (b < 0.0f) {
		b = 0.0f;
	} else if (b > 1.0f) {
		b = 1.0f;
	}
	if (a < 0.0f) {
		a = 0.0f;
	} else if (a > 1.0f) {
		a = 1.0f;
	}
	p[0] = (unsigned char)(r * 255.0f + 0.5f);
	p[1] = (unsigned char)(g * 255.0f + 0.5f);
	p[2] = (unsigned char)(b * 255.0f + 0.5f);
	p[3] = (unsigned char)(a * 255.0f + 0.5f);
}

static int uir_grad_axis_aligned_angle(float angleDeg, int *outHoriz, int *outFlip)
{
	float a = fmodf(angleDeg, 360.0f);
	if (a < 0.0f) {
		a += 360.0f;
	}
	if (fabsf(a - 0.0f) <= 0.5f || fabsf(a - 360.0f) <= 0.5f) {
		/* to top: t increases upward → flip vertical strip */
		*outHoriz = 0;
		*outFlip = 1;
		return 1;
	}
	if (fabsf(a - 90.0f) <= 0.5f) {
		*outHoriz = 1;
		*outFlip = 0;
		return 1;
	}
	if (fabsf(a - 180.0f) <= 0.5f) {
		*outHoriz = 0;
		*outFlip = 0;
		return 1;
	}
	if (fabsf(a - 270.0f) <= 0.5f) {
		*outHoriz = 1;
		*outFlip = 1;
		return 1;
	}
	return 0;
}

uir_status_t UIR_GradientRasterize(const uir_gradient_t *grad, int w, int h, unsigned char *rgba)
{
	int x;
	int y;

	if (!grad || !rgba || w <= 0 || h <= 0 || grad->stopCount < 1) {
		return UIR_ERR_INVALID_ARG;
	}

	if (grad->kind == UIR_GRADIENT_LINEAR) {
		int horiz = 0;
		int flip = 0;
		if (w == UIR_GRAD_STRIP_LEN && h == 1 && uir_grad_axis_aligned_angle(grad->angleDeg, &horiz, &flip) && horiz) {
			for (x = 0; x < w; x++) {
				float t = ((float)x + 0.5f) / (float)w;
				uir_color_t c;
				if (flip) {
					t = 1.0f - t;
				}
				uir_grad_sample(grad, t, &c);
				uir_grad_put_px(rgba, w, x, 0, &c);
			}
			return UIR_OK;
		}
		if (w == 1 && h == UIR_GRAD_STRIP_LEN && uir_grad_axis_aligned_angle(grad->angleDeg, &horiz, &flip) && !horiz) {
			for (y = 0; y < h; y++) {
				float t = ((float)y + 0.5f) / (float)h;
				uir_color_t c;
				if (flip) {
					t = 1.0f - t;
				}
				uir_grad_sample(grad, t, &c);
				uir_grad_put_px(rgba, w, 0, y, &c);
			}
			return UIR_OK;
		}

		{
			/* CSS angle: 0deg = to top. Direction vector in box space (x right, y down). */
			const float rad = grad->angleDeg * (UIR_GRAD_PI / 180.0f);
			const float dx = sinf(rad);
			const float dy = -cosf(rad);
			float minP = 0.0f;
			float maxP = 0.0f;
			int first = 1;
			const float corners[4][2] = {
				{0.0f, 0.0f},
				{1.0f, 0.0f},
				{0.0f, 1.0f},
				{1.0f, 1.0f}
			};
			for (x = 0; x < 4; x++) {
				const float p = corners[x][0] * dx + corners[x][1] * dy;
				if (first || p < minP) {
					minP = p;
				}
				if (first || p > maxP) {
					maxP = p;
				}
				first = 0;
			}
			for (y = 0; y < h; y++) {
				for (x = 0; x < w; x++) {
					const float nx = ((float)x + 0.5f) / (float)w;
					const float ny = ((float)y + 0.5f) / (float)h;
					const float p = nx * dx + ny * dy;
					float t = (maxP > minP + 1e-6f) ? ((p - minP) / (maxP - minP)) : 0.0f;
					uir_color_t c;
					uir_grad_sample(grad, t, &c);
					uir_grad_put_px(rgba, w, x, y, &c);
				}
			}
		}
		return UIR_OK;
	}

	/* Radial: ellipse fitted to box; radius = distance to nearer edge along axes scaled. */
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			const float nx = ((float)x + 0.5f) / (float)w;
			const float ny = ((float)y + 0.5f) / (float)h;
			const float dx = (nx - grad->centerX) / 0.5f;
			const float dy = (ny - grad->centerY) / 0.5f;
			float t = sqrtf(dx * dx + dy * dy);
			uir_color_t c;
			if (t > 1.0f) {
				t = 1.0f;
			}
			uir_grad_sample(grad, t, &c);
			uir_grad_put_px(rgba, w, x, y, &c);
		}
	}
	return UIR_OK;
}

static unsigned int uir_grad_hash_bytes(const void *data, size_t len, unsigned int seed)
{
	const unsigned char *p = (const unsigned char *)data;
	unsigned int h = seed;
	size_t i;
	for (i = 0; i < len; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

static unsigned int uir_grad_fingerprint(const uir_gradient_t *g)
{
	unsigned int h = 2166136261u;
	int i;
	h = uir_grad_hash_bytes(&g->kind, sizeof(g->kind), h);
	h = uir_grad_hash_bytes(&g->angleDeg, sizeof(g->angleDeg), h);
	h = uir_grad_hash_bytes(&g->centerX, sizeof(g->centerX), h);
	h = uir_grad_hash_bytes(&g->centerY, sizeof(g->centerY), h);
	h = uir_grad_hash_bytes(&g->stopCount, sizeof(g->stopCount), h);
	for (i = 0; i < g->stopCount; i++) {
		h = uir_grad_hash_bytes(&g->stops[i], sizeof(g->stops[i]), h);
	}
	return h;
}

static int uir_grad_bucket(float size)
{
	static const int buckets[] = {32, 64, 128, 256};
	int i;
	int s = (int)ceilf(size);
	if (s < 8) {
		s = 8;
	}
	for (i = 0; i < 4; i++) {
		if (s <= buckets[i]) {
			return buckets[i];
		}
	}
	return 256;
}

static void uir_grad_choose_bake_size(const uir_gradient_t *g, float destW, float destH, int *outW, int *outH)
{
	int horiz = 0;
	int flip = 0;

	if (g->kind == UIR_GRADIENT_LINEAR && uir_grad_axis_aligned_angle(g->angleDeg, &horiz, &flip)) {
		if (horiz) {
			*outW = UIR_GRAD_STRIP_LEN;
			*outH = 1;
			return;
		}
		*outW = 1;
		*outH = UIR_GRAD_STRIP_LEN;
		return;
	}
	*outW = uir_grad_bucket(destW);
	*outH = uir_grad_bucket(destH);
}

static uir_grad_cache_entry_t *uir_grad_cache_find(unsigned int fp, int bakeW, int bakeH)
{
	int i;
	for (i = 0; i < g_gradCacheCount; i++) {
		if (g_gradCache[i].fingerprint == fp && g_gradCache[i].bakeW == bakeW &&
			g_gradCache[i].bakeH == bakeH && g_gradCache[i].gpuGeneration == g_gradGpuGeneration &&
			g_gradCache[i].shader != 0) {
			g_gradCache[i].lruTick = ++g_gradLruTick;
			return &g_gradCache[i];
		}
	}
	return NULL;
}

static uir_grad_cache_entry_t *uir_grad_cache_slot(void)
{
	int i;
	int worst = 0;

	if (g_gradCacheCount < UIR_GRAD_CACHE_MAX) {
		uir_grad_cache_entry_t *e = &g_gradCache[g_gradCacheCount++];
		memset(e, 0, sizeof(*e));
		return e;
	}
	for (i = 1; i < g_gradCacheCount; i++) {
		if (g_gradCache[i].lruTick < g_gradCache[worst].lruTick) {
			worst = i;
		}
	}
	memset(&g_gradCache[worst], 0, sizeof(g_gradCache[worst]));
	return &g_gradCache[worst];
}

static uir_status_t uir_grad_ensure(
	const uir_gradient_t *g,
	float destW,
	float destH,
	int *outShader,
	int *outBakeW,
	int *outBakeH
)
{
	unsigned int fp;
	int bakeW = 0;
	int bakeH = 0;
	uir_grad_cache_entry_t *ent;
	unsigned char *rgba;
	char name[64];
	int shader;

	if (!g_gradBackend.createAtlas) {
		return UIR_ERR_NOT_READY;
	}

	uir_grad_choose_bake_size(g, destW, destH, &bakeW, &bakeH);
	fp = uir_grad_fingerprint(g);
	ent = uir_grad_cache_find(fp, bakeW, bakeH);
	if (ent) {
		*outShader = ent->shader;
		*outBakeW = bakeW;
		*outBakeH = bakeH;
		return UIR_OK;
	}

	rgba = (unsigned char *)malloc((size_t)bakeW * (size_t)bakeH * 4u);
	if (!rgba) {
		return UIR_ERR_OVERFLOW;
	}
	if (UIR_GradientRasterize(g, bakeW, bakeH, rgba) != UIR_OK) {
		free(rgba);
		return UIR_ERR_UNSUPPORTED;
	}

	/* Added in OPM: stable synthetic atlas id under MAX_QPATH. */
	snprintf(name, sizeof(name), "*uir_grad/%08x_%dx%d", fp, bakeW, bakeH);
	shader = g_gradBackend.createAtlas(name, rgba, bakeW, bakeH);
	free(rgba);
	if (!shader) {
		return UIR_ERR_NOT_READY;
	}

	ent = uir_grad_cache_slot();
	ent->fingerprint = fp;
	ent->bakeW = bakeW;
	ent->bakeH = bakeH;
	ent->shader = shader;
	ent->gpuGeneration = g_gradGpuGeneration;
	ent->lruTick = ++g_gradLruTick;

	*outShader = shader;
	*outBakeW = bakeW;
	*outBakeH = bakeH;
	return UIR_OK;
}

uir_status_t UIR_GradientEnsureShader(
	const char *brush,
	float destW,
	float destH,
	int *outShader,
	int *outBakeW,
	int *outBakeH
)
{
	uir_gradient_t grad;
	uir_status_t st;

	if (!brush || !outShader || !outBakeW || !outBakeH) {
		return UIR_ERR_INVALID_ARG;
	}
	if (!(destW > 0.0f) || !(destH > 0.0f)) {
		return UIR_ERR_INVALID_ARG;
	}

	st = UIR_GradientParse(brush, &grad);
	if (st != UIR_OK) {
		return st;
	}
	return uir_grad_ensure(&grad, destW, destH, outShader, outBakeW, outBakeH);
}

/* ---- draw (mirrors image clipped stretch path; uses image backend) ---- */

static uir_status_t uir_grad_build_clip_path(
	const char *pathD,
	float x,
	float y,
	float w,
	float h,
	float viewW,
	float viewH,
	float rotationDeg,
	uir_path_t *outPath
)
{
	uir_path_t local;
	uir_path_t mapped;
	uir_path_t rotated;
	uir_viewbox_t view;
	uir_rect_t dest;
	uir_parse_result_t pr;
	uir_status_t st;
	float sx;
	float sy;
	float scale;
	float flatness;

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

	sx = (view.width > 1e-6f) ? (dest.w / view.width) : 1.0f;
	sy = (view.height > 1e-6f) ? (dest.h / view.height) : 1.0f;
	scale = (sx < sy) ? sx : sy;
	flatness = (scale > 1e-6f) ? (0.25f / scale) : 0.25f;
	if (flatness < 0.01f) {
		flatness = 0.01f;
	}
	if (flatness > 0.25f) {
		flatness = 0.25f;
	}

	pr = UIR_SvgParsePathD(pathD, flatness, &local);
	if (pr.status != UIR_OK) {
		return pr.status;
	}

	st = UIR_SvgMapPathToRect(&local, &view, &dest, UIR_FIT_STRETCH, &mapped);
	UIR_PathFree(&local);
	if (st != UIR_OK) {
		return st;
	}

	if (rotationDeg != 0.0f) {
		const float cx = dest.x + dest.w * 0.5f;
		const float cy = dest.y + dest.h * 0.5f;
		st = UIR_PathRotate(&mapped, cx, cy, rotationDeg, &rotated);
		UIR_PathFree(&mapped);
		if (st != UIR_OK) {
			return st;
		}
		*outPath = rotated;
	} else {
		*outPath = mapped;
	}
	return UIR_OK;
}

static uir_rect_t uir_grad_intersect_rect(const uir_rect_t *a, const uir_rect_t *b)
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

static uir_rect_t uir_grad_union_path_bounds(const uir_path_t *paths, int pathCount)
{
	uir_rect_t unionBounds;
	uir_rect_t bounds;
	int i;

	memset(&unionBounds, 0, sizeof(unionBounds));
	if (!paths || pathCount <= 0) {
		return unionBounds;
	}
	if (UIR_PathBounds(&paths[0], &unionBounds) != UIR_OK) {
		memset(&unionBounds, 0, sizeof(unionBounds));
		return unionBounds;
	}
	for (i = 1; i < pathCount; i++) {
		if (UIR_PathBounds(&paths[i], &bounds) != UIR_OK) {
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

static int uir_grad_path_is_axis_aligned_rect(const uir_path_t *path)
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

static void uir_grad_set_tint(const uir_color_t *tintRgba)
{
	float c[4];
	if (!g_gradImageDraw.setColor) {
		return;
	}
	if (tintRgba) {
		g_gradTint = *tintRgba;
		c[0] = tintRgba->r;
		c[1] = tintRgba->g;
		c[2] = tintRgba->b;
		c[3] = tintRgba->a;
		g_gradImageDraw.setColor(c);
	} else {
		g_gradTint.r = g_gradTint.g = g_gradTint.b = g_gradTint.a = 1.0f;
		g_gradImageDraw.setColor(NULL);
	}
}

static void uir_grad_emit_quad(
	int shader,
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

	if (!g_gradImageDraw.drawStretchPic) {
		return;
	}

	if (!rotated && UIR_BatchEnabled()) {
		if (UIR_BatchQuad(shader, qx, qy, qw, qh, s1, t1, s2, t2, &g_gradTint) == UIR_OK) {
			return;
		}
	}

	UIR_BatchFlush();

	if (!rotated || !g_gradImageDraw.drawTrianglePic) {
		g_gradImageDraw.drawStretchPic(qx, qy, qw, qh, s1, t1, s2, t2, shader);
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
	g_gradImageDraw.drawTrianglePic(tri, uv, shader);

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
	g_gradImageDraw.drawTrianglePic(tri, uv, shader);
}

uir_status_t UIR_GradientDrawClipped(
	const char *brush,
	float x,
	float y,
	float w,
	float h,
	const char *const *clipPathD,
	int clipPathCount,
	float viewW,
	float viewH,
	float rotationDeg,
	const uir_color_t *tintRgba
)
{
	uir_gradient_t grad;
	uir_status_t st;
	int shader = 0;
	int bakeW = 0;
	int bakeH = 0;
	const uir_viewport_t *vp = UIR_CompositorViewport();
	uir_rect_t bounds;
	uir_rect_t clipAabb;
	uir_path_t builtPaths[UIR_GRAD_MAX_CLIP];
	int builtCount = 0;
	int useStencil = 0;
	int useAxisScissor = 0;
	int pushedClip = 0;
	int i;
	float pivotX;
	float pivotY;
	float cosr;
	float sinr;
	int rotated;

	if (!brush || !(w > 0.0f) || !(h > 0.0f)) {
		return UIR_ERR_INVALID_ARG;
	}
	if (!vp || !g_gradImageDraw.drawStretchPic) {
		return UIR_ERR_NOT_READY;
	}

	st = UIR_GradientParse(brush, &grad);
	if (st != UIR_OK) {
		return st;
	}
	st = uir_grad_ensure(&grad, w, h, &shader, &bakeW, &bakeH);
	if (st != UIR_OK) {
		return st;
	}

	bounds.x = x;
	bounds.y = y;
	bounds.w = w;
	bounds.h = h;

	if (clipPathCount > UIR_GRAD_MAX_CLIP) {
		clipPathCount = UIR_GRAD_MAX_CLIP;
	}
	for (i = 0; i < clipPathCount; i++) {
		UIR_PathInit(&builtPaths[i]);
		st = uir_grad_build_clip_path(clipPathD[i], x, y, w, h, viewW, viewH, rotationDeg, &builtPaths[i]);
		if (st != UIR_OK) {
			int j;
			for (j = 0; j <= i; j++) {
				UIR_PathFree(&builtPaths[j]);
			}
			return st;
		}
		builtCount++;
	}

	if (builtCount > 0) {
		clipAabb = uir_grad_union_path_bounds(builtPaths, builtCount);
		clipAabb = uir_grad_intersect_rect(&clipAabb, &bounds);
		if (builtCount == 1 && uir_grad_path_is_axis_aligned_rect(&builtPaths[0])) {
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
			for (i = 0; i < builtCount; i++) {
				UIR_PathFree(&builtPaths[i]);
			}
			return st;
		}
		for (i = 0; i < builtCount; i++) {
			st = UIR_StencilWritePath(vp, &builtPaths[i]);
			if (st != UIR_OK) {
				UIR_EndShapeClip();
				for (; i < builtCount; i++) {
					UIR_PathFree(&builtPaths[i]);
				}
				return st;
			}
		}
		st = UIR_BeginShapeClipDraw();
		if (st != UIR_OK) {
			UIR_EndShapeClip();
			for (i = 0; i < builtCount; i++) {
				UIR_PathFree(&builtPaths[i]);
			}
			return st;
		}
	} else if (useAxisScissor) {
		UIR_PushClipRect(clipAabb.x, clipAabb.y, clipAabb.w, clipAabb.h);
		pushedClip = 1;
	} else {
		UIR_PushClipRect(x, y, w, h);
		pushedClip = 1;
	}

	uir_grad_set_tint(tintRgba);
	pivotX = x + w * 0.5f;
	pivotY = y + h * 0.5f;
	rotated = (rotationDeg != 0.0f) ? 1 : 0;
	if (rotated) {
		const float rad = rotationDeg * (UIR_GRAD_PI / 180.0f);
		cosr = cosf(rad);
		sinr = sinf(rad);
	} else {
		cosr = 1.0f;
		sinr = 0.0f;
	}

	uir_grad_emit_quad(shader, x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, pivotX, pivotY, cosr, sinr, rotated);

	if (g_gradImageDraw.setColor) {
		g_gradImageDraw.setColor(NULL);
	}

	if (useStencil) {
		UIR_EndShapeClip();
	} else if (pushedClip) {
		UIR_PopClipRect();
	}

	for (i = 0; i < builtCount; i++) {
		UIR_PathFree(&builtPaths[i]);
	}
	(void)bakeW;
	(void)bakeH;
	return UIR_OK;
}
