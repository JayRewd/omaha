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

#include "uid_document.h"
#include "uid_value.h"

#include <cstring>
#include <new>
#include <string>

void UID_InitNodeDef(uid_node_def_t *node)
{
	if (!node) {
		return;
	}
	node->kind = UID_NODE_CONTAINER;
	node->source.path = nullptr;
	node->source.line = 0;
	node->source.column = 0;
	node->id.clear();
	node->properties.Clear();
	node->text.clear();
	node->children.clear();
	node->handlers.clear();
	node->options.clear();
	node->optionSource.clear();
	node->appearance.clear();
	node->openModal.clear();
	node->valueType.clear();
	node->visibleIf.clear();
	node->enabledIf.clear();
	node->visibleExpr.clear();
	node->enabledExpr.clear();
	node->visibleExprBound = false;
	node->enabledExprBound = false;
	node->visibleExprProbed = false;
	node->enabledExprProbed = false;
	node->styleExprs.clear();
	node->setValue.clear();
	node->cvarBoundProps.clear();
	node->exprBoundProps.clear();
	node->bindingFlags = 0;
	node->bindingFlagsValid = false;
	node->templateId.clear();
	node->deferredUse = false;
	node->deferredUseExpanded = false;
	node->shapeId.clear();
	node->modelPath.clear();
	node->team.clear();
	node->anim.clear();
	node->animPhase = 0.0f;
	node->hasAnimPhase = false;
	node->animVariant = 0;
	node->hasAnimVariant = false;
	node->modelAngles[0] = node->modelAngles[1] = node->modelAngles[2] = 0.0f;
	node->hasModelAngles = false;
	node->modelOffset[0] = node->modelOffset[1] = node->modelOffset[2] = 0.0f;
	node->hasModelOffset = false;
	node->bboxMins[0] = node->bboxMins[1] = node->bboxMins[2] = 0.0f;
	node->bboxMaxs[0] = node->bboxMaxs[1] = node->bboxMaxs[2] = 0.0f;
	node->hasBbox = false;
	node->bboxFromModel = false;
	node->modelFov = 0.0f;
	node->hasModelFov = false;
	node->modelScale = 0.0f;
	node->hasModelScale = false;
	node->framingScale = 0.0f;
	node->hasFramingScale = false;
	node->modelColor[0] = node->modelColor[1] = node->modelColor[2] = 0.0f;
	node->modelColor[3] = 1.0f;
	node->hasModelColor = false;
	node->role.clear();
	node->collectionSource.clear();
	node->collectionDisplay.clear();
	node->collectionDefaultIndex = -1;
	node->hasCollectionDefaultIndex = false;
	node->indexBind.clear();
	node->collectionWrap = false;
	node->collectionScroll = false;
	node->foreachMode.clear();
	node->foreachCountExpr.clear();
	node->hasForeachCount = false;
	node->foreachRowHeight = 0.0f;
	node->hasForeachRowHeight = false;
	node->hasForeachLifetime = false;
	node->foreachLifetimeMs = 0;
	node->foreachFadeDurationMs = 0;
	node->foreachTemplateNodes.clear();
	node->foreachTemplateRoot = UID_INVALID_NODE_ID;
	node->foreachScopeId = UID_INVALID_NODE_ID;
	node->foreachItemIndex = -1;
	node->foreachGenerated = false;
	node->hasStepIndex = false;
	node->stepIndex = 0;
	node->hasSetIndex = false;
	node->setIndexValue = -1;
	node->visibleIfIndex.clear();
	node->scrollbarTemplateId.clear();
	node->scrollbarGenerated = false;
	node->inputType.clear();
	node->binding.clear();
	node->confirmModal.clear();
	node->modalCvar.clear();
	node->bindSlot = 0;
	node->minValue = 0.0;
	node->maxValue = 0.0;
	node->stepValue = 0.0;
	node->hasMin = false;
	node->hasMax = false;
	node->hasStep = false;
	node->bind.clear();
	node->commit = UID_COMMIT_CHANGE;
	node->hasCommit = false;
}

