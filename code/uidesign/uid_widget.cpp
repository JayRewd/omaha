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

#include "uid_widget.h"

#include "uid_binding.h"
#include "uid_collection.h"
#include "uid_expr_bool.h"
#include "uid_modal.h"
#include "uid_layout.h"
#include "uid_opt.h"
#include "uid_scrollbar.h"
#include "uid_shape.h"
#include "uid_value.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool IsPaintKind(uid_node_kind_t kind)
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

bool ParseColorProp(const uid_node_def_t &node, const char *name, uid_color_t *out)
{
	return node.properties.GetColorCached(name, out);
}

void ColorToRgba(const uid_color_t &c, float rgba[4], float opacityMul = 1.0f)
{
	rgba[0] = c.r;
	rgba[1] = c.g;
	rgba[2] = c.b;
	rgba[3] = c.a * opacityMul;
}

float NodeOpacity(const uid_node_def_t &node)
{
	double v = 1.0;
	if (!node.properties.GetNumberCached("opacity", &v)) {
		const uid_prop_entry_t *b = UID_BuiltinDefaultParsed("opacity");
		if (b) {
			if (!(UID_OptEnabled(UID_OPT_PARSE_CACHE) && (b->cacheValid & UID_PROP_CACHE_NUMBER)
				  && (b->cacheOk & UID_PROP_CACHE_NUMBER))) {
				if (UID_ParseNumber(b->value.c_str(), &b->number, nullptr)) {
					b->cacheValid |= UID_PROP_CACHE_NUMBER;
					b->cacheOk |= UID_PROP_CACHE_NUMBER;
				}
			}
			if (b->cacheOk & UID_PROP_CACHE_NUMBER) {
				v = b->number;
			}
		} else {
			const char *o = PropCStr(node, "opacity", "1");
			UID_ParseNumber(o, &v, nullptr);
		}
	}
	return std::clamp(static_cast<float>(v), 0.0f, 1.0f);
}

void ApplyBgRotationOrigin(
	uid_rect_t            &geom,
	const uid_rect_t      &borderBox,
	float                  rotationDeg,
	const uid_node_def_t &node,
	uid_document_t        *doc
)
{
	if (std::fabs(rotationDeg) <= 1e-6f) {
		return;
	}
	const float uiScale = (doc && doc->lastUiPxScale > 0.0f) ? doc->lastUiPxScale : 1.0f;
	float ox = borderBox.w * 0.5f;
	float oy = borderBox.h * 0.5f;
	const char *orig = PropCStr(node, "rotation-origin", nullptr);
	if (orig && orig[0]) {
		(void)UID_ParseRotationOrigin(orig, borderBox.w, borderBox.h, uiScale, &ox, &oy, nullptr);
	}
	const float cx = geom.x + geom.w * 0.5f;
	const float cy = geom.y + geom.h * 0.5f;
	const float oAbsX = borderBox.x + ox;
	const float oAbsY = borderBox.y + oy;
	const float dx = cx - oAbsX;
	const float dy = cy - oAbsY;
	const float rad = rotationDeg * (static_cast<float>(M_PI) / 180.0f);
	const float s = std::sin(rad);
	const float c = std::cos(rad);
	geom.x += (oAbsX + dx * c - dy * s) - cx;
	geom.y += (oAbsY + dx * s + dy * c) - cy;
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

void PushClip(const uid_backend_t *backend, const uid_rect_t &clip)
{
	if (backend && backend->pushClip) {
		backend->pushClip(clip.x, clip.y, clip.w, clip.h);
	}
}

void PopClip(const uid_backend_t *backend)
{
	if (backend && backend->popClip) {
		backend->popClip();
	}
}

float FontLogicalPx(const uid_document_t *doc, const uid_node_def_t &node)
{
	uid_length_t len;
	len.unit = UID_LENGTH_PX;
	len.value = 12.0f;
	if (!node.properties.GetLengthCached("font-size", &len)) {
		const char *v = PropCStr(node, "font-size", "12px");
		if (v) {
			UID_ParseLength(v, &len, nullptr);
		}
	}
	float px = 12.0f;
	if (len.unit == UID_LENGTH_PX && len.value > 0.0f) {
		px = len.value;
	}
	return UID_ScaleAuthoredPx(doc, px);
}

/*
 * Fixed in OPM: leaf text used contentBox origin + an extra font-size Y offset,
 * so button/label glyphs sat low-left instead of respecting halign/valign.
 * fontDraw's Y is the top of the typographic box (ascent applied inside UIR).
 */
void ResolveTextAlign(const uid_node_def_t &node, uid_align_t *halign, uid_align_t *valign)
{
	uid_align_t hDefault = UID_ALIGN_START;
	uid_align_t vDefault = UID_ALIGN_START;

	switch (node.kind) {
	case UID_NODE_BUTTON:
	case UID_NODE_KEYBIND:
	case UID_NODE_SELECT:
		hDefault = UID_ALIGN_CENTER;
		vDefault = UID_ALIGN_CENTER;
		break;
	case UID_NODE_INPUT:
	case UID_NODE_LABEL:
		hDefault = UID_ALIGN_START;
		vDefault = UID_ALIGN_CENTER;
		break;
	default:
		break;
	}

	*halign = hDefault;
	*valign = vDefault;
	/* Use authored props only — UID_BuiltinDefault("halign") is "start" for containers. */
	int hEnum = static_cast<int>(hDefault);
	int vEnum = static_cast<int>(vDefault);
	if (node.properties.GetEnumCached("halign", UID_PROP_ENUM_ALIGN, &hEnum)) {
		*halign = static_cast<uid_align_t>(hEnum);
	}
	if (node.properties.GetEnumCached("valign", UID_PROP_ENUM_ALIGN, &vEnum)) {
		*valign = static_cast<uid_align_t>(vEnum);
	}
	/* equal-spacing / space-between are container-only; fall back to kind default for text. */
	if (*halign == UID_ALIGN_EQUAL_SPACING || *halign == UID_ALIGN_SPACE_BETWEEN) {
		*halign = hDefault;
	}
	if (*valign == UID_ALIGN_EQUAL_SPACING || *valign == UID_ALIGN_SPACE_BETWEEN) {
		*valign = vDefault;
	}
}

uint64_t HashTextCacheKey(
	const char *text,
	const char *fontId,
	int weight,
	float fontPx,
	float uiPxScale,
	float fbScale
)
{
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
	a.f = fontPx;
	b.f = uiPxScale;
	c.f = fbScale;
	h ^= a.u;
	h *= 1099511628211ull;
	h ^= b.u;
	h *= 1099511628211ull;
	h ^= c.u;
	h *= 1099511628211ull;
	return h;
}

/* Added in OPM: FNV-1a key for per-node resolved shape cache. */
unsigned long long HashShapeKey(
	const char *shapeName,
	float parentW,
	float parentH,
	float uiPxScale,
	const char *fillOv,
	const char *strokeOv,
	const char *strokeWOv,
	unsigned propsVersion
)
{
	unsigned long long h = 14695981039346656037ull;
	auto mixBytes = [&](const void *p, size_t n) {
		const unsigned char *b = static_cast<const unsigned char *>(p);
		for (size_t i = 0; i < n; ++i) {
			h ^= b[i];
			h *= 1099511628211ull;
		}
	};
	auto mixStr = [&](const char *s) {
		if (!s) {
			h ^= 0;
			h *= 1099511628211ull;
			return;
		}
		while (*s) {
			h ^= static_cast<unsigned char>(*s++);
			h *= 1099511628211ull;
		}
		h ^= 0xff;
		h *= 1099511628211ull;
	};
	mixStr(shapeName);
	mixBytes(&parentW, sizeof(parentW));
	mixBytes(&parentH, sizeof(parentH));
	mixBytes(&uiPxScale, sizeof(uiPxScale));
	mixStr(fillOv);
	mixStr(strokeOv);
	mixStr(strokeWOv);
	h ^= static_cast<unsigned long long>(propsVersion);
	h *= 1099511628211ull;
	return h;
}

/*
 * Added in OPM: resolve shape paths with optional per-node cache (UID_OPT_SHAPE_CACHE).
 * outPaths points at st->cachedShapePaths on hit/miss with cache on, else a thread_local scratch.
 */
bool ResolveShapeCached(
	uid_document_t *doc,
	uid_node_id_t id,
	uid_node_state_t *st,
	const uid_shape_def_t *shape,
	uid_shape_resolve_params_t *params,
	const char *shapeNameForKey,
	const std::vector<uid_resolved_path_t> **outPaths
)
{
	if (!outPaths || !shape || !params) {
		return false;
	}
	*outPaths = nullptr;

	const unsigned propsVersion =
		(doc && id >= 0 && static_cast<size_t>(id) < doc->nodes.size())
			? doc->nodes[static_cast<size_t>(id)].properties.Version()
			: 0u;
	const unsigned long long key = HashShapeKey(
		shapeNameForKey,
		params->parentWidth,
		params->parentHeight,
		params->uiPxScale,
		params->fillOverride,
		params->strokeOverride,
		params->strokeWidthOverride,
		propsVersion
	);

	if (UID_OptEnabled(UID_OPT_SHAPE_CACHE) && st) {
		if (st->cachedShapeValid && st->cachedShapeKey == key) {
			*outPaths = &st->cachedShapePaths;
			return !st->cachedShapePaths.empty();
		}
		st->cachedShapePaths.clear();
		if (UID_ResolveShape(shape, params, &st->cachedShapePaths, nullptr) != UID_OK) {
			st->cachedShapeValid = false;
			st->cachedShapeKey = 0;
			*outPaths = &st->cachedShapePaths;
			return false;
		}
		st->cachedShapeKey = key;
		st->cachedShapeValid = true;
		*outPaths = &st->cachedShapePaths;
		return !st->cachedShapePaths.empty();
	}

	static thread_local std::vector<uid_resolved_path_t> scratch;
	scratch.clear();
	if (UID_ResolveShape(shape, params, &scratch, nullptr) != UID_OK) {
		*outPaths = &scratch;
		return false;
	}
	*outPaths = &scratch;
	return !scratch.empty();
}

void ComputeTextDrawOrigin(
	const uid_document_t *doc,
	const uid_node_def_t &node,
	const uid_node_state_t &st,
	const char *text,
	void *font,
	const uid_backend_t *backend,
	float *outX,
	float *outY
)
{
	uid_align_t halign;
	uid_align_t valign;
	float textW = 0.0f;
	float lineH = FontLogicalPx(doc, node);

	ResolveTextAlign(node, &halign, &valign);

	if (font && backend && backend->fontMeasure && text) {
		uid_node_state_t *mst = const_cast<uid_node_state_t *>(&st);
		const char *fontId = PropCStr(node, "font", "body");
		int weight = 400;
		{
			double w = 400.0;
			const char *fw = PropCStr(node, "font-weight", "400");
			if (fw) {
				UID_ParseNumber(fw, &w, nullptr);
			}
			weight = static_cast<int>(w);
		}
		const float fontPx = FontLogicalPx(doc, node);
		const float fbScale = (doc && doc->lastFbScale > 0.0f) ? doc->lastFbScale : 1.0f;
		const float uiPxScale = (doc && doc->lastUiPxScale > 0.0f) ? doc->lastUiPxScale : 1.0f;
		const uint64_t measureKey = HashTextCacheKey(text, fontId, weight, fontPx, uiPxScale, fbScale);
		if (UID_OptEnabled(UID_OPT_TEXT_CACHE) && mst->cachedTextWidth >= 0.0f
			&& mst->cachedMeasureKey == measureKey) {
			textW = mst->cachedTextWidth;
		} else {
			textW = backend->fontMeasure(font, text);
			const char *trackProp = node.properties.GetCStr("letter-spacing", nullptr);
			if (trackProp && trackProp[0] && text[0]) {
				uid_length_t len;
				len.unit = UID_LENGTH_PX;
				len.value = 0.0f;
				if (node.properties.GetLengthCached("letter-spacing", &len) || UID_ParseLength(trackProp, &len, nullptr)) {
					if (len.unit == UID_LENGTH_PX && len.value != 0.0f) {
						const int n = static_cast<int>(std::strlen(text));
						if (n > 1) {
							textW += UID_ScaleAuthoredPx(doc, len.value) * static_cast<float>(n - 1);
						}
					}
				}
			}
			if (UID_OptEnabled(UID_OPT_TEXT_CACHE)) {
				mst->cachedMeasureKey = measureKey;
				mst->cachedTextWidth = textW;
			}
		}
	} else if (text) {
		textW = static_cast<float>(std::strlen(text)) * 8.0f;
	}
	/* lineH kept as font-size fallback when ascent is unavailable */

	float x = st.contentBox.x;
	float y = st.contentBox.y;

	if (halign == UID_ALIGN_CENTER) {
		x = st.contentBox.x + (st.contentBox.w - textW) * 0.5f;
	} else if (halign == UID_ALIGN_END) {
		x = st.contentBox.x + st.contentBox.w - textW;
	}

	/*
	 * Cap-centric vertical align: UI strings are mostly caps / x-height ink.
	 * Centering the full line box (incl. descenders) sits glyphs too low.
	 * Place so ~0.62*ascent below the box midline matches optical center.
	 */
	float ascent = lineH;
	if (font && backend && backend->fontAscent) {
		const float a = backend->fontAscent(font);
		if (a > 0.0f) {
			ascent = a;
		}
	}
	if (valign == UID_ALIGN_CENTER) {
		y = st.contentBox.y + st.contentBox.h * 0.5f - ascent * 0.62f;
	} else if (valign == UID_ALIGN_END) {
		y = st.contentBox.y + st.contentBox.h - ascent;
	} else {
		y = st.contentBox.y;
	}

	/*
	 * Both-sided skew parallelogram: mid-line shifts with Y. Keep upright text
	 * on that mid-line (skew-tab legacy + skew-rect with skewl and skewr).
	 */
	const char *shapeName = node.properties.GetCStr("shape", nullptr);
	bool        parallelogram = false;
	if (shapeName && std::strcmp(shapeName, "skew-tab") == 0) {
		parallelogram = true;
	} else if (shapeName && std::strcmp(shapeName, "skew-rect") == 0) {
		uid_length_t skewL{};
		uid_length_t skewR{};
		const char  *sl = node.properties.GetCStr("skewl", "12px");
		const char  *sr = node.properties.GetCStr("skewr", "12px");
		if (UID_ParseLength(sl, &skewL, nullptr) && UID_ParseLength(sr, &skewR, nullptr) &&
		    skewL.value > 0.0f && skewR.value > 0.0f) {
			parallelogram = true;
		}
	}
	if (parallelogram && st.borderBox.h > 0.0f) {
		const float skew = st.borderBox.h * 0.3249f;
		const float textMidY = y + ascent * 0.5f;
		const float yRel = (textMidY - st.borderBox.y) / st.borderBox.h;
		x += skew * (0.5f - yRel);
	}

	if (outX) {
		*outX = x;
	}
	if (outY) {
		*outY = y;
	}
}

bool WantsDropShadow(const uid_node_def_t &node)
{
	if (node.kind != UID_NODE_LABEL && node.kind != UID_NODE_BUTTON) {
		return false;
	}
	return PropBool(node, "drop-shadow", false);
}

uid_rect_t ExpandClipRect(const uid_rect_t &clip, float margin)
{
	uid_rect_t out;
	out.x = clip.x - margin;
	out.y = clip.y - margin;
	out.w = clip.w + margin * 2.0f;
	out.h = clip.h + margin * 2.0f;
	if (out.w < 0.0f) {
		out.w = 0.0f;
	}
	if (out.h < 0.0f) {
		out.h = 0.0f;
	}
	return out;
}

/*
 * Drop-shadow glyphs extend past the node clip. Pop the duplicate node clip,
 * push an expanded rect intersected with the parent clip, then restore on scope end.
 */
class DropShadowPaintScope {
public:
	DropShadowPaintScope(
		const uid_backend_t *backend,
		const uid_document_t *doc,
		const uid_node_def_t &node,
		const uid_rect_t &nodeClip
	)
		: m_backend(backend)
		, m_nodeClip(nodeClip)
		, m_active(WantsDropShadow(node))
	{
		if (m_active) {
			const float margin = UID_ScaleAuthoredPx(doc, 2.0f);
			PopClip(m_backend);
			PushClip(m_backend, ExpandClipRect(m_nodeClip, margin));
		}
	}

	~DropShadowPaintScope()
	{
		if (m_active) {
			PopClip(m_backend);
			PushClip(m_backend, m_nodeClip);
		}
	}

private:
	const uid_backend_t *m_backend;
	uid_rect_t           m_nodeClip;
	bool                 m_active;
};

void PaintTextGlyphs(
	const uid_backend_t   *backend,
	void                  *font,
	const uid_document_t  *doc,
	const uid_node_def_t  &node,
	float                  x,
	float                  y,
	const char            *text,
	const float            rgba[4],
	float                  opacityMul,
	float                  skewTan,
	float                  skewOriginY,
	float                  tracking
)
{
	if (!backend || !font || !text || !backend->fontDraw) {
		return;
	}

	static const struct {
		float dx;
		float dy;
		float a;
	} kShadowPasses[] = {
		{0.0f, -1.0f, 0.50f},
		{0.0f, 1.0f, 0.50f},
		{-1.0f, 0.0f, 0.50f},
		{1.0f, 0.0f, 0.50f},
		{1.5f, 1.5f, 0.33f},
	};

	auto drawAt = [&](float drawX, float drawY, const float *color) {
		if (skewTan != 0.0f && backend->fontDrawSkewed) {
			backend->fontDrawSkewed(font, drawX, drawY, text, color, skewTan, skewOriginY, tracking);
		} else {
			backend->fontDraw(font, drawX, drawY, text, color, tracking);
		}
	};

	if (WantsDropShadow(node)) {
		float shadowRgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		for (const auto &pass : kShadowPasses) {
			const float dx = UID_ScaleAuthoredPx(doc, pass.dx);
			const float dy = UID_ScaleAuthoredPx(doc, pass.dy);
			/* Fade shadow faster than main so overlapping layers don't show through fading text. */
			shadowRgba[3] = pass.a * opacityMul * opacityMul;
			drawAt(x + dx, y + dy, shadowRgba);
		}
	}
	drawAt(x, y, rgba);
}

/* UTF-8 byte offset of the Nth Unicode scalar (same rules as uid_input). */
size_t Utf8ByteOffsetForCodepoint(const std::string &s, size_t codepoint)
{
	size_t count = 0;
	size_t i = 0;
	while (i < s.size() && count < codepoint) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		size_t len = 1;
		if ((c & 0x80u) == 0) {
			len = 1;
		} else if ((c & 0xE0u) == 0xC0u) {
			len = 2;
		} else if ((c & 0xF0u) == 0xE0u) {
			len = 3;
		} else if ((c & 0xF8u) == 0xF0u) {
			len = 4;
		}
		if (i + len > s.size()) {
			len = 1;
		}
		i += len;
		++count;
	}
	return i;
}

