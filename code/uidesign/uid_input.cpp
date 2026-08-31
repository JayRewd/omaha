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

#include "uid_input.h"

#include "uid_action.h"
#include "uid_binding.h"
#include "uid_collection.h"
#include "uid_layout.h"
#include "uid_modal.h"
#include "uid_scrollbar.h"
#include "uid_value.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char *PropCStr(const uid_node_def_t &node, const char *name, const char *fallback)
{
	const char *v = node.properties.GetCStr(name, nullptr);
	if (v) {
		return v;
	}
	const char *b = UID_BuiltinDefault(name);
	return b ? b : fallback;
}

bool PropBool(const uid_node_def_t &node, const char *name, bool fallback)
{
	const char *v = PropCStr(node, name, nullptr);
	if (!v) {
		return fallback;
	}
	bool out = fallback;
	UID_ParseBool(v, &out, nullptr);
	return out;
}

uid_node_state_t *State(uid_document_t *doc, uid_node_id_t id)
{
	if (!doc || id < 0 || static_cast<size_t>(id) >= doc->states.size()) {
		return nullptr;
	}
	return &doc->states[static_cast<size_t>(id)];
}

const uid_node_state_t *StateC(const uid_document_t *doc, uid_node_id_t id)
{
	if (!doc || id < 0 || static_cast<size_t>(id) >= doc->states.size()) {
		return nullptr;
	}
	return &doc->states[static_cast<size_t>(id)];
}

bool EnsureStates(uid_document_t *doc)
{
	if (!doc) {
		return false;
	}
	if (doc->states.size() != doc->nodes.size()) {
		doc->states.resize(doc->nodes.size());
		for (uid_node_state_t &st : doc->states) {
			UID_InitNodeState(&st);
		}
	}
	return true;
}

void MarkDirty(uid_document_t *doc, int flags)
{
	if (doc) {
		doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty | flags);
	}
}

void CollectFocusWalk(const uid_document_t *doc, uid_node_id_t id, bool ancVis, bool ancEn, std::vector<uid_node_id_t> *out)
{
	const uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node) {
		return;
	}

	const bool visible = ancVis && PropBool(*node, "visible", true);
	const bool enabled = ancEn && PropBool(*node, "enabled", true);

	if (UID_IsInteractiveKind(node->kind) && node->kind != UID_NODE_SERVER_LIST && visible && enabled) {
		out->push_back(id);
	}

	if (!visible) {
		return;
	}

	for (uid_node_id_t c : node->children) {
		CollectFocusWalk(doc, c, visible, enabled, out);
	}
}

int TabIndexOf(const uid_node_def_t &node)
{
	const char *v = node.properties.GetCStr("tab-index", nullptr);
	if (!v) {
		return -1;
	}
	double n = 0.0;
	if (!UID_ParseNumber(v, &n, nullptr)) {
		return -1;
	}
	return static_cast<int>(n);
}

size_t Utf8SeqLenAt(const std::string &s, size_t i)
{
	if (i >= s.size()) {
		return 0;
	}
	const unsigned char c = static_cast<unsigned char>(s[i]);
	size_t len = 1;
	if ((c & 0x80) == 0) {
		len = 1;
	} else if ((c & 0xE0) == 0xC0) {
		len = 2;
	} else if ((c & 0xF0) == 0xE0) {
		len = 3;
	} else if ((c & 0xF8) == 0xF0) {
		len = 4;
	}
	/* Truncated / overlong lead: consume one byte, never walk past NUL/end. */
	if (i + len > s.size()) {
		return 1;
	}
	return len;
}

size_t Utf8CodepointCount(const std::string &s)
{
	size_t count = 0;
	for (size_t i = 0; i < s.size();) {
		const size_t len = Utf8SeqLenAt(s, i);
		if (len == 0) {
			break;
		}
		i += len;
		++count;
	}
	return count;
}

size_t Utf8OffsetForCodepoint(const std::string &s, size_t codepoint)
{
	size_t count = 0;
	size_t i = 0;
	while (i < s.size() && count < codepoint) {
		const size_t len = Utf8SeqLenAt(s, i);
		if (len == 0) {
			break;
		}
		i += len;
		++count;
	}
	return i;
}

void AppendUtf8(std::string *s, unsigned codepoint)
{
	if (!s) {
		return;
	}
	/* Reject surrogates and non-Unicode scalars. */
	if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
		return;
	}
	if (codepoint < 0x80) {
		s->push_back(static_cast<char>(codepoint));
	} else if (codepoint < 0x800) {
		s->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
		s->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	} else if (codepoint < 0x10000) {
		s->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
		s->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		s->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	} else {
		s->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
		s->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
		s->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		s->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	}
}

void SetRuntimeString(uid_node_state_t *st, const std::string &value)
{
	st->runtimeValue.hasValue = true;
	st->runtimeValue.stringValue = value;
}

void MaybeWriteBinding(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend, uid_commit_mode_t when)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node || !backend) {
		return;
	}
	if (node->kind == UID_NODE_KEYBIND) {
		if (node->binding.empty()) {
			return;
		}
	} else if (node->bind.empty()) {
		return;
	}
	uid_commit_mode_t mode = node->hasCommit ? node->commit : UID_COMMIT_CHANGE;
	if (mode != when) {
		return;
	}
	UID_WriteBinding(doc, id, backend);
}

void CommitInput(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st) {
		return;
	}

	std::string value = st->editBuffer;
	if (node->inputType == "number") {
		double n = 0.0;
		if (!UID_ParseNumber(value.c_str(), &n, nullptr)) {
			value = st->preEditValue;
		} else {
			if (node->hasMin && n < node->minValue) {
				n = node->minValue;
			}
			if (node->hasMax && n > node->maxValue) {
				n = node->maxValue;
			}
			char buf[64];
			UID_FormatNumberForStep(
				n,
				node->hasMin ? node->minValue : n,
				node->hasMax ? node->maxValue : n,
				node->hasStep ? node->stepValue : 0.0,
				node->hasMin,
				node->hasMax,
				node->hasStep,
				buf,
				sizeof(buf));
			value = buf;
		}
	}

	SetRuntimeString(st, value);
	st->editBuffer = value;
	MarkDirty(doc, UID_DIRTY_PAINT | UID_DIRTY_BINDING);
	UID_DispatchEvent(doc, id, UID_EVENT_CHANGE, backend);
	MaybeWriteBinding(doc, id, backend, UID_COMMIT_CHANGE);
	MaybeWriteBinding(doc, id, backend, UID_COMMIT_SUBMIT);
	UID_DispatchEvent(doc, id, UID_EVENT_SUBMIT, backend);
}

