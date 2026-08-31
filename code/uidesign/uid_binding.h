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
#ifndef UID_BINDING_H
#define UID_BINDING_H

#include "uid_backend.h"
#include "uid_document.h"
#include "uid_types.h"

#include <string>

/*
 * Local mirrors of q_shared.h cvar flags used to refuse UI writes.
 * CVAR_INIT=0x0010, CVAR_ROM=0x0040, CVAR_PROTECTED=0x10000.
 */
enum {
	UID_CVAR_INIT      = 0x0010,
	UID_CVAR_ROM       = 0x0040,
	UID_CVAR_PROTECTED = 0x10000
};

#define UID_CVAR_WRITE_DENIED (UID_CVAR_INIT | UID_CVAR_ROM | UID_CVAR_PROTECTED)

/*
 * Parse bind="cvar:name" (canonical) or bind="cvar(name)" (compat).
 * On success writes the cvar name into *cvarNameOut and returns true.
 */
bool UID_ParseCvarBind(const char *bind, std::string *cvarNameOut);

/*
 * Added in OPM: parse bind="item.field:name" (or item.field.name) for read-only
 * collection selection driven by an enclosing foreach item field.
 */
bool UID_ParseItemFieldBind(const char *bind, std::string *fieldNameOut);

/*
 * Pull external state into node runtime values:
 * - cvar binds via backend->cvarDescribe
 * - keybind display names via getKeysForCommand / keyNumToName
 * - label text-cvar attrs
 * - select optionSource via queryOptions (once into node.options when empty)
 * Marks Layout when displayed text may change size, else Paint.
 */
void UID_SyncBindings(uid_document_t *doc, const uid_backend_t *backend);

/*
 * Push the node's staged runtimeValue to the bound cvar (or keybind adapter).
 * Respects commit mode: CHANGE/SUBMIT/APPLY all write when called; callers
 * invoke this on change, submit/blur, or apply respectively.
 * Refuses INIT/ROM/PROTECTED cvars. Returns UID_OK, UID_ERR_INVALID_ARG,
 * or UID_ERR_VALIDATE when denied / write fails.
 */
uid_result_t UID_WriteBinding(uid_document_t *doc, uid_node_id_t nodeId, const uid_backend_t *backend);

/* Write every apply-mode cvar bind and every keybind node. */
uid_result_t UID_WriteAllBindings(uid_document_t *doc, const uid_backend_t *backend);

/* Added in Omaha: clear commit=apply staged runtime so sync can pull again (defaults). */
void UID_ClearApplyStagedBindings(uid_document_t *doc);

/* Added in OPM: collection scope bind helpers. */
std::string UID_TransformCvarToUi(
	const uid_node_def_t &node,
	const std::string &cvarValue,
	const uid_backend_t *backend
);

/* Added in OPM: keybind display strings from empty-label / capture-label attrs. */
std::string UID_KeybindEmptyLabel(const uid_node_def_t &node);
std::string UID_KeybindCaptureLabel(const uid_node_def_t &node);

/* Added in OPM: keybind capture with optional confirm-modal on conflict. */
uid_result_t UID_TryCommitKeybindCapture(
	uid_document_t *doc,
	uid_node_id_t nodeId,
	int capturedKey,
	const uid_backend_t *backend
);

/* Added in OPM: commit keybind from ui_modal_bind_* context cvars (modal yes). */
uid_result_t UID_CommitKeybindFromModalCvars(uid_document_t *doc, const uid_backend_t *backend);

/* Added in OPM: shared cvar reads for shape props / cvar-rgba fills. */
bool   UID_ReadCvarString(const uid_backend_t *backend, const char *name, std::string *out);
double UID_ReadCvarNumber(const uid_backend_t *backend, const char *name, double fallback);
bool   UID_ResolvePropString(const uid_backend_t *backend, const std::string &input, std::string *out);
bool   UID_ResolveCvarRgba(const uid_backend_t *backend, const char *spec, uid_color_t *out);
/* Added in OPM: exact {cvar:name} / {cvar.name} property bindings */
bool   UID_ParseExactCvarBraceBinding(const std::string &value, std::string *cvarNameOut);
void   UID_RegisterCvarBoundProps(uid_node_def_t *node);

/* Added in OPM: evaluate runtime numeric expressions on layout/paint props. */
bool UID_EvalRuntimeNumericExpr(
	uid_document_t      *doc,
	uid_node_id_t        nodeId,
	const std::string   &expr,
	const uid_backend_t *backend,
	double              *out
);

#endif /* UID_BINDING_H */
