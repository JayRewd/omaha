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
	qboolean maskWriting; /* colorMask off — stencil REPLACE phase */
} ui_stencil_saved_t;

static ui_stencil_saved_t s_saved;


qboolean RE_UiStencilAvailable(void)
{
	/*
	 * Soft-mask / chrome-cache layer FBOs are color-only. Main UI MSAA FBO
	 * reports hasStencil after DEPTH24_STENCIL8 attach. Window
	 * glConfig.stencilBits is only used when UI FBO mode is off.
	 */
	if (RE_UiLayerIsActive() || RE_UiChromeCacheIsActive()) {
		return qfalse;
	}
	if (RE_UI2DTargetAvailable()) {
		return RE_UI2DTargetHasStencil();
	}
	return (glConfig.stencilBits >= 8) ? qtrue : qfalse;
}

/*
 * Added in Omaha: R_IssuePendingRenderCommands / UI2D batch begin can restore
 * color writes. Re-assert mask-write GL state so tessellated stencil fills do
 * not flash white into the color buffer.
 *
 * Also force blend ZERO,ONE so even if colorMask is clobbered, destination
 * color is preserved (src*0 + dst*1).
 */
void RE_UiStencilReassertMaskWrite(void)
{
	if (!s_saved.active || !s_saved.maskWriting) {
		return;
	}
	qglEnable(GL_STENCIL_TEST);
	qglStencilMask(0xFF);
	qglStencilFunc(GL_ALWAYS, 1, 0xFF);
	qglStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	qglColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	qglEnable(GL_BLEND);
	qglBlendFunc(GL_ZERO, GL_ONE);
}

qboolean RE_UiStencilIsMaskWriting(void)
{
	return (s_saved.active && s_saved.maskWriting) ? qtrue : qfalse;
}

void RE_BeginUiStencilMask(int x, int y, int width, int height)
{

	R_IssuePendingRenderCommands();
	/* Ensure mask begin/write/draw target the same UI attachment. */
	RE_UI2DTargetRebind();


	/*
	 * Fixed in Omaha Phase 3: no synchronous glGet* — UI chrome never uses
	 * stencil outside shape clips, and UIR_EndShapeClip invalidates applied
	 * scissor so the clip stack re-applies after end.
	 */
	memset(&s_saved, 0, sizeof(s_saved));
	s_saved.active = qtrue;
	s_saved.maskWriting = qtrue;

	/* Color inert before clear — some drivers glitch packed depth-stencil clears. */
	qglColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	qglEnable(GL_BLEND);
	qglBlendFunc(GL_ZERO, GL_ONE);

	/*
	 * Changed in Omaha: Phase 3 — only tighten scissor when the mask AABB is
	 * not already inside the current box. Still's two health-chip stencil
	 * begins were +2 glScissor against a fullscreen clip.
	 */
	GL_ScissorEnable(qtrue);
	{
		const int cx = glState.scissorBox[0];
		const int cy = glState.scissorBox[1];
		const int cw = glState.scissorBox[2];
		const int ch = glState.scissorBox[3];
		if (x < cx || y < cy || (x + width) > (cx + cw) || (y + height) > (cy + ch)) {
			const int prevSite = re_uiScissorSite;
			re_uiScissorSite = RE_UI_SCISSOR_STENCIL;
			GL_Scissor(x, y, width, height);
			re_uiScissorSite = prevSite;
		}
	}
	qglEnable(GL_STENCIL_TEST);
	qglStencilMask(0xFF);
	qglClearStencil(0);
	qglClear(GL_STENCIL_BUFFER_BIT);


	RE_UiStencilReassertMaskWrite();

	tr_uiStats.stencilBegins++;
}

/*
 * Added in Omaha: stencil mask fill that never goes through UI2D batch / GL_State.
 * Shape background is already painted; this must not touch color.
 */
void RE_DrawUiStencilMaskTris(const float *xy, int strideBytes, int nv, const unsigned short *idx, int ni)
{
	if (!s_saved.active || !s_saved.maskWriting || !xy || !idx || nv < 3 || ni < 3) {
		return;
	}
	if (strideBytes < (int)(sizeof(float) * 2)) {
		strideBytes = (int)(sizeof(float) * 2);
	}

	RE_UI2DTargetRebind();
	RE_UiStencilReassertMaskWrite();

	/* Added in Omaha: Phase 4 — UI VBO stays bound across the session; this path uses client arrays. */
	RE_UI2D_UnbindBuffers();

	qglDisable(GL_TEXTURE_2D);
	qglDisableClientState(GL_COLOR_ARRAY);
	qglDisableClientState(GL_TEXTURE_COORD_ARRAY);
	qglEnableClientState(GL_VERTEX_ARRAY);
	qglVertexPointer(2, GL_FLOAT, strideBytes, xy);
	/* Changed in Omaha: prefer DrawRangeElements when available (Phase 1.4). */
	if (qglDrawRangeElements) {
		qglDrawRangeElements(GL_TRIANGLES, 0, (GLuint)(nv - 1), ni, GL_UNSIGNED_SHORT, idx);
	} else {
		qglDrawElements(GL_TRIANGLES, ni, GL_UNSIGNED_SHORT, idx);
	}

	/*
	 * Fixed in Omaha: the backend treats GL_TEXTURE_2D on unit 0 as always enabled
	 * (legacy tess draws never re-enable it). Leaving it off made the next clipped
	 * host draw — classic rotated compassface — render as a flat white quad.
	 */
	qglEnable(GL_TEXTURE_2D);

	/*
	 * Fixed in Omaha: record the blend actually set (ZERO,ONE) instead of XOR-toggling
	 * the cache. Mask-tris followed by BeginUiStencilDraw used to XOR twice and cancel,
	 * so GL_State skipped the UI FBO Separate(alpha=ONE) re-apply and clipped legacy
	 * draws (classic compassface) wrote reduced destination alpha — see-through dial.
	 */
	glState.glStateBits = (glState.glStateBits & ~(GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS)) |
		GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE;
}

void RE_BeginUiStencilDraw(void)
{
	if (!s_saved.active) {
		return;
	}
	s_saved.maskWriting = qfalse;
	qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	/* Restore normal UI straight-alpha blend after ZERO,ONE mask write. */
	qglEnable(GL_BLEND);
	qglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	/* Same state GL_State produces for SRC_ALPHA,ONE_MINUS_SRC_ALPHA on the UI FBO. */
	RE_UI2DRestoreFboBlend();
	glState.glStateBits = (glState.glStateBits & ~(GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS)) |
		GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
	qglStencilFunc(GL_EQUAL, 1, 0xFF);
	qglStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

void RE_EndUiStencil(void)
{
	if (!s_saved.active) {
		return;
	}

	s_saved.maskWriting = qfalse;
	qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	/* UI chrome does not keep stencil enabled outside shape clips. */
	qglDisable(GL_STENCIL_TEST);
	qglStencilFunc(GL_ALWAYS, 0, (GLuint)~0);
	qglStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	/*
	 * Leave scissor at the shape bounds; UIR_EndShapeClip calls
	 * UIR_InvalidateAppliedClip so the next clip push re-applies the stack.
	 */
	s_saved.active = qfalse;
}