void UID_InitNodeState(uid_node_state_t *state)
{
	if (!state) {
		return;
	}
	state->hovered = false;
	state->pressed = false;
	state->focused = false;
	state->effectivelyEnabled = true;
	state->runtimeValue.hasValue = false;
	state->runtimeValue.stringValue.clear();
	state->editBuffer.clear();
	state->preEditValue.clear();
	state->caretCodepoint = 0;
	state->anchorCodepoint = 0;
	state->scrollX = 0.0f;
	state->scrollY = 0.0f;
	state->contentExtentW = 0.0f;
	state->contentExtentH = 0.0f;
	std::memset(&state->marginBox, 0, sizeof(state->marginBox));
	std::memset(&state->borderBox, 0, sizeof(state->borderBox));
	std::memset(&state->contentBox, 0, sizeof(state->contentBox));
	std::memset(&state->effectiveClip, 0, sizeof(state->effectiveClip));
	state->capturing = false;
	state->overlayOpen = false;
	state->dragging = false;
	state->highlightIndex = -1;
	state->collectionItems.clear();
	state->collectionSelectedIndex = -1;
	state->collectionItemCount = 0;
	state->collectionRevision = 0;
	state->collectionScrollOffset = 0;
	state->collectionRefreshFrame = 0;
	state->foreachExpandSig = 0;
	state->foreachAppearAtMs.clear();
	state->lifetimeOpacityMul = 1.0f;
	std::memset(&state->scrollbarTrackRect, 0, sizeof(state->scrollbarTrackRect));
	std::memset(&state->scrollbarThumbRect, 0, sizeof(state->scrollbarThumbRect));
	state->scrollbarDragging = false;
	state->scrollbarDragOffset = 0.0f;
	state->scrollbarVisible = false;
	state->cachedFont = nullptr;
	state->cachedTextWidth = -1.0f;
	state->cachedTextKey = 0;
	state->cachedMeasureKey = 0;
	state->visibleEpoch = 0;
	state->visibleCached = false;
	state->visibleCachedValue = true;
	state->enabledEpoch = 0;
	state->enabledCached = false;
	state->enabledCachedValue = true;
	state->styleExprEpoch = 0;
	state->styleExprCached = false;
	state->cachedShapePaths.clear();
	state->cachedShapeKey = 0;
	state->cachedShapeValid = false;
}

uid_document_t *UID_CreateDocument(void)
{
	uid_document_t *doc = new (std::nothrow) uid_document_t();
	if (!doc) {
		return nullptr;
	}
	UID_ClearDocument(doc);
	return doc;
}

void UID_DestroyDocument(uid_document_t *doc)
{
	if (!doc) {
		return;
	}
	UID_ClearDocument(doc);
	delete doc;
}

void UID_ClearDocument(uid_document_t *doc)
{
	if (!doc) {
		return;
	}
	doc->sourceName.clear();
	doc->hasMenuMeta = false;
	doc->menuId.clear();
	doc->drawOrder = 0;
	doc->menuBackdrop = UID_MENU_BACKDROP_NONE;
	doc->pointerExpr.clear();
	UID_DefaultLimits(&doc->limits);
	doc->definitions.defaults.Clear();
	doc->definitions.fonts.clear();
	doc->definitions.shapes.clear();
	doc->definitions.templates.clear();
	doc->definitions.modals.clear();
	doc->definitions.sources.clear();
	doc->definitions.vars.clear();
	doc->nodes.clear();
	doc->rootNode = UID_INVALID_NODE_ID;
	doc->states.clear();
	doc->idIndex.clear();
	doc->dirty = UID_DIRTY_NONE;
	doc->expanded = false;
	doc->lastFbScale = 1.0f;
	doc->lastUiPxScale = 1.0f;
	doc->lastLogicalW = 0;
	doc->lastLogicalH = 0;
	doc->inputScratch.shiftDown = false;
	doc->inputScratch.lastButtons = 0;
	doc->inputScratch.pressNode = UID_INVALID_NODE_ID;
	doc->inputScratch.lastClickTime = 0;
	doc->inputScratch.lastClickNode = UID_INVALID_NODE_ID;
	doc->inputScratch.lastClickX = 0.0f;
	doc->inputScratch.lastClickY = 0.0f;
	doc->updateTimeMs = 0;
	doc->collectionFieldsApplied = false;
	doc->syncFrameCounter = 0;
	doc->activeModalId.clear();
	doc->modalOverlayBase = 0;
	doc->modalRootNode = UID_INVALID_NODE_ID;
	doc->modalOpenerNode = UID_INVALID_NODE_ID;
	doc->keybindPending.active = false;
	doc->keybindPending.nodeId = UID_INVALID_NODE_ID;
	doc->keybindPending.slot = 0;
	doc->keybindPending.newKey = -1;
	doc->keybindPending.command.clear();
}

uid_node_def_t *UID_GetNodeById(uid_document_t *doc, const char *id)
{
	if (!doc || !id || !id[0]) {
		return nullptr;
	}
	auto it = doc->idIndex.find(id);
	if (it == doc->idIndex.end()) {
		return nullptr;
	}
	return UID_GetNode(doc, it->second);
}

const uid_node_def_t *UID_GetNodeById(const uid_document_t *doc, const char *id)
{
	if (!doc || !id || !id[0]) {
		return nullptr;
	}
	auto it = doc->idIndex.find(id);
	if (it == doc->idIndex.end()) {
		return nullptr;
	}
	return UID_GetNode(doc, it->second);
}

