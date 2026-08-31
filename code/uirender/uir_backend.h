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
#ifndef UIR_BACKEND_H
#define UIR_BACKEND_H

#include "uir_types.h"
#include "uir_compositor.h"
#include "uir_svg.h"
#include "uir_font.h"
#include "uir_modelpreview.h"
#include "uir_pathcache.h"

#ifdef __cplusplus
extern "C" {
#endif

void UIR_Init(void);
void UIR_Shutdown(void);
void UIR_OnRendererRegistration(void);
void UIR_OnResolutionChanged(int width, int height);

/* High-level disconnected main entry used by the client bridge. */
void UIR_RenderDisconnectedMain(int logicalW, int logicalH, int fbW, int fbH, int realtime);

/* Added in OPM: connected gameplay overlay (chrome only, no menu world). */
void UIR_RenderConnectedOverlay(int logicalW, int logicalH, int fbW, int fbH, int realtime);

uir_status_t UIR_DrawSvgGeometry(
	const char *pathD,
	const uir_viewbox_t *viewBox,
	const uir_rect_t *dest,
	uir_fit_mode_t fit,
	const uir_color_t *fillRgba,   /* NULL or a<=0 skips fill */
	const uir_color_t *strokeRgba, /* NULL skips stroke */
	float strokeWidthPx,           /* draw-space px; ignored without stroke */
	float rotationDeg,             /* Added in OPM: clockwise degrees around dest center */
	int crisp                      /* Added in OPM: binary coverage, no soft AA */
);

uir_status_t UIR_DrawText(
	float x,
	float y,
	const char *fontPath,
	float pixelHeight,
	const char *text,
	const uir_color_t *rgba,
	float tracking
);

#ifdef __cplusplus
}
#endif

#endif /* UIR_BACKEND_H */
