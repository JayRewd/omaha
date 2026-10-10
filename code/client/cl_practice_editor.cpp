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

// In-game practice scenario editor (pause panels "practice" / "practice_nodes").
// Edits a local draft; Save forwards practice_meta / practice_order to the listen
// server, which owns the scenario and writes the file.

#include "client.h"
#include "cl_practice.h"
#include "cl_uivars.h"
#include "cl_uirender.h"
#include "../uidesign/uid_backend.h"
#include "../qcommon/practice_scenario.hpp"

#include <string>
#include <vector>

typedef struct {
	char        key[16];
	char        id[16];
	char        type[16];
	char        label[16];
	char        detail[48];
	char        moved[4];
	char        first[4];
	char        last[4];
	const char *fieldNames[7];
	const char *fieldValues[7];
} practice_node_slot_t;

static PracticeScenario s_draft;
static std::vector<int> s_order;    // original node ids of non-start nodes, draft order
static int              s_firstMovable;
static int              s_movedId = -1;
static qboolean         s_loaded;
static qboolean         s_saved;
static uint64_t         s_revision;

static qboolean CL_PracticeEditor_Available(void)
{
	return com_sv_running && com_sv_running->integer && Cvar_VariableIntegerValue("g_practice_edit")
		&& clc.state == CA_ACTIVE;
}

static qboolean CL_PracticeEditor_OrderDirty(void)
{
	size_t i;

	for (i = 0; i < s_order.size(); i++) {
		if (s_order[i] != (int)i + s_firstMovable) {
			return qtrue;
		}
	}
	return qfalse;
}

static qboolean CL_PracticeEditor_MetaDirty(void)
{
	return strcmp(CL_UIVar_String("ui_om_practice_edit_name"), s_draft.name)
			|| strcmp(CL_UIVar_String("ui_om_practice_edit_author"), s_draft.author)
			|| CL_UIVar_Integer("ui_om_practice_edit_maxalive") != s_draft.maxAlive
		? qtrue
		: qfalse;
}

static void CL_PracticeEditor_UpdateStatus(void)
{
	const qboolean dirty = CL_PracticeEditor_OrderDirty() || CL_PracticeEditor_MetaDirty();
	char           status[96];

	if (!s_loaded) {
		CL_UIVar_Set("ui_om_practice_edit_status", "NO SCENARIO LOADED");
		CL_UIVar_Set("ui_om_practice_edit_dirty", "0");
		return;
	}
	if (dirty) {
		s_saved = qfalse;
	}
	Com_sprintf(
		status,
		sizeof(status),
		"%i ENEMIES / %i WAYPOINTS%s",
		(int)s_draft.spots.size(),
		(int)s_draft.waypoints.size(),
		dirty ? " - UNSAVED CHANGES" : (s_saved ? " - SAVED" : "")
	);
	CL_UIVar_Set("ui_om_practice_edit_status", status);
	CL_UIVar_Set("ui_om_practice_edit_dirty", dirty ? "1" : "0");
}

static void CL_PracticeEditor_ResetOrder(void)
{
	int i;

	s_order.clear();
	s_firstMovable = (!s_draft.nodes.empty() && s_draft.nodes[0].type == PRACTICE_NODE_START) ? 1 : 0;
	for (i = s_firstMovable; i < (int)s_draft.nodes.size(); i++) {
		s_order.push_back(i);
	}
	CL_UIVar_Set("ui_om_practice_edit_count", va("%i", (int)s_order.size()));
	s_revision++;
}

static void CL_PracticeEditor_PublishMeta(void)
{
	CL_UIVar_Set("ui_om_practice_edit_name", s_draft.name);
	CL_UIVar_Set("ui_om_practice_edit_author", s_draft.author);
	CL_UIVar_Set("ui_om_practice_edit_maxalive", va("%i", s_draft.maxAlive));
}

static qboolean CL_PracticeEditor_Load(void)
{
	const char *file = Cvar_VariableString("g_practice_file");
	char       *buf  = NULL;
	char        err[256];
	long        len;

	s_draft.Clear();
	s_loaded  = qfalse;
	s_saved   = qfalse;
	s_movedId = -1;

	if (file && file[0]) {
		len = FS_ReadFile(file, (void **)&buf);
		if (len > 0 && buf) {
			if (PracticeScenario_Parse(buf, &s_draft, err, sizeof(err))) {
				s_loaded = qtrue;
			} else {
				Com_Printf("Practice editor: parse error in %s: %s\n", file, err);
			}
			FS_FreeFile(buf);
		} else {
			Com_Printf("Practice editor: could not read %s\n", file);
		}
	}

	CL_PracticeEditor_ResetOrder();
	CL_PracticeEditor_PublishMeta();
	CL_PracticeEditor_UpdateStatus();
	return s_loaded;
}

