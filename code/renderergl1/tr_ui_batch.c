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
// tr_ui_batch.c -- batched 2D UI geometry for modern UI (GL1 GPU path)
#include "tr_local.h"

qboolean RE_UI2DBatchSupported(void)
{
	return qtrue;
}

qboolean RE_UI2DCanBatchShader(qhandle_t hShader)
{
	shader_t *sh;

	if (hShader == 0) {
		return qtrue;
	}

	sh = R_GetShaderByHandle(hShader);
	if (!sh || sh == tr.defaultShader || sh->defaultShader) {
		return qfalse;
	}
	if (sh->numUnfoggedPasses != 1) {
		return qfalse;
	}
	if (!sh->unfoggedStages[0] || !sh->unfoggedStages[0]->bundle[0].image[0]) {
		return qfalse;
	}
	if (sh->unfoggedStages[0]->bundle[0].numImageAnimations > 1) {
		return qfalse;
	}
	if (sh->unfoggedStages[0]->bundle[0].numTexMods != 0) {
		return qfalse;
	}

	return qtrue;
}

void RE_DrawUI2D(
	const ui2dVert_t *verts,
	int numVerts,
	const unsigned short *indexes,
	int numIndexes,
	qhandle_t hShader
)
{
	shader_t *sh;
	GLboolean msaaEnabled = GL_FALSE;

	if (numVerts < 3 || numIndexes < 3 || !verts || !indexes) {
		return;
	}

	R_IssuePendingRenderCommands();
	RE_UI2DTargetRebind();

#ifdef GL_MULTISAMPLE
	if (!RE_UI2DTargetIsActive() || RE_UI2DTargetSamples() <= 0) {
		msaaEnabled = qglIsEnabled(GL_MULTISAMPLE);
		if (msaaEnabled) {
			qglDisable(GL_MULTISAMPLE);
		}
	}
#else
	msaaEnabled = GL_FALSE;
#endif

	GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);

	if (glConfig.numTextureUnits > 1) {
		GL_SelectTexture(1);
		qglDisable(GL_TEXTURE_2D);
		GL_SelectTexture(0);
	}

	if (hShader != 0) {
		sh = R_GetShaderByHandle(hShader);
		if (sh && sh != tr.defaultShader && !sh->defaultShader &&
		    sh->unfoggedStages[0] && sh->unfoggedStages[0]->bundle[0].image[0]) {
			GL_Bind(sh->unfoggedStages[0]->bundle[0].image[0]);
			qglEnable(GL_TEXTURE_2D);
			qglEnableClientState(GL_TEXTURE_COORD_ARRAY);
			qglTexCoordPointer(2, GL_FLOAT, sizeof(ui2dVert_t), verts[0].st);
		} else {
			qglDisable(GL_TEXTURE_2D);
			qglDisableClientState(GL_TEXTURE_COORD_ARRAY);
		}
	} else {
		qglDisable(GL_TEXTURE_2D);
		qglDisableClientState(GL_TEXTURE_COORD_ARRAY);
	}

	qglEnableClientState(GL_VERTEX_ARRAY);
	qglVertexPointer(2, GL_FLOAT, sizeof(ui2dVert_t), verts[0].xy);

	qglEnableClientState(GL_COLOR_ARRAY);
	qglColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ui2dVert_t), verts[0].rgba);

	qglDrawElements(GL_TRIANGLES, numIndexes, GL_UNSIGNED_SHORT, indexes);

	qglDisableClientState(GL_COLOR_ARRAY);
	qglEnableClientState(GL_TEXTURE_COORD_ARRAY);
	qglEnable(GL_TEXTURE_2D);
	qglColor4ubv(backEnd.color2D);

#ifdef GL_MULTISAMPLE
	if (!RE_UI2DTargetIsActive() || RE_UI2DTargetSamples() <= 0) {
		if (msaaEnabled) {
			qglEnable(GL_MULTISAMPLE);
		}
	}
#endif
}
