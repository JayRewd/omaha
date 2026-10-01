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

#include "uid_runtime.h"

#include "uid_binding.h"
#include "uid_compile.h"
#include "uid_diag.h"
#include "uid_expr_bool.h"
#include "uid_input.h"
#include "uid_layout.h"
#include "uid_profile.h"
#include "uid_template.h"
#include "uid_widget.h"
#include "uid_xml.h"

#include <cstring>
#include <new>
#include <string>
#include <cstdio>

struct uid_runtime_s {
	uid_backend_t     backend;
	uid_limits_t      limits;
	uid_document_t   *doc;

	std::string       lastVfsPath;
	std::string       lastMemoryName;
	std::string       lastMemoryXml;
	bool              haveMemorySource;
	bool              haveFileSource;

	int               logicalW;
	int               logicalH;
	int               framebufferW;
	int               framebufferH;
	float             fbScale;
	float             uiPxScale; /* Added in OPM: authored px × refScale × ui_scale */
};

namespace {

void ReportDiags(const uid_backend_t *backend, const uid_diag_list_t &diags, const char *fallbackPath)
{
	if (!backend || !backend->diag) {
		return;
	}
	for (const uid_diag_t &item : diags.Items()) {
		const char *path = item.location.path;
		if (!path || !path[0]) {
			path = fallbackPath;
		}
		backend->diag(
			static_cast<int>(item.severity),
			path,
			item.location.line,
			item.message.c_str(),
			backend->userdata
		);
	}
}

float ComputeFbScale(int logicalW, int logicalH, int framebufferW, int framebufferH)
{
	float sx = 1.0f;
	float sy = 1.0f;
	if (logicalW > 0 && framebufferW > 0) {
		sx = static_cast<float>(framebufferW) / static_cast<float>(logicalW);
	}
	if (logicalH > 0 && framebufferH > 0) {
		sy = static_cast<float>(framebufferH) / static_cast<float>(logicalH);
	}
	return (sx + sy) * 0.5f;
}

uid_result_t AdoptExpandedDocument(uid_runtime_t *runtime, uid_document_t *fresh, const char *sourceName)
{
	if (!runtime || !fresh) {
		UID_DestroyDocument(fresh);
		return UID_ERR_INVALID_ARG;
	}

	UID_ProfileBegin(UID_PROF_LOAD_ADOPT);

	uid_document_t *old = runtime->doc;

	/* Migrate compatible retained state by expanded id before destroying old. */
	if (old) {
		std::string focusId;
		for (size_t i = 0; i < old->nodes.size() && i < old->states.size(); ++i) {
			if (old->states[i].focused && !old->nodes[i].id.empty()) {
				focusId = old->nodes[i].id;
				break;
			}
		}

		for (size_t i = 0; i < fresh->nodes.size(); ++i) {
			const uid_node_def_t &fn = fresh->nodes[i];
			if (fn.id.empty()) {
				continue;
			}
			auto it = old->idIndex.find(fn.id);
			if (it == old->idIndex.end()) {
				continue;
			}
			const uid_node_id_t oid = it->second;
			if (oid < 0 || static_cast<size_t>(oid) >= old->nodes.size() ||
				static_cast<size_t>(oid) >= old->states.size()) {
				continue;
			}
			const uid_node_def_t &on = old->nodes[static_cast<size_t>(oid)];
			if (on.kind != fn.kind) {
				continue;
			}
			if (i >= fresh->states.size()) {
				fresh->states.resize(fresh->nodes.size());
				for (uid_node_state_t &st : fresh->states) {
					UID_InitNodeState(&st);
				}
			}
			uid_node_state_t &dst = fresh->states[i];
			const uid_node_state_t &src = old->states[static_cast<size_t>(oid)];

			dst.scrollX = src.scrollX;
			dst.scrollY = src.scrollY;

			if (src.runtimeValue.hasValue) {
				if (fn.kind == UID_NODE_KEYBIND) {
					if (on.binding == fn.binding) {
						dst.runtimeValue = src.runtimeValue;
					}
				} else if (on.bind == fn.bind) {
					dst.runtimeValue = src.runtimeValue;
				}
			}
			if (fn.kind == UID_NODE_INPUT) {
				dst.editBuffer = src.editBuffer;
				dst.preEditValue = src.preEditValue;
				dst.caretCodepoint = src.caretCodepoint;
				dst.anchorCodepoint = src.anchorCodepoint;
			}

			/* Clear captures / overlays / drags on adopt. */
			dst.capturing = false;
			dst.overlayOpen = false;
			dst.dragging = false;
			dst.hovered = false;
			dst.pressed = false;
			dst.focused = false;
			dst.highlightIndex = -1;
		}

		if (!focusId.empty()) {
			auto fit = fresh->idIndex.find(focusId);
			if (fit != fresh->idIndex.end()) {
				const uid_node_id_t fid = fit->second;
				if (fid >= 0 && static_cast<size_t>(fid) < fresh->nodes.size() &&
					static_cast<size_t>(fid) < fresh->states.size() &&
					UID_IsInteractiveKind(fresh->nodes[static_cast<size_t>(fid)].kind)) {
					fresh->states[static_cast<size_t>(fid)].focused = true;
				}
			}
		}
	}

	runtime->doc = fresh;
	if (sourceName && sourceName[0]) {
		runtime->doc->sourceName = sourceName;
	}
	runtime->doc->dirty = static_cast<uid_dirty_flags_t>(
		UID_DIRTY_STRUCTURE | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT | UID_DIRTY_BINDING
	);
	UID_DestroyDocument(old);
	UID_ProfileEnd(UID_PROF_LOAD_ADOPT);
	return UID_OK;
}

uid_result_t LoadParsedDocument(
	uid_runtime_t *runtime,
	const char *sourceName,
	const char *xml,
	size_t xmlSize
)
{
	if (!runtime || !xml) {
		return UID_ERR_INVALID_ARG;
	}

	UID_ProfileSetLoadLabel(sourceName);

	uid_document_t *fresh = UID_CreateDocument();
	if (!fresh) {
		return UID_ERR_OVERFLOW;
	}
	fresh->limits = runtime->limits;
	/* Own the source path before parse/expand/compile so locations can rebind to it. */
	fresh->sourceName = (sourceName && sourceName[0]) ? sourceName : "";

	uid_diag_list_t diags(runtime->limits.maxDiagnostics);
	uid_parse_io_t io;
	io.readFile = runtime->backend.readFile;
	io.freeFile = runtime->backend.freeFile;
	UID_ProfileBegin(UID_PROF_LOAD_PARSE);
	uid_result_t result = UID_ParseXml(
		fresh->sourceName.c_str(),
		xml,
		xmlSize,
		&runtime->limits,
		&io,
		fresh,
		&diags
	);
	UID_ProfileEnd(UID_PROF_LOAD_PARSE);
	if (result != UID_OK) {
		ReportDiags(&runtime->backend, diags, fresh->sourceName.c_str());
		UID_DestroyDocument(fresh);
		return result;
	}

	UID_ProfileBegin(UID_PROF_LOAD_EXPAND);
	result = UID_ExpandDocument(fresh, &diags);
	UID_ProfileEnd(UID_PROF_LOAD_EXPAND);
	if (result != UID_OK) {
		ReportDiags(&runtime->backend, diags, fresh->sourceName.c_str());
		UID_DestroyDocument(fresh);
		return result;
	}

	UID_ProfileBegin(UID_PROF_LOAD_COMPILE);
	result = UID_CompileDocument(fresh, &diags);
	UID_ProfileEnd(UID_PROF_LOAD_COMPILE);
	if (result != UID_OK) {
		ReportDiags(&runtime->backend, diags, fresh->sourceName.c_str());
		UID_DestroyDocument(fresh);
		return result;
	}

	/* Surface warnings/info even on success. */
	ReportDiags(&runtime->backend, diags, fresh->sourceName.c_str());
	return AdoptExpandedDocument(runtime, fresh, fresh->sourceName.c_str());
}

void ClearTransientState(uid_document_t *doc)
{
	if (!doc) {
		return;
	}
	for (uid_node_state_t &st : doc->states) {
		st.hovered = false;
		st.pressed = false;
		st.focused = false;
		st.capturing = false;
		st.overlayOpen = false;
		st.dragging = false;
		st.highlightIndex = -1;
		st.editBuffer.clear();
		st.caretCodepoint = 0;
		st.anchorCodepoint = 0;
	}
	doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty | UID_DIRTY_PAINT);
}

} // namespace

