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

#include "uir_menuworld.h"
#include "uir_fov.h"
#include "uir_map_env.h"
#include "uir_menu_weather.h"

#include "../qcommon/q_shared.h"
#include "../renderercommon/tr_types.h"

#include <string.h>

static uir_menuworld_backend_t g_mw;
static uir_menuworld_state_t   g_mwState = UIR_MW_UNLOADED;
static int                     g_mwLoggedMissing = 0;
static int                     g_mwLoggedSwitchFail = 0;
/* Fixed in OPM: only clearWorld when this module loaded the shared slot. */
static int                     g_mwOwnsWorld = 0;
static uir_menu_map_view_t     g_mwDesiredView;
static uir_menu_map_view_t     g_mwActiveView;
static int                     g_mwDesiredDirty = 1;
/* Fixed in OPM: skip draw on the frame that loads/switches the menu world (renderer flush mid-frame). */
static int                     g_mwDeferDraw = 0;

static int uir_mw_same_bsp(const uir_menu_map_view_t *a, const uir_menu_map_view_t *b)
{
	if (!a || !b) {
		return 0;
	}
	return Q_stricmp(a->bsp, b->bsp) == 0;
}

static void uir_mw_copy_view(uir_menu_map_view_t *dst, const uir_menu_map_view_t *src)
{
	if (!dst || !src) {
		return;
	}
	*dst = *src;
}

void UIR_MenuWorldSetBackend(const uir_menuworld_backend_t *backend)
{
	if (backend) {
		g_mw = *backend;
	} else {
		memset(&g_mw, 0, sizeof(g_mw));
	}
}

void UIR_MenuWorldSetDesiredView(const uir_menu_map_view_t *view)
{
	if (!view) {
		return;
	}
	if (memcmp(&g_mwDesiredView, view, sizeof(g_mwDesiredView)) != 0) {
		uir_mw_copy_view(&g_mwDesiredView, view);
		g_mwDesiredDirty = 1;
	}
}

void UIR_MenuWorldReleaseOwnership(void)
{
	if (!g_mwOwnsWorld) {
		return;
	}
	if (g_mw.cancelMenuWorldStaging) {
		g_mw.cancelMenuWorldStaging();
	}
	if (g_mw.clearWorld) {
		g_mw.clearWorld();
	}
	g_mwOwnsWorld = 0;
	if (g_mwState == UIR_MW_READY || g_mwState == UIR_MW_SWITCHING) {
		g_mwState = UIR_MW_NEEDS_RELOAD;
	}
}

void UIR_MenuWorldShutdown(void)
{
	UIR_MenuWeatherInvalidate();
	UIR_MenuWorldReleaseOwnership();
	UIR_MapEnvClear();
	g_mwState = UIR_MW_UNLOADED;
	g_mwLoggedMissing = 0;
	g_mwLoggedSwitchFail = 0;
	g_mwDesiredDirty = 1;
	UIR_MenuMapViewSetDefaults(&g_mwDesiredView);
	UIR_MenuMapViewSetDefaults(&g_mwActiveView);
}

void UIR_MenuWorldMarkNeedsReload(void)
{
	if (g_mwState == UIR_MW_READY || g_mwState == UIR_MW_SWITCHING) {
		g_mwState = UIR_MW_NEEDS_RELOAD;
	}
	g_mwDesiredDirty = 1;
}

uir_menuworld_state_t UIR_MenuWorldState(void)
{
	return g_mwState;
}

static void uir_mw_load_map_env(void)
{
	uir_map_env_backend_t envBackend;

	memset(&envBackend, 0, sizeof(envBackend));
	envBackend.cmEntityString = g_mw.cmEntityString;
	envBackend.cmModelBoundsFromName = g_mw.cmModelBoundsFromName;
	envBackend.readFile = g_mw.readFile;
	envBackend.freeFile = g_mw.freeFile;
	envBackend.fileExists = g_mw.fileExists;
	UIR_MapEnvLoad(g_mwDesiredView.bsp, &envBackend);
	g_mwDeferDraw = 1;
}

