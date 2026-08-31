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

#include "client.h"
#include "cl_uirender.h"
#include "cl_uimenu_dispatcher.h"
#include "cl_ui.h"

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#include "cl_uiplayermodelpicker.h"
#include "cl_browser_host.h"
#include "cl_modern_browser.h"
#include "cl_scoreboard_host.h"
#include "cl_objectives_host.h"
#include "cl_messages_host.h"
#include "cl_hud_registry.h"
#include "cl_hud_host.h"

#include "../uirender/uir_backend.h"
#include "../uirender/uir_batch.h"
#include "../uirender/uir_compositor.h"
#include "../uirender/uir_draw2d.h"
#include "../uirender/uir_font.h"
#include "../uirender/uir_gradient.h"
#include "crosshair_draw.h"
#include "../uirender/uir_image.h"
#include "../uirender/uir_menuworld.h"
#include "../uirender/uir_meshcache.h"
#include "../uirender/uir_modelpreview.h"
#include "../uirender/uir_weapon_bake_list.h"
#include "../uirender/uir_stencil.h"
#include "../uirender/uir_layer.h"
#include "../uirender/uir_debug.h"
#include "../uirender/uir_viewport.h"
#include "../corepp/tiki.h"
#include "../renderercommon/tr_types.h"
#include "../tiki/tiki_anim.h"
#include "../tiki/tiki_utility.h"

#include "../uidesign/uid_backend.h"
#include "../uidesign/uid_runtime.h"
#include "../uidesign/uid_binding.h"
#include "../uidesign/uid_document.h"
#include "../uidesign/uid_input.h"
#include "../uidesign/uid_invoke.h"
#include "../uidesign/uid_menu_map_view.h"
#include "../uidesign/uid_profile.h"
#include "../uidesign/uid_opt.h"
#include "../uidesign/uid_widget.h"
#include "../uilib/ui_public.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static cvar_t *ui_legacy;
static cvar_t *ui_om_hud;
static cvar_t *ui_scale;
static cvar_t *uir_debug;
static cvar_t *ui_render_stats;
static cvar_t *ui_gpu_draw;
static cvar_t *ui_debug_render;
static cvar_t *ui_om_menu_map_view;
static cvar_t *ui_profile; /* Added in OPM: 0=off 1=periodic 2=every frame */
static cvar_t *ui_profile_interval;
static cvar_t *ui_opt; /* Added in OPM: bitmask of UID_OPT_* (-1 = all on) */
static cvar_t *ui_clip_dedup; /* Added in OPM: skip unchanged clip/scissor applies */
static cvar_t *ui_mesh_cache; /* Added in OPM: tessellated mesh cache for GPU fills/strokes */
static cvar_t *ui_chrome_cache; /* Added in OPM: retained chrome RT (gl1; default off) */
static int     g_uiProfileFrameCounter;
static qboolean g_useLegacyMain = qfalse;
static qboolean g_legacyCached  = qfalse;
static int       g_compareKeepConsolesClosedUntil = 0; /* cls.realtime deadline */
static qboolean g_uirStarted = qfalse;
static float     g_lastUiPxScale = 1.0f; /* Added in OPM */

static void CL_UIR_ProfilePrint(const char *kind, const uid_prof_timings_t *t)
{
	int i;

	if (!t || !kind) {
		return;
	}
	Com_Printf(
		"UIProfile %s '%s' total=%.3fms layout=%d nodes=%d\n",
		kind,
		t->label[0] ? t->label : "-",
		(double)t->totalUs / 1000.0,
		t->layoutRan,
		t->nodeCount
	);
	for (i = 0; i < UID_PROF_COUNT; ++i) {
		if (t->us[i] <= 0) {
			continue;
		}
		Com_Printf(
			"  %-22s %8.3f ms\n",
			UID_ProfilePhaseName((uid_prof_phase_t)i),
			(double)t->us[i] / 1000.0
		);
	}
}

void CL_UIR_ProfileSyncFromCvar(void)
{
	const int enabled = (ui_profile && ui_profile->integer) ? 1 : 0;
	UID_ProfileSetEnabled(enabled);

	/* Added in OPM: sync UI optimization bitmask from ui_opt (-1 = all on). */
	if (ui_opt) {
		if (ui_opt->integer < 0) {
			UID_SetOptFlags(UID_OPT_ALL);
		} else {
			UID_SetOptFlags((unsigned)ui_opt->integer);
		}
	}
	/* Added in OPM: clip/scissor dedup for chrome paint. */
	if (ui_clip_dedup) {
		UIR_SetClipDedup(ui_clip_dedup->integer != 0);
	}
	/* Added in OPM: tessellated mesh cache for GPU fills/strokes. */
	if (ui_mesh_cache) {
		UIR_MeshCacheSetEnabled(ui_mesh_cache->integer != 0);
	}
	/* Added in OPM: retained chrome RT (idle blit). */
	if (ui_chrome_cache) {
		UIR_SetChromeCache(ui_chrome_cache->integer != 0);
	}
}

void CL_UIR_ProfileBeginSample(const char *label)
{
	CL_UIR_ProfileSyncFromCvar();
	if (!UID_ProfileEnabled()) {
		return;
	}
	UID_ProfileResetFrame();
	if (label && label[0]) {
		UID_ProfileSetFrameLabel(label);
	}
}

void CL_UIR_ProfileEndSample(const char *kind)
{
	uid_prof_timings_t t;
	int                interval;
	int                shouldPrint;

	if (!UID_ProfileEnabled()) {
		return;
	}
	UID_ProfileCaptureFrame(&t);
	if (t.totalUs <= 0 && t.us[UID_PROF_FRAME_PAINT_CHROME] <= 0 && t.us[UID_PROF_FRAME_BIND] <= 0
		&& t.us[UID_PROF_LEGACY_URC] <= 0 && t.us[UID_PROF_LEGACY_EVENTS] <= 0
		&& t.us[UID_PROF_LEGACY_VIEW3D] <= 0 && t.us[UID_PROF_LEGACY_MISC] <= 0) {
		return;
	}

	interval = (ui_profile_interval && ui_profile_interval->integer > 0) ? ui_profile_interval->integer : 60;
	g_uiProfileFrameCounter++;
	shouldPrint = (ui_profile && ui_profile->integer >= 2)
		|| (g_uiProfileFrameCounter % interval) == 0;

	if (shouldPrint) {
		CL_UIR_ProfilePrint(kind ? kind : "frame", &t);
	}
}

void CL_UIR_ProfileDumpLoad(void)
{
	uid_prof_timings_t t;

	CL_UIR_ProfileSyncFromCvar();
	if (!UID_ProfileEnabled()) {
		return;
	}
	UID_ProfileCaptureLoad(&t);
	if (t.totalUs <= 0) {
		return;
	}
	CL_UIR_ProfilePrint("load", &t);
}

static int CL_UIR_GpuDrawEnabled(void)
{
	/* Default on: batched GPU tess path (ui_gpu_draw 1). */
	return ui_gpu_draw ? ui_gpu_draw->integer : 1;
}

static void CL_UIR_SyncGpuDrawBatch(void)
{
	const int gpuDraw = CL_UIR_GpuDrawEnabled();

	UIR_BatchSetEnabled(gpuDraw);
	if (!gpuDraw) {
		/* ui_gpu_draw 0: legacy CPU/backbuffer path only — never keep MSAA FBO active. */
		UIR_BatchTargetEnd();
	}
}
/* Added in OPM: frame-driven ui_scale stress (slider thrash / font registry). */
static int       g_uiScaleStressLeft = 0;
static float     g_uiScaleStressValue = 1.0f;
static int       g_uiScaleStressDir = 1;
static qboolean  g_uiScaleStressQuit = qfalse;
static int       g_uiScaleStressPeakFonts = 0;

static void CL_UIR_GetSurfaceSizes(int *logicalW, int *logicalH, int *fbW, int *fbH);
static void CL_UIR_PushUiPxScale(void);
/* Fixed in OPM: SDL mouse is window-client space; map into UID layout space. */
static void CL_UIR_MapMouseToLayout(float *x, float *y, int layoutW, int layoutH);
static qboolean CL_UIR_SyncHudLayerMenus(unsigned int time, int *lw, int *lh, int *fw, int *fh);
static void CL_UIR_SyncPauseVoteCvars(void);

static uid_backend_t  g_uidBackend;
static int            g_lastLogicalW = 0;
static int            g_lastLogicalH = 0;
static int            g_lastFbW = 0;
static int            g_lastFbH = 0;
/* Added in OPM: last raw window size dumped under uir_debug (pointer map). */
static int            g_lastPointerRawW = -1;
static int            g_lastPointerRawH = -1;
static int            g_lastPointerLayoutW = -1;
static int            g_lastPointerLayoutH = -1;

static uid_runtime_t *CL_UIR_MainRuntime(void)
{
	return CL_UIMenu_RuntimeById("main");
}
/* Accumulated from K_MWHEEL* in KeyEvent; consumed in UpdateModern. */
static int            g_pointerWheelDelta = 0;
/* Added in OPM: mirror legacy UIFAKKServerList first-draw server refresh. */
static qboolean       g_browserDidFirstRefresh = qfalse;

/* ------------------------------------------------------------------------- */
/* UIR draw / font / world backends                                          */
/* ------------------------------------------------------------------------- */

static void uir_set_color(const float *rgba)
{
	re.SetColor(rgba);
}

static void uir_draw_box(float x, float y, float w, float h)
{
	re.DrawBox(x, y, w, h);
}

static void uir_set2d(
	int x,
	int y,
	int w,
	int h,
	float left,
	float right,
	float bottom,
	float top,
	float n,
	float f
)
{
	re.Set2DWindow(x, y, w, h, left, right, bottom, top, n, f);
}

static void uir_scissor(int x, int y, int w, int h)
{
	re.Scissor(x, y, w, h);
}

static int uir_batch_supported(void)
{
	return re.UI2DBatchSupported ? (re.UI2DBatchSupported() ? 1 : 0) : 0;
}

static int uir_batch_can_shader(int shader)
{
	return re.UI2DCanBatchShader ? (re.UI2DCanBatchShader((qhandle_t)shader) ? 1 : 0) : 0;
}

static void uir_batch_draw(const uir_vert_t *v, int nv, const unsigned short *idx, int ni, int shader)
{
	if (re.DrawUI2D) {
		re.DrawUI2D((const ui2dVert_t *)v, nv, idx, ni, (qhandle_t)shader);
	}
}

static int uir_target_available(void)
{
	if (!CL_UIR_GpuDrawEnabled()) {
		return 0;
	}
	return re.UI2DTargetAvailable ? (re.UI2DTargetAvailable() ? 1 : 0) : 0;
}

static int uir_target_samples(void)
{
	return re.UI2DTargetSamples ? re.UI2DTargetSamples() : 0;
}

static int uir_begin_target(void)
{
	if (!CL_UIR_GpuDrawEnabled()) {
		return 0;
	}
	return re.BeginUI2DTarget ? (re.BeginUI2DTarget() ? 1 : 0) : 0;
}

static void uir_end_target(void)
{
	if (re.EndUI2DTarget) {
		re.EndUI2DTarget();
	}
}

static int uir_create_atlas(const char *name, const unsigned char *rgba, int width, int height)
{
	if (!re.CreateUIAtlas) {
		return 0;
	}
	return (int)re.CreateUIAtlas(name, rgba, width, height);
}

static int uir_update_atlas(int h, const unsigned char *rgba, int width, int height)
{
	if (!re.UpdateUIAtlas) {
		return 0;
	}
	return re.UpdateUIAtlas((qhandle_t)h, rgba, width, height) ? 1 : 0;
}

static void uir_draw_pic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, int shader)
{
	re.DrawStretchPic(x, y, w, h, s1, t1, s2, t2, (qhandle_t)shader);
}

static void uir_draw_tile_pic(float x, float y, float w, float h, int shader)
{
	re.DrawTilePic(x, y, w, h, (qhandle_t)shader);
}

static void uir_draw_triangle_pic(const float points[3][2], const float texCoords[3][2], int shader)
{
	vec2_t p[3];
	vec2_t t[3];
	int    i;

	for (i = 0; i < 3; i++) {
		p[i][0] = points[i][0];
		p[i][1] = points[i][1];
		t[i][0] = texCoords[i][0];
		t[i][1] = texCoords[i][1];
	}
	re.DrawTrianglePic(p, t, (qhandle_t)shader);
}

static int uir_register_shader_nomip(const char *path)
{
	if (!path || !path[0]) {
		return 0;
	}
	return (int)re.RegisterShaderNoMip(path);
}

static void uir_get_shader_size(int shader, int *width, int *height)
{
	if (width) {
		*width = re.GetShaderWidth((qhandle_t)shader);
	}
	if (height) {
		*height = re.GetShaderHeight((qhandle_t)shader);
	}
}

static int uir_stencil_available(void)
{
	if (!re.UiStencilAvailable) {
		return 0;
	}
	return re.UiStencilAvailable() ? 1 : 0;
}

static void uir_begin_stencil_mask(int x, int y, int w, int h)
{
	if (re.BeginUiStencilMask) {
		re.BeginUiStencilMask(x, y, w, h);
	}
}

static void uir_stencil_mask_box(float x, float y, float w, float h)
{
	re.DrawBox(x, y, w, h);
}

static void uir_begin_stencil_draw(void)
{
	if (re.BeginUiStencilDraw) {
		re.BeginUiStencilDraw();
	}
}

static void uir_end_stencil(void)
{
	if (re.EndUiStencil) {
		re.EndUiStencil();
	}
}

static int uir_layer_available(void)
{
	if (!re.UiLayerAvailable) {
		return 0;
	}
	return re.UiLayerAvailable() ? 1 : 0;
}

static int uir_begin_ui_layer(int fbX, int fbY, int fbW, int fbH, float uiX, float uiY, float uiW, float uiH)
{
	if (!re.BeginUiLayer) {
		return 0;
	}
	return re.BeginUiLayer(fbX, fbY, fbW, fbH, uiX, uiY, uiW, uiH) ? 1 : 0;
}

static void uir_layer_apply_mask(
	int shader,
	float x,
	float y,
	float w,
	float h,
	float s1,
	float t1,
	float s2,
	float t2
)
{
	if (re.UiLayerApplyMask) {
		re.UiLayerApplyMask((qhandle_t)shader, x, y, w, h, s1, t1, s2, t2);
	}
}

static void uir_end_ui_layer(void)
{
	if (re.EndUiLayer) {
		re.EndUiLayer();
	}
}

static int uir_chrome_cache_available(void)
{
	if (!re.UiChromeCacheAvailable) {
		return 0;
	}
	return re.UiChromeCacheAvailable() ? 1 : 0;
}

static int uir_chrome_cache_begin(float uiX, float uiY, float uiW, float uiH)
{
	if (!re.BeginUiChromeCacheCapture) {
		return 0;
	}
	return re.BeginUiChromeCacheCapture(uiX, uiY, uiW, uiH) ? 1 : 0;
}

static void uir_chrome_cache_end(void)
{
	if (re.EndUiChromeCacheCapture) {
		re.EndUiChromeCacheCapture();
	}
}

static void uir_chrome_cache_blit(void)
{
	if (re.BlitUiChromeCache) {
		re.BlitUiChromeCache();
	}
}

static void uir_chrome_cache_invalidate(void)
{
	if (re.InvalidateUiChromeCache) {
		re.InvalidateUiChromeCache();
	}
}

static long uir_read_file(const char *path, void **buffer)
{
	return FS_ReadFile(path, buffer);
}

static void uir_free_file(void *buffer)
{
	FS_FreeFile(buffer);
}

static void *uir_alloc(size_t size)
{
	/* Font atlases are multi-MB; keep them off the game zone allocator. */
	return size ? malloc(size) : NULL;
}

static void uir_free(void *ptr)
{
	free(ptr);
}

static void uir_clear_world(void)
{
	if (re.ClearWorld) {
		re.ClearWorld();
	}
}

static void uir_load_menu_world(const char *name)
{
	if (re.LoadMenuWorld) {
		re.LoadMenuWorld(name);
	}
}

static int uir_load_menu_world_staged(const char *name)
{
	if (re.LoadMenuWorldStaged) {
		return re.LoadMenuWorldStaged(name) ? 1 : 0;
	}
	return 0;
}

static void uir_commit_menu_world(void)
{
	if (re.CommitMenuWorld) {
		re.CommitMenuWorld();
	}
}

static void uir_cancel_menu_world_staging(void)
{
	if (re.CancelMenuWorldStaging) {
		re.CancelMenuWorldStaging();
	}
}

static int uir_has_active_world(void)
{
	if (re.HasActiveWorld) {
		return re.HasActiveWorld() ? 1 : 0;
	}
	return 0;
}

static void uir_set_world_vis(const unsigned char *vis)
{
	re.SetWorldVisData(vis);
}

static void uir_clear_scene(void)
{
	re.ClearScene();
}

static void uir_render_scene(const void *refdef)
{
	re.RenderScene((const refdef_t *)refdef);
}

static int uir_file_exists(const char *path)
{
	fileHandle_t f = 0;
	int          len;

	/* Fixed in OPM: close on any successful open (incl. zero-length files). */
	len = FS_FOpenFileRead(path, &f, qfalse, qtrue);
	if (len >= 0) {
		FS_FCloseFile(f);
		return 1;
	}
	return 0;
}

static void uir_cm_load_map(const char *name, int clientload, int *checksum)
{
	CM_LoadMap(name, clientload ? qtrue : qfalse, checksum);
}

static const unsigned char *uir_cm_vis(void)
{
	return CM_VisibilityPointer();
}

static const char *uir_cm_entity_string(void)
{
	return CM_EntityString();
}

static void uir_cm_model_bounds_from_name(const char *name, float mins[3], float maxs[3])
{
	vec3_t vMins;
	vec3_t vMaxs;

	CM_ModelBoundsFromName(name, vMins, vMaxs);
	if (mins) {
		VectorCopy(vMins, mins);
	}
	if (maxs) {
		VectorCopy(vMaxs, maxs);
	}
}

static int uir_register_shader(const char *name)
{
	return re.RegisterShader(name);
}

static void uir_add_poly_to_scene(int shader, int numVerts, const void *verts, int renderfx)
{
	if (re.AddPolyToScene) {
		re.AddPolyToScene(shader, numVerts, (const polyVert_t *)verts, renderfx);
	}
}

static float uir_cvar_float(const char *name, float fallback)
{
	cvar_t *var = Cvar_Get(name, "", CVAR_ARCHIVE);
	if (!var) {
		return fallback;
	}
	return var->value;
}

static int uir_cvar_integer(const char *name, int fallback)
{
	cvar_t *var = Cvar_Get(name, "", CVAR_ARCHIVE);
	if (!var) {
		return fallback;
	}
	return var->integer;
}

static void uir_angles_to_axis(const float angles[3], float axis[3][3])
{
	AnglesToAxis(angles, axis);
}

static void QDECL uir_printf(const char *fmt, ...)
{
	va_list argptr;
	char    msg[MAXPRINTMSG];
	va_start(argptr, fmt);
	Q_vsnprintf(msg, sizeof(msg), fmt, argptr);
	va_end(argptr);
	Com_Printf("%s", msg);
}

static void uir_add_ref(const void *ent, int parentEntityNumber)
{
	re.AddRefEntityToScene((const refEntity_t *)ent, parentEntityNumber);
}

static void *uir_model_handle(int model)
{
	if (!re.R_Model_GetHandle) {
		return NULL;
	}
	return re.R_Model_GetHandle((qhandle_t)model);
}

static int uir_anim_num_variant(void *tiki, const char *name, int variant)
{
	return TIKI_Anim_NumForNameVariant((dtiki_t *)tiki, name, variant);
}

static float uir_anim_time(void *tiki, int index)
{
	return TIKI_Anim_Time((dtiki_t *)tiki, index);
}

static void uir_model_bounds(void *tiki, float scale, float mins[3], float maxs[3])
{
	vec3_t vMins;
	vec3_t vMaxs;

	TIKI_CalculateBounds((dtiki_t *)tiki, scale, vMins, vMaxs);
	if (mins) {
		VectorCopy(vMins, mins);
	}
	if (maxs) {
		VectorCopy(vMaxs, maxs);
	}
}

static void uir_force_update_pose(void *ent)
{
	if (re.ForceUpdatePose) {
		re.ForceUpdatePose((refEntity_t *)ent);
	}
}

static int uir_export_model_png(const void *refdef, const char *vfsPath)
{
	if (!re.ExportModelPreviewPNG) {
		return 0;
	}
	return re.ExportModelPreviewPNG((const refdef_t *)refdef, vfsPath) ? 1 : 0;
}

static void CL_UIR_WireBackends(void)
{
	uir_draw2d_backend_t      d2d;
	uir_batch_backend_t       batch;
	uir_font_backend_t        font;
	uir_image_backend_t       image;
	uir_stencil_backend_t     stencil;
	uir_layer_backend_t       layer;
	uir_menuworld_backend_t   mw;
	uir_modelpreview_backend_t mp;

	memset(&d2d, 0, sizeof(d2d));
	d2d.setColor = uir_set_color;
	d2d.drawBox = uir_draw_box;
	d2d.set2DWindow = uir_set2d;
	d2d.scissor = uir_scissor;
	UIR_Draw2D_SetBackend(&d2d);

	memset(&batch, 0, sizeof(batch));
	batch.supported = uir_batch_supported;
	batch.canBatchShader = uir_batch_can_shader;
	batch.draw = uir_batch_draw;
	batch.targetAvailable = uir_target_available;
	batch.targetSamples = uir_target_samples;
	batch.beginTarget = uir_begin_target;
	batch.endTarget = uir_end_target;
	UIR_BatchSetBackend(&batch);
	CL_UIR_SyncGpuDrawBatch();

	memset(&font, 0, sizeof(font));
	font.createAtlas = uir_create_atlas;
	font.updateAtlas = uir_update_atlas;
	font.setColor = uir_set_color;
	font.drawPic = uir_draw_pic;
	font.drawTrianglePic = uir_draw_triangle_pic; /* Added in Omaha: rotated glyph fallback */
	font.readFile = uir_read_file;
	font.freeFile = uir_free_file;
	font.allocMem = uir_alloc;
	font.freeMem = uir_free;
	UIR_FontSetBackend(&font);

	{
		uir_gradient_backend_t grad;
		memset(&grad, 0, sizeof(grad));
		grad.createAtlas = uir_create_atlas;
		grad.updateAtlas = uir_update_atlas;
		UIR_GradientSetBackend(&grad);
	}

	memset(&image, 0, sizeof(image));
	image.registerShaderNoMip = uir_register_shader_nomip;
	image.getShaderSize = uir_get_shader_size;
	image.setColor = uir_set_color;
	image.drawStretchPic = uir_draw_pic;
	image.drawTilePic = uir_draw_tile_pic;
	image.drawTrianglePic = uir_draw_triangle_pic;
	UIR_ImageSetBackend(&image);

	memset(&stencil, 0, sizeof(stencil));
	stencil.available = uir_stencil_available;
	stencil.beginMask = uir_begin_stencil_mask;
	stencil.maskBox = uir_stencil_mask_box;
	stencil.beginDraw = uir_begin_stencil_draw;
	stencil.end = uir_end_stencil;
	UIR_StencilSetBackend(&stencil);

	memset(&layer, 0, sizeof(layer));
	layer.available = uir_layer_available;
	layer.beginLayer = uir_begin_ui_layer;
	layer.applyMask = uir_layer_apply_mask;
	layer.endLayer = uir_end_ui_layer;
	layer.registerShaderNoMip = uir_register_shader_nomip;
	layer.getShaderSize = uir_get_shader_size;
	UIR_LayerSetBackend(&layer);

	{
		uir_chrome_cache_backend_t chromeCache;
		memset(&chromeCache, 0, sizeof(chromeCache));
		chromeCache.available = uir_chrome_cache_available;
		chromeCache.beginCapture = uir_chrome_cache_begin;
		chromeCache.endCapture = uir_chrome_cache_end;
		chromeCache.blit = uir_chrome_cache_blit;
		chromeCache.invalidate = uir_chrome_cache_invalidate;
		UIR_ChromeCacheSetBackend(&chromeCache);
	}

	memset(&mw, 0, sizeof(mw));
	mw.clearWorld = uir_clear_world;
	mw.loadMenuWorld = uir_load_menu_world;
	mw.loadMenuWorldStaged = uir_load_menu_world_staged;
	mw.commitMenuWorld = uir_commit_menu_world;
	mw.cancelMenuWorldStaging = uir_cancel_menu_world_staging;
	mw.hasActiveWorld = uir_has_active_world;
	mw.setWorldVisData = uir_set_world_vis;
	mw.clearScene = uir_clear_scene;
	mw.renderScene = uir_render_scene;
	mw.setColor = uir_set_color;
	mw.drawBox = uir_draw_box;
	mw.set2DWindow = uir_set2d;
	mw.scissor = uir_scissor;
	mw.fileExists = uir_file_exists;
	mw.cmLoadMap = uir_cm_load_map;
	mw.cmVisibilityPointer = uir_cm_vis;
	mw.cmEntityString = uir_cm_entity_string;
	mw.cmModelBoundsFromName = uir_cm_model_bounds_from_name;
	mw.readFile = uir_read_file;
	mw.freeFile = uir_free_file;
	mw.registerShader = uir_register_shader;
	mw.addPolyToScene = uir_add_poly_to_scene;
	mw.anglesToAxis = uir_angles_to_axis;
	mw.cvarFloat = uir_cvar_float;
	mw.cvarInteger = uir_cvar_integer;
	mw.printf = uir_printf;
	UIR_MenuWorldSetBackend(&mw);

	memset(&mp, 0, sizeof(mp));
	mp.clearScene = uir_clear_scene;
	mp.addRefEntity = uir_add_ref;
	mp.renderScene = uir_render_scene;
	mp.set2DWindow = uir_set2d;
	mp.scissor = uir_scissor;
	mp.anglesToAxis = uir_angles_to_axis;
	mp.modelGetHandle = uir_model_handle;
	mp.animNumForNameVariant = uir_anim_num_variant;
	mp.animTime = uir_anim_time;
	mp.modelBounds = uir_model_bounds;
	mp.forceUpdatePose = uir_force_update_pose;
	mp.exportModelPreviewPNG = uir_export_model_png;
	UIR_ModelPreviewSetBackend(&mp);
}

/* ------------------------------------------------------------------------- */
/* Design-format (UID) host adapters                                         */
/* ------------------------------------------------------------------------- */

static void *uid_alloc(size_t size)
{
	return Z_Malloc(size);
}

static void uid_free(void *ptr)
{
	Z_Free(ptr);
}

static long uid_read_file(const char *path, void **buf)
{
	return FS_ReadFile(path, buf);
}

static void uid_free_file(void *buf)
{
	FS_FreeFile(buf);
}

static bool uid_cvar_describe(const char *name, int *flags, char *valueBuf, size_t valueBufSize)
{
	cvar_t *var;

	if (!name || !name[0]) {
		return false;
	}
	var = Cvar_FindVar(name);
	if (!var) {
		return false;
	}
	if (flags) {
		*flags = var->flags;
	}
	if (valueBuf && valueBufSize > 0) {
		Q_strncpyz(valueBuf, var->string ? var->string : "", (int)valueBufSize);
	}
	return true;
}

/* Added in OPM: expose cvar_globalModCount to uidesign expression memos. */
static unsigned uid_cvar_epoch(void)
{
	return (unsigned)Cvar_GlobalModCount();
}

