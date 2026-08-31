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
#ifndef CL_UIRENDER_H
#define CL_UIRENDER_H

#ifdef __cplusplus
extern "C" {
#endif

void CL_UIR_RegisterCvars(void);
void CL_UIR_RegisterBakeCommands(void);
void CL_UIR_Init(void);
void CL_UIR_EnsureStarted(void);
void CL_UIR_Shutdown(void);
void CL_UIR_OnRendererRegistration(void);
void CL_UIR_OnResolutionChanged(void);

/* Added in OPM: push surface + ui px scale into all open menu runtimes now. */
void CL_UIR_ApplyMenuSurfaceNow(void);

/* Cached launch-time selection (CVAR_INIT ui_legacy). Default = modern. */
qboolean CL_UIR_UseLegacyMain(void);

/*
 * Added in OPM: shared UI vid size for modern vs legacy.
 * Legacy → glconfig; modern → GetSurfaceSizes logical (matches design layout).
 */
void CL_UIR_GetUiVidSize(int *w, int *h);
/* Map SDL window-client mouse into UiVid size (no-op when sizes match / legacy). */
void CL_UIR_MapMouseToUiVid(float *x, float *y);

/* Added in OPM: in-game HUD pack routing (ui_om_hud + ui_legacy). */
const char *CL_UIR_ActiveHudId(void);
const char *CL_UIR_DmPauseMenuId(void);
qboolean    CL_UIR_UseLegacyHud(void);
qboolean    CL_UIR_UseModernHudPack(void);

/*
 * Added in OPM: in-HUD chat compose (messagemode) when the active pack has
 * hud_chat_input — replaces the legacy floating dm_console for that pack.
 */
qboolean CL_UIR_HudChatHasInput(void);
qboolean CL_UIR_HudChatIsOpen(void);
void     CL_UIR_OpenHudChat(int iMode);
void     CL_UIR_CloseHudChat(void);
void     CL_UIR_ToggleHudChat(int iMode);

/* Session selected modern and activated (does not imply render/input eligibility). */
qboolean CL_UIR_IsModernMainActive(void);
void CL_UIR_ActivateModernMain(void);
void CL_UIR_DeactivateModernMain(void);

/* Intro finished, CA_DISCONNECTED, !server_loading, !com_sv_running. */
qboolean CL_UIR_IsEligibleForModernMain(void);

/* If active but no longer eligible, deactivate (release world ownership). */
void CL_UIR_SyncEligibility(void);

/* True when the disconnected main surface should be owned by modern compositor. */
qboolean CL_UIR_ShouldRenderModernDisconnected(void);

/* Added in OPM: modern main overlay while connected (CA_ACTIVE). */
qboolean CL_UIR_IsConnectedOverlayOpen(void);
void     CL_UIR_OpenConnectedOverlay(void);
void     CL_UIR_CloseConnectedOverlay(void);
void     CL_UIR_ToggleConnectedOverlay(void);
qboolean CL_UIR_ShouldRenderConnectedOverlay(void);

/* Added in OPM: modern MP pause replacing UIFAKK dm_main when modern HUD is active. */
void     CL_UIR_OpenDmPause(const char *panel);
void     CL_UIR_CloseDmPause(void);
qboolean CL_UIR_IsDmPauseOpen(void);

/* Added in OPM: GUI mouse mode for modern UI surfaces (overlay / disconnected main). */
void CL_UIR_EnterModernInputMode(void);
void CL_UIR_EnterModernInputModeKeepKeys(void);
void CL_UIR_LeaveModernInputMode(void);

/* Active + eligible + legacy modal not owning input. */
qboolean CL_UIR_ShouldOwnInput(void);

void CL_UIR_RenderDisconnectedMain(void);
void CL_UIR_RenderModernOverlay(void);

/* Added in OPM: procedural crosshair via modern uirender (in-game HUD). */
void CL_UIR_DrawCrosshair(void);

/* Added in OPM: ui_profile dump helpers (XML load → paint phase timings). */
void CL_UIR_ProfileSyncFromCvar(void);
void CL_UIR_ProfileBeginSample(const char *label);
void CL_UIR_ProfileEndSample(const char *kind);
void CL_UIR_ProfileDumpLoad(void);

/* Design-format runtime (XML UI) — returns qtrue if the event was consumed. */
qboolean CL_UIR_KeyEvent(int key, qboolean down, unsigned time);
qboolean CL_UIR_CharEvent(int ch);
void     CL_UIR_UpdateModern(void); /* sync pointer + UID_Update before render */
void     CL_UIR_SyncPointerMenus(void); /* keep GUI mouse before input events */
qboolean CL_UIR_LegacyModalOwnsInput(void); /* console / bind / dialog / non-main menu */
qboolean CL_UIR_IsCapturingKeybind(void); /* design keybind capture mode */

#ifdef __cplusplus
}
#endif

#endif /* CL_UIRENDER_H */
