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

#include "client.h"
#include "cl_ghostspec.h"
#include "cl_uivars.h"

/*
 * Changed in Omaha: keep weapon suppress through delayed spectator (servers can
 * take many seconds) plus a short window after deferred join_team.
 */
static const int GHOSTSPEC_TRANSITION_MS    = 25000;
static const int GHOSTSPEC_POST_JOIN_ARM_MS = 5000;
/*
 * Changed in Omaha: wait up to 25s for server to put us in spectate after
 * join (variable respawn/spec delay), then cycle for another 20s.
 */
static const int GHOSTSPEC_AWAIT_SPECTATE_MS = 25000;
static const int GHOSTSPEC_SEEK_MS           = 20000;
/* Changed in Omaha: clean usercmd edges — press 1 cmd, force release, wait for snap. */
static const int GHOSTSPEC_SEEK_RELEASE_CMDS = 6;
static const int GHOSTSPEC_SEEK_WAIT_MS      = 350;
static const int GHOSTSPEC_SEEK_MAX_PULSES   = 64;
static const int GHOSTSPEC_SEEK_MAX_STUCK    = 10;

typedef enum {
	GHOSTSPEC_PULSE_IDLE = 0,
	GHOSTSPEC_PULSE_PRESS,
	GHOSTSPEC_PULSE_RELEASE,
	GHOSTSPEC_PULSE_WAIT
} ghostspecPulsePhase_t;

typedef enum {
	GHOSTSPEC_CYCLE_JUMP = 0, /* TA+: +moveup next */
	GHOSTSPEC_CYCLE_USE      /* base MOH: USE next; TA+ free-cam start */
} ghostspecCycleKind_t;

static int                   s_transitionArmUntil = 0;
static int                   s_pendingJoinTeam    = TEAM_NONE; /* deferred join after spectator sticks */
static int                   s_seekClient         = -1;
static int                   s_seekAwaitUntil     = 0; /* deadline while waiting for PMF_SPECTATING */
static int                   s_seekUntil          = 0; /* cycle deadline after spectating starts */
static int                   s_seekPulses         = 0;
static int                   s_seekStuck          = 0;
static qboolean              s_seekSawSpectating  = qfalse;
static ghostspecPulsePhase_t s_pulsePhase         = GHOSTSPEC_PULSE_IDLE;
static ghostspecCycleKind_t  s_cycleKind          = GHOSTSPEC_CYCLE_JUMP;
static int                   s_releaseLeft        = 0;
static int                   s_pulseWaitUntil     = 0;
static int                   s_infoAtPulse        = -1;
static cvar_t               *s_suppressWeaponSelect;

static qboolean GhostSpec_IsTaProtocol(void)
{
	return (com_protocol && com_protocol->integer >= PROTOCOL_MOHTA_MIN) ? qtrue : qfalse;
}

static qboolean GhostSpec_IsSpectating(void)
{
	if (clc.state != CA_ACTIVE || !cl.snap.valid) {
		return qfalse;
	}
	return (cl.snap.ps.pm_flags & PMF_SPECTATING) ? qtrue : qfalse;
}

static qboolean GhostSpec_SnapIsGhost(void)
{
	int team;

	if (!GhostSpec_IsSpectating()) {
		return qfalse;
	}
	team = cl.snap.ps.stats[STAT_TEAM];
	return (team == TEAM_ALLIES || team == TEAM_AXIS) ? qtrue : qfalse;
}

qboolean CL_GhostSpec_IsActive(void)
{
	return GhostSpec_SnapIsGhost();
}

static int GhostSpec_LocalTeam(void)
{
	if (clc.state != CA_ACTIVE || !cl.snap.valid) {
		return TEAM_NONE;
	}
	return cl.snap.ps.stats[STAT_TEAM];
}

static int GhostSpec_InfoClient(void)
{
	if (clc.state != CA_ACTIVE || !cl.snap.valid) {
		return -1;
	}
	return cl.snap.ps.stats[STAT_INFOCLIENT];
}

static void GhostSpec_ClearSeek(void)
{
	s_seekClient        = -1;
	s_seekAwaitUntil    = 0;
	s_seekUntil         = 0;
	s_seekPulses        = 0;
	s_seekStuck         = 0;
	s_seekSawSpectating = qfalse;
	s_pulsePhase        = GHOSTSPEC_PULSE_IDLE;
	s_cycleKind         = GHOSTSPEC_CYCLE_JUMP;
	s_releaseLeft       = 0;
	s_pulseWaitUntil    = 0;
	s_infoAtPulse       = -1;
}

