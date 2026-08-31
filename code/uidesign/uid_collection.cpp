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

#include "uid_collection.h"

#include "uid_binding.h"
#include "uid_document.h"
#include "uid_layout.h"
#include "uid_template.h"
#include "uid_xml.h"
#include "uid_modal.h"
#include "uid_opt.h"
#include "uid_profile.h"
#include "uid_value.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

/* Added in OPM: RAII for nested ui_profile detail phases. */
struct UidProfScope {
	uid_prof_phase_t phase;
	bool             armed;

	explicit UidProfScope(uid_prof_phase_t p, bool enable = true)
		: phase(p)
		, armed(enable)
	{
		if (armed) {
			UID_ProfileBegin(phase);
		}
	}

	~UidProfScope()
	{
		if (armed) {
			UID_ProfileEnd(phase);
		}
	}

	UidProfScope(const UidProfScope &) = delete;
	UidProfScope &operator=(const UidProfScope &) = delete;
};

void MarkDirty(uid_document_t *doc, uid_dirty_flags_t flags)
{
	if (doc) {
		doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty | flags);
	}
}

std::vector<uid_node_id_t> BuildParentMap(const uid_document_t *doc)
{
	std::vector<uid_node_id_t> parent(doc ? doc->nodes.size() : 0, UID_INVALID_NODE_ID);
	if (!doc) {
		return parent;
	}
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		for (uid_node_id_t c : doc->nodes[i].children) {
			if (c >= 0 && static_cast<size_t>(c) < parent.size()) {
				parent[static_cast<size_t>(c)] = static_cast<uid_node_id_t>(i);
			}
		}
	}
	return parent;
}

void CollectDescendants(const uid_document_t *doc, uid_node_id_t root, std::set<uid_node_id_t> *out)
{
	if (!doc || !out || root < 0 || static_cast<size_t>(root) >= doc->nodes.size()) {
		return;
	}
	out->insert(root);
	for (uid_node_id_t c : doc->nodes[static_cast<size_t>(root)].children) {
		CollectDescendants(doc, c, out);
	}
}

void ApplyItemContextToNode(
	uid_node_def_t *node,
	const uid_collection_entry_t &item,
	int itemIndex,
	int itemCount,
	int selectedIndex,
	const char *displayMode
);

static void ApplyItemContextToSubtree(
	uid_document_t *doc,
	uid_node_id_t rootId,
	const uid_collection_entry_t &item,
	int itemIndex,
	int itemCount,
	int selectedIndex,
	const char *displayMode
)
{
	std::set<uid_node_id_t> nodes;
	CollectDescendants(doc, rootId, &nodes);
	for (uid_node_id_t id : nodes) {
		if (id < 0 || static_cast<size_t>(id) >= doc->nodes.size()) {
			continue;
		}
		ApplyItemContextToNode(
			&doc->nodes[static_cast<size_t>(id)],
			item,
			itemIndex,
			itemCount,
			selectedIndex,
			displayMode
		);
	}
}

bool IsCollectionScope(const uid_node_def_t &node)
{
	return !node.collectionSource.empty();
}

bool PropBool(const uid_node_def_t &node, const char *name, bool fallback)
{
	const char *v = node.properties.GetCStr(name, nullptr);
	if (!v || !v[0]) {
		return fallback;
	}
	bool out = fallback;
	if (!UID_ParseBool(v, &out, nullptr)) {
		return fallback;
	}
	return out;
}

/* Added in OPM: mode=window visible count — viewport / row-height + overscan. */
constexpr int kWindowFallbackVisible = 32;
constexpr int kWindowOverscan = 2;

float WindowRowHeightPx(const uid_document_t *doc, const uid_node_def_t *fn)
{
	if (!doc || !fn || !fn->hasForeachRowHeight || fn->foreachRowHeight <= 0.0f) {
		return 0.0f;
	}
	return UID_ScaleAuthoredPx(doc, fn->foreachRowHeight);
}

int WindowVisibleCount(const uid_document_t *doc, const uid_node_def_t *fn, float viewportH)
{
	const float rowH = WindowRowHeightPx(doc, fn);
	if (rowH > 0.0f && viewportH > 0.0f) {
		const int visible =
			static_cast<int>(std::ceil(static_cast<double>(viewportH / rowH))) + kWindowOverscan;
		return std::max(1, visible);
	}
	return kWindowFallbackVisible;
}

uid_node_id_t FindOverflowScrollAncestor(
	const uid_document_t *doc,
	uid_node_id_t from,
	const std::vector<uid_node_id_t> &parents
)
{
	if (!doc || from < 0 || static_cast<size_t>(from) >= doc->nodes.size()) {
		return UID_INVALID_NODE_ID;
	}
	for (uid_node_id_t p = parents[static_cast<size_t>(from)]; p != UID_INVALID_NODE_ID;
		 p = parents[static_cast<size_t>(p)]) {
		if (p < 0 || static_cast<size_t>(p) >= doc->nodes.size()) {
			break;
		}
		uid_overflow_t ov = UID_OVERFLOW_NONE;
		UID_ParseOverflow(doc->nodes[static_cast<size_t>(p)].properties.GetCStr("overflow", "none"), &ov, nullptr);
		if (ov == UID_OVERFLOW_SCROLL) {
			return p;
		}
	}
	return UID_INVALID_NODE_ID;
}

/* Added in OPM: drive collectionScrollOffset from overflow scrollY (discrete index window). */
void SyncWindowOffsetFromOverflow(
	uid_document_t *doc,
	uid_node_id_t foreachId,
	uid_node_id_t scopeId,
	const std::vector<uid_node_id_t> &parents,
	const uid_node_def_t *fn,
	uid_node_state_t *scopeSt
)
{
	if (!doc || !fn || !scopeSt || scopeId < 0) {
		return;
	}
	const float rowH = WindowRowHeightPx(doc, fn);
	if (rowH <= 0.0f) {
		return;
	}
	const uid_node_id_t overflowId = FindOverflowScrollAncestor(doc, foreachId, parents);
	if (overflowId == UID_INVALID_NODE_ID || static_cast<size_t>(overflowId) >= doc->states.size()) {
		return;
	}
	uid_node_state_t *ovSt = &doc->states[static_cast<size_t>(overflowId)];
	const int visible = WindowVisibleCount(doc, fn, ovSt->contentBox.h);
	const int count = scopeSt->collectionItemCount;
	const int maxOff = std::max(0, count - visible);
	int offset = static_cast<int>(std::floor(static_cast<double>(ovSt->scrollY / rowH)));
	if (offset < 0) {
		offset = 0;
	}
	if (offset > maxOff) {
		offset = maxOff;
	}
	if (offset != scopeSt->collectionScrollOffset) {
		scopeSt->collectionScrollOffset = offset;
		MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_STRUCTURE | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
	}
}

uid_node_id_t FindWindowForeachUnderScope(const uid_document_t *doc, uid_node_id_t scopeId)
{
	if (!doc || scopeId < 0 || static_cast<size_t>(scopeId) >= doc->nodes.size()) {
		return UID_INVALID_NODE_ID;
	}
	std::function<uid_node_id_t(uid_node_id_t)> walk;
	walk = [&](uid_node_id_t id) -> uid_node_id_t {
		if (id < 0 || static_cast<size_t>(id) >= doc->nodes.size()) {
			return UID_INVALID_NODE_ID;
		}
		const uid_node_def_t &n = doc->nodes[static_cast<size_t>(id)];
		if (n.kind == UID_NODE_FOREACH) {
			const std::string mode = n.foreachMode.empty() ? "all" : n.foreachMode;
			if (mode == "window") {
				return id;
			}
		}
		for (uid_node_id_t c : n.children) {
			const uid_node_id_t hit = walk(c);
			if (hit != UID_INVALID_NODE_ID) {
				return hit;
			}
		}
		return UID_INVALID_NODE_ID;
	};
	return walk(scopeId);
}

void EnsureSelectionInWindow(
	uid_document_t *doc,
	uid_node_id_t scopeId,
	uid_node_state_t *st,
	int selected
)
{
	if (!doc || !st) {
		return;
	}
	const uid_node_id_t foreachId = FindWindowForeachUnderScope(doc, scopeId);
	if (foreachId == UID_INVALID_NODE_ID) {
		return;
	}
	const uid_node_def_t *fn = &doc->nodes[static_cast<size_t>(foreachId)];
	const std::vector<uid_node_id_t> parents = BuildParentMap(doc);
	const uid_node_id_t overflowId = FindOverflowScrollAncestor(doc, foreachId, parents);
	float viewportH = 0.0f;
	if (overflowId != UID_INVALID_NODE_ID && static_cast<size_t>(overflowId) < doc->states.size()) {
		viewportH = doc->states[static_cast<size_t>(overflowId)].contentBox.h;
	}
	const int visible = WindowVisibleCount(doc, fn, viewportH);
	if (selected < st->collectionScrollOffset) {
		st->collectionScrollOffset = selected;
	} else if (selected >= st->collectionScrollOffset + visible) {
		st->collectionScrollOffset = selected - visible + 1;
	}
	if (st->collectionScrollOffset < 0) {
		st->collectionScrollOffset = 0;
	}
	const float rowH = WindowRowHeightPx(doc, fn);
	if (rowH > 0.0f && overflowId != UID_INVALID_NODE_ID
		&& static_cast<size_t>(overflowId) < doc->states.size()) {
		doc->states[static_cast<size_t>(overflowId)].scrollY =
			static_cast<float>(st->collectionScrollOffset) * rowH;
	}
}

