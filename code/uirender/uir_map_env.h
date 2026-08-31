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
#ifndef UIR_MAP_ENV_H
#define UIR_MAP_ENV_H

#include "uir_types.h"

#include "../qcommon/q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UIR_MAX_RAIN_VOLUMES  32
#define UIR_MAX_RAIN_SHADERS  16

typedef struct {
	float density;
	float speed;
	int   speed_vary;
	int   slant;
	float length;
	float min_dist;
	float width;
	char  shader[UIR_MAX_RAIN_SHADERS][MAX_QPATH];
	char  currentShader[MAX_QPATH];
	int   numshaders;
} uir_weather_params_t;

typedef struct {
	vec3_t mins;
	vec3_t maxs;
} uir_rain_volume_t;

typedef struct {
	char bsp[MAX_QPATH];

	qboolean hasFarplane;
	float    farplane;
	float    farplane_bias;
	float    farplane_color[3];

	int               numRainVolumes;
	uir_rain_volume_t rainVolumes[UIR_MAX_RAIN_VOLUMES];
	uir_weather_params_t weather;
} uir_map_env_t;

typedef struct {
	const char *(*cmEntityString)(void);
	void (*cmModelBoundsFromName)(const char *name, vec3_t mins, vec3_t maxs);
	long (*readFile)(const char *path, void **buffer);
	void (*freeFile)(void *buffer);
	int (*fileExists)(const char *path);
} uir_map_env_backend_t;

void UIR_WeatherParamsSetDefaults(uir_weather_params_t *out);
void UIR_MapEnvSetFogDefaults(uir_map_env_t *env);
void UIR_MapEnvClear(void);

const uir_map_env_t *UIR_MapEnvActive(void);

uir_status_t UIR_MapEnvLoad(const char *bspPath, const uir_map_env_backend_t *backend);

/* Test / internal entry points */
void UIR_MapEnvParseEntities(const char *ents, const uir_map_env_backend_t *backend, uir_map_env_t *out);
void UIR_MapEnvParseScript(const char *scrText, uir_weather_params_t *out);
void UIR_WeatherParamsBuildShaderList(uir_weather_params_t *params);

#ifdef __cplusplus
}
#endif

#endif /* UIR_MAP_ENV_H */
