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
#include "cl_scoreboard_host.h"

#include <cmath>
#include <cstring>

static uir_scoreboard_row_t  g_scoreboardRows[UIR_SCOREBOARD_MAX_ROWS];
static uir_scoreboard_meta_t g_scoreboardMeta;
static int                   g_scoreboardCount = 0;
static uint64_t              g_scoreboardRevision = 1;

static cvar_t *ui_om_scoreboard_team_mode = NULL;
static cvar_t *ui_om_scoreboard_deaths_label = NULL;
static cvar_t *ui_om_scoreboard_gametype = NULL;
static cvar_t *ui_om_scoreboard_tow_allied_obj1 = NULL;
static cvar_t *ui_om_scoreboard_tow_allied_obj2 = NULL;
static cvar_t *ui_om_scoreboard_tow_allied_obj3 = NULL;
static cvar_t *ui_om_scoreboard_tow_allied_obj4 = NULL;
static cvar_t *ui_om_scoreboard_tow_allied_obj5 = NULL;
static cvar_t *ui_om_scoreboard_tow_axis_obj1 = NULL;
static cvar_t *ui_om_scoreboard_tow_axis_obj2 = NULL;
static cvar_t *ui_om_scoreboard_tow_axis_obj3 = NULL;
static cvar_t *ui_om_scoreboard_tow_axis_obj4 = NULL;
static cvar_t *ui_om_scoreboard_tow_axis_obj5 = NULL;
static cvar_t *ui_om_scoreboard_lib_toggle1 = NULL;
static cvar_t *ui_om_scoreboard_lib_toggle2 = NULL;
static cvar_t *ui_om_scoreboard_sort = NULL;
static cvar_t *ui_om_scoreboard_sort_asc = NULL;
static cvar_t *ui_om_scoreboard_server_name = NULL;
static cvar_t *ui_om_scoreboard_gamemode = NULL;
static cvar_t *ui_om_scoreboard_spectator_count = NULL;

static const char *UIR_Scoreboard_GamemodeLabel(int gametype)
{
	switch (gametype) {
	case GT_FFA:
		return "FFA";
	case GT_TEAM:
		return "TDM";
	case GT_TEAM_ROUNDS:
		return "ROUNDS";
	case GT_OBJECTIVE:
		return "OBJECTIVE";
	case GT_TOW:
		return "TOW";
	case GT_LIBERATION:
		return "LIBERATION";
	default:
		return "MP";
	}
}

typedef enum {
	UIR_SCOREBOARD_SORT_NAME,
	UIR_SCOREBOARD_SORT_KILLS,
	UIR_SCOREBOARD_SORT_DEATHS,
	UIR_SCOREBOARD_SORT_KD,
	UIR_SCOREBOARD_SORT_TIME,
	UIR_SCOREBOARD_SORT_PING,
} uir_scoreboard_sort_t;

static uir_scoreboard_sort_t g_scoreboardSortKey = UIR_SCOREBOARD_SORT_KILLS;
static qboolean              g_scoreboardSortAsc = qfalse;

static void UIR_Scoreboard_EnsureMetaCvars(void);

static const char *UIR_Scoreboard_ResolveServerName(void)
{
	const char *hostname = NULL;

	if (clc.state >= CA_CONNECTED && cl.gameState.stringOffsets[CS_SERVERINFO]) {
		const char *info = cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO];

		hostname = Info_ValueForKey(info, "hostname");
		if (!hostname || !hostname[0]) {
			hostname = Info_ValueForKey(info, "sv_hostname");
		}
	}
	if (!hostname || !hostname[0]) {
		hostname = Cvar_VariableString("sv_hostname");
	}
	if (!hostname || !hostname[0]) {
		hostname = clc.servername;
	}
	return hostname ? hostname : "";
}

