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
#ifndef UID_MODAL_H
#define UID_MODAL_H

#include "uid_backend.h"
#include "uid_document.h"

/* Added in OPM: mount/unmount definition modals via ui_om_modal (or custom cvar). */
void UID_SyncModals(uid_document_t *doc, const uid_backend_t *backend);

bool UID_IsModalActive(const uid_document_t *doc);
uid_node_id_t UID_GetModalRoot(const uid_document_t *doc);

void UID_MountModal(uid_document_t *doc, const char *modalId);
void UID_UnmountModal(uid_document_t *doc);

/* Dispatch cvar name for modal show/hide actions (default ui_om_modal). */
const char *UID_DefaultModalCvarName(void);

/* Find a button under modalRoot with modal-role="<role>" (e.g. confirm, cancel). */
uid_node_id_t UID_FindModalRoleButton(const uid_document_t *doc, uid_node_id_t modalRoot, const char *role);

#endif /* UID_MODAL_H */