static bool uid_cvar_write(const char *name, const char *value)
{
	cvar_t *var;
	char    work[MAX_CVAR_VALUE_STRING];

	if (!name || !name[0] || !value) {
		return false;
	}
	var = Cvar_FindVar(name);
	if (var && (var->flags & (CVAR_ROM | CVAR_INIT | CVAR_PROTECTED))) {
		return false;
	}

	Q_strncpyz(work, value, sizeof(work));
	if (!Q_stricmp(name, "in_mouse")) {
		int v = atoi(work);
		if (v < -1) {
			v = -1;
		}
		if (v > 1) {
			v = 1;
		}
		if (v == 0) {
			v = 1;
		}
		Com_sprintf(work, sizeof(work), "%d", v);
	} else if (!Q_stricmp(name, "r_lodscale")) {
		float v = (float)atof(work);
		if (v < 0.5f) {
			v = 0.5f;
		}
		if (v > 2.0f) {
			v = 2.0f;
		}
		Com_sprintf(work, sizeof(work), "%.1f", v);
	}

	Cvar_Set(name, work);
	if (!Q_stricmp(name, "ui_om_hud")) {
		CL_UIMenu_ValidateHudCvar();
		CL_UIMenu_SyncAutoMenus();
		/* Added in OPM: server gates spectate HUD copy on userinfo om_hud. */
		Cvar_Set("om_hud", CL_UIR_UseModernHudPack() ? "1" : "0");
	}
	return true;
}

static bool uid_cvar_reset(const char *name)
{
	cvar_t *var;

	if (!name || !name[0]) {
		return false;
	}
	var = Cvar_FindVar(name);
	if (var) {
		Cvar_Reset(name);
		return true;
	}
	return false;
}

static bool uid_key_name_to_num(const char *name, int *key)
{
	int k;

	if (!name || !key) {
		return false;
	}
	k = Key_StringToKeynum(name);
	if (k < 0) {
		return false;
	}
	*key = k;
	return true;
}

static bool uid_key_num_to_name(int key, char *out, size_t outSize)
{
	const char *s;

	if (!out || outSize == 0) {
		return false;
	}
	s = Key_KeynumToString(key);
	if (!s) {
		return false;
	}
	Q_strncpyz(out, s, (int)outSize);
	return true;
}

static bool uid_get_binding(int key, char *out, size_t outSize)
{
	const char *b;

	if (!out || outSize == 0) {
		return false;
	}
	b = Key_GetBinding(key);
	Q_strncpyz(out, b ? b : "", (int)outSize);
	return true;
}

static bool uid_set_binding(int key, const char *binding)
{
	Key_SetBinding(key, binding ? binding : "");
	return true;
}

static int uid_find_conflicts(const char *binding, int *keysOut, int maxKeys)
{
	int i;
	int n = 0;

	if (!binding || !binding[0] || !keysOut || maxKeys <= 0) {
		return 0;
	}
	for (i = 0; i < MAX_KEYS; i++) {
		const char *b = Key_GetBinding(i);
		if (b && b[0] && !Q_stricmp(b, binding)) {
			if (n < maxKeys) {
				keysOut[n] = i;
			}
			n++;
		}
	}
	return n > maxKeys ? maxKeys : n;
}

static bool uid_get_keys_for_command(const char *command, int *key1, int *key2)
{
	if (!command || !key1 || !key2) {
		return false;
	}
	Key_GetKeysForCommand(command, key1, key2);
	return true;
}

/*
 * Added in OPM: in-memory modern UI server browser backed by Gamespy queries.
 * Status strip lives in XML; host updates count cvars each Update.
 */
typedef enum {
	UIR_BROWSER_SORT_NAME = 0,
	UIR_BROWSER_SORT_MAP,
	UIR_BROWSER_SORT_PLAYERS,
	UIR_BROWSER_SORT_GAMETYPE,
	UIR_BROWSER_SORT_PING,
	UIR_BROWSER_SORT_IP
} uir_browser_sort_t;

static uir_browser_row_t  g_browserRows[UIR_BROWSER_MAX_ROWS];
static int                g_browserCount;
static int                g_browserVisibleIdx[UIR_BROWSER_MAX_ROWS];
static int                g_browserVisibleCount;
static int                g_browserSelected = -1;
static int                g_browserHover = -1;
static uint64_t           g_browserCollectionRevision = 1;
static uir_browser_sort_t g_browserSortKey = UIR_BROWSER_SORT_PLAYERS;
static qboolean           g_browserSortAsc = qfalse;
static qboolean           g_browserInited = qfalse;
static float              g_browserScrollY = 0.0f;
static int                g_browserDiscovered = 0;
static int                g_browserCompleted = 0;
static int                g_browserTotalPlayers = 0;
static qboolean           g_browserScanning = qfalse;
static const float        UIR_BROWSER_HEADER_H = 36.0f;
static const float        UIR_BROWSER_ROW_H = 36.0f;

static void uir_browser_rebuild_visible(void);

const char *UIR_Browser_NormalizeGametype(const char *raw)
{
	if (!raw || !raw[0]) {
		return "Other";
	}
	if (Q_stristr(raw, "free") || Q_stristr(raw, "ffa")) {
		return "FFA";
	}
	if (Q_stristr(raw, "team") || Q_stristr(raw, "tdm") || Q_stristr(raw, "deathmatch")) {
		return "TDM";
	}
	if (Q_stristr(raw, "objective") || Q_stristr(raw, "tow") || Q_stristr(raw, "tug")
	    || Q_stristr(raw, "liberation")) {
		return "Objective";
	}
	if (Q_stristr(raw, "round")) {
		return "Round";
	}
	return "Other";
}

void UIR_Browser_ClearRows(void)
{
	g_browserCount = 0;
	g_browserSelected = -1;
	g_browserHover = -1;
	g_browserScrollY = 0.0f;
	g_browserInited = qtrue;
}

int UIR_Browser_FindRow(unsigned int realIP, int queryPort)
{
	int i;

	for (i = 0; i < g_browserCount; i++) {
		if (g_browserRows[i].realIP == realIP && g_browserRows[i].queryPort == queryPort) {
			return i;
		}
	}
	return -1;
}

int UIR_Browser_FindRowByIp(const char *ip)
{
	int i;

	if (!ip || !ip[0]) {
		return -1;
	}
	for (i = 0; i < g_browserCount; i++) {
		if (!Q_stricmp(g_browserRows[i].ip, ip)) {
			return i;
		}
	}
	return -1;
}

int UIR_Browser_UpsertRow(unsigned int realIP, int queryPort, const uir_browser_row_t *row)
{
	int index;

	if (!row) {
		return -1;
	}

	index = UIR_Browser_FindRow(realIP, queryPort);
	if (index < 0) {
		if (g_browserCount >= UIR_BROWSER_MAX_ROWS) {
			return -1;
		}
		index = g_browserCount++;
	}

	g_browserRows[index] = *row;
	g_browserRows[index].realIP = realIP;
	g_browserRows[index].queryPort = queryPort;
	g_browserInited = qtrue;
	return index;
}

void UIR_Browser_NotifyChanged(void)
{
	g_browserCollectionRevision++;
}

void UIR_Browser_SetQueryStats(int discovered, int completed, int totalPlayers, qboolean scanning)
{
	g_browserDiscovered = discovered;
	g_browserCompleted = completed;
	g_browserTotalPlayers = totalPlayers;
	g_browserScanning = scanning;
}

int UIR_Browser_GetRowCount(void)
{
	return g_browserCount;
}

const uir_browser_row_t *UIR_Browser_GetRow(int index)
{
	if (index < 0 || index >= g_browserCount) {
		return NULL;
	}
	return &g_browserRows[index];
}

static qboolean uir_browser_token_in_list(const char *list, const char *token)
{
	const char *start;
	const char *end;
	size_t      len;

	if (!list || !token || !token[0]) {
		return qfalse;
	}
	len = strlen(token);
	start = list;
	while (start && *start) {
		while (*start == ' ') {
			start++;
		}
		if (!*start) {
			break;
		}
		end = strchr(start, ' ');
		if (!end) {
			return strlen(start) == len && !Q_stricmpn(start, token, len) ? qtrue : qfalse;
		}
		if ((size_t)(end - start) == len && !Q_stricmpn(start, token, len)) {
			return qtrue;
		}
		start = end + 1;
	}
	return qfalse;
}

qboolean UIR_Browser_IsFavoriteIp(const char *ip)
{
	return uir_browser_token_in_list(Cvar_VariableString("ui_om_favorite_servers"), ip);
}

void UIR_Browser_ApplyFavorites(void)
{
	int i;

	for (i = 0; i < g_browserCount; i++) {
		g_browserRows[i].favorite = UIR_Browser_IsFavoriteIp(g_browserRows[i].ip);
	}
}

void UIR_Browser_SaveFavoritesToCvar(void)
{
	char        buf[MAX_CVAR_VALUE_STRING];
	int         i;
	const char *sep = "";

	buf[0] = '\0';
	for (i = 0; i < g_browserCount; i++) {
		if (!g_browserRows[i].favorite) {
			continue;
		}
		Q_strcat(buf, sizeof(buf), sep);
		Q_strcat(buf, sizeof(buf), g_browserRows[i].ip);
		sep = " ";
	}
	Cvar_Set("ui_om_favorite_servers", buf);
}

void UIR_Browser_ToggleFavoriteByIp(const char *ip)
{
	int index;

	index = UIR_Browser_FindRowByIp(ip);
	if (index < 0) {
		return;
	}
	g_browserRows[index].favorite = g_browserRows[index].favorite ? qfalse : qtrue;
	UIR_Browser_SaveFavoritesToCvar();
	UIR_Browser_NotifyChanged();
}

void UIR_Browser_SeedMock(void)
{
	static const uir_browser_row_t seed[] = {
		{qfalse, "OpenMoHAA Community #1", "mp_stadt", 12, 32, "TDM", 24, "203.0.113.10:12203", "1.11", qfalse, 0, 0},
		{qfalse, "Night Ops EU", "mp_stalingrad", 8, 20, "Objective", 41, "198.51.100.22:12203", "1.11", qfalse, 0, 0},
		{qfalse, "Freeze Tag Friday", "mp_flensburg", 16, 24, "Other", 18, "192.0.2.55:12203", "1.11", qfalse, 0, 0},
		{qfalse, "Classic FFA", "mp_bazaar", 5, 16, "FFA", 63, "203.0.113.88:12203", "1.11", qfalse, 0, 0},
		{qfalse, "Round Based Ranked", "mp_depot", 20, 20, "Round", 35, "198.51.100.9:12203", "1.11", qfalse, 0, 0},
	};
	int i;

	g_browserCount = (int)(sizeof(seed) / sizeof(seed[0]));
	for (i = 0; i < g_browserCount; i++) {
		g_browserRows[i] = seed[i];
	}
	g_browserSelected = 0;
	g_browserHover = -1;
	g_browserScrollY = 0.0f;
	g_browserInited = qtrue;
	UIR_Browser_ApplyFavorites();
}

static int uir_browser_cmp_rows(const uir_browser_row_t *a, const uir_browser_row_t *b)
{
	int dir = g_browserSortAsc ? 1 : -1;
	int cmp = 0;

	if (a->favorite != b->favorite) {
		return a->favorite ? -1 : 1;
	}

	switch (g_browserSortKey) {
	case UIR_BROWSER_SORT_MAP:
		cmp = Q_stricmp(a->map, b->map);
		break;
	case UIR_BROWSER_SORT_PLAYERS:
		cmp = a->players - b->players;
		break;
	case UIR_BROWSER_SORT_GAMETYPE:
		cmp = Q_stricmp(a->gametype, b->gametype);
		break;
	case UIR_BROWSER_SORT_PING:
		cmp = a->ping - b->ping;
		break;
	case UIR_BROWSER_SORT_IP:
		cmp = Q_stricmp(a->ip, b->ip);
		break;
	case UIR_BROWSER_SORT_NAME:
	default:
		cmp = Q_stricmp(a->name, b->name);
		break;
	}
	if (cmp < 0) {
		return -dir;
	}
	if (cmp > 0) {
		return dir;
	}
	return 0;
}

static const char *uir_browser_sort_column_name(uir_browser_sort_t key)
{
	switch (key) {
	case UIR_BROWSER_SORT_MAP:
		return "map";
	case UIR_BROWSER_SORT_PLAYERS:
		return "players";
	case UIR_BROWSER_SORT_GAMETYPE:
		return "gametype";
	case UIR_BROWSER_SORT_PING:
		return "ping";
	case UIR_BROWSER_SORT_IP:
		return "ip";
	case UIR_BROWSER_SORT_NAME:
	default:
		return "name";
	}
}

static uir_browser_sort_t uir_browser_sort_from_column(const char *column)
{
	if (!column || !column[0]) {
		return UIR_BROWSER_SORT_PLAYERS;
	}
	if (!Q_stricmp(column, "map")) {
		return UIR_BROWSER_SORT_MAP;
	}
	if (!Q_stricmp(column, "players")) {
		return UIR_BROWSER_SORT_PLAYERS;
	}
	if (!Q_stricmp(column, "gametype")) {
		return UIR_BROWSER_SORT_GAMETYPE;
	}
	if (!Q_stricmp(column, "ping")) {
		return UIR_BROWSER_SORT_PING;
	}
	if (!Q_stricmp(column, "ip")) {
		return UIR_BROWSER_SORT_IP;
	}
	if (!Q_stricmp(column, "name")) {
		return UIR_BROWSER_SORT_NAME;
	}
	return UIR_BROWSER_SORT_PLAYERS;
}

static void uir_browser_sync_sort_cvars(void)
{
	Cvar_Set("ui_om_browser_sort", uir_browser_sort_column_name(g_browserSortKey));
	Cvar_Set("ui_om_browser_sort_asc", g_browserSortAsc ? "1" : "0");
}

/*
 * Fixed in OPM: modern list selection writes ui_selected_server via UID bind;
 * legacy g_browserSelected is only updated by the old mouse handler. Prefer the cvar
 * when it still matches a known row so JOIN uses the row the user clicked.
 */
static void uir_browser_sync_selected_from_cvar(void)
{
	const char *selected = Cvar_VariableString("ui_selected_server");
	int         idx;

	if (!selected || !selected[0]) {
		return;
	}
	idx = UIR_Browser_FindRowByIp(selected);
	if (idx >= 0) {
		g_browserSelected = idx;
	}
}

static void uir_browser_apply_sort_click(uir_browser_sort_t key)
{
	if (g_browserSortKey == key) {
		g_browserSortAsc = g_browserSortAsc ? qfalse : qtrue;
	} else {
		g_browserSortKey = key;
		g_browserSortAsc = (key == UIR_BROWSER_SORT_PLAYERS) ? qfalse : qtrue;
	}
	uir_browser_rebuild_visible();
	uir_browser_sync_sort_cvars();
}

static void uir_browser_rebuild_visible(void)
{
	const char *search = Cvar_VariableString("ui_om_server_search");
	const char *gtFilter = Cvar_VariableString("ui_om_server_gametype");
	char        searchLower[128];
	int         i, j;

	if (!g_browserInited) {
		g_browserVisibleCount = 0;
		g_browserCollectionRevision++;
		return;
	}

	searchLower[0] = '\0';
	if (search && search[0]) {
		Q_strncpyz(searchLower, search, sizeof(searchLower));
		Q_strlwr(searchLower);
	}

	g_browserVisibleCount = 0;
	for (i = 0; i < g_browserCount; i++) {
		char nameLower[64];

		if (gtFilter && gtFilter[0] && Q_stricmp(gtFilter, "all") != 0) {
			if (Q_stricmp(g_browserRows[i].gametype, gtFilter) != 0) {
				continue;
			}
		}
		if (searchLower[0]) {
			Q_strncpyz(nameLower, g_browserRows[i].name, sizeof(nameLower));
			Q_strlwr(nameLower);
			if (!strstr(nameLower, searchLower)) {
				continue;
			}
		}
		g_browserVisibleIdx[g_browserVisibleCount++] = i;
	}

	for (i = 0; i + 1 < g_browserVisibleCount; i++) {
		for (j = i + 1; j < g_browserVisibleCount; j++) {
			const uir_browser_row_t *ra = &g_browserRows[g_browserVisibleIdx[i]];
			const uir_browser_row_t *rb = &g_browserRows[g_browserVisibleIdx[j]];
			if (uir_browser_cmp_rows(ra, rb) > 0) {
				int tmp = g_browserVisibleIdx[i];
				g_browserVisibleIdx[i] = g_browserVisibleIdx[j];
				g_browserVisibleIdx[j] = tmp;
			}
		}
	}

	uir_browser_sync_selected_from_cvar();
	{
		const char *selected = Cvar_VariableString("ui_selected_server");

		if (!selected || !selected[0]) {
			g_browserSelected = -1;
		} else if (g_browserSelected < 0 || g_browserSelected >= g_browserCount) {
			g_browserSelected = -1;
		} else {
			qboolean visible = qfalse;
			for (i = 0; i < g_browserVisibleCount; i++) {
				if (g_browserVisibleIdx[i] == g_browserSelected) {
					visible = qtrue;
					break;
				}
			}
			if (!visible) {
				g_browserSelected = -1;
			}
		}
	}
	g_browserCollectionRevision++;
}

static void uir_browser_update_status_cvars(void)
{
	int players = 0;
	int i;
	int totalListed = g_browserCount > 0 ? g_browserCount : g_browserDiscovered;

	uir_browser_rebuild_visible();
	for (i = 0; i < g_browserCount; i++) {
		players += g_browserRows[i].players;
	}
	if (g_browserTotalPlayers > 0) {
		players = g_browserTotalPlayers;
	}
	Cvar_Set("ui_om_servers_visible", va("%d", g_browserVisibleCount));
	Cvar_Set("ui_om_servers_total", va("%d", totalListed));
	Cvar_Set("ui_om_players_total", va("%d", players));
	if (g_browserDiscovered > 0) {
		Cvar_Set(
			"ui_om_status_servers",
			va("# SERVERS: %d / %d", g_browserVisibleCount, g_browserDiscovered)
		);
	} else {
		Cvar_Set(
			"ui_om_status_servers",
			va("# SERVERS: %d / %d", g_browserVisibleCount, totalListed)
		);
	}
	Cvar_Set("ui_om_status_players", va("# PLAYERS: %d", players));
	Cvar_Set("ui_om_status_phase", g_browserScanning ? "SCANNING" : "READY");
	uir_browser_sync_sort_cvars();
}

static void uir_browser_refresh(void)
{
	CL_ModernBrowser_Refresh();
	Cvar_Set("ui_browser_want_refresh", "0");
	uir_browser_update_status_cvars();
}

static void uir_browser_col_rects(float x, float w, float *outX, float *outW)
{
	/* Match HTML: 26px 32% 18% 10% 13% 7% 18% (fav fixed; rest share remaining). */
	static const float weights[6] = {32.0f, 18.0f, 10.0f, 13.0f, 7.0f, 18.0f};
	const float fav = 26.0f;
	const float rem = std::max(0.0f, w - fav);
	float       sum = 0.0f;
	int         i;

	for (i = 0; i < 6; i++) {
		sum += weights[i];
	}
	outW[0] = std::min(fav, w);
	outX[0] = x;
	float cursor = x + outW[0];
	for (i = 0; i < 6; i++) {
		outW[i + 1] = (sum > 0.0f) ? (rem * (weights[i] / sum)) : 0.0f;
		outX[i + 1] = cursor;
		cursor += outW[i + 1];
	}
}

static void uir_browser_draw_text(
	float x,
	float y,
	float size,
	const char *text,
	const float *rgba
)
{
	uir_font_t           *font;
	uir_color_t           color;
	const uir_viewport_t *vp = UIR_CompositorViewport();

	if (!text || !rgba || !vp) {
		return;
	}
	font = UIR_FontResolve("fonts/Oswald-Medium.ttf", size, vp->scaleY > 0.0f ? vp->scaleY : 1.0f);
	if (!font) {
		return;
	}
	color.r = rgba[0];
	color.g = rgba[1];
	color.b = rgba[2];
	color.a = rgba[3];
	UIR_FontDraw(vp, font, x, y, text, &color, 0.0f);
}

static float g_browserLastW = 800.0f;
static float g_browserLastH = 400.0f;

static int uir_browser_col_at(float localX, float w)
{
	float colX[7];
	float colW[7];
	int   i;

	uir_browser_col_rects(0.0f, w > 0.0f ? w : 1.0f, colX, colW);
	for (i = 0; i < 7; i++) {
		if (localX >= colX[i] && localX < colX[i] + colW[i]) {
			return i;
		}
	}
	return -1;
}

/* Removed in OPM: CL_UIR_ShouldPaintCrosshairMenu (crosshair is procedural in cgame). */

static qboolean CL_UIR_ShouldPaintHudLayer(void)
{
	if (CL_UIR_UseLegacyHud()) {
		return qfalse;
	}
	if (clc.state != CA_ACTIVE) {
		return qfalse;
	}
	if (CL_UIMenu_HasInteractiveOpen()) {
		return qfalse;
	}
	if (!cl.snap.valid) {
		return qfalse;
	}
	/*
	 * Fixed in OPM: intermission / no-hud / letterbox suppress play chrome via
	 * ui_om_hud_show in XML, but messaging (chat / kill-feed / game msgs) and the
	 * hold-TAB / end-of-match scoreboard must still paint. Check scoreboard first.
	 */
	if (CL_UIMenu_IsOpen(CL_UIR_ScoreboardMenuId())) {
		return qtrue;
	}
	if (UI_LetterboxActive()) {
		return qfalse;
	}
	if (CL_UIR_UseModernHudPack()) {
		const char *activeHud = CL_UIR_ActiveHudId();
		if (activeHud && activeHud[0] && CL_UIMenu_IsOpen(activeHud)) {
			/*
			 * Added in OPM: keep the HUD pack painting during intermission so
			 * messaging survives even before the scoreboard hold opens. Play
			 * chrome is hidden by ui_om_hud_show, not by skipping this layer.
			 */
			return qtrue;
		}
	}
	if ((cl.snap.ps.pm_flags & PMF_NO_HUD) || (cl.snap.ps.pm_flags & PMF_INTERMISSION)) {
		return qfalse;
	}
	return qfalse;
}

static void CL_UIR_SyncScoreboardPointer(void)
{
	static qboolean scoreboardPointerActive = qfalse;
	static int      lastClientState = -1;
	const qboolean  hasPtr = CL_UIMenu_HasPointerMenuOpen();
	const qboolean  consoleVis = UI_ConsoleIsVisible();
	const qboolean  legacyOwns = UI_LegacyOverlayOwnsInput();
	const qboolean  overlayOpen = CL_UIR_IsConnectedOverlayOpen();
	/*
	 * Fixed in OPM: intermission scoreboard pointer must yield to the console and
	 * legacy loading/continue menus. Also drop a stale OpenHold scoreboard when
	 * leaving CA_ACTIVE (cgame reload never sends -scores / CloseHold).
	 */
	if (lastClientState == CA_ACTIVE && clc.state != CA_ACTIVE) {
		CL_UIMenu_CloseHold(CL_UIR_ScoreboardMenuId());
	}
	lastClientState = clc.state;

	const qboolean wantsPointer =
		hasPtr && clc.state == CA_ACTIVE && !CL_UIMenu_ShouldOwnInput() && !consoleVis && !legacyOwns;

	if (wantsPointer) {
		CL_UIR_EnterModernInputModeKeepKeys();
	} else if (scoreboardPointerActive) {
		/*
		 * Skip LeaveModernInputMode when an interactive overlay owns input, or
		 * when yielding to the console / legacy loading Continue — ActivateView3D
		 * / MouseOff would steal focus or kill that button.
		 */
		if (!CL_UIMenu_ShouldOwnInput() && !overlayOpen && !consoleVis && !legacyOwns) {
			CL_UIR_LeaveModernInputMode();
		}
	}
	scoreboardPointerActive = wantsPointer;
}

void CL_UIR_SyncPointerMenus(void)
{
	CL_UIR_SyncScoreboardPointer();
}

static void CL_UIR_UpdateHudMenus(unsigned int time, qboolean applyWheel)
{
	uid_pointer_state_t pointer;
	int                 lw, lh;
	float               x, y;

	CL_UIR_SyncScoreboardPointer();
	if (CL_UIR_UseModernHudPack() && clc.state == CA_ACTIVE) {
		UIR_Hud_Sync();
	}
	/*
	 * Changed in OPM: hold-TAB scoreboard without a pointer still needs the
	 * pointer update path when applyWheel is set so overflow=scroll lists move.
	 */
	const qboolean scoreboardWheel =
		applyWheel && CL_UIMenu_IsOpen(CL_UIR_ScoreboardMenuId()) && g_pointerWheelDelta != 0;
	if (!CL_UIMenu_HasPointerMenuOpen() && !scoreboardWheel) {
		CL_UIMenu_UpdateAll(time);
		return;
	}

	CL_FillUIDef();
	CL_UIR_GetSurfaceSizes(&lw, &lh, nullptr, nullptr);
	if (CL_UIMenu_IsOpen(CL_UIR_ScoreboardMenuId())) {
		UIR_Scoreboard_UpdateLayoutForViewport(lh);
	}

	/* Map from window-space cl.mouse* — FillUIDef already mapped uid for winman. */
	x = (float)cl.mousex;
	y = (float)cl.mousey;
	CL_UIR_MapMouseToUiVid(&x, &y);
	/*
	 * Added in OPM: hold-TAB without a cursor keeps relative look mouse coords off
	 * the panel. Aim wheel hit-tests at the UI center so overflow=scroll lists still
	 * receive the wheel (FFA / team panels are centered).
	 */
	if (scoreboardWheel && !CL_UIMenu_HasPointerMenuOpen() && lw > 0 && lh > 0) {
		x = (float)lw * 0.5f;
		y = (float)lh * 0.5f;
	}

	std::memset(&pointer, 0, sizeof(pointer));
	pointer.x = x;
	pointer.y = y;
	pointer.buttons = (int)uid.mouseFlags;
	/*
	 * Fixed in OPM: spectator / intermission scoreboard is a pointer HUD (not
	 * ShouldOwnInput). Pass mouse wheel so overflow=scroll lists scroll; layout-only
	 * syncs leave applyWheel false so pause-menu wheel is not stolen.
	 */
	pointer.wheel = applyWheel ? g_pointerWheelDelta : 0;
	pointer.moved = false;
	if (applyWheel) {
		g_pointerWheelDelta = 0;
	}
	CL_UIMenu_UpdateAllWithPointer(time, &pointer);
}

static bool uid_get_hi_res_scale(float *scaleX, float *scaleY)
{
	vec2_t hi;

	if (!scaleX || !scaleY) {
		return false;
	}
	UI_GetHighResolutionScale(hi);
	*scaleX = hi[0];
	*scaleY = hi[1];
	return true;
}

static bool uid_get_framebuffer_size(int *width, int *height)
{
	int fw = 0;
	int fh = 0;

	if (!width || !height) {
		return false;
	}
	CL_UIR_GetSurfaceSizes(nullptr, nullptr, &fw, &fh);
	if (fw <= 0) {
		fw = cls.glconfig.vidWidth;
	}
	if (fh <= 0) {
		fh = cls.glconfig.vidHeight;
	}
	*width = fw;
	*height = fh;
	return fw > 0 && fh > 0;
}

