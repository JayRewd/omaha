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

#include "uir_backend.h"
#include "uir_batch.h"
#include "uir_compositor.h"
#include "uir_debug.h"
#include "uir_draw2d.h"
#include "uir_font.h"
#include "uir_gradient.h"
#include "uir_image.h"
#include "uir_menuworld.h"
#include "uir_menu_weather.h"
#include "uir_meshcache.h"
#include "uir_path.h"
#include "uir_pathcache.h"
#include "uir_svg.h"
#include "uir_viewport.h"

#include <math.h>
#include <string.h>

static int g_uirInited = 0;
static int g_screenW = 640;
static int g_screenH = 480;

void UIR_Init(void)
{
	g_uirInited = 1;
	UIR_CompositorReset();
}

void UIR_Shutdown(void)
{
	UIR_PathCacheClear();
	UIR_MeshCacheClear();
	UIR_FontShutdown();
	UIR_GradientShutdown();
	UIR_MenuWorldShutdown();
	UIR_CompositorReset();
	g_uirInited = 0;
}

void UIR_OnRendererRegistration(void)
{
	UIR_PathCacheClear();
	UIR_MeshCacheClear();
	UIR_CompositorReset();
	UIR_FontInvalidateGpu();
	UIR_ImageInvalidateGpu();
	UIR_GradientInvalidateGpu();
	UIR_MenuWeatherInvalidate();
	UIR_MenuWorldMarkNeedsReload();
}

void UIR_OnResolutionChanged(int width, int height)
{
	UIR_PathCacheClear();
	UIR_MeshCacheClear();
	if (width > 0) {
		g_screenW = width;
	}
	if (height > 0) {
		g_screenH = height;
	}
	UIR_FontInvalidateGpu();
	UIR_ImageInvalidateGpu();
	UIR_GradientInvalidateGpu();
}

void UIR_RenderDisconnectedMain(int logicalW, int logicalH, int fbW, int fbH, int realtime)
{
	uir_viewport_t vp;
	int            lw = logicalW;
	int            lh = logicalH;
	int            fw = fbW;
	int            fh = fbH;

	if (!g_uirInited) {
		UIR_Init();
	}
	if (fw <= 0) {
		fw = g_screenW;
	}
	if (fh <= 0) {
		fh = g_screenH;
	}
	if (lw <= 0) {
		lw = fw;
	}
	if (lh <= 0) {
		lh = fh;
	}
	g_screenW = fw;
	g_screenH = fh;

	if (UIR_ViewportMakeOrtho(0, 0, fw, fh, 0.0f, (float)lw, 0.0f, (float)lh, &vp) != UIR_OK) {
		return;
	}

	if (UIR_BeginDisconnectedFrame(&vp, realtime) != UIR_OK) {
		return;
	}
	UIR_EndDisconnectedFrame();
	UIR_BatchFlush();

	if (UIR_DebugEnabled()) {
		UIR_DebugDumpStats(UIR_CompositorStats());
	}
}

void UIR_RenderConnectedOverlay(int logicalW, int logicalH, int fbW, int fbH, int realtime)
{
	uir_viewport_t vp;
	int            lw = logicalW;
	int            lh = logicalH;
	int            fw = fbW;
	int            fh = fbH;

	if (!g_uirInited) {
		UIR_Init();
	}
	if (fw <= 0) {
		fw = g_screenW;
	}
	if (fh <= 0) {
		fh = g_screenH;
	}
	if (lw <= 0) {
		lw = fw;
	}
	if (lh <= 0) {
		lh = fh;
	}
	g_screenW = fw;
	g_screenH = fh;

	if (UIR_ViewportMakeOrtho(0, 0, fw, fh, 0.0f, (float)lw, 0.0f, (float)lh, &vp) != UIR_OK) {
		return;
	}

	if (UIR_BeginOverlayFrame(&vp, realtime) != UIR_OK) {
		return;
	}
	UIR_EndOverlayFrame();
	UIR_BatchFlush();

	if (UIR_DebugEnabled()) {
		UIR_DebugDumpStats(UIR_CompositorStats());
	}
}

uir_status_t UIR_DrawSvgGeometry(
	const char *pathD,
	const uir_viewbox_t *viewBox,
	const uir_rect_t *dest,
	uir_fit_mode_t fit,
	const uir_color_t *fillRgba,
	const uir_color_t *strokeRgba,
	float strokeWidthPx,
	float rotationDeg,
	int crisp
)
{
	uir_status_t st;
	const uir_viewport_t *vp = UIR_CompositorViewport();
	const uir_path_t *drawPath = NULL;
	uir_rect_t snappedDest;
	const uir_rect_t *useDest = dest;
	float useStrokeW = strokeWidthPx;
	int stroked = 0;

	if (!pathD || !viewBox || !dest) {
		return UIR_ERR_INVALID_ARG;
	}
	if (!fillRgba && !(strokeRgba && strokeWidthPx > 0.0f)) {
		return UIR_ERR_INVALID_ARG;
	}

	/*
	 * Added in OPM: crisp HUD marks (crosshair) snap to the FB pixel grid and
	 * quantize stroke width to whole framebuffer pixels so edges stay hard.
	 */
	if (crisp && vp) {
		snappedDest = *dest;
		UIR_ViewportSnapQuad(vp, &snappedDest.x, &snappedDest.y, &snappedDest.w, &snappedDest.h);
		useDest = &snappedDest;
		if (useStrokeW > 0.0f && vp->scaleX > 0.0f) {
			float fbW = useStrokeW * vp->scaleX;
			if (fbW < 1.0f) {
				fbW = 1.0f;
			} else {
				fbW = floorf(fbW + 0.5f);
			}
			useStrokeW = fbW * vp->invX;
		}
	}

	/* Added in OPM: zero-copy mapped path from shared path cache (do not free). */
	st = UIR_GetMappedPathCached(pathD, useDest, viewBox, fit, rotationDeg, crisp, &drawPath);
	if (st != UIR_OK || !drawPath) {
		return st != UIR_OK ? st : UIR_ERR_INVALID_ARG;
	}

	st = UIR_OK;
	/*
	 * Changed in OPM: stroke then fill when both are set. Centerline/outside stroke
	 * otherwise covers thin ring fills and leaves dark gaps at concave star corners;
	 * redrawing fill on top restores interior while keeping the outer outline.
	 */
	if (strokeRgba && useStrokeW > 0.0f && strokeRgba->a > 0.0f) {
		st = UIR_StrokePath2D(drawPath, strokeRgba, useStrokeW, crisp);
		if (st != UIR_OK) {
			return st;
		}
		stroked = 1;
	}
	if (fillRgba && fillRgba->a > 0.0f) {
		st = UIR_FillPath2D(drawPath, fillRgba, crisp, stroked);
	}
	return st;
}

uir_status_t UIR_DrawText(
	float x,
	float y,
	const char *fontPath,
	float pixelHeight,
	const char *text,
	const uir_color_t *rgba,
	float tracking
)
{
	uir_font_t *font;
	const uir_viewport_t *vp = UIR_CompositorViewport();

	if (!fontPath || !text || !rgba || !vp) {
		return UIR_ERR_INVALID_ARG;
	}
	font = UIR_FontLoad(fontPath, pixelHeight);
	if (!font) {
		return UIR_ERR_MISSING_ASSET;
	}
	return UIR_FontDraw(vp, font, x, y, text, rgba, tracking);
}
