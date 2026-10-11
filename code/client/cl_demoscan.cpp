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
//
// A server packet is laid out as: reliable ack, server commands, snapshot,
// then cgame messages / VoIP. The scanner only needs the gamestate
// configstrings, the server commands (cs / bcs* / scores) and the snapshot
// serverTime, so it stops reading each packet right after svc_snapshot's
// first field. That avoids delta decoding and does not require cgame.

#include "client.h"
#include "cl_demoscan.h"

#include <ctime>
#include <string>
#include <vector>

// Defined in msg.cpp with C++ linkage; read the protocol 15+ string encoding
// regardless of the running com_protocol.
char *MSG_ReadScrambledString_ver_15(msg_t *msg);
char *MSG_ReadScrambledBigString_ver_15(msg_t *msg);

#define DEMOSCAN_MAX_ARGS       512
#define DEMOSCAN_MAX_SNAP_GAP   10000

struct demoScanSlot_t {
	qboolean active;
	char     name[MAX_NAME_LENGTH * 2];
	int      team;
	int      kills;
	int      deaths;
	qboolean hasScore;
};

struct demoScanJob_t {
	int                      index;
	fileHandle_t             file;
	qboolean                 gotGamestate;
	int                      lastCommandSeq;
	int                      firstTime;
	int                      lastTime;
	int                      durationMs;
	std::vector<std::string> configstrings;
	std::string              bigConfigstring;
	demoScanSlot_t           slots[MAX_CLIENTS];
	std::vector<demoScanPlayer_t> departed;
};

static std::vector<demoScanEntry_t> s_entries;
static std::vector<int>             s_queue;
static demoScanJob_t               *s_job;
static uint64_t                     s_revision = 1;
static byte                         s_msgData[MAX_MSGLEN];

static int DS_NormalizeConfigstring(int protocol, int num)
{
	if (protocol <= PROTOCOL_MOH) {
		if (num <= 5 || num >= 26) {
			return num;
		}
		return num - 2;
	}
	return num;
}

static const char *DS_ReadString(int protocol, msg_t *msg)
{
	if (protocol >= PROTOCOL_MOHTA_MIN) {
		return MSG_ReadScrambledString_ver_15(msg);
	}
	return MSG_ReadString(msg);
}

static const char *DS_ReadBigString(int protocol, msg_t *msg)
{
	if (protocol >= PROTOCOL_MOHTA_MIN) {
		return MSG_ReadScrambledBigString_ver_15(msg);
	}
	return MSG_ReadBigString(msg);
}

/*
 * Whitespace tokenizer with quote grouping (same rules the server uses when
 * it quotes configstring payloads).
 */
static int DS_Tokenize(const char *text, std::vector<std::string> *out)
{
	const char *p = text;

	out->clear();
	while (*p && out->size() < DEMOSCAN_MAX_ARGS) {
		while (*p && (unsigned char)*p <= ' ') {
			p++;
		}
		if (!*p) {
			break;
		}
		std::string token;
		if (*p == '"') {
			p++;
			while (*p && *p != '"') {
				token.push_back(*p++);
			}
			if (*p == '"') {
				p++;
			}
		} else {
			while (*p && (unsigned char)*p > ' ') {
				token.push_back(*p++);
			}
		}
		out->push_back(token);
	}
	return (int)out->size();
}

static int DS_ArgInt(const std::vector<std::string> &args, int i)
{
	if (i < 0 || i >= (int)args.size()) {
		return 0;
	}
	return atoi(args[i].c_str());
}

static const char *DS_ServerInfo(const demoScanJob_t *job)
{
	return job->configstrings[CS_SERVERINFO].c_str();
}

static void DS_PushDeparted(demoScanJob_t *job, int clientNum)
{
	demoScanSlot_t  *slot = &job->slots[clientNum];
	demoScanPlayer_t player;

	if (!slot->active) {
		return;
	}
	memset(&player, 0, sizeof(player));
	Q_strncpyz(player.name, slot->name, sizeof(player.name));
	player.clientNum = clientNum;
	player.team = slot->team;
	player.kills = slot->kills;
	player.deaths = slot->deaths;
	player.hasScore = slot->hasScore;
	job->departed.push_back(player);
	memset(slot, 0, sizeof(*slot));
}

