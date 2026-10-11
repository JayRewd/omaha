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

// DESCRIPTION:
// Offline demo metadata scanner for the modern demo browser.

#pragma once

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

#include <stdint.h>

#include <string>
#include <vector>

typedef enum {
	DEMOSCAN_PENDING = 0,
	DEMOSCAN_OK,
	DEMOSCAN_BAD
} demoScanState_t;

typedef struct {
	char     name[MAX_NAME_LENGTH * 2];
	int      clientNum;
	int      team;
	int      kills;
	int      deaths;
	qboolean hasScore;
} demoScanPlayer_t;

typedef struct {
	char             file[MAX_QPATH];     // file name inside demos/, with extension
	char             baseName[MAX_QPATH]; // file name without extension
	int              protocol;
	int64_t          fileSize;
	int64_t          fileTime;  // mtime (epoch seconds)
	int64_t          createdAt; // list/sort date: mtime, else filename stamp
	demoScanState_t  state;
	qboolean         truncated;

	char             mapName[MAX_QPATH];
	char             hostname[128];
	char             gametypeName[64];
	int              gametype;
	int              fraglimit;
	int              timelimit;
	int              durationMs;

	int              numPlayers;
	demoScanPlayer_t players[MAX_CLIENTS];

	char             searchText[4096]; // lowercase blob of all searchable fields
} demoScanEntry_t;

void                   CL_DemoScan_Init(void);
void                   CL_DemoScan_Shutdown(void);
// Re-list demos/ and queue new or changed files for scanning
void                   CL_DemoScan_Refresh(void);
// Scan queued files for up to budgetMs; returns qtrue if any entry changed
qboolean               CL_DemoScan_Think(int budgetMs);
qboolean               CL_DemoScan_IsBusy(void);
int                    CL_DemoScan_Count(void);
const demoScanEntry_t *CL_DemoScan_Get(int index);
int                    CL_DemoScan_Find(const char *file);
uint64_t               CL_DemoScan_Revision(void);
const char            *CL_DemoScan_GametypeLabel(const demoScanEntry_t *entry);

// Lightweight per-message walker shared by the scanner and the playback
// timeline index. It reads server commands, gamestate configstrings and the
// snapshot header, and never decodes deltas.
typedef struct {
	void *ctx;
	// Return qfalse to stop walking the rest of the demo
	qboolean (*onGamestate)(void *ctx, int serverCommandSequence);
	void (*onConfigstring)(void *ctx, int index, const char *value);
	void (*onServerCommand)(void *ctx, int seq, const char *text);
	// deltaNum is the raw byte (0 = uncompressed snapshot)
	void (*onSnapshot)(void *ctx, int serverTime, int deltaNum);
} demoWalkHandlers_t;

// Returns qfalse when onGamestate asked to stop
qboolean CL_DemoWalk_ParseMessage(int protocol, msg_t *msg, const demoWalkHandlers_t *h);
int      CL_DemoWalk_NormalizeConfigstring(int protocol, int num);
int      CL_DemoWalk_Tokenize(const char *text, std::vector<std::string> *out);