static void cl_uir_paint_crosshair_preview(float x, float y, float w, float h)
{
	xhair_config_t cfg;
	xhair_frame_t  frame;
	xhair_rect_t   rects[XHAIR_MAX_RECTS];
	uir_color_t    color;
	const float    scaleX = 1.0f;
	const float    scaleY = 1.0f;
	int            count;
	int            i;

	if (w <= 0.0f || h <= 0.0f) {
		return;
	}

	XHair_ReadConfigFromCvars(&cfg);
	if (cfg.mode == XHAIR_MODE_NONE) {
		return;
	}

	cfg.dynamicSpreadPx = 0.0f;
	XHair_BuildFrame(&cfg, x + w * 0.5f, y + h * 0.5f, scaleX, scaleY, &frame);

	count = XHair_EmitRects(&frame, XHAIR_PASS_OUTLINE, rects, XHAIR_MAX_RECTS);
	for (i = 0; i < count; i++) {
		color.r = rects[i].r;
		color.g = rects[i].g;
		color.b = rects[i].b;
		color.a = rects[i].a;
		UIR_DrawSolidRect(rects[i].x, rects[i].y, rects[i].w, rects[i].h, &color);
	}

	count = XHair_EmitRects(&frame, XHAIR_PASS_FILL, rects, XHAIR_MAX_RECTS);
	for (i = 0; i < count; i++) {
		color.r = rects[i].r;
		color.g = rects[i].g;
		color.b = rects[i].b;
		color.a = rects[i].a;
		UIR_DrawSolidRect(rects[i].x, rects[i].y, rects[i].w, rects[i].h, &color);
	}
}

/* Added in OPM: black sniper arms from region edges inward to gap (no fixed length). */
static void cl_uir_paint_sniper_reticle(float x, float y, float w, float h, float scale)
{
	uir_color_t black = {0.0f, 0.0f, 0.0f, 1.0f};
	float       th;
	float       gap;
	float       cx;
	float       cy;
	float       barY;
	float       barX;
	float       armW;
	float       armH;
	qboolean    tStyle;

	if (w <= 0.0f || h <= 0.0f || scale <= 0.0f) {
		return;
	}

	/* Changed in OPM: float thickness (no integer cvar/floor/min clamp). */
	th = Cvar_VariableValue("cg_crosshair_sniper_thickness") * scale;
	if (!(th > 0.0f) || th != th) {
		return;
	}
	gap = Cvar_VariableValue("cg_crosshair_sniper_gap") * scale;
	if (gap < 0.0f || gap != gap) {
		gap = 0.0f;
	}
	tStyle = Cvar_VariableIntegerValue("cg_crosshair_sniper_t") ? qtrue : qfalse;

	cx = x + w * 0.5f;
	cy = y + h * 0.5f;
	barY = cy - th * 0.5f;
	barX = cx - th * 0.5f;

	/* Left: left edge -> center - gap */
	armW = (cx - gap) - x;
	if (armW > 0.0f) {
		UIR_DrawSolidRect(x, barY, armW, th, &black);
	}
	/* Right: center + gap -> right edge */
	armW = (x + w) - (cx + gap);
	if (armW > 0.0f) {
		UIR_DrawSolidRect(cx + gap, barY, armW, th, &black);
	}
	/* Bottom: center + gap -> bottom edge */
	armH = (y + h) - (cy + gap);
	if (armH > 0.0f) {
		UIR_DrawSolidRect(barX, cy + gap, th, armH, &black);
	}
	/* Top: top edge -> center - gap (unless T-style) */
	if (!tStyle) {
		armH = (cy - gap) - y;
		if (armH > 0.0f) {
			UIR_DrawSolidRect(barX, y, th, armH, &black);
		}
	}
}

/* Added in OPM: settings WYSIWYG for modern sniper scope (vignette + reticle). */
static void cl_uir_paint_sniper_preview(float x, float y, float w, float h)
{
	float side;
	float ox;
	float oy;

	if (w <= 0.0f || h <= 0.0f) {
		return;
	}
	if (!Cvar_VariableIntegerValue("cg_crosshair_sniper_modern")) {
		return;
	}

	/* Fit square vignette inside the preview plate (same radial stop ratios as zoom). */
	side = (w < h) ? w : h;
	ox = x + (w - side) * 0.5f;
	oy = y + (h - side) * 0.5f;
	(void)UIR_GradientDrawClipped(
		"radial(50% 50%, #00000000 0%, #00000000 88%, #000000FF 96%, #000000FF 100%)",
		ox,
		oy,
		side,
		side,
		NULL,
		0,
		side,
		side,
		0.0f,
		NULL
	);

	cl_uir_paint_sniper_reticle(x, y, w, h, 1.0f);
}

static void uid_draw_host_region(const char *role, float x, float y, float w, float h, void *userdata)
{
	float       colX[7];
	float       colW[7];
	float       bodyY;
	float       bodyH;
	float       textRgba[4] = {1.0f, 1.0f, 1.0f, 0.92f};
	float       mutedRgba[4] = {1.0f, 1.0f, 1.0f, 0.88f};
	float       favRgba[4] = {1.0f, 1.0f, 1.0f, 0.35f};
	uir_color_t headerFill = {0.0f, 0.0f, 0.0f, 0.45f};
	uir_color_t accentLine = {0.102f, 0.435f, 0.831f, 0.55f};
	uir_color_t rowLine = {1.0f, 1.0f, 1.0f, 0.06f};
	uir_color_t hoverFill = {1.0f, 1.0f, 1.0f, 0.05f};
	uir_color_t selFill = {26.0f / 255.0f, 111.0f / 255.0f, 212.0f / 255.0f, 0.28f};
	const char *headers[7] = {"", "SERVER NAME", "MAP", "PLAYERS", "GAMETYPE", "PING", "IP"};
	int         i;
	int         firstRow;
	int         maxRows;

	(void)userdata;
	if (!role || w <= 0.0f || h <= 0.0f) {
		return;
	}
	if (Q_stricmp(role, "crosshair-preview") == 0) {
		cl_uir_paint_crosshair_preview(x, y, w, h);
		return;
	}
	if (Q_stricmp(role, "sniper-preview") == 0) {
		cl_uir_paint_sniper_preview(x, y, w, h);
		return;
	}
	if (Q_stricmp(role, "server-list") == 0) {
		g_browserLastW = w;
		g_browserLastH = h;
		uir_browser_rebuild_visible();
		uir_browser_col_rects(x, w, colX, colW);

		UIR_DrawSolidRect(x, y, w, UIR_BROWSER_HEADER_H, &headerFill);
		for (i = 0; i < 7; i++) {
			if (headers[i][0]) {
				/* Cap-optical vertical center in header band; letter-spacing ~1px via size. */
				uir_browser_draw_text(
					colX[i] + 4.0f,
					y + UIR_BROWSER_HEADER_H * 0.5f - 14.0f * 0.62f,
					14.0f,
					headers[i],
					mutedRgba
				);
			}
		}
		UIR_DrawSolidRect(x, y + UIR_BROWSER_HEADER_H - 1.0f, w, 1.0f, &accentLine);

		bodyY = y + UIR_BROWSER_HEADER_H;
		bodyH = h - UIR_BROWSER_HEADER_H;
		if (bodyH <= 0.0f) {
			return;
		}

		UIR_PushClipRect(x, bodyY, w, bodyH);
		firstRow = (int)(g_browserScrollY / UIR_BROWSER_ROW_H);
		if (firstRow < 0) {
			firstRow = 0;
		}
		maxRows = (int)(bodyH / UIR_BROWSER_ROW_H) + 2;
	for (i = firstRow; i < g_browserVisibleCount && i < firstRow + maxRows; i++) {
		const int               rowId = g_browserVisibleIdx[i];
		const uir_browser_row_t *row = &g_browserRows[rowId];
		float                   rowY = bodyY + (float)i * UIR_BROWSER_ROW_H - g_browserScrollY;
		char                    playersBuf[32];
		char                    pingBuf[16];

		if (rowY + UIR_BROWSER_ROW_H < bodyY || rowY > bodyY + bodyH) {
			continue;
		}
		if (rowId == g_browserSelected) {
			UIR_DrawSolidRect(x, rowY, w, UIR_BROWSER_ROW_H, &selFill);
		} else if (i == g_browserHover) {
			UIR_DrawSolidRect(x, rowY, w, UIR_BROWSER_ROW_H, &hoverFill);
		}

		Com_sprintf(playersBuf, sizeof(playersBuf), "%d/%d", row->players, row->maxPlayers);
		Com_sprintf(pingBuf, sizeof(pingBuf), "%d", row->ping);
		uir_browser_draw_text(colX[0] + 4.0f, rowY + UIR_BROWSER_ROW_H * 0.5f - 16.0f * 0.62f, 18.0f, row->favorite ? "*" : "o", favRgba);
		uir_browser_draw_text(colX[1] + 4.0f, rowY + UIR_BROWSER_ROW_H * 0.5f - 16.0f * 0.62f, 16.0f, row->name, textRgba);
		uir_browser_draw_text(colX[2] + 4.0f, rowY + UIR_BROWSER_ROW_H * 0.5f - 16.0f * 0.62f, 16.0f, row->map, textRgba);
		uir_browser_draw_text(colX[3] + 4.0f, rowY + UIR_BROWSER_ROW_H * 0.5f - 16.0f * 0.62f, 16.0f, playersBuf, textRgba);
		uir_browser_draw_text(colX[4] + 4.0f, rowY + UIR_BROWSER_ROW_H * 0.5f - 16.0f * 0.62f, 16.0f, row->gametype, textRgba);
		uir_browser_draw_text(colX[5] + 4.0f, rowY + UIR_BROWSER_ROW_H * 0.5f - 16.0f * 0.62f, 16.0f, pingBuf, textRgba);
		uir_browser_draw_text(colX[6] + 4.0f, rowY + UIR_BROWSER_ROW_H * 0.5f - 16.0f * 0.62f, 16.0f, row->ip, mutedRgba);
		UIR_DrawSolidRect(x, rowY + UIR_BROWSER_ROW_H - 1.0f, w, 1.0f, &rowLine);
	}
	UIR_PopClipRect();
		return;
	}

}

static bool uid_host_region_pointer(
	const char *role,
	float localX,
	float localY,
	int buttons,
	int wheel,
	void *userdata
)
{
	static int lastButtons = 0;
	const bool leftDown = (buttons & 1) != 0;
	const bool wasLeft = (lastButtons & 1) != 0;
	const bool pressed = leftDown && !wasLeft;
	float      bodyH;
	float      maxScroll;

	(void)userdata;
	lastButtons = buttons;

	if (!role || Q_stricmp(role, "server-list") != 0) {
		return false;
	}

	uir_browser_rebuild_visible();
	bodyH = g_browserLastH - UIR_BROWSER_HEADER_H;
	if (bodyH < 1.0f) {
		bodyH = 1.0f;
	}
	maxScroll = std::max(0.0f, (float)g_browserVisibleCount * UIR_BROWSER_ROW_H - bodyH);

	if (wheel != 0) {
		g_browserScrollY -= (float)wheel * UIR_BROWSER_ROW_H;
		if (g_browserScrollY < 0.0f) {
			g_browserScrollY = 0.0f;
		} else if (g_browserScrollY > maxScroll) {
			g_browserScrollY = maxScroll;
		}
		return true;
	}

	g_browserHover = -1;
	if (localY >= UIR_BROWSER_HEADER_H && localY < g_browserLastH) {
		const int vis = (int)((localY - UIR_BROWSER_HEADER_H + g_browserScrollY) / UIR_BROWSER_ROW_H);
		if (vis >= 0 && vis < g_browserVisibleCount) {
			g_browserHover = vis;
		}
	}

	if (!pressed) {
		return true;
	}

	if (localY >= 0.0f && localY < UIR_BROWSER_HEADER_H) {
		const int col = uir_browser_col_at(localX, g_browserLastW);
		uir_browser_sort_t key = UIR_BROWSER_SORT_NAME;
		switch (col) {
		case 1:
			key = UIR_BROWSER_SORT_NAME;
			break;
		case 2:
			key = UIR_BROWSER_SORT_MAP;
			break;
		case 3:
			key = UIR_BROWSER_SORT_PLAYERS;
			break;
		case 4:
			key = UIR_BROWSER_SORT_GAMETYPE;
			break;
		case 5:
			key = UIR_BROWSER_SORT_PING;
			break;
		case 6:
			key = UIR_BROWSER_SORT_IP;
			break;
		default:
			return true;
		}
		uir_browser_apply_sort_click(key);
		return true;
	}

	if (localY >= UIR_BROWSER_HEADER_H && g_browserHover >= 0 && g_browserHover < g_browserVisibleCount) {
		const int rowId = g_browserVisibleIdx[g_browserHover];
		const int col = uir_browser_col_at(localX, g_browserLastW);
		if (col == 0) {
			g_browserRows[rowId].favorite = g_browserRows[rowId].favorite ? qfalse : qtrue;
		} else {
			g_browserSelected = rowId;
			Cvar_Set("ui_selected_server", g_browserRows[rowId].ip);
		}
	}
	return true;
}

/* Added in OPM: resolve display name / set name / path to a TIKI handle */
static qhandle_t uid_resolve_model_handle(const char *modelPathOrName)
{
	char        path[MAX_QPATH];
	char        remapped[MAX_QPATH];
	const char *file;

	if (!modelPathOrName || !modelPathOrName[0]) {
		return 0;
	}
	if (strchr(modelPathOrName, '/') || strstr(modelPathOrName, ".tik")) {
		return re.RegisterModel(modelPathOrName);
	}
	file = PM_DisplaynameToFilename(modelPathOrName);
	/* Fixed in OPM: menu set names use german_waffen_*; on-disk TIKIs are german_waffenss_*. */
	if (!Q_stricmpn(file, "german_waffen_", 14)) {
		Com_sprintf(remapped, sizeof(remapped), "german_waffenss_%s", file + 14);
		file = remapped;
	}
	Com_sprintf(path, sizeof(path), "models/player/%s.tik", file);
	return re.RegisterModel(path);
}

/*
 * Fixed in OPM: modern menu binds ui_dm_*_set, which legacy UI only creates via
 * ui_getplayermodel. Fall back through set / dm_ / stock defaults so previews
 * still resolve when those cvars were never written.
 */
static const char *uid_fallback_model_name(const char *team)
{
	const qboolean axis = (team && !Q_stricmp(team, "axis")) ? qtrue : qfalse;

	if (axis) {
		const char *name = CvarGetForUI("ui_dm_playergermanmodel_set", "");
		if (name && name[0]) {
			return name;
		}
		name = CvarGetForUI("dm_playergermanmodel", "german_wehrmacht_soldier");
		return (name && name[0]) ? name : "german_wehrmacht_soldier";
	}

	{
		const char *name = CvarGetForUI("ui_dm_playermodel_set", "");
		if (name && name[0]) {
			return name;
		}
		name = CvarGetForUI("dm_playermodel", "american_army");
		return (name && name[0]) ? name : "american_army";
	}
}

static void uid_queue_model_preview(const uid_model_preview_desc_t *desc)
{
	uir_rect_t                 rect;
	uir_model_preview_params_t params;
	const uir_viewport_t      *vp = UIR_CompositorViewport();
	float                      x0, y0, x1, y1;
	qhandle_t                  handle;
	const char                *anim = desc ? desc->anim : NULL;
	const char                *modelName = desc ? desc->model : NULL;
	char                       tunakAnim[64];
	unsigned int               instanceHash = 0;
	const char                *key;

	if (!desc || !vp || desc->w <= 1.0f || desc->h <= 1.0f) {
		if (uir_debug && uir_debug->integer && desc) {
			Com_Printf("UIR: model preview skipped (vp=%p size=%.1fx%.1f)\n", (void *)vp, desc->w, desc->h);
		}
		return;
	}
	if (!modelName || !modelName[0]) {
		modelName = uid_fallback_model_name(desc->team);
	}
	handle = uid_resolve_model_handle(modelName);
	if (!handle) {
		if (uir_debug && uir_debug->integer) {
			Com_Printf("UIR: model preview RegisterModel failed for '%s'\n", modelName ? modelName : "");
		}
		return;
	}
	if (uir_debug && uir_debug->integer) {
		Com_Printf(
			"UIR: queue model '%s' handle=%d rect=%.1f,%.1f %.1fx%.1f\n",
			modelName,
			(int)handle,
			desc->x,
			desc->y,
			desc->w,
			desc->h
		);
	}

	UIR_ViewportDrawToFb(vp, desc->x, desc->y, &x0, &y0);
	UIR_ViewportDrawToFb(vp, desc->x + desc->w, desc->y + desc->h, &x1, &y1);
	rect.x = (x0 < x1) ? x0 : x1;
	rect.y = (y0 < y1) ? y0 : y1;
	rect.w = std::fabs(x1 - x0);
	rect.h = std::fabs(y1 - y0);

	memset(&params, 0, sizeof(params));
	if (desc->hasAngles) {
		params.angles[0] = desc->angles[0];
		params.angles[1] = desc->angles[1];
		params.angles[2] = desc->angles[2];
		params.hasAngles = 1;
	} else {
		params.angles[0] = 10.0f;
		params.angles[1] = 180.0f;
		params.angles[2] = 0.0f;
	}
	params.modelHandle = handle;
	if (Cvar_VariableIntegerValue("tunak") && anim && anim[0]) {
		if (desc->team && !Q_stricmp(desc->team, "axis")) {
			Q_strncpyz(tunakAnim, "zzgermanspecialidle", sizeof(tunakAnim));
		} else if (strstr(anim, "german")) {
			Q_strncpyz(tunakAnim, "zzgermanspecialidle", sizeof(tunakAnim));
		} else {
			Q_strncpyz(tunakAnim, "zzamericanspecialidle", sizeof(tunakAnim));
		}
		anim = tunakAnim;
	}
	params.animName = anim;
	params.animVariant = desc->animVariant >= 0 ? desc->animVariant : 0;
	params.modelScale = desc->hasScale ? desc->scale : UIR_MP_MODEL_SCALE_DEFAULT;
	if (desc->hasOffset) {
		params.offset[0] = desc->offset[0];
		params.offset[1] = desc->offset[1];
		params.offset[2] = desc->offset[2];
		params.hasOffset = 1;
	}
	if (desc->hasFramingScale) {
		params.framingScale = desc->framingScale;
		params.hasFramingScale = 1;
	}
	if (desc->hasBbox) {
		VectorCopy(desc->bboxMins, params.bboxMins);
		VectorCopy(desc->bboxMaxs, params.bboxMaxs);
		params.hasBbox = 1;
	}
	params.bboxFromModel = desc->bboxFromModel ? 1 : 0;
	if (desc->hasFov) {
		params.fov = desc->fov;
	} else {
		params.fov = UIR_MP_FOV_DEFAULT;
	}
	if (desc->hasColor) {
		params.color[0] = desc->color[0];
		params.color[1] = desc->color[1];
		params.color[2] = desc->color[2];
		params.color[3] = desc->color[3];
		params.hasColor = 1;
	} else {
		params.color[0] = params.color[1] = params.color[2] = params.color[3] = 1.0f;
	}
	for (key = desc->instanceKey; key && *key; key++) {
		instanceHash = instanceHash * 31u + (unsigned char)*key;
	}
	if (!desc->instanceKey || !desc->instanceKey[0]) {
		instanceHash = (unsigned int)(desc->x * 17.0f + desc->y * 31.0f + desc->w * 13.0f + desc->h * 7.0f);
	}
	params.instanceId = (int)(instanceHash & 0x7fffffff);
	if (desc->hasAnimPhase) {
		params.animPhase = desc->animPhase;
	} else {
		params.animPhase = (float)(instanceHash % 1000u) / 1000.0f;
		if (desc->team && !Q_stricmp(desc->team, "axis")) {
			params.animPhase += 0.5f;
		}
	}
	while (params.animPhase >= 1.0f) {
		params.animPhase -= 1.0f;
	}
	while (params.animPhase < 0.0f) {
		params.animPhase += 1.0f;
	}
	params.serverTime = cl.serverTime;
	params.realtime = cls.realtime;

	UIR_QueueModelPreview(&rect, &params);
}

/* Draft settings cvars reset by settings-defaults (catalog apply/change set). */
static const char *const g_uidSettingsDraftCvars[] = {
	"r_mode",
	"r_fullscreen",
	"r_noborder",
	"r_displayRefresh",
	"r_swapInterval",
	"r_gamma",
	"r_colorbits",
	"r_texturebits",
	"r_picmip",
	"r_textureMode",
	"r_ext_compressed_textures",
	"r_fastentlight",
	"r_entlightmap",
	"r_flares",
	"r_drawstaticdecals",
	"r_lodscale",
	"com_maxfps",
	"fps",
	"s_volume",
	"s_musicvolume",
	"s_speaker_type",
	"s_khz",
	"s_reverb",
	"s_initsound",
	"s_milesdriver",
	"cg_fov",
	"cg_drawviewmodel",
	"cg_hud",
	"cg_autoswitch",
	"cg_rain",
	"cg_marks_add",
	"cg_shadows",
	"cg_effectdetail",
	"vss_draw",
	"ui_weaponsbar",
	"ui_om_hud",
	"ui_om_menu_map_view",
	"com_blood",
	"m_filter",
	"m_yaw",
	"m_pitch",
	"cl_mouseAccel",
	"cl_run",
	"in_mouse",
	"sensitivity",
	"ui_modernsettings_dpi",
	"ui_modernsettings_sensitivity_mode",
	"ui_scale",
	"cg_crosshair_mode",
	"cg_crosshairsize",
	"cg_crosshairgap",
	"cg_crosshairthickness",
	"cg_crosshaircolor",
	"cg_crosshaircolor_r",
	"cg_crosshaircolor_g",
	"cg_crosshaircolor_b",
	"cg_crosshairalpha",
	"cg_crosshairusealpha",
	"cg_crosshairdot",
	"cg_crosshair_t",
	"cg_crosshair_drawoutline",
	"cg_crosshair_outlinethickness",
	"cg_crosshair_recoil",
	"cg_crosshair_friendly_warning",
	"cg_crosshair_sniper_thickness",
	"cg_crosshair_sniper_gap",
	"cg_crosshair_sniper_size",
	"cg_crosshair_sniper_t",
	"cg_crosshair_sniper_modern",
	"cg_zoomSensitivity",
	"cg_crosshair_solid_size",
	"cg_crosshair_dot_size",
	"cg_crosshair_dynamic",
	"cg_crosshair_dynamic_movement",
	"cg_crosshair_dynamic_scale",
	"ui_om_scoreboard_disable_cursor",
	NULL
};

/* Video cvars that require vid_restart when changed by settings-apply. */
static const char *const g_uidVideoRestartCvars[] = {
	"r_mode",
	"r_fullscreen",
	"r_picmip",
	"r_ext_compressed_textures",
	"r_noborder",
	"r_displayRefresh",
	"r_swapInterval",
	"r_colorbits",
	"r_texturebits",
	NULL
};

/* Sound cvars that require snd_restart when changed by settings-apply. */
static const char *const g_uidSoundRestartCvars[] = {
	"s_speaker_type",
	"s_khz",
	"s_reverb",
	"s_initsound",
	"s_milesdriver",
	NULL
};

/*
 * R_GetModeInfo lives in the renderer DLL and is not on the client import
 * table, so enumerate a fixed list matching retail r_mode indices.
 * Player model lists match ImprovedBrowser menu.html option values.
 */
/* Added in OPM: catalog option sources for cyclic selects (value + label). */
static int uid_fill_option_pairs(
	const char *const *pairs,
	int pairCount,
	char **values,
	char **labels,
	int max,
	char valueBuf[][96],
	char labelBuf[][96],
	int bufCount
)
{
	int i;
	int n = pairCount / 2;
	if (n > max) {
		n = max;
	}
	if (n > bufCount) {
		n = bufCount;
	}
	for (i = 0; i < n; i++) {
		Q_strncpyz(valueBuf[i], pairs[i * 2], 96);
		Q_strncpyz(labelBuf[i], pairs[i * 2 + 1], 96);
		values[i] = valueBuf[i];
		labels[i] = labelBuf[i];
	}
	return n;
}

