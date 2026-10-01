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

#include "cl_uivars.h"

#include "client.h"

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct cl_uivar_s {
	std::string name;
	std::string resetString;
	std::string string;
	float       value;
	int         integer;
	unsigned    modificationCount;
};

typedef struct {
	const char *name;
	const char *reset;
} cl_uivar_def_t;

static const cl_uivar_def_t s_uivarDefs[] = {
	{ "ui_browser_favorite_target", "" },
	{ "ui_browser_want_refresh", "0" },
	{ "ui_legacy_pushmenu_target", "" },
	{ "ui_menu_close_target", "" },
	{ "ui_menu_open_target", "" },
	{ "ui_modal_bind_command", "" },
	{ "ui_modal_bind_existing", "" },
	{ "ui_modal_bind_key", "" },
	{ "ui_modal_bind_slot", "" },
	{ "ui_modal_confirm_invoke", "" },
	{ "ui_modal_message", "" },
	{ "ui_om_browser_sort", "players" },
	{ "ui_om_browser_sort_asc", "0" },
	{ "ui_om_cbuf", "" },
	{ "ui_om_connected", "0" },
	{ "ui_om_hud_active_weapon", "" },
	{ "ui_om_hud_allied_score", "" },
	{ "ui_om_hud_ammo", "0" },
	{ "ui_om_hud_attacker_friendly", "0" },
	{ "ui_om_hud_attacker_name", "" },
	{ "ui_om_hud_attacker_team", "0" },
	{ "ui_om_hud_axis_score", "" },
	{ "ui_om_hud_boss_frac", "0" },
	{ "ui_om_hud_boss_health", "0" },
	{ "ui_om_hud_boss_right", "0" },
	{ "ui_om_hud_centerprint", "" },
	{ "ui_om_hud_chat_label", "Chat: " },
	{ "ui_om_hud_chat_mode", "100" },
	{ "ui_om_hud_chat_open", "0" },
	{ "ui_om_hud_chat_text", "" },
	{ "ui_om_hud_clip", "0" },
	{ "ui_om_hud_compass_angle", "0" },
	{ "ui_om_hud_compass_heading", "0" },
	{ "ui_om_hud_compass_north", "0" },
	{ "ui_om_hud_damage_alpha", "0" },
	{ "ui_om_hud_damage_angle", "0" },
	{ "ui_om_hud_damage_dir", "0" },
	{ "ui_om_hud_following_name", "" },
	{ "ui_om_hud_following_team", "" },
	{ "ui_om_hud_following_text", "" },
	{ "ui_om_hud_frag_limit_text", "" },
	{ "ui_om_hud_fuse_frac", "0" },
	{ "ui_om_hud_fuse_right", "0" },
	{ "ui_om_hud_grenade_count", "0" },
	{ "ui_om_hud_health", "0" },
	{ "ui_om_hud_health_frac", "1" },
	{ "ui_om_hud_health_top", "1" },
	{ "ui_om_hud_im_image", "" },
	{ "ui_om_hud_im_menu", "0" },
	{ "ui_om_hud_in_zoom", "0" },
	{ "ui_om_hud_info_friendly", "0" },
	{ "ui_om_hud_info_health", "0" },
	{ "ui_om_hud_info_name", "" },
	{ "ui_om_hud_info_team", "0" },
	{ "ui_om_hud_item0_image", "" },
	{ "ui_om_hud_item0_name", "" },
	{ "ui_om_hud_item0_state", "3" },
	{ "ui_om_hud_item1_image", "" },
	{ "ui_om_hud_item1_name", "" },
	{ "ui_om_hud_item1_state", "3" },
	{ "ui_om_hud_item2_image", "" },
	{ "ui_om_hud_item2_name", "" },
	{ "ui_om_hud_item2_state", "3" },
	{ "ui_om_hud_item3_image", "" },
	{ "ui_om_hud_item3_name", "" },
	{ "ui_om_hud_item3_state", "3" },
	{ "ui_om_hud_items_equipped", "0" },
	{ "ui_om_hud_items_owned", "0" },
	{ "ui_om_hud_items_visible", "0" },
	{ "ui_om_hud_last_gun", "" },
	{ "ui_om_hud_last_gun_ammo", "0" },
	{ "ui_om_hud_last_gun_clip", "0" },
	{ "ui_om_hud_level_exit_icon", "0" },
	{ "ui_om_hud_max_ammo", "1" },
	{ "ui_om_hud_max_clip", "1" },
	{ "ui_om_hud_max_health", "100" },
	{ "ui_om_hud_obj_arrow_angle", "0" },
	{ "ui_om_hud_obj_arrow_visible", "0" },
	{ "ui_om_hud_obj_left_angle", "0" },
	{ "ui_om_hud_obj_left_visible", "0" },
	{ "ui_om_hud_obj_right_angle", "0" },
	{ "ui_om_hud_obj_right_visible", "0" },
	{ "ui_om_hud_objective_center", "0" },
	{ "ui_om_hud_objective_left", "0" },
	{ "ui_om_hud_objective_right", "0" },
	{ "ui_om_hud_objectives_alpha", "0" },
	{ "ui_om_hud_objectives_count", "0" },
	{ "ui_om_hud_objectives_visible", "0" },
	{ "ui_om_hud_pause_icon", "0" },
	{ "ui_om_hud_primary_name", "" },
	{ "ui_om_hud_score_leader", "" },
	{ "ui_om_hud_score_self", "" },
	{ "ui_om_hud_score_text", "" },
	{ "ui_om_hud_show", "1" },
	{ "ui_om_hud_sidearm_name", "" },
	{ "ui_om_hud_sniper_zoom", "0" },
	{ "ui_om_hud_spectator_text", "" },
	{ "ui_om_hud_stopwatch_angle", "0" },
	{ "ui_om_hud_stopwatch_frac", "0" },
	{ "ui_om_hud_stopwatch_ms", "" },
	{ "ui_om_hud_stopwatch_text", "" },
	{ "ui_om_hud_stopwatch_type", "" },
	{ "ui_om_hud_team", "0" },
	{ "ui_om_hud_time_message", "" },
	{ "ui_om_hud_time_seconds", "" },
	{ "ui_om_hud_vote_keys", "" },
	{ "ui_om_hud_vote_prompt", "" },
	{ "ui_om_hud_vote_seconds", "0" },
	{ "ui_om_hud_vote_stats", "" },
	{ "ui_om_hud_vote_text", "" },
	{ "ui_om_hud_weap_grenade_state", "3" },
	{ "ui_om_hud_weap_heavy_state", "3" },
	{ "ui_om_hud_weap_mg_state", "3" },
	{ "ui_om_hud_weap_pistol_state", "3" },
	{ "ui_om_hud_weap_rifle_state", "3" },
	{ "ui_om_hud_weap_smg_state", "3" },
	{ "ui_om_hud_weapons_equipped", "0" },
	{ "ui_om_hud_weapons_owned", "0" },
	{ "ui_om_hud_weapons_visible", "0" },
	{ "ui_om_intermission", "0" },
	{ "ui_om_main_panel", "" },
	{ "ui_om_modal", "" },
	{ "ui_om_nav_target", "" },
	{ "ui_om_nav_value", "" },
	{ "ui_om_pause_panel", "root" },
	{ "ui_om_players_total", "0" },
	{ "ui_om_scoreboard_cursor", "0" },
	{ "ui_om_scoreboard_deaths_label", "Deaths" },
	{ "ui_om_scoreboard_gamemode", "" },
	{ "ui_om_scoreboard_gametype", "0" },
	{ "ui_om_scoreboard_lib_toggle1", "0" },
	{ "ui_om_scoreboard_lib_toggle2", "0" },
	{ "ui_om_scoreboard_server_name", "" },
	{ "ui_om_scoreboard_sort", "kills" },
	{ "ui_om_scoreboard_sort_asc", "0" },
	{ "ui_om_scoreboard_spectator_count", "0" },
	{ "ui_om_scoreboard_team_mode", "0" },
	{ "ui_om_scoreboard_tow_allied_obj1", "0" },
	{ "ui_om_scoreboard_tow_allied_obj2", "0" },
	{ "ui_om_scoreboard_tow_allied_obj3", "0" },
	{ "ui_om_scoreboard_tow_allied_obj4", "0" },
	{ "ui_om_scoreboard_tow_allied_obj5", "0" },
	{ "ui_om_scoreboard_tow_axis_obj1", "0" },
	{ "ui_om_scoreboard_tow_axis_obj2", "0" },
	{ "ui_om_scoreboard_tow_axis_obj3", "0" },
	{ "ui_om_scoreboard_tow_axis_obj4", "0" },
	{ "ui_om_scoreboard_tow_axis_obj5", "0" },
	{ "ui_om_server_gametype", "" },
	{ "ui_om_server_search", "" },
	{ "ui_om_servers_total", "0" },
	{ "ui_om_servers_visible", "0" },
	{ "ui_om_settings_apply_pending", "0" },
	{ "ui_om_settings_search", "" },
	{ "ui_om_settings_tab", "input" },
	{ "ui_om_spectator", "0" },
	{ "ui_om_status_phase", "READY" },
	{ "ui_om_status_players", "" },
	{ "ui_om_status_servers", "" },
	{ "ui_om_vote_active", "0" },
	{ "ui_om_vote_allow", "1" },
	{ "ui_om_vote_count", "0" },
	{ "ui_om_vote_list_kind", "main" },
	{ "ui_om_vote_selected", "" },
	{ "ui_om_voted", "0" },
	{ "ui_reset_cvar", "" },
	{ "ui_selected_server", "" },

};