static void DS_UpdatePlayerSlot(demoScanJob_t *job, int clientNum, const char *info)
{
	demoScanSlot_t *slot = &job->slots[clientNum];
	const char     *name;

	name = info && info[0] ? Info_ValueForKey(info, "name") : "";
	if (!name[0]) {
		DS_PushDeparted(job, clientNum);
		return;
	}
	// A different name in the same slot is a new player, not a rename, unless
	// the slot never got a score (cheap heuristic that keeps renames merged).
	if (slot->active && Q_stricmp(slot->name, name) && slot->hasScore) {
		DS_PushDeparted(job, clientNum);
	}
	slot->active = qtrue;
	Q_strncpyz(slot->name, name, sizeof(slot->name));
	slot->team = atoi(Info_ValueForKey(info, "team"));
}

static void DS_SetConfigstring(demoScanJob_t *job, int index, const char *value)
{
	if (index < 0 || index >= MAX_CONFIGSTRINGS) {
		return;
	}
	job->configstrings[index] = value ? value : "";
	if (index >= CS_PLAYERS && index < CS_PLAYERS + MAX_CLIENTS) {
		DS_UpdatePlayerSlot(job, index - CS_PLAYERS, job->configstrings[index].c_str());
	}
}

static void DS_ParseScores(demoScanJob_t *job, int protocol, const std::vector<std::string> &args)
{
	const int gametype = atoi(Info_ValueForKey(DS_ServerInfo(job), "g_gametype"));
	const int datumCount = gametype > GT_FFA ? 6 : 5;
	int       entryCount;
	int       cur = 1;
	int       i;

	entryCount = DS_ArgInt(args, cur++);
	if (entryCount > MAX_CLIENTS) {
		entryCount = MAX_CLIENTS;
	}
	if (protocol >= PROTOCOL_MOHTA_MIN) {
		if (gametype == GT_TOW) {
			cur += 10;
		} else if (gametype == GT_LIBERATION) {
			cur += 2;
		}
	}

	for (i = 0; i < entryCount; i++) {
		const int base = cur + datumCount * i;
		int       clientNum, team, kills, deaths;

		if (base >= (int)args.size()) {
			break;
		}
		clientNum = DS_ArgInt(args, base);
		if (gametype > GT_FFA) {
			team = DS_ArgInt(args, base + 1);
			if (team < 0) {
				team = -team;
			}
			if (clientNum == -1 && protocol >= PROTOCOL_MOHTA_MIN && (team == TEAM_ALLIES || team == TEAM_AXIS)) {
				// Team headers carry one extra token on protocol 15+
				cur++;
			}
			kills = DS_ArgInt(args, base + 2);
			deaths = DS_ArgInt(args, base + 3);
		} else {
			team = -1;
			kills = DS_ArgInt(args, base + 1);
			deaths = DS_ArgInt(args, base + 2);
		}
		if (clientNum < 0 || clientNum >= MAX_CLIENTS) {
			continue;
		}

		demoScanSlot_t *slot = &job->slots[clientNum];
		if (!slot->active) {
			const char *info = job->configstrings[CS_PLAYERS + clientNum].c_str();
			DS_UpdatePlayerSlot(job, clientNum, info);
			if (!slot->active) {
				continue;
			}
		}
		slot->kills = kills;
		slot->deaths = deaths;
		slot->hasScore = qtrue;
		if (team >= 0) {
			slot->team = team;
		}
	}
}

static void DS_HandleCommand(demoScanJob_t *job, int protocol, const char *text)
{
	std::vector<std::string> args;
	const char              *cmd;

	if (!DS_Tokenize(text, &args)) {
		return;
	}
	cmd = args[0].c_str();

	if (!strcmp(cmd, "cs")) {
		DS_SetConfigstring(job, DS_NormalizeConfigstring(protocol, DS_ArgInt(args, 1)), args.size() > 2 ? args[2].c_str() : "");
	} else if (!strcmp(cmd, "bcs0")) {
		job->bigConfigstring = args.size() > 2 ? args[2] : "";
	} else if (!strcmp(cmd, "bcs1")) {
		job->bigConfigstring += args.size() > 2 ? args[2] : "";
	} else if (!strcmp(cmd, "bcs2")) {
		job->bigConfigstring += args.size() > 2 ? args[2] : "";
		DS_SetConfigstring(job, DS_NormalizeConfigstring(protocol, DS_ArgInt(args, 1)), job->bigConfigstring.c_str());
		job->bigConfigstring.clear();
	} else if (!strcmp(cmd, "scores")) {
		DS_ParseScores(job, protocol, args);
	}
}

