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

#include "uid_layout.h"

#include "uid_binding.h"
#include "uid_collection.h"
#include "uid_modal.h"

#include "uid_opt.h"
#include "uid_value.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

uid_rect_t UID_ScrollbarChromeClip(const uid_node_def_t *container, const uid_node_state_t *st);

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
	bool out = fallback;
	if (node.properties.GetBoolCached(name, &out)) {
		return out;
	}
	const uid_prop_entry_t *b = UID_BuiltinDefaultParsed(name);
	if (!b) {
		return fallback;
	}
	if (UID_OptEnabled(UID_OPT_PARSE_CACHE) && (b->cacheValid & UID_PROP_CACHE_BOOL)) {
		return (b->cacheOk & UID_PROP_CACHE_BOOL) ? b->boolean : fallback;
	}
	if (!UID_ParseBool(b->value.c_str(), &b->boolean, nullptr)) {
		b->cacheValid |= UID_PROP_CACHE_BOOL;
		b->cacheOk &= ~UID_PROP_CACHE_BOOL;
		return fallback;
	}
	b->cacheValid |= UID_PROP_CACHE_BOOL;
	b->cacheOk |= UID_PROP_CACHE_BOOL;
	return b->boolean;
}

uid_length_t PropLength(const uid_node_def_t &node, const char *name, uid_length_unit_t fallbackUnit)
{
	uid_length_t len;
	len.unit = fallbackUnit;
	len.value = 0.0f;
	if (node.properties.GetLengthCached(name, &len)) {
		return len;
	}
	const uid_prop_entry_t *b = UID_BuiltinDefaultParsed(name);
	if (b) {
		if (UID_OptEnabled(UID_OPT_PARSE_CACHE) && (b->cacheValid & UID_PROP_CACHE_LENGTH)
			&& (b->cacheOk & UID_PROP_CACHE_LENGTH)) {
			return b->length;
		}
		if (UID_ParseLength(b->value.c_str(), &b->length, nullptr)) {
			b->cacheValid |= UID_PROP_CACHE_LENGTH;
			b->cacheOk |= UID_PROP_CACHE_LENGTH;
			return b->length;
		}
		b->cacheValid |= UID_PROP_CACHE_LENGTH;
		b->cacheOk &= ~UID_PROP_CACHE_LENGTH;
	}
	return len;
}

uid_sides_t PropSides(const uid_node_def_t &node, const char *name)
{
	uid_sides_t sides;
	std::memset(&sides, 0, sizeof(sides));
	const char *v = PropCStr(node, name, "0");
	if (!UID_ParseSides(v, &sides, nullptr)) {
		std::memset(&sides, 0, sizeof(sides));
	}
	return sides;
}

float ResolveSidePx(const uid_document_t *doc, const uid_length_t &len, float percentBase)
{
	if (len.unit == UID_LENGTH_PERCENT) {
		return percentBase * (len.value / 100.0f);
	}
	if (len.unit == UID_LENGTH_PX) {
		return UID_ScaleAuthoredPx(doc, len.value);
	}
	return 0.0f;
}

float ResolveLengthPx(
	const uid_document_t *doc,
	const uid_length_t &len,
	float percentBase,
	float fillSize,
	float autoSize
)
{
	switch (len.unit) {
	case UID_LENGTH_PX:
		return std::max(0.0f, UID_ScaleAuthoredPx(doc, len.value));
	case UID_LENGTH_PERCENT:
		return std::max(0.0f, percentBase * (len.value / 100.0f));
	case UID_LENGTH_FILL:
		return std::max(0.0f, fillSize);
	case UID_LENGTH_AUTO:
	default:
		return std::max(0.0f, autoSize);
	}
}

/* Added in Omaha: max-width / max-height clamp when prop is definite px/%. */
float ClampAxisToMax(
	const uid_document_t *doc,
	const uid_node_def_t &node,
	bool forWidth,
	float size,
	float percentBaseW,
	float percentBaseH
)
{
	const char *attr = forWidth ? "max-width" : "max-height";
	if (!node.properties.Has(attr)) {
		return size;
	}
	const uid_length_t maxLen = PropLength(node, attr, UID_LENGTH_AUTO);
	if (maxLen.unit != UID_LENGTH_PX && maxLen.unit != UID_LENGTH_PERCENT) {
		return size;
	}
	const float maxPx =
		ResolveLengthPx(doc, maxLen, forWidth ? percentBaseW : percentBaseH, 0.0f, 0.0f);
	return std::min(size, std::max(0.0f, maxPx));
}

bool NodeOverflowClipsOrScrolls(const uid_node_def_t &node)
{
	uid_overflow_t ov = UID_OVERFLOW_NONE;
	UID_ParseOverflow(PropCStr(node, "overflow", "none"), &ov, nullptr);
	return ov == UID_OVERFLOW_SCROLL || ov == UID_OVERFLOW_HIDDEN;
}

uid_rect_t MakeRect(float x, float y, float w, float h)
{
	uid_rect_t r;
	r.x = x;
	r.y = y;
	r.w = std::max(0.0f, w);
	r.h = std::max(0.0f, h);
	return r;
}

uid_rect_t IntersectRect(const uid_rect_t &a, const uid_rect_t &b)
{
	const float x0 = std::max(a.x, b.x);
	const float y0 = std::max(a.y, b.y);
	const float x1 = std::min(a.x + a.w, b.x + b.w);
	const float y1 = std::min(a.y + a.h, b.y + b.h);
	return MakeRect(x0, y0, x1 - x0, y1 - y0);
}

