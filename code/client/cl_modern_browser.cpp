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
#include "cl_modern_browser.h"
#include "cl_browser_host.h"

#include "../gamespy/goaceng.h"
#include "../gamespy/gserverlist_scheduler.h"
#include "../gamespy/sv_gamespy.h"

#include <math.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

static size_t ModernBrowser_NamePrefixLen(const char *gameVer, qboolean isDemo)
{
	size_t len = 2; /* " (" */

	if (isDemo) {
		len += 1;
	}
	if (gameVer) {
		len += strlen(gameVer);
	}
	len += 2; /* ") " */
	return len;
}

static void ModernBrowser_FormatServerName(
	const char *gameVer,
	const char *hostName,
	qboolean isDemo,
	char *out,
	size_t outSize
)
{
	char hostTrunc[UIR_BROWSER_NAME_LEN];
	size_t prefixLen;
	size_t hostMax;

	if (!out || outSize == 0) {
		return;
	}

	prefixLen = ModernBrowser_NamePrefixLen(gameVer, isDemo);
	hostMax = (prefixLen < outSize) ? outSize - prefixLen - 1 : 0;
	Q_strncpyz(hostTrunc, hostName ? hostName : "", hostMax + 1);

	if (isDemo) {
		Com_sprintf(out, outSize, " (d%s) %s", gameVer ? gameVer : "", hostTrunc);
	} else {
		Com_sprintf(out, outSize, " (%s) %s", gameVer ? gameVer : "", hostTrunc);
	}
}

extern "C" unsigned int ServerListGetNumMasters(void);

#define MODERN_BROWSER_LISTS 2

typedef struct {
	int iServerType;
} modern_browser_list_inst_t;

static GServerList                g_modernServerList[MODERN_BROWSER_LISTS];
static modern_browser_list_inst_t g_modernListInst[MODERN_BROWSER_LISTS];
static qboolean                   g_modernDoneUpdating[MODERN_BROWSER_LISTS];
static qboolean                   g_modernGettingList[MODERN_BROWSER_LISTS];
static qboolean                   g_modernUpdatingList = qfalse;
static qboolean                   g_modernRefreshCancelled = qfalse;
static int                        g_modernTotalPlayers = 0;
static unsigned int               g_modernRefreshStartTime = 0;

static void ModernBrowser_AddFilter(char *filter, const char *value, size_t maxsize)
{
	const char *newval;
	size_t      valuelen;
	size_t      filterlen;

	if (*filter) {
		newval = va(" and %s", value);
	} else {
		newval = value;
	}

	valuelen  = strlen(newval);
	filterlen = strlen(filter);
	if (filterlen + valuelen >= maxsize) {
		return;
	}

	Q_strncpyz(filter + filterlen, newval, maxsize - filterlen);
}

static int ModernBrowser_ClampCvar(cvar_t *cvar, int minimum, int maximum)
{
	if (cvar->integer < minimum) {
		return minimum;
	}
	if (cvar->integer > maximum) {
		return maximum;
	}
	return cvar->integer;
}

static void ModernBrowser_ConfigureInternetList(GServerList serverlist, int firstTimeout, int retryTimeout)
{
	ServerListSetMasterConcurrency(serverlist, (int)ServerListGetNumMasters());
	ServerListSetRetryTimeouts(serverlist, (unsigned long)firstTimeout, (unsigned long)retryTimeout);
	ServerListSetPipelining(serverlist, 1);
}

static void ModernBrowser_NewServerList(qboolean internet);

void CL_ModernBrowser_ServerListCallback(
	GServerList serverlist, int msg, void *instance, void *param1, void *param2
);

