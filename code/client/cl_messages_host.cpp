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

#include "cl_messages_host.h"

#include "client.h"

#include <cstring>

typedef struct {
	char     text[512];
	char     color[16];
	int      bold;
	int      beginDecay;
	int      endDecay;
	uint64_t stableId;
} uir_hud_message_slot_t;

static uir_hud_message_slot_t g_hudMessageSlots[UIR_HUD_MESSAGES_MAX_ROWS];
static int                    g_hudMessageCount = 0;
static float                  g_hudMessageAlphaScale = 1.0f;
static uint64_t               g_hudMessageRevision = 1;

static uir_hud_message_slot_t g_hudGameMessageSlots[UIR_HUD_GAME_MESSAGES_MAX_ROWS];
static int                    g_hudGameMessageCount = 0;
static float                  g_hudGameMessageAlphaScale = 1.0f;
static uint64_t               g_hudGameMessageRevision = 1;

/* Added in OPM: chat-only and structured kill-feed hosts. */
static uir_hud_message_slot_t g_hudChatSlots[UIR_HUD_CHAT_MAX_ROWS];
static int                    g_hudChatCount = 0;
static float                  g_hudChatAlphaScale = 1.0f;
static uint64_t               g_hudChatRevision = 1;

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
} uir_hud_kill_feed_slot_t;

static uir_hud_kill_feed_slot_t g_hudKillFeedSlots[UIR_HUD_KILL_FEED_MAX_ROWS];
static int                      g_hudKillFeedCount = 0;
static uint64_t                 g_hudKillFeedRevision = 1;

static void UIR_Messages_ColorToHex(float r, float g, float b, float a, char *out, size_t outSize)
{
	int ir;
	int ig;
	int ib;
	int ia;

	if (!out || outSize < 10) {
		if (out && outSize > 0) {
			out[0] = '\0';
		}
		return;
	}

	ir = static_cast<int>(r * 255.0f + 0.5f);
	ig = static_cast<int>(g * 255.0f + 0.5f);
	ib = static_cast<int>(b * 255.0f + 0.5f);
	ia = static_cast<int>(a * 255.0f + 0.5f);
	if (ir < 0) {
		ir = 0;
	}
	if (ig < 0) {
		ig = 0;
	}
	if (ib < 0) {
		ib = 0;
	}
	if (ia < 0) {
		ia = 0;
	}
	if (ir > 255) {
		ir = 255;
	}
	if (ig > 255) {
		ig = 255;
	}
	if (ib > 255) {
		ib = 255;
	}
	if (ia > 255) {
		ia = 255;
	}
	Com_sprintf(out, outSize, "#%02X%02X%02X%02X", ir, ig, ib, ia);
}

static float UIR_Messages_ClampAlpha(float alpha)
{
	if (alpha < 0.0f) {
		return 0.0f;
	}
	if (alpha > 1.0f) {
		return 1.0f;
	}
	return alpha;
}

static void UIR_Messages_AddRowToSlots(
	uir_hud_message_slot_t *slots,
	int maxRows,
	int *count,
	const uir_hud_message_input_t *row
)
{
	uir_hud_message_slot_t *slot;

	if (!row || !count || !slots || *count >= maxRows) {
		return;
	}

	slot = &slots[*count];
	if (row->text) {
		Q_strncpyz(slot->text, row->text, sizeof(slot->text));
	} else {
		slot->text[0] = '\0';
	}
	UIR_Messages_ColorToHex(row->colorR, row->colorG, row->colorB, row->colorA, slot->color, sizeof(slot->color));
	slot->bold = row->bold ? 1 : 0;
	slot->beginDecay = row->beginDecay;
	slot->endDecay = row->endDecay;
	slot->stableId = row->stableId;
	(*count)++;
}