int FindItemIndexByValue(const std::vector<uid_collection_entry_t> &items, const std::string &value)
{
	for (size_t i = 0; i < items.size(); ++i) {
		if (items[i].value == value) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int ResolveFallbackIndex(
	const uid_document_t *doc,
	const uid_node_def_t *scope,
	const std::vector<uid_collection_entry_t> &items
)
{
	if (items.empty()) {
		return -1;
	}
	if (doc && scope && !scope->collectionSource.empty()) {
		auto it = doc->definitions.sources.find(scope->collectionSource);
		if (it != doc->definitions.sources.end()) {
			const int idx = FindItemIndexByValue(items, it->second.defaultValue);
			if (idx >= 0) {
				return idx;
			}
		}
	}
	if (scope && scope->hasCollectionDefaultIndex) {
		const int idx = scope->collectionDefaultIndex;
		if (idx < 0) {
			return -1;
		}
		if (idx < static_cast<int>(items.size())) {
			return idx;
		}
	}
	return -1;
}

static void ClampCollectionSelection(uid_node_state_t *st)
{
	if (!st) {
		return;
	}
	if (st->collectionSelectedIndex >= st->collectionItemCount ||
		(st->collectionSelectedIndex >= 0 &&
		 static_cast<size_t>(st->collectionSelectedIndex) >= st->collectionItems.size())) {
		st->collectionSelectedIndex = -1;
	}
}

const char *CollectionDisplayMode(const uid_node_def_t &scope)
{
	if (scope.collectionDisplay == "value") {
		return "value";
	}
	return "label";
}

static void PropagateForeachMetadata(
	uid_document_t *doc,
	uid_node_id_t rootId,
	uid_node_id_t scopeId,
	int itemIndex
)
{
	std::set<uid_node_id_t> nodes;
	CollectDescendants(doc, rootId, &nodes);
	for (uid_node_id_t id : nodes) {
		if (id < 0 || static_cast<size_t>(id) >= doc->nodes.size()) {
			continue;
		}
		uid_node_def_t &dst = doc->nodes[static_cast<size_t>(id)];
		dst.foreachGenerated = true;
		dst.foreachScopeId = scopeId;
		dst.foreachItemIndex = itemIndex;
	}
}

uid_node_id_t ExpandDeferredUseIfReady(
	uid_document_t *doc,
	uid_node_id_t nodeId,
	const uid_collection_entry_t *item,
	int itemIndex,
	int itemCount,
	int selectedIndex,
	const char *displayMode
)
{
	if (!doc || nodeId < 0 || static_cast<size_t>(nodeId) >= doc->nodes.size()) {
		return nodeId;
	}
	const uid_node_def_t &node = doc->nodes[static_cast<size_t>(nodeId)];
	const bool foreachUse = node.foreachGenerated && !node.deferredUse;
	if (node.kind != UID_NODE_USE || node.deferredUseExpanded) {
		return nodeId;
	}
	if (!node.deferredUse && !foreachUse) {
		return nodeId;
	}
	if (node.templateId.empty() || node.templateId.find('{') != std::string::npos) {
		return nodeId;
	}
	if (doc->definitions.templates.find(node.templateId) == doc->definitions.templates.end()) {
		return nodeId;
	}

	uid_node_def_t useCopy = node;
	if (foreachUse && useCopy.id.empty()) {
		useCopy.id = "__foreach_use." + std::to_string(itemIndex) + "." + node.templateId;
	}
	const std::string templateId = useCopy.templateId;
	const uid_node_id_t expanded = UID_CloneTemplateRoot(doc, templateId.c_str(), useCopy, nullptr);
	if (expanded < 0) {
		return nodeId;
	}
	if (item) {
		ApplyItemContextToSubtree(doc, expanded, *item, itemIndex, itemCount, selectedIndex, displayMode);
	}
	if (foreachUse) {
		PropagateForeachMetadata(doc, expanded, node.foreachScopeId, itemIndex);
	}
	uid_node_def_t &useNode = doc->nodes[static_cast<size_t>(nodeId)];
	useNode.deferredUseExpanded = true;
	useNode.properties.Set("visible", "false");
	MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_STRUCTURE | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
	return expanded;
}

bool LoadXmlCollectionItems(
	const uid_document_t *doc,
	const std::string &sourceId,
	std::vector<uid_collection_entry_t> *outItems
)
{
	if (!doc || !outItems) {
		return false;
	}
	auto it = doc->definitions.sources.find(sourceId);
	if (it == doc->definitions.sources.end()) {
		return false;
	}
	outItems->clear();
	outItems->reserve(it->second.items.size());
	for (size_t i = 0; i < it->second.items.size(); ++i) {
		const uid_source_item_def_t &src = it->second.items[i];
		uid_collection_entry_t item;
		item.key = std::to_string(i);
		item.value = src.value;
		item.label = src.label.empty() ? src.value : src.label;
		item.fields = src.fields;
		outItems->push_back(item);
	}
	return true;
}

bool SubstituteItemToken(
	std::string *text,
	const uid_collection_entry_t &item,
	int itemIndex,
	int itemCount,
	int selectedIndex,
	const char *displayMode,
	bool expandFields
)
{
	if (!text || text->empty()) {
		return false;
	}
	bool changed = false;
	std::string out;
	out.reserve(text->size());
	for (size_t i = 0; i < text->size();) {
		if (text->at(i) != '{') {
			out.push_back(text->at(i++));
			continue;
		}
		const size_t end = text->find('}', i + 1);
		if (end == std::string::npos) {
			out.push_back(text->at(i++));
			continue;
		}
		const std::string key = text->substr(i + 1, end - i - 1);
		std::string rep;
		bool matched = false;
		if (key == "item.index") {
			rep = std::to_string(itemIndex);
			matched = true;
		} else if (key == "item.key") {
			rep = item.key;
			matched = true;
		} else if (key == "item.value") {
			rep = item.value;
			matched = true;
		} else if (key == "item.label") {
			rep = item.label;
			matched = true;
		} else if (key == "item.display") {
			rep = (displayMode && std::strcmp(displayMode, "value") == 0) ? item.value : item.label;
			matched = true;
		} else if (key == "item.count") {
			rep = std::to_string(itemCount);
			matched = true;
		} else if (key == "item.selected") {
			rep = (itemIndex == selectedIndex) ? "true" : "false";
			matched = true;
		} else if (key.rfind("item.field.", 0) == 0) {
			/*
			 * Props keep {item.field.*} live for SyncBindings (message feeds, etc.).
			 * Changed in OPM: action handlers bake field values at foreach expand so
			 * set-cvar/invoke payloads (e.g. pause cmd) are concrete at click time.
			 */
			if (!expandFields) {
				out += text->substr(i, end - i + 1);
				i = end + 1;
				continue;
			}
			const std::string fieldName = key.substr(11);
			auto fit = item.fields.find(fieldName);
			rep = (fit != item.fields.end()) ? fit->second : "";
			matched = true;
		} else if (key == "item.lifetime_alpha") {
			/* Added in OPM: live token; SyncExprBoundProps / numeric lookup resolve. */
			out += text->substr(i, end - i + 1);
			i = end + 1;
			continue;
		}
		if (matched) {
			out += rep;
			changed = true;
		} else {
			out += text->substr(i, end - i + 1);
		}
		i = end + 1;
	}
	if (changed) {
		*text = out;
	}
	return changed;
}

void ApplyItemContextToNode(
	uid_node_def_t *node,
	const uid_collection_entry_t &item,
	int itemIndex,
	int itemCount,
	int selectedIndex,
	const char *displayMode
)
{
	if (!node) {
		return;
	}
	SubstituteItemToken(&node->text, item, itemIndex, itemCount, selectedIndex, displayMode, false);
	SubstituteItemToken(&node->bind, item, itemIndex, itemCount, selectedIndex, displayMode, false);
	SubstituteItemToken(&node->setValue, item, itemIndex, itemCount, selectedIndex, displayMode, false);
	SubstituteItemToken(&node->visibleIf, item, itemIndex, itemCount, selectedIndex, displayMode, false);
	SubstituteItemToken(&node->visibleIfIndex, item, itemIndex, itemCount, selectedIndex, displayMode, false);
	SubstituteItemToken(&node->visibleExpr, item, itemIndex, itemCount, selectedIndex, displayMode, false);
	SubstituteItemToken(&node->enabledExpr, item, itemIndex, itemCount, selectedIndex, displayMode, false);
	for (auto &kv : node->styleExprs) {
		SubstituteItemToken(&kv.second, item, itemIndex, itemCount, selectedIndex, displayMode, false);
	}
	if (node->hasSetIndex && node->setIndexValue < 0) {
		node->setIndexValue = itemIndex;
	}
	std::string tmp;
	for (const auto &kv : node->properties.Attrs()) {
		tmp = kv.second.value;
		if (SubstituteItemToken(&tmp, item, itemIndex, itemCount, selectedIndex, displayMode, false)) {
			node->properties.Set(kv.first.c_str(), tmp.c_str());
		}
	}
	if (node->kind == UID_NODE_USE && node->deferredUse) {
		std::string tid;
		if (node->properties.Get("template", &tid) && !tid.empty()) {
			node->templateId = tid;
		}
	}
	for (uid_action_handler_t &handler : node->handlers) {
		for (uid_action_t &act : handler.actions) {
			/* Changed in OPM: bake item.field.* into click actions (set-cvar cmd payloads). */
			SubstituteItemToken(&act.target, item, itemIndex, itemCount, selectedIndex, displayMode, true);
			SubstituteItemToken(&act.value, item, itemIndex, itemCount, selectedIndex, displayMode, true);
			SubstituteItemToken(&act.name, item, itemIndex, itemCount, selectedIndex, displayMode, true);
		}
	}
	UID_RegisterCvarBoundProps(node);
}

uid_node_id_t CloneForeachSubtree(
	uid_document_t *doc,
	const std::vector<uid_node_def_t> &tmpl,
	uid_node_id_t tmplRoot,
	uid_node_id_t scopeId,
	int itemIndex,
	const uid_collection_entry_t &item,
	int itemCount,
	int selectedIndex,
	const char *displayMode
)
{
	if (!doc || tmplRoot < 0 || static_cast<size_t>(tmplRoot) >= tmpl.size()) {
		return UID_INVALID_NODE_ID;
	}

	std::map<uid_node_id_t, uid_node_id_t> idMap;
	std::vector<uid_node_id_t> stack;
	stack.push_back(tmplRoot);
	while (!stack.empty()) {
		const uid_node_id_t srcId = stack.back();
		stack.pop_back();
		if (idMap.count(srcId)) {
			continue;
		}
		const uid_node_def_t &src = tmpl[static_cast<size_t>(srcId)];
		uid_node_def_t dst = src;
		dst.children.clear();
		/*
		 * Fixed in OPM: nested <foreach> must keep foreachTemplateNodes / Root so
		 * inner lists (e.g. kill-feed weapon icon source) can expand after the
		 * outer row is cloned. Non-foreach clones drop any stale template payload.
		 */
		if (src.kind != UID_NODE_FOREACH) {
			dst.foreachTemplateNodes.clear();
			dst.foreachTemplateRoot = UID_INVALID_NODE_ID;
		}
		dst.foreachGenerated = true;
		dst.foreachScopeId = scopeId;
		dst.foreachItemIndex = itemIndex;
		const uid_node_id_t newId = static_cast<uid_node_id_t>(doc->nodes.size());
		idMap[srcId] = newId;
		doc->nodes.push_back(dst);
		doc->states.emplace_back();
		UID_InitNodeState(&doc->states.back());
		for (uid_node_id_t c : src.children) {
			stack.push_back(c);
		}
	}
	for (const auto &kv : idMap) {
		uid_node_def_t &dst = doc->nodes[static_cast<size_t>(kv.second)];
		const uid_node_def_t &src = tmpl[static_cast<size_t>(kv.first)];
		ApplyItemContextToNode(&dst, item, itemIndex, itemCount, selectedIndex, displayMode);
	}
	for (const auto &kv : idMap) {
		const uid_node_id_t dstId = kv.second;
		const uid_node_def_t &src = tmpl[static_cast<size_t>(kv.first)];
		doc->nodes[static_cast<size_t>(dstId)].children.clear();
		for (uid_node_id_t c : src.children) {
			auto it = idMap.find(c);
			if (it != idMap.end()) {
				const uid_node_id_t childId = ExpandDeferredUseIfReady(
					doc,
					it->second,
					&item,
					itemIndex,
					itemCount,
					selectedIndex,
					displayMode
				);
				doc->nodes[static_cast<size_t>(dstId)].children.push_back(childId);
			}
		}
		if (!doc->nodes[static_cast<size_t>(dstId)].id.empty()) {
			doc->idIndex[doc->nodes[static_cast<size_t>(dstId)].id] = dstId;
		}
	}
	const uid_node_id_t rootId = idMap[tmplRoot];
	return ExpandDeferredUseIfReady(
		doc,
		rootId,
		&item,
		itemIndex,
		itemCount,
		selectedIndex,
		displayMode
	);
}

void RemoveExpandedForeach(uid_document_t *doc, uid_node_id_t foreachId)
{
	if (!doc || foreachId < 0 || static_cast<size_t>(foreachId) >= doc->nodes.size()) {
		return;
	}
	uid_node_def_t *fn = &doc->nodes[static_cast<size_t>(foreachId)];
	std::set<uid_node_id_t> remove;
	for (uid_node_id_t c : fn->children) {
		CollectDescendants(doc, c, &remove);
	}
	if (remove.empty()) {
		fn->children.clear();
		return;
	}

	/* Preserve cvar-dispatched modal overlay nodes (appended after modalOverlayBase). */
	size_t overlayBase = 0;
	std::vector<uid_node_def_t> savedOverlayNodes;
	std::vector<uid_node_state_t> savedOverlayStates;
	uid_node_id_t savedModalRootOffset = UID_INVALID_NODE_ID;
	std::string savedModalId;
	if (!doc->activeModalId.empty() && doc->modalOverlayBase > 0 && doc->modalOverlayBase <= doc->nodes.size()) {
		overlayBase = doc->modalOverlayBase;
		savedOverlayNodes.assign(doc->nodes.begin() + overlayBase, doc->nodes.end());
		savedOverlayStates.assign(doc->states.begin() + overlayBase, doc->states.end());
		savedModalId = doc->activeModalId;
		if (doc->modalRootNode != UID_INVALID_NODE_ID &&
			static_cast<size_t>(doc->modalRootNode) >= overlayBase) {
			savedModalRootOffset =
				static_cast<uid_node_id_t>(static_cast<size_t>(doc->modalRootNode) - overlayBase);
		}
	}

	const size_t rebuildLimit = savedOverlayNodes.empty() ? doc->nodes.size() : overlayBase;

	std::vector<uid_node_def_t> newNodes;
	std::vector<uid_node_state_t> newStates;
	std::map<uid_node_id_t, uid_node_id_t> remap;

	for (size_t i = 0; i < rebuildLimit; ++i) {
		if (remove.count(static_cast<uid_node_id_t>(i))) {
			continue;
		}
		remap[static_cast<uid_node_id_t>(i)] = static_cast<uid_node_id_t>(newNodes.size());
		newNodes.push_back(doc->nodes[i]);
		newStates.push_back(doc->states[i]);
	}

	for (uid_node_def_t &node : newNodes) {
		std::vector<uid_node_id_t> kids;
		for (uid_node_id_t c : node.children) {
			auto it = remap.find(c);
			if (it != remap.end()) {
				kids.push_back(it->second);
			}
		}
		node.children = kids;
	}

	if (!savedOverlayNodes.empty()) {
		const size_t newOverlayBase = newNodes.size();
		std::map<uid_node_id_t, uid_node_id_t> overlayRemap;
		for (size_t i = 0; i < savedOverlayNodes.size(); ++i) {
			const uid_node_id_t oldId = static_cast<uid_node_id_t>(overlayBase + i);
			const uid_node_id_t newId = static_cast<uid_node_id_t>(newOverlayBase + i);
			overlayRemap[oldId] = newId;
			newNodes.push_back(savedOverlayNodes[i]);
			newStates.push_back(savedOverlayStates[i]);
		}
		for (size_t i = newOverlayBase; i < newNodes.size(); ++i) {
			uid_node_def_t &node = newNodes[i];
			for (uid_node_id_t &child : node.children) {
				if (child != UID_INVALID_NODE_ID) {
					auto it = overlayRemap.find(child);
					if (it != overlayRemap.end()) {
						child = it->second;
					}
				}
			}
			if (node.foreachTemplateRoot != UID_INVALID_NODE_ID) {
				auto it = overlayRemap.find(node.foreachTemplateRoot);
				if (it != overlayRemap.end()) {
					node.foreachTemplateRoot = it->second;
				}
			}
			if (node.foreachScopeId != UID_INVALID_NODE_ID) {
				auto it = overlayRemap.find(node.foreachScopeId);
				if (it != overlayRemap.end()) {
					node.foreachScopeId = it->second;
				}
			}
		}
		doc->modalOverlayBase = newOverlayBase;
		doc->modalRootNode =
			savedModalRootOffset != UID_INVALID_NODE_ID
				? static_cast<uid_node_id_t>(newOverlayBase + static_cast<size_t>(savedModalRootOffset))
				: UID_INVALID_NODE_ID;
		doc->activeModalId = savedModalId;
	}

	doc->nodes = std::move(newNodes);
	doc->states = std::move(newStates);
	doc->idIndex.clear();
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		if (!doc->nodes[i].id.empty()) {
			doc->idIndex[doc->nodes[i].id] = static_cast<uid_node_id_t>(i);
		}
	}

	auto fit = remap.find(foreachId);
	if (fit != remap.end()) {
		doc->nodes[static_cast<size_t>(fit->second)].children.clear();
	}
}

bool RefreshCollectionScope(uid_document_t *doc, uid_node_id_t scopeId, const uid_backend_t *backend)
{
	if (!doc) {
		return false;
	}
	if (scopeId < 0 || static_cast<size_t>(scopeId) >= doc->nodes.size()) {
		return false;
	}
	uid_node_def_t *scope = &doc->nodes[static_cast<size_t>(scopeId)];
	uid_node_state_t *st = &doc->states[static_cast<size_t>(scopeId)];
	if (!IsCollectionScope(*scope)) {
		return false;
	}

	/* Added in OPM: XML sources use constant revision 1 — skip rebuild when already loaded. */
	auto itSrc = doc->definitions.sources.find(scope->collectionSource);
	const bool isXmlSource = (itSrc != doc->definitions.sources.end());
	if (isXmlSource) {
		const int n = static_cast<int>(itSrc->second.items.size());
		const int total = n;
		const uint64_t revision = 1;
		if (!st->collectionItems.empty() && revision == st->collectionRevision &&
			n == static_cast<int>(st->collectionItems.size()) && total == st->collectionItemCount) {
			st->collectionRefreshFrame = doc->syncFrameCounter;
			return false;
		}

		std::vector<uid_collection_entry_t> xmlItems;
		if (!LoadXmlCollectionItems(doc, scope->collectionSource, &xmlItems)) {
			return false;
		}
		st->collectionItems = std::move(xmlItems);
		st->collectionItemCount = total > 0 ? total : n;
		st->collectionRevision = revision;
		st->collectionRefreshFrame = doc->syncFrameCounter;
		ClampCollectionSelection(st);
		MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT | UID_DIRTY_BINDING));
		return true;
	}

	if (!backend || !backend->queryCollectionItems) {
		return false;
	}
	uid_collection_query_t q{};
	q.source = scope->collectionSource.c_str();
	q.offset = 0;
	q.limit = doc->limits.maxOptionsPerSelect > 0 ? doc->limits.maxOptionsPerSelect : 512;
	int total = 0;
	uint64_t revision = 0;
	q.outTotal = &total;
	q.outRevision = &revision;

	const int maxItems = q.limit;
	std::vector<uid_collection_item_t> hostItems(static_cast<size_t>(maxItems));

	const int n = backend->queryCollectionItems(&q, hostItems.data(), maxItems);
	if (n < 0) {
		return false;
	}

	if (revision == st->collectionRevision && n == static_cast<int>(st->collectionItems.size()) &&
		total == st->collectionItemCount) {
		st->collectionRefreshFrame = doc->syncFrameCounter;
		return false;
	}

	/* Same keys/count: refresh field text only — do not dirty layout / rebuild foreach. */
	if (n == static_cast<int>(st->collectionItems.size()) &&
		(total > 0 ? total : n) == st->collectionItemCount) {
		bool sameKeys = true;
		for (int i = 0; i < n; ++i) {
			const char *key = hostItems[static_cast<size_t>(i)].key
				? hostItems[static_cast<size_t>(i)].key
				: "";
			if (st->collectionItems[static_cast<size_t>(i)].key != key) {
				sameKeys = false;
				break;
			}
		}
		if (sameKeys) {
			for (int i = 0; i < n; ++i) {
				uid_collection_entry_t &item = st->collectionItems[static_cast<size_t>(i)];
				item.value = hostItems[static_cast<size_t>(i)].value
					? hostItems[static_cast<size_t>(i)].value
					: "";
				item.label = hostItems[static_cast<size_t>(i)].label
					? hostItems[static_cast<size_t>(i)].label
					: item.value;
				item.fields.clear();
				for (int f = 0; f < hostItems[static_cast<size_t>(i)].nfields; ++f) {
					const char *name = hostItems[static_cast<size_t>(i)].fieldNames
						? hostItems[static_cast<size_t>(i)].fieldNames[f]
						: nullptr;
					const char *val = hostItems[static_cast<size_t>(i)].fieldValues
						? hostItems[static_cast<size_t>(i)].fieldValues[f]
						: nullptr;
					if (name && name[0]) {
						item.fields[name] = val ? val : "";
					}
				}
			}
			st->collectionRevision = revision;
			st->collectionRefreshFrame = doc->syncFrameCounter;
			MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_PAINT | UID_DIRTY_BINDING));
			return false;
		}
	}

	st->collectionItems.clear();
	st->collectionItems.reserve(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		uid_collection_entry_t item;
		item.key = hostItems[static_cast<size_t>(i)].key ? hostItems[static_cast<size_t>(i)].key : "";
		item.value = hostItems[static_cast<size_t>(i)].value ? hostItems[static_cast<size_t>(i)].value : "";
		item.label = hostItems[static_cast<size_t>(i)].label ? hostItems[static_cast<size_t>(i)].label : item.value;
		for (int f = 0; f < hostItems[static_cast<size_t>(i)].nfields; ++f) {
			const char *name = hostItems[static_cast<size_t>(i)].fieldNames
				? hostItems[static_cast<size_t>(i)].fieldNames[f]
				: nullptr;
			const char *val = hostItems[static_cast<size_t>(i)].fieldValues
				? hostItems[static_cast<size_t>(i)].fieldValues[f]
				: nullptr;
			if (name && name[0]) {
				item.fields[name] = val ? val : "";
			}
		}
		st->collectionItems.push_back(item);
	}
	st->collectionItemCount = total > 0 ? total : n;
	st->collectionRevision = revision;
	st->collectionRefreshFrame = doc->syncFrameCounter;
	ClampCollectionSelection(st);
	MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT | UID_DIRTY_BINDING));
	return true;
}

