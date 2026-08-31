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

#include "client.h"
#include "cl_uimenu_dispatcher.h"
#include "cl_hud_registry.h"
#include "cl_ui.h"

extern "C" {
qboolean CL_UIR_UseLegacyMain(void);
qboolean CL_UIR_IsEligibleForModernMain(void);
qboolean CL_UIR_LegacyModalOwnsInput(void);
qboolean CL_UIR_UseLegacyHud(void);
qboolean CL_UIR_UseModernHudPack(void);
qboolean CL_UIR_HudChatIsOpen(void);
void     CL_UIR_CloseHudChat(void);
const char *CL_UIR_ActiveHudId(void);
void CL_UIR_EnsureStarted(void);
void CL_UIR_ApplyMenuSurfaceNow(void);
void CL_UIR_LeaveModernInputMode(void);
void CL_UIR_CloseDmPause(void);
void CL_UIR_ProfileDumpLoad(void);
void CL_UIR_ProfileSyncFromCvar(void);
}

#include "../uidesign/uid_runtime.h"
#include "../uidesign/uid_widget.h"
#include "../uidesign/uid_xml.h"
#include "../uidesign/uid_backend.h"
#include "../uidesign/uid_document.h"
#include "../uirender/uir_compositor.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct MenuRegistryEntry {
	std::string         vfsPath;
	int                 drawOrder;
	uid_menu_backdrop_t backdrop;
};

struct OpenMenuEntry {
	std::string         menuId;
	std::string         vfsPath;
	int                 drawOrder;
	uid_menu_backdrop_t backdrop;
	uid_runtime_t      *runtime;
	int                 holdRefCount;
	qboolean            persistent;
	qboolean            autoManaged;
	qboolean            isHudPack;
};

static uid_backend_t *g_backend = NULL;
static std::unordered_map<std::string, MenuRegistryEntry> g_registry;
static std::vector<OpenMenuEntry> g_openMenus;
static std::unordered_set<std::string> g_failedHudLoads;
static int g_pointerWheelDelta = 0;

static long uid_read_file(const char *path, void **buf)
{
	return FS_ReadFile(path, buf);
}

static void uid_free_file(void *buf)
{
	FS_FreeFile(buf);
}

static uid_parse_io_t g_parseIo = {uid_read_file, uid_free_file};

