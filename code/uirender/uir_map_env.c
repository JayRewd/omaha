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

#include "uir_map_env.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static uir_map_env_t g_mapEnv;

void UIR_WeatherParamsSetDefaults(uir_weather_params_t *out)
{
	int i;

	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	out->speed      = 2048.0f;
	out->speed_vary = 512;
	out->slant      = 50;
	out->length     = 90.0f;
	out->min_dist   = 512.0f;
	out->width      = 1.0f;
	Q_strncpyz(out->currentShader, "textures/rain", sizeof(out->currentShader));
	Q_strncpyz(out->shader[0], "textures/rain", sizeof(out->shader[0]));
	for (i = 1; i < UIR_MAX_RAIN_SHADERS; i++) {
		out->shader[i][0] = '\0';
	}
}

void UIR_MapEnvSetFogDefaults(uir_map_env_t *env)
{
	if (!env) {
		return;
	}
	env->hasFarplane = qtrue;
	env->farplane    = 7000.0f;
	env->farplane_bias = 7000.0f * 0.18f;
	env->farplane_color[0] = 0.5f;
	env->farplane_color[1] = 0.4f;
	env->farplane_color[2] = 0.2f;
}

void UIR_MapEnvClear(void)
{
	memset(&g_mapEnv, 0, sizeof(g_mapEnv));
}

const uir_map_env_t *UIR_MapEnvActive(void)
{
	return &g_mapEnv;
}

void UIR_WeatherParamsBuildShaderList(uir_weather_params_t *params)
{
	int i;

	if (!params) {
		return;
	}

	if (params->numshaders > 0) {
		size_t len = strlen(params->currentShader);
		if (len > 0 && isdigit((unsigned char)params->currentShader[len - 1])) {
			params->currentShader[len - 1] = '\0';
		}
		if (params->numshaders > UIR_MAX_RAIN_SHADERS) {
			params->numshaders = UIR_MAX_RAIN_SHADERS;
		}
		for (i = 0; i < params->numshaders; i++) {
			Com_sprintf(params->shader[i], sizeof(params->shader[i]), "%s%i", params->currentShader, i);
		}
	} else {
		Q_strncpyz(params->shader[0], params->currentShader, sizeof(params->shader[0]));
		for (i = 1; i < UIR_MAX_RAIN_SHADERS; i++) {
			params->shader[i][0] = '\0';
		}
	}
}

static void uir_env_script_path_from_bsp(const char *bsp, char *out, size_t outSize)
{
	char *dot;

	if (!bsp || !out || outSize == 0) {
		return;
	}
	Q_strncpyz(out, bsp, outSize);
	dot = strrchr(out, '.');
	if (dot) {
		Q_strncpyz(dot, ".scr", outSize - (size_t)(dot - out));
	}
}

static void uir_env_trim_line(char *line)
{
	char *start;
	char *end;

	if (!line) {
		return;
	}
	start = line;
	while (*start == ' ' || *start == '\t') {
		start++;
	}
	if (start != line) {
		memmove(line, start, strlen(start) + 1);
	}
	end = line + strlen(line);
	while (end > line && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) {
		end--;
	}
	*end = '\0';
}