void WriteScopeIndexToBind(uid_document_t *doc, uid_node_id_t scopeId, const uid_backend_t *backend)
{
	if (!doc || !backend) {
		return;
	}
	uid_node_def_t *scope = &doc->nodes[static_cast<size_t>(scopeId)];
	uid_node_state_t *st = &doc->states[static_cast<size_t>(scopeId)];
	if (scope->bind.empty() || st->collectionItems.empty()) {
		return;
	}
	/* Added in OPM: item.field binds are read-only (no reverse write). */
	std::string itemField;
	if (UID_ParseItemFieldBind(scope->bind.c_str(), &itemField)) {
		return;
	}
	const int idx = st->collectionSelectedIndex;
	if (idx < 0 || static_cast<size_t>(idx) >= st->collectionItems.size()) {
		return;
	}
	std::string cvarName;
	if (!UID_ParseCvarBind(scope->bind.c_str(), &cvarName)) {
		return;
	}
	st->runtimeValue.hasValue = true;
	st->runtimeValue.stringValue = st->collectionItems[static_cast<size_t>(idx)].value;
	/*
	 * Fixed in Omaha: commit=apply stages the selection until UID_WriteAllBindings
	 * (settings-apply). Writing here made before/after identical so vid_restart never ran.
	 */
	const uid_commit_mode_t mode = scope->hasCommit ? scope->commit : UID_COMMIT_CHANGE;
	if (mode == UID_COMMIT_APPLY) {
		return;
	}
	(void)UID_WriteBinding(doc, scopeId, backend);
}

