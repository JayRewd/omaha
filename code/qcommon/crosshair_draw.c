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

#include "crosshair_draw.h"

#include "q_shared.h"

#include <math.h>
#include <string.h>

#if !defined(CGAME_DLL) && !defined(GAME_DLL)
#include "qcommon.h"

static float xhair_cvar_float(const char *name, float fallback)
{
	const char *value;

	value = Cvar_VariableString(name);
	if (!value || !value[0]) {
		return fallback;
	}
	return (float)atof(value);
}

static int xhair_cvar_int(const char *name, int fallback)
{
	const char *value;

	value = Cvar_VariableString(name);
	if (!value || !value[0]) {
		return fallback;
	}
	return atoi(value);
}

static qboolean xhair_cvar_bool(const char *name, qboolean fallback)
{
	return xhair_cvar_int(name, fallback ? 1 : 0) ? qtrue : qfalse;
}
#endif

xhair_mode_t XHair_ParseMode(const char *mode)
{
	if (!mode || !mode[0]) {
		return XHAIR_MODE_OPEN;
	}
	if (!Q_stricmp(mode, "none")) {
		return XHAIR_MODE_NONE;
	}
	if (!Q_stricmp(mode, "solid")) {
		return XHAIR_MODE_SOLID;
	}
	if (!Q_stricmp(mode, "open")) {
		return XHAIR_MODE_OPEN;
	}
	if (!Q_stricmp(mode, "dot")) {
		return XHAIR_MODE_DOT;
	}
	return XHAIR_MODE_OPEN;
}

void XHair_ResolveColor(int preset, float customR, float customG, float customB, int useAlpha, int alpha,
                        float *outR, float *outG, float *outB, float *outA)
{
	static const float presetColors[5][3] = {
		{0.0f, 1.0f, 0.0f},
		{1.0f, 1.0f, 0.0f},
		{0.0f, 0.0f, 1.0f},
		{0.0f, 1.0f, 1.0f},
		{1.0f, 0.0f, 0.0f},
	};

	if (preset >= 0 && preset < 5) {
		*outR = presetColors[preset][0];
		*outG = presetColors[preset][1];
		*outB = presetColors[preset][2];
	} else {
		*outR = customR / 255.0f;
		*outG = customG / 255.0f;
		*outB = customB / 255.0f;
	}

	if (useAlpha) {
		*outA = alpha / 255.0f;
	} else {
		*outA = 1.0f;
	}
}

#if !defined(CGAME_DLL) && !defined(GAME_DLL)
void XHair_ReadConfigFromCvars(xhair_config_t *cfg)
{
	const char *modeStr;
	int colorPreset;

	if (!cfg) {
		return;
	}

	memset(cfg, 0, sizeof(*cfg));

	modeStr = Cvar_VariableString("cg_crosshair_mode");
	cfg->mode = XHair_ParseMode(modeStr);
	cfg->length = xhair_cvar_float("cg_crosshairsize", 5.0f);
	cfg->gap = xhair_cvar_float("cg_crosshairgap", -1.0f);
	cfg->thickness = xhair_cvar_float("cg_crosshairthickness", 1.0f);
	cfg->solidSize = xhair_cvar_float("cg_crosshair_solid_size", 4.0f);
	cfg->dotRadius = xhair_cvar_float("cg_crosshair_dot_size", 2.0f);
	cfg->centerDot = xhair_cvar_bool("cg_crosshairdot", qfalse);
	cfg->tStyle = xhair_cvar_bool("cg_crosshair_t", qfalse);
	cfg->drawOutline = xhair_cvar_bool("cg_crosshair_drawoutline", qtrue);
	cfg->outline = xhair_cvar_float("cg_crosshair_outlinethickness", 1.0f);
	cfg->dynamicEnabled = xhair_cvar_bool("cg_crosshair_dynamic", qfalse);
	cfg->splitDist = xhair_cvar_float("cg_crosshair_dynamic_splitdist", 7.0f);
	cfg->splitRatio = xhair_cvar_float("cg_crosshair_dynamic_maxdist_splitratio", 0.35f);
	cfg->splitInnerAlphaMod = xhair_cvar_float("cg_crosshair_dynamic_splitalpha_innermod", 1.0f);
	cfg->splitOuterAlphaMod = 0.5f;
	cfg->style = 0;
	cfg->dynamicSpreadPx = 0.0f;
	cfg->dynamicScale = xhair_cvar_float("cg_crosshair_dynamic_scale", 1.0f);

	colorPreset = xhair_cvar_int("cg_crosshaircolor", 5);
	if (colorPreset != 5) {
		colorPreset = 5;
	}
	XHair_ResolveColor(
		colorPreset,
		(float)xhair_cvar_int("cg_crosshaircolor_r", 255),
		(float)xhair_cvar_int("cg_crosshaircolor_g", 255),
		(float)xhair_cvar_int("cg_crosshaircolor_b", 255),
		xhair_cvar_bool("cg_crosshairusealpha", qtrue),
		xhair_cvar_int("cg_crosshairalpha", 255),
		&cfg->r,
		&cfg->g,
		&cfg->b,
		&cfg->a
	);
}
#endif

