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
// tr_ui_fbo.c -- optional offscreen MSAA target for modern UI (GL1)
#include "tr_local.h"

#include <math.h>

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif

extern cvar_t *r_uiFramebuffer;
extern cvar_t *r_uiMultisample;
extern cvar_t *r_uiClearMode;
extern cvar_t *r_uiResolveRects;

typedef struct {
	qboolean active;
	qboolean hasStencil;
	int      width;
	int      height;
	int      samples;
	GLuint   msaaFbo;
	GLuint   msaaColorRb;
	GLuint   msaaDepthRb; /* DEPTH24_STENCIL8 when hasStencil, else DEPTH_COMPONENT24 */
	GLuint   resolveFbo;
	GLuint   resolveTex;
} ui_fbo_state_t;

/* Added in Omaha: Phase 2 dirty-rect resolve tracking (GL window coords, origin bottom-left). */
typedef struct {
	int x, y, w, h;
} uiRectI_t;

#define UI_FBO_MAX_RECTS 8
#define UI_FBO_MERGE_PAD 8

static ui_fbo_state_t s_uiFbo;
static uiRectI_t      s_uiRects[UI_FBO_MAX_RECTS];
static int            s_uiRectCount;
static qboolean       s_uiFullResolve;
/*
 * Added in Omaha: Phase 4.6 — retained UI target. A session begun with
 * RE_BeginUI2DTargetKeep is "retainable": its End leaves the MSAA colour and
 * the resolve texture as a complete UI image. The next Keep session may skip
 * the clear and only redraw/blit dirty rects; the composite then covers the
 * union of everything drawn since the last clear (s_uiKeepRects).
 */
static uiRectI_t      s_uiKeepRects[UI_FBO_MAX_RECTS];
static int            s_uiKeepRectCount;
static qboolean       s_uiKeepFull;
static qboolean       s_uiContentValid; /* FBO holds a complete retainable image */
static qboolean       s_uiRetainable;   /* current session begun via Keep */
static qboolean       s_uiRetained;     /* current session skipped the clear */
static int            s_win2DX, s_win2DY, s_win2DW, s_win2DH;
static float          s_win2DLeft, s_win2DRight, s_win2DBottom, s_win2DTop;
static qboolean       s_win2DValid;

/* Added in Omaha: Phase 2 — capture current Set2DWindow for draw→window conversion. */
void RE_UI2D_NoteWin2D(int x, int y, int w, int h, float left, float right, float bottom, float top)
{
	s_win2DX = x;
	s_win2DY = y;
	s_win2DW = w;
	s_win2DH = h;
	s_win2DLeft = left;
	s_win2DRight = right;
	s_win2DBottom = bottom;
	s_win2DTop = top;
	s_win2DValid = qtrue;
}

void RE_UI2D_DrawToWindow(float dx, float dy, float *wx, float *wy)
{
	float sx;
	float sy;
	float denomX;
	float denomY;

	if (!wx || !wy) {
		return;
	}
	if (!s_win2DValid || s_win2DW <= 0 || s_win2DH <= 0) {
		*wx = dx;
		*wy = dy;
		return;
	}
	denomX = s_win2DRight - s_win2DLeft;
	denomY = s_win2DBottom - s_win2DTop;
	sx = (denomX != 0.0f) ? ((float)s_win2DW / denomX) : 1.0f;
	sy = (denomY != 0.0f) ? ((float)s_win2DH / denomY) : 1.0f;
	/* Ortho bottom > top in UI top-left space → flip Y into GL bottom-left window. */
	*wx = (float)s_win2DX + (dx - s_win2DLeft) * sx;
	*wy = (float)s_win2DY + (s_win2DBottom - dy) * sy;
}

static int RE_UI2D_RectsTouchOrIntersect(const uiRectI_t *a, const uiRectI_t *b, int pad)
{
	int ax1 = a->x - pad;
	int ay1 = a->y - pad;
	int ax2 = a->x + a->w + pad;
	int ay2 = a->y + a->h + pad;
	int bx2 = b->x + b->w;
	int by2 = b->y + b->h;

	return !(ax2 < b->x || bx2 < ax1 || ay2 < b->y || by2 < ay1);
}

