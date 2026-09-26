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

#include "uid_paint.h"

#include "uid_layout.h"
#include "uid_profile.h"
#include "uid_widget.h"

#include "../uirender/uir_batch.h"
#include "../uirender/uir_compositor.h"
#include "../uirender/uir_layer.h"
#include "../uirender/uir_stencil.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

static int g_paintList = 1;
static int g_replayClipDedup = 1;
static int g_paintRegions = 1;
static int g_liveTranslateCache = 1;
static int g_liveOpacityCache = 1;

enum {
	UID_PAINT_CMD_CLIP = 0,
	UID_PAINT_CMD_DRAW = 1,
	UID_PAINT_CMD_SHAPE_CLIP_BEGIN = 2,
	UID_PAINT_CMD_SHAPE_CLIP_END = 3,
	UID_PAINT_CMD_IMAGE_MASK_BEGIN = 4,
	UID_PAINT_CMD_IMAGE_MASK_END = 5,
	/* Bound translate-x/y subtrees — replay live so motion isn't baked. */
	UID_PAINT_CMD_LIVE_SUBTREE = 6,
	/* Added in Omaha: Phase 4.3 — foreach lifetime fade rows. */
	UID_PAINT_CMD_LIVE_OPACITY = 7
};

struct uid_paint_cmd_t {
	int   kind;
	float clipX, clipY, clipW, clipH;
	int   shader; /* DRAW: GL shader; IMAGE_MASK_BEGIN: uir_image_fit_t; LIVE_SUBTREE: node id */
	int   vertStart;
	int   vertCount;
	int   idxStart;
	int   idxCount;
	/* Shape clip begin: dest rect in clipX..clipH; paths in pathStrings[pathStart..). */
	float shapeViewW, shapeViewH, shapeRot;
	int   pathStart;
	int   pathCount;
};

/* Added in Omaha: Phase 4.6 — draw-space AABB (x0,y0 inclusive .. x1,y1). */
struct uid_paint_aabb_t {
	float x0, y0, x1, y1;
	bool  valid;
};

/* Added in Omaha: Phase 4.2 — DRAW-only cache for one LIVE_SUBTREE. */
struct uid_live_cache_t {
	uid_node_id_t               node;
	bool                        valid;
	bool                        unsupported;
	float                       originX;
	float                       originY;
	std::vector<uir_vert_t>     verts;
	std::vector<unsigned short> idxs;
	std::vector<uid_paint_cmd_t> cmds;
	/* Added in Omaha: Phase 4.6 — geometry AABB at capture, ambient clip, last replay params. */
	uid_paint_aabb_t            vertBox;
	float                       clipX, clipY, clipW, clipH;
	float                       lastDx, lastDy, lastMul;
	bool                        hasLast;
	int                         kind; /* UID_PAINT_CMD_LIVE_SUBTREE / UID_PAINT_CMD_LIVE_OPACITY */
};

struct uid_paint_list_t {
	std::vector<uid_paint_cmd_t> cmds;
	std::vector<uir_vert_t>      verts;
	std::vector<unsigned short>  idxs;
	std::vector<std::string>     pathStrings; /* SVG path D + soft mask-image specs */
	std::vector<uid_live_cache_t> lives;
	bool                         valid;
	bool                         sawHostDraw;
	float                        uiPxScale;
	int                          logicalW;
	int                          logicalH;
	/* Added in Omaha: Phase 4.6 — AABB of the static (non-live) commands. */
	uid_paint_aabb_t             staticBox;
};

struct uid_paint_region_t {
	uid_node_id_t   root;
	uid_paint_list_t list;
	/* Added in Omaha: Phase 4.6 — pixels this region owns in the retained target. */
	uid_paint_aabb_t prevBox;
	bool             forceDirty;
	bool             planDirty;
	/* List valid; only live translate/opacity changed — clear+replay lives, keep static. */
	bool             planLiveOnly;
};

struct uid_paint_regions_t {
	std::vector<uid_paint_region_t> regions;
	uid_node_id_t                   chromeRoot;
	bool                            usable;
	/* Added in Omaha: Phase 4.6 — retained target bookkeeping. */
	bool                            needFullClear;
	bool                            planPartial;
};

static uid_paint_list_t *g_recording = nullptr;
static int               g_recordPauseDepth = 0;
/* Added in Omaha: Phase 4.6 — document whose regions own the retained UI target. */
static uid_document_t   *g_retainOwner = nullptr;
static uid_live_cache_t *g_liveCapture = nullptr;
static int               g_liveCaptureNoCull = 0;
static int               g_liveCaptureOpacity = 0;
static uid_document_t   *g_liveCaptureDoc = nullptr;
static float             g_liveAmbX;
static float             g_liveAmbY;
static float             g_liveAmbW;
static float             g_liveAmbH;

void EnsurePaintList(uid_document_t *doc)
{
	if (!doc) {
		return;
	}
	if (!doc->paintList) {
		doc->paintList = new uid_paint_list_t();
	}
}

uid_paint_list_t *ListOf(uid_document_t *doc)
{
	return doc ? static_cast<uid_paint_list_t *>(doc->paintList) : nullptr;
}

uid_paint_regions_t *RegionsOf(uid_document_t *doc)
{
	return doc ? static_cast<uid_paint_regions_t *>(doc->paintRegions) : nullptr;
}

const uid_paint_regions_t *RegionsOfC(const uid_document_t *doc)
{
	return doc ? static_cast<const uid_paint_regions_t *>(doc->paintRegions) : nullptr;
}

static int ClipRectsNearlyEqual(float x0, float y0, float w0, float h0, float x1, float y1, float w1, float h1)
{
	return std::fabs(x0 - x1) < 0.01f && std::fabs(y0 - y1) < 0.01f && std::fabs(w0 - w1) < 0.01f
		&& std::fabs(h0 - h1) < 0.01f;
}

/* Added in Omaha: Phase 4.6 — AABB helpers (draw space). */
static void AabbReset(uid_paint_aabb_t *b)
{
	b->x0 = b->y0 = 0.0f;
	b->x1 = b->y1 = 0.0f;
	b->valid = false;
}

static void AabbAdd(uid_paint_aabb_t *b, float x0, float y0, float x1, float y1)
{
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	if (!b->valid) {
		b->x0 = x0;
		b->y0 = y0;
		b->x1 = x1;
		b->y1 = y1;
		b->valid = true;
		return;
	}
	b->x0 = std::min(b->x0, x0);
	b->y0 = std::min(b->y0, y0);
	b->x1 = std::max(b->x1, x1);
	b->y1 = std::max(b->y1, y1);
}

static void AabbUnion(uid_paint_aabb_t *b, const uid_paint_aabb_t &o)
{
	if (o.valid) {
		AabbAdd(b, o.x0, o.y0, o.x1, o.y1);
	}
}

static uid_paint_aabb_t AabbClip(const uid_paint_aabb_t &b, float cx, float cy, float cw, float ch)
{
	uid_paint_aabb_t r;
	AabbReset(&r);
	if (!b.valid || cw <= 0.0f || ch <= 0.0f) {
		return r;
	}
	AabbAdd(&r, std::max(b.x0, cx), std::max(b.y0, cy), std::min(b.x1, cx + cw), std::min(b.y1, cy + ch));
	return r;
}