void XHair_ApplyStyleDynamic(xhair_config_t *cfg)
{
	if (!cfg || cfg->mode == XHAIR_MODE_NONE || !cfg->dynamicEnabled) {
		return;
	}

	if (cfg->mode == XHAIR_MODE_DOT) {
		cfg->dotRadius += cfg->dynamicSpreadPx * 0.5f;
		return;
	}

	if (cfg->mode == XHAIR_MODE_SOLID) {
		cfg->solidSize += cfg->dynamicSpreadPx * 0.5f;
	}
}

void XHair_BuildFrame(const xhair_config_t *cfg, float cx, float cy, float scaleX, float scaleY, xhair_frame_t *out)
{
	xhair_config_t working;

	if (!cfg || !out) {
		return;
	}

	working = *cfg;
	if (working.dynamicScale < 0.0f) {
		working.dynamicScale = 0.0f;
	}
	working.dynamicSpreadPx *= working.dynamicScale;
	XHair_ApplyStyleDynamic(&working);

	memset(out, 0, sizeof(*out));
	out->mode = working.mode;
	out->cx = cx;
	out->cy = cy;
	out->scaleX = scaleX;
	out->scaleY = scaleY;
	out->gap = working.gap;
	out->length = working.length;
	out->thickness = working.thickness;
	out->dotRadius = working.dotRadius;
	out->solidSize = working.solidSize;
	out->r = working.r;
	out->g = working.g;
	out->b = working.b;
	out->a = working.a;
	out->outline = working.outline;
	out->drawOutline = working.drawOutline;
	out->tStyle = working.tStyle;
	out->centerDot = working.centerDot;
	out->dynamicEnabled = working.dynamicEnabled;
	out->style = working.style;
	out->splitDist = working.splitDist;
	out->splitRatio = working.splitRatio;
	out->splitInnerAlphaMod = working.splitInnerAlphaMod;
	out->splitOuterAlphaMod = working.splitOuterAlphaMod;
	out->dynamicSpreadPx = working.dynamicSpreadPx;
	out->dynamicScale = working.dynamicScale;
}

static int xhair_push_rect(
	xhair_rect_t *out,
	int maxOut,
	int count,
	float x,
	float y,
	float w,
	float h,
	float r,
	float g,
	float b,
	float a,
	float outline,
	float scaleX,
	float scaleY,
	xhair_pass_t pass
)
{
	if (count >= maxOut || w <= 0.0f || h <= 0.0f) {
		return count;
	}

	if (pass == XHAIR_PASS_OUTLINE) {
		if (outline <= 0.0f) {
			return count;
		}
		{
			const int ox = (int)ceilf(outline * scaleX);
			const int oy = (int)ceilf(outline * scaleY);
			x -= (float)ox;
			y -= (float)oy;
			w += (float)(ox * 2);
			h += (float)(oy * 2);
		}
		r = 0.0f;
		g = 0.0f;
		b = 0.0f;
	}

	out[count].x = x;
	out[count].y = y;
	out[count].w = w;
	out[count].h = h;
	out[count].r = r;
	out[count].g = g;
	out[count].b = b;
	out[count].a = a;
	return count + 1;
}

