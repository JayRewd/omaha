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
// Client-only first-person chase spectate (eye, lean, zoom, followed hands).

#include "cg_local.h"

/* Matches bg_local.h bone controllers. */
#define SPECTATEFP_HEAD_TAG   0
#define SPECTATEFP_TORSO_TAG  1
#define SPECTATEFP_ARMS_TAG   2
#define SPECTATEFP_PELVIS_TAG 3
#define SPECTATEFP_ZOOM_FOV   45.0f
#define SPECTATEFP_LEAN_MAX   45.0f
/* Same height ease as CG_OffsetFirstPersonView. */
#define SPECTATEFP_HEIGHT_RATE   12.5f
#define SPECTATEFP_HEIGHT_MAXGAP 32.0f
#define SPECTATEFP_IDLE_WAIT     2000

cvar_t *cg_spectate_firstperson;
/* Added in Omaha: suppress stock weapon-select push while ghost-spectating. */
cvar_t *cg_om_suppressWeaponSelect;

static int   s_fpLastClient = -1;
static float s_fpViewZ      = 0.0f;
static qboolean s_fpViewZSet = qfalse;
static float s_fpBobPhase   = 0.0f;
static float s_fpBobAmp     = 0.0f;
static qboolean s_fpWalking   = qfalse;
static qboolean s_fpCrouching = qfalse;
static vec3_t   s_fpVelocity;

static int   s_fpWeaponEntity = -1;
static int   s_fpWeaponClass  = 0;
static char  s_fpWeaponName[64];
static int   s_fpAnim         = VM_ANIM_IDLE;
static int   s_fpAnimCount    = 1;
static int   s_fpIdleSince    = 0;
static qboolean s_fpPullout   = qfalse;
static int   s_fpArmsFrame    = -1;
static int   s_fpArmsClient   = -1;
static float s_fpBobSpeed     = 0.0f;
static float s_fpStepSpeed    = 0.0f;
static vec3_t s_fpEyeLast;
static qboolean s_fpEyeLastSet = qfalse;

static void CG_SpectateFP_UpdateWeapon(int followClient);

void CG_SpectateFP_RegisterCvars(void)
{
	/* Added in Omaha: toggle with `toggle cg_spectate_firstperson`. */
	cg_spectate_firstperson = cgi.Cvar_Get("cg_spectate_firstperson", "0", CVAR_ARCHIVE);
	/* Added in Omaha: client-driven weapon-menu suppress for ghost-spec. */
	cg_om_suppressWeaponSelect = cgi.Cvar_Get("cg_om_suppressWeaponSelect", "0", 0);
}

/*
===============
CG_GhostSpec_IsLocalGhost

Added in Omaha: spectating with Allies/Axis team (covert ghost-spec state).
===============
*/
qboolean CG_GhostSpec_IsLocalGhost(void)
{
	int team;

	if (!cg.snap) {
		return qfalse;
	}
	if (!(cg.snap->ps.pm_flags & PMF_SPECTATING)) {
		return qfalse;
	}
	team = cg.snap->ps.stats[STAT_TEAM];
	return (team == TEAM_ALLIES || team == TEAM_AXIS) ? qtrue : qfalse;
}

/*
===============
CG_GhostSpec_SuppressWeaponSelect

Added in Omaha: true when weapon-select menus must be ignored.
===============
*/
qboolean CG_GhostSpec_SuppressWeaponSelect(void)
{
	/* Changed in Omaha: only the client-armed enter transition; not while ghosted. */
	if (cg_om_suppressWeaponSelect && cg_om_suppressWeaponSelect->integer) {
		return qtrue;
	}
	return qfalse;
}

qboolean CG_SpectateFP_Wanted(void)
{
	return (cg_spectate_firstperson && cg_spectate_firstperson->integer) ? qtrue : qfalse;
}

qboolean CG_SpectateFP_Active(void)
{
	return cg.spectateFp.active;
}

int CG_SpectateFP_FollowClient(void)
{
	return cg.spectateFp.active ? cg.spectateFp.clientNum : -1;
}

qboolean CG_SpectateFP_InZoom(void)
{
	return (cg.spectateFp.active && cg.spectateFp.inZoom) ? qtrue : qfalse;
}

int CG_SpectateFP_ZoomFov(void)
{
	return cg.spectateFp.active ? cg.spectateFp.zoomFov : 0;
}

float CG_SpectateFP_LeanAngle(void)
{
	return cg.spectateFp.active ? cg.spectateFp.leanAngle : 0.0f;
}

static float CG_SpectateFP_LeanFromBones(const entityState_t *es)
{
	float lean;

	if (!es) {
		return 0.0f;
	}

	/* PmoveAdjustAngleSettings: pelvis roll = fLeanAngle * 0.8 */
	lean = es->bone_angles[SPECTATEFP_PELVIS_TAG][ROLL] / 0.8f;
	if (lean > SPECTATEFP_LEAN_MAX) {
		lean = SPECTATEFP_LEAN_MAX;
	} else if (lean < -SPECTATEFP_LEAN_MAX) {
		lean = -SPECTATEFP_LEAN_MAX;
	}
	return lean;
}