/*
 * Fixed in OPM: caret used a hardcoded 8px/codepoint advance while glyphs use
 * fontMeasure (+ letter-spacing). Measure the prefix up to the caret instead.
 */
float MeasureCaretAdvance(
	const uid_document_t *doc,
	const uid_node_def_t &node,
	const std::string &text,
	size_t caretCodepoint,
	void *font,
	const uid_backend_t *backend
)
{
	if (caretCodepoint == 0) {
		return 0.0f;
	}
	const size_t byteEnd = Utf8ByteOffsetForCodepoint(text, caretCodepoint);
	if (byteEnd == 0) {
		return 0.0f;
	}
	if (font && backend && backend->fontMeasure) {
		const std::string prefix = text.substr(0, byteEnd);
		float advance = backend->fontMeasure(font, prefix.c_str());
		const char *trackProp = node.properties.GetCStr("letter-spacing", nullptr);
		if (trackProp && trackProp[0] && prefix.size() > 1) {
			uid_length_t len;
			len.unit = UID_LENGTH_PX;
			len.value = 0.0f;
			if (node.properties.GetLengthCached("letter-spacing", &len) || UID_ParseLength(trackProp, &len, nullptr)) {
				if (len.unit == UID_LENGTH_PX && len.value != 0.0f) {
					advance += UID_ScaleAuthoredPx(doc, len.value) * static_cast<float>(prefix.size() - 1);
				}
			}
		}
		return advance;
	}
	return static_cast<float>(caretCodepoint) * 8.0f;
}

void *ResolveFont(
	const uid_document_t *doc,
	const uid_node_def_t &node,
	const uid_backend_t *backend,
	uid_node_state_t *st = nullptr
)
{
	if (!backend || !backend->fontResolve) {
		return nullptr;
	}
	const char *fontId = PropCStr(node, "font", "body");
	int weight = 400;
	{
		double w = 400.0;
		const char *fw = PropCStr(node, "font-weight", "400");
		if (fw) {
			UID_ParseNumber(fw, &w, nullptr);
		}
		weight = static_cast<int>(w);
	}
	const float fontPx = FontLogicalPx(doc, node);
	const float fbScale = (doc && doc->lastFbScale > 0.0f) ? doc->lastFbScale : 1.0f;
	const float uiPxScale = (doc && doc->lastUiPxScale > 0.0f) ? doc->lastUiPxScale : 1.0f;
	const uint64_t fontKey = HashTextCacheKey("", fontId, weight, fontPx, uiPxScale, fbScale);
	if (UID_OptEnabled(UID_OPT_TEXT_CACHE) && st && st->cachedFont && st->cachedTextKey == fontKey) {
		return st->cachedFont;
	}
	const uid_font_def_t *fontDef = UID_FindFontDef(doc, fontId, weight);
	if (!fontDef) {
		return nullptr;
	}
	void *font = backend->fontResolve(fontDef->src.c_str(), fontPx, fbScale);
	if (UID_OptEnabled(UID_OPT_TEXT_CACHE) && st) {
		st->cachedFont = font;
		st->cachedTextKey = fontKey;
		st->cachedTextWidth = -1.0f;
	}
	return font;
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

/*
 * Added in OPM: resolve non-rect owner shape paths for descendant stencil clipping.
 * Layout ignores shape; paint clips children to this geometry. Returns false when
 * no clip should be applied (default rectangle, edge-clip, missing shape, etc.).
 */
bool ResolveShapeChildClip(
	uid_document_t *doc,
	uid_node_id_t id,
	const uid_backend_t *backend,
	uid_rect_t *outGeom,
	float *outViewW,
	float *outViewH,
	float *outRotationDeg,
	std::vector<std::string> *outPathD
)
{
	if (!doc || !outGeom || !outViewW || !outViewH || !outRotationDeg || !outPathD) {
		return false;
	}
	outPathD->clear();
	const uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st) {
		return false;
	}

	const char *shapeName = PropCStr(*node, "shape", "rectangle");
	if (!shapeName || !shapeName[0] || std::strcmp(shapeName, "edge-clip") == 0) {
		return false;
	}
	if (IsDefaultRectShape(*node)) {
		return false;
	}
	if (doc->definitions.shapes.find(shapeName) == doc->definitions.shapes.end()) {
		return false;
	}

	float strokeWidthPx = 0.0f;
	{
		const char *strokeStr = PropCStr(*node, "stroke", nullptr);
		if (strokeStr && strokeStr[0]) {
			uid_color_t stroke{};
			if (UID_ParseColor(strokeStr, &stroke, nullptr) && stroke.a > 0.0f) {
				std::string widthStr = PropCStr(*node, "stroke-width", "1px");
				if (backend) {
					std::string resolved;
					if (UID_ResolvePropString(backend, widthStr, &resolved)) {
						widthStr = resolved;
					}
				}
				uid_length_t wLen{};
				if (UID_ParseLength(widthStr.c_str(), &wLen, nullptr) && wLen.unit == UID_LENGTH_PX &&
				    wLen.value > 0.0f) {
					strokeWidthPx = UID_ScaleAuthoredPx(doc, wLen.value);
				}
			}
		}
	}

	uid_rect_t geom = st->borderBox;
	if (strokeWidthPx > 0.0f) {
		float inset = strokeWidthPx;
		if (inset * 2.0f > geom.w) {
			inset = geom.w * 0.5f;
		}
		if (inset * 2.0f > geom.h) {
			inset = std::min(inset, geom.h * 0.5f);
		}
		geom.x += inset;
		geom.y += inset;
		geom.w -= inset * 2.0f;
		geom.h -= inset * 2.0f;
		if (geom.w < 0.0f) {
			geom.w = 0.0f;
		}
		if (geom.h < 0.0f) {
			geom.h = 0.0f;
		}
	}
	if (geom.w <= 0.0f || geom.h <= 0.0f) {
		return false;
	}

	float pathRotationDeg = 0.0f;
	{
		const char *rotStr = PropCStr(*node, "shape-rotation", nullptr);
		if (rotStr && rotStr[0]) {
			(void)UID_ParseRotationDeg(rotStr, &pathRotationDeg, nullptr);
		}
	}

	const auto sit = doc->definitions.shapes.find(shapeName);
	uid_shape_resolve_params_t params{};
	float viewW = geom.w;
	float viewH = geom.h;
	if (sit->second.hasIntrinsicSize && sit->second.width > 0.0f && sit->second.height > 0.0f) {
		params.parentWidth = sit->second.width;
		params.parentHeight = sit->second.height;
		viewW = sit->second.width;
		viewH = sit->second.height;
	} else {
		params.parentWidth = geom.w;
		params.parentHeight = geom.h;
	}
	if (sit->second.hasIntrinsicSize && (viewW != geom.w || viewH != geom.h)) {
		params.uiPxScale = 1.0f;
	} else {
		params.uiPxScale = (doc->lastUiPxScale > 0.0f) ? doc->lastUiPxScale : 1.0f;
	}
	params.shapeProps = &node->properties;
	params.limits = &doc->limits;
	params.backend = backend;
	params.doc = doc;
	params.nodeId = id;
	params.parentProps = &node->properties;

	const std::vector<uid_resolved_path_t> *paths = nullptr;
	if (!ResolveShapeCached(doc, id, st, &sit->second, &params, shapeName, &paths) || !paths) {
		return false;
	}
	for (const uid_resolved_path_t &p : *paths) {
		if (!p.d.empty()) {
			outPathD->push_back(p.d);
		}
	}
	if (outPathD->empty()) {
		return false;
	}

	*outGeom = geom;
	*outViewW = viewW;
	*outViewH = viewH;
	*outRotationDeg = pathRotationDeg;
	return true;
}