static void uir_env_parse_script_assignment(const char *line, uir_weather_params_t *out)
{
	const char *eq;
	char        key[64];
	char        value[128];
	size_t      keyLen;

	if (!line || !out || !line[0]) {
		return;
	}
	if (strncmp(line, "level.rain_", 11) != 0) {
		return;
	}
	eq = strchr(line, '=');
	if (!eq) {
		return;
	}
	keyLen = (size_t)(eq - line);
	if (keyLen >= sizeof(key)) {
		return;
	}
	memcpy(key, line, keyLen);
	key[keyLen] = '\0';
	uir_env_trim_line(key);

	Q_strncpyz(value, eq + 1, sizeof(value));
	uir_env_trim_line(value);
	if (value[0] == '"') {
		size_t vlen = strlen(value);
		if (vlen >= 2 && value[vlen - 1] == '"') {
			value[vlen - 1] = '\0';
			memmove(value, value + 1, vlen - 1);
		}
	}

	if (!strcmp(key, "level.rain_density")) {
		out->density = (float)atof(value);
	} else if (!strcmp(key, "level.rain_speed")) {
		out->speed = (float)atof(value);
	} else if (!strcmp(key, "level.rain_speed_vary")) {
		out->speed_vary = atoi(value);
	} else if (!strcmp(key, "level.rain_slant")) {
		out->slant = atoi(value);
	} else if (!strcmp(key, "level.rain_length")) {
		out->length = (float)atof(value);
	} else if (!strcmp(key, "level.rain_min_dist")) {
		out->min_dist = (float)atof(value);
	} else if (!strcmp(key, "level.rain_width")) {
		out->width = (float)atof(value);
	} else if (!strcmp(key, "level.rain_shader")) {
		Q_strncpyz(out->currentShader, value, sizeof(out->currentShader));
	} else if (!strcmp(key, "level.rain_numshaders")) {
		out->numshaders = atoi(value);
	}
}

void UIR_MapEnvParseScript(const char *scrText, uir_weather_params_t *out)
{
	char        line[256];
	const char *cursor;
	const char *lineStart;

	if (!out) {
		return;
	}
	UIR_WeatherParamsSetDefaults(out);
	if (!scrText) {
		return;
	}

	cursor = scrText;
	while (*cursor) {
		size_t lineLen = 0;

		lineStart = cursor;
		while (*cursor && *cursor != '\n' && *cursor != '\r') {
			cursor++;
		}
		lineLen = (size_t)(cursor - lineStart);
		if (lineLen >= sizeof(line)) {
			lineLen = sizeof(line) - 1;
		}
		memcpy(line, lineStart, lineLen);
		line[lineLen] = '\0';
		while (*cursor == '\n' || *cursor == '\r') {
			cursor++;
		}

		uir_env_trim_line(line);
		if (line[0] == '/' && line[1] == '/') {
			continue;
		}
		uir_env_parse_script_assignment(line, out);
	}

	UIR_WeatherParamsBuildShaderList(out);
}

static void uir_env_note_rain_volume(uir_map_env_t *env)
{
	if (!env || env->numRainVolumes >= UIR_MAX_RAIN_VOLUMES) {
		return;
	}
	env->numRainVolumes++;
}

static void uir_env_add_rain_volume(
	uir_map_env_t *env,
	const uir_map_env_backend_t *backend,
	const char *model,
	const float origin[3])
{
	(void)backend;
	(void)origin;

	if (!env || !model || !model[0] || model[0] != '*') {
		return;
	}
	/* Fixed in OPM: menu weather gates on rain volume count only; bounds unused. */
	uir_env_note_rain_volume(env);
}