/*
===============
CG_SpectateFP_LeanTarget

Fixed in Omaha: interpolate lean between current/next entity states (same idea as
ps.fLeanAngle). The eye uses this directly, the way the player's own view uses
the current lean.
===============
*/
static float CG_SpectateFP_LeanTarget(centity_t *cent)
{
	float lean0;
	float lean1;
	float f;

	lean0 = CG_SpectateFP_LeanFromBones(&cent->currentState);
	if (!cent->interpolate || !cg.nextSnap) {
		return lean0;
	}

	lean1 = CG_SpectateFP_LeanFromBones(&cent->nextState);
	f     = cg.frameInterpolation;
	if (f < 0.0f) {
		f = 0.0f;
	} else if (f > 1.0f) {
		f = 1.0f;
	}
	return lean0 + (lean1 - lean0) * f;
}

static void CG_SpectateFP_ResetFollow(void)
{
	s_fpLastClient    = -1;
	s_fpViewZ         = 0.0f;
	s_fpViewZSet      = qfalse;
	s_fpBobAmp        = 0.0f;
	s_fpBobSpeed      = 0.0f;
	s_fpStepSpeed     = 0.0f;
	s_fpEyeLastSet    = qfalse;
	s_fpWeaponEntity  = -1;
	s_fpWeaponClass   = 0;
	s_fpWeaponName[0] = 0;
	s_fpAnim          = VM_ANIM_IDLE;
	s_fpIdleSince     = 0;
	s_fpPullout       = qfalse;
	s_fpArmsFrame     = -1;
	s_fpArmsClient    = -1;
	VectorClear(s_fpVelocity);
}

void CG_SpectateFP_Update(void)
{
	int        followClient;
	centity_t *followCent;
	float      fov;

	cg.spectateFp.active    = qfalse;
	cg.spectateFp.clientNum = -1;
	cg.spectateFp.leanAngle = 0.0f;
	cg.spectateFp.inZoom    = qfalse;
	cg.spectateFp.zoomFov   = 0;

	if (!CG_SpectateFP_Wanted() || !cg.snap) {
		CG_SpectateFP_ResetFollow();
		return;
	}

	if (!(cg.snap->ps.pm_flags & PMF_CAMERA_VIEW)) {
		CG_SpectateFP_ResetFollow();
		return;
	}

	followClient = cg.snap->ps.stats[STAT_INFOCLIENT];
	if (followClient < 0 || followClient >= MAX_CLIENTS) {
		CG_SpectateFP_ResetFollow();
		return;
	}

	followCent = &cg_entities[followClient];
	if (!followCent->currentValid || !followCent->currentState.modelindex) {
		CG_SpectateFP_ResetFollow();
		return;
	}

	if (followClient != s_fpLastClient) {
		CG_SpectateFP_ResetFollow();
		s_fpLastClient = followClient;
	}

	cg.spectateFp.active    = qtrue;
	cg.spectateFp.clientNum = followClient;
	cg.spectateFp.leanAngle = CG_SpectateFP_LeanTarget(followCent);

	fov = cg.snap->ps.fov;
	if (fov <= 0.0f) {
		fov = cg.camera_fov;
	}
	cg.spectateFp.inZoom  = (fov > 0.0f && fov <= SPECTATEFP_ZOOM_FOV) ? qtrue : qfalse;
	cg.spectateFp.zoomFov = cg.spectateFp.inZoom ? (int)(fov + 0.5f) : 0;
	if (cg.spectateFp.zoomFov < 1) {
		cg.spectateFp.zoomFov = 1;
	}

	CG_SpectateFP_UpdateWeapon(followClient);
}

/*
===============
CG_SpectateFP_SnapOrigin

Added in Omaha: server snapshot origin of the followed player, blended between
current/next snaps. Do not use lerpOrigin — with cg_smoothClients that pose
runs ahead by velocity for up to cg_smoothClientsTime, and remote prediction
pushes it further. Either path can put the spectate eye through a wall the
server never placed the player inside. Live play is unchanged.
===============
*/
static void CG_SpectateFP_SnapOrigin(const centity_t *cent, vec3_t out)
{
	float f;
	int   i;

	if (!cent->interpolate || !cg.nextSnap) {
		VectorCopy(cent->currentState.origin, out);
		return;
	}

	f = cg.frameInterpolation;
	if (f < 0.0f) {
		f = 0.0f;
	} else if (f > 1.0f) {
		f = 1.0f;
	}

	for (i = 0; i < 3; i++) {
		out[i] = cent->currentState.origin[i]
		         + f * (cent->nextState.origin[i] - cent->currentState.origin[i]);
	}
}