static void RE_UI2D_UnionRect(uiRectI_t *dst, const uiRectI_t *src)
{
	int x0 = dst->x < src->x ? dst->x : src->x;
	int y0 = dst->y < src->y ? dst->y : src->y;
	int x1 = (dst->x + dst->w) > (src->x + src->w) ? (dst->x + dst->w) : (src->x + src->w);
	int y1 = (dst->y + dst->h) > (src->y + src->h) ? (dst->y + dst->h) : (src->y + src->h);
	dst->x = x0;
	dst->y = y0;
	dst->w = x1 - x0;
	dst->h = y1 - y0;
}

static int RE_UI2D_RectArea(const uiRectI_t *r)
{
	return r->w * r->h;
}

void RE_UI2D_MarkFullResolve(void)
{
	if (!RE_UI2DTargetIsActive()) {
		return;
	}
	s_uiFullResolve = qtrue;
}

/* Added in Omaha: Phase 4.6 — merge nr into a bounded rect list (touch/pad union, least-growth spill). */
static void RE_UI2D_RectListAdd(uiRectI_t *rects, int *count, const uiRectI_t *nr)
{
	int i;
	int best;
	int bestGrowth;

	for (i = 0; i < *count; i++) {
		if (RE_UI2D_RectsTouchOrIntersect(&rects[i], nr, UI_FBO_MERGE_PAD)) {
			RE_UI2D_UnionRect(&rects[i], nr);
			/* One merge pass against others after growth. */
			{
				int j;
				for (j = 0; j < *count; j++) {
					if (j == i) {
						continue;
					}
					if (RE_UI2D_RectsTouchOrIntersect(&rects[i], &rects[j], UI_FBO_MERGE_PAD)) {
						RE_UI2D_UnionRect(&rects[i], &rects[j]);
						rects[j] = rects[*count - 1];
						(*count)--;
						if (j < i) {
							i = j;
						}
						j--;
					}
				}
			}
			return;
		}
	}

	if (*count < UI_FBO_MAX_RECTS) {
		rects[(*count)++] = *nr;
		return;
	}

	best = 0;
	bestGrowth = 0x7fffffff;
	for (i = 0; i < *count; i++) {
		uiRectI_t u = rects[i];
		int growth;
		RE_UI2D_UnionRect(&u, nr);
		growth = RE_UI2D_RectArea(&u) - RE_UI2D_RectArea(&rects[i]);
		if (growth < bestGrowth) {
			bestGrowth = growth;
			best = i;
		}
	}
	RE_UI2D_UnionRect(&rects[best], nr);
}

void RE_UI2D_AccumRectFb(int x, int y, int w, int h)
{
	uiRectI_t nr;
	int i = 0;
	int best = 0;
	int bestGrowth = 0;
	int maxW;
	int maxH;

	if (!RE_UI2DTargetIsActive() || s_uiFullResolve) {
		return;
	}
	if (w <= 0 || h <= 0) {
		return;
	}

	maxW = s_uiFbo.width > 0 ? s_uiFbo.width : glConfig.vidWidth;
	maxH = s_uiFbo.height > 0 ? s_uiFbo.height : glConfig.vidHeight;
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > maxW) {
		w = maxW - x;
	}
	if (y + h > maxH) {
		h = maxH - y;
	}
	if (w <= 0 || h <= 0) {
		return;
	}

	/*
	 * Added in Omaha: Phase 4.6 — a scissored draw cannot touch pixels outside
	 * the scissor box (compass tape verts span 3 periods; only the FOV window
	 * is written). Clamp so the dirty rect stays the visible strip.
	 */
	if (glState.scissorEnabled) {
		int sx0 = glState.scissorBox[0];
		int sy0 = glState.scissorBox[1];
		int sx1 = sx0 + glState.scissorBox[2];
		int sy1 = sy0 + glState.scissorBox[3];
		int x1 = x + w;
		int y1 = y + h;
		if (x < sx0) {
			x = sx0;
		}
		if (y < sy0) {
			y = sy0;
		}
		if (x1 > sx1) {
			x1 = sx1;
		}
		if (y1 > sy1) {
			y1 = sy1;
		}
		w = x1 - x;
		h = y1 - y;
		if (w <= 0 || h <= 0) {
			return;
		}
	}

	nr.x = x;
	nr.y = y;
	nr.w = w;
	nr.h = h;
	RE_UI2D_RectListAdd(s_uiRects, &s_uiRectCount, &nr);
	(void)i;
	(void)best;
	(void)bestGrowth;
}

