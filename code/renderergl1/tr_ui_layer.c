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
// tr_ui_layer.c -- soft mask-image layer RT for modern UI (GL1)
#include "tr_local.h"

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
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

extern cvar_t *r_uiFramebuffer;

typedef struct {
	qboolean active;
	int      width;
	int      height;
	GLuint   fbo;
	GLuint   colorTex;
	int      scissorX;
	int      scissorY;
	int      scissorW;
	int      scissorH;
	float    uiX;
	float    uiY;
	float    uiW;
	float    uiH;
	GLint    savedScissor[4];
	GLboolean savedScissorEnabled;
} ui_layer_state_t;

static ui_layer_state_t s_uiLayer;

static qboolean RE_UiLayer_FboProcsReady(void)
{
	if (!qglGenFramebuffers || !qglDeleteFramebuffers || !qglBindFramebuffer ||
	    !qglFramebufferTexture2D || !qglCheckFramebufferStatus || !qglBlendFuncSeparate) {
		return qfalse;
	}
	return qtrue;
}

void RE_UiLayerShutdown(void)
{
	if (s_uiLayer.fbo) {
		qglDeleteFramebuffers(1, &s_uiLayer.fbo);
	}
	if (s_uiLayer.colorTex) {
		qglDeleteTextures(1, &s_uiLayer.colorTex);
	}
	memset(&s_uiLayer, 0, sizeof(s_uiLayer));
}

static qboolean RE_UiLayer_Ensure(int width, int height)
{
	if (width <= 0 || height <= 0) {
		return qfalse;
	}

	if (s_uiLayer.fbo && s_uiLayer.width == width && s_uiLayer.height == height) {
		return qtrue;
	}

	RE_UiLayerShutdown();

	s_uiLayer.width = width;
	s_uiLayer.height = height;

	qglGenTextures(1, &s_uiLayer.colorTex);
	qglBindTexture(GL_TEXTURE_2D, s_uiLayer.colorTex);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, haveClampToEdge ? GL_CLAMP_TO_EDGE : GL_CLAMP);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, haveClampToEdge ? GL_CLAMP_TO_EDGE : GL_CLAMP);
	qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	qglGenFramebuffers(1, &s_uiLayer.fbo);
	qglBindFramebuffer(GL_FRAMEBUFFER, s_uiLayer.fbo);
	qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_uiLayer.colorTex, 0);
	if (qglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		RE_UiLayerShutdown();
		return qfalse;
	}

	qglBindFramebuffer(GL_FRAMEBUFFER, 0);
	return qtrue;
}

qboolean RE_UiLayerIsActive(void)
{
	return s_uiLayer.active;
}

void RE_UiLayerRebind(void)
{
	if (!s_uiLayer.active || !s_uiLayer.fbo) {
		return;
	}
	qglBindFramebuffer(GL_FRAMEBUFFER, s_uiLayer.fbo);
	qglViewport(0, 0, s_uiLayer.width, s_uiLayer.height);
}

qboolean RE_UiLayerAvailable(void)
{
	if (!r_uiFramebuffer || !r_uiFramebuffer->integer) {
		return qfalse;
	}
	if (!RE_UI2DTargetIsActive()) {
		return qfalse;
	}
	return RE_UiLayer_FboProcsReady();
}

qboolean RE_BeginUiLayer(int fbX, int fbY, int fbW, int fbH, float uiX, float uiY, float uiW, float uiH)
{
	if (!RE_UiLayerAvailable()) {
		return qfalse;
	}
	if (s_uiLayer.active) {
		return qfalse;
	}
	if (fbW <= 0 || fbH <= 0 || !(uiW > 0.0f) || !(uiH > 0.0f)) {
		return qfalse;
	}

	R_IssuePendingRenderCommands();

	if (!RE_UiLayer_Ensure(glConfig.vidWidth, glConfig.vidHeight)) {
		return qfalse;
	}

	s_uiLayer.savedScissorEnabled = qglIsEnabled(GL_SCISSOR_TEST);
	qglGetIntegerv(GL_SCISSOR_BOX, s_uiLayer.savedScissor);

	s_uiLayer.scissorX = fbX;
	s_uiLayer.scissorY = fbY;
	s_uiLayer.scissorW = fbW;
	s_uiLayer.scissorH = fbH;
	s_uiLayer.uiX = uiX;
	s_uiLayer.uiY = uiY;
	s_uiLayer.uiW = uiW;
	s_uiLayer.uiH = uiH;

	qglBindFramebuffer(GL_FRAMEBUFFER, s_uiLayer.fbo);
	qglViewport(0, 0, s_uiLayer.width, s_uiLayer.height);
	qglEnable(GL_SCISSOR_TEST);
	qglScissor(fbX, fbY, fbW, fbH);
	qglClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	qglClear(GL_COLOR_BUFFER_BIT);
	qglBlendFuncSeparate(
		GL_SRC_ALPHA,
		GL_ONE_MINUS_SRC_ALPHA,
		GL_ONE,
		GL_ONE_MINUS_SRC_ALPHA
	);

	s_uiLayer.active = qtrue;
	return qtrue;
}

