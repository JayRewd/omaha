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
#ifndef UIR_MESHCACHE_H
#define UIR_MESHCACHE_H

#include "uir_batch.h"
#include "uir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Added in OPM: Stage D tessellated mesh cache (fill + stroke). */
void UIR_MeshCacheSetEnabled(int enable);
void UIR_MeshCacheClear(void);
int UIR_MeshCacheEnabled(void);

/* Contour fingerprint + draw params → direct-map key. */
unsigned UIR_MeshCacheKeyFill(
	const uir_path_t *path,
	const uir_color_t *rgba,
	int crisp,
	float fringeFbPx
);
unsigned UIR_MeshCacheKeyStroke(
	const uir_path_t *path,
	const uir_color_t *rgba,
	float widthPx,
	int crisp
);

/*
 * Lookup: returns 1 on hit. *outVerts / *outIdx point at cache storage
 * (valid until overwrite/clear — do not free).
 */
int UIR_MeshCacheLookup(
	unsigned key,
	const uir_vert_t **outVerts,
	int *outVertCount,
	const unsigned short **outIdx,
	int *outIdxCount
);

/* Store a tessellated mesh (malloc'd copy; frees prior slot contents). */
void UIR_MeshCacheStore(
	unsigned key,
	const uir_vert_t *verts,
	int vertCount,
	const unsigned short *idx,
	int idxCount
);

#ifdef __cplusplus
}
#endif

#endif /* UIR_MESHCACHE_H */