bool PointInRect(const uid_rect_t &r, float x, float y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
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

bool IsLayoutKind(uid_node_kind_t kind)
{
	switch (kind) {
	case UID_NODE_CONTAINER:
	case UID_NODE_LABEL:
	case UID_NODE_BUTTON:
	case UID_NODE_INPUT:
	case UID_NODE_TOGGLE:
	case UID_NODE_SLIDER:
	case UID_NODE_SLIDER_TRACK:
	case UID_NODE_SLIDER_RANGE:
	case UID_NODE_SLIDER_THUMB:
	case UID_NODE_SCROLLBAR:
	case UID_NODE_SCROLLBAR_TRACK:
	case UID_NODE_SCROLLBAR_THUMB:
	case UID_NODE_SELECT:
	case UID_NODE_KEYBIND:
	case UID_NODE_SHAPE_INSTANCE:
	case UID_NODE_IMAGE: /* Added in OPM: leaf bitmap */
	case UID_NODE_MODEL:
	case UID_NODE_SERVER_LIST:
	case UID_NODE_FOREACH:
		return true;
	default:
		return false;
	}
}

bool IsInteractiveKind(uid_node_kind_t kind)
{
	switch (kind) {
	case UID_NODE_BUTTON:
	case UID_NODE_INPUT:
	case UID_NODE_TOGGLE:
	case UID_NODE_SLIDER:
	case UID_NODE_SELECT:
	case UID_NODE_KEYBIND:
	case UID_NODE_SERVER_LIST: /* Added in OPM: host region receives pointer */
		return true;
	default:
		return false;
	}
}

/*
 * Added in OPM: leaf <image> aspect from measured texels (fallback 1).
 * Returns true when outAspect is usable (> 0).
 */
bool LeafImageAspect(
	uid_document_t *doc,
	const uid_node_def_t &node,
	const uid_backend_t *backend,
	float *outAspect
)
{
	if (!outAspect || node.kind != UID_NODE_IMAGE) {
		return false;
	}
	float texW = 32.0f;
	float texH = 32.0f;
	std::string imageId;
	if (!node.properties.Get("src", &imageId) || imageId.empty()) {
		(void)node.properties.Get("background-image", &imageId);
	}
	if (!imageId.empty() && imageId.find('{') == std::string::npos) {
		if (backend) {
			std::string resolved;
			if (UID_ResolvePropString(backend, imageId, &resolved) && !resolved.empty()) {
				imageId = resolved;
			}
		}
		std::string vfs;
		if (doc) {
			const auto iit = doc->definitions.images.find(imageId);
			if (iit != doc->definitions.images.end()) {
				vfs = iit->second.src;
			} else if (imageId.find('/') != std::string::npos && imageId.find("..") == std::string::npos &&
					   !imageId.empty() && imageId[0] != '/') {
				vfs = imageId;
			}
		}
		float mw = 0.0f;
		float mh = 0.0f;
		if (!vfs.empty() && backend && backend->imageMeasure && backend->imageMeasure(vfs.c_str(), &mw, &mh) &&
			mw > 0.0f && mh > 0.0f) {
			texW = mw;
			texH = mh;
		}
	}
	if (!(texH > 0.0f)) {
		return false;
	}
	*outAspect = texW / texH;
	return *outAspect > 0.0f;
}

float FallbackTextWidth(const char *text)
{
	if (!text) {
		return 0.0f;
	}
	float w = 0.0f;
	for (const char *p = text; *p; ++p) {
		w += 8.0f;
	}
	return w;
}

float MeasureText(
	const uid_document_t *doc,
	const uid_node_def_t &node,
	const char *text,
	float fbScale,
	const uid_backend_t *backend,
	uid_node_state_t *st = nullptr
)
{
	if (!text || !text[0]) {
		return 0.0f;
	}

	uid_length_t fontSize = PropLength(node, "font-size", UID_LENGTH_PX);
	float logicalPx = 12.0f;
	if (fontSize.unit == UID_LENGTH_PX) {
		logicalPx = fontSize.value > 0.0f ? fontSize.value : 12.0f;
	}
	logicalPx = UID_ScaleAuthoredPx(doc, logicalPx);

	const char *fontId = PropCStr(node, "font", "body");
	std::string src;
	int weight = 400;
	{
		double w = 400.0;
		std::string dm;
		const char *fw = PropCStr(node, "font-weight", "400");
		if (fw) {
			UID_ParseNumber(fw, &w, &dm);
		}
		weight = static_cast<int>(w);
	}
	const uid_font_def_t *fontDef = UID_FindFontDef(doc, fontId, weight);
	if (fontDef) {
		src = fontDef->src;
	}

	const float uiPxScale = (doc && doc->lastUiPxScale > 0.0f) ? doc->lastUiPxScale : 1.0f;
	const float useFb = fbScale > 0.0f ? fbScale : 1.0f;
	/* Added in OPM: FNV-1a key matching uid_widget HashTextCacheKey. */
	auto hashMeasureKey = [&]() -> uint64_t {
		uint64_t h = 14695981039346656037ull;
		auto mix = [&](const char *s) {
			if (!s) {
				return;
			}
			while (*s) {
				h ^= static_cast<unsigned char>(*s++);
				h *= 1099511628211ull;
			}
			h ^= 0xff;
			h *= 1099511628211ull;
		};
		mix(text);
		mix(fontId);
		h ^= static_cast<uint64_t>(weight);
		h *= 1099511628211ull;
		union {
			float f;
			uint32_t u;
		} a, b, c;
		a.f = logicalPx;
		b.f = uiPxScale;
		c.f = useFb;
		h ^= a.u;
		h *= 1099511628211ull;
		h ^= b.u;
		h *= 1099511628211ull;
		h ^= c.u;
		h *= 1099511628211ull;
		return h;
	};

	if (UID_OptEnabled(UID_OPT_TEXT_CACHE) && st && st->cachedTextWidth >= 0.0f) {
		const uint64_t key = hashMeasureKey();
		if (st->cachedMeasureKey == key) {
			return st->cachedTextWidth;
		}
	}

	if (backend && backend->fontResolve && backend->fontMeasure && !src.empty()) {
		void *font = backend->fontResolve(src.c_str(), logicalPx, useFb);
		if (UID_OptEnabled(UID_OPT_TEXT_CACHE) && st && font) {
			st->cachedFont = font;
		}
		if (font) {
			float w = backend->fontMeasure(font, text);
			const char *trackProp = PropCStr(node, "letter-spacing", nullptr);
			if (trackProp && trackProp[0] && text[0]) {
				uid_length_t track;
				track.unit = UID_LENGTH_PX;
				track.value = 0.0f;
				if (UID_ParseLength(trackProp, &track, nullptr) && track.unit == UID_LENGTH_PX) {
					const int n = static_cast<int>(std::strlen(text));
					if (n > 1) {
						w += UID_ScaleAuthoredPx(doc, track.value) * static_cast<float>(n - 1);
					}
				}
			}
			if (w > 0.0f) {
				if (UID_OptEnabled(UID_OPT_TEXT_CACHE) && st) {
					st->cachedMeasureKey = hashMeasureKey();
					st->cachedTextWidth = w;
				}
				return w;
			}
		}
	}
	return FallbackTextWidth(text);
}

static bool EqIgnoreCase(const char *a, const char *b)
{
	if (!a || !b) {
		return false;
	}
	while (*a && *b) {
		const unsigned char ca = static_cast<unsigned char>(*a++);
		const unsigned char cb = static_cast<unsigned char>(*b++);
		if (std::tolower(ca) != std::tolower(cb)) {
			return false;
		}
	}
	return *a == '\0' && *b == '\0';
}

static uid_text_wrap_t TextWrapMode(const uid_node_def_t &node)
{
	const char *wrap = PropCStr(node, "text-wrap", "none");
	if (wrap && EqIgnoreCase(wrap, "word")) {
		return UID_TEXT_WRAP_WORD;
	}
	return UID_TEXT_WRAP_NONE;
}

static float TextLineHeight(
	const uid_document_t *doc,
	const uid_node_def_t &node,
	float fbScale,
	const uid_backend_t *backend
)
{
	uid_length_t fontSize = PropLength(node, "font-size", UID_LENGTH_PX);
	float logicalPx = (fontSize.unit == UID_LENGTH_PX && fontSize.value > 0.0f) ? fontSize.value : 12.0f;
	logicalPx = UID_ScaleAuthoredPx(doc, logicalPx);

	const char *lineHeightProp = PropCStr(node, "line-height", nullptr);
	if (lineHeightProp && lineHeightProp[0]) {
		uid_length_t lh;
		lh.unit = UID_LENGTH_PX;
		lh.value = 0.0f;
		if (UID_ParseLength(lineHeightProp, &lh, nullptr) && lh.unit == UID_LENGTH_PX && lh.value > 0.0f) {
			return UID_ScaleAuthoredPx(doc, lh.value);
		}
		double mult = 0.0;
		if (UID_ParseNumber(lineHeightProp, &mult, nullptr) && mult > 0.0) {
			return logicalPx * static_cast<float>(mult);
		}
	}

	void *font = nullptr;
	const char *fontId = PropCStr(node, "font", "body");
	int weight = 400;
	{
		double w = 400.0;
		std::string dm;
		const char *fw = PropCStr(node, "font-weight", "400");
		if (fw) {
			UID_ParseNumber(fw, &w, &dm);
		}
		weight = static_cast<int>(w);
	}
	const uid_font_def_t *fontDef = UID_FindFontDef(doc, fontId, weight);
	if (fontDef && backend && backend->fontResolve) {
		font = backend->fontResolve(fontDef->src.c_str(), logicalPx, fbScale > 0.0f ? fbScale : 1.0f);
	}
	if (font && backend && backend->fontAscent) {
		const float ascent = backend->fontAscent(font);
		if (ascent > 0.0f) {
			return ascent * 1.4f;
		}
	}
	return logicalPx * 1.4f;
}

static void BuildTextLines(
	const uid_document_t *doc,
	const uid_node_def_t &node,
	const char *text,
	float maxWrapWidth,
	float fbScale,
	const uid_backend_t *backend,
	std::vector<std::string> *lines,
	uid_text_block_metrics_t *metrics
)
{
	if (!metrics) {
		return;
	}
	metrics->lineHeight = TextLineHeight(doc, node, fbScale, backend);
	metrics->blockWidth = 0.0f;
	metrics->blockHeight = 0.0f;
	metrics->lineCount = 0;
	if (!lines) {
		return;
	}
	lines->clear();
	if (!text || !text[0]) {
		return;
	}

	const uid_text_wrap_t wrap = TextWrapMode(node);
	const bool doWrap = (wrap == UID_TEXT_WRAP_WORD && maxWrapWidth > 0.0f);

	std::string segment;
	for (const char *p = text; *p; ++p) {
		if (*p == '\n') {
			if (!segment.empty() || !lines->empty()) {
				lines->push_back(segment);
				segment.clear();
			}
		} else {
			segment.push_back(*p);
		}
	}
	if (!segment.empty() || (text[0] == '\n')) {
		lines->push_back(segment);
	}
	if (lines->empty()) {
		lines->push_back(std::string());
	}

	if (!doWrap) {
		for (const std::string &line : *lines) {
			const float w = MeasureText(doc, node, line.c_str(), fbScale, backend);
			metrics->blockWidth = std::max(metrics->blockWidth, w);
		}
		metrics->lineCount = static_cast<int>(lines->size());
		metrics->blockHeight = metrics->lineHeight * static_cast<float>(metrics->lineCount);
		return;
	}

	std::vector<std::string> wrapped;
	for (const std::string &logicalLine : *lines) {
		if (logicalLine.empty()) {
			wrapped.push_back(std::string());
			continue;
		}
		size_t start = 0;
		while (start < logicalLine.size()) {
			size_t end = logicalLine.size();
			std::string candidate = logicalLine.substr(start, end - start);
			float w = MeasureText(doc, node, candidate.c_str(), fbScale, backend);
			while (w > maxWrapWidth && end > start + 1) {
				--end;
				while (end > start && logicalLine[end] != ' ') {
					--end;
				}
				if (end == start) {
					end = logicalLine.size();
					while (end > start + 1) {
						--end;
						candidate = logicalLine.substr(start, end - start);
						w = MeasureText(doc, node, candidate.c_str(), fbScale, backend);
						if (w <= maxWrapWidth) {
							break;
						}
					}
					if (w > maxWrapWidth) {
						end = start + 1;
					}
					break;
				}
				candidate = logicalLine.substr(start, end - start);
				w = MeasureText(doc, node, candidate.c_str(), fbScale, backend);
			}
			while (end < logicalLine.size() && logicalLine[end] == ' ') {
				++end;
			}
			wrapped.push_back(logicalLine.substr(start, end - start));
			start = end;
		}
	}
	*lines = wrapped;
	for (const std::string &line : *lines) {
		const float w = MeasureText(doc, node, line.c_str(), fbScale, backend);
		metrics->blockWidth = std::max(metrics->blockWidth, w);
	}
	metrics->lineCount = static_cast<int>(lines->size());
	metrics->blockHeight = metrics->lineHeight * static_cast<float>(metrics->lineCount);
}

std::string ControlText(const uid_document_t *doc, uid_node_id_t id)
{
	const uid_node_def_t *node = UID_GetNode(doc, id);
	const uid_node_state_t *st = StateC(doc, id);
	if (!node) {
		return std::string();
	}
	if (node->kind == UID_NODE_BUTTON) {
		return node->text;
	}
	if (st && st->runtimeValue.hasValue) {
		return st->runtimeValue.stringValue;
	}
	if (st && !st->editBuffer.empty()) {
		return st->editBuffer;
	}
	if (!node->text.empty()) {
		return node->text;
	}
	if (node->kind == UID_NODE_SELECT && !node->options.empty()) {
		return node->options.front().label.empty() ? node->options.front().value : node->options.front().label;
	}
	if (node->kind == UID_NODE_KEYBIND) {
		return node->binding;
	}
	return std::string();
}

float IntrinsicStrokePad(const uid_document_t *doc, const uid_node_def_t &node);

float IntrinsicBorderSize(
	uid_document_t *doc,
	uid_node_id_t id,
	bool forWidth,
	float percentBaseW,
	float percentBaseH,
	float fbScale,
	const uid_backend_t *backend
);

void LayoutNode(
	uid_document_t *doc,
	uid_node_id_t id,
	float marginX,
	float marginY,
	float marginW,
	float marginH,
	float percentBaseW,
	float percentBaseH,
	const uid_rect_t &parentClip,
	bool ancestorVisible,
	bool ancestorEnabled,
	float fbScale,
	const uid_backend_t *backend,
	uid_diag_list_t *diags
);

/* Added in OPM: find role=relative-panel under modal root. */
uid_node_id_t FindRelativePanel(const uid_document_t *doc, uid_node_id_t id)
{
	const uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node) {
		return UID_INVALID_NODE_ID;
	}
	if (node->role == "relative-panel") {
		return id;
	}
	for (uid_node_id_t c : node->children) {
		const uid_node_id_t found = FindRelativePanel(doc, c);
		if (found != UID_INVALID_NODE_ID) {
			return found;
		}
	}
	return UID_INVALID_NODE_ID;
}

/* Added in OPM: place type=relative modal panel against modalOpenerNode. */
void PlaceRelativeModalPanel(
	uid_document_t *doc,
	float fbScale,
	const uid_backend_t *backend,
	uid_diag_list_t *diags
)
{
	if (!doc || !UID_IsModalActive(doc)) {
		return;
	}
	auto it = doc->definitions.modals.find(doc->activeModalId);
	if (it == doc->definitions.modals.end() || it->second.type != "relative") {
		return;
	}
	const uid_node_id_t openerId = doc->modalOpenerNode;
	const uid_node_id_t modalRoot = UID_GetModalRoot(doc);
	if (openerId == UID_INVALID_NODE_ID || modalRoot == UID_INVALID_NODE_ID) {
		return;
	}
	if (openerId < 0 || static_cast<size_t>(openerId) >= doc->states.size()) {
		return;
	}
	const uid_node_state_t &openerSt = doc->states[static_cast<size_t>(openerId)];
	if (openerSt.borderBox.w <= 0.0f || openerSt.borderBox.h <= 0.0f) {
		return;
	}

	const uid_node_id_t panelId = FindRelativePanel(doc, modalRoot);
	if (panelId == UID_INVALID_NODE_ID) {
		return;
	}

	const float margin = UID_ScaleAuthoredPx(doc, 4.0f);
	const float vpW = std::max(0.0f, static_cast<float>(doc->lastLogicalW) - margin * 2.0f);
	const float vpH = std::max(0.0f, static_cast<float>(doc->lastLogicalH) - margin * 2.0f);
	const uid_rect_t viewport = MakeRect(margin, margin, vpW, vpH);
	const uid_rect_t canvasClip = MakeRect(0.0f, 0.0f, static_cast<float>(doc->lastLogicalW), static_cast<float>(doc->lastLogicalH));

	const float panelW = openerSt.borderBox.w;
	uid_node_state_t *pst = State(doc, panelId);
	if (!pst) {
		return;
	}
	/*
	 * Fixed in OPM: the measure pass uses height=auto so content fills the box and
	 * LayoutChildren clamps scrollY to 0. Capture scroll before that wipe.
	 */
	const float savedScrollY = pst->scrollY;

	/* Measure intrinsic height at opener width. */
	LayoutNode(
		doc,
		panelId,
		0.0f,
		0.0f,
		panelW,
		vpH,
		panelW,
		vpH,
		canvasClip,
		true,
		true,
		fbScale,
		backend,
		diags
	);
	pst = State(doc, panelId);
	if (!pst) {
		return;
	}
	const float contentH = std::max(pst->contentExtentH, pst->borderBox.h);
	/* Soft max ~5 rows of 28px authored. */
	const float maxPanelH = UID_ScaleAuthoredPx(doc, 28.0f) * 5.0f;
	const uid_overlay_placement_t placement = UID_PlaceOverlayInViewport(
		viewport,
		openerSt.borderBox,
		panelW,
		contentH,
		savedScrollY,
		0.0f,
		maxPanelH
	);

	/*
	 * Fixed in OPM: height=auto ignores the placed clamp for scroll purposes unless we
	 * temporarily force an explicit px height for this layout pass.
	 */
	uid_node_def_t *panelNode = UID_GetNode(doc, panelId);
	std::string prevHeight = "auto";
	if (panelNode) {
		prevHeight = PropCStr(*panelNode, "height", "auto");
		const float uiPx = (doc->lastUiPxScale > 0.0f) ? doc->lastUiPxScale : 1.0f;
		char heightBuf[64];
		std::snprintf(
			heightBuf,
			sizeof(heightBuf),
			"%gpx",
			static_cast<double>(placement.panel.h / uiPx)
		);
		panelNode->properties.Set("height", heightBuf);
	}

	/* Restore scroll before the clamped layout so children shift correctly. */
	pst->scrollY = placement.scrollY;

	LayoutNode(
		doc,
		panelId,
		placement.panel.x,
		placement.panel.y,
		placement.panel.w,
		placement.panel.h,
		placement.panel.w,
		placement.panel.h,
		canvasClip,
		true,
		true,
		fbScale,
		backend,
		diags
	);

	if (panelNode) {
		panelNode->properties.Set("height", prevHeight.c_str());
	}

	pst = State(doc, panelId);
	if (!pst) {
		return;
	}
	/* Keep measured content extent so wheel scroll sees overflow after the clamp. */
	if (pst->contentExtentH < contentH) {
		pst->contentExtentH = contentH;
	}
	const float maxScroll = std::max(0.0f, pst->contentExtentH - pst->contentBox.h);
	pst->scrollY = std::min(maxScroll, std::max(0.0f, placement.scrollY));
}