void UIR_MapEnvParseEntities(const char *ents, const uir_map_env_backend_t *backend, uir_map_env_t *out)
{
	char *cursor;
	char        key[128];
	char        value[256];
	char        entityClass[128];
	qboolean    inWorldspawn;
	char        rainModel[64];
	float       rainOrigin[3];
	int         rainHasModel;
	int         rainHasOrigin;
	char        savedBsp[MAX_QPATH];

	if (!out) {
		return;
	}

	Q_strncpyz(savedBsp, out->bsp, sizeof(savedBsp));
	memset(out, 0, sizeof(*out));
	Q_strncpyz(out->bsp, savedBsp, sizeof(out->bsp));
	out->numRainVolumes = 0;
	UIR_WeatherParamsSetDefaults(&out->weather);

	if (!ents) {
		UIR_MapEnvSetFogDefaults(out);
		return;
	}

	cursor     = (char *)ents;
	inWorldspawn = qfalse;
	entityClass[0] = '\0';
	rainHasModel = 0;
	rainHasOrigin = 0;
	rainModel[0] = '\0';

	while (1) {
		const char *token = COM_Parse(&cursor);
		if (!cursor) {
			break;
		}
		if (token[0] == '{') {
			inWorldspawn = qfalse;
			entityClass[0] = '\0';
			rainHasModel = 0;
			rainHasOrigin = 0;
			rainModel[0] = '\0';
			continue;
		}
		if (token[0] == '}') {
			/* Fixed in OPM: MOHAA entities often list model/origin before classname. */
			if (!Q_stricmp(entityClass, "func_rain") && rainHasModel) {
				if (!rainHasOrigin) {
					rainOrigin[0] = rainOrigin[1] = rainOrigin[2] = 0.0f;
				}
				uir_env_add_rain_volume(out, backend, rainModel, rainOrigin);
			}
			inWorldspawn = qfalse;
			entityClass[0] = '\0';
			rainHasModel = 0;
			rainHasOrigin = 0;
			rainModel[0] = '\0';
			continue;
		}
		if (!token[0]) {
			continue;
		}

		Q_strncpyz(key, token, sizeof(key));
		token = COM_Parse(&cursor);
		if (!cursor) {
			break;
		}
		Q_strncpyz(value, token, sizeof(value));

		if (!strcmp(key, "classname")) {
			Q_strncpyz(entityClass, value, sizeof(entityClass));
			inWorldspawn = !Q_stricmp(value, "worldspawn");
			continue;
		}

		if (inWorldspawn) {
			if (!strcmp(key, "farplane")) {
				out->farplane = (float)atof(value);
				out->hasFarplane = qtrue;
			} else if (!strcmp(key, "farplane_color")) {
				sscanf(value, "%f %f %f", &out->farplane_color[0], &out->farplane_color[1], &out->farplane_color[2]);
			} else if (!strcmp(key, "farplane_bias")) {
				out->farplane_bias = (float)atof(value);
			}
		} else if (!strcmp(key, "model") && value[0] == '*') {
			Q_strncpyz(rainModel, value, sizeof(rainModel));
			rainHasModel = 1;
		} else if (!strcmp(key, "origin")) {
			sscanf(value, "%f %f %f", &rainOrigin[0], &rainOrigin[1], &rainOrigin[2]);
			rainHasOrigin = 1;
		}
	}

	if (!out->hasFarplane) {
		UIR_MapEnvSetFogDefaults(out);
	} else if (out->farplane_bias <= 0.001f) {
		out->farplane_bias = out->farplane * 0.18f;
	}
}

uir_status_t UIR_MapEnvLoad(const char *bspPath, const uir_map_env_backend_t *backend)
{
	char        scrPath[MAX_QPATH];
	void       *scrBuf = NULL;
	long        scrLen;
	const char *ents;

	if (!bspPath || !bspPath[0] || !backend) {
		return UIR_ERR_INVALID_ARG;
	}

	memset(&g_mapEnv, 0, sizeof(g_mapEnv));
	Q_strncpyz(g_mapEnv.bsp, bspPath, sizeof(g_mapEnv.bsp));
	UIR_WeatherParamsSetDefaults(&g_mapEnv.weather);

	if (backend->cmEntityString) {
		ents = backend->cmEntityString();
		UIR_MapEnvParseEntities(ents, backend, &g_mapEnv);
	} else {
		UIR_MapEnvSetFogDefaults(&g_mapEnv);
	}

	uir_env_script_path_from_bsp(bspPath, scrPath, sizeof(scrPath));
	scrLen = 0;
	if (backend->readFile) {
		scrLen = backend->readFile(scrPath, &scrBuf);
		if (scrLen > 0 && scrBuf) {
			UIR_MapEnvParseScript((const char *)scrBuf, &g_mapEnv.weather);
		}
		if (scrBuf && backend->freeFile) {
			backend->freeFile(scrBuf);
		}
	}

	return UIR_OK;
}
