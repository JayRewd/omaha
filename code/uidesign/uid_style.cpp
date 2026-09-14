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

#include "uid_style.h"

#include "uid_binding.h"
#include "uid_expr_bool.h"
#include "uid_layout.h"
#include "uid_profile.h"
#include "uid_value.h"
#include "uid_widget.h"

#include <cstdio>
#include <cstring>
#include <cmath>

namespace {

static int g_styleCache = 1;

unsigned ScaleEpochOf(const uid_document_t *doc)
{
	float s = (doc && doc->lastUiPxScale > 0.0f) ? doc->lastUiPxScale : 1.0f;
	unsigned u = 0;
	std::memcpy(&u, &s, sizeof(u));
	return u;
}

unsigned InteractionKeyOf(const uid_node_state_t *st)
{
	if (!st) {
		return 0;
	}
	unsigned key = 0;
	if (st->hovered) {
		key |= 1u;
	}
	if (st->pressed) {
		key |= 2u;
	}
	if (st->focused) {
		key |= 4u;
	}
	if (st->effectivelyEnabled) {
		key |= 8u;
	}
	return key;
}

unsigned CvarEpochOf(const uid_backend_t *backend)
{
	if (backend && backend->cvarEpoch) {
		return backend->cvarEpoch();
	}
	return 0u;
}

const char *PropCStr(const uid_node_def_t &node, const char *name, const char *fallback)
{
	const char *v = node.properties.GetCStr(name, nullptr);
	if (v && v[0]) {
		return v;
	}
	const char *b = UID_BuiltinDefault(name);
	if (b) {
		return b;
	}
	return fallback;
}

bool IsDefaultRectShape(const uid_node_def_t &node)
{
	const char *shape = PropCStr(node, "shape", "rectangle");
	if (!shape || std::strcmp(shape, "rectangle") != 0) {
		return false;
	}
	uid_length_t radius;
	radius.unit = UID_LENGTH_PX;
	radius.value = 0.0f;
	if (!node.properties.GetLengthCached("radius", &radius)) {
		return true;
	}
	return radius.unit == UID_LENGTH_PX && radius.value <= 0.0f;
}

bool TextLooksCvarDependent(const char *text)
{
	if (!text || !text[0]) {
		return false;
	}
	return std::strstr(text, "cvar") != nullptr;
}

bool NodePaintDependsOnCvar(const uid_node_def_t &node)
{
	static const char *kProps[] = {
		"fill",
		"stroke",
		"color",
		"hover-fill",
		"hoverfill",
		"pressed-fill",
		"disabled-fill",
		"focus-fill",
		"hover-color",
		"pressed-color",
		"disabled-color",
		"focus-color",
		"stroke-width"
	};
	for (const char *name : kProps) {
		const char *v = node.properties.GetCStr(name, nullptr);
		if (TextLooksCvarDependent(v)) {
			return true;
		}
	}
	for (const auto &kv : node.styleExprs) {
		if (TextLooksCvarDependent(kv.second.c_str())) {
			return true;
		}
	}
	return false;
}

void RebuildComputedStyle(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend, uid_node_state_t *st)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node || !st) {
		return;
	}

	uid_computed_style_t &cs = st->computedStyle;
	cs.valid = false;
	cs.dependsOnCvar = NodePaintDependsOnCvar(*node);
	cs.propsVersion = node->properties.Version();
	cs.scaleEpoch = ScaleEpochOf(doc);
	cs.cvarEpoch = cs.dependsOnCvar ? CvarEpochOf(backend) : 0u;
	cs.interactionKey = InteractionKeyOf(st);
	cs.hasFill = false;
	cs.hasGradient = false;
	cs.fill = {};
	cs.gradientBrush.clear();
	cs.hasStroke = false;
	cs.stroke = {};
	cs.strokeWidthPx = 0.0f;
	cs.shapeName[0] = '\0';
	cs.rectShape = true;
	cs.isEdgeClip = false;
	cs.pathRotationDeg = 0.0f;
	cs.bgRotationDeg = 0.0f;
	cs.textColor = {1.0f, 1.0f, 1.0f, 1.0f};

	std::string fillOverride;
	if (UID_ResolveFillStyleTernary(doc, id, backend, &fillOverride)) {
	}

	uid_color_t fill{};
	std::string gradientBrush;
	const bool resolvedPaint = UID_ResolveFillPaint(
		doc,
		id,
		backend,
		&fill,
		&gradientBrush,
		fillOverride.empty() ? nullptr : fillOverride.c_str()
	);
	cs.hasGradient = resolvedPaint && !gradientBrush.empty();
	cs.hasFill = resolvedPaint && !cs.hasGradient && fill.a > 0.0f;
	cs.fill = fill;
	cs.gradientBrush = std::move(gradientBrush);

	{
		const char *strokeStr = PropCStr(*node, "stroke", nullptr);
		if (strokeStr && strokeStr[0]) {
			std::string dm;
			if (UID_ParseColor(strokeStr, &cs.stroke, &dm) && cs.stroke.a > 0.0f) {
				std::string widthStr = PropCStr(*node, "stroke-width", "1px");
				if (backend) {
					std::string resolved;
					if (UID_ResolvePropString(backend, widthStr, &resolved)) {
						widthStr = resolved;
					}
				}
				uid_length_t wLen{};
				if (UID_ParseLength(widthStr.c_str(), &wLen, &dm) && wLen.unit == UID_LENGTH_PX && wLen.value > 0.0f) {
					cs.strokeWidthPx = UID_ScaleAuthoredPx(doc, wLen.value);
					cs.hasStroke = cs.strokeWidthPx > 0.0f;
				}
			}
		}
	}

	{
		const char *shapeName = PropCStr(*node, "shape", "rectangle");
		if (shapeName) {
			std::snprintf(cs.shapeName, sizeof(cs.shapeName), "%s", shapeName);
		} else {
			std::snprintf(cs.shapeName, sizeof(cs.shapeName), "rectangle");
		}
		cs.isEdgeClip = std::strcmp(cs.shapeName, "edge-clip") == 0;
		cs.rectShape = IsDefaultRectShape(*node) || !shapeName || !shapeName[0] ||
			doc->definitions.shapes.find(cs.shapeName) == doc->definitions.shapes.end();
	}

	{
		const char *rotStr = PropCStr(*node, "shape-rotation", nullptr);
		if (!rotStr || !rotStr[0]) {
			rotStr = PropCStr(*node, "rotation", nullptr);
		}
		if (rotStr && rotStr[0]) {
			(void)UID_ParseRotationDeg(rotStr, &cs.pathRotationDeg, nullptr);
		}
	}
	{
		const char *rotStr = PropCStr(*node, "rotation", nullptr);
		if (!rotStr || !rotStr[0]) {
			rotStr = PropCStr(*node, "shape-rotation", nullptr);
		}
		if (rotStr && rotStr[0]) {
			(void)UID_ParseRotationDeg(rotStr, &cs.bgRotationDeg, nullptr);
		}
	}

	(void)UID_ResolveTextColor(doc, id, &cs.textColor);

	cs.valid = true;
}

} // namespace