void RE_UI2D_AccumRectDraw(float x0, float y0, float x1, float y1)
{
	float wx0, wy0, wx1, wy1;
	float minx, miny, maxx, maxy;
	int ix, iy, iw, ih;

	if (!RE_UI2DTargetIsActive() || s_uiFullResolve) {
		return;
	}

	RE_UI2D_DrawToWindow(x0, y0, &wx0, &wy0);
	RE_UI2D_DrawToWindow(x1, y1, &wx1, &wy1);
	minx = wx0 < wx1 ? wx0 : wx1;
	maxx = wx0 > wx1 ? wx0 : wx1;
	miny = wy0 < wy1 ? wy0 : wy1;
	maxy = wy0 > wy1 ? wy0 : wy1;
	ix = (int)floorf(minx) - 2;
	iy = (int)floorf(miny) - 2;
	iw = (int)ceilf(maxx) - ix + 2;
	ih = (int)ceilf(maxy) - iy + 2;
	RE_UI2D_AccumRectFb(ix, iy, iw, ih);
}

static qboolean RE_UI2D_FboProcsReady(void)
{
	if (!qglGenFramebuffers || !qglDeleteFramebuffers || !qglBindFramebuffer ||
	    !qglGenRenderbuffers || !qglDeleteRenderbuffers || !qglBindRenderbuffer ||
	    !qglRenderbufferStorageMultisample || !qglRenderbufferStorage ||
	    !qglFramebufferRenderbuffer || !qglFramebufferTexture2D ||
	    !qglCheckFramebufferStatus || !qglBlitFramebuffer || !qglBlendFuncSeparate) {
		return qfalse;
	}
	return qtrue;
}

void RE_UI2D_FboShutdown(void)
{
	/* Added in Omaha: tear down soft mask-image layer with the main UI FBO. */
	RE_UiLayerShutdown();
	/* Added in Omaha: tear down chrome cache RT with the main UI FBO. */
	RE_UiChromeCacheShutdown();
	/* Added in Omaha: Phase 3 — drop UI VBO/IBO with FBO/vid_restart. */
	RE_UI2D_VboShutdown();

	if (s_uiFbo.msaaFbo) {
		qglDeleteFramebuffers(1, &s_uiFbo.msaaFbo);
	}
	if (s_uiFbo.resolveFbo) {
		qglDeleteFramebuffers(1, &s_uiFbo.resolveFbo);
	}
	if (s_uiFbo.msaaColorRb) {
		qglDeleteRenderbuffers(1, &s_uiFbo.msaaColorRb);
	}
	if (s_uiFbo.msaaDepthRb) {
		qglDeleteRenderbuffers(1, &s_uiFbo.msaaDepthRb);
	}
	if (s_uiFbo.resolveTex) {
		qglDeleteTextures(1, &s_uiFbo.resolveTex);
	}
	/* Changed in Omaha: deleted FBOs unbind implicitly — invalidate tracked binding. */
	GL_InvalidateFramebufferBinding();
	memset(&s_uiFbo, 0, sizeof(s_uiFbo));
	/* Added in Omaha: Phase 4.6 — no retained image after teardown. */
	s_uiContentValid = qfalse;
	s_uiKeepRectCount = 0;
	s_uiKeepFull = qfalse;
	s_uiRetainable = qfalse;
	s_uiRetained = qfalse;
}