static const char *UIR_Scoreboard_SortColumnName(uir_scoreboard_sort_t key)
{
	switch (key) {
	case UIR_SCOREBOARD_SORT_NAME:
		return "name";
	case UIR_SCOREBOARD_SORT_KILLS:
		return "kills";
	case UIR_SCOREBOARD_SORT_DEATHS:
		return "deaths";
	case UIR_SCOREBOARD_SORT_KD:
		return "kd";
	case UIR_SCOREBOARD_SORT_TIME:
		return "time";
	case UIR_SCOREBOARD_SORT_PING:
		return "ping";
	default:
		return "kills";
	}
}

static uir_scoreboard_sort_t UIR_Scoreboard_SortFromColumn(const char *column)
{
	if (!column || !column[0]) {
		return UIR_SCOREBOARD_SORT_KILLS;
	}
	if (!Q_stricmp(column, "name")) {
		return UIR_SCOREBOARD_SORT_NAME;
	}
	if (!Q_stricmp(column, "kills")) {
		return UIR_SCOREBOARD_SORT_KILLS;
	}
	if (!Q_stricmp(column, "deaths") || !Q_stricmp(column, "total")) {
		return UIR_SCOREBOARD_SORT_DEATHS;
	}
	if (!Q_stricmp(column, "kd")) {
		return UIR_SCOREBOARD_SORT_KD;
	}
	if (!Q_stricmp(column, "time")) {
		return UIR_SCOREBOARD_SORT_TIME;
	}
	if (!Q_stricmp(column, "ping")) {
		return UIR_SCOREBOARD_SORT_PING;
	}
	return UIR_SCOREBOARD_SORT_KILLS;
}

static int UIR_Scoreboard_ParseTimeSeconds(const char *text)
{
	const char *sep;
	int         minutes;
	int         seconds;

	if (!text || !text[0]) {
		return 0;
	}
	sep = strchr(text, ':');
	if (!sep) {
		return atoi(text);
	}
	minutes = atoi(text);
	seconds = atoi(sep + 1);
	if (minutes < 0) {
		minutes = 0;
	}
	if (seconds < 0) {
		seconds = 0;
	}
	return minutes * 60 + seconds;
}

static int UIR_Scoreboard_CompareRows(const uir_scoreboard_row_t *a, const uir_scoreboard_row_t *b)
{
	int cmp = 0;

	if (!a || !b) {
		return 0;
	}
	switch (g_scoreboardSortKey) {
	case UIR_SCOREBOARD_SORT_NAME:
		cmp = Q_stricmp(a->name, b->name);
		break;
	case UIR_SCOREBOARD_SORT_KILLS:
		cmp = atoi(a->kills) - atoi(b->kills);
		break;
	case UIR_SCOREBOARD_SORT_DEATHS:
		cmp = atoi(a->deaths) - atoi(b->deaths);
		break;
	case UIR_SCOREBOARD_SORT_KD:
		cmp = (int)((atof(a->kd) - atof(b->kd)) * 100.0f);
		break;
	case UIR_SCOREBOARD_SORT_TIME:
		cmp = UIR_Scoreboard_ParseTimeSeconds(a->time) - UIR_Scoreboard_ParseTimeSeconds(b->time);
		break;
	case UIR_SCOREBOARD_SORT_PING:
		cmp = atoi(a->ping) - atoi(b->ping);
		break;
	default:
		cmp = 0;
		break;
	}
	if (cmp == 0) {
		cmp = Q_stricmp(a->name, b->name);
	}
	return g_scoreboardSortAsc ? cmp : -cmp;
}

static void UIR_Scoreboard_SortPlayerRange(int start, int end)
{
	while (start < end) {
		int best = start;
		int i;

		for (i = start + 1; i < end; i++) {
			if (UIR_Scoreboard_CompareRows(&g_scoreboardRows[i], &g_scoreboardRows[best]) < 0) {
				best = i;
			}
		}
		if (best != start) {
			uir_scoreboard_row_t tmp = g_scoreboardRows[start];
			g_scoreboardRows[start] = g_scoreboardRows[best];
			g_scoreboardRows[best] = tmp;
		}
		start++;
	}
}