static void UIR_Messages_GetRowFromSlots(
	const uir_hud_message_slot_t *slots,
	int count,
	float alphaScale,
	int index,
	uir_hud_message_row_t *out
)
{
	const uir_hud_message_slot_t *slot;

	if (!out || !slots || index < 0 || index >= count) {
		return;
	}

	(void)alphaScale;
	slot = &slots[index];
	Q_strncpyz(out->text, slot->text, sizeof(out->text));
	Q_strncpyz(out->color, slot->color, sizeof(out->color));
	out->bold = slot->bold;
	out->stableId = slot->stableId;

	/* Changed in OPM: foreach lifetime owns fade; host publishes solid alpha. */
	out->alpha = 1.0f;
}

void UIR_HudMessages_Clear(void)
{
	g_hudMessageCount = 0;
	std::memset(g_hudMessageSlots, 0, sizeof(g_hudMessageSlots));
}

void UIR_HudMessages_SetAlphaScale(float scale)
{
	g_hudMessageAlphaScale = UIR_Messages_ClampAlpha(scale);
}

void UIR_HudMessages_AddRow(const uir_hud_message_input_t *row)
{
	UIR_Messages_AddRowToSlots(g_hudMessageSlots, UIR_HUD_MESSAGES_MAX_ROWS, &g_hudMessageCount, row);
}

void UIR_HudMessages_NotifyChanged(void)
{
	g_hudMessageRevision++;
}

int UIR_HudMessages_GetRowCount(void)
{
	return g_hudMessageCount;
}

void UIR_HudMessages_GetRow(int index, uir_hud_message_row_t *out)
{
	UIR_Messages_GetRowFromSlots(
		g_hudMessageSlots, g_hudMessageCount, g_hudMessageAlphaScale, index, out
	);
}

uint64_t UIR_HudMessages_GetRevision(void)
{
	return g_hudMessageRevision;
}

void UIR_HudGameMessages_Clear(void)
{
	g_hudGameMessageCount = 0;
	std::memset(g_hudGameMessageSlots, 0, sizeof(g_hudGameMessageSlots));
}

void UIR_HudGameMessages_SetAlphaScale(float scale)
{
	g_hudGameMessageAlphaScale = UIR_Messages_ClampAlpha(scale);
}

void UIR_HudGameMessages_AddRow(const uir_hud_message_input_t *row)
{
	UIR_Messages_AddRowToSlots(
		g_hudGameMessageSlots, UIR_HUD_GAME_MESSAGES_MAX_ROWS, &g_hudGameMessageCount, row
	);
}

void UIR_HudGameMessages_NotifyChanged(void)
{
	g_hudGameMessageRevision++;
}

int UIR_HudGameMessages_GetRowCount(void)
{
	return g_hudGameMessageCount;
}

void UIR_HudGameMessages_GetRow(int index, uir_hud_message_row_t *out)
{
	UIR_Messages_GetRowFromSlots(
		g_hudGameMessageSlots, g_hudGameMessageCount, g_hudGameMessageAlphaScale, index, out
	);
}

uint64_t UIR_HudGameMessages_GetRevision(void)
{
	return g_hudGameMessageRevision;
}

void UIR_HudChat_Clear(void)
{
	g_hudChatCount = 0;
	std::memset(g_hudChatSlots, 0, sizeof(g_hudChatSlots));
}

void UIR_HudChat_SetAlphaScale(float scale)
{
	g_hudChatAlphaScale = UIR_Messages_ClampAlpha(scale);
}

void UIR_HudChat_AddRow(const uir_hud_message_input_t *row)
{
	UIR_Messages_AddRowToSlots(g_hudChatSlots, UIR_HUD_CHAT_MAX_ROWS, &g_hudChatCount, row);
}

void UIR_HudChat_NotifyChanged(void)
{
	g_hudChatRevision++;
}

int UIR_HudChat_GetRowCount(void)
{
	return g_hudChatCount;
}

void UIR_HudChat_GetRow(int index, uir_hud_message_row_t *out)
{
	UIR_Messages_GetRowFromSlots(g_hudChatSlots, g_hudChatCount, g_hudChatAlphaScale, index, out);
}

uint64_t UIR_HudChat_GetRevision(void)
{
	return g_hudChatRevision;
}

void UIR_HudKillFeed_Clear(void)
{
	g_hudKillFeedCount = 0;
	std::memset(g_hudKillFeedSlots, 0, sizeof(g_hudKillFeedSlots));
}