/* Added in Omaha: unique SDL refresh rates for display-refresh cyclic (+ Default=0). */
static int uid_cmp_refresh_asc(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static void uid_add_unique_refresh(int *rates, int *nRates, int maxRates, int hz)
{
	int i;

	if (hz <= 0 || !rates || !nRates || *nRates >= maxRates) {
		return;
	}
	for (i = 0; i < *nRates; i++) {
		if (rates[i] == hz) {
			return;
		}
	}
	rates[(*nRates)++] = hz;
}

static int uid_fill_display_refresh(
	char **values,
	char **labels,
	int max,
	char valueBuf[][96],
	char labelBuf[][96],
	int bufCount
)
{
	static const int fallback[] = {50, 60, 75, 100, 120, 144, 165, 180, 200, 240, 360};
	int              rates[64];
	int              nRates = 0;
	int              n = 0;
	int              i;
	int              current;
	const int        maxRates = (int)(sizeof(rates) / sizeof(rates[0]));

	if (!values || !labels || max <= 0 || bufCount <= 0) {
		return 0;
	}

	if (SDL_WasInit(SDL_INIT_VIDEO)) {
		const int numDisplays = SDL_GetNumVideoDisplays();
		int       d;

		for (d = 0; d < numDisplays; d++) {
			const int numModes = SDL_GetNumDisplayModes(d);
			int       mi;

			for (mi = 0; mi < numModes; mi++) {
				SDL_DisplayMode mode;

				if (SDL_GetDisplayMode(d, mi, &mode) < 0) {
					continue;
				}
				uid_add_unique_refresh(rates, &nRates, maxRates, mode.refresh_rate);
			}
		}
	}

	if (nRates == 0) {
		for (i = 0; i < (int)(sizeof(fallback) / sizeof(fallback[0])); i++) {
			uid_add_unique_refresh(rates, &nRates, maxRates, fallback[i]);
		}
	}

	current = Cvar_VariableIntegerValue("r_displayRefresh");
	uid_add_unique_refresh(rates, &nRates, maxRates, current);

	if (nRates > 1) {
		qsort(rates, (size_t)nRates, sizeof(rates[0]), uid_cmp_refresh_asc);
	}

	if (n < max && n < bufCount) {
		Q_strncpyz(valueBuf[n], "0", 96);
		Q_strncpyz(labelBuf[n], "Default", 96);
		values[n] = valueBuf[n];
		labels[n] = labelBuf[n];
		n++;
	}
	for (i = 0; i < nRates && n < max && n < bufCount; i++) {
		Com_sprintf(valueBuf[n], 96, "%d", rates[i]);
		Com_sprintf(labelBuf[n], 96, "%d Hz", rates[i]);
		values[n] = valueBuf[n];
		labels[n] = labelBuf[n];
		n++;
	}
	return n;
}

typedef struct {
	char        key[16];
	char        value[64];
	char        label[UIR_BROWSER_NAME_LEN];
	char        fav[8];
	char        favFill[16];
	char        name[UIR_BROWSER_NAME_LEN];
	char        map[32];
	char        players[16];
	char        gametype[32];
	char        ping[16];
	const char *fieldNames[7];
	const char *fieldValues[7];
} uid_server_collection_slot_t;

static int uid_query_collection_options(
	const char *source,
	int offset,
	int limit,
	int *outTotal,
	uint64_t *outRevision,
	uid_collection_item_t *out,
	int max
)
{
	static char valueBuf[64][96];
	static char labelBuf[64][96];
	static char keyBuf[64][16];
	static const char *const videoModes[] = {
		"-2", "Desktop", "-1", "Custom", "6", "1024 x 768", "7", "1152 x 864", "8", "1280 x 1024",
		"9", "1600 x 1200", "10", "2048 x 1536",
	};
	const int bufCount = (int)(sizeof(valueBuf) / sizeof(valueBuf[0]));

	if (!source || !out || max <= 0) {
		return 0;
	}

	char *values[64];
	char *labels[64];
	int   total = 0;

	if (!Q_stricmp(source, "display-refresh")) {
		/* Added in Omaha: host-backed SDL refresh rates (not XML). */
		total = uid_fill_display_refresh(values, labels, bufCount, valueBuf, labelBuf, bufCount);
	} else if (!Q_stricmp(source, "video-modes")) {
		total = uid_fill_option_pairs(
			videoModes,
			(int)(sizeof(videoModes) / sizeof(videoModes[0])),
			values,
			labels,
			bufCount,
			valueBuf,
			labelBuf,
			bufCount
		);
	} else {
		return 0;
	}

	if (total <= 0) {
		return 0;
	}

	if (outTotal) {
		*outTotal = total;
	}
	if (outRevision) {
		*outRevision = 1;
	}
	if (offset < 0) {
		offset = 0;
	}
	if (limit <= 0) {
		limit = max;
	}

	int written = 0;
	for (int i = offset; i < total && written < max && written < limit; i++) {
		Com_sprintf(keyBuf[written], sizeof(keyBuf[written]), "%d", i);
		out[written].key = keyBuf[written];
		out[written].value = valueBuf[i];
		out[written].label = labelBuf[i];
		out[written].nfields = 0;
		out[written].fieldNames = NULL;
		out[written].fieldValues = NULL;
		out[written].flags = 0;
		written++;
	}
	return written;
}

static int uid_query_collection_servers(
	int offset,
	int limit,
	int *outTotal,
	uint64_t *outRevision,
	uid_collection_item_t *out,
	int max
)
{
	static uid_server_collection_slot_t slots[UIR_BROWSER_MAX_ROWS];
	static const char *const kFieldNames[] = {
		"favorite", "favorite_fill", "name", "map", "players", "gametype", "ping",
	};
	int i;
	int written = 0;

	uir_browser_rebuild_visible();
	if (outTotal) {
		*outTotal = g_browserVisibleCount;
	}
	if (outRevision) {
		*outRevision = g_browserCollectionRevision;
	}
	if (!out || max <= 0) {
		return 0;
	}
	if (offset < 0) {
		offset = 0;
	}
	if (limit <= 0) {
		limit = max;
	}

	for (i = offset; i < g_browserVisibleCount && written < max && written < limit; i++) {
		const int               rowId = g_browserVisibleIdx[i];
		const uir_browser_row_t *row = &g_browserRows[rowId];
		uid_server_collection_slot_t *slot = &slots[written];

		Com_sprintf(slot->key, sizeof(slot->key), "%d", i);
		Q_strncpyz(slot->value, row->ip, sizeof(slot->value));
		Q_strncpyz(slot->label, row->name, sizeof(slot->label));
		Q_strncpyz(slot->fav, row->favorite ? "*" : "o", sizeof(slot->fav));
		Q_strncpyz(
			slot->favFill,
			row->favorite ? "#F4C430EB" : "#EBF0F559",
			sizeof(slot->favFill)
		);
		Q_strncpyz(slot->name, row->name, sizeof(slot->name));
		Q_strncpyz(slot->map, row->map, sizeof(slot->map));
		Com_sprintf(slot->players, sizeof(slot->players), "%d/%d", row->players, row->maxPlayers);
		Q_strncpyz(slot->gametype, row->gametype, sizeof(slot->gametype));
		Com_sprintf(slot->ping, sizeof(slot->ping), "%d", row->ping);
		slot->fieldNames[0] = kFieldNames[0];
		slot->fieldNames[1] = kFieldNames[1];
		slot->fieldNames[2] = kFieldNames[2];
		slot->fieldNames[3] = kFieldNames[3];
		slot->fieldNames[4] = kFieldNames[4];
		slot->fieldNames[5] = kFieldNames[5];
		slot->fieldNames[6] = kFieldNames[6];
		slot->fieldValues[0] = slot->fav;
		slot->fieldValues[1] = slot->favFill;
		slot->fieldValues[2] = slot->name;
		slot->fieldValues[3] = slot->map;
		slot->fieldValues[4] = slot->players;
		slot->fieldValues[5] = slot->gametype;
		slot->fieldValues[6] = slot->ping;

		out[written].key = slot->key;
		out[written].value = slot->value;
		out[written].label = slot->label;
		out[written].nfields = 7;
		out[written].fieldNames = slot->fieldNames;
		out[written].fieldValues = slot->fieldValues;
		out[written].flags = 0;
		written++;
	}
	return written;
}

typedef struct {
	char        key[16];
	char        kind[16];
	char        slot[16];
	char        name[64];
	char        kills[16];
	char        deaths[16];
	char        kd[16];
	char        time[32];
	char        ping[16];
	char        textColor[16];
	char        rowFill[16];
	char        isHeader[4];
	char        isSpectator[4];
	char        isLocal[4];
	char        isDead[4];
	char        team[12];
	const char *fieldNames[15];
	const char *fieldValues[15];
} uid_scoreboard_collection_slot_t;

static int uid_query_collection_scoreboard(
	int offset,
	int limit,
	int *outTotal,
	uint64_t *outRevision,
	uid_collection_item_t *out,
	int max
)
{
	static uid_scoreboard_collection_slot_t slots[UIR_SCOREBOARD_MAX_ROWS];
	static const char *const kFieldNames[] = {
		"kind", "slot", "name", "kills", "deaths", "kd", "time", "ping", "text_color", "row_fill", "is_header",
		"is_spectator", "is_local", "is_dead", "team",
	};
	const int rowCount = UIR_Scoreboard_GetRowCount();
	int       written = 0;
	int       i;

	if (outTotal) {
		*outTotal = rowCount;
	}
	if (outRevision) {
		*outRevision = UIR_Scoreboard_GetRevision();
	}
	if (!out || max <= 0) {
		return 0;
	}
	if (offset < 0) {
		offset = 0;
	}
	if (limit <= 0) {
		limit = max;
	}

	for (i = offset; i < rowCount && written < max && written < limit; i++) {
		const uir_scoreboard_row_t *row = UIR_Scoreboard_GetRow(i);
		uid_scoreboard_collection_slot_t *slot;

		if (!row) {
			continue;
		}
		slot = &slots[written];

		Com_sprintf(slot->key, sizeof(slot->key), "%d", i);
		Q_strncpyz(slot->kind, row->kind == UIR_SCORE_ROW_HEADER ? "header"
			: row->kind == UIR_SCORE_ROW_SPACER ? "spacer" : "player", sizeof(slot->kind));
		Q_strncpyz(slot->slot, row->slot, sizeof(slot->slot));
		Q_strncpyz(slot->name, row->name, sizeof(slot->name));
		Q_strncpyz(slot->kills, row->kills, sizeof(slot->kills));
		Q_strncpyz(slot->deaths, row->deaths, sizeof(slot->deaths));
		Q_strncpyz(slot->kd, row->kd, sizeof(slot->kd));
		Q_strncpyz(slot->time, row->time, sizeof(slot->time));
		Q_strncpyz(slot->ping, row->ping, sizeof(slot->ping));
		Q_strncpyz(slot->textColor, row->textColor, sizeof(slot->textColor));
		Q_strncpyz(slot->rowFill, row->rowFill, sizeof(slot->rowFill));
		Q_strncpyz(slot->isHeader, row->isHeader ? "1" : "0", sizeof(slot->isHeader));
		Q_strncpyz(slot->isSpectator, row->isSpectator ? "1" : "0", sizeof(slot->isSpectator));
		/* Added in OPM: local player row for scoreboard bold styling. */
		Q_strncpyz(
			slot->isLocal,
			(row->clientNum >= 0 && row->clientNum == cl.snap.ps.clientNum) ? "1" : "0",
			sizeof(slot->isLocal)
		);
		/* Added in Omaha: dead from signed team id for modern scoreboard mute styling. */
		Q_strncpyz(slot->isDead, row->isDead ? "1" : "0", sizeof(slot->isDead));
		Q_strncpyz(slot->team, row->team, sizeof(slot->team));

		slot->fieldNames[0] = kFieldNames[0];
		slot->fieldNames[1] = kFieldNames[1];
		slot->fieldNames[2] = kFieldNames[2];
		slot->fieldNames[3] = kFieldNames[3];
		slot->fieldNames[4] = kFieldNames[4];
		slot->fieldNames[5] = kFieldNames[5];
		slot->fieldNames[6] = kFieldNames[6];
		slot->fieldNames[7] = kFieldNames[7];
		slot->fieldNames[8] = kFieldNames[8];
		slot->fieldNames[9] = kFieldNames[9];
		slot->fieldNames[10] = kFieldNames[10];
		slot->fieldNames[11] = kFieldNames[11];
		slot->fieldNames[12] = kFieldNames[12];
		slot->fieldNames[13] = kFieldNames[13];
		slot->fieldNames[14] = kFieldNames[14];
		slot->fieldValues[0] = slot->kind;
		slot->fieldValues[1] = slot->slot;
		slot->fieldValues[2] = slot->name;
		slot->fieldValues[3] = slot->kills;
		slot->fieldValues[4] = slot->deaths;
		slot->fieldValues[5] = slot->kd;
		slot->fieldValues[6] = slot->time;
		slot->fieldValues[7] = slot->ping;
		slot->fieldValues[8] = slot->textColor;
		slot->fieldValues[9] = slot->rowFill;
		slot->fieldValues[10] = slot->isHeader;
		slot->fieldValues[11] = slot->isSpectator;
		slot->fieldValues[12] = slot->isLocal;
		slot->fieldValues[13] = slot->isDead;
		slot->fieldValues[14] = slot->team;

		out[written].key = slot->key;
		out[written].value = slot->key;
		out[written].label = slot->name[0] ? slot->name : slot->kind;
		out[written].nfields = 15;
		out[written].fieldNames = slot->fieldNames;
		out[written].fieldValues = slot->fieldValues;
		out[written].flags = 0;
		written++;
	}
	return written;
}

static int uid_query_collection_hud_packs(
	int offset,
	int limit,
	int *outTotal,
	uint64_t *outRevision,
	uid_collection_item_t *out,
	int max
)
{
	static char valueBuf[64][96];
	static char labelBuf[64][96];
	static char keyBuf[64][16];
	const int total = CL_UIMenu_HudCount();

	if (outTotal) {
		*outTotal = total;
	}
	if (outRevision) {
		*outRevision = CL_UIMenu_HudRegistryRevision();
	}
	if (!out || max <= 0) {
		return 0;
	}
	if (offset < 0) {
		offset = 0;
	}
	if (limit <= 0) {
		limit = max;
	}

	int written = 0;
	for (int i = offset; i < total && written < max && written < limit; i++) {
		const char *id = NULL;
		const char *label = NULL;
		CL_UIMenu_HudEntryAt(i, &id, &label, NULL);
		if (!id || !id[0]) {
			continue;
		}
		Q_strncpyz(valueBuf[written], id, sizeof(valueBuf[written]));
		Q_strncpyz(labelBuf[written], label && label[0] ? label : id, sizeof(labelBuf[written]));
		Com_sprintf(keyBuf[written], sizeof(keyBuf[written]), "%d", i);
		out[written].key = keyBuf[written];
		out[written].value = valueBuf[written];
		out[written].label = labelBuf[written];
		out[written].nfields = 0;
		out[written].fieldNames = NULL;
		out[written].fieldValues = NULL;
		out[written].flags = 0;
		written++;
	}
	return written;
}

typedef struct {
	char key[16];
	char text[256];
	char hidden[8];
	char completed[8];
	char current[8];
	char highlight[8];
	const char *fieldNames[5];
	const char *fieldValues[5];
} uid_objective_collection_slot_t;

static int uid_query_collection_objectives(
	int offset,
	int limit,
	int *outTotal,
	uint64_t *outRevision,
	uid_collection_item_t *out,
	int max
)
{
	static uid_objective_collection_slot_t slots[UIR_OBJECTIVES_MAX_ROWS];
	static const char *const kFieldNames[] = {"text", "hidden", "completed", "current", "highlight"};
	const int rowCount = UIR_Objectives_GetRowCount();
	int       written = 0;
	int       i;

	if (outTotal) {
		*outTotal = rowCount;
	}
	if (outRevision) {
		*outRevision = UIR_Objectives_GetRevision();
	}
	if (!out || max <= 0) {
		return 0;
	}
	if (offset < 0) {
		offset = 0;
	}
	if (limit <= 0) {
		limit = max;
	}

	for (i = offset; i < rowCount && written < max && written < limit; i++) {
		const uir_objective_row_t *row = UIR_Objectives_GetRow(i);
		uid_objective_collection_slot_t *slot;

		if (!row) {
			continue;
		}
		slot = &slots[written];
		Com_sprintf(slot->key, sizeof(slot->key), "%d", i);
		Q_strncpyz(slot->text, row->text, sizeof(slot->text));
		Com_sprintf(slot->hidden, sizeof(slot->hidden), "%d", row->hidden);
		Com_sprintf(slot->completed, sizeof(slot->completed), "%d", row->completed);
		Com_sprintf(slot->current, sizeof(slot->current), "%d", row->current);
		Com_sprintf(slot->highlight, sizeof(slot->highlight), "%d", row->highlight);

		slot->fieldNames[0] = kFieldNames[0];
		slot->fieldNames[1] = kFieldNames[1];
		slot->fieldNames[2] = kFieldNames[2];
		slot->fieldNames[3] = kFieldNames[3];
		slot->fieldNames[4] = kFieldNames[4];
		slot->fieldValues[0] = slot->text;
		slot->fieldValues[1] = slot->hidden;
		slot->fieldValues[2] = slot->completed;
		slot->fieldValues[3] = slot->current;
		slot->fieldValues[4] = slot->highlight;

		out[written].key = slot->key;
		out[written].value = slot->key;
		out[written].label = slot->text;
		out[written].nfields = 5;
		out[written].fieldNames = slot->fieldNames;
		out[written].fieldValues = slot->fieldValues;
		out[written].flags = 0;
		written++;
	}
	return written;
}

typedef struct {
	char key[32];
	char text[512];
	char color[16];
	char alpha[16];
	char bold[8];
	const char *fieldNames[4];
	const char *fieldValues[4];
} uid_hud_message_collection_slot_t;

static int uid_query_collection_hud_messages(
	int offset,
	int limit,
	int *outTotal,
	uint64_t *outRevision,
	uid_collection_item_t *out,
	int max,
	int which /* 0=messages 1=game 2=chat */
)
{
	static uid_hud_message_collection_slot_t slots[UIR_HUD_MESSAGES_MAX_ROWS];
	static const char *const kFieldNames[] = {"text", "color", "alpha", "bold"};
	int       rowCount;
	uint64_t  revision;
	int       written = 0;
	int       i;

	if (which == 1) {
		rowCount = UIR_HudGameMessages_GetRowCount();
		revision = UIR_HudGameMessages_GetRevision();
	} else if (which == 2) {
		rowCount = UIR_HudChat_GetRowCount();
		revision = UIR_HudChat_GetRevision();
	} else {
		rowCount = UIR_HudMessages_GetRowCount();
		revision = UIR_HudMessages_GetRevision();
	}

	if (outTotal) {
		*outTotal = rowCount;
	}
	if (outRevision) {
		*outRevision = revision;
	}
	if (!out || max <= 0) {
		return 0;
	}
	if (offset < 0) {
		offset = 0;
	}
	if (limit <= 0) {
		limit = max;
	}

	for (i = offset; i < rowCount && written < max && written < limit; i++) {
		uir_hud_message_row_t             row;
		uid_hud_message_collection_slot_t *slot;

		std::memset(&row, 0, sizeof(row));
		if (which == 1) {
			UIR_HudGameMessages_GetRow(i, &row);
		} else if (which == 2) {
			UIR_HudChat_GetRow(i, &row);
		} else {
			UIR_HudMessages_GetRow(i, &row);
		}

		slot = &slots[written];
		/* Changed in OPM: stable monotonic keys so foreach lifetime tracks rows. */
		Com_sprintf(slot->key, sizeof(slot->key), "msg_%llu", (unsigned long long)row.stableId);
		Q_strncpyz(slot->text, row.text, sizeof(slot->text));
		Q_strncpyz(slot->color, row.color, sizeof(slot->color));
		Com_sprintf(slot->alpha, sizeof(slot->alpha), "%.3f", row.alpha);
		Com_sprintf(slot->bold, sizeof(slot->bold), "%d", row.bold);

		slot->fieldNames[0] = kFieldNames[0];
		slot->fieldNames[1] = kFieldNames[1];
		slot->fieldNames[2] = kFieldNames[2];
		slot->fieldNames[3] = kFieldNames[3];
		slot->fieldValues[0] = slot->text;
		slot->fieldValues[1] = slot->color;
		slot->fieldValues[2] = slot->alpha;
		slot->fieldValues[3] = slot->bold;

		out[written].key = slot->key;
		out[written].value = slot->key;
		out[written].label = slot->text;
		out[written].nfields = 4;
		out[written].fieldNames = slot->fieldNames;
		out[written].fieldValues = slot->fieldValues;
		out[written].flags = 0;
		written++;
	}
	return written;
}

/* Added in OPM: structured kill-feed collection query. */
typedef struct {
	char key[32];
	char killer[64];
	char victim[64];
	char weaponClass[32];
	char killerTeam[16];
	char victimTeam[16];
	char iconTeam[16];
	char killKind[16];
	char text[512];
	char color[16];
	char headshot[8];
	char friendly[8];
	const char *fieldNames[11];
	const char *fieldValues[11];
} uid_hud_kill_feed_collection_slot_t;

static int uid_query_collection_hud_kill_feed(
	int offset,
	int limit,
	int *outTotal,
	uint64_t *outRevision,
	uid_collection_item_t *out,
	int max
)
{
	static uid_hud_kill_feed_collection_slot_t slots[UIR_HUD_KILL_FEED_MAX_ROWS];
	static const char *const kFieldNames[] = {
		"killer",
		"victim",
		"weapon_class",
		"killer_team",
		"victim_team",
		"icon_team",
		"headshot",
		"kill_kind",
		"friendly",
		"text",
		"color"
	};
	const int rowCount = UIR_HudKillFeed_GetRowCount();
	int       written = 0;
	int       i;

	if (outTotal) {
		*outTotal = rowCount;
	}
	if (outRevision) {
		*outRevision = UIR_HudKillFeed_GetRevision();
	}
	if (!out || max <= 0) {
		return 0;
	}
	if (offset < 0) {
		offset = 0;
	}
	if (limit <= 0) {
		limit = max;
	}

	for (i = offset; i < rowCount && written < max && written < limit; i++) {
		uir_hud_kill_feed_row_t               row;
		uid_hud_kill_feed_collection_slot_t *slot;
		int                                   f;

		std::memset(&row, 0, sizeof(row));
		UIR_HudKillFeed_GetRow(i, &row);

		slot = &slots[written];
		Com_sprintf(slot->key, sizeof(slot->key), "kill_%llu", (unsigned long long)row.stableId);
		Q_strncpyz(slot->killer, row.killer, sizeof(slot->killer));
		Q_strncpyz(slot->victim, row.victim, sizeof(slot->victim));
		Q_strncpyz(slot->weaponClass, row.weaponClass, sizeof(slot->weaponClass));
		Q_strncpyz(slot->killerTeam, row.killerTeam, sizeof(slot->killerTeam));
		Q_strncpyz(slot->victimTeam, row.victimTeam, sizeof(slot->victimTeam));
		Q_strncpyz(slot->iconTeam, row.iconTeam, sizeof(slot->iconTeam));
		Q_strncpyz(slot->killKind, row.killKind, sizeof(slot->killKind));
		Q_strncpyz(slot->text, row.text, sizeof(slot->text));
		Q_strncpyz(slot->color, row.color, sizeof(slot->color));
		Com_sprintf(slot->headshot, sizeof(slot->headshot), "%d", row.headshot);
		Com_sprintf(slot->friendly, sizeof(slot->friendly), "%d", row.friendly);

		for (f = 0; f < 11; f++) {
			slot->fieldNames[f] = kFieldNames[f];
		}
		slot->fieldValues[0] = slot->killer;
		slot->fieldValues[1] = slot->victim;
		slot->fieldValues[2] = slot->weaponClass;
		slot->fieldValues[3] = slot->killerTeam;
		slot->fieldValues[4] = slot->victimTeam;
		slot->fieldValues[5] = slot->iconTeam;
		slot->fieldValues[6] = slot->headshot;
		slot->fieldValues[7] = slot->killKind;
		slot->fieldValues[8] = slot->friendly;
		slot->fieldValues[9] = slot->text;
		slot->fieldValues[10] = slot->color;

		out[written].key = slot->key;
		out[written].value = slot->key;
		out[written].label = slot->text;
		out[written].nfields = 11;
		out[written].fieldNames = slot->fieldNames;
		out[written].fieldValues = slot->fieldValues;
		out[written].flags = 0;
		written++;
	}
	return written;
}

/* Added in OPM: vote option rows filled by cgame into ui_om_vote_* cvars. */
typedef struct {
	char        key[16];
	char        label[128];
	char        cmd[256];
	char        type[16];
	char        index[16];
	const char *fieldNames[4];
	const char *fieldValues[4];
} uid_vote_collection_slot_t;

static int uid_query_collection_vote_options(
	int offset,
	int limit,
	int *outTotal,
	uint64_t *outRevision,
	uid_collection_item_t *out,
	int max
)
{
	static uid_vote_collection_slot_t slots[64];
	static const char *const kFieldNames[] = {"label", "cmd", "type", "index"};
	const int count = Cvar_VariableIntegerValue("ui_om_vote_count");
	int       written = 0;
	int       i;
	int       n = count;

	if (n < 0) {
		n = 0;
	}
	if (n > 64) {
		n = 64;
	}
	if (outTotal) {
		*outTotal = n;
	}
	if (outRevision) {
		*outRevision = (uint64_t)n + ((uint64_t)Cvar_VariableIntegerValue("ui_om_vote_active") << 16);
	}
	if (!out || max <= 0) {
		return 0;
	}
	if (offset < 0) {
		offset = 0;
	}
	if (limit <= 0) {
		limit = max;
	}

	for (i = offset; i < n && written < max && written < limit; i++) {
		uid_vote_collection_slot_t *slot = &slots[written];
		char                        nameBuf[48];

		Com_sprintf(slot->key, sizeof(slot->key), "%d", i);
		Com_sprintf(nameBuf, sizeof(nameBuf), "ui_om_vote_%d_label", i);
		Q_strncpyz(slot->label, Cvar_VariableString(nameBuf), sizeof(slot->label));
		Com_sprintf(nameBuf, sizeof(nameBuf), "ui_om_vote_%d_cmd", i);
		Q_strncpyz(slot->cmd, Cvar_VariableString(nameBuf), sizeof(slot->cmd));
		Com_sprintf(nameBuf, sizeof(nameBuf), "ui_om_vote_%d_type", i);
		Q_strncpyz(slot->type, Cvar_VariableString(nameBuf), sizeof(slot->type));
		Com_sprintf(nameBuf, sizeof(nameBuf), "ui_om_vote_%d_index", i);
		Q_strncpyz(slot->index, Cvar_VariableString(nameBuf), sizeof(slot->index));

		slot->fieldNames[0] = kFieldNames[0];
		slot->fieldNames[1] = kFieldNames[1];
		slot->fieldNames[2] = kFieldNames[2];
		slot->fieldNames[3] = kFieldNames[3];
		slot->fieldValues[0] = slot->label;
		slot->fieldValues[1] = slot->cmd;
		slot->fieldValues[2] = slot->type;
		slot->fieldValues[3] = slot->index;

		out[written].key = slot->key;
		out[written].value = slot->cmd;
		out[written].label = slot->label;
		out[written].nfields = 4;
		out[written].fieldNames = slot->fieldNames;
		out[written].fieldValues = slot->fieldValues;
		out[written].flags = 0;
		written++;
	}
	return written;
}

static int uid_query_collection_items(const uid_collection_query_t *query, uid_collection_item_t *out, int max)
{
	if (!query || !query->source) {
		return 0;
	}
	if (!Q_stricmp(query->source, "vote-options")) {
		return uid_query_collection_vote_options(
			query->offset, query->limit, query->outTotal, query->outRevision, out, max
		);
	}
	if (!Q_stricmp(query->source, "servers")) {
		return uid_query_collection_servers(
			query->offset, query->limit, query->outTotal, query->outRevision, out, max
		);
	}
	if (!Q_stricmp(query->source, "scoreboard")) {
		return uid_query_collection_scoreboard(
			query->offset, query->limit, query->outTotal, query->outRevision, out, max
		);
	}
	if (!Q_stricmp(query->source, "hud-packs")) {
		return uid_query_collection_hud_packs(
			query->offset, query->limit, query->outTotal, query->outRevision, out, max
		);
	}
	if (!Q_stricmp(query->source, "hud-objectives")) {
		return uid_query_collection_objectives(
			query->offset, query->limit, query->outTotal, query->outRevision, out, max
		);
	}
	if (!Q_stricmp(query->source, "hud-messages")) {
		return uid_query_collection_hud_messages(
			query->offset, query->limit, query->outTotal, query->outRevision, out, max, 0
		);
	}
	if (!Q_stricmp(query->source, "hud-game-messages")) {
		return uid_query_collection_hud_messages(
			query->offset, query->limit, query->outTotal, query->outRevision, out, max, 1
		);
	}
	/* Added in OPM: chat-only and structured kill-feed collections. */
	if (!Q_stricmp(query->source, "hud-chat")) {
		return uid_query_collection_hud_messages(
			query->offset, query->limit, query->outTotal, query->outRevision, out, max, 2
		);
	}
	if (!Q_stricmp(query->source, "hud-kill-feed")) {
		return uid_query_collection_hud_kill_feed(
			query->offset, query->limit, query->outTotal, query->outRevision, out, max
		);
	}
	return uid_query_collection_options(
		query->source,
		query->offset,
		query->limit,
		query->outTotal,
		query->outRevision,
		out,
		max
	);
}

static int uid_query_options(const char *source, char **values, char **labels, int max)
{
	static char valueBuf[64][96];
	static char labelBuf[64][96];
	static const char *const videoModes[] = {
		"-2", "Desktop", "-1", "Custom", "6", "1024 x 768", "7", "1152 x 864", "8", "1280 x 1024",
		"9", "1600 x 1200", "10", "2048 x 1536",
	};
	static const char *const displayMode[] = {"1", "Fullscreen", "2", "Borderless", "0", "Windowed"};
	static const char *const maxFps[] = {"125", "125", "250", "250", "500", "500"};
	static const char *const colorDepth[] = {"16", "16-bit", "32", "32-bit"};
	static const char *const speakerSetup[] = {
		"0", "Two Speakers", "1", "Headphones", "2", "Surround (5.1)", "3", "Quad Speakers",
	};
	static const char *const sampleRate[] = {"11", "11 kHz", "22", "22 kHz", "44", "44 kHz"};
	static const char *const milesDriver[] = {
		"auto", "Auto", "DirectSound3D Hardware Support", "DirectSound3D",
		"Creative Labs EAX 2 (TM)", "Creative EAX 2", "Creative Labs EAX (TM)", "Creative EAX",
		"Aureal A3D 2.0 (TM)", "Aureal A3D 2.0", "Dolby Surround", "Dolby Surround",
		"Miles Fast 2D Positional Audio", "2D Positional",
	};
	static const char *const picmip[] = {
		"0", "Highest (0)", "1", "High (1)", "2", "Medium (2)", "3", "Low (3)",
	};
	static const char *const textureFilter[] = {
		"GL_NEAREST", "Nearest", "GL_LINEAR", "Linear", "GL_NEAREST_MIPMAP_NEAREST", "Nearest mipmap",
		"GL_LINEAR_MIPMAP_NEAREST", "Bilinear", "GL_LINEAR_MIPMAP_LINEAR", "Trilinear",
	};
	static const char *const shadows[] = {"0", "Off", "1", "Mode 1", "2", "Mode 2", "3", "Mode 3"};
	static const char *const effectDetail[] = {"0.2", "Low", "0.6", "Medium", "1.0", "High"};
	static const char *const lodScale[] = {"0.5", "Performance", "1.1", "Balanced", "1.5", "High Detail"};
	static const char *const drawViewmodel[] = {"0", "Off", "1", "Weapon only", "2", "Weapon and hands"};
	static const char *const weaponsBar[] = {"0", "Off", "1", "On", "2", "Fade"};
	static const char *const alliesModels[] = {
		"allied_manon", "Manon", "allied_airborne", "Airborne", "allied_pilot", "Pilot", "allied_sas", "SAS",
		"american_army", "American Army", "american_ranger", "American Ranger",
	};
	static const char *const axisModels[] = {
		"german_afrika_officer", "Afrika Officer", "german_afrika_private", "Afrika Private",
		"german_elite_officer", "Elite Officer", "german_wehrmacht_officer", "Wehrmacht Officer",
		"german_wehrmacht_soldier", "Wehrmacht Soldier", "german_waffen_officer", "Waffen Officer",
	};
	const int bufCount = (int)(sizeof(valueBuf) / sizeof(valueBuf[0]));

	if (!source || !values || !labels || max <= 0) {
		return 0;
	}

	/* Added in Omaha: SDL-enumerated refresh rates for cyclic Refresh Rate. */
	if (!Q_stricmp(source, "display-refresh")) {
		return uid_fill_display_refresh(values, labels, max, valueBuf, labelBuf, bufCount);
	}

#define UID_OPT_SRC(name, arr)                                                                                         \
	do {                                                                                                               \
		if (!Q_stricmp(source, name)) {                                                                                \
			return uid_fill_option_pairs(                                                                              \
				arr, (int)(sizeof(arr) / sizeof(arr[0])), values, labels, max, valueBuf, labelBuf, bufCount             \
			);                                                                                                         \
		}                                                                                                              \
	} while (0)

	UID_OPT_SRC("video-modes", videoModes);
	UID_OPT_SRC("display-mode", displayMode);
	UID_OPT_SRC("max-fps", maxFps);
	UID_OPT_SRC("color-depth", colorDepth);
	UID_OPT_SRC("texture-depth", colorDepth);
	UID_OPT_SRC("speaker-setup", speakerSetup);
	UID_OPT_SRC("sample-rate", sampleRate);
	UID_OPT_SRC("miles-driver", milesDriver);
	UID_OPT_SRC("picmip", picmip);
	UID_OPT_SRC("texture-filter", textureFilter);
	UID_OPT_SRC("shadows", shadows);
	UID_OPT_SRC("effect-detail", effectDetail);
	UID_OPT_SRC("lod-scale", lodScale);
	UID_OPT_SRC("draw-viewmodel", drawViewmodel);
	UID_OPT_SRC("weapons-bar", weaponsBar);
	UID_OPT_SRC("player-models-allies", alliesModels);
	UID_OPT_SRC("player-models-axis", axisModels);
#undef UID_OPT_SRC

	return 0;
}

static void uid_write_all_doc_bindings(void)
{
	uid_runtime_t *runtime = CL_UIR_MainRuntime();
	if (runtime && UID_HasDocument(runtime)) {
		uid_document_t *doc = const_cast<uid_document_t *>(UID_GetDocument(runtime));
		UID_WriteAllBindings(doc, &g_uidBackend);
	}
}

static uid_node_id_t uir_browser_find_servers_scope(const uid_document_t *doc)
{
	if (!doc) {
		return UID_INVALID_NODE_ID;
	}
	if (doc->idIndex.count("browser_panel.browser_list")) {
		return doc->idIndex.at("browser_panel.browser_list");
	}
	if (doc->idIndex.count("browser_list")) {
		return doc->idIndex.at("browser_list");
	}
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		const uid_node_def_t &node = doc->nodes[i];
		if (!node.collectionSource.empty() && !Q_stricmp(node.collectionSource.c_str(), "servers")) {
			return static_cast<uid_node_id_t>(i);
		}
	}
	return UID_INVALID_NODE_ID;
}

