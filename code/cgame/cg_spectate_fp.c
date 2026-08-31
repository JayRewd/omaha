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
// Client-only first-person chase spectate (eye + lean + zoom; no viewmodel).

#include "cg_local.h"

/* Matches bg_local.h PELVIS_TAG — lean roll lives here on entityState. */
#define SPECTATEFP_PELVIS_TAG 3
#define SPECTATEFP_ZOOM_FOV   45.0f
#define SPECTATEFP_LEAN_MAX   45.0f
/* Client ease rate toward snap-lerped lean (higher = snappier). */
#define SPECTATEFP_LEAN_SMOOTH 12.0f

cvar_t *cg_spectate_firstperson;

static int   s_fpLastClient   = -1;
static float s_fpLeanSmoothed = 0.0f;

void CG_SpectateFP_RegisterCvars(void)
{
	/* Added in OPM: toggle with `toggle cg_spectate_firstperson`. */
	cg_spectate_firstperson = cgi.Cvar_Get("cg_spectate_firstperson", "0", CVAR_ARCHIVE);
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

Fixed in OPM: interpolate lean between current/next entity states (same idea as
ps.fLeanAngle lerp), then the caller eases toward this target for display.
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
		s_fpLastClient   = -1;
		s_fpLeanSmoothed = 0.0f;
		return;
	}

	if (!(cg.snap->ps.pm_flags & PMF_CAMERA_VIEW)) {
		s_fpLastClient   = -1;
		s_fpLeanSmoothed = 0.0f;
		return;
	}

	followClient = cg.snap->ps.stats[STAT_INFOCLIENT];
	if (followClient < 0 || followClient >= MAX_CLIENTS) {
		s_fpLastClient   = -1;
		s_fpLeanSmoothed = 0.0f;
		return;
	}

	followCent = &cg_entities[followClient];
	if (!followCent->currentValid || !followCent->currentState.modelindex) {
		s_fpLastClient   = -1;
		s_fpLeanSmoothed = 0.0f;
		return;
	}

	if (followClient != s_fpLastClient) {
		s_fpLastClient   = followClient;
		s_fpLeanSmoothed = CG_SpectateFP_LeanTarget(followCent);
	}

	cg.spectateFp.active    = qtrue;
	cg.spectateFp.clientNum = followClient;

	{
		float target = CG_SpectateFP_LeanTarget(followCent);
		float frac   = (float)cg.frametime / 1000.0f * SPECTATEFP_LEAN_SMOOTH;

		if (frac > 1.0f) {
			frac = 1.0f;
		} else if (frac < 0.0f) {
			frac = 0.0f;
		}
		s_fpLeanSmoothed += (target - s_fpLeanSmoothed) * frac;
		cg.spectateFp.leanAngle = s_fpLeanSmoothed;
	}

	fov = cg.snap->ps.fov;
	if (fov <= 0.0f) {
		fov = cg.camera_fov;
	}
	cg.spectateFp.inZoom  = (fov > 0.0f && fov <= SPECTATEFP_ZOOM_FOV) ? qtrue : qfalse;
	cg.spectateFp.zoomFov = cg.spectateFp.inZoom ? (int)(fov + 0.5f) : 0;
	if (cg.spectateFp.zoomFov < 1) {
		cg.spectateFp.zoomFov = 1;
	}
}

qboolean CG_SpectateFP_CalcEye(vec3_t outOrigin, vec3_t outAngles)
{
	centity_t *cent;
	vec3_t     right;
	vec3_t     mins, maxs;
	float      viewHeight;
	cvar_t    *pitchBias;

	if (!CG_SpectateFP_Active()) {
		return qfalse;
	}

	cent = &cg_entities[cg.spectateFp.clientNum];
	VectorCopy(cg.camera_angles, outAngles);

	/*
	 * Fixed in OPM: stock chase pads pitch with g_spectatefollow_pitch * trace
	 * fraction (default +2). Undo the full bias — best we can do client-side
	 * without the server fraction — so FP looks with the followed view, not
	 * the over-the-shoulder chase tilt.
	 */
	pitchBias = cgi.Cvar_Get("g_spectatefollow_pitch", "2", 0);
	if (pitchBias) {
		outAngles[PITCH] -= pitchBias->value;
	}

	/*
	 * Fixed in OPM: use origin + stand/crouch viewheight instead of eyes bone.
	 * A bare ForceUpdatePose on the followed world model often left the bone at
	 * feet (camera in the floor). Match retail chase: origin + viewheight.
	 */
	IntegerToBoundingBox(cent->currentState.solid, mins, maxs);
	if (maxs[2] > 0.0f && maxs[2] < 60.0f) {
		viewHeight = (float)CROUCH_VIEWHEIGHT;
	} else {
		viewHeight = (float)DEFAULT_VIEWHEIGHT;
	}

	VectorCopy(cent->lerpOrigin, outOrigin);
	outOrigin[2] += viewHeight;

	if (cg.spectateFp.leanAngle != 0.0f) {
		AngleVectors(outAngles, NULL, right, NULL);
		VectorMA(outOrigin, cg.spectateFp.leanAngle * 0.35f, right, outOrigin);
		outAngles[ROLL] += cg.spectateFp.leanAngle * 0.25f;
	}

	return qtrue;
}
