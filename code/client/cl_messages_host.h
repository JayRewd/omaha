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
#pragma once

#include "../qcommon/q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UIR_HUD_MESSAGES_MAX_ROWS 5
#define UIR_HUD_GAME_MESSAGES_MAX_ROWS 4
#define UIR_HUD_CHAT_MAX_ROWS 5
#define UIR_HUD_KILL_FEED_MAX_ROWS 6

typedef struct {
	char  text[512];
	char  color[16];
	float alpha;
	int   bold;
	uint64_t stableId; /* Added in OPM: monotonic key for foreach lifetime */
} uir_hud_message_row_t;

typedef struct {
	const char *text;
	float       colorR;
	float       colorG;
	float       colorB;
	float       colorA;
	int         bold;
	int         beginDecay;
	int         endDecay;
	uint64_t    stableId; /* Added in OPM */
} uir_hud_message_input_t;

/* Added in OPM: structured kill-feed rows for hud-kill-feed collection. */
typedef struct {
	char     killer[64];
	char     victim[64];
	char     weaponClass[32];
	char     killerTeam[16];
	char     victimTeam[16];
	char     iconTeam[16];
	char     killKind[16];
	char     text[512];
	char     color[16];
	int      headshot;
	int      friendly;
	uint64_t stableId;
} uir_hud_kill_feed_row_t;

typedef struct {
	const char *killer;
	const char *victim;
	const char *weaponClass;
	const char *killerTeam;
	const char *victimTeam;
	const char *iconTeam;
	const char *killKind;
	const char *text;
	float       colorR;
	float       colorG;
	float       colorB;
	float       colorA;
	int         headshot;
	int         friendly;
	uint64_t    stableId;
} uir_hud_kill_feed_input_t;

void UIR_HudMessages_Clear(void);
void UIR_HudMessages_SetAlphaScale(float scale);
void UIR_HudMessages_AddRow(const uir_hud_message_input_t *row);
void UIR_HudMessages_NotifyChanged(void);
int  UIR_HudMessages_GetRowCount(void);
void UIR_HudMessages_GetRow(int index, uir_hud_message_row_t *out);
uint64_t UIR_HudMessages_GetRevision(void);

void UIR_HudGameMessages_Clear(void);
void UIR_HudGameMessages_SetAlphaScale(float scale);
void UIR_HudGameMessages_AddRow(const uir_hud_message_input_t *row);
void UIR_HudGameMessages_NotifyChanged(void);
int  UIR_HudGameMessages_GetRowCount(void);
void UIR_HudGameMessages_GetRow(int index, uir_hud_message_row_t *out);
uint64_t UIR_HudGameMessages_GetRevision(void);

/* Added in OPM: chat-only collection (MESSAGE_CHAT_WHITE / non-death dmbox rows). */
void UIR_HudChat_Clear(void);
void UIR_HudChat_SetAlphaScale(float scale);
void UIR_HudChat_AddRow(const uir_hud_message_input_t *row);
void UIR_HudChat_NotifyChanged(void);
int  UIR_HudChat_GetRowCount(void);
void UIR_HudChat_GetRow(int index, uir_hud_message_row_t *out);
uint64_t UIR_HudChat_GetRevision(void);

/* Added in OPM: structured kill-feed collection from printdeathmsg. */
void UIR_HudKillFeed_Clear(void);
void UIR_HudKillFeed_AddRow(const uir_hud_kill_feed_input_t *row);
void UIR_HudKillFeed_NotifyChanged(void);
int  UIR_HudKillFeed_GetRowCount(void);
void UIR_HudKillFeed_GetRow(int index, uir_hud_kill_feed_row_t *out);
uint64_t UIR_HudKillFeed_GetRevision(void);

#ifdef __cplusplus
}
#endif