static void LiveCommitTextInputIfNeeded(
	uid_document_t *doc,
	uid_node_id_t id,
	uid_node_def_t *node,
	uid_node_state_t *st,
	const uid_backend_t *backend
)
{
	if (!doc || !node || !st || !backend || node->kind != UID_NODE_INPUT) {
		return;
	}
	if (node->inputType == "number") {
		return;
	}
	const uid_commit_mode_t mode = node->hasCommit ? node->commit : UID_COMMIT_CHANGE;
	if (mode != UID_COMMIT_CHANGE) {
		return;
	}
	SetRuntimeString(st, st->editBuffer);
	MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_PAINT | UID_DIRTY_BINDING));
	MaybeWriteBinding(doc, id, backend, UID_COMMIT_CHANGE);
}

void ActivateButton(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	/* Snapshot set-value before any SyncCollections (foreach rebuild moves nodes). */
	const bool wantSetValue = node && !node->setValue.empty() && !node->bind.empty();
	const std::string setValueCopy = wantSetValue ? node->setValue : std::string();
	/* Fixed in OPM: SyncCollections may rebuild foreach nodes and move vectors —
	 * copy fields / re-fetch node+state after every sync (no dangling pointers). */
	if (node && node->hasStepIndex) {
		const int step = node->stepIndex;
		const uid_node_id_t scopeId = UID_FindCollectionScope(doc, id);
		if (scopeId != UID_INVALID_NODE_ID) {
			(void)UID_StepCollectionIndex(doc, scopeId, step, backend);
			UID_SyncCollections(doc, backend);
			node = UID_GetNode(doc, id);
		}
	}
	if (node && node->hasSetIndex) {
		const uid_node_id_t scopeId = node->foreachScopeId != UID_INVALID_NODE_ID
			? node->foreachScopeId
			: UID_FindCollectionScope(doc, id);
		int idx = node->setIndexValue;
		if (idx < 0 && node->foreachItemIndex >= 0) {
			idx = node->foreachItemIndex;
		}
		if (scopeId != UID_INVALID_NODE_ID && idx >= 0) {
			(void)UID_SetCollectionIndex(doc, scopeId, idx, backend);
			UID_SyncCollections(doc, backend);
			node = UID_GetNode(doc, id);
		}
	}
	uid_node_state_t *st = State(doc, id);
	/* Added in OPM: set-value buttons write the shared bind (Off/On groups). */
	if (wantSetValue && st) {
		SetRuntimeString(st, setValueCopy);
		MarkDirty(doc, UID_DIRTY_PAINT | UID_DIRTY_BINDING);
		UID_DispatchEvent(doc, id, UID_EVENT_CHANGE, backend);
		MaybeWriteBinding(doc, id, backend, UID_COMMIT_CHANGE);
		MaybeWriteBinding(doc, id, backend, UID_COMMIT_SUBMIT);
		UID_SyncBindings(doc, backend);
	}
	UID_DispatchEvent(doc, id, UID_EVENT_CLICK, backend);
	MarkDirty(doc, UID_DIRTY_PAINT);
}

void ToggleValue(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend)
{
	uid_node_state_t *st = State(doc, id);
	if (!st) {
		return;
	}
	const bool on = st->runtimeValue.hasValue &&
		(st->runtimeValue.stringValue == "true" || st->runtimeValue.stringValue == "1");
	SetRuntimeString(st, on ? "false" : "true");
	MarkDirty(doc, UID_DIRTY_PAINT | UID_DIRTY_BINDING);
	UID_DispatchEvent(doc, id, UID_EVENT_CHANGE, backend);
	MaybeWriteBinding(doc, id, backend, UID_COMMIT_CHANGE);
}

double SliderValue(const uid_node_def_t &node, const uid_node_state_t &st)
{
	double minV = node.hasMin ? node.minValue : 0.0;
	double val = minV;
	if (st.runtimeValue.hasValue) {
		UID_ParseNumber(st.runtimeValue.stringValue.c_str(), &val, nullptr);
	}
	return val;
}

void SetSliderValue(uid_document_t *doc, uid_node_id_t id, double val, const uid_backend_t *backend, bool emit)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st) {
		return;
	}
	double minV = node->hasMin ? node->minValue : 0.0;
	double maxV = node->hasMax ? node->maxValue : 1.0;
	double step = node->hasStep ? node->stepValue : 0.0;
	if (maxV < minV) {
		maxV = minV;
	}
	val = std::min(maxV, std::max(minV, val));
	if (step > 0.0) {
		val = minV + std::round((val - minV) / step) * step;
		val = std::min(maxV, std::max(minV, val));
	}
	char buf[64];
	UID_FormatNumberForStep(
		val,
		minV,
		maxV,
		step,
		node->hasMin,
		node->hasMax,
		node->hasStep,
		buf,
		sizeof(buf));
	SetRuntimeString(st, buf);
	/*
	 * Added in OPM: composed thumb/range need layout; companion number inputs
	 * share the bind and must track the staged value while dragging.
	 */
	if (!node->bind.empty()) {
		for (size_t i = 0; i < doc->nodes.size() && i < doc->states.size(); ++i) {
			uid_node_def_t &peer = doc->nodes[i];
			if (peer.kind != UID_NODE_INPUT || peer.inputType != "number") {
				continue;
			}
			if (peer.bind != node->bind) {
				continue;
			}
			uid_node_state_t &pst = doc->states[i];
			if (pst.focused) {
				continue;
			}
			SetRuntimeString(&pst, buf);
			pst.editBuffer = buf;
		}
	}
	MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT | UID_DIRTY_BINDING));
	if (emit) {
		UID_DispatchEvent(doc, id, UID_EVENT_CHANGE, backend);
		/*
		 * Changed in OPM: commit=change writes live during drag; commit=submit
		 * stages until pointer release (CommitSliderBinding) — used by ui_scale.
		 */
		MaybeWriteBinding(doc, id, backend, UID_COMMIT_CHANGE);
	}
}