void DrawSolid(const uid_backend_t *backend, const uid_rect_t &box, const uid_color_t &color, float opacityMul = 1.0f)
{
	if (!backend || !backend->drawSolidRect || color.a <= 0.0f || opacityMul <= 0.001f) {
		return;
	}
	float rgba[4];
	ColorToRgba(color, rgba, opacityMul);
	if (rgba[3] <= 0.001f) {
		return;
	}
	backend->drawSolidRect(box.x, box.y, box.w, box.h, rgba);
}

/*
 * Added in OPM: resolve mask-image to a VFS path or linear(...)/radial(...) brush.
 * Returns false when missing or invalid.
 */
bool ResolveMaskImageSpec(
	uid_document_t *doc,
	const uid_node_def_t &node,
	const uid_backend_t *backend,
	std::string *outSpec
)
{
	if (!doc || !outSpec) {
		return false;
	}
	std::string raw;
	if (!node.properties.Get("mask-image", &raw) || raw.empty()) {
		return false;
	}
	if (backend) {
		std::string resolved;
		if (UID_ResolvePropString(backend, raw, &resolved)) {
			raw = resolved;
		}
	}
	if (UID_IsGradientBrush(raw.c_str())) {
		*outSpec = raw;
		return true;
	}
	const auto iit = doc->definitions.images.find(raw);
	if (iit != doc->definitions.images.end()) {
		*outSpec = iit->second.src;
		return !outSpec->empty();
	}
	if (raw.find("..") != std::string::npos || raw.empty() || raw[0] == '/') {
		return false;
	}
	if (raw.find('/') == std::string::npos) {
		return false;
	}
	*outSpec = raw;
	return true;
}

static void PaintBackgroundImage(
	uid_document_t           *doc,
	uid_node_id_t             nodeId,
	const uid_node_def_t     &node,
	const uid_rect_t         &geom,
	bool                      usePathPaint,
	const char               *shapeName,
	bool                      rectShape,
	float                     rotationDeg,
	float                     opacityMul,
	const uid_backend_t      *backend
)
{
	std::string imageId;
	/* Added in OPM: leaf <image> prefers src; containers keep background-image. */
	if (node.kind == UID_NODE_IMAGE) {
		if (!node.properties.Get("src", &imageId) || imageId.empty()) {
			(void)node.properties.Get("background-image", &imageId);
		}
	} else if (!node.properties.Get("background-image", &imageId) || imageId.empty()) {
		return;
	}
	if (!backend || !backend->drawImage || imageId.empty()) {
		return;
	}
	if (geom.w <= 0.0f || geom.h <= 0.0f || opacityMul <= 0.001f) {
		return;
	}

	if (backend) {
		std::string resolved;
		if (UID_ResolvePropString(backend, imageId, &resolved)) {
			imageId = resolved;
		}
	}

	std::string imageSrc;
	const auto iit = doc->definitions.images.find(imageId);
	if (iit != doc->definitions.images.end()) {
		imageSrc = iit->second.src;
	} else if (imageId.find("..") != std::string::npos || imageId.empty() || imageId[0] == '/') {
		return;
	} else if (imageId.find('/') == std::string::npos) {
		return;
	} else {
		imageSrc = imageId;
	}

	/* Leaf <image> defaults to contain; container backgrounds default to stretch. */
	uid_image_fit_t fit = (node.kind == UID_NODE_IMAGE) ? UID_IMAGE_FIT_CONTAIN : UID_IMAGE_FIT_STRETCH;
	{
		std::string fitStr;
		if (node.kind == UID_NODE_IMAGE) {
			if (!node.properties.Get("fit", &fitStr) || fitStr.empty()) {
				(void)node.properties.Get("background-fit", &fitStr);
			}
		} else {
			(void)node.properties.Get("background-fit", &fitStr);
		}
		if (!fitStr.empty()) {
			(void)UID_ParseImageFit(fitStr.c_str(), &fit, nullptr);
		}
	}

	float backgroundScale = 1.0f;
	{
		std::string scaleStr;
		if (node.kind == UID_NODE_IMAGE) {
			if (!node.properties.Get("scale", &scaleStr) || scaleStr.empty()) {
				(void)node.properties.Get("background-scale", &scaleStr);
			}
		} else {
			(void)node.properties.Get("background-scale", &scaleStr);
		}
		if (!scaleStr.empty()) {
			double scaleVal = 1.0;
			if (UID_EvalRuntimeNumericExpr(doc, nodeId, scaleStr, backend, &scaleVal) ||
			    UID_ParseNumber(scaleStr.c_str(), &scaleVal, nullptr)) {
				if (scaleVal > 0.0) {
					backgroundScale = static_cast<float>(scaleVal);
				}
			}
		}
	}

	static auto evalEdgeClipFrac = [](
		uid_document_t           *doc,
		uid_node_id_t             nodeId,
		const uid_property_set_t &props,
		const char               *name,
		double                    defaultVal,
		const uid_backend_t      *backend,
		double                   *out
	) {
		std::string raw;
		if (!props.Get(name, &raw) || raw.empty()) {
			*out = defaultVal;
			return;
		}
		if (UID_EvalRuntimeNumericExpr(doc, nodeId, raw, backend, out)) {
			return;
		}
		std::string dm;
		if (!UID_ParseNumber(raw.c_str(), out, &dm)) {
			*out = defaultVal;
		}
	};

	const bool isEdgeClip = shapeName && std::strcmp(shapeName, "edge-clip") == 0;

	std::vector<std::string> clipStorage;
	std::vector<const char *> clipDs;
	float viewW = geom.w;
	float viewH = geom.h;
	float drawX = geom.x;
	float drawY = geom.y;
	float drawW = geom.w;
	float drawH = geom.h;

	if (fit == UID_IMAGE_FIT_REPEAT && isEdgeClip) {
		double leftFrac = 0.0;
		double topFrac = 0.0;
		double rightFrac = 1.0;
		double bottomFrac = 1.0;
		evalEdgeClipFrac(doc, nodeId, node.properties, "left", 0.0, backend, &leftFrac);
		evalEdgeClipFrac(doc, nodeId, node.properties, "top", 0.0, backend, &topFrac);
		evalEdgeClipFrac(doc, nodeId, node.properties, "right", 1.0, backend, &rightFrac);
		evalEdgeClipFrac(doc, nodeId, node.properties, "bottom", 1.0, backend, &bottomFrac);

		if (leftFrac < 0.0) {
			leftFrac = 0.0;
		}
		if (topFrac < 0.0) {
			topFrac = 0.0;
		}
		if (rightFrac > 1.0) {
			rightFrac = 1.0;
		}
		if (bottomFrac > 1.0) {
			bottomFrac = 1.0;
		}
		if (rightFrac < leftFrac) {
			rightFrac = leftFrac;
		}
		if (bottomFrac < topFrac) {
			bottomFrac = topFrac;
		}

		const bool retailOffset =
			topFrac > 1e-6 || leftFrac > 1e-6 || rightFrac < 1.0 - 1e-6 || bottomFrac < 1.0 - 1e-6;

		if (retailOffset) {
			/*
			 * Retail statbar parity (cl_uistd.cpp): offset the draw position by the
			 * clipped fraction and keep full frame height so repeat UVs stay aligned
			 * to tile rows. Scissoring mid-pattern leaves half-cartridge drift.
			 */
			drawX = geom.x + static_cast<float>(leftFrac) * geom.w;
			drawY = geom.y + static_cast<float>(topFrac) * geom.h;
			drawW = geom.w * static_cast<float>(rightFrac - leftFrac);
			drawH = geom.h;

			const float visW = drawW;
			const float visH = geom.h * static_cast<float>(bottomFrac - topFrac);
			viewW = drawW;
			viewH = drawH;

			char pathBuf[128];
			std::snprintf(
				pathBuf,
				sizeof(pathBuf),
				"M 0 0 L %.4f 0 L %.4f %.4f L 0 %.4f Z",
				visW,
				visW,
				visH,
				visH
			);
			clipStorage.push_back(pathBuf);
			clipDs.push_back(clipStorage.back().c_str());
		} else if (usePathPaint) {
			const std::string pathShapeId = rectShape ? "rectangle" : std::string(shapeName);
			auto sit = doc->definitions.shapes.find(pathShapeId);
			if (sit != doc->definitions.shapes.end()) {
				uid_shape_resolve_params_t params{};
				if (sit->second.hasIntrinsicSize && sit->second.width > 0.0f && sit->second.height > 0.0f) {
					params.parentWidth = sit->second.width;
					params.parentHeight = sit->second.height;
					viewW = sit->second.width;
					viewH = sit->second.height;
				} else {
					params.parentWidth = geom.w;
					params.parentHeight = geom.h;
				}
				/*
				 * Fixed in OPM: intrinsic viewbox props must not also take uiPxScale —
				 * layout already sized geom and SvgMap stretches view→dest.
				 */
				if (sit->second.hasIntrinsicSize && (viewW != geom.w || viewH != geom.h)) {
					params.uiPxScale = 1.0f;
				} else {
					params.uiPxScale = (doc->lastUiPxScale > 0.0f) ? doc->lastUiPxScale : 1.0f;
				}
				params.shapeProps = &node.properties;
				params.limits = &doc->limits;
				params.parentProps = &node.properties;
				params.backend = backend;
				params.doc = doc;
				params.nodeId = nodeId;

				uid_node_state_t *st = State(doc, nodeId);
				const std::vector<uid_resolved_path_t> *paths = nullptr;
				if (ResolveShapeCached(doc, nodeId, st, &sit->second, &params, pathShapeId.c_str(), &paths) &&
					paths) {
					for (const uid_resolved_path_t &p : *paths) {
						if (p.d.empty()) {
							continue;
						}
						clipStorage.push_back(p.d);
						clipDs.push_back(clipStorage.back().c_str());
					}
				}
			}
		}
	} else if (usePathPaint) {
		const std::string pathShapeId = rectShape ? "rectangle" : std::string(shapeName);
		auto sit = doc->definitions.shapes.find(pathShapeId);
		if (sit != doc->definitions.shapes.end()) {
			uid_shape_resolve_params_t params{};
			if (sit->second.hasIntrinsicSize && sit->second.width > 0.0f && sit->second.height > 0.0f) {
				params.parentWidth = sit->second.width;
				params.parentHeight = sit->second.height;
				viewW = sit->second.width;
				viewH = sit->second.height;
			} else {
				params.parentWidth = geom.w;
				params.parentHeight = geom.h;
			}
			/* Fixed in OPM: intrinsic stretch already applies DIP; avoid uiPxScale². */
			if (sit->second.hasIntrinsicSize && (viewW != geom.w || viewH != geom.h)) {
				params.uiPxScale = 1.0f;
			} else {
				params.uiPxScale = (doc->lastUiPxScale > 0.0f) ? doc->lastUiPxScale : 1.0f;
			}
			params.shapeProps = &node.properties;
			params.limits = &doc->limits;
			params.parentProps = &node.properties;
			params.backend = backend;
			params.doc = doc;
			params.nodeId = nodeId;

			uid_node_state_t *st = State(doc, nodeId);
			const std::vector<uid_resolved_path_t> *paths = nullptr;
			if (ResolveShapeCached(doc, nodeId, st, &sit->second, &params, pathShapeId.c_str(), &paths) &&
				paths) {
				for (const uid_resolved_path_t &p : *paths) {
					if (p.d.empty()) {
						continue;
					}
					clipStorage.push_back(p.d);
					clipDs.push_back(clipStorage.back().c_str());
				}
			}
		}
	}

	const float tintRgba[4] = {1.0f, 1.0f, 1.0f, opacityMul};
	backend->drawImage(
		imageSrc.c_str(),
		drawX,
		drawY,
		drawW,
		drawH,
		clipDs.empty() ? nullptr : clipDs.data(),
		static_cast<int>(clipDs.size()),
		viewW,
		viewH,
		static_cast<int>(fit),
		rotationDeg,
		backgroundScale,
		tintRgba
	);
}