/*
===============
CG_SpectateFP_Motion

Added in Omaha: walk/crouch/speed for the spectate eye bob and hand sway.
Speed is the snapshot origin delta, not lerpOrigin (that pose runs ahead).
===============
*/
static void CG_SpectateFP_Motion(const centity_t *cent, const vec3_t bodyOrigin)
{
	vec3_t  mins, maxs;
	vec3_t  end;
	vec3_t  rawVel;
	trace_t tr;
	float   speed;
	int     dt;
	int     i;

	VectorClear(rawVel);
	if (cent->interpolate && cg.nextSnap) {
		dt = cg.nextSnap->serverTime - cg.snap->serverTime;
		if (dt > 0) {
			for (i = 0; i < 3; i++) {
				rawVel[i] = (cent->nextState.origin[i] - cent->currentState.origin[i]) * 1000.0f / (float)dt;
			}
		}
	}

	speed = VectorLength(rawVel);
	if (speed > 320.0f) {
		VectorScale(rawVel, 320.0f / speed, rawVel);
		speed = 320.0f;
	}
	/*
	 * Snapshot speed jumps between a walk and the 320 cap while the eye step stays
	 * steady. The gun's run offset is full above 250 and zero below 100, so those
	 * jumps slide the weapon. Ease the speed the gun uses.
	 */
	{
		float ease;
		float mag;

		ease = (float)cg.frametime / 1000.0f * 4.0f;
		if (ease > 1.0f) {
			ease = 1.0f;
		} else if (ease < 0.0f) {
			ease = 0.0f;
		}
		if (speed > 1.0f) {
			if (s_fpBobSpeed < 1.0f) {
				s_fpBobSpeed = speed;
			} else {
				s_fpBobSpeed += (speed - s_fpBobSpeed) * ease;
			}
			VectorScale(rawVel, s_fpBobSpeed / speed, s_fpVelocity);
		} else if (s_fpWalking && s_fpBobSpeed > 1.0f) {
			mag = VectorLength(s_fpVelocity);
			if (mag > 1.0f) {
				VectorScale(s_fpVelocity, s_fpBobSpeed / mag, s_fpVelocity);
			}
		} else {
			VectorCopy(rawVel, s_fpVelocity);
			s_fpBobSpeed += (0.0f - s_fpBobSpeed) * ease;
		}
	}

	VectorCopy(bodyOrigin, end);
	end[2] -= 4.0f;
	/* CM_BoxTrace reads mins/maxs with no null check. A point trace is a zero box. */
	CG_Trace(
		&tr, bodyOrigin, vec3_origin, vec3_origin, end, cent->currentState.number, MASK_PLAYERSOLID, qfalse, qfalse,
		"SpectateFP ground"
	);
	s_fpWalking = (tr.fraction < 1.0f) ? qtrue : qfalse;

	IntegerToBoundingBox(cent->currentState.solid, mins, maxs);
	s_fpCrouching = (maxs[2] > 0.0f && maxs[2] < 60.0f) ? qtrue : qfalse;
}

static void CG_SpectateFP_EaseHeight(float targetZ)
{
	float delta;
	float change;

	if (!s_fpViewZSet) {
		s_fpViewZ    = targetZ;
		s_fpViewZSet = qtrue;
		return;
	}

	delta = targetZ - s_fpViewZ;
	if (fabs(delta) < 0.1f) {
		s_fpViewZ = targetZ;
		return;
	}

	if (delta > SPECTATEFP_HEIGHT_MAXGAP) {
		delta     = SPECTATEFP_HEIGHT_MAXGAP;
		s_fpViewZ = targetZ - SPECTATEFP_HEIGHT_MAXGAP;
	} else if (delta < -SPECTATEFP_HEIGHT_MAXGAP) {
		delta     = -SPECTATEFP_HEIGHT_MAXGAP;
		s_fpViewZ = targetZ + SPECTATEFP_HEIGHT_MAXGAP;
	}

	change = (float)cg.frametime / 1000.0f * delta * SPECTATEFP_HEIGHT_RATE;
	if (!s_fpWalking) {
		change += change;
	}
	if (fabs(delta) < fabs(change)) {
		change = delta;
	}
	s_fpViewZ += change;
}

static void CG_SpectateFP_StepBob(vec3_t origin, const vec3_t left, float pitch, float lean)
{
	float vel;
	float phase;

	if (s_fpWalking) {
		float target;
		float ease;

		vel = s_fpStepSpeed;
		if (vel > 250.0f) {
			vel = 250.0f;
		}
		phase = vel * 0.0015f + 0.9f;
		s_fpBobPhase += ((float)cg.frametime / 1000.0f + (float)cg.frametime / 1000.0f) * (float)M_PI * phase;

		target = vel;
		if (lean != 0.0f) {
			target *= 0.75f;
		}
		target *= (1.0f - fabs(pitch) * (1.0f / 90.0f) * 0.5f) * 0.5f;
		ease = (float)cg.frametime / 1000.0f * 8.0f;
		if (ease > 1.0f) {
			ease = 1.0f;
		} else if (ease < 0.0f) {
			ease = 0.0f;
		}
		s_fpBobAmp += (target - s_fpBobAmp) * ease;
	} else if (s_fpBobAmp > 0.0f) {
		s_fpBobAmp -= ((float)cg.frametime / 1000.0f * s_fpBobAmp) + ((float)cg.frametime / 1000.0f * s_fpBobAmp);
		if (s_fpBobAmp < 0.1f) {
			s_fpBobAmp = 0.0f;
		}
	}

	if (s_fpBobAmp <= 0.0f) {
		return;
	}

	phase = sin(s_fpBobPhase) * s_fpBobAmp * 0.03f;
	if (phase > 16.0f) {
		phase = 16.0f;
	} else if (phase < -16.0f) {
		phase = -16.0f;
	}
	VectorMA(origin, phase, left, origin);

	phase = sin(s_fpBobPhase - 0.94f);
	phase = (fabs(phase) - 0.5f) * s_fpBobAmp * 0.06f;
	if (phase > 16.0f) {
		phase = 16.0f;
	} else if (phase < -16.0f) {
		phase = -16.0f;
	}
	origin[2] += phase;
}