/* unique_ptr keeps entry addresses stable across map rehash. */
static std::unordered_map<std::string, std::unique_ptr<cl_uivar_t>> s_uivars;
static std::unordered_set<const void *>                             s_uivarPtrs;
static unsigned                                                     s_uivarEpoch = 0;
static qboolean                                                     s_uivarInited = qfalse;

static void CL_UIVar_ParseNumber(cl_uivar_t *e)
{
	e->value = (float)atof(e->string.c_str());
	e->integer = (int)e->value;
}

static qboolean CL_UIVar_IsPrefixFamily(const char *name)
{
	if (!name || !name[0]) {
		return qfalse;
	}
	/* ui_om_vote_<n>_* published by bg_voteoptions via cgi.Cvar_Set. */
	if (!Q_stricmpn(name, "ui_om_vote_", 11)) {
		const char *p = name + 11;
		if (*p >= '0' && *p <= '9') {
			return qtrue;
		}
	}
	if (!Q_stricmpn(name, "ui_om_scoreboard_tow_", 21)) {
		return qtrue;
	}
	if (!Q_stricmpn(name, "ui_om_hud_item", 14)) {
		const char *p = name + 14;
		if (*p >= '0' && *p <= '9') {
			return qtrue;
		}
	}
	return qfalse;
}