static void DS_HandleSnapshotTime(demoScanJob_t *job, int serverTime)
{
	if (!job->firstTime) {
		job->firstTime = serverTime;
	} else {
		const int delta = serverTime - job->lastTime;
		if (delta > 0 && delta < DEMOSCAN_MAX_SNAP_GAP) {
			job->durationMs += delta;
		}
	}
	job->lastTime = serverTime;
}

int CL_DemoWalk_NormalizeConfigstring(int protocol, int num)
{
	return DS_NormalizeConfigstring(protocol, num);
}

int CL_DemoWalk_Tokenize(const char *text, std::vector<std::string> *out)
{
	return DS_Tokenize(text, out);
}

static qboolean DS_WalkGamestate(int protocol, msg_t *msg, const demoWalkHandlers_t *h)
{
	const int seq = MSG_ReadLong(msg);

	if (h->onGamestate && !h->onGamestate(h->ctx, seq)) {
		return qfalse;
	}
	for (;;) {
		const int cmd = MSG_ReadByte(msg);

		if (cmd != svc_configstring) {
			// Baselines follow the configstrings; nothing needed past this point
			break;
		}
		const int   index = DS_NormalizeConfigstring(protocol, MSG_ReadShort(msg));
		const char *s = DS_ReadBigString(protocol, msg);
		if (h->onConfigstring) {
			h->onConfigstring(h->ctx, index, s);
		}
	}
	return qtrue;
}

qboolean CL_DemoWalk_ParseMessage(int protocol, msg_t *msg, const demoWalkHandlers_t *h)
{
	MSG_Bitstream(msg);
	MSG_ReadLong(msg);

	for (;;) {
		int cmd;

		if ((size_t)msg->readcount > msg->cursize) {
			return qtrue;
		}
		cmd = MSG_ReadByte(msg);
		switch (cmd) {
		case svc_nop:
			break;
		case svc_serverCommand:
		{
			const int   seq = MSG_ReadLong(msg);
			const char *s = DS_ReadString(protocol, msg);
			if (h->onServerCommand) {
				h->onServerCommand(h->ctx, seq, s);
			}
			break;
		}
		case svc_gamestate:
			return DS_WalkGamestate(protocol, msg, h);
		case svc_snapshot:
		{
			const int serverTime = MSG_ReadLong(msg);
			int       deltaNum;

			MSG_ReadByte(msg); // serverTimeResidual
			deltaNum = MSG_ReadByte(msg);
			if (h->onSnapshot) {
				h->onSnapshot(h->ctx, serverTime, deltaNum);
			}
			return qtrue;
		}
		default:
			// svc_EOF, or data that only follows the snapshot
			return qtrue;
		}
	}
}

struct demoScanWalk_t {
	demoScanJob_t *job;
	int            protocol;
};

static qboolean DS_OnGamestate(void *ctx, int serverCommandSequence)
{
	demoScanWalk_t *w = (demoScanWalk_t *)ctx;

	// A second gamestate means a map change; the browser describes the first map only
	if (w->job->gotGamestate) {
		return qfalse;
	}
	w->job->gotGamestate = qtrue;
	w->job->lastCommandSeq = serverCommandSequence;
	return qtrue;
}

static void DS_OnConfigstring(void *ctx, int index, const char *value)
{
	demoScanWalk_t *w = (demoScanWalk_t *)ctx;
	DS_SetConfigstring(w->job, index, value);
}

static void DS_OnServerCommand(void *ctx, int seq, const char *text)
{
	demoScanWalk_t *w = (demoScanWalk_t *)ctx;

	if (seq > w->job->lastCommandSeq) {
		w->job->lastCommandSeq = seq;
		DS_HandleCommand(w->job, w->protocol, text);
	}
}

static void DS_OnSnapshot(void *ctx, int serverTime, int deltaNum)
{
	demoScanWalk_t *w = (demoScanWalk_t *)ctx;
	DS_HandleSnapshotTime(w->job, serverTime);
}

static qboolean DS_ParseMessage(demoScanJob_t *job, int protocol, msg_t *msg)
{
	demoScanWalk_t     walk = {job, protocol};
	demoWalkHandlers_t h;

	memset(&h, 0, sizeof(h));
	h.ctx = &walk;
	h.onGamestate = DS_OnGamestate;
	h.onConfigstring = DS_OnConfigstring;
	h.onServerCommand = DS_OnServerCommand;
	h.onSnapshot = DS_OnSnapshot;
	return CL_DemoWalk_ParseMessage(protocol, msg, &h);
}