static int xhair_push_rect_direct(
	xhair_rect_t *out,
	int maxOut,
	int count,
	float x,
	float y,
	float w,
	float h,
	float r,
	float g,
	float b,
	float a
)
{
	if (count >= maxOut || w <= 0.0f || h <= 0.0f) {
		return count;
	}

	out[count].x = x;
	out[count].y = y;
	out[count].w = w;
	out[count].h = h;
	out[count].r = r;
	out[count].g = g;
	out[count].b = b;
	out[count].a = a;
	return count + 1;
}

static int xhair_emit_filled_circle(
	xhair_rect_t *out,
	int maxOut,
	int count,
	int cx,
	int cy,
	int radius,
	const xhair_frame_t *frame,
	xhair_pass_t pass,
	float alphaScale
)
{
	int y;
	float r;
	float g;
	float b;
	float a;

	if (radius < 1) {
		radius = 1;
	}

	if (pass == XHAIR_PASS_OUTLINE) {
		if (!frame->drawOutline || frame->outline <= 0.0f) {
			return count;
		}
		radius += (int)ceilf(frame->outline * frame->scaleX);
		r = 0.0f;
		g = 0.0f;
		b = 0.0f;
		a = frame->a * alphaScale;
	} else {
		r = frame->r;
		g = frame->g;
		b = frame->b;
		a = frame->a * alphaScale;
	}

	for (y = -radius; y <= radius; y++) {
		const int y2 = y * y;
		const int r2 = radius * radius;
		int dx;

		if (y2 > r2) {
			continue;
		}
		dx = (int)floorf(sqrtf((float)(r2 - y2)));
		count = xhair_push_rect_direct(
			out,
			maxOut,
			count,
			(float)(cx - dx),
			(float)(cy + y),
			(float)(dx * 2 + 1),
			1.0f,
			r,
			g,
			b,
			a
		);
	}

	return count;
}

static int xhair_emit_bar(
	xhair_rect_t *out,
	int maxOut,
	int count,
	float x,
	float y,
	float w,
	float h,
	const xhair_frame_t *frame,
	xhair_pass_t pass,
	float alphaScale
)
{
	return xhair_push_rect(
		out,
		maxOut,
		count,
		x,
		y,
		w,
		h,
		frame->r,
		frame->g,
		frame->b,
		frame->a * alphaScale,
		frame->outline,
		frame->scaleX,
		frame->scaleY,
		pass
	);
}

static int xhair_emit_open_arms(
	xhair_rect_t *out,
	int maxOut,
	int count,
	const xhair_frame_t *frame,
	xhair_pass_t pass,
	float gap,
	float len,
	float th,
	float alphaScale
)
{
	/* Changed in OPM: float thickness/gap/length (no integer floor) so 1.5px works. */
	const float cx = frame->cx;
	const float cy = frame->cy;
	const float barY = cy - th * 0.5f;
	const float barX = cx - th * 0.5f;

	count = xhair_emit_bar(out, maxOut, count, cx + gap, barY, len, th, frame, pass, alphaScale);
	count = xhair_emit_bar(out, maxOut, count, cx - gap - len, barY, len, th, frame, pass, alphaScale);
	count = xhair_emit_bar(out, maxOut, count, barX, cy + gap, th, len, frame, pass, alphaScale);
	if (!frame->tStyle) {
		count = xhair_emit_bar(
			out, maxOut, count, barX, cy - gap - len, th, len, frame, pass, alphaScale
		);
	}
	return count;
}

static int xhair_emit_open(
	xhair_rect_t *out,
	int maxOut,
	int count,
	const xhair_frame_t *frame,
	xhair_pass_t pass
)
{
	float dynamicGap;
	float th;
	float gap;
	float len;

	dynamicGap = frame->gap;
	if (frame->dynamicEnabled) {
		dynamicGap += frame->dynamicSpreadPx;
	}

	/* Changed in OPM: keep authored thickness as float FB size (no floor/min-1). */
	th = frame->thickness * frame->scaleX;
	gap = dynamicGap * frame->scaleX;
	len = frame->length * frame->scaleX;

	count = xhair_emit_open_arms(out, maxOut, count, frame, pass, gap, len, th, 1.0f);

	if (frame->centerDot) {
		count = xhair_emit_bar(
			out,
			maxOut,
			count,
			frame->cx - th * 0.5f,
			frame->cy - th * 0.5f,
			th,
			th,
			frame,
			pass,
			1.0f
		);
	}

	return count;
}

