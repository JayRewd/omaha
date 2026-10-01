/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#ifndef CL_UIMENU_DISPATCHER_H
#define CL_UIMENU_DISPATCHER_H

#include "../uidesign/uid_backend.h"
#include "../uidesign/uid_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct uid_runtime_s;

void CL_UIMenu_Init(uid_backend_t *backend);
void CL_UIMenu_Shutdown(void);
void CL_UIMenu_ReloadRegistry(void);
void CL_UIMenu_List_f(void);
void CL_UIMenu_SyncAutoMenus(void);

qboolean CL_UIMenu_Open(const char *menuId, qboolean persistent);
qboolean CL_UIMenu_Close(const char *menuId);
qboolean CL_UIMenu_Hold(const char *menuId, qboolean down);
qboolean CL_UIMenu_OpenHold(const char *menuId);
qboolean CL_UIMenu_CloseHold(const char *menuId);
qboolean CL_UIMenu_IsOpen(const char *menuId);
qboolean CL_UIMenu_HasAnyOpen(void);
qboolean CL_UIMenu_HasInteractiveOpen(void);
int      CL_UIMenu_GetOpenCount(void);

void     CL_UIMenu_UpdateAll(unsigned int time);
void     CL_UIMenu_ApplySurface(int lw, int lh, int fw, int fh);
void     CL_UIMenu_ApplyUiPxScale(float scale);
void     CL_UIMenu_UpdateAllWithPointer(unsigned int time, const void *pointer);
qboolean CL_UIMenu_HasMenusUpTo(int maxDrawOrder);
qboolean CL_UIMenu_HasPointerMenuOpen(void);
void     CL_UIMenu_PaintChromeUpTo(int maxDrawOrder);
void     CL_UIMenu_PaintOverlayUpTo(int maxDrawOrder);
void     CL_UIMenu_PaintChrome(void);
void     CL_UIMenu_PaintOverlay(void);
qboolean CL_UIMenu_KeyEvent(int key, qboolean down, unsigned time);
qboolean CL_UIMenu_CharEvent(int ch);
qboolean CL_UIMenu_ShouldOwnInput(void);
qboolean CL_UIMenu_IsCapturingKeybind(void);

qboolean CL_UIMenu_TopmostWantsMenuWorld(uid_menu_backdrop_t *outBackdrop);
struct uid_runtime_s *CL_UIMenu_TopmostMenuWorldRuntime(void);
struct uid_runtime_s *CL_UIMenu_RuntimeForInput(void);
struct uid_runtime_s *CL_UIMenu_RuntimeById(const char *menuId);

int      CL_UIMenu_PointerWheelDelta(void);
void     CL_UIMenu_ClearPointerWheelDelta(void);
void     CL_UIMenu_AddPointerWheelDelta(int delta);

void     CL_UIMenu_CloseAllInteractive(void);
void     CL_UIMenu_OnSessionDeactivate(void);
void     CL_UIMenu_RegisterCommands(void);
void     CL_UIMenu_UnregisterCommands(void);

#ifdef __cplusplus
}
#endif

#endif /* CL_UIMENU_DISPATCHER_H */
