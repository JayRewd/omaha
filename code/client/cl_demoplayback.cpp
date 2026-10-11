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
// Demo playback overlay.
//
// Timeline: both the background index and live playback measure time as the
// sum of snapshot serverTime steps (gaps of 10s or more are skipped, like the
// demo browser), so a position from the index can be compared directly with
// the live position.
//
// Pause / rate shift cl.serverTimeDelta. Seeking forward reads a bounded
// batch of messages per frame so cgame keeps every reliable command; seeking
// backward restarts the demo and then seeks forward.
//
// Crop export plays the range through the client: the first message becomes
// a gamestate plus a non-delta snapshot built from the parsed state, and
// later messages are copied raw unless their snapshot deltas from before the
// crop, in which case the snapshot is rewritten as a keyframe and the
// surrounding bits are spliced in unchanged.

#include "client.h"
#include "cl_demoplayback.h"
#include "cl_demoscan.h"
#include "cl_killfeed.h"
#include "cl_uimenu_dispatcher.h"
#include "cl_uirender.h"
#include "cl_uivars.h"

#include <string>
#include <vector>

#define DEMOPB_MAX_SNAP_GAP     10000
#define DEMOPB_INDEX_BUDGET_MS  3
#define DEMOPB_FF_BATCH         24
#define DEMOPB_RATE_STEP        0.25f
#define DEMOPB_RATE_MAX         4.0f
#define DEMOPB_MENU_ID          "demo_overlay"

static cvar_t *ui_om_demo_overlay_default;

//=============================================================================
// Timeline index (background walk on its own file handle)
//=============================================================================

struct demoPbIndex_t {
	std::string              path;
	int                      protocol;
	fileHandle_t             file;
	qboolean                 running;
	qboolean                 done;
	int                      clientNum;
	qboolean                 gotGamestate;
	int                      lastCmdSeq;
	qboolean                 haveTime;
	int                      lastServerTime;
	int                      timeline;
	int                      pendingFrags;
	std::vector<std::string> configstrings;
	std::string              bigConfigstring;
	std::vector<int>         fragTimes;
	unsigned                 revision;
};

static demoPbIndex_t s_index;
static byte          s_indexData[MAX_MSGLEN];

typedef enum {
	EXPORT_IDLE,
	EXPORT_WAIT_START,
	EXPORT_WRITING
} demoPbExportPhase_t;

//=============================================================================
// Live playback state
//=============================================================================

static struct {
	char     path[MAX_OSPATH];
	char     arg[MAX_OSPATH];
	int      protocol;

	// timeline of the messages read so far
	int      lastSnapMessageNum;
	qboolean haveTime;
	int      lastServerTime;
	int      timeline;

	qboolean paused;
	qboolean atEnd;
	float    rate;
	float    rateAccum;

	qboolean seeking;
	int      seekTargetMs;

	// set when a rewind restarts the demo; consumed by OnDemoStart
	qboolean restartPending;
	char     restartPath[MAX_OSPATH];

	// bit offsets in the message being parsed (-1 = not present)
	int      bits[DEMOBITS_END + 1];

	demoPbExportPhase_t exportPhase;
	fileHandle_t        exportFile;
	char                exportName[MAX_QPATH];
	int                 exportStartMs;
	int                 exportEndMs;
	int                 exportFirstMsgNum;
	int                 exportMessages;
	int                 exportResumeMs;
} s_pb;

static void DP_SetVar(const char *name, const char *value)
{
	const char *cur = CL_UIVar_String(name);
	if (!cur || strcmp(cur, value)) {
		CL_UIVar_Set(name, value);
	}
}

static void DP_FormatTime(int ms, char *out, size_t outSize)
{
	if (ms < 0) {
		ms = 0;
	}
	Com_sprintf(out, (int)outSize, "%d:%02d", ms / 60000, (ms / 1000) % 60);
}

static int DP_ProtocolFromPath(const char *path)
{
	const char *ext = strrchr(path, '.');

	if (ext && !Q_stricmpn(ext, ".dm_", 4) && ext[4]) {
		return atoi(ext + 4);
	}
	return com_protocol ? com_protocol->integer : 0;
}

static void DP_NormalizeName(const char *in, char *out, size_t outSize)
{
	size_t n = 0;

	for (; in && *in && n + 1 < outSize; in++) {
		const unsigned char c = (unsigned char)*in;
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) {
			out[n++] = (char)c;
		} else if (c >= 'A' && c <= 'Z') {
			out[n++] = (char)(c - 'A' + 'a');
		}
	}
	out[n] = 0;
}

//=============================================================================
// Index
//=============================================================================

static void DP_IndexClose(void)
{
	if (s_index.file) {
		FS_FCloseFile(s_index.file);
		s_index.file = 0;
	}
	s_index.running = qfalse;
}

