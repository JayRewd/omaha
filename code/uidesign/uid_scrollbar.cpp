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

#include "uid_scrollbar.h"

#include "uid_input.h"
#include "uid_layout.h"
#include "uid_template.h"
#include "uid_value.h"
#include "uid_widget.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr const char *kDefaultScrollbarTemplate = "scrollbar-default";

void MarkDirty(uid_document_t *doc, uid_dirty_flags_t flags)
{
	if (doc) {
		doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty | flags);
	}
}

uid_node_state_t *State(uid_document_t *doc, uid_node_id_t id)
{
	if (!doc || id < 0 || static_cast<size_t>(id) >= doc->states.size()) {
		return nullptr;
	}
	return &doc->states[static_cast<size_t>(id)];
}

const char *PropCStr(const uid_node_def_t &node, const char *key, const char *fallback)
{
	const char *v = node.properties.GetCStr(key, nullptr);
	return (v && v[0]) ? v : fallback;
}

bool PropBool(const uid_node_def_t &node, const char *key, bool fallback)
{
	const char *v = PropCStr(node, key, nullptr);
	if (!v) {
		return fallback;
	}
	bool b = fallback;
	UID_ParseBool(v, &b, nullptr);
	return b;
}

bool ContainerOverflowScroll(const uid_node_def_t &node)
{
	uid_overflow_t ov = UID_OVERFLOW_NONE;
	UID_ParseOverflow(PropCStr(node, "overflow", "none"), &ov, nullptr);
	return ov == UID_OVERFLOW_SCROLL;
}

bool PointInRect(const uid_rect_t &r, float x, float y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

uid_node_id_t FindScrollbarChrome(const uid_document_t *doc, uid_node_id_t containerId)
{
	return UID_FindChildOfKind(doc, containerId, UID_NODE_SCROLLBAR);
}

} // namespace

uid_result_t UID_ExpandScrollbars(uid_document_t *doc, uid_diag_list_t *diags)
{
	if (!doc) {
		return UID_ERR_INVALID_ARG;
	}

	std::vector<uid_node_id_t> targets;
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		uid_node_def_t &n = doc->nodes[static_cast<size_t>(i)];
		if (n.kind != UID_NODE_CONTAINER) {
			continue;
		}
		if (!n.scrollbarTemplateId.empty() && !ContainerOverflowScroll(n)) {
			if (diags) {
				diags->Error(n.source, "scrollbar attribute requires overflow=\"scroll\"");
			}
			return UID_ERR_VALIDATE;
		}
		if (!ContainerOverflowScroll(n)) {
			continue;
		}
		if (FindScrollbarChrome(doc, static_cast<uid_node_id_t>(i)) != UID_INVALID_NODE_ID) {
			if (diags) {
				diags->Error(n.source, "container already has scrollbar chrome");
			}
			return UID_ERR_VALIDATE;
		}
		targets.push_back(static_cast<uid_node_id_t>(i));
	}

	for (uid_node_id_t containerId : targets) {
		uid_node_def_t &container = doc->nodes[static_cast<size_t>(containerId)];
		const bool explicitTemplate = !container.scrollbarTemplateId.empty();
		std::string tmplId = container.scrollbarTemplateId;
		if (tmplId.empty()) {
			tmplId = kDefaultScrollbarTemplate;
		}
		if (doc->definitions.templates.find(tmplId) == doc->definitions.templates.end()) {
			if (explicitTemplate) {
				if (diags) {
					diags->Error(container.source, "unknown scrollbar template: " + tmplId);
				}
				return UID_ERR_VALIDATE;
			}
			/* No default template in definitions — engine paints fallback chrome at runtime. */
			continue;
		}

		const uid_source_location_t containerSource = container.source;
		const uid_node_id_t chromeRoot = UID_CloneTemplateRoot(doc, tmplId.c_str(), container, diags);
		if (chromeRoot == UID_INVALID_NODE_ID) {
			return UID_ERR_VALIDATE;
		}

		if (chromeRoot < 0 || static_cast<size_t>(chromeRoot) >= doc->nodes.size()) {
			return UID_ERR_VALIDATE;
		}
		uid_node_def_t &chrome = doc->nodes[static_cast<size_t>(chromeRoot)];
		if (chrome.kind != UID_NODE_SCROLLBAR) {
			if (diags) {
				diags->Error(containerSource, "scrollbar template root must be <scrollbar>");
			}
			return UID_ERR_VALIDATE;
		}
		chrome.scrollbarGenerated = true;
		doc->nodes[static_cast<size_t>(containerId)].children.push_back(chromeRoot);
	}

	return UID_OK;
}