void UIR_HudKillFeed_AddRow(const uir_hud_kill_feed_input_t *row)
{
	uir_hud_kill_feed_slot_t *slot;
	int                       i;

	if (!row) {
		return;
	}

	/* Ring: drop oldest when full. */
	if (g_hudKillFeedCount >= UIR_HUD_KILL_FEED_MAX_ROWS) {
		for (i = 1; i < UIR_HUD_KILL_FEED_MAX_ROWS; i++) {
			g_hudKillFeedSlots[i - 1] = g_hudKillFeedSlots[i];
		}
		g_hudKillFeedCount = UIR_HUD_KILL_FEED_MAX_ROWS - 1;
	}

	slot = &g_hudKillFeedSlots[g_hudKillFeedCount];
	std::memset(slot, 0, sizeof(*slot));
	if (row->killer) {
		Q_strncpyz(slot->killer, row->killer, sizeof(slot->killer));
	}
	if (row->victim) {
		Q_strncpyz(slot->victim, row->victim, sizeof(slot->victim));
	}
	if (row->weaponClass) {
		Q_strncpyz(slot->weaponClass, row->weaponClass, sizeof(slot->weaponClass));
	} else {
		Q_strncpyz(slot->weaponClass, "unknown", sizeof(slot->weaponClass));
	}
	if (row->killerTeam) {
		Q_strncpyz(slot->killerTeam, row->killerTeam, sizeof(slot->killerTeam));
	}
	if (row->victimTeam) {
		Q_strncpyz(slot->victimTeam, row->victimTeam, sizeof(slot->victimTeam));
	}
	if (row->iconTeam && row->iconTeam[0]) {
		Q_strncpyz(slot->iconTeam, row->iconTeam, sizeof(slot->iconTeam));
	} else {
		Q_strncpyz(slot->iconTeam, "allies", sizeof(slot->iconTeam));
	}
	if (row->killKind) {
		Q_strncpyz(slot->killKind, row->killKind, sizeof(slot->killKind));
	} else {
		Q_strncpyz(slot->killKind, "player", sizeof(slot->killKind));
	}
	if (row->text) {
		Q_strncpyz(slot->text, row->text, sizeof(slot->text));
	}
	UIR_Messages_ColorToHex(
		row->colorR, row->colorG, row->colorB, row->colorA, slot->color, sizeof(slot->color)
	);
	slot->headshot = row->headshot ? 1 : 0;
	slot->friendly = row->friendly ? 1 : 0;
	slot->stableId = row->stableId;
	g_hudKillFeedCount++;
}

void UIR_HudKillFeed_NotifyChanged(void)
{
	g_hudKillFeedRevision++;
}

int UIR_HudKillFeed_GetRowCount(void)
{
	return g_hudKillFeedCount;
}

void UIR_HudKillFeed_GetRow(int index, uir_hud_kill_feed_row_t *out)
{
	const uir_hud_kill_feed_slot_t *slot;

	if (!out || index < 0 || index >= g_hudKillFeedCount) {
		return;
	}

	slot = &g_hudKillFeedSlots[index];
	Q_strncpyz(out->killer, slot->killer, sizeof(out->killer));
	Q_strncpyz(out->victim, slot->victim, sizeof(out->victim));
	Q_strncpyz(out->weaponClass, slot->weaponClass, sizeof(out->weaponClass));
	Q_strncpyz(out->killerTeam, slot->killerTeam, sizeof(out->killerTeam));
	Q_strncpyz(out->victimTeam, slot->victimTeam, sizeof(out->victimTeam));
	Q_strncpyz(out->iconTeam, slot->iconTeam, sizeof(out->iconTeam));
	Q_strncpyz(out->killKind, slot->killKind, sizeof(out->killKind));
	Q_strncpyz(out->text, slot->text, sizeof(out->text));
	Q_strncpyz(out->color, slot->color, sizeof(out->color));
	out->headshot = slot->headshot;
	out->friendly = slot->friendly;
	out->stableId = slot->stableId;
}

uint64_t UIR_HudKillFeed_GetRevision(void)
{
	return g_hudKillFeedRevision;
}