static void DP_IndexReset(void)
{
	DP_IndexClose();
	s_index.path.clear();
	s_index.protocol = 0;
	s_index.done = qfalse;
	s_index.clientNum = -1;
	s_index.gotGamestate = qfalse;
	s_index.lastCmdSeq = 0;
	s_index.haveTime = qfalse;
	s_index.lastServerTime = 0;
	s_index.timeline = 0;
	s_index.pendingFrags = 0;
	s_index.configstrings.clear();
	s_index.bigConfigstring.clear();
	s_index.fragTimes.clear();
	s_index.revision++;
}

static const char *DP_IndexConfigstring(int index)
{
	if (index < 0 || index >= (int)s_index.configstrings.size()) {
		return "";
	}
	return s_index.configstrings[index].c_str();
}

static void DP_IndexSetConfigstring(int index, const char *value)
{
	if (index >= 0 && index < (int)s_index.configstrings.size()) {
		s_index.configstrings[index] = value ? value : "";
	}
}

static const char *DP_IndexPlayerInfo(int clientNum)
{
	if (clientNum < 0 || clientNum >= MAX_CLIENTS) {
		return "";
	}
	return DP_IndexConfigstring(CS_PLAYERS + clientNum);
}

// Returns the team of the first player whose name matches, or -1
static int DP_IndexTeamByName(const char *normalizedName)
{
	char name[MAX_NAME_LENGTH * 2];
	int  i;

	for (i = 0; i < MAX_CLIENTS; i++) {
		const char *info = DP_IndexPlayerInfo(i);
		if (!info[0]) {
			continue;
		}
		DP_NormalizeName(Info_ValueForKey(info, "name"), name, sizeof(name));
		if (name[0] && !strcmp(name, normalizedName)) {
			return atoi(Info_ValueForKey(info, "team"));
		}
	}
	return -1;
}

static void DP_IndexHandleDeath(const char *attacker, const char *victim, char typeChar, qboolean teamKnown, qboolean teamkill)
{
	char self[MAX_NAME_LENGTH * 2];
	char att[MAX_NAME_LENGTH * 2];
	char vic[MAX_NAME_LENGTH * 2];

	if (typeChar != 'p' && typeChar != 'P') {
		return;
	}
	DP_NormalizeName(Info_ValueForKey(DP_IndexPlayerInfo(s_index.clientNum), "name"), self, sizeof(self));
	DP_NormalizeName(attacker, att, sizeof(att));
	DP_NormalizeName(victim, vic, sizeof(vic));
	if (!self[0] || strcmp(att, self) || !strcmp(vic, self)) {
		return;
	}

	if (!teamKnown) {
		// Base protocol: compare teams from the player configstrings
		const int gametype = atoi(Info_ValueForKey(DP_IndexConfigstring(CS_SERVERINFO), "g_gametype"));
		const int myTeam = atoi(Info_ValueForKey(DP_IndexPlayerInfo(s_index.clientNum), "team"));
		const int vicTeam = DP_IndexTeamByName(vic);

		teamkill = (gametype > GT_FFA && (myTeam == TEAM_ALLIES || myTeam == TEAM_AXIS) && vicTeam == myTeam)
				 ? qtrue
				 : qfalse;
	}
	if (teamkill) {
		return;
	}
	s_index.pendingFrags++;
}

static void DP_IndexHandleCommand(const char *text)
{
	std::vector<std::string> args;
	const char              *cmd;

	if (CL_DemoWalk_Tokenize(text, &args) < 1) {
		return;
	}
	cmd = args[0].c_str();

	if (!strcmp(cmd, "cs")) {
		if (args.size() > 1) {
			DP_IndexSetConfigstring(
				CL_DemoWalk_NormalizeConfigstring(s_index.protocol, atoi(args[1].c_str())),
				args.size() > 2 ? args[2].c_str() : ""
			);
		}
	} else if (!strcmp(cmd, "bcs0")) {
		s_index.bigConfigstring = args.size() > 2 ? args[2] : "";
	} else if (!strcmp(cmd, "bcs1")) {
		s_index.bigConfigstring += args.size() > 2 ? args[2] : "";
	} else if (!strcmp(cmd, "bcs2")) {
		s_index.bigConfigstring += args.size() > 2 ? args[2] : "";
		if (args.size() > 1) {
			DP_IndexSetConfigstring(
				CL_DemoWalk_NormalizeConfigstring(s_index.protocol, atoi(args[1].c_str())),
				s_index.bigConfigstring.c_str()
			);
		}
		s_index.bigConfigstring.clear();
	} else if (!strcmp(cmd, "printdeathmsg")) {
		// Uppercase type: the recipient is not on the victim's team
		if (args.size() > 5 && args[5].size()) {
			const char type = args[5][0];
			DP_IndexHandleDeath(args[3].c_str(), args[4].c_str(), type, qtrue, (type >= 'a' && type <= 'z') ? qtrue : qfalse);
		}
	} else if (!strcmp(cmd, "print") && s_index.protocol < PROTOCOL_MOHTA_MIN) {
		const char *msg = args.size() > 1 ? args[1].c_str() : "";
		char        victim[128];
		char        attacker[128];
		char        s1[128];
		char        s2[256];
		char        typeChar = 0;

		if (msg[0] != MESSAGE_CHAT_RED && msg[0] != MESSAGE_CHAT_GREEN) {
			return;
		}
		if (CL_KillFeed_ParseDeathPrint(
				msg + 1, victim, sizeof(victim), attacker, sizeof(attacker), s1, sizeof(s1), s2, sizeof(s2), &typeChar
			)) {
			DP_IndexHandleDeath(attacker, victim, typeChar, qfalse, qfalse);
		}
	}
}