void ClearNodeGeometry(uid_node_state_t *st)
{
	if (!st) {
		return;
	}
	std::memset(&st->marginBox, 0, sizeof(st->marginBox));
	std::memset(&st->borderBox, 0, sizeof(st->borderBox));
	std::memset(&st->contentBox, 0, sizeof(st->contentBox));
	std::memset(&st->effectiveClip, 0, sizeof(st->effectiveClip));
	st->contentExtentW = 0.0f;
	st->contentExtentH = 0.0f;
}

float IntrinsicBorderSize(
	uid_document_t *doc,
	uid_node_id_t id,
	bool forWidth,
	float percentBaseW,
	float percentBaseH,
	float fbScale,
	const uid_backend_t *backend
);

/*
 * Added in OPM: flex child sum/max along main/cross axis (no padding).
 * Shared by containers and buttons with nested layout children.
 */
float IntrinsicFlexChildrenContentSize(
	uid_document_t *doc,
	const uid_node_def_t &node,
	bool forWidth,
	float percentBaseW,
	float percentBaseH,
	float fbScale,
	const uid_backend_t *backend
)
{
	uid_layout_axis_t axis = UID_AXIS_VERTICAL;
	UID_ParseAxis(PropCStr(node, "type", "vertical"), &axis, nullptr);

	std::vector<uid_node_id_t> kids;
	for (uid_node_id_t c : node.children) {
		const uid_node_def_t *cn = UID_GetNode(doc, c);
		if (!cn || cn->kind == UID_NODE_SCROLLBAR || !IsLayoutKind(cn->kind) || !PropBool(*cn, "visible", true)) {
			continue;
		}
		kids.push_back(c);
	}

	if (axis == UID_AXIS_OVERLAP) {
		float maxSz = 0.0f;
		for (uid_node_id_t c : kids) {
			const uid_node_def_t *cn = UID_GetNode(doc, c);
			const uid_sides_t margin = PropSides(*cn, "margin");
			const float m0 = forWidth ? (ResolveSidePx(doc, margin.left, percentBaseW) + ResolveSidePx(doc, margin.right, percentBaseW))
			                          : (ResolveSidePx(doc, margin.top, percentBaseH) + ResolveSidePx(doc, margin.bottom, percentBaseH));
			maxSz = std::max(
				maxSz,
				m0 + IntrinsicBorderSize(doc, c, forWidth, percentBaseW, percentBaseH, fbScale, backend)
			);
		}
		return maxSz;
	}
	const bool horiz = (axis == UID_AXIS_HORIZONTAL);

	float gap = 0.0f;
	uid_length_t gapLen;
	if (UID_ParseLength(PropCStr(node, "gap", "0"), &gapLen, nullptr)) {
		gap = ResolveSidePx(doc, gapLen, horiz ? percentBaseW : percentBaseH);
	}

	if (kids.empty()) {
		return 0.0f;
	}

	if (forWidth == horiz) {
		float sum = 0.0f;
		for (size_t i = 0; i < kids.size(); ++i) {
			const uid_node_def_t *cn = UID_GetNode(doc, kids[i]);
			const uid_sides_t margin = PropSides(*cn, "margin");
			const float m0 = horiz ? ResolveSidePx(doc, margin.left, percentBaseW) : ResolveSidePx(doc, margin.top, percentBaseH);
			const float m1 = horiz ? ResolveSidePx(doc, margin.right, percentBaseW) : ResolveSidePx(doc, margin.bottom, percentBaseH);
			sum += m0 + m1 + IntrinsicBorderSize(doc, kids[i], forWidth, percentBaseW, percentBaseH, fbScale, backend);
			if (i + 1 < kids.size()) {
				sum += gap;
			}
		}
		return sum;
	}

	float maxC = 0.0f;
	for (uid_node_id_t c : kids) {
		const uid_node_def_t *cn = UID_GetNode(doc, c);
		const uid_sides_t margin = PropSides(*cn, "margin");
		const float m0 = forWidth ? ResolveSidePx(doc, margin.left, percentBaseW) : ResolveSidePx(doc, margin.top, percentBaseH);
		const float m1 = forWidth ? ResolveSidePx(doc, margin.right, percentBaseW) : ResolveSidePx(doc, margin.bottom, percentBaseH);
		maxC = std::max(maxC, m0 + m1 + IntrinsicBorderSize(doc, c, forWidth, percentBaseW, percentBaseH, fbScale, backend));
	}
	return maxC;
}

void ApplySelfBoxes(
	uid_document_t *doc,
	uid_node_id_t id,
	float borderX,
	float borderY,
	float borderW,
	float borderH,
	float percentBaseW,
	float percentBaseH,
	const uid_rect_t &parentClip
)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st) {
		return;
	}

	const uid_sides_t margin = PropSides(*node, "margin");
	const float ml = ResolveSidePx(doc, margin.left, percentBaseW);
	const float mr = ResolveSidePx(doc, margin.right, percentBaseW);
	const float mt = ResolveSidePx(doc, margin.top, percentBaseH);
	const float mb = ResolveSidePx(doc, margin.bottom, percentBaseH);

	/* Added in OPM: post-flow translate (does not affect sibling packing). */
	{
		const uid_length_t txLen = PropLength(*node, "translate-x", UID_LENGTH_PX);
		const uid_length_t tyLen = PropLength(*node, "translate-y", UID_LENGTH_PX);
		borderX += ResolveSidePx(doc, txLen, percentBaseW);
		borderY += ResolveSidePx(doc, tyLen, percentBaseH);
	}

	st->borderBox = MakeRect(borderX, borderY, borderW, borderH);
	st->marginBox = MakeRect(borderX - ml, borderY - mt, borderW + ml + mr, borderH + mt + mb);

	const uid_sides_t padding = PropSides(*node, "padding");
	float pl = ResolveSidePx(doc, padding.left, percentBaseW);
	float pr = ResolveSidePx(doc, padding.right, percentBaseW);
	float pt = ResolveSidePx(doc, padding.top, percentBaseH);
	float pb = ResolveSidePx(doc, padding.bottom, percentBaseH);

	/* Added in OPM: authored size includes stroke when stroke-layout (default true). */
	{
		const float strokePad = IntrinsicStrokePad(doc, *node);
		if (strokePad > 0.0f) {
			const float half = strokePad * 0.5f;
			pl += half;
			pr += half;
			pt += half;
			pb += half;
		}
	}

	st->contentBox = MakeRect(borderX + pl, borderY + pt, borderW - pl - pr, borderH - pt - pb);
	if (st->contentBox.w < 0.0f) {
		st->contentBox.w = 0.0f;
	}
	if (st->contentBox.h < 0.0f) {
		st->contentBox.h = 0.0f;
	}

	uid_overflow_t overflow = UID_OVERFLOW_NONE;
	{
		int ovEnum = static_cast<int>(UID_OVERFLOW_NONE);
		if (node->properties.GetEnumCached("overflow", UID_PROP_ENUM_OVERFLOW, &ovEnum)) {
			overflow = static_cast<uid_overflow_t>(ovEnum);
		} else {
			UID_ParseOverflow(PropCStr(*node, "overflow", "none"), &overflow, nullptr);
		}
	}
	if (overflow == UID_OVERFLOW_HIDDEN || overflow == UID_OVERFLOW_SCROLL) {
		st->effectiveClip = IntersectRect(parentClip, st->contentBox);
	} else {
		st->effectiveClip = parentClip;
	}

	st->contentExtentW = st->contentBox.w;
	st->contentExtentH = st->contentBox.h;
}

/* Added in OPM: stroke width on both sides when stroke-layout is true (default). */
float IntrinsicStrokePad(const uid_document_t *doc, const uid_node_def_t &node)
{
	/* Changed in OPM: stroke-layout=false keeps stroke paint-only (no layout pad). */
	if (!PropBool(node, "stroke-layout", true)) {
		return 0.0f;
	}
	const char *strokeStr = PropCStr(node, "stroke", nullptr);
	if (!strokeStr || !strokeStr[0]) {
		return 0.0f;
	}
	uid_color_t strokeCol{};
	if (!UID_ParseColor(strokeStr, &strokeCol, nullptr) || strokeCol.a <= 0.0f) {
		return 0.0f;
	}
	const char *wStr = PropCStr(node, "stroke-width", "1px");
	uid_length_t wLen{};
	if (!UID_ParseLength(wStr, &wLen, nullptr) || wLen.unit != UID_LENGTH_PX || wLen.value <= 0.0f) {
		return 0.0f;
	}
	const float sw = UID_ScaleAuthoredPx(doc, wLen.value);
	return sw + sw;
}

bool ContainerMainAxisIsHorizontal(const uid_node_def_t &node)
{
	uid_layout_axis_t axis = UID_AXIS_VERTICAL;
	UID_ParseAxis(PropCStr(node, "type", "vertical"), &axis, nullptr);
	return axis == UID_AXIS_HORIZONTAL;
}

/* Added in OPM: direct child with fill on this container's flex main axis. */
bool ContainerHasFillOnMainAxis(uid_document_t *doc, const uid_node_def_t &node)
{
	if (node.kind != UID_NODE_CONTAINER && node.kind != UID_NODE_FOREACH) {
		return false;
	}
	uid_layout_axis_t axis = UID_AXIS_VERTICAL;
	UID_ParseAxis(PropCStr(node, "type", "vertical"), &axis, nullptr);
	if (axis == UID_AXIS_OVERLAP) {
		return false;
	}
	const bool horiz = (axis == UID_AXIS_HORIZONTAL);
	for (uid_node_id_t c : node.children) {
		const uid_node_def_t *cn = UID_GetNode(doc, c);
		if (!cn || cn->kind == UID_NODE_SCROLLBAR || !IsLayoutKind(cn->kind) || !PropBool(*cn, "visible", true)) {
			continue;
		}
		const uid_length_t mainLen = PropLength(*cn, horiz ? "width" : "height", UID_LENGTH_AUTO);
		if (mainLen.unit == UID_LENGTH_FILL) {
			return true;
		}
	}
	return false;
}

