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
#ifndef UIR_COMPOSITOR_H
#define UIR_COMPOSITOR_H

#include "uir_types.h"
#include "uir_modelpreview.h"
#include "uir_path.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UIR_MAX_PREVIEW_QUEUE 8

typedef void (*uir_layer_fn)(void *userdata);

typedef struct {
	uir_rect_t                 rect;
	uir_model_preview_params_t params;
	char                       animStorage[64]; /* Fixed in OPM: own anim name for deferred draw */
} uir_queued_preview_t;

void UIR_CompositorReset(void);
uir_frame_phase_t UIR_CompositorPhase(void);

uir_status_t UIR_BeginDisconnectedFrame(const uir_viewport_t *vp, int realtime);
uir_status_t UIR_BeginOverlayFrame(const uir_viewport_t *vp, int realtime);
uir_status_t UIR_EndOverlayFrame(void);
uir_status_t UIR_DrawSolidRect(float x, float y, float w, float h, const uir_color_t *rgba);
uir_status_t UIR_FillPolygon2D(const uir_point_t *pts, int count, const uir_color_t *rgba);
uir_status_t UIR_FillPath2D(const uir_path_t *path, const uir_color_t *rgba, int crisp, int noFringe);
/* Added in OPM */
uir_status_t UIR_StrokePath2D(const uir_path_t *path, const uir_color_t *rgba, float widthPx, int crisp);
uir_status_t UIR_QueueModelPreview(const uir_rect_t *rect, const uir_model_preview_params_t *params);
uir_status_t UIR_EndDisconnectedFrame(void);

/* Optional chrome/overlay callbacks invoked during the frame. */
void UIR_CompositorSetChromeCallback(uir_layer_fn fn, void *userdata);
void UIR_CompositorSetOverlayCallback(uir_layer_fn fn, void *userdata);

const uir_viewport_t *UIR_CompositorViewport(void);
uir_stats_t *UIR_CompositorStats(void);

/* Nested clip stack (logical draw units). Max depth matches XML depth (64). */
#define UIR_MAX_CLIP_DEPTH 64
uir_status_t UIR_PushClipRect(float x, float y, float w, float h);
void         UIR_PopClipRect(void);
void         UIR_ResetClipStack(void);
/* Added in OPM: toggle / invalidate clip-scissor dedup. */
void         UIR_SetClipDedup(int enable);
void         UIR_InvalidateAppliedClip(void);

/* Added in OPM: optional retained chrome RT (gl1); default off via ui_chrome_cache. */
typedef struct {
	int (*available)(void);
	int (*beginCapture)(float uiX, float uiY, float uiW, float uiH);
	void (*endCapture)(void);
	void (*blit)(void);
	void (*invalidate)(void);
} uir_chrome_cache_backend_t;

void UIR_ChromeCacheSetBackend(const uir_chrome_cache_backend_t *backend);
void UIR_SetChromeCache(int enable);
void UIR_InvalidateChromeCache(void);
void UIR_ChromeCacheRequestRebuild(void);

#ifdef __cplusplus
}
#endif

#endif /* UIR_COMPOSITOR_H */
