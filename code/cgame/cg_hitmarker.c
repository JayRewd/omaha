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

#include "cg_hitmarker.h"

#include "cg_local.h"

#include <math.h>

/* Display window after a confirmed / approximated hit. */
#define HITMARKER_DURATION_MS  250
#define HITMARKER_SOUND_VOLUME 2.0f /* Same as retail dm_hit_notify. */
#define HITMARKER_OPACITY      0.5f /* Peak alpha; fade still runs to 0. */
#define HITMARKER_INNER_PX     6.0f
#define HITMARKER_OUTER_PX     14.0f
#define HITMARKER_THICKNESS    2.0f
#define HITMARKER_TRACE_DIST   8192.0f

/* Changed in Omaha: client approx mode kept in source but not selectable yet. */
#define HITMARKER_CLIENT_MODE_ENABLED 0

static cvar_t *cg_hitmarker;
static cvar_t *cg_hitmarker_mode;
static cvar_t *cg_hitmarker_sound;

static int s_hitTime;

void CG_Hitmarker_RegisterCvars(void)
{
	const int flags = CVAR_ARCHIVE;

	cg_hitmarker = cgi.Cvar_Get("cg_hitmarker", "0", flags);
	cg_hitmarker_mode = cgi.Cvar_Get("cg_hitmarker_mode", "server", flags);
	cg_hitmarker_sound = cgi.Cvar_Get("cg_hitmarker_sound", "hitmarker", flags);

#if !HITMARKER_CLIENT_MODE_ENABLED
	if (cg_hitmarker_mode && !Q_stricmp(cg_hitmarker_mode->string, "client")) {
		cgi.Cvar_Set("cg_hitmarker_mode", "server");
	}
#endif
}

qboolean CG_Hitmarker_Enabled(void)
{
	return (cg_hitmarker && cg_hitmarker->integer) ? qtrue : qfalse;
}

qboolean CG_Hitmarker_IsServerMode(void)
{
#if !HITMARKER_CLIENT_MODE_ENABLED
	return qtrue;
#else
	if (!cg_hitmarker_mode || !cg_hitmarker_mode->string[0]) {
		return qtrue;
	}
	return Q_stricmp(cg_hitmarker_mode->string, "client") ? qtrue : qfalse;
#endif
}

qboolean CG_Hitmarker_SuppressRetailNotify(void)
{
	return CG_Hitmarker_Enabled();
}

/* Changed in Omaha: CHAN_LOCAL like retail dm_hit_notify — S_StartLocalSound uses CHAN_MENU and can spatialize to silence. */
static void CG_Hitmarker_PlaySound(void)
{
	char        path[MAX_QPATH];
	char        name[MAX_QPATH];
	sfxHandle_t handle;
	int         entNum;
	size_t      len;

	if (cg_hitmarker_sound && cg_hitmarker_sound->string[0]) {
		Q_strncpyz(name, cg_hitmarker_sound->string, sizeof(name));
	} else {
		Q_strncpyz(name, "hitmarker", sizeof(name));
	}
	/* Basename only — reject path traversal from the archived cvar. */
	if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
		Q_strncpyz(name, "hitmarker", sizeof(name));
	}
	len = strlen(name);
	if (len > 4 && !Q_stricmp(name + len - 4, ".wav")) {
		name[len - 4] = '\0';
	}
	if (!name[0]) {
		Q_strncpyz(name, "hitmarker", sizeof(name));
	}
	/* Changed in Omaha: cyclic "None" disables hitmarker sound. */
	if (!Q_stricmp(name, "none")) {
		return;
	}

	Com_sprintf(path, sizeof(path), "sound/prom/hitmarkers/%s.wav", name);
	handle = cgi.S_RegisterSound(path, qfalse);
	if (!handle) {
		handle = cgi.S_RegisterSound("sound/prom/hitmarkers/hitmarker.wav", qfalse);
	}
	/* clientNum + CHAN_LOCAL: always reclaimable; ENTITYNUM_NONE can fail PickChannel when 2D is busy. */
	entNum = cg.snap ? cg.snap->ps.clientNum : 0;
	cgi.S_StartSound(NULL, entNum, CHAN_LOCAL, handle, HITMARKER_SOUND_VOLUME, -1.0f, 1.0f, -1.0f, qfalse);
}

void CG_Hitmarker_Trigger(void)
{
	if (!CG_Hitmarker_Enabled()) {
		return;
	}

	s_hitTime = cg.time;
	CG_Hitmarker_PlaySound();
}