static int RE_UI2D_FboClampSamples(int samples)
{
	if (samples <= 1) {
		return 0;
	}
	if (samples <= 2) {
		return 2;
	}
	if (samples <= 4) {
		return 4;
	}
	return 8;
}

static qboolean RE_UI2D_FboEnsure(int width, int height, int samples)
{
	if (width <= 0 || height <= 0) {
		return qfalse;
	}

	if (s_uiFbo.msaaFbo && s_uiFbo.width == width && s_uiFbo.height == height &&
	    s_uiFbo.samples == samples) {
		return qtrue;
	}

	RE_UI2D_FboShutdown();

	s_uiFbo.width = width;
	s_uiFbo.height = height;
	s_uiFbo.samples = samples;

	if (r_uiFramebuffer && r_uiFramebuffer->integer) {
		ri.Printf(PRINT_DEVELOPER, "UI FBO: %dx%d samples=%d\n", width, height, samples);
	}

	qglGenFramebuffers(1, &s_uiFbo.msaaFbo);
	GL_BindFramebuffer(GL_FRAMEBUFFER, s_uiFbo.msaaFbo);

	qglGenRenderbuffers(1, &s_uiFbo.msaaColorRb);
	qglBindRenderbuffer(GL_RENDERBUFFER, s_uiFbo.msaaColorRb);
	if (samples > 0) {
		qglRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
	} else {
		qglRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
	}
	qglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_uiFbo.msaaColorRb);

	qglGenRenderbuffers(1, &s_uiFbo.msaaDepthRb);
	qglBindRenderbuffer(GL_RENDERBUFFER, s_uiFbo.msaaDepthRb);
	/*
	 * Added in Omaha: packed depth+stencil so UI shape clips can write a real
	 * stencil mask into the offscreen target (window glConfig.stencilBits is
	 * the wrong probe when r_uiFramebuffer is on).
	 */
	if (samples > 0) {
		qglRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
	} else {
		qglRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
	}
	qglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_uiFbo.msaaDepthRb);

	if (qglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		/* Fallback: depth-only (no UI stencil). Shape clips stay on AABB. */
		qglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
		if (samples > 0) {
			qglRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, width, height);
		} else {
			qglRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
		}
		qglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_uiFbo.msaaDepthRb);
		if (qglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			RE_UI2D_FboShutdown();
			return qfalse;
		}
		s_uiFbo.hasStencil = qfalse;
		ri.Printf(PRINT_WARNING, "UI FBO: DEPTH24_STENCIL8 unavailable; shape clips use AABB\n");
	} else {
		s_uiFbo.hasStencil = qtrue;
	}

	qglGenTextures(1, &s_uiFbo.resolveTex);
	qglBindTexture(GL_TEXTURE_2D, s_uiFbo.resolveTex);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, haveClampToEdge ? GL_CLAMP_TO_EDGE : GL_CLAMP);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, haveClampToEdge ? GL_CLAMP_TO_EDGE : GL_CLAMP);
	qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	qglGenFramebuffers(1, &s_uiFbo.resolveFbo);
	GL_BindFramebuffer(GL_FRAMEBUFFER, s_uiFbo.resolveFbo);
	qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_uiFbo.resolveTex, 0);
	if (qglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		RE_UI2D_FboShutdown();
		return qfalse;
	}

	GL_BindFramebuffer(GL_FRAMEBUFFER, 0);
	return qtrue;
}

qboolean RE_UI2DTargetIsActive(void)
{
	return s_uiFbo.active;
}

/* Added in Omaha: re-assert FBO straight-alpha Separate after GL_State/glBlendFunc. */
void RE_UI2DRestoreFboBlend(void)
{
	if (!s_uiFbo.active || !qglBlendFuncSeparate) {
		return;
	}
	qglBlendFuncSeparate(
		GL_SRC_ALPHA,
		GL_ONE_MINUS_SRC_ALPHA,
		GL_ONE,
		GL_ONE_MINUS_SRC_ALPHA
	);
}