/* Same layout as dtikicmd_t. cgame's dtiki anim header only forward-declares it. */
typedef struct {
	int    frame_num;
	int    num_args;
	char **args;
} spectateFpCmd_t;

static qboolean CG_SpectateFP_TikiArg(dtiki_t *tiki, const char *cmd, char *out, int outSize)
{
	int              i;
	spectateFpCmd_t *c;
	spectateFpCmd_t *cmds;

	if (!tiki || !tiki->a || !cmd || !out || outSize <= 0) {
		return qfalse;
	}

	cmds = (spectateFpCmd_t *)tiki->a->server_initcmds;
	for (i = 0; i < tiki->a->num_server_initcmds; i++) {
		c = &cmds[i];
		if (c->num_args >= 2 && c->args && c->args[0] && !Q_stricmp(c->args[0], cmd)) {
			Q_strncpyz(out, c->args[1], outSize);
			return qtrue;
		}
	}
	cmds = (spectateFpCmd_t *)tiki->a->client_initcmds;
	for (i = 0; i < tiki->a->num_client_initcmds; i++) {
		c = &cmds[i];
		if (c->num_args >= 2 && c->args && c->args[0] && !Q_stricmp(c->args[0], cmd)) {
			Q_strncpyz(out, c->args[1], outSize);
			return qtrue;
		}
	}
	return qfalse;
}

static int CG_SpectateFP_ClassFromName(const char *name)
{
	int i;
	static const struct {
		const char *name;
		int         cls;
	} map[] = {
		{ "Papers", WEAPON_CLASS_ITEM },
		{ "Packed MG42 Turret", WEAPON_CLASS_ITEM },
		{ "Colt 45", WEAPON_CLASS_PISTOL },
		{ "Walther P38", WEAPON_CLASS_PISTOL },
		{ "Hi-Standard Silenced", WEAPON_CLASS_PISTOL },
		{ "Webley Revolver", WEAPON_CLASS_PISTOL },
		{ "Nagant Revolver", WEAPON_CLASS_PISTOL },
		{ "Beretta", WEAPON_CLASS_PISTOL },
		{ "M1 Garand", WEAPON_CLASS_RIFLE },
		{ "Mauser KAR 98K", WEAPON_CLASS_RIFLE },
		{ "KAR98 - Sniper", WEAPON_CLASS_RIFLE },
		{ "Springfield '03 Sniper", WEAPON_CLASS_RIFLE },
		{ "Lee-Enfield", WEAPON_CLASS_RIFLE },
		{ "SVT 40", WEAPON_CLASS_RIFLE },
		{ "Mosin Nagant Rifle", WEAPON_CLASS_RIFLE },
		{ "G 43", WEAPON_CLASS_RIFLE },
		{ "Enfield L42A1", WEAPON_CLASS_RIFLE },
		{ "Carcano", WEAPON_CLASS_RIFLE },
		{ "DeLisle", WEAPON_CLASS_RIFLE },
		{ "Thompson", WEAPON_CLASS_SMG },
		{ "MP40", WEAPON_CLASS_SMG },
		{ "Sten Mark II", WEAPON_CLASS_SMG },
		{ "PPSH SMG", WEAPON_CLASS_SMG },
		{ "Moschetto", WEAPON_CLASS_SMG },
		{ "BAR", WEAPON_CLASS_MG },
		{ "StG 44", WEAPON_CLASS_MG },
		{ "FG 42", WEAPON_CLASS_MG },
		{ "Vickers-Berthier", WEAPON_CLASS_MG },
		{ "Breda", WEAPON_CLASS_MG },
		{ "Frag Grenade", WEAPON_CLASS_GRENADE },
		{ "Stielhandgranate", WEAPON_CLASS_GRENADE },
		{ "F1 Grenade", WEAPON_CLASS_GRENADE },
		{ "Mills Grenade", WEAPON_CLASS_GRENADE },
		{ "Nebelhandgranate", WEAPON_CLASS_GRENADE },
		{ "M18 Smoke Grenade", WEAPON_CLASS_GRENADE },
		{ "RDG-1 Smoke Grenade", WEAPON_CLASS_GRENADE },
		{ "Bomba A Mano", WEAPON_CLASS_GRENADE },
		{ "Bomba A Mano Breda", WEAPON_CLASS_GRENADE },
		{ "LandmineAllies", WEAPON_CLASS_GRENADE },
		{ "LandmineAxis", WEAPON_CLASS_GRENADE },
		{ "Minensuchgerat", WEAPON_CLASS_GRENADE },
		{ "Minedetector", WEAPON_CLASS_GRENADE },
		{ "Bazooka", WEAPON_CLASS_HEAVY },
		{ "Panzerschreck", WEAPON_CLASS_HEAVY },
		{ "Gewehrgranate", WEAPON_CLASS_HEAVY },
		{ "Shotgun", WEAPON_CLASS_HEAVY },
		{ "PIAT", WEAPON_CLASS_HEAVY },
	};

	if (!name || !name[0]) {
		return 0;
	}
	for (i = 0; i < (int)ARRAY_LEN(map); i++) {
		if (!Q_stricmp(name, map[i].name)) {
			return map[i].cls;
		}
	}
	return 0;
}

