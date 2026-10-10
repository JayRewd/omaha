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

#include "client.h"
#include "cl_practice.h"
#include "cl_uivars.h"
#include "cl_ui.h"
#include "../uidesign/uid_backend.h"
#include "../qcommon/practice_scenario.hpp"

#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cctype>

typedef struct {
	char file[MAX_QPATH];
	char name[MAX_QPATH];
	char map[MAX_QPATH];
	char category[16];
	char categoryLabel[16]; // Added in Omaha: upper-case MODE column text; category stays lowercase for g_practice
	char author[MAX_QPATH];
	char bestTime[32]; // Added in Omaha: formatted BEST column (or "--"); may include difficulty
	int  bestTimeMs;   // Added in Omaha: 0 = none; used for sort
} practice_entry_t;

typedef struct {
	char key[MAX_QPATH];
	char name[MAX_QPATH];
	char map[MAX_QPATH];
	char category[16];
	char bestTime[32];
	char author[64];
	const char *fieldNames[5];
	const char *fieldValues[5];
} practice_slot_t;

typedef struct {
	char key[MAX_QPATH];
	char label[MAX_QPATH];
	const char *fieldNames[1];
	const char *fieldValues[1];
} practice_map_slot_t;

static std::vector<practice_entry_t> g_practiceAll;
static std::vector<int>              g_practiceVisible;
static uint64_t                      g_practiceRevision;
static std::vector<std::string>      g_practiceMaps;
static uint64_t                      g_practiceMapRevision;
static std::string                   g_practiceAppliedSort;

// Added in Omaha: format clear time as M:SS.t [DIFF] for the BEST column.
static void Practice_FormatBestTime(int ms, const char *difficulty, char *out, size_t outSize)
{
	int  totalSec;
	int  tenths;
	int  mins;
	int  secs;
	char timeBuf[16];
	char diffLabel[16];

	if (ms <= 0) {
		Q_strncpyz(out, "--", outSize);
		return;
	}
	totalSec = ms / 1000;
	tenths   = (ms % 1000) / 100;
	mins     = totalSec / 60;
	secs     = totalSec % 60;
	Com_sprintf(timeBuf, sizeof(timeBuf), "%i:%02i.%i", mins, secs, tenths);
	if (difficulty && difficulty[0]) {
		Q_strncpyz(diffLabel, difficulty, sizeof(diffLabel));
		Q_strupr(diffLabel);
		Com_sprintf(out, outSize, "%s %s", timeBuf, diffLabel);
	} else {
		Q_strncpyz(out, timeBuf, outSize);
	}
}

// Added in Omaha: sortable practice columns
static int Practice_Compare(const practice_entry_t &a, const practice_entry_t &b, const char *sort)
{
	int cmp = 0;

	if (!Q_stricmp(sort, "map")) {
		cmp = Q_stricmp(a.map, b.map);
	} else if (!Q_stricmp(sort, "category")) {
		cmp = Q_stricmp(a.category, b.category);
	} else if (!Q_stricmp(sort, "author")) {
		cmp = Q_stricmp(a.author, b.author);
	} else if (!Q_stricmp(sort, "bestTime")) {
		// Added in Omaha: unset (0) always sorts last regardless of direction.
		if (a.bestTimeMs == 0 && b.bestTimeMs == 0) {
			cmp = 0;
		} else if (a.bestTimeMs == 0) {
			cmp = 1;
		} else if (b.bestTimeMs == 0) {
			cmp = -1;
		} else if (a.bestTimeMs < b.bestTimeMs) {
			cmp = -1;
		} else if (a.bestTimeMs > b.bestTimeMs) {
			cmp = 1;
		}
	} else {
		cmp = Q_stricmp(a.name, b.name);
	}
	if (!cmp) {
		cmp = Q_stricmp(a.file, b.file);
	}
	return cmp;
}