void CG_Hitmarker_OnNotify(qboolean isKill)
{
	(void)isKill;

	if (!CG_Hitmarker_Enabled() || !CG_Hitmarker_IsServerMode()) {
		return;
	}

	CG_Hitmarker_Trigger();
}

static qboolean CG_Hitmarker_IsEnemyPlayer(const centity_t *cent)
{
	int myFlags;
	int theirFlags;

	if (!cent || cent->currentState.eType != ET_PLAYER) {
		return qfalse;
	}
	if (cent->currentState.number == cg.snap->ps.clientNum) {
		return qfalse;
	}

	/* Kill shots often already have EF_DEAD — still count for client approx. */
	if (cgs.gametype == GT_FFA) {
		return qtrue;
	}

	if (cgs.gametype != GT_SINGLE_PLAYER) {
		myFlags = cg_entities[cg.snap->ps.clientNum].currentState.eFlags & EF_ANY_TEAM;
	} else {
		myFlags = EF_ALLIES;
	}
	theirFlags = cent->currentState.eFlags & EF_ANY_TEAM;
	if (((myFlags & EF_ALLIES) && (theirFlags & EF_ALLIES))
	    || ((myFlags & EF_AXIS) && (theirFlags & EF_AXIS))) {
		return qfalse;
	}
	return qtrue;
}

/*
 * CG_BuildSolidList only adds ents with nextState.solid. On a kill shot the next
 * snapshot can already clear solid while currentState still has the living hull —
 * CG_Trace then tunnels through the player. Retest exact currentState solids.
 */
static int CG_Hitmarker_TraceCurrentSolids(const vec3_t start, const vec3_t end, float maxFraction)
{
	int    i;
	int    bestEnt;
	float  bestFrac;
	vec3_t zmins, zmaxs;

	bestEnt  = -1;
	bestFrac = maxFraction;
	VectorClear(zmins);
	VectorClear(zmaxs);

	for (i = 0; i < MAX_CLIENTS; i++) {
		centity_t   *cent;
		vec3_t       pmins, pmaxs;
		vec3_t       angles;
		trace_t      tr;
		clipHandle_t cmodel;
		int          solid;

		cent = &cg_entities[i];
		if (!CG_Hitmarker_IsEnemyPlayer(cent)) {
			continue;
		}
		solid = cent->currentState.solid;
		if (!solid || solid == SOLID_BMODEL) {
			continue;
		}

		IntegerToBoundingBox(solid, pmins, pmaxs);
		cmodel = cgi.CM_TempBoxModel(pmins, pmaxs, CONTENTS_BBOX);
		VectorClear(angles);
		if (cent->currentState.eFlags & EF_LINKANGLES) {
			VectorCopy(cent->lerpAngles, angles);
		}
		cgi.CM_TransformedBoxTrace(
			&tr, start, end, zmins, zmaxs, cmodel, MASK_SHOT_TRIG, cent->lerpOrigin, angles, qfalse
		);
		if (tr.startsolid || tr.allsolid) {
			continue;
		}
		if (tr.fraction < bestFrac) {
			bestFrac = tr.fraction;
			bestEnt  = i;
		}
	}

	return bestEnt;
}

/*
 * Client approx mirrors player BulletAttack geometry (see Weapon::GetMuzzlePosition
 * + BulletAttack): view origin, view-forward, point hull, MASK_SHOT_TRIG.
 */
void CG_Hitmarker_OnLocalFire(void)
{
	vec3_t     angles;
	vec3_t     forward;
	vec3_t     start;
	vec3_t     end;
	vec3_t     mins, maxs;
	trace_t    trace;
	centity_t *cent;
	int        hitEnt;

	if (!CG_Hitmarker_Enabled() || CG_Hitmarker_IsServerMode()) {
		return;
	}

	if (!cg.snap) {
		return;
	}

	if ((cg.snap->ps.pm_flags & PMF_SPECTATING) || cg.snap->ps.stats[STAT_TEAM] == TEAM_SPECTATOR) {
		return;
	}

	/*
	 * Same inputs as server player GetMuzzlePosition: view angles + view pos.
	 * Use predicted angles (pre-viewkick accumulate) and current refdef eye.
	 */
	VectorCopy(cg.predicted_player_state.viewangles, angles);
	AngleVectors(angles, forward, NULL, NULL);
	VectorCopy(cg.refdef.vieworg, start);
	VectorMA(start, HITMARKER_TRACE_DIST, forward, end);
	VectorClear(mins);
	VectorClear(maxs);

	/* BulletAttack first segment: point trace, MASK_SHOT_TRIG. */
	CG_Trace(
		&trace,
		start,
		mins,
		maxs,
		end,
		cg.snap->ps.clientNum,
		MASK_SHOT_TRIG,
		qfalse,
		qtrue,
		"CG_Hitmarker_OnLocalFire"
	);

	hitEnt = -1;
	if (trace.entityNum != ENTITYNUM_NONE && trace.entityNum != ENTITYNUM_WORLD
	    && trace.entityNum != cg.snap->ps.clientNum) {
		cent = &cg_entities[trace.entityNum];
		if (CG_Hitmarker_IsEnemyPlayer(cent)) {
			hitEnt = trace.entityNum;
		}
	}

	if (hitEnt < 0) {
		hitEnt = CG_Hitmarker_TraceCurrentSolids(start, end, trace.fraction);
		if (hitEnt < 0) {
			return;
		}
	}

	CG_Hitmarker_Trigger();
}

