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

#include "cg_crosshair_spread.h"

#include "cg_local.h"
#include "crosshair_spread.h"

#include <string.h>

typedef struct {
	const char *name;
	xspread_weapon_t weapon;
} xspread_weapon_entry_t;

/* Fallback SMG-like profile for unknown weapons. */
static const xspread_weapon_t g_xspreadFallbackWeapon = {
	40.0f, 40.0f, 50.0f, 50.0f, 0.3f, 0.6f, 150.0f, 0.25f, 1.0f
};

#include "cg_crosshair_spread_profiles.inc"

static struct {
	float fireMult;
	float lastShotTime;
	int lastWeaponIndex;
	float displaySpreadPx;
} g_xspreadState;

/* Display-only: ease crosshair closed over this duration when spread drops. */
static const float kCrosshairRecoverSeconds = 0.12f;

static float CG_CrosshairSpread_SmoothDisplay(float targetPx, float deltaTime)
{
	float displayed;

	displayed = g_xspreadState.displaySpreadPx;

	if (targetPx >= displayed) {
		displayed = targetPx;
	} else if (deltaTime > 0.0f && kCrosshairRecoverSeconds > 0.0f) {
		float step;

		step = deltaTime / kCrosshairRecoverSeconds;
		if (step > 1.0f) {
			step = 1.0f;
		}
		displayed += (targetPx - displayed) * step;
	} else {
		displayed = targetPx;
	}

	if (displayed < 0.0f) {
		displayed = 0.0f;
	}

	g_xspreadState.displaySpreadPx = displayed;
	return displayed;
}

cvar_t *cg_crosshair_dynamic_movement;
cvar_t *cg_crosshair_dynamic_scale;

void CG_CrosshairSpread_RegisterCvars(void)
{
	const int flags = CVAR_ARCHIVE;

	cg_crosshair_dynamic_movement = cgi.Cvar_Get("cg_crosshair_dynamic_movement", "1", flags);
	cg_crosshair_dynamic_scale = cgi.Cvar_Get("cg_crosshair_dynamic_scale", "1", flags);
}

static const xspread_weapon_t *CG_CrosshairSpread_FindWeapon(const char *weaponName)
{
	int i;

	if (!weaponName || !weaponName[0]) {
		return &g_xspreadFallbackWeapon;
	}

	for (i = 0; i < (int)(sizeof(g_xspreadWeaponProfiles) / sizeof(g_xspreadWeaponProfiles[0])); i++) {
		if (!Q_stricmp(g_xspreadWeaponProfiles[i].name, weaponName)) {
			return &g_xspreadWeaponProfiles[i].weapon;
		}
	}

	return &g_xspreadFallbackWeapon;
}

static const xspread_weapon_t *CG_CrosshairSpread_ActiveWeapon(void)
{
	const char *weaponName;

	if (!cg.snap || cg.snap->ps.activeItems[1] < 0) {
		return &g_xspreadFallbackWeapon;
	}

	weaponName = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
	return CG_CrosshairSpread_FindWeapon(weaponName);
}

static qboolean CG_CrosshairSpread_IsZoomed(void)
{
	if (!cg.snap) {
		return qfalse;
	}
	/* Added in OPM: FP spectate synthesizes zoom from followed FOV. */
	if (cg.snap->ps.stats[STAT_INZOOM] || CG_SpectateFP_InZoom()) {
		return qtrue;
	}
	return qfalse;
}

void CG_CrosshairSpread_Reset(void)
{
	memset(&g_xspreadState, 0, sizeof(g_xspreadState));
}

void CG_CrosshairSpread_NotifyShot(void)
{
	const xspread_weapon_t *weapon;
	float now;

	weapon = CG_CrosshairSpread_ActiveWeapon();
	if (!weapon || weapon->fireMultAmount == 0.0f) {
		return;
	}

	now = (float)cg.time / 1000.0f;

	/* Mirror Weapon::Fire — decay stored mult since previous shot, then stamp time. */
	XSpread_ApplyFireSpreadDecay(&g_xspreadState.fireMult, g_xspreadState.lastShotTime, now, weapon);
	g_xspreadState.lastShotTime = now;

	/* Mirror post-fire add in Weapon::Fire. */
	XSpread_AddFireMult(&g_xspreadState.fireMult, weapon);
}

float CG_CrosshairSpread_Update(qboolean dynamicEnabled, qboolean movementEnabled, float fovXDeg, float vidHeight)
{
	const xspread_weapon_t *weapon;
	vec3_t velocity;
	float speed;
	float runSpeed;
	float movementFactor;
	float effectiveFireMult;
	float spreadX;
	float spreadY;
	float minSpreadX;
	float minSpreadY;
	float minSpread;
	float effective;
	float excess;
	float targetPx;
	float deltaTime;
	float now;
	int weaponIndex;

	if (!dynamicEnabled || !cg.snap) {
		g_xspreadState.displaySpreadPx = 0.0f;
		return 0.0f;
	}

	weapon = CG_CrosshairSpread_ActiveWeapon();
	weaponIndex = cg.snap->ps.activeItems[1];
	if (weaponIndex != g_xspreadState.lastWeaponIndex) {
		g_xspreadState.fireMult = 0.0f;
		g_xspreadState.lastShotTime = 0.0f;
		g_xspreadState.displaySpreadPx = 0.0f;
		g_xspreadState.lastWeaponIndex = weaponIndex;
	}

	deltaTime = (float)cg.frametime / 1000.0f;
	if (deltaTime < 0.0f) {
		deltaTime = 0.0f;
	}

	now = (float)cg.time / 1000.0f;

	VectorCopy(cg.predicted_player_state.velocity, velocity);
	speed = VectorLength(velocity);
	runSpeed = cgi.Cvar_Get("sv_runspeed", "287", 0)->value;
	movementFactor = XSpread_MovementFactor(speed, runSpeed);
	if (!movementEnabled) {
		movementFactor = 0.0f;
	}

	effectiveFireMult =
		XSpread_GetEffectiveFireMult(g_xspreadState.fireMult, g_xspreadState.lastShotTime, now, weapon);

	XSpread_ComputeBulletSpread(weapon, 0.0f, 0.0f, CG_CrosshairSpread_IsZoomed(), &minSpreadX, &minSpreadY);
	minSpread = XSpread_SpreadMagnitude(minSpreadX, minSpreadY);

	XSpread_ComputeBulletSpread(
		weapon,
		movementFactor,
		effectiveFireMult,
		CG_CrosshairSpread_IsZoomed(),
		&spreadX,
		&spreadY
	);
	effective = XSpread_SpreadMagnitude(spreadX, spreadY);
	excess = effective - minSpread;

	targetPx = XSpread_SpreadExcessPixels(excess, fovXDeg, vidHeight);
	return CG_CrosshairSpread_SmoothDisplay(targetPx, deltaTime);
}