uid_runtime_t *UID_Create(const uid_backend_t *backend, const uid_limits_t *limits)
{
	if (!backend) {
		return nullptr;
	}

	uid_runtime_t *runtime = new (std::nothrow) uid_runtime_t();
	if (!runtime) {
		return nullptr;
	}

	std::memset(&runtime->backend, 0, sizeof(runtime->backend));
	runtime->backend = *backend;
	if (limits) {
		runtime->limits = *limits;
	} else {
		UID_DefaultLimits(&runtime->limits);
	}

	runtime->doc = nullptr;
	runtime->haveMemorySource = false;
	runtime->haveFileSource = false;
	runtime->logicalW = 0;
	runtime->logicalH = 0;
	runtime->framebufferW = 0;
	runtime->framebufferH = 0;
	runtime->fbScale = 1.0f;
	runtime->uiPxScale = 1.0f;
	return runtime;
}

void UID_Destroy(uid_runtime_t *runtime)
{
	if (!runtime) {
		return;
	}
	UID_DestroyDocument(runtime->doc);
	runtime->doc = nullptr;
	delete runtime;
}

uid_result_t UID_LoadFile(uid_runtime_t *runtime, const char *vfsPath)
{
	if (!runtime || !vfsPath || !vfsPath[0]) {
		return UID_ERR_INVALID_ARG;
	}
	if (!runtime->backend.readFile || !runtime->backend.freeFile) {
		return UID_ERR_NOT_READY;
	}

	/* Added in OPM: phase timings for ui_profile (read → parse → expand → compile). */
	UID_ProfileResetLoad();
	UID_ProfileSetLoadLabel(vfsPath);

	UID_ProfileBegin(UID_PROF_LOAD_READ);
	void *buf = nullptr;
	const long size = runtime->backend.readFile(vfsPath, &buf);
	UID_ProfileEnd(UID_PROF_LOAD_READ);
	if (size < 0 || !buf) {
		if (runtime->backend.diag) {
			runtime->backend.diag(
				static_cast<int>(UID_SEVERITY_ERROR),
				vfsPath,
				0,
				"failed to read UI design file",
				runtime->backend.userdata
			);
		}
		return UID_ERR_IO;
	}

	const uid_result_t result = LoadParsedDocument(
		runtime,
		vfsPath,
		static_cast<const char *>(buf),
		static_cast<size_t>(size)
	);
	runtime->backend.freeFile(buf);

	if (result == UID_OK) {
		runtime->lastVfsPath = vfsPath;
		runtime->haveFileSource = true;
		runtime->haveMemorySource = false;
		runtime->lastMemoryName.clear();
		runtime->lastMemoryXml.clear();
	}
	return result;
}