static bool AabbIntersects(const uid_paint_aabb_t &a, const uid_paint_aabb_t &b)
{
	return a.valid && b.valid && a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

static void AabbFromVerts(const uir_vert_t *v, int n, uid_paint_aabb_t *out)
{
	if (!v || n <= 0) {
		return;
	}
	float ax = v[0].x, ay = v[0].y, bx = ax, by = ay;
	for (int i = 1; i < n; ++i) {
		ax = std::min(ax, v[i].x);
		ay = std::min(ay, v[i].y);
		bx = std::max(bx, v[i].x);
		by = std::max(by, v[i].y);
	}
	AabbAdd(out, ax, ay, bx, by);
}

/* Collapse consecutive CLIP runs; drop trailing CLIP. Keep CLIP before special cmds. */
static void CollapseClipCommands(uid_paint_list_t *list)
{
	std::vector<uid_paint_cmd_t> out;
	bool                         haveEff = false;
	float                        ex = 0.0f, ey = 0.0f, ew = 0.0f, eh = 0.0f;

	if (!list || !g_replayClipDedup) {
		return;
	}
	out.reserve(list->cmds.size());
	for (size_t i = 0; i < list->cmds.size();) {
		if (list->cmds[i].kind != UID_PAINT_CMD_CLIP) {
			out.push_back(list->cmds[i]);
			++i;
			continue;
		}
		size_t j = i;
		while (j < list->cmds.size() && list->cmds[j].kind == UID_PAINT_CMD_CLIP) {
			++j;
		}
		const uid_paint_cmd_t &last = list->cmds[j - 1];
		/* Trailing CLIP at end of list is a no-op for subsequent draws. */
		if (j == list->cmds.size()) {
			i = j;
			continue;
		}
		if (haveEff
			&& ClipRectsNearlyEqual(ex, ey, ew, eh, last.clipX, last.clipY, last.clipW, last.clipH)) {
			i = j;
			continue;
		}
		out.push_back(last);
		haveEff = true;
		ex = last.clipX;
		ey = last.clipY;
		ew = last.clipW;
		eh = last.clipH;
		i = j;
	}
	list->cmds.swap(out);
}

/*
 * Added in Omaha: Phase 4 — record-time draw merging.
 *
 * Recorded DRAW commands alternate shaders in node order (tick fill, label
 * font, tick fill, ...), so replay issues one GL draw per command. Merging a
 * later same-shader DRAW into an earlier one is exact when its geometry does
 * not overlap any draw it is moved in front of (blend order only matters on
 * shared pixels). Non-DRAW commands (clip / mask / live) delimit segments.
 */
struct merge_box_t {
	float x0, y0, x1, y1;
};

struct merge_group_t {
	int                      shader;
	float                    x0, y0, x1, y1;
	int                      nv;
	int                      ni;
	std::vector<int>         members;
	/* Per-triangle boxes: overlap is tested per primitive, not per group hull
	 * (a tick row and a label row share a hull but no pixels). */
	std::vector<merge_box_t> prims;
};

static void CmdPrimBoxes(
	const std::vector<uir_vert_t> &verts,
	const std::vector<unsigned short> &idxs,
	const uid_paint_cmd_t &cmd,
	std::vector<merge_box_t> *out
)
{
	const uir_vert_t *v = &verts[static_cast<size_t>(cmd.vertStart)];
	for (int t = 0; t + 2 < cmd.idxCount; t += 3) {
		const int a = idxs[static_cast<size_t>(cmd.idxStart + t)];
		const int b = idxs[static_cast<size_t>(cmd.idxStart + t + 1)];
		const int c = idxs[static_cast<size_t>(cmd.idxStart + t + 2)];
		if (a >= cmd.vertCount || b >= cmd.vertCount || c >= cmd.vertCount) {
			continue;
		}
		merge_box_t bx;
		bx.x0 = std::min(v[a].x, std::min(v[b].x, v[c].x));
		bx.y0 = std::min(v[a].y, std::min(v[b].y, v[c].y));
		bx.x1 = std::max(v[a].x, std::max(v[b].x, v[c].x));
		bx.y1 = std::max(v[a].y, std::max(v[b].y, v[c].y));
		out->push_back(bx);
	}
}

static bool PrimBoxesOverlap(const std::vector<merge_box_t> &a, const std::vector<merge_box_t> &b)
{
	for (const merge_box_t &p : a) {
		for (const merge_box_t &q : b) {
			if (p.x0 < q.x1 && q.x0 < p.x1 && p.y0 < q.y1 && q.y0 < p.y1) {
				return true;
			}
		}
	}
	return false;
}

static bool CmdBounds(
	const std::vector<uir_vert_t> &verts,
	const uid_paint_cmd_t &cmd,
	float *x0,
	float *y0,
	float *x1,
	float *y1
)
{
	if (cmd.vertStart < 0 || cmd.vertCount < 3 ||
	    static_cast<size_t>(cmd.vertStart) + static_cast<size_t>(cmd.vertCount) > verts.size()) {
		return false;
	}
	const uir_vert_t *v = &verts[static_cast<size_t>(cmd.vertStart)];
	float ax = v[0].x, ay = v[0].y, bx = ax, by = ay;
	for (int i = 1; i < cmd.vertCount; ++i) {
		ax = std::min(ax, v[i].x);
		ay = std::min(ay, v[i].y);
		bx = std::max(bx, v[i].x);
		by = std::max(by, v[i].y);
	}
	*x0 = ax;
	*y0 = ay;
	*x1 = bx;
	*y1 = by;
	return true;
}

static int MergeDrawCommands(
	std::vector<uid_paint_cmd_t> &cmds,
	std::vector<uir_vert_t> &verts,
	std::vector<unsigned short> &idxs
)
{
	if (cmds.size() < 2) {
		return 0;
	}
	for (const uid_paint_cmd_t &cmd : cmds) {
		if (cmd.kind != UID_PAINT_CMD_DRAW) {
			continue;
		}
		if (cmd.vertStart < 0 || cmd.vertCount < 3 || cmd.idxStart < 0 || cmd.idxCount < 3 ||
		    static_cast<size_t>(cmd.vertStart) + static_cast<size_t>(cmd.vertCount) > verts.size() ||
		    static_cast<size_t>(cmd.idxStart) + static_cast<size_t>(cmd.idxCount) > idxs.size()) {
			return 0;
		}
	}

	std::vector<uid_paint_cmd_t> outCmds;
	std::vector<uir_vert_t>      outVerts;
	std::vector<unsigned short>  outIdxs;
	std::vector<merge_group_t>   groups;
	int                          merged = 0;
	outCmds.reserve(cmds.size());
	outVerts.reserve(verts.size());
	outIdxs.reserve(idxs.size());

	auto flushGroups = [&]() {
		for (const merge_group_t &g : groups) {
			uid_paint_cmd_t cmd{};
			cmd.kind = UID_PAINT_CMD_DRAW;
			cmd.shader = g.shader;
			cmd.vertStart = static_cast<int>(outVerts.size());
			cmd.idxStart = static_cast<int>(outIdxs.size());
			int base = 0;
			for (int m : g.members) {
				const uid_paint_cmd_t &src = cmds[static_cast<size_t>(m)];
				outVerts.insert(
					outVerts.end(),
					verts.begin() + src.vertStart,
					verts.begin() + src.vertStart + src.vertCount
				);
				for (int k = 0; k < src.idxCount; ++k) {
					outIdxs.push_back(static_cast<unsigned short>(
						idxs[static_cast<size_t>(src.idxStart + k)] + base
					));
				}
				base += src.vertCount;
			}
			cmd.vertCount = g.nv;
			cmd.idxCount = g.ni;
			outCmds.push_back(cmd);
		}
		groups.clear();
	};

	for (size_t i = 0; i < cmds.size(); ++i) {
		const uid_paint_cmd_t &cmd = cmds[i];
		if (cmd.kind != UID_PAINT_CMD_DRAW) {
			flushGroups();
			outCmds.push_back(cmd);
			continue;
		}
		float x0, y0, x1, y1;
		if (!CmdBounds(verts, cmd, &x0, &y0, &x1, &y1)) {
			return 0;
		}
		std::vector<merge_box_t> prims;
		CmdPrimBoxes(verts, idxs, cmd, &prims);
		/*
		 * Walk groups from the most recent back. The nearest same-shader group
		 * is the merge target, but only if none of the groups drawn after it
		 * touch a pixel this command touches (hull test first, primitives when
		 * the hulls intersect).
		 */
		int target = -1;
		for (int g = static_cast<int>(groups.size()) - 1; g >= 0; --g) {
			const merge_group_t &grp = groups[static_cast<size_t>(g)];
			if (grp.shader == cmd.shader) {
				if (grp.nv + cmd.vertCount <= UIR_BATCH_MAX_VERTS &&
				    grp.ni + cmd.idxCount <= UIR_BATCH_MAX_INDEXES) {
					target = g;
				}
				break;
			}
			if (grp.x0 < x1 && x0 < grp.x1 && grp.y0 < y1 && y0 < grp.y1 &&
			    PrimBoxesOverlap(grp.prims, prims)) {
				break;
			}
		}
		if (target >= 0) {
			merge_group_t &grp = groups[static_cast<size_t>(target)];
			grp.members.push_back(static_cast<int>(i));
			grp.nv += cmd.vertCount;
			grp.ni += cmd.idxCount;
			grp.x0 = std::min(grp.x0, x0);
			grp.y0 = std::min(grp.y0, y0);
			grp.x1 = std::max(grp.x1, x1);
			grp.y1 = std::max(grp.y1, y1);
			grp.prims.insert(grp.prims.end(), prims.begin(), prims.end());
			++merged;
			continue;
		}
		merge_group_t grp;
		grp.shader = cmd.shader;
		grp.x0 = x0;
		grp.y0 = y0;
		grp.x1 = x1;
		grp.y1 = y1;
		grp.nv = cmd.vertCount;
		grp.ni = cmd.idxCount;
		grp.members.push_back(static_cast<int>(i));
		grp.prims.swap(prims);
		groups.push_back(grp);
	}
	flushGroups();
	if (!merged) {
		return 0;
	}
	cmds.swap(outCmds);
	verts.swap(outVerts);
	idxs.swap(outIdxs);
	return merged;
}


void ClearList(uid_paint_list_t *list)
{
	if (!list) {
		return;
	}
	list->cmds.clear();
	list->verts.clear();
	list->idxs.clear();
	list->pathStrings.clear();
	list->lives.clear();
	list->valid = false;
	list->sawHostDraw = false;
	AabbReset(&list->staticBox);
}

/* Added in Omaha: Phase 4.6 — AABB of everything a list paints except live blocks. */
static void ComputeListStaticBox(uid_paint_list_t *list)
{
	AabbReset(&list->staticBox);
	for (const uid_paint_cmd_t &cmd : list->cmds) {
		if (cmd.kind == UID_PAINT_CMD_DRAW) {
			if (cmd.vertStart >= 0 && cmd.vertCount > 0 &&
			    static_cast<size_t>(cmd.vertStart) + static_cast<size_t>(cmd.vertCount) <= list->verts.size()) {
				AabbFromVerts(&list->verts[static_cast<size_t>(cmd.vertStart)], cmd.vertCount, &list->staticBox);
			}
		} else if (cmd.kind == UID_PAINT_CMD_SHAPE_CLIP_BEGIN || cmd.kind == UID_PAINT_CMD_IMAGE_MASK_BEGIN) {
			/* Stencil/soft-mask passes clear + write inside their dest rect. */
			AabbAdd(&list->staticBox, cmd.clipX, cmd.clipY, cmd.clipX + cmd.clipW, cmd.clipY + cmd.clipH);
		}
	}
}

void OnRecordDraw(const uir_vert_t *v, int nv, const unsigned short *idx, int ni, int shader, void *userdata)
{
	uid_paint_list_t *list = static_cast<uid_paint_list_t *>(userdata);
	if (!list || !v || nv < 3 || !idx || ni < 3) {
		return;
	}
	uid_paint_cmd_t cmd{};
	cmd.kind = UID_PAINT_CMD_DRAW;
	cmd.shader = shader;
	cmd.vertStart = static_cast<int>(list->verts.size());
	cmd.vertCount = nv;
	cmd.idxStart = static_cast<int>(list->idxs.size());
	cmd.idxCount = ni;
	list->verts.insert(list->verts.end(), v, v + nv);
	/* Indexes stay 0-based relative to this draw's verts (BatchTriangles rebases). */
	list->idxs.insert(list->idxs.end(), idx, idx + ni);
	list->cmds.push_back(cmd);
}

void OnRecordClip(float x, float y, float w, float h, void *userdata)
{
	uid_paint_list_t *list = static_cast<uid_paint_list_t *>(userdata);
	if (!list) {
		return;
	}
	uid_paint_cmd_t cmd{};
	cmd.kind = UID_PAINT_CMD_CLIP;
	cmd.clipX = x;
	cmd.clipY = y;
	cmd.clipW = w;
	cmd.clipH = h;
	list->cmds.push_back(cmd);
}

void OnLiveDraw(const uir_vert_t *v, int nv, const unsigned short *idx, int ni, int shader, void *userdata)
{
	uid_live_cache_t *live = static_cast<uid_live_cache_t *>(userdata);
	if (!live || live->unsupported || !v || nv < 3 || !idx || ni < 3) {
		return;
	}
	uid_paint_cmd_t cmd{};
	cmd.kind = UID_PAINT_CMD_DRAW;
	cmd.shader = shader;
	cmd.vertStart = static_cast<int>(live->verts.size());
	cmd.vertCount = nv;
	cmd.idxStart = static_cast<int>(live->idxs.size());
	cmd.idxCount = ni;
	live->verts.insert(live->verts.end(), v, v + nv);
	live->idxs.insert(live->idxs.end(), idx, idx + ni);
	live->cmds.push_back(cmd);
}

void OnLiveClip(float x, float y, float w, float h, void *userdata)
{
	uid_live_cache_t *live = static_cast<uid_live_cache_t *>(userdata);
	if (!live || live->unsupported) {
		return;
	}
	/* Empty clips are off-window ticks; ambient scissor already cuts them. */
	if (w <= 0.01f || h <= 0.01f) {
		return;
	}
	if (ClipRectsNearlyEqual(g_liveAmbX, g_liveAmbY, g_liveAmbW, g_liveAmbH, x, y, w, h)) {
		return;
	}
	live->unsupported = true;
}

static void MarkLiveUnsupported(void)
{
	if (g_liveCapture) {
		g_liveCapture->unsupported = true;
	}
}

static void ResetLiveCaptureState(void)
{
	g_liveCapture = nullptr;
	g_liveCaptureNoCull = 0;
	g_liveCaptureOpacity = 0;
	g_liveCaptureDoc = nullptr;
}

/* Only drop flags for lives in the list being re-recorded. Clearing the whole
 * document made a timer/health recapture mark every fade row uncached, which
 * then foreach_fade-dirtied the messaging region and flashed full-alpha. */
static void ClearLiveOpacityFlagsForList(uid_document_t *doc, uid_paint_list_t *list)
{
	if (!doc || !list) {
		return;
	}
	for (const uid_live_cache_t &live : list->lives) {
		if (live.node < 0 || static_cast<size_t>(live.node) >= doc->states.size()) {
			continue;
		}
		doc->states[static_cast<size_t>(live.node)].liveOpacityCached = false;
	}
}

static void BeginLiveCapturePtr(
	uid_live_cache_t *live,
	float originX,
	float originY,
	float clipX,
	float clipY,
	float clipW,
	float clipH,
	int noCull
)
{
	if (!live || g_liveCapture) {
		return;
	}
	live->verts.clear();
	live->idxs.clear();
	live->cmds.clear();
	live->valid = false;
	live->unsupported = false;
	live->originX = originX;
	live->originY = originY;
	AabbReset(&live->vertBox);
	live->clipX = clipX;
	live->clipY = clipY;
	live->clipW = clipW;
	live->clipH = clipH;
	live->hasLast = false;
	g_liveCapture = live;
	g_liveCaptureNoCull = noCull ? 1 : 0;
	g_liveAmbX = clipX;
	g_liveAmbY = clipY;
	g_liveAmbW = clipW;
	g_liveAmbH = clipH;
	UIR_BatchFlush();
	uir_paint_recorder_t rec{};
	rec.onDraw = OnLiveDraw;
	rec.onClip = OnLiveClip;
	rec.userdata = live;
	rec.skipSubmit = g_liveCaptureOpacity ? 1 : 0;
	UIR_BatchSetPaintRecorder(&rec);
}

static void EndLiveCapturePtr(void)
{
	if (!g_liveCapture) {
		g_liveCaptureNoCull = 0;
		return;
	}
	UIR_BatchFlush();
	UIR_BatchSetPaintRecorder(nullptr);
	if (g_liveCapture->unsupported) {
		g_liveCapture->cmds.clear();
		g_liveCapture->verts.clear();
		g_liveCapture->idxs.clear();
		g_liveCapture->valid = false;
	} else {
		g_liveCapture->valid = true;
		MergeDrawCommands(g_liveCapture->cmds, g_liveCapture->verts, g_liveCapture->idxs);
		/* Phase 4.6: geometry AABB (absolute at capture; replay adds dx/dy). */
		AabbReset(&g_liveCapture->vertBox);
		AabbFromVerts(
			g_liveCapture->verts.empty() ? nullptr : g_liveCapture->verts.data(),
			static_cast<int>(g_liveCapture->verts.size()),
			&g_liveCapture->vertBox
		);
		/* Translate captures submit at the origin (dx=0); opacity captures replay right after. */
		g_liveCapture->lastDx = 0.0f;
		g_liveCapture->lastDy = 0.0f;
		g_liveCapture->lastMul = 1.0f;
		g_liveCapture->hasLast = !g_liveCaptureOpacity;
		if (g_liveCaptureOpacity && g_liveCaptureDoc && g_liveCapture->node != UID_INVALID_NODE_ID &&
		    static_cast<size_t>(g_liveCapture->node) < g_liveCaptureDoc->states.size()) {
			g_liveCaptureDoc->states[static_cast<size_t>(g_liveCapture->node)].liveOpacityCached = true;
		}
	}
	ResetLiveCaptureState();
}

static int ReplayLiveOffset(uid_document_t *doc, uid_live_cache_t *live)
{
	if (!doc || !live || !live->valid || live->unsupported) {
		return 0;
	}
	if (live->node == UID_INVALID_NODE_ID ||
	    static_cast<size_t>(live->node) >= doc->states.size()) {
		return 0;
	}
	const uid_node_state_t *st = &doc->states[static_cast<size_t>(live->node)];
	float dx = st->borderBox.x - live->originX;
	float dy = st->borderBox.y - live->originY;
	for (const uid_paint_cmd_t &cmd : live->cmds) {
		if (cmd.kind != UID_PAINT_CMD_DRAW || cmd.vertCount < 3 || cmd.idxCount < 3) {
			continue;
		}
		if (cmd.vertStart < 0 || cmd.idxStart < 0 ||
		    cmd.vertStart + cmd.vertCount > static_cast<int>(live->verts.size()) ||
		    cmd.idxStart + cmd.idxCount > static_cast<int>(live->idxs.size())) {
			live->valid = false;
			return 0;
		}
		UIR_BatchTrianglesOffset(
			cmd.shader,
			&live->verts[static_cast<size_t>(cmd.vertStart)],
			cmd.vertCount,
			&live->idxs[static_cast<size_t>(cmd.idxStart)],
			cmd.idxCount,
			dx,
			dy
		);
	}
	live->lastDx = dx;
	live->lastDy = dy;
	live->lastMul = 1.0f;
	live->hasLast = true;
	return 1;
}

static int ReplayOrCaptureLive(
	uid_document_t *doc,
	uid_live_cache_t *live,
	const uid_backend_t *backend
)
{
	if (!g_liveTranslateCache || !live || !backend) {
		return 0;
	}
	if (live->unsupported) {
		return 0;
	}
	if (live->valid) {
		return ReplayLiveOffset(doc, live);
	}
	if (live->node == UID_INVALID_NODE_ID ||
	    static_cast<size_t>(live->node) >= doc->states.size()) {
		return 0;
	}
	const uid_node_state_t *st = &doc->states[static_cast<size_t>(live->node)];
	BeginLiveCapturePtr(
		live,
		st->borderBox.x,
		st->borderBox.y,
		st->effectiveClip.x,
		st->effectiveClip.y,
		st->effectiveClip.w,
		st->effectiveClip.h,
		1
	);
	UID_PaintChromeSubtree(doc, live->node, backend);
	EndLiveCapturePtr();
	return 1;
}

static int ReplayLiveScaled(uid_document_t *doc, uid_live_cache_t *live)
{
	if (!doc || !live || !live->valid || live->unsupported) {
		return 0;
	}
	if (live->node == UID_INVALID_NODE_ID ||
	    static_cast<size_t>(live->node) >= doc->states.size()) {
		return 0;
	}
	const float mul = doc->states[static_cast<size_t>(live->node)].lifetimeOpacityMul;
	live->lastDx = 0.0f;
	live->lastDy = 0.0f;
	live->lastMul = mul;
	live->hasLast = true;
	if (mul <= 0.001f) {
		return 1;
	}
	for (const uid_paint_cmd_t &cmd : live->cmds) {
		if (cmd.kind != UID_PAINT_CMD_DRAW || cmd.vertCount < 3 || cmd.idxCount < 3) {
			continue;
		}
		if (cmd.vertStart < 0 || cmd.idxStart < 0 ||
		    cmd.vertStart + cmd.vertCount > static_cast<int>(live->verts.size()) ||
		    cmd.idxStart + cmd.idxCount > static_cast<int>(live->idxs.size())) {
			live->valid = false;
			return 0;
		}
		UIR_BatchTrianglesScaledAlpha(
			cmd.shader,
			&live->verts[static_cast<size_t>(cmd.vertStart)],
			cmd.vertCount,
			&live->idxs[static_cast<size_t>(cmd.idxStart)],
			cmd.idxCount,
			mul
		);
	}
	return 1;
}

static int ReplayOrCaptureLiveOpacity(
	uid_document_t *doc,
	uid_live_cache_t *live,
	const uid_backend_t *backend
)
{
	if (!g_liveOpacityCache || !live || !backend) {
		return 0;
	}
	if (live->unsupported) {
		return 0;
	}
	if (live->valid) {
		return ReplayLiveScaled(doc, live);
	}
	if (live->node == UID_INVALID_NODE_ID ||
	    static_cast<size_t>(live->node) >= doc->states.size()) {
		return 0;
	}
	uid_node_state_t *st = &doc->states[static_cast<size_t>(live->node)];
	const float savedMul = st->lifetimeOpacityMul;
	st->lifetimeOpacityMul = 1.0f;
	g_liveCaptureOpacity = 1;
	g_liveCaptureDoc = doc;
	BeginLiveCapturePtr(
		live,
		st->borderBox.x,
		st->borderBox.y,
		st->effectiveClip.x,
		st->effectiveClip.y,
		st->effectiveClip.w,
		st->effectiveClip.h,
		0
	);
	UID_PaintChromeSubtree(doc, live->node, backend);
	st->lifetimeOpacityMul = savedMul;
	UID_PaintListEndLiveCapture();
	return 1;
}

void ReplayUnwindClips(int *shapeDepth, int *maskDepth)
{
	while (maskDepth && *maskDepth > 0) {
		UIR_EndImageMask();
		--(*maskDepth);
	}
	while (shapeDepth && *shapeDepth > 0) {
		UIR_EndShapeClip();
		--(*shapeDepth);
	}
}

} // namespace

