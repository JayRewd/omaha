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

#include <stdio.h>

static int g_failures;

static void expect_true(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		g_failures++;
	}
}

static void expect_near(float value, float expected, float epsilon, const char *message)
{
	if (value < expected - epsilon || value > expected + epsilon) {
		fprintf(stderr, "FAIL: %s (got %f expected %f)\n", message, value, expected);
		g_failures++;
	}
}

static void test_movement_factor(void)
{
	expect_near(XSpread_MovementFactor(143.5f, 287.0f), 0.5f, 0.001f, "half run speed");
	expect_near(XSpread_MovementFactor(400.0f, 287.0f), 1.0f, 0.001f, "clamped to 1");
	expect_near(XSpread_MovementFactor(0.0f, 287.0f), 0.0f, 0.001f, "standing still");
}

static void test_bullet_spread(void)
{
	xspread_weapon_t weapon;
	float spreadX;
	float spreadY;

	weapon.spreadMinX = 40.0f;
	weapon.spreadMinY = 40.0f;
	weapon.spreadMaxX = 50.0f;
	weapon.spreadMaxY = 50.0f;
	weapon.fireMultAmount = 0.3f;
	weapon.fireMultFalloff = 0.6f;
	weapon.fireMultCap = 150.0f;
	weapon.fireMultTimeCap = 0.25f;
	weapon.zoomSpreadMult = 1.0f;

	XSpread_ComputeBulletSpread(&weapon, 0.0f, 0.0f, qfalse, &spreadX, &spreadY);
	expect_near(spreadX, 40.0f, 0.001f, "still min spread x");
	expect_near(spreadY, 40.0f, 0.001f, "still min spread y");

	XSpread_ComputeBulletSpread(&weapon, 1.0f, 0.0f, qfalse, &spreadX, &spreadY);
	expect_near(spreadX, 50.0f, 0.001f, "run max spread x");

	XSpread_ComputeBulletSpread(&weapon, 0.0f, 1.0f, qfalse, &spreadX, &spreadY);
	expect_near(spreadX, 80.0f, 0.001f, "fire mult doubles spread at rest");
}

static void test_effective_fire_mult(void)
{
	xspread_weapon_t weapon;
	float effective;

	weapon.spreadMinX = 40.0f;
	weapon.spreadMinY = 40.0f;
	weapon.spreadMaxX = 50.0f;
	weapon.spreadMaxY = 50.0f;
	weapon.fireMultAmount = 0.3f;
	weapon.fireMultFalloff = 0.6f;
	weapon.fireMultCap = 150.0f;
	weapon.fireMultTimeCap = 0.25f;
	weapon.zoomSpreadMult = 1.0f;

	effective = XSpread_GetEffectiveFireMult(3.0f, 1.0f, 1.1f, &weapon);
	expect_near(effective, 2.94f, 0.001f, "effective mult decays with elapsed since last shot");

	effective = XSpread_GetEffectiveFireMult(3.0f, 1.0f, 1.26f, &weapon);
	expect_near(effective, 0.0f, 0.001f, "effective mult resets after weapon time cap");

	effective = XSpread_GetEffectiveFireMult(3.0f, 0.0f, 100.0f, &weapon);
	expect_near(effective, 0.0f, 0.001f, "long idle yields zero effective mult");
}

static void test_apply_fire_spread_decay(void)
{
	xspread_weapon_t weapon;
	float stored;

	weapon.spreadMinX = 40.0f;
	weapon.spreadMinY = 40.0f;
	weapon.spreadMaxX = 50.0f;
	weapon.spreadMaxY = 50.0f;
	weapon.fireMultAmount = 0.3f;
	weapon.fireMultFalloff = 0.6f;
	weapon.fireMultCap = 150.0f;
	weapon.fireMultTimeCap = 0.25f;
	weapon.zoomSpreadMult = 1.0f;

	stored = 2.0f;
	XSpread_ApplyFireSpreadDecay(&stored, 1.0f, 1.1f, &weapon);
	expect_near(stored, 1.94f, 0.001f, "apply decay mutates stored mult at shot time");
}

static void test_normalize(void)
{
	expect_near(XSpread_Normalize(60.0f, 40.0f, 60.0f), 1.0f, 0.001f, "normalize clamp high");
	expect_near(XSpread_Normalize(30.0f, 40.0f, 60.0f), 0.0f, 0.001f, "normalize clamp low");
	expect_near(XSpread_Normalize(50.0f, 40.0f, 60.0f), 0.5f, 0.001f, "normalize midpoint");
}

static void test_spread_excess_pixels(void)
{
	float px;

	px = XSpread_SpreadExcessPixels(0.0f, 80.0f, 1080.0f);
	expect_near(px, 0.0f, 0.001f, "zero excess yields zero pixels");

	px = XSpread_SpreadExcessPixels(10.0f, 80.0f, 1080.0f);
	expect_true(px > 1.0f && px < 15.0f, "running-scale excess maps to visible pixels");

	px = XSpread_SpreadExcessPixels(500.0f, 80.0f, 1080.0f);
	expect_near(px, 25.0f, 0.001f, "spread pixel expansion is capped");
}

int main(void)
{
	test_movement_factor();
	test_bullet_spread();
	test_effective_fire_mult();
	test_apply_fire_spread_decay();
	test_normalize();
	test_spread_excess_pixels();

	if (g_failures != 0) {
		fprintf(stderr, "%d test(s) failed\n", g_failures);
		return 1;
	}

	printf("test_crosshair_spread: all tests passed\n");
	return 0;
}