static void GhostSpec_StartSeek(int clientNum)
{
	if (clientNum < 0 || clientNum >= MAX_CLIENTS) {
		GhostSpec_ClearSeek();
		return;
	}
	s_seekClient        = clientNum;
	s_seekPulses        = 0;
	s_seekStuck         = 0;
	s_pulsePhase        = GHOSTSPEC_PULSE_IDLE;
	s_cycleKind         = GHOSTSPEC_CYCLE_JUMP;
	s_releaseLeft       = 0;
	s_pulseWaitUntil    = 0;
	s_infoAtPulse       = GhostSpec_InfoClient();
	s_seekSawSpectating = GhostSpec_IsSpectating();
	if (s_seekSawSpectating) {
		s_seekAwaitUntil = 0;
		s_seekUntil      = cls.realtime + GHOSTSPEC_SEEK_MS;
	} else {
		/* Do not burn cycle budget until the server actually puts us in spec. */
		s_seekAwaitUntil = cls.realtime + GHOSTSPEC_AWAIT_SPECTATE_MS;
		s_seekUntil      = 0;
	}
}

/*
===============
CL_GhostSpec_AdjustMove

Changed in Omaha: protocol-aware spectate cycle inject.
- TA+: jump edge cycles; never hold upmove across frames.
- Base MOH: USE cycles; upmove CLEARS chase (must stay 0 or we ping-pong).
===============
*/
void CL_GhostSpec_AdjustMove(usercmd_t *cmd)
{
	if (!cmd || s_seekClient < 0) {
		return;
	}

	/*
	 * Fixed in Omaha: base protocol treats any upmove as "leave chase". Force 0
	 * for the whole seek except a single TA+ jump press frame.
	 */
	if (s_pulsePhase != GHOSTSPEC_PULSE_PRESS || s_cycleKind != GHOSTSPEC_CYCLE_JUMP) {
		cmd->upmove = 0;
	}

	if (s_pulsePhase == GHOSTSPEC_PULSE_PRESS) {
		if (s_cycleKind == GHOSTSPEC_CYCLE_USE) {
			cmd->buttons |= BUTTON_USE;
			cmd->upmove = 0;
		} else {
			cmd->upmove = 127;
		}
		s_pulsePhase  = GHOSTSPEC_PULSE_RELEASE;
		s_releaseLeft = GHOSTSPEC_SEEK_RELEASE_CMDS;
		s_infoAtPulse = GhostSpec_InfoClient();
		s_seekPulses++;
		return;
	}

	if (s_pulsePhase == GHOSTSPEC_PULSE_RELEASE) {
		cmd->upmove = 0;
		cmd->buttons &= ~BUTTON_USE;
		if (--s_releaseLeft <= 0) {
			s_pulsePhase     = GHOSTSPEC_PULSE_WAIT;
			s_pulseWaitUntil = cls.realtime + GHOSTSPEC_SEEK_WAIT_MS;
		}
		return;
	}

	if (s_pulsePhase == GHOSTSPEC_PULSE_WAIT) {
		cmd->upmove = 0;
		cmd->buttons &= ~BUTTON_USE;
	}
}

static void GhostSpec_ArmTransition(int ms)
{
	const int until = cls.realtime + ms;
	if (until > s_transitionArmUntil) {
		s_transitionArmUntil = until;
	}
}

static void GhostSpec_ClearTransition(void)
{
	s_transitionArmUntil = 0;
	s_pendingJoinTeam    = TEAM_NONE;
}

/*
===============
GhostSpec_FlushPendingJoin

Fixed in Omaha: join_team must run only after PMF_SPECTATING is observed.
A fixed wait fired while still alive (noop), then spectator landed later and
left the client on TEAM_SPECTATOR.
===============
*/
static void GhostSpec_FlushPendingJoin(void)
{
	int joinTeam;

	if (s_pendingJoinTeam == TEAM_NONE) {
		return;
	}
	if (!GhostSpec_IsSpectating()) {
		return;
	}

	joinTeam          = s_pendingJoinTeam;
	s_pendingJoinTeam = TEAM_NONE;

	if (joinTeam == TEAM_ALLIES) {
		Cbuf_AddText("join_team allies\n");
	} else if (joinTeam == TEAM_AXIS) {
		Cbuf_AddText("join_team axis\n");
	} else {
		Cbuf_AddText("auto_join_team\n");
	}

	/* Keep suppress through the join→ghost settle. */
	GhostSpec_ArmTransition(GHOSTSPEC_POST_JOIN_ARM_MS);
}

static qboolean GhostSpec_TransitionArmed(void)
{
	return (s_transitionArmUntil > 0 && cls.realtime < s_transitionArmUntil) ? qtrue : qfalse;
}