/* Added in OPM: flush slider bind on pointer release / key step (not during drag). */
void CommitSliderBinding(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend)
{
	MaybeWriteBinding(doc, id, backend, UID_COMMIT_CHANGE);
	MaybeWriteBinding(doc, id, backend, UID_COMMIT_SUBMIT);
	UID_DispatchEvent(doc, id, UID_EVENT_SUBMIT, backend);
}

void OpenSelect(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st) {
		return;
	}
	/* Added in OPM: cyclic selects never open a modal/overlay. */
	if (node->appearance == "cyclic") {
		return;
	}
	/* Added in OPM: dropdown opens via modal= (relative or fullscreen). */
	if (node->openModal.empty()) {
		return;
	}
	for (size_t i = 0; i < doc->states.size(); ++i) {
		if (doc->nodes[i].kind == UID_NODE_SELECT) {
			doc->states[i].overlayOpen = false;
		}
	}
	doc->modalOpenerNode = id;
	if (backend && backend->cvarWrite) {
		backend->cvarWrite(UID_DefaultModalCvarName(), node->openModal.c_str());
	}
	MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_BINDING | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
}

void CloseSelect(uid_document_t *doc, uid_node_id_t id)
{
	uid_node_state_t *st = State(doc, id);
	if (!st) {
		return;
	}
	st->overlayOpen = false;
	MarkDirty(doc, UID_DIRTY_PAINT);
}

void ChooseSelect(uid_document_t *doc, uid_node_id_t id, int index, const uid_backend_t *backend)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st || index < 0 || static_cast<size_t>(index) >= node->options.size()) {
		return;
	}
	st->highlightIndex = index;
	SetRuntimeString(st, node->options[static_cast<size_t>(index)].value);
	st->overlayOpen = false;
	MarkDirty(doc, UID_DIRTY_PAINT | UID_DIRTY_BINDING);
	UID_DispatchEvent(doc, id, UID_EVENT_CHANGE, backend);
	MaybeWriteBinding(doc, id, backend, UID_COMMIT_CHANGE);
}

/* Added in OPM: wrap selectedIndex for appearance=cyclic. */
int CyclicSelectIndex(const uid_node_def_t &node, const uid_node_state_t &st)
{
	if (node.options.empty()) {
		return -1;
	}
	if (st.highlightIndex >= 0 && static_cast<size_t>(st.highlightIndex) < node.options.size()) {
		if (!st.runtimeValue.hasValue ||
			node.options[static_cast<size_t>(st.highlightIndex)].value == st.runtimeValue.stringValue) {
			return st.highlightIndex;
		}
	}
	if (st.runtimeValue.hasValue) {
		for (size_t i = 0; i < node.options.size(); ++i) {
			if (node.options[i].value == st.runtimeValue.stringValue) {
				return static_cast<int>(i);
			}
		}
	}
	return 0;
}

void StepCyclicSelect(uid_document_t *doc, uid_node_id_t id, int delta, const uid_backend_t *backend)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st || node->options.empty() || delta == 0) {
		return;
	}
	const int n = static_cast<int>(node->options.size());
	int cur = CyclicSelectIndex(*node, *st);
	if (cur < 0) {
		cur = 0;
	}
	int next = (cur + delta) % n;
	if (next < 0) {
		next += n;
	}
	ChooseSelect(doc, id, next, backend);
}

void ApplySliderAtPointer(uid_document_t *doc, uid_node_id_t id, float x, const uid_backend_t *backend)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st || st->contentBox.w <= 0.0f) {
		return;
	}

	uid_rect_t track = st->contentBox;
	const uid_node_id_t trackId = UID_FindChildOfKind(doc, id, UID_NODE_SLIDER_TRACK);
	if (trackId != UID_INVALID_NODE_ID) {
		const uid_node_state_t *tst = State(doc, trackId);
		if (tst && tst->borderBox.w > 0.0f) {
			track = tst->borderBox;
		}
	}
	if (track.w <= 0.0f) {
		return;
	}

	double minV = node->hasMin ? node->minValue : 0.0;
	double maxV = node->hasMax ? node->maxValue : 1.0;
	const float t = std::min(1.0f, std::max(0.0f, (x - track.x) / track.w));
	SetSliderValue(doc, id, minV + t * (maxV - minV), backend, true);
}

bool FindNodePath(const uid_document_t *doc, uid_node_id_t id, uid_node_id_t target, std::vector<uid_node_id_t> *path)
{
	if (!doc || !path || id == UID_INVALID_NODE_ID) {
		return false;
	}
	path->push_back(id);
	if (id == target) {
		return true;
	}
	const uid_node_def_t *n = UID_GetNode(doc, id);
	if (n) {
		for (uid_node_id_t c : n->children) {
			if (FindNodePath(doc, c, target, path)) {
				return true;
			}
		}
	}
	path->pop_back();
	return false;
}

bool ApplyScrollWheel(uid_document_t *doc, uid_node_id_t containerId, int wheel)
{
	if (!doc || wheel == 0 || containerId == UID_INVALID_NODE_ID) {
		return false;
	}

	uid_node_state_t *st = State(doc, containerId);
	if (!st) {
		return false;
	}

	const float maxY = std::max(0.0f, st->contentExtentH - st->contentBox.h);
	const float maxX = std::max(0.0f, st->contentExtentW - st->contentBox.w);
	if (maxY <= 0.0f && maxX <= 0.0f) {
		return false;
	}
	if (maxY > 0.0f) {
		st->scrollY = std::min(maxY, std::max(0.0f, st->scrollY - static_cast<float>(wheel) * 24.0f));
	} else {
		st->scrollX = std::min(maxX, std::max(0.0f, st->scrollX - static_cast<float>(wheel) * 24.0f));
	}
	MarkDirty(doc, UID_DIRTY_LAYOUT | UID_DIRTY_PAINT);
	return true;
}