static void ModernBrowser_NewServerList(qboolean internet)
{
	int         iNumConcurrent;
	int         iActiveLists;
	int         firstTimeout;
	int         retryTimeout;
	const char *secret_key;
	const char *game_name;
	cvar_t     *pRateCvar = Cvar_Get("rate", "5000", CVAR_ARCHIVE | CVAR_USERINFO);
	static cvar_t *browserMaxQueries =
		Cvar_Get("cl_browserMaxQueries", "32", CVAR_ARCHIVE);
	static cvar_t *browserTimeout = Cvar_Get("cl_browserTimeout", "400", CVAR_ARCHIVE);
	static cvar_t *browserRetryTimeout =
		Cvar_Get("cl_browserRetryTimeout", "3000", CVAR_ARCHIVE);
	static cvar_t *dm_omit_spearhead = Cvar_Get("dm_omit_spearhead", "0", 1);

	if (internet) {
		iActiveLists =
			com_target_game->integer >= target_game_e::TG_MOHTT && !dm_omit_spearhead->integer ? 2 : 1;
		iNumConcurrent = GServerListSchedulerBudget(
			ModernBrowser_ClampCvar(browserMaxQueries, 4, 64), iActiveLists
		);
		firstTimeout = ModernBrowser_ClampCvar(browserTimeout, 250, 10000);
		retryTimeout = ModernBrowser_ClampCvar(browserRetryTimeout, 250, 10000);
	} else if (pRateCvar->integer > 25000) {
		iNumConcurrent = 15;
	} else if (pRateCvar->integer > 5000) {
		iNumConcurrent = 10;
	} else if (pRateCvar->integer > 3000) {
		iNumConcurrent = 6;
	} else {
		iNumConcurrent = 4;
	}

	if (com_target_game->integer < target_game_e::TG_MOHTT) {
		game_name  = GS_GetCurrentGameName();
		secret_key = GS_GetCurrentGameKey();

		g_modernListInst[0].iServerType = com_target_game->integer;
		g_modernServerList[0] = ServerListNew(
			game_name,
			game_name,
			secret_key,
			iNumConcurrent,
			(void *)&CL_ModernBrowser_ServerListCallback,
			1,
			(void *)&g_modernListInst[0]
		);

		if (internet) {
			ModernBrowser_ConfigureInternetList(g_modernServerList[0], firstTimeout, retryTimeout);
		}

		g_modernServerList[1] = NULL;
	} else {
		game_name  = GS_GetGameName(target_game_e::TG_MOHTT);
		secret_key = GS_GetGameKey(target_game_e::TG_MOHTT);

		g_modernListInst[0].iServerType = target_game_e::TG_MOHTT;
		g_modernServerList[0] = ServerListNew(
			game_name,
			game_name,
			secret_key,
			iNumConcurrent,
			(void *)&CL_ModernBrowser_ServerListCallback,
			1,
			(void *)&g_modernListInst[0]
		);

		if (internet) {
			ModernBrowser_ConfigureInternetList(g_modernServerList[0], firstTimeout, retryTimeout);
		}

		if (!dm_omit_spearhead->integer) {
			game_name  = GS_GetGameName(target_game_e::TG_MOHTA);
			secret_key = GS_GetGameKey(target_game_e::TG_MOHTA);

			g_modernListInst[1].iServerType = target_game_e::TG_MOHTA;
			g_modernServerList[1] = ServerListNew(
				game_name,
				game_name,
				secret_key,
				iNumConcurrent,
				(void *)&CL_ModernBrowser_ServerListCallback,
				1,
				(void *)&g_modernListInst[1]
			);

			if (internet) {
				ModernBrowser_ConfigureInternetList(g_modernServerList[1], firstTimeout, retryTimeout);
			}
		} else {
			g_modernServerList[1] = NULL;
		}
	}
}

static void ModernBrowser_RefreshStatus(void)
{
	int  i;
	int  discovered = 0;
	int  completed  = 0;
	int  listDiscovered;
	int  listCompleted;
	int  listResponsive;
	int  listTimedout;
	qboolean scanning = qfalse;

	g_modernTotalPlayers = 0;
	for (i = 0; i < UIR_Browser_GetRowCount(); i++) {
		const uir_browser_row_t *row = UIR_Browser_GetRow(i);
		if (row) {
			g_modernTotalPlayers += row->players;
		}
	}

	for (i = 0; i < MODERN_BROWSER_LISTS; i++) {
		if (!g_modernServerList[i]) {
			continue;
		}

		ServerListGetQueryStats(
			g_modernServerList[i], &listDiscovered, &listCompleted, &listResponsive, &listTimedout, NULL
		);
		discovered += listDiscovered;
		completed += listCompleted;

		if (ServerListState(g_modernServerList[i]) != GServerListState::sl_idle) {
			scanning = qtrue;
		}
	}

	if (!scanning && g_modernUpdatingList) {
		qboolean doneUpdating = qtrue;
		for (i = 0; i < MODERN_BROWSER_LISTS; i++) {
			if (!g_modernServerList[i]) {
				continue;
			}
			if (ServerListState(g_modernServerList[i]) != GServerListState::sl_idle
			    || !g_modernDoneUpdating[i]) {
				doneUpdating = qfalse;
				break;
			}
		}
		if (doneUpdating) {
			g_modernUpdatingList = qfalse;
		} else {
			scanning = qtrue;
		}
	}

	UIR_Browser_SetQueryStats(discovered, completed, g_modernTotalPlayers, scanning);
}