static int CG_SpectateFP_ClassFromType(const char *typeName)
{
	if (!typeName || !typeName[0]) {
		return 0;
	}
	if (!Q_stricmp(typeName, "pistol")) {
		return WEAPON_CLASS_PISTOL;
	}
	if (!Q_stricmp(typeName, "rifle")) {
		return WEAPON_CLASS_RIFLE;
	}
	if (!Q_stricmp(typeName, "smg")) {
		return WEAPON_CLASS_SMG;
	}
	if (!Q_stricmp(typeName, "mg")) {
		return WEAPON_CLASS_MG;
	}
	if (!Q_stricmp(typeName, "grenade")) {
		return WEAPON_CLASS_GRENADE;
	}
	if (!Q_stricmp(typeName, "heavy")) {
		return WEAPON_CLASS_HEAVY;
	}
	if (!Q_stricmp(typeName, "item") || !Q_stricmp(typeName, "item1") || !Q_stricmp(typeName, "item2")
	    || !Q_stricmp(typeName, "item3") || !Q_stricmp(typeName, "item4")) {
		return WEAPON_CLASS_ITEM;
	}
	return 0;
}

static int CG_SpectateFP_AnimFromName(const char *name)
{
	int         nlen;
	int         slen;
	int         i;
	const char *suffixes[] = {
		"fire_secondary", "reload_single", "reload_end", "rechamber", "pullout", "putaway",
		"ladderstep",     "charge",        "reload",    "fire",      "idle2",    "idle1",
		"idle0",          "idle",
	};
	const int anims[] = {
		VM_ANIM_FIRE_SECONDARY, VM_ANIM_RELOAD_SINGLE, VM_ANIM_RELOAD_END, VM_ANIM_RECHAMBER, VM_ANIM_PULLOUT,
		VM_ANIM_PUTAWAY,        VM_ANIM_LADDERSTEP,    VM_ANIM_CHARGE,     VM_ANIM_RELOAD,    VM_ANIM_FIRE,
		VM_ANIM_IDLE_2,         VM_ANIM_IDLE_1,        VM_ANIM_IDLE_0,     VM_ANIM_IDLE,
	};

	if (!name || !name[0]) {
		return VM_ANIM_IDLE;
	}

	nlen = (int)strlen(name);
	for (i = 0; i < (int)ARRAY_LEN(suffixes); i++) {
		slen = (int)strlen(suffixes[i]);
		if (!Q_stricmp(name, suffixes[i])) {
			return anims[i];
		}
		if (nlen > slen && name[nlen - slen - 1] == '_' && !Q_stricmp(name + nlen - slen, suffixes[i])) {
			return anims[i];
		}
	}
	return VM_ANIM_IDLE;
}

static centity_t *CG_SpectateFP_FindWeapon(int follow)
{
	int        i;
	centity_t *player;
	dtiki_t   *playerTiki;
	const char *tag;

	player = &cg_entities[follow];
	if (!player->currentState.modelindex) {
		return NULL;
	}
	playerTiki = cgi.R_Model_GetHandle(cgs.model_draw[player->currentState.modelindex]);
	if (!playerTiki) {
		return NULL;
	}

	for (i = MAX_CLIENTS; i < MAX_GENTITIES; i++) {
		centity_t *cent = &cg_entities[i];

		if (!cent->currentValid) {
			continue;
		}
		if (cent->currentState.parent != follow) {
			continue;
		}
		if (cent->currentState.renderfx & RF_DONTDRAW) {
			continue;
		}
		tag = cgi.Tag_NameForNum(playerTiki, cent->currentState.tag_num & TAG_MASK);
		if (tag && !Q_stricmp(tag, "tag_weapon_right")) {
			return cent;
		}
	}
	return NULL;
}

static qboolean CG_SpectateFP_HandsAnimDone(void)
{
	int     slot;
	int     index;
	float   len;
	dtiki_t *fps;

	fps = cg.pPlayerFPSModel;
	if (!fps) {
		return qtrue;
	}
	slot  = cgi.anim->g_iCurrentVMAnimSlot;
	index = cgi.anim->g_VMFrameInfo[slot].index;
	if (index < 0) {
		return qtrue;
	}
	len = cgi.Anim_Time(fps, index);
	if (len <= 0.0f) {
		return qtrue;
	}
	return cgi.anim->g_VMFrameInfo[slot].time >= len ? qtrue : qfalse;
}

static void CG_SpectateFP_SetAnim(int anim)
{
	if (anim == s_fpAnim) {
		return;
	}
	s_fpAnim = anim;
	s_fpAnimCount++;
}

static void CG_SpectateFP_ReadWeaponInfo(centity_t *weap)
{
	dtiki_t *tiki;
	char     typeName[32];

	s_fpWeaponName[0] = 0;
	s_fpWeaponClass   = 0;
	if (!weap->currentState.modelindex) {
		return;
	}
	tiki = cgi.R_Model_GetHandle(cgs.model_draw[weap->currentState.modelindex]);
	if (!tiki) {
		return;
	}
	CG_SpectateFP_TikiArg(tiki, "name", s_fpWeaponName, sizeof(s_fpWeaponName));
	typeName[0] = 0;
	if (CG_SpectateFP_TikiArg(tiki, "weapontype", typeName, sizeof(typeName))) {
		s_fpWeaponClass = CG_SpectateFP_ClassFromType(typeName);
	}
	if (!s_fpWeaponClass) {
		s_fpWeaponClass = CG_SpectateFP_ClassFromName(s_fpWeaponName);
	}
}

