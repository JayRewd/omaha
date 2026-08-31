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

#include "uir_menu_weather.h"

#include "../qcommon/q_shared.h"
#include "../renderercommon/tr_types.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UIR_MENU_WEATHER_MAX_PARTICLES 512
#define UIR_MENU_WEATHER_EXTENT_MIN    384.0f
#define UIR_MENU_WEATHER_EXTENT_MAX    1024.0f

typedef struct {
	vec3_t start;
	vec3_t end;
	int    birth;
	int    life;
	int    shader;
	int    active;
} uir_weather_particle_t;

static uir_weather_particle_t s_particles[UIR_MENU_WEATHER_MAX_PARTICLES];
static char                   s_particleBsp[MAX_QPATH];

void UIR_MenuWeatherInvalidate(void)
{
	memset(s_particles, 0, sizeof(s_particles));
	s_particleBsp[0] = '\0';
}

static float uir_weather_menu_extent(const uir_weather_params_t *rain)
{
	float extent;

	if (!rain) {
		return UIR_MENU_WEATHER_EXTENT_MIN;
	}
	extent = rain->min_dist > 0.0f ? rain->min_dist : 512.0f;
	if (extent > UIR_MENU_WEATHER_EXTENT_MAX) {
		extent = UIR_MENU_WEATHER_EXTENT_MAX;
	}
	if (extent < UIR_MENU_WEATHER_EXTENT_MIN) {
		extent = UIR_MENU_WEATHER_EXTENT_MIN;
	}
	return extent;
}

static void uir_weather_build_camera_volume(const float vieworg[3], const uir_weather_params_t *rain, uir_rain_volume_t *out)
{
	float extent;

	if (!vieworg || !rain || !out) {
		return;
	}

	extent = uir_weather_menu_extent(rain);
	out->mins[0] = vieworg[0] - extent;
	out->mins[1] = vieworg[1] - extent;
	out->mins[2] = vieworg[2] - 96.0f;
	out->maxs[0] = vieworg[0] + extent;
	out->maxs[1] = vieworg[1] + extent;
	out->maxs[2] = vieworg[2] + extent * 0.75f;
}

static int uir_weather_lcg(int seed)
{
	uint32_t state = (uint32_t)seed;

	/* Fixed in OPM: preserve the retail 32-bit wrap instead of signed-overflow UB. */
	state = 214013u * state + 2531011u;
	return (int)((state >> 16) & 0x7FFFu);
}

static float uir_weather_cvar_float(const uir_menuworld_backend_t *backend, const char *name, float fallback)
{
	if (backend && backend->cvarFloat) {
		return backend->cvarFloat(name, fallback);
	}
	return fallback;
}

static int uir_weather_add_beam(
	const uir_menuworld_backend_t *backend,
	const float start[3],
	const float end[3],
	float scale,
	int shader,
	const float viewaxis[3][3])
{
	polyVert_t points[4];
	int        i;
	int        j;
	byte       color[4] = {255, 255, 255, 255};

	if (!backend || !backend->addPolyToScene || shader <= 0) {
		return 0;
	}

	VectorMA(end, scale, viewaxis[1], points[0].xyz);
	VectorMA(start, scale, viewaxis[1], points[1].xyz);
	VectorMA(start, -scale, viewaxis[1], points[2].xyz);
	VectorMA(end, -scale, viewaxis[1], points[3].xyz);

	points[0].st[0] = 1.0f;
	points[0].st[1] = 1.0f;
	points[1].st[0] = 0.0f;
	points[1].st[1] = 1.0f;
	points[2].st[0] = 0.0f;
	points[2].st[1] = 0.0f;
	points[3].st[0] = 1.0f;
	points[3].st[1] = 0.0f;

	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			points[i].modulate[j] = color[j];
		}
	}

	backend->addPolyToScene(shader, 4, points, 0);
	return 1;
}