static int FindOpenIndex(const char *menuId)
{
	if (!menuId || !menuId[0]) {
		return -1;
	}
	for (size_t i = 0; i < g_openMenus.size(); ++i) {
		if (!Q_stricmp(g_openMenus[i].menuId.c_str(), menuId)) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

static void DestroyRuntime(uid_runtime_t *runtime)
{
	if (runtime) {
		UID_Destroy(runtime);
	}
}

static void SortOpenIndicesByDrawOrder(std::vector<int> *indices, qboolean ascending)
{
	if (!indices) {
		return;
	}
	std::sort(indices->begin(), indices->end(), [ascending](int a, int b) {
		const int da = g_openMenus[static_cast<size_t>(a)].drawOrder;
		const int db = g_openMenus[static_cast<size_t>(b)].drawOrder;
		if (da != db) {
			return ascending ? (da < db) : (da > db);
		}
		return ascending ? (a < b) : (a > b);
	});
}

static void UpdateInputCatcher(void)
{
	if (CL_UIMenu_ShouldOwnInput()) {
		Key_SetCatcher(Key_GetCatcher() | KEYCATCH_UI);
	} else if (!UI_LegacyOverlayOwnsInput() && !UI_ConsoleIsVisible() && !UI_BindActive()) {
		/*
		 * Fixed in OPM: use ConsoleIsVisible (not only focused) so opening the
		 * console during intermission scoreboard does not clear KEYCATCH_UI
		 * before the first CharEvent.
		 */
		Key_SetCatcher(Key_GetCatcher() & ~KEYCATCH_UI);
	}
}

static qboolean MenuWantsInput(const OpenMenuEntry &entry)
{
	return entry.drawOrder >= 5 ? qtrue : qfalse;
}

/*
 * Added in OPM: draw-order 4 HUD packs normally do not catch keys; while in-HUD
 * chat compose is open they must receive Key/Char events without being treated
 * as interactive menus for CloseAllInteractive.
 */
static qboolean MenuAcceptsKeys(const OpenMenuEntry &entry)
{
	if (MenuWantsInput(entry)) {
		return qtrue;
	}
	return (entry.isHudPack && CL_UIR_HudChatIsOpen()) ? qtrue : qfalse;
}

/*
 * Added in OPM: mirror match state used by canvas pointer="{...}" exprs.
 * Hold-TAB scoreboard stays cursorless unless spectator or intermission.
 */
static void SyncHudPointerStateCvars(void)
{
	static cvar_t *ui_om_intermission = NULL;
	static cvar_t *ui_om_spectator = NULL;
	int            intermission = 0;
	int            spectator = 0;

	if (!ui_om_intermission) {
		ui_om_intermission = Cvar_Get("ui_om_intermission", "0", CVAR_TEMP);
		ui_om_spectator = Cvar_Get("ui_om_spectator", "0", CVAR_TEMP);
		/* Added in OPM: sticky in-play scoreboard cursor; cleared on scoreboard CloseHold. */
		Cvar_Get("ui_om_scoreboard_cursor", "0", CVAR_TEMP);
	}

	if (clc.state == CA_ACTIVE && cl.snap.valid) {
		if (cl.snap.ps.pm_flags & PMF_INTERMISSION) {
			intermission = 1;
		}
		if ((cl.snap.ps.pm_flags & PMF_SPECTATING) || cl.snap.ps.stats[STAT_TEAM] == TEAM_SPECTATOR) {
			spectator = 1;
		}
	}

	if (ui_om_intermission->integer != intermission) {
		Cvar_Set("ui_om_intermission", intermission ? "1" : "0");
	}
	if (ui_om_spectator->integer != spectator) {
		Cvar_Set("ui_om_spectator", spectator ? "1" : "0");
	}
}

static qboolean MenuWantsPointer(const OpenMenuEntry &entry)
{
	if (!entry.runtime || !UID_HasDocument(entry.runtime)) {
		return qfalse;
	}
	return UID_RuntimeWantsPointer(entry.runtime) ? qtrue : qfalse;
}

static qboolean EnsureRuntimeLoaded(OpenMenuEntry *entry)
{
	if (!entry || !g_backend) {
		return qfalse;
	}
	if (entry->runtime && UID_HasDocument(entry->runtime)) {
		return qtrue;
	}
	if (entry->runtime) {
		DestroyRuntime(entry->runtime);
		entry->runtime = NULL;
	}
	entry->runtime = UID_Create(g_backend, NULL);
	if (!entry->runtime) {
		Com_Printf("UIMenu: UID_Create failed for '%s'\n", entry->menuId.c_str());
		return qfalse;
	}
	{
		const uid_result_t loadResult = UID_LoadFile(entry->runtime, entry->vfsPath.c_str());
		if (loadResult != UID_OK) {
			Com_Printf(
				"UIMenu: failed to load menu '%s' from '%s' (err=%d)\n",
				entry->menuId.c_str(),
				entry->vfsPath.c_str(),
				(int)loadResult
			);
			DestroyRuntime(entry->runtime);
			entry->runtime = NULL;
			return qfalse;
		}
		CL_UIR_ProfileDumpLoad();
	}
	return qtrue;
}

static qboolean OpenMenuInternal(
	const char *menuId,
	qboolean persistent,
	qboolean autoManaged,
	qboolean incrementHold
)
{
	if (!menuId || !menuId[0]) {
		return qfalse;
	}

	CL_UIR_EnsureStarted();

	auto regIt = g_registry.find(menuId);
	if (regIt == g_registry.end()) {
		Com_Printf("UIMenu: unknown menu id '%s'\n", menuId);
		return qfalse;
	}

	const int existing = FindOpenIndex(menuId);
	if (existing >= 0) {
		OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(existing)];
		if (incrementHold) {
			entry.holdRefCount++;
		}
		if (persistent) {
			entry.persistent = qtrue;
		}
		return qtrue;
	}

	OpenMenuEntry entry;
	entry.menuId = menuId;
	entry.vfsPath = regIt->second.vfsPath;
	entry.drawOrder = regIt->second.drawOrder;
	entry.backdrop = regIt->second.backdrop;
	entry.runtime = NULL;
	entry.holdRefCount = incrementHold ? 1 : 0;
	entry.persistent = persistent;
	entry.autoManaged = autoManaged;
	entry.isHudPack = qfalse;

	if (!EnsureRuntimeLoaded(&entry)) {
		return qfalse;
	}

	if (entry.drawOrder >= 5 && clc.state == CA_ACTIVE && !Q_stricmp(menuId, "main")) {
		Com_FakePause();
	}

	g_openMenus.push_back(entry);
	CL_UIR_ApplyMenuSurfaceNow();
	UpdateInputCatcher();
	return qtrue;
}

static qboolean OpenHudPackInternal(const char *hudId, qboolean autoManaged)
{
	const char *vfsPath;
	int         drawOrder;

	if (!hudId || !hudId[0] || CL_UIMenu_HudIsBuiltinLegacy(hudId)) {
		return qfalse;
	}

	if (g_failedHudLoads.count(hudId)) {
		return qfalse;
	}

	vfsPath = CL_UIMenu_HudPath(hudId);
	if (!vfsPath || !vfsPath[0]) {
		Com_Printf("UIMenu: unknown HUD pack id '%s'\n", hudId);
		return qfalse;
	}

	CL_UIR_EnsureStarted();

	const int existing = FindOpenIndex(hudId);
	if (existing >= 0) {
		return qtrue;
	}

	drawOrder = CL_UIMenu_HudDrawOrder(hudId);

	OpenMenuEntry entry;
	entry.menuId = hudId;
	entry.vfsPath = vfsPath;
	entry.drawOrder = drawOrder;
	entry.backdrop = UID_MENU_BACKDROP_NONE;
	entry.runtime = NULL;
	entry.holdRefCount = 0;
	entry.persistent = qtrue;
	entry.autoManaged = autoManaged;
	entry.isHudPack = qtrue;

	if (!EnsureRuntimeLoaded(&entry)) {
		g_failedHudLoads.insert(hudId);
		return qfalse;
	}

	g_failedHudLoads.erase(hudId);
	g_openMenus.push_back(entry);
	CL_UIR_ApplyMenuSurfaceNow();
	UpdateInputCatcher();
	return qtrue;
}

static void CloseHudPackMenus(const char *exceptHudId)
{
	for (int i = static_cast<int>(g_openMenus.size()) - 1; i >= 0; --i) {
		OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(i)];
		if (!entry.isHudPack) {
			continue;
		}
		if (exceptHudId && !Q_stricmp(entry.menuId.c_str(), exceptHudId)) {
			continue;
		}
		DestroyRuntime(entry.runtime);
		g_openMenus.erase(g_openMenus.begin() + i);
	}
	UpdateInputCatcher();
}