uid_node_def_t *UID_GetNode(uid_document_t *doc, uid_node_id_t id)
{
	if (!doc || id < 0 || static_cast<size_t>(id) >= doc->nodes.size()) {
		return nullptr;
	}
	return &doc->nodes[static_cast<size_t>(id)];
}

const uid_node_def_t *UID_GetNode(const uid_document_t *doc, uid_node_id_t id)
{
	if (!doc || id < 0 || static_cast<size_t>(id) >= doc->nodes.size()) {
		return nullptr;
	}
	return &doc->nodes[static_cast<size_t>(id)];
}

const uid_font_def_t *UID_FindFontDef(const uid_document_t *doc, const char *fontId, int requestedWeight)
{
	if (!doc) {
		return nullptr;
	}

	const uid_font_def_t *byId = nullptr;
	if (fontId && fontId[0]) {
		auto it = doc->definitions.fonts.find(fontId);
		if (it != doc->definitions.fonts.end()) {
			byId = &it->second;
			if (byId->weight == requestedWeight) {
				return byId;
			}
		}
	}

	/* Prefer any registered face whose weight matches font-weight. */
	for (const auto &kv : doc->definitions.fonts) {
		if (kv.second.weight == requestedWeight) {
			return &kv.second;
		}
	}

	/* Fall back to the id match so one path still works. */
	return byId;
}

bool UID_IsSliderPartKind(uid_node_kind_t kind)
{
	return kind == UID_NODE_SLIDER_TRACK || kind == UID_NODE_SLIDER_RANGE ||
		kind == UID_NODE_SLIDER_THUMB;
}

bool UID_IsScrollbarPartKind(uid_node_kind_t kind)
{
	return kind == UID_NODE_SCROLLBAR_TRACK || kind == UID_NODE_SCROLLBAR_THUMB;
}

bool UID_SyncSliderBounds(uid_node_def_t *node)
{
	std::string dm;
	std::string v;

	if (!node || node->kind != UID_NODE_SLIDER) {
		return false;
	}

	node->hasMin = false;
	node->hasMax = false;
	node->hasStep = false;

	if (node->properties.Get("min", &v) && !v.empty()) {
		if (!UID_ParseNumber(v.c_str(), &node->minValue, &dm)) {
			return false;
		}
		node->hasMin = true;
	}
	if (node->properties.Get("max", &v) && !v.empty()) {
		if (!UID_ParseNumber(v.c_str(), &node->maxValue, &dm)) {
			return false;
		}
		node->hasMax = true;
	}
	if (node->properties.Get("step", &v) && !v.empty()) {
		if (!UID_ParseNumber(v.c_str(), &node->stepValue, &dm)) {
			return false;
		}
		node->hasStep = true;
	}

	if (!node->hasMin || !node->hasMax || !node->hasStep) {
		return false;
	}
	if (!(node->minValue <= node->maxValue) || !(node->stepValue > 0.0)) {
		return false;
	}
	return true;
}

/* Added in OPM: optional number-input min/max/step from properties (after template expand). */
bool UID_SyncInputBounds(uid_node_def_t *node)
{
	std::string dm;
	std::string v;

	if (!node || node->kind != UID_NODE_INPUT) {
		return false;
	}

	node->hasMin = false;
	node->hasMax = false;
	node->hasStep = false;

	if (node->properties.Get("min", &v) && !v.empty()) {
		if (!UID_ParseNumber(v.c_str(), &node->minValue, &dm)) {
			return false;
		}
		node->hasMin = true;
	}
	if (node->properties.Get("max", &v) && !v.empty()) {
		if (!UID_ParseNumber(v.c_str(), &node->maxValue, &dm)) {
			return false;
		}
		node->hasMax = true;
	}
	if (node->properties.Get("step", &v) && !v.empty()) {
		if (!UID_ParseNumber(v.c_str(), &node->stepValue, &dm)) {
			return false;
		}
		node->hasStep = true;
	}

	if (node->hasMin && node->hasMax && !(node->minValue <= node->maxValue)) {
		return false;
	}
	if (node->hasStep && !(node->stepValue > 0.0)) {
		return false;
	}
	return true;
}

uid_node_id_t UID_FindChildOfKind(const uid_document_t *doc, uid_node_id_t parent, uid_node_kind_t kind)
{
	const uid_node_def_t *node = UID_GetNode(doc, parent);
	if (!node) {
		return UID_INVALID_NODE_ID;
	}
	for (uid_node_id_t c : node->children) {
		const uid_node_def_t *child = UID_GetNode(doc, c);
		if (child && child->kind == kind) {
			return c;
		}
	}
	return UID_INVALID_NODE_ID;
}