/*
 * Fixed in OPM: JOIN must use the design list's selected row (servers collection scope),
 * not a stale ui_selected_server left at the default first row.
 */
static const char *uir_browser_resolve_join_addr(void)
{
	uid_runtime_t *runtime = CL_UIR_MainRuntime();

	if (runtime && UID_HasDocument(runtime)) {
		const uid_document_t *doc = UID_GetDocument(runtime);
		const uid_node_id_t scopeId = uir_browser_find_servers_scope(doc);
		if (scopeId >= 0 && static_cast<size_t>(scopeId) < doc->states.size()) {
			const uid_node_state_t *st = &doc->states[static_cast<size_t>(scopeId)];
			const int idx = st->collectionSelectedIndex;
			if (idx >= 0 && static_cast<size_t>(idx) < st->collectionItems.size()) {
				const std::string &value = st->collectionItems[static_cast<size_t>(idx)].value;
				if (!value.empty()) {
					Cvar_Set("ui_selected_server", value.c_str());
					uir_browser_sync_selected_from_cvar();
					return Cvar_VariableString("ui_selected_server");
				}
			}
		}
	}

	uir_browser_sync_selected_from_cvar();
	return Cvar_VariableString("ui_selected_server");
}

static bool uid_video_cvars_changed(const char *const *names, char (*before)[MAX_CVAR_VALUE_STRING], int count)
{
	int i;

	for (i = 0; i < count; i++) {
		const char *now = Cvar_VariableString(names[i]);
		if (Q_stricmp(before[i], now ? now : "") != 0) {
			return true;
		}
	}
	return false;
}

static void CL_UIR_FillUidBackend(uid_backend_t *out);

static bool invoke_apply_video(void *userdata)
{
	(void)userdata;
	uid_write_all_doc_bindings();
	Cbuf_AddText("vid_restart\n");
	return true;
}

static bool invoke_quit(void *userdata)
{
	(void)userdata;
	Cbuf_AddText("quit\n");
	return true;
}

static bool invoke_refresh_servers(void *userdata)
{
	(void)userdata;
	uir_browser_refresh();
	return true;
}

static bool invoke_sort_servers(void *userdata)
{
	(void)userdata;
	uir_browser_apply_sort_click(uir_browser_sort_from_column(Cvar_VariableString("ui_om_browser_sort")));
	uir_browser_update_status_cvars();
	return true;
}

static bool invoke_sort_scoreboard(void *userdata)
{
	(void)userdata;
	UIR_Scoreboard_ApplySortColumn(Cvar_VariableString("ui_om_scoreboard_sort"));
	return true;
}

/* Added in Omaha: Misc keybind / console cycle for scoreboard column sort. */
static void CL_UIR_CycleScoreboardSort_f(void)
{
	UIR_Scoreboard_CycleSort();
}

static bool invoke_toggle_server_favorite(void *userdata)
{
	const char *ip;

	(void)userdata;
	ip = Cvar_VariableString("ui_browser_favorite_target");
	if (!ip || !ip[0]) {
		return false;
	}
	UIR_Browser_ToggleFavoriteByIp(ip);
	uir_browser_update_status_cvars();
	return true;
}

static bool invoke_join_selected(void *userdata)
{
	const char               *addr;
	const uir_browser_row_t *row;

	(void)userdata;
	addr = uir_browser_resolve_join_addr();
	if (!addr || !addr[0]) {
		Com_Printf("UIR: join-selected: no ui_selected_server\n");
		return false;
	}
	row = UIR_Browser_GetRow(UIR_Browser_FindRowByIp(addr));
	if (row && row->diffVersion) {
		const char *message;
		float       neededVersion = com_target_shortversion->value;
		float       serverVersion = (float)atof(row->gameVer[0] == 'd' ? row->gameVer + 1 : row->gameVer);

		if (Q_fabs(neededVersion - serverVersion) >= 0.1f) {
			UI_SetReturnMenuToCurrent();
			message = va(
				"Server is version %s, you are targeting %s",
				row->gameVer,
				com_target_shortversion->string
			);
			Cvar_Set("com_errormessage", message);
			UI_PushMenu("wrongversion");
			return true;
		}
		message = va(
			"Can not connect to v%s server, you are targeting v%s",
			row->gameVer,
			com_target_shortversion->string
		);
		Cvar_Set("dm_serverstatus", message);
		return true;
	}
	CL_UIR_DeactivateModernMain();
	CL_ModernBrowser_HaltRefresh();
	UI_SetReturnMenuToCurrent();
	Cbuf_AddText(va("connect %s\n", addr));
	return true;
}

static bool invoke_apply_profile(void *userdata)
{
	const char *playerName;

	(void)userdata;
	uid_write_all_doc_bindings();
	playerName = Cvar_VariableString("name");
	if (playerName && playerName[0]) {
		Cvar_Set("name", playerName);
	}
	Cbuf_AddText("ui_applyplayermodel\n");
	return true;
}

static bool invoke_settings_defaults(void *userdata)
{
	int i;

	(void)userdata;
	for (i = 0; g_uidSettingsDraftCvars[i]; i++) {
		if (Cvar_FindVar(g_uidSettingsDraftCvars[i])) {
			Cvar_Reset(g_uidSettingsDraftCvars[i]);
		}
	}
	/* Fixed in Omaha: drop staged apply values so cyclics / toggles re-sync. */
	{
		uid_runtime_t *runtime = CL_UIR_MainRuntime();
		if (runtime && UID_HasDocument(runtime)) {
			uid_document_t *doc = const_cast<uid_document_t *>(UID_GetDocument(runtime));
			UID_ClearApplyStagedBindings(doc);
		}
	}
	return true;
}

static bool invoke_modal_commit_keybind(void *userdata)
{
	(void)userdata;
	uid_runtime_t *runtime = CL_UIMenu_RuntimeForInput();
	if (!runtime) {
		runtime = CL_UIR_MainRuntime();
	}
	if (!runtime || !UID_HasDocument(runtime)) {
		return false;
	}
	{
		uid_document_t *doc = const_cast<uid_document_t *>(UID_GetDocument(runtime));
		uid_backend_t   be;

		CL_UIR_FillUidBackend(&be);
		return UID_CommitKeybindFromModalCvars(doc, &be) == UID_OK;
	}
}

static bool invoke_settings_apply(void *userdata)
{
	char beforeVideo[32][MAX_CVAR_VALUE_STRING];
	char beforeSound[16][MAX_CVAR_VALUE_STRING];
	int  nVideo = 0;
	int  nSound = 0;
	int  i;
	qboolean needVidRestart;
	qboolean needSndRestart;

	(void)userdata;
	for (i = 0; g_uidVideoRestartCvars[i] && nVideo < (int)(sizeof(beforeVideo) / sizeof(beforeVideo[0])); i++) {
		Q_strncpyz(beforeVideo[nVideo], Cvar_VariableString(g_uidVideoRestartCvars[i]), sizeof(beforeVideo[nVideo]));
		nVideo++;
	}
	for (i = 0; g_uidSoundRestartCvars[i] && nSound < (int)(sizeof(beforeSound) / sizeof(beforeSound[0])); i++) {
		Q_strncpyz(beforeSound[nSound], Cvar_VariableString(g_uidSoundRestartCvars[i]), sizeof(beforeSound[nSound]));
		nSound++;
	}
	uid_write_all_doc_bindings();
	needVidRestart = uid_video_cvars_changed(g_uidVideoRestartCvars, beforeVideo, nVideo) ? qtrue : qfalse;
	needSndRestart = uid_video_cvars_changed(g_uidSoundRestartCvars, beforeSound, nSound) ? qtrue : qfalse;
	if (needVidRestart) {
		Cbuf_AddText("vid_restart\n");
	} else if (needSndRestart) {
		Cbuf_AddText("snd_restart\n");
	}
	return true;
}

static bool invoke_reset_cvar(void *userdata)
{
	const char *cvarName;

	(void)userdata;
	cvarName = Cvar_VariableString("ui_reset_cvar");
	if (!cvarName || !cvarName[0]) {
		return false;
	}
	if (!Cvar_FindVar(cvarName)) {
		return false;
	}
	Cvar_Reset(cvarName);
	return true;
}

static bool invoke_navigate(void *userdata)
{
	const char *target;
	const char *value;

	(void)userdata;
	target = Cvar_VariableString("ui_om_nav_target");
	value = Cvar_VariableString("ui_om_nav_value");
	if (!target || !target[0] || !value || !value[0]) {
		return false;
	}
	if (!Q_stricmp(target, "main_panel")) {
		Cvar_Set("ui_om_main_panel", value);
		return true;
	}
	if (!Q_stricmp(target, "settings_tab")) {
		Cvar_Set("ui_om_settings_tab", value);
		return true;
	}
	return false;
}

static bool invoke_back(void *userdata)
{
	(void)userdata;
	Cvar_Set("ui_om_main_panel", "play");
	return true;
}

static bool invoke_close(void *userdata)
{
	(void)userdata;
	Cvar_Set("ui_om_modal", "");
	return true;
}

static bool invoke_legacy_pushmenu(void *userdata)
{
	const char *target;

	(void)userdata;
	target = Cvar_VariableString("ui_legacy_pushmenu_target");
	if (!target || !target[0]) {
		return false;
	}
	Cbuf_AddText(va("pushmenu %s\n", target));
	return true;
}

static bool invoke_menu_open(void *userdata)
{
	const char *target;

	(void)userdata;
	target = Cvar_VariableString("ui_menu_open_target");
	if (!target || !target[0]) {
		return false;
	}
	return CL_UIMenu_Open(target, qtrue) ? true : false;
}

static bool invoke_cbuf(void *userdata)
{
	const char *cmd;

	(void)userdata;
	cmd = Cvar_VariableString("ui_om_cbuf");
	if (!cmd || !cmd[0]) {
		return false;
	}
	Cbuf_AddText(cmd);
	if (cmd[strlen(cmd) - 1] != '\n') {
		Cbuf_AddText("\n");
	}
	return true;
}

static bool invoke_menu_close(void *userdata)
{
	const char *target;

	(void)userdata;
	target = Cvar_VariableString("ui_menu_close_target");
	if (!target || !target[0]) {
		return false;
	}
	return CL_UIMenu_Close(target) ? true : false;
}

/* Added in OPM: in-HUD chat compose (see CL_UIR_OpenHudChat). */
static qboolean g_hudChatOpen = qfalse;
static int      g_hudChatMode = 100;

static uid_runtime_t *CL_UIR_ActiveHudRuntime(void)
{
	const char *hudId = CL_UIR_ActiveHudId();
	if (!hudId || !hudId[0] || CL_UIMenu_HudIsBuiltinLegacy(hudId)) {
		return NULL;
	}
	return CL_UIMenu_RuntimeById(hudId);
}

static int CL_UIR_NormalizeHudChatMode(int iMode)
{
	if (iMode < 0) {
		iMode = -iMode;
	}
	if (iMode == 300) {
		iMode = (g_hudChatMode > 0) ? g_hudChatMode : 100;
	}
	if (iMode <= 0) {
		iMode = 100;
	}
	return iMode;
}

static const char *CL_UIR_HudChatLabelForMode(int iMode)
{
	if (iMode == 200) {
		return "Team Chat: ";
	}
	return "Chat: ";
}

static void CL_UIR_FocusHudChatInput(void)
{
	uid_runtime_t  *runtime = CL_UIR_ActiveHudRuntime();
	uid_document_t *doc;
	uid_node_id_t   nodeId;

	if (!runtime || !UID_HasDocument(runtime)) {
		return;
	}
	doc = const_cast<uid_document_t *>(UID_GetDocument(runtime));
	if (!doc) {
		return;
	}
	{
		auto it = doc->idIndex.find("hud_chat_input");
		if (it == doc->idIndex.end()) {
			return;
		}
		nodeId = it->second;
	}
	UID_SyncBindings(doc, &g_uidBackend);
	UID_SetFocus(doc, nodeId, &g_uidBackend);
}

qboolean CL_UIR_HudChatHasInput(void)
{
	uid_runtime_t        *runtime;
	const uid_document_t *doc;

	if (!CL_UIR_UseModernHudPack() || clc.state != CA_ACTIVE) {
		return qfalse;
	}
	runtime = CL_UIR_ActiveHudRuntime();
	if (!runtime || !UID_HasDocument(runtime)) {
		return qfalse;
	}
	doc = UID_GetDocument(runtime);
	return (doc && UID_GetNodeById(doc, "hud_chat_input")) ? qtrue : qfalse;
}

qboolean CL_UIR_HudChatIsOpen(void)
{
	return g_hudChatOpen;
}

void CL_UIR_CloseHudChat(void)
{
	uid_runtime_t *runtime;

	if (!g_hudChatOpen) {
		Cvar_Set("ui_om_hud_chat_open", "0");
		return;
	}
	g_hudChatOpen = qfalse;
	Cvar_Set("ui_om_hud_chat_open", "0");
	Cvar_Set("ui_om_hud_chat_text", "");
	/* Deactivate clears focus without re-CommitInput (avoids double submit). */
	runtime = CL_UIR_ActiveHudRuntime();
	if (runtime) {
		UID_Deactivate(runtime);
	}
	if (!CL_UIMenu_HasInteractiveOpen()) {
		Key_SetCatcher(Key_GetCatcher() & ~KEYCATCH_UI);
	}
}

void CL_UIR_OpenHudChat(int iMode)
{
	iMode = CL_UIR_NormalizeHudChatMode(iMode);
	if (!CL_UIR_HudChatHasInput()) {
		return;
	}
	g_hudChatMode = iMode;
	g_hudChatOpen = qtrue;
	Cvar_Set("ui_om_hud_chat_open", "1");
	Cvar_Set("ui_om_hud_chat_label", CL_UIR_HudChatLabelForMode(iMode));
	Cvar_Set("ui_om_hud_chat_text", "");
	Cvar_SetValue("ui_om_hud_chat_mode", (float)iMode);
	Key_SetCatcher(Key_GetCatcher() | KEYCATCH_UI);
	IN_MouseOff();
	CL_UIR_FocusHudChatInput();
}

void CL_UIR_ToggleHudChat(int iMode)
{
	const int normalized = CL_UIR_NormalizeHudChatMode(iMode);

	if (!CL_UIR_HudChatHasInput()) {
		return;
	}
	if (g_hudChatOpen && g_hudChatMode == normalized) {
		CL_UIR_CloseHudChat();
		return;
	}
	CL_UIR_OpenHudChat(iMode);
}

static bool invoke_hud_chat_submit(void *userdata)
{
	const char *txt;
	int         iMode;
	char        szStringOut[1024];

	(void)userdata;
	if (!g_hudChatOpen) {
		return false;
	}
	txt = Cvar_VariableString("ui_om_hud_chat_text");
	if (txt && txt[0]) {
		/* Match DMConsoleCommandHandler: all→0, team→-1, else client index. */
		iMode = 0;
		if (g_hudChatMode != 100) {
			if (g_hudChatMode == 200) {
				iMode = -1;
			} else {
				iMode = g_hudChatMode;
			}
		}
		Com_sprintf(szStringOut, sizeof(szStringOut), "dmmessage %i %s\n", iMode, txt);
		CL_AddReliableCommand(szStringOut, qfalse);
	}
	CL_UIR_CloseHudChat();
	return true;
}

static bool invoke_hud_chat_cancel(void *userdata)
{
	(void)userdata;
	CL_UIR_CloseHudChat();
	return true;
}

static void CL_UIR_RegisterInvokes(void)
{
	UID_ClearInvokes();
	UID_RegisterInvoke("apply-video", invoke_apply_video, NULL);
	UID_RegisterInvoke("restart-video", invoke_apply_video, NULL);
	UID_RegisterInvoke("quit", invoke_quit, NULL);
	UID_RegisterInvoke("refresh-servers", invoke_refresh_servers, NULL);
	UID_RegisterInvoke("sort-servers", invoke_sort_servers, NULL);
	UID_RegisterInvoke("sort-scoreboard", invoke_sort_scoreboard, NULL);
	UID_RegisterInvoke("toggle-server-favorite", invoke_toggle_server_favorite, NULL);
	UID_RegisterInvoke("join-selected", invoke_join_selected, NULL);
	UID_RegisterInvoke("apply-profile", invoke_apply_profile, NULL);
	UID_RegisterInvoke("settings-defaults", invoke_settings_defaults, NULL);
	UID_RegisterInvoke("modal-commit-keybind", invoke_modal_commit_keybind, NULL);
	UID_RegisterInvoke("settings-apply", invoke_settings_apply, NULL);
	UID_RegisterInvoke("reset-cvar", invoke_reset_cvar, NULL);
	UID_RegisterInvoke("navigate", invoke_navigate, NULL);
	UID_RegisterInvoke("back", invoke_back, NULL);
	UID_RegisterInvoke("close", invoke_close, NULL);
	UID_RegisterInvoke("legacy-pushmenu", invoke_legacy_pushmenu, NULL);
	UID_RegisterInvoke("menu-open", invoke_menu_open, NULL);
	UID_RegisterInvoke("menu-close", invoke_menu_close, NULL);
	/* Added in OPM: modern pause console bridge (XML set-cvar ui_om_cbuf + invoke). */
	UID_RegisterInvoke("cbuf", invoke_cbuf, NULL);
	/* Added in OPM: in-HUD chat compose submit / Escape cancel. */
	UID_RegisterInvoke("hud-chat-submit", invoke_hud_chat_submit, NULL);
	UID_RegisterInvoke("hud-chat-cancel", invoke_hud_chat_cancel, NULL);
}

static bool uid_invoke_action(const char *name, void *userdata)
{
	(void)userdata;
	if (!name || !name[0]) {
		return false;
	}
	return UID_Invoke(name);
}

/*
============
CL_UIR_ForceConsolesClosed

Hide the main (fakk) console and the developer console. Capture harness
holds them shut across the screenshot frame.
============
*/
static void CL_UIR_ForceConsolesClosed(void)
{
	Key_SetCatcher(Key_GetCatcher() & ~KEYCATCH_CONSOLE);
	UI_CloseConsole();
	UI_CloseDeveloperConsole();
	/* Keep forcing closed for a short window so prints / focus cannot reopen mid-shot. */
	g_compareKeepConsolesClosedUntil = cls.realtime + 2000;
}

/* Removed in OPM: uid_set_node_visible / uid_set_node_prop (compare path uses cvars). */

/*
============
CL_UIR_CompareGoto_f

ui_compare_goto <play|settings> [input|video|audio|gameplay]

Toggles panel_play / panel_settings and optional settings_page_* visibility
for the screenshot-compare harness.
============
*/
static void CL_UIR_CompareGoto_f(void)
{
	const char     *panel;
	const char     *page;
	static const char *const settingsPages[] = {
		"input", "video", "audio", "gameplay", NULL
	};
	int i;
	qboolean wantPlay;

	if (Cmd_Argc() < 2) {
		Com_Printf("usage: ui_compare_goto <play|settings> [input|video|audio|gameplay]\n");
		return;
	}
	if (!CL_UIR_MainRuntime() || !UID_HasDocument(CL_UIR_MainRuntime())) {
		Com_Printf("UIR: ui_compare_goto: no design document loaded\n");
		return;
	}

	CL_UIR_ForceConsolesClosed();

	panel = Cmd_Argv(1);
	page = Cmd_Argc() >= 3 ? Cmd_Argv(2) : NULL;

	if (!Q_stricmp(panel, "play")) {
		wantPlay = qtrue;
	} else if (!Q_stricmp(panel, "settings")) {
		wantPlay = qfalse;
	} else {
		Com_Printf("usage: ui_compare_goto <play|settings> [input|video|audio|gameplay]\n");
		return;
	}

	Cvar_Set("ui_om_main_panel", wantPlay ? "play" : "settings");

	if (!wantPlay) {
		const char *active = page && page[0] ? page : "input";
		qboolean    valid = qfalse;

		for (i = 0; settingsPages[i]; i++) {
			if (Q_stricmp(settingsPages[i], active) == 0) {
				valid = qtrue;
				break;
			}
		}
		if (!valid) {
			active = "input";
		}
		Cvar_Set("ui_om_settings_tab", active);
	}

	/* Force a layout pass before shot so geometry matches visibility. */
	CL_UIR_UpdateModern();

	Com_DPrintf("UIR: ui_compare_goto %s%s%s\n", panel, page ? " " : "", page ? page : "");
}

/*
============
CL_UIR_BakeModel_f / CL_UIR_BakeMpWeapons_f — transparent PNG model bake
============
*/
#define UIR_BAKE_MP_WIDTH       500
#define UIR_BAKE_MP_HEIGHT      100
#define UIR_BAKE_GRENADE_SIZE   100
#define UIR_BAKE_PROBE_SIZE     500
#define UIR_BAKE_PROBE_SCALE    0.50f
/* Converged shared scales (updated by bake tune loop). */
#define UIR_BAKE_GUN_SCALE      2.20f
#define UIR_BAKE_GRENADE_SCALE  2.88f

typedef enum {
	UIR_BAKE_STAGE_FINAL = 0,
	UIR_BAKE_STAGE_CENTER,
	UIR_BAKE_STAGE_SCALE,
	UIR_BAKE_STAGE_ROTATE
} uir_bake_stage_t;

static uir_bake_stage_t uir_bake_stage_from_string(const char *s)
{
	if (!s || !s[0]) {
		return UIR_BAKE_STAGE_FINAL;
	}
	if (!Q_stricmp(s, "center")) {
		return UIR_BAKE_STAGE_CENTER;
	}
	if (!Q_stricmp(s, "scale")) {
		return UIR_BAKE_STAGE_SCALE;
	}
	if (!Q_stricmp(s, "rotate")) {
		return UIR_BAKE_STAGE_ROTATE;
	}
	if (!Q_stricmp(s, "final")) {
		return UIR_BAKE_STAGE_FINAL;
	}
	return UIR_BAKE_STAGE_FINAL;
}

static void uir_bake_lowercase_base(const char *modelPath, char *out, int outSize)
{
	char        base[MAX_QPATH];
	char        stripped[MAX_QPATH];
	const char *slash;
	int         i;

	slash = modelPath;
	while (slash[0] && slash[1]) {
		const char *next = strchr(slash, '/');
		if (!next) {
			break;
		}
		slash = next + 1;
	}
	Q_strncpyz(base, slash, sizeof(base));
	COM_StripExtension(base, stripped, sizeof(stripped));
	for (i = 0; stripped[i]; i++) {
		if (stripped[i] >= 'A' && stripped[i] <= 'Z') {
			stripped[i] = (char)(stripped[i] - 'A' + 'a');
		}
	}
	Com_sprintf(out, outSize, "%s.png", stripped);
}