qboolean RE_UI2DTargetHasStencil(void)
{
	return (s_uiFbo.msaaFbo && s_uiFbo.hasStencil) ? qtrue : qfalse;
}

int RE_UI2DTargetSamples(void)
{
	if (!s_uiFbo.active) {
		return 0;
	}
	return s_uiFbo.samples;
}

void RE_UI2DTargetRebind(void)
{
	/* Added in Omaha: soft mask-image layer is the top of the UI target stack. */
	if (RE_UiLayerIsActive()) {
		RE_UiLayerRebind();
		return;
	}
	/* Added in Omaha: chrome cache capture sits under the soft-mask layer. */
	if (RE_UiChromeCacheIsActive()) {
		RE_UiChromeCacheRebind();
		return;
	}
	if (s_uiFbo.active && s_uiFbo.msaaFbo) {
		GL_BindFramebuffer(GL_FRAMEBUFFER, s_uiFbo.msaaFbo);
		qglViewport(0, 0, s_uiFbo.width, s_uiFbo.height);
	}
}

qboolean RE_UI2DTargetAvailable(void)
{
	if (!r_uiFramebuffer || !r_uiFramebuffer->integer) {
		return qfalse;
	}
	return RE_UI2D_FboProcsReady();
}

/*
 * Changed in Omaha: Phase 4.6 — shared begin. keep!=0 marks the session
 * retainable and skips the clear when the FBO still holds a valid retainable
 * image of the same size. Returns 0 fail, 1 began (cleared), 2 began (kept).
 */
static int RE_UI2D_BeginTargetImpl(int retainable, int keep)
{
	int samples;
	int kept;

	if (!RE_UI2DTargetAvailable()) {
		return 0;
	}

	R_IssuePendingRenderCommands();

	samples = RE_UI2D_FboClampSamples(r_uiMultisample ? r_uiMultisample->integer : 0);
	if (s_uiFbo.msaaFbo &&
	    (s_uiFbo.width != glConfig.vidWidth || s_uiFbo.height != glConfig.vidHeight || s_uiFbo.samples != samples)) {
		s_uiContentValid = qfalse;
	}
	if (!RE_UI2D_FboEnsure(glConfig.vidWidth, glConfig.vidHeight, samples)) {
		return 0;
	}

	kept = (retainable && keep && s_uiContentValid) ? 1 : 0;

	GL_BindFramebuffer(GL_FRAMEBUFFER, s_uiFbo.msaaFbo);
	/* Added in Omaha: Phase 1 — FBO viewport change invalidates Set2DWindow dedup. */
	RE_InvalidateSet2DWindow();
	qglViewport(0, 0, s_uiFbo.width, s_uiFbo.height);
	GL_ScissorEnable(qfalse);
	if (!kept) {
		qglClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		/* Changed in Omaha: Phase 2 — color-only clear by default (r_uiClearMode 0). */
		if (r_uiClearMode && r_uiClearMode->integer) {
			qglClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		} else {
			qglClear(GL_COLOR_BUFFER_BIT);
		}
		s_uiKeepRectCount = 0;
		s_uiKeepFull = qfalse;
	}
	qglBlendFuncSeparate(
		GL_SRC_ALPHA,
		GL_ONE_MINUS_SRC_ALPHA,
		GL_ONE,
		GL_ONE_MINUS_SRC_ALPHA
	);
	s_uiFbo.active = qtrue;
	s_uiRectCount = 0;
	s_uiFullResolve = qfalse;
	s_uiRetainable = retainable ? qtrue : qfalse;
	s_uiRetained = kept ? qtrue : qfalse;
	/* Content is rebuilt by this session; valid again only after a retainable End. */
	s_uiContentValid = qfalse;
	tr_uiStats.targetBegins++;
	/* Added in Omaha: GPU timer for UI FBO span (r_uiPerfGpu). */
	RE_UiGpuBeginUi();
	return kept ? 2 : 1;
}