static void ModernBrowser_UpsertFromServer(GServer server, int iServerType)
{
	const char     *pszHostName;
	const char     *pszGameVer;
	const char     *pszGameVerNumber;
	const char     *pszMap;
	const char     *pszGametypeRaw;
	char            nameBuf[UIR_BROWSER_NAME_LEN];
	char            ipBuf[64];
	unsigned int    iRealIP;
	int             iPort;
	int             iGameSpyPort;
	int             iNumPlayers;
	int             iMaxPlayers;
	int             rowIndex;
	int             oldPlayers = 0;
	float           fGameVer;
	qboolean        bDiffVersion = qfalse;
	qboolean        bIsDemo = qfalse;
	uir_browser_row_t row;

	if (!server) {
		return;
	}

	pszHostName      = ServerGetStringValue(server, "hostname", "(NONE)");
	pszGameVer       = ServerGetStringValue(server, "gamever", "1.00");
	pszGameVerNumber = pszGameVer;
	pszMap           = ServerGetStringValue(server, "mapname", "(NONE)");
	pszGametypeRaw   = ServerGetStringValue(server, "gametype", "(NONE)");

	if (pszGameVerNumber[0] == 'd') {
		pszGameVerNumber++;
		bIsDemo = qtrue;
	}

	fGameVer = (float)atof(pszGameVerNumber);

	if (com_target_game->integer >= target_game_e::TG_MOHTT) {
		if (iServerType == target_game_e::TG_MOHTT) {
			if (fabs(fGameVer) < 2.3f) {
				bDiffVersion = qtrue;
			}
		} else {
			if (fabs(fGameVer) < 2.1f) {
				bDiffVersion = qtrue;
			}
		}
	} else {
		if (fabs(fGameVer - com_target_shortversion->value) > 0.1f) {
			bDiffVersion = qtrue;
		}
	}

	ModernBrowser_FormatServerName(pszGameVerNumber, pszHostName, bIsDemo, nameBuf, sizeof(nameBuf));

	iRealIP      = inet_addr(ServerGetAddress(server));
	iPort        = ServerGetIntValue(server, "hostport", PORT_SERVER);
	iGameSpyPort = ServerGetQueryPort(server);
	Com_sprintf(ipBuf, sizeof(ipBuf), "%s:%i", ServerGetAddress(server), iPort);

	iNumPlayers = ServerGetIntValue(server, "numplayers", 0);
	iMaxPlayers = ServerGetIntValue(server, "maxplayers", 0);

	rowIndex = UIR_Browser_FindRow(iRealIP, iGameSpyPort);
	if (rowIndex >= 0) {
		const uir_browser_row_t *existing = UIR_Browser_GetRow(rowIndex);
		if (existing) {
			oldPlayers = existing->players;
		}
	}

	memset(&row, 0, sizeof(row));
	row.favorite = UIR_Browser_IsFavoriteIp(ipBuf);
	Q_strncpyz(row.name, nameBuf, sizeof(row.name));
	Q_strncpyz(row.map, pszMap, sizeof(row.map));
	row.players = iNumPlayers;
	row.maxPlayers = iMaxPlayers;
	Q_strncpyz(row.gametype, UIR_Browser_NormalizeGametype(pszGametypeRaw), sizeof(row.gametype));
	row.ping = ServerGetPing(server);
	Q_strncpyz(row.ip, ipBuf, sizeof(row.ip));
	Q_strncpyz(row.gameVer, pszGameVer, sizeof(row.gameVer));
	row.diffVersion = bDiffVersion;
	row.realIP = iRealIP;
	row.queryPort = iGameSpyPort;

	UIR_Browser_UpsertRow(iRealIP, iGameSpyPort, &row);
	(void)oldPlayers;
}