static void UIR_Scoreboard_SortRowsInPlace(void)
{
	int i = 0;

	while (i < g_scoreboardCount) {
		if (g_scoreboardRows[i].kind != UIR_SCORE_ROW_PLAYER) {
			i++;
			continue;
		}
		const qboolean spectatorRun = g_scoreboardRows[i].isSpectator;
		int runStart = i;
		while (i < g_scoreboardCount &&
		       g_scoreboardRows[i].kind == UIR_SCORE_ROW_PLAYER &&
		       g_scoreboardRows[i].isSpectator == spectatorRun) {
			i++;
		}
		UIR_Scoreboard_SortPlayerRange(runStart, i);
	}
}

static void UIR_Scoreboard_PublishSortCvars(void)
{
	UIR_Scoreboard_EnsureMetaCvars();
	Cvar_Set("ui_om_scoreboard_sort", UIR_Scoreboard_SortColumnName(g_scoreboardSortKey));
	Cvar_Set("ui_om_scoreboard_sort_asc", g_scoreboardSortAsc ? "1" : "0");
}

static void UIR_Scoreboard_PublishLayoutCvars(int logicalHeight)
{
	int spectatorRows = 0;
	int i;

	/*
	 * Changed in OPM: row height/font live in scoreboard XML (theme tokens +
	 * hardcoded list height). Host only publishes spectator count here.
	 */
	UIR_Scoreboard_EnsureMetaCvars();
	for (i = 0; i < g_scoreboardCount; i++) {
		if (g_scoreboardRows[i].kind == UIR_SCORE_ROW_SPACER) {
			continue;
		}
		if (g_scoreboardRows[i].isSpectator) {
			spectatorRows++;
		}
	}
	Cvar_Set("ui_om_scoreboard_spectator_count", va("%d", spectatorRows));
	(void)logicalHeight;
}

static void UIR_Scoreboard_EnsureMetaCvars(void)
{
	if (ui_om_scoreboard_team_mode) {
		return;
	}
	ui_om_scoreboard_team_mode = Cvar_Get("ui_om_scoreboard_team_mode", "0", CVAR_TEMP);
	ui_om_scoreboard_deaths_label = Cvar_Get("ui_om_scoreboard_deaths_label", "Deaths", CVAR_TEMP);
	ui_om_scoreboard_gametype = Cvar_Get("ui_om_scoreboard_gametype", "0", CVAR_TEMP);
	ui_om_scoreboard_tow_allied_obj1 = Cvar_Get("ui_om_scoreboard_tow_allied_obj1", "0", CVAR_TEMP);
	ui_om_scoreboard_tow_allied_obj2 = Cvar_Get("ui_om_scoreboard_tow_allied_obj2", "0", CVAR_TEMP);
	ui_om_scoreboard_tow_allied_obj3 = Cvar_Get("ui_om_scoreboard_tow_allied_obj3", "0", CVAR_TEMP);
	ui_om_scoreboard_tow_allied_obj4 = Cvar_Get("ui_om_scoreboard_tow_allied_obj4", "0", CVAR_TEMP);
	ui_om_scoreboard_tow_allied_obj5 = Cvar_Get("ui_om_scoreboard_tow_allied_obj5", "0", CVAR_TEMP);
	ui_om_scoreboard_tow_axis_obj1 = Cvar_Get("ui_om_scoreboard_tow_axis_obj1", "0", CVAR_TEMP);
	ui_om_scoreboard_tow_axis_obj2 = Cvar_Get("ui_om_scoreboard_tow_axis_obj2", "0", CVAR_TEMP);
	ui_om_scoreboard_tow_axis_obj3 = Cvar_Get("ui_om_scoreboard_tow_axis_obj3", "0", CVAR_TEMP);
	ui_om_scoreboard_tow_axis_obj4 = Cvar_Get("ui_om_scoreboard_tow_axis_obj4", "0", CVAR_TEMP);
	ui_om_scoreboard_tow_axis_obj5 = Cvar_Get("ui_om_scoreboard_tow_axis_obj5", "0", CVAR_TEMP);
	ui_om_scoreboard_lib_toggle1 = Cvar_Get("ui_om_scoreboard_lib_toggle1", "0", CVAR_TEMP);
	ui_om_scoreboard_lib_toggle2 = Cvar_Get("ui_om_scoreboard_lib_toggle2", "0", CVAR_TEMP);
	ui_om_scoreboard_sort = Cvar_Get("ui_om_scoreboard_sort", "kills", CVAR_TEMP);
	ui_om_scoreboard_sort_asc = Cvar_Get("ui_om_scoreboard_sort_asc", "0", CVAR_TEMP);
	ui_om_scoreboard_server_name = Cvar_Get("ui_om_scoreboard_server_name", "", CVAR_TEMP);
	ui_om_scoreboard_gamemode = Cvar_Get("ui_om_scoreboard_gamemode", "", CVAR_TEMP);
	ui_om_scoreboard_spectator_count = Cvar_Get("ui_om_scoreboard_spectator_count", "0", CVAR_TEMP);
}

