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

#include "cg_crosshair.h"

#include "cg_crosshair_spread.h"
#include "cg_local.h"
#include "crosshair_draw.h"

#include <math.h>
#include <string.h>

cvar_t *cg_crosshair_mode;
cvar_t *cg_crosshairsize;
cvar_t *cg_crosshairgap;
cvar_t *cg_crosshairthickness;
cvar_t *cg_crosshaircolor;
cvar_t *cg_crosshaircolor_r;
cvar_t *cg_crosshaircolor_g;
cvar_t *cg_crosshaircolor_b;
cvar_t *cg_crosshairalpha;
cvar_t *cg_crosshairusealpha;
cvar_t *cg_crosshairdot;
cvar_t *cg_crosshair_t;
cvar_t *cg_crosshair_drawoutline;
cvar_t *cg_crosshair_outlinethickness;
cvar_t *cg_crosshairgap_useweaponvalue;
cvar_t *cg_crosshair_recoil;
cvar_t *cg_crosshair_dynamic_splitdist;
cvar_t *cg_crosshair_dynamic_maxdist_splitratio;
cvar_t *cg_crosshair_dynamic_splitalpha_innermod;
cvar_t *cg_crosshair_friendly_warning;
cvar_t *cg_crosshair_sniper_thickness;
cvar_t *cg_crosshair_sniper_gap;
cvar_t *cg_crosshair_sniper_size;
cvar_t *cg_crosshair_sniper_t;
cvar_t *cg_crosshair_sniper_modern;
cvar_t *cg_crosshair_sniper_show_normal_inaccuracy;
cvar_t *cg_crosshair_solid_size;
cvar_t *cg_crosshair_dot_size;
cvar_t *cg_crosshair_dynamic;

extern cvar_t *cg_crosshair_dynamic_movement;
extern cvar_t *cg_crosshair_dynamic_scale;

static int cg_xhair_cvar_int(cvar_t *cv, int fallback)
{
	return cv ? cv->integer : fallback;
}

static float cg_xhair_cvar_float(cvar_t *cv, float fallback)
{
	return cv ? cv->value : fallback;
}

static qboolean cg_xhair_cvar_bool(cvar_t *cv, qboolean fallback)
{
	if (!cv) {
		return fallback;
	}
	return cv->integer ? qtrue : qfalse;
}

static void CG_Crosshair_ReadConfig(xhair_config_t *cfg)
{
	const char *modeStr;
	int colorPreset;

	if (!cfg) {
		return;
	}

	memset(cfg, 0, sizeof(*cfg));

	modeStr = cg_crosshair_mode ? cg_crosshair_mode->string : "open";
	cfg->mode = XHair_ParseMode(modeStr);
	cfg->style = 0;
	cfg->length = cg_xhair_cvar_float(cg_crosshairsize, 5.0f);
	cfg->gap = cg_xhair_cvar_float(cg_crosshairgap, -1.0f);
	cfg->thickness = cg_xhair_cvar_float(cg_crosshairthickness, 1.0f);
	cfg->solidSize = cg_xhair_cvar_float(cg_crosshair_solid_size, 4.0f);
	cfg->dotRadius = cg_xhair_cvar_float(cg_crosshair_dot_size, 2.0f);
	cfg->centerDot = cg_xhair_cvar_bool(cg_crosshairdot, qfalse);
	cfg->tStyle = cg_xhair_cvar_bool(cg_crosshair_t, qfalse);
	cfg->drawOutline = cg_xhair_cvar_bool(cg_crosshair_drawoutline, qtrue);
	cfg->outline = cg_xhair_cvar_float(cg_crosshair_outlinethickness, 1.0f);
	cfg->dynamicEnabled = cg_xhair_cvar_bool(cg_crosshair_dynamic, qfalse);
	cfg->splitDist = cg_xhair_cvar_float(cg_crosshair_dynamic_splitdist, 7.0f);
	cfg->splitRatio = cg_xhair_cvar_float(cg_crosshair_dynamic_maxdist_splitratio, 0.35f);
	cfg->splitInnerAlphaMod = cg_xhair_cvar_float(cg_crosshair_dynamic_splitalpha_innermod, 1.0f);
	cfg->splitOuterAlphaMod = 0.5f;
	cfg->dynamicSpreadPx = 0.0f;
	cfg->dynamicScale = cg_xhair_cvar_float(cg_crosshair_dynamic_scale, 1.0f);

	colorPreset = cg_xhair_cvar_int(cg_crosshaircolor, 5);
	if (colorPreset != 5) {
		colorPreset = 5;
	}
	XHair_ResolveColor(
		colorPreset,
		(float)cg_xhair_cvar_int(cg_crosshaircolor_r, 255),
		(float)cg_xhair_cvar_int(cg_crosshaircolor_g, 255),
		(float)cg_xhair_cvar_int(cg_crosshaircolor_b, 255),
		cg_xhair_cvar_bool(cg_crosshairusealpha, qtrue),
		cg_xhair_cvar_int(cg_crosshairalpha, 255),
		&cfg->r,
		&cfg->g,
		&cfg->b,
		&cfg->a
	);
}