qboolean CL_UIVar_MatchesStoreFamily(const char *name)
{
	return CL_UIVar_IsStoreName(name);
}

qboolean CL_UIVar_IsStoreName(const char *name)
{
	size_t i;

	if (!name || !name[0]) {
		return qfalse;
	}
	/* Archived / player-facing Omaha cvars stay real cvars. */
	if (!Q_stricmp(name, "ui_om_hud") || !Q_stricmp(name, "ui_om_favorite_servers")
		|| !Q_stricmp(name, "ui_om_scoreboard_disable_cursor")
		|| !Q_stricmp(name, "ui_om_menu_map_view") || !Q_stricmp(name, "ui_om_browser_mock")) {
		return qfalse;
	}
	for (i = 0; i < ARRAY_LEN(s_uivarDefs); i++) {
		if (!Q_stricmp(name, s_uivarDefs[i].name)) {
			return qtrue;
		}
	}
	return CL_UIVar_IsPrefixFamily(name);
}

static cl_uivar_t *CL_UIVar_Ensure(const char *name, const char *reset)
{
	auto it = s_uivars.find(name);
	if (it != s_uivars.end()) {
		return it->second.get();
	}
	auto e = std::make_unique<cl_uivar_t>();
	e->name = name;
	e->resetString = reset ? reset : "";
	e->string = e->resetString;
	CL_UIVar_ParseNumber(e.get());
	e->modificationCount = 1;
	cl_uivar_t *ptr = e.get();
	s_uivarPtrs.insert(ptr);
	s_uivars.emplace(name, std::move(e));
	s_uivarEpoch++;
	return ptr;
}