float IntrinsicBorderSize(
	uid_document_t *doc,
	uid_node_id_t id,
	bool forWidth,
	float percentBaseW,
	float percentBaseH,
	float fbScale,
	const uid_backend_t *backend
)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node) {
		return 0.0f;
	}

	float size = 0.0f;
	const uid_length_t dim = PropLength(*node, forWidth ? "width" : "height", UID_LENGTH_AUTO);
	if (dim.unit == UID_LENGTH_PX || dim.unit == UID_LENGTH_PERCENT) {
		size = ResolveLengthPx(doc, dim, forWidth ? percentBaseW : percentBaseH, 0.0f, 0.0f);
		return ClampAxisToMax(doc, *node, forWidth, size, percentBaseW, percentBaseH);
	}
	if (dim.unit == UID_LENGTH_FILL) {
		return 0.0f;
	}

	const uid_sides_t padding = PropSides(*node, "padding");
	const float padMain = forWidth
		? (ResolveSidePx(doc, padding.left, percentBaseW) + ResolveSidePx(doc, padding.right, percentBaseW))
		: (ResolveSidePx(doc, padding.top, percentBaseH) + ResolveSidePx(doc, padding.bottom, percentBaseH));

	if (node->kind == UID_NODE_CONTAINER || node->kind == UID_NODE_FOREACH) {
		float content = 0.0f;
		/*
		 * Added in Omaha: windowed foreach under overflow=scroll — preferred height is
		 * full collection extent (count * row-height), not the expanded window only.
		 */
		if (!forWidth) {
			const float synthetic = UID_WindowedForeachSyntheticExtentH(doc, id);
			if (synthetic >= 0.0f) {
				content = synthetic;
			} else {
				content = IntrinsicFlexChildrenContentSize(
					doc, *node, forWidth, percentBaseW, percentBaseH, fbScale, backend
				);
			}
		} else {
			content = IntrinsicFlexChildrenContentSize(
				doc, *node, forWidth, percentBaseW, percentBaseH, fbScale, backend
			);
		}
		size = content + padMain + IntrinsicStrokePad(doc, *node);
		return ClampAxisToMax(doc, *node, forWidth, size, percentBaseW, percentBaseH);
	}

	if (node->kind == UID_NODE_SHAPE_INSTANCE) {
		auto sit = doc->definitions.shapes.find(node->shapeId);
		if (sit != doc->definitions.shapes.end() && sit->second.hasIntrinsicSize) {
			size = forWidth ? sit->second.width : sit->second.height;
		} else {
			size = 32.0f;
		}
		return ClampAxisToMax(doc, *node, forWidth, size, percentBaseW, percentBaseH);
	}

	/* Added in OPM: leaf <image> — natural DIP size from texel measure.
	 * Fixed in OPM: when one axis is fixed and the other is auto, derive the
	 * auto axis from texel aspect (same rule as the place pass). Parents that
	 * size from IntrinsicBorderSize (killfeed foreach/overlap) must not lock to
	 * full natural tex width while height="20px" width="auto". */
	if (node->kind == UID_NODE_IMAGE) {
		float texW = 32.0f;
		float texH = 32.0f;
		std::string imageId;
		if (!node->properties.Get("src", &imageId) || imageId.empty()) {
			(void)node->properties.Get("background-image", &imageId);
		}
		if (!imageId.empty() && imageId.find('{') == std::string::npos) {
			if (backend && backend->cvarDescribe) {
				std::string resolved;
				if (UID_ResolvePropString(backend, imageId, &resolved) && !resolved.empty()) {
					imageId = resolved;
				}
			}
			std::string vfs;
			const auto iit = doc->definitions.images.find(imageId);
			if (iit != doc->definitions.images.end()) {
				vfs = iit->second.src;
			} else if (imageId.find('/') != std::string::npos && imageId.find("..") == std::string::npos &&
					   !imageId.empty() && imageId[0] != '/') {
				vfs = imageId;
			}
			float mw = 0.0f;
			float mh = 0.0f;
			if (!vfs.empty() && backend && backend->imageMeasure && backend->imageMeasure(vfs.c_str(), &mw, &mh) &&
				mw > 0.0f && mh > 0.0f) {
				texW = mw;
				texH = mh;
			}
		}
		const float aspect = (texH > 0.0f) ? (texW / texH) : 1.0f;
		const uid_length_t otherLen = PropLength(*node, forWidth ? "height" : "width", UID_LENGTH_AUTO);
		if (otherLen.unit == UID_LENGTH_PX || otherLen.unit == UID_LENGTH_PERCENT) {
			const float otherPx = ResolveLengthPx(
				doc, otherLen, forWidth ? percentBaseH : percentBaseW, 0.0f, 0.0f
			);
			if (aspect > 0.0f) {
				const float derived = forWidth ? (otherPx * aspect) : (otherPx / aspect);
				size = derived + padMain;
				return ClampAxisToMax(doc, *node, forWidth, size, percentBaseW, percentBaseH);
			}
		}
		size = (forWidth ? UID_ScaleAuthoredPx(doc, texW) : UID_ScaleAuthoredPx(doc, texH)) + padMain;
		return ClampAxisToMax(doc, *node, forWidth, size, percentBaseW, percentBaseH);
	}

	/* Added in OPM: leaf controls sized by width/height attrs; fallback like empty box */
	if (node->kind == UID_NODE_MODEL || node->kind == UID_NODE_SERVER_LIST) {
		return ClampAxisToMax(doc, *node, forWidth, 32.0f, percentBaseW, percentBaseH);
	}

	const std::string text = ControlText(doc, id);
	const uid_text_wrap_t wrap = TextWrapMode(*node);
	float wrapMaxW = 0.0f;
	const uid_length_t widthLen = PropLength(*node, "width", UID_LENGTH_AUTO);
	const uid_sides_t padSides = PropSides(*node, "padding");
	const float padW =
		ResolveSidePx(doc, padSides.left, percentBaseW) + ResolveSidePx(doc, padSides.right, percentBaseW);
	if (widthLen.unit == UID_LENGTH_PX || widthLen.unit == UID_LENGTH_PERCENT) {
		wrapMaxW = ResolveLengthPx(doc, widthLen, percentBaseW, 0.0f, 0.0f) - padW;
		if (wrapMaxW < 0.0f) {
			wrapMaxW = 0.0f;
		}
	}

	float contentW = 0.0f;
	float contentH = 0.0f;
	if (wrap == UID_TEXT_WRAP_WORD && wrapMaxW > 0.0f && !forWidth) {
		uid_text_block_metrics_t metrics{};
		std::vector<std::string> lines;
		BuildTextLines(doc, *node, text.c_str(), wrapMaxW, fbScale, backend, &lines, &metrics);
		contentW = metrics.blockWidth;
		contentH = metrics.blockHeight;
	} else if (forWidth && wrap == UID_TEXT_WRAP_WORD && wrapMaxW > 0.0f) {
		contentW = wrapMaxW;
		contentH = TextLineHeight(doc, *node, fbScale, backend);
	} else {
		contentW = MeasureText(doc, *node, text.c_str(), fbScale, backend, State(doc, id));
		contentH = TextLineHeight(doc, *node, fbScale, backend);
	}
	if (node->kind == UID_NODE_SLIDER) {
		contentW = std::max(contentW, 80.0f);
		contentH = std::max(contentH, 16.0f);
	} else if (node->kind == UID_NODE_TOGGLE) {
		contentW = std::max(contentW, 28.0f);
		contentH = std::max(contentH, 16.0f);
	} else if (node->kind == UID_NODE_INPUT) {
		contentW = std::max(contentW, 64.0f);
	} else if (node->kind == UID_NODE_BUTTON || node->kind == UID_NODE_SELECT || node->kind == UID_NODE_KEYBIND) {
		contentW = std::max(contentW, 24.0f);
	}
	/* Added in OPM: icon/shape children contribute to auto width/height alongside text. */
	if (node->kind == UID_NODE_BUTTON && !node->children.empty()) {
		contentW = std::max(
			contentW,
			IntrinsicFlexChildrenContentSize(doc, *node, true, percentBaseW, percentBaseH, fbScale, backend)
		);
		contentH = std::max(
			contentH,
			IntrinsicFlexChildrenContentSize(doc, *node, false, percentBaseW, percentBaseH, fbScale, backend)
		);
	}
	/* Added in OPM: cyclic select body = value + ticks + padding (~HTML 2.15rem). */
	if (node->kind == UID_NODE_SELECT && node->appearance == "cyclic") {
		contentW = std::max(contentW, UID_ScaleAuthoredPx(doc, 240.0f));
		const float padY = UID_ScaleAuthoredPx(doc, 6.0f);
		const float gap = UID_ScaleAuthoredPx(doc, 4.0f);
		const float tickRowH = UID_ScaleAuthoredPx(doc, 5.0f);
		contentH = padY + contentH + gap + tickRowH + padY;
	}

	return ClampAxisToMax(
		doc, *node, forWidth, (forWidth ? contentW : contentH) + padMain, percentBaseW, percentBaseH
	);
}

void LayoutChildren(
	uid_document_t *doc,
	uid_node_id_t parentId,
	bool parentEnabled,
	float fbScale,
	const uid_backend_t *backend,
	uid_diag_list_t *diags
);

void LayoutOverlapChildren(
	uid_document_t *doc,
	uid_node_id_t parentId,
	bool parentEnabled,
	float fbScale,
	const uid_backend_t *backend,
	uid_diag_list_t *diags
);

void HideSubtree(uid_document_t *doc, uid_node_id_t id)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	ClearNodeGeometry(st);
	if (!node) {
		return;
	}
	for (uid_node_id_t c : node->children) {
		HideSubtree(doc, c);
	}
}

float SliderNormalizedT(const uid_document_t *doc, uid_node_id_t sliderId)
{
	const uid_node_def_t *node = UID_GetNode(doc, sliderId);
	const uid_node_state_t *st = StateC(doc, sliderId);
	if (!node || !st) {
		return 0.0f;
	}
	double minV = node->hasMin ? node->minValue : 0.0;
	double maxV = node->hasMax ? node->maxValue : 1.0;
	double val = minV;
	if (st->runtimeValue.hasValue) {
		UID_ParseNumber(st->runtimeValue.stringValue.c_str(), &val, nullptr);
	}
	if (maxV <= minV) {
		return 0.0f;
	}
	const float t = static_cast<float>((val - minV) / (maxV - minV));
	return std::min(1.0f, std::max(0.0f, t));
}

float PartLengthPx(
	const uid_document_t *doc,
	const uid_node_def_t &part,
	const char *attr,
	float percentBase,
	float fallback
)
{
	uid_length_t len = PropLength(part, attr, UID_LENGTH_AUTO);
	if (len.unit == UID_LENGTH_AUTO || len.unit == UID_LENGTH_FILL) {
		return fallback;
	}
	return ResolveLengthPx(doc, len, percentBase, percentBase, fallback);
}

/*
 * Added in OPM: browser-style scrollbar thumb length along the scroll axis.
 * thumbAlong = clamp(trackAlong * (viewport / content), minPx, trackAlong).
 * Min is 20 authored px (scaled); max is the track. Never exceeds trackAlong.
 */
float ScrollbarThumbAlongPx(
	const uid_document_t *doc,
	float trackAlong,
	float viewportAlong,
	float contentAlong
)
{
	if (trackAlong <= 0.0f) {
		return 0.0f;
	}
	const float ratio = viewportAlong / std::max(contentAlong, 1.0f);
	const float raw = trackAlong * ratio;
	const float minPx = std::min(UID_ScaleAuthoredPx(doc, 20.0f), trackAlong);
	return std::min(trackAlong, std::max(minPx, raw));
}

void PlaceSliderPart(
	uid_document_t *doc,
	uid_node_id_t partId,
	float x,
	float y,
	float w,
	float h,
	const uid_rect_t &clip,
	bool enabled
)
{
	uid_node_state_t *st = State(doc, partId);
	if (!st) {
		return;
	}
	st->effectivelyEnabled = enabled;
	ApplySelfBoxes(doc, partId, x, y, w, h, w, h, clip);
}

/*
 * Added in OPM: position track/range/thumb inside the slider content box.
 * Not flex — value drives range width and thumb X.
 */
void LayoutSliderParts(uid_document_t *doc, uid_node_id_t sliderId, bool parentEnabled)
{
	uid_node_def_t *slider = UID_GetNode(doc, sliderId);
	uid_node_state_t *sst = State(doc, sliderId);
	if (!slider || !sst) {
		return;
	}

	const uid_node_id_t trackId = UID_FindChildOfKind(doc, sliderId, UID_NODE_SLIDER_TRACK);
	const uid_node_id_t rangeId = UID_FindChildOfKind(doc, sliderId, UID_NODE_SLIDER_RANGE);
	const uid_node_id_t thumbId = UID_FindChildOfKind(doc, sliderId, UID_NODE_SLIDER_THUMB);
	if (trackId == UID_INVALID_NODE_ID && rangeId == UID_INVALID_NODE_ID &&
		thumbId == UID_INVALID_NODE_ID) {
		return;
	}

	const uid_rect_t &box = sst->contentBox;
	const float t = SliderNormalizedT(doc, sliderId);
	const bool enabled = parentEnabled && sst->effectivelyEnabled;

	float trackH = std::max(4.0f, box.h * 0.2f);
	float trackY = box.y + (box.h - trackH) * 0.5f;
	float trackX = box.x;
	float trackW = box.w;

	if (trackId != UID_INVALID_NODE_ID) {
		const uid_node_def_t *track = UID_GetNode(doc, trackId);
		if (track) {
			trackH = PartLengthPx(doc, *track, "height", box.h, trackH);
			trackW = PartLengthPx(doc, *track, "width", box.w, box.w);
			trackX = box.x + (box.w - trackW) * 0.5f;
			trackY = box.y + (box.h - trackH) * 0.5f;
			PlaceSliderPart(doc, trackId, trackX, trackY, trackW, trackH, sst->effectiveClip, enabled);
		}
	}

	uid_rect_t trackBox = MakeRect(trackX, trackY, trackW, trackH);
	if (trackId != UID_INVALID_NODE_ID) {
		const uid_node_state_t *tst = StateC(doc, trackId);
		if (tst) {
			trackBox = tst->borderBox;
		}
	}

	if (rangeId != UID_INVALID_NODE_ID) {
		const float rangeW = trackBox.w * t;
		PlaceSliderPart(
			doc,
			rangeId,
			trackBox.x,
			trackBox.y,
			rangeW,
			trackBox.h,
			sst->effectiveClip,
			enabled
		);
	}

	if (thumbId != UID_INVALID_NODE_ID) {
		const uid_node_def_t *thumb = UID_GetNode(doc, thumbId);
		float thumbW = 8.0f;
		float thumbH = box.h;
		if (thumb) {
			thumbW = PartLengthPx(doc, *thumb, "width", box.w, thumbW);
			thumbH = PartLengthPx(doc, *thumb, "height", box.h, thumbH);
		}
		float cx = trackBox.x + t * trackBox.w;
		float thumbX = cx - thumbW * 0.5f;
		float thumbY = box.y + (box.h - thumbH) * 0.5f;
		if (thumbX < box.x) {
			thumbX = box.x;
		} else if (thumbX + thumbW > box.x + box.w) {
			thumbX = box.x + box.w - thumbW;
		}
		if (thumbY < box.y) {
			thumbY = box.y;
		} else if (thumbY + thumbH > box.y + box.h) {
			thumbY = box.y + box.h - thumbH;
		}
		PlaceSliderPart(doc, thumbId, thumbX, thumbY, thumbW, thumbH, sst->effectiveClip, enabled);
	}
}