static qboolean CloseMenuInternal(const char *menuId, qboolean force)
{
	const int idx = FindOpenIndex(menuId);
	if (idx < 0) {
		return qfalse;
	}

	OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(idx)];
	if (!force) {
		if (entry.holdRefCount > 0) {
			return qfalse;
		}
		if (entry.autoManaged) {
			return qfalse;
		}
	}

	const qboolean wasPause =
		entry.drawOrder >= 5 && clc.state == CA_ACTIVE
		&& (!Q_stricmp(menuId, "main") || !Q_stricmp(menuId, "dm_pause")
		    || !Q_stricmp(menuId, "dm_pause_modern"));

	DestroyRuntime(entry.runtime);
	g_openMenus.erase(g_openMenus.begin() + idx);

	/*
	 * Fixed in OPM: ui_close menu dm_pause (and connected main) must leave GUI
	 * mouse mode. Escape uses CL_UIR_CloseDmPause which already does this; XML
	 * click cbufs only hit this path and previously left in_guimouse stuck until
	 * View3D ate a click.
	 */
	if (wasPause && !CL_UIMenu_HasInteractiveOpen()) {
		Com_FakeUnpause();
		CL_UIR_LeaveModernInputMode();
	}

	UpdateInputCatcher();
	return qtrue;
}

} // namespace

void CL_UIMenu_Init(uid_backend_t *backend)
{
	g_backend = backend;
	CL_UIMenu_ReloadRegistry();
	CL_UIMenu_ReloadHudRegistry();
	CL_UIMenu_ValidateHudCvar();
}

void CL_UIMenu_Shutdown(void)
{
	for (size_t i = 0; i < g_openMenus.size(); ++i) {
		DestroyRuntime(g_openMenus[i].runtime);
	}
	g_openMenus.clear();
	g_registry.clear();
	g_failedHudLoads.clear();
	g_backend = NULL;
	g_pointerWheelDelta = 0;
}

