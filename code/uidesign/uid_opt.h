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
#ifndef UID_OPT_H
#define UID_OPT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Added in OPM: runtime toggles for UI CPU optimizations (A/B + rollback). */
enum {
	UID_OPT_PARSE_CACHE = 1u << 0,
	UID_OPT_PAINT_CULL = 1u << 1,
	UID_OPT_BIND_CULL = 1u << 2,
	UID_OPT_CVAR_MEMO = 1u << 3,
	UID_OPT_EXPR_CACHE = 1u << 4,
	UID_OPT_TEXT_CACHE = 1u << 5,
	UID_OPT_COLLECTION_CULL = 1u << 6,
	UID_OPT_SHAPE_CACHE = 1u << 7,
	UID_OPT_ALL = (UID_OPT_PARSE_CACHE | UID_OPT_PAINT_CULL | UID_OPT_BIND_CULL | UID_OPT_CVAR_MEMO
				   | UID_OPT_EXPR_CACHE | UID_OPT_TEXT_CACHE | UID_OPT_COLLECTION_CULL
				   | UID_OPT_SHAPE_CACHE)
};

void     UID_SetOptFlags(unsigned flags);
unsigned UID_OptFlags(void);
int      UID_OptEnabled(unsigned flag);

#ifdef __cplusplus
}
#endif

#endif /* UID_OPT_H */
