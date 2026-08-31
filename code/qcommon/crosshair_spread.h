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

#pragma once

#include "q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	float spreadMinX;
	float spreadMinY;
	float spreadMaxX;
	float spreadMaxY;
	float fireMultAmount;
	float fireMultFalloff;
	float fireMultCap;
	float fireMultTimeCap;
	float zoomSpreadMult;
} xspread_weapon_t;

float XSpread_MovementFactor(float speed, float runSpeed);

float XSpread_SpreadMagnitude(float spreadX, float spreadY);

void XSpread_ComputeBulletSpread(
	const xspread_weapon_t *weapon,
	float movementFactor,
	float fireMult,
	qboolean zoomed,
	float *outSpreadX,
	float *outSpreadY
);

void XSpread_AddFireMult(float *fireMult, const xspread_weapon_t *weapon);

/* Weapon::GetCurrentFireSpreadMult — elapsed since last shot, hard reset after timeCap. */
float XSpread_GetEffectiveFireMult(
	float storedMult,
	float lastShotTime,
	float currentTime,
	const xspread_weapon_t *weapon
);

/* Weapon::Fire spread-mult decay applied at the start of each shot. */
void XSpread_ApplyFireSpreadDecay(
	float *fireMult,
	float lastShotTime,
	float currentTime,
	const xspread_weapon_t *weapon
);

float XSpread_Normalize(float effective, float minSpread, float maxSpread);

void XSpread_ComputeSpreadBounds(const xspread_weapon_t *weapon, float *outMin, float *outMax);

/* Map standing-relative spread excess to screen pixels (CS-style FOV projection). */
float XSpread_SpreadExcessPixels(float excessSpread, float fovXDeg, float vidHeight);

#ifdef __cplusplus
}
#endif
