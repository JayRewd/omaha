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
#ifndef UID_COLLECTION_H
#define UID_COLLECTION_H

#include "uid_backend.h"
#include "uid_document.h"

/* Added in OPM: composable foreach / collection scopes. */
void UID_SyncCollections(uid_document_t *doc, const uid_backend_t *backend);

uid_node_id_t UID_FindCollectionScope(const uid_document_t *doc, uid_node_id_t from);

bool UID_StepCollectionIndex(uid_document_t *doc, uid_node_id_t scopeId, int delta, const uid_backend_t *backend);

bool UID_SetCollectionIndex(uid_document_t *doc, uid_node_id_t scopeId, int index, const uid_backend_t *backend);

/* Added in OPM: foreach lifetime fade alpha for {item.lifetime_alpha} / wrap opacity. */
float UID_EvalItemLifetimeAlpha(const uid_document_t *doc, uid_node_id_t nodeId);

/* Added in OPM: windowed foreach scroll helpers for layout (synthetic extent). */
bool  UID_ScrollParentHasWindowedForeach(const uid_document_t *doc, uid_node_id_t parentId);
float UID_WindowedForeachSyntheticExtentH(const uid_document_t *doc, uid_node_id_t parentId);

/*
 * Added in OPM: snapshot collection rows for string aggregates (join) without a
 * nearby source= / foreach scope. Prefers document XML <sources>, else host query.
 */
bool UID_FetchCollectionEntries(
	const uid_document_t *doc,
	const uid_backend_t *backend,
	const char *sourceId,
	std::vector<uid_collection_entry_t> *outItems
);

#endif /* UID_COLLECTION_H */