void UID_SetReplayClipDedup(int enabled)
{
	g_replayClipDedup = enabled ? 1 : 0;
}

void UID_SetPaintList(int enabled)
{
	g_paintList = enabled ? 1 : 0;
	if (!g_paintList) {
		UIR_BatchSetPaintRecorder(nullptr);
		g_recording = nullptr;
	}
}

void UID_SetPaintRegions(int enabled)
{
	g_paintRegions = enabled ? 1 : 0;
}

void UID_SetLiveTranslateCache(int enabled)
{
	g_liveTranslateCache = enabled ? 1 : 0;
}

int UID_LiveTranslateCacheEnabled(void)
{
	return g_liveTranslateCache;
}

int UID_PaintLiveCaptureNoCull(void)
{
	return g_liveCaptureNoCull;
}

int UID_PaintListBeginLiveCapture(
	uid_node_id_t nodeId,
	float originX,
	float originY,
	float clipX,
	float clipY,
	float clipW,
	float clipH
)
{
	if (!g_liveTranslateCache || !g_recording || g_recording->lives.empty() || g_liveCapture) {
		return 0;
	}
	uid_live_cache_t *live = &g_recording->lives.back();
	if (live->node != nodeId) {
		return 0;
	}
	BeginLiveCapturePtr(live, originX, originY, clipX, clipY, clipW, clipH, 1);
	return g_liveCapture ? 1 : 0;
}