void RE_UiLayerApplyMask(qhandle_t hShader, float x, float y, float w, float h, float s1, float t1, float s2, float t2)
{
	shader_t *shader;
	image_t  *image;

	if (!s_uiLayer.active || !(w > 0.0f) || !(h > 0.0f)) {
		return;
	}

	R_IssuePendingRenderCommands();
	RE_UiLayerRebind();

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	} else {
		shader = tr.defaultShader;
	}
	if (!shader || !shader->unfoggedStages[0] || !shader->unfoggedStages[0]->bundle[0].image[0]) {
		return;
	}
	image = shader->unfoggedStages[0]->bundle[0].image[0];

	qglEnable(GL_SCISSOR_TEST);
	qglScissor(s_uiLayer.scissorX, s_uiLayer.scissorY, s_uiLayer.scissorW, s_uiLayer.scissorH);
	qglEnable(GL_BLEND);
	qglBlendFuncSeparate(GL_ZERO, GL_SRC_ALPHA, GL_ZERO, GL_SRC_ALPHA);
	qglDisable(GL_DEPTH_TEST);
	qglEnable(GL_TEXTURE_2D);
	GL_Bind(image);
	{
		static const byte white[4] = {255, 255, 255, 255};
		qglColor4ubv(white);
	}

	/*
	 * Dest-in: dst.rgb *= mask.a; dst.a *= mask.a.
	 * Premultiplied layer content scales coverage for soft mask edges.
	 */
	qglBegin(GL_QUADS);
	qglTexCoord2f(s1, t1);
	qglVertex2f(x, y);
	qglTexCoord2f(s2, t1);
	qglVertex2f(x + w, y);
	qglTexCoord2f(s2, t2);
	qglVertex2f(x + w, y + h);
	qglTexCoord2f(s1, t2);
	qglVertex2f(x, y + h);
	qglEnd();

	qglBlendFuncSeparate(
		GL_SRC_ALPHA,
		GL_ONE_MINUS_SRC_ALPHA,
		GL_ONE,
		GL_ONE_MINUS_SRC_ALPHA
	);
}

