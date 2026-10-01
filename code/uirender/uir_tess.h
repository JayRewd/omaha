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
#ifndef UIR_TESS_H
#define UIR_TESS_H

#include "uir_batch.h"
#include "uir_path.h"

#ifdef __cplusplus
extern "C" {
#endif

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
);

void UIR_TessSetStats(uir_stats_t *stats);

float UIR_TessDefaultFringeFbPx(float avgScale);

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
);

#ifdef __cplusplus
}
#endif

#endif /* UIR_TESS_H */
