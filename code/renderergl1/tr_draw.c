/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_draw.c -- drawing

#include "tr_local.h"

vec4_t r_colorWhite = { 1.0, 1.0, 1.0, 1.0 };

/*
================
Draw_SetColor
================
*/
void Draw_SetColor(const vec4_t rgba) {
#if 1
	if (!rgba) {
		rgba = r_colorWhite;
	}

	backEnd.color2D[0] = (byte)(rgba[0] * tr.identityLightByte);
	backEnd.color2D[1] = (byte)(rgba[1] * tr.identityLightByte);
	backEnd.color2D[2] = (byte)(rgba[2] * tr.identityLightByte);
	backEnd.color2D[3] = (byte)(rgba[3] * 255.0);
	qglColor4ubv(backEnd.color2D);
#else
	RE_SetColor(rgba);
#endif
}

/*
================
Draw_StretchPic
================
*/
void Draw_StretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader) {
#if 1
	shader_t* shader;

	R_IssuePendingRenderCommands();
	RE_UI2DTargetRebind();
	if (RE_UI2DTargetIsActive()) {
		/* Added in Omaha: Phase 2 — host/legacy FBO draws force full resolve. */
		RE_UI2D_MarkFullResolve();
	}

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	if (w <= 0) {
		w = shader->unfoggedStages[0]->bundle[0].image[0]->width;
		h = shader->unfoggedStages[0]->bundle[0].image[0]->height;
	}

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);
	RB_BeginSurface(shader);

	RB_Texcoord2f(s1, t1);
	RB_Vertex2f(x, y);

	RB_Texcoord2f(s2, t1);
	RB_Vertex2f(x + w, y);

	RB_Texcoord2f(s1, t2);
	RB_Vertex2f(x, y + h);

	RB_Texcoord2f(s2, t2);
	RB_Vertex2f(x + w, y + h);

	RB_StreamEnd();
#else
	RE_StretchPic(x, y, w, h, s1, t1, s2, t2, hShader);
#endif
}

/*
================
Draw_StretchPic2
================
*/
void Draw_StretchPic2(float x, float y, float w, float h, float s1, float t1, float s2, float t2, float sx, float sy, qhandle_t hShader) {
	shader_t* shader;
	float halfWidth, halfHeight;
	float scaledWidth1, scaledHeight1;
	float scaledWidth2, scaledHeight2;

	R_IssuePendingRenderCommands();
	RE_UI2DTargetRebind();
	if (RE_UI2DTargetIsActive()) {
		/* Added in Omaha: Phase 2 — host/legacy FBO draws force full resolve. */
		RE_UI2D_MarkFullResolve();
	}

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	if (w <= 0) {
		w = shader->unfoggedStages[0]->bundle[0].image[0]->width;
		h = shader->unfoggedStages[0]->bundle[0].image[0]->height;
	}

	halfWidth = w * 0.5f;
	halfHeight = h * 0.5f;
	scaledWidth1 = halfWidth * sy;
	scaledHeight1 = halfHeight * sx;
	scaledWidth2 = halfWidth * sx;
	scaledHeight2 = halfHeight * sy;

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);
	RB_BeginSurface(shader);

	RB_Texcoord2f(s1, t1);
	RB_Vertex3f(x + halfWidth - (scaledWidth2 + -scaledHeight2), y + halfWidth - scaledHeight1 - scaledWidth1, 0);

	RB_Texcoord2f(t2, t1);
	RB_Vertex3f(scaledWidth2 - -scaledHeight2 + x + halfWidth, scaledWidth1 - scaledHeight1 + y + halfWidth, 0);

	RB_Texcoord2f(s1, s2);
	RB_Vertex3f(x+ halfWidth - (scaledWidth2 + scaledHeight2), scaledHeight1 - scaledWidth1 + y + halfWidth, 0);

	RB_Texcoord2f(t2, s2);
	RB_Vertex3f(scaledWidth2 - scaledHeight2 + x + halfWidth, scaledWidth1 + scaledHeight1 + y + halfWidth, 0);

	RB_StreamEnd();
}