static qboolean DP_IndexOnGamestate(void *ctx, int serverCommandSequence)
{
	// A second gamestate is a map change; the timeline covers the first map only
	if (s_index.gotGamestate) {
		return qfalse;
	}
	s_index.gotGamestate = qtrue;
	s_index.lastCmdSeq = serverCommandSequence;
	return qtrue;
}

static void DP_IndexOnConfigstring(void *ctx, int index, const char *value)
{
	DP_IndexSetConfigstring(index, value);
}

static void DP_IndexOnServerCommand(void *ctx, int seq, const char *text)
{
	if (seq > s_index.lastCmdSeq) {
		s_index.lastCmdSeq = seq;
		DP_IndexHandleCommand(text);
	}
}

static void DP_IndexOnSnapshot(void *ctx, int serverTime, int deltaNum)
{
	if (s_index.haveTime) {
		const int delta = serverTime - s_index.lastServerTime;
		if (delta > 0 && delta < DEMOPB_MAX_SNAP_GAP) {
			s_index.timeline += delta;
		}
	}
	s_index.haveTime = qtrue;
	s_index.lastServerTime = serverTime;
}

static void DP_IndexFinish(void)
{
	DP_IndexClose();
	s_index.done = qtrue;
	s_index.revision++;
}

// Returns qfalse when the walk ended
static qboolean DP_IndexStep(void)
{
	demoWalkHandlers_t h;
	int                header[2];
	int                len;
	msg_t              msg;
	qboolean           more;

	if (FS_Read(header, sizeof(header), s_index.file) != sizeof(header)) {
		return qfalse;
	}
	len = LittleLong(header[1]);
	if (len < 0 || len > MAX_MSGLEN) {
		return qfalse;
	}
	MSG_Init(&msg, s_indexData, sizeof(s_indexData));
	if (FS_Read(s_indexData, len, s_index.file) != (size_t)len) {
		return qfalse;
	}
	msg.cursize = len;
	msg.readcount = 0;

	memset(&h, 0, sizeof(h));
	h.onGamestate = DP_IndexOnGamestate;
	h.onConfigstring = DP_IndexOnConfigstring;
	h.onServerCommand = DP_IndexOnServerCommand;
	h.onSnapshot = DP_IndexOnSnapshot;

	s_index.pendingFrags = 0;
	more = CL_DemoWalk_ParseMessage(s_index.protocol, &msg, &h);
	// Kills arrive alongside the snapshot that shows them
	while (s_index.pendingFrags > 0) {
		s_index.fragTimes.push_back(s_index.timeline);
		s_index.pendingFrags--;
		s_index.revision++;
	}
	return more;
}

static void DP_IndexStart(const char *path)
{
	DP_IndexReset();
	s_index.path = path;
	s_index.protocol = DP_ProtocolFromPath(path);
	s_index.configstrings.resize(MAX_CONFIGSTRINGS);
	FS_FOpenFileRead(path, &s_index.file, qtrue, qtrue);
	if (!s_index.file) {
		DP_IndexFinish();
		return;
	}
	s_index.running = qtrue;
}

static void DP_IndexThink(void)
{
	int start;

	if (!s_index.running) {
		return;
	}
	// Need the recording player's client number from the live gamestate
	if (s_index.clientNum < 0) {
		if (clc.state < CA_PRIMED) {
			return;
		}
		s_index.clientNum = clc.clientNum;
	}

	start = Sys_Milliseconds();
	do {
		if (!DP_IndexStep()) {
			DP_IndexFinish();
			return;
		}
	} while (Sys_Milliseconds() - start < DEMOPB_INDEX_BUDGET_MS);
}

static int DP_DurationMs(void)
{
	if (s_index.done) {
		return s_index.timeline > s_pb.timeline ? s_index.timeline : s_pb.timeline;
	}
	return 0;
}

