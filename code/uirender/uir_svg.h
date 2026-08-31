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
#ifndef UIR_SVG_H
#define UIR_SVG_H

#include "uir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uir_status_t status;
	int          offset; /* byte offset of parse error, if any */
} uir_parse_result_t;

/* Locale-independent float lexer; accepts scientific notation. */
uir_status_t UIR_SvgParseFloat(const char **cursor, float *out);

/* Parse SVG path `d` into flattened contours in viewBox/local space. */
uir_parse_result_t UIR_SvgParsePathD(const char *d, float flatness, uir_path_t *outPath);

/* Parse polygon points="x,y x,y ..." into one closed contour. */
uir_parse_result_t UIR_SvgParsePolygonPoints(const char *points, uir_path_t *outPath);

/*
 * Map a path from viewBox space into destination rectangle.
 * fit=CONTAIN uses uniform scale centered in dest; STRETCH scales axes independently.
 */
uir_status_t UIR_SvgMapPathToRect(
	const uir_path_t    *src,
	const uir_viewbox_t *viewBox,
	const uir_rect_t    *dest,
	uir_fit_mode_t       fit,
	uir_path_t          *out
);

/* Convenience: map a single local point using contain-fit into a centered diameter. */
void UIR_SvgMapPointContain(
	float localX,
	float localY,
	const uir_viewbox_t *viewBox,
	float cx,
	float cy,
	float diameter,
	float *outX,
	float *outY
);

#ifdef __cplusplus
}
#endif

#endif /* UIR_SVG_H */