int XHair_EmitRects(const xhair_frame_t *frame, xhair_pass_t pass, xhair_rect_t *out, int maxOut)
{
	int count = 0;

	if (!frame || !out || maxOut <= 0 || frame->mode == XHAIR_MODE_NONE) {
		return 0;
	}

	if (pass == XHAIR_PASS_OUTLINE && (!frame->drawOutline || frame->outline <= 0.0f)) {
		return 0;
	}

	switch (frame->mode) {
	case XHAIR_MODE_SOLID: {
		const float solidSize = frame->solidSize * frame->scaleX;
		const float solidSizeY = frame->solidSize * frame->scaleY;
		const float thX = frame->thickness * frame->scaleX;
		const float thY = frame->thickness * frame->scaleY;

		count = xhair_emit_bar(
			out, maxOut, count, frame->cx - solidSize, frame->cy - thY * 0.5f, solidSize * 2.0f, thY, frame, pass, 1.0f
		);
		count = xhair_emit_bar(
			out, maxOut, count, frame->cx - thX * 0.5f, frame->cy - solidSizeY, thX, solidSizeY * 2.0f, frame, pass,
			1.0f
		);
		break;
	}
	case XHAIR_MODE_OPEN:
		count = xhair_emit_open(out, maxOut, count, frame, pass);
		break;
	case XHAIR_MODE_DOT: {
		const int icx = (int)floorf(frame->cx);
		const int icy = (int)floorf(frame->cy);
		const int radius = (int)floorf(frame->dotRadius * 0.5f * frame->scaleX);

		count = xhair_emit_filled_circle(out, maxOut, count, icx, icy, radius, frame, pass, 1.0f);
		break;
	}
	default:
		break;
	}

	return count;
}

#if !defined(CGAME_DLL) && !defined(GAME_DLL)
typedef struct {
	const char *cgName;
	const char *clName;
	const char *defaultValue;
} xhair_alias_def_t;

static const xhair_alias_def_t g_xhairAliasDefs[] = {
	{"cg_crosshairsize", "cl_crosshairsize", "5"},
	{"cg_crosshairgap", "cl_crosshairgap", "-1"},
	{"cg_crosshairthickness", "cl_crosshairthickness", "1"},
	{"cg_crosshaircolor", "cl_crosshaircolor", "5"},
	{"cg_crosshaircolor_r", "cl_crosshaircolor_r", "255"},
	{"cg_crosshaircolor_g", "cl_crosshaircolor_g", "255"},
	{"cg_crosshaircolor_b", "cl_crosshaircolor_b", "255"},
	{"cg_crosshairalpha", "cl_crosshairalpha", "255"},
	{"cg_crosshairusealpha", "cl_crosshairusealpha", "1"},
	{"cg_crosshairdot", "cl_crosshairdot", "0"},
	{"cg_crosshair_t", "cl_crosshair_t", "0"},
	{"cg_crosshair_drawoutline", "cl_crosshair_drawoutline", "1"},
	{"cg_crosshair_outlinethickness", "cl_crosshair_outlinethickness", "1"},
	{"cg_crosshairgap_useweaponvalue", "cl_crosshairgap_useweaponvalue", "0"},
	{"cg_crosshair_recoil", "cl_crosshair_recoil", "0"},
	{"cg_crosshair_dynamic_splitdist", "cl_crosshair_dynamic_splitdist", "7"},
	{"cg_crosshair_dynamic_maxdist_splitratio", "cl_crosshair_dynamic_maxdist_splitratio", "0.35"},
	{"cg_crosshair_dynamic_splitalpha_innermod", "cl_crosshair_dynamic_splitalpha_innermod", "1"},
	{"cg_crosshair_friendly_warning", "cl_crosshair_friendly_warning", "1"},
	{"cg_crosshair_sniper_thickness", "cl_crosshair_sniper_thickness", "3"},
	{"cg_crosshair_sniper_gap", "cl_crosshair_sniper_gap", "0"},
	{"cg_crosshair_sniper_size", "cl_crosshair_sniper_size", "5"},
	{"cg_crosshair_sniper_t", "cl_crosshair_sniper_t", "0"},
	{"cg_crosshair_sniper_modern", "cl_crosshair_sniper_modern", "1"},
	{"cg_crosshair_sniper_show_normal_inaccuracy", "cl_crosshair_sniper_show_normal_inaccuracy", "0"},
};