/*
================
Draw_TilePic
================
*/
void Draw_TilePic(float x, float y, float w, float h, qhandle_t hShader) {
	shader_t* shader;
	float		picw, pich;

	R_IssuePendingRenderCommands();
	RE_UI2DTargetRebind();
	if (RE_UI2DTargetIsActive()) {
		/* Added in Omaha: Phase 2 — host/legacy FBO draws force full resolve. */
		RE_UI2D_MarkFullResolve();
	}

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	if (w <= 0) {
		w = shader->unfoggedStages[0]->bundle[0].image[0]->width;
		h = shader->unfoggedStages[0]->bundle[0].image[0]->height;
	}

	picw = shader->unfoggedStages[0]->bundle[0].image[0]->uploadWidth;
	pich = shader->unfoggedStages[0]->bundle[0].image[0]->uploadHeight;

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);

	RB_StreamBegin(shader);

	RB_Texcoord2f(x / picw, y / pich);
	RB_Vertex2f(x, y);

	RB_Texcoord2f((x + w) / picw, y / pich);
	RB_Vertex2f(x + w, y);

	RB_Texcoord2f(x / picw, (y + h) / pich);
	RB_Vertex2f(x, y + h);

	RB_Texcoord2f((x + w) / picw, (y + h) / pich);
	RB_Vertex2f(x + w, y + h);

	RB_StreamEnd();
}

/*
================
Draw_TilePicOffset
================
*/
void Draw_TilePicOffset(float x, float y, float w, float h, qhandle_t hShader, int offsetX, int offsetY) {
	shader_t* shader;
	float		picw, pich;

	R_IssuePendingRenderCommands();
	RE_UI2DTargetRebind();
	if (RE_UI2DTargetIsActive()) {
		/* Added in Omaha: Phase 2 — host/legacy FBO draws force full resolve. */
		RE_UI2D_MarkFullResolve();
	}

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	if (w <= 0) {
		w = shader->unfoggedStages[0]->bundle[0].image[0]->width;
		h = shader->unfoggedStages[0]->bundle[0].image[0]->height;
	}

	picw = shader->unfoggedStages[0]->bundle[0].image[0]->uploadWidth;
	pich = shader->unfoggedStages[0]->bundle[0].image[0]->uploadHeight;

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);

	RB_StreamBegin(shader);

	RB_Texcoord2f(x / picw, y / pich);
	RB_Vertex2f(x + offsetX, y + offsetY);

	RB_Texcoord2f((x + w) / picw, y / pich);
	RB_Vertex2f(x + offsetX + w, y + offsetY);

	RB_Texcoord2f(x / picw, (y + h) / pich);
	RB_Vertex2f(x + offsetX, y + offsetY + h);

	RB_Texcoord2f((x + w) / picw, (y + h) / pich);
	RB_Vertex2f(x + offsetX + w, y + offsetY + h);

	RB_StreamEnd();
}

/*
================
Draw_TrianglePic
================
*/
void Draw_TrianglePic(const vec2_t vPoints[3], const vec2_t vTexCoords[3], qhandle_t hShader) {
	int			i;
	shader_t* shader;

	R_IssuePendingRenderCommands();
	RE_UI2DTargetRebind();
	if (RE_UI2DTargetIsActive()) {
		/* Added in Omaha: Phase 2 — host/legacy FBO draws force full resolve. */
		RE_UI2D_MarkFullResolve();
	}

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);

	RB_BeginSurface(shader);

	for (i = 0; i < 3; i++) {
		RB_Texcoord2f(vTexCoords[i][0], vTexCoords[i][1]);
		RB_Vertex2f(vPoints[i][0], vPoints[i][1]);
	}

	RB_StreamEnd();
}

/*
================
RE_DrawBackground_TexSubImage
================
*/
void RE_DrawBackground_TexSubImage(int cols, int rows, int bgr, byte* data) {
	GLenum	format;
	int		w, h;

	w = glConfig.vidWidth;
	h = glConfig.vidHeight;

	R_IssuePendingRenderCommands();
	RE_UI2DTargetRebind();
	if (RE_UI2DTargetIsActive()) {
		/* Added in Omaha: Phase 2 — host/legacy FBO draws force full resolve. */
		RE_UI2D_MarkFullResolve();
	}
	qglFinish();

	if (bgr) {
		format = GL_BGR_EXT;
	}
	else {
		format = GL_RGB;
	}

	GL_Bind(tr.scratchImage);

	if (cols == tr.scratchImage->width && rows == tr.scratchImage->height && format == tr.scratchImage->internalFormat)
	{
		qglTexSubImage2D(3553, 0, 0, 0, cols, rows, format, 5121, data);
	}
	else
	{
		tr.scratchImage->uploadWidth = cols;
		tr.scratchImage->uploadHeight = rows;
		tr.scratchImage->internalFormat = format;
		qglTexImage2D(GL_TEXTURE_2D, 0, 3, cols, rows, 0, format, 5121, data);
		qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 9729.0);
		qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, 9729.0);
	}

	qglDisable(GL_CULL_FACE);
	qglDisable(GL_DEPTH_TEST);
	qglEnable(GL_TEXTURE_2D);

	qglBegin(GL_QUADS);

	qglTexCoord2f(0.5 / (GLfloat)cols, ((GLfloat)rows - 0.5) / rows);
	qglVertex2f(0, 0);

	qglTexCoord2f(((GLfloat)cols - 0.5) / cols, ((GLfloat)rows - 0.5) / rows);
	qglVertex2f(w, 0);

	qglTexCoord2f(((GLfloat)cols - 0.5) / cols, 0.5 / (GLfloat)rows);
	qglVertex2f(w, h);

	qglTexCoord2f(0.5 / (GLfloat)rows, 0.5 / (GLfloat)rows);
	qglVertex2f(0, h);

	qglEnd();
}