/* Added in OPM: atlas gradient fill, clipped like background-image. */
static void PaintGradientFill(
	uid_document_t           *doc,
	uid_node_id_t             nodeId,
	const uid_node_def_t     &node,
	const uid_rect_t         &geom,
	const std::string        &brush,
	bool                      usePathPaint,
	const char               *shapeName,
	bool                      rectShape,
	float                     rotationDeg,
	float                     opacityMul,
	const uid_backend_t      *backend
)
{
	if (!backend || !backend->drawGradient || brush.empty()) {
		return;
	}
	if (geom.w <= 0.0f || geom.h <= 0.0f || opacityMul <= 0.001f) {
		return;
	}

	std::vector<std::string> clipStorage;
	std::vector<const char *> clipDs;
	float viewW = geom.w;
	float viewH = geom.h;

	if (usePathPaint && shapeName && std::strcmp(shapeName, "edge-clip") != 0) {
		const std::string pathShapeId = rectShape ? "rectangle" : std::string(shapeName);
		auto sit = doc->definitions.shapes.find(pathShapeId);
		if (sit != doc->definitions.shapes.end()) {
			uid_shape_resolve_params_t params{};
			if (sit->second.hasIntrinsicSize && sit->second.width > 0.0f && sit->second.height > 0.0f) {
				params.parentWidth = sit->second.width;
				params.parentHeight = sit->second.height;
				viewW = sit->second.width;
				viewH = sit->second.height;
			} else {
				params.parentWidth = geom.w;
				params.parentHeight = geom.h;
			}
			if (sit->second.hasIntrinsicSize && (viewW != geom.w || viewH != geom.h)) {
				params.uiPxScale = 1.0f;
			} else {
				params.uiPxScale = (doc->lastUiPxScale > 0.0f) ? doc->lastUiPxScale : 1.0f;
			}
			params.shapeProps = &node.properties;
			params.limits = &doc->limits;
			params.parentProps = &node.properties;
			params.backend = backend;
			params.doc = doc;
			params.nodeId = nodeId;

			/* Transparent parent.fill — shape path fill must stay solid-parseable. */
			params.fillOverride = "#00000000";

			uid_node_state_t *st = State(doc, nodeId);
			const std::vector<uid_resolved_path_t> *paths = nullptr;
			if (ResolveShapeCached(doc, nodeId, st, &sit->second, &params, pathShapeId.c_str(), &paths) &&
				paths) {
				for (const uid_resolved_path_t &p : *paths) {
					if (p.d.empty()) {
						continue;
					}
					clipStorage.push_back(p.d);
					clipDs.push_back(clipStorage.back().c_str());
				}
			}
		}
	}

	float tint[4] = {1.0f, 1.0f, 1.0f, opacityMul};
	backend->drawGradient(
		brush.c_str(),
		geom.x,
		geom.y,
		geom.w,
		geom.h,
		clipDs.empty() ? nullptr : clipDs.data(),
		static_cast<int>(clipDs.size()),
		viewW,
		viewH,
		rotationDeg,
		tint
	);
}

bool IsCyclicSelect(const uid_node_def_t &node)
{
	return node.kind == UID_NODE_SELECT && node.appearance == "cyclic";
}

bool IsDropdownSelect(const uid_node_def_t &node)
{
	return node.kind == UID_NODE_SELECT && node.appearance != "cyclic";
}

/* Added in OPM: closed dropdown field = value label + trailing caret. */
void PaintDropdownSelect(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend, float opacityMul)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st || !backend) {
		return;
	}

	const uid_rect_t box = st->contentBox;
	if (box.w <= 0.0f || box.h <= 0.0f) {
		return;
	}

	const float caretW = UID_ScaleAuthoredPx(doc, 18.0f);
	const float padX = UID_ScaleAuthoredPx(doc, 8.0f);
	const float labelW = std::max(0.0f, box.w - caretW - padX);

	uid_color_t fg = {1.0f, 1.0f, 1.0f, 1.0f};
	UID_ResolveTextColor(doc, id, &fg);
	float rgba[4];
	ColorToRgba(fg, rgba, opacityMul);

	const std::string text = UID_NodeDisplayText(doc, id);
	void *font = ResolveFont(doc, *node, backend, st);
	if (!text.empty() && font && backend->fontDraw && labelW > 0.0f) {
		DropShadowPaintScope shadowScope(backend, doc, *node, st->effectiveClip);
		uid_node_state_t labelSt = *st;
		labelSt.contentBox = {box.x + padX * 0.25f, box.y, labelW, box.h};
		float x = labelSt.contentBox.x;
		float y = labelSt.contentBox.y;
		ComputeTextDrawOrigin(doc, *node, labelSt, text.c_str(), font, backend, &x, &y);

		float tracking = 0.0f;
		const char *trackProp = node->properties.GetCStr("letter-spacing", nullptr);
		if (trackProp && trackProp[0]) {
			uid_length_t len;
			len.unit = UID_LENGTH_PX;
			len.value = 0.0f;
			if (UID_ParseLength(trackProp, &len, nullptr) && len.unit == UID_LENGTH_PX) {
				tracking = UID_ScaleAuthoredPx(doc, len.value);
			}
		}
		PaintTextGlyphs(
			backend,
			font,
			doc,
			*node,
			x,
			y,
			text.c_str(),
			rgba,
			opacityMul,
			0.0f,
			box.y + box.h * 0.5f,
			tracking
		);
	}

	if (!font || !backend->fontDraw) {
		/* Still try caret shape below. */
	}

	/* Added in OPM: fonts are ASCII-only — UTF-8 ▾ becomes "???"; draw a triangle path. */
	if (backend->drawPath) {
		const float tw = UID_ScaleAuthoredPx(doc, 10.0f);
		const float th = UID_ScaleAuthoredPx(doc, 6.0f);
		const float cx = box.x + box.w - caretW * 0.5f;
		const float cy = box.y + box.h * 0.5f;
		char pathBuf[96];
		std::snprintf(
			pathBuf,
			sizeof(pathBuf),
			"M 0 0 L %.4f 0 L %.4f %.4f Z",
			static_cast<double>(tw),
			static_cast<double>(tw * 0.5f),
			static_cast<double>(th)
		);
		uid_color_t caretFg = fg;
		caretFg.a *= 0.85f;
		float caretRgba[4];
		ColorToRgba(caretFg, caretRgba, opacityMul);
		backend->drawPath(
			pathBuf,
			cx - tw * 0.5f,
			cy - th * 0.35f,
			tw,
			th,
			tw,
			th,
			caretRgba,
			nullptr,
			0.0f,
			0.0f,
			1
		);
	}
}

int CyclicOptionIndex(const uid_node_def_t &node, const uid_node_state_t &st)
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

/* Added in OPM: HTML-parity cyclic select (chevrons + centered value + ticks). */
void PaintCyclicSelect(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st || !backend) {
		return;
	}

	const uid_rect_t box = st->contentBox;
	if (box.w <= 0.0f || box.h <= 0.0f) {
		return;
	}

	const float chevronW = UID_ScaleAuthoredPx(doc, 32.0f);
	const float padX = UID_ScaleAuthoredPx(doc, 16.0f);
	const float padY = UID_ScaleAuthoredPx(doc, 6.0f);
	const float gap = UID_ScaleAuthoredPx(doc, 4.0f);
	const float tickRowH = UID_ScaleAuthoredPx(doc, 5.0f);
	const float tickInactiveH = UID_ScaleAuthoredPx(doc, 3.0f);
	const float tickActiveH = UID_ScaleAuthoredPx(doc, 5.0f);
	const float tickGap = UID_ScaleAuthoredPx(doc, 2.0f);
	const float divW = UID_ScaleAuthoredPx(doc, 1.0f);

	uid_color_t chevronBg = {0.0f, 0.0f, 0.0f, 0.28f};
	uid_color_t chevronFg = {235.0f / 255.0f, 240.0f / 255.0f, 245.0f / 255.0f, 0.82f};
	uid_color_t divColor = {1.0f, 1.0f, 1.0f, 0.12f};
	uid_color_t tickOff = {1.0f, 1.0f, 1.0f, 0.22f};
	uid_color_t tickOn = {26.0f / 255.0f, 111.0f / 255.0f, 212.0f / 255.0f, 1.0f};
	uid_color_t valueFg = {1.0f, 1.0f, 1.0f, 1.0f};
	UID_ResolveTextColor(doc, id, &valueFg);

	const float leftW = std::min(chevronW, box.w * 0.5f);
	const float rightW = leftW;
	DrawSolid(backend, {box.x, box.y, leftW, box.h}, chevronBg);
	DrawSolid(backend, {box.x + box.w - rightW, box.y, rightW, box.h}, chevronBg);
	if (divW > 0.0f && box.w > leftW + rightW) {
		DrawSolid(backend, {box.x + leftW, box.y, divW, box.h}, divColor);
		DrawSolid(backend, {box.x + box.w - rightW - divW, box.y, divW, box.h}, divColor);
	}

	void *font = ResolveFont(doc, *node, backend, st);
	const float fontPx = FontLogicalPx(doc, *node);
	float chevronRgba[4];
	ColorToRgba(chevronFg, chevronRgba);
	if (font && backend->fontDraw) {
		const char *prevGlyph = "\xE2\x80\xB9"; /* ‹ */
		const char *nextGlyph = "\xE2\x80\xBA"; /* › */
		float glyphW = 10.0f;
		const float chevronFontPx = UID_ScaleAuthoredPx(doc, 20.0f);
		void *chevronFont = font;
		if (backend->fontResolve) {
			const char *fontId = PropCStr(*node, "font", "control");
			int weight = 400;
			{
				double w = 400.0;
				const char *fw = PropCStr(*node, "font-weight", "400");
				if (fw) {
					UID_ParseNumber(fw, &w, nullptr);
				}
				weight = static_cast<int>(w);
			}
			const uid_font_def_t *fontDef = UID_FindFontDef(doc, fontId, weight);
			if (fontDef) {
				const float fbScale = (doc && doc->lastFbScale > 0.0f) ? doc->lastFbScale : 1.0f;
				void *resolved = backend->fontResolve(fontDef->src.c_str(), chevronFontPx, fbScale);
				if (resolved) {
					chevronFont = resolved;
				}
			}
		}
		if (backend->fontMeasure) {
			glyphW = backend->fontMeasure(chevronFont, prevGlyph);
		}
		float ascent = chevronFontPx;
		if (backend->fontAscent) {
			const float a = backend->fontAscent(chevronFont);
			if (a > 0.0f) {
				ascent = a;
			}
		}
		const float prevX = box.x + (leftW - glyphW) * 0.5f;
		const float nextX = box.x + box.w - rightW + (rightW - glyphW) * 0.5f;
		const float glyphY = box.y + box.h * 0.5f - ascent * 0.62f;
		backend->fontDraw(chevronFont, prevX, glyphY, prevGlyph, chevronRgba, 0.0f);
		backend->fontDraw(chevronFont, nextX, glyphY, nextGlyph, chevronRgba, 0.0f);
	}

	const float bodyX = box.x + leftW + padX;
	const float bodyW = std::max(0.0f, box.w - leftW - rightW - padX * 2.0f);
	const float bodyY = box.y + padY;
	const float bodyH = std::max(0.0f, box.h - padY * 2.0f);

	const int optIdx = CyclicOptionIndex(*node, *st);
	std::string label;
	if (optIdx >= 0) {
		const uid_select_option_t &opt = node->options[static_cast<size_t>(optIdx)];
		label = opt.label.empty() ? opt.value : opt.label;
	}

	float tracking = 0.0f;
	const char *trackProp = node->properties.GetCStr("letter-spacing", nullptr);
	if (trackProp && trackProp[0]) {
		uid_length_t len;
		len.unit = UID_LENGTH_PX;
		len.value = 0.0f;
		if (UID_ParseLength(trackProp, &len, nullptr) && len.unit == UID_LENGTH_PX) {
			tracking = UID_ScaleAuthoredPx(doc, len.value);
		}
	}

	float valueRgba[4];
	ColorToRgba(valueFg, valueRgba);
	if (!label.empty() && font && backend->fontDraw && bodyW > 0.0f) {
		float textW = 0.0f;
		if (backend->fontMeasure) {
			textW = backend->fontMeasure(font, label.c_str());
			if (tracking != 0.0f && label.size() > 1) {
				textW += tracking * static_cast<float>(label.size() - 1);
			}
		}
		float ascent = fontPx;
		if (backend->fontAscent) {
			const float a = backend->fontAscent(font);
			if (a > 0.0f) {
				ascent = a;
			}
		}
		const float valueH = bodyH - tickRowH - gap;
		const float textX = bodyX + std::max(0.0f, (bodyW - textW) * 0.5f);
		const float textY = bodyY + std::max(0.0f, valueH * 0.5f - ascent * 0.62f);
		PushClip(backend, {bodyX, bodyY, bodyW, std::max(0.0f, valueH)});
		backend->fontDraw(font, textX, textY, label.c_str(), valueRgba, tracking);
		PopClip(backend);
	}

	const size_t nTicks = node->options.size();
	if (nTicks > 0 && bodyW > 0.0f && backend->drawSolidRect) {
		const float ticksY = bodyY + bodyH - tickRowH;
		const float totalGap = tickGap * static_cast<float>(nTicks > 1 ? nTicks - 1 : 0);
		const float tickW = std::max(1.0f, (bodyW - totalGap) / static_cast<float>(nTicks));
		for (size_t i = 0; i < nTicks; ++i) {
			const bool active = (static_cast<int>(i) == optIdx);
			const float h = active ? tickActiveH : tickInactiveH;
			const float x = bodyX + static_cast<float>(i) * (tickW + tickGap);
			const float y = ticksY + (tickRowH - h) * 0.5f;
			DrawSolid(backend, {x, y, tickW, h}, active ? tickOn : tickOff);
		}
	}
}