void CL_UIMenu_ReloadRegistry(void)
{
	g_registry.clear();
	g_failedHudLoads.clear();

	int numFiles = 0;
	char **files = FS_ListFiles("ui", "xml", qtrue, &numFiles);
	if (!files) {
		return;
	}

	for (int i = 0; i < numFiles; ++i) {
		char vfsPath[MAX_QPATH];
		uid_menu_meta_t meta;

		Com_sprintf(vfsPath, sizeof(vfsPath), "ui/%s", files[i]);
		if (UID_PeekMenuMetadata(vfsPath, &g_parseIo, &meta, NULL) != UID_OK || !meta.valid) {
			continue;
		}

		auto existing = g_registry.find(meta.menuId);
		if (existing != g_registry.end()) {
			Com_DPrintf(
				"UIMenu: overriding menu id '%s' (%s -> %s)\n",
				meta.menuId,
				existing->second.vfsPath.c_str(),
				vfsPath
			);
		}

		MenuRegistryEntry entry;
		entry.vfsPath = vfsPath;
		entry.drawOrder = meta.drawOrder;
		entry.backdrop = meta.backdrop;
		g_registry[meta.menuId] = entry;
	}

	FS_FreeFileList(files);

	CL_UIMenu_ReloadHudRegistry();
	CL_UIMenu_ValidateHudCvar();

	for (size_t i = 0; i < g_openMenus.size(); ++i) {
		OpenMenuEntry &open = g_openMenus[i];
		if (open.isHudPack) {
			const char *vfsPath = CL_UIMenu_HudPath(open.menuId.c_str());
			if (!vfsPath || !vfsPath[0]) {
				continue;
			}
			open.vfsPath = vfsPath;
			open.drawOrder = CL_UIMenu_HudDrawOrder(open.menuId.c_str());
			if (open.runtime) {
				UID_LoadFile(open.runtime, open.vfsPath.c_str());
				CL_UIR_ProfileDumpLoad();
			}
			continue;
		}
		auto regIt = g_registry.find(open.menuId);
		if (regIt == g_registry.end()) {
			continue;
		}
		open.vfsPath = regIt->second.vfsPath;
		open.drawOrder = regIt->second.drawOrder;
		open.backdrop = regIt->second.backdrop;
		if (open.runtime) {
			UID_LoadFile(open.runtime, open.vfsPath.c_str());
			CL_UIR_ProfileDumpLoad();
		}
	}
}

void CL_UIMenu_List_f(void)
{
	Com_Printf("UIMenu registry (%d entries):\n", (int)g_registry.size());
	for (const auto &kv : g_registry) {
		const char *backdrop = kv.second.backdrop == UID_MENU_BACKDROP_MENU_MAP ? "menu-map" : "none";
		const int openIdx = FindOpenIndex(kv.first.c_str());
		Com_Printf(
			"  id='%s' draw=%d backdrop=%s path='%s'%s\n",
			kv.first.c_str(),
			kv.second.drawOrder,
			backdrop,
			kv.second.vfsPath.c_str(),
			openIdx >= 0 ? " [open]" : ""
		);
	}
}

void CL_UIMenu_SyncAutoMenus(void)
{
	if (CL_UIR_UseLegacyMain()) {
		return;
	}

	if (CL_UIR_UseLegacyHud()) {
		CloseHudPackMenus(NULL);
	} else if (clc.state == CA_ACTIVE) {
		const char *activeHud = CL_UIR_ActiveHudId();
		CloseHudPackMenus(activeHud);
		if (activeHud && activeHud[0] && !CL_UIMenu_HudIsBuiltinLegacy(activeHud)) {
			if (!CL_UIMenu_IsOpen(activeHud)) {
				OpenHudPackInternal(activeHud, qtrue);
			}
		}
	} else {
		CloseHudPackMenus(NULL);
	}

	if (CL_UIR_IsEligibleForModernMain()) {
		if (!CL_UIMenu_IsOpen("main")) {
			OpenMenuInternal("main", qtrue, qtrue, qfalse);
		}
	} else if (clc.state != CA_ACTIVE) {
		CloseMenuInternal("main", qtrue);
	}
}

qboolean CL_UIMenu_Open(const char *menuId, qboolean persistent)
{
	return OpenMenuInternal(menuId, persistent, qfalse, qfalse);
}

qboolean CL_UIMenu_Close(const char *menuId)
{
	OpenMenuEntry *entry = NULL;
	const int idx = FindOpenIndex(menuId);
	if (idx >= 0) {
		entry = &g_openMenus[static_cast<size_t>(idx)];
		entry->persistent = qfalse;
		if (entry->holdRefCount > 0) {
			entry->holdRefCount = 0;
		}
	}
	return CloseMenuInternal(menuId, qfalse);
}