void UID_SetStyleCache(int enabled)
{
	g_styleCache = enabled ? 1 : 0;
}

int UID_StyleCacheEnabled(void)
{
	return g_styleCache;
}

void UID_InvalidateComputedStyle(uid_node_state_t *st)
{
	if (st) {
		st->computedStyle.valid = false;
	}
}

bool UID_ResolveFillStyleTernary(
	uid_document_t *doc,
	uid_node_id_t id,
	const uid_backend_t *backend,
	std::string *out
)
{
	if (!doc || !out) {
		return false;
	}
	out->clear();
	uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node) {
		return false;
	}

	const char *fillCur = node->properties.GetCStr("fill", nullptr);
	uid_color_t probe{};
	const bool knownPaint = fillCur && (UID_ParseColor(fillCur, &probe, nullptr) || UID_IsGradientBrush(fillCur) ||
		std::strncmp(fillCur, "cvar-rgba:", 10) == 0);
	if (knownPaint || node->styleExprs.empty()) {
		return false;
	}

	auto sit = node->styleExprs.find("fill");
	if (sit == node->styleExprs.end() || sit->second.empty()) {
		return false;
	}

	uid_bool_lookup_ctx_t bctx{};
	bctx.backend = backend;
	bctx.doc = doc;
	bctx.nodeId = id;
	bctx.item = nullptr;
	bctx.itemIndex = -1;
	bctx.itemCount = 0;
	bctx.selectedIndex = -1;
	if (node->foreachGenerated && node->foreachScopeId >= 0 &&
	    static_cast<size_t>(node->foreachScopeId) < doc->states.size()) {
		const uid_node_state_t &scopeSt = doc->states[static_cast<size_t>(node->foreachScopeId)];
		const int idx = node->foreachItemIndex;
		bctx.itemIndex = idx;
		bctx.itemCount = scopeSt.collectionItemCount;
		bctx.selectedIndex = scopeSt.collectionSelectedIndex;
		if (idx >= 0 && static_cast<size_t>(idx) < scopeSt.collectionItems.size()) {
			bctx.item = &scopeSt.collectionItems[static_cast<size_t>(idx)];
		}
	}

	std::string resolved;
	std::string diag;
	std::string expr = sit->second;
	if (expr.size() >= 2 && expr.front() == '{' && expr.back() == '}') {
		expr = expr.substr(1, expr.size() - 2);
	}
	if (!UID_EvalStyleTernary(expr.c_str(), &bctx, nullptr, &resolved, &diag)) {
		return false;
	}
	*out = std::move(resolved);
	return !out->empty();
}

const uid_computed_style_t *UID_EnsureComputedStyle(
	uid_document_t *doc,
	uid_node_id_t id,
	const uid_backend_t *backend
)
{
	if (!doc || id < 0 || static_cast<size_t>(id) >= doc->states.size()) {
		return nullptr;
	}
	uid_node_state_t *st = &doc->states[static_cast<size_t>(id)];
	const uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node) {
		return nullptr;
	}

	const unsigned propsVersion = node->properties.Version();
	const unsigned scaleEpoch = ScaleEpochOf(doc);
	const unsigned cvarEpoch = CvarEpochOf(backend);
	const unsigned interactionKey = InteractionKeyOf(st);
	const bool cvarOk = !st->computedStyle.dependsOnCvar || st->computedStyle.cvarEpoch == cvarEpoch;


	if (st->computedStyle.valid && st->computedStyle.propsVersion == propsVersion &&
	    st->computedStyle.scaleEpoch == scaleEpoch && cvarOk &&
	    st->computedStyle.interactionKey == interactionKey) {
		return &st->computedStyle;
	}

	RebuildComputedStyle(doc, id, backend, st);
	return st->computedStyle.valid ? &st->computedStyle : nullptr;
}