/*
 * Added in OPM: resolve item.field bind against the enclosing foreach row.
 * Nested collection scopes (e.g. kill-feed weapon icons) are foreach-generated
 * with foreachScopeId / foreachItemIndex pointing at the outer list.
 */
static bool ResolveItemFieldBindWant(
	uid_document_t *doc,
	uid_node_id_t scopeId,
	const std::string &fieldName,
	std::string *wantOut
)
{
	if (!doc || !wantOut || fieldName.empty()) {
		return false;
	}
	wantOut->clear();
	if (scopeId < 0 || static_cast<size_t>(scopeId) >= doc->nodes.size()) {
		return false;
	}

	const uid_node_def_t &scope = doc->nodes[static_cast<size_t>(scopeId)];
	if (!scope.foreachGenerated || scope.foreachScopeId < 0 ||
		static_cast<size_t>(scope.foreachScopeId) >= doc->states.size()) {
		return false;
	}

	const uid_node_state_t &outerSt = doc->states[static_cast<size_t>(scope.foreachScopeId)];
	const int idx = scope.foreachItemIndex;
	if (idx < 0 || static_cast<size_t>(idx) >= outerSt.collectionItems.size()) {
		return false;
	}

	const uid_collection_entry_t &item = outerSt.collectionItems[static_cast<size_t>(idx)];
	auto it = item.fields.find(fieldName);
	if (it != item.fields.end()) {
		*wantOut = it->second;
		return true;
	}
	if (fieldName == "value") {
		*wantOut = item.value;
		return true;
	}
	if (fieldName == "key") {
		*wantOut = item.key;
		return true;
	}
	if (fieldName == "label") {
		*wantOut = item.label;
		return true;
	}
	return false;
}