qboolean RE_BeginUI2DTarget(void)
{
	return RE_UI2D_BeginTargetImpl(0, 0) != 0 ? qtrue : qfalse;
}

int RE_BeginUI2DTargetKeep(int keep)
{
	return RE_UI2D_BeginTargetImpl(1, keep);
}

/*
 * Added in Omaha: Phase 4.6 — clear a window-space rect (GL bottom-left) of the
 * active UI target to transparent black; w<=0 or h<=0 clears the whole target.
 * The rect joins the resolve dirty list.
 */
void RE_UI2DClearRectFb(int x, int y, int w, int h)
{
	qboolean savedEnabled;
	int      savedBox[4];

	if (!s_uiFbo.active || !s_uiFbo.msaaFbo) {
		return;
	}
	R_IssuePendingRenderCommands();
	RE_UI2DTargetRebind();
	qglClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	if (w <= 0 || h <= 0) {
		savedEnabled = glState.scissorEnabled;
		GL_ScissorEnable(qfalse);
		qglClear(GL_COLOR_BUFFER_BIT);
		GL_ScissorEnable(savedEnabled);
		s_uiFullResolve = qtrue;
		s_uiKeepRectCount = 0;
		s_uiKeepFull = qfalse;
		return;
	}
	savedEnabled = glState.scissorEnabled;
	memcpy(savedBox, glState.scissorBox, sizeof(savedBox));
	GL_ScissorEnable(qtrue);
	GL_Scissor(x, y, w, h);
	qglClear(GL_COLOR_BUFFER_BIT);
	GL_Scissor(savedBox[0], savedBox[1], savedBox[2], savedBox[3]);
	GL_ScissorEnable(savedEnabled);
	/* Bypass the scissor clamp: the clear used its own scissor. */
	{
		uiRectI_t nr;
		int maxW = s_uiFbo.width;
		int maxH = s_uiFbo.height;
		if (x < 0) {
			w += x;
			x = 0;
		}
		if (y < 0) {
			h += y;
			y = 0;
		}
		if (x + w > maxW) {
			w = maxW - x;
		}
		if (y + h > maxH) {
			h = maxH - y;
		}
		if (w <= 0 || h <= 0 || s_uiFullResolve) {
			return;
		}
		nr.x = x;
		nr.y = y;
		nr.w = w;
		nr.h = h;
		RE_UI2D_RectListAdd(s_uiRects, &s_uiRectCount, &nr);
	}
}

static void RE_UI2D_CompositeRects(const uiRectI_t *rects, int count, int full)
{
	int i;

	qglBegin(GL_QUADS);
	if (full) {
		tr_uiStats.immediateQuads++;
		qglTexCoord2f(0.0f, 1.0f);
		qglVertex2f(0.0f, 0.0f);
		qglTexCoord2f(1.0f, 1.0f);
		qglVertex2f((float)s_uiFbo.width, 0.0f);
		qglTexCoord2f(1.0f, 0.0f);
		qglVertex2f((float)s_uiFbo.width, (float)s_uiFbo.height);
		qglTexCoord2f(0.0f, 0.0f);
		qglVertex2f(0.0f, (float)s_uiFbo.height);
	} else {
		float invW = (s_uiFbo.width > 0) ? (1.0f / (float)s_uiFbo.width) : 0.0f;
		float invH = (s_uiFbo.height > 0) ? (1.0f / (float)s_uiFbo.height) : 0.0f;
		for (i = 0; i < count; i++) {
			const uiRectI_t *r = &rects[i];
			float yTop = (float)(s_uiFbo.height - (r->y + r->h));
			float yBot = (float)(s_uiFbo.height - r->y);
			float s0 = (float)r->x * invW;
			float s1 = (float)(r->x + r->w) * invW;
			float tTop = (float)(r->y + r->h) * invH;
			float tBot = (float)r->y * invH;

			tr_uiStats.immediateQuads++;
			qglTexCoord2f(s0, tTop);
			qglVertex2f((float)r->x, yTop);
			qglTexCoord2f(s1, tTop);
			qglVertex2f((float)(r->x + r->w), yTop);
			qglTexCoord2f(s1, tBot);
			qglVertex2f((float)(r->x + r->w), yBot);
			qglTexCoord2f(s0, tBot);
			qglVertex2f((float)r->x, yBot);
		}
	}
	qglEnd();
}