static void UIR_Scoreboard_PublishSessionCvars(void)
{
	UIR_Scoreboard_EnsureMetaCvars();
	Cvar_Set("ui_om_scoreboard_server_name", UIR_Scoreboard_ResolveServerName());
}

static void UIR_Scoreboard_PublishMetaCvars(void)
{
	UIR_Scoreboard_EnsureMetaCvars();
	Cvar_Set("ui_om_scoreboard_team_mode", g_scoreboardMeta.teamMode ? "1" : "0");
	Cvar_Set("ui_om_scoreboard_deaths_label", g_scoreboardMeta.deathsColLabel);
	Cvar_Set("ui_om_scoreboard_gametype", va("%d", g_scoreboardMeta.gametype));
	Cvar_Set("ui_om_scoreboard_gamemode", UIR_Scoreboard_GamemodeLabel(g_scoreboardMeta.gametype));
	Cvar_Set("ui_om_scoreboard_tow_allied_obj1", va("%d", g_scoreboardMeta.towAlliedObj[0]));
	Cvar_Set("ui_om_scoreboard_tow_allied_obj2", va("%d", g_scoreboardMeta.towAlliedObj[1]));
	Cvar_Set("ui_om_scoreboard_tow_allied_obj3", va("%d", g_scoreboardMeta.towAlliedObj[2]));
	Cvar_Set("ui_om_scoreboard_tow_allied_obj4", va("%d", g_scoreboardMeta.towAlliedObj[3]));
	Cvar_Set("ui_om_scoreboard_tow_allied_obj5", va("%d", g_scoreboardMeta.towAlliedObj[4]));
	Cvar_Set("ui_om_scoreboard_tow_axis_obj1", va("%d", g_scoreboardMeta.towAxisObj[0]));
	Cvar_Set("ui_om_scoreboard_tow_axis_obj2", va("%d", g_scoreboardMeta.towAxisObj[1]));
	Cvar_Set("ui_om_scoreboard_tow_axis_obj3", va("%d", g_scoreboardMeta.towAxisObj[2]));
	Cvar_Set("ui_om_scoreboard_tow_axis_obj4", va("%d", g_scoreboardMeta.towAxisObj[3]));
	Cvar_Set("ui_om_scoreboard_tow_axis_obj5", va("%d", g_scoreboardMeta.towAxisObj[4]));
	Cvar_Set("ui_om_scoreboard_lib_toggle1", va("%d", g_scoreboardMeta.libToggle1));
	Cvar_Set("ui_om_scoreboard_lib_toggle2", va("%d", g_scoreboardMeta.libToggle2));
}