static void Practice_SortVisible(void)
{
	const char *sort = CL_UIVar_String("ui_om_practice_sort");
	const int   asc  = CL_UIVar_Integer("ui_om_practice_sort_asc") ? 1 : 0;

	if (!sort) {
		sort = "";
	}
	g_practiceAppliedSort = sort;
	std::stable_sort(g_practiceVisible.begin(), g_practiceVisible.end(), [sort, asc](int ia, int ib) {
		const practice_entry_t &a = g_practiceAll[ia];
		const practice_entry_t &b = g_practiceAll[ib];

		// Added in Omaha: BEST unset rows stay at the bottom for both directions.
		if (!Q_stricmp(sort, "bestTime")) {
			if (a.bestTimeMs == 0 && b.bestTimeMs != 0) {
				return false;
			}
			if (b.bestTimeMs == 0 && a.bestTimeMs != 0) {
				return true;
			}
			if (a.bestTimeMs != 0 && b.bestTimeMs != 0) {
				return asc ? a.bestTimeMs < b.bestTimeMs : a.bestTimeMs > b.bestTimeMs;
			}
			return Q_stricmp(a.file, b.file) < 0;
		}

		const int cmp = Practice_Compare(a, b, sort);
		return asc ? cmp < 0 : cmp > 0;
	});
}

// Added in Omaha: re-sort the current visible list without rescanning practice/
void CL_Practice_Sort(void)
{
	Practice_SortVisible();
	g_practiceRevision++;
}

// Added in Omaha: column the visible list was last sorted by (for header toggle)
const char *CL_Practice_AppliedSort(void)
{
	return g_practiceAppliedSort.c_str();
}

// Added in Omaha: file-slug only — display names keep commas/spaces via SanitizeDisplayName.
static void Practice_SanitizeFileSlug(char *name, size_t size)
{
	size_t i;

	if (!name || size == 0) {
		return;
	}
	name[size - 1] = 0;
	for (i = 0; name[i] && i + 1 < size; i++) {
		if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-') {
			name[i] = '_';
		}
	}
	name[size - 1] = 0;
}

// Added in Omaha: strip controls for JSON display name; do not rewrite punctuation to '_'.
static void Practice_SanitizeDisplayName(const char *in, char *out, size_t outSize)
{
	size_t n = 0;

	for (; in && *in && n + 1 < outSize; in++) {
		unsigned char c = (unsigned char)*in;

		if (c < ' ' || c == 127) {
			continue;
		}
		if (c == '"') {
			c = '\'';
		}
		out[n++] = (char)c;
	}
	out[n] = 0;
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) {
		out[--n] = 0;
	}
}

static qboolean Practice_FileExists(const char *file)
{
	return (FS_FileExists_HomeData(file) || FS_ReadFile(file, NULL) > 0) ? qtrue : qfalse;
}

// Added in Omaha: unique practice/<map>/<slug>.json — append _2, _3, … on collision.
static qboolean Practice_UniqueScenarioPath(const char *map, const char *slug, char *out, size_t outSize)
{
	char trySlug[MAX_QPATH];
	int  n;

	if (!map || !map[0] || !slug || !slug[0] || !out || outSize == 0) {
		return qfalse;
	}

	for (n = 1; n < 1000; n++) {
		if (n == 1) {
			Q_strncpyz(trySlug, slug, sizeof(trySlug));
		} else {
			Com_sprintf(trySlug, sizeof(trySlug), "%s_%i", slug, n);
		}
		PracticeScenario_MakePath(map, trySlug, out, outSize);
		if (!out[0]) {
			return qfalse;
		}
		if (!Practice_FileExists(out)) {
			return qtrue;
		}
	}
	out[0] = 0;
	return qfalse;
}

