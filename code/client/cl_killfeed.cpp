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

#include "cl_killfeed.h"
#include "cl_messages_host.h"
#include "client.h"

#include <cstring>

/* Cmd_Argv is provided by the client/qcommon link unit. */
extern "C" char *Cmd_Argv(int arg);

static uint64_t s_killFeedNextId = 1;

/* Added in OPM: match sanitized death-message names to CS_PLAYERS team. */
static int CL_KillFeed_TeamForPlayerName(const char *name)
{
	char want[MAX_NAME_LENGTH];
	int  i;

	if (!name || !name[0]) {
		return TEAM_NONE;
	}

	want[0] = '\0';
	Com_SanitizeName(name, want, sizeof(want));
	if (!want[0]) {
		Q_strncpyz(want, name, sizeof(want));
	}
	Q_CleanStr(want);

	for (i = 0; i < MAX_CLIENTS; i++) {
		const char *info = CL_ConfigString(CS_PLAYERS + i);
		const char *n;
		char        have[MAX_NAME_LENGTH];

		if (!info || !info[0]) {
			continue;
		}
		n = Info_ValueForKey(info, "name");
		if (!n || !n[0]) {
			continue;
		}
		have[0] = '\0';
		Com_SanitizeName(n, have, sizeof(have));
		if (!have[0]) {
			Q_strncpyz(have, n, sizeof(have));
		}
		Q_CleanStr(have);
		if (!have[0] || Q_stricmp(have, want) != 0) {
			continue;
		}
		{
			const char *teamStr = Info_ValueForKey(info, "team");
			if (teamStr && teamStr[0]) {
				return atoi(teamStr);
			}
		}
		return TEAM_NONE;
	}
	return TEAM_NONE;
}

/* Added in OPM: "allies" / "axis" / "" for HUD fields (icon_team never empty). */
static const char *CL_KillFeed_TeamToken(int team, qboolean forIcon)
{
	if (team == TEAM_AXIS) {
		return "axis";
	}
	if (team == TEAM_ALLIES) {
		return "allies";
	}
	return forIcon ? "allies" : "";
}

static void CL_KillFeed_AddStructuredRow(
	const char *attacker,
	const char *victim,
	const char *weaponClass,
	const char *killKind,
	int         headshot,
	int         friendly
)
{
	char                      text[512];
	uir_hud_kill_feed_input_t row;
	const char               *killerTeam;
	const char               *victimTeam;
	const char               *iconTeam;
	int                       killerTeamId;
	int                       victimTeamId;

	if (!attacker) {
		attacker = "";
	}
	if (!victim) {
		victim = "";
	}
	if (!weaponClass || !weaponClass[0]) {
		weaponClass = "unknown";
	}
	if (!killKind || !killKind[0]) {
		killKind = "player";
	}

	killerTeamId = CL_KillFeed_TeamForPlayerName(attacker);
	victimTeamId = CL_KillFeed_TeamForPlayerName(victim);
	killerTeam = CL_KillFeed_TeamToken(killerTeamId, qfalse);
	victimTeam = CL_KillFeed_TeamToken(victimTeamId, qfalse);
	/* Same rule as the old suffix: killer if present, else victim; unknown → allies. */
	iconTeam = CL_KillFeed_TeamToken(attacker[0] ? killerTeamId : victimTeamId, qtrue);

	if (killKind[0] == 's') {
		Com_sprintf(text, sizeof(text), "%s", victim);
	} else if (killKind[0] == 'w') {
		Com_sprintf(text, sizeof(text), "%s", victim);
	} else {
		Com_sprintf(text, sizeof(text), "%s %s %s", attacker, weaponClass, victim);
	}

	std::memset(&row, 0, sizeof(row));
	row.killer = attacker;
	row.victim = victim;
	row.weaponClass = weaponClass;
	row.killerTeam = killerTeam;
	row.victimTeam = victimTeam;
	row.iconTeam = iconTeam;
	row.killKind = killKind;
	row.text = text;
	if (friendly) {
		row.colorR = 0.5f;
		row.colorG = 1.0f;
		row.colorB = 0.5f;
	} else {
		row.colorR = 1.0f;
		row.colorG = 0.5f;
		row.colorB = 0.5f;
	}
	row.colorA = 1.0f;
	row.headshot = headshot;
	row.friendly = friendly;
	row.stableId = s_killFeedNextId++;

	UIR_HudKillFeed_AddRow(&row);
	UIR_HudKillFeed_NotifyChanged();
}

void CL_KillFeed_HandlePrintDeathMsg(void)
{
	const char *s1;
	const char *s2;
	const char *attacker;
	const char *victim;
	const char *typeStr;
	char        typeChar;
	char        weaponClass[32];
	char        killKind[16];
	int         headshot;
	int         friendly;

	s1 = Cmd_Argv(1);
	s2 = Cmd_Argv(2);
	attacker = Cmd_Argv(3);
	victim = Cmd_Argv(4);
	typeStr = Cmd_Argv(5);
	typeChar = (typeStr && typeStr[0]) ? typeStr[0] : 'p';

	CL_KillFeed_Classify(
		s1, s2, typeChar, weaponClass, sizeof(weaponClass), killKind, sizeof(killKind), &headshot, &friendly
	);

	if (attacker && attacker[0] == 'x' && !attacker[1]) {
		attacker = "";
	}
	if (victim && victim[0] == 'x' && !victim[1]) {
		victim = "";
	}

	CL_KillFeed_AddStructuredRow(attacker, victim, weaponClass, killKind, headshot, friendly);
}

void CL_KillFeed_HandleDeathPrint(const char *text, int friendly)
{
	char        victim[128];
	char        attacker[128];
	char        s1[128];
	char        s2[256];
	char        typeChar;
	char        weaponClass[32];
	char        killKind[16];
	int         headshot;
	int         friendlyOut;

	if (!text || !text[0]) {
		return;
	}

	if (!CL_KillFeed_ParseDeathPrint(
			text, victim, sizeof(victim), attacker, sizeof(attacker), s1, sizeof(s1), s2, sizeof(s2), &typeChar
		)) {
		/* Fallback: still show a row so Base deaths are never invisible. */
		Q_strncpyz(victim, text, sizeof(victim));
		attacker[0] = '\0';
		Q_strncpyz(weaponClass, "unknown", sizeof(weaponClass));
		Q_strncpyz(killKind, "player", sizeof(killKind));
		headshot = 0;
		friendlyOut = friendly ? 1 : 0;
		CL_KillFeed_AddStructuredRow(attacker, victim, weaponClass, killKind, headshot, friendlyOut);
		return;
	}

	CL_KillFeed_Classify(
		s1, s2, typeChar, weaponClass, sizeof(weaponClass), killKind, sizeof(killKind), &headshot, &friendlyOut
	);
	if (friendly) {
		friendlyOut = 1;
		if (typeChar >= 'a' && typeChar <= 'z') {
			typeChar = static_cast<char>(typeChar - 'a' + 'A');
		}
	}

	CL_KillFeed_AddStructuredRow(attacker, victim, weaponClass, killKind, headshot, friendlyOut);
}
