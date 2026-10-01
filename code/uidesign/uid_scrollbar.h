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
#ifndef UID_SCROLLBAR_H
#define UID_SCROLLBAR_H

#include "uid_backend.h"
#include "uid_diag.h"
#include "uid_document.h"

/* Compile-time: expand scrollbar templates onto overflow=scroll containers. */
uid_result_t UID_ExpandScrollbars(uid_document_t *doc, uid_diag_list_t *diags);

/* Layout overlay rail/track/thumb after container children. */
void UID_LayoutScrollbar(
	uid_document_t *doc,
	uid_node_id_t containerId,
	bool parentEnabled,
	float fbScale,
	const uid_backend_t *backend
);

/* Pointer handling for thumb drag and track page-click (before content). */
bool UID_HandleScrollbarPointer(
	uid_document_t *doc,
	float x,
	float y,
	int buttons,
	int lastButtons,
	const uid_backend_t *backend
);

/* Engine fallback when chrome subtree is missing after expand. */
void UID_PaintScrollbarFallback(
	uid_document_t *doc,
	uid_node_id_t containerId,
	const uid_backend_t *backend
);

/* Paint layout track/thumb rects (template colors when present). */
void UID_PaintScrollbarChrome(
	uid_document_t *doc,
	uid_node_id_t containerId,
	const uid_backend_t *backend
);

#endif /* UID_SCROLLBAR_H */
