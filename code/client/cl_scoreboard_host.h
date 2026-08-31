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

#define UIR_SCOREBOARD_MAX_ROWS 64

typedef enum {
	UIR_SCORE_ROW_HEADER,
	UIR_SCORE_ROW_PLAYER,
	UIR_SCORE_ROW_SPACER,
} uir_score_row_kind_t;

typedef struct {
	uir_score_row_kind_t kind;
	int                  clientNum;
	char                 slot[8];
	char                 name[64];
	char                 kills[16];
	char                 deaths[16];
	char                 kd[16];
	char                 time[32];
	char                 ping[16];
	char                 textColor[16];
	char                 rowFill[16];
	char                 team[12];
	qboolean             isHeader;
	qboolean             isSpectator;
} uir_scoreboard_row_t;

typedef struct {
	int      gametype;
	qboolean teamMode;
	qboolean roundMode;
	char     deathsColLabel[16];
	int      towAlliedObj[5];
	int      towAxisObj[5];
	int      libToggle1;
	int      libToggle2;
} uir_scoreboard_meta_t;

void UIR_Scoreboard_Clear(void);
void UIR_Scoreboard_SetMeta(const uir_scoreboard_meta_t *meta);
void UIR_Scoreboard_AddRow(const uir_scoreboard_row_t *row);
void UIR_Scoreboard_SetRowCount(int count);
void UIR_Scoreboard_NotifyChanged(void);
int  UIR_Scoreboard_GetRowCount(void);
const uir_scoreboard_row_t *UIR_Scoreboard_GetRow(int index);
uint64_t UIR_Scoreboard_GetRevision(void);

void UIR_Scoreboard_FormatKd(int kills, int deaths, char *out, size_t outSize);
void UIR_Scoreboard_Vec4ToHex(const float *rgba, char *out, size_t outSize);
void UIR_Scoreboard_ApplySortColumn(const char *column);
void UIR_Scoreboard_UpdateLayoutForViewport(int logicalHeight);

#ifdef __cplusplus
}
#endif