void RE_EndUiLayer(void)
{
	float x;
	float y;
	float w;
	float h;
	float u0;
	float u1;
	float tTop;
	float tBot;
	float invW;
	float invH;

	if (!s_uiLayer.active) {
		return;
	}

	R_IssuePendingRenderCommands();

	x = s_uiLayer.uiX;
	y = s_uiLayer.uiY;
	w = s_uiLayer.uiW;
	h = s_uiLayer.uiH;
	invW = (s_uiLayer.width > 0) ? (1.0f / (float)s_uiLayer.width) : 0.0f;
	invH = (s_uiLayer.height > 0) ? (1.0f / (float)s_uiLayer.height) : 0.0f;
	u0 = x * invW;
	u1 = (x + w) * invW;
	/* UI y=0 at top; GL texture t=0 at bottom. */
	tTop = 1.0f - (y * invH);
	tBot = 1.0f - ((y + h) * invH);

	s_uiLayer.active = qfalse;

	/* Rebind main UI MSAA target for composite. */
	if (RE_UI2DTargetIsActive()) {
		RE_UI2DTargetRebind();
	}

	qglEnable(GL_SCISSOR_TEST);
	qglScissor(s_uiLayer.scissorX, s_uiLayer.scissorY, s_uiLayer.scissorW, s_uiLayer.scissorH);
	GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	qglEnable(GL_TEXTURE_2D);
	qglBindTexture(GL_TEXTURE_2D, s_uiLayer.colorTex);
	{
		static const byte compositeWhite[4] = {255, 255, 255, 255};
		qglColor4ubv(compositeWhite);
	}

	qglBegin(GL_QUADS);
	qglTexCoord2f(u0, tTop);
	qglVertex2f(x, y);
	qglTexCoord2f(u1, tTop);
	qglVertex2f(x + w, y);
	qglTexCoord2f(u1, tBot);
	qglVertex2f(x + w, y + h);
	qglTexCoord2f(u0, tBot);
	qglVertex2f(x, y + h);
	qglEnd();

	if (s_uiLayer.savedScissorEnabled) {
		qglEnable(GL_SCISSOR_TEST);
	} else {
		qglDisable(GL_SCISSOR_TEST);
	}
	qglScissor(
		s_uiLayer.savedScissor[0],
		s_uiLayer.savedScissor[1],
		s_uiLayer.savedScissor[2],
		s_uiLayer.savedScissor[3]
	);

	/*
	 * Fixed in OPM: GL_State uses glBlendFunc and would clobber the UI FBO's
	 * BlendFuncSeparate(alpha=ONE). Apply GL_State first, then restore separate.
	 */
	GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	qglBlendFuncSeparate(
		GL_SRC_ALPHA,
		GL_ONE_MINUS_SRC_ALPHA,
		GL_ONE,
		GL_ONE_MINUS_SRC_ALPHA
	);
}

/* -------------------------------------------------------------------------- */
/* Added in OPM: retained chrome cache RT (idle-frame blit).                  */
/* -------------------------------------------------------------------------- */

typedef struct {
	qboolean active;   /* capturing into FBO */
	qboolean valid;    /* texture has a complete chrome frame */
	int      width;
	int      height;
	GLuint   fbo;
	GLuint   colorTex;
	float    uiX;
	float    uiY;
	float    uiW;
	float    uiH;
} ui_chrome_cache_t;

static ui_chrome_cache_t s_uiChromeCache;

void RE_UiChromeCacheShutdown(void)
{
	if (s_uiChromeCache.fbo) {
		qglDeleteFramebuffers(1, &s_uiChromeCache.fbo);
	}
	if (s_uiChromeCache.colorTex) {
		qglDeleteTextures(1, &s_uiChromeCache.colorTex);
	}
	memset(&s_uiChromeCache, 0, sizeof(s_uiChromeCache));
}

static qboolean RE_UiChromeCache_Ensure(int width, int height)
{
	if (width <= 0 || height <= 0) {
		return qfalse;
	}
	if (s_uiChromeCache.fbo && s_uiChromeCache.width == width && s_uiChromeCache.height == height) {
		return qtrue;
	}

	RE_UiChromeCacheShutdown();
	s_uiChromeCache.width = width;
	s_uiChromeCache.height = height;

	qglGenTextures(1, &s_uiChromeCache.colorTex);
	qglBindTexture(GL_TEXTURE_2D, s_uiChromeCache.colorTex);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, haveClampToEdge ? GL_CLAMP_TO_EDGE : GL_CLAMP);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, haveClampToEdge ? GL_CLAMP_TO_EDGE : GL_CLAMP);
	qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	qglGenFramebuffers(1, &s_uiChromeCache.fbo);
	qglBindFramebuffer(GL_FRAMEBUFFER, s_uiChromeCache.fbo);
	qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_uiChromeCache.colorTex, 0);
	if (qglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		RE_UiChromeCacheShutdown();
		return qfalse;
	}
	qglBindFramebuffer(GL_FRAMEBUFFER, 0);
	return qtrue;
}

qboolean RE_UiChromeCacheIsActive(void)
{
	return s_uiChromeCache.active;
}

void RE_UiChromeCacheRebind(void)
{
	if (!s_uiChromeCache.active || !s_uiChromeCache.fbo) {
		return;
	}
	qglBindFramebuffer(GL_FRAMEBUFFER, s_uiChromeCache.fbo);
	qglViewport(0, 0, s_uiChromeCache.width, s_uiChromeCache.height);
}