int UID_PaintListBeginLiveOpacityCapture(
	uid_document_t *doc,
	uid_node_id_t nodeId,
	float originX,
	float originY,
	float clipX,
	float clipY,
	float clipW,
	float clipH
)
{
	if (!g_liveOpacityCache || !g_recording || g_recording->lives.empty() || g_liveCapture) {
		return 0;
	}
	uid_live_cache_t *live = &g_recording->lives.back();
	if (live->node != nodeId) {
		return 0;
	}
	g_liveCaptureOpacity = 1;
	g_liveCaptureDoc = doc;
	BeginLiveCapturePtr(live, originX, originY, clipX, clipY, clipW, clipH, 0);
	return g_liveCapture ? 1 : 0;
}

void UID_PaintListEndLiveCapture(void)
{
	uid_document_t *doc = g_liveCaptureDoc;
	uid_live_cache_t *live = g_liveCapture;
	const int wasOp = g_liveCaptureOpacity;
	EndLiveCapturePtr();
	/* Capture skipped the framebuffer submit; draw the new cache at the current mul. */
	if (wasOp && doc && live && live->valid) {
		ReplayLiveScaled(doc, live);
	}
}

void UID_SetLiveOpacityCache(int enabled)
{
	g_liveOpacityCache = enabled ? 1 : 0;
}

int UID_LiveOpacityCacheEnabled(void)
{
	return g_liveOpacityCache;
}

int UID_PaintLiveOpacityRowsCached(const uid_document_t *doc, uid_node_id_t foreachId)
{
	if (!g_liveOpacityCache || !doc || foreachId < 0 ||
	    static_cast<size_t>(foreachId) >= doc->nodes.size()) {
		return 0;
	}
	const uid_node_def_t &fn = doc->nodes[static_cast<size_t>(foreachId)];
	if (fn.children.empty()) {
		return 0;
	}
	for (uid_node_id_t childId : fn.children) {
		if (childId < 0 || static_cast<size_t>(childId) >= doc->states.size()) {
			return 0;
		}
		if (!doc->states[static_cast<size_t>(childId)].liveOpacityCached) {
			return 0;
		}
	}
	return 1;
}

int UID_PaintRegionsEnabled(void)
{
	/* Regions own their lists; do not require ui_paint_list (archived 0 on this box). */
	return g_paintRegions ? 1 : 0;
}

int UID_PaintListFlag(void)
{
	return g_paintList;
}

int UID_PaintRegionsFlag(void)
{
	return g_paintRegions;
}

int UID_PaintRegionsRawUsable(const uid_document_t *doc)
{
	const uid_paint_regions_t *regs = RegionsOfC(doc);
	return (regs && regs->usable && !regs->regions.empty()) ? 1 : 0;
}

int UID_PaintListEnabled(void)
{
	return g_paintList;
}

void UID_PaintListInvalidate(uid_document_t *doc)
{
	uid_paint_list_t *list = ListOf(doc);
	if (list) {
		list->valid = false;
	}
}

void UID_PaintRegionsFree(uid_document_t *doc)
{
	if (!doc || !doc->paintRegions) {
		return;
	}
	uid_paint_regions_t *regs = RegionsOf(doc);
	if (regs) {
		for (uid_paint_region_t &r : regs->regions) {
			if (g_recording == &r.list) {
				UIR_BatchSetPaintRecorder(nullptr);
				g_recording = nullptr;
			}
		}
	}
	if (g_liveCapture) {
		UIR_BatchSetPaintRecorder(nullptr);
		ResetLiveCaptureState();
	}
	if (g_retainOwner == doc) {
		g_retainOwner = nullptr;
	}
	delete regs;
	doc->paintRegions = nullptr;
}

void UID_PaintListFree(uid_document_t *doc)
{
	if (!doc) {
		return;
	}
	if (doc->paintList) {
		if (g_recording == doc->paintList) {
			UIR_BatchSetPaintRecorder(nullptr);
			g_recording = nullptr;
		}
		if (g_liveCapture) {
			UIR_BatchSetPaintRecorder(nullptr);
			ResetLiveCaptureState();
		}
		delete static_cast<uid_paint_list_t *>(doc->paintList);
		doc->paintList = nullptr;
	}
	UID_PaintRegionsFree(doc);
}

void UID_PaintListMarkHostDraw(void)
{
	MarkLiveUnsupported();
	if (g_recording) {
		g_recording->sawHostDraw = true;
	}
}

int UID_PaintListIsActivelyRecording(void)
{
	return (g_recording != nullptr && g_recordPauseDepth == 0) ? 1 : 0;
}

void UID_PaintListPauseRecord(void)
{
	if (!g_recording) {
		return;
	}
	if (g_recordPauseDepth++ == 0) {
		UIR_BatchFlush();
		UIR_BatchSetPaintRecorder(nullptr);
	}
}

void UID_PaintListResumeRecord(void)
{
	if (!g_recording || g_recordPauseDepth <= 0) {
		return;
	}
	if (--g_recordPauseDepth == 0) {
		UIR_BatchFlush();
		uir_paint_recorder_t rec{};
		rec.onDraw = OnRecordDraw;
		rec.onClip = OnRecordClip;
		rec.userdata = g_recording;
		UIR_BatchSetPaintRecorder(&rec);
	}
}

void UID_PaintListRecordLiveSubtree(uid_node_id_t nodeId)
{
	if (g_liveCapture) {
		MarkLiveUnsupported();
		return;
	}
	if (!UID_PaintListIsActivelyRecording() || nodeId == UID_INVALID_NODE_ID) {
		return;
	}
	UIR_BatchFlush();
	uid_paint_cmd_t cmd{};
	cmd.kind = UID_PAINT_CMD_LIVE_SUBTREE;
	cmd.shader = static_cast<int>(nodeId);
	cmd.vertStart = static_cast<int>(g_recording->lives.size());
	uid_live_cache_t live{};
	live.node = nodeId;
	live.valid = false;
	live.unsupported = false;
	live.kind = UID_PAINT_CMD_LIVE_SUBTREE;
	g_recording->lives.push_back(live);
	g_recording->cmds.push_back(cmd);
}

void UID_PaintListRecordLiveOpacity(uid_node_id_t nodeId)
{
	if (g_liveCapture) {
		MarkLiveUnsupported();
		return;
	}
	if (!UID_PaintListIsActivelyRecording() || nodeId == UID_INVALID_NODE_ID) {
		return;
	}
	UIR_BatchFlush();
	uid_paint_cmd_t cmd{};
	cmd.kind = UID_PAINT_CMD_LIVE_OPACITY;
	cmd.shader = static_cast<int>(nodeId);
	cmd.vertStart = static_cast<int>(g_recording->lives.size());
	uid_live_cache_t live{};
	live.node = nodeId;
	live.valid = false;
	live.unsupported = false;
	live.kind = UID_PAINT_CMD_LIVE_OPACITY;
	g_recording->lives.push_back(live);
	g_recording->cmds.push_back(cmd);
}

void UID_PaintListRecordShapeClipBegin(
	float x,
	float y,
	float w,
	float h,
	const char *const *pathD,
	int pathCount,
	float viewW,
	float viewH,
	float rotationDeg
)
{
	if (g_liveCapture) {
		MarkLiveUnsupported();
		return;
	}
	if (!UID_PaintListIsActivelyRecording() || !pathD || pathCount <= 0) {
		return;
	}
	/* Flush so prior draws land before the clip command in the retained list. */
	UIR_BatchFlush();
	uid_paint_cmd_t cmd{};
	cmd.kind = UID_PAINT_CMD_SHAPE_CLIP_BEGIN;
	cmd.clipX = x;
	cmd.clipY = y;
	cmd.clipW = w;
	cmd.clipH = h;
	cmd.shapeViewW = viewW;
	cmd.shapeViewH = viewH;
	cmd.shapeRot = rotationDeg;
	cmd.pathStart = static_cast<int>(g_recording->pathStrings.size());
	cmd.pathCount = pathCount;
	for (int i = 0; i < pathCount; ++i) {
		g_recording->pathStrings.emplace_back(pathD[i] ? pathD[i] : "");
	}
	g_recording->cmds.push_back(cmd);
}

void UID_PaintListRecordShapeClipEnd(void)
{
	if (g_liveCapture) {
		MarkLiveUnsupported();
		return;
	}
	if (!UID_PaintListIsActivelyRecording()) {
		return;
	}
	UIR_BatchFlush();
	uid_paint_cmd_t cmd{};
	cmd.kind = UID_PAINT_CMD_SHAPE_CLIP_END;
	g_recording->cmds.push_back(cmd);
}