void CL_UIVar_Init(void)
{
	size_t i;

	if (s_uivarInited) {
		return;
	}
	s_uivars.clear();
	s_uivarPtrs.clear();
	s_uivarEpoch = 0;
	for (i = 0; i < ARRAY_LEN(s_uivarDefs); i++) {
		CL_UIVar_Ensure(s_uivarDefs[i].name, s_uivarDefs[i].reset);
	}
	s_uivarInited = qtrue;
}

cl_uivar_t *CL_UIVar_Find(const char *name)
{
	if (!name || !name[0]) {
		return NULL;
	}
	if (!s_uivarInited) {
		CL_UIVar_Init();
	}
	auto it = s_uivars.find(name);
	if (it == s_uivars.end()) {
		return NULL;
	}
	return it->second.get();
}

qboolean CL_UIVar_IsEntry(const void *ptr)
{
	if (!ptr) {
		return qfalse;
	}
	return s_uivarPtrs.find(ptr) != s_uivarPtrs.end() ? qtrue : qfalse;
}

void CL_UIVar_Set(const char *name, const char *value)
{
	cl_uivar_t *e;

	if (!name || !name[0] || !value) {
		return;
	}
	if (!s_uivarInited) {
		CL_UIVar_Init();
	}
	e = CL_UIVar_Find(name);
	if (!e) {
		if (!CL_UIVar_IsStoreName(name)) {
			return;
		}
		e = CL_UIVar_Ensure(name, "");
	}
	if (e->string == value) {
		return;
	}
	e->string = value;
	CL_UIVar_ParseNumber(e);
	e->modificationCount++;
	s_uivarEpoch++;
}

void CL_UIVar_SetValue(const char *name, float value)
{
	char buf[64];

	if (!name || !name[0]) {
		return;
	}
	if (value == (float)(int)value) {
		Com_sprintf(buf, sizeof(buf), "%d", (int)value);
	} else {
		Com_sprintf(buf, sizeof(buf), "%f", value);
	}
	CL_UIVar_Set(name, buf);
}

const char *CL_UIVar_String(const char *name)
{
	cl_uivar_t *e = CL_UIVar_Find(name);
	if (!e) {
		return "";
	}
	return e->string.c_str();
}

int CL_UIVar_Integer(const char *name)
{
	cl_uivar_t *e = CL_UIVar_Find(name);
	if (!e) {
		return 0;
	}
	return e->integer;
}

float CL_UIVar_Value(const char *name)
{
	cl_uivar_t *e = CL_UIVar_Find(name);
	if (!e) {
		return 0.0f;
	}
	return e->value;
}

void CL_UIVar_Reset(const char *name)
{
	cl_uivar_t *e = CL_UIVar_Find(name);
	if (!e) {
		return;
	}
	CL_UIVar_Set(name, e->resetString.c_str());
}

unsigned CL_UIVar_ModCount(const char *name)
{
	cl_uivar_t *e = CL_UIVar_Find(name);
	if (!e) {
		return 0u;
	}
	return e->modificationCount;
}

unsigned CL_UIVar_ModCountEntry(const cl_uivar_t *entry)
{
	if (!entry) {
		return 0u;
	}
	return entry->modificationCount;
}

const char *CL_UIVar_EntryString(const cl_uivar_t *entry)
{
	if (!entry) {
		return "";
	}
	return entry->string.c_str();
}

float CL_UIVar_EntryValue(const cl_uivar_t *entry)
{
	if (!entry) {
		return 0.0f;
	}
	return entry->value;
}

unsigned CL_UIVar_Epoch(void)
{
	return s_uivarEpoch;
}