static void FindDeepestScrollContainerAtPointRecurse(
	const uid_document_t *doc,
	uid_node_id_t id,
	float x,
	float y,
	int depth,
	uid_node_id_t *best,
	int *bestDepth
)
{
	const uid_node_def_t *node = UID_GetNode(doc, id);
	const uid_node_state_t *st = StateC(doc, id);
	if (!node || !st || !PropBool(*node, "visible", true)) {
		return;
	}

	uid_overflow_t ov = UID_OVERFLOW_NONE;
	UID_ParseOverflow(PropCStr(*node, "overflow", "none"), &ov, nullptr);
	if (ov == UID_OVERFLOW_SCROLL) {
		const float maxY = std::max(0.0f, st->contentExtentH - st->contentBox.h);
		const float maxX = std::max(0.0f, st->contentExtentW - st->contentBox.w);
		if ((maxY > 0.0f || maxX > 0.0f) &&
			UID_PointInClippedRect(st->borderBox, st->effectiveClip, x, y)) {
			if (depth > *bestDepth) {
				*bestDepth = depth;
				*best = id;
			}
		}
	}

	for (uid_node_id_t c : node->children) {
		FindDeepestScrollContainerAtPointRecurse(doc, c, x, y, depth + 1, best, bestDepth);
	}
}

static uid_node_id_t FindDeepestScrollContainerAtPoint(const uid_document_t *doc, float x, float y)
{
	if (!doc) {
		return UID_INVALID_NODE_ID;
	}

	uid_node_id_t best = UID_INVALID_NODE_ID;
	int bestDepth = -1;
	if (UID_IsModalActive(doc)) {
		const uid_node_id_t modalRoot = UID_GetModalRoot(doc);
		if (modalRoot != UID_INVALID_NODE_ID) {
			FindDeepestScrollContainerAtPointRecurse(doc, modalRoot, x, y, 0, &best, &bestDepth);
		}
		return best;
	}
	if (doc->rootNode == UID_INVALID_NODE_ID) {
		return UID_INVALID_NODE_ID;
	}
	FindDeepestScrollContainerAtPointRecurse(doc, doc->rootNode, x, y, 0, &best, &bestDepth);
	return best;
}

bool HandleScrollWheel(uid_document_t *doc, uid_node_id_t hit, int wheel)
{
	if (!doc || wheel == 0 || hit == UID_INVALID_NODE_ID) {
		return false;
	}

	std::vector<uid_node_id_t> path;
	const uid_node_id_t searchRoot =
		UID_IsModalActive(doc) ? UID_GetModalRoot(doc) : doc->rootNode;
	if (searchRoot == UID_INVALID_NODE_ID) {
		return false;
	}
	FindNodePath(doc, searchRoot, hit, &path);

	for (int i = static_cast<int>(path.size()) - 1; i >= 0; --i) {
		uid_node_id_t id = path[static_cast<size_t>(i)];
		uid_node_def_t *node = UID_GetNode(doc, id);
		if (!node) {
			continue;
		}
		uid_overflow_t ov = UID_OVERFLOW_NONE;
		UID_ParseOverflow(PropCStr(*node, "overflow", "none"), &ov, nullptr);
		if (ov != UID_OVERFLOW_SCROLL) {
			continue;
		}
		return ApplyScrollWheel(doc, id, wheel);
	}
	return false;
}

uid_node_id_t FindCollectionActionNode(const uid_document_t *doc, uid_node_id_t from)
{
	if (!doc || from < 0 || static_cast<size_t>(from) >= doc->nodes.size()) {
		return UID_INVALID_NODE_ID;
	}
	std::vector<uid_node_id_t> parent(doc->nodes.size(), UID_INVALID_NODE_ID);
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		for (uid_node_id_t c : doc->nodes[i].children) {
			if (c >= 0 && static_cast<size_t>(c) < parent.size()) {
				parent[static_cast<size_t>(c)] = static_cast<uid_node_id_t>(i);
			}
		}
	}
	for (uid_node_id_t id = from; id != UID_INVALID_NODE_ID; id = parent[static_cast<size_t>(id)]) {
		const uid_node_def_t *node = UID_GetNode(doc, id);
		if (!node) {
			break;
		}
		if (node->hasSetIndex || node->hasStepIndex || node->kind == UID_NODE_BUTTON) {
			return id;
		}
	}
	return from;
}

} // namespace

bool UID_IsInteractiveKind(uid_node_kind_t kind)
{
	switch (kind) {
	case UID_NODE_BUTTON:
	case UID_NODE_INPUT:
	case UID_NODE_TOGGLE:
	case UID_NODE_SLIDER:
	case UID_NODE_SELECT:
	case UID_NODE_KEYBIND:
	case UID_NODE_SERVER_LIST: /* Added in OPM */
		return true;
	default:
		return false;
	}
}

bool UID_NodeEffectivelyVisible(const uid_document_t *doc, uid_node_id_t id)
{
	const uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node) {
		return false;
	}
	return PropBool(*node, "visible", true);
}

bool UID_NodeEffectivelyEnabled(const uid_document_t *doc, uid_node_id_t id)
{
	const uid_node_state_t *st = StateC(doc, id);
	if (st) {
		return st->effectivelyEnabled;
	}
	const uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node) {
		return false;
	}
	return PropBool(*node, "enabled", true);
}

void UID_BuildFocusOrder(const uid_document_t *doc, std::vector<uid_node_id_t> *out)
{
	if (!doc || !out) {
		return;
	}
	out->clear();
	if (doc->rootNode != UID_INVALID_NODE_ID) {
		CollectFocusWalk(doc, doc->rootNode, true, true, out);
	}

	std::stable_sort(out->begin(), out->end(), [&](uid_node_id_t a, uid_node_id_t b) {
		const uid_node_def_t *na = UID_GetNode(doc, a);
		const uid_node_def_t *nb = UID_GetNode(doc, b);
		const int ta = na ? TabIndexOf(*na) : -1;
		const int tb = nb ? TabIndexOf(*nb) : -1;
		const bool aHas = ta >= 0;
		const bool bHas = tb >= 0;
		if (aHas != bHas) {
			return aHas && !bHas;
		}
		if (aHas && bHas && ta != tb) {
			return ta < tb;
		}
		return a < b;
	});
}

