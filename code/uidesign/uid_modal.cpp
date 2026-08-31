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

#include "uid_modal.h"

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "uid_input.h"
#include "uid_xml.h"

namespace {

constexpr const char *kDefaultModalCvar = "ui_om_modal";

void TrimInPlace(std::string *s)
{
	if (!s) {
		return;
	}
	size_t start = 0;
	while (start < s->size() && std::isspace(static_cast<unsigned char>((*s)[start]))) {
		++start;
	}
	size_t end = s->size();
	while (end > start && std::isspace(static_cast<unsigned char>((*s)[end - 1]))) {
		--end;
	}
	if (start == 0 && end == s->size()) {
		return;
	}
	*s = s->substr(start, end - start);
}

bool ReadCvarString(const uid_backend_t *backend, const char *name, std::string *out)
{
	if (!backend || !backend->cvarDescribe || !name || !out) {
		return false;
	}
	char buf[1024];
	buf[0] = '\0';
	int flags = 0;
	if (!backend->cvarDescribe(name, &flags, buf, sizeof(buf))) {
		return false;
	}
	(void)flags;
	*out = buf;
	return true;
}

void MarkDirty(uid_document_t *doc, uid_dirty_flags_t flags)
{
	if (doc) {
		doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty | flags);
	}
}

void CollectModalButtons(const uid_document_t *doc, uid_node_id_t id, std::vector<uid_node_id_t> *out)
{
	if (!doc || id == UID_INVALID_NODE_ID || !out) {
		return;
	}
	const uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node) {
		return;
	}
	if (node->kind == UID_NODE_BUTTON) {
		out->push_back(id);
	}
	for (uid_node_id_t child : node->children) {
		CollectModalButtons(doc, child, out);
	}
}

void FocusModalDefault(uid_document_t *doc, const uid_backend_t *backend)
{
	if (!doc || !backend) {
		return;
	}
	const uid_node_id_t root = UID_GetModalRoot(doc);
	if (root == UID_INVALID_NODE_ID) {
		return;
	}
	uid_node_id_t focusId = UID_FindModalRoleButton(doc, root, "confirm");
	if (focusId == UID_INVALID_NODE_ID) {
		focusId = UID_FindModalRoleButton(doc, root, "cancel");
	}
	if (focusId == UID_INVALID_NODE_ID) {
		std::vector<uid_node_id_t> buttons;
		CollectModalButtons(doc, root, &buttons);
		if (!buttons.empty()) {
			focusId = buttons.front();
		}
	}
	if (focusId != UID_INVALID_NODE_ID) {
		UID_SetFocus(doc, focusId, backend);
	}
}

} // namespace

const char *UID_DefaultModalCvarName(void)
{
	return kDefaultModalCvar;
}

bool UID_IsModalActive(const uid_document_t *doc)
{
	return doc && !doc->activeModalId.empty();
}

uid_node_id_t UID_GetModalRoot(const uid_document_t *doc)
{
	if (!doc || doc->activeModalId.empty()) {
		return UID_INVALID_NODE_ID;
	}
	return doc->modalRootNode;
}

void UID_UnmountModal(uid_document_t *doc)
{
	if (!doc || doc->activeModalId.empty()) {
		return;
	}

	for (size_t i = doc->modalOverlayBase; i < doc->nodes.size(); ++i) {
		const std::string &id = doc->nodes[i].id;
		if (!id.empty()) {
			doc->idIndex.erase(id);
		}
	}

	if (doc->modalOverlayBase < doc->nodes.size()) {
		doc->nodes.resize(doc->modalOverlayBase);
		doc->states.resize(doc->modalOverlayBase);
	}

	doc->activeModalId.clear();
	doc->modalOverlayBase = 0;
	doc->modalRootNode = UID_INVALID_NODE_ID;
	doc->modalOpenerNode = UID_INVALID_NODE_ID;
	doc->keybindPending.active = false;
	MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
}

void UID_MountModal(uid_document_t *doc, const char *modalId)
{
	if (!doc || !modalId || !modalId[0]) {
		return;
	}

	auto it = doc->definitions.modals.find(modalId);
	if (it == doc->definitions.modals.end()) {
		return;
	}

	if (doc->activeModalId == modalId) {
		return;
	}

	/* Preserve opener across Unmount (clears when a modal was already active). */
	const uid_node_id_t openerKeep = doc->modalOpenerNode;
	UID_UnmountModal(doc);
	doc->modalOpenerNode = openerKeep;

	const uid_modal_def_t &def = it->second;
	const size_t base = doc->nodes.size();
	doc->modalOverlayBase = base;

	for (size_t i = 0; i < def.nodes.size(); ++i) {
		uid_node_def_t n = def.nodes[i];
		for (uid_node_id_t &child : n.children) {
			if (child != UID_INVALID_NODE_ID) {
				child = static_cast<uid_node_id_t>(base + static_cast<size_t>(child));
			}
		}
		/*
		 * Fixed in OPM: foreachTemplateRoot indexes foreachTemplateNodes (usually 0),
		 * not doc->nodes — do not offset it by modalOverlayBase or ExpandForeach
		 * never clones rows (relative dropdowns stayed empty / 2px tall).
		 */
		if (n.foreachScopeId != UID_INVALID_NODE_ID) {
			n.foreachScopeId = static_cast<uid_node_id_t>(base + static_cast<size_t>(n.foreachScopeId));
		}
		/* Fixed in OPM: stamp source→collectionSource on mount (defs are outside the canvas apply pass). */
		UID_ApplyCollectionAndIndexFields(&n);
		doc->nodes.push_back(n);
		uid_node_state_t st;
		UID_InitNodeState(&st);
		doc->states.push_back(st);
		if (!n.id.empty()) {
			doc->idIndex[n.id] = static_cast<uid_node_id_t>(doc->nodes.size() - 1);
		}
	}

	doc->modalRootNode =
		def.rootNode != UID_INVALID_NODE_ID ? static_cast<uid_node_id_t>(base + static_cast<size_t>(def.rootNode))
											: UID_INVALID_NODE_ID;
	doc->activeModalId = modalId;
	MarkDirty(
		doc,
		static_cast<uid_dirty_flags_t>(UID_DIRTY_STRUCTURE | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT)
	);
}

uid_node_id_t UID_FindModalRoleButton(const uid_document_t *doc, uid_node_id_t modalRoot, const char *role)
{
	if (!doc || modalRoot == UID_INVALID_NODE_ID || !role || !role[0]) {
		return UID_INVALID_NODE_ID;
	}
	std::vector<uid_node_id_t> buttons;
	CollectModalButtons(doc, modalRoot, &buttons);
	for (uid_node_id_t id : buttons) {
		const uid_node_def_t *node = UID_GetNode(doc, id);
		if (!node) {
			continue;
		}
		const char *r = node->properties.GetCStr("modal-role", nullptr);
		if (r && std::strcmp(r, role) == 0) {
			return id;
		}
	}
	return UID_INVALID_NODE_ID;
}

void UID_SyncModals(uid_document_t *doc, const uid_backend_t *backend)
{
	if (!doc || !backend) {
		return;
	}

	std::string want;
	if (!ReadCvarString(backend, kDefaultModalCvar, &want)) {
		want.clear();
	}
	TrimInPlace(&want);

	if (want == doc->activeModalId) {
		return;
	}

	if (want.empty()) {
		UID_UnmountModal(doc);
		return;
	}

	UID_MountModal(doc, want.c_str());
	FocusModalDefault(doc, backend);
}
