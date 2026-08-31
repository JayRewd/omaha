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
#ifndef UIR_PATHCACHE_H
#define UIR_PATHCACHE_H

#include "uir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Added in OPM: drop all cached mapped paths (resolution / renderer / shutdown). */
void UIR_PathCacheClear(void);

/*
 * Added in OPM: shared mapped-path lookup for draw + clip paths.
 * On success *out points at a cache-owned path — caller must NOT free it.
 * Clips should only read; if a mutable copy is required, deep-copy first.
 */
uir_status_t UIR_GetMappedPathCached(
	const char *pathD,
	const uir_rect_t *dest,
	const uir_viewbox_t *viewBox,
	uir_fit_mode_t fit,
	float rotationDeg,
	int crisp,
	const uir_path_t **out
);

#ifdef __cplusplus
}
#endif

#endif /* UIR_PATHCACHE_H */