//=============================================================================
// Live playback
//=============================================================================

static qboolean DP_OverlayAvailable(void)
{
	return CL_UIR_UseLegacyMain() ? qfalse : qtrue;
}

static int DP_CurrentMs(void)
{
	int pos;

	if (!s_pb.haveTime) {
		return 0;
	}
	pos = s_pb.timeline;
	if (cl.snap.serverTime > cl.serverTime) {
		const int behind = cl.snap.serverTime - cl.serverTime;
		if (behind < DEMOPB_MAX_SNAP_GAP) {
			pos -= behind;
		}
	}
	return pos < 0 ? 0 : pos;
}

static void DP_ResetTimeline(void)
{
	s_pb.lastSnapMessageNum = -1;
	s_pb.haveTime = qfalse;
	s_pb.lastServerTime = 0;
	s_pb.timeline = 0;
	s_pb.atEnd = qfalse;
	s_pb.rateAccum = 0;
}

static void DP_SetStatus(const char *text)
{
	DP_SetVar("ui_om_demo_export_status", text);
}

static void DP_CloseExportFile(qboolean writeTrailer)
{
	if (!s_pb.exportFile) {
		return;
	}
	if (writeTrailer) {
		int len = -1;
		FS_Write(&len, 4, s_pb.exportFile);
		FS_Write(&len, 4, s_pb.exportFile);
	}
	FS_FCloseFile(s_pb.exportFile);
	s_pb.exportFile = 0;
}

static void DP_AbortExport(const char *reason)
{
	if (s_pb.exportPhase == EXPORT_IDLE) {
		return;
	}
	DP_CloseExportFile(qtrue);
	s_pb.exportPhase = EXPORT_IDLE;
	DP_SetStatus(reason);
}

static void DP_Seek(int targetMs);

static void DP_FinishExport(void)
{
	char status[MAX_QPATH + 32];

	DP_CloseExportFile(qtrue);
	s_pb.exportPhase = EXPORT_IDLE;
	Com_Printf("Demo crop written to %s (%d messages)\n", s_pb.exportName, s_pb.exportMessages);
	Com_sprintf(status, sizeof(status), "Saved %s", s_pb.exportName);
	DP_SetStatus(status);
	CL_DemoScan_Refresh();

	// Return to where the user was, paused
	s_pb.paused = qtrue;
	DP_Seek(s_pb.exportResumeMs);
}

static void DP_WriteFramed(int sequence, const byte *data, int size)
{
	int v;

	v = LittleLong(sequence);
	FS_Write(&v, 4, s_pb.exportFile);
	v = LittleLong(size);
	FS_Write(&v, 4, s_pb.exportFile);
	FS_Write(data, size, s_pb.exportFile);
	s_pb.exportMessages++;
}

// Copies raw bits [from, to) of a bitstream message
static void DP_CopyBits(msg_t *dst, const byte *data, int size, int from, int to)
{
	msg_t in;
	int   b;

	if (from < 0 || to <= from) {
		return;
	}
	MSG_Init(&in, (byte *)data, size);
	in.cursize = size;
	MSG_Bitstream(&in);
	in.bit = from;
	in.readcount = (from >> 3) + 1;
	for (b = from; b < to; b++) {
		MSG_WriteBits(dst, MSG_ReadBits(&in, 1), 1);
	}
}

// Writes cl.snap as an uncompressed svc_snapshot
static void DP_WriteKeyframe(msg_t *msg)
{
	const clSnapshot_t *snap = &cl.snap;
	int                 i;

	MSG_WriteByte(msg, svc_snapshot);
	MSG_WriteLong(msg, snap->serverTime);
	MSG_WriteByte(msg, snap->serverTimeResidual);
	MSG_WriteByte(msg, 0);
	MSG_WriteByte(msg, snap->snapFlags);
	MSG_WriteByte(msg, sizeof(snap->areamask));
	MSG_WriteData(msg, snap->areamask, sizeof(snap->areamask));

	MSG_WriteDeltaPlayerstate(msg, NULL, (playerState_t *)&snap->ps, cls.serverFrameTime);

	for (i = 0; i < snap->numEntities; i++) {
		entityState_t *ent = &cl.parseEntities[(snap->parseEntitiesNum + i) & (MAX_PARSE_ENTITIES - 1)];
		MSG_WriteDeltaEntity(msg, &cl.entityBaselines[ent->number], ent, qtrue, cls.serverFrameTime);
	}
	MSG_WriteEntityNum(msg, MAX_GENTITIES - 1);

	MSG_WriteSounds(msg, (server_sound_t *)snap->sounds, snap->number_of_sounds);
}