uid_result_t UID_LoadMemory(uid_runtime_t *runtime, const char *sourceName, const char *xml, size_t xmlSize)
{
	if (!runtime || !xml) {
		return UID_ERR_INVALID_ARG;
	}

	const char *name = (sourceName && sourceName[0]) ? sourceName : "<memory>";
	UID_ProfileResetLoad();
	UID_ProfileSetLoadLabel(name);
	const uid_result_t result = LoadParsedDocument(runtime, name, xml, xmlSize);
	if (result == UID_OK) {
		runtime->lastMemoryName = name;
		runtime->lastMemoryXml.assign(xml, xmlSize);
		runtime->haveMemorySource = true;
		runtime->haveFileSource = false;
		runtime->lastVfsPath.clear();
	}
	return result;
}

uid_result_t UID_Reload(uid_runtime_t *runtime)
{
	if (!runtime) {
		return UID_ERR_INVALID_ARG;
	}
	if (runtime->haveFileSource && !runtime->lastVfsPath.empty()) {
		return UID_LoadFile(runtime, runtime->lastVfsPath.c_str());
	}
	if (runtime->haveMemorySource) {
		return UID_LoadMemory(
			runtime,
			runtime->lastMemoryName.c_str(),
			runtime->lastMemoryXml.data(),
			runtime->lastMemoryXml.size()
		);
	}
	return UID_ERR_NOT_READY;
}