static qboolean CL_UIR_RunModelBake(
	const char *modelPath,
	const char *outPath,
	float pitch,
	float yaw,
	float roll,
	float scale,
	float fov,
	float offX,
	float offY,
	float offZ,
	int width,
	int height,
	float sharedExtent
)
{
	uir_model_bake_params_t params;
	qhandle_t               handle;
	uir_status_t            st;

	CL_UIR_WireBackends();

	if (!re.ExportModelPreviewPNG) {
		Com_Printf(
			"ui_bake_model: ExportModelPreviewPNG unavailable (launch with +set cl_renderer opengl2)\n"
		);
		return qfalse;
	}

	handle = re.RegisterModel(modelPath);
	if (!handle) {
		Com_Printf("ui_bake_model: RegisterModel failed for '%s'\n", modelPath);
		return qfalse;
	}

	if (re.BeginFrame) {
		re.BeginFrame(STEREO_CENTER);
	}

	memset(&params, 0, sizeof(params));
	params.modelHandle = handle;
	params.angles[0] = pitch;
	params.angles[1] = yaw;
	params.angles[2] = roll;
	params.offset[0] = offX;
	params.offset[1] = offY;
	params.offset[2] = offZ;
	/* Match cl_uistd inventory HUD: scale drives camera distance, entity scale stays 1. */
	params.modelScale = 1.0f;
	params.framingScale = scale > 0.0f ? scale : 1.0f;
	params.sharedExtent = sharedExtent;
	params.fov = fov > 0.0f ? fov : UIR_MP_FOV_DEFAULT;
	params.width = width;
	params.height = height;
	params.outPath = outPath;

	st = UIR_ModelBakeToPNG(&params);
	if (re.EndFrame) {
		int fe = 0, be = 0;
		re.EndFrame(&fe, &be);
	}
	if (st != UIR_OK) {
		Com_Printf("ui_bake_model: bake failed for '%s' (status=%d)\n", modelPath, (int)st);
		return qfalse;
	}

	Com_Printf("Wrote %s (%dx%d)\n", outPath, width, height);
	return qtrue;
}

static void CL_UIR_BakeModel_f(void)
{
	float sharedExtent = 0.0f;

	if (Cmd_Argc() < 13) {
		Com_Printf(
			"usage: ui_bake_model <model> <out.png> <yaw> <pitch> <roll> <scale> <fov> "
			"<offX> <offY> <offZ> <width> <height> [sharedExtent]\n"
		);
		return;
	}
	if (Cmd_Argc() >= 14) {
		sharedExtent = atof(Cmd_Argv(13));
	}

	CL_UIR_RunModelBake(
		Cmd_Argv(1),
		Cmd_Argv(2),
		atof(Cmd_Argv(4)),
		atof(Cmd_Argv(3)),
		atof(Cmd_Argv(5)),
		atof(Cmd_Argv(6)),
		atof(Cmd_Argv(7)),
		atof(Cmd_Argv(8)),
		atof(Cmd_Argv(9)),
		atof(Cmd_Argv(10)),
		atoi(Cmd_Argv(11)),
		atoi(Cmd_Argv(12)),
		sharedExtent
	);
}

static float CL_UIR_BakeGroupExtent(uir_bake_kind_t kind)
{
	int                            i;
	float                          maxExtent = 0.0f;
	const uir_weapon_bake_entry_t *entry;
	qhandle_t                      handle;
	float                          extent;

	CL_UIR_WireBackends();
	for (i = 0; i < UIR_WeaponBakeEntryCount(); i++) {
		entry = UIR_WeaponBakeEntry(i);
		if (!entry || entry->kind != kind) {
			continue;
		}
		handle = re.RegisterModel(entry->modelPath);
		if (!handle) {
			continue;
		}
		if (entry->excludeSharedPool) {
			continue;
		}
		if (entry->hasAngles) {
			extent = UIR_ModelBakeExtent(handle, 1.0f, entry->angles);
		} else {
			extent = UIR_ModelBakeExtent(handle, 1.0f, NULL);
		}
		if (extent > maxExtent) {
			maxExtent = extent;
		}
	}
	return maxExtent;
}

static void CL_UIR_BakeKindGroup(
	uir_bake_kind_t kind,
	const char *outDir,
	int width,
	int height,
	float groupScale,
	float sharedExtent,
	uir_bake_stage_t stage,
	int *written,
	int *skipped
)
{
	int                            i;
	char                           outPath[MAX_OSPATH];
	char                           outName[MAX_QPATH];
	const uir_weapon_bake_entry_t *entry;
	float                          pitch;
	float                          yaw;
	float                          roll;
	const float                    fov = UIR_MP_FOV_DEFAULT;
	float                          offX;
	float                          offY;
	float                          offZ;
	float                          scale;

	for (i = 0; i < UIR_WeaponBakeEntryCount(); i++) {
		entry = UIR_WeaponBakeEntry(i);
		if (!entry || entry->kind != kind) {
			continue;
		}
		uir_bake_lowercase_base(entry->modelPath, outName, sizeof(outName));
		if (stage == UIR_BAKE_STAGE_CENTER || stage == UIR_BAKE_STAGE_SCALE) {
			pitch = 0.0f;
			yaw = 0.0f;
			roll = 0.0f;
		} else if (entry->hasAngles) {
			pitch = entry->angles[0];
			yaw = entry->angles[1];
			roll = entry->angles[2];
		} else {
			pitch = 0.0f;
			yaw = 0.0f;
			roll = 0.0f;
		}
		if (entry->hasOffset) {
			offX = entry->offset[0];
			offY = entry->offset[1];
			offZ = entry->offset[2];
		} else {
			offX = 0.0f;
			offY = 0.0f;
			offZ = 0.0f;
		}
		if (stage == UIR_BAKE_STAGE_CENTER) {
			scale = UIR_BAKE_PROBE_SCALE;
		} else if (entry->framingScale > 0.0f) {
			scale = entry->framingScale;
		} else if (stage == UIR_BAKE_STAGE_SCALE) {
			scale = UIR_BAKE_PROBE_SCALE;
		} else {
			scale = groupScale;
		}
		Com_sprintf(outPath, sizeof(outPath), "%s/%s", outDir, outName);
		if (CL_UIR_RunModelBake(
			    entry->modelPath,
			    outPath,
			    pitch,
			    yaw,
			    roll,
			    scale,
			    fov,
			    offX,
			    offY,
			    offZ,
			    width,
			    height,
			    (stage == UIR_BAKE_STAGE_FINAL || stage == UIR_BAKE_STAGE_ROTATE) ? sharedExtent : 0.0f)) {
			(*written)++;
		} else {
			(*skipped)++;
		}
	}
}

static void CL_UIR_BakeAllGroups(
	const char *outDir,
	float gunScale,
	float nadeScale,
	uir_bake_stage_t stage,
	int *written,
	int *skipped
)
{
	float gunExtent = 0.0f;
	float nadeExtent = 0.0f;
	int   gunW = UIR_BAKE_MP_WIDTH;
	int   gunH = UIR_BAKE_MP_HEIGHT;
	int   nadeW = UIR_BAKE_GRENADE_SIZE;
	int   nadeH = UIR_BAKE_GRENADE_SIZE;

	if (stage != UIR_BAKE_STAGE_FINAL) {
		gunW = nadeW = gunH = nadeH = UIR_BAKE_PROBE_SIZE;
	} else {
		gunExtent = CL_UIR_BakeGroupExtent(UIR_BAKE_GUN);
		nadeExtent = CL_UIR_BakeGroupExtent(UIR_BAKE_GRENADE);
		Com_Printf(
			"ui_bake_mp_weapons: gunExtent=%.2f nadeExtent=%.2f gunScale=%.3f nadeScale=%.3f\n",
			gunExtent,
			nadeExtent,
			gunScale,
			nadeScale
		);
	}

	CL_UIR_BakeKindGroup(UIR_BAKE_GUN, outDir, gunW, gunH, gunScale, gunExtent, stage, written, skipped);
	CL_UIR_BakeKindGroup(
		UIR_BAKE_GRENADE, outDir, nadeW, nadeH, nadeScale, nadeExtent, stage, written, skipped
	);
	CL_UIR_BakeKindGroup(UIR_BAKE_OTHER, outDir, gunW, gunH, gunScale, 0.0f, stage, written, skipped);
}

static void CL_UIR_BakeMpWeapons_f(void)
{
	const char *outDir;
	int         written = 0;
	int         skipped = 0;
	float       gunScale = UIR_BAKE_GUN_SCALE;
	float       nadeScale = UIR_BAKE_GRENADE_SCALE;

	outDir = (Cmd_Argc() >= 2) ? Cmd_Argv(1) : "ui/modern/textures/weapons";
	if (Cmd_Argc() >= 3) {
		gunScale = atof(Cmd_Argv(2));
		if (!(gunScale > 0.0f)) {
			gunScale = UIR_BAKE_GUN_SCALE;
		}
	}
	if (Cmd_Argc() >= 4) {
		nadeScale = atof(Cmd_Argv(3));
		if (!(nadeScale > 0.0f)) {
			nadeScale = UIR_BAKE_GRENADE_SCALE;
		}
	}

	CL_UIR_BakeAllGroups(outDir, gunScale, nadeScale, UIR_BAKE_STAGE_FINAL, &written, &skipped);
	Com_Printf("ui_bake_mp_weapons: %d written, %d skipped (dir=%s)\n", written, skipped, outDir);
}

static void CL_UIR_BakeMpWeaponsProbe_f(void)
{
	const char       *stageStr;
	const char       *outDir;
	uir_bake_stage_t  stage;
	int               written = 0;
	int               skipped = 0;
	float             gunScale = UIR_BAKE_GUN_SCALE;
	float             nadeScale = UIR_BAKE_GRENADE_SCALE;

	if (Cmd_Argc() < 2) {
		Com_Printf(
			"usage: ui_bake_mp_weapons_probe <center|scale|rotate|final> [outDir] [gunScale] [nadeScale]\n"
			"  center/scale/rotate: 500x500 scratch bake; final: production 500x100 / 100x100\n"
		);
		return;
	}
	stageStr = Cmd_Argv(1);
	stage = uir_bake_stage_from_string(stageStr);
	outDir = (Cmd_Argc() >= 3) ? Cmd_Argv(2) : "ui/modern/textures/weapons/_probe";
	if (Cmd_Argc() >= 4) {
		gunScale = atof(Cmd_Argv(3));
		if (!(gunScale > 0.0f)) {
			gunScale = UIR_BAKE_GUN_SCALE;
		}
	}
	if (Cmd_Argc() >= 5) {
		nadeScale = atof(Cmd_Argv(4));
		if (!(nadeScale > 0.0f)) {
			nadeScale = UIR_BAKE_GRENADE_SCALE;
		}
	}

	Com_Printf("ui_bake_mp_weapons_probe: stage=%s dir=%s\n", stageStr, outDir);
	CL_UIR_BakeAllGroups(outDir, gunScale, nadeScale, stage, &written, &skipped);
	Com_Printf("ui_bake_mp_weapons_probe: %d written, %d skipped\n", written, skipped);
}

void CL_UIR_RegisterBakeCommands(void)
{
	static qboolean registered = qfalse;

	if (registered) {
		return;
	}
	registered = qtrue;
	Cmd_AddCommand("ui_bake_model", CL_UIR_BakeModel_f);
	Cmd_AddCommand("ui_bake_mp_weapons", CL_UIR_BakeMpWeapons_f);
	Cmd_AddCommand("ui_bake_mp_weapons_probe", CL_UIR_BakeMpWeaponsProbe_f);
}

/*
============
CL_UIR_CompareShot_f

ui_compare_shot <basename>

Closes consoles (fakk + developer), waits briefly so UI redraws, then
writes screenshots/<basename>.jpg.
============
*/
static void CL_UIR_CompareShot_f(void)
{
	const char *name;

	if (Cmd_Argc() < 2) {
		Com_Printf("usage: ui_compare_shot <basename>\n");
		return;
	}
	name = Cmd_Argv(1);
	CL_UIR_ForceConsolesClosed();
	/*
	 * Insert immediately after this command so the shot runs before a trailing
	 * `quit` already buffered by the capture cfg (AddText would append after quit).
	 */
	Cbuf_ExecuteText(EXEC_INSERT, va("wait 500 ; screenshotJPEG %s\n", name));
}

static void *uid_font_resolve(const char *vfsPath, float logicalPx, float fbScale)
{
	return UIR_FontResolve(vfsPath, logicalPx, fbScale);
}

static float uid_font_measure(void *font, const char *text)
{
	return UIR_FontMeasure((const uir_font_t *)font, text, 0.0f);
}

static float uid_font_ascent(void *font)
{
	return UIR_FontAscent((const uir_font_t *)font);
}

static void uid_font_draw(void *font, float x, float y, const char *text, const float *rgba, float tracking)
{
	uir_color_t            color;
	const uir_viewport_t *vp = UIR_CompositorViewport();

	if (!font || !text || !rgba || !vp) {
		return;
	}
	color.r = rgba[0];
	color.g = rgba[1];
	color.b = rgba[2];
	color.a = rgba[3];
	UIR_FontDraw(vp, (uir_font_t *)font, x, y, text, &color, tracking);
}

static void uid_font_draw_skewed(
	void *font,
	float x,
	float y,
	const char *text,
	const float *rgba,
	float skewTan,
	float originY,
	float tracking
)
{
	uir_color_t           color;
	const uir_viewport_t *vp = UIR_CompositorViewport();

	if (!font || !text || !rgba || !vp) {
		return;
	}
	color.r = rgba[0];
	color.g = rgba[1];
	color.b = rgba[2];
	color.a = rgba[3];
	UIR_FontDrawSkewed(vp, (uir_font_t *)font, x, y, text, &color, tracking, skewTan, originY);
}

/* Added in Omaha: paint-time text rotation around a shared pivot. */
static void uid_font_draw_rotated(
	void *font,
	float x,
	float y,
	const char *text,
	const float *rgba,
	float tracking,
	float rotationDeg,
	float pivotX,
	float pivotY
)
{
	uir_color_t           color;
	const uir_viewport_t *vp = UIR_CompositorViewport();

	if (!font || !text || !rgba || !vp) {
		return;
	}
	color.r = rgba[0];
	color.g = rgba[1];
	color.b = rgba[2];
	color.a = rgba[3];
	UIR_FontDrawRotated(vp, (uir_font_t *)font, x, y, text, &color, tracking, rotationDeg, pivotX, pivotY);
}

static void uid_draw_solid_rect(float x, float y, float w, float h, const float *rgba)
{
	uir_color_t color;

	if (!rgba) {
		return;
	}
	color.r = rgba[0];
	color.g = rgba[1];
	color.b = rgba[2];
	color.a = rgba[3];
	UIR_DrawSolidRect(x, y, w, h, &color);
}

static void uid_draw_path(
	const char *svgD,
	float x,
	float y,
	float w,
	float h,
	float viewW,
	float viewH,
	const float *fillRgba,
	const float *strokeRgba,
	float strokeWidthPx,
	float rotationDeg,
	int crisp
)
{
	uir_viewbox_t view;
	uir_rect_t    dest;
	uir_color_t   fillColor;
	uir_color_t   strokeColor;
	const uir_color_t *fillPtr = NULL;
	const uir_color_t *strokePtr = NULL;

	if (!svgD) {
		return;
	}
	view.minX = 0.0f;
	view.minY = 0.0f;
	view.width = (viewW > 0.0f) ? viewW : w;
	view.height = (viewH > 0.0f) ? viewH : h;
	dest.x = x;
	dest.y = y;
	dest.w = w;
	dest.h = h;
	if (fillRgba && fillRgba[3] > 0.0f) {
		fillColor.r = fillRgba[0];
		fillColor.g = fillRgba[1];
		fillColor.b = fillRgba[2];
		fillColor.a = fillRgba[3];
		fillPtr = &fillColor;
	}
	if (strokeRgba && strokeRgba[3] > 0.0f && strokeWidthPx > 0.0f) {
		strokeColor.r = strokeRgba[0];
		strokeColor.g = strokeRgba[1];
		strokeColor.b = strokeRgba[2];
		strokeColor.a = strokeRgba[3];
		strokePtr = &strokeColor;
	}
	UIR_DrawSvgGeometry(svgD, &view, &dest, UIR_FIT_STRETCH, fillPtr, strokePtr, strokeWidthPx, rotationDeg, crisp);
}

static void uid_draw_image(
	const char *vfsPath,
	float x,
	float y,
	float w,
	float h,
	const char *const *clipPathD,
	int clipPathCount,
	float viewW,
	float viewH,
	int fit,
	float rotationDeg,
	float backgroundScale,
	const float *tintRgba
)
{
	uir_color_t tint;
	const uir_color_t *tintPtr = NULL;

	if (!vfsPath) {
		return;
	}
	if (tintRgba) {
		tint.r = tintRgba[0];
		tint.g = tintRgba[1];
		tint.b = tintRgba[2];
		tint.a = tintRgba[3];
		tintPtr = &tint;
	}
	(void)UIR_ImageDrawClipped(
		vfsPath,
		x,
		y,
		w,
		h,
		clipPathD,
		clipPathCount,
		viewW,
		viewH,
		static_cast<uir_image_fit_t>(fit),
		rotationDeg,
		g_lastUiPxScale,
		backgroundScale,
		tintPtr
	);
}

/* Added in OPM: texel size for leaf <image> auto / aspect layout. */
static bool uid_image_measure(const char *vfsPath, float *outW, float *outH)
{
	uir_image_t *image;

	if (!vfsPath || !vfsPath[0] || !outW || !outH) {
		return false;
	}
	image = UIR_ImageResolve(vfsPath);
	if (!image) {
		return false;
	}
	*outW = UIR_ImageWidth(image);
	*outH = UIR_ImageHeight(image);
	return (*outW > 0.0f && *outH > 0.0f) ? true : false;
}

/* Added in OPM: atlas-baked gradient fill brush. */
static void uid_draw_gradient(
	const char *brush,
	float x,
	float y,
	float w,
	float h,
	const char *const *clipPathD,
	int clipPathCount,
	float viewW,
	float viewH,
	float rotationDeg,
	const float *tintRgba
)
{
	uir_color_t tint;
	const uir_color_t *tintPtr = NULL;

	if (!brush) {
		return;
	}
	if (tintRgba) {
		tint.r = tintRgba[0];
		tint.g = tintRgba[1];
		tint.b = tintRgba[2];
		tint.a = tintRgba[3];
		tintPtr = &tint;
	}
	(void)UIR_GradientDrawClipped(
		brush,
		x,
		y,
		w,
		h,
		clipPathD,
		clipPathCount,
		viewW,
		viewH,
		rotationDeg,
		tintPtr
	);
}

static void uid_push_clip(float x, float y, float w, float h)
{
	UIR_PushClipRect(x, y, w, h);
}

static void uid_pop_clip(void)
{
	UIR_PopClipRect();
}

/* Added in OPM: stencil (or AABB fallback) clip for shaped-container children. */
static bool uid_begin_shape_clip(
	float x,
	float y,
	float w,
	float h,
	const char *const *pathD,
	int pathCount,
	float viewW,
	float viewH,
	float rotationDeg
)
{
	return UIR_BeginSvgShapeClip(x, y, w, h, pathD, pathCount, viewW, viewH, rotationDeg) == UIR_OK;
}

static void uid_end_shape_clip(void)
{
	UIR_EndShapeClip();
}

/* Added in OPM: soft mask coverage for container + subtree (image path or gradient brush). */
static bool uid_begin_image_mask(float x, float y, float w, float h, const char *vfsPathOrBrush, int fit)
{
	return UIR_BeginImageMask(x, y, w, h, vfsPathOrBrush, static_cast<uir_image_fit_t>(fit)) == UIR_OK;
}

static void uid_end_image_mask(void)
{
	UIR_EndImageMask();
}

static void uid_diag(int severity, const char *path, int line, const char *msg, void *userdata)
{
	const char *prefix;

	(void)userdata;
	switch (severity) {
	case UID_SEVERITY_ERROR:
		prefix = "UID ERROR";
		break;
	case UID_SEVERITY_WARNING:
		prefix = "UID WARN";
		break;
	default:
		prefix = "UID";
		break;
	}
	if (path && path[0] && line > 0) {
		Com_Printf("%s: %s:%d: %s\n", prefix, path, line, msg ? msg : "");
	} else if (path && path[0]) {
		Com_Printf("%s: %s: %s\n", prefix, path, msg ? msg : "");
	} else {
		Com_Printf("%s: %s\n", prefix, msg ? msg : "");
	}
}

static void CL_UIR_FillUidBackend(uid_backend_t *out)
{
	memset(out, 0, sizeof(*out));
	out->alloc = uid_alloc;
	out->free = uid_free;
	out->readFile = uid_read_file;
	out->freeFile = uid_free_file;
	out->cvarDescribe = uid_cvar_describe;
	out->cvarWrite = uid_cvar_write;
	out->cvarReset = uid_cvar_reset;
	out->cvarEpoch = uid_cvar_epoch;
	out->keyNameToNum = uid_key_name_to_num;
	out->keyNumToName = uid_key_num_to_name;
	out->getBinding = uid_get_binding;
	out->setBinding = uid_set_binding;
	out->findConflicts = uid_find_conflicts;
	out->getKeysForCommand = uid_get_keys_for_command;
	out->queryOptions = uid_query_options;
	out->queryCollectionItems = uid_query_collection_items;
	out->invokeAction = uid_invoke_action;
	out->fontResolve = uid_font_resolve;
	out->fontMeasure = uid_font_measure;
	out->fontAscent = uid_font_ascent;
	out->fontDraw = uid_font_draw;
	out->fontDrawSkewed = uid_font_draw_skewed;
	out->fontDrawRotated = uid_font_draw_rotated;
	out->drawSolidRect = uid_draw_solid_rect;
	out->drawPath = uid_draw_path;
	out->drawImage = uid_draw_image;
	out->imageMeasure = uid_image_measure;
	out->drawGradient = uid_draw_gradient;
	out->pushClip = uid_push_clip;
	out->popClip = uid_pop_clip;
	out->beginShapeClip = uid_begin_shape_clip;
	out->endShapeClip = uid_end_shape_clip;
	out->beginImageMask = uid_begin_image_mask;
	out->endImageMask = uid_end_image_mask;
	out->queueModelPreview = uid_queue_model_preview;
	out->drawHostRegion = uid_draw_host_region;
	out->hostRegionPointer = uid_host_region_pointer;
	out->getHiResScale = uid_get_hi_res_scale;
	out->getFramebufferSize = uid_get_framebuffer_size;
	out->diag = uid_diag;
	out->userdata = NULL;
}

static qboolean CL_UIR_IsHiDpiSurface(int lw, int lh, int fw, int fh)
{
	const float sx = static_cast<float>(fw) / static_cast<float>(lw);
	const float sy = static_cast<float>(fh) / static_cast<float>(lh);

	if (!(lw > 0 && lh > 0 && fw > lw + 1 && fh > lh + 1)) {
		return qfalse;
	}
	if (sx < 1.4f || sx > 2.6f || sy < 1.4f || sy > 2.6f) {
		return qfalse;
	}
	return fabsf(sx - sy) <= 0.15f ? qtrue : qfalse;
}

static void CL_UIR_GetSurfaceSizes(int *logicalW, int *logicalH, int *fbW, int *fbH)
{
	int lw = 0;
	int lh = 0;
	int fw = 0;
	int fh = 0;

	IN_GetWindowLogicalSize(&lw, &lh);
	IN_GetWindowFramebufferSize(&fw, &fh);
	if (fw <= 0) {
		fw = cls.glconfig.vidWidth;
	}
	if (fh <= 0) {
		fh = cls.glconfig.vidHeight;
	}
	if (lw <= 0) {
		lw = fw;
	}
	if (lh <= 0) {
		lh = fh;
	}

	/*
	 * Fixed in OPM: after vid_restart shrinks the window, SDL_GetWindowSize can
	 * still report the pre-restart logical size — layout stays too wide.
	 */
	if (fw > 0 && fh > 0 && (lw > fw || lh > fh)) {
		lw = fw;
		lh = fh;
	}
	/*
	 * Fixed in OPM: cls.glconfig can lag the recreated SDL window after
	 * r_mode/vid_restart. A mismatched fw/lw ratio maps chrome off-screen and
	 * produces black or striped frames. HiDPI (≈2× uniform scale) is preserved.
	 */
	if (lw > 0 && lh > 0 && fw > 0 && fh > 0 && (fw != lw || fh != lh)) {
		if (!CL_UIR_IsHiDpiSurface(lw, lh, fw, fh)) {
			fw = lw;
			fh = lh;
		}
	}

	if (logicalW) {
		*logicalW = lw;
	}
	if (logicalH) {
		*logicalH = lh;
	}
	if (fbW) {
		*fbW = fw;
	}
	if (fbH) {
		*fbH = fh;
	}
}

/*
 * Fixed in OPM: SDL_GetMouseState is in SDL window client space (same as
 * SDL_GetWindowSize). Layout may be smaller when GetSurfaceSizes clamps a
 * stale/larger window down to the drawable — remap and clamp into layout.
 * Do not scale by framebuffer size; mouse is not in FB pixels.
 */
static void CL_UIR_DebugDumpSurfaceOnce(int layoutW, int layoutH, int fbW, int fbH, float mouseRawX, float mouseRawY)
{
	int rawW = 0;
	int rawH = 0;

	if (!uir_debug || !uir_debug->integer) {
		return;
	}

	IN_GetWindowLogicalSize(&rawW, &rawH);
	if (fbW <= 0 || fbH <= 0) {
		IN_GetWindowFramebufferSize(&fbW, &fbH);
	}
	if (rawW == g_lastPointerRawW && rawH == g_lastPointerRawH &&
		layoutW == g_lastPointerLayoutW && layoutH == g_lastPointerLayoutH) {
		return;
	}

	Com_Printf(
		"UIR surface/pointer: mouse_raw=(%.0f,%.0f) raw=%dx%d layout=%dx%d uid.vid=%dx%d fb=%dx%d glconfig=%dx%d\n",
		mouseRawX,
		mouseRawY,
		rawW,
		rawH,
		layoutW,
		layoutH,
		uid.vidWidth,
		uid.vidHeight,
		fbW,
		fbH,
		cls.glconfig.vidWidth,
		cls.glconfig.vidHeight
	);
	g_lastPointerRawW = rawW;
	g_lastPointerRawH = rawH;
	g_lastPointerLayoutW = layoutW;
	g_lastPointerLayoutH = layoutH;
}

static void CL_UIR_MapMouseToLayout(float *x, float *y, int layoutW, int layoutH)
{
	int rawW = 0;
	int rawH = 0;

	if (!x || !y) {
		return;
	}

	IN_GetWindowLogicalSize(&rawW, &rawH);
	if (rawW > 0 && rawH > 0 && layoutW > 0 && layoutH > 0 &&
		(rawW != layoutW || rawH != layoutH)) {
		*x = *x * (float)layoutW / (float)rawW;
		*y = *y * (float)layoutH / (float)rawH;
	}

	if (*x < 0.0f) {
		*x = 0.0f;
	} else if (layoutW > 0 && *x > (float)layoutW) {
		*x = (float)layoutW;
	}
	if (*y < 0.0f) {
		*y = 0.0f;
	} else if (layoutH > 0 && *y > (float)layoutH) {
		*y = (float)layoutH;
	}
}

/*
 * Added in OPM: one UI coordinate space for modern chrome + legacy winman (console).
 * Legacy launch keeps glconfig; modern uses surface logical size S.
 */
void CL_UIR_GetUiVidSize(int *w, int *h)
{
	int lw = 0;
	int lh = 0;

	if (CL_UIR_UseLegacyMain()) {
		lw = cls.glconfig.vidWidth;
		lh = cls.glconfig.vidHeight;
	} else {
		CL_UIR_GetSurfaceSizes(&lw, &lh, nullptr, nullptr);
		if (lw <= 0) {
			lw = cls.glconfig.vidWidth;
		}
		if (lh <= 0) {
			lh = cls.glconfig.vidHeight;
		}
	}

	if (w) {
		*w = lw;
	}
	if (h) {
		*h = lh;
	}
}