uid_node_id_t UID_GetFocusedNode(const uid_document_t *doc)
{
	if (!doc) {
		return UID_INVALID_NODE_ID;
	}
	for (size_t i = 0; i < doc->states.size(); ++i) {
		if (doc->states[i].focused) {
			return static_cast<uid_node_id_t>(i);
		}
	}
	return UID_INVALID_NODE_ID;
}

void UID_SetFocus(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend)
{
	if (!doc) {
		return;
	}
	EnsureStates(doc);

	const uid_node_id_t prev = UID_GetFocusedNode(doc);
	if (prev == id) {
		return;
	}

	if (prev != UID_INVALID_NODE_ID) {
		uid_node_state_t *pst = State(doc, prev);
		uid_node_def_t *pnode = UID_GetNode(doc, prev);
		if (pst) {
			if (pnode && pnode->kind == UID_NODE_INPUT && pst->focused) {
				CommitInput(doc, prev, backend);
			}
			pst->focused = false;
			pst->capturing = false;
			if (pnode && pnode->kind == UID_NODE_SELECT) {
				pst->overlayOpen = false;
			}
		}
		UID_DispatchEvent(doc, prev, UID_EVENT_BLUR, backend);
	}

	if (id != UID_INVALID_NODE_ID) {
		uid_node_state_t *st = State(doc, id);
		uid_node_def_t *node = UID_GetNode(doc, id);
		if (st && node && UID_IsInteractiveKind(node->kind) && st->effectivelyEnabled) {
			st->focused = true;
			if (node->kind == UID_NODE_INPUT) {
				st->preEditValue = st->runtimeValue.hasValue ? st->runtimeValue.stringValue : node->text;
				st->editBuffer = st->preEditValue;
				st->caretCodepoint = Utf8CodepointCount(st->editBuffer);
				st->anchorCodepoint = st->caretCodepoint;
			}
			UID_DispatchEvent(doc, id, UID_EVENT_FOCUS, backend);
		}
	}

	MarkDirty(doc, UID_DIRTY_PAINT);
}