static void Practice_AddMapsFrom(const char *path)
{
	char **files;
	int    num = 0;
	int    i;
	const char *prefix = "";

	files = FS_ListFiles(path, ".bsp", qfalse, &num);
	if (!files) {
		return;
	}
	// Fixed in Omaha: strip a literal "maps/" prefix instead of hard-coded path+5.
	if (path && !Q_stricmpn(path, "maps/", 5)) {
		prefix = path + 5;
	}
	for (i = 0; i < num; i++) {
		char mapName[MAX_QPATH];
		char full[MAX_QPATH];
		Q_strncpyz(mapName, files[i], sizeof(mapName));
		COM_StripExtension(mapName, mapName, sizeof(mapName));
		if (!COM_IsMapValid(mapName)) {
			continue;
		}
		Com_sprintf(full, sizeof(full), "%s%s", prefix, mapName);
		g_practiceMaps.push_back(full);
	}
	FS_FreeFileList(files);
}

void CL_Practice_Refresh(void)
{
	char **files;
	int    num = 0;
	int    i;
	const char *search;
	const char *catFilter;
	const char *mapFilter;

	g_practiceAll.clear();
	g_practiceVisible.clear();

	// wantSubs: scenarios live in practice/<map>/<name>.json
	files = FS_ListFiles("practice", ".json", qtrue, &num);
	if (files) {
		for (i = 0; i < num; i++) {
			char              path[MAX_QPATH];
			char             *buf = NULL;
			long              len;
			PracticeScenario  sc;
			practice_entry_t  e;

			Com_sprintf(path, sizeof(path), "practice/%s", files[i]);
			len = FS_ReadFile(path, (void **)&buf);
			if (len <= 0 || !buf) {
				continue;
			}
			if (!PracticeScenario_Parse(buf, &sc, NULL, 0)) {
				FS_FreeFile(buf);
				continue;
			}
			FS_FreeFile(buf);
			memset(&e, 0, sizeof(e));
			Q_strncpyz(e.file, path, sizeof(e.file));
			if (sc.name[0]) {
				Q_strncpyz(e.name, sc.name, sizeof(e.name));
			} else {
				// Changed in Omaha: fall back to the bare file stem, not dm/<map>/<name>.json
				COM_StripExtension(COM_SkipPath(files[i]), e.name, sizeof(e.name));
			}
			Q_strncpyz(e.map, sc.map, sizeof(e.map));
			Q_strncpyz(e.category, Practice_CategoryName(sc.category), sizeof(e.category));
			Q_strncpyz(e.categoryLabel, e.category, sizeof(e.categoryLabel));
			Q_strupr(e.categoryLabel);
			Q_strncpyz(e.author, sc.author, sizeof(e.author));
			e.bestTimeMs = sc.bestTimeMs;
			Practice_FormatBestTime(e.bestTimeMs, sc.bestDifficulty, e.bestTime, sizeof(e.bestTime));
			g_practiceAll.push_back(e);
		}
		FS_FreeFileList(files);
	}

	search    = CL_UIVar_String("ui_om_practice_search");
	catFilter = CL_UIVar_String("ui_om_practice_filter_cat");
	mapFilter = CL_UIVar_String("ui_om_practice_filter_map");

	for (i = 0; i < (int)g_practiceAll.size(); i++) {
		const practice_entry_t &e = g_practiceAll[i];
		if (catFilter && catFilter[0] && Q_stricmp(catFilter, e.category)) {
			continue;
		}
		if (mapFilter && mapFilter[0] && !Q_stristr(e.map, mapFilter)) {
			continue;
		}
		if (search && search[0] && !Q_stristr(e.name, search) && !Q_stristr(e.map, search) && !Q_stristr(e.file, search)) {
			continue;
		}
		g_practiceVisible.push_back(i);
	}
	Practice_SortVisible(); // Added in Omaha

	g_practiceRevision++;
	CL_UIVar_Set("ui_om_practice_status", va("%i SCENARIOS", (int)g_practiceVisible.size()));
	CL_UIVar_Set("ui_om_practice_selected", g_practiceVisible.empty() ? "0" : "1");
}