bool UID_HandleScrollbarPointer(
	uid_document_t *doc,
	float x,
	float y,
	int buttons,
	int lastButtons,
	const uid_backend_t *backend
)
{
	if (!doc) {
		return false;
	}

	const bool leftDown = (buttons & UID_POINTER_BUTTON_LEFT) != 0;
	const bool wasLeft = (lastButtons & UID_POINTER_BUTTON_LEFT) != 0;
	const bool pressed = leftDown && !wasLeft;
	const bool released = !leftDown && wasLeft;

	bool handled = false;

	for (size_t i = 0; i < doc->nodes.size() && i < doc->states.size(); ++i) {
		uid_node_def_t &node = doc->nodes[i];
		uid_node_state_t &st = doc->states[i];
		if (node.kind != UID_NODE_CONTAINER || !ContainerOverflowScroll(node)) {
			continue;
		}
		if (!st.scrollbarVisible) {
			if (st.scrollbarDragging && released) {
				st.scrollbarDragging = false;
			}
			continue;
		}

		const uid_rect_t &track = st.scrollbarTrackRect;
		const uid_rect_t &thumb = st.scrollbarThumbRect;
		const uid_node_id_t chromeId = FindScrollbarChrome(doc, static_cast<uid_node_id_t>(i));
		const uid_node_def_t *chrome = UID_GetNode(doc, chromeId);
		uid_layout_axis_t axis = UID_AXIS_VERTICAL;
		if (chrome) {
			UID_ParseAxis(PropCStr(*chrome, "axis", "vertical"), &axis, nullptr);
		}
		const bool vertical = (axis == UID_AXIS_VERTICAL);
		const float maxScroll = vertical
			? std::max(0.0f, st.contentExtentH - st.contentBox.h)
			: std::max(0.0f, st.contentExtentW - st.contentBox.w);

		if (st.scrollbarDragging) {
			if (leftDown && maxScroll > 0.0f && track.w > 0.0f && track.h > 0.0f) {
				const float trackTravel = vertical
					? std::max(1.0f, track.h - thumb.h)
					: std::max(1.0f, track.w - thumb.w);
				const float pointerAlong = vertical ? (y - st.scrollbarDragOffset) : (x - st.scrollbarDragOffset);
				const float trackStart = vertical ? track.y : track.x;
				const float t = std::min(1.0f, std::max(0.0f, (pointerAlong - trackStart) / trackTravel));
				if (vertical) {
					st.scrollY = t * maxScroll;
				} else {
					st.scrollX = t * maxScroll;
				}
				MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
				handled = true;
			} else if (released) {
				st.scrollbarDragging = false;
				const uid_node_id_t thumbId = UID_FindChildOfKind(doc, chromeId, UID_NODE_SCROLLBAR_THUMB);
				if (uid_node_state_t *thumbSt = State(doc, thumbId)) {
					thumbSt->pressed = false;
				}
				MarkDirty(doc, UID_DIRTY_PAINT);
			}
			continue;
		}

		if (!pressed) {
			continue;
		}

		if (thumb.w > 0.0f && thumb.h > 0.0f && PointInRect(thumb, x, y)) {
			st.scrollbarDragging = true;
			st.scrollbarDragOffset = vertical ? (y - thumb.y) : (x - thumb.x);
			const uid_node_id_t thumbId = UID_FindChildOfKind(doc, chromeId, UID_NODE_SCROLLBAR_THUMB);
			if (uid_node_state_t *thumbSt = State(doc, thumbId)) {
				thumbSt->pressed = true;
				thumbSt->hovered = true;
			}
			MarkDirty(doc, UID_DIRTY_PAINT);
			handled = true;
			continue;
		}

		if (track.w > 0.0f && track.h > 0.0f && PointInRect(track, x, y) && maxScroll > 0.0f) {
			const float rel = vertical ? ((y - track.y) / track.h) : ((x - track.x) / track.w);
			const float page = vertical ? st.contentBox.h : st.contentBox.w;
			const float target = rel * maxScroll - page * 0.5f;
			if (vertical) {
				st.scrollY = std::min(maxScroll, std::max(0.0f, target));
			} else {
				st.scrollX = std::min(maxScroll, std::max(0.0f, target));
			}
			MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
			handled = true;
		}
	}

	(void)backend;
	return handled;
}

