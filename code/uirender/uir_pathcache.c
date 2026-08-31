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

#include "uir_pathcache.h"

#include "uir_compositor.h"
#include "uir_path.h"
#include "uir_svg.h"

#include <string.h>

/* Added in OPM: Stage D path cache (256 direct-mapped slots). */
#define UIR_PATH_CACHE_SIZE 256

typedef struct {
	unsigned int key;
	int          valid;
	uir_path_t   path;
} uir_path_cache_entry_t;

static uir_path_cache_entry_t g_pathCache[UIR_PATH_CACHE_SIZE];

static unsigned int uir_fnv1a_add(unsigned int h, const void *data, size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	size_t i;

	for (i = 0; i < len; i++) {
		h ^= (unsigned int)p[i];
		h *= 16777619u;
	}
	return h;
}

static unsigned int uir_path_cache_key(
	const char *pathD,
	const uir_rect_t *dest,
	const uir_viewbox_t *viewBox,
	uir_fit_mode_t fit,
	float rotationDeg,
	int crisp
)
{
	unsigned int h = 2166136261u;

	h = uir_fnv1a_add(h, pathD, strlen(pathD));
	h = uir_fnv1a_add(h, dest, sizeof(*dest));
	h = uir_fnv1a_add(h, viewBox, sizeof(*viewBox));
	h = uir_fnv1a_add(h, &fit, sizeof(fit));
	h = uir_fnv1a_add(h, &rotationDeg, sizeof(rotationDeg));
	h = uir_fnv1a_add(h, &crisp, sizeof(crisp));
	return h;
}

void UIR_PathCacheClear(void)
{
	int i;

	for (i = 0; i < UIR_PATH_CACHE_SIZE; i++) {
		if (g_pathCache[i].valid) {
			UIR_PathFree(&g_pathCache[i].path);
			g_pathCache[i].valid = 0;
		}
	}
}

/* Added in OPM: store mapped path; *out points at the slot (zero-copy). */
static uir_status_t uir_path_cache_store(unsigned int key, const uir_path_t *src, const uir_path_t **out)
{
	uir_path_cache_entry_t *slot;
	int i;

	slot = &g_pathCache[key % UIR_PATH_CACHE_SIZE];
	if (slot->valid) {
		UIR_PathFree(&slot->path);
		slot->valid = 0;
	}

	UIR_PathInit(&slot->path);
	for (i = 0; i < src->contourCount; i++) {
		uir_contour_t *dstContour = NULL;
		uir_status_t st;
		int p;

		st = UIR_PathBeginContour(&slot->path, &dstContour);
		if (st != UIR_OK) {
			UIR_PathFree(&slot->path);
			return st;
		}
		for (p = 0; p < src->contours[i].count; p++) {
			st = UIR_ContourAddPoint(dstContour, src->contours[i].points[p].x, src->contours[i].points[p].y);
			if (st != UIR_OK) {
				UIR_PathFree(&slot->path);
				return st;
			}
		}
		if (src->contours[i].closed) {
			UIR_ContourClose(dstContour);
		}
	}
	slot->path.fillRule = src->fillRule;
	slot->key = key;
	slot->valid = 1;
	if (out) {
		*out = &slot->path;
	}
	return UIR_OK;
}

/* Added in OPM: zero-copy hit — pointer into the cache slot (or NULL). */
static const uir_path_t *uir_path_cache_lookup(unsigned int key)
{
	uir_path_cache_entry_t *slot = &g_pathCache[key % UIR_PATH_CACHE_SIZE];

	if (!slot->valid || slot->key != key) {
		return NULL;
	}
	return &slot->path;
}

uir_status_t UIR_GetMappedPathCached(
	const char *pathD,
	const uir_rect_t *dest,
	const uir_viewbox_t *viewBox,
	uir_fit_mode_t fit,
	float rotationDeg,
	int crisp,
	const uir_path_t **out
)
{
	uir_path_t local;
	uir_path_t mapped;
	uir_path_t rotated;
	uir_parse_result_t pr;
	uir_status_t st;
	uir_stats_t *stats;
	const uir_path_t *drawPath;
	const uir_path_t *cached;
	unsigned int cacheKey;
	float sx;
	float sy;
	float scale;
	float flatness;

	if (!pathD || !dest || !viewBox || !out) {
		return UIR_ERR_INVALID_ARG;
	}
	*out = NULL;

	stats = UIR_CompositorStats();
	cacheKey = uir_path_cache_key(pathD, dest, viewBox, fit, rotationDeg, crisp);
	cached = uir_path_cache_lookup(cacheKey);
	if (cached) {
		if (stats) {
			stats->pathCacheHits++;
		}
		*out = cached;
		return UIR_OK;
	}
	if (stats) {
		stats->pathCacheMisses++;
	}

	/* Added in OPM: path-space flatness so dest-space chord error stays ~0.25px. */
	sx = (viewBox->width > 1e-6f) ? (dest->w / viewBox->width) : 1.0f;
	sy = (viewBox->height > 1e-6f) ? (dest->h / viewBox->height) : 1.0f;
	scale = sx < sy ? sx : sy;
	flatness = (scale > 1e-6f) ? (0.25f / scale) : 0.25f;
	if (crisp) {
		flatness = (scale > 1e-6f) ? (0.5f / scale) : 0.5f;
	}
	if (flatness < 0.01f) {
		flatness = 0.01f;
	}
	if (flatness > 0.5f) {
		flatness = 0.5f;
	}

	pr = UIR_SvgParsePathD(pathD, flatness, &local);
	if (pr.status != UIR_OK) {
		return pr.status;
	}

	st = UIR_SvgMapPathToRect(&local, viewBox, dest, fit, &mapped);
	UIR_PathFree(&local);
	if (st != UIR_OK) {
		return st;
	}

	drawPath = &mapped;
	if (rotationDeg != 0.0f) {
		const float cx = dest->x + dest->w * 0.5f;
		const float cy = dest->y + dest->h * 0.5f;
		st = UIR_PathRotate(&mapped, cx, cy, rotationDeg, &rotated);
		UIR_PathFree(&mapped);
		if (st != UIR_OK) {
			return st;
		}
		drawPath = &rotated;
	}

	st = uir_path_cache_store(cacheKey, drawPath, out);
	if (drawPath == &rotated) {
		UIR_PathFree(&rotated);
	} else {
		UIR_PathFree(&mapped);
	}
	return st;
}