static void xhair_register_alias_pair(const char *cgName, const char *clName, const char *defaultValue, int flags)
{
	Cvar_Get(cgName, defaultValue, flags);
	Cvar_Get(clName, defaultValue, flags);
}

void XHair_RegisterClientCvars(void)
{
	const int flags = CVAR_ARCHIVE;
	int i;

	Cvar_Get("cg_crosshair_mode", "open", flags);
	Cvar_Get("cg_crosshairsize", "5", flags);
	Cvar_Get("cg_crosshairgap", "-1", flags);
	Cvar_Get("cg_crosshairthickness", "1", flags);
	/* Preset 5 = custom RGB from sliders (no preset picker in UI). */
	Cvar_Get("cg_crosshaircolor", "5", flags);
	Cvar_Get("cg_crosshaircolor_r", "255", flags);
	Cvar_Get("cg_crosshaircolor_g", "255", flags);
	Cvar_Get("cg_crosshaircolor_b", "255", flags);
	Cvar_Get("cg_crosshairalpha", "255", flags);
	Cvar_Get("cg_crosshairusealpha", "1", flags);
	Cvar_Get("cg_crosshairdot", "0", flags);
	Cvar_Get("cg_crosshair_t", "0", flags);
	Cvar_Get("cg_crosshair_drawoutline", "1", flags);
	Cvar_Get("cg_crosshair_outlinethickness", "1", flags);
	Cvar_Get("cg_crosshairgap_useweaponvalue", "0", flags);
	Cvar_Get("cg_crosshair_recoil", "0", flags);
	Cvar_Get("cg_crosshair_dynamic_splitdist", "7", flags);
	Cvar_Get("cg_crosshair_dynamic_maxdist_splitratio", "0.35", flags);
	Cvar_Get("cg_crosshair_dynamic_splitalpha_innermod", "1", flags);
	Cvar_Get("cg_crosshair_friendly_warning", "1", flags);
	/* Added in OPM: modern sniper zoom open crosshair (UI px). */
	Cvar_Get("cg_crosshair_sniper_thickness", "3", flags);
	Cvar_Get("cg_crosshair_sniper_gap", "0", flags);
	Cvar_Get("cg_crosshair_sniper_size", "5", flags);
	Cvar_Get("cg_crosshair_sniper_t", "0", flags);
	Cvar_Get("cg_crosshair_sniper_modern", "1", flags);
	Cvar_Get("cg_crosshair_sniper_show_normal_inaccuracy", "0", flags);
	Cvar_Get("cg_crosshair_solid_size", "4", flags);
	Cvar_Get("cg_crosshair_dot_size", "2", flags);
	Cvar_Get("cg_crosshair_dynamic", "0", flags);
	Cvar_Get("cg_crosshair_dynamic_movement", "1", flags);
	Cvar_Get("cg_crosshair_dynamic_scale", "1", flags);

	for (i = 0; i < (int)(sizeof(g_xhairAliasDefs) / sizeof(g_xhairAliasDefs[0])); i++) {
		xhair_register_alias_pair(g_xhairAliasDefs[i].cgName, g_xhairAliasDefs[i].clName, g_xhairAliasDefs[i].defaultValue,
		                          flags);
	}
}

void XHair_ClientSyncClAliases(void)
{
	int i;

	for (i = 0; i < (int)(sizeof(g_xhairAliasDefs) / sizeof(g_xhairAliasDefs[0])); i++) {
		cvar_t *cgVar;
		cvar_t *clVar;

		cgVar = Cvar_FindVar(g_xhairAliasDefs[i].cgName);
		clVar = Cvar_FindVar(g_xhairAliasDefs[i].clName);
		if (!cgVar || !clVar) {
			continue;
		}
		if (Q_stricmp(cgVar->string, clVar->string)) {
			if (clVar->modified && !cgVar->modified) {
				Cvar_Set(g_xhairAliasDefs[i].cgName, clVar->string);
			} else {
				Cvar_Set(g_xhairAliasDefs[i].clName, cgVar->string);
			}
		}
	}
}
#endif