void UID_PaintListRecordImageMaskBegin(
	float x,
	float y,
	float w,
	float h,
	const char *maskSpec,
	int fit
)
{
	if (g_liveCapture) {
		MarkLiveUnsupported();
		return;
	}
	if (!UID_PaintListIsActivelyRecording() || !maskSpec || !maskSpec[0]) {
		return;
	}
	UIR_BatchFlush();
	uid_paint_cmd_t cmd{};
	cmd.kind = UID_PAINT_CMD_IMAGE_MASK_BEGIN;
	cmd.clipX = x;
	cmd.clipY = y;
	cmd.clipW = w;
	cmd.clipH = h;
	cmd.shader = fit;
	cmd.pathStart = static_cast<int>(g_recording->pathStrings.size());
	cmd.pathCount = 1;
	g_recording->pathStrings.emplace_back(maskSpec);
	g_recording->cmds.push_back(cmd);
}

void UID_PaintListRecordImageMaskEnd(void)
{
	if (g_liveCapture) {
		MarkLiveUnsupported();
		return;
	}
	if (!UID_PaintListIsActivelyRecording()) {
		return;
	}
	UIR_BatchFlush();
	uid_paint_cmd_t cmd{};
	cmd.kind = UID_PAINT_CMD_IMAGE_MASK_END;
	g_recording->cmds.push_back(cmd);
}

static int ReplayList(
	uid_document_t *doc,
	uid_paint_list_t *list,
	const uid_backend_t *backend,
	int allowEmpty,
	int flushBookends
)
{
	if (!doc || !list || !list->valid) {
		return 0;
	}
	if (list->cmds.empty()) {
		return allowEmpty ? 1 : 0;
	}
	if (list->uiPxScale != doc->lastUiPxScale || list->logicalW != doc->lastLogicalW ||
	    list->logicalH != doc->lastLogicalH) {
		list->valid = false;
		return 0;
	}

	if (flushBookends) {
		UIR_BatchFlush();
	}
	int shapeDepth = 0;
	int maskDepth = 0;
	for (const uid_paint_cmd_t &cmd : list->cmds) {
		if (cmd.kind == UID_PAINT_CMD_LIVE_OPACITY) {
			uid_live_cache_t *live = nullptr;
			if (cmd.vertStart >= 0 && cmd.vertStart < static_cast<int>(list->lives.size())) {
				live = &list->lives[static_cast<size_t>(cmd.vertStart)];
			}
			if (live && ReplayOrCaptureLiveOpacity(doc, live, backend)) {
				continue;
			}
			if (backend) {
				UID_PaintChromeSubtree(doc, static_cast<uid_node_id_t>(cmd.shader), backend);
			}
			continue;
		}
		if (cmd.kind == UID_PAINT_CMD_LIVE_SUBTREE) {
			uid_live_cache_t *live = nullptr;
			if (cmd.vertStart >= 0 && cmd.vertStart < static_cast<int>(list->lives.size())) {
				live = &list->lives[static_cast<size_t>(cmd.vertStart)];
			}
			if (live && ReplayOrCaptureLive(doc, live, backend)) {
				continue;
			}
			if (backend) {
				UID_PaintChromeSubtree(doc, static_cast<uid_node_id_t>(cmd.shader), backend);
			}
			continue;
		}
		if (cmd.kind == UID_PAINT_CMD_CLIP) {
			if (g_replayClipDedup) {
				UIR_ApplyClipRect(cmd.clipX, cmd.clipY, cmd.clipW, cmd.clipH);
			} else {
				UIR_BatchFlush();
				UIR_ForceClipRect(cmd.clipX, cmd.clipY, cmd.clipW, cmd.clipH);
			}
			continue;
		}
		if (cmd.kind == UID_PAINT_CMD_SHAPE_CLIP_BEGIN) {
			UIR_BatchFlush();
			if (cmd.pathCount <= 0 || cmd.pathStart < 0 ||
			    cmd.pathStart + cmd.pathCount > static_cast<int>(list->pathStrings.size())) {
				ReplayUnwindClips(&shapeDepth, &maskDepth);
				list->valid = false;
				return 0;
			}
			const char *pathPtrs[UIR_SHAPE_CLIP_MAX_PATHS];
			const int n = cmd.pathCount < UIR_SHAPE_CLIP_MAX_PATHS ? cmd.pathCount : UIR_SHAPE_CLIP_MAX_PATHS;
			for (int i = 0; i < n; ++i) {
				pathPtrs[i] = list->pathStrings[static_cast<size_t>(cmd.pathStart + i)].c_str();
			}
			if (UIR_BeginSvgShapeClip(
					cmd.clipX,
					cmd.clipY,
					cmd.clipW,
					cmd.clipH,
					pathPtrs,
					n,
					cmd.shapeViewW,
					cmd.shapeViewH,
					cmd.shapeRot
				) != UIR_OK) {
				ReplayUnwindClips(&shapeDepth, &maskDepth);
				list->valid = false;
				return 0;
			}
			++shapeDepth;
			continue;
		}
		if (cmd.kind == UID_PAINT_CMD_SHAPE_CLIP_END) {
			UIR_BatchFlush();
			if (shapeDepth > 0) {
				UIR_EndShapeClip();
				--shapeDepth;
			}
			continue;
		}
		if (cmd.kind == UID_PAINT_CMD_IMAGE_MASK_BEGIN) {
			UIR_BatchFlush();
			if (cmd.pathCount != 1 || cmd.pathStart < 0 ||
			    cmd.pathStart + 1 > static_cast<int>(list->pathStrings.size())) {
				ReplayUnwindClips(&shapeDepth, &maskDepth);
				list->valid = false;
				return 0;
			}
			const char *maskSpec = list->pathStrings[static_cast<size_t>(cmd.pathStart)].c_str();
			if (UIR_BeginImageMask(
					cmd.clipX,
					cmd.clipY,
					cmd.clipW,
					cmd.clipH,
					maskSpec,
					static_cast<uir_image_fit_t>(cmd.shader)
				) != UIR_OK) {
				ReplayUnwindClips(&shapeDepth, &maskDepth);
				list->valid = false;
				return 0;
			}
			++maskDepth;
			continue;
		}
		if (cmd.kind == UID_PAINT_CMD_IMAGE_MASK_END) {
			UIR_BatchFlush();
			if (maskDepth > 0) {
				UIR_EndImageMask();
				--maskDepth;
			}
			continue;
		}
		if (cmd.kind != UID_PAINT_CMD_DRAW || cmd.vertCount < 3 || cmd.idxCount < 3) {
			continue;
		}
		if (cmd.vertStart < 0 || cmd.idxStart < 0 ||
		    cmd.vertStart + cmd.vertCount > static_cast<int>(list->verts.size()) ||
		    cmd.idxStart + cmd.idxCount > static_cast<int>(list->idxs.size())) {
			ReplayUnwindClips(&shapeDepth, &maskDepth);
			list->valid = false;
			return 0;
		}
		UIR_BatchTriangles(
			cmd.shader,
			&list->verts[static_cast<size_t>(cmd.vertStart)],
			cmd.vertCount,
			&list->idxs[static_cast<size_t>(cmd.idxStart)],
			cmd.idxCount
		);
	}
	if (flushBookends || shapeDepth > 0 || maskDepth > 0) {
		UIR_BatchFlush();
	}
	ReplayUnwindClips(&shapeDepth, &maskDepth);
	return 1;
}


int UID_PaintListTryReplay(uid_document_t *doc, const uid_backend_t *backend)
{
	if (!g_paintList || !doc) {
		return 0;
	}
	if ((doc->dirty & (UID_DIRTY_PAINT | UID_DIRTY_LAYOUT | UID_DIRTY_STRUCTURE)) != 0) {
		return 0;
	}
	uid_paint_list_t *list = ListOf(doc);
	if (!ReplayList(doc, list, backend, 0, 1)) {
		return 0;
	}
	doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty & ~UID_DIRTY_PAINT);
	return 1;
}

static void BeginRecordList(uid_paint_list_t *list, uid_document_t *doc)
{
	ClearLiveOpacityFlagsForList(doc, list);
	UIR_BatchFlush();
	ClearList(list);
	g_recording = list;
	g_recordPauseDepth = 0;

	uir_paint_recorder_t rec{};
	rec.onDraw = OnRecordDraw;
	rec.onClip = OnRecordClip;
	rec.userdata = list;
	UIR_BatchSetPaintRecorder(&rec);
}

static void EndRecordList(uid_document_t *doc, uid_paint_list_t *list, int allowEmpty)
{
	while (g_recordPauseDepth > 0) {
		UID_PaintListResumeRecord();
	}
	UIR_BatchFlush();
	UIR_BatchSetPaintRecorder(nullptr);
	g_recording = nullptr;
	g_recordPauseDepth = 0;
	if (!list) {
		return;
	}

	if (list->sawHostDraw) {
		ClearList(list);
		return;
	}
	if (list->cmds.empty()) {
		if (allowEmpty) {
			list->uiPxScale = doc ? doc->lastUiPxScale : 1.0f;
			list->logicalW = doc ? doc->lastLogicalW : 0;
			list->logicalH = doc ? doc->lastLogicalH : 0;
			list->valid = true;
			return;
		}
		ClearList(list);
		return;
	}

	CollapseClipCommands(list);
	if (list->cmds.empty()) {
		if (allowEmpty) {
			list->uiPxScale = doc ? doc->lastUiPxScale : 1.0f;
			list->logicalW = doc ? doc->lastLogicalW : 0;
			list->logicalH = doc ? doc->lastLogicalH : 0;
			list->valid = true;
			return;
		}
		ClearList(list);
		return;
	}
	MergeDrawCommands(list->cmds, list->verts, list->idxs);
	ComputeListStaticBox(list);

	list->uiPxScale = doc ? doc->lastUiPxScale : 1.0f;
	list->logicalW = doc ? doc->lastLogicalW : 0;
	list->logicalH = doc ? doc->lastLogicalH : 0;
	list->valid = true;
}

