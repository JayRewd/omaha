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

#include "uir_meshcache.h"

#include <stdlib.h>
#include <string.h>

/* Added in OPM: Stage D tessellated mesh cache. */
#define UIR_MESH_CACHE_SIZE 256

typedef struct {
	unsigned key;
	int      valid;
	int      vertCount;
	int      idxCount;
	uir_vert_t *verts;
	unsigned short *indices;
} uir_mesh_cache_entry_t;

static uir_mesh_cache_entry_t g_meshCache[UIR_MESH_CACHE_SIZE];
static int g_meshCacheEnabled = 1;

static unsigned uir_fnv1a_add(unsigned h, const void *data, size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	size_t i;

	for (i = 0; i < len; i++) {
		h ^= (unsigned)p[i];
		h *= 16777619u;
	}
	return h;
}

/* Added in OPM: contour fingerprint (path already in dest space). */
static unsigned uir_mesh_path_fingerprint(unsigned h, const uir_path_t *path)
{
	int i;

	if (!path) {
		return h;
	}
	h = uir_fnv1a_add(h, &path->fillRule, sizeof(path->fillRule));
	h = uir_fnv1a_add(h, &path->contourCount, sizeof(path->contourCount));
	for (i = 0; i < path->contourCount; i++) {
		const uir_contour_t *c = &path->contours[i];
		h = uir_fnv1a_add(h, &c->closed, sizeof(c->closed));
		h = uir_fnv1a_add(h, &c->count, sizeof(c->count));
		if (c->count > 0 && c->points) {
			h = uir_fnv1a_add(h, c->points, (size_t)c->count * sizeof(uir_point_t));
		}
	}
	return h;
}

static void uir_mesh_cache_entry_free(uir_mesh_cache_entry_t *slot)
{
	if (!slot) {
		return;
	}
	free(slot->verts);
	free(slot->indices);
	slot->verts = NULL;
	slot->indices = NULL;
	slot->vertCount = 0;
	slot->idxCount = 0;
	slot->valid = 0;
}

void UIR_MeshCacheSetEnabled(int enable)
{
	g_meshCacheEnabled = enable ? 1 : 0;
	if (!g_meshCacheEnabled) {
		UIR_MeshCacheClear();
	}
}

int UIR_MeshCacheEnabled(void)
{
	return g_meshCacheEnabled;
}

void UIR_MeshCacheClear(void)
{
	int i;

	for (i = 0; i < UIR_MESH_CACHE_SIZE; i++) {
		if (g_meshCache[i].valid) {
			uir_mesh_cache_entry_free(&g_meshCache[i]);
		}
	}
}

unsigned UIR_MeshCacheKeyFill(
	const uir_path_t *path,
	const uir_color_t *rgba,
	int crisp,
	float fringeFbPx
)
{
	unsigned h = 2166136261u;
	float strokeWidth = 0.0f;
	int kind = 0; /* fill */

	h = uir_fnv1a_add(h, &kind, sizeof(kind));
	h = uir_mesh_path_fingerprint(h, path);
	if (rgba) {
		h = uir_fnv1a_add(h, rgba, sizeof(*rgba));
	}
	h = uir_fnv1a_add(h, &crisp, sizeof(crisp));
	h = uir_fnv1a_add(h, &fringeFbPx, sizeof(fringeFbPx));
	h = uir_fnv1a_add(h, &strokeWidth, sizeof(strokeWidth));
	return h;
}

unsigned UIR_MeshCacheKeyStroke(
	const uir_path_t *path,
	const uir_color_t *rgba,
	float widthPx,
	int crisp
)
{
	unsigned h = 2166136261u;
	float fringeFbPx = 0.0f;
	int kind = 1; /* stroke */

	h = uir_fnv1a_add(h, &kind, sizeof(kind));
	h = uir_mesh_path_fingerprint(h, path);
	if (rgba) {
		h = uir_fnv1a_add(h, rgba, sizeof(*rgba));
	}
	h = uir_fnv1a_add(h, &crisp, sizeof(crisp));
	h = uir_fnv1a_add(h, &fringeFbPx, sizeof(fringeFbPx));
	h = uir_fnv1a_add(h, &widthPx, sizeof(widthPx));
	return h;
}

int UIR_MeshCacheLookup(
	unsigned key,
	const uir_vert_t **outVerts,
	int *outVertCount,
	const unsigned short **outIdx,
	int *outIdxCount
)
{
	uir_mesh_cache_entry_t *slot;

	if (!g_meshCacheEnabled || !outVerts || !outVertCount || !outIdx || !outIdxCount) {
		return 0;
	}
	slot = &g_meshCache[key % UIR_MESH_CACHE_SIZE];
	if (!slot->valid || slot->key != key || !slot->verts || !slot->indices) {
		return 0;
	}
	*outVerts = slot->verts;
	*outVertCount = slot->vertCount;
	*outIdx = slot->indices;
	*outIdxCount = slot->idxCount;
	return 1;
}

void UIR_MeshCacheStore(
	unsigned key,
	const uir_vert_t *verts,
	int vertCount,
	const unsigned short *idx,
	int idxCount
)
{
	uir_mesh_cache_entry_t *slot;
	uir_vert_t *vCopy;
	unsigned short *iCopy;

	if (!g_meshCacheEnabled || !verts || !idx || vertCount <= 0 || idxCount <= 0) {
		return;
	}

	vCopy = (uir_vert_t *)malloc((size_t)vertCount * sizeof(uir_vert_t));
	iCopy = (unsigned short *)malloc((size_t)idxCount * sizeof(unsigned short));
	if (!vCopy || !iCopy) {
		free(vCopy);
		free(iCopy);
		return;
	}
	memcpy(vCopy, verts, (size_t)vertCount * sizeof(uir_vert_t));
	memcpy(iCopy, idx, (size_t)idxCount * sizeof(unsigned short));

	slot = &g_meshCache[key % UIR_MESH_CACHE_SIZE];
	if (slot->valid) {
		uir_mesh_cache_entry_free(slot);
	}
	slot->key = key;
	slot->verts = vCopy;
	slot->indices = iCopy;
	slot->vertCount = vertCount;
	slot->idxCount = idxCount;
	slot->valid = 1;
}
