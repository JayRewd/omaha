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
#ifndef UID_STYLE_H
#define UID_STYLE_H

#include "uid_backend.h"
#include "uid_document.h"

#include <string>

/*
 * Added in Omaha: Stage 3 computed style cache (ui_style_cache).
 * Rebuilds when properties.Version(), ui px scale, cvar epoch, or interaction
 * flags change. Paint reads the POD instead of re-parsing props each frame.
 */
void UID_SetStyleCache(int enabled);
int  UID_StyleCacheEnabled(void);

void UID_InvalidateComputedStyle(uid_node_state_t *st);

/*
 * Ensure st->computedStyle is valid for this node; rebuild on key mismatch.
 * Returns nullptr only when doc/id/state are invalid.
 */
const uid_computed_style_t *UID_EnsureComputedStyle(
	uid_document_t *doc,
	uid_node_id_t id,
	const uid_backend_t *backend
);

/*
 * Evaluate styleExprs["fill"] into *out without writing properties (avoids
 * Version() bumps that defeat shape cache). Returns true when resolved.
 */
bool UID_ResolveFillStyleTernary(
	uid_document_t *doc,
	uid_node_id_t id,
	const uid_backend_t *backend,
	std::string *out
);

#endif /* UID_STYLE_H */