void UID_PaintListBeginRecord(uid_document_t *doc)
{
	if (!g_paintList || !doc) {
		return;
	}
	EnsurePaintList(doc);
	BeginRecordList(ListOf(doc), doc);
}

uid_node_id_t UID_PaintChromeRootId(const uid_document_t *doc);

void UID_PaintListEndRecord(uid_document_t *doc)
{
	EndRecordList(doc, ListOf(doc), 0);
}

static bool RegionPaintKind(uid_node_kind_t kind)
{
	switch (kind) {
	case UID_NODE_CONTAINER:
	case UID_NODE_LABEL:
	case UID_NODE_BUTTON:
	case UID_NODE_INPUT:
	case UID_NODE_TOGGLE:
	case UID_NODE_SLIDER:
	case UID_NODE_SELECT:
	case UID_NODE_KEYBIND:
	case UID_NODE_SHAPE_INSTANCE:
	case UID_NODE_IMAGE:
	case UID_NODE_MODEL:
	case UID_NODE_SERVER_LIST:
	case UID_NODE_FOREACH:
		return true;
	default:
		return false;
	}
}

uid_node_id_t UID_PaintChromeRootId(const uid_document_t *doc)
{
	if (!doc || doc->rootNode == UID_INVALID_NODE_ID) {
		return UID_INVALID_NODE_ID;
	}
	auto mit = doc->idIndex.find("menu_root");
	if (mit != doc->idIndex.end()) {
		return mit->second;
	}
	return doc->rootNode;
}

static bool ChromeRootBlocksRegions(const uid_document_t *doc, uid_node_id_t rootId)
{
	const uid_node_def_t *node = UID_GetNode(doc, rootId);
	if (!node) {
		return true;
	}
	std::string mask;
	if (node->properties.Get("mask-image", &mask) && !mask.empty()) {
		return true;
	}
	std::string shape;
	if (node->properties.Get("shape", &shape) && !shape.empty() && shape != "rectangle" &&
	    !node->children.empty()) {
		return true;
	}
	return false;
}

static void CollectPaintChildren(const uid_document_t *doc, uid_node_id_t id, std::vector<uid_node_id_t> *out)
{
	const uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node || !out) {
		return;
	}
	for (uid_node_id_t c : node->children) {
		const uid_node_def_t *child = UID_GetNode(doc, c);
		if (child && RegionPaintKind(child->kind)) {
			out->push_back(c);
		}
	}
}

static uid_node_id_t UnwrapRegionParent(const uid_document_t *doc, uid_node_id_t chromeRoot)
{
	uid_node_id_t id = chromeRoot;
	for (int guard = 0; guard < 8; ++guard) {
		std::vector<uid_node_id_t> kids;
		CollectPaintChildren(doc, id, &kids);
		if (kids.size() != 1) {
			break;
		}
		id = kids[0];
	}
	return id;
}

/*
 * Added in Omaha: Phase 4.6 — a layout-only container (no paint, visibility or
 * style props of its own) can be split into one region per child so a turning
 * compass does not redraw the kill feed sharing its row.
 */
static bool NodeIsPureWrapper(const uid_document_t *doc, uid_node_id_t id)
{
	static const char *const kPaintProps[] = {
		"fill", "stroke", "stroke-width", "shape", "mask-image", "src", "background-image",
		"background", "gradient", "edge-halo", "style", "class", "visible", "opacity",
		"overflow", "translate-x", "translate-y", "rotate", "scale", "skewl", "skewr", "crisp"
	};
	const uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node || node->kind != UID_NODE_CONTAINER) {
		return false;
	}
	for (const char *p : kPaintProps) {
		std::string v;
		if (node->properties.Get(p, &v)) {
			return false;
		}
	}
	return !node->exprBoundProps.empty() ? false : node->cvarBoundProps.empty();
}

static void CollectRegionRoots(const uid_document_t *doc, uid_node_id_t chromeRoot, std::vector<uid_node_id_t> *out)
{
	if (!doc || !out) {
		return;
	}
	const uid_node_id_t parent = UnwrapRegionParent(doc, chromeRoot);
	std::vector<uid_node_id_t> kids;
	CollectPaintChildren(doc, parent, &kids);
	uid_node_id_t hudMain = UID_INVALID_NODE_ID;
	auto hit = doc->idIndex.find("hud_main");
	if (hit != doc->idIndex.end()) {
		hudMain = hit->second;
	}
	std::vector<uid_node_id_t> rows;
	for (uid_node_id_t c : kids) {
		if (c == hudMain) {
			CollectPaintChildren(doc, c, &rows);
		} else {
			rows.push_back(c);
		}
	}
	for (uid_node_id_t r : rows) {
		std::vector<uid_node_id_t> cells;
		if (NodeIsPureWrapper(doc, r)) {
			CollectPaintChildren(doc, r, &cells);
		}
		if (cells.size() >= 2) {
			out->insert(out->end(), cells.begin(), cells.end());
		} else {
			out->push_back(r);
		}
	}
}

static void MarkRegionSubtree(uid_document_t *doc, uid_node_id_t id, int region)
{
	if (!doc || id < 0 || static_cast<size_t>(id) >= doc->regionOf.size()) {
		return;
	}
	doc->regionOf[static_cast<size_t>(id)] = region;
	const uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node) {
		return;
	}
	for (uid_node_id_t c : node->children) {
		MarkRegionSubtree(doc, c, region);
	}
}

static uid_paint_regions_t *EnsureRegions(uid_document_t *doc)
{
	if (!doc) {
		return nullptr;
	}
	if (!doc->paintRegions) {
		doc->paintRegions = new uid_paint_regions_t();
	}
	return RegionsOf(doc);
}

void UID_RebuildPaintRegions(uid_document_t *doc)
{
	uid_paint_regions_t *regs = EnsureRegions(doc);
	if (!doc || !regs) {
		return;
	}
	if (doc->parentOf.size() != doc->nodes.size()) {
		UID_RebuildParentMap(doc);
	}
	regs->chromeRoot = UID_PaintChromeRootId(doc);
	regs->usable = (regs->chromeRoot != UID_INVALID_NODE_ID) && !ChromeRootBlocksRegions(doc, regs->chromeRoot);
	regs->regions.clear();
	/* Phase 4.6: region ownership changed — the retained image must be rebuilt from a clear. */
	regs->needFullClear = true;
	regs->planPartial = false;
	doc->regionOf.assign(doc->nodes.size(), -1);
	if (!regs->usable) {
		doc->regionsStale = false;
		return;
	}

	std::vector<uid_node_id_t> roots;
	CollectRegionRoots(doc, regs->chromeRoot, &roots);
	regs->regions.resize(roots.size() + 1);
	regs->regions[0].root = regs->chromeRoot;
	regs->regions[0].list.valid = false;
	doc->regionOf[static_cast<size_t>(regs->chromeRoot)] = 0;
	for (size_t i = 0; i < roots.size(); ++i) {
		regs->regions[i + 1].root = roots[i];
		regs->regions[i + 1].list.valid = false;
		MarkRegionSubtree(doc, roots[i], static_cast<int>(i + 1));
	}
	for (uid_paint_region_t &r : regs->regions) {
		AabbReset(&r.prevBox);
		AabbReset(&r.list.staticBox);
		r.forceDirty = false;
		r.planDirty = true;
		r.planLiveOnly = false;
	}
	doc->regionsStale = false;
}

void UID_PaintRegionsInvalidateAll(uid_document_t *doc)
{
	uid_paint_regions_t *regs = RegionsOf(doc);
	if (!regs) {
		return;
	}
	for (uid_paint_region_t &r : regs->regions) {
		r.list.valid = false;
	}
}

void UID_PaintRegionsInvalidateNode(uid_document_t *doc, uid_node_id_t nodeId)
{
	if (!doc) {
		return;
	}
	if (doc->regionsStale || doc->regionOf.size() != doc->nodes.size()) {
		UID_PaintRegionsInvalidateAll(doc);
		return;
	}
	if (nodeId < 0 || static_cast<size_t>(nodeId) >= doc->regionOf.size()) {
		UID_PaintRegionsInvalidateAll(doc);
		return;
	}
	const int region = doc->regionOf[static_cast<size_t>(nodeId)];
	if (region < 0) {
		uid_paint_regions_t *regs = RegionsOf(doc);
		if (regs && nodeId == regs->chromeRoot) {
			if (!regs->regions.empty()) {
				regs->regions[0].list.valid = false;
			}
			return;
		}
		/* Under chrome but not in one child region (wrapper) — drop all. */
		if (regs && regs->chromeRoot != UID_INVALID_NODE_ID) {
			uid_node_id_t walk = nodeId;
			int           hops = 0;
			while (walk != UID_INVALID_NODE_ID && hops < 64) {
				if (walk == regs->chromeRoot) {
					UID_PaintRegionsInvalidateAll(doc);
					return;
				}
				if (static_cast<size_t>(walk) >= doc->parentOf.size()) {
					break;
				}
				walk = doc->parentOf[static_cast<size_t>(walk)];
				++hops;
			}
		}
		return;
	}
	uid_paint_regions_t *regs = RegionsOf(doc);
	if (regs && region >= 0 && static_cast<size_t>(region) < regs->regions.size()) {
		regs->regions[static_cast<size_t>(region)].list.valid = false;
	}
}

int UID_PaintRegionsPrepare(uid_document_t *doc)
{
	if (!UID_PaintRegionsEnabled() || !doc) {
		return 0;
	}
	if (doc->regionsStale || !doc->paintRegions || doc->regionOf.size() != doc->nodes.size()) {
		UID_RebuildPaintRegions(doc);
	}
	uid_paint_regions_t *regs = RegionsOf(doc);
	return (regs && regs->usable && !regs->regions.empty()) ? 1 : 0;
}

int UID_PaintRegionsUsable(const uid_document_t *doc)
{
	const uid_paint_regions_t *regs = RegionsOfC(doc);
	return (UID_PaintRegionsEnabled() && regs && regs->usable && !regs->regions.empty()) ? 1 : 0;
}

int UID_PaintRegionsCount(const uid_document_t *doc)
{
	const uid_paint_regions_t *regs = RegionsOfC(doc);
	return regs ? static_cast<int>(regs->regions.size()) : 0;
}