void SyncScopeIndexFromBind(uid_document_t *doc, uid_node_id_t scopeId, const uid_backend_t *backend)
{
	if (!doc || !backend) {
		return;
	}
	uid_node_def_t *scope = &doc->nodes[static_cast<size_t>(scopeId)];
	uid_node_state_t *st = &doc->states[static_cast<size_t>(scopeId)];
	if (scope->bind.empty()) {
		return;
	}
	if (st->collectionItems.empty()) {
		return;
	}

	/*
	 * Fixed in Omaha: commit=apply keeps a staged selection; do not pull the
	 * live cvar over local cyclic edits before settings-apply flushes.
	 */
	const uid_commit_mode_t mode = scope->hasCommit ? scope->commit : UID_COMMIT_CHANGE;
	if (mode == UID_COMMIT_APPLY && st->runtimeValue.hasValue) {
		return;
	}

	/* Added in OPM: select by enclosing foreach item.field (read-only). */
	std::string itemField;
	if (UID_ParseItemFieldBind(scope->bind.c_str(), &itemField)) {
		std::string want;
		const bool haveWant = ResolveItemFieldBindWant(doc, scopeId, itemField, &want);
		if (!haveWant || want.empty()) {
			st->collectionSelectedIndex = -1;
			return;
		}
		const int idx = FindItemIndexByValue(st->collectionItems, want);
		st->collectionSelectedIndex = idx;
		return;
	}

	std::string cvarName;
	if (!UID_ParseCvarBind(scope->bind.c_str(), &cvarName)) {
		return;
	}
	std::string raw;
	const bool haveCvar = UID_ReadCvarString(backend, cvarName.c_str(), &raw);
	std::string want;
	if (haveCvar) {
		want = UID_TransformCvarToUi(*scope, raw, backend);
	}
	int idx = haveCvar ? FindItemIndexByValue(st->collectionItems, want) : -1;
	if (idx < 0) {
		idx = ResolveFallbackIndex(doc, scope, st->collectionItems);
		st->collectionSelectedIndex = idx;
		if (idx >= 0) {
			if (!haveCvar || want != st->collectionItems[static_cast<size_t>(idx)].value) {
				WriteScopeIndexToBind(doc, scopeId, backend);
			}
		}
		return;
	}
	st->collectionSelectedIndex = idx;
}

float LifetimeAlphaFromAge(int ageMs, int lifetimeMs, int fadeMs)
{
	if (lifetimeMs <= 0) {
		return 1.0f;
	}
	if (ageMs < 0) {
		ageMs = 0;
	}
	if (ageMs >= lifetimeMs) {
		return 0.0f;
	}
	int fade = fadeMs;
	if (fade < 0) {
		fade = 0;
	}
	if (fade > lifetimeMs) {
		fade = lifetimeMs;
	}
	const int fadeStart = lifetimeMs - fade;
	if (fade <= 0 || ageMs < fadeStart) {
		return 1.0f;
	}
	const float t = static_cast<float>(ageMs - fadeStart) / static_cast<float>(fade);
	return std::clamp(1.0f - t, 0.0f, 1.0f);
}

void SyncForeachAppearMap(
	uid_node_state_t *fnSt,
	const std::vector<uid_collection_entry_t> &items,
	int nowMs
)
{
	if (!fnSt) {
		return;
	}
	std::unordered_set<std::string> hostKeys;
	hostKeys.reserve(items.size());
	for (const uid_collection_entry_t &item : items) {
		hostKeys.insert(item.key);
		if (fnSt->foreachAppearAtMs.find(item.key) == fnSt->foreachAppearAtMs.end()) {
			fnSt->foreachAppearAtMs[item.key] = nowMs;
		}
	}
	for (auto it = fnSt->foreachAppearAtMs.begin(); it != fnSt->foreachAppearAtMs.end();) {
		if (hostKeys.find(it->first) == hostKeys.end()) {
			/* Host dropped key early — immediate remove, no exit fade. */
			it = fnSt->foreachAppearAtMs.erase(it);
		} else {
			++it;
		}
	}
}

void CollectLifetimeVisibleIndices(
	const uid_node_def_t *fn,
	uid_node_state_t *fnSt,
	const std::vector<uid_collection_entry_t> &items,
	int start,
	int end,
	int nowMs,
	std::vector<int> *outVisible
)
{
	if (!fn || !fnSt || !outVisible) {
		return;
	}
	outVisible->clear();
	const int lifetimeMs = fn->foreachLifetimeMs;
	for (int i = start; i < end; ++i) {
		if (i < 0 || static_cast<size_t>(i) >= items.size()) {
			continue;
		}
		const std::string &key = items[static_cast<size_t>(i)].key;
		auto it = fnSt->foreachAppearAtMs.find(key);
		const int appeared = (it != fnSt->foreachAppearAtMs.end()) ? it->second : nowMs;
		const int age = nowMs - appeared;
		if (age < lifetimeMs) {
			outVisible->push_back(i);
		}
		/* Keep appear time while host still publishes the key so rows do not re-spawn. */
	}
}

bool ApplyForeachLifetimeOpacity(
	uid_document_t *doc,
	uid_node_id_t foreachId,
	const uid_node_def_t *fn,
	uid_node_state_t *fnSt,
	const std::vector<uid_collection_entry_t> &items,
	int nowMs
)
{
	if (!doc || !fn || !fnSt || foreachId < 0 || static_cast<size_t>(foreachId) >= doc->nodes.size()) {
		return false;
	}
	bool anyFading = false;
	const int lifetimeMs = fn->foreachLifetimeMs;
	const int fadeMs = fn->foreachFadeDurationMs;
	const int fadeStart = lifetimeMs - std::min(fadeMs, lifetimeMs);
	uid_node_def_t *foreachNode = &doc->nodes[static_cast<size_t>(foreachId)];
	for (uid_node_id_t childId : foreachNode->children) {
		if (childId < 0 || static_cast<size_t>(childId) >= doc->nodes.size()) {
			continue;
		}
		uid_node_def_t &wrap = doc->nodes[static_cast<size_t>(childId)];
		uid_node_state_t &wrapSt = doc->states[static_cast<size_t>(childId)];
		const int idx = wrap.foreachItemIndex;
		float alpha = 1.0f;
		if (idx >= 0 && static_cast<size_t>(idx) < items.size()) {
			const std::string &key = items[static_cast<size_t>(idx)].key;
			auto it = fnSt->foreachAppearAtMs.find(key);
			const int appeared = (it != fnSt->foreachAppearAtMs.end()) ? it->second : nowMs;
			const int age = nowMs - appeared;
			alpha = LifetimeAlphaFromAge(age, lifetimeMs, fadeMs);
			if (fadeMs > 0 && age >= fadeStart && age < lifetimeMs) {
				anyFading = true;
			}
		}
		wrapSt.lifetimeOpacityMul = alpha;
	}
	return anyFading;
}

uint64_t ForeachExpandSig(
	const uid_node_def_t *fn,
	const uid_node_state_t *scopeSt,
	int countOverride,
	const std::vector<int> *visibleIndices
)
{
	if (!fn) {
		return 0;
	}
	uint64_t sig = 0;
	/*
	 * Fixed in OPM: do not fold collectionRevision into the expand signature.
	 * Hosts may bump revision every frame for field-only updates (e.g. message
	 * alpha). Structure should key off item count + keys so foreach rows are not
	 * rebuilt when only label/field text changes.
	 */
	if (scopeSt) {
		sig ^= static_cast<uint64_t>(scopeSt->collectionSelectedIndex) << 20;
		sig ^= static_cast<uint64_t>(scopeSt->collectionScrollOffset) << 10;
		if (visibleIndices) {
			sig ^= static_cast<uint64_t>(visibleIndices->size());
			for (int idx : *visibleIndices) {
				if (idx < 0 || static_cast<size_t>(idx) >= scopeSt->collectionItems.size()) {
					continue;
				}
				for (char c : scopeSt->collectionItems[static_cast<size_t>(idx)].key) {
					sig = sig * 131 + static_cast<unsigned char>(c);
				}
				sig = sig * 131 + 1;
			}
		} else {
			sig ^= static_cast<uint64_t>(scopeSt->collectionItemCount);
			for (const uid_collection_entry_t &item : scopeSt->collectionItems) {
				for (char c : item.key) {
					sig = sig * 131 + static_cast<unsigned char>(c);
				}
				sig = sig * 131 + 1;
			}
		}
	}
	if (fn->hasForeachCount) {
		sig ^= static_cast<uint64_t>(countOverride) << 32;
	}
	const std::string mode = fn->foreachMode.empty() ? "all" : fn->foreachMode;
	for (char c : mode) {
		sig = sig * 131 + static_cast<unsigned char>(c);
	}
	return sig;
}

