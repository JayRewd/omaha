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
#ifndef UID_EXPR_H
#define UID_EXPR_H

#include "uid_types.h"

#include <string>

/*
 * Lookup callback for dotted property paths such as "parent.width".
 * Return true and write *out on success; false if the path is unknown.
 */
typedef bool (*uid_expr_lookup_fn)(void *userdata, const char *path, double *out);

struct uid_expr_limits_t {
	int maxExprBytes;
	int maxExprNodes;
};

void UID_DefaultExprLimits(uid_expr_limits_t *out);

/*
 * Evaluate a numeric expression. No assignment.
 * Whitelisted functions: abs(x), min(a,b), max(a,b), clamp(x,lo,hi).
 * On failure, returns false and optionally writes diagMessage.
 */
bool UID_EvalNumber(
	const char *expr,
	uid_expr_lookup_fn lookup,
	void *userdata,
	const uid_expr_limits_t *limits,
	double *out,
	std::string *diagMessage
);

/*
 * Expand `{...}` embeds in text using UID_EvalNumber for each expression.
 * Number formatting is locale-independent (always uses '.' as decimal separator).
 */
bool UID_InterpolateString(
	const char *text,
	uid_expr_lookup_fn lookup,
	void *userdata,
	const uid_expr_limits_t *limits,
	std::string *out,
	std::string *diagMessage
);

#endif /* UID_EXPR_H */