void LayoutPartChildren(
	uid_document_t *doc,
	uid_node_id_t partId,
	bool parentEnabled,
	float fbScale,
	const uid_backend_t *backend,
	uid_diag_list_t *diags
)
{
	uid_node_def_t *part = UID_GetNode(doc, partId);
	uid_node_state_t *pst = State(doc, partId);
	if (!part || !pst) {
		return;
	}
	for (uid_node_id_t c : part->children) {
		LayoutNode(
			doc,
			c,
			pst->contentBox.x,
			pst->contentBox.y,
			pst->contentBox.w,
			pst->contentBox.h,
			pst->contentBox.w,
			pst->contentBox.h,
			pst->effectiveClip,
			true,
			parentEnabled,
			fbScale,
			backend,
			diags
		);
	}
}

float AlignCross(uid_align_t align, float start, float size, float partSize)
{
	switch (align) {
	case UID_ALIGN_CENTER:
		return start + (size - partSize) * 0.5f;
	case UID_ALIGN_END:
		return start + size - partSize;
	default:
		return start;
	}
}

float AlignAlong(uid_align_t align, float trackStart, float trackLen, float partLen, float fraction)
{
	const float travel = std::max(0.0f, trackLen - partLen);
	switch (align) {
	case UID_ALIGN_CENTER:
		return trackStart + fraction * trackLen - partLen * 0.5f;
	case UID_ALIGN_END:
		return trackStart + fraction * trackLen - partLen;
	default:
		return trackStart + fraction * travel;
	}
}

void UID_LayoutScrollbar(
	uid_document_t *doc,
	uid_node_id_t containerId,
	bool parentEnabled,
	float fbScale,
	const uid_backend_t *backend
)
{
	uid_node_def_t *container = UID_GetNode(doc, containerId);
	uid_node_state_t *cst = State(doc, containerId);
	if (!container || !cst) {
		return;
	}

	const uid_node_id_t sbId = UID_FindChildOfKind(doc, containerId, UID_NODE_SCROLLBAR);
	const float maxY = std::max(0.0f, cst->contentExtentH - cst->contentBox.h);
	const float maxX = std::max(0.0f, cst->contentExtentW - cst->contentBox.w);

	auto clearRects = [&]() {
		cst->scrollbarTrackRect = MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
		cst->scrollbarThumbRect = MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
	};

	if (sbId == UID_INVALID_NODE_ID) {
		/* No-overflow: hide chrome entirely when content fits the viewport. */
		cst->scrollbarVisible = maxY > 0.0f;
		if (!cst->scrollbarVisible) {
			clearRects();
			return;
		}
		const float railW = UID_ScaleAuthoredPx(doc, 8.0f);
		const uid_rect_t area = UID_ScrollbarChromeClip(container, cst);
		cst->scrollbarTrackRect = MakeRect(area.x + area.w - railW, area.y, railW, area.h);
		const float thumbH = ScrollbarThumbAlongPx(
			doc,
			cst->scrollbarTrackRect.h,
			cst->contentBox.h,
			cst->contentExtentH
		);
		const float travel = std::max(0.0f, cst->scrollbarTrackRect.h - thumbH);
		const float t = maxY > 0.0f ? (cst->scrollY / maxY) : 0.0f;
		cst->scrollbarThumbRect = MakeRect(
			cst->scrollbarTrackRect.x,
			cst->scrollbarTrackRect.y + t * travel,
			railW,
			thumbH
		);
		return;
	}

	uid_node_def_t *sb = UID_GetNode(doc, sbId);
	if (!sb) {
		clearRects();
		return;
	}

	uid_layout_axis_t axis = UID_AXIS_VERTICAL;
	UID_ParseAxis(PropCStr(*sb, "axis", "vertical"), &axis, nullptr);
	const bool vertical = (axis == UID_AXIS_VERTICAL);
	const float maxScroll = vertical ? maxY : maxX;
	/* No-overflow: hide chrome entirely when content fits the viewport. */
	cst->scrollbarVisible = maxScroll > 0.0f;

	if (!cst->scrollbarVisible) {
		HideSubtree(doc, sbId);
		clearRects();
		return;
	}

	const bool enabled = parentEnabled && cst->effectivelyEnabled;
	const float fraction = maxScroll > 0.0f ? (vertical ? (cst->scrollY / maxScroll) : (cst->scrollX / maxScroll)) : 0.0f;

	const uid_rect_t area = UID_ScrollbarChromeClip(container, cst);
	const uid_rect_t chromeClip = area;
	float railX = area.x;
	float railY = area.y;
	float railW = area.w;
	float railH = area.h;
	if (vertical) {
		railW = PartLengthPx(doc, *sb, "width", area.w, UID_ScaleAuthoredPx(doc, 8.0f));
		railX = area.x + area.w - railW;
		railH = area.h;
	} else {
		railH = PartLengthPx(doc, *sb, "height", area.h, UID_ScaleAuthoredPx(doc, 8.0f));
		railY = area.y + area.h - railH;
		railW = area.w;
	}

	PlaceSliderPart(doc, sbId, railX, railY, railW, railH, chromeClip, enabled);

	const uid_node_id_t trackId = UID_FindChildOfKind(doc, sbId, UID_NODE_SCROLLBAR_TRACK);
	const uid_node_id_t thumbId = UID_FindChildOfKind(doc, sbId, UID_NODE_SCROLLBAR_THUMB);

	uid_rect_t trackBox = MakeRect(railX, railY, railW, railH);
	if (trackId != UID_INVALID_NODE_ID) {
		const uid_node_def_t *track = UID_GetNode(doc, trackId);
		if (track) {
			float trackW = railW;
			float trackH = railH;
			float trackX = railX;
			float trackY = railY;
			trackW = PartLengthPx(doc, *track, "width", railW, railW);
			trackH = PartLengthPx(doc, *track, "height", railH, railH);
			uid_align_t halign = UID_ALIGN_START;
			uid_align_t valign = UID_ALIGN_START;
			UID_ParseAlign(PropCStr(*track, "halign", "start"), &halign, nullptr);
			UID_ParseAlign(PropCStr(*track, "valign", "start"), &valign, nullptr);
			trackX = AlignCross(halign, railX, railW, trackW);
			trackY = AlignCross(valign, railY, railH, trackH);
			PlaceSliderPart(doc, trackId, trackX, trackY, trackW, trackH, chromeClip, enabled);
			LayoutPartChildren(doc, trackId, enabled, fbScale, backend, nullptr);
			const uid_node_state_t *tst = StateC(doc, trackId);
			if (tst) {
				trackBox = tst->borderBox;
			}
		}
	}

	cst->scrollbarTrackRect = trackBox;

	if (thumbId != UID_INVALID_NODE_ID) {
		const uid_node_def_t *thumb = UID_GetNode(doc, thumbId);
		/* Cross-axis thickness from XML; along-axis from viewport/content ratio. */
		float thumbW = vertical ? railW : UID_ScaleAuthoredPx(doc, 8.0f);
		float thumbH = vertical ? UID_ScaleAuthoredPx(doc, 20.0f) : railH;
		uid_align_t along = UID_ALIGN_START;
		uid_align_t cross = UID_ALIGN_CENTER;
		if (thumb) {
			if (vertical) {
				thumbW = PartLengthPx(doc, *thumb, "width", trackBox.w, thumbW);
				thumbH = ScrollbarThumbAlongPx(
					doc, trackBox.h, cst->contentBox.h, cst->contentExtentH
				);
				UID_ParseAlign(PropCStr(*thumb, "valign", "start"), &along, nullptr);
				UID_ParseAlign(PropCStr(*thumb, "halign", "center"), &cross, nullptr);
			} else {
				thumbH = PartLengthPx(doc, *thumb, "height", trackBox.h, thumbH);
				thumbW = ScrollbarThumbAlongPx(
					doc, trackBox.w, cst->contentBox.w, cst->contentExtentW
				);
				UID_ParseAlign(PropCStr(*thumb, "halign", "start"), &along, nullptr);
				UID_ParseAlign(PropCStr(*thumb, "valign", "center"), &cross, nullptr);
			}
		} else if (vertical) {
			thumbH = ScrollbarThumbAlongPx(
				doc, trackBox.h, cst->contentBox.h, cst->contentExtentH
			);
		} else {
			thumbW = ScrollbarThumbAlongPx(
				doc, trackBox.w, cst->contentBox.w, cst->contentExtentW
			);
		}

		float thumbX = trackBox.x;
		float thumbY = trackBox.y;
		if (vertical) {
			thumbX = AlignCross(cross, trackBox.x, trackBox.w, thumbW);
			thumbY = AlignAlong(along, trackBox.y, trackBox.h, thumbH, fraction);
		} else {
			thumbX = AlignAlong(along, trackBox.x, trackBox.w, thumbW, fraction);
			thumbY = AlignCross(cross, trackBox.y, trackBox.h, thumbH);
		}

		PlaceSliderPart(doc, thumbId, thumbX, thumbY, thumbW, thumbH, chromeClip, enabled);
		LayoutPartChildren(doc, thumbId, enabled, fbScale, backend, nullptr);
		const uid_node_state_t *thumbSt = StateC(doc, thumbId);
		if (thumbSt) {
			cst->scrollbarThumbRect = thumbSt->borderBox;
		} else {
			cst->scrollbarThumbRect = MakeRect(thumbX, thumbY, thumbW, thumbH);
		}
	} else {
		cst->scrollbarThumbRect = MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
	}
}

