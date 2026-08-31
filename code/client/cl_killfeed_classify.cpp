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

#include <cctype>
#include <cstring>

typedef struct {
	const char *s1;
	const char *s2Prefix; /* NULL = ignore s2; "" = require empty/x */
	const char *weaponClass;
} cl_killfeed_phrase_t;

/* Added in OPM: distinctive Obituary English keys → weapon_class. */
static const cl_killfeed_phrase_t s_playerPhrases[] = {
	{"was gunned down by", "", "pistol"},
	{"was rifled by", "", "rifle"},
	{"was sniped by", "", "sniper"},
	{"was perforated by", "'s' SMG", "smg"},
	{"was machine-gunned by", "", "mg"},
	{"was hunted down by", "", "shotgun"},
	{"was pumped full of buckshot by", "", "shotgun"},
	{"tripped on", "'s grenade", "grenade"},
	{"is picking", "'s shrapnel out of his teeth", "grenade"},
	{"took", "'s rocket right in the kisser", "rocket"},
	{"took", "'s rocket in the face", "rocket"},
	{"stepped on", "'s landmine", "landmine"},
	{"was bashed by", "", "bash"},
	{"was clubbed by", "", "bash"},
	{"was crushed by", "", "crush"},
	{"was telefragged by", "", "telefrag"},
	{"was shot by", "", "unknown"},
	{NULL, NULL, NULL},
};

static const cl_killfeed_phrase_t s_suicidePhrases[] = {
	{"played catch with himself", NULL, "grenade"},
	{"tripped on his own grenade", NULL, "grenade"},
	{"rocketed himself", NULL, "rocket"},
	{"was hoist on his own pitard", NULL, "landmine"},
	{NULL, NULL, NULL},
};

static const cl_killfeed_phrase_t s_worldPhrases[] = {
	{"caught some shrapnel", NULL, "grenade"},
	{"caught a rocket", NULL, "rocket"},
	{"stepped on a land mine", NULL, "landmine"},
	{NULL, NULL, NULL},
};

static qboolean CL_KillFeed_S2IsEmpty(const char *s2)
{
	return !s2 || !s2[0] || (s2[0] == 'x' && !s2[1]) ? qtrue : qfalse;
}

static const char *CL_KillFeed_FindHitloc(const char *s2)
{
	const char *p;

	if (!s2) {
		return NULL;
	}
	p = strstr(s2, " in the ");
	if (!p) {
		return NULL;
	}
	return p + 8;
}

static int CL_KillFeed_IsHeadshotHitloc(const char *hitloc)
{
	if (!hitloc || !hitloc[0]) {
		return 0;
	}
	if (!Q_stricmpn(hitloc, "head", 4) && (hitloc[4] == '\0' || hitloc[4] == ' ')) {
		return 1;
	}
	if (!Q_stricmpn(hitloc, "helmet", 6) && (hitloc[6] == '\0' || hitloc[6] == ' ')) {
		return 1;
	}
	if (!Q_stricmpn(hitloc, "neck", 4) && (hitloc[4] == '\0' || hitloc[4] == ' ')) {
		return 1;
	}
	return 0;
}

static void CL_KillFeed_S2Base(const char *s2, char *out, size_t outSize)
{
	const char *hit;
	size_t      len;

	if (!out || outSize == 0) {
		return;
	}
	out[0] = '\0';
	if (!s2) {
		return;
	}
	hit = strstr(s2, " in the ");
	if (!hit) {
		Q_strncpyz(out, s2, outSize);
		return;
	}
	len = static_cast<size_t>(hit - s2);
	if (len >= outSize) {
		len = outSize - 1;
	}
	std::memcpy(out, s2, len);
	out[len] = '\0';
}

