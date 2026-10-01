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

#pragma once

/*
 * Added in Omaha: client-only UI state store for internal ui_om_* / mailbox
 * values that must not appear in the console / cvarlist / tab completion.
 */

#include "../qcommon/q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cl_uivar_s cl_uivar_t;

void          CL_UIVar_Init(void);
qboolean      CL_UIVar_IsStoreName(const char *name);
cl_uivar_t   *CL_UIVar_Find(const char *name);
qboolean      CL_UIVar_IsEntry(const void *ptr);
void          CL_UIVar_Set(const char *name, const char *value);
void          CL_UIVar_SetValue(const char *name, float value);
const char   *CL_UIVar_String(const char *name);
int           CL_UIVar_Integer(const char *name);
float         CL_UIVar_Value(const char *name);
void          CL_UIVar_Reset(const char *name);
unsigned      CL_UIVar_ModCount(const char *name);
unsigned      CL_UIVar_ModCountEntry(const cl_uivar_t *entry);
const char   *CL_UIVar_EntryString(const cl_uivar_t *entry);
float         CL_UIVar_EntryValue(const cl_uivar_t *entry);
unsigned      CL_UIVar_Epoch(void);
/* True when name is a store entry (for purge of leftover archived cvars). */
qboolean      CL_UIVar_MatchesStoreFamily(const char *name);

#ifdef __cplusplus
}
#endif