/*
================
RE_DrawBackground_DrawPixels
================
*/
void RE_DrawBackground_DrawPixels(int cols, int rows, int bgr, byte* data) {
	R_IssuePendingRenderCommands();
	RE_UI2DTargetRebind();
	if (RE_UI2DTargetIsActive()) {
		/* Added in Omaha: Phase 2 — host/legacy FBO draws force full resolve. */
		RE_UI2D_MarkFullResolve();
	}

	GL_State(0);
	qglDisable(GL_TEXTURE_2D);

	qglPixelZoom(glConfig.vidWidth / rows, glConfig.vidHeight / cols);

	if (bgr) {
		qglDrawPixels(cols, rows, GL_BGR, GL_UNSIGNED_BYTE, data);
	} else {
		qglDrawPixels(cols, rows, GL_RGB, GL_UNSIGNED_BYTE, data);
	}

	qglPixelZoom(1.0, 1.0);

	qglEnable(GL_TEXTURE_2D);
}

/*
================
AddBox
================
*/
void AddBox(float x, float y, float w, float h) {
	R_IssuePendingRenderCommands();
	RE_UI2DTargetRebind();
	if (RE_UI2DTargetIsActive()) {
		/* Added in Omaha: Phase 2 — host/legacy FBO draws force full resolve. */
		RE_UI2D_MarkFullResolve();
	}

	qglColor4ubv(backEnd.color2D);
	qglDisable(GL_TEXTURE_2D);
	GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

	qglBegin(GL_QUADS);

	qglVertex2f(x, y);
	qglVertex2f(x + w, y);
	qglVertex2f(x + w, y + h);
	qglVertex2f(x, y + h);

	qglEnd();

	qglEnable(GL_TEXTURE_2D);
}

/*
================
DrawBox
================
*/
void DrawBox(float x, float y, float w, float h) {
	qboolean msaaWasEnabled = qfalse;

	R_IssuePendingRenderCommands();
	RE_UI2DTargetRebind();
	if (RE_UI2DTargetIsActive()) {
		/* Added in Omaha: Phase 2 — host/legacy FBO draws force full resolve. */
		RE_UI2D_MarkFullResolve();
	}

#ifdef GL_MULTISAMPLE
	/* Changed in Omaha: track MSAA via glState; r_uiSyncQueries restores old glIsEnabled. */
	if (r_uiSyncQueries && r_uiSyncQueries->integer) {
		msaaWasEnabled = qglIsEnabled(GL_MULTISAMPLE) ? qtrue : qfalse;
		tr_uiStats.glQueries++;
	} else {
		msaaWasEnabled = glState.multisampleEnabled;
	}
	if (msaaWasEnabled) {
		GL_MultisampleEnable(qfalse);
	}
#endif

	qglColor4ubv(backEnd.color2D);
	qglDisable(GL_TEXTURE_2D);
	GL_State(GLS_DEPTHTEST_DISABLE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_SRCBLEND_SRC_ALPHA);
	/* After GL_State — keep stencil mask write from painting white. */
	RE_UiStencilReassertMaskWrite();

	qglBegin(GL_QUADS);
	tr_uiStats.immediateQuads++;

	qglVertex2f(x, y);
	qglVertex2f(x + w, y);
	qglVertex2f(x + w, y + h);
	qglVertex2f(x, y + h);

	qglEnd();

	qglEnable(GL_TEXTURE_2D);

#ifdef GL_MULTISAMPLE
	if (msaaWasEnabled) {
		GL_MultisampleEnable(qtrue);
	}
#endif
}