void UID_HandlePointer(
	uid_document_t *doc,
	const uid_pointer_state_t *pointer,
	unsigned int realtime,
	const uid_backend_t *backend
)
{
	if (!doc || !pointer) {
		return;
	}
	EnsureStates(doc);
	uid_input_scratch_t &scratch = doc->inputScratch;

	const int buttons = pointer->buttons;
	const bool leftDown = (buttons & UID_POINTER_BUTTON_LEFT) != 0;
	const bool wasLeft = (scratch.lastButtons & UID_POINTER_BUTTON_LEFT) != 0;
	const bool pressed = leftDown && !wasLeft;
	const bool released = !leftDown && wasLeft;

	if (pointer->wheel != 0) {
		const uid_node_id_t scrollId = FindDeepestScrollContainerAtPoint(doc, pointer->x, pointer->y);
		if (!ApplyScrollWheel(doc, scrollId, pointer->wheel)) {
			const uid_node_id_t wheelHit = UID_HitTest(doc, pointer->x, pointer->y, true);
			HandleScrollWheel(doc, wheelHit, pointer->wheel);
		}
	}

	if (UID_HandleScrollbarPointer(doc, pointer->x, pointer->y, buttons, scratch.lastButtons, backend)) {
		scratch.lastButtons = buttons;
		return;
	}

	const uid_node_id_t hit = UID_HitTest(doc, pointer->x, pointer->y, true);

	/*
	 * Added in OPM: host regions (server-list) receive pointer before normal
	 * control handling so row select / header sort / wheel stay host-owned.
	 */
	if (hit != UID_INVALID_NODE_ID && backend && backend->hostRegionPointer) {
		uid_node_def_t *hnode = UID_GetNode(doc, hit);
		uid_node_state_t *hst = State(doc, hit);
		if (hnode && hst && hnode->kind == UID_NODE_SERVER_LIST && hst->effectivelyEnabled) {
			const char *role = hnode->role.empty() ? "server-list" : hnode->role.c_str();
			const float localX = pointer->x - hst->borderBox.x;
			const float localY = pointer->y - hst->borderBox.y;
			const bool consumed = backend->hostRegionPointer(
				role,
				localX,
				localY,
				buttons,
				pointer->wheel,
				backend->userdata
			);
			for (uid_node_state_t &st : doc->states) {
				st.hovered = false;
			}
			hst->hovered = true;
			if (consumed) {
				scratch.lastButtons = buttons;
				if (pressed) {
					scratch.pressNode = hit;
				}
				if (released) {
					scratch.pressNode = UID_INVALID_NODE_ID;
				}
				MarkDirty(doc, UID_DIRTY_PAINT);
				return;
			}
		}
	}

	for (uid_node_state_t &st : doc->states) {
		st.hovered = false;
	}
	if (hit != UID_INVALID_NODE_ID) {
		uid_node_state_t *hst = State(doc, hit);
		if (hst) {
			hst->hovered = true;
		}
	}

	/* Slider drag — live write if commit=change; stage until release if submit. */
	for (size_t i = 0; i < doc->nodes.size() && i < doc->states.size(); ++i) {
		if (doc->nodes[i].kind == UID_NODE_SLIDER && doc->states[i].dragging) {
			if (leftDown) {
				ApplySliderAtPointer(doc, static_cast<uid_node_id_t>(i), pointer->x, backend);
			} else {
				/* Changed in OPM: flush commit=submit (and change) on release. */
				doc->states[i].dragging = false;
				CommitSliderBinding(doc, static_cast<uid_node_id_t>(i), backend);
			}
		}
	}

	if (pressed) {
		scratch.pressNode = hit;
		if (hit != UID_INVALID_NODE_ID) {
			uid_node_def_t *node = UID_GetNode(doc, hit);
			uid_node_state_t *st = State(doc, hit);
			if (node && st && st->effectivelyEnabled && UID_IsInteractiveKind(node->kind) &&
				node->kind != UID_NODE_SERVER_LIST) {
				st->pressed = true;
				UID_SetFocus(doc, hit, backend);
				if (node->kind == UID_NODE_SLIDER) {
					st->dragging = true;
					ApplySliderAtPointer(doc, hit, pointer->x, backend);
				}
				if (node->kind == UID_NODE_SELECT) {
					if (node->appearance == "cyclic") {
						/* Added in OPM: left/right 32px chevron columns step and wrap. */
						const float chevronW = UID_ScaleAuthoredPx(doc, 32.0f);
						const uid_rect_t &box = st->borderBox;
						if (pointer->x < box.x + chevronW) {
							StepCyclicSelect(doc, hit, -1, backend);
						} else if (pointer->x >= box.x + box.w - chevronW) {
							StepCyclicSelect(doc, hit, 1, backend);
						}
					}
					/* Added in OPM: dropdown modal opens on release (not press) so the
					 * fullscreen dismiss scrim cannot eat the same click. */
				}
			} else {
				/* Click outside closes selects */
				for (size_t i = 0; i < doc->states.size(); ++i) {
					if (doc->states[i].overlayOpen) {
						doc->states[i].overlayOpen = false;
						MarkDirty(doc, UID_DIRTY_PAINT);
					}
				}
				UID_SetFocus(doc, UID_INVALID_NODE_ID, backend);
			}
		} else {
			for (size_t i = 0; i < doc->states.size(); ++i) {
				doc->states[i].overlayOpen = false;
			}
			UID_SetFocus(doc, UID_INVALID_NODE_ID, backend);
		}
		MarkDirty(doc, UID_DIRTY_PAINT);
	}

	if (released) {
		uid_node_id_t pressNode = scratch.pressNode;
		scratch.pressNode = UID_INVALID_NODE_ID;

		for (uid_node_state_t &st : doc->states) {
			st.pressed = false;
			/* Slider dragging cleared above with CommitSliderBinding when button up. */
		}

		if (pressNode != UID_INVALID_NODE_ID && pressNode == hit) {
			const uid_node_id_t actionId = FindCollectionActionNode(doc, pressNode);
			uid_node_def_t *node = UID_GetNode(doc, actionId);
			uid_node_state_t *st = State(doc, actionId);
			if (node && st && st->effectivelyEnabled) {
				/* Fixed in OPM: ActivateButton/SyncCollections may rebuild nodes —
				 * snapshot kind/flags before activate; never touch stale node*. */
				const bool isStepOrSet = node->hasSetIndex || node->hasStepIndex;
				const int nodeKind = static_cast<int>(node->kind);
				if (isStepOrSet) {
					ActivateButton(doc, actionId, backend);
				} else {
					switch (node->kind) {
					case UID_NODE_BUTTON:
						ActivateButton(doc, actionId, backend);
						break;
					case UID_NODE_SELECT:
						/* Added in OPM: open relative/fullscreen modal on click release. */
						if (node->appearance != "cyclic") {
							OpenSelect(doc, actionId, backend);
						}
						break;
					case UID_NODE_TOGGLE:
						ToggleValue(doc, actionId, backend);
						break;
					case UID_NODE_KEYBIND:
						st->capturing = true;
						MarkDirty(doc, UID_DIRTY_PAINT);
						break;
					default:
						break;
					}
				}
				if (nodeKind == static_cast<int>(UID_NODE_BUTTON) || isStepOrSet) {
					const float dx = pointer->x - scratch.lastClickX;
					const float dy = pointer->y - scratch.lastClickY;
					const bool sameNode = scratch.lastClickNode == actionId;
					const bool closeInTime =
						scratch.lastClickTime > 0 && realtime - scratch.lastClickTime <= 500;
					const bool closeInSpace = (dx * dx + dy * dy) <= 4.0f;
					if (sameNode && closeInTime && closeInSpace) {
						UID_DispatchEvent(doc, actionId, UID_EVENT_DBLCLICK, backend);
						scratch.lastClickTime = 0;
						scratch.lastClickNode = UID_INVALID_NODE_ID;
					} else {
						scratch.lastClickTime = realtime;
						scratch.lastClickNode = actionId;
						scratch.lastClickX = pointer->x;
						scratch.lastClickY = pointer->y;
					}
				}
			}
		}
		MarkDirty(doc, UID_DIRTY_PAINT);
	}

	scratch.lastButtons = buttons;
}