static const practice_entry_t *Practice_Selected(void)
{
	const char *key = CL_UIVar_String("ui_om_practice_file");
	size_t      i;

	if (!key || !key[0]) {
		return NULL;
	}
	for (i = 0; i < g_practiceAll.size(); i++) {
		if (!Q_stricmp(g_practiceAll[i].file, key)) {
			return &g_practiceAll[i];
		}
	}
	return NULL;
}

void CL_Practice_Play(int edit)
{
	const practice_entry_t *e = Practice_Selected();
	const char             *diff;
	char                    map[MAX_QPATH];
	int                     bots;

	if (!e) {
		Com_Printf("Practice: no scenario selected\n");
		return;
	}

	diff = CL_UIVar_String("ui_om_practice_difficulty");
	if (!diff[0]) {
		diff = "medium";
	}
	Q_strncpyz(map, e->map, sizeof(map));
	if (!map[0]) {
		Com_Printf("Practice: scenario has no map\n");
		return;
	}
	// Added in Omaha: refuse cbuf launch if scenario fields are not allowlisted.
	if (!COM_IsMapValid(map) || !Practice_IsSafeToken(map)) {
		Com_Printf("Practice: invalid map in scenario\n");
		return;
	}
	if (!Practice_IsSafeCategory(e->category)) {
		Com_Printf("Practice: invalid category in scenario\n");
		return;
	}
	if (!Practice_IsPracticeScenarioPath(e->file)) {
		Com_Printf("Practice: invalid scenario file path\n");
		return;
	}
	if (!Practice_IsSafeDifficulty(diff)) {
		Com_Printf("Practice: invalid difficulty\n");
		return;
	}

	bots = 32;
	Cbuf_AddText(va(
		"set dedicated 0;"
		"set sv_gamespy 0;"
		"set sv_maxclients 1;"
		"set sv_maxbots %i;"
		"set sv_numbots 0;"
		"set g_gametype 1;"
		"set g_gametypestring \"Free-For-All\";"
		"set fraglimit 0;"
		"set timelimit 0;"
		"set g_practice \"%s\";"
		"set g_practice_file \"%s\";"
		"set g_practice_edit %i;"
		"set g_practice_difficulty \"%s\";"
		"wait;"
		"map \"%s\"\n",
		bots,
		e->category,
		e->file,
		edit,
		diff,
		map
	));
}

qboolean CL_Practice_New(void)
{
	char             displayName[MAX_QPATH];
	char             fileSlug[MAX_QPATH];
	char             file[MAX_QPATH];
	const char      *cat;
	const char      *map;
	PracticeScenario sc;
	std::string      json;

	CL_UIVar_Set("ui_om_practice_new_status", "");

	// Changed in Omaha: keep typed display name in JSON; only the file path is slugified.
	Practice_SanitizeDisplayName(CL_UIVar_String("ui_om_practice_new_name"), displayName, sizeof(displayName));
	if (!displayName[0]) {
		Q_strncpyz(displayName, "untitled", sizeof(displayName));
	}
	Q_strncpyz(fileSlug, displayName, sizeof(fileSlug));
	Practice_SanitizeFileSlug(fileSlug, sizeof(fileSlug));
	if (!fileSlug[0]) {
		Q_strncpyz(fileSlug, "untitled", sizeof(fileSlug));
	}

	cat = CL_UIVar_String("ui_om_practice_new_category");
	map = CL_UIVar_String("ui_om_host_map");
	if (!map[0]) {
		map = CL_UIVar_String("ui_om_practice_filter_map");
	}
	if (!map[0]) {
		Com_Printf("Practice: select a map in the host panel first\n");
		CL_UIVar_Set("ui_om_practice_new_status", "SELECT A MAP FIRST");
		return qfalse;
	}

	if (!Practice_UniqueScenarioPath(map, fileSlug, file, sizeof(file))) {
		Com_Printf("Practice: could not allocate a unique scenario path\n");
		CL_UIVar_Set("ui_om_practice_new_status", "INVALID NAME OR MAP");
		return qfalse;
	}

	sc.Clear();
	sc.version  = 2;
	sc.category = Practice_CategoryFromName(cat[0] ? cat : "angles");
	Q_strncpyz(sc.name, displayName, sizeof(sc.name));
	Q_strncpyz(sc.map, map, sizeof(sc.map));
	if (!PracticeScenario_Dump(sc, &json)) {
		CL_UIVar_Set("ui_om_practice_new_status", "FAILED TO CREATE");
		return qfalse;
	}
	FS_WriteFile(file, json.c_str(), (int)json.size());
	CL_UIVar_Set("ui_om_practice_file", file);
	CL_UIVar_Set("ui_om_practice_new_status", "");
	CL_Practice_Refresh();
	CL_Practice_Play(1);
	return qtrue;
}