void UIR_Scoreboard_FormatKd(int kills, int deaths, char *out, size_t outSize)
{
	float ratio;

	if (!out || outSize == 0) {
		return;
	}
	if (deaths < 1) {
		deaths = 1;
	}
	ratio = static_cast<float>(kills) / static_cast<float>(deaths);
	Com_sprintf(out, outSize, "%.2f", ratio);
}

void UIR_Scoreboard_Vec4ToHex(const float *rgba, char *out, size_t outSize)
{
	int r;
	int g;
	int b;
	int a;

	if (!out || outSize < 10 || !rgba) {
		if (out && outSize > 0) {
			out[0] = '\0';
		}
		return;
	}
	r = static_cast<int>(rgba[0] * 255.0f + 0.5f);
	g = static_cast<int>(rgba[1] * 255.0f + 0.5f);
	b = static_cast<int>(rgba[2] * 255.0f + 0.5f);
	a = static_cast<int>(rgba[3] * 255.0f + 0.5f);
	if (r < 0) {
		r = 0;
	}
	if (g < 0) {
		g = 0;
	}
	if (b < 0) {
		b = 0;
	}
	if (a < 0) {
		a = 0;
	}
	if (r > 255) {
		r = 255;
	}
	if (g > 255) {
		g = 255;
	}
	if (b > 255) {
		b = 255;
	}
	if (a > 255) {
		a = 255;
	}
	Com_sprintf(out, outSize, "#%02X%02X%02X%02X", r, g, b, a);
}

void UIR_Scoreboard_Clear(void)
{
	g_scoreboardCount = 0;
	std::memset(g_scoreboardRows, 0, sizeof(g_scoreboardRows));
}

void UIR_Scoreboard_SetMeta(const uir_scoreboard_meta_t *meta)
{
	if (!meta) {
		return;
	}
	g_scoreboardMeta = *meta;
}

void UIR_Scoreboard_AddRow(const uir_scoreboard_row_t *row)
{
	if (!row || g_scoreboardCount >= UIR_SCOREBOARD_MAX_ROWS) {
		return;
	}
	g_scoreboardRows[g_scoreboardCount++] = *row;
}

void UIR_Scoreboard_SetRowCount(int count)
{
	if (count < 0) {
		count = 0;
	}
	if (count > UIR_SCOREBOARD_MAX_ROWS) {
		count = UIR_SCOREBOARD_MAX_ROWS;
	}
	g_scoreboardCount = count;
}

void UIR_Scoreboard_NotifyChanged(void)
{
	UIR_Scoreboard_SortRowsInPlace();
	g_scoreboardRevision++;
	UIR_Scoreboard_PublishSessionCvars();
	UIR_Scoreboard_PublishMetaCvars();
	UIR_Scoreboard_PublishSortCvars();
}

void UIR_Scoreboard_ApplySortColumn(const char *column)
{
	const uir_scoreboard_sort_t next = UIR_Scoreboard_SortFromColumn(column);

	if (next == g_scoreboardSortKey) {
		g_scoreboardSortAsc = g_scoreboardSortAsc ? qfalse : qtrue;
	} else {
		g_scoreboardSortKey = next;
		g_scoreboardSortAsc = (next == UIR_SCOREBOARD_SORT_NAME) ? qtrue : qfalse;
	}
	UIR_Scoreboard_SortRowsInPlace();
	g_scoreboardRevision++;
	UIR_Scoreboard_PublishSortCvars();
}

void UIR_Scoreboard_UpdateLayoutForViewport(int logicalHeight)
{
	UIR_Scoreboard_PublishSessionCvars();
	UIR_Scoreboard_PublishLayoutCvars(logicalHeight);
}

int UIR_Scoreboard_GetRowCount(void)
{
	return g_scoreboardCount;
}

const uir_scoreboard_row_t *UIR_Scoreboard_GetRow(int index)
{
	if (index < 0 || index >= g_scoreboardCount) {
		return NULL;
	}
	return &g_scoreboardRows[index];
}

uint64_t UIR_Scoreboard_GetRevision(void)
{
	return g_scoreboardRevision;
}
