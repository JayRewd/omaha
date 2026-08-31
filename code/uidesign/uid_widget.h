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
#ifndef UID_WIDGET_H
#define UID_WIDGET_H

#include "uid_backend.h"
#include "uid_document.h"
#include "uid_types.h"

#include <string>

/* Resolve fill considering disabled/pressed/hover/focus overrides. */
bool UID_ResolveFillColor(const uid_document_t *doc, uid_node_id_t id, uid_color_t *out, const uid_backend_t *backend = nullptr);
/*
 * Added in OPM: resolve fill paint — solid color and/or atlas gradient brush.
 * Returns true if something should be painted. Gradient takes precedence when
 * the resolved string is a linear(...)/radial(...) brush.
 */
bool UID_ResolveFillPaint(
	const uid_document_t *doc,
	uid_node_id_t id,
	const uid_backend_t *backend,
	uid_color_t *outSolid,
	std::string *outGradient
);

/* Resolve text/control foreground color with state overrides. */
bool UID_ResolveTextColor(const uid_document_t *doc, uid_node_id_t id, uid_color_t *out);

/* Display text: runtime value, else edit buffer / node text. */
std::string UID_NodeDisplayText(const uid_document_t *doc, uid_node_id_t id);

/*
 * Added in Omaha: shorten text so Measure(prefix + "...") fits maxWidth (paint-time
 * text-overflow=ellipsis). Returns text unchanged when it already fits. tracking is
 * the same letter-spacing used by label paint (0 when unused).
 */
std::string UID_EllipsizeToWidth(
	const char *text,
	float maxWidth,
	void *font,
	const uid_backend_t *backend,
	float tracking
);

/* Paint one node's background/shape into the border box (no children). */
void UID_PaintNodeBackground(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend, float opacityMul = 1.0f);

/* Paint text / control chrome for one node (no children). */
void UID_PaintNodeContent(uid_document_t *doc, uid_node_id_t id, const uid_backend_t *backend, float opacityMul = 1.0f);

/*
 * Preorder chrome pass: bg/shape → text/control → children.
 * Skips painting children of open select popups (those go to overlay).
 */
void UID_PaintChrome(uid_document_t *doc, const uid_backend_t *backend);

/* Paint one subtree root (e.g. hud_crosshair) without menu chrome. */
void UID_PaintChromeSubtree(uid_document_t *doc, uid_node_id_t rootId, const uid_backend_t *backend);

/* Overlay pass: open selects (and future tooltips) in opening order. */
void UID_PaintOverlay(uid_document_t *doc, const uid_backend_t *backend);

#endif /* UID_WIDGET_H */