static uir_status_t uir_mw_begin_initial_load(void)
{
	int checksum = 0;

	if (!g_mw.fileExists || !g_mw.cmLoadMap || !g_mw.loadMenuWorld || !g_mw.clearWorld) {
		return UIR_ERR_NOT_READY;
	}

	if (!g_mw.fileExists(g_mwDesiredView.bsp)) {
		g_mwState = UIR_MW_UNAVAILABLE;
		if (!g_mwLoggedMissing && g_mw.printf) {
			g_mw.printf("UIR: menu world '%s' not found; using solid fallback\n", g_mwDesiredView.bsp);
			g_mwLoggedMissing = 1;
		}
		return UIR_ERR_MISSING_ASSET;
	}

	g_mw.cmLoadMap(g_mwDesiredView.bsp, qtrue, &checksum);
	if (g_mw.setWorldVisData && g_mw.cmVisibilityPointer) {
		g_mw.setWorldVisData(g_mw.cmVisibilityPointer());
	}
	g_mw.clearWorld();
	g_mw.loadMenuWorld(g_mwDesiredView.bsp);
	uir_mw_load_map_env();
	g_mwOwnsWorld = 1;
	uir_mw_copy_view(&g_mwActiveView, &g_mwDesiredView);
	g_mwState = UIR_MW_READY;
	g_mwDesiredDirty = 0;
	return UIR_OK;
}

static uir_status_t uir_mw_begin_staged_switch(void)
{
	int checksum = 0;

	if (!g_mw.fileExists || !g_mw.cmLoadMap || !g_mw.loadMenuWorld) {
		return UIR_ERR_NOT_READY;
	}

	if (!g_mw.fileExists(g_mwDesiredView.bsp)) {
		if (!g_mwLoggedMissing && g_mw.printf) {
			g_mw.printf("UIR: menu world '%s' not found; keeping current backdrop\n", g_mwDesiredView.bsp);
			g_mwLoggedMissing = 1;
		}
		g_mwDesiredDirty = 0;
		return UIR_ERR_MISSING_ASSET;
	}

	g_mwState = UIR_MW_SWITCHING;

	g_mw.cmLoadMap(g_mwDesiredView.bsp, qtrue, &checksum);
	if (g_mw.setWorldVisData && g_mw.cmVisibilityPointer) {
		g_mw.setWorldVisData(g_mw.cmVisibilityPointer());
	}
	g_mw.loadMenuWorld(g_mwDesiredView.bsp);
	uir_mw_load_map_env();
	g_mwOwnsWorld = 1;
	uir_mw_copy_view(&g_mwActiveView, &g_mwDesiredView);
	g_mwState = UIR_MW_READY;
	g_mwDesiredDirty = 0;
	return UIR_OK;
}

uir_status_t UIR_MenuWorldEnsureLoaded(void)
{
	static int viewsInited = 0;

	if (!viewsInited) {
		UIR_MenuMapViewSetDefaults(&g_mwDesiredView);
		UIR_MenuMapViewSetDefaults(&g_mwActiveView);
		viewsInited = 1;
	}

	if (g_mwState == UIR_MW_UNAVAILABLE) {
		return UIR_ERR_MISSING_ASSET;
	}

	if (g_mwState == UIR_MW_UNLOADED || g_mwState == UIR_MW_NEEDS_RELOAD) {
		return uir_mw_begin_initial_load();
	}

	if (!g_mwDesiredDirty) {
		return g_mwState == UIR_MW_READY ? UIR_OK : UIR_ERR_NOT_READY;
	}

	if (uir_mw_same_bsp(&g_mwActiveView, &g_mwDesiredView)) {
		uir_mw_copy_view(&g_mwActiveView, &g_mwDesiredView);
		g_mwDesiredDirty = 0;
		return UIR_OK;
	}

	return uir_mw_begin_staged_switch();
}