static int CG_SpectateFP_WeaponAnim(centity_t *weap)
{
	dtiki_t    *tiki;
	const char *animName;
	int         i;
	int         best;
	float       bestW;

	if (!weap->currentState.modelindex) {
		return VM_ANIM_IDLE;
	}
	tiki = cgi.R_Model_GetHandle(cgs.model_draw[weap->currentState.modelindex]);
	if (!tiki) {
		return VM_ANIM_IDLE;
	}

	best  = -1;
	bestW = 0.0f;
	for (i = 0; i < MAX_FRAMEINFOS; i++) {
		if (weap->currentState.frameInfo[i].weight > bestW && weap->currentState.frameInfo[i].index >= 0) {
			bestW = weap->currentState.frameInfo[i].weight;
			best  = weap->currentState.frameInfo[i].index;
		}
	}
	if (best < 0) {
		return VM_ANIM_IDLE;
	}
	animName = cgi.Anim_NameForNum(tiki, best);
	return CG_SpectateFP_AnimFromName(animName);
}

static qboolean CG_SpectateFP_IsIdleAnim(int anim)
{
	return (anim == VM_ANIM_IDLE || anim == VM_ANIM_IDLE_0 || anim == VM_ANIM_IDLE_1 || anim == VM_ANIM_IDLE_2)
	           ? qtrue
	           : qfalse;
}

static void CG_SpectateFP_UpdateWeapon(int followClient)
{
	centity_t *weap;
	int        anim;

	weap = CG_SpectateFP_FindWeapon(followClient);
	if (!weap) {
		s_fpWeaponEntity = -1;
		s_fpPullout      = qfalse;
		s_fpIdleSince    = 0;
		return;
	}

	if (weap->currentState.number != s_fpWeaponEntity) {
		s_fpWeaponEntity = weap->currentState.number;
		CG_SpectateFP_ReadWeaponInfo(weap);
		s_fpPullout   = qtrue;
		s_fpIdleSince = 0;
		CG_SpectateFP_SetAnim(VM_ANIM_PULLOUT);
		return;
	}

	anim = CG_SpectateFP_WeaponAnim(weap);
	if (!CG_SpectateFP_IsIdleAnim(anim)) {
		s_fpPullout   = qfalse;
		s_fpIdleSince = 0;
		CG_SpectateFP_SetAnim(anim);
		return;
	}

	if (s_fpPullout || !CG_SpectateFP_IsIdleAnim(s_fpAnim)) {
		if (!s_fpIdleSince) {
			s_fpIdleSince = cg.time;
		}
		if (!CG_SpectateFP_HandsAnimDone() && cg.time - s_fpIdleSince < SPECTATEFP_IDLE_WAIT) {
			return;
		}
		s_fpPullout   = qfalse;
		s_fpIdleSince = cg.time;
		CG_SpectateFP_SetAnim(VM_ANIM_IDLE);
		return;
	}

	if (!s_fpIdleSince) {
		s_fpIdleSince = cg.time;
	}
	if (cg.time - s_fpIdleSince >= SPECTATEFP_IDLE_WAIT && CG_SpectateFP_HandsAnimDone()) {
		int fidget = VM_ANIM_IDLE_0 + (cg.time / SPECTATEFP_IDLE_WAIT) % 3;
		s_fpIdleSince = cg.time;
		CG_SpectateFP_SetAnim(fidget);
	}
}

qboolean CG_SpectateFP_ArmsReady(void)
{
	return (s_fpArmsFrame == cg.clientFrame) ? qtrue : qfalse;
}

qboolean CG_SpectateFP_DrawArms(const centity_t *cent)
{
	if (!CG_SpectateFP_Active() || !cent) {
		return qfalse;
	}
	if (cent->currentState.number != cg.spectateFp.clientNum) {
		return qfalse;
	}
	if (cent->currentState.eFlags & (EF_DEAD | EF_CLIMBWALL | EF_UNARMED)) {
		return qfalse;
	}
	if (CG_SpectateFP_InZoom()) {
		return qfalse;
	}
	if (cg_drawviewmodel && cg_drawviewmodel->integer <= 1) {
		return qfalse;
	}
	if (s_fpWeaponEntity < 0 || !s_fpWeaponName[0] || !s_fpWeaponClass) {
		return qfalse;
	}
	return qtrue;
}

qboolean CG_SpectateFP_DrawHeldWeapon(const centity_t *cent)
{
	if (!cent || s_fpWeaponEntity < 0 || !CG_SpectateFP_ArmsReady()) {
		return qfalse;
	}
	if (cent->currentState.number != s_fpWeaponEntity) {
		return qfalse;
	}
	return CG_SpectateFP_DrawArms(&cg_entities[cg.spectateFp.clientNum]);
}

static qboolean CG_SpectateFP_BoltRifle(const char *name)
{
	if (!name) {
		return qfalse;
	}
	return (!Q_stricmp(name, "M1 Garand") || !Q_stricmp(name, "Springfield '03 Sniper")
	        || !Q_stricmp(name, "Mauser KAR 98K") || !Q_stricmp(name, "KAR98 - Sniper"))
	           ? qtrue
	           : qfalse;
}