void CL_ModernBrowser_ServerListCallback(
	GServerList serverlist, int msg, void *instance, void *param1, void *param2
)
{
	GServer                     server;
	modern_browser_list_inst_t *listInst;
	int                         iServerType;
	static cvar_t              *dm_omit_spearhead = Cvar_Get("dm_omit_spearhead", "0", 1);
	static cvar_t              *cl_browserDebug = Cvar_Get("cl_browserDebug", "0", CVAR_ARCHIVE);

	listInst    = (modern_browser_list_inst_t *)instance;
	iServerType = listInst ? listInst->iServerType : 0;
	server      = (GServer)param1;

	if (msg == LIST_SERVERADDED || msg == LIST_QUERYSTARTED || msg == LIST_QUERYRETRY
	    || msg == LIST_QUERYTIMEOUT) {
		if (cl_browserDebug->integer && server) {
			const char *label;

			if (msg == LIST_SERVERADDED) {
				label = "listed";
			} else if (msg == LIST_QUERYSTARTED) {
				label = "query";
			} else if (msg == LIST_QUERYRETRY) {
				label = "retry";
			} else {
				label = "timeout";
			}

			Com_Printf(
				"modern browser +%u ms: %s %s:%i\n",
				Sys_Milliseconds() - g_modernRefreshStartTime,
				label,
				ServerGetAddress(server),
				ServerGetQueryPort(server)
			);
		}

		ModernBrowser_RefreshStatus();
		return;
	}

	if (msg == LIST_PROGRESS && param2 == (void *)-1) {
		ModernBrowser_RefreshStatus();
		return;
	}

	if (msg == LIST_PROGRESS) {
		if (cl_browserDebug->integer && server) {
			Com_Printf(
				"modern browser +%u ms: response %s:%i (%i ms)\n",
				Sys_Milliseconds() - g_modernRefreshStartTime,
				ServerGetAddress(server),
				ServerGetQueryPort(server),
				ServerGetPing(server)
			);
		}

		ModernBrowser_UpsertFromServer(server, iServerType);
		UIR_Browser_NotifyChanged();
		ModernBrowser_RefreshStatus();
	} else if (msg == LIST_STATECHANGED) {
		switch (ServerListState(serverlist)) {
		case GServerListState::sl_idle:
			if (com_target_game->integer >= target_game_e::TG_MOHTT) {
				if (iServerType == target_game_e::TG_MOHTT) {
					g_modernDoneUpdating[0] = qtrue;
				} else if (iServerType == target_game_e::TG_MOHTA || dm_omit_spearhead->integer) {
					g_modernDoneUpdating[1] = qtrue;
				}
			} else {
				g_modernDoneUpdating[0] = qtrue;
				g_modernDoneUpdating[1] = qtrue;
			}

			if (g_modernDoneUpdating[0] && g_modernDoneUpdating[1]) {
				g_modernUpdatingList = qfalse;
			}
			break;
		case GServerListState::sl_listxfer:
			if (com_target_game->integer >= target_game_e::TG_MOHTT) {
				if (iServerType == target_game_e::TG_MOHTT) {
					g_modernGettingList[0] = qtrue;
				}
				if (iServerType == target_game_e::TG_MOHTA) {
					g_modernGettingList[1] = qtrue;
				}
			} else {
				g_modernGettingList[0] = qtrue;
				g_modernGettingList[1] = qfalse;
			}
			g_modernUpdatingList = qtrue;
			ModernBrowser_RefreshStatus();
			return;
		case GServerListState::sl_lanlist:
		case GServerListState::sl_querying:
			g_modernUpdatingList = qtrue;
			break;
		default:
			break;
		}

		ModernBrowser_RefreshStatus();
	}
}

void CL_ModernBrowser_Init(void)
{
	int i;

	for (i = 0; i < MODERN_BROWSER_LISTS; i++) {
		g_modernServerList[i] = NULL;
		g_modernDoneUpdating[i] = qfalse;
		g_modernGettingList[i] = qfalse;
	}
	g_modernUpdatingList = qfalse;
	g_modernRefreshCancelled = qfalse;
}

void CL_ModernBrowser_Shutdown(void)
{
	int i;

	for (i = 0; i < MODERN_BROWSER_LISTS; i++) {
		if (g_modernServerList[i]) {
			ServerListHalt(g_modernServerList[i]);
			ServerListClear(g_modernServerList[i]);
			ServerListFree(g_modernServerList[i]);
			g_modernServerList[i] = NULL;
		}
	}
}

void CL_ModernBrowser_Think(void)
{
	int i;

	for (i = 0; i < MODERN_BROWSER_LISTS; i++) {
		if (g_modernServerList[i]) {
			ServerListThink(g_modernServerList[i]);
		}
	}
}

void CL_ModernBrowser_HaltRefresh(void)
{
	int i;

	for (i = 0; i < MODERN_BROWSER_LISTS; i++) {
		if (g_modernServerList[i]) {
			ServerListHalt(g_modernServerList[i]);
		}
	}
	g_modernUpdatingList = qfalse;
}