void PaintChromeNode(
	uid_document_t      *doc,
	uid_node_id_t         id,
	const uid_backend_t *backend,
	bool                  ancestorVisible,
	float                 parentOpacity = 1.0f
)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st || !IsPaintKind(node->kind)) {
		return;
	}

	const bool visible = ancestorVisible && PropBool(*node, "visible", true);
	if (!visible) {
		return;
	}

	const float effectiveOpacity = parentOpacity * NodeOpacity(*node) * st->lifetimeOpacityMul;
	if (effectiveOpacity <= 0.001f) {
		return;
	}

	/* Added in OPM: descendant clips are intersections of this clip, so an empty
	   clip means the whole subtree is invisible. */
	if (UID_OptEnabled(UID_OPT_PAINT_CULL)) {
		if (st->effectiveClip.w <= 0.0f || st->effectiveClip.h <= 0.0f) {
			return;
		}
	}

	/*
	 * Fixed in OPM: windowed-foreach overscan rows sit below overflow=scroll
	 * viewports with overflow=none, so effectiveClip stays the full viewport
	 * (non-empty). They used to keep recursing and paint 1px row dividers past
	 * the list (and past the parent panel). Skip the whole subtree when the
	 * border box misses the clip; in-flow children cannot be visible then.
	 */
	bool skipSelfDraw = false;
	if (UID_OptEnabled(UID_OPT_PAINT_CULL)) {
		const float pad = 4.0f;
		const float bx0 = st->borderBox.x - pad;
		const float by0 = st->borderBox.y - pad;
		const float bx1 = st->borderBox.x + st->borderBox.w + pad;
		const float by1 = st->borderBox.y + st->borderBox.h + pad;
		const float cx0 = st->effectiveClip.x;
		const float cy0 = st->effectiveClip.y;
		const float cx1 = st->effectiveClip.x + st->effectiveClip.w;
		const float cy1 = st->effectiveClip.y + st->effectiveClip.h;
		if (bx1 <= cx0 || bx0 >= cx1 || by1 <= cy0 || by0 >= cy1) {
			return;
		}
		/* Tight miss without pad: skip self but still allow edge halo / children. */
		if (st->borderBox.x + st->borderBox.w <= cx0 || st->borderBox.x >= cx1
			|| st->borderBox.y + st->borderBox.h <= cy0 || st->borderBox.y >= cy1) {
			skipSelfDraw = true;
		}
	}

	PushClip(backend, st->effectiveClip);

	bool imageMaskActive = false;
	if (!skipSelfDraw && backend->beginImageMask && backend->endImageMask &&
	    (node->kind == UID_NODE_CONTAINER || node->kind == UID_NODE_BUTTON || node->kind == UID_NODE_FOREACH)) {
		std::string maskSpec;
		if (ResolveMaskImageSpec(doc, *node, backend, &maskSpec)) {
			uid_image_fit_t maskFit = UID_IMAGE_FIT_STRETCH;
			std::string fitStr;
			if (node->properties.Get("mask-fit", &fitStr) && !fitStr.empty()) {
				(void)UID_ParseImageFit(fitStr.c_str(), &maskFit, nullptr);
				if (maskFit == UID_IMAGE_FIT_REPEAT) {
					maskFit = UID_IMAGE_FIT_STRETCH;
				}
			}
			const uid_rect_t &box = st->borderBox;
			if (box.w > 0.0f && box.h > 0.0f) {
				imageMaskActive = backend->beginImageMask(
					box.x,
					box.y,
					box.w,
					box.h,
					maskSpec.c_str(),
					static_cast<int>(maskFit)
				);
			}
		}
	}

	if (!skipSelfDraw) {
		UID_PaintNodeBackground(doc, id, backend, effectiveOpacity);
		UID_PaintNodeContent(doc, id, backend, effectiveOpacity);
	}

	bool shapeChildClip = false;
	uid_rect_t clipGeom{};
	float clipViewW = 0.0f;
	float clipViewH = 0.0f;
	float clipRot = 0.0f;
	std::vector<std::string> clipPaths;
	std::vector<const char *> pathPtrs;
	auto beginChildShapeClip = [&]() -> bool {
		if (!backend->beginShapeClip || clipPaths.empty()) {
			return false;
		}
		pathPtrs.clear();
		pathPtrs.reserve(clipPaths.size());
		for (const std::string &d : clipPaths) {
			pathPtrs.push_back(d.c_str());
		}
		return backend->beginShapeClip(
			clipGeom.x,
			clipGeom.y,
			clipGeom.w,
			clipGeom.h,
			pathPtrs.data(),
			static_cast<int>(pathPtrs.size()),
			clipViewW,
			clipViewH,
			clipRot
		);
	};
	if (backend->beginShapeClip && backend->endShapeClip &&
	    (node->kind == UID_NODE_CONTAINER || node->kind == UID_NODE_BUTTON || node->kind == UID_NODE_FOREACH)) {
		if (ResolveShapeChildClip(doc, id, backend, &clipGeom, &clipViewW, &clipViewH, &clipRot, &clipPaths)) {
			shapeChildClip = beginChildShapeClip();
		}
	}

	std::vector<uid_node_id_t> scrollbarChildren;
	for (uid_node_id_t c : node->children) {
		const uid_node_def_t *child = UID_GetNode(doc, c);
		if (child && child->kind == UID_NODE_SCROLLBAR) {
			scrollbarChildren.push_back(c);
		} else {
			PaintChromeNode(doc, c, backend, true, effectiveOpacity);
		}
	}
	if (shapeChildClip && backend->endShapeClip) {
		backend->endShapeClip();
		shapeChildClip = false;
	}
	if (imageMaskActive && backend->endImageMask) {
		backend->endImageMask();
		imageMaskActive = false;
	}

	for (uid_node_id_t c : scrollbarChildren) {
		PopClip(backend);
		PushClip(backend, UID_ScrollbarChromeClip(node, st));
		PaintChromeNode(doc, c, backend, true, effectiveOpacity);
		PopClip(backend);
		PushClip(backend, st->effectiveClip);
	}

	if (node->kind == UID_NODE_CONTAINER) {
		uid_overflow_t ov = UID_OVERFLOW_NONE;
		int ovEnum = static_cast<int>(UID_OVERFLOW_NONE);
		if (node->properties.GetEnumCached("overflow", UID_PROP_ENUM_OVERFLOW, &ovEnum)) {
			ov = static_cast<uid_overflow_t>(ovEnum);
		} else {
			UID_ParseOverflow(PropCStr(*node, "overflow", "none"), &ov, nullptr);
		}
		if (ov == UID_OVERFLOW_SCROLL) {
			PopClip(backend);
			PushClip(backend, UID_ScrollbarChromeClip(node, st));
			UID_PaintScrollbarChrome(doc, id, backend);
			PopClip(backend);
			PushClip(backend, st->effectiveClip);
		}
	}
	PopClip(backend);
}

} // namespace

bool UID_ResolveFillColor(const uid_document_t *doc, uid_node_id_t id, uid_color_t *out, const uid_backend_t *backend)
{
	std::string gradient;
	uid_color_t solid{};
	if (!UID_ResolveFillPaint(doc, id, backend, &solid, &gradient)) {
		return false;
	}
	if (!gradient.empty()) {
		return false;
	}
	if (out) {
		*out = solid;
	}
	return true;
}

static bool TryFillPropString(const uid_node_def_t &node, const char *name, std::string *out)
{
	std::string v;
	if (!node.properties.Get(name, &v) || v.empty()) {
		return false;
	}
	if (!UID_IsFillPaint(v.c_str())) {
		return false;
	}
	*out = v;
	return true;
}

bool UID_ResolveFillPaint(
	const uid_document_t *doc,
	uid_node_id_t id,
	const uid_backend_t *backend,
	uid_color_t *outSolid,
	std::string *outGradient
)
{
	if (!doc) {
		return false;
	}
	const uid_node_def_t *node = UID_GetNode(doc, id);
	const uid_node_state_t *st = StateC(doc, id);
	if (!node || !st) {
		return false;
	}

	if (outSolid) {
		std::memset(outSolid, 0, sizeof(*outSolid));
	}
	if (outGradient) {
		outGradient->clear();
	}

	std::string fillStr;
	bool have = false;

	if (!st->effectivelyEnabled || !PropBool(*node, "enabled", true)) {
		have = TryFillPropString(*node, "disabled-fill", &fillStr);
	}
	if (!have && st->pressed) {
		have = TryFillPropString(*node, "pressed-fill", &fillStr);
	}
	if (!have && st->hovered) {
		have = TryFillPropString(*node, "hoverfill", &fillStr) ||
			TryFillPropString(*node, "hover-fill", &fillStr);
	}
	if (!have && st->focused) {
		have = TryFillPropString(*node, "focus-fill", &fillStr);
	}
	if (!have) {
		const char *fill = PropCStr(*node, "fill", "#00000000");
		if (fill && fill[0]) {
			fillStr = fill;
			have = true;
		}
	}
	if (!have) {
		return false;
	}

	if (UID_IsGradientBrush(fillStr.c_str())) {
		if (outGradient) {
			*outGradient = fillStr;
		}
		return true;
	}

	uid_color_t solid{};
	if (backend && fillStr.compare(0, 10, "cvar-rgba:") == 0) {
		if (!UID_ResolveCvarRgba(backend, fillStr.c_str() + 10, &solid)) {
			return false;
		}
	} else if (!UID_ParseColor(fillStr.c_str(), &solid, nullptr)) {
		return false;
	}
	if (outSolid) {
		*outSolid = solid;
	}
	return true;
}