void UID_PaintScrollbarFallback(uid_document_t *doc, uid_node_id_t containerId, const uid_backend_t *backend)
{
	if (!doc || !backend || !backend->drawSolidRect) {
		return;
	}

	uid_node_def_t *node = UID_GetNode(doc, containerId);
	uid_node_state_t *st = State(doc, containerId);
	if (!node || !st || node->kind != UID_NODE_CONTAINER || !ContainerOverflowScroll(*node)) {
		return;
	}
	if (FindScrollbarChrome(doc, containerId) != UID_INVALID_NODE_ID) {
		return;
	}
	UID_PaintScrollbarChrome(doc, containerId, backend);
}

static bool ResolveNodeFillRgba(
	const uid_document_t *doc,
	uid_node_id_t id,
	float rgba[4]
)
{
	uid_color_t color{};
	if (!UID_ResolveFillColor(doc, id, &color) || color.a <= 0.0f) {
		return false;
	}
	rgba[0] = color.r;
	rgba[1] = color.g;
	rgba[2] = color.b;
	rgba[3] = color.a;
	return true;
}

void UID_PaintScrollbarChrome(uid_document_t *doc, uid_node_id_t containerId, const uid_backend_t *backend)
{
	if (!doc || !backend || !backend->drawSolidRect) {
		return;
	}

	uid_node_def_t *node = UID_GetNode(doc, containerId);
	uid_node_state_t *st = State(doc, containerId);
	if (!node || !st || node->kind != UID_NODE_CONTAINER || !ContainerOverflowScroll(*node)) {
		return;
	}
	if (!st->scrollbarVisible) {
		return;
	}

	const uid_rect_t &track = st->scrollbarTrackRect;
	const uid_rect_t &thumb = st->scrollbarThumbRect;
	if (track.w <= 0.0f || track.h <= 0.0f) {
		return;
	}

	float trackRgba[4] = {1.0f, 1.0f, 1.0f, 0.12f};
	float thumbRgba[4] = {0.10f, 0.44f, 0.83f, 1.0f};

	const uid_node_id_t chromeId = FindScrollbarChrome(doc, containerId);
	if (chromeId != UID_INVALID_NODE_ID) {
		const uid_node_id_t trackId = UID_FindChildOfKind(doc, chromeId, UID_NODE_SCROLLBAR_TRACK);
		const uid_node_id_t thumbId = UID_FindChildOfKind(doc, chromeId, UID_NODE_SCROLLBAR_THUMB);
		if (trackId != UID_INVALID_NODE_ID) {
			ResolveNodeFillRgba(doc, trackId, trackRgba);
		}
		if (thumbId != UID_INVALID_NODE_ID) {
			ResolveNodeFillRgba(doc, thumbId, thumbRgba);
		}
	}

	backend->drawSolidRect(track.x, track.y, track.w, track.h, trackRgba);
	if (thumb.w > 0.0f && thumb.h > 0.0f) {
		backend->drawSolidRect(thumb.x, thumb.y, thumb.w, thumb.h, thumbRgba);
	}
}