/*
================
DrawLineLoop
================
*/
void DrawLineLoop(const vec2_t* points, int count, int stipple_factor, int stipple_mask) {
	int		i;

	R_IssuePendingRenderCommands();
	RE_UI2DTargetRebind();
	if (RE_UI2DTargetIsActive()) {
		/* Added in Omaha: Phase 2 — host/legacy FBO draws force full resolve. */
		RE_UI2D_MarkFullResolve();
	}

	qglDisable(GL_TEXTURE_2D);

	if (stipple_factor) {
		qglEnable(GL_LINE_STIPPLE);
		qglLineStipple(stipple_factor, stipple_mask);
	}

	qglBegin(GL_LINE_LOOP);

	for (i = 0; i < count; i++) {
		qglVertex2f(points[i][0], points[i][1]);
	}

	qglEnd();

	qglEnable(GL_TEXTURE_2D);

	if (stipple_factor) {
		qglDisable(GL_LINE_STIPPLE);
	}
}

/*
================
Set2DWindow
================
*/
/*
================
Set2DWindow
================
*/
/* Added in Omaha: Phase 1 — software dedup for Set2DWindow issued calls. */
static int   s_set2dAppliedValid;
static int   s_set2dAX, s_set2dAY, s_set2dAW, s_set2dAH;
static float s_set2dAL, s_set2dAR, s_set2dAB, s_set2dAT, s_set2dAN, s_set2dAF;

void RE_InvalidateSet2DWindow(void)
{
	s_set2dAppliedValid = 0;
}

void Set2DWindow(int x, int y, int w, int h, float left, float right, float bottom, float top, float n, float f) {
	/* Added in Omaha: Phase 1 — skip identical Set2DWindow after 3D invalidates tracking. */
	if (!backEnd.in2D) {
		s_set2dAppliedValid = 0;
	}
	if (s_set2dAppliedValid && backEnd.in2D && s_set2dAX == x && s_set2dAY == y && s_set2dAW == w && s_set2dAH == h
		&& s_set2dAL == left && s_set2dAR == right && s_set2dAB == bottom && s_set2dAT == top
		&& s_set2dAN == n && s_set2dAF == f) {
		return;
	}

	/* Added in Omaha: ui_perf_hud counter. */
	tr_uiStats.set2DWindow++;
	R_IssuePendingRenderCommands();
	RE_UI2DTargetRebind();
	if (RE_UI2DTargetIsActive()) {
		/* Added in Omaha: Phase 2 — host/legacy FBO draws force full resolve. */
		RE_UI2D_MarkFullResolve();
	}
	qglViewport(x, y, w, h);
	/* Changed in Omaha: route through tracked scissor wrapper. */
	{
		const int prevSite = re_uiScissorSite;
		re_uiScissorSite = RE_UI_SCISSOR_SET2D;
		GL_Scissor(x, y, w, h);
		re_uiScissorSite = prevSite;
	}
	qglMatrixMode(GL_PROJECTION);
	qglLoadIdentity();
	qglOrtho(left, right, bottom, top, n, f);
	qglMatrixMode(GL_MODELVIEW);

	qglLoadIdentity();
	GL_State(GLS_DEPTHTEST_DISABLE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_SRCBLEND_SRC_ALPHA);
	qglEnable(GL_BLEND);
	qglDisable(GL_CULL_FACE);
	qglDisable(GL_CLIP_PLANE0);
	// Make sure to disable the fog to avoid messing up with the UI
	qglDisable(GL_FOG);
	qglFogf(GL_FOG_START, 0);

	if (r_reset_tc_array->integer) {
		qglDisableClientState(GL_TEXTURE_COORD_ARRAY);
	}

	if (!backEnd.in2D)
	{
		backEnd.refdef.time = ri.Milliseconds();
		backEnd.in2D = qtrue;
		backEnd.refdef.floatTime = backEnd.refdef.time / 1000.0;
        backEnd.shaderStartTime = 0; 
	}
	s_set2dAX = x;
	s_set2dAY = y;
	s_set2dAW = w;
	s_set2dAH = h;
	s_set2dAL = left;
	s_set2dAR = right;
	s_set2dAB = bottom;
	s_set2dAT = top;
	s_set2dAN = n;
	s_set2dAF = f;
	s_set2dAppliedValid = 1;
	/* Added in Omaha: Phase 2 — feed draw→window conversion for dirty rects. */
	RE_UI2D_NoteWin2D(x, y, w, h, left, right, bottom, top);
}

/*
================
RE_Scissor
================
*/
void RE_Scissor(int x, int y, int width, int height) {
	/* Changed in Omaha: route through tracked scissor wrappers. */
	const int prevSite = re_uiScissorSite;
	re_uiScissorSite = RE_UI_SCISSOR_CLIP;
	GL_ScissorEnable(qtrue);
	GL_Scissor(x, y, width, height);
	re_uiScissorSite = prevSite;
}

/*
================
Set2DInitialShaderTime
================
*/
void Set2DInitialShaderTime(float startTime)
{
	if (backEnd.in2D)
	{
		backEnd.shaderStartTime = startTime;
	}
}