bool UID_ResolveTextColor(const uid_document_t *doc, uid_node_id_t id, uid_color_t *out)
{
	if (!doc || !out) {
		return false;
	}
	const uid_node_def_t *node = UID_GetNode(doc, id);
	const uid_node_state_t *st = StateC(doc, id);
	if (!node || !st) {
		return false;
	}

	out->r = out->g = out->b = 1.0f;
	out->a = 1.0f;

	if (!st->effectivelyEnabled || !PropBool(*node, "enabled", true)) {
		if (ParseColorProp(*node, "disabled-color", out)) {
			return true;
		}
	}
	if (st->pressed && ParseColorProp(*node, "pressed-color", out)) {
		return true;
	}
	if (st->hovered && ParseColorProp(*node, "hover-color", out)) {
		return true;
	}
	if (st->focused && ParseColorProp(*node, "focus-color", out)) {
		return true;
	}

	const char *color = PropCStr(*node, "color", "#FFFFFFFF");
	return UID_ParseColor(color, out, nullptr);
}

std::string UID_NodeDisplayText(const uid_document_t *doc, uid_node_id_t id)
{
	const uid_node_def_t *node = UID_GetNode(doc, id);
	const uid_node_state_t *st = StateC(doc, id);
	if (!node) {
		return std::string();
	}

	if (node->kind == UID_NODE_INPUT && st && st->focused) {
		return st->editBuffer;
	}
	/* Select: map bound value → option label (do not return raw cvar early). */
	if (node->kind == UID_NODE_SELECT) {
		if (st && !node->options.empty() && node->appearance == "cyclic") {
			const int idx = CyclicOptionIndex(*node, *st);
			if (idx >= 0) {
				const uid_select_option_t &opt = node->options[static_cast<size_t>(idx)];
				return opt.label.empty() ? opt.value : opt.label;
			}
		}
		if (st && st->runtimeValue.hasValue) {
			const std::string &val = st->runtimeValue.stringValue;
			for (const uid_select_option_t &opt : node->options) {
				if (opt.value == val) {
					return opt.label.empty() ? opt.value : opt.label;
				}
			}
			return val;
		}
		if (!node->options.empty()) {
			return node->options.front().label.empty() ? node->options.front().value
													  : node->options.front().label;
		}
		return std::string();
	}
	if (node->kind == UID_NODE_KEYBIND) {
		if (st && st->capturing) {
			return UID_KeybindCaptureLabel(*node);
		}
		if (st && st->runtimeValue.hasValue && !st->runtimeValue.stringValue.empty()) {
			return st->runtimeValue.stringValue;
		}
		return UID_KeybindEmptyLabel(*node);
	}
	if (node->kind == UID_NODE_BUTTON) {
		return node->text;
	}
	if (st && st->runtimeValue.hasValue && !st->runtimeValue.stringValue.empty()) {
		return st->runtimeValue.stringValue;
	}
	if (node->kind == UID_NODE_INPUT && st && !st->editBuffer.empty()) {
		return st->editBuffer;
	}
	if (node->kind == UID_NODE_INPUT) {
		const char *ph = PropCStr(*node, "placeholder", nullptr);
		if (ph && ph[0]) {
			return std::string(ph);
		}
	}
	if (st && st->runtimeValue.hasValue) {
		return st->runtimeValue.stringValue;
	}
	if (!node->text.empty()) {
		return node->text;
	}
	if (node->kind == UID_NODE_TOGGLE) {
		const bool on = st && st->runtimeValue.hasValue &&
			(st->runtimeValue.stringValue == "true" || st->runtimeValue.stringValue == "1");
		return on ? "On" : "Off";
	}
	return std::string();
}

void UID_PaintNodeBackground(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend, float opacityMul)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st || !backend || opacityMul <= 0.001f) {
		return;
	}

	/*
	 * Fixed in OPM: if fill is still an unresolved style ternary, evaluate it
	 * before shape resolve. skew-rect paths use fill="{parent.fill}" which must
	 * ParseColor or ResolveShape fails and stroke is dropped.
	 */
	{
		const char *fillCur = PropCStr(*node, "fill", nullptr);
		uid_color_t probe{};
		const bool knownPaint = fillCur && (UID_ParseColor(fillCur, &probe, nullptr) || UID_IsGradientBrush(fillCur) ||
			std::strncmp(fillCur, "cvar-rgba:", 10) == 0);
		if (!knownPaint && !node->styleExprs.empty()) {
			auto sit = node->styleExprs.find("fill");
			if (sit != node->styleExprs.end() && !sit->second.empty()) {
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
				if (UID_EvalStyleTernary(expr.c_str(), &bctx, nullptr, &resolved, &diag)) {
					node->properties.Set("fill", resolved.c_str());
				}
			}
		}
	}

	uid_color_t fill{};
	std::string gradientBrush;
	const bool resolvedPaint = UID_ResolveFillPaint(doc, id, backend, &fill, &gradientBrush);
	const bool hasGradient = resolvedPaint && !gradientBrush.empty();
	const bool hasFill = resolvedPaint && !hasGradient && fill.a > 0.0f;

	/* Added in OPM: element-owned stroke drilled into shape path draw. */
	uid_color_t stroke{};
	float strokeWidthPx = 0.0f;
	bool hasStroke = false;
	{
		const char *strokeStr = PropCStr(*node, "stroke", nullptr);
		if (strokeStr && strokeStr[0]) {
			std::string dm;
			if (UID_ParseColor(strokeStr, &stroke, &dm) && stroke.a > 0.0f) {
				std::string widthStr = PropCStr(*node, "stroke-width", "1px");
				if (backend) {
					std::string resolved;
					if (UID_ResolvePropString(backend, widthStr, &resolved)) {
						widthStr = resolved;
					}
				}
				uid_length_t wLen{};
				if (UID_ParseLength(widthStr.c_str(), &wLen, &dm) && wLen.unit == UID_LENGTH_PX && wLen.value > 0.0f) {
					strokeWidthPx = UID_ScaleAuthoredPx(doc, wLen.value);
					hasStroke = strokeWidthPx > 0.0f;
				}
			}
		}
	}

	const char *shapeName = PropCStr(*node, "shape", "rectangle");
	const bool isEdgeClip = shapeName && std::strcmp(shapeName, "edge-clip") == 0;
	const bool rectShape = IsDefaultRectShape(*node) || !shapeName || !shapeName[0] ||
		doc->definitions.shapes.find(shapeName) == doc->definitions.shapes.end();

	float pathRotationDeg = 0.0f;
	{
		const char *rotStr = PropCStr(*node, "shape-rotation", nullptr);
		if (rotStr && rotStr[0]) {
			(void)UID_ParseRotationDeg(rotStr, &pathRotationDeg, nullptr);
		}
	}
	float bgRotationDeg = 0.0f;
	{
		const char *rotStr = PropCStr(*node, "rotation", nullptr);
		if (!rotStr || !rotStr[0]) {
			rotStr = PropCStr(*node, "shape-rotation", nullptr);
		}
		if (rotStr && rotStr[0]) {
			(void)UID_ParseRotationDeg(rotStr, &bgRotationDeg, nullptr);
		}
	}
	const bool rotateShape = std::fabs(pathRotationDeg) > 1e-6f;
	const bool usePathPaint = !rectShape || rotateShape || isEdgeClip;

	auto formatColor = [](const uid_color_t &c, char *buf, size_t bufSize) {
		std::snprintf(
			buf,
			bufSize,
			"#%02X%02X%02X%02X",
			static_cast<unsigned>(std::min(255.0f, c.r * 255.0f + 0.5f)),
			static_cast<unsigned>(std::min(255.0f, c.g * 255.0f + 0.5f)),
			static_cast<unsigned>(std::min(255.0f, c.b * 255.0f + 0.5f)),
			static_cast<unsigned>(std::min(255.0f, c.a * 255.0f + 0.5f))
		);
	};

	float strokeRgba[4];
	const float *strokePtr = nullptr;
	if (hasStroke) {
		ColorToRgba(stroke, strokeRgba, opacityMul);
		strokePtr = strokeRgba;
	}

	/*
	 * Changed in OPM: authored size (incl. width=100%) is the outer box that
	 * includes stroke. Fill/shape geometry is inset by stroke-width; outside-
	 * aligned stroke then sits in that margin and is not clipped.
	 */
	uid_rect_t geom = st->borderBox;
	if (hasStroke && strokeWidthPx > 0.0f) {
		float inset = strokeWidthPx;
		if (inset * 2.0f > geom.w) {
			inset = geom.w * 0.5f;
		}
		if (inset * 2.0f > geom.h) {
			inset = std::min(inset, geom.h * 0.5f);
		}
		geom.x += inset;
		geom.y += inset;
		geom.w -= inset * 2.0f;
		geom.h -= inset * 2.0f;
		if (geom.w < 0.0f) {
			geom.w = 0.0f;
		}
		if (geom.h < 0.0f) {
			geom.h = 0.0f;
		}
	}

	ApplyBgRotationOrigin(geom, st->borderBox, bgRotationDeg, *node, doc);
	PaintBackgroundImage(
		doc,
		id,
		*node,
		geom,
		usePathPaint,
		shapeName,
		rectShape,
		bgRotationDeg,
		opacityMul,
		backend
	);

	if (hasGradient && geom.w > 0.0f && geom.h > 0.0f) {
		PaintGradientFill(
			doc,
			id,
			*node,
			geom,
			gradientBrush,
			usePathPaint,
			shapeName,
			rectShape,
			pathRotationDeg,
			opacityMul,
			backend
		);
	}

	if (hasFill || hasStroke) {
		if (!usePathPaint) {
			if (hasFill && geom.w > 0.0f && geom.h > 0.0f) {
				DrawSolid(backend, geom, fill, opacityMul);
			}
			/*
			 * Rectangle stroke frame via solid edge quads (not path fill).
			 * Large panel stroke AABBs used to exceed UIR_MAX_POLYGON_AREA and
			 * vanish, leaving a see-through margin that looked "transparent".
			 */
			if (hasStroke && strokeWidthPx > 0.0f && st->borderBox.w > 0.0f && st->borderBox.h > 0.0f) {
				const uid_rect_t &outer = st->borderBox;
				const float top = geom.y - outer.y;
				const float left = geom.x - outer.x;
				const float right = (outer.x + outer.w) - (geom.x + geom.w);
				const float bottom = (outer.y + outer.h) - (geom.y + geom.h);
				if (top > 0.0f) {
					DrawSolid(backend, {outer.x, outer.y, outer.w, top}, stroke, opacityMul);
				}
				if (bottom > 0.0f) {
					DrawSolid(backend, {outer.x, geom.y + geom.h, outer.w, bottom}, stroke, opacityMul);
				}
				if (left > 0.0f && geom.h > 0.0f) {
					DrawSolid(backend, {outer.x, geom.y, left, geom.h}, stroke, opacityMul);
				}
				if (right > 0.0f && geom.h > 0.0f) {
					DrawSolid(backend, {geom.x + geom.w, geom.y, right, geom.h}, stroke, opacityMul);
				}
			}
		} else if (!isEdgeClip || hasFill || hasStroke || hasGradient) {
			const std::string pathShapeId = rectShape ? "rectangle" : std::string(shapeName);
			auto sit = doc->definitions.shapes.find(pathShapeId);
			if (sit != doc->definitions.shapes.end()) {
			uid_shape_resolve_params_t params{};
			float viewW = geom.w;
			float viewH = geom.h;
			if (sit->second.hasIntrinsicSize && sit->second.width > 0.0f && sit->second.height > 0.0f) {
				params.parentWidth = sit->second.width;
				params.parentHeight = sit->second.height;
				viewW = sit->second.width;
				viewH = sit->second.height;
			} else {
				params.parentWidth = geom.w;
				params.parentHeight = geom.h;
			}
			/*
			 * Fixed in OPM: intrinsic viewbox props must not also take uiPxScale —
			 * layout already sized geom and SvgMap stretches view→dest (else uiPxScale²).
			 * Owner-sized shapes (view == geom) still scale props via uiPxScale.
			 */
			if (sit->second.hasIntrinsicSize && (viewW != geom.w || viewH != geom.h)) {
				params.uiPxScale = 1.0f;
			} else {
				params.uiPxScale = (doc->lastUiPxScale > 0.0f) ? doc->lastUiPxScale : 1.0f;
			}
			params.shapeProps = &node->properties;
			params.limits = &doc->limits;
			params.backend = backend;
			params.doc = doc;
			params.nodeId = id;
			params.parentProps = &node->properties;

			/*
			 * Fixed in OPM: parent.fill must always be a parseable color for shape
			 * paths (fill="{parent.fill}"). Unresolved ternaries used to make
			 * ResolveShape fail and drop stroke entirely.
			 * Added in OPM: overrides avoid cloning the full property map.
			 */
			char fillBuf[32];
			char strokeBuf[32];
			std::string widthStr;
			{
				uid_color_t parentFillColor{};
				const char *fp = node->properties.GetCStr("fill", "#00000000");
				if (hasGradient) {
					/* Added in OPM: gradient is drawn via atlas; shape fill stays transparent. */
					std::snprintf(fillBuf, sizeof(fillBuf), "#00000000");
				} else if (hasFill) {
					formatColor(fill, fillBuf, sizeof(fillBuf));
				} else if (fp && UID_ParseColor(fp, &parentFillColor, nullptr)) {
					formatColor(parentFillColor, fillBuf, sizeof(fillBuf));
				} else {
					std::snprintf(fillBuf, sizeof(fillBuf), "#00000000");
				}
				params.fillOverride = fillBuf;
			}
			if (hasStroke) {
				formatColor(stroke, strokeBuf, sizeof(strokeBuf));
				params.strokeOverride = strokeBuf;
				char widthBuf[32];
				std::snprintf(widthBuf, sizeof(widthBuf), "%gpx", strokeWidthPx / ((doc->lastUiPxScale > 0.0f) ? doc->lastUiPxScale : 1.0f));
				widthStr = PropCStr(*node, "stroke-width", widthBuf);
				if (backend) {
					std::string resolved;
					if (UID_ResolvePropString(backend, widthStr, &resolved)) {
						widthStr = resolved;
					}
				}
				params.strokeWidthOverride = widthStr.c_str();
			}

			const std::vector<uid_resolved_path_t> *paths = nullptr;
			const bool shapeOk =
				ResolveShapeCached(doc, id, st, &sit->second, &params, pathShapeId.c_str(), &paths) &&
				paths && !paths->empty();
			if (!shapeOk) {
				if (hasFill && geom.w > 0.0f && geom.h > 0.0f) {
					DrawSolid(backend, geom, fill, opacityMul);
				}
			} else if (!backend->drawPath) {
				if (hasFill && geom.w > 0.0f && geom.h > 0.0f) {
					DrawSolid(backend, geom, fill, opacityMul);
				}
			} else {
				for (const uid_resolved_path_t &p : *paths) {
					float fillRgba[4];
					const float *fillPtr = nullptr;
					if (p.fill.a > 0.0f) {
						ColorToRgba(p.fill, fillRgba, opacityMul);
						fillPtr = fillRgba;
					}
					if (!fillPtr && !strokePtr) {
						continue;
					}
					/* Added in OPM: crisp disables soft path AA (crosshair pixel marks). */
					const int crisp = PropBool(*node, "crisp", false) ? 1 : 0;
					backend->drawPath(
						p.d.c_str(),
						geom.x,
						geom.y,
						geom.w,
						geom.h,
						viewW,
						viewH,
						fillPtr,
						strokePtr,
						strokeWidthPx,
						pathRotationDeg,
						crisp
					);
				}
			}
			} else if (hasFill && geom.w > 0.0f && geom.h > 0.0f) {
				DrawSolid(backend, geom, fill, opacityMul);
			}
		}
	}
}