static qboolean CL_KillFeed_MatchPhrase(
	const cl_killfeed_phrase_t *table,
	const char *s1,
	const char *s2Base,
	const char **weaponOut
)
{
	int i;

	if (!table || !s1 || !weaponOut) {
		return qfalse;
	}

	for (i = 0; table[i].s1; i++) {
		if (Q_stricmp(s1, table[i].s1) != 0) {
			continue;
		}
		if (table[i].s2Prefix == NULL) {
			*weaponOut = table[i].weaponClass;
			return qtrue;
		}
		if (table[i].s2Prefix[0] == '\0') {
			if (CL_KillFeed_S2IsEmpty(s2Base) || (s2Base[0] == 'x' && s2Base[1] == '\0')) {
				*weaponOut = table[i].weaponClass;
				return qtrue;
			}
			continue;
		}
		if (s2Base && !Q_stricmpn(s2Base, table[i].s2Prefix, strlen(table[i].s2Prefix))) {
			*weaponOut = table[i].weaponClass;
			return qtrue;
		}
	}
	return qfalse;
}

void CL_KillFeed_Classify(
	const char *s1,
	const char *s2,
	char        typeChar,
	char       *weaponClassOut,
	size_t      weaponClassSize,
	char       *killKindOut,
	size_t      killKindSize,
	int        *headshotOut,
	int        *friendlyOut
)
{
	char        s2Base[256];
	const char *weapon = "unknown";
	const char *kind = "player";
	const char *hitloc;
	char        lowerType;

	if (weaponClassOut && weaponClassSize > 0) {
		Q_strncpyz(weaponClassOut, "unknown", weaponClassSize);
	}
	if (killKindOut && killKindSize > 0) {
		Q_strncpyz(killKindOut, "player", killKindSize);
	}
	if (headshotOut) {
		*headshotOut = 0;
	}
	if (friendlyOut) {
		*friendlyOut = 0;
	}

	lowerType = static_cast<char>(tolower(static_cast<unsigned char>(typeChar)));
	if (friendlyOut) {
		*friendlyOut = (typeChar && typeChar != lowerType) ? 1 : 0;
	}

	if (lowerType == 's') {
		kind = "suicide";
	} else if (lowerType == 'w') {
		kind = "world";
	} else {
		kind = "player";
	}
	if (killKindOut && killKindSize > 0) {
		Q_strncpyz(killKindOut, kind, killKindSize);
	}

	CL_KillFeed_S2Base(s2, s2Base, sizeof(s2Base));
	hitloc = CL_KillFeed_FindHitloc(s2);
	if (headshotOut) {
		*headshotOut = CL_KillFeed_IsHeadshotHitloc(hitloc);
	}

	if (lowerType == 's') {
		CL_KillFeed_MatchPhrase(s_suicidePhrases, s1 ? s1 : "", s2Base, &weapon);
	} else if (lowerType == 'w') {
		CL_KillFeed_MatchPhrase(s_worldPhrases, s1 ? s1 : "", s2Base, &weapon);
	} else {
		CL_KillFeed_MatchPhrase(s_playerPhrases, s1 ? s1 : "", s2Base, &weapon);
	}

	if (weaponClassOut && weaponClassSize > 0) {
		Q_strncpyz(weaponClassOut, weapon, weaponClassSize);
	}
}

static void CL_KillFeed_TrimCopy(const char *src, size_t len, char *out, size_t outSize)
{
	size_t start = 0;
	size_t end;

	if (!out || outSize == 0) {
		return;
	}
	out[0] = '\0';
	if (!src || len == 0) {
		return;
	}
	while (start < len && (src[start] == ' ' || src[start] == '\t')) {
		start++;
	}
	end = len;
	while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t' || src[end - 1] == '\n' || src[end - 1] == '\r')) {
		end--;
	}
	if (end <= start) {
		return;
	}
	len = end - start;
	if (len >= outSize) {
		len = outSize - 1;
	}
	std::memcpy(out, src + start, len);
	out[len] = '\0';
}