static const char *DS_GametypeFallbackLabel(int gametype)
{
	switch (gametype) {
	case GT_SINGLE_PLAYER:
		return "Single Player";
	case GT_FFA:
		return "Free-For-All";
	case GT_TEAM:
		return "Team-Match";
	case GT_TEAM_ROUNDS:
		return "Round-Based-Match";
	case GT_OBJECTIVE:
		return "Objective-Match";
	case GT_TOW:
		return "Tug-of-War";
	case GT_LIBERATION:
		return "Liberation";
	default:
		return "Unknown";
	}
}

const char *CL_DemoScan_GametypeLabel(const demoScanEntry_t *entry)
{
	if (!entry) {
		return "";
	}
	if (entry->gametypeName[0]) {
		return entry->gametypeName;
	}
	if (entry->state != DEMOSCAN_OK) {
		return "";
	}
	return DS_GametypeFallbackLabel(entry->gametype);
}

static void DS_AppendSearch(demoScanEntry_t *entry, const char *text)
{
	if (!text || !text[0]) {
		return;
	}
	Q_strcat(entry->searchText, sizeof(entry->searchText), text);
	Q_strcat(entry->searchText, sizeof(entry->searchText), "\n");
}

static void DS_BuildSearchText(demoScanEntry_t *entry)
{
	char       buf[64];
	struct tm *tmv;
	time_t     t = (time_t)entry->createdAt;
	int        i;

	entry->searchText[0] = 0;
	DS_AppendSearch(entry, entry->baseName);
	DS_AppendSearch(entry, entry->mapName);
	DS_AppendSearch(entry, entry->hostname);
	DS_AppendSearch(entry, CL_DemoScan_GametypeLabel(entry));
	if (entry->state == DEMOSCAN_OK) {
		Com_sprintf(buf, sizeof(buf), "fraglimit %d", entry->fraglimit);
		DS_AppendSearch(entry, buf);
		Com_sprintf(buf, sizeof(buf), "timelimit %d", entry->timelimit);
		DS_AppendSearch(entry, buf);
		Com_sprintf(buf, sizeof(buf), "%d:%02d", entry->durationMs / 60000, (entry->durationMs / 1000) % 60);
		DS_AppendSearch(entry, buf);
	}
	if (t > 0 && (tmv = localtime(&t))) {
		strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tmv);
		DS_AppendSearch(entry, buf);
	}
	for (i = 0; i < entry->numPlayers; i++) {
		DS_AppendSearch(entry, entry->players[i].name);
	}
	Q_strlwr(entry->searchText);
}

static void DS_FinishJob(demoScanJob_t *job, qboolean ok, qboolean truncated)
{
	demoScanEntry_t *entry = &s_entries[job->index];
	const char      *info;
	const char      *gtString;
	int              i;

	if (job->file) {
		FS_FCloseFile(job->file);
		job->file = 0;
	}

	ok = ok && job->gotGamestate;
	entry->state = ok ? DEMOSCAN_OK : DEMOSCAN_BAD;
	entry->truncated = truncated;
	entry->numPlayers = 0;

	if (ok) {
		info = DS_ServerInfo(job);
		Q_strncpyz(entry->mapName, Info_ValueForKey(info, "mapname"), sizeof(entry->mapName));
		Q_strncpyz(entry->hostname, Info_ValueForKey(info, "sv_hostname"), sizeof(entry->hostname));
		entry->gametype = atoi(Info_ValueForKey(info, "g_gametype"));
		gtString = Info_ValueForKey(info, "g_gametypestring");
		if (gtString[0] && strcmp(gtString, "0")) {
			Q_strncpyz(entry->gametypeName, gtString, sizeof(entry->gametypeName));
		} else {
			entry->gametypeName[0] = 0;
		}
		entry->fraglimit = atoi(Info_ValueForKey(info, "fraglimit"));
		entry->timelimit = atoi(Info_ValueForKey(info, "timelimit"));
		entry->durationMs = job->durationMs;

		for (i = 0; i < MAX_CLIENTS; i++) {
			DS_PushDeparted(job, i);
		}
		for (const demoScanPlayer_t &player : job->departed) {
			if (entry->numPlayers >= MAX_CLIENTS) {
				break;
			}
			entry->players[entry->numPlayers++] = player;
		}
	}

	DS_BuildSearchText(entry);
	s_revision++;
}