// Rewrites the message just parsed with a keyframe snapshot; keepCommands
// controls whether the server commands before the snapshot are kept
static void DP_WriteRekeyed(const byte *data, int size, qboolean keepCommands)
{
	static byte out[MAX_MSGLEN];
	msg_t       msg;

	MSG_Init(&msg, out, sizeof(out));
	MSG_Bitstream(&msg);
	MSG_WriteLong(&msg, clc.reliableSequence);
	if (keepCommands) {
		DP_CopyBits(&msg, data, size, s_pb.bits[DEMOBITS_COMMANDS], s_pb.bits[DEMOBITS_SNAPSHOT_START]);
	}
	DP_WriteKeyframe(&msg);
	if (s_pb.bits[DEMOBITS_END] >= 0) {
		DP_CopyBits(&msg, data, size, s_pb.bits[DEMOBITS_SNAPSHOT_END], s_pb.bits[DEMOBITS_END]);
	} else {
		MSG_WriteByte(&msg, svc_EOF);
	}
	if (msg.overflowed) {
		DP_AbortExport("Export failed: message overflow");
		return;
	}
	DP_WriteFramed(clc.serverMessageSequence, msg.data, (int)msg.cursize);
}

static qboolean DP_MessageHasValidSnapshot(void)
{
	return s_pb.bits[DEMOBITS_SNAPSHOT_START] >= 0 && s_pb.bits[DEMOBITS_SNAPSHOT_END] >= 0 && cl.snap.valid
		&& cl.snap.messageNum == clc.serverMessageSequence;
}

static void DP_BeginExportWriting(const byte *data, int size)
{
	s_pb.exportFile = FS_FOpenFileWrite_HomeData(s_pb.exportName);
	if (!s_pb.exportFile) {
		DP_AbortExport("Export failed: could not open file");
		return;
	}
	s_pb.exportPhase = EXPORT_WRITING;
	s_pb.exportFirstMsgNum = clc.serverMessageSequence;
	s_pb.exportMessages = 0;

	CL_WriteDemoGamestate(s_pb.exportFile, clc.serverMessageSequence - 1);
	// The gamestate already reflects this message's commands
	DP_WriteRekeyed(data, size, qfalse);
	DP_SetStatus("Exporting...");
}

static void DP_ExportMessage(const byte *data, int size)
{
	if (s_pb.bits[DEMOBITS_GAMESTATE] >= 0) {
		// map change inside the range: stop at the end of the first map
		if (s_pb.exportPhase == EXPORT_WRITING) {
			DP_FinishExport();
		}
		return;
	}

	if (s_pb.exportPhase == EXPORT_WAIT_START) {
		if (s_pb.timeline >= s_pb.exportStartMs && DP_MessageHasValidSnapshot()) {
			DP_BeginExportWriting(data, size);
		}
		return;
	}

	if (DP_MessageHasValidSnapshot() && cl.snap.deltaNum > 0 && cl.snap.deltaNum < s_pb.exportFirstMsgNum) {
		DP_WriteRekeyed(data, size, qtrue);
	} else {
		DP_WriteFramed(clc.serverMessageSequence, data, size);
	}

	if (s_pb.exportPhase == EXPORT_WRITING && s_pb.timeline >= s_pb.exportEndMs) {
		DP_FinishExport();
	}
}

static void DP_Restart(int targetMs)
{
	s_pb.restartPending = qtrue;
	Q_strncpyz(s_pb.restartPath, s_pb.path, sizeof(s_pb.restartPath));
	s_pb.seekTargetMs = targetMs;
	s_pb.seeking = targetMs > 0 ? qtrue : qfalse;
	Cbuf_AddText(va("demo \"%s\"\n", s_pb.arg));
}

static void DP_Seek(int targetMs)
{
	if (!clc.demoplaying) {
		return;
	}
	if (targetMs < 0) {
		targetMs = 0;
	}
	if (s_pb.atEnd || targetMs < DP_CurrentMs()) {
		DP_Restart(targetMs);
		return;
	}
	s_pb.seekTargetMs = targetMs;
	s_pb.seeking = s_pb.timeline < targetMs ? qtrue : qfalse;
}

//=============================================================================
// Engine hooks
//=============================================================================