qboolean CL_UIMenu_Hold(const char *menuId, qboolean down)
{
	if (down) {
		return OpenMenuInternal(menuId, qfalse, qfalse, qtrue);
	}

	const int idx = FindOpenIndex(menuId);
	if (idx < 0) {
		return qfalse;
	}

	OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(idx)];
	if (entry.holdRefCount > 0) {
		entry.holdRefCount--;
	}
	if (entry.holdRefCount <= 0 && !entry.persistent) {
		return CloseMenuInternal(menuId, qfalse);
	}
	return qtrue;
}

qboolean CL_UIMenu_OpenHold(const char *menuId)
{
	return CL_UIMenu_Hold(menuId, qtrue);
}

qboolean CL_UIMenu_CloseHold(const char *menuId)
{
	const int idx = FindOpenIndex(menuId);
	if (idx < 0) {
		return qfalse;
	}

	OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(idx)];
	entry.holdRefCount = 0;
	/*
	 * Added in OPM: sticky scoreboard cursor is in-play only. Clear on close so
	 * spectator / intermission pointer (ui_om_spectator / ui_om_intermission) is
	 * unaffected and the next hold-TAB starts cursorless.
	 */
	if (menuId && !Q_stricmp(menuId, "scoreboard")) {
		Cvar_Set("ui_om_scoreboard_cursor", "0");
	}
	if (!entry.persistent) {
		return CloseMenuInternal(menuId, qfalse);
	}
	return qtrue;
}

qboolean CL_UIMenu_IsOpen(const char *menuId)
{
	return FindOpenIndex(menuId) >= 0 ? qtrue : qfalse;
}

qboolean CL_UIMenu_HasAnyOpen(void)
{
	return g_openMenus.empty() ? qfalse : qtrue;
}

qboolean CL_UIMenu_HasInteractiveOpen(void)
{
	for (size_t i = 0; i < g_openMenus.size(); ++i) {
		if (MenuWantsInput(g_openMenus[i])) {
			return qtrue;
		}
	}
	/*
	 * Fixed in OPM: in-HUD chat is keyboard-only (draw-order 4). It must not
	 * count as an interactive overlay — that gated HUD paint off, forced the
	 * connected-overlay compositor, and showed an OS cursor via KEYCATCH_UI.
	 * Key ownership for chat is handled in CL_UIMenu_ShouldOwnInput instead.
	 */
	return qfalse;
}

int CL_UIMenu_GetOpenCount(void)
{
	return static_cast<int>(g_openMenus.size());
}

void CL_UIMenu_UpdateAll(unsigned int time)
{
	std::vector<int> indices;
	indices.reserve(g_openMenus.size());
	for (int i = 0; i < static_cast<int>(g_openMenus.size()); ++i) {
		indices.push_back(i);
	}
	SortOpenIndicesByDrawOrder(&indices, qtrue);

	for (int idx : indices) {
		OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(idx)];
		if (!EnsureRuntimeLoaded(&entry)) {
			continue;
		}
		UID_Update(entry.runtime, time, NULL);
	}
	/* Added in OPM: invalidate retained chrome before the compositor chrome phase. */
	for (size_t i = 0; i < g_openMenus.size(); ++i) {
		const uid_document_t *doc =
			(g_openMenus[i].runtime && UID_HasDocument(g_openMenus[i].runtime))
				? UID_GetDocument(g_openMenus[i].runtime)
				: nullptr;
		if (doc && (doc->dirty & (UID_DIRTY_PAINT | UID_DIRTY_LAYOUT | UID_DIRTY_STRUCTURE))) {
			UIR_ChromeCacheRequestRebuild();
			break;
		}
	}
}

void CL_UIMenu_ApplySurface(int lw, int lh, int fw, int fh)
{
	for (size_t i = 0; i < g_openMenus.size(); ++i) {
		if (g_openMenus[i].runtime) {
			UID_SetSurface(g_openMenus[i].runtime, lw, lh, fw, fh);
		}
	}
}

void CL_UIMenu_ApplyUiPxScale(float scale)
{
	for (size_t i = 0; i < g_openMenus.size(); ++i) {
		if (g_openMenus[i].runtime) {
			UID_SetUiPxScale(g_openMenus[i].runtime, scale);
		}
	}
}