void CL_UIR_MapMouseToUiVid(float *x, float *y)
{
	int lw = 0;
	int lh = 0;

	if (!x || !y) {
		return;
	}
	if (CL_UIR_UseLegacyMain()) {
		return;
	}
	CL_UIR_GetUiVidSize(&lw, &lh);
	CL_UIR_MapMouseToLayout(x, y, lw, lh);
}

static void CL_UIR_ApplySurface(void)
{
	int lw, lh, fw, fh;

	CL_UIR_GetSurfaceSizes(&lw, &lh, &fw, &fh);
	CL_UIMenu_ApplySurface(lw, lh, fw, fh);
	g_lastLogicalW = lw;
	g_lastLogicalH = lh;
	g_lastFbW = fw;
	g_lastFbH = fh;
	CL_UIR_DebugDumpSurfaceOnce(lw, lh, fw, fh, (float)uid.mouseX, (float)uid.mouseY);
}

/*
 * Added in OPM: uiPxScale = UIR_RefPxScale(W,H) * clamp(ui_scale).
 * Reference is 1920x1080 (uniform contain). Pushes when the product changes.
 */
static void CL_UIR_PushUiPxScale(void)
{
	float refScale;
	float userScale;
	float uiPxScale;
	int   lw = 0;
	int   lh = 0;

	if (!ui_scale) {
		ui_scale = Cvar_Get("ui_scale", "1.0", CVAR_ARCHIVE);
	}

	userScale = ui_scale->value;
	if (!(userScale == userScale)) {
		userScale = 1.0f;
	}
	/* Changed in OPM: match settings slider range 0.25–2.0. */
	if (userScale < 0.25f) {
		userScale = 0.25f;
	} else if (userScale > 2.0f) {
		userScale = 2.0f;
	}

	CL_UIR_GetSurfaceSizes(&lw, &lh, nullptr, nullptr);
	refScale = UIR_RefPxScale(lw, lh);

	uiPxScale = refScale * userScale;
	if (uiPxScale < 0.25f) {
		uiPxScale = 0.25f;
	} else if (uiPxScale > 8.0f) {
		uiPxScale = 8.0f;
	}

	if (fabsf(uiPxScale - g_lastUiPxScale) > 0.001f) {
		g_lastUiPxScale = uiPxScale;
		if (uir_debug && uir_debug->integer) {
			Com_Printf(
				"UIR: uiPxScale=%.3f (refScale=%.3f ui_scale=%.3f) fonts=%d/%d\n",
				uiPxScale,
				refScale,
				userScale,
				UIR_FontRegistryCount(),
				UIR_FontRegistryCapacity()
			);
		}
	}
	CL_UIMenu_ApplyUiPxScale(uiPxScale);
}

/*
 * Added in OPM: surface + uiPxScale + UID_Update for draw-order <= 4 menus.
	 * Runs each frame so HUD pack layout stays fresh when paint is gated off.
 */
static qboolean CL_UIR_SyncHudLayerMenus(unsigned int time, int *lw, int *lh, int *fw, int *fh)
{
	int localLw = 0;
	int localLh = 0;
	int localFw = 0;
	int localFh = 0;

	if (clc.state != CA_ACTIVE || CL_UIR_UseLegacyHud() || !CL_UIMenu_HasMenusUpTo(4)) {
		return qfalse;
	}

	CL_UIR_GetSurfaceSizes(&localLw, &localLh, &localFw, &localFh);
	if (localFw <= 0 || localFh <= 0) {
		return qfalse;
	}

	CL_UIMenu_ApplySurface(localLw, localLh, localFw, localFh);
	g_lastLogicalW = localLw;
	g_lastLogicalH = localLh;
	g_lastFbW = localFw;
	g_lastFbH = localFh;
	CL_UIR_PushUiPxScale();
	/*
	 * Changed in OPM: consume MWHEEL while the scoreboard is open even without a
	 * pointer (hold-TAB in play). Overlay UpdateModern also applies wheel; this
	 * covers the normal HUD paint tick.
	 */
	{
		const qboolean applyWheel =
			(CL_UIMenu_IsOpen(CL_UIR_ScoreboardMenuId()) && g_pointerWheelDelta != 0) ? qtrue : qfalse;
		CL_UIR_UpdateHudMenus(time, applyWheel);
	}

	if (lw) {
		*lw = localLw;
	}
	if (lh) {
		*lh = localLh;
	}
	if (fw) {
		*fw = localFw;
	}
	if (fh) {
		*fh = localFh;
	}
	return qtrue;
}

/*
 * Added in OPM: push current surface + ui px scale into all open runtimes.
 * Called by the menu dispatcher when a menu is opened so a newly created
 * runtime never lays out against the 0x0 default from UID_Create.
 */
void CL_UIR_ApplyMenuSurfaceNow(void)
{
	CL_UIR_ApplySurface();
	CL_UIR_PushUiPxScale();
}

/*
===============
CL_UIR_TickUiScaleStress

One slider-like ui_scale step per frame; logs peak font registry pressure.
===============
*/
static void CL_UIR_TickUiScaleStress(void)
{
	int fonts;

	if (g_uiScaleStressLeft <= 0) {
		return;
	}

	Cvar_SetValue("ui_scale", g_uiScaleStressValue);
	fonts = UIR_FontRegistryCount();
	if (fonts > g_uiScaleStressPeakFonts) {
		g_uiScaleStressPeakFonts = fonts;
	}

	g_uiScaleStressValue += (float)g_uiScaleStressDir * 0.05f;
	if (g_uiScaleStressValue >= 2.5f) {
		g_uiScaleStressValue = 2.5f;
		g_uiScaleStressDir = -1;
	} else if (g_uiScaleStressValue <= 0.5f) {
		g_uiScaleStressValue = 0.5f;
		g_uiScaleStressDir = 1;
	}

	g_uiScaleStressLeft--;
	if (g_uiScaleStressLeft > 0) {
		return;
	}

	Cvar_SetValue("ui_scale", 1.0f);
	Com_Printf(
		"UIR: ui_scale stress done peakFonts=%d/%d (approx steps logged above)\n",
		g_uiScaleStressPeakFonts,
		UIR_FontRegistryCapacity()
	);
	if (g_uiScaleStressQuit) {
		Cbuf_AddText("quit\n");
	}
}

static void CL_UIR_StressUiScale_f(void)
{
	int steps = 120;

	if (Cmd_Argc() >= 2) {
		steps = atoi(Cmd_Argv(1));
	}
	if (steps < 1) {
		steps = 1;
	} else if (steps > 2000) {
		steps = 2000;
	}

	g_uiScaleStressLeft = steps;
	g_uiScaleStressValue = 0.5f;
	g_uiScaleStressDir = 1;
	g_uiScaleStressQuit = (Cmd_Argc() >= 3 && !Q_stricmp(Cmd_Argv(2), "quit")) ? qtrue : qfalse;
	g_uiScaleStressPeakFonts = UIR_FontRegistryCount();

	Com_Printf(
		"UIR: ui_scale stress start steps=%d quit=%d fonts=%d/%d\n",
		steps,
		g_uiScaleStressQuit ? 1 : 0,
		g_uiScaleStressPeakFonts,
		UIR_FontRegistryCapacity()
	);
}

static void CL_UIR_ChromeCallback(void *userdata)
{
	(void)userdata;
	CL_UIMenu_PaintChrome();
}

static void CL_UIR_OverlayCallback(void *userdata)
{
	(void)userdata;
	CL_UIMenu_PaintOverlay();
}

/* Added in OPM: modern sniper zoom overlay (uirender radial + solid rects). */
static void cl_uir_paint_sniper_zoom(void)
{
	const uir_viewport_t *vp;
	uir_color_t           black = {0.0f, 0.0f, 0.0f, 1.0f};
	float                 lw;
	float                 lh;
	float                 side;
	float                 xOffset;

	if (!Cvar_VariableIntegerValue("ui_om_hud_sniper_zoom")) {
		return;
	}
	/* Added in OPM: off -> retail PK3 zoom overlays via CG_DrawZoomOverlay. */
	if (!Cvar_VariableIntegerValue("cg_crosshair_sniper_modern")) {
		return;
	}
	if (CL_UIR_UseLegacyHud() || !CL_UIR_UseModernHudPack()) {
		return;
	}

	vp = UIR_CompositorViewport();
	if (!vp) {
		return;
	}

	lw = vp->orthoR - vp->orthoL;
	lh = vp->orthoB - vp->orthoT;
	if (lw < 0.0f) {
		lw = -lw;
	}
	if (lh < 0.0f) {
		lh = -lh;
	}
	if (lw <= 0.0f || lh <= 0.0f) {
		return;
	}

	/* Same geometry as retail CG_DrawOverlayMiddle: square of view height, centered. */
	side = lh;
	xOffset = (lw - side) * 0.5f;
	if (xOffset < 0.0f) {
		xOffset = 0.0f;
		side = lw;
	}

	if (xOffset > 0.0f) {
		UIR_DrawSolidRect(0.0f, 0.0f, xOffset, lh, &black);
		UIR_DrawSolidRect(xOffset + side, 0.0f, lw - xOffset - side, lh, &black);
	}

	(void)UIR_GradientDrawClipped(
		"radial(50% 50%, #00000000 0%, #00000000 88%, #000000FF 96%, #000000FF 100%)",
		xOffset,
		0.0f,
		side,
		side,
		NULL,
		0,
		side,
		side,
		0.0f,
		NULL
	);

	/* Sniper arms from screen edges inward to gap; thickness/gap/T in UI px. */
	cl_uir_paint_sniper_reticle(0.0f, 0.0f, lw, lh, g_lastUiPxScale);
}

static void CL_UIR_HudChromeCallback(void *userdata)
{
	(void)userdata;
	/* Scope under HUD pack chrome (retail zoom-overlay order). */
	cl_uir_paint_sniper_zoom();
	CL_UIMenu_PaintChromeUpTo(4);
}

static void CL_UIR_SniperZoomChromeCallback(void *userdata)
{
	(void)userdata;
	cl_uir_paint_sniper_zoom();
}

static void CL_UIR_HudOverlayCallback(void *userdata)
{
	(void)userdata;
	CL_UIMenu_PaintOverlayUpTo(4);
}

static void CL_UIR_SetDesignLayerCallbacks(qboolean enable)
{
	if (enable) {
		UIR_CompositorSetChromeCallback(CL_UIR_ChromeCallback, NULL);
		UIR_CompositorSetOverlayCallback(CL_UIR_OverlayCallback, NULL);
	} else {
		UIR_CompositorSetChromeCallback(NULL, NULL);
		UIR_CompositorSetOverlayCallback(NULL, NULL);
	}
}

static void CL_UIR_DesignDump_f(void)
{
	const uid_document_t *doc;
	size_t                i;
	uid_runtime_t        *runtime = CL_UIR_MainRuntime();

	if (!runtime || !UID_HasDocument(runtime)) {
		Com_Printf("UIR: no main menu document loaded\n");
		return;
	}
	doc = UID_GetDocument(runtime);
	Com_Printf(
		"UIR: design source='%s' nodes=%d root=%d expanded=%d dirty=0x%x\n",
		doc->sourceName.empty() ? "(none)" : doc->sourceName.c_str(),
		(int)doc->nodes.size(),
		(int)doc->rootNode,
		doc->expanded ? 1 : 0,
		(int)doc->dirty
	);
	for (i = 0; i < doc->nodes.size(); i++) {
		const uid_node_def_t   *node = &doc->nodes[i];
		const uid_node_state_t *st;

		if (node->kind != UID_NODE_MODEL) {
			continue;
		}
		st = (i < doc->states.size()) ? &doc->states[i] : NULL;
		Com_Printf(
			"UIR: model id='%s' team='%s' bind='%s' runtime='%s' box=%.1f,%.1f %.1fx%.1f enabled=%d\n",
			node->id.c_str(),
			node->team.c_str(),
			node->bind.c_str(),
			(st && st->runtimeValue.hasValue) ? st->runtimeValue.stringValue.c_str() : "",
			st ? st->contentBox.x : 0.0f,
			st ? st->contentBox.y : 0.0f,
			st ? st->contentBox.w : 0.0f,
			st ? st->contentBox.h : 0.0f,
			st ? (st->effectivelyEnabled ? 1 : 0) : 0
		);
	}
}

/* ------------------------------------------------------------------------- */
/* Public bridge                                                             */
/* ------------------------------------------------------------------------- */

/* Added in OPM: register gameplay/settings cvars before cgame loads (cg_autoswitch pattern). */
static void CL_UIR_RegisterSettingsCvars(void)
{
	cvar_t *v;

	Cvar_Get("fps", "0", CVAR_ARCHIVE);
	Cvar_Get("cg_fov", "80", CVAR_ARCHIVE);
	Cvar_Get("cg_zoomSensitivity", "screen", CVAR_ARCHIVE); /* Added in OPM: off|legacy|screen */
	Cvar_Get("cg_crosshair_sniper_thickness", "3", CVAR_ARCHIVE);
	Cvar_Get("cg_crosshair_sniper_gap", "0", CVAR_ARCHIVE);
	Cvar_Get("cg_crosshair_sniper_size", "5", CVAR_ARCHIVE);
	Cvar_Get("cg_crosshair_sniper_t", "0", CVAR_ARCHIVE);
	Cvar_Get("cg_crosshair_sniper_modern", "1", CVAR_ARCHIVE);
	Cvar_Get("cg_drawviewmodel", "2", CVAR_ARCHIVE);
	Cvar_Get("cg_hud", "1", CVAR_ARCHIVE);
	Cvar_Get("cg_rain", "1", CVAR_ARCHIVE);
	Cvar_Get("cg_marks_add", "1", CVAR_ARCHIVE);
	Cvar_Get("cg_shadows", "0", CVAR_ARCHIVE);
	Cvar_Get("cg_effectdetail", "1.0", CVAR_ARCHIVE);
	Cvar_Get("vss_draw", "1", CVAR_ARCHIVE);
	Cvar_Get("com_blood", "1", CVAR_ARCHIVE);

	v = Cvar_Get("in_mouse", "1", CVAR_ARCHIVE);
	Cvar_CheckRange(v, -1, 1, qfalse);

	v = Cvar_FindVar("r_lodscale");
	if (v) {
		Cvar_CheckRange(v, 0.5f, 2.0f, qfalse);
	}
}

static void CL_UIR_ApplyDebugRenderPreset(void)
{
	static qboolean s_warnedLatch;

	if (!ui_debug_render || !ui_debug_render->integer) {
		return;
	}

	Cvar_Set("ui_gpu_draw", "1");
	Cvar_Set("ui_render_stats", "1");
	Cvar_Set("uir_debug", "1");
	UIR_DebugSetEnabled(1);
	UIR_BatchSetEnabled(1);

	if (!s_warnedLatch) {
		s_warnedLatch = qtrue;
		Com_Printf(
			"UIR debug preset: ui_gpu_draw=1 ui_render_stats=1 uir_debug=1\n"
		);
	}
}