void CL_DemoPlayback_OnDemoStart(const char *path, const char *arg)
{
	const qboolean restart = s_pb.restartPending && !Q_stricmp(path, s_pb.restartPath);

	s_pb.restartPending = qfalse;
	Q_strncpyz(s_pb.path, path, sizeof(s_pb.path));
	Q_strncpyz(s_pb.arg, arg, sizeof(s_pb.arg));
	s_pb.protocol = DP_ProtocolFromPath(path);
	DP_ResetTimeline();

	if (restart) {
		s_pb.seeking = s_pb.seekTargetMs > 0 ? qtrue : qfalse;
	} else {
		DP_AbortExport("");
		s_pb.paused = qfalse;
		s_pb.rate = 1.0f;
		s_pb.seeking = qfalse;
		s_pb.seekTargetMs = 0;
		CL_UIVar_Set("ui_om_demo_crop_start", "0");
		CL_UIVar_Set("ui_om_demo_crop_end", "1");
		DP_SetStatus("");
		if (CL_AVI_DemoExportActive()) {
			CL_UIVar_Set("ui_om_demo_overlay", "0");
		} else {
			CL_UIVar_Set(
				"ui_om_demo_overlay", ui_om_demo_overlay_default && ui_om_demo_overlay_default->integer ? "1" : "0"
			);
		}
	}

	if (s_index.path != path) {
		DP_IndexStart(path);
	}
}

void CL_DemoPlayback_BeforeMessage(void)
{
	int i;

	for (i = 0; i <= DEMOBITS_END; i++) {
		s_pb.bits[i] = -1;
	}
}

void CL_DemoPlayback_NoteBits(int which, int bit)
{
	if (which >= 0 && which <= DEMOBITS_END) {
		s_pb.bits[which] = bit;
	}
}

void CL_DemoPlayback_AfterMessage(const byte *data, int size)
{
	if (cl.snap.valid && cl.snap.messageNum != s_pb.lastSnapMessageNum) {
		s_pb.lastSnapMessageNum = cl.snap.messageNum;
		if (s_pb.haveTime) {
			const int delta = cl.snap.serverTime - s_pb.lastServerTime;
			if (delta > 0 && delta < DEMOPB_MAX_SNAP_GAP) {
				s_pb.timeline += delta;
			}
		}
		s_pb.haveTime = qtrue;
		s_pb.lastServerTime = cl.snap.serverTime;
	}

	if (s_pb.exportPhase != EXPORT_IDLE && !s_pb.restartPending) {
		DP_ExportMessage(data, size);
	}

	if (s_pb.seeking && !s_pb.restartPending && s_pb.timeline >= s_pb.seekTargetMs) {
		s_pb.seeking = qfalse;
	}
}

qboolean CL_DemoPlayback_HoldAtEnd(void)
{
	if (clc.state != CA_ACTIVE || (cl_timedemo && cl_timedemo->integer)) {
		return qfalse;
	}
	if (CL_AVI_DemoExportActive() || CL_VideoRecording()) {
		return qfalse;
	}
	if (Cvar_VariableString("nextdemo")[0]) {
		return qfalse;
	}
	if (!DP_OverlayAvailable()) {
		return qfalse;
	}

	if (!s_pb.atEnd) {
		s_pb.atEnd = qtrue;
		s_pb.seeking = qfalse;
		s_pb.paused = qtrue;
		if (s_pb.exportPhase == EXPORT_WRITING) {
			DP_FinishExport();
		} else if (s_pb.exportPhase == EXPORT_WAIT_START) {
			DP_AbortExport("Export failed: range is past the end");
		}
		CL_UIVar_Set("ui_om_demo_overlay", "1");
	}
	return qtrue;
}

qboolean CL_DemoPlayback_AtEnd(void)
{
	return clc.demoplaying && s_pb.atEnd;
}

void CL_DemoPlayback_AdjustTimeDelta(void)
{
	if (s_pb.paused || s_pb.atEnd) {
		cl.serverTimeDelta -= cls.frametime;
		return;
	}
	if (s_pb.rate != 1.0f) {
		int step;

		s_pb.rateAccum += (s_pb.rate - 1.0f) * cls.frametime;
		step = (int)s_pb.rateAccum;
		s_pb.rateAccum -= step;
		cl.serverTimeDelta += step;
	}
}

qboolean CL_DemoPlayback_FastForwarding(void)
{
	if (!clc.demoplaying || s_pb.atEnd || s_pb.restartPending) {
		return qfalse;
	}
	return s_pb.seeking || s_pb.exportPhase != EXPORT_IDLE ? qtrue : qfalse;
}

int CL_DemoPlayback_FastForwardBatch(void)
{
	return DEMOPB_FF_BATCH;
}

qboolean CL_DemoPlayback_CanReadAhead(void)
{
	const int gap = clc.serverCommandSequence - clc.lastExecutedServerCommand;

	// keep cgame's reliable command ring from cycling out
	if (gap < MAX_RELIABLE_COMMANDS / 2) {
		return qtrue;
	}

	/* Fixed in Omaha: after a rewind restart, PlayDemo can parse far more
	 * reliable commands than the ring can hold before cgame drains them.
	 * Skip past already-lost commands so seek fast-forward is not wedged. */
	if (gap >= MAX_RELIABLE_COMMANDS) {
		clc.lastExecutedServerCommand = clc.serverCommandSequence - (MAX_RELIABLE_COMMANDS / 2) + 1;
		return qtrue;
	}

	return qfalse;
}