uid_node_id_t UID_PaintRegionRootId(const uid_document_t *doc, int region)
{
	const uid_paint_regions_t *regs = RegionsOfC(doc);
	if (!regs || region < 0 || static_cast<size_t>(region) >= regs->regions.size()) {
		return UID_INVALID_NODE_ID;
	}
	return regs->regions[static_cast<size_t>(region)].root;
}

int UID_PaintRegionCanReplay(const uid_document_t *doc, int region)
{
	const uid_paint_regions_t *regs = RegionsOfC(doc);
	if (!regs || region < 0 || static_cast<size_t>(region) >= regs->regions.size()) {
		return 0;
	}
	const uid_paint_list_t *list = &regs->regions[static_cast<size_t>(region)].list;
	if (!list->valid) {
		return 0;
	}
	if (list->uiPxScale != doc->lastUiPxScale || list->logicalW != doc->lastLogicalW ||
	    list->logicalH != doc->lastLogicalH) {
		return 0;
	}
	return 1;
}

int UID_PaintRegionReplay(uid_document_t *doc, int region, const uid_backend_t *backend)
{
	uid_paint_regions_t *regs = RegionsOf(doc);
	if (!regs || region < 0 || static_cast<size_t>(region) >= regs->regions.size()) {
		return 0;
	}
	return ReplayList(doc, &regs->regions[static_cast<size_t>(region)].list, backend, 1, 0);
}

void UID_PaintRegionBeginRecord(uid_document_t *doc, int region)
{
	uid_paint_regions_t *regs = RegionsOf(doc);
	if (!regs || region < 0 || static_cast<size_t>(region) >= regs->regions.size()) {
		return;
	}
	BeginRecordList(&regs->regions[static_cast<size_t>(region)].list, doc);
}

void UID_PaintRegionEndRecord(uid_document_t *doc, int region)
{
	uid_paint_regions_t *regs = RegionsOf(doc);
	if (!regs || region < 0 || static_cast<size_t>(region) >= regs->regions.size()) {
		return;
	}
	EndRecordList(doc, &regs->regions[static_cast<size_t>(region)].list, 1);
}

void UID_PaintRegionMarkInvalid(uid_document_t *doc, int region)
{
	uid_paint_regions_t *regs = RegionsOf(doc);
	if (!regs || region < 0 || static_cast<size_t>(region) >= regs->regions.size()) {
		return;
	}
	regs->regions[static_cast<size_t>(region)].list.valid = false;
}

/*
 * Added in Omaha: Phase 4.6 — retained UI target, partial redraw.
 *
 * A region is "clean" when replaying it would write exactly the pixels it
 * wrote last frame: its list is valid for the current surface and every live
 * block would replay with the same offset / fade multiplier. Clean regions are
 * skipped entirely; dirty regions get their previous and predicted rects
 * cleared and are repainted. Any clean region overlapping a cleared rect joins
 * the dirty set (closure) so paint order inside the cleared area is rebuilt.
 */
namespace {

static const float kRetainPadPx = 2.0f;

static uid_paint_aabb_t PadBox(const uid_paint_aabb_t &b)
{
	uid_paint_aabb_t r = b;
	if (r.valid) {
		r.x0 -= kRetainPadPx;
		r.y0 -= kRetainPadPx;
		r.x1 += kRetainPadPx;
		r.y1 += kRetainPadPx;
	}
	return r;
}

/* Offset ReplayLiveOffset would use this frame; false when it would recapture instead. */
static bool LivePredictOffset(
	const uid_document_t *doc,
	const uid_live_cache_t &live,
	float *dx,
	float *dy
)
{
	if (live.node == UID_INVALID_NODE_ID || static_cast<size_t>(live.node) >= doc->states.size()) {
		return false;
	}
	const uid_node_state_t &st = doc->states[static_cast<size_t>(live.node)];
	*dx = st.borderBox.x - live.originX;
	*dy = st.borderBox.y - live.originY;
	return true;
}

static bool LiveReplayUnchanged(const uid_document_t *doc, const uid_live_cache_t &live, const uid_backend_t *backend)
{
	if (!live.valid || live.unsupported || !live.hasLast) {
		return false;
	}
	if (live.node == UID_INVALID_NODE_ID || static_cast<size_t>(live.node) >= doc->states.size()) {
		return false;
	}
	const uid_node_state_t &st = doc->states[static_cast<size_t>(live.node)];
	if (live.kind == UID_PAINT_CMD_LIVE_SUBTREE) {
		if (!g_liveTranslateCache) {
			return false;
		}
		float dx = 0.0f, dy = 0.0f;
		if (!LivePredictOffset(doc, live, &dx, &dy)) {
			return false;
		}
		return dx == live.lastDx && dy == live.lastDy;
	}
	if (live.kind == UID_PAINT_CMD_LIVE_OPACITY) {
		if (!g_liveOpacityCache) {
			return false;
		}
		const float mul = st.lifetimeOpacityMul;
		if (mul <= 0.001f && live.lastMul <= 0.001f) {
			return true;
		}
		return std::fabs(mul - live.lastMul) < 0.0005f;
	}
	return false;
}

/* Box a live block covers when replayed with (dx,dy,mul); clipped to its ambient clip. */
static uid_paint_aabb_t LiveBoxAt(const uid_live_cache_t &live, float dx, float dy, float mul)
{
	uid_paint_aabb_t r;
	AabbReset(&r);
	if (!live.valid || !live.vertBox.valid || mul <= 0.001f) {
		return r;
	}
	uid_paint_aabb_t shifted = live.vertBox;
	shifted.x0 += dx;
	shifted.x1 += dx;
	shifted.y0 += dy;
	shifted.y1 += dy;
	return AabbClip(shifted, live.clipX, live.clipY, live.clipW, live.clipH);
}

static bool RegionIsClean(const uid_document_t *doc, const uid_paint_region_t &r, const uid_backend_t *backend)
{
	const uid_paint_list_t &list = r.list;
	if (r.forceDirty || !list.valid) {
		return false;
	}
	if (list.uiPxScale != doc->lastUiPxScale || list.logicalW != doc->lastLogicalW ||
	    list.logicalH != doc->lastLogicalH) {
		return false;
	}
	for (const uid_live_cache_t &live : list.lives) {
		if (!LiveReplayUnchanged(doc, live, backend)) {
			return false;
		}
	}
	return true;
}

/* Predicted box a region will paint this frame. known=false when some live must recapture. */
static uid_paint_aabb_t RegionPredictBox(
	const uid_document_t *doc,
	const uid_paint_region_t &r,
	const uid_backend_t *backend,
	const uid_rect_t &rootClip,
	bool *known
)
{
	uid_paint_aabb_t box = r.list.staticBox;
	*known = r.list.valid;
	if (r.list.valid) {
		for (const uid_live_cache_t &live : r.list.lives) {
			if (!live.valid || live.unsupported || !live.vertBox.valid ||
			    live.node == UID_INVALID_NODE_ID || static_cast<size_t>(live.node) >= doc->states.size()) {
				*known = false;
				continue;
			}
			const uid_node_state_t &st = doc->states[static_cast<size_t>(live.node)];
			if (live.kind == UID_PAINT_CMD_LIVE_SUBTREE) {
				float dx = 0.0f, dy = 0.0f;
				if (!LivePredictOffset(doc, live, &dx, &dy)) {
					*known = false;
					continue;
				}
				AabbUnion(&box, LiveBoxAt(live, dx, dy, 1.0f));
			} else {
				AabbUnion(&box, LiveBoxAt(live, 0.0f, 0.0f, st.lifetimeOpacityMul));
			}
		}
	}
	return PadBox(AabbClip(box, rootClip.x, rootClip.y, rootClip.w, rootClip.h));
}

/* Box a region actually painted this frame (lives at their last replay params). */
static uid_paint_aabb_t RegionPaintedBox(const uid_paint_region_t &r, const uid_rect_t &rootClip)
{
	uid_paint_aabb_t box = r.list.staticBox;
	if (r.list.valid) {
		for (const uid_live_cache_t &live : r.list.lives) {
			if (live.hasLast) {
				AabbUnion(&box, LiveBoxAt(live, live.lastDx, live.lastDy, live.lastMul));
			}
		}
	}
	return PadBox(AabbClip(box, rootClip.x, rootClip.y, rootClip.w, rootClip.h));
}

static uid_rect_t g_retainRootClip;

static int ClassifyDirtyReason(
	const uid_document_t *doc,
	const uid_paint_region_t &r,
	const uid_backend_t *backend,
	int *liveOnly
)
{
	*liveOnly = 0;
	const uid_paint_list_t &list = r.list;
	if (r.forceDirty) {
		return 1; /* force */
	}
	if (!list.valid) {
		return 2; /* invalid */
	}
	if (list.uiPxScale != doc->lastUiPxScale || list.logicalW != doc->lastLogicalW ||
	    list.logicalH != doc->lastLogicalH) {
		return 3; /* scale */
	}
	int tx = 0, op = 0, other = 0;
	for (const uid_live_cache_t &live : list.lives) {
		if (LiveReplayUnchanged(doc, live, backend)) {
			continue;
		}
		if (live.kind == UID_PAINT_CMD_LIVE_SUBTREE) {
			tx++;
		} else if (live.kind == UID_PAINT_CMD_LIVE_OPACITY) {
			op++;
		} else {
			other++;
		}
	}
	if (tx + op + other == 0) {
		return 0; /* clean — should not be called */
	}
	*liveOnly = 1;
	if (tx > 0 && op == 0 && other == 0) {
		return 4; /* liveTx */
	}
	if (op > 0 && tx == 0 && other == 0) {
		return 5; /* liveOp */
	}
	return 6; /* liveOther / mixed */
}

/*
 * Replay only lives whose translate/opacity changed. Skips static DRAW/mask/shape
 * cmds (H47/H50: those dominated Look replayUs while lives alone are 1–2 draws).
 * Returns 0 if any live must fall back to a chrome walk (hole under static).
 */
static int ReplayLivesOnly(uid_document_t *doc, uid_paint_list_t *list, const uid_backend_t *backend)
{
	if (!doc || !list || !list->valid || !backend) {
		return 0;
	}
	for (const uid_paint_cmd_t &cmd : list->cmds) {
		if (cmd.kind != UID_PAINT_CMD_LIVE_SUBTREE && cmd.kind != UID_PAINT_CMD_LIVE_OPACITY) {
			continue;
		}
		if (cmd.vertStart < 0 || cmd.vertStart >= static_cast<int>(list->lives.size())) {
			return 0;
		}
		uid_live_cache_t *live = &list->lives[static_cast<size_t>(cmd.vertStart)];
		if (LiveReplayUnchanged(doc, *live, backend)) {
			continue;
		}
		if (live->clipW > 0.0f && live->clipH > 0.0f) {
			UIR_ApplyClipRect(live->clipX, live->clipY, live->clipW, live->clipH);
		}
		const int ok = (cmd.kind == UID_PAINT_CMD_LIVE_OPACITY)
			? ReplayOrCaptureLiveOpacity(doc, live, backend)
			: ReplayOrCaptureLive(doc, live, backend);
		if (!ok) {
			return 0;
		}
	}
	return 1;
}

static void CollectLiveClearRects(
	const uid_document_t *doc,
	const uid_paint_region_t &r,
	const uid_backend_t *backend,
	std::vector<uid_paint_aabb_t> *out
)
{
	if (!r.list.valid || !out) {
		return;
	}
	for (const uid_live_cache_t &live : r.list.lives) {
		if (!live.valid || live.unsupported || !live.vertBox.valid) {
			continue;
		}
		if (LiveReplayUnchanged(doc, live, backend)) {
			continue;
		}
		if (live.hasLast) {
			uid_paint_aabb_t prev = PadBox(LiveBoxAt(live, live.lastDx, live.lastDy, live.lastMul));
			if (prev.valid) {
				out->push_back(prev);
			}
		}
		if (live.kind == UID_PAINT_CMD_LIVE_SUBTREE) {
			float dx = 0.0f, dy = 0.0f;
			if (LivePredictOffset(doc, live, &dx, &dy)) {
				uid_paint_aabb_t cur = PadBox(LiveBoxAt(live, dx, dy, 1.0f));
				if (cur.valid) {
					out->push_back(cur);
				}
			}
		} else if (live.node != UID_INVALID_NODE_ID &&
		           static_cast<size_t>(live.node) < doc->states.size()) {
			const float mul = doc->states[static_cast<size_t>(live.node)].lifetimeOpacityMul;
			uid_paint_aabb_t cur = PadBox(LiveBoxAt(live, 0.0f, 0.0f, mul));
			if (cur.valid) {
				out->push_back(cur);
			}
		}
	}
}

} // namespace