void CG_SpectateFP_ArmsModel(centity_t *cent, refEntity_t *model)
{
	dtiki_t           *worldTiki;
	qhandle_t          hModel;
	char               fpsname[128];
	vec3_t             angles;
	vec3_t             torsoAngles;
	cgVMAnimOverride_t ovr;
	int                i;
	int                iTag;
	orientation_t      eyes;
	vec3_t             ofs;
	vec3_t             sway;
	vec3_t             swayAng;
	float              mat[3][3];

	if (!cent || !model || !model->tiki || !model->tiki->a || !model->tiki->a->name) {
		return;
	}

	if (s_fpArmsClient != cent->currentState.number) {
		for (i = 0; i < MAX_FRAMEINFOS; i++) {
			cent->animLast[i] = -1;
		}
		cent->animLastWeight = 0;
		cent->usageIndexLast = 0;
		s_fpArmsClient       = cent->currentState.number;
	}

	QuatClear(cent->bone_quat[SPECTATEFP_HEAD_TAG]);
	QuatClear(cent->bone_quat[SPECTATEFP_TORSO_TAG]);
	QuatClear(cent->bone_quat[SPECTATEFP_PELVIS_TAG]);

	VectorSet(angles, 0.0f, AngleMod(cg.refdefViewAngles[YAW]), 0.0f);
	VectorSet(torsoAngles, AngleMod(cg.refdefViewAngles[PITCH]), 0.0f, cg.spectateFp.leanAngle * 0.7f);
	if (torsoAngles[0] > 180.0f) {
		torsoAngles[0] -= 360.0f;
	}
	torsoAngles[0] += 8.0f;
	EulerToQuat(torsoAngles, cent->bone_quat[SPECTATEFP_ARMS_TAG]);
	AnglesToAxis(angles, model->axis);

	worldTiki = model->tiki;
	COM_StripExtension(worldTiki->a->name, fpsname, sizeof(fpsname));
	Q_strcat(fpsname, sizeof(fpsname), "_fps.tik");
	hModel = cgi.R_RegisterModel(fpsname);
	if (hModel) {
		cg.hPlayerFPSModelHandle = hModel;
		cg.pPlayerFPSModel       = cgi.R_Model_GetHandle(hModel);
		if (!cg.pPlayerFPSModel) {
			cg.pPlayerFPSModel = worldTiki;
		}
	} else {
		cg.hPlayerFPSModelHandle = cgs.model_draw[cent->currentState.modelindex];
		cg.pPlayerFPSModel       = worldTiki;
	}
	cg.pLastPlayerWorldModel = worldTiki;

	model->tiki   = cg.pPlayerFPSModel;
	model->hModel = cg.hPlayerFPSModelHandle;
	memset(model->surfaces, 0, sizeof(model->surfaces));

	memset(&ovr, 0, sizeof(ovr));
	ovr.valid               = qtrue;
	ovr.equippedWeaponStat  = s_fpWeaponClass;
	ovr.viewModelAnim       = s_fpAnim;
	ovr.viewModelAnimChanged = s_fpAnimCount;
	Q_strncpyz(ovr.activeItem, s_fpWeaponName, sizeof(ovr.activeItem));
	CG_ViewModelAnimationEx(model, &ovr);
	model->renderfx |= RF_FRAMELERP;
	cgi.ForceUpdatePose(model);

	{
		static dtiki_t *s_handTiki = NULL;
		static char     s_handName[64];
		static int      s_left     = -1;
		static int      s_garand   = -1;

		if (model->tiki != s_handTiki || Q_stricmp(s_handName, s_fpWeaponName)) {
			s_handTiki = model->tiki;
			Q_strncpyz(s_handName, s_fpWeaponName, sizeof(s_handName));
			s_left   = cgi.Surface_NameToNum(model->tiki, "lefthand");
			s_garand = cgi.Surface_NameToNum(model->tiki, "garandhand");
		}
		if (CG_SpectateFP_BoltRifle(s_fpWeaponName)) {
			if (s_left >= 0 && s_left < MAX_MODEL_SURFACES) {
				model->surfaces[s_left] |= MDL_SURFACE_NODRAW;
			}
		} else if (s_garand >= 0 && s_garand < MAX_MODEL_SURFACES) {
			model->surfaces[s_garand] |= MDL_SURFACE_NODRAW;
		}
	}

	VectorCopy(cg.refdef.vieworg, model->origin);
	iTag = CG_TikiEyesTag(model->tiki);
	if (iTag != -1) {
		eyes = cgi.TIKI_Orientation(model, iTag);
		VectorClear(ofs);
		for (i = 0; i < 3; i++) {
			VectorMA(ofs, eyes.origin[i], model->axis[i], ofs);
		}
		VectorSubtract(model->origin, ofs, model->origin);
	}

	VectorCopy(cg.refdefViewAngles, swayAng);
	swayAng[PITCH] *= 0.5f;
	swayAng[ROLL] *= 0.75f;
	AngleVectorsLeft(swayAng, mat[0], mat[1], mat[2]);
	CG_CalcViewModelMovementFor(
		s_fpBobPhase, s_fpBobAmp, s_fpVelocity, sway, s_fpWalking, s_fpCrouching, cg.spectateFp.leanAngle
	);
	VectorMA(model->origin, sway[0], mat[0], model->origin);
	VectorMA(model->origin, sway[1], mat[1], model->origin);
	VectorMA(model->origin, sway[2], mat[2], model->origin);
	VectorCopy(model->origin, model->oldorigin);

	model->renderfx &= ~(RF_THIRD_PERSON | RF_DEPTHHACK);
	model->renderfx |= RF_FIRST_PERSON;
	s_fpArmsFrame = cg.clientFrame;
}