void CL_Practice_StartHost(void)
{
	const char *map      = CL_UIVar_String("ui_om_host_map");
	const char *gt       = CL_UIVar_String("ui_om_host_gametype");
	const char *hostname = CL_UIVar_String("ui_om_host_hostname");
	int         online   = CL_UIVar_Integer("ui_om_host_online");
	int         maxcl    = CL_UIVar_Integer("ui_om_host_maxclients");
	int         frag     = CL_UIVar_Integer("ui_om_host_fraglimit");
	int         time     = CL_UIVar_Integer("ui_om_host_timelimit");
	int         gametype = GT_FFA;
	const char *gtString = "Free-For-All";

	if (!map || !map[0]) {
		Com_Printf("Host: no map selected\n");
		return;
	}
	if (!COM_IsMapValid(map) || !Practice_IsSafeToken(map)) {
		Com_Printf("Host: invalid map\n");
		return;
	}
	if (!Q_stricmp(gt, "tdm") || !Q_stricmp(gt, "2")) {
		gametype = GT_TEAM;
		gtString = "Team Deathmatch";
	} else if (!Q_stricmp(gt, "obj") || !Q_stricmp(gt, "4")) {
		gametype = GT_OBJECTIVE;
		gtString = "Objective";
	} else if (!Q_stricmp(gt, "round") || !Q_stricmp(gt, "3")) {
		gametype = GT_TEAM_ROUNDS;
		gtString = "Round-based-TDM";
	}
	if (maxcl < 1) {
		maxcl = 8;
	}
	if (!hostname[0]) {
		hostname = "Omaha Server";
	}
	{
		char hostSafe[MAX_QPATH];
		size_t n = 0;

		for (; hostname[n] && n + 1 < sizeof(hostSafe); n++) {
			const unsigned char c = (unsigned char)hostname[n];

			if (c < 0x20 || c == 0x7f || c == '"' || c == ';' || c == '\\' || c == '$' || c == '%') {
				Com_Printf("Host: invalid hostname\n");
				return;
			}
			hostSafe[n] = (char)c;
		}
		hostSafe[n] = 0;
		if (!hostSafe[0]) {
			Com_Printf("Host: invalid hostname\n");
			return;
		}
		hostname = hostSafe;

		Cbuf_AddText(va(
			"set dedicated 0;"
			"set sv_maxclients %i;"
			"set sv_gamespy %i;"
			"set g_gametype %i;"
			"set g_gametypestring \"%s\";"
			"set fraglimit %i;"
			"set timelimit %i;"
			"set g_practice 0;"
			"set g_practice_file \"\";"
			"set g_practice_edit 0;"
			"set sv_hostname \"%s\";"
			"set cheats 0;"
			"wait;"
			"map \"%s\"\n",
			maxcl,
			online ? 1 : 0,
			gametype,
			gtString,
			frag,
			time,
			hostname,
			map
		));
	}
}