void LayoutNode(
	uid_document_t *doc,
	uid_node_id_t id,
	float marginX,
	float marginY,
	float marginW,
	float marginH,
	float percentBaseW,
	float percentBaseH,
	const uid_rect_t &parentClip,
	bool ancestorVisible,
	bool ancestorEnabled,
	float fbScale,
	const uid_backend_t *backend,
	uid_diag_list_t *diags
)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st) {
		return;
	}

	if (!IsLayoutKind(node->kind)) {
		ClearNodeGeometry(st);
		return;
	}

	const bool visible = ancestorVisible && PropBool(*node, "visible", true);
	const bool enabled = ancestorEnabled && PropBool(*node, "enabled", true);
	st->effectivelyEnabled = visible && enabled;

	if (!visible) {
		HideSubtree(doc, id);
		return;
	}

	const uid_sides_t margin = PropSides(*node, "margin");
	const float ml = ResolveSidePx(doc, margin.left, percentBaseW);
	const float mr = ResolveSidePx(doc, margin.right, percentBaseW);
	const float mt = ResolveSidePx(doc, margin.top, percentBaseH);
	const float mb = ResolveSidePx(doc, margin.bottom, percentBaseH);

	const float borderAvailW = std::max(0.0f, marginW - ml - mr);
	const float borderAvailH = std::max(0.0f, marginH - mt - mb);

	const uid_length_t width = PropLength(*node, "width", UID_LENGTH_AUTO);
	const uid_length_t height = PropLength(*node, "height", UID_LENGTH_AUTO);

	float bw;
	float bh;
	if (width.unit == UID_LENGTH_FILL) {
		bw = borderAvailW;
	} else if (width.unit == UID_LENGTH_AUTO) {
		if (ContainerMainAxisIsHorizontal(*node) && ContainerHasFillOnMainAxis(doc, *node) && borderAvailW > 0.0f) {
			bw = borderAvailW;
		} else {
			bw = IntrinsicBorderSize(doc, id, true, percentBaseW, percentBaseH, fbScale, backend);
			if (borderAvailW > 0.0f) {
				bw = std::min(bw, borderAvailW);
			}
		}
	} else {
		bw = ResolveLengthPx(doc, width, borderAvailW, borderAvailW, borderAvailW);
	}

	if (height.unit == UID_LENGTH_FILL) {
		bh = borderAvailH;
	} else if (height.unit == UID_LENGTH_AUTO) {
		if (!ContainerMainAxisIsHorizontal(*node) && ContainerHasFillOnMainAxis(doc, *node) && borderAvailH > 0.0f) {
			bh = borderAvailH;
		} else {
			bh = IntrinsicBorderSize(doc, id, false, percentBaseW, percentBaseH, fbScale, backend);
			if (borderAvailH > 0.0f) {
				bh = std::min(bh, borderAvailH);
			}
		}
	} else {
		bh = ResolveLengthPx(doc, height, borderAvailH, borderAvailH, borderAvailH);
	}

	/* Added in Omaha: max-width / max-height after authored/intrinsic/fill resolve. */
	bw = ClampAxisToMax(doc, *node, true, bw, percentBaseW, percentBaseH);
	bh = ClampAxisToMax(doc, *node, false, bh, percentBaseW, percentBaseH);

	ApplySelfBoxes(doc, id, marginX + ml, marginY + mt, bw, bh, percentBaseW, percentBaseH, parentClip);

	if (node->kind == UID_NODE_CONTAINER || node->kind == UID_NODE_FOREACH) {
		LayoutChildren(doc, id, enabled, fbScale, backend, diags);
	} else if (node->kind == UID_NODE_BUTTON && !node->children.empty()) {
		/* Added in OPM: icon/shape children inside buttons fill the content box. */
		LayoutChildren(doc, id, enabled, fbScale, backend, diags);
	} else if (node->kind == UID_NODE_SLIDER) {
		LayoutSliderParts(doc, id, enabled);
	}
}

void LayoutOverlapChildren(
	uid_document_t *doc,
	uid_node_id_t parentId,
	bool parentEnabled,
	float fbScale,
	const uid_backend_t *backend,
	uid_diag_list_t *diags
)
{
	uid_node_def_t *parent = UID_GetNode(doc, parentId);
	uid_node_state_t *pst = State(doc, parentId);
	if (!parent || !pst) {
		return;
	}

	const float pctW = pst->contentBox.w;
	const float pctH = pst->contentBox.h;
	const float contentW = pst->contentBox.w;
	const float contentH = pst->contentBox.h;

	for (uid_node_id_t c : parent->children) {
		uid_node_def_t *cn = UID_GetNode(doc, c);
		if (!cn || cn->kind == UID_NODE_SCROLLBAR || !IsLayoutKind(cn->kind)) {
			continue;
		}
		if (!PropBool(*cn, "visible", true)) {
			HideSubtree(doc, c);
			continue;
		}

		uid_align_t halign = UID_ALIGN_START;
		uid_align_t valign = UID_ALIGN_START;
		UID_ParseAlign(PropCStr(*cn, "halign", "start"), &halign, nullptr);
		UID_ParseAlign(PropCStr(*cn, "valign", "start"), &valign, nullptr);
		if (halign == UID_ALIGN_EQUAL_SPACING || halign == UID_ALIGN_SPACE_BETWEEN) {
			if (diags) {
				diags->Error(cn->source, "equal-spacing and space-between are not valid on overlap children");
			}
			halign = UID_ALIGN_START;
		}
		if (valign == UID_ALIGN_EQUAL_SPACING || valign == UID_ALIGN_SPACE_BETWEEN) {
			if (diags) {
				diags->Error(cn->source, "equal-spacing and space-between are not valid on overlap children");
			}
			valign = UID_ALIGN_START;
		}

		const uid_sides_t margin = PropSides(*cn, "margin");
		const float ml = ResolveSidePx(doc, margin.left, pctW);
		const float mr = ResolveSidePx(doc, margin.right, pctW);
		const float mt = ResolveSidePx(doc, margin.top, pctH);
		const float mb = ResolveSidePx(doc, margin.bottom, pctH);

		const uid_length_t wLen = PropLength(*cn, "width", UID_LENGTH_AUTO);
		const uid_length_t hLen = PropLength(*cn, "height", UID_LENGTH_AUTO);

		const float availW = std::max(0.0f, contentW - ml - mr);
		const float availH = std::max(0.0f, contentH - mt - mb);

		float bw = 0.0f;
		float bh = 0.0f;
		if (wLen.unit == UID_LENGTH_FILL) {
			bw = availW;
		} else if (wLen.unit == UID_LENGTH_AUTO) {
			bw = IntrinsicBorderSize(doc, c, true, pctW, pctH, fbScale, backend);
		} else {
			/*
			 * Fixed in OPM: percentage size is relative to the containing
			 * block, not the space left after margins.  The old calculation
			 * compounded URC percentage offsets (x + width), shrinking every
			 * absolutely placed pause-menu rect.
			 */
			bw = ResolveLengthPx(doc, wLen, contentW, contentW, contentW);
		}
		if (hLen.unit == UID_LENGTH_FILL) {
			bh = availH;
		} else if (hLen.unit == UID_LENGTH_AUTO) {
			bh = IntrinsicBorderSize(doc, c, false, pctW, pctH, fbScale, backend);
		} else {
			bh = ResolveLengthPx(doc, hLen, contentH, contentH, contentH);
		}

		if (cn->kind == UID_NODE_SHAPE_INSTANCE) {
			auto sit = doc->definitions.shapes.find(cn->shapeId);
			if (sit != doc->definitions.shapes.end() && sit->second.hasIntrinsicSize &&
			    sit->second.width > 0.0f && sit->second.height > 0.0f) {
				const float aspect = sit->second.width / sit->second.height;
				if (wLen.unit == UID_LENGTH_AUTO && hLen.unit != UID_LENGTH_AUTO && hLen.unit != UID_LENGTH_FILL) {
					bw = bh * aspect;
				} else if (hLen.unit == UID_LENGTH_AUTO && wLen.unit != UID_LENGTH_AUTO && wLen.unit != UID_LENGTH_FILL) {
					bh = bw / aspect;
				}
			}
		} else if (cn->kind == UID_NODE_IMAGE) {
			/* Added in OPM: leaf <image> preserves texel aspect when one axis is auto. */
			float aspect = 1.0f;
			if (LeafImageAspect(doc, *cn, backend, &aspect)) {
				if (wLen.unit == UID_LENGTH_AUTO && hLen.unit != UID_LENGTH_AUTO && hLen.unit != UID_LENGTH_FILL) {
					bw = bh * aspect;
				} else if (hLen.unit == UID_LENGTH_AUTO && wLen.unit != UID_LENGTH_AUTO && wLen.unit != UID_LENGTH_FILL) {
					bh = bw / aspect;
				}
			}
		}

		/* Added in Omaha */
		bw = ClampAxisToMax(doc, *cn, true, bw, pctW, pctH);
		bh = ClampAxisToMax(doc, *cn, false, bh, pctW, pctH);

		const float bx = pst->contentBox.x + ml + AlignCross(halign, 0.0f, availW, bw);
		const float by = pst->contentBox.y + mt + AlignCross(valign, 0.0f, availH, bh);

		ApplySelfBoxes(doc, c, bx, by, bw, bh, pctW, pctH, pst->effectiveClip);

		uid_node_state_t *cstWrite = State(doc, c);
		const bool childEnabled = parentEnabled && PropBool(*cn, "enabled", true);
		if (cstWrite) {
			cstWrite->effectivelyEnabled = childEnabled;
		}
		if (cn->kind == UID_NODE_CONTAINER || cn->kind == UID_NODE_FOREACH) {
			LayoutChildren(doc, c, childEnabled, fbScale, backend, diags);
		} else if (cn->kind == UID_NODE_BUTTON && !cn->children.empty()) {
			LayoutChildren(doc, c, childEnabled, fbScale, backend, diags);
		} else if (cn->kind == UID_NODE_SLIDER) {
			LayoutSliderParts(doc, c, childEnabled);
		}
	}

	pst->contentExtentW = pst->contentBox.w;
	pst->contentExtentH = pst->contentBox.h;
	pst->scrollX = 0.0f;
	pst->scrollY = 0.0f;
}