qboolean CG_SpectateFP_CalcEye(vec3_t outOrigin, vec3_t outAngles)
{
	centity_t *cent;
	vec3_t     mins, maxs;
	vec3_t     bodyOrigin;
	vec3_t     pivot;
	vec3_t     left;
	vec3_t     delta;
	float      viewHeight;
	float      lean;

	if (!CG_SpectateFP_Active()) {
		return qfalse;
	}

	cent = &cg_entities[cg.spectateFp.clientNum];
	VectorCopy(cg.camera_angles, outAngles);

	/*
	 * Fixed in Omaha: stock chase pads pitch with g_spectatefollow_pitch * trace
	 * fraction (default +2). Undo the full bias — best we can do client-side
	 * without the server fraction — so FP looks with the followed view, not
	 * the over-the-shoulder chase tilt.
	 * Changed in Omaha: hard-coded bias (do not create a console cvar).
	 */
#define CG_SPECFP_PITCH_BIAS 2.0f
	outAngles[PITCH] -= CG_SPECFP_PITCH_BIAS;
	/*
	 * Fixed in Omaha: chase camera_angles wrap pitch into ~350 for slight look-up.
	 * Normalize so the pitch pivot below matches the player's own view.
	 */
	outAngles[PITCH] = AngleNormalize180(outAngles[PITCH]);

	/*
	 * Fixed in Omaha: use origin + stand/crouch viewheight instead of eyes bone.
	 * A bare ForceUpdatePose on the followed world model often left the bone at
	 * feet (camera in the floor). Match retail chase: origin + viewheight.
	 */
	IntegerToBoundingBox(cent->currentState.solid, mins, maxs);
	if (maxs[2] > 0.0f && maxs[2] < 60.0f) {
		viewHeight = (float)CROUCH_VIEWHEIGHT;
	} else {
		viewHeight = (float)DEFAULT_VIEWHEIGHT;
	}

	/*
	 * Fixed in Omaha: snap origins only — not lerpOrigin (smoothClients velocity
	 * lead and/or remote prediction). Clearance still starts from this body pose.
	 */
	CG_SpectateFP_SnapOrigin(cent, bodyOrigin);
	CG_SpectateFP_Motion(cent, bodyOrigin);

	VectorCopy(bodyOrigin, outOrigin);
	/* Added in Omaha: ease eye height the way the player's own view does. */
	CG_SpectateFP_EaseHeight(bodyOrigin[2] + viewHeight);
	outOrigin[2] = s_fpViewZ;
	/*
	 * Chase camera pitch is not the player's look pitch (the entity pitch stays near 0
	 * while the chase pitch sits around +16). Pivoting on it sags the eye. The view
	 * direction still uses the chase angles.
	 */
	pivot[0] = outAngles[PITCH];
	pivot[1] = outAngles[YAW];
	pivot[2] = 0.0f;
	AngleVectorsLeft(pivot, NULL, left, NULL);

	lean = cg.spectateFp.leanAngle;
	CG_FirstPersonApplyLean(outOrigin, outAngles, lean);

	/* A late snapshot arrives as a multi-unit pop. Catch up no faster than a sprint. */
	{
		float  dist;
		float  dt;
		float  maxStep;

		if (!s_fpEyeLastSet) {
			s_fpEyeLastSet = qtrue;
			s_fpStepSpeed  = 0.0f;
		} else {
			VectorSubtract(outOrigin, s_fpEyeLast, delta);
			dist = VectorLength(delta);
			dt   = (float)cg.frametime / 1000.0f;
			if (dt < 0.001f) {
				dt = 0.001f;
			}
			if (dist > 48.0f) {
				s_fpStepSpeed = 0.0f;
			} else {
				maxStep = 280.0f * dt;
				if (dist > maxStep && dist > 0.0f) {
					VectorMA(s_fpEyeLast, maxStep / dist, delta, outOrigin);
					dist = maxStep;
				}
				s_fpStepSpeed = dist / dt;
				if (s_fpStepSpeed > 250.0f) {
					s_fpStepSpeed = 250.0f;
				}
			}
		}
		VectorCopy(outOrigin, s_fpEyeLast);
	}

	/* Added in Omaha: step bob from the followed player's snapshot speed. */
	CG_SpectateFP_StepBob(outOrigin, left, outAngles[PITCH], lean);
	CG_FirstPersonApplyEyeClearance(
		outOrigin, bodyOrigin, viewHeight, cg.spectateFp.clientNum, MASK_PLAYERSOLID
	);

	/*
	 * Match live FP roll: CalcViewValues uses *0.1; OffsetFirstPersonView adds
	 * *0.2 (TA+) or *0.3 (base) when eyes bone exists (normal case).
	 */
	if (lean != 0.0f) {
		outAngles[ROLL] += lean * 0.1f;
		if (cg_target_game >= TG_MOHTA) {
			outAngles[ROLL] += lean * 0.2f;
		} else {
			outAngles[ROLL] += lean * 0.3f;
		}
	}

	return qtrue;
}
