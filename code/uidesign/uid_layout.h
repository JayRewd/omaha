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
#ifndef UID_LAYOUT_H
#define UID_LAYOUT_H

#include "uid_backend.h"
#include "uid_diag.h"
#include "uid_document.h"
#include "uid_types.h"

#include <vector>

/*
 * Added in Omaha: Stage 2 scoped layout. When enabled, UID_Update re-lays out from
 * nearest fixed-size boundaries instead of the full document tree.
 */
void UID_SetLayoutScoped(int enabled);
int  UID_LayoutScopedEnabled(void);

/*
 * Added in Omaha: re-layout dirtyLayoutNodes from nearest layout boundaries using
 * cached LayoutNode inputs. Returns UID_OK on success, or an error to force a
 * full UID_LayoutDocument fallback.
 */
uid_result_t UID_LayoutScoped(
	uid_document_t *doc,
	float fbScale,
	const uid_backend_t *backend,
	uid_diag_list_t *diags
);

/*
 * Measure and place the expanded document tree in logical pixels.
 * Does not paint. Writes margin/border/content boxes, effectiveClip,
 * scroll clamps, and content extents onto uid_node_state_t.
 * uiPxScale multiplies authored px (reference-resolution scale); %/fill/auto unchanged.
 */
uid_result_t UID_LayoutDocument(
	uid_document_t *doc,
	int logicalW,
	int logicalH,
	float fbScale,
	float uiPxScale,
	const uid_backend_t *backend,
	uid_diag_list_t *diags
);

/* Added in Omaha: authored px × lastUiPxScale (reference-resolution scale). */
float UID_ScaleAuthoredPx(const uid_document_t *doc, float px);

/*
 * Added in Omaha: translate-x/y paint-only fast path — offset boxes and re-intersect clips
 * without a full measure/place pass. Used when LAYOUT is clean and pendingTranslateDeltas
 * are queued from binding updates.
 */
void UID_ShiftSubtreeBoxes(uid_document_t *doc, uid_node_id_t id, float dx, float dy, const uid_rect_t &parentClip);
void UID_ApplyPendingTranslateDeltas(uid_document_t *doc);
/* Added in Omaha: Phase 4.1 — node → parent; rebuilt on STRUCTURE. */
void UID_RebuildParentMap(uid_document_t *doc);

/*
 * Topmost interactive node under (x,y) in logical space.
 * overlayFirst is retained for API compatibility (modals own hit-test when active).
 */
uid_node_id_t UID_HitTest(const uid_document_t *doc, float x, float y, bool overlayFirst);

/* True when the point lies inside rect and inside effectiveClip. */
bool UID_PointInClippedRect(const uid_rect_t &rect, const uid_rect_t &clip, float x, float y);

/* Added in Omaha: paint/hit clip for scrollbar chrome (content vs border edge). */
uid_rect_t UID_ScrollbarChromeClip(const uid_node_def_t *container, const uid_node_state_t *st);

/* Added in Omaha: multiline label measurement/paint helpers. */
typedef enum {
	UID_TEXT_WRAP_NONE = 0,
	UID_TEXT_WRAP_WORD
} uid_text_wrap_t;

typedef struct {
	float lineHeight;
	float blockWidth;
	float blockHeight;
	int   lineCount;
} uid_text_block_metrics_t;

uid_text_wrap_t UID_TextWrapMode(const uid_node_def_t &node);
float UID_TextLineHeight(
	const uid_document_t *doc,
	const uid_node_def_t &node,
	float fbScale,
	const uid_backend_t *backend
);
void UID_BuildTextLines(
	const uid_document_t *doc,
	const uid_node_def_t &node,
	const char *text,
	float maxWrapWidth,
	float fbScale,
	const uid_backend_t *backend,
	std::vector<std::string> *lines,
	uid_text_block_metrics_t *metrics
);

/* Added in Omaha: viewport-clamped overlay placement (flip Y, clamp X/height). */
struct uid_overlay_placement_t {
	uid_rect_t panel;
	float      contentH;
	float      scrollY;
	bool       flippedY;
};

uid_overlay_placement_t UID_PlaceOverlayInViewport(
	const uid_rect_t &viewport,
	const uid_rect_t &anchor,
	float panelW,
	float contentH,
	float currentScrollY,
	float gapPx,
	float maxPanelH /* Added in Omaha: 0 = viewport-only; else also clamp panel height */
);

#endif /* UID_LAYOUT_H */
