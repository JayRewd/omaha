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

/* tr_ui_batch.c -- batched 2D UI geometry for modern UI (GL1 GPU path) */

#include "tr_local.h"

#include <stddef.h>

/* Added in Omaha Stage 5: session hoists IssuePending/MSAA/client-state. */

static qboolean s_ui2dBatchOpen = qfalse;

static qboolean s_ui2dMsaaWasEnabled = qfalse;

/* Added in Omaha: Phase 3 — UI batch VBO/IBO streaming (r_uiVbo). */
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif

static GLuint s_uiVbo = 0;
static GLuint s_uiIbo = 0;

/*
 * Added in Omaha: Phase 4 — streaming ring. Append with BufferSubData and
 * orphan only when the ring wraps, instead of two BufferData orphans per draw.
 * Buffers stay bound for the whole UI2D session (unbound at session end).
 */
#define UI_VBO_RING_BYTES (1024 * 1024)
#define UI_IBO_RING_BYTES (256 * 1024)
static GLsizeiptr s_uiVboRingUsed = -1; /* -1: (re)allocate on next draw */
static GLsizeiptr s_uiIboRingUsed = -1;
static qboolean   s_uiBuffersBound = qfalse;

void RE_UI2D_UnbindBuffers(void)
{
	if (qglBindBuffer) {
		qglBindBuffer(GL_ARRAY_BUFFER, 0);
		qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}
	s_uiBuffersBound = qfalse;
}

/*
 * Fixed in Omaha: an owned UI batch session keeps its VBO/IBO bound between
 * draws. Client-array draws (tess / RB_EndSurface) issued inside that session
 * would have their CPU pointers read as buffer offsets → GPU out-of-bounds
 * fetch (NVIDIA "kernel exception", 0xC0000409 in nvoglv64). Release first.
 */
void RE_UI2D_ReleaseBuffersForClientArrays(void)
{
	if (!s_uiBuffersBound) {
		return;
	}
	RE_UI2D_UnbindBuffers();
}

void RE_UI2D_VboShutdown(void)
{
	RE_UI2D_UnbindBuffers();
	s_uiVboRingUsed = -1;
	s_uiIboRingUsed = -1;
	if (qglDeleteBuffers) {
		if (s_uiVbo) {
			qglDeleteBuffers(1, &s_uiVbo);
			s_uiVbo = 0;
		}
		if (s_uiIbo) {
			qglDeleteBuffers(1, &s_uiIbo);
			s_uiIbo = 0;
		}
	} else {
		s_uiVbo = 0;
		s_uiIbo = 0;
	}
}

static qboolean RE_UI2D_VboEnsure(void)
{
	if (!r_uiVbo || !r_uiVbo->integer) {
		return qfalse;
	}
	if (!qglGenBuffers || !qglBindBuffer || !qglBufferData || !qglDeleteBuffers) {
		return qfalse;
	}
	if (!s_uiVbo) {
		qglGenBuffers(1, &s_uiVbo);
		s_uiVboRingUsed = -1;
		s_uiBuffersBound = qfalse;
	}
	if (!s_uiIbo) {
		qglGenBuffers(1, &s_uiIbo);
		s_uiIboRingUsed = -1;
		s_uiBuffersBound = qfalse;
	}
	return (s_uiVbo && s_uiIbo) ? qtrue : qfalse;
}



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



void RE_UI2DBatchBegin(void)

{

	if (s_ui2dBatchOpen) {

		return;

	}



	R_IssuePendingRenderCommands();

	RE_UI2DTargetRebind();



	s_ui2dMsaaWasEnabled = qfalse;

#ifdef GL_MULTISAMPLE

	if (!RE_UI2DTargetIsActive() || RE_UI2DTargetSamples() <= 0) {

		/* Changed in Omaha: track MSAA via glState; r_uiSyncQueries restores old glIsEnabled. */

		if (r_uiSyncQueries && r_uiSyncQueries->integer) {

			s_ui2dMsaaWasEnabled = qglIsEnabled(GL_MULTISAMPLE) ? qtrue : qfalse;

			tr_uiStats.glQueries++;

		} else {

			s_ui2dMsaaWasEnabled = glState.multisampleEnabled;

		}

		if (s_ui2dMsaaWasEnabled) {

			GL_MultisampleEnable(qfalse);

		}

	}

#endif



	GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	/* Fixed in Omaha: GL_State uses glBlendFunc — re-assert FBO alpha=ONE Separate. */
	RE_UI2DRestoreFboBlend();



	if (glConfig.numTextureUnits > 1) {

		GL_SelectTexture(1);

		qglDisable(GL_TEXTURE_2D);

		GL_SelectTexture(0);

	}



	qglEnableClientState(GL_VERTEX_ARRAY);

	qglEnableClientState(GL_COLOR_ARRAY);



	/* After GL_State — mask write must keep color inert (ZERO,ONE + colorMask off). */

	RE_UiStencilReassertMaskWrite();



	s_ui2dBatchOpen = qtrue;

}