void CL_UIMenu_UpdateAllWithPointer(unsigned int time, const void *pointer)
{
	const uid_pointer_state_t *ptr = static_cast<const uid_pointer_state_t *>(pointer);
	SyncHudPointerStateCvars();
	std::vector<int> indices;
	indices.reserve(g_openMenus.size());
	for (int i = 0; i < static_cast<int>(g_openMenus.size()); ++i) {
		indices.push_back(i);
	}
	SortOpenIndicesByDrawOrder(&indices, qtrue);

	int topInteractive = -1;
	for (int i = static_cast<int>(indices.size()) - 1; i >= 0; --i) {
		const int idx = indices[static_cast<size_t>(i)];
		if (MenuWantsInput(g_openMenus[static_cast<size_t>(idx)])) {
			topInteractive = idx;
			break;
		}
	}

	for (int idx : indices) {
		OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(idx)];
		if (!EnsureRuntimeLoaded(&entry)) {
			continue;
		}
		if ((idx == topInteractive || MenuWantsPointer(entry)) && ptr) {
			UID_Update(entry.runtime, time, ptr);
		} else {
			UID_Update(entry.runtime, time, NULL);
		}
	}
	/* Added in OPM: invalidate retained chrome before the compositor chrome phase. */
	for (size_t i = 0; i < g_openMenus.size(); ++i) {
		const uid_document_t *doc =
			(g_openMenus[i].runtime && UID_HasDocument(g_openMenus[i].runtime))
				? UID_GetDocument(g_openMenus[i].runtime)
				: nullptr;
		if (doc && (doc->dirty & (UID_DIRTY_PAINT | UID_DIRTY_LAYOUT | UID_DIRTY_STRUCTURE))) {
			UIR_ChromeCacheRequestRebuild();
			break;
		}
	}
}

qboolean CL_UIMenu_HasMenusUpTo(int maxDrawOrder)
{
	for (size_t i = 0; i < g_openMenus.size(); ++i) {
		if (g_openMenus[static_cast<size_t>(i)].drawOrder <= maxDrawOrder) {
			return qtrue;
		}
	}
	return qfalse;
}

qboolean CL_UIMenu_HasPointerMenuOpen(void)
{
	SyncHudPointerStateCvars();
	for (size_t i = 0; i < g_openMenus.size(); ++i) {
		OpenMenuEntry &entry = g_openMenus[i];
		if (!EnsureRuntimeLoaded(&entry)) {
			continue;
		}
		if (MenuWantsPointer(entry)) {
			return qtrue;
		}
	}
	return qfalse;
}

static void PaintChromeFiltered(int maxDrawOrder)
{
	std::vector<int> indices;
	indices.reserve(g_openMenus.size());
	for (int i = 0; i < static_cast<int>(g_openMenus.size()); ++i) {
		if (g_openMenus[static_cast<size_t>(i)].drawOrder <= maxDrawOrder) {
			indices.push_back(i);
		}
	}
	SortOpenIndicesByDrawOrder(&indices, qtrue);

	for (int idx : indices) {
		OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(idx)];
		if (entry.runtime && UID_HasDocument(entry.runtime)) {
			UID_DrawChrome(entry.runtime);
		}
	}
}

static void PaintOverlayFiltered(int maxDrawOrder)
{
	std::vector<int> indices;
	indices.reserve(g_openMenus.size());
	for (int i = 0; i < static_cast<int>(g_openMenus.size()); ++i) {
		if (g_openMenus[static_cast<size_t>(i)].drawOrder <= maxDrawOrder) {
			indices.push_back(i);
		}
	}
	SortOpenIndicesByDrawOrder(&indices, qtrue);

	for (int idx : indices) {
		OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(idx)];
		if (entry.runtime && UID_HasDocument(entry.runtime)) {
			UID_DrawOverlay(entry.runtime);
		}
	}
}

void CL_UIMenu_PaintChromeUpTo(int maxDrawOrder)
{
	PaintChromeFiltered(maxDrawOrder);
}

void CL_UIMenu_PaintOverlayUpTo(int maxDrawOrder)
{
	PaintOverlayFiltered(maxDrawOrder);
}

void CL_UIMenu_PaintChrome(void)
{
	PaintChromeFiltered(9);
}

void CL_UIMenu_PaintOverlay(void)
{
	PaintOverlayFiltered(9);
}