static void CL_PracticeEditor_Sanitize(const char *in, char *out, size_t outSize)
{
	// Changed in Omaha: shared meta sanitizer (controls, quotes, %).
	Practice_SanitizeMetaField(in, out, outSize);
}

static void CL_PracticeEditor_Open_f(void)
{
	if (!CL_PracticeEditor_Available()) {
		Com_Printf("Practice editor: only available while editing a practice scenario\n");
		return;
	}
	if (!CL_UIR_UseModernHudPack()) {
		Com_Printf("Practice editor: requires a modern HUD pack (ui_om_hud)\n");
		return;
	}
	CL_PracticeEditor_Load();
	if (CL_UIR_IsDmPauseOpen()) {
		CL_UIVar_Set("ui_om_pause_panel", "practice");
	} else {
		CL_UIR_OpenDmPause("practice");
	}
}

static void CL_PracticeEditor_Move_f(void)
{
	int row;
	int delta;
	int target;
	int tmp;

	if (Cmd_Argc() < 3) {
		Com_Printf("usage: practice_editor_move <row> <delta>\n");
		return;
	}
	row    = atoi(Cmd_Argv(1));
	delta  = atoi(Cmd_Argv(2));
	target = row + delta;
	if (row < 0 || row >= (int)s_order.size() || target < 0 || target >= (int)s_order.size() || !delta) {
		return;
	}
	tmp            = s_order[row];
	s_order[row]   = s_order[target];
	s_order[target] = tmp;
	s_movedId      = tmp;
	s_revision++;
	CL_PracticeEditor_UpdateStatus();
}

static void CL_PracticeEditor_Save_f(void)
{
	char        name[PRACTICE_META_MAX_CHARS + 1];
	char        author[PRACTICE_META_MAX_CHARS + 1];
	int         maxAlive;
	size_t      i;
	qboolean    orderDirty;
	qboolean    orderOk = qtrue;

	if (!s_loaded || !CL_PracticeEditor_Available()) {
		Com_Printf("Practice editor: nothing to save\n");
		return;
	}

	CL_PracticeEditor_Sanitize(CL_UIVar_String("ui_om_practice_edit_name"), name, sizeof(name));
	CL_PracticeEditor_Sanitize(CL_UIVar_String("ui_om_practice_edit_author"), author, sizeof(author));
	if (!name[0]) {
		Q_strncpyz(name, s_draft.name, sizeof(name));
	}
	maxAlive = CL_UIVar_Integer("ui_om_practice_edit_maxalive");
	if (maxAlive < 1) {
		maxAlive = 1;
	} else if (maxAlive > 32) {
		maxAlive = 32;
	}

	CL_AddReliableCommand(va("practice_meta \"%s\" \"%s\" %i", name, author, maxAlive), qfalse);

	orderDirty = CL_PracticeEditor_OrderDirty();
	if (orderDirty) {
		const size_t count = s_order.size();
		// Leave headroom under MAX_STRING_CHARS for the command name and spaces.
		static const size_t kMaxAppendPayload = MAX_STRING_CHARS - 64;
		std::string         chunk;
		size_t              chunkStart = 0;

		if ((int)count > PRACTICE_MAX_NODES) {
			Com_Printf("Practice editor: too many nodes to reorder\n");
			orderOk = qfalse;
		} else {
			CL_AddReliableCommand(va("practice_order_begin %i", (int)count), qfalse);
			chunk.reserve(256);
			for (i = 0; i < count; i++) {
				char idBuf[16];
				size_t need;

				Com_sprintf(idBuf, sizeof(idBuf), "%i", s_order[i]);
				need = strlen(idBuf) + (chunk.empty() ? 0 : 1);
				if (!chunk.empty() && chunk.size() + need > kMaxAppendPayload) {
					CL_AddReliableCommand(va("practice_order_append %s", chunk.c_str()), qfalse);
					chunk.clear();
					chunkStart = i;
					(void)chunkStart;
				}
				if (!chunk.empty()) {
					chunk += ' ';
				}
				chunk += idBuf;
			}
			if (!chunk.empty()) {
				CL_AddReliableCommand(va("practice_order_append %s", chunk.c_str()), qfalse);
			}
			CL_AddReliableCommand("practice_order_commit", qfalse);
		}
	}

	// Mirror the server so the list shows the renumbered ids without a disk round-trip.
	Q_strncpyz(s_draft.name, name, sizeof(s_draft.name));
	Q_strncpyz(s_draft.author, author, sizeof(s_draft.author));
	s_draft.maxAlive = maxAlive;
	if (orderDirty && orderOk) {
		for (i = 0; i < s_order.size(); i++) {
			if (s_order[i] == s_movedId) {
				s_movedId = (int)i + s_firstMovable;
				break;
			}
		}
		if (!s_draft.ApplyNodeOrder(s_order)) {
			Com_Printf("Practice editor: local order apply failed\n");
			orderOk = qfalse;
		}
	}
	if (orderDirty && !orderOk) {
		CL_PracticeEditor_UpdateStatus();
		return;
	}
	CL_PracticeEditor_ResetOrder();
	CL_PracticeEditor_PublishMeta();
	s_saved = qtrue;
	CL_PracticeEditor_UpdateStatus();
}