void RE_UI2DBatchEnd(void)

{

	if (!s_ui2dBatchOpen) {

		return;

	}



	qglDisableClientState(GL_COLOR_ARRAY);

	qglEnableClientState(GL_TEXTURE_COORD_ARRAY);

	qglEnable(GL_TEXTURE_2D);

	qglColor4ubv(backEnd.color2D);



#ifdef GL_MULTISAMPLE

	if (!RE_UI2DTargetIsActive() || RE_UI2DTargetSamples() <= 0) {

		if (s_ui2dMsaaWasEnabled) {

			GL_MultisampleEnable(qtrue);

		}

	}

#endif



	s_ui2dMsaaWasEnabled = qfalse;

	s_ui2dBatchOpen = qfalse;

	/* Added in Omaha: Phase 3 — never leave UI VBO/IBO bound for legacy paths. */
	RE_UI2D_UnbindBuffers();

}



static void RE_DrawUI2D_Inner(

	const ui2dVert_t *verts,

	int numVerts,

	const unsigned short *indexes,

	int numIndexes,

	qhandle_t hShader,

	qboolean sessionOwned

)

{

	shader_t *sh;

	qboolean msaaWasEnabled = qfalse;



	if (numVerts < 3 || numIndexes < 3 || !verts || !indexes) {

		return;

	}



	/* Mask-write phase: keep color disabled even when the UI2D session is already open. */

	RE_UiStencilReassertMaskWrite();



	if (!sessionOwned) {

		R_IssuePendingRenderCommands();

		RE_UI2DTargetRebind();

		RE_UiStencilReassertMaskWrite();



#ifdef GL_MULTISAMPLE

		if (!RE_UI2DTargetIsActive() || RE_UI2DTargetSamples() <= 0) {

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

		}

#endif



		GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
		/* Fixed in Omaha: GL_State uses glBlendFunc — re-assert FBO alpha=ONE Separate. */
		RE_UI2DRestoreFboBlend();



		if (glConfig.numTextureUnits > 1) {

			GL_SelectTexture(1);

			qglDisable(GL_TEXTURE_2D);

			GL_SelectTexture(0);

		}



		qglEnableClientState(GL_VERTEX_ARRAY);

		qglEnableClientState(GL_COLOR_ARRAY);



		/* GL_State above restores SRC_ALPHA blend — re-assert mask write. */

		RE_UiStencilReassertMaskWrite();

	}



	if (hShader != 0) {

		sh = R_GetShaderByHandle(hShader);

		if (sh && sh != tr.defaultShader && !sh->defaultShader &&

		    sh->unfoggedStages[0] && sh->unfoggedStages[0]->bundle[0].image[0]) {

			GL_Bind(sh->unfoggedStages[0]->bundle[0].image[0]);

			qglEnable(GL_TEXTURE_2D);

			qglEnableClientState(GL_TEXTURE_COORD_ARRAY);

		} else {

			qglDisable(GL_TEXTURE_2D);

			qglDisableClientState(GL_TEXTURE_COORD_ARRAY);

			sh = NULL;

		}

	} else {

		qglDisable(GL_TEXTURE_2D);

		qglDisableClientState(GL_TEXTURE_COORD_ARRAY);

		sh = NULL;

	}



	/* Last chance before draw — sessionOwned path never hit GL_State above. */

	RE_UiStencilReassertMaskWrite();



	/* Added in Omaha: Phase 3 — stream verts/indexes via VBO when available. */
	if (RE_UI2D_VboEnsure()) {
		const qboolean textured = (sh && sh->unfoggedStages[0] && sh->unfoggedStages[0]->bundle[0].image[0]) ? qtrue : qfalse;
		const GLsizeiptr vBytes = (GLsizeiptr)(numVerts * (int)sizeof(ui2dVert_t));
		const GLsizeiptr iBytes = (GLsizeiptr)(numIndexes * (int)sizeof(unsigned short));
		GLsizeiptr vOff = 0;
		GLsizeiptr iOff = 0;

		if (!s_uiBuffersBound) {
			qglBindBuffer(GL_ARRAY_BUFFER, s_uiVbo);
			qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_uiIbo);
			s_uiBuffersBound = qtrue;
		}

		/* Added in Omaha: Phase 4 — ring append; orphan only on wrap. */
		if (qglBufferSubData && vBytes <= UI_VBO_RING_BYTES && iBytes <= UI_IBO_RING_BYTES) {
			if (s_uiVboRingUsed < 0 || s_uiVboRingUsed + vBytes > UI_VBO_RING_BYTES) {
				qglBufferData(GL_ARRAY_BUFFER, UI_VBO_RING_BYTES, NULL, GL_STREAM_DRAW);
				s_uiVboRingUsed = 0;
			}
			vOff = s_uiVboRingUsed;
			qglBufferSubData(GL_ARRAY_BUFFER, vOff, vBytes, verts);
			s_uiVboRingUsed += (vBytes + 3) & ~(GLsizeiptr)3;

			if (s_uiIboRingUsed < 0 || s_uiIboRingUsed + iBytes > UI_IBO_RING_BYTES) {
				qglBufferData(GL_ELEMENT_ARRAY_BUFFER, UI_IBO_RING_BYTES, NULL, GL_STREAM_DRAW);
				s_uiIboRingUsed = 0;
			}
			iOff = s_uiIboRingUsed;
			qglBufferSubData(GL_ELEMENT_ARRAY_BUFFER, iOff, iBytes, indexes);
			s_uiIboRingUsed += (iBytes + 3) & ~(GLsizeiptr)3;
		} else {
			qglBufferData(GL_ARRAY_BUFFER, vBytes, verts, GL_STREAM_DRAW);
			qglBufferData(GL_ELEMENT_ARRAY_BUFFER, iBytes, indexes, GL_STREAM_DRAW);
			s_uiVboRingUsed = -1;
			s_uiIboRingUsed = -1;
		}

		qglVertexPointer(2, GL_FLOAT, (GLsizei)sizeof(ui2dVert_t), (const void *)(size_t)(vOff + offsetof(ui2dVert_t, xy)));
		qglColorPointer(4, GL_UNSIGNED_BYTE, (GLsizei)sizeof(ui2dVert_t), (const void *)(size_t)(vOff + offsetof(ui2dVert_t, rgba)));
		if (textured) {
			qglTexCoordPointer(2, GL_FLOAT, (GLsizei)sizeof(ui2dVert_t), (const void *)(size_t)(vOff + offsetof(ui2dVert_t, st)));
		}

		if (qglDrawRangeElements) {
			qglDrawRangeElements(GL_TRIANGLES, 0, (GLuint)(numVerts - 1), numIndexes, GL_UNSIGNED_SHORT, (const void *)(size_t)iOff);
		} else {
			qglDrawElements(GL_TRIANGLES, numIndexes, GL_UNSIGNED_SHORT, (const void *)(size_t)iOff);
		}
	} else {
		if (sh && sh->unfoggedStages[0] && sh->unfoggedStages[0]->bundle[0].image[0]) {
			qglTexCoordPointer(2, GL_FLOAT, sizeof(ui2dVert_t), verts[0].st);
		}
		qglVertexPointer(2, GL_FLOAT, sizeof(ui2dVert_t), verts[0].xy);
		qglColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ui2dVert_t), verts[0].rgba);

		/* Changed in Omaha: prefer DrawRangeElements when available (Phase 1.4). */
		if (qglDrawRangeElements) {
			qglDrawRangeElements(GL_TRIANGLES, 0, (GLuint)(numVerts - 1), numIndexes, GL_UNSIGNED_SHORT, indexes);
		} else {
			qglDrawElements(GL_TRIANGLES, numIndexes, GL_UNSIGNED_SHORT, indexes);
		}
	}

	tr_uiStats.drawElements++;

	tr_uiStats.drawVerts += numVerts;

	/* Added in Omaha: Phase 2 — accumulate UI FBO dirty rect from batch verts. */
	if (RE_UI2DTargetIsActive() && !RE_UiStencilIsMaskWriting()) {
		int i;
		float minx = verts[0].xy[0];
		float miny = verts[0].xy[1];
		float maxx = minx;
		float maxy = miny;
		for (i = 1; i < numVerts; i++) {
			float vx = verts[i].xy[0];
			float vy = verts[i].xy[1];
			if (vx < minx) {
				minx = vx;
			}
			if (vy < miny) {
				miny = vy;
			}
			if (vx > maxx) {
				maxx = vx;
			}
			if (vy > maxy) {
				maxy = vy;
			}
		}
		RE_UI2D_AccumRectDraw(minx, miny, maxx, maxy);
	}



	if (!sessionOwned) {

		qglDisableClientState(GL_COLOR_ARRAY);

		qglEnableClientState(GL_TEXTURE_COORD_ARRAY);

		qglEnable(GL_TEXTURE_2D);

		qglColor4ubv(backEnd.color2D);

		RE_UI2D_UnbindBuffers();



#ifdef GL_MULTISAMPLE

		if (!RE_UI2DTargetIsActive() || RE_UI2DTargetSamples() <= 0) {

			if (msaaWasEnabled) {

				GL_MultisampleEnable(qtrue);

			}

		}

#endif

	}

}



void RE_DrawUI2D(

	const ui2dVert_t *verts,

	int numVerts,

	const unsigned short *indexes,

	int numIndexes,

	qhandle_t hShader

)

{

	RE_DrawUI2D_Inner(verts, numVerts, indexes, numIndexes, hShader, s_ui2dBatchOpen);

}