static qboolean DS_StartJob(int index)
{
	demoScanEntry_t *entry = &s_entries[index];
	char             path[MAX_QPATH];

	s_job = new demoScanJob_t();
	s_job->index = index;
	s_job->configstrings.resize(MAX_CONFIGSTRINGS);
	memset(s_job->slots, 0, sizeof(s_job->slots));

	Com_sprintf(path, sizeof(path), "demos/%s", entry->file);
	FS_FOpenFileRead(path, &s_job->file, qtrue, qtrue);
	if (!s_job->file) {
		DS_FinishJob(s_job, qfalse, qfalse);
		return qfalse;
	}
	return qtrue;
}

static void DS_EndJob(void)
{
	delete s_job;
	s_job = NULL;
}

/*
 * Reads one framed message. Returns 1 to continue, 0 when the demo ended
 * cleanly, -1 when it was truncated or corrupt.
 */
static int DS_StepJob(demoScanJob_t *job)
{
	const int protocol = s_entries[job->index].protocol;
	int       header[2];
	msg_t     msg;
	int       len;

	if (FS_Read(header, sizeof(header), job->file) != sizeof(header)) {
		return -1;
	}
	len = LittleLong(header[1]);
	if (len == -1) {
		return 0;
	}
	if (len < 0 || len > MAX_MSGLEN) {
		return -1;
	}

	MSG_Init(&msg, s_msgData, sizeof(s_msgData));
	if (FS_Read(s_msgData, len, job->file) != (size_t)len) {
		return -1;
	}
	msg.cursize = len;
	msg.readcount = 0;

	return DS_ParseMessage(job, protocol, &msg) ? 1 : 0;
}

static int64_t DS_ParseFilenameTime(const char *name)
{
	struct tm tmv;
	int       i;

	for (i = 0; i < 14; i++) {
		if (name[i] < '0' || name[i] > '9') {
			return 0;
		}
	}
	if (name[14] != '-') {
		return 0;
	}

	memset(&tmv, 0, sizeof(tmv));
	tmv.tm_year = (name[0] - '0') * 1000 + (name[1] - '0') * 100 + (name[2] - '0') * 10 + (name[3] - '0') - 1900;
	tmv.tm_mon = (name[4] - '0') * 10 + (name[5] - '0') - 1;
	tmv.tm_mday = (name[6] - '0') * 10 + (name[7] - '0');
	tmv.tm_hour = (name[8] - '0') * 10 + (name[9] - '0');
	tmv.tm_min = (name[10] - '0') * 10 + (name[11] - '0');
	tmv.tm_sec = (name[12] - '0') * 10 + (name[13] - '0');
	tmv.tm_isdst = -1;
	return (int64_t)mktime(&tmv);
}

int CL_DemoScan_Find(const char *file)
{
	size_t i;

	if (!file || !file[0]) {
		return -1;
	}
	for (i = 0; i < s_entries.size(); i++) {
		if (!Q_stricmp(s_entries[i].file, file)) {
			return (int)i;
		}
	}
	return -1;
}

void CL_DemoScan_Refresh(void)
{
	std::vector<demoScanEntry_t> next;
	char                       **list;
	int                          numFiles;
	int                          i;

	if (s_job) {
		if (s_job->file) {
			FS_FCloseFile(s_job->file);
		}
		DS_EndJob();
	}
	s_queue.clear();

	list = FS_ListFiles("demos", NULL, qfalse, &numFiles);
	for (i = 0; list && i < numFiles; i++) {
		const char *name = list[i];
		const char *ext;
		char        path[MAX_QPATH];
		int64_t     mtime = 0, size = 0;
		int         existing;

		if (!name || !name[0] || strchr(name, '/') || strchr(name, '\\')) {
			continue;
		}
		if (!FS_IsDemoExt(name, (int)strlen(name))) {
			continue;
		}
		ext = strrchr(name, '.');
		Com_sprintf(path, sizeof(path), "demos/%s", name);
		FS_FileStat(path, &mtime, &size);

		existing = CL_DemoScan_Find(name);
		if (existing >= 0 && s_entries[existing].state != DEMOSCAN_PENDING && s_entries[existing].fileSize == size
			&& s_entries[existing].fileTime == mtime) {
			next.push_back(s_entries[existing]);
			continue;
		}

		demoScanEntry_t entry;
		memset(&entry, 0, sizeof(entry));
		Q_strncpyz(entry.file, name, sizeof(entry.file));
		Q_strncpyz(entry.baseName, name, sizeof(entry.baseName));
		entry.baseName[ext - name] = 0;
		entry.protocol = atoi(ext + 1 + strlen(DEMOEXT));
		entry.fileSize = size;
		entry.fileTime = mtime;
		/* Changed in Omaha: prefer filesystem mtime so newly added/cropped demos
		 * sort to the top; fall back to the YYYYMMDDHHMMSS- filename stamp. */
		entry.createdAt = mtime;
		if (!entry.createdAt) {
			entry.createdAt = DS_ParseFilenameTime(name);
		}
		entry.state = DEMOSCAN_PENDING;
		DS_BuildSearchText(&entry);
		next.push_back(entry);
	}
	if (list) {
		FS_FreeFileList(list);
	}

	s_entries.swap(next);
	for (i = 0; i < (int)s_entries.size(); i++) {
		if (s_entries[i].state == DEMOSCAN_PENDING) {
			s_queue.push_back(i);
		}
	}
	s_revision++;
}