void ExpandForeach(uid_document_t *doc, uid_node_id_t foreachId, const uid_backend_t *backend)
{
	if (!doc || foreachId < 0 || static_cast<size_t>(foreachId) >= doc->nodes.size()) {
		return;
	}
	/* Foreach rebuild remaps node indices. Defer canvas foreaches while a modal is
	 * mounted, but still expand foreaches inside the modal overlay itself. */
	if (UID_IsModalActive(doc) && doc->modalOverlayBase > 0 &&
		static_cast<size_t>(foreachId) < doc->modalOverlayBase) {
		return;
	}
	uid_node_def_t *fn = &doc->nodes[static_cast<size_t>(foreachId)];
	if (fn->kind != UID_NODE_FOREACH || fn->foreachTemplateRoot < 0) {
		return;
	}

	const std::string foreachStableId = fn->id;
	const uid_node_id_t tmplRoot = fn->foreachTemplateRoot;
	uid_node_state_t *fnSt = &doc->states[static_cast<size_t>(foreachId)];

	if (fn->hasForeachCount) {
		double countD = 0.0;
		if (!UID_EvalRuntimeNumericExpr(doc, foreachId, fn->foreachCountExpr, backend, &countD)) {
			countD = 0.0;
		}
		int count = static_cast<int>(countD);
		if (count < 0) {
			count = 0;
		}
		const int maxExpand = doc->limits.maxExpandedNodes > 0 ? doc->limits.maxExpandedNodes : 4096;
		if (count > maxExpand) {
			count = maxExpand;
		}

		const uint64_t sig = ForeachExpandSig(fn, nullptr, count, nullptr);
		if (fnSt->foreachExpandSig == sig && !fn->children.empty()) {
			return;
		}

		/* Added in OPM: copy templates only when rebuilding. */
		const std::vector<uid_node_def_t> tmplNodes = fn->foreachTemplateNodes;

		RemoveExpandedForeach(doc, foreachId);

		if (!foreachStableId.empty()) {
			auto fit = doc->idIndex.find(foreachStableId);
			if (fit != doc->idIndex.end()) {
				foreachId = fit->second;
				fn = &doc->nodes[static_cast<size_t>(foreachId)];
				fnSt = &doc->states[static_cast<size_t>(foreachId)];
			}
		}

		fnSt->collectionItemCount = count;
		fnSt->collectionSelectedIndex = -1;
		fnSt->collectionItems.clear();
		fnSt->foreachExpandSig = sig;

		for (int i = 0; i < count; ++i) {
			uid_collection_entry_t entry;
			entry.key = std::to_string(i);
			entry.value = std::to_string(i);
			entry.label = entry.value;
			const uid_node_id_t root = CloneForeachSubtree(
				doc,
				tmplNodes,
				tmplRoot,
				foreachId,
				i,
				entry,
				count,
				-1,
				"label"
			);
			if (root != UID_INVALID_NODE_ID) {
				doc->nodes[static_cast<size_t>(foreachId)].children.push_back(root);
			}
		}
		MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_STRUCTURE | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
		return;
	}

	/* Added in OPM: find scope without template copy; parent map only if window mode needs it. */
	const std::string mode = fn->foreachMode.empty() ? "all" : fn->foreachMode;
	UidProfScope profWindow(UID_PROF_FRAME_FOREACH_WINDOW, mode == "window");

	std::vector<uid_node_id_t> parents;
	uid_node_id_t scopeId = UID_INVALID_NODE_ID;
	if (mode == "window") {
		parents = BuildParentMap(doc);
		for (uid_node_id_t p = parents[static_cast<size_t>(foreachId)]; p != UID_INVALID_NODE_ID;
			 p = parents[static_cast<size_t>(p)]) {
			if (IsCollectionScope(doc->nodes[static_cast<size_t>(p)])) {
				scopeId = p;
				break;
			}
		}
	} else {
		scopeId = UID_FindCollectionScope(doc, foreachId);
	}
	if (scopeId == UID_INVALID_NODE_ID) {
		return;
	}

	/* Added in OPM: skip Refresh/Sync when the cull walk already refreshed this frame. */
	uid_node_state_t *scopeSt = &doc->states[static_cast<size_t>(scopeId)];
	if (scopeSt->collectionRefreshFrame != doc->syncFrameCounter) {
		RefreshCollectionScope(doc, scopeId, backend);
		SyncScopeIndexFromBind(doc, scopeId, backend);
		scopeSt = &doc->states[static_cast<size_t>(scopeId)];
		fn = &doc->nodes[static_cast<size_t>(foreachId)];
		fnSt = &doc->states[static_cast<size_t>(foreachId)];
	}

	const int collectionItemCount = scopeSt->collectionItemCount;
	const int collectionSelectedIndex = scopeSt->collectionSelectedIndex;
	const int count = collectionItemCount;
	const int nowMs = doc->updateTimeMs;

	int start = 0;
	int end = count;
	if (mode == "selected") {
		start = collectionSelectedIndex;
		end = start + 1;
	} else if (mode == "window") {
		if (parents.empty()) {
			parents = BuildParentMap(doc);
		}
		SyncWindowOffsetFromOverflow(doc, foreachId, scopeId, parents, fn, scopeSt);
		scopeSt = &doc->states[static_cast<size_t>(scopeId)];
		fn = &doc->nodes[static_cast<size_t>(foreachId)];
		fnSt = &doc->states[static_cast<size_t>(foreachId)];
		float viewportH = 0.0f;
		const uid_node_id_t overflowId = FindOverflowScrollAncestor(doc, foreachId, parents);
		if (overflowId != UID_INVALID_NODE_ID && static_cast<size_t>(overflowId) < doc->states.size()) {
			viewportH = doc->states[static_cast<size_t>(overflowId)].contentBox.h;
		}
		const int visible = WindowVisibleCount(doc, fn, viewportH);
		start = scopeSt->collectionScrollOffset;
		end = std::min(count, start + visible);
	}

	std::vector<int> visibleIndices;
	const std::vector<int> *sigVisible = nullptr;
	if (fn->hasForeachLifetime) {
		SyncForeachAppearMap(fnSt, scopeSt->collectionItems, nowMs);
		CollectLifetimeVisibleIndices(fn, fnSt, scopeSt->collectionItems, start, end, nowMs, &visibleIndices);
		sigVisible = &visibleIndices;
	} else {
		fnSt->foreachAppearAtMs.clear();
		for (int i = start; i < end; ++i) {
			if (i >= 0 && static_cast<size_t>(i) < scopeSt->collectionItems.size()) {
				visibleIndices.push_back(i);
			}
		}
	}

	const uint64_t sig = ForeachExpandSig(fn, scopeSt, 0, sigVisible ? sigVisible : &visibleIndices);
	if (fnSt->foreachExpandSig == sig && !fn->children.empty()) {
		if (fn->hasForeachLifetime) {
			const bool fading =
				ApplyForeachLifetimeOpacity(doc, foreachId, fn, fnSt, scopeSt->collectionItems, nowMs);
			if (fading) {
				MarkDirty(doc, UID_DIRTY_PAINT);
			}
		}
		return;
	}

	/* CollectionDisplayMode returns string literals — safe after RemoveExpandedForeach. */
	const char *displayMode = CollectionDisplayMode(doc->nodes[static_cast<size_t>(scopeId)]);
	const std::string scopeStableId = doc->nodes[static_cast<size_t>(scopeId)].id;
	/* Added in OPM: copy templates only when rebuilding. */
	const std::vector<uid_node_def_t> tmplNodes = fn->foreachTemplateNodes;

	if (visibleIndices.empty()) {
		RemoveExpandedForeach(doc, foreachId);
		if (!foreachStableId.empty()) {
			auto fit = doc->idIndex.find(foreachStableId);
			if (fit != doc->idIndex.end()) {
				foreachId = fit->second;
				fnSt = &doc->states[static_cast<size_t>(foreachId)];
			}
		}
		fnSt->foreachExpandSig = sig;
		MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_STRUCTURE | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
		return;
	}

	RemoveExpandedForeach(doc, foreachId);

	if (!foreachStableId.empty()) {
		auto fit = doc->idIndex.find(foreachStableId);
		if (fit != doc->idIndex.end()) {
			foreachId = fit->second;
		}
	}
	if (!scopeStableId.empty()) {
		auto sit = doc->idIndex.find(scopeStableId);
		if (sit != doc->idIndex.end()) {
			scopeId = sit->second;
		}
	} else {
		scopeId = UID_FindCollectionScope(doc, foreachId);
	}
	if (foreachId < 0 || static_cast<size_t>(foreachId) >= doc->nodes.size()) {
		return;
	}
	if (scopeId == UID_INVALID_NODE_ID) {
		return;
	}

	fn = &doc->nodes[static_cast<size_t>(foreachId)];
	fnSt = &doc->states[static_cast<size_t>(foreachId)];
	scopeSt = &doc->states[static_cast<size_t>(scopeId)];
	fnSt->foreachExpandSig = sig;

	for (int i : visibleIndices) {
		if (i < 0 || static_cast<size_t>(i) >= scopeSt->collectionItems.size()) {
			continue;
		}
		const uid_node_id_t root = CloneForeachSubtree(
			doc,
			tmplNodes,
			tmplRoot,
			scopeId,
			i,
			scopeSt->collectionItems[static_cast<size_t>(i)],
			count,
			collectionSelectedIndex,
			displayMode
		);
		if (root != UID_INVALID_NODE_ID) {
			doc->nodes[static_cast<size_t>(foreachId)].children.push_back(root);
		}
		scopeSt = &doc->states[static_cast<size_t>(scopeId)];
	}
	/*
	 * Fixed in OPM: CloneForeachSubtree may reallocate nodes/states vectors.
	 * Rebind fn/fnSt before touching hasForeachLifetime or lifetime state.
	 */
	fn = &doc->nodes[static_cast<size_t>(foreachId)];
	fnSt = &doc->states[static_cast<size_t>(foreachId)];
	scopeSt = &doc->states[static_cast<size_t>(scopeId)];
	if (fn->hasForeachLifetime) {
		ApplyForeachLifetimeOpacity(doc, foreachId, fn, fnSt, scopeSt->collectionItems, nowMs);
	}
	MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_STRUCTURE | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
}

} // namespace

