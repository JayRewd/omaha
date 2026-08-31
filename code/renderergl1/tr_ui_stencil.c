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
// tr_ui_stencil.c -- stencil masking for modern UI shape clips
#include "tr_local.h"

typedef struct {
	qboolean active;
	GLboolean colorMask[4];
	GLint stencilFunc;
	GLint stencilRef;
	GLint stencilValueMask;
	GLint stencilFail;
	GLint stencilZFail;
	GLint stencilPass;
	GLint scissorBox[4];
} ui_stencil_saved_t;

static ui_stencil_saved_t s_saved;

qboolean RE_UiStencilAvailable(void)
{
	return (glConfig.stencilBits >= 8) ? qtrue : qfalse;
}

void RE_BeginUiStencilMask(int x, int y, int width, int height)
{
	R_IssuePendingRenderCommands();

	memset(&s_saved, 0, sizeof(s_saved));
	s_saved.active = qtrue;

	qglGetBooleanv(GL_COLOR_WRITEMASK, s_saved.colorMask);
	qglGetIntegerv(GL_STENCIL_FUNC, &s_saved.stencilFunc);
	qglGetIntegerv(GL_STENCIL_REF, &s_saved.stencilRef);
	qglGetIntegerv(GL_STENCIL_VALUE_MASK, &s_saved.stencilValueMask);
	qglGetIntegerv(GL_STENCIL_FAIL, &s_saved.stencilFail);
	qglGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &s_saved.stencilZFail);
	qglGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &s_saved.stencilPass);
	qglGetIntegerv(GL_SCISSOR_BOX, s_saved.scissorBox);

	qglEnable(GL_SCISSOR_TEST);
	qglScissor(x, y, width, height);
	qglEnable(GL_STENCIL_TEST);
	qglStencilMask(0xFF);
	qglClearStencil(0);
	qglClear(GL_STENCIL_BUFFER_BIT);
	qglStencilFunc(GL_ALWAYS, 1, 0xFF);
	qglStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	qglColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
}

void RE_BeginUiStencilDraw(void)
{
	if (!s_saved.active) {
		return;
	}
	qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	qglStencilFunc(GL_EQUAL, 1, 0xFF);
	qglStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

void RE_EndUiStencil(void)
{
	if (!s_saved.active) {
		return;
	}

	qglColorMask(s_saved.colorMask[0], s_saved.colorMask[1], s_saved.colorMask[2], s_saved.colorMask[3]);
	qglStencilFunc(s_saved.stencilFunc, s_saved.stencilRef, s_saved.stencilValueMask);
	qglStencilOp(s_saved.stencilFail, s_saved.stencilZFail, s_saved.stencilPass);
	/* 2D UI chrome does not keep stencil enabled outside shape clips. */
	qglDisable(GL_STENCIL_TEST);
	qglEnable(GL_SCISSOR_TEST);
	qglScissor(s_saved.scissorBox[0], s_saved.scissorBox[1], s_saved.scissorBox[2], s_saved.scissorBox[3]);
	s_saved.active = qfalse;
}