qboolean CL_UIMenu_KeyEvent(int key, qboolean down, unsigned time)
{
	std::vector<int> indices;
	indices.reserve(g_openMenus.size());
	for (int i = 0; i < static_cast<int>(g_openMenus.size()); ++i) {
		if (MenuAcceptsKeys(g_openMenus[static_cast<size_t>(i)])) {
			indices.push_back(i);
		}
	}
	SortOpenIndicesByDrawOrder(&indices, qfalse);

	/* Added in OPM: keypad Enter commits text inputs like primary Enter. */
	if (key == K_KP_ENTER) {
		key = K_ENTER;
	}

	for (int idx : indices) {
		OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(idx)];
		if (!entry.runtime || !UID_HasDocument(entry.runtime)) {
			continue;
		}
		if (UID_KeyEvent(entry.runtime, key, down ? true : false, time)) {
			return qtrue;
		}
	}
	return qfalse;
}

qboolean CL_UIMenu_CharEvent(int ch)
{
	std::vector<int> indices;
	indices.reserve(g_openMenus.size());
	for (int i = 0; i < static_cast<int>(g_openMenus.size()); ++i) {
		if (MenuAcceptsKeys(g_openMenus[static_cast<size_t>(i)])) {
			indices.push_back(i);
		}
	}
	SortOpenIndicesByDrawOrder(&indices, qfalse);

	for (int idx : indices) {
		OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(idx)];
		if (!entry.runtime || !UID_HasDocument(entry.runtime)) {
			continue;
		}
		if (UID_CharEvent(entry.runtime, ch)) {
			return qtrue;
		}
	}
	return qfalse;
}

qboolean CL_UIMenu_ShouldOwnInput(void)
{
	if (CL_UIR_UseLegacyMain()) {
		return qfalse;
	}
	/*
	 * Added in OPM: in-HUD chat compose owns keys while open without counting
	 * as an interactive menu (see HasInteractiveOpen).
	 */
	if (!CL_UIMenu_HasInteractiveOpen() && !CL_UIR_HudChatIsOpen()) {
		return qfalse;
	}
	if (CL_UIR_LegacyModalOwnsInput()) {
		return qfalse;
	}
	return qtrue;
}

qboolean CL_UIMenu_IsCapturingKeybind(void)
{
	std::vector<int> indices;
	indices.reserve(g_openMenus.size());
	for (int i = 0; i < static_cast<int>(g_openMenus.size()); ++i) {
		if (MenuWantsInput(g_openMenus[static_cast<size_t>(i)])) {
			indices.push_back(i);
		}
	}
	SortOpenIndicesByDrawOrder(&indices, qfalse);

	for (int idx : indices) {
		OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(idx)];
		if (entry.runtime && UID_IsCapturingKeybind(entry.runtime)) {
			return qtrue;
		}
	}
	return qfalse;
}

qboolean CL_UIMenu_TopmostWantsMenuWorld(uid_menu_backdrop_t *outBackdrop)
{
	int bestOrder = -1;
	uid_menu_backdrop_t bestBackdrop = UID_MENU_BACKDROP_NONE;
	qboolean found = qfalse;

	for (size_t i = 0; i < g_openMenus.size(); ++i) {
		const OpenMenuEntry &entry = g_openMenus[i];
		if (entry.backdrop != UID_MENU_BACKDROP_MENU_MAP) {
			continue;
		}
		if (!found || entry.drawOrder >= bestOrder) {
			bestOrder = entry.drawOrder;
			bestBackdrop = entry.backdrop;
			found = qtrue;
		}
	}

	if (outBackdrop) {
		*outBackdrop = bestBackdrop;
	}
	return found;
}

uid_runtime_t *CL_UIMenu_TopmostMenuWorldRuntime(void)
{
	int bestOrder = -1;
	uid_runtime_t *bestRuntime = NULL;

	for (size_t i = 0; i < g_openMenus.size(); ++i) {
		const OpenMenuEntry &entry = g_openMenus[i];
		if (entry.backdrop != UID_MENU_BACKDROP_MENU_MAP || !entry.runtime) {
			continue;
		}
		if (!bestRuntime || entry.drawOrder >= bestOrder) {
			bestOrder = entry.drawOrder;
			bestRuntime = entry.runtime;
		}
	}
	return bestRuntime;
}

uid_runtime_t *CL_UIMenu_RuntimeForInput(void)
{
	std::vector<int> indices;
	indices.reserve(g_openMenus.size());
	for (int i = 0; i < static_cast<int>(g_openMenus.size()); ++i) {
		if (MenuAcceptsKeys(g_openMenus[static_cast<size_t>(i)])) {
			indices.push_back(i);
		}
	}
	if (indices.empty()) {
		return NULL;
	}
	SortOpenIndicesByDrawOrder(&indices, qfalse);
	OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(indices.front())];
	return entry.runtime;
}