void UID_PaintNodeContent(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend, float opacityMul)
{
	uid_node_def_t *node = UID_GetNode(doc, id);
	uid_node_state_t *st = State(doc, id);
	if (!node || !st || !backend || opacityMul <= 0.001f) {
		return;
	}

	if (node->kind == UID_NODE_CONTAINER || node->kind == UID_NODE_SHAPE_INSTANCE) {
		if (node->kind == UID_NODE_CONTAINER && !node->role.empty() && backend->drawHostRegion) {
			backend->drawHostRegion(
				node->role.c_str(),
				st->contentBox.x,
				st->contentBox.y,
				st->contentBox.w,
				st->contentBox.h,
				backend->userdata
			);
		}
		return;
	}

	/* Added in OPM: model preview queues into compositor via host hook */
	if (node->kind == UID_NODE_MODEL) {
		if (!backend->queueModelPreview) {
			return;
		}
		uid_model_preview_desc_t desc {};
		std::string              modelName;
		std::string              anim = node->anim;

		if (st->runtimeValue.hasValue) {
			modelName = st->runtimeValue.stringValue;
		} else if (!node->bind.empty() && backend->cvarDescribe) {
			std::string cvarName;
			if (UID_ParseCvarBind(node->bind.c_str(), &cvarName)) {
				char buf[256];
				if (backend->cvarDescribe(cvarName.c_str(), nullptr, buf, sizeof(buf))) {
					modelName = buf;
				}
			}
		}
		if (modelName.empty() && !node->modelPath.empty()) {
			modelName = node->modelPath;
		}
		if (anim.empty() && !node->team.empty()) {
			anim = (node->team == "axis") ? "germanselectionidle" : "americanselectionidle";
		}

		desc.x = st->contentBox.x;
		desc.y = st->contentBox.y;
		desc.w = st->contentBox.w;
		desc.h = st->contentBox.h;
		desc.model = modelName.empty() ? nullptr : modelName.c_str();
		desc.anim = anim.empty() ? nullptr : anim.c_str();
		desc.team = node->team.empty() ? nullptr : node->team.c_str();
		desc.instanceKey = node->id.empty() ? nullptr : node->id.c_str();
		desc.animVariant = node->hasAnimVariant ? node->animVariant : 0;
		desc.animPhase = node->animPhase;
		desc.hasAnimPhase = node->hasAnimPhase ? 1 : 0;
		if (node->hasModelAngles) {
			desc.angles[0] = node->modelAngles[0];
			desc.angles[1] = node->modelAngles[1];
			desc.angles[2] = node->modelAngles[2];
			desc.hasAngles = 1;
		}
		if (node->hasModelOffset) {
			desc.offset[0] = node->modelOffset[0];
			desc.offset[1] = node->modelOffset[1];
			desc.offset[2] = node->modelOffset[2];
			desc.hasOffset = 1;
		}
		if (node->hasBbox) {
			desc.bboxMins[0] = node->bboxMins[0];
			desc.bboxMins[1] = node->bboxMins[1];
			desc.bboxMins[2] = node->bboxMins[2];
			desc.bboxMaxs[0] = node->bboxMaxs[0];
			desc.bboxMaxs[1] = node->bboxMaxs[1];
			desc.bboxMaxs[2] = node->bboxMaxs[2];
			desc.hasBbox = 1;
		}
		desc.bboxFromModel = node->bboxFromModel ? 1 : 0;
		if (node->hasModelFov) {
			desc.fov = node->modelFov;
			desc.hasFov = 1;
		}
		if (node->hasModelScale) {
			desc.scale = node->modelScale;
			desc.hasScale = 1;
		}
		if (node->hasFramingScale) {
			desc.framingScale = node->framingScale;
			desc.hasFramingScale = 1;
		}
		if (node->hasModelColor) {
			desc.color[0] = node->modelColor[0];
			desc.color[1] = node->modelColor[1];
			desc.color[2] = node->modelColor[2];
			desc.color[3] = node->modelColor[3];
			desc.hasColor = 1;
		}
		desc.userdata = backend->userdata;
		backend->queueModelPreview(&desc);
		return;
	}

	/* Added in OPM: host draws server-list header+body */
	if (node->kind == UID_NODE_SERVER_LIST) {
		if (backend->drawHostRegion) {
			const char *role = node->role.empty() ? "server-list" : node->role.c_str();
			backend->drawHostRegion(
				role,
				st->contentBox.x,
				st->contentBox.y,
				st->contentBox.w,
				st->contentBox.h,
				backend->userdata
			);
		}
		return;
	}

	uid_color_t color;
	UID_ResolveTextColor(doc, id, &color);
	float rgba[4];
	ColorToRgba(color, rgba, opacityMul);

	if (node->kind == UID_NODE_SLIDER) {
		const bool composed =
			UID_FindChildOfKind(doc, id, UID_NODE_SLIDER_TRACK) != UID_INVALID_NODE_ID ||
			UID_FindChildOfKind(doc, id, UID_NODE_SLIDER_RANGE) != UID_INVALID_NODE_ID ||
			UID_FindChildOfKind(doc, id, UID_NODE_SLIDER_THUMB) != UID_INVALID_NODE_ID;
		if (composed) {
			/* Parts paint as children via PaintChromeNode; sync thumb hover/press. */
			const uid_node_id_t thumbId = UID_FindChildOfKind(doc, id, UID_NODE_SLIDER_THUMB);
			if (thumbId != UID_INVALID_NODE_ID) {
				uid_node_state_t *thumbSt = State(doc, thumbId);
				if (thumbSt) {
					thumbSt->hovered = st->hovered || st->dragging;
					thumbSt->pressed = st->pressed || st->dragging;
				}
			}
			return;
		}

		const uid_rect_t track = {
			st->contentBox.x,
			st->contentBox.y + st->contentBox.h * 0.4f,
			st->contentBox.w,
			std::max(4.0f, st->contentBox.h * 0.2f)
		};
		uid_color_t trackColor = {0.3f, 0.3f, 0.3f, 1.0f};
		DrawSolid(backend, track, trackColor, opacityMul);

		double minV = node->hasMin ? node->minValue : 0.0;
		double maxV = node->hasMax ? node->maxValue : 1.0;
		double val = minV;
		if (st->runtimeValue.hasValue) {
			UID_ParseNumber(st->runtimeValue.stringValue.c_str(), &val, nullptr);
		}
		if (maxV <= minV) {
			maxV = minV + 1.0;
		}
		const float t = static_cast<float>((val - minV) / (maxV - minV));
		const float thumbX = track.x + std::min(1.0f, std::max(0.0f, t)) * track.w;
		DrawSolid(backend, {thumbX - 4.0f, st->contentBox.y, 8.0f, st->contentBox.h}, color, opacityMul);
		return;
	}

	if (node->kind == UID_NODE_SLIDER_TRACK || node->kind == UID_NODE_SLIDER_RANGE ||
		node->kind == UID_NODE_SLIDER_THUMB) {
		/* Background/shape only — painted via UID_PaintNodeBackground in chrome walk. */
		return;
	}

	if (node->kind == UID_NODE_SCROLLBAR) {
		return;
	}

	if (node->kind == UID_NODE_SCROLLBAR_TRACK || node->kind == UID_NODE_SCROLLBAR_THUMB) {
		return;
	}

	if (node->kind == UID_NODE_TOGGLE) {
		const bool on = st->runtimeValue.hasValue &&
			(st->runtimeValue.stringValue == "true" || st->runtimeValue.stringValue == "1");
		uid_color_t box = on ? color : uid_color_t{0.25f, 0.25f, 0.25f, 1.0f};
		const float s = std::min(st->contentBox.w, st->contentBox.h);
		DrawSolid(backend, {st->contentBox.x, st->contentBox.y, s, s}, box, opacityMul);
		return;
	}

	/* Added in OPM: cyclic select paints its own chrome (not dropdown field text). */
	if (IsCyclicSelect(*node)) {
		PaintCyclicSelect(doc, id, backend);
		return;
	}
	/* Added in OPM: dropdown select paints value + trailing caret. */
	if (IsDropdownSelect(*node)) {
		PaintDropdownSelect(doc, id, backend, opacityMul);
		return;
	}

	const std::string text = UID_NodeDisplayText(doc, id);
	const bool paintCaret = (node->kind == UID_NODE_INPUT && st->focused && backend->drawSolidRect);
	/* Empty focused inputs still need a caret; other empty leaves skip paint. */
	if (text.empty() && !paintCaret) {
		return;
	}

	void *font = ResolveFont(doc, *node, backend, st);
	if (!text.empty() && font && backend->fontDraw) {
		DropShadowPaintScope shadowScope(backend, doc, *node, st->effectiveClip);
		const uid_text_wrap_t wrapMode = UID_TextWrapMode(*node);
		const bool multiline = (wrapMode == UID_TEXT_WRAP_WORD) || (text.find('\n') != std::string::npos);

		if (!multiline) {
			float x = st->contentBox.x;
			float y = st->contentBox.y;
			ComputeTextDrawOrigin(doc, *node, *st, text.c_str(), font, backend, &x, &y);

			float skewTan = 0.0f;
			float tracking = 0.0f;
			const char *skewProp = node->properties.GetCStr("text-skew", nullptr);
			if (skewProp && skewProp[0]) {
				char *end = nullptr;
				const double deg = std::strtod(skewProp, &end);
				if (end != skewProp && std::isfinite(deg)) {
					skewTan = static_cast<float>(std::tan(deg * (3.14159265358979323846 / 180.0)));
				}
			}
			const char *trackProp = node->properties.GetCStr("letter-spacing", nullptr);
			if (trackProp && trackProp[0]) {
				uid_length_t len;
				len.unit = UID_LENGTH_PX;
				len.value = 0.0f;
				if (UID_ParseLength(trackProp, &len, nullptr) && len.unit == UID_LENGTH_PX) {
					tracking = UID_ScaleAuthoredPx(doc, len.value);
				}
			}

			/* Added in OPM: paint-time marquee (parent overflow=hidden clips). */
			enum { kMarqueeNone = 0, kMarqueeH = 1, kMarqueeV = 2 };
			int marqueeAxis = kMarqueeNone;
			const char *marqueeProp = node->properties.GetCStr("marquee", "none");
			if (marqueeProp) {
				if (std::strcmp(marqueeProp, "horizontal") == 0 || std::strcmp(marqueeProp, "x") == 0) {
					marqueeAxis = kMarqueeH;
				} else if (std::strcmp(marqueeProp, "vertical") == 0 || std::strcmp(marqueeProp, "y") == 0) {
					marqueeAxis = kMarqueeV;
				}
			}

			float textW = 0.0f;
			float textH = FontLogicalPx(doc, *node);
			if (backend->fontMeasure) {
				textW = backend->fontMeasure(font, text.c_str());
				if (tracking > 0.0f && text.size() > 1) {
					textW += tracking * static_cast<float>(text.size() - 1);
				}
			}
			if (backend->fontAscent) {
				const float asc = backend->fontAscent(font);
				if (asc > 0.0f) {
					textH = asc * 1.25f;
				}
			}

			float marqueeDx = 0.0f;
			float marqueeDy = 0.0f;
			bool marqueeLoop = false;
			float loopPitch = 0.0f;
			if (marqueeAxis != kMarqueeNone) {
				float speedPx = 40.0f;
				float gapPx = 48.0f;
				int delayMs = 0;
				const char *speedProp = node->properties.GetCStr("marquee-speed", nullptr);
				if (speedProp && speedProp[0]) {
					uid_length_t sl;
					sl.unit = UID_LENGTH_PX;
					sl.value = 0.0f;
					if (UID_ParseLength(speedProp, &sl, nullptr) && sl.unit == UID_LENGTH_PX) {
						speedPx = UID_ScaleAuthoredPx(doc, sl.value);
					}
				}
				const char *gapProp = node->properties.GetCStr("marquee-gap", nullptr);
				if (gapProp && gapProp[0]) {
					uid_length_t gl;
					gl.unit = UID_LENGTH_PX;
					gl.value = 0.0f;
					if (UID_ParseLength(gapProp, &gl, nullptr) && gl.unit == UID_LENGTH_PX) {
						gapPx = UID_ScaleAuthoredPx(doc, gl.value);
					}
				}
				const char *delayProp = node->properties.GetCStr("marquee-delay", nullptr);
				if (delayProp && delayProp[0]) {
					(void)UID_ParseDurationMs(delayProp, &delayMs, nullptr);
				}

				const bool overflow =
					(marqueeAxis == kMarqueeH && textW > st->contentBox.w + 0.5f) ||
					(marqueeAxis == kMarqueeV && textH > st->contentBox.h + 0.5f);
				if (overflow && speedPx > 0.0f) {
					marqueeLoop = true;
					loopPitch = (marqueeAxis == kMarqueeH ? textW : textH) + gapPx;
					int t = doc->updateTimeMs - delayMs;
					if (t < 0) {
						t = 0;
					}
					const float dist = (static_cast<float>(t) / 1000.0f) * speedPx;
					float phase = std::fmod(dist, loopPitch);
					if (phase < 0.0f) {
						phase += loopPitch;
					}
					if (marqueeAxis == kMarqueeH) {
						marqueeDx = -phase;
					} else {
						marqueeDy = -phase;
					}
				}
			}

			const float skewOriginY = st->contentBox.y + st->contentBox.h * 0.5f;
			PaintTextGlyphs(
				backend,
				font,
				doc,
				*node,
				x + marqueeDx,
				y + marqueeDy,
				text.c_str(),
				rgba,
				opacityMul,
				skewTan,
				skewOriginY,
				tracking
			);
			if (marqueeLoop) {
				PaintTextGlyphs(
					backend,
					font,
					doc,
					*node,
					x + marqueeDx + (marqueeAxis == kMarqueeH ? loopPitch : 0.0f),
					y + marqueeDy + (marqueeAxis == kMarqueeV ? loopPitch : 0.0f),
					text.c_str(),
					rgba,
					opacityMul,
					skewTan,
					skewOriginY,
					tracking
				);
			}
		} else {
		std::vector<std::string> lines;
		uid_text_block_metrics_t metrics{};
		const float wrapWidth = st->contentBox.w;
		UID_BuildTextLines(doc, *node, text.c_str(), wrapWidth, doc->lastFbScale, backend, &lines, &metrics);

		float skewTan = 0.0f;
		float tracking = 0.0f;
		const char *skewProp = node->properties.GetCStr("text-skew", nullptr);
		if (skewProp && skewProp[0]) {
			char *end = nullptr;
			const double deg = std::strtod(skewProp, &end);
			if (end != skewProp && std::isfinite(deg)) {
				skewTan = static_cast<float>(std::tan(deg * (3.14159265358979323846 / 180.0)));
			}
		}
		const char *trackProp = node->properties.GetCStr("letter-spacing", nullptr);
		if (trackProp && trackProp[0]) {
			uid_length_t len;
			len.unit = UID_LENGTH_PX;
			len.value = 0.0f;
			if (UID_ParseLength(trackProp, &len, nullptr) && len.unit == UID_LENGTH_PX) {
				tracking = UID_ScaleAuthoredPx(doc, len.value);
			}
		}

		uid_align_t halign;
		uid_align_t valign;
		ResolveTextAlign(*node, &halign, &valign);

		const float blockH = metrics.lineCount > 0 ? metrics.blockHeight : metrics.lineHeight;
		float blockY = st->contentBox.y;
		if (valign == UID_ALIGN_CENTER) {
			blockY = st->contentBox.y + (st->contentBox.h - blockH) * 0.5f;
		} else if (valign == UID_ALIGN_END) {
			blockY = st->contentBox.y + st->contentBox.h - blockH;
		}

		if (lines.empty()) {
			lines.push_back(std::string());
		}

		for (size_t i = 0; i < lines.size(); ++i) {
			const std::string &line = lines[i];
			float lineW = 0.0f;
			if (font && backend->fontMeasure) {
				lineW = backend->fontMeasure(font, line.c_str());
				if (tracking > 0.0f && line.size() > 1) {
					lineW += tracking * static_cast<float>(line.size() - 1);
				}
			}
			float x = st->contentBox.x;
			if (halign == UID_ALIGN_CENTER) {
				x = st->contentBox.x + (st->contentBox.w - lineW) * 0.5f;
			} else if (halign == UID_ALIGN_END) {
				x = st->contentBox.x + st->contentBox.w - lineW;
			}
			float y = blockY + metrics.lineHeight * static_cast<float>(i);

			const float skewOriginY = st->contentBox.y + st->contentBox.h * 0.5f;
			PaintTextGlyphs(
				backend,
				font,
				doc,
				*node,
				x,
				y,
				line.c_str(),
				rgba,
				opacityMul,
				skewTan,
				skewOriginY,
				tracking
			);
		}
		}
	} else if (!text.empty() && backend->drawSolidRect) {
		/* Fallback glyph bar when fonts are unavailable. */
		float x = st->contentBox.x;
		float y = st->contentBox.y;
		ComputeTextDrawOrigin(doc, *node, *st, text.c_str(), nullptr, backend, &x, &y);
		DrawSolid(
			backend,
			{x, y + FontLogicalPx(doc, *node) * 0.3f, std::min(st->contentBox.w, static_cast<float>(text.size()) * 8.0f), 2.0f},
			color
		);
	}

	if (paintCaret) {
		float x = st->contentBox.x;
		float y = st->contentBox.y;
		ComputeTextDrawOrigin(doc, *node, *st, text.c_str(), font, backend, &x, &y);
		const float caretX = x + MeasureCaretAdvance(doc, *node, text, st->caretCodepoint, font, backend);
		const float caretH = st->contentBox.h * 0.65f;
		const float caretY = st->contentBox.y + (st->contentBox.h - caretH) * 0.5f;
		DrawSolid(backend, {caretX, caretY, 2.0f, caretH}, color);
	}
}