void CL_UIR_RegisterCvars(void)
{
	CL_UIR_RegisterSettingsCvars();
	ui_legacy = Cvar_Get("ui_legacy", "0", CVAR_INIT);
	ui_om_hud = Cvar_Get("ui_om_hud", "classic", CVAR_ARCHIVE);
	/* Added in OPM: user multiplier on reference-resolution px scale. */
	ui_scale = Cvar_Get("ui_scale", "1.0", CVAR_ARCHIVE);
	uir_debug = Cvar_Get("uir_debug", "0", CVAR_TEMP);
	ui_render_stats = Cvar_Get("ui_render_stats", "0", CVAR_TEMP);
	/* Added in OPM: wall-clock UI pipeline profiler (load + frame phases). */
	ui_profile = Cvar_Get("ui_profile", "0", CVAR_TEMP);
	ui_profile_interval = Cvar_Get("ui_profile_interval", "60", CVAR_TEMP);
	/* Added in OPM: UID_OPT_* bitmask; -1 enables all optimizations. */
	ui_opt = Cvar_Get("ui_opt", "-1", CVAR_ARCHIVE);
	/* Added in OPM: skip redundant flush+scissor when clip unchanged. */
	ui_clip_dedup = Cvar_Get("ui_clip_dedup", "1", CVAR_ARCHIVE);
	/* Added in OPM: tessellated mesh cache for GPU path fills/strokes. */
	ui_mesh_cache = Cvar_Get("ui_mesh_cache", "1", CVAR_ARCHIVE);
	/* Added in OPM: retained chrome RT; default off (enable after measuring idle UI CPU). */
	ui_chrome_cache = Cvar_Get("ui_chrome_cache", "0", CVAR_ARCHIVE);
	/* Added in OPM: batched GPU UI path; default on. */
	ui_gpu_draw = Cvar_Get("ui_gpu_draw", "1", CVAR_ARCHIVE);
	Cvar_CheckRange(ui_gpu_draw, 0, 1, qtrue);
	ui_debug_render = Cvar_Get("ui_debug_render", "0", CVAR_TEMP);
	/* Added in OPM: modern browser host state / status strip bindings */
	Cvar_Get("ui_om_server_search", "", CVAR_TEMP);
	Cvar_Get("ui_om_server_gametype", "", CVAR_TEMP);
	Cvar_Get("ui_om_browser_sort", "players", CVAR_TEMP);
	Cvar_Get("ui_om_browser_sort_asc", "0", CVAR_TEMP);
	Cvar_Get("ui_om_settings_tab", "input", CVAR_TEMP);
	Cvar_Get("ui_om_settings_search", "", CVAR_TEMP);
	Cvar_Get("ui_om_main_panel", "play", CVAR_TEMP);
	Cvar_Get("ui_om_pause_panel", "root", CVAR_TEMP);
	Cvar_Get("ui_om_cbuf", "", CVAR_TEMP);
	/* Added in Omaha: 1 while CA_ACTIVE — gates Disconnect on modern main. */
	Cvar_Get("ui_om_connected", "0", CVAR_TEMP);
	Cvar_Get("ui_om_vote_allow", "1", CVAR_TEMP);
	Cvar_Get("ui_om_vote_active", "0", CVAR_TEMP);
	Cvar_Get("ui_om_voted", "0", CVAR_TEMP);
	Cvar_Get("ui_om_vote_count", "0", CVAR_TEMP);
	Cvar_Get("ui_om_vote_list_kind", "main", CVAR_TEMP);
	Cvar_Get("ui_om_vote_selected", "", CVAR_TEMP);
	Cvar_Get("ui_om_nav_target", "", CVAR_TEMP);
	Cvar_Get("ui_om_nav_value", "", CVAR_TEMP);
	Cvar_Get("ui_om_servers_visible", "0", CVAR_TEMP);
	Cvar_Get("ui_om_servers_total", "0", CVAR_TEMP);
	Cvar_Get("ui_om_players_total", "0", CVAR_TEMP);
	Cvar_Get("ui_selected_server", "", CVAR_TEMP);
	Cvar_Get("ui_browser_want_refresh", "0", CVAR_TEMP);
	Cvar_Get("ui_om_status_phase", "READY", CVAR_TEMP);
	Cvar_Get("ui_om_favorite_servers", "", CVAR_ARCHIVE);
	Cvar_Get("ui_om_browser_mock", "0", CVAR_TEMP);
	Cvar_Get("ui_browser_favorite_target", "", CVAR_TEMP);
	/* Added in OPM: modern settings helpers (cm/360 + sensitivity mode). */
	Cvar_Get("ui_modernsettings_dpi", "800", CVAR_ARCHIVE);
	Cvar_Get("ui_modernsettings_sensitivity_mode", "sensitivity", CVAR_ARCHIVE);
	/* Added in OPM: cvar-dispatched modals and keybind overwrite context. */
	Cvar_Get("ui_om_modal", "", CVAR_TEMP);
	Cvar_Get("ui_modal_message", "", CVAR_TEMP);
	Cvar_Get("ui_modal_bind_command", "", CVAR_TEMP);
	Cvar_Get("ui_modal_bind_key", "", CVAR_TEMP);
	Cvar_Get("ui_modal_bind_slot", "", CVAR_TEMP);
	Cvar_Get("ui_modal_bind_existing", "", CVAR_TEMP);
	Cvar_Get("ui_modal_confirm_invoke", "", CVAR_TEMP);
	/* Added in OPM: menu map backdrop catalog selection (sources.xml menu-map-views). */
	ui_om_menu_map_view = Cvar_Get("ui_om_menu_map_view", "remagen", CVAR_ARCHIVE);
	/* Added in OPM: HUD pack selection and host sync cvars. */
	ui_om_hud = Cvar_Get("ui_om_hud", "classic", CVAR_ARCHIVE);
	/*
	 * Fixed in OPM: cache legacy flags before UseModernHudPack(). That helper
	 * calls UseLegacyHud(), which re-enters RegisterCvars while g_legacyCached
	 * is still false — infinite recursion / SIGSEGV on launch.
	 */
	g_useLegacyMain = ui_legacy->integer ? qtrue : qfalse;
	g_legacyCached  = qtrue;
	/* Added in OPM: CS2-compatible procedural crosshair settings (after g_legacyCached). */
	XHair_RegisterClientCvars();
	XHair_ClientSyncClAliases();
	/* Added in OPM: per-client opt-in for spectate combat HUD copy on the server. */
	Cvar_Get("om_hud", "0", CVAR_USERINFO | CVAR_TEMP);
	Cvar_Set("om_hud", CL_UIR_UseModernHudPack() ? "1" : "0");
	Cvar_Get("ui_om_hud_health", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_max_health", "100", CVAR_TEMP);
	Cvar_Get("ui_om_hud_health_frac", "1", CVAR_TEMP);
	Cvar_Get("ui_om_hud_clip", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_ammo", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_max_clip", "1", CVAR_TEMP);
	Cvar_Get("ui_om_hud_max_ammo", "1", CVAR_TEMP);
	Cvar_Get("ui_om_hud_team", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_in_zoom", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_sniper_zoom", "0", CVAR_TEMP); /* Added in OPM: sniper scope overlay gate */
	Cvar_Get("ui_om_hud_compass_north", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_damage_dir", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_boss_health", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_show", "1", CVAR_TEMP);
	Cvar_Get("ui_om_hud_vote_text", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_vote_seconds", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_vote_stats", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_vote_prompt", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_vote_keys", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_time_message", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_time_seconds", "", CVAR_TEMP); /* Added in OPM: total seconds left */
	Cvar_Get("ui_om_hud_info_name", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_info_health", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_info_team", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_info_friendly", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_weapons_owned", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_weapons_equipped", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_weapons_visible", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_items_owned", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_items_equipped", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_items_visible", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_active_weapon", "", CVAR_TEMP);
	/* Added in OPM: modern weapons-bar sticky primary/sidearm/last-gun state. */
	Cvar_Get("ui_om_hud_primary_name", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_sidearm_name", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_last_gun", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_last_gun_clip", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_last_gun_ammo", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_attacker_name", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_attacker_team", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_attacker_friendly", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_spectator_text", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_following_text", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_following_name", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_following_team", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_grenade_count", "0", CVAR_TEMP);
	/* Added in OPM: in-HUD chat compose (messagemode) when pack has hud_chat_input. */
	Cvar_Get("ui_om_hud_chat_open", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_chat_label", "Chat: ", CVAR_TEMP);
	Cvar_Get("ui_om_hud_chat_text", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_chat_mode", "100", CVAR_TEMP);
	Cvar_Get("ui_om_hud_im_menu", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_im_image", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_stopwatch_ms", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_stopwatch_type", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_stopwatch_text", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_stopwatch_frac", "0", CVAR_TEMP); /* Added in Omaha: plant bar remaining 0..1 */
	Cvar_Get("ui_om_hud_pause_icon", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_level_exit_icon", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_score_text", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_frag_limit_text", "", CVAR_TEMP);
	/* Added in OPM: modern HUD Allied|timer|Axis / self|timer|leader strip. */
	Cvar_Get("ui_om_hud_allied_score", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_axis_score", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_score_self", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_score_leader", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_objective_left", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_objective_right", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_objective_center", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_objectives_visible", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_objectives_alpha", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_objectives_count", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_compass_angle", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_compass_heading", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_damage_angle", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_damage_alpha", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_obj_arrow_angle", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_obj_left_angle", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_obj_right_angle", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_stopwatch_angle", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_obj_left_visible", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_obj_right_visible", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_obj_arrow_visible", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_boss_frac", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_fuse_frac", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_health_top", "1", CVAR_TEMP);
	Cvar_Get("ui_om_hud_boss_right", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_fuse_right", "0", CVAR_TEMP);
	Cvar_Get("ui_om_hud_weap_pistol_state", "3", CVAR_TEMP);
	Cvar_Get("ui_om_hud_weap_rifle_state", "3", CVAR_TEMP);
	Cvar_Get("ui_om_hud_weap_smg_state", "3", CVAR_TEMP);
	Cvar_Get("ui_om_hud_weap_mg_state", "3", CVAR_TEMP);
	Cvar_Get("ui_om_hud_weap_grenade_state", "3", CVAR_TEMP);
	Cvar_Get("ui_om_hud_weap_heavy_state", "3", CVAR_TEMP);
	Cvar_Get("ui_om_hud_item0_image", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_item0_state", "3", CVAR_TEMP);
	Cvar_Get("ui_om_hud_item0_name", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_item1_image", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_item1_state", "3", CVAR_TEMP);
	Cvar_Get("ui_om_hud_item1_name", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_item2_image", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_item2_state", "3", CVAR_TEMP);
	Cvar_Get("ui_om_hud_item2_name", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_item3_image", "", CVAR_TEMP);
	Cvar_Get("ui_om_hud_item3_state", "3", CVAR_TEMP);
	Cvar_Get("ui_om_hud_item3_name", "", CVAR_TEMP);
	g_useLegacyMain = ui_legacy->integer ? qtrue : qfalse;
	g_legacyCached = qtrue;
	CL_UIR_ApplyDebugRenderPreset();
	UIR_DebugSetEnabled(uir_debug->integer || (ui_render_stats && ui_render_stats->integer));
	CL_UIR_SyncGpuDrawBatch();
}

static void CL_UIR_ReloadWorld_f(void)
{
	UIR_MenuWorldShutdown();
	if (CL_UIR_ShouldRenderModernDisconnected()) {
		UIR_MenuWorldEnsureLoaded();
	}
	Com_Printf("UIR: menu world reload requested (state=%d)\n", (int)UIR_MenuWorldState());
}

static void CL_UIR_TestChrome(void *userdata)
{
	uir_color_t r = {1, 0, 0, 0.75f};
	uir_color_t g = {0, 1, 0, 0.75f};
	uir_point_t t[3] = {{40, 40}, {140, 40}, {90, 120}};
	uir_viewbox_t view = {0, 0, 10, 10};
	uir_rect_t d = {200, 40, 80, 80};
	uir_color_t b = {0.2f, 0.4f, 1.0f, 0.9f};
	(void)userdata;
	UIR_DrawSolidRect(20, 20, 60, 30, &r);
	UIR_FillPolygon2D(t, 3, &g);
	UIR_DrawSvgGeometry("M0 0 C3 10 7 10 10 0 Z", &view, &d, UIR_FIT_CONTAIN, &b, NULL, 0.0f, 0.0f, 0);
	UIR_DrawText(40, 160, "fonts/Oswald-Medium.ttf", 32.0f, "OpenMoHAA", &r, 0.0f);
	/* One-shot: clear so subsequent frames are clean. */
	UIR_CompositorSetChromeCallback(NULL, NULL);
}

static void CL_UIR_Test_f(void)
{
	if (!CL_UIR_IsModernMainActive()) {
		Com_Printf("UIR: ui_render_test requires modern disconnected main\n");
		return;
	}
	UIR_CompositorSetChromeCallback(CL_UIR_TestChrome, NULL);
	Com_Printf("UIR: test chrome scheduled for next modern frame\n");
}

void CL_UIR_EnsureStarted(void)
{
	if (!g_uirStarted) {
		CL_UIR_Init();
	}
	CL_UIR_ApplyDebugRenderPreset();
}

void CL_UIR_Init(void)
{
	if (g_uirStarted) {
		return;
	}
	if (!g_legacyCached) {
		CL_UIR_RegisterCvars();
	}
	CL_UIR_WireBackends();
	CL_UIR_FillUidBackend(&g_uidBackend);
	CL_UIR_RegisterInvokes();
	CL_UIMenu_Init(&g_uidBackend);
	CL_UIMenu_RegisterCommands();
	UIR_Init();
	Cmd_AddCommand("ui_render_reloadworld", CL_UIR_ReloadWorld_f);
	Cmd_AddCommand("ui_render_test", CL_UIR_Test_f);
	Cmd_AddCommand("uir_stress_uiscale", CL_UIR_StressUiScale_f);
	Cmd_AddCommand("ui_design_dump", CL_UIR_DesignDump_f);
	Cmd_AddCommand("ui_compare_goto", CL_UIR_CompareGoto_f);
	Cmd_AddCommand("ui_compare_shot", CL_UIR_CompareShot_f);
	/* Added in Omaha: bindable scoreboard sort cycle (Misc options). */
	Cmd_AddCommand("cycle_scoreboard_sort", CL_UIR_CycleScoreboardSort_f);
	CL_UIR_RegisterBakeCommands();
	Cvar_Get("ui_browser_want_refresh", "0", CVAR_TEMP);
	Cvar_Get("ui_selected_server", "", CVAR_TEMP);
	Cvar_Get("ui_reset_cvar", "", CVAR_TEMP);
	Cvar_Get("ui_legacy_pushmenu_target", "", CVAR_TEMP);
	Cvar_Get("ui_menu_open_target", "", CVAR_TEMP);
	Cvar_Get("ui_menu_close_target", "", CVAR_TEMP);
	Cvar_Get("ui_browser_favorite_target", "", CVAR_TEMP);
	CL_ModernBrowser_Init();
	g_browserDidFirstRefresh = qfalse;
	g_uirStarted = qtrue;
}

void CL_UIR_Shutdown(void)
{
	if (!g_uirStarted) {
		return;
	}
	Cmd_RemoveCommand("ui_render_reloadworld");
	Cmd_RemoveCommand("ui_render_test");
	Cmd_RemoveCommand("uir_stress_uiscale");
	Cmd_RemoveCommand("ui_design_dump");
	Cmd_RemoveCommand("ui_compare_goto");
	Cmd_RemoveCommand("ui_compare_shot");
	Cmd_RemoveCommand("cycle_scoreboard_sort");
	Cmd_RemoveCommand("ui_bake_model");
	Cmd_RemoveCommand("ui_bake_mp_weapons");
	CL_UIMenu_UnregisterCommands();
	CL_UIR_SetDesignLayerCallbacks(qfalse);
	CL_UIMenu_Shutdown();
	UID_ClearInvokes();
	CL_ModernBrowser_Shutdown();
	UIR_Shutdown();
	g_browserDidFirstRefresh = qfalse;
	g_uirStarted = qfalse;
}

void CL_UIR_OnRendererRegistration(void)
{
	CL_UIR_WireBackends();
	CL_UIR_FillUidBackend(&g_uidBackend);
	UIR_OnRendererRegistration();
	UIR_MenuWorldReleaseOwnership();
	UIR_MenuWorldMarkNeedsReload();
	/*
	 * Added in OPM: vid_restart re-inits GL but keeps modern main active — refresh
	 * surface, fonts, layout, and compositor callbacks so the menu is not black/frozen.
	 */
	if (CL_UIMenu_HasAnyOpen()) {
		g_lastLogicalW = -1;
		g_lastLogicalH = -1;
		g_lastFbW = -1;
		g_lastFbH = -1;
		CL_UIR_ApplySurface();
		CL_UIR_PushUiPxScale();
		CL_UIR_SetDesignLayerCallbacks(qtrue);
		CL_UIR_UpdateModern();
	}
}

void CL_UIR_OnResolutionChanged(void)
{
	int lw, lh, fw, fh;

	CL_UIR_GetSurfaceSizes(&lw, &lh, &fw, &fh);
	UIR_OnResolutionChanged(fw, fh);
	if (CL_UIMenu_HasAnyOpen()) {
		g_lastLogicalW = -1;
		g_lastLogicalH = -1;
		g_lastFbW = -1;
		g_lastFbH = -1;
		CL_UIR_ApplySurface();
	}
}

qboolean CL_UIR_UseLegacyMain(void)
{
	if (!g_legacyCached) {
		CL_UIR_RegisterCvars();
	}
	return g_useLegacyMain;
}

const char *CL_UIR_ActiveHudId(void)
{
	if (!g_legacyCached) {
		CL_UIR_RegisterCvars();
	}
	if (g_useLegacyMain) {
		return CL_HUD_LEGACY_ID;
	}
	if (!ui_om_hud || !ui_om_hud->string[0]) {
		return "classic";
	}
	return ui_om_hud->string;
}

const char *CL_UIR_DmPauseMenuId(void)
{
	/* Changed in Omaha: pause companion comes from the active HUD pack XML. */
	const char *hud = CL_UIR_ActiveHudId();
	const char *pauseMenu = CL_UIMenu_HudPauseMenu(hud);
	if (pauseMenu && pauseMenu[0]) {
		return pauseMenu;
	}
	return "dm_pause";
}

/* Added in Omaha: scoreboard companion from the active HUD pack XML. */
const char *CL_UIR_ScoreboardMenuId(void)
{
	const char *hud = CL_UIR_ActiveHudId();
	const char *scoreboardMenu = CL_UIMenu_HudScoreboardMenu(hud);
	if (scoreboardMenu && scoreboardMenu[0]) {
		return scoreboardMenu;
	}
	return "scoreboard";
}

qboolean CL_UIR_UseLegacyHud(void)
{
	if (!g_legacyCached) {
		CL_UIR_RegisterCvars();
	}
	if (g_useLegacyMain) {
		return qtrue;
	}
	if (!ui_om_hud || !ui_om_hud->string[0]) {
		return qfalse;
	}
	return !Q_stricmp(ui_om_hud->string, CL_HUD_LEGACY_ID) ? qtrue : qfalse;
}

qboolean CL_UIR_UseModernHudPack(void)
{
	return CL_UIR_UseLegacyHud() ? qfalse : qtrue;
}

qboolean CL_UIR_IsModernMainActive(void)
{
	return !g_useLegacyMain && CL_UIMenu_IsOpen("main");
}

qboolean CL_UIR_IsEligibleForModernMain(void)
{
	if (!CL_FinishedIntro()) {
		return qfalse;
	}
	if (clc.state != CA_DISCONNECTED) {
		return qfalse;
	}
	if (server_loading) {
		return qfalse;
	}
	if (com_sv_running && com_sv_running->integer) {
		return qfalse;
	}
	return qtrue;
}

void CL_UIR_SyncEligibility(void)
{
	CL_UIMenu_SyncAutoMenus();

	/* Added in Omaha: mirror connection for main-menu Disconnect visibility. */
	Cvar_Set("ui_om_connected", clc.state == CA_ACTIVE ? "1" : "0");

	if (clc.state != CA_ACTIVE) {
		if (CL_UIMenu_IsOpen("main") && clc.state != CA_DISCONNECTED) {
			CL_UIMenu_Close("main");
		}
	}

	if (!CL_UIR_IsEligibleForModernMain() && CL_UIMenu_IsOpen("main") && clc.state != CA_ACTIVE) {
		CL_UIMenu_Close("main");
	}

	/*
	 * Fixed in OPM: 758175fa only released the menu world when every menu closed.
	 * Auto HUD menus stay open in-game, so backdrop loads were never torn down before
	 * gameplay LoadWorld — restore the pre-HUD explicit release on eligibility loss.
	 */
	if (!CL_UIR_IsEligibleForModernMain()) {
		UIR_MenuWorldReleaseOwnership();
		UIR_MenuWorldMarkNeedsReload();
	}

	if (CL_UIMenu_HasAnyOpen()) {
		CL_UIR_SetDesignLayerCallbacks(qtrue);
	} else {
		CL_UIR_SetDesignLayerCallbacks(qfalse);
		CL_UIMenu_OnSessionDeactivate();
		UIR_MenuWorldReleaseOwnership();
		UIR_MenuWorldMarkNeedsReload();
	}
}

void CL_UIR_ActivateModernMain(void)
{
	if (g_useLegacyMain) {
		return;
	}
	if (!g_uirStarted) {
		CL_UIR_Init();
	}
	CL_UIR_SyncEligibility();
	if (!CL_UIMenu_IsOpen("main")) {
		return;
	}
	UI_GetPlayerModel_f();
	CL_UIR_PushUiPxScale();
	CL_UIR_SetDesignLayerCallbacks(qtrue);
	g_pointerWheelDelta = 0;
	if (clc.state == CA_DISCONNECTED) {
		UI_ForceMenuOff(true);
	}
	/* Fixed in OPM: parity with connected overlay — enable GUI mouse / absolute pointer. */
	CL_UIR_EnterModernInputMode();
}

void CL_UIR_DeactivateModernMain(void)
{
	CL_UIMenu_Close("main");
	CL_UIMenu_OnSessionDeactivate();
	CL_UIR_LeaveModernInputMode();
	CL_UIR_SetDesignLayerCallbacks(qfalse);
	g_pointerWheelDelta = 0;
	UIR_MenuWorldReleaseOwnership();
	UIR_MenuWorldMarkNeedsReload();
}

qboolean CL_UIR_ShouldRenderModernDisconnected(void)
{
	CL_UIR_SyncEligibility();
	if (g_useLegacyMain) {
		return qfalse;
	}
	if (!CL_UIR_IsEligibleForModernMain()) {
		return qfalse;
	}
	/* Match pre-758175fa: render whenever disconnected modern main is active. */
	return CL_UIMenu_IsOpen("main");
}

qboolean CL_UIR_ShouldOwnInput(void)
{
	return CL_UIMenu_ShouldOwnInput();
}

qboolean CL_UIR_LegacyModalOwnsInput(void)
{
	/* Changed in OPM: expanded via UI helpers (console, bind, dialogs, menus). */
	if (UI_ConsoleIsOpen() || UI_BindActive()) {
		return qtrue;
	}
	if (UI_LegacyOverlayOwnsInput()) {
		return qtrue;
	}
	return qfalse;
}

qboolean CL_UIR_IsCapturingKeybind(void)
{
	return CL_UIMenu_IsCapturingKeybind();
}

void CL_UIR_UpdateModern(void)
{
	CL_UIR_SyncGpuDrawBatch();
	uid_pointer_state_t pointer;
	int                 lw, lh, fw, fh;
	float               x, y;
	qboolean            ownInput;

	CL_UIR_SyncEligibility();

	if (CL_UIR_IsDmPauseOpen()) {
		CL_UIR_SyncPauseVoteCvars();
	}

	const qboolean hasMenus = CL_UIMenu_HasAnyOpen();
	const qboolean overlayActive = CL_UIMenu_HasInteractiveOpen() && clc.state == CA_ACTIVE;
	if (!hasMenus || (!overlayActive && !CL_UIR_IsEligibleForModernMain() && !CL_UIMenu_HasMenusUpTo(4))) {
		g_pointerWheelDelta = 0;
		return;
	}

	/*
	 * Fixed in OPM: menus opened after the last resolution change (e.g. the
	 * connected main overlay) would keep the 0x0 surface UID_Create starts with
	 * and lay out into nothing. UID_SetSurface no-ops when values are unchanged.
	 */
	CL_UIR_GetSurfaceSizes(&lw, &lh, &fw, &fh);
	CL_UIMenu_ApplySurface(lw, lh, fw, fh);
	g_lastLogicalW = lw;
	g_lastLogicalH = lh;
	g_lastFbW = fw;
	g_lastFbH = fh;
	CL_UIR_DebugDumpSurfaceOnce(lw, lh, fw, fh, (float)uid.mouseX, (float)uid.mouseY);

	CL_UIR_TickUiScaleStress();
	CL_UIR_PushUiPxScale();
	/*
	 * Added in OPM: keep draw-order<=4 HUD runtimes (crosshair, pack, scoreboard)
	 * laid out even when UpdateModern is the only tick (connected overlay / settings).
	 */
	if (clc.state == CA_ACTIVE && CL_UIMenu_HasMenusUpTo(4)) {
		CL_UIR_SyncHudLayerMenus(cls.realtime, nullptr, nullptr, nullptr, nullptr);
	}
	CL_ModernBrowser_Think();

	if (!g_browserDidFirstRefresh && CL_UIMenu_IsOpen("main")) {
		g_browserDidFirstRefresh = qtrue;
		uir_browser_refresh();
	}

	ownInput = CL_UIR_ShouldOwnInput();
	if (!ownInput) {
		if ((CL_UIMenu_HasPointerMenuOpen() || CL_UIMenu_IsOpen(CL_UIR_ScoreboardMenuId())) && clc.state == CA_ACTIVE) {
			/* Consume wheel into the pointer HUD (scoreboard list scroll). */
			CL_UIR_UpdateHudMenus(cls.realtime, qtrue);
		} else {
			g_pointerWheelDelta = 0;
			CL_UIMenu_UpdateAll(cls.realtime);
		}
		uir_browser_update_status_cvars();
		return;
	}

	/* Map from window-space cl.mouse* — FillUIDef maps a copy into uid for winman. */
	x = (float)cl.mousex;
	y = (float)cl.mousey;
	CL_UIR_MapMouseToUiVid(&x, &y);

	memset(&pointer, 0, sizeof(pointer));
	pointer.x = x;
	pointer.y = y;
	pointer.buttons = (int)uid.mouseFlags;
	pointer.wheel = g_pointerWheelDelta;
	pointer.moved = false;
	g_pointerWheelDelta = 0;

	CL_UIMenu_UpdateAllWithPointer(cls.realtime, &pointer);
	uir_browser_update_status_cvars();
}

qboolean CL_UIR_KeyEvent(int key, qboolean down, unsigned time)
{
	CL_UIR_SyncEligibility();

	if (!CL_UIMenu_HasAnyOpen()) {
		return qfalse;
	}
	if (CL_UIR_LegacyModalOwnsInput()) {
		return qfalse;
	}

	if (down) {
		/*
		 * Fixed in OPM: while capturing a keybind, MWHEEL* is the bind key —
		 * do not accumulate scroll delta that steals the wheel from capture.
		 */
		if (!CL_UIR_IsCapturingKeybind()) {
			if (key == K_MWHEELUP) {
				g_pointerWheelDelta++;
			} else if (key == K_MWHEELDOWN) {
				g_pointerWheelDelta--;
			}
		}
	}

	return CL_UIMenu_KeyEvent(key, down, time);
}

qboolean CL_UIR_CharEvent(int ch)
{
	if (!CL_UIR_ShouldOwnInput()) {
		return qfalse;
	}
	if (ch <= 0) {
		return qfalse;
	}
	return CL_UIMenu_CharEvent(ch);
}

void CL_UIR_RenderDisconnectedMain(void)
{
	int lw, lh, fw, fh;
	uir_menu_map_view_t mapView;

	if (!g_uirStarted) {
		CL_UIR_Init();
	}
	if (uir_debug || ui_render_stats) {
		UIR_DebugSetEnabled(
			(uir_debug && uir_debug->integer) || (ui_render_stats && ui_render_stats->integer)
		);
	}

	/* Capture harness: hold consoles shut across the screenshot frame. */
	if (g_compareKeepConsolesClosedUntil && cls.realtime < g_compareKeepConsolesClosedUntil) {
		Key_SetCatcher(Key_GetCatcher() & ~KEYCATCH_CONSOLE);
		UI_CloseConsole();
		UI_CloseDeveloperConsole();
	} else {
		g_compareKeepConsolesClosedUntil = 0;
	}

	CL_UIR_ProfileBeginSample("disconnected_main");
	CL_UIR_UpdateModern();

	UIR_MenuMapViewSetDefaults(&mapView);
	{
		uid_runtime_t *runtime = CL_UIMenu_TopmostMenuWorldRuntime();
		if (runtime && UID_HasDocument(runtime)) {
			const uid_document_t *doc = UID_GetDocument(runtime);
			const char *viewId = (ui_om_menu_map_view && ui_om_menu_map_view->string[0])
				? ui_om_menu_map_view->string
				: UIR_MENU_MAP_VIEW_DEFAULT_ID;
			UID_ResolveMenuMapView(doc, viewId, &mapView);
		}
	}
	UIR_MenuWorldSetDesiredView(&mapView);

	CL_UIR_GetSurfaceSizes(&lw, &lh, &fw, &fh);
	UIR_RenderDisconnectedMain(lw, lh, fw, fh, cls.realtime);
	CL_UIR_ProfileEndSample("disconnected_main");
}

qboolean CL_UIR_IsConnectedOverlayOpen(void)
{
	return CL_UIMenu_IsOpen("main") && clc.state == CA_ACTIVE;
}

void CL_UIR_EnterModernInputMode(void)
{
	UI_EnterModernInputMode();
}

void CL_UIR_EnterModernInputModeKeepKeys(void)
{
	UI_EnterModernInputModeKeepKeys();
}

void CL_UIR_LeaveModernInputMode(void)
{
	UI_LeaveModernInputMode();
}

void CL_UIR_OpenConnectedOverlay(void)
{
	if (CL_UIR_UseLegacyMain() || clc.state != CA_ACTIVE) {
		return;
	}
	if (!g_uirStarted) {
		CL_UIR_Init();
	}
	/*
	 * Fixed in OPM: bail before tearing down the legacy pause menu / latching
	 * KEYCATCH_UI, otherwise a failed load leaves the client with dead input.
	 */
	if (!CL_UIMenu_Open("main", qtrue)) {
		Com_Printf("UIR: cannot open modern main overlay (menu 'main' failed to open)\n");
		return;
	}
	CL_UIR_SetDesignLayerCallbacks(qtrue);
	CL_UIR_ApplyMenuSurfaceNow();
	g_pointerWheelDelta = 0;
	UI_ForceMenuOff(qtrue);
	Key_SetCatcher(Key_GetCatcher() | KEYCATCH_UI);
	CL_UIR_EnterModernInputMode();
	Com_FakePause();
}

void CL_UIR_CloseConnectedOverlay(void)
{
	if (!CL_UIR_IsConnectedOverlayOpen()) {
		return;
	}
	CL_UIMenu_Close("main");
	CL_UIR_LeaveModernInputMode();
	if (clc.state == CA_ACTIVE) {
		Com_FakeUnpause();
	}
}

/* Added in OPM: sync vote widget visibility cvars for dm_pause XML conditionals. */
static void CL_UIR_SyncPauseVoteCvars(void)
{
	const cvar_t *cg_allowvote = Cvar_Get("cg_allowvote", "1", 0);
	int           allow = cg_allowvote && cg_allowvote->integer;
	int           active = 0;
	int           voted = 0;

	if (clc.state == CA_ACTIVE && cl.snap.valid) {
		active = atoi(CL_ConfigString(CS_VOTE_TIME)) != 0;
		voted = cl.snap.ps.voted ? 1 : 0;
	}
	Cvar_Set("ui_om_vote_allow", allow ? "1" : "0");
	Cvar_Set("ui_om_vote_active", active ? "1" : "0");
	Cvar_Set("ui_om_voted", voted ? "1" : "0");
}

void CL_UIR_OpenDmPause(const char *panel)
{
	if (!CL_UIR_UseModernHudPack() || clc.state != CA_ACTIVE) {
		return;
	}
	/*
	 * Changed in OPM: retail mpoptions → modern main Play panel (name/models).
	 * Escape "Main Menu" already opens Settings via dm_pause hit cbuf.
	 */
	if (panel && !Q_stricmp(panel, "options")) {
		if (CL_UIR_IsDmPauseOpen()) {
			CL_UIR_CloseDmPause();
		}
		Cvar_Set("ui_om_main_panel", "play");
		CL_UIR_OpenConnectedOverlay();
		return;
	}
	if (!g_uirStarted) {
		CL_UIR_Init();
	}
	/* Fixed in OPM: populate the retail display-model cvars before team panels bind them. */
	UI_GetPlayerModel_f();
	Cvar_Set("ui_om_pause_panel", (panel && panel[0]) ? panel : "root");
	CL_UIR_SyncPauseVoteCvars();
	const char *menuId = CL_UIR_DmPauseMenuId();
	if (!CL_UIMenu_Open(menuId, qtrue)) {
		Com_Printf("UIR: cannot open modern pause (menu '%s' failed to open)\n", menuId);
		return;
	}
	CL_UIR_SetDesignLayerCallbacks(qtrue);
	CL_UIR_ApplyMenuSurfaceNow();
	g_pointerWheelDelta = 0;
	UI_ForceMenuOff(qtrue);
	Key_SetCatcher(Key_GetCatcher() | KEYCATCH_UI);
	CL_UIR_EnterModernInputMode();
	Com_FakePause();
}

void CL_UIR_CloseDmPause(void)
{
	if (!CL_UIR_IsDmPauseOpen()) {
		return;
	}
	/* Changed in Omaha: close every registered HUD pause companion that is open. */
	const int hudCount = CL_UIMenu_HudCount();
	for (int i = 0; i < hudCount; ++i) {
		const char *hudId = NULL;
		CL_UIMenu_HudEntryAt(i, &hudId, NULL, NULL);
		const char *pauseMenu = CL_UIMenu_HudPauseMenu(hudId);
		if (pauseMenu && pauseMenu[0] && CL_UIMenu_IsOpen(pauseMenu)) {
			CL_UIMenu_Close(pauseMenu);
		}
	}
	if (!CL_UIMenu_HasInteractiveOpen()) {
		CL_UIR_LeaveModernInputMode();
		if (clc.state == CA_ACTIVE) {
			Com_FakeUnpause();
		}
	}
}

qboolean CL_UIR_IsDmPauseOpen(void)
{
	/* Changed in Omaha: any HUD-declared pause companion counts as open. */
	const int hudCount = CL_UIMenu_HudCount();
	for (int i = 0; i < hudCount; ++i) {
		const char *hudId = NULL;
		CL_UIMenu_HudEntryAt(i, &hudId, NULL, NULL);
		const char *pauseMenu = CL_UIMenu_HudPauseMenu(hudId);
		if (pauseMenu && pauseMenu[0] && CL_UIMenu_IsOpen(pauseMenu)) {
			return qtrue;
		}
	}
	return qfalse;
}

void CL_UIR_ToggleConnectedOverlay(void)
{
	if (CL_UIR_IsConnectedOverlayOpen()) {
		CL_UIR_CloseConnectedOverlay();
	} else {
		CL_UIR_OpenConnectedOverlay();
	}
}

qboolean CL_UIR_ShouldRenderConnectedOverlay(void)
{
	CL_UIR_SyncEligibility();
	if (CL_UIR_UseLegacyMain()) {
		return qfalse;
	}
	return CL_UIMenu_HasInteractiveOpen() && clc.state == CA_ACTIVE;
}

void CL_UIR_RenderModernOverlay(void)
{
	int lw, lh, fw, fh;

	if (!CL_UIR_ShouldRenderConnectedOverlay()) {
		return;
	}
	if (!g_uirStarted) {
		CL_UIR_Init();
	}
	if (uir_debug || ui_render_stats) {
		UIR_DebugSetEnabled(
			(uir_debug && uir_debug->integer) || (ui_render_stats && ui_render_stats->integer)
		);
	}
	CL_UIR_ProfileBeginSample("connected_overlay");
	CL_UIR_UpdateModern();
	CL_UIR_GetSurfaceSizes(&lw, &lh, &fw, &fh);
	UIR_RenderConnectedOverlay(lw, lh, fw, fh, cls.realtime);
	CL_UIR_ProfileEndSample("connected_overlay");
}

void CL_UIR_DrawCrosshair(void)
{
	uir_viewport_t vp;
	int            lw = 0;
	int            lh = 0;
	int            fw = 0;
	int            fh = 0;
	qboolean       hadCallbacks;
	qboolean       paintHud;
	qboolean       paintSniper;

	CL_UIR_EnsureStarted();
	CL_UIR_SyncEligibility();
	/*
	 * Fixed in OPM: do not start a modern hud_layer profile sample on the
	 * legacy URC path — UI_Update already owns the legacy_ui sample and a
	 * nested BeginSample was resetting mid-frame timings/labels.
	 */
	if (CL_UIR_UseLegacyHud()) {
		return;
	}
	/*
	 * Fixed in OPM: sync HUD menus before paint gates so layout/uiPxScale stay
	 * current even when ShouldPaintHudLayer returns false (zoom, settings, etc.).
	 */
	CL_UIR_ProfileBeginSample("hud_layer");
	(void)CL_UIR_SyncHudLayerMenus(cls.realtime, &lw, &lh, &fw, &fh);

	paintSniper = (Cvar_VariableIntegerValue("ui_om_hud_sniper_zoom") != 0
				   && Cvar_VariableIntegerValue("cg_crosshair_sniper_modern") != 0
				   && CL_UIR_UseModernHudPack())
					  ? qtrue
					  : qfalse;
	{
		/* Fixed in OPM: scope lives in chrome; rebuild retained chrome when zoom toggles. */
		static qboolean s_lastSniperZoom = qfalse;
		if (paintSniper != s_lastSniperZoom) {
			UIR_InvalidateChromeCache();
			s_lastSniperZoom = paintSniper;
		}
	}
	paintHud = qfalse;
	if (CL_UIR_ShouldPaintHudLayer() && CL_UIMenu_HasMenusUpTo(4)) {
		paintHud = qtrue;
	}
	if (!paintHud && !paintSniper) {
		CL_UIR_ProfileEndSample("hud_layer_sync_only");
		return;
	}
	if (fw <= 0 || fh <= 0) {
		CL_UIR_GetSurfaceSizes(&lw, &lh, &fw, &fh);
		if (fw <= 0 || fh <= 0) {
			CL_UIR_ProfileEndSample("hud_layer_nosurface");
			return;
		}
	}

	if (UIR_ViewportMakeOrtho(0, 0, fw, fh, 0.0f, (float)lw, 0.0f, (float)lh, &vp) != UIR_OK) {
		CL_UIR_ProfileEndSample("hud_layer_viewport_fail");
		return;
	}

	if (uir_debug || ui_render_stats) {
		UIR_DebugSetEnabled(
			(uir_debug && uir_debug->integer) || (ui_render_stats && ui_render_stats->integer)
		);
	}

	hadCallbacks = CL_UIMenu_HasInteractiveOpen() ? qtrue : qfalse;
	if (!hadCallbacks) {
		if (paintHud) {
			UIR_CompositorSetChromeCallback(CL_UIR_HudChromeCallback, NULL);
			UIR_CompositorSetOverlayCallback(CL_UIR_HudOverlayCallback, NULL);
		} else {
			/* Added in OPM: sniper scope still paints when HUD chrome is gated off. */
			UIR_CompositorSetChromeCallback(CL_UIR_SniperZoomChromeCallback, NULL);
			UIR_CompositorSetOverlayCallback(NULL, NULL);
		}
	}
	if (UIR_BeginOverlayFrame(&vp, cls.realtime) == UIR_OK) {
		UIR_EndOverlayFrame();
	}
	if (!hadCallbacks) {
		UIR_CompositorSetChromeCallback(NULL, NULL);
		UIR_CompositorSetOverlayCallback(NULL, NULL);
	}
	CL_UIR_ProfileEndSample("hud_layer");
}