static void CL_PracticeEditor_Revert_f(void)
{
	CL_PracticeEditor_Load();
}

void CL_PracticeEditor_Frame(void)
{
	static int lastCheck;

	if (!s_loaded || cls.realtime - lastCheck < 100) {
		return;
	}
	lastCheck = cls.realtime;
	CL_PracticeEditor_UpdateStatus();
}

void CL_PracticeEditor_Init(void)
{
	Cmd_AddCommand("practice_editor", CL_PracticeEditor_Open_f);
	Cmd_AddCommand("practice_editor_move", CL_PracticeEditor_Move_f);
	Cmd_AddCommand("practice_editor_save", CL_PracticeEditor_Save_f);
	Cmd_AddCommand("practice_editor_revert", CL_PracticeEditor_Revert_f);
}

int CL_PracticeEditor_QueryNodes(
	int offset, int limit, int *outTotal, uint64_t *outRevision, uid_collection_item_t *out, int max
)
{
	static std::vector<practice_node_slot_t> slots;
	static const char *const                 kNames[] = {"id", "type", "label", "detail", "moved", "first", "last"};
	const int                                count    = (int)s_order.size();
	int                                      written  = 0;
	int                                      i, f;

	if (outTotal) {
		*outTotal = count;
	}
	if (outRevision) {
		*outRevision = s_revision;
	}
	if (!out || max <= 0) {
		return 0;
	}
	if (offset < 0) {
		offset = 0;
	}
	if (limit <= 0) {
		limit = max;
	}
	if ((int)slots.size() < max) {
		slots.resize(max);
	}

	for (i = offset; i < count && written < max && written < limit; i++) {
		const int id = s_order[i];
		practice_node_slot_t *slot;
		qboolean              isWaypoint;

		if (id < 0 || id >= (int)s_draft.nodes.size()) {
			continue;
		}
		const PracticeNode &node = s_draft.nodes[id];
		slot                     = &slots[written];
		isWaypoint               = node.type == PRACTICE_NODE_WAYPOINT ? qtrue : qfalse;

		Com_sprintf(slot->key, sizeof(slot->key), "%i", id);
		Q_strncpyz(slot->id, slot->key, sizeof(slot->id));
		Q_strncpyz(slot->type, isWaypoint ? "waypoint" : "enemy", sizeof(slot->type));
		Q_strncpyz(slot->label, isWaypoint ? "WAYPOINT" : "ENEMY", sizeof(slot->label));
		if (isWaypoint) {
			slot->detail[0] = 0;
		} else {
			Com_sprintf(
				slot->detail,
				sizeof(slot->detail),
				"%s%s",
				node.stance == PRACTICE_STANCE_CROUCH ? "CROUCH" : "STAND",
				node.lean == PRACTICE_LEAN_LEFT ? "  LEAN LEFT" : (node.lean == PRACTICE_LEAN_RIGHT ? "  LEAN RIGHT" : "")
			);
		}
		Q_strncpyz(slot->moved, id == s_movedId ? "1" : "0", sizeof(slot->moved));
		Q_strncpyz(slot->first, i == 0 ? "1" : "0", sizeof(slot->first));
		Q_strncpyz(slot->last, i == count - 1 ? "1" : "0", sizeof(slot->last));

		for (f = 0; f < 7; f++) {
			slot->fieldNames[f] = kNames[f];
		}
		slot->fieldValues[0] = slot->id;
		slot->fieldValues[1] = slot->type;
		slot->fieldValues[2] = slot->label;
		slot->fieldValues[3] = slot->detail;
		slot->fieldValues[4] = slot->moved;
		slot->fieldValues[5] = slot->first;
		slot->fieldValues[6] = slot->last;

		out[written].key         = slot->key;
		out[written].value       = slot->key;
		out[written].label       = slot->label;
		out[written].nfields     = 7;
		out[written].fieldNames  = slot->fieldNames;
		out[written].fieldValues = slot->fieldValues;
		out[written].flags       = 0;
		written++;
	}
	return written;
}