qboolean CL_DemoPlayback_MuteEventSounds(void)
{
	return CL_DemoPlayback_FastForwarding();
}

qboolean CL_DemoPlayback_MuteLoopingSounds(void)
{
	if (!clc.demoplaying) {
		return qfalse;
	}
	return CL_DemoPlayback_FastForwarding() || s_pb.paused || s_pb.atEnd ? qtrue : qfalse;
}

static void DP_PublishVars(void)
{
	char      buf[64];
	char      a[16];
	char      b[16];
	const int duration = DP_DurationMs();
	const int cur = DP_CurrentMs();

	DP_SetVar("ui_om_demo_indexed", s_index.done ? "1" : "0");
	DP_SetVar("ui_om_demo_paused", (s_pb.paused || s_pb.atEnd) ? "1" : "0");
	DP_SetVar("ui_om_demo_seeking", s_pb.seeking ? "1" : "0");
	DP_SetVar("ui_om_demo_exporting", s_pb.exportPhase != EXPORT_IDLE ? "1" : "0");

	Com_sprintf(buf, sizeof(buf), "%.2fx", s_pb.rate);
	DP_SetVar("ui_om_demo_rate", buf);

	DP_FormatTime(cur, buf, sizeof(buf));
	DP_SetVar("ui_om_demo_time", buf);

	if (duration > 0) {
		float cropStart = CL_UIVar_Value("ui_om_demo_crop_start");
		float cropEnd = CL_UIVar_Value("ui_om_demo_crop_end");

		DP_FormatTime(duration, buf, sizeof(buf));
		DP_SetVar("ui_om_demo_length", buf);

		// The playhead follows playback; while the user drags it the slider
		// holds its own staged value
		if (!s_pb.seeking && s_pb.exportPhase == EXPORT_IDLE) {
			const float pos = cur >= duration ? 1.0f : (float)cur / (float)duration;
			Com_sprintf(buf, sizeof(buf), "%.4f", pos);
			DP_SetVar("ui_om_demo_pos", buf);
		}

		DP_FormatTime((int)(cropStart * duration), a, sizeof(a));
		DP_FormatTime((int)(cropEnd * duration), b, sizeof(b));
		Com_sprintf(buf, sizeof(buf), "%s - %s", a, b);
		DP_SetVar("ui_om_demo_crop_label", buf);
	} else {
		DP_SetVar("ui_om_demo_length", "--:--");
		DP_SetVar("ui_om_demo_crop_label", "Indexing...");
	}
}

static void DP_SyncOverlayMenu(void)
{
	qboolean want;
	const qboolean open = CL_UIMenu_IsOpen(DEMOPB_MENU_ID);

	/* Added in Omaha: AVI export must not paint the playback overlay. */
	if (CL_AVI_DemoExportActive() || CL_VideoRecording()) {
		if (CL_UIVar_Integer("ui_om_demo_overlay")) {
			CL_UIVar_Set("ui_om_demo_overlay", "0");
		}
		if (open) {
			CL_UIMenu_Close(DEMOPB_MENU_ID);
		}
		return;
	}

	want = clc.demoplaying && clc.state == CA_ACTIVE && DP_OverlayAvailable()
		&& CL_UIVar_Integer("ui_om_demo_overlay");

	if (want && !open) {
		CL_UIMenu_Open(DEMOPB_MENU_ID, qtrue);
	} else if (!want && open) {
		CL_UIMenu_Close(DEMOPB_MENU_ID);
	}
}

void CL_DemoPlayback_Frame(void)
{
	if (!clc.demoplaying) {
		if (!s_pb.restartPending) {
			DP_AbortExport("Export cancelled");
			s_pb.seeking = qfalse;
			s_pb.atEnd = qfalse;
		}
		if (CL_UIMenu_IsOpen(DEMOPB_MENU_ID)) {
			CL_UIMenu_Close(DEMOPB_MENU_ID);
		}
		return;
	}

	DP_IndexThink();
	DP_PublishVars();
	DP_SyncOverlayMenu();
}

//=============================================================================
// UI actions
//=============================================================================

void CL_DemoPlayback_TogglePause(void)
{
	if (!clc.demoplaying || s_pb.exportPhase != EXPORT_IDLE) {
		return;
	}
	if (s_pb.atEnd) {
		s_pb.paused = qfalse;
		DP_Restart(0);
		return;
	}
	s_pb.paused = !s_pb.paused;
}

void CL_DemoPlayback_RateReset(void)
{
	s_pb.rate = 1.0f;
	s_pb.rateAccum = 0;
}

void CL_DemoPlayback_RateStep(void)
{
	s_pb.rate += DEMOPB_RATE_STEP;
	if (s_pb.rate > DEMOPB_RATE_MAX) {
		s_pb.rate = DEMOPB_RATE_MAX;
	}
}