bool UID_FetchCollectionEntries(
	const uid_document_t *doc,
	const uid_backend_t *backend,
	const char *sourceId,
	std::vector<uid_collection_entry_t> *outItems
)
{
	if (!doc || !sourceId || !sourceId[0] || !outItems) {
		return false;
	}
	outItems->clear();

	auto itSrc = doc->definitions.sources.find(sourceId);
	if (itSrc != doc->definitions.sources.end()) {
		outItems->reserve(itSrc->second.items.size());
		for (size_t i = 0; i < itSrc->second.items.size(); ++i) {
			const uid_source_item_def_t &src = itSrc->second.items[i];
			uid_collection_entry_t item;
			item.key = std::to_string(i);
			item.value = src.value;
			item.label = src.label.empty() ? src.value : src.label;
			item.fields = src.fields;
			outItems->push_back(item);
		}
		return true;
	}

	if (!backend || !backend->queryCollectionItems) {
		return false;
	}

	uid_collection_query_t q{};
	q.source = sourceId;
	q.offset = 0;
	q.limit = doc->limits.maxOptionsPerSelect > 0 ? doc->limits.maxOptionsPerSelect : 512;
	int total = 0;
	uint64_t revision = 0;
	q.outTotal = &total;
	q.outRevision = &revision;

	const int maxItems = q.limit;
	std::vector<uid_collection_item_t> hostItems(static_cast<size_t>(maxItems));
	const int n = backend->queryCollectionItems(&q, hostItems.data(), maxItems);
	if (n < 0) {
		return false;
	}

	outItems->reserve(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		uid_collection_entry_t item;
		item.key = hostItems[static_cast<size_t>(i)].key ? hostItems[static_cast<size_t>(i)].key : "";
		item.value = hostItems[static_cast<size_t>(i)].value ? hostItems[static_cast<size_t>(i)].value : "";
		item.label = hostItems[static_cast<size_t>(i)].label ? hostItems[static_cast<size_t>(i)].label : item.value;
		for (int f = 0; f < hostItems[static_cast<size_t>(i)].nfields; ++f) {
			const char *name = hostItems[static_cast<size_t>(i)].fieldNames
				? hostItems[static_cast<size_t>(i)].fieldNames[f]
				: nullptr;
			const char *val = hostItems[static_cast<size_t>(i)].fieldValues
				? hostItems[static_cast<size_t>(i)].fieldValues[f]
				: nullptr;
			if (name && name[0]) {
				item.fields[name] = val ? val : "";
			}
		}
		outItems->push_back(item);
	}
	(void)total;
	(void)revision;
	return true;
}

uid_node_id_t UID_FindCollectionScope(const uid_document_t *doc, uid_node_id_t from)
{
	if (!doc || from < 0 || static_cast<size_t>(from) >= doc->nodes.size()) {
		return UID_INVALID_NODE_ID;
	}
	const std::vector<uid_node_id_t> parents = BuildParentMap(doc);
	for (uid_node_id_t p = from; p != UID_INVALID_NODE_ID; p = parents[static_cast<size_t>(p)]) {
		if (IsCollectionScope(doc->nodes[static_cast<size_t>(p)])) {
			return p;
		}
	}
	return UID_INVALID_NODE_ID;
}

float UID_EvalItemLifetimeAlpha(const uid_document_t *doc, uid_node_id_t nodeId)
{
	if (!doc || nodeId < 0 || static_cast<size_t>(nodeId) >= doc->nodes.size()) {
		return 1.0f;
	}
	const uid_node_def_t &node = doc->nodes[static_cast<size_t>(nodeId)];
	if (!node.foreachGenerated) {
		return 1.0f;
	}
	const std::vector<uid_node_id_t> parents = BuildParentMap(doc);
	uid_node_id_t foreachId = UID_INVALID_NODE_ID;
	for (uid_node_id_t p = parents[static_cast<size_t>(nodeId)]; p != UID_INVALID_NODE_ID;
	     p = parents[static_cast<size_t>(p)]) {
		if (doc->nodes[static_cast<size_t>(p)].kind == UID_NODE_FOREACH) {
			foreachId = p;
			break;
		}
	}
	if (foreachId == UID_INVALID_NODE_ID) {
		return 1.0f;
	}
	const uid_node_def_t &fn = doc->nodes[static_cast<size_t>(foreachId)];
	if (!fn.hasForeachLifetime) {
		return 1.0f;
	}
	const uid_node_state_t &fnSt = doc->states[static_cast<size_t>(foreachId)];
	const int idx = node.foreachItemIndex;
	if (node.foreachScopeId < 0 || static_cast<size_t>(node.foreachScopeId) >= doc->states.size()) {
		return 1.0f;
	}
	const uid_node_state_t &scopeSt = doc->states[static_cast<size_t>(node.foreachScopeId)];
	if (idx < 0 || static_cast<size_t>(idx) >= scopeSt.collectionItems.size()) {
		return 1.0f;
	}
	const std::string &key = scopeSt.collectionItems[static_cast<size_t>(idx)].key;
	auto it = fnSt.foreachAppearAtMs.find(key);
	const int nowMs = doc->updateTimeMs;
	const int appeared = (it != fnSt.foreachAppearAtMs.end()) ? it->second : nowMs;
	return LifetimeAlphaFromAge(nowMs - appeared, fn.foreachLifetimeMs, fn.foreachFadeDurationMs);
}

bool UID_StepCollectionIndex(uid_document_t *doc, uid_node_id_t scopeId, int delta, const uid_backend_t *backend)
{
	if (!doc || delta == 0 || scopeId < 0 || static_cast<size_t>(scopeId) >= doc->nodes.size()) {
		return false;
	}
	uid_node_def_t *scope = &doc->nodes[static_cast<size_t>(scopeId)];
	uid_node_state_t *st = &doc->states[static_cast<size_t>(scopeId)];
	if (!IsCollectionScope(*scope) || st->collectionItemCount <= 0) {
		return false;
	}
	int next = st->collectionSelectedIndex + delta;
	if (scope->collectionWrap) {
		const int n = st->collectionItemCount;
		next = ((next % n) + n) % n;
	} else {
		next = std::max(0, std::min(st->collectionItemCount - 1, next));
	}
	if (next == st->collectionSelectedIndex) {
		return false;
	}
	st->collectionSelectedIndex = next;
	if (scope->collectionScroll || FindWindowForeachUnderScope(doc, scopeId) != UID_INVALID_NODE_ID) {
		EnsureSelectionInWindow(doc, scopeId, st, next);
	}
	WriteScopeIndexToBind(doc, scopeId, backend);
	MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT | UID_DIRTY_BINDING));
	return true;
}