void UID_PaintChrome(uid_document_t *doc, const uid_backend_t *backend)
{
	if (!doc || !backend) {
		return;
	}
	if (doc->states.size() != doc->nodes.size()) {
		doc->states.resize(doc->nodes.size());
		for (uid_node_state_t &st : doc->states) {
			UID_InitNodeState(&st);
		}
	}
	if (doc->rootNode != UID_INVALID_NODE_ID) {
		uid_node_id_t chromeRoot = doc->rootNode;
		auto mit = doc->idIndex.find("menu_root");
		if (mit != doc->idIndex.end()) {
			chromeRoot = mit->second;
		}
		PaintChromeNode(doc, chromeRoot, backend, true);
	}
	/*
	 * Fixed in OPM: modals paint in UID_PaintOverlay (after 3D model previews).
	 * Chrome still queues <model> previews; drawing the modal here put dropdowns
	 * under the player previews on the Profile panel.
	 */
	doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty & ~UID_DIRTY_PAINT);
}

void UID_PaintChromeSubtree(uid_document_t *doc, uid_node_id_t rootId, const uid_backend_t *backend)
{
	if (!doc || !backend || rootId == UID_INVALID_NODE_ID) {
		return;
	}
	if (doc->states.size() != doc->nodes.size()) {
		doc->states.resize(doc->nodes.size());
		for (uid_node_state_t &st : doc->states) {
			UID_InitNodeState(&st);
		}
	}
	PaintChromeNode(doc, rootId, backend, true);
}

void UID_PaintOverlay(uid_document_t *doc, const uid_backend_t *backend)
{
	if (!doc || !backend) {
		return;
	}
	if (doc->states.size() != doc->nodes.size()) {
		doc->states.resize(doc->nodes.size());
		for (uid_node_state_t &st : doc->states) {
			UID_InitNodeState(&st);
		}
	}
	/* Added in OPM: modals (incl. type=relative dropdowns) draw above model previews. */
	if (UID_IsModalActive(doc)) {
		const uid_node_id_t modalRoot = UID_GetModalRoot(doc);
		if (modalRoot != UID_INVALID_NODE_ID) {
			PaintChromeNode(doc, modalRoot, backend, true);
		}
	}
}
