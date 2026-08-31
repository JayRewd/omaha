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
#include "cl_hud_registry.h"
#include "../uidesign/uid_xml.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct HudRegistryEntry {
	std::string hudId;
	std::string hudLabel;
	std::string vfsPath;
	int         drawOrder;
	qboolean    builtinLegacy;
};

static std::vector<HudRegistryEntry> g_hudEntries;
static std::unordered_map<std::string, size_t> g_hudIndex;
static uint64_t g_hudRegistryRevision = 1;

static long uid_read_file(const char *path, void **buf)
{
	return FS_ReadFile(path, buf);
}

static void uid_free_file(void *buf)
{
	FS_FreeFile(buf);
}

static uid_parse_io_t g_parseIo = {uid_read_file, uid_free_file};

static void AddHudEntry(
	const char *hudId,
	const char *hudLabel,
	const char *vfsPath,
	int drawOrder,
	qboolean builtinLegacy
)
{
	HudRegistryEntry entry;
	entry.hudId = hudId ? hudId : "";
	entry.hudLabel = hudLabel ? hudLabel : hudId;
	entry.vfsPath = vfsPath ? vfsPath : "";
	entry.drawOrder = drawOrder;
	entry.builtinLegacy = builtinLegacy;

	auto existing = g_hudIndex.find(entry.hudId);
	if (existing != g_hudIndex.end()) {
		Com_DPrintf(
			"UIMenu: overriding HUD pack '%s' (%s -> %s)\n",
			entry.hudId.c_str(),
			g_hudEntries[existing->second].vfsPath.c_str(),
			entry.vfsPath.c_str()
		);
		g_hudEntries[existing->second] = entry;
	} else {
		g_hudIndex[entry.hudId] = g_hudEntries.size();
		g_hudEntries.push_back(entry);
	}
}

} // namespace

void CL_UIMenu_ReloadHudRegistry(void)
{
	g_hudEntries.clear();
	g_hudIndex.clear();
	g_hudRegistryRevision++;

	AddHudEntry(CL_HUD_LEGACY_ID, "Legacy", "", 4, qtrue);

	int numFiles = 0;
	char **files = FS_ListFiles("ui/modern/huds", "xml", qfalse, &numFiles);
	if (files) {
		for (int i = 0; i < numFiles; ++i) {
			char vfsPath[MAX_QPATH];
			uid_hud_meta_t meta;

			Com_sprintf(vfsPath, sizeof(vfsPath), "ui/modern/huds/%s", files[i]);
			if (UID_PeekHudMetadata(vfsPath, &g_parseIo, &meta, NULL) != UID_OK || !meta.valid) {
				continue;
			}
			if (!Q_stricmp(meta.hudId, CL_HUD_LEGACY_ID)) {
				Com_Printf("UIMenu: HUD pack '%s' cannot use reserved id '%s'\n", vfsPath, CL_HUD_LEGACY_ID);
				continue;
			}
			AddHudEntry(meta.hudId, meta.hudLabel, vfsPath, meta.drawOrder, qfalse);
		}
		FS_FreeFileList(files);
	}
}

void CL_UIMenu_ListHuds_f(void)
{
	Com_Printf(
		"HUD pack registry (%d entries, revision %llu):\n",
		CL_UIMenu_HudCount(),
		(unsigned long long)g_hudRegistryRevision
	);
	for (size_t i = 0; i < g_hudEntries.size(); ++i) {
		const HudRegistryEntry &entry = g_hudEntries[i];
		Com_Printf(
			"  id='%s' label='%s' draw=%d path='%s'%s\n",
			entry.hudId.c_str(),
			entry.hudLabel.c_str(),
			entry.drawOrder,
			entry.vfsPath.c_str(),
			entry.builtinLegacy ? " [builtin]" : ""
		);
	}
}

int CL_UIMenu_HudCount(void)
{
	return static_cast<int>(g_hudEntries.size());
}

qboolean CL_UIMenu_HudExists(const char *hudId)
{
	if (!hudId || !hudId[0]) {
		return qfalse;
	}
	return g_hudIndex.find(hudId) != g_hudIndex.end() ? qtrue : qfalse;
}

const char *CL_UIMenu_HudPath(const char *hudId)
{
	if (!hudId || !hudId[0]) {
		return NULL;
	}
	auto it = g_hudIndex.find(hudId);
	if (it == g_hudIndex.end()) {
		return NULL;
	}
	const HudRegistryEntry &entry = g_hudEntries[it->second];
	return entry.vfsPath.empty() ? NULL : entry.vfsPath.c_str();
}

const char *CL_UIMenu_HudLabel(const char *hudId)
{
	if (!hudId || !hudId[0]) {
		return NULL;
	}
	auto it = g_hudIndex.find(hudId);
	if (it == g_hudIndex.end()) {
		return NULL;
	}
	return g_hudEntries[it->second].hudLabel.c_str();
}

int CL_UIMenu_HudDrawOrder(const char *hudId)
{
	if (!hudId || !hudId[0]) {
		return 4;
	}
	auto it = g_hudIndex.find(hudId);
	if (it == g_hudIndex.end()) {
		return 4;
	}
	return g_hudEntries[it->second].drawOrder;
}

qboolean CL_UIMenu_HudIsBuiltinLegacy(const char *hudId)
{
	if (!hudId || !hudId[0]) {
		return qfalse;
	}
	auto it = g_hudIndex.find(hudId);
	if (it == g_hudIndex.end()) {
		return qfalse;
	}
	return g_hudEntries[it->second].builtinLegacy;
}

void CL_UIMenu_HudEntryAt(int index, const char **outId, const char **outLabel, const char **outPath)
{
	if (index < 0 || index >= static_cast<int>(g_hudEntries.size())) {
		if (outId) {
			*outId = NULL;
		}
		if (outLabel) {
			*outLabel = NULL;
		}
		if (outPath) {
			*outPath = NULL;
		}
		return;
	}
	const HudRegistryEntry &entry = g_hudEntries[static_cast<size_t>(index)];
	if (outId) {
		*outId = entry.hudId.c_str();
	}
	if (outLabel) {
		*outLabel = entry.hudLabel.c_str();
	}
	if (outPath) {
		*outPath = entry.vfsPath.empty() ? NULL : entry.vfsPath.c_str();
	}
}

uint64_t CL_UIMenu_HudRegistryRevision(void)
{
	return g_hudRegistryRevision;
}

void CL_UIMenu_ValidateHudCvar(void)
{
	cvar_t *ui_om_hud = Cvar_Get("ui_om_hud", "classic", CVAR_ARCHIVE);
	if (!ui_om_hud || !ui_om_hud->string[0] || !CL_UIMenu_HudExists(ui_om_hud->string)) {
		Cvar_Set("ui_om_hud", "classic");
	}
}