void LayoutChildren(
	uid_document_t *doc,
	uid_node_id_t parentId,
	bool parentEnabled,
	float fbScale,
	const uid_backend_t *backend,
	uid_diag_list_t *diags
)
{
	uid_node_def_t *parent = UID_GetNode(doc, parentId);
	uid_node_state_t *pst = State(doc, parentId);
	if (!parent || !pst) {
		return;
	}

	uid_layout_axis_t axis = UID_AXIS_VERTICAL;
	UID_ParseAxis(PropCStr(*parent, "type", "vertical"), &axis, nullptr);
	if (axis == UID_AXIS_OVERLAP) {
		uid_overflow_t overflow = UID_OVERFLOW_NONE;
		UID_ParseOverflow(PropCStr(*parent, "overflow", "none"), &overflow, nullptr);
		if (overflow == UID_OVERFLOW_SCROLL && diags) {
			diags->Error(parent->source, "overflow scroll is not supported on overlap containers");
		}
		LayoutOverlapChildren(doc, parentId, parentEnabled, fbScale, backend, diags);
		return;
	}
	const bool horiz = (axis == UID_AXIS_HORIZONTAL);

	uid_align_t halign = UID_ALIGN_START;
	uid_align_t valign = UID_ALIGN_START;
	UID_ParseAlign(PropCStr(*parent, "halign", "start"), &halign, nullptr);
	UID_ParseAlign(PropCStr(*parent, "valign", "start"), &valign, nullptr);

	const uid_align_t mainAlign = horiz ? halign : valign;
	const uid_align_t crossAlign = horiz ? valign : halign;

	if (crossAlign == UID_ALIGN_EQUAL_SPACING && diags) {
		diags->Error(parent->source, "equal-spacing is only valid on the main axis");
	}
	if (crossAlign == UID_ALIGN_SPACE_BETWEEN && diags) {
		diags->Error(parent->source, "space-between is only valid on the main axis");
	}

	float gap = 0.0f;
	uid_length_t gapLen;
	if (UID_ParseLength(PropCStr(*parent, "gap", "0"), &gapLen, nullptr)) {
		gap = ResolveSidePx(doc, gapLen, horiz ? pst->contentBox.w : pst->contentBox.h);
		if (gap < 0.0f) {
			gap = 0.0f;
		}
	}

	struct ChildGeom {
		uid_node_id_t id;
		float margin0;
		float margin1;
		float crossM0;
		float crossM1;
		float borderMain;
		float borderCross;
		bool  fillMain;
		bool  shrinkOverflow; /* Added in Omaha: auto/fill + overflow scroll|hidden */
	};

	std::vector<ChildGeom> kids;
	const float contentMain = horiz ? pst->contentBox.w : pst->contentBox.h;
	const float contentCross = horiz ? pst->contentBox.h : pst->contentBox.w;
	const float pctW = pst->contentBox.w;
	const float pctH = pst->contentBox.h;

	for (uid_node_id_t c : parent->children) {
		uid_node_def_t *cn = UID_GetNode(doc, c);
		if (!cn || cn->kind == UID_NODE_SCROLLBAR || !IsLayoutKind(cn->kind)) {
			continue;
		}
		if (!PropBool(*cn, "visible", true)) {
			HideSubtree(doc, c);
			continue;
		}

		ChildGeom g;
		g.id = c;
		const uid_sides_t margin = PropSides(*cn, "margin");
		g.margin0 = horiz ? ResolveSidePx(doc, margin.left, pctW) : ResolveSidePx(doc, margin.top, pctH);
		g.margin1 = horiz ? ResolveSidePx(doc, margin.right, pctW) : ResolveSidePx(doc, margin.bottom, pctH);
		g.crossM0 = horiz ? ResolveSidePx(doc, margin.top, pctH) : ResolveSidePx(doc, margin.left, pctW);
		g.crossM1 = horiz ? ResolveSidePx(doc, margin.bottom, pctH) : ResolveSidePx(doc, margin.right, pctW);

		const uid_length_t mainLen = PropLength(*cn, horiz ? "width" : "height", UID_LENGTH_AUTO);
		const uid_length_t crossLen = PropLength(*cn, horiz ? "height" : "width", UID_LENGTH_AUTO);
		g.fillMain = (mainLen.unit == UID_LENGTH_FILL);
		/*
		 * Fixed in OPM: only promote auto→fill when the child's flex main axis
		 * matches the parent's. A horizontal row with width=fill children must
		 * not become height=fill inside a vertical parent (authored height=auto).
		 */
		if (!g.fillMain && mainLen.unit == UID_LENGTH_AUTO &&
		    (cn->kind == UID_NODE_CONTAINER || cn->kind == UID_NODE_FOREACH) &&
		    ContainerMainAxisIsHorizontal(*cn) == horiz &&
		    ContainerHasFillOnMainAxis(doc, *cn)) {
			g.fillMain = true;
		}
		/* Added in Omaha: shrink scroll/hidden auto|fill children when parent budget is tight. */
		g.shrinkOverflow =
			(g.fillMain || mainLen.unit == UID_LENGTH_AUTO) && NodeOverflowClipsOrScrolls(*cn);

		const float childAvailMain = std::max(0.0f, contentMain - g.margin0 - g.margin1);
		const float childAvailCross = std::max(0.0f, contentCross - g.crossM0 - g.crossM1);

		if (g.fillMain) {
			g.borderMain = 0.0f;
		} else if (mainLen.unit == UID_LENGTH_AUTO) {
			g.borderMain = IntrinsicBorderSize(doc, c, horiz, pctW, pctH, fbScale, backend);
		} else {
			g.borderMain = ResolveLengthPx(doc, mainLen, childAvailMain, 0.0f, 0.0f);
		}

		if (crossLen.unit == UID_LENGTH_FILL) {
			g.borderCross = childAvailCross;
		} else if (crossLen.unit == UID_LENGTH_AUTO) {
			const bool childHoriz = ContainerMainAxisIsHorizontal(*cn);
			if ((cn->kind == UID_NODE_CONTAINER || cn->kind == UID_NODE_FOREACH) &&
			    ContainerHasFillOnMainAxis(doc, *cn) && childHoriz != horiz && childAvailCross > 0.0f) {
				g.borderCross = childAvailCross;
			} else {
				g.borderCross = IntrinsicBorderSize(doc, c, !horiz, pctW, pctH, fbScale, backend);
			}
		} else {
			g.borderCross = ResolveLengthPx(doc, crossLen, childAvailCross, childAvailCross, 0.0f);
		}

		/*
		 * Intrinsic shapes (e.g. allied-star): if one axis is explicit and the
		 * other is auto, preserve viewBox aspect like CSS width:auto; height:68%.
		 */
		if (cn->kind == UID_NODE_SHAPE_INSTANCE) {
			auto sit = doc->definitions.shapes.find(cn->shapeId);
			if (sit != doc->definitions.shapes.end() && sit->second.hasIntrinsicSize &&
			    sit->second.width > 0.0f && sit->second.height > 0.0f) {
				const float aspect = sit->second.width / sit->second.height;
				const uid_length_t wLen = PropLength(*cn, "width", UID_LENGTH_AUTO);
				const uid_length_t hLen = PropLength(*cn, "height", UID_LENGTH_AUTO);
				if (wLen.unit == UID_LENGTH_AUTO && hLen.unit != UID_LENGTH_AUTO &&
				    hLen.unit != UID_LENGTH_FILL) {
					const float h = horiz ? g.borderCross : g.borderMain;
					const float w = h * aspect;
					if (horiz) {
						g.borderMain = w;
					} else {
						g.borderCross = w;
					}
				} else if (hLen.unit == UID_LENGTH_AUTO && wLen.unit != UID_LENGTH_AUTO &&
				           wLen.unit != UID_LENGTH_FILL) {
					const float w = horiz ? g.borderMain : g.borderCross;
					const float h = w / aspect;
					if (horiz) {
						g.borderCross = h;
					} else {
						g.borderMain = h;
					}
				}
			}
		} else if (cn->kind == UID_NODE_IMAGE) {
			/* Added in OPM: leaf <image> preserves texel aspect when one axis is auto. */
			float aspect = 1.0f;
			if (LeafImageAspect(doc, *cn, backend, &aspect)) {
				const uid_length_t wLen = PropLength(*cn, "width", UID_LENGTH_AUTO);
				const uid_length_t hLen = PropLength(*cn, "height", UID_LENGTH_AUTO);
				if (wLen.unit == UID_LENGTH_AUTO && hLen.unit != UID_LENGTH_AUTO &&
				    hLen.unit != UID_LENGTH_FILL) {
					const float h = horiz ? g.borderCross : g.borderMain;
					const float w = h * aspect;
					if (horiz) {
						g.borderMain = w;
					} else {
						g.borderCross = w;
					}
				} else if (hLen.unit == UID_LENGTH_AUTO && wLen.unit != UID_LENGTH_AUTO &&
				           wLen.unit != UID_LENGTH_FILL) {
					const float w = horiz ? g.borderMain : g.borderCross;
					const float h = w / aspect;
					if (horiz) {
						g.borderCross = h;
					} else {
						g.borderMain = h;
					}
				}
			}
		}

		/* Added in Omaha: clamp child main/cross after measure (max-width / max-height). */
		g.borderMain = ClampAxisToMax(doc, *cn, horiz, g.borderMain, pctW, pctH);
		g.borderCross = ClampAxisToMax(doc, *cn, !horiz, g.borderCross, pctW, pctH);

		kids.push_back(g);
	}

	const int n = static_cast<int>(kids.size());
	const float gapTotal = (n > 1) ? gap * static_cast<float>(n - 1) : 0.0f;

	float fixedMain = gapTotal;
	int fillCount = 0;
	for (const ChildGeom &g : kids) {
		fixedMain += g.margin0 + g.margin1;
		if (g.fillMain) {
			++fillCount;
		} else {
			fixedMain += g.borderMain;
		}
	}

	float remaining = contentMain - fixedMain;
	if (remaining < 0.0f) {
		remaining = 0.0f;
	}
	const float fillEach = (fillCount > 0) ? (remaining / static_cast<float>(fillCount)) : 0.0f;
	for (ChildGeom &g : kids) {
		if (g.fillMain) {
			g.borderMain = fillEach;
		}
	}

	/*
	 * Added in Omaha: when autos exceed the parent main budget (e.g. max-height
	 * clamped panel), shrink overflow=scroll|hidden children so nested scroll
	 * viewports get a reduced box while non-scroll siblings keep intrinsic size.
	 */
	{
		float usedMainCheck = gapTotal;
		for (const ChildGeom &g : kids) {
			usedMainCheck += g.margin0 + g.borderMain + g.margin1;
		}
		float deficit = usedMainCheck - contentMain;
		if (deficit > 0.0f) {
			float shrinkableTotal = 0.0f;
			for (const ChildGeom &g : kids) {
				if (g.shrinkOverflow && g.borderMain > 0.0f) {
					shrinkableTotal += g.borderMain;
				}
			}
			if (shrinkableTotal > 0.0f) {
				const float keep = std::max(0.0f, shrinkableTotal - deficit);
				const float scale = keep / shrinkableTotal;
				for (ChildGeom &g : kids) {
					if (g.shrinkOverflow && g.borderMain > 0.0f) {
						g.borderMain *= scale;
					}
				}
			}
		}
	}

	float usedMain = gapTotal;
	for (const ChildGeom &g : kids) {
		usedMain += g.margin0 + g.borderMain + g.margin1;
	}
	float freeMain = contentMain - usedMain;
	if (freeMain < 0.0f) {
		freeMain = 0.0f;
	}

	float mainCursor = horiz ? pst->contentBox.x : pst->contentBox.y;
	/* Added in OPM: windowed foreach keeps rows glued to the viewport; scrollY only
	 * drives collectionScrollOffset + scrollbar thumb (synthetic contentExtentH). */
	const bool windowedScroll = !horiz && UID_ScrollParentHasWindowedForeach(doc, parentId);
	const float layoutScrollX = windowedScroll ? 0.0f : pst->scrollX;
	const float layoutScrollY = windowedScroll ? 0.0f : pst->scrollY;
	if (horiz) {
		mainCursor -= layoutScrollX;
	} else {
		mainCursor -= layoutScrollY;
	}

	float slot = 0.0f;
	if (mainAlign == UID_ALIGN_EQUAL_SPACING && n > 0) {
		slot = freeMain / static_cast<float>(n + 1);
		mainCursor += slot;
	} else if (mainAlign == UID_ALIGN_SPACE_BETWEEN && n > 1) {
		/* CSS space-between: free space only between children (no leading/trailing). */
		slot = freeMain / static_cast<float>(n - 1);
	} else if (mainAlign == UID_ALIGN_CENTER) {
		mainCursor += freeMain * 0.5f;
	} else if (mainAlign == UID_ALIGN_END) {
		mainCursor += freeMain;
	}

	float extentMain = 0.0f;
	float extentCross = 0.0f;

	for (int i = 0; i < n; ++i) {
		ChildGeom &g = kids[static_cast<size_t>(i)];
		mainCursor += g.margin0;

		float crossPos = (horiz ? pst->contentBox.y : pst->contentBox.x);
		if (horiz) {
			crossPos -= pst->scrollY;
		} else {
			crossPos -= pst->scrollX;
		}
		crossPos += g.crossM0;

		const float crossAvail = contentCross - g.crossM0 - g.crossM1;
		float crossFree = crossAvail - g.borderCross;
		if (crossFree < 0.0f) {
			crossFree = 0.0f;
		}
		if (crossAlign == UID_ALIGN_CENTER) {
			crossPos += crossFree * 0.5f;
		} else if (crossAlign == UID_ALIGN_END) {
			crossPos += crossFree;
		}

		float bx, by, bw, bh;
		if (horiz) {
			bx = mainCursor;
			by = crossPos;
			bw = g.borderMain;
			bh = g.borderCross;
		} else {
			bx = crossPos;
			by = mainCursor;
			bw = g.borderCross;
			bh = g.borderMain;
		}

		ApplySelfBoxes(doc, g.id, bx, by, bw, bh, pctW, pctH, pst->effectiveClip);

		uid_node_def_t *cn = UID_GetNode(doc, g.id);
		uid_node_state_t *cstWrite = State(doc, g.id);
		const bool childEnabled = parentEnabled && cn && PropBool(*cn, "enabled", true);
		if (cstWrite) {
			cstWrite->effectivelyEnabled = childEnabled;
		}
		if (cn && (cn->kind == UID_NODE_CONTAINER || cn->kind == UID_NODE_FOREACH)) {
			LayoutChildren(doc, g.id, childEnabled, fbScale, backend, diags);
		} else if (cn && cn->kind == UID_NODE_BUTTON && !cn->children.empty()) {
			LayoutChildren(doc, g.id, childEnabled, fbScale, backend, diags);
		} else if (cn && cn->kind == UID_NODE_SLIDER) {
			/* Added in OPM: compose track/range/thumb after host box is known. */
			LayoutSliderParts(doc, g.id, childEnabled);
		}

		const uid_node_state_t *cst = State(doc, g.id);
		if (cst) {
			const float localMainEnd = horiz
				? (cst->marginBox.x + cst->marginBox.w - pst->contentBox.x + layoutScrollX)
				: (cst->marginBox.y + cst->marginBox.h - pst->contentBox.y + layoutScrollY);
			const float localCrossEnd = horiz
				? (cst->marginBox.y + cst->marginBox.h - pst->contentBox.y + layoutScrollY)
				: (cst->marginBox.x + cst->marginBox.w - pst->contentBox.x + layoutScrollX);
			extentMain = std::max(extentMain, localMainEnd);
			extentCross = std::max(extentCross, localCrossEnd);
		}

		mainCursor += g.borderMain + g.margin1;
		if (i + 1 < n) {
			mainCursor += gap;
			if (mainAlign == UID_ALIGN_EQUAL_SPACING || mainAlign == UID_ALIGN_SPACE_BETWEEN) {
				mainCursor += slot;
			}
		}
	}

	pst->contentExtentW = horiz ? extentMain : std::max(extentCross, pst->contentBox.w);
	pst->contentExtentH = horiz ? std::max(extentCross, pst->contentBox.h) : extentMain;

	/* Added in OPM: fake full-list height so scrollbar thumb matches collection count. */
	if (!horiz) {
		const float synthetic = UID_WindowedForeachSyntheticExtentH(doc, parentId);
		if (synthetic >= 0.0f) {
			pst->contentExtentH = synthetic;
		}
	}

	uid_overflow_t overflow = UID_OVERFLOW_NONE;
	UID_ParseOverflow(PropCStr(*parent, "overflow", "none"), &overflow, nullptr);
	if (overflow == UID_OVERFLOW_SCROLL) {
		const float maxX = std::max(0.0f, pst->contentExtentW - pst->contentBox.w);
		const float maxY = std::max(0.0f, pst->contentExtentH - pst->contentBox.h);
		pst->scrollX = std::min(std::max(pst->scrollX, 0.0f), maxX);
		pst->scrollY = std::min(std::max(pst->scrollY, 0.0f), maxY);
	} else {
		pst->scrollX = 0.0f;
		pst->scrollY = 0.0f;
	}

	if (parent->kind == UID_NODE_CONTAINER && overflow == UID_OVERFLOW_SCROLL) {
		UID_LayoutScrollbar(doc, parentId, parentEnabled, fbScale, backend);
	}
}