bool UID_HandleKey(uid_document_t *doc, int key, bool down, unsigned time, const uid_backend_t *backend)
{
	(void)time;
	if (!doc) {
		return false;
	}
	EnsureStates(doc);
	uid_input_scratch_t &scratch = doc->inputScratch;

	if (key == UID_KEY_SHIFT) {
		scratch.shiftDown = down;
		return false;
	}

	if (!down) {
		return false;
	}

	/* Modal blocks underlying UI; ESC dismisses, Enter activates confirm role. */
	if (UID_IsModalActive(doc)) {
		if (key == UID_KEY_ESCAPE) {
			if (backend && backend->cvarWrite) {
				backend->cvarWrite(UID_DefaultModalCvarName(), "");
			}
			MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_BINDING | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
			return true;
		}
		const uid_node_id_t modalRoot = UID_GetModalRoot(doc);
		if (modalRoot != UID_INVALID_NODE_ID &&
			(key == UID_KEY_ENTER || key == UID_KEY_SPACE)) {
			uid_node_id_t actionId = UID_FindModalRoleButton(doc, modalRoot, "confirm");
			if (actionId == UID_INVALID_NODE_ID) {
				actionId = UID_FindModalRoleButton(doc, modalRoot, "cancel");
			}
			if (actionId != UID_INVALID_NODE_ID) {
				ActivateButton(doc, actionId, backend);
				return true;
			}
		}
		return true;
	}

	/* Keybind capture takes priority */
	for (size_t i = 0; i < doc->nodes.size() && i < doc->states.size(); ++i) {
		if (doc->nodes[i].kind != UID_NODE_KEYBIND || !doc->states[i].capturing) {
			continue;
		}
		uid_node_state_t *st = &doc->states[i];
		if (key == UID_KEY_ESCAPE) {
			st->capturing = false;
			MarkDirty(doc, UID_DIRTY_PAINT);
			UID_DispatchEvent(doc, static_cast<uid_node_id_t>(i), UID_EVENT_CANCEL, backend);
			return true;
		}
		if (key == UID_KEY_BACKSPACE || key == UID_KEY_DEL) {
			SetRuntimeString(st, "");
			st->capturing = false;
			MarkDirty(doc, UID_DIRTY_PAINT | UID_DIRTY_BINDING);
			UID_DispatchEvent(doc, static_cast<uid_node_id_t>(i), UID_EVENT_CHANGE, backend);
			MaybeWriteBinding(doc, static_cast<uid_node_id_t>(i), backend, UID_COMMIT_CHANGE);
			return true;
		}
		/* Commit capture directly; do not MaybeWriteBinding on CHANGE (stale display text). */
		st->capturing = false;
		MarkDirty(doc, UID_DIRTY_PAINT | UID_DIRTY_BINDING);
		UID_DispatchEvent(doc, static_cast<uid_node_id_t>(i), UID_EVENT_CHANGE, backend);
		UID_TryCommitKeybindCapture(doc, static_cast<uid_node_id_t>(i), key, backend);
		return true;
	}

	uid_node_id_t focus = UID_GetFocusedNode(doc);

	if (focus != UID_INVALID_NODE_ID) {
		uid_node_def_t *focusNode = UID_GetNode(doc, focus);
		uid_node_state_t *focusSt = State(doc, focus);
		if (focusNode && focusSt && focusNode->kind == UID_NODE_KEYBIND && !focusSt->capturing) {
			if (key == UID_KEY_BACKSPACE || key == UID_KEY_DEL) {
				SetRuntimeString(focusSt, "");
				MarkDirty(doc, UID_DIRTY_PAINT | UID_DIRTY_BINDING);
				UID_DispatchEvent(doc, focus, UID_EVENT_CHANGE, backend);
				MaybeWriteBinding(doc, focus, backend, UID_COMMIT_CHANGE);
				return true;
			}
		}
	}

	focus = UID_GetFocusedNode(doc);

	if (key == UID_KEY_TAB) {
		std::vector<uid_node_id_t> order;
		UID_BuildFocusOrder(doc, &order);
		if (order.empty()) {
			return true;
		}
		int idx = -1;
		for (size_t i = 0; i < order.size(); ++i) {
			if (order[i] == focus) {
				idx = static_cast<int>(i);
				break;
			}
		}
		int next;
		if (scratch.shiftDown) {
			next = (idx <= 0) ? static_cast<int>(order.size()) - 1 : idx - 1;
		} else {
			next = (idx < 0 || idx + 1 >= static_cast<int>(order.size())) ? 0 : idx + 1;
		}
		UID_SetFocus(doc, order[static_cast<size_t>(next)], backend);
		return true;
	}

	if (focus == UID_INVALID_NODE_ID) {
		return false;
	}

	uid_node_def_t *node = UID_GetNode(doc, focus);
	uid_node_state_t *st = State(doc, focus);
	if (!node || !st || !st->effectivelyEnabled) {
		return false;
	}

	if (node->kind == UID_NODE_BUTTON) {
		if (key == UID_KEY_ENTER || key == UID_KEY_SPACE) {
			ActivateButton(doc, focus, backend);
			return true;
		}
	}

	if (node->kind == UID_NODE_TOGGLE) {
		if (key == UID_KEY_ENTER || key == UID_KEY_SPACE) {
			ToggleValue(doc, focus, backend);
			return true;
		}
	}

	if (node->kind == UID_NODE_SLIDER) {
		const double step = node->hasStep ? node->stepValue : 1.0;
		double val = SliderValue(*node, *st);
		if (key == UID_KEY_LEFTARROW || key == UID_KEY_DOWNARROW) {
			SetSliderValue(doc, focus, val - step, backend, true);
			CommitSliderBinding(doc, focus, backend);
			return true;
		}
		if (key == UID_KEY_RIGHTARROW || key == UID_KEY_UPARROW) {
			SetSliderValue(doc, focus, val + step, backend, true);
			CommitSliderBinding(doc, focus, backend);
			return true;
		}
		if (key == UID_KEY_HOME) {
			SetSliderValue(doc, focus, node->hasMin ? node->minValue : 0.0, backend, true);
			CommitSliderBinding(doc, focus, backend);
			return true;
		}
		if (key == UID_KEY_END) {
			SetSliderValue(doc, focus, node->hasMax ? node->maxValue : 1.0, backend, true);
			CommitSliderBinding(doc, focus, backend);
			return true;
		}
	}

	if (node->kind == UID_NODE_SELECT) {
		if (node->appearance == "cyclic") {
			/* Added in OPM: arrows step; Enter/Space do not open overlay. */
			if (key == UID_KEY_LEFTARROW || key == UID_KEY_DOWNARROW) {
				StepCyclicSelect(doc, focus, -1, backend);
				return true;
			}
			if (key == UID_KEY_RIGHTARROW || key == UID_KEY_UPARROW) {
				StepCyclicSelect(doc, focus, 1, backend);
				return true;
			}
			if (key == UID_KEY_ENTER || key == UID_KEY_SPACE) {
				return true;
			}
		} else {
			if (key == UID_KEY_ENTER || key == UID_KEY_SPACE) {
				OpenSelect(doc, focus, backend);
				return true;
			}
			if (key == UID_KEY_ESCAPE && st->overlayOpen) {
				CloseSelect(doc, focus);
				return true;
			}
		}
	}

	{
		const uid_node_id_t scopeId = UID_FindCollectionScope(doc, focus);
		if (scopeId != UID_INVALID_NODE_ID) {
			uid_node_def_t *scope = UID_GetNode(doc, scopeId);
			if (scope && !scope->collectionSource.empty()) {
				uid_layout_axis_t axis = UID_AXIS_HORIZONTAL;
				UID_ParseAxis(PropCStr(*scope, "type", "horizontal"), &axis, nullptr);
				const bool horiz = (axis == UID_AXIS_HORIZONTAL);
				int delta = 0;
				if (horiz && key == UID_KEY_LEFTARROW) {
					delta = -1;
				} else if (horiz && key == UID_KEY_RIGHTARROW) {
					delta = 1;
				} else if (!horiz && key == UID_KEY_UPARROW) {
					delta = -1;
				} else if (!horiz && key == UID_KEY_DOWNARROW) {
					delta = 1;
				}
				if (delta != 0) {
					if (UID_StepCollectionIndex(doc, scopeId, delta, backend)) {
						UID_SyncCollections(doc, backend);
					}
					return true;
				}
				if (key == UID_KEY_HOME) {
					if (UID_SetCollectionIndex(doc, scopeId, 0, backend)) {
						UID_SyncCollections(doc, backend);
					}
					return true;
				}
				if (key == UID_KEY_END) {
					const uid_node_state_t *scopeSt = State(doc, scopeId);
					if (scopeSt && scopeSt->collectionItemCount > 0) {
						if (UID_SetCollectionIndex(doc, scopeId, scopeSt->collectionItemCount - 1, backend)) {
							UID_SyncCollections(doc, backend);
						}
					}
					return true;
				}
			}
		}
	}

	if (node->kind == UID_NODE_KEYBIND) {
		if (key == UID_KEY_ENTER || key == UID_KEY_SPACE) {
			st->capturing = true;
			MarkDirty(doc, UID_DIRTY_PAINT);
			return true;
		}
	}

	if (node->kind == UID_NODE_INPUT) {
		const size_t len = Utf8CodepointCount(st->editBuffer);
		if (key == UID_KEY_ESCAPE) {
			st->editBuffer = st->preEditValue;
			st->caretCodepoint = Utf8CodepointCount(st->editBuffer);
			MarkDirty(doc, UID_DIRTY_PAINT);
			UID_DispatchEvent(doc, focus, UID_EVENT_CANCEL, backend);
			return true;
		}
		if (key == UID_KEY_ENTER) {
			CommitInput(doc, focus, backend);
			return true;
		}
		if (key == UID_KEY_HOME) {
			st->caretCodepoint = 0;
			st->anchorCodepoint = 0;
			MarkDirty(doc, UID_DIRTY_PAINT);
			return true;
		}
		if (key == UID_KEY_END) {
			st->caretCodepoint = len;
			st->anchorCodepoint = len;
			MarkDirty(doc, UID_DIRTY_PAINT);
			return true;
		}
		if (key == UID_KEY_LEFTARROW) {
			if (st->caretCodepoint > 0) {
				--st->caretCodepoint;
			}
			st->anchorCodepoint = st->caretCodepoint;
			MarkDirty(doc, UID_DIRTY_PAINT);
			return true;
		}
		if (key == UID_KEY_RIGHTARROW) {
			if (st->caretCodepoint < len) {
				++st->caretCodepoint;
			}
			st->anchorCodepoint = st->caretCodepoint;
			MarkDirty(doc, UID_DIRTY_PAINT);
			return true;
		}
		if (key == UID_KEY_BACKSPACE) {
			if (st->caretCodepoint > 0) {
				const size_t start = Utf8OffsetForCodepoint(st->editBuffer, st->caretCodepoint - 1);
				const size_t end = Utf8OffsetForCodepoint(st->editBuffer, st->caretCodepoint);
				st->editBuffer.erase(start, end - start);
				--st->caretCodepoint;
				st->anchorCodepoint = st->caretCodepoint;
				MarkDirty(doc, UID_DIRTY_PAINT | UID_DIRTY_LAYOUT);
				LiveCommitTextInputIfNeeded(doc, focus, node, st, backend);
			}
			return true;
		}
		if (key == UID_KEY_DEL) {
			if (st->caretCodepoint < len) {
				const size_t start = Utf8OffsetForCodepoint(st->editBuffer, st->caretCodepoint);
				const size_t end = Utf8OffsetForCodepoint(st->editBuffer, st->caretCodepoint + 1);
				st->editBuffer.erase(start, end - start);
				MarkDirty(doc, UID_DIRTY_PAINT | UID_DIRTY_LAYOUT);
				LiveCommitTextInputIfNeeded(doc, focus, node, st, backend);
			}
			return true;
		}
	}

	return false;
}

