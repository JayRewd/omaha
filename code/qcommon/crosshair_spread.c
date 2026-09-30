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

#include "crosshair_spread.h"

#include <math.h>

float XSpread_MovementFactor(float speed, float runSpeed)
{
	float factor;

	if (runSpeed <= 0.0f) {
		return 0.0f;
	}

	factor = speed / runSpeed;
	if (factor > 1.0f) {
		factor = 1.0f;
	}
	if (factor < 0.0f) {
		factor = 0.0f;
	}
	return factor;
}

float XSpread_SpreadMagnitude(float spreadX, float spreadY)
{
	const float ax = spreadX >= 0.0f ? spreadX : -spreadX;
	const float ay = spreadY >= 0.0f ? spreadY : -spreadY;

	if (ax > ay) {
		return ax;
	}
	return ay;
}

void XSpread_ComputeBulletSpread(
	const xspread_weapon_t *weapon,
	float movementFactor,
	float fireMult,
	qboolean zoomed,
	float *outSpreadX,
	float *outSpreadY
)
{
	float spreadFactor;
	float stillFactor;
	float spreadX;
	float spreadY;

	if (!weapon || !outSpreadX || !outSpreadY) {
		return;
	}

	if (movementFactor > 1.0f) {
		movementFactor = 1.0f;
	}
	if (movementFactor < 0.0f) {
		movementFactor = 0.0f;
	}

	spreadFactor = movementFactor;
	spreadX = weapon->spreadMaxX * spreadFactor;
	spreadY = weapon->spreadMaxY * spreadFactor;
	stillFactor = 1.0f - spreadFactor;
	spreadX += weapon->spreadMinX * stillFactor;
	spreadY += weapon->spreadMinY * stillFactor;

	if (weapon->fireMultAmount != 0.0f) {
		spreadX *= fireMult + 1.0f;
		spreadY *= fireMult + 1.0f;
	}

	if (zoomed && weapon->zoomSpreadMult != 0.0f && weapon->zoomSpreadMult != 1.0f) {
		const float zoomScale = 1.0f + stillFactor * (weapon->zoomSpreadMult - 1.0f);
		spreadX *= zoomScale;
		spreadY *= zoomScale;
	}

	*outSpreadX = spreadX;
	*outSpreadY = spreadY;
}

float XSpread_GetEffectiveFireMult(
	float storedMult,
	float lastShotTime,
	float currentTime,
	const xspread_weapon_t *weapon
)
{
	float elapsed;
	float effective;

	if (!weapon || weapon->fireMultAmount == 0.0f) {
		return 0.0f;
	}

	elapsed = currentTime - lastShotTime;
	if (elapsed < 0.0f) {
		elapsed = 0.0f;
	}

	if (weapon->fireMultTimeCap > 0.0f && elapsed > weapon->fireMultTimeCap) {
		return 0.0f;
	}

	effective = storedMult - elapsed * weapon->fireMultFalloff;
	if (effective < 0.0f) {
		return 0.0f;
	}
	return effective;
}

void XSpread_ApplyFireSpreadDecay(
	float *fireMult,
	float lastShotTime,
	float currentTime,
	const xspread_weapon_t *weapon
)
{
	if (!fireMult) {
		return;
	}

	*fireMult = XSpread_GetEffectiveFireMult(*fireMult, lastShotTime, currentTime, weapon);
}

void XSpread_AddFireMult(float *fireMult, const xspread_weapon_t *weapon)
{
	if (!fireMult || !weapon || weapon->fireMultAmount == 0.0f) {
		return;
	}

	*fireMult += weapon->fireMultAmount;

	if (weapon->fireMultCap > 0.0f) {
		if (*fireMult > weapon->fireMultCap) {
			*fireMult = weapon->fireMultCap;
		} else if (*fireMult < 0.0f) {
			*fireMult = 0.0f;
		}
	} else if (weapon->fireMultCap < 0.0f) {
		if (*fireMult < weapon->fireMultCap) {
			*fireMult = weapon->fireMultCap;
		} else if (*fireMult > 0.0f) {
			*fireMult = 0.0f;
		}
	}
}

float XSpread_Normalize(float effective, float minSpread, float maxSpread)
{
	const float range = maxSpread - minSpread;

	if (range <= 0.0f) {
		return 0.0f;
	}

	if (effective <= minSpread) {
		return 0.0f;
	}
	if (effective >= maxSpread) {
		return 1.0f;
	}
	return (effective - minSpread) / range;
}

float XSpread_SpreadExcessPixels(float excessSpread, float fovXDeg, float vidHeight)
{
	float halfFovRad;
	float tanHalfFov;
	float pixels;
	/* MOHAA script spread units -> pixels; tuned for ~6px full run at 1080p/80fov. */
	const float kSpreadPixelScale = 0.25f;
	const float kMaxSpreadPixels = 25.0f;

	if (excessSpread <= 0.0f || fovXDeg <= 0.0f || vidHeight <= 0.0f) {
		return 0.0f;
	}

	halfFovRad = (fovXDeg * 0.5f) * ((float)M_PI / 180.0f);
	tanHalfFov = tanf(halfFovRad);
	if (tanHalfFov <= 0.0f) {
		return 0.0f;
	}

	pixels = excessSpread * kSpreadPixelScale * (vidHeight / 480.0f) / tanHalfFov;
	if (pixels > kMaxSpreadPixels) {
		pixels = kMaxSpreadPixels;
	}
	return pixels;
}

void XSpread_ComputeSpreadBounds(const xspread_weapon_t *weapon, float *outMin, float *outMax)
{
	float minX;
	float minY;
	float maxX;
	float maxY;
	float minMag;
	float maxMag;

	if (!weapon || !outMin || !outMax) {
		return;
	}

	XSpread_ComputeBulletSpread(weapon, 0.0f, 0.0f, qfalse, &minX, &minY);
	minMag = XSpread_SpreadMagnitude(minX, minY);

	XSpread_ComputeBulletSpread(weapon, 1.0f, weapon->fireMultCap, qfalse, &maxX, &maxY);
	maxMag = XSpread_SpreadMagnitude(maxX, maxY);

	*outMin = minMag;
	*outMax = maxMag;
	if (*outMax <= *outMin) {
		*outMax = *outMin + 1.0f;
	}
}
