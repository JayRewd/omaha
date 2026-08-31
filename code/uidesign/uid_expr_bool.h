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
#ifndef UID_EXPR_BOOL_H
#define UID_EXPR_BOOL_H

#include "uid_backend.h"
#include "uid_document.h"
#include "uid_expr.h"

#include <string>

struct uid_bool_lookup_ctx_t {
	const uid_backend_t           *backend;
	const uid_document_t          *doc;
	uid_node_id_t                  nodeId;
	const uid_collection_entry_t  *item;
	int                            itemIndex;
	int                            itemCount;
	int                            selectedIndex;
};

/* Returns true when attr is exactly "{inner}" (single brace-wrapped expression). */
bool UID_ParseBraceBoolExpr(const char *attrValue, std::string *outInner);

/* Convert legacy visible-if="cvar:name=value" into bool expression text. */
bool UID_VisibleIfToBoolExpr(const std::string &visibleIf, std::string *outExpr);

/* Convert legacy visible-if-index keywords into bool expression text. */
bool UID_VisibleIfIndexToBoolExpr(const std::string &visibleIfIndex, std::string *outExpr);

bool UID_EvalBool(
	const char                    *expr,
	const uid_bool_lookup_ctx_t   *ctx,
	const uid_expr_limits_t       *limits,
	bool                          *out,
	std::string                   *diagMessage
);

/*
 * Evaluate style ternary "boolExpr ? literal : literal" (inner text of {...}).
 * Literals are trimmed raw attribute values (#RRGGBBAA, 5px, etc.).
 */
bool UID_EvalStyleTernary(
	const char                    *expr,
	const uid_bool_lookup_ctx_t   *ctx,
	const uid_expr_limits_t       *limits,
	std::string                   *outValue,
	std::string                   *diagMessage
);

#endif /* UID_EXPR_BOOL_H */