void CL_ModernBrowser_Refresh(void)
{
	int   i;
	char  filter[256] = {0};
	static cvar_t *dm_max_players       = Cvar_Get("dm_max_players", "0", CVAR_ARCHIVE);
	static cvar_t *dm_min_players       = Cvar_Get("dm_min_players", "0", CVAR_ARCHIVE);
	static cvar_t *dm_show_demo_servers = Cvar_Get("dm_show_demo_servers", "1", CVAR_ARCHIVE);
	static cvar_t *dm_realism_mode      = Cvar_Get("dm_realism_mode", "0", CVAR_ARCHIVE);
	static cvar_t *dm_filter_listen     = Cvar_Get("dm_filter_listen", "1", CVAR_ARCHIVE);
	static cvar_t *dm_filter_empty      = Cvar_Get("dm_filter_empty", "0", CVAR_ARCHIVE);
	static cvar_t *dm_filter_full       = Cvar_Get("dm_filter_full", "0", CVAR_ARCHIVE);
	static cvar_t *ui_om_browser_mock   = Cvar_Get("ui_om_browser_mock", "0", CVAR_TEMP);

	if (ui_om_browser_mock->integer) {
		UIR_Browser_SeedMock();
		UIR_Browser_NotifyChanged();
		UIR_Browser_SetQueryStats(5, 5, 61, qfalse);
		return;
	}

	for (i = 0; i < MODERN_BROWSER_LISTS; i++) {
		if (g_modernServerList[i] && ServerListState(g_modernServerList[i]) != GServerListState::sl_idle) {
			return;
		}
	}

	UIR_Browser_ClearRows();

	for (i = 0; i < MODERN_BROWSER_LISTS; i++) {
		if (g_modernServerList[i]) {
			ServerListClear(g_modernServerList[i]);
			ServerListFree(g_modernServerList[i]);
			g_modernServerList[i] = NULL;
		}
	}

	ModernBrowser_NewServerList(qtrue);

	g_modernDoneUpdating[0]       = qfalse;
	g_modernDoneUpdating[1]       = g_modernServerList[1] == NULL ? qtrue : qfalse;
	g_modernRefreshCancelled      = qfalse;
	g_modernRefreshStartTime      = Sys_Milliseconds();
	g_modernUpdatingList          = qtrue;

	if (dm_min_players->integer) {
		ModernBrowser_AddFilter(filter, va("numplayers >= %d", dm_min_players->integer), sizeof(filter));
	}
	if (dm_max_players->integer) {
		ModernBrowser_AddFilter(filter, va("numplayers <= %d", dm_max_players->integer), sizeof(filter));
	}
	if (dm_show_demo_servers && !dm_show_demo_servers->integer) {
		ModernBrowser_AddFilter(filter, "gamever not like 'd%'", sizeof(filter));
	}
	if (dm_realism_mode && dm_realism_mode->integer == 1) {
		ModernBrowser_AddFilter(filter, "realism=1", sizeof(filter));
	}
	if (dm_filter_listen->integer == 1) {
		ModernBrowser_AddFilter(filter, "dedicated=1", sizeof(filter));
	}
	if (dm_filter_empty && dm_filter_empty->integer) {
		ModernBrowser_AddFilter(filter, "numplayers > 0", sizeof(filter));
	}
	if (dm_filter_full && dm_filter_full->integer == 1) {
		ModernBrowser_AddFilter(filter, "numplayers < maxplayers", sizeof(filter));
	}

	if (g_modernServerList[0]) {
		ServerListUpdate2(g_modernServerList[0], qtrue, filter, GQueryType::qt_status);
	}
	if (g_modernServerList[1]) {
		ServerListUpdate2(g_modernServerList[1], qtrue, filter, GQueryType::qt_status);
	}

	UIR_Browser_ApplyFavorites();
	UIR_Browser_NotifyChanged();
	ModernBrowser_RefreshStatus();
}

qboolean CL_ModernBrowser_IsScanning(void)
{
	int i;

	if (g_modernUpdatingList) {
		return qtrue;
	}
	for (i = 0; i < MODERN_BROWSER_LISTS; i++) {
		if (g_modernServerList[i]
		    && ServerListState(g_modernServerList[i]) != GServerListState::sl_idle) {
			return qtrue;
		}
	}
	return qfalse;
}