int CL_Practice_QueryScenarios(
	int offset, int limit, int *outTotal, uint64_t *outRevision, uid_collection_item_t *out, int max
)
{
	static std::vector<practice_slot_t> slots;
	static const char *const            kNames[] = {"name", "map", "category", "bestTime", "author"};
	int                                 i;
	int                                 written = 0;

	if (g_practiceAll.empty() && g_practiceRevision == 0) {
		CL_Practice_Refresh();
	}

	if (outTotal) {
		*outTotal = (int)g_practiceVisible.size();
	}
	if (outRevision) {
		*outRevision = g_practiceRevision;
	}
	if (!out || max <= 0) {
		return 0;
	}
	if (offset < 0) {
		offset = 0;
	}
	if (limit <= 0) {
		limit = max;
	}
	if ((int)slots.size() < max) {
		slots.resize(max);
	}

	// Fixed in Omaha: fill persistent slots in place; field pointers into a stack copy dangled
	for (i = offset; i < (int)g_practiceVisible.size() && written < limit && written < max; i++) {
		const practice_entry_t &e    = g_practiceAll[g_practiceVisible[i]];
		practice_slot_t        *slot = &slots[written];

		Q_strncpyz(slot->key, e.file, sizeof(slot->key));
		Q_strncpyz(slot->name, e.name, sizeof(slot->name));
		Q_strncpyz(slot->map, e.map, sizeof(slot->map));
		Q_strncpyz(slot->category, e.categoryLabel, sizeof(slot->category));
		Q_strncpyz(slot->bestTime, e.bestTime, sizeof(slot->bestTime));
		Q_strncpyz(slot->author, e.author, sizeof(slot->author));
		slot->fieldNames[0]  = kNames[0];
		slot->fieldNames[1]  = kNames[1];
		slot->fieldNames[2]  = kNames[2];
		slot->fieldNames[3]  = kNames[3];
		slot->fieldNames[4]  = kNames[4];
		slot->fieldValues[0] = slot->name;
		slot->fieldValues[1] = slot->map;
		slot->fieldValues[2] = slot->category;
		slot->fieldValues[3] = slot->bestTime;
		slot->fieldValues[4] = slot->author;

		out[written].key         = slot->key;
		out[written].value       = slot->key;
		out[written].label       = slot->name;
		out[written].nfields     = 5;
		out[written].fieldNames  = slot->fieldNames;
		out[written].fieldValues = slot->fieldValues;
		out[written].flags       = 0;
		written++;
	}
	return written;
}

int CL_Practice_QueryMaps(
	int offset, int limit, int *outTotal, uint64_t *outRevision, uid_collection_item_t *out, int max
)
{
	static std::vector<practice_map_slot_t> slots;
	int                                     i;
	int                                     written = 0;

	if (g_practiceMaps.empty()) {
		Practice_AddMapsFrom("maps/dm/");
		Practice_AddMapsFrom("maps/obj/");
		Practice_AddMapsFrom("maps/lib/");
		g_practiceMapRevision++;
	}

	if (outTotal) {
		*outTotal = (int)g_practiceMaps.size();
	}
	if (outRevision) {
		*outRevision = g_practiceMapRevision;
	}
	if (!out || max <= 0) {
		return 0;
	}
	if (offset < 0) {
		offset = 0;
	}
	if (limit <= 0) {
		limit = max;
	}
	if ((int)slots.size() < max) {
		slots.resize(max);
	}

	// Fixed in Omaha: fill persistent slots in place; field pointers into a stack copy dangled
	for (i = offset; i < (int)g_practiceMaps.size() && written < limit && written < max; i++) {
		practice_map_slot_t *slot = &slots[written];

		Q_strncpyz(slot->key, g_practiceMaps[i].c_str(), sizeof(slot->key));
		Q_strncpyz(slot->label, g_practiceMaps[i].c_str(), sizeof(slot->label));
		slot->fieldNames[0]  = "name";
		slot->fieldValues[0] = slot->label;

		out[written].key         = slot->key;
		out[written].value       = slot->key;
		out[written].label       = slot->label;
		out[written].nfields     = 1;
		out[written].fieldNames  = slot->fieldNames;
		out[written].fieldValues = slot->fieldValues;
		out[written].flags       = 0;
		written++;
	}
	return written;
}