void CL_DemoPlayback_SeekToUIPosition(void)
{
	const int duration = DP_DurationMs();
	float     pos;

	if (!clc.demoplaying || duration <= 0 || s_pb.exportPhase != EXPORT_IDLE) {
		return;
	}
	pos = CL_UIVar_Value("ui_om_demo_pos");
	if (pos < 0) {
		pos = 0;
	} else if (pos > 1) {
		pos = 1;
	}
	DP_Seek((int)(pos * duration));
}

static qboolean DP_PickExportName(char *out, size_t outSize)
{
	char        base[MAX_OSPATH];
	const char *file;
	const char *ext;
	char       *dot;
	int         n;

	file = s_pb.path;
	if (!Q_stricmpn(file, "demos/", 6)) {
		file += 6;
	}
	Q_strncpyz(base, file, sizeof(base));
	ext = strrchr(s_pb.path, '.');
	dot = strrchr(base, '.');
	if (dot) {
		*dot = 0;
	}
	if (!ext) {
		ext = va(".dm_%d", s_pb.protocol);
	}

	for (n = 1; n <= 99; n++) {
		Com_sprintf(out, (int)outSize, "demos/%s_crop_%02d%s", base, n, ext);
		if (!FS_FileExists_HomeData(out)) {
			return qtrue;
		}
	}
	return qfalse;
}

void CL_DemoPlayback_Export(void)
{
	const int duration = DP_DurationMs();
	float     cropStart;
	float     cropEnd;

	if (!clc.demoplaying || s_pb.exportPhase != EXPORT_IDLE) {
		return;
	}
	if (duration <= 0) {
		DP_SetStatus("Still indexing the demo");
		return;
	}

	cropStart = CL_UIVar_Value("ui_om_demo_crop_start");
	cropEnd = CL_UIVar_Value("ui_om_demo_crop_end");
	if (cropStart < 0) {
		cropStart = 0;
	}
	if (cropEnd > 1) {
		cropEnd = 1;
	}
	if (cropEnd <= cropStart) {
		DP_SetStatus("Export failed: empty range");
		return;
	}
	if (!DP_PickExportName(s_pb.exportName, sizeof(s_pb.exportName))) {
		DP_SetStatus("Export failed: too many crops of this demo");
		return;
	}

	s_pb.exportStartMs = (int)(cropStart * duration);
	s_pb.exportEndMs = cropEnd >= 1.0f ? duration + DEMOPB_MAX_SNAP_GAP : (int)(cropEnd * duration);
	s_pb.exportResumeMs = DP_CurrentMs();
	s_pb.exportPhase = EXPORT_WAIT_START;
	DP_SetStatus("Exporting...");

	if (s_pb.atEnd || s_pb.exportStartMs < s_pb.timeline) {
		DP_Restart(s_pb.exportStartMs);
	} else {
		s_pb.seekTargetMs = s_pb.exportStartMs;
		s_pb.seeking = qtrue;
	}
}

void CL_DemoPlayback_ToggleOverlay(void)
{
	CL_UIVar_Set("ui_om_demo_overlay", CL_UIVar_Integer("ui_om_demo_overlay") ? "0" : "1");
}

int CL_DemoPlayback_FragCount(void)
{
	return DP_DurationMs() > 0 ? (int)s_index.fragTimes.size() : 0;
}

float CL_DemoPlayback_FragPos(int index)
{
	const int duration = DP_DurationMs();

	if (index < 0 || index >= (int)s_index.fragTimes.size() || duration <= 0) {
		return 0;
	}
	return (float)s_index.fragTimes[index] / (float)duration;
}

unsigned CL_DemoPlayback_FragRevision(void)
{
	return s_index.revision;
}

//=============================================================================

static void CL_DemoOverlay_f(void)
{
	CL_DemoPlayback_ToggleOverlay();
}

static void CL_DemoPause_f(void)
{
	CL_DemoPlayback_TogglePause();
}

void CL_DemoPlayback_Init(void)
{
	memset(&s_pb, 0, sizeof(s_pb));
	s_pb.rate = 1.0f;
	s_pb.lastSnapMessageNum = -1;
	DP_IndexReset();

	ui_om_demo_overlay_default = Cvar_Get("ui_om_demo_overlay_default", "1", CVAR_ARCHIVE);
	Cmd_AddCommand("demooverlay", CL_DemoOverlay_f);
	Cmd_AddCommand("demopause", CL_DemoPause_f);
}

void CL_DemoPlayback_Shutdown(void)
{
	DP_AbortExport("");
	DP_IndexReset();
	Cmd_RemoveCommand("demooverlay");
	Cmd_RemoveCommand("demopause");
}