static qboolean CL_KillFeed_ParseWithTable(
	const cl_killfeed_phrase_t *table,
	char                        typeChar,
	const char                 *text,
	char                       *victimOut,
	size_t                      victimSize,
	char                       *attackerOut,
	size_t                      attackerSize,
	char                       *s1Out,
	size_t                      s1Size,
	char                       *s2Out,
	size_t                      s2Size,
	char                       *typeCharOut
)
{
	int i;

	for (i = 0; table[i].s1; i++) {
		const char *hit = Q_stristr(text, table[i].s1);
		const char *after;
		size_t      phraseLen;

		if (!hit) {
			continue;
		}
		phraseLen = strlen(table[i].s1);
		CL_KillFeed_TrimCopy(text, static_cast<size_t>(hit - text), victimOut, victimSize);
		Q_strncpyz(s1Out, table[i].s1, s1Size);
		after = hit + phraseLen;
		while (*after == ' ') {
			after++;
		}

		if (typeChar == 's' || typeChar == 'w') {
			attackerOut[0] = '\0';
			Q_strncpyz(s2Out, "", s2Size);
			if (typeCharOut) {
				*typeCharOut = typeChar;
			}
			return qtrue;
		}

		if (table[i].s2Prefix && table[i].s2Prefix[0]) {
			const char *pref = Q_stristr(after, table[i].s2Prefix);
			if (!pref) {
				continue;
			}
			CL_KillFeed_TrimCopy(after, static_cast<size_t>(pref - after), attackerOut, attackerSize);
			Q_strncpyz(s2Out, pref, s2Size);
		} else {
			const char *inThe = Q_stristr(after, " in the ");
			if (inThe) {
				CL_KillFeed_TrimCopy(after, static_cast<size_t>(inThe - after), attackerOut, attackerSize);
				Q_strncpyz(s2Out, inThe, s2Size);
			} else {
				Q_strncpyz(attackerOut, after, attackerSize);
				/* Strip trailing whitespace from attacker copy. */
				{
					size_t n = strlen(attackerOut);
					while (n > 0 &&
					       (attackerOut[n - 1] == ' ' || attackerOut[n - 1] == '\n' || attackerOut[n - 1] == '\r')) {
						attackerOut[--n] = '\0';
					}
				}
				Q_strncpyz(s2Out, "", s2Size);
			}
		}
		if (typeCharOut) {
			*typeCharOut = typeChar;
		}
		return qtrue;
	}
	return qfalse;
}

qboolean CL_KillFeed_ParseDeathPrint(
	const char *text,
	char       *victimOut,
	size_t      victimSize,
	char       *attackerOut,
	size_t      attackerSize,
	char       *s1Out,
	size_t      s1Size,
	char       *s2Out,
	size_t      s2Size,
	char       *typeCharOut
)
{
	char        buf[512];
	const char *p;

	if (!text || !victimOut || !attackerOut || !s1Out || !s2Out || victimSize == 0 || attackerSize == 0
	    || s1Size == 0 || s2Size == 0) {
		return qfalse;
	}
	victimOut[0] = '\0';
	attackerOut[0] = '\0';
	s1Out[0] = '\0';
	s2Out[0] = '\0';

	Q_strncpyz(buf, text, sizeof(buf));
	{
		size_t n = strlen(buf);
		while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
			buf[--n] = '\0';
		}
	}
	p = buf;
	while (*p == ' ' || *p == '\t') {
		p++;
	}

	if (CL_KillFeed_ParseWithTable(
			s_playerPhrases, 'p', p, victimOut, victimSize, attackerOut, attackerSize, s1Out, s1Size, s2Out, s2Size,
			typeCharOut
		)) {
		return qtrue;
	}
	if (CL_KillFeed_ParseWithTable(
			s_suicidePhrases, 's', p, victimOut, victimSize, attackerOut, attackerSize, s1Out, s1Size, s2Out, s2Size,
			typeCharOut
		)) {
		return qtrue;
	}
	if (CL_KillFeed_ParseWithTable(
			s_worldPhrases, 'w', p, victimOut, victimSize, attackerOut, attackerSize, s1Out, s1Size, s2Out, s2Size,
			typeCharOut
		)) {
		return qtrue;
	}
	return qfalse;
}
