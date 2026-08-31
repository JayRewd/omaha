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
#ifndef UID_INPUT_H
#define UID_INPUT_H

#include "uid_backend.h"
#include "uid_document.h"
#include "uid_types.h"

#include <vector>

/* Quake-compatible key numbers so adapters can pass engine keys unchanged. */
enum {
	UID_KEY_TAB = 9,
	UID_KEY_ENTER = 13,
	UID_KEY_ESCAPE = 27,
	UID_KEY_SPACE = 32,
	UID_KEY_BACKSPACE = 127,
	UID_KEY_UPARROW = 132,
	UID_KEY_DOWNARROW = 133,
	UID_KEY_LEFTARROW = 134,
	UID_KEY_RIGHTARROW = 135,
	UID_KEY_ALT = 136,
	UID_KEY_CTRL = 137,
	UID_KEY_SHIFT = 138,
	UID_KEY_INS = 139,
	UID_KEY_DEL = 140,
	UID_KEY_HOME = 143,
	UID_KEY_END = 144
};

#define UID_POINTER_BUTTON_LEFT 1

/* Document-order focus candidates; nonnegative tab-index sorts first. */
void UID_BuildFocusOrder(const uid_document_t *doc, std::vector<uid_node_id_t> *out);

uid_node_id_t UID_GetFocusedNode(const uid_document_t *doc);
void          UID_SetFocus(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend);

/* Hover / press / release / click and wheel scroll on overflow=scroll. */
void UID_HandlePointer(
	uid_document_t *doc,
	const uid_pointer_state_t *pointer,
	unsigned int realtime,
	const uid_backend_t *backend
);

/* Returns true when the key event is consumed. */
bool UID_HandleKey(uid_document_t *doc, int key, bool down, unsigned time, const uid_backend_t *backend);

/* Append one Unicode code point to the focused text input. */
bool UID_HandleChar(uid_document_t *doc, unsigned codepoint, const uid_backend_t *backend);

bool UID_IsInteractiveKind(uid_node_kind_t kind);
bool UID_NodeEffectivelyVisible(const uid_document_t *doc, uid_node_id_t id);
bool UID_NodeEffectivelyEnabled(const uid_document_t *doc, uid_node_id_t id);

#endif /* UID_INPUT_H */