uid_runtime_t *CL_UIMenu_RuntimeById(const char *menuId)
{
	const int idx = FindOpenIndex(menuId);
	if (idx < 0) {
		return NULL;
	}
	OpenMenuEntry &entry = g_openMenus[static_cast<size_t>(idx)];
	if (!EnsureRuntimeLoaded(&entry)) {
		return NULL;
	}
	return entry.runtime;
}

int CL_UIMenu_PointerWheelDelta(void)
{
	return g_pointerWheelDelta;
}

void CL_UIMenu_ClearPointerWheelDelta(void)
{
	g_pointerWheelDelta = 0;
}

void CL_UIMenu_AddPointerWheelDelta(int delta)
{
	g_pointerWheelDelta += delta;
}

void CL_UIMenu_CloseAllInteractive(void)
{
	for (int i = static_cast<int>(g_openMenus.size()) - 1; i >= 0; --i) {
		if (MenuWantsInput(g_openMenus[static_cast<size_t>(i)])) {
			const std::string id = g_openMenus[static_cast<size_t>(i)].menuId;
			CL_UIMenu_Close(id.c_str());
		}
	}
}

void CL_UIMenu_OnSessionDeactivate(void)
{
	/* Added in OPM: drop in-HUD chat compose with the rest of session UI. */
	CL_UIR_CloseHudChat();
	for (size_t i = 0; i < g_openMenus.size(); ++i) {
		if (g_openMenus[i].runtime) {
			UID_Deactivate(g_openMenus[i].runtime);
		}
	}
}

static void CL_UIMenu_Open_f(void)
{
	if (Cmd_Argc() < 3) {
		Com_Printf("usage: ui_open menu <menuid>\n");
		return;
	}
	if (Q_stricmp(Cmd_Argv(1), "menu") != 0) {
		Com_Printf("usage: ui_open menu <menuid>\n");
		return;
	}
	CL_UIMenu_Open(Cmd_Argv(2), qtrue);
}

static void CL_UIMenu_Close_f(void)
{
	if (Cmd_Argc() < 3) {
		Com_Printf("usage: ui_close menu <menuid>\n");
		return;
	}
	if (Q_stricmp(Cmd_Argv(1), "menu") != 0) {
		Com_Printf("usage: ui_close menu <menuid>\n");
		return;
	}
	if (!Q_stricmp(Cmd_Argv(2), "dm_pause")) {
		CL_UIR_CloseDmPause();
		return;
	}
	CL_UIMenu_Close(Cmd_Argv(2));
}

static void CL_UIMenu_OpenDown_f(void)
{
	if (Cmd_Argc() < 3) {
		return;
	}
	if (Q_stricmp(Cmd_Argv(1), "menu") != 0) {
		return;
	}
	CL_UIMenu_Hold(Cmd_Argv(2), qtrue);
}

static void CL_UIMenu_OpenUp_f(void)
{
	if (Cmd_Argc() < 3) {
		return;
	}
	if (Q_stricmp(Cmd_Argv(1), "menu") != 0) {
		return;
	}
	CL_UIMenu_Hold(Cmd_Argv(2), qfalse);
}

static void CL_UIMenu_Reload_f(void)
{
	CL_UIMenu_ReloadRegistry();
	Com_Printf("UIMenu: registry reloaded (%d menus, %d HUD packs)\n", (int)g_registry.size(), CL_UIMenu_HudCount());
}

void CL_UIMenu_RegisterCommands(void)
{
	Cmd_AddCommand("ui_open", CL_UIMenu_Open_f);
	Cmd_AddCommand("ui_close", CL_UIMenu_Close_f);
	Cmd_AddCommand("+ui_open", CL_UIMenu_OpenDown_f);
	Cmd_AddCommand("-ui_open", CL_UIMenu_OpenUp_f);
	Cmd_AddCommand("ui_menu_reload", CL_UIMenu_Reload_f);
	Cmd_AddCommand("ui_menu_list", CL_UIMenu_List_f);
	Cmd_AddCommand("ui_hud_list", CL_UIMenu_ListHuds_f);
}

void CL_UIMenu_UnregisterCommands(void)
{
	Cmd_RemoveCommand("ui_open");
	Cmd_RemoveCommand("ui_close");
	Cmd_RemoveCommand("+ui_open");
	Cmd_RemoveCommand("-ui_open");
	Cmd_RemoveCommand("ui_menu_reload");
	Cmd_RemoveCommand("ui_menu_list");
	Cmd_RemoveCommand("ui_hud_list");
}
