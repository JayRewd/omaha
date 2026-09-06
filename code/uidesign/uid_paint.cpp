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

#include "../uirender/uir_batch.h"
#include "../uirender/uir_compositor.h"
#include "../uirender/uir_stencil.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

static int g_paintList = 1;

enum {
	UID_PAINT_CMD_CLIP = 0,
	UID_PAINT_CMD_DRAW = 1,
	UID_PAINT_CMD_SHAPE_CLIP_BEGIN = 2,
	UID_PAINT_CMD_SHAPE_CLIP_END = 3
};

struct uid_paint_cmd_t {
	int   kind;
	float clipX, clipY, clipW, clipH;
	int   shader;
	int   vertStart;
	int   vertCount;
	int   idxStart;
	int   idxCount;
	/* Shape clip begin: dest rect in clipX..clipH; paths in pathStrings[pathStart..). */
	float shapeViewW, shapeViewH, shapeRot;
	int   pathStart;
	int   pathCount;
};

struct uid_paint_list_t {
	std::vector<uid_paint_cmd_t> cmds;
	std::vector<uir_vert_t>      verts;
	std::vector<unsigned short>  idxs;
	std::vector<std::string>     pathStrings;
	bool                         valid;
	bool                         sawHostDraw;
	bool                         sawImageMask; /* Fixed in Omaha: soft mask-image not replayable */
	float                        uiPxScale;
	int                          logicalW;
	int                          logicalH;
};

static uid_paint_list_t *g_recording = nullptr;

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

void ClearList(uid_paint_list_t *list)
{
	if (!list) {
		return;
	}
	list->cmds.clear();
	list->verts.clear();
	list->idxs.clear();
	list->pathStrings.clear();
	list->valid = false;
	list->sawHostDraw = false;
	list->sawImageMask = false;
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

} // namespace

void UID_SetPaintList(int enabled)
{
	g_paintList = enabled ? 1 : 0;
	if (!g_paintList) {
		UIR_BatchSetPaintRecorder(nullptr);
		g_recording = nullptr;
	}
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

void UID_PaintListFree(uid_document_t *doc)
{
	if (!doc || !doc->paintList) {
		return;
	}
	if (g_recording == doc->paintList) {
		UIR_BatchSetPaintRecorder(nullptr);
		g_recording = nullptr;
	}
	delete static_cast<uid_paint_list_t *>(doc->paintList);
	doc->paintList = nullptr;
}

void UID_PaintListMarkHostDraw(void)
{
	if (g_recording) {
		g_recording->sawHostDraw = true;
	}
}

/* Added in Omaha: soft mask-image composites via layer RT; batch replay skips that. */
void UID_PaintListMarkImageMask(void)
{
	if (g_recording) {
		g_recording->sawImageMask = true;
	}
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
	if (!g_recording || !pathD || pathCount <= 0) {
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
	if (!g_recording) {
		return;
	}
	UIR_BatchFlush();
	uid_paint_cmd_t cmd{};
	cmd.kind = UID_PAINT_CMD_SHAPE_CLIP_END;
	g_recording->cmds.push_back(cmd);
}

int UID_PaintListTryReplay(uid_document_t *doc)
{
	if (!g_paintList || !doc) {
		return 0;
	}
	uid_paint_list_t *list = ListOf(doc);
	if (!list || !list->valid || list->cmds.empty()) {
		return 0;
	}
	if ((doc->dirty & (UID_DIRTY_PAINT | UID_DIRTY_LAYOUT | UID_DIRTY_STRUCTURE)) != 0) {
		return 0;
	}
	if (list->uiPxScale != doc->lastUiPxScale || list->logicalW != doc->lastLogicalW ||
	    list->logicalH != doc->lastLogicalH) {
		list->valid = false;
		return 0;
	}

	UIR_BatchFlush();
	int shapeDepth = 0;
	for (const uid_paint_cmd_t &cmd : list->cmds) {
		if (cmd.kind == UID_PAINT_CMD_CLIP) {
			UIR_BatchFlush();
			UIR_ForceClipRect(cmd.clipX, cmd.clipY, cmd.clipW, cmd.clipH);
			continue;
		}
		if (cmd.kind == UID_PAINT_CMD_SHAPE_CLIP_BEGIN) {
			UIR_BatchFlush();
			if (cmd.pathCount <= 0 || cmd.pathStart < 0 ||
			    cmd.pathStart + cmd.pathCount > static_cast<int>(list->pathStrings.size())) {
				while (shapeDepth > 0) {
					UIR_EndShapeClip();
					--shapeDepth;
				}
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
				while (shapeDepth > 0) {
					UIR_EndShapeClip();
					--shapeDepth;
				}
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
		if (cmd.kind != UID_PAINT_CMD_DRAW || cmd.vertCount < 3 || cmd.idxCount < 3) {
			continue;
		}
		if (cmd.vertStart < 0 || cmd.idxStart < 0 ||
		    cmd.vertStart + cmd.vertCount > static_cast<int>(list->verts.size()) ||
		    cmd.idxStart + cmd.idxCount > static_cast<int>(list->idxs.size())) {
			while (shapeDepth > 0) {
				UIR_EndShapeClip();
				--shapeDepth;
			}
			list->valid = false;
			return 0;
		}
		/* Replay submits through the live batch path (flush→draw). */
		UIR_BatchTriangles(
			cmd.shader,
			&list->verts[static_cast<size_t>(cmd.vertStart)],
			cmd.vertCount,
			&list->idxs[static_cast<size_t>(cmd.idxStart)],
			cmd.idxCount
		);
	}
	UIR_BatchFlush();
	while (shapeDepth > 0) {
		UIR_EndShapeClip();
		--shapeDepth;
	}

	doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty & ~UID_DIRTY_PAINT);
	return 1;
}

void UID_PaintListBeginRecord(uid_document_t *doc)
{
	if (!g_paintList || !doc) {
		return;
	}
	/* Drop any pending batch before attaching the recorder. */
	UIR_BatchFlush();
	EnsurePaintList(doc);
	uid_paint_list_t *list = ListOf(doc);
	ClearList(list);
	g_recording = list;

	uir_paint_recorder_t rec{};
	rec.onDraw = OnRecordDraw;
	rec.onClip = OnRecordClip;
	rec.userdata = list;
	UIR_BatchSetPaintRecorder(&rec);
}

void UID_PaintListEndRecord(uid_document_t *doc)
{
	UIR_BatchFlush();
	UIR_BatchSetPaintRecorder(nullptr);

	uid_paint_list_t *list = ListOf(doc);
	g_recording = nullptr;
	if (!list) {
		return;
	}

	if (list->sawHostDraw || list->sawImageMask || list->cmds.empty()) {
		ClearList(list);
		return;
	}

	list->uiPxScale = doc ? doc->lastUiPxScale : 1.0f;
	list->logicalW = doc ? doc->lastLogicalW : 0;
	list->logicalH = doc ? doc->lastLogicalH : 0;
	list->valid = true;
}