void CollectPaintOrder(
	const uid_document_t *doc,
	uid_node_id_t id,
	bool ancestorVisible,
	std::vector<uid_node_id_t> *out
)
{
	const uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node || !IsLayoutKind(node->kind)) {
		return;
	}
	if (!(ancestorVisible && PropBool(*node, "visible", true))) {
		return;
	}
	for (uid_node_id_t c : node->children) {
		CollectPaintOrder(doc, c, true, out);
	}
	out->push_back(id);
}

} // namespace

uid_text_wrap_t UID_TextWrapMode(const uid_node_def_t &node)
{
	return TextWrapMode(node);
}

float UID_TextLineHeight(
	const uid_document_t *doc,
	const uid_node_def_t &node,
	float fbScale,
	const uid_backend_t *backend
)
{
	return TextLineHeight(doc, node, fbScale, backend);
}

void UID_BuildTextLines(
	const uid_document_t *doc,
	const uid_node_def_t &node,
	const char *text,
	float maxWrapWidth,
	float fbScale,
	const uid_backend_t *backend,
	std::vector<std::string> *lines,
	uid_text_block_metrics_t *metrics
)
{
	BuildTextLines(doc, node, text, maxWrapWidth, fbScale, backend, lines, metrics);
}

uid_rect_t UID_ScrollbarChromeClip(const uid_node_def_t *container, const uid_node_state_t *st)
{
	if (!container || !st) {
		uid_rect_t empty{};
		return empty;
	}
	const char *edgeStr = container->properties.GetCStr("scrollbar-edge", nullptr);
	if (!edgeStr || !edgeStr[0]) {
		edgeStr = "content";
	}
	uid_scrollbar_edge_t edge = UID_SCROLLBAR_EDGE_CONTENT;
	UID_ParseScrollbarEdge(edgeStr, &edge, nullptr);
	if (edge == UID_SCROLLBAR_EDGE_BORDER) {
		return st->borderBox;
	}
	return st->contentBox;
}

float UID_ScaleAuthoredPx(const uid_document_t *doc, float px)
{
	/* Added in OPM: authored px × lastUiPxScale (reference-resolution scale). */
	const float s = (doc && doc->lastUiPxScale > 0.0f) ? doc->lastUiPxScale : 1.0f;
	return px * s;
}

bool UID_PointInClippedRect(const uid_rect_t &rect, const uid_rect_t &clip, float x, float y)
{
	return PointInRect(rect, x, y) && PointInRect(clip, x, y);
}

uid_overlay_placement_t UID_PlaceOverlayInViewport(
	const uid_rect_t &viewport,
	const uid_rect_t &anchor,
	float panelW,
	float contentH,
	float currentScrollY,
	float gapPx,
	float maxPanelH
)
{
	uid_overlay_placement_t out;
	out.contentH = std::max(0.0f, contentH);
	out.scrollY = std::max(0.0f, currentScrollY);
	out.flippedY = false;

	const float vpLeft = viewport.x;
	const float vpTop = viewport.y;
	const float vpRight = viewport.x + viewport.w;
	const float vpBottom = viewport.y + viewport.h;

	panelW = std::max(0.0f, panelW);
	/* Added in OPM: soft cap so long option lists scroll instead of covering the menu. */
	float panelH = out.contentH;
	if (maxPanelH > 0.0f) {
		panelH = std::min(panelH, maxPanelH);
	}

	const float belowY = anchor.y + anchor.h + gapPx;
	const float aboveY = anchor.y - gapPx - panelH;
	const float belowSpace = vpBottom - belowY;
	const float aboveSpace = anchor.y - gapPx - vpTop;

	bool useBelow = true;
	if (panelH <= belowSpace) {
		useBelow = true;
	} else if (panelH <= aboveSpace) {
		useBelow = false;
	} else if (belowSpace >= aboveSpace) {
		useBelow = true;
		panelH = std::min(panelH, std::max(0.0f, belowSpace));
	} else {
		useBelow = false;
		panelH = std::min(panelH, std::max(0.0f, aboveSpace));
	}

	float panelY = useBelow ? belowY : aboveY;
	if (panelH < out.contentH) {
		if (useBelow) {
			panelY = belowY;
			panelH = std::min(out.contentH, std::max(0.0f, belowSpace));
		} else {
			panelH = std::min(out.contentH, std::max(0.0f, aboveSpace));
			panelY = anchor.y - gapPx - panelH;
		}
		if (maxPanelH > 0.0f) {
			panelH = std::min(panelH, maxPanelH);
			if (!useBelow) {
				panelY = anchor.y - gapPx - panelH;
			}
		}
	}

	out.flippedY = !useBelow;

	float panelX = anchor.x;
	if (panelX + panelW > vpRight) {
		panelX = vpRight - panelW;
	}
	if (panelX < vpLeft) {
		panelX = vpLeft;
	}
	if (panelW > viewport.w) {
		panelW = viewport.w;
		panelX = vpLeft;
	}

	out.panel = MakeRect(panelX, panelY, panelW, panelH);

	if (out.panel.y + out.panel.h > vpBottom) {
		out.panel.y = vpBottom - out.panel.h;
	}
	if (out.panel.y < vpTop) {
		out.panel.y = vpTop;
	}
	if (out.panel.y + out.panel.h > vpBottom) {
		out.panel.h = std::max(0.0f, vpBottom - out.panel.y);
	}

	const float maxScroll = std::max(0.0f, out.contentH - out.panel.h);
	if (out.scrollY > maxScroll) {
		out.scrollY = maxScroll;
	}

	return out;
}

uid_node_id_t UID_HitTest(const uid_document_t *doc, float x, float y, bool overlayFirst)
{
	if (!doc) {
		return UID_INVALID_NODE_ID;
	}

	if (UID_IsModalActive(doc)) {
		const uid_node_id_t modalRoot = UID_GetModalRoot(doc);
		if (modalRoot != UID_INVALID_NODE_ID) {
			std::vector<uid_node_id_t> order;
			CollectPaintOrder(doc, modalRoot, true, &order);
			for (int i = static_cast<int>(order.size()) - 1; i >= 0; --i) {
				const uid_node_id_t id = order[static_cast<size_t>(i)];
				const uid_node_def_t *node = UID_GetNode(doc, id);
				const uid_node_state_t *st = StateC(doc, id);
				if (!node || !st || !IsInteractiveKind(node->kind)) {
					continue;
				}
				if (!PropBool(*node, "visible", true)) {
					continue;
				}
				if (UID_PointInClippedRect(st->borderBox, st->effectiveClip, x, y)) {
					return id;
				}
			}
			for (int i = static_cast<int>(order.size()) - 1; i >= 0; --i) {
				const uid_node_id_t id = order[static_cast<size_t>(i)];
				const uid_node_state_t *st = StateC(doc, id);
				if (st && UID_PointInClippedRect(st->borderBox, st->effectiveClip, x, y)) {
					return id;
				}
			}
		}
		return UID_INVALID_NODE_ID;
	}

	auto hitsBorder = [&](uid_node_id_t id) -> bool {
		const uid_node_def_t *node = UID_GetNode(doc, id);
		const uid_node_state_t *st = StateC(doc, id);
		if (!node || !st) {
			return false;
		}
		if (!PropBool(*node, "visible", true)) {
			return false;
		}
		return UID_PointInClippedRect(st->borderBox, st->effectiveClip, x, y);
	};

	(void)overlayFirst;

	std::vector<uid_node_id_t> order;
	if (doc->rootNode != UID_INVALID_NODE_ID) {
		CollectPaintOrder(doc, doc->rootNode, true, &order);
	}
	for (int i = static_cast<int>(order.size()) - 1; i >= 0; --i) {
		const uid_node_id_t id = order[static_cast<size_t>(i)];
		const uid_node_def_t *node = UID_GetNode(doc, id);
		if (!node) {
			continue;
		}
		if (!IsInteractiveKind(node->kind)) {
			continue;
		}
		if (hitsBorder(id)) {
			return id;
		}
	}

	/* Fallback: any visible laid-out node (for scroll containers). */
	for (int i = static_cast<int>(order.size()) - 1; i >= 0; --i) {
		if (hitsBorder(order[static_cast<size_t>(i)])) {
			return order[static_cast<size_t>(i)];
		}
	}

	return UID_INVALID_NODE_ID;
}

uid_result_t UID_LayoutDocument(
	uid_document_t *doc,
	int logicalW,
	int logicalH,
	float fbScale,
	float uiPxScale,
	const uid_backend_t *backend,
	uid_diag_list_t *diags
)
{
	if (!doc) {
		return UID_ERR_INVALID_ARG;
	}
	if (logicalW < 0 || logicalH < 0) {
		return UID_ERR_INVALID_ARG;
	}

	EnsureStates(doc);
	doc->lastFbScale = fbScale > 0.0f ? fbScale : 1.0f;
	doc->lastUiPxScale = uiPxScale > 0.0f ? uiPxScale : 1.0f;
	doc->lastLogicalW = logicalW;
	doc->lastLogicalH = logicalH;

	const float lw = static_cast<float>(logicalW);
	const float lh = static_cast<float>(logicalH);
	const uid_rect_t canvasClip = MakeRect(0.0f, 0.0f, lw, lh);

	if (doc->rootNode == UID_INVALID_NODE_ID) {
		doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty & ~UID_DIRTY_LAYOUT);
		return UID_OK;
	}

	LayoutNode(doc, doc->rootNode, 0.0f, 0.0f, lw, lh, lw, lh, canvasClip, true, true, doc->lastFbScale, backend, diags);

	if (UID_IsModalActive(doc)) {
		const uid_node_id_t modalRoot = UID_GetModalRoot(doc);
		if (modalRoot != UID_INVALID_NODE_ID) {
			/*
			 * Fixed in OPM: height=auto relative panels fill their content during this
			 * pass, which clamps scrollY to 0. Preserve scroll for PlaceRelativeModalPanel.
			 */
			float savedRelativeScrollY = 0.0f;
			const uid_node_id_t relativePanelId = FindRelativePanel(doc, modalRoot);
			if (relativePanelId != UID_INVALID_NODE_ID) {
				if (uid_node_state_t *rst = State(doc, relativePanelId)) {
					savedRelativeScrollY = rst->scrollY;
				}
			}

			LayoutNode(
				doc,
				modalRoot,
				0.0f,
				0.0f,
				lw,
				lh,
				lw,
				lh,
				canvasClip,
				true,
				true,
				doc->lastFbScale,
				backend,
				diags
			);

			if (relativePanelId != UID_INVALID_NODE_ID) {
				if (uid_node_state_t *rst = State(doc, relativePanelId)) {
					rst->scrollY = savedRelativeScrollY;
				}
			}

			/* Added in OPM: type=relative panels anchor to modalOpenerNode. */
			PlaceRelativeModalPanel(doc, doc->lastFbScale, backend, diags);
		}
	}

	doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty & ~UID_DIRTY_LAYOUT);
	doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty | UID_DIRTY_PAINT);
	return UID_OK;
}
