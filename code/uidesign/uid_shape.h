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
#ifndef UID_SHAPE_H
#define UID_SHAPE_H

#include "uid_backend.h"
#include "uid_diag.h"
#include "uid_document.h"
#include "uid_types.h"
#include "uid_value.h"

#include <string>
#include <vector>

struct uid_shape_resolve_params_t {
	float                     parentWidth;
	float                     parentHeight;
	float                     uiPxScale; /* Added in OPM: authored shape prop px multiplier */
	const uid_property_set_t *parentProps; /* owner box props (e.g. fill) */
	const uid_property_set_t *shapeProps;  /* instance props (e.g. radius) */
	const uid_limits_t       *limits;      /* optional; defaults used if null */
	const uid_backend_t      *backend;     /* Added in OPM: resolve {cvar:} shape props */
	uid_document_t           *doc;         /* Added in OPM: live numeric edge props */
	uid_node_id_t             nodeId;    /* Added in OPM: foreach item context for expr eval */
	/* Added in OPM: optional overrides for {parent.fill}/{parent.stroke} without cloning props. */
	const char               *fillOverride;        /* nullptr = use parentProps fill */
	const char               *strokeOverride;      /* nullptr = use parentProps stroke */
	const char               *strokeWidthOverride; /* nullptr = use parentProps stroke-width */
};

/*
 * Merge instance attributes with shape declaration defaults / required props.
 * Writes the typed argument bag into *outProps (declaration prop names only).
 */
uid_result_t UID_BuildShapeInstanceProps(
	const uid_shape_def_t *shape,
	const uid_property_set_t *instanceProps,
	uid_property_set_t *outProps,
	uid_diag_list_t *diags,
	const uid_source_location_t &loc
);

/*
 * Evaluate shape path fill/d expressions after layout.
 * Owner-sized shapes use parentWidth/parentHeight; intrinsic shapes still
 * evaluate expressions against the laid-out owner box unless callers pass
 * the intrinsic size as parent dimensions.
 * Missing declared props use declaration defaults before expression eval.
 */
uid_result_t UID_ResolveShape(
	const uid_shape_def_t *shape,
	const uid_shape_resolve_params_t *params,
	std::vector<uid_resolved_path_t> *outPaths,
	uid_diag_list_t *diags
);

#endif /* UID_SHAPE_H */