void UID_SetSurface(uid_runtime_t *runtime, int logicalW, int logicalH, int framebufferW, int framebufferH)
{
	if (!runtime) {
		return;
	}

	const bool changed =
		runtime->logicalW != logicalW ||
		runtime->logicalH != logicalH ||
		runtime->framebufferW != framebufferW ||
		runtime->framebufferH != framebufferH;

	runtime->logicalW = logicalW;
	runtime->logicalH = logicalH;
	runtime->framebufferW = framebufferW;
	runtime->framebufferH = framebufferH;
	runtime->fbScale = ComputeFbScale(logicalW, logicalH, framebufferW, framebufferH);

	if (changed && runtime->doc) {
		runtime->doc->dirty = static_cast<uid_dirty_flags_t>(
			runtime->doc->dirty | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT
		);
	}
}

void UID_SetUiPxScale(uid_runtime_t *runtime, float uiPxScale)
{
	float s = uiPxScale;

	if (!runtime) {
		return;
	}
	if (!(s == s) || s <= 0.0f) {
		s = 1.0f;
	}
	if (s < 0.25f) {
		s = 0.25f;
	} else if (s > 9.0f) {
		s = 9.0f;
	}
	if (runtime->uiPxScale == s) {
		return;
	}
	runtime->uiPxScale = s;
	if (runtime->doc) {
		runtime->doc->lastUiPxScale = s;
		runtime->doc->dirty = static_cast<uid_dirty_flags_t>(
			runtime->doc->dirty | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT
		);
	}
}

float UID_GetUiPxScale(const uid_runtime_t *runtime)
{
	if (!runtime || !(runtime->uiPxScale > 0.0f)) {
		return 1.0f;
	}
	return runtime->uiPxScale;
}

void UID_Update(uid_runtime_t *runtime, int realtime, const uid_pointer_state_t *pointer)
{
	if (!runtime || !runtime->doc) {
		return;
	}

	uid_document_t *doc = runtime->doc;
	int             layoutRan = 0;
	/* Added in OPM: drive foreach lifetime / fade from update clock. */
	doc->updateTimeMs = realtime;
	UID_ProfileBegin(UID_PROF_FRAME_BIND);
	UID_SyncBindings(doc, &runtime->backend);
	UID_ProfileEnd(UID_PROF_FRAME_BIND);

	if (doc->dirty & (UID_DIRTY_STRUCTURE | UID_DIRTY_LAYOUT)) {
		uid_diag_list_t diags(runtime->limits.maxDiagnostics);
		UID_ProfileBegin(UID_PROF_FRAME_LAYOUT);
		UID_LayoutDocument(
			doc,
			runtime->logicalW,
			runtime->logicalH,
			runtime->fbScale,
			runtime->uiPxScale > 0.0f ? runtime->uiPxScale : 1.0f,
			&runtime->backend,
			&diags
		);
		UID_ProfileEnd(UID_PROF_FRAME_LAYOUT);
		layoutRan = 1;
		doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty & ~UID_DIRTY_STRUCTURE);
		ReportDiags(&runtime->backend, diags, doc->sourceName.c_str());
	}

	if (pointer) {
		UID_ProfileBegin(UID_PROF_FRAME_POINTER);
		UID_HandlePointer(doc, pointer, static_cast<unsigned int>(realtime), &runtime->backend);
		UID_ProfileEnd(UID_PROF_FRAME_POINTER);
	}

	if (UID_ProfileEnabled()) {
		UID_ProfileSetFrameMeta(layoutRan, static_cast<int>(doc->nodes.size()));
		if (!doc->sourceName.empty()) {
			UID_ProfileSetFrameLabel(doc->sourceName.c_str());
		}
	}
}