static void RE_UI2D_LeaveTarget(void)
{
	GL_BindFramebuffer(GL_FRAMEBUFFER, 0);
	/* Fixed in Omaha: clear active before Set2DWindow — its RE_UI2DTargetRebind must not re-bind MSAA FBO during composite. */
	s_uiFbo.active = qfalse;
	/* Changed in Omaha: Phase 1 — match UIR ortho near/far so Set2DWindow dedup can coalesce. */
	Set2DWindow(
		0,
		0,
		glConfig.vidWidth,
		glConfig.vidHeight,
		0.0f,
		(float)glConfig.vidWidth,
		(float)glConfig.vidHeight,
		0.0f,
		-1.0f,
		1.0f
	);
	GL_ScissorEnable(qtrue);
}

void RE_EndUI2DTarget(void)
{
	int rectsMode;
	int blitFull;
	int compositeFull;
	const uiRectI_t *compRects;
	int compCount;
	uiRectI_t blitRects[UI_FBO_MAX_RECTS * 2];
	int blitCount;
	int i;

	if (!s_uiFbo.active) {
		return;
	}

	R_IssuePendingRenderCommands();

	/* Nested TIME_ELAPSED illegal — end ui before resolve (r_uiPerfGpu). */
	RE_UiGpuEndUi();
	RE_UiGpuBeginResolve();

	rectsMode = (r_uiResolveRects && r_uiResolveRects->integer != 0) ? 1 : 0;
	blitFull = (!rectsMode || s_uiFullResolve) ? 1 : 0;

	/*
	 * Phase 4.6: the keep list is the union of everything drawn since the last
	 * clear. Non-retained sessions start it fresh from this frame's rects.
	 */
	blitCount = 0;
	if (blitFull) {
		s_uiKeepFull = qtrue;
		s_uiKeepRectCount = 0;
	} else {
		uiRectI_t prevKeep[UI_FBO_MAX_RECTS];
		const int prevKeepCount = s_uiKeepRectCount;
		int j;

		memcpy(prevKeep, s_uiKeepRects, sizeof(prevKeep));
		for (i = 0; i < s_uiRectCount; i++) {
			RE_UI2D_RectListAdd(s_uiKeepRects, &s_uiKeepRectCount, &s_uiRects[i]);
		}
		/*
		 * The keep list merges into bounding boxes, so a grown or new keep rect
		 * can cover pixels no earlier blit wrote since the last clear. The resolve
		 * texture still holds whatever was there before (a closed scoreboard or
		 * menu), and the composite would show it. Resolve such rects in full.
		 */
		if (!s_uiKeepFull) {
			for (i = 0; i < s_uiKeepRectCount; i++) {
				const uiRectI_t *k = &s_uiKeepRects[i];
				qboolean unchanged = qfalse;
				for (j = 0; j < prevKeepCount; j++) {
					if (prevKeep[j].x == k->x && prevKeep[j].y == k->y && prevKeep[j].w == k->w && prevKeep[j].h == k->h) {
						unchanged = qtrue;
						break;
					}
				}
				if (!unchanged) {
					blitRects[blitCount++] = *k;
				}
			}
		}
		for (i = 0; i < s_uiRectCount; i++) {
			const uiRectI_t *r = &s_uiRects[i];
			qboolean covered = qfalse;
			for (j = 0; j < blitCount; j++) {
				const uiRectI_t *b = &blitRects[j];
				if (r->x >= b->x && r->y >= b->y && r->x + r->w <= b->x + b->w && r->y + r->h <= b->y + b->h) {
					covered = qtrue;
					break;
				}
			}
			if (!covered) {
				blitRects[blitCount++] = *r;
			}
		}
	}
	compositeFull = s_uiKeepFull ? 1 : 0;
	compRects = s_uiKeepRects;
	compCount = s_uiKeepRectCount;

	if (!compositeFull && compCount == 0) {
		/* Nothing drawn (and nothing retained) — skip blit/composite. */
		RE_UI2D_LeaveTarget();
		s_uiContentValid = s_uiRetainable;
		s_uiRetained = qfalse;
		RE_UiGpuEndResolve();
		tr_uiStats.targetEnds++;
		return;
	}

	if (blitFull || blitCount > 0) {
		GL_BindFramebuffer(GL_READ_FRAMEBUFFER, s_uiFbo.msaaFbo);
		GL_BindFramebuffer(GL_DRAW_FRAMEBUFFER, s_uiFbo.resolveFbo);

		if (blitFull) {
			qglBlitFramebuffer(
				0,
				0,
				s_uiFbo.width,
				s_uiFbo.height,
				0,
				0,
				s_uiFbo.width,
				s_uiFbo.height,
				GL_COLOR_BUFFER_BIT,
				GL_NEAREST
			);
			tr_uiStats.resolvePixels += s_uiFbo.width * s_uiFbo.height;
		} else {
			for (i = 0; i < blitCount; i++) {
				const uiRectI_t *r = &blitRects[i];
				qglBlitFramebuffer(
					r->x,
					r->y,
					r->x + r->w,
					r->y + r->h,
					r->x,
					r->y,
					r->x + r->w,
					r->y + r->h,
					GL_COLOR_BUFFER_BIT,
					GL_NEAREST
				);
				tr_uiStats.resolvePixels += r->w * r->h;
			}
		}
	}

	RE_UI2D_LeaveTarget();

	/*
	 * Fixed in Omaha: FBO UI draws use straight-alpha blending into a transparent clear,
	 * so resolve RGB is premultiplied (rgb×α). Composite with ONE, ONE_MINUS_SRC_ALPHA.
	 */
	GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	qglEnable(GL_TEXTURE_2D);
	qglBindTexture(GL_TEXTURE_2D, s_uiFbo.resolveTex);
	/* Fixed in Omaha: composite must not inherit stale backEnd.color2D from prior UI draws. */
	{
		static const byte compositeWhite[4] = {255, 255, 255, 255};

		qglColor4ubv(compositeWhite);
	}

	RE_UI2D_CompositeRects(compRects, compCount, compositeFull);

	/* Added in Omaha: Phase 2 — debug outlines of resolve rects (r_uiResolveRects 2). */
	if (!blitFull && r_uiResolveRects && r_uiResolveRects->integer == 2) {
		qglDisable(GL_TEXTURE_2D);
		GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
		qglColor4f(0.0f, 1.0f, 0.0f, 0.75f);
		qglBegin(GL_LINES);
		for (i = 0; i < s_uiRectCount; i++) {
			uiRectI_t *r = &s_uiRects[i];
			float yTop = (float)(s_uiFbo.height - (r->y + r->h));
			float yBot = (float)(s_uiFbo.height - r->y);
			float x0 = (float)r->x;
			float x1 = (float)(r->x + r->w);
			qglVertex2f(x0, yTop);
			qglVertex2f(x1, yTop);
			qglVertex2f(x1, yTop);
			qglVertex2f(x1, yBot);
			qglVertex2f(x1, yBot);
			qglVertex2f(x0, yBot);
			qglVertex2f(x0, yBot);
			qglVertex2f(x0, yTop);
		}
		qglEnd();
		qglEnable(GL_TEXTURE_2D);
	}

	/* Phase 4.6: only a retainable session leaves a reusable image behind. */
	s_uiContentValid = s_uiRetainable;
	s_uiRetained = qfalse;
	RE_UiGpuEndResolve();
	tr_uiStats.targetEnds++;
}

/* Added in Omaha: Phase 4.6 — did the active session keep the previous image? */
qboolean RE_UI2DTargetRetained(void)
{
	return (s_uiFbo.active && s_uiRetained) ? qtrue : qfalse;
}