int UID_PaintRegionsRetainPlan(
	uid_document_t *doc,
	const uid_backend_t *backend,
	float clipX,
	float clipY,
	float clipW,
	float clipH
)
{
	uid_paint_regions_t *regs = RegionsOf(doc);
	if (!doc || !regs) {
		return 0;
	}
	g_retainRootClip.x = clipX;
	g_retainRootClip.y = clipY;
	g_retainRootClip.w = clipW;
	g_retainRootClip.h = clipH;
	regs->planPartial = false;
	for (uid_paint_region_t &r : regs->regions) {
		r.planDirty = true;
		r.planLiveOnly = false;
	}
	/* This document owns the retainable session (eligibility for the next frame). */
	UIR_BatchRetainClaim();
	if (!UIR_BatchTargetRetained()) {
		regs->needFullClear = false;
		g_retainOwner = doc;
		return 0;
	}
	if (regs->needFullClear || g_retainOwner != doc) {
		UIR_BatchTargetDropRetained();
		regs->needFullClear = false;
		g_retainOwner = doc;
		return 0;
	}

	const size_t n = regs->regions.size();
	std::vector<uid_paint_aabb_t> cur(n);
	std::vector<char>             known(n, 0);
	std::vector<uid_paint_aabb_t> dirtyRects;
	dirtyRects.reserve(n * 2);
	for (size_t i = 0; i < n; ++i) {
		uid_paint_region_t &r = regs->regions[i];
		bool k = false;
		cur[i] = RegionPredictBox(doc, r, backend, g_retainRootClip, &k);
		known[i] = k ? 1 : 0;
		r.planDirty = !RegionIsClean(doc, r, backend);
		if (r.planDirty) {
			int liveOnly = 0;
			(void)ClassifyDirtyReason(doc, r, backend, &liveOnly);
			(void)liveOnly;
			/*
			 * Live-only clear+replay is disabled: AABB intersection with staticBox
			 * still missed cases (timer/score flash) and fade clears with closure=0
			 * punched holes in skipped regions (weapon icon disappear). Dirty regions
			 * always full-clear + full list replay; clean regions still skip.
			 */
			r.planLiveOnly = false;
			if (r.prevBox.valid) {
				dirtyRects.push_back(r.prevBox);
			}
			if (k && cur[i].valid) {
				dirtyRects.push_back(cur[i]);
			}
		}
	}
	/* Closure: clean regions touching a cleared rect must repaint in order. */
	bool changed = true;
	while (changed) {
		changed = false;
		for (size_t i = 0; i < n; ++i) {
			uid_paint_region_t &r = regs->regions[i];
			if (r.planDirty) {
				continue;
			}
			bool hit = false;
			for (const uid_paint_aabb_t &d : dirtyRects) {
				if (AabbIntersects(d, r.prevBox) || AabbIntersects(d, cur[i])) {
					hit = true;
					break;
				}
			}
			if (hit) {
				r.planDirty = true;
				r.planLiveOnly = false; /* paint order rebuild needs full region */
				changed = true;
				if (r.prevBox.valid) {
					dirtyRects.push_back(r.prevBox);
				}
				if (cur[i].valid) {
					dirtyRects.push_back(cur[i]);
				}
			}
		}
	}
	/* Clear what dirty regions owned (and will own, when predictable). */
	for (size_t i = 0; i < n; ++i) {
		uid_paint_region_t &r = regs->regions[i];
		if (!r.planDirty) {
			continue;
		}
		uid_paint_aabb_t clr = r.prevBox;
		if (known[i]) {
			AabbUnion(&clr, cur[i]);
		}
		if (clr.valid) {
			UIR_BatchClearRect(clr.x0, clr.y0, clr.x1 - clr.x0, clr.y1 - clr.y0);
		}
	}
	regs->planPartial = true;
	return 1;
}

int UID_PaintRegionRetainSkip(const uid_document_t *doc, int region)
{
	const uid_paint_regions_t *regs = RegionsOfC(doc);
	if (!regs || !regs->planPartial || region < 0 || static_cast<size_t>(region) >= regs->regions.size()) {
		return 0;
	}
	return regs->regions[static_cast<size_t>(region)].planDirty ? 0 : 1;
}

int UID_PaintRegionRetainLiveOnly(const uid_document_t *doc, int region)
{
	const uid_paint_regions_t *regs = RegionsOfC(doc);
	if (!regs || !regs->planPartial || region < 0 || static_cast<size_t>(region) >= regs->regions.size()) {
		return 0;
	}
	const uid_paint_region_t &r = regs->regions[static_cast<size_t>(region)];
	return (r.planDirty && r.planLiveOnly) ? 1 : 0;
}

int UID_PaintRegionReplayLives(uid_document_t *doc, int region, const uid_backend_t *backend)
{
	uid_paint_regions_t *regs = RegionsOf(doc);
	if (!regs || region < 0 || static_cast<size_t>(region) >= regs->regions.size()) {
		return 0;
	}
	uid_paint_region_t &r = regs->regions[static_cast<size_t>(region)];
	if (!r.planLiveOnly) {
		return 0;
	}
	if (ReplayLivesOnly(doc, &r.list, backend)) {
		return 1;
	}
	/*
	 * Live miss used a chrome walk or failed: static under the punched hole may
	 * be gone. Clear the full prior box and fall back to a full list replay.
	 */
	if (r.prevBox.valid) {
		UIR_BatchClearRect(
			r.prevBox.x0,
			r.prevBox.y0,
			r.prevBox.x1 - r.prevBox.x0,
			r.prevBox.y1 - r.prevBox.y0
		);
	}
	r.planLiveOnly = false;
	return ReplayList(doc, &r.list, backend, 1, 0);
}

void UID_PaintRegionNoteDrawn(uid_document_t *doc, int region)
{
	uid_paint_regions_t *regs = RegionsOf(doc);
	if (!regs || region < 0 || static_cast<size_t>(region) >= regs->regions.size()) {
		return;
	}
	uid_paint_region_t &r = regs->regions[static_cast<size_t>(region)];
	if (!r.list.valid) {
		/* Unrecordable content: its pixels are unknown, so the target cannot be kept. */
		AabbReset(&r.prevBox);
		regs->needFullClear = true;
		r.forceDirty = true;
		return;
	}
	r.prevBox = RegionPaintedBox(r, g_retainRootClip);
	r.forceDirty = false;
}

void UID_PaintRegionsRetainEnd(uid_document_t *doc)
{
	uid_paint_regions_t *regs = RegionsOf(doc);
	if (!regs) {
		return;
	}
	if (regs->planPartial) {
		/*
		 * A repainted region whose new box (unknown at plan time) now overlaps a
		 * later clean region sits above pixels that should be on top of it.
		 * Force that region dirty so the next frame rebuilds the overlap in order.
		 */
		const size_t n = regs->regions.size();
		for (size_t i = 0; i < n; ++i) {
			const uid_paint_region_t &ri = regs->regions[i];
			if (!ri.planDirty || !ri.prevBox.valid) {
				continue;
			}
			for (size_t j = i + 1; j < n; ++j) {
				uid_paint_region_t &rj = regs->regions[j];
				if (!rj.planDirty && AabbIntersects(ri.prevBox, rj.prevBox)) {
					rj.forceDirty = true;
				}
			}
		}
	}
	regs->planPartial = false;
}

void UID_PaintRegionsRetainDrop(uid_document_t *doc)
{
	uid_paint_regions_t *regs = RegionsOf(doc);
	if (UIR_BatchTargetRetained()) {
		UIR_BatchTargetDropRetained();
	}
	if (regs) {
		regs->needFullClear = true;
		regs->planPartial = false;
	}
	if (g_retainOwner == doc) {
		g_retainOwner = nullptr;
	}
}
