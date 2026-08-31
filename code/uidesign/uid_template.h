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
#ifndef UID_TEMPLATE_H
#define UID_TEMPLATE_H

#include "uid_diag.h"
#include "uid_document.h"

/*
 * Expand <use> templates into the runtime node list, apply property cascade,
 * namespace IDs, build idIndex, and allocate matching states.
 */
uid_result_t UID_ExpandDocument(uid_document_t *doc, uid_diag_list_t *diags);

/*
 * Clone a template definition subtree into an existing expanded document.
 * propSource supplies template prop overrides via its property bag (like <use>).
 */
uid_node_id_t UID_CloneTemplateRoot(
	uid_document_t *doc,
	const char *templateId,
	const uid_node_def_t &propSource,
	uid_diag_list_t *diags
);

/* Expand deferred <use template="{item.*}"> nodes (foreach clones expand inline in uid_collection). */
void UID_ExpandDeferredUses(uid_document_t *doc, uid_diag_list_t *diags);

#endif /* UID_TEMPLATE_H */