static void GhostSpec_Leave(void)
{
	GhostSpec_ClearTransition();
	GhostSpec_ClearSeek();
	Cbuf_AddText("spectator\n");
}

/*
===============
GhostSpec_EnterFromAlive

Changed in Omaha: only for players currently in the game (not spectating).
Issue spectator now; defer join_team until spectating is observed so the
stock no-primary ghost join can stick on the original team.
===============
*/
static void GhostSpec_EnterFromAlive(int followClient)
{
	const int localTeam = GhostSpec_LocalTeam();

	GhostSpec_ArmTransition(GHOSTSPEC_TRANSITION_MS);
	if (followClient >= 0) {
		GhostSpec_StartSeek(followClient);
	} else {
		GhostSpec_ClearSeek();
	}

	if (localTeam == TEAM_ALLIES || localTeam == TEAM_AXIS) {
		s_pendingJoinTeam = localTeam;
	} else {
		/* FFA / unknown → auto_join_team once spectating. */
		s_pendingJoinTeam = TEAM_FREEFORALL;
	}

	Cbuf_AddText("spectator\n");
}

/*
===============
GhostSpec_SeekFrame

Changed in Omaha: cycle via usercmd inject (AdjustMove). Wait for STAT_INFOCLIENT
to change after each edge before pulsing again.
===============
*/
static void GhostSpec_SeekFrame(void)
{
	int infoClient;

	if (s_seekClient < 0) {
		return;
	}

	if (clc.state != CA_ACTIVE || !cl.snap.valid) {
		GhostSpec_ClearSeek();
		return;
	}

	if (!GhostSpec_IsSpectating()) {
		if (s_seekAwaitUntil > 0 && cls.realtime >= s_seekAwaitUntil) {
			GhostSpec_ClearSeek();
		}
		return;
	}

	if (!s_seekSawSpectating) {
		s_seekSawSpectating = qtrue;
		s_seekAwaitUntil    = 0;
		s_seekUntil         = cls.realtime + GHOSTSPEC_SEEK_MS;
		s_seekPulses        = 0;
		s_pulsePhase        = GHOSTSPEC_PULSE_IDLE;
	}

	if (cls.realtime >= s_seekUntil || s_seekPulses >= GHOSTSPEC_SEEK_MAX_PULSES
	    || s_seekStuck >= GHOSTSPEC_SEEK_MAX_STUCK) {
		GhostSpec_ClearSeek();
		return;
	}

	infoClient = GhostSpec_InfoClient();
	if ((cl.snap.ps.pm_flags & PMF_CAMERA_VIEW) && infoClient == s_seekClient) {
		GhostSpec_ClearSeek();
		return;
	}

	/* Mid press/release: AdjustMove owns the edge; do not queue another. */
	if (s_pulsePhase == GHOSTSPEC_PULSE_PRESS || s_pulsePhase == GHOSTSPEC_PULSE_RELEASE) {
		return;
	}

	if (s_pulsePhase == GHOSTSPEC_PULSE_WAIT) {
		if (infoClient != s_infoAtPulse && infoClient >= 0) {
			s_infoAtPulse = infoClient;
			s_seekStuck   = 0;
			if ((cl.snap.ps.pm_flags & PMF_CAMERA_VIEW) && infoClient == s_seekClient) {
				GhostSpec_ClearSeek();
				return;
			}
			/* Landed on someone else — settle then pulse again. */
			s_pulseWaitUntil = cls.realtime + (GHOSTSPEC_SEEK_WAIT_MS / 2);
		}
		if (cls.realtime < s_pulseWaitUntil) {
			return;
		}
		if (infoClient == s_infoAtPulse) {
			s_seekStuck++;
		}
		s_pulsePhase = GHOSTSPEC_PULSE_IDLE;
	}

	/*
	 * Choose cycle control:
	 * - Base MOH: always USE (jump clears chase and caused 0↔1 ping-pong).
	 * - TA+ free-cam: USE once to start chase.
	 * - TA+ chasing: jump to next.
	 */
	if (!GhostSpec_IsTaProtocol()) {
		s_cycleKind = GHOSTSPEC_CYCLE_USE;
	} else if (!(cl.snap.ps.pm_flags & PMF_CAMERA_VIEW)) {
		s_cycleKind = GHOSTSPEC_CYCLE_USE;
	} else {
		s_cycleKind = GHOSTSPEC_CYCLE_JUMP;
	}

	s_pulsePhase = GHOSTSPEC_PULSE_PRESS;
}