typedef struct {
	const char *cgName;
	const char *clName;
	const char *defaultValue;
} xhair_cl_alias_t;

static const xhair_cl_alias_t g_xhairClAliases[] = {
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

static void cg_crosshair_register_alias_pair(const char *cgName, const char *clName, const char *defaultValue, int flags)
{
	cgi.Cvar_Get(cgName, defaultValue, flags);
	cgi.Cvar_Get(clName, defaultValue, flags);
}

void CG_Crosshair_RegisterCvars(void)
{
	const int flags = CVAR_ARCHIVE;
	int i;

	cg_crosshair_mode = cgi.Cvar_Get("cg_crosshair_mode", "open", flags);
	cg_crosshairsize = cgi.Cvar_Get("cg_crosshairsize", "5", flags);
	cg_crosshairgap = cgi.Cvar_Get("cg_crosshairgap", "-1", flags);
	cg_crosshairthickness = cgi.Cvar_Get("cg_crosshairthickness", "1", flags);
	/* Preset 5 = custom RGB from sliders (no preset picker in UI). */
	cg_crosshaircolor = cgi.Cvar_Get("cg_crosshaircolor", "5", flags);
	cg_crosshaircolor_r = cgi.Cvar_Get("cg_crosshaircolor_r", "255", flags);
	cg_crosshaircolor_g = cgi.Cvar_Get("cg_crosshaircolor_g", "255", flags);
	cg_crosshaircolor_b = cgi.Cvar_Get("cg_crosshaircolor_b", "255", flags);
	cg_crosshairalpha = cgi.Cvar_Get("cg_crosshairalpha", "255", flags);
	cg_crosshairusealpha = cgi.Cvar_Get("cg_crosshairusealpha", "1", flags);
	cg_crosshairdot = cgi.Cvar_Get("cg_crosshairdot", "0", flags);
	cg_crosshair_t = cgi.Cvar_Get("cg_crosshair_t", "0", flags);
	cg_crosshair_drawoutline = cgi.Cvar_Get("cg_crosshair_drawoutline", "1", flags);
	cg_crosshair_outlinethickness = cgi.Cvar_Get("cg_crosshair_outlinethickness", "1", flags);
	cg_crosshairgap_useweaponvalue = cgi.Cvar_Get("cg_crosshairgap_useweaponvalue", "0", flags);
	cg_crosshair_recoil = cgi.Cvar_Get("cg_crosshair_recoil", "0", flags);
	cg_crosshair_dynamic_splitdist = cgi.Cvar_Get("cg_crosshair_dynamic_splitdist", "7", flags);
	cg_crosshair_dynamic_maxdist_splitratio = cgi.Cvar_Get("cg_crosshair_dynamic_maxdist_splitratio", "0.35", flags);
	cg_crosshair_dynamic_splitalpha_innermod =
		cgi.Cvar_Get("cg_crosshair_dynamic_splitalpha_innermod", "1", flags);
	cg_crosshair_friendly_warning = cgi.Cvar_Get("cg_crosshair_friendly_warning", "1", flags);
	/* Added in OPM: modern sniper zoom open crosshair (UI px). */
	cg_crosshair_sniper_thickness = cgi.Cvar_Get("cg_crosshair_sniper_thickness", "3", flags);
	cg_crosshair_sniper_gap = cgi.Cvar_Get("cg_crosshair_sniper_gap", "0", flags);
	cg_crosshair_sniper_size = cgi.Cvar_Get("cg_crosshair_sniper_size", "5", flags);
	cg_crosshair_sniper_t = cgi.Cvar_Get("cg_crosshair_sniper_t", "0", flags);
	/* Added in OPM: 1 = modern uirender sniper scope; 0 = retail PK3 zoom overlays. */
	cg_crosshair_sniper_modern = cgi.Cvar_Get("cg_crosshair_sniper_modern", "1", flags);
	cg_crosshair_sniper_show_normal_inaccuracy =
		cgi.Cvar_Get("cg_crosshair_sniper_show_normal_inaccuracy", "0", flags);
	cg_crosshair_solid_size = cgi.Cvar_Get("cg_crosshair_solid_size", "4", flags);
	cg_crosshair_dot_size = cgi.Cvar_Get("cg_crosshair_dot_size", "2", flags);
	cg_crosshair_dynamic = cgi.Cvar_Get("cg_crosshair_dynamic", "0", flags);

	CG_CrosshairSpread_RegisterCvars();

	for (i = 0; i < (int)(sizeof(g_xhairClAliases) / sizeof(g_xhairClAliases[0])); i++) {
		cg_crosshair_register_alias_pair(g_xhairClAliases[i].cgName, g_xhairClAliases[i].clName,
		                                 g_xhairClAliases[i].defaultValue, flags);
	}
}

void CG_Crosshair_SyncClAliases(void)
{
	int i;

	for (i = 0; i < (int)(sizeof(g_xhairClAliases) / sizeof(g_xhairClAliases[0])); i++) {
		cvar_t *cgVar;
		cvar_t *clVar;
		const char *cgValue;
		const char *clValue;

		cgVar = cgi.Cvar_Find(g_xhairClAliases[i].cgName);
		clVar = cgi.Cvar_Find(g_xhairClAliases[i].clName);
		if (!cgVar || !clVar) {
			continue;
		}
		cgValue = cgVar->string;
		clValue = clVar->string;
		if (Q_stricmp(cgValue, clValue)) {
			if (clVar->modified && !cgVar->modified) {
				cgi.Cvar_Set(g_xhairClAliases[i].cgName, clValue);
			} else {
				cgi.Cvar_Set(g_xhairClAliases[i].clName, cgValue);
			}
		}
	}
}

qboolean CG_Crosshair_ModeEnabled(void)
{
	if (!cg_crosshair_mode || !cg_crosshair_mode->string[0]) {
		return qfalse;
	}
	return Q_stricmp(cg_crosshair_mode->string, "none") ? qtrue : qfalse;
}

static void CG_Crosshair_ApplyFriendTint(xhair_config_t *cfg, qboolean friendTarget)
{
	if (!friendTarget || !cg_crosshair_friendly_warning || !cg_crosshair_friendly_warning->integer) {
		return;
	}
	cfg->r = 0.2f;
	cfg->g = 0.85f;
	cfg->b = 0.2f;
}

static void CG_Crosshair_ApplyRecoilOffset(float *cx, float *cy)
{
	float kickScale;

	if (!cg_crosshair_recoil || !cg_crosshair_recoil->integer) {
		return;
	}

	kickScale = 4.0f * cgs.uiHiResScale[1];
	*cy += cg.viewkick[0] * kickScale;
	*cx += cg.viewkick[1] * kickScale * cgs.uiHiResScale[0] / cgs.uiHiResScale[1];
}

static void CG_Crosshair_DrawRects(const xhair_rect_t *rects, int count)
{
	int i;
	vec4_t color;
	float x;
	float y;
	float w;
	float h;

	for (i = 0; i < count; i++) {
		color[0] = rects[i].r;
		color[1] = rects[i].g;
		color[2] = rects[i].b;
		color[3] = rects[i].a;
		/* Changed in OPM: no floor snap — preserves fractional thickness (e.g. 1.5px). */
		x = rects[i].x;
		y = rects[i].y;
		w = rects[i].w;
		h = rects[i].h;
		if (w <= 0.0f || h <= 0.0f) {
			continue;
		}
		cgi.R_SetColor(color);
		cgi.R_DrawBox(x, y, w, h);
	}
	cgi.R_SetColor(NULL);
}

void CG_DrawModernCrosshair(qboolean friendTarget)
{
	xhair_config_t cfg;
	xhair_frame_t frame;
	xhair_rect_t rects[XHAIR_MAX_RECTS];
	float cx;
	float cy;
	int count;
	const float scaleX = 1.0f;
	const float scaleY = 1.0f;
	qboolean dynamicEnabled;
	qboolean movementEnabled;

	if (!CG_Crosshair_ModeEnabled()) {
		return;
	}

	CG_Crosshair_ReadConfig(&cfg);
	if (cfg.mode == XHAIR_MODE_NONE) {
		return;
	}

	dynamicEnabled = cg_xhair_cvar_bool(cg_crosshair_dynamic, qfalse);
	movementEnabled = cg_xhair_cvar_bool(cg_crosshair_dynamic_movement, qtrue);
	cfg.dynamicEnabled = dynamicEnabled;
	if (dynamicEnabled) {
		float fovX;

		fovX = cg.refdef.fov_x;
		if (fovX <= 0.0f) {
			fovX = cg_xhair_cvar_float(cg_fov, 80.0f);
		}
		cfg.dynamicSpreadPx =
			CG_CrosshairSpread_Update(dynamicEnabled, movementEnabled, fovX, (float)cgs.glconfig.vidHeight);
	} else {
		cfg.dynamicSpreadPx = 0.0f;
	}

	CG_Crosshair_ApplyFriendTint(&cfg, friendTarget);

	cx = floorf(cgs.glconfig.vidWidth * 0.5f);
	cy = floorf(cgs.glconfig.vidHeight * 0.5f);
	CG_Crosshair_ApplyRecoilOffset(&cx, &cy);
	cx = floorf(cx);
	cy = floorf(cy);

	XHair_BuildFrame(&cfg, cx, cy, scaleX, scaleY, &frame);

	count = XHair_EmitRects(&frame, XHAIR_PASS_OUTLINE, rects, XHAIR_MAX_RECTS);
	CG_Crosshair_DrawRects(rects, count);

	count = XHair_EmitRects(&frame, XHAIR_PASS_FILL, rects, XHAIR_MAX_RECTS);
	CG_Crosshair_DrawRects(rects, count);
}