static void CG_Hitmarker_ApplyRecoilOffset(float *cx, float *cy)
{
	cvar_t *recoil;
	float   kickScale;

	recoil = cgi.Cvar_Find("cg_crosshair_recoil");
	if (!recoil || !recoil->integer) {
		return;
	}

	kickScale = 4.0f * cgs.uiHiResScale[1];
	*cy += cg.viewkick[0] * kickScale;
	*cx += cg.viewkick[1] * kickScale * cgs.uiHiResScale[0] / cgs.uiHiResScale[1];
}

static void CG_Hitmarker_DrawSegment(
	float cx, float cy, float ux, float uy, float inner, float outer, float thickness, float alpha
)
{
	float  len;
	int    steps;
	int    i;
	vec4_t color;

	len = outer - inner;
	if (len <= 0.0f || thickness <= 0.0f || alpha <= 0.0f) {
		return;
	}

	steps = (int)(len + 0.5f);
	if (steps < 1) {
		steps = 1;
	}

	color[0] = 1.0f;
	color[1] = 1.0f;
	color[2] = 1.0f;
	color[3] = alpha;

	cgi.R_SetColor(color);
	for (i = 0; i <= steps; i++) {
		float t = inner + (len * (float)i) / (float)steps;
		float x = cx + ux * t - thickness * 0.5f;
		float y = cy + uy * t - thickness * 0.5f;

		cgi.R_DrawBox(x, y, thickness, thickness);
	}
	cgi.R_SetColor(NULL);
}

void CG_DrawHitmarker(void)
{
	float cx, cy;
	float scale;
	float inner, outer, thickness;
	float alpha;
	float invSqrt2;
	int   age;

	if (!CG_Hitmarker_Enabled()) {
		return;
	}

	if (!cg_hud || !cg_hud->integer) {
		return;
	}

	if (!cg.snap) {
		return;
	}

	if ((cg.snap->ps.pm_flags & PMF_NO_HUD) || (cg.snap->ps.pm_flags & PMF_INTERMISSION)) {
		return;
	}

	if (!s_hitTime) {
		return;
	}

	age = cg.time - s_hitTime;
	if (age < 0 || age >= HITMARKER_DURATION_MS) {
		s_hitTime = 0;
		return;
	}

	alpha = HITMARKER_OPACITY * (1.0f - ((float)age / (float)HITMARKER_DURATION_MS));
	if (alpha <= 0.0f) {
		return;
	}

	scale = cgs.uiHiResScale[1];
	if (scale <= 0.0f) {
		scale = 1.0f;
	}

	inner = HITMARKER_INNER_PX * scale;
	outer = HITMARKER_OUTER_PX * scale;
	thickness = HITMARKER_THICKNESS * scale;
	if (thickness < 1.0f) {
		thickness = 1.0f;
	}

	cx = floorf(cgs.glconfig.vidWidth * 0.5f);
	cy = floorf(cgs.glconfig.vidHeight * 0.5f);
	CG_Hitmarker_ApplyRecoilOffset(&cx, &cy);
	cx = floorf(cx);
	cy = floorf(cy);

	invSqrt2 = 0.70710678f;
	CG_Hitmarker_DrawSegment(cx, cy, invSqrt2, invSqrt2, inner, outer, thickness, alpha);
	CG_Hitmarker_DrawSegment(cx, cy, invSqrt2, -invSqrt2, inner, outer, thickness, alpha);
	CG_Hitmarker_DrawSegment(cx, cy, -invSqrt2, invSqrt2, inner, outer, thickness, alpha);
	CG_Hitmarker_DrawSegment(cx, cy, -invSqrt2, -invSqrt2, inner, outer, thickness, alpha);
}