/*
===============
CL_GhostSpec_f

Usage: om_ghostspec allies|axis|auto [clientNum]
Scoreboard always supplies clientNum. Keybind may omit it (enter/leave only).

Changed in Omaha:
- Already spectating (any team) + clientNum → jump-cycle seek only (no team cmds).
- Alive in game + clientNum → ghost enter on YOUR team, then seek.
- Weapon suppress only during that alive→ghost transition.
===============
*/
static void CL_GhostSpec_f(void)
{
	const char *teamArg;
	int         followClient = -1;
	qboolean    ghosted;
	qboolean    spectating;

	if (clc.state != CA_ACTIVE || !cl.snap.valid) {
		Com_Printf("om_ghostspec: not in a game\n");
		return;
	}
	if (cl.snap.ps.pm_flags & PMF_INTERMISSION) {
		return;
	}

	if (Cmd_Argc() < 2) {
		Com_Printf("usage: om_ghostspec allies|axis|auto [clientNum]\n");
		return;
	}

	teamArg = Cmd_Argv(1);
	if (Q_stricmp(teamArg, "allies") && Q_stricmp(teamArg, "axis") && Q_stricmp(teamArg, "auto")) {
		Com_Printf("usage: om_ghostspec allies|axis|auto [clientNum]\n");
		return;
	}

	if (Cmd_Argc() >= 3) {
		followClient = atoi(Cmd_Argv(2));
		if (followClient < 0 || followClient >= MAX_CLIENTS) {
			Com_Printf("om_ghostspec: invalid clientNum\n");
			return;
		}
	}

	ghosted    = GhostSpec_SnapIsGhost();
	spectating = GhostSpec_IsSpectating();

	/* Leave: ghosted + no target (keybind) or dblclick own row. */
	if (followClient < 0 && ghosted) {
		GhostSpec_Leave();
		return;
	}
	if (followClient >= 0 && followClient == cl.snap.ps.clientNum && (ghosted || spectating)) {
		GhostSpec_Leave();
		return;
	}

	/*
	 * Already spectating (pure spectator OR ghost): never touch teams — only
	 * jump-cycle to the requested player.
	 */
	if (spectating && followClient >= 0) {
		GhostSpec_StartSeek(followClient);
		return;
	}

	/* Alive in game: ghost-enter on current team, then optional seek. */
	if (!spectating) {
		GhostSpec_EnterFromAlive(followClient);
		return;
	}

	/* Spectating, no clientNum: nothing to do (avoid accidental join). */
}

void CL_GhostSpec_Frame(void)
{
	qboolean    ghosted;
	qboolean    armed;
	qboolean    wantSuppress;
	const char *ghostUi;

	if (!s_suppressWeaponSelect) {
		s_suppressWeaponSelect = Cvar_Get("cg_om_suppressWeaponSelect", "0", 0);
	}

	ghosted = GhostSpec_SnapIsGhost();
	armed   = GhostSpec_TransitionArmed();

	/* Fixed in Omaha: join only after spectator state is visible on the snap. */
	GhostSpec_FlushPendingJoin();
	armed = GhostSpec_TransitionArmed();

	if (s_transitionArmUntil > 0 && !armed) {
		s_transitionArmUntil = 0;
		s_pendingJoinTeam    = TEAM_NONE;
	}

	/*
	 * Changed in Omaha: suppress weapon-select ONLY during alive→ghost enter
	 * arm (includes deferred-join wait). Not for pure spectate swap/seek.
	 */
	wantSuppress = (armed || s_pendingJoinTeam != TEAM_NONE) ? qtrue : qfalse;
	if (s_suppressWeaponSelect->integer != (wantSuppress ? 1 : 0)) {
		Cvar_Set("cg_om_suppressWeaponSelect", wantSuppress ? "1" : "0");
	}

	ghostUi = ghosted ? "1" : "0";
	if (CL_UIVar_Integer("ui_om_ghostspec") != (ghosted ? 1 : 0)) {
		CL_UIVar_Set("ui_om_ghostspec", ghostUi);
	}

	GhostSpec_SeekFrame();
}

void CL_GhostSpec_Init(void)
{
	s_suppressWeaponSelect = Cvar_Get("cg_om_suppressWeaponSelect", "0", 0);
	Cvar_Set("cg_om_suppressWeaponSelect", "0");
	GhostSpec_ClearSeek();
	GhostSpec_ClearTransition();
	CL_UIVar_Set("ui_om_ghostspec", "0");
	Cmd_AddCommand("om_ghostspec", CL_GhostSpec_f);
}

void CL_GhostSpec_Shutdown(void)
{
	Cmd_RemoveCommand("om_ghostspec");
	GhostSpec_ClearTransition();
	GhostSpec_ClearSeek();
}
