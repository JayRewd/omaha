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

#define UIR_BROWSER_MAX_ROWS 512
#define UIR_BROWSER_NAME_LEN 96

typedef struct {
	qboolean    favorite;
	char        name[UIR_BROWSER_NAME_LEN];
	char        map[32];
	int         players;
	int         maxPlayers;
	char        gametype[32];
	int         ping;
	char        ip[64];
	char        gameVer[16];
	qboolean    diffVersion;
	unsigned int realIP;
	int         queryPort;
} uir_browser_row_t;

void UIR_Browser_ClearRows(void);
int  UIR_Browser_FindRow(unsigned int realIP, int queryPort);
int  UIR_Browser_UpsertRow(unsigned int realIP, int queryPort, const uir_browser_row_t *row);
void UIR_Browser_NotifyChanged(void);
void UIR_Browser_SetQueryStats(int discovered, int completed, int totalPlayers, qboolean scanning);
const char *UIR_Browser_NormalizeGametype(const char *raw);
qboolean UIR_Browser_IsFavoriteIp(const char *ip);
void UIR_Browser_ApplyFavorites(void);
void UIR_Browser_ToggleFavoriteByIp(const char *ip);
void UIR_Browser_SaveFavoritesToCvar(void);
int  UIR_Browser_GetRowCount(void);
const uir_browser_row_t *UIR_Browser_GetRow(int index);
int  UIR_Browser_FindRowByIp(const char *ip);
void UIR_Browser_SeedMock(void);

#ifdef __cplusplus
}
#endif