static void uir_weather_normalize(vec3_t v)
{
	float length = VectorLength(v);

	if (length > 0.0001f) {
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
}

/* Fixed in OPM: match CG_AddBeams BEAM_INVERTED_FAST — short streak of rain_length, not floor span. */
static int uir_weather_draw_inverted_fast(
	const uir_menuworld_backend_t *backend,
	const uir_weather_particle_t *p,
	float streakLength,
	float scale,
	int realtime,
	const float viewaxis[3][3])
{
	float  fade;
	vec3_t vDir;
	vec3_t vCurrStart;
	vec3_t vCurrEnd;

	if (!p || p->life <= 0) {
		return 0;
	}

	fade = (float)((p->birth + p->life) - realtime) / (float)p->life;
	if (fade <= 0.0f || fade > 1.0f) {
		return 0;
	}

	VectorSubtract(p->end, p->start, vDir);
	VectorMA(p->start, 1.0f - fade, vDir, vCurrEnd);
	uir_weather_normalize(vDir);
	VectorMA(vCurrEnd, -streakLength, vDir, vCurrStart);

	return uir_weather_add_beam(backend, vCurrStart, vCurrEnd, scale, p->shader, viewaxis);
}

static int uir_weather_alloc_particle(void)
{
	int i;

	for (i = 0; i < UIR_MENU_WEATHER_MAX_PARTICLES; i++) {
		if (!s_particles[i].active) {
			return i;
		}
	}
	return -1;
}

static void uir_weather_expire_particles(int realtime)
{
	int i;

	for (i = 0; i < UIR_MENU_WEATHER_MAX_PARTICLES; i++) {
		if (!s_particles[i].active) {
			continue;
		}
		if (realtime >= s_particles[i].birth + s_particles[i].life) {
			s_particles[i].active = 0;
		}
	}
}

static void uir_weather_spawn_new(
	const uir_map_env_t *env,
	const uir_rain_volume_t *volume,
	const float vieworg[3],
	int realtime,
	const uir_menuworld_backend_t *backend,
	int *shaderHandles,
	int shaderCount)
{
	vec3_t      vOmins;
	vec3_t      vOmaxs;
	vec3_t      vOe;
	vec3_t      vStart;
	vec3_t      vEnd;
	vec3_t      vLength;
	float       fDensity;
	float       spawnDist;
	int         iNumSpawn;
	int         i;
	int         iRandom;
	float       windX;
	float       windY;
	const uir_weather_params_t *rain = &env->weather;

	if (!env || !volume || !vieworg || !backend || !shaderHandles || shaderCount <= 0) {
		return;
	}

	spawnDist = uir_weather_menu_extent(rain);

	vOmins[0] = volume->mins[0];
	if (vOmins[0] < vieworg[0] - spawnDist) {
		vOmins[0] = vieworg[0] - spawnDist;
	}
	vOmins[1] = volume->mins[1];
	if (vOmins[1] < vieworg[1] - spawnDist) {
		vOmins[1] = vieworg[1] - spawnDist;
	}
	vOmins[2] = volume->mins[2];

	vOmaxs[0] = volume->maxs[0];
	if (vOmaxs[0] > vieworg[0] + spawnDist) {
		vOmaxs[0] = vieworg[0] + spawnDist;
	}
	vOmaxs[1] = volume->maxs[1];
	if (vOmaxs[1] > vieworg[1] + spawnDist) {
		vOmaxs[1] = vieworg[1] + spawnDist;
	}
	vOmaxs[2] = volume->maxs[2];

	if (vOmins[0] > vOmaxs[0] || vOmins[1] > vOmaxs[1]) {
		return;
	}

	VectorSubtract(vOmaxs, vOmins, vOe);
	fDensity  = rain->density / 200.0f;
	iNumSpawn = (int)(sqrtf(vOe[0] * vOe[1]) * fDensity);
	if (iNumSpawn > UIR_MENU_WEATHER_MAX_PARTICLES) {
		iNumSpawn = UIR_MENU_WEATHER_MAX_PARTICLES;
	}

	windX = uir_weather_cvar_float(backend, "vss_wind_x", 0.0f);
	windY = uir_weather_cvar_float(backend, "vss_wind_y", 0.0f);

	iRandom = rand();
	for (i = 0; i < iNumSpawn; i++) {
		int   iLife;
		int   shaderIndex;
		int   slot;
		float spanX;
		float spanY;
		float spanZ;
		int   slant;
		int   speedVary;

		slot = uir_weather_alloc_particle();
		if (slot < 0) {
			break;
		}

		spanX = vOe[0] + 1.0f;
		spanY = vOe[1] + 1.0f;
		spanZ = vOe[2] + 1.0f;
		if (spanX < 1.0f) {
			spanX = 1.0f;
		}
		if (spanY < 1.0f) {
			spanY = 1.0f;
		}
		if (spanZ < 1.0f) {
			spanZ = 1.0f;
		}

		vStart[0] = (float)(iRandom % (int)spanX) + vOmins[0];
		iRandom   = uir_weather_lcg(iRandom);
		vStart[1] = (float)(iRandom % (int)spanY) + vOmins[1];
		iRandom   = uir_weather_lcg(iRandom);
		vStart[2] = (float)(iRandom % (int)spanZ) + vOmins[2];

		VectorSubtract(vieworg, vStart, vLength);
		vLength[2] = 0.0f;
		if (VectorLengthSquared(vLength) > Square(spawnDist)) {
			continue;
		}

		slant = rain->slant > 0 ? rain->slant : 50;
		iRandom = uir_weather_lcg(iRandom);
		vEnd[0] = (float)(iRandom % slant) + vStart[0] + windX;
		iRandom = uir_weather_lcg(iRandom);
		vEnd[1] = (float)(iRandom % slant) + vStart[1] + windY;
		vEnd[2] = vOmins[2];

		speedVary = rain->speed_vary > 0 ? rain->speed_vary : 512;
		iLife = (int)((vStart[2] - vOmins[2]) / ((float)(iRandom % speedVary) + rain->speed) * 1000.0f);
		if (iLife <= 0) {
			continue;
		}
		if (iLife > 10000) {
			iLife = 10000;
		}

		shaderIndex = iRandom % shaderCount;
		if (shaderHandles[shaderIndex] <= 0) {
			continue;
		}

		VectorCopy(vStart, s_particles[slot].start);
		VectorCopy(vEnd, s_particles[slot].end);
		s_particles[slot].birth  = realtime;
		s_particles[slot].life   = iLife;
		s_particles[slot].shader = shaderHandles[shaderIndex];
		s_particles[slot].active = 1;
	}
}

static void uir_weather_draw_active(
	const uir_weather_params_t *rain,
	const float viewaxis[3][3],
	int realtime,
	const uir_menuworld_backend_t *backend)
{
	int   i;
	float streakLength;

	streakLength = rain->length > 0.0f ? rain->length : 90.0f;

	for (i = 0; i < UIR_MENU_WEATHER_MAX_PARTICLES; i++) {
		if (!s_particles[i].active) {
			continue;
		}
		uir_weather_draw_inverted_fast(
			backend, &s_particles[i], streakLength, rain->width, realtime, viewaxis);
	}
}

void UIR_MenuWeatherAddToScene(
	const uir_map_env_t *env,
	const float vieworg[3],
	const float viewaxis[3][3],
	int cgRainEnabled,
	int realtime,
	const uir_menuworld_backend_t *backend)
{
	int               shaderHandles[UIR_MAX_RAIN_SHADERS];
	int               shaderCount;
	int               i;
	uir_rain_volume_t cameraVolume;

	if (!env || !vieworg || !viewaxis || !backend) {
		return;
	}
	if (!cgRainEnabled || env->weather.density <= 0.0f) {
		return;
	}
	if (!backend->registerShader || !backend->addPolyToScene) {
		return;
	}

	if (Q_stricmp(s_particleBsp, env->bsp) != 0) {
		UIR_MenuWeatherInvalidate();
		Q_strncpyz(s_particleBsp, env->bsp, sizeof(s_particleBsp));
	}

	shaderCount = env->weather.numshaders > 0 ? env->weather.numshaders : 1;
	if (shaderCount > UIR_MAX_RAIN_SHADERS) {
		shaderCount = UIR_MAX_RAIN_SHADERS;
	}
	for (i = 0; i < shaderCount; i++) {
		const char *shaderName = env->weather.shader[i];
		if (!shaderName[0]) {
			shaderName = env->weather.shader[0];
		}
		shaderHandles[i] = shaderName[0] ? backend->registerShader(shaderName) : 0;
	}

	uir_weather_expire_particles(realtime);

	/* Fixed in OPM: menu cameras are fixed viewpoints, often outside func_rain brushes.
	 * Spawn around the catalog camera using script params, then animate like CG_AddBeams. */
	uir_weather_build_camera_volume(vieworg, &env->weather, &cameraVolume);
	uir_weather_spawn_new(env, &cameraVolume, vieworg, realtime, backend, shaderHandles, shaderCount);
	uir_weather_draw_active(&env->weather, viewaxis, realtime, backend);
}
