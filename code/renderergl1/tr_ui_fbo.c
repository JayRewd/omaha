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

typedef struct {
	qboolean active;
	int      width;
	int      height;
	int      samples;
	GLuint   msaaFbo;
	GLuint   msaaColorRb;
	GLuint   msaaDepthRb;
	GLuint   resolveFbo;
	GLuint   resolveTex;
} ui_fbo_state_t;

static ui_fbo_state_t s_uiFbo;

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
	/* Added in OPM: tear down soft mask-image layer with the main UI FBO. */
	RE_UiLayerShutdown();
	/* Added in OPM: tear down chrome cache RT with the main UI FBO. */
	RE_UiChromeCacheShutdown();

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
	memset(&s_uiFbo, 0, sizeof(s_uiFbo));
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
	qglBindFramebuffer(GL_FRAMEBUFFER, s_uiFbo.msaaFbo);

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

	qglGenTextures(1, &s_uiFbo.resolveTex);
	qglBindTexture(GL_TEXTURE_2D, s_uiFbo.resolveTex);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, haveClampToEdge ? GL_CLAMP_TO_EDGE : GL_CLAMP);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, haveClampToEdge ? GL_CLAMP_TO_EDGE : GL_CLAMP);
	qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	qglGenFramebuffers(1, &s_uiFbo.resolveFbo);
	qglBindFramebuffer(GL_FRAMEBUFFER, s_uiFbo.resolveFbo);
	qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_uiFbo.resolveTex, 0);
	if (qglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		RE_UI2D_FboShutdown();
		return qfalse;
	}

	qglBindFramebuffer(GL_FRAMEBUFFER, 0);
	return qtrue;
}

qboolean RE_UI2DTargetIsActive(void)
{
	return s_uiFbo.active;
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
	/* Added in OPM: soft mask-image layer is the top of the UI target stack. */
	if (RE_UiLayerIsActive()) {
		RE_UiLayerRebind();
		return;
	}
	/* Added in OPM: chrome cache capture sits under the soft-mask layer. */
	if (RE_UiChromeCacheIsActive()) {
		RE_UiChromeCacheRebind();
		return;
	}
	if (s_uiFbo.active && s_uiFbo.msaaFbo) {
		qglBindFramebuffer(GL_FRAMEBUFFER, s_uiFbo.msaaFbo);
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

qboolean RE_BeginUI2DTarget(void)
{
	int samples;

	if (!RE_UI2DTargetAvailable()) {
		return qfalse;
	}

	R_IssuePendingRenderCommands();

	samples = RE_UI2D_FboClampSamples(r_uiMultisample ? r_uiMultisample->integer : 0);
	if (!RE_UI2D_FboEnsure(glConfig.vidWidth, glConfig.vidHeight, samples)) {
		return qfalse;
	}

	qglBindFramebuffer(GL_FRAMEBUFFER, s_uiFbo.msaaFbo);
	qglViewport(0, 0, s_uiFbo.width, s_uiFbo.height);
	qglDisable(GL_SCISSOR_TEST);
	qglClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	qglClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	qglBlendFuncSeparate(
		GL_SRC_ALPHA,
		GL_ONE_MINUS_SRC_ALPHA,
		GL_ONE,
		GL_ONE_MINUS_SRC_ALPHA
	);
	s_uiFbo.active = qtrue;
	return qtrue;
}

void RE_EndUI2DTarget(void)
{
	if (!s_uiFbo.active) {
		return;
	}

	R_IssuePendingRenderCommands();

	qglBindFramebuffer(GL_READ_FRAMEBUFFER, s_uiFbo.msaaFbo);
	qglBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_uiFbo.resolveFbo);
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

	qglBindFramebuffer(GL_FRAMEBUFFER, 0);
	/* Fixed in OPM: clear active before Set2DWindow — its RE_UI2DTargetRebind must not re-bind MSAA FBO during composite. */
	s_uiFbo.active = qfalse;
	Set2DWindow(
		0,
		0,
		glConfig.vidWidth,
		glConfig.vidHeight,
		0.0f,
		(float)glConfig.vidWidth,
		(float)glConfig.vidHeight,
		0.0f,
		0.0f,
		1.0f
	);
	qglEnable(GL_SCISSOR_TEST);

	/*
	 * Fixed in OPM: FBO UI draws use straight-alpha blending into a transparent clear,
	 * so resolve RGB is premultiplied (rgb×α). Composite with ONE, ONE_MINUS_SRC_ALPHA.
	 */
	GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	qglEnable(GL_TEXTURE_2D);
	qglBindTexture(GL_TEXTURE_2D, s_uiFbo.resolveTex);
	/* Fixed in OPM: composite must not inherit stale backEnd.color2D from prior UI draws. */
	{
		static const byte compositeWhite[4] = {255, 255, 255, 255};

		qglColor4ubv(compositeWhite);
	}
	/*
	 * FBO color is stored bottom-up in GL texture space; UI draw space is top-down
	 * (orthoT=0 at top). Match DrawStretchPic: t=0 at screen top, t=1 at bottom.
	 */
	qglBegin(GL_QUADS);
	qglTexCoord2f(0.0f, 1.0f);
	qglVertex2f(0.0f, 0.0f);
	qglTexCoord2f(1.0f, 1.0f);
	qglVertex2f((float)s_uiFbo.width, 0.0f);
	qglTexCoord2f(1.0f, 0.0f);
	qglVertex2f((float)s_uiFbo.width, (float)s_uiFbo.height);
	qglTexCoord2f(0.0f, 0.0f);
	qglVertex2f(0.0f, (float)s_uiFbo.height);
	qglEnd();
}