static void uir_mw_restore_2d(const uir_rect_t *dest)
{
	int w = (int)dest->w;
	int h = (int)dest->h;
	if (g_mw.set2DWindow) {
		g_mw.set2DWindow(0, 0, w, h, 0, (float)w, (float)h, 0, -1, 1);
	}
	if (g_mw.scissor) {
		g_mw.scissor(0, 0, w, h);
	}
}

static void uir_mw_draw_fallback(const uir_rect_t *destPx)
{
	vec4_t black = {0, 0, 0, 1};

	if (g_mw.setColor && g_mw.drawBox) {
		uir_mw_restore_2d(destPx);
		g_mw.setColor(black);
		g_mw.drawBox(destPx->x, destPx->y, destPx->w, destPx->h);
		g_mw.setColor(NULL);
	}
}

uir_status_t UIR_MenuWorldDraw(const uir_rect_t *destPx, int realtime)
{
	uir_status_t st;
	refdef_t     rd;
	vec3_t       angles;
	float        fovX, fovY;
	const uir_menu_map_view_t *view = &g_mwActiveView;
	const uir_map_env_t       *mapEnv;
	int                        cgRain;

	if (!destPx || destPx->w <= 0 || destPx->h <= 0) {
		return UIR_ERR_INVALID_ARG;
	}

	st = UIR_MenuWorldEnsureLoaded();
	if (st != UIR_OK) {
		uir_mw_draw_fallback(destPx);
		return st;
	}

	if (g_mwDeferDraw) {
		g_mwDeferDraw = 0;
		uir_mw_draw_fallback(destPx);
		return UIR_OK;
	}

	if (!g_mw.clearScene || !g_mw.renderScene || !g_mw.anglesToAxis) {
		return UIR_ERR_NOT_READY;
	}

	if (g_mw.hasActiveWorld && !g_mw.hasActiveWorld()) {
		uir_mw_draw_fallback(destPx);
		return UIR_ERR_NOT_READY;
	}

	if (UIR_CalcWorldFov((int)destPx->w, (int)destPx->h, view->fov, &fovX, &fovY) != UIR_OK) {
		return UIR_ERR_INVALID_ARG;
	}

	memset(&rd, 0, sizeof(rd));
	rd.x = (int)destPx->x;
	rd.y = (int)destPx->y;
	rd.width = (int)destPx->w;
	rd.height = (int)destPx->h;
	rd.fov_x = fovX;
	rd.fov_y = fovY;
	rd.vieworg[0] = view->vieworg[0];
	rd.vieworg[1] = view->vieworg[1];
	rd.vieworg[2] = view->vieworg[2];
	angles[0] = view->pitch;
	angles[1] = view->yaw;
	angles[2] = view->roll;
	g_mw.anglesToAxis(angles, rd.viewaxis);
	rd.time = realtime;
	mapEnv = UIR_MapEnvActive();
	if (mapEnv) {
		rd.farplane_distance = mapEnv->farplane;
		rd.farplane_bias = mapEnv->farplane_bias;
		rd.farplane_color[0] = mapEnv->farplane_color[0];
		rd.farplane_color[1] = mapEnv->farplane_color[1];
		rd.farplane_color[2] = mapEnv->farplane_color[2];
	}
	rd.farplane_cull = qtrue;
	rd.renderTerrain = qtrue;

	cgRain = 1;
	if (g_mw.cvarInteger) {
		cgRain = g_mw.cvarInteger("cg_rain", 1);
	}

	g_mw.clearScene();
	if (mapEnv) {
		UIR_MenuWeatherAddToScene(mapEnv, rd.vieworg, rd.viewaxis, cgRain, realtime, &g_mw);
	}
	g_mw.renderScene(&rd);
	uir_mw_restore_2d(destPx);
	return UIR_OK;
}
