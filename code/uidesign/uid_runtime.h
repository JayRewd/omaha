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
#ifndef UID_RUNTIME_H
#define UID_RUNTIME_H

#include "uid_backend.h"
#include "uid_document.h"
#include "uid_types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

typedef struct uid_runtime_s uid_runtime_t;

uid_runtime_t *UID_Create(const uid_backend_t *backend, const uid_limits_t *limits);
void           UID_Destroy(uid_runtime_t *runtime);

uid_result_t UID_LoadFile(uid_runtime_t *runtime, const char *vfsPath);
uid_result_t UID_LoadMemory(uid_runtime_t *runtime, const char *sourceName, const char *xml, size_t xmlSize);
uid_result_t UID_Reload(uid_runtime_t *runtime);

void UID_SetSurface(uid_runtime_t *runtime, int logicalW, int logicalH, int framebufferW, int framebufferH);
/* Added in OPM: authored-px multiplier (refScale × ui_scale). */
void  UID_SetUiPxScale(uid_runtime_t *runtime, float uiPxScale);
float UID_GetUiPxScale(const uid_runtime_t *runtime);
void UID_Update(uid_runtime_t *runtime, int realtime, const uid_pointer_state_t *pointer);
void UID_DrawChrome(uid_runtime_t *runtime);
void UID_DrawOverlay(uid_runtime_t *runtime);

bool UID_KeyEvent(uid_runtime_t *runtime, int key, bool down, unsigned time);
bool UID_CharEvent(uid_runtime_t *runtime, unsigned codepoint);
void UID_Deactivate(uid_runtime_t *runtime);

/* True while a keybind control is waiting for the next key. */
bool UID_IsCapturingKeybind(const uid_runtime_t *runtime);

bool                 UID_HasDocument(const uid_runtime_t *runtime);
const uid_document_t *UID_GetDocument(const uid_runtime_t *runtime);

/* Added in OPM: evaluate canvas pointer="{bool expr}"; false when unset or eval fails. */
bool UID_RuntimeWantsPointer(const uid_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif /* UID_RUNTIME_H */