void UID_DrawChrome(uid_runtime_t *runtime)
{
	if (!runtime || !runtime->doc) {
		return;
	}
	UID_ProfileBegin(UID_PROF_FRAME_PAINT_CHROME);
	UID_PaintChrome(runtime->doc, &runtime->backend);
	UID_ProfileEnd(UID_PROF_FRAME_PAINT_CHROME);
}

void UID_DrawOverlay(uid_runtime_t *runtime)
{
	if (!runtime || !runtime->doc) {
		return;
	}
	UID_ProfileBegin(UID_PROF_FRAME_PAINT_OVERLAY);
	UID_PaintOverlay(runtime->doc, &runtime->backend);
	UID_ProfileEnd(UID_PROF_FRAME_PAINT_OVERLAY);
}

bool UID_KeyEvent(uid_runtime_t *runtime, int key, bool down, unsigned time)
{
	if (!runtime || !runtime->doc) {
		return false;
	}
	return UID_HandleKey(runtime->doc, key, down, time, &runtime->backend);
}

bool UID_CharEvent(uid_runtime_t *runtime, unsigned codepoint)
{
	if (!runtime || !runtime->doc) {
		return false;
	}
	return UID_HandleChar(runtime->doc, codepoint, &runtime->backend);
}

void UID_Deactivate(uid_runtime_t *runtime)
{
	if (!runtime || !runtime->doc) {
		return;
	}
	ClearTransientState(runtime->doc);
}

bool UID_IsCapturingKeybind(const uid_runtime_t *runtime)
{
	if (!runtime || !runtime->doc) {
		return false;
	}
	const uid_document_t *doc = runtime->doc;
	for (size_t i = 0; i < doc->nodes.size() && i < doc->states.size(); ++i) {
		if (doc->nodes[i].kind == UID_NODE_KEYBIND && doc->states[i].capturing) {
			return true;
		}
	}
	return false;
}

bool UID_HasDocument(const uid_runtime_t *runtime)
{
	return runtime && runtime->doc != nullptr;
}

const uid_document_t *UID_GetDocument(const uid_runtime_t *runtime)
{
	if (!runtime) {
		return nullptr;
	}
	return runtime->doc;
}

bool UID_RuntimeWantsPointer(const uid_runtime_t *runtime)
{
	if (!runtime || !runtime->doc || runtime->doc->pointerExpr.empty()) {
		return false;
	}

	uid_bool_lookup_ctx_t ctx;
	std::memset(&ctx, 0, sizeof(ctx));
	ctx.backend = &runtime->backend;
	ctx.doc = runtime->doc;
	ctx.nodeId = UID_INVALID_NODE_ID;
	ctx.item = nullptr;
	ctx.itemIndex = -1;
	ctx.itemCount = 0;
	ctx.selectedIndex = -1;

	bool        result = false;
	std::string diag;
	if (!UID_EvalBool(runtime->doc->pointerExpr.c_str(), &ctx, nullptr, &result, &diag)) {
		return false;
	}
	return result;
}