bool UID_HandleChar(uid_document_t *doc, unsigned codepoint, const uid_backend_t *backend)
{
	if (!doc || codepoint < 32 || codepoint == 127) {
		return false;
	}
	EnsureStates(doc);

	const uid_node_id_t focus = UID_GetFocusedNode(doc);
	if (focus == UID_INVALID_NODE_ID) {
		return false;
	}
	uid_node_def_t *node = UID_GetNode(doc, focus);
	uid_node_state_t *st = State(doc, focus);
	if (!node || !st || node->kind != UID_NODE_INPUT || !st->effectivelyEnabled) {
		return false;
	}

	if (node->inputType == "number") {
		if (!(codepoint == '-' || codepoint == '.' || codepoint == '+' ||
			  (codepoint >= '0' && codepoint <= '9'))) {
			return true; /* consume but ignore */
		}
	}

	const char *maxLenStr = node->properties.GetCStr("max-length", nullptr);
	if (maxLenStr) {
		double maxLen = 0.0;
		if (UID_ParseNumber(maxLenStr, &maxLen, nullptr) && maxLen >= 0.0) {
			if (static_cast<double>(Utf8CodepointCount(st->editBuffer)) >= maxLen) {
				return true;
			}
		}
	}

	const size_t insertAt = Utf8OffsetForCodepoint(st->editBuffer, st->caretCodepoint);
	std::string ch;
	AppendUtf8(&ch, codepoint);
	if (ch.empty()) {
		return true; /* rejected scalar */
	}
	if (doc->limits.maxTextBytes > 0 &&
		static_cast<int>(st->editBuffer.size() + ch.size()) > doc->limits.maxTextBytes) {
		return true;
	}
	st->editBuffer.insert(insertAt, ch);
	++st->caretCodepoint;
	st->anchorCodepoint = st->caretCodepoint;
	MarkDirty(doc, UID_DIRTY_PAINT | UID_DIRTY_LAYOUT);
	LiveCommitTextInputIfNeeded(doc, focus, node, st, backend);
	return true;
}