qboolean RE_UiChromeCacheAvailable(void)
{
	if (!r_uiFramebuffer || !r_uiFramebuffer->integer) {
		return qfalse;
	}
	if (!RE_UI2DTargetIsActive()) {
		return qfalse;
	}
	return RE_UiLayer_FboProcsReady();
}

void RE_InvalidateUiChromeCache(void)
{
	s_uiChromeCache.valid = qfalse;
}

qboolean RE_BeginUiChromeCacheCapture(float uiX, float uiY, float uiW, float uiH)
{
	if (!RE_UiChromeCacheAvailable()) {
		return qfalse;
	}
	if (s_uiChromeCache.active || s_uiLayer.active) {
		return qfalse;
	}
	if (!(uiW > 0.0f) || !(uiH > 0.0f)) {
		return qfalse;
	}

	R_IssuePendingRenderCommands();

	if (!RE_UiChromeCache_Ensure(glConfig.vidWidth, glConfig.vidHeight)) {
		return qfalse;
	}

	s_uiChromeCache.uiX = uiX;
	s_uiChromeCache.uiY = uiY;
	s_uiChromeCache.uiW = uiW;
	s_uiChromeCache.uiH = uiH;
	s_uiChromeCache.valid = qfalse;

	qglBindFramebuffer(GL_FRAMEBUFFER, s_uiChromeCache.fbo);
	qglViewport(0, 0, s_uiChromeCache.width, s_uiChromeCache.height);
	qglDisable(GL_SCISSOR_TEST);
	qglClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	qglClear(GL_COLOR_BUFFER_BIT);
	qglBlendFuncSeparate(
		GL_SRC_ALPHA,
		GL_ONE_MINUS_SRC_ALPHA,
		GL_ONE,
		GL_ONE_MINUS_SRC_ALPHA
	);

	s_uiChromeCache.active = qtrue;
	return qtrue;
}

void RE_EndUiChromeCacheCapture(void)
{
	if (!s_uiChromeCache.active) {
		return;
	}

	R_IssuePendingRenderCommands();
	s_uiChromeCache.active = qfalse;
	s_uiChromeCache.valid = qtrue;

	if (RE_UI2DTargetIsActive()) {
		RE_UI2DTargetRebind();
	}
}

void RE_BlitUiChromeCache(void)
{
	float x, y, w, h;
	float u0, u1, tTop, tBot;
	float invW, invH;

	if (!s_uiChromeCache.valid || !s_uiChromeCache.colorTex) {
		return;
	}
	if (s_uiChromeCache.active) {
		return;
	}

	R_IssuePendingRenderCommands();

	if (RE_UI2DTargetIsActive()) {
		RE_UI2DTargetRebind();
	}

	x = s_uiChromeCache.uiX;
	y = s_uiChromeCache.uiY;
	w = s_uiChromeCache.uiW;
	h = s_uiChromeCache.uiH;
	invW = (s_uiChromeCache.width > 0) ? (1.0f / (float)s_uiChromeCache.width) : 0.0f;
	invH = (s_uiChromeCache.height > 0) ? (1.0f / (float)s_uiChromeCache.height) : 0.0f;
	u0 = x * invW;
	u1 = (x + w) * invW;
	tTop = 1.0f - (y * invH);
	tBot = 1.0f - ((y + h) * invH);

	GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	qglEnable(GL_TEXTURE_2D);
	qglBindTexture(GL_TEXTURE_2D, s_uiChromeCache.colorTex);
	{
		static const byte white[4] = {255, 255, 255, 255};
		qglColor4ubv(white);
	}

	qglBegin(GL_QUADS);
	qglTexCoord2f(u0, tTop);
	qglVertex2f(x, y);
	qglTexCoord2f(u1, tTop);
	qglVertex2f(x + w, y);
	qglTexCoord2f(u1, tBot);
	qglVertex2f(x + w, y + h);
	qglTexCoord2f(u0, tBot);
	qglVertex2f(x, y + h);
	qglEnd();

	GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	qglBlendFuncSeparate(
		GL_SRC_ALPHA,
		GL_ONE_MINUS_SRC_ALPHA,
		GL_ONE,
		GL_ONE_MINUS_SRC_ALPHA
	);
}