bool UID_SetCollectionIndex(uid_document_t *doc, uid_node_id_t scopeId, int index, const uid_backend_t *backend)
{
	if (!doc || scopeId < 0 || static_cast<size_t>(scopeId) >= doc->nodes.size()) {
		return false;
	}
	uid_node_state_t *st = &doc->states[static_cast<size_t>(scopeId)];
	if (index < 0) {
		if (index != -1) {
			return false;
		}
		if (st->collectionSelectedIndex == -1) {
			return false;
		}
		st->collectionSelectedIndex = -1;
		MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT | UID_DIRTY_BINDING));
		return true;
	}
	if (index >= st->collectionItemCount) {
		return false;
	}
	if (index == st->collectionSelectedIndex) {
		return false;
	}
	st->collectionSelectedIndex = index;
	if (FindWindowForeachUnderScope(doc, scopeId) != UID_INVALID_NODE_ID) {
		EnsureSelectionInWindow(doc, scopeId, st, index);
	}
	WriteScopeIndexToBind(doc, scopeId, backend);
	MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT | UID_DIRTY_BINDING));
	return true;
}

/* Added in OPM: windowed foreach under overflow=scroll — synthetic extent / no pixel shift. */
bool UID_ScrollParentHasWindowedForeach(const uid_document_t *doc, uid_node_id_t parentId)
{
	if (!doc || parentId < 0 || static_cast<size_t>(parentId) >= doc->nodes.size()) {
		return false;
	}
	for (uid_node_id_t c : doc->nodes[static_cast<size_t>(parentId)].children) {
		if (c < 0 || static_cast<size_t>(c) >= doc->nodes.size()) {
			continue;
		}
		const uid_node_def_t &n = doc->nodes[static_cast<size_t>(c)];
		if (n.kind != UID_NODE_FOREACH) {
			continue;
		}
		const std::string mode = n.foreachMode.empty() ? "all" : n.foreachMode;
		if (mode == "window" && n.hasForeachRowHeight && n.foreachRowHeight > 0.0f) {
			return true;
		}
	}
	return false;
}

float UID_WindowedForeachSyntheticExtentH(const uid_document_t *doc, uid_node_id_t parentId)
{
	if (!doc || parentId < 0 || static_cast<size_t>(parentId) >= doc->nodes.size()) {
		return -1.0f;
	}
	for (uid_node_id_t c : doc->nodes[static_cast<size_t>(parentId)].children) {
		if (c < 0 || static_cast<size_t>(c) >= doc->nodes.size()) {
			continue;
		}
		const uid_node_def_t &n = doc->nodes[static_cast<size_t>(c)];
		if (n.kind != UID_NODE_FOREACH) {
			continue;
		}
		const std::string mode = n.foreachMode.empty() ? "all" : n.foreachMode;
		if (mode != "window" || !n.hasForeachRowHeight || n.foreachRowHeight <= 0.0f) {
			continue;
		}
		const uid_node_id_t scopeId = UID_FindCollectionScope(doc, c);
		if (scopeId == UID_INVALID_NODE_ID || static_cast<size_t>(scopeId) >= doc->states.size()) {
			return -1.0f;
		}
		const float rowH = UID_ScaleAuthoredPx(doc, n.foreachRowHeight);
		const int count = doc->states[static_cast<size_t>(scopeId)].collectionItemCount;
		return std::max(0.0f, static_cast<float>(count) * rowH);
	}
	return -1.0f;
}

void UID_SyncCollections(uid_document_t *doc, const uid_backend_t *backend)
{
	if (!doc || !backend) {
		return;
	}

	/* Added in OPM: apply {collection.*}/{index.*} field stamps only when structure changes. */
	if (!doc->collectionFieldsApplied || (doc->dirty & UID_DIRTY_STRUCTURE)) {
		for (uid_node_def_t &node : doc->nodes) {
			UID_ApplyCollectionAndIndexFields(&node);
		}
		doc->collectionFieldsApplied = true;
	}

	const bool cull = UID_OptEnabled(UID_OPT_COLLECTION_CULL) != 0;

	if (!cull) {
		for (size_t i = 0; i < doc->nodes.size(); ++i) {
			if (IsCollectionScope(doc->nodes[i])) {
				RefreshCollectionScope(doc, static_cast<uid_node_id_t>(i), backend);
				SyncScopeIndexFromBind(doc, static_cast<uid_node_id_t>(i), backend);
			}
		}

		/* Fixed in OPM: ExpandForeach may rebuild/remap node indices. Restart the
		 * scan after any structural change so we never walk a stale index stream. */
		for (;;) {
			bool expanded = false;
			const size_t n = doc->nodes.size();
			for (size_t i = 0; i < n && i < doc->nodes.size(); ++i) {
				if (doc->nodes[i].kind != UID_NODE_FOREACH) {
					continue;
				}
				const size_t sizeBefore = doc->nodes.size();
				const uintptr_t dataBefore = reinterpret_cast<uintptr_t>(doc->nodes.data());
				ExpandForeach(doc, static_cast<uid_node_id_t>(i), backend);
				if (doc->nodes.size() != sizeBefore ||
					reinterpret_cast<uintptr_t>(doc->nodes.data()) != dataBefore) {
					expanded = true;
					break;
				}
			}
			if (!expanded) {
				break;
			}
		}
	} else {
		/* Added in OPM: skip Expand under hidden ancestors (Settings stays warm).
		 * Still Refresh empty scopes once so collection defaults can seed cvars. */
		UidProfScope profCull(UID_PROF_FRAME_COLLECTION_CULL);
		bool remapped = false;

		struct CullWalkCtx {
			uid_document_t      *doc;
			const uid_backend_t *backend;
			bool                *remapped;
		};

		/* Added in OPM: re-index children via doc->nodes[id] (no vector copy; safe across Expand). */
		auto walkImpl = [](CullWalkCtx *ctx, uid_node_id_t id, bool ancestorVisible,
						   auto &walkRef) -> void {
			if (*ctx->remapped || id < 0 || static_cast<size_t>(id) >= ctx->doc->nodes.size()) {
				return;
			}
			uid_node_def_t *node = &ctx->doc->nodes[static_cast<size_t>(id)];
			uid_node_state_t *st = &ctx->doc->states[static_cast<size_t>(id)];
			const bool selfVisible = ancestorVisible && PropBool(*node, "visible", true);

			if (IsCollectionScope(*node)) {
				const bool needsWarmRefresh = st->collectionItems.empty();
				if (selfVisible || needsWarmRefresh) {
					RefreshCollectionScope(ctx->doc, id, ctx->backend);
					SyncScopeIndexFromBind(ctx->doc, id, ctx->backend);
				}
			}

			if (!selfVisible) {
				for (size_t i = 0; i < ctx->doc->nodes[static_cast<size_t>(id)].children.size(); ++i) {
					const uid_node_id_t c = ctx->doc->nodes[static_cast<size_t>(id)].children[i];
					walkRef(ctx, c, false, walkRef);
					if (*ctx->remapped) {
						return;
					}
				}
				return;
			}

			node = &ctx->doc->nodes[static_cast<size_t>(id)];
			if (node->kind == UID_NODE_FOREACH) {
				const size_t sizeBefore = ctx->doc->nodes.size();
				const uintptr_t dataBefore = reinterpret_cast<uintptr_t>(ctx->doc->nodes.data());
				ExpandForeach(ctx->doc, id, ctx->backend);
				if (ctx->doc->nodes.size() != sizeBefore ||
					reinterpret_cast<uintptr_t>(ctx->doc->nodes.data()) != dataBefore) {
					*ctx->remapped = true;
					return;
				}
			}
			for (size_t i = 0; i < ctx->doc->nodes[static_cast<size_t>(id)].children.size(); ++i) {
				const uid_node_id_t c = ctx->doc->nodes[static_cast<size_t>(id)].children[i];
				walkRef(ctx, c, true, walkRef);
				if (*ctx->remapped) {
					return;
				}
			}
		};

		CullWalkCtx ctx{doc, backend, &remapped};
		for (;;) {
			remapped = false;
			if (doc->rootNode != UID_INVALID_NODE_ID) {
				walkImpl(&ctx, doc->rootNode, true, walkImpl);
			}
			if (!remapped && UID_IsModalActive(doc)) {
				const uid_node_id_t modalRoot = UID_GetModalRoot(doc);
				if (modalRoot != UID_INVALID_NODE_ID) {
					walkImpl(&ctx, modalRoot, true, walkImpl);
				}
			}
			if (!remapped) {
				break;
			}
		}
	}

	/* Added in OPM: foreach rebuild can introduce new nodes that need field stamps. */
	if (doc->dirty & UID_DIRTY_STRUCTURE) {
		for (uid_node_def_t &node : doc->nodes) {
			UID_ApplyCollectionAndIndexFields(&node);
		}
		doc->collectionFieldsApplied = true;
	}

	/* Deferred <use template="{item.*}"> nodes expand inline in CloneForeachSubtree. */
}