qboolean CL_DemoScan_Think(int budgetMs)
{
	const int start = Sys_Milliseconds();
	qboolean  changed = qfalse;

	while (s_job || !s_queue.empty()) {
		if (!s_job) {
			const int index = s_queue.front();
			s_queue.erase(s_queue.begin());
			if (!DS_StartJob(index)) {
				DS_EndJob();
				changed = qtrue;
				continue;
			}
		}

		const int result = DS_StepJob(s_job);
		if (result <= 0) {
			DS_FinishJob(s_job, qtrue, result < 0 ? qtrue : qfalse);
			DS_EndJob();
			changed = qtrue;
		}

		if (budgetMs >= 0 && Sys_Milliseconds() - start >= budgetMs) {
			break;
		}
	}
	return changed;
}

qboolean CL_DemoScan_IsBusy(void)
{
	return (s_job || !s_queue.empty()) ? qtrue : qfalse;
}

int CL_DemoScan_Count(void)
{
	return (int)s_entries.size();
}

const demoScanEntry_t *CL_DemoScan_Get(int index)
{
	if (index < 0 || index >= (int)s_entries.size()) {
		return NULL;
	}
	return &s_entries[index];
}

uint64_t CL_DemoScan_Revision(void)
{
	return s_revision;
}

/*
 * demo_scan_dump [filter]: synchronously scan demos/ and print the metadata
 * the demo browser shows.
 */
static void CL_DemoScan_Dump_f(void)
{
	const char *filter = Cmd_Argc() > 1 ? Cmd_Argv(1) : "";
	int         i, j;

	CL_DemoScan_Refresh();
	CL_DemoScan_Think(-1);

	for (i = 0; i < CL_DemoScan_Count(); i++) {
		const demoScanEntry_t *e = CL_DemoScan_Get(i);
		char                   date[64] = "---";
		time_t                 t = (time_t)e->createdAt;
		struct tm             *tmv;

		if (filter[0] && !Q_stristr(e->file, filter)) {
			continue;
		}
		if (t > 0 && (tmv = localtime(&t))) {
			strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", tmv);
		}
		Com_Printf(
			"%s | state %d%s | proto %d | map %s | host %s | gt %s (%d) | frag %d | time %d | dur %d:%02d | date %s\n",
			e->file,
			e->state,
			e->truncated ? " (truncated)" : "",
			e->protocol,
			e->mapName,
			e->hostname,
			CL_DemoScan_GametypeLabel(e),
			e->gametype,
			e->fraglimit,
			e->timelimit,
			e->durationMs / 60000,
			(e->durationMs / 1000) % 60,
			date
		);
		for (j = 0; j < e->numPlayers; j++) {
			const demoScanPlayer_t *p = &e->players[j];
			if (p->hasScore) {
				Com_Printf("    [%2d] %-32s team %d  K %d  D %d\n", p->clientNum, p->name, p->team, p->kills, p->deaths);
			} else {
				Com_Printf("    [%2d] %-32s team %d  (no score)\n", p->clientNum, p->name, p->team);
			}
		}
	}
	Com_Printf("demo_scan_dump: %d demos\n", CL_DemoScan_Count());
}

void CL_DemoScan_Init(void)
{
	Cmd_AddCommand("demo_scan_dump", CL_DemoScan_Dump_f);
}

void CL_DemoScan_Shutdown(void)
{
	if (s_job) {
		if (s_job->file) {
			FS_FCloseFile(s_job->file);
		}
		DS_EndJob();
	}
	s_queue.clear();
	Cmd_RemoveCommand("demo_scan_dump");
}
