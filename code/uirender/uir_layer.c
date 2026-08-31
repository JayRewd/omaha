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

#include "uir_layer.h"
#include "uir_image.h"
#include "uir_gradient.h"
#include "uir_viewport.h"
#include "uir_batch.h"
#include "uir_compositor.h"

#include <math.h>
#include <string.h>

static uir_layer_backend_t g_layerBackend;
static int                 g_layerDepth = 0;
static int                 g_maskShader = 0;
static float               g_maskDrawX = 0.0f;
static float               g_maskDrawY = 0.0f;
static float               g_maskDrawW = 0.0f;
static float               g_maskDrawH = 0.0f;
static float               g_maskS1 = 0.0f;
static float               g_maskT1 = 0.0f;
static float               g_maskS2 = 1.0f;
static float               g_maskT2 = 1.0f;

void UIR_LayerSetBackend(const uir_layer_backend_t *backend)
{
	if (backend) {
		g_layerBackend = *backend;
	} else {
		memset(&g_layerBackend, 0, sizeof(g_layerBackend));
	}
}

int UIR_LayerAvailable(void)
{
	if (!g_layerBackend.available || !g_layerBackend.beginLayer || !g_layerBackend.endLayer ||
	    !g_layerBackend.applyMask) {
		return 0;
	}
	return g_layerBackend.available() ? 1 : 0;
}

static void uir_layer_bounds_to_scissor(
	const uir_viewport_t *vp,
	float x,
	float y,
	float w,
	float h,
	int *sx,
	int *sy,
	int *sw,
	int *sh
)
{
	float fx0;
	float fy0;
	float fx1;
	float fy1;

	UIR_ViewportDrawToFb(vp, x, y, &fx0, &fy0);
	UIR_ViewportDrawToFb(vp, x + w, y + h, &fx1, &fy1);
	*sx = (int)floorf(fx0 < fx1 ? fx0 : fx1);
	*sy = (int)floorf(fy0 < fy1 ? fy0 : fy1);
	*sw = (int)ceilf(fx0 > fx1 ? fx0 : fx1) - *sx;
	*sh = (int)ceilf(fy0 > fy1 ? fy0 : fy1) - *sy;
	if (*sw < 0) {
		*sw = 0;
	}
	if (*sh < 0) {
		*sh = 0;
	}
	*sy = vp->vpY + vp->vpH - (*sy + *sh);
}

uir_status_t UIR_BeginImageMask(
	float x,
	float y,
	float w,
	float h,
	const char *maskSpec,
	uir_image_fit_t fit
)
{
	const uir_viewport_t *vp;
	int sx;
	int sy;
	int sw;
	int sh;
	int shader = 0;
	int imgW = 0;
	int imgH = 0;
	float drawX;
	float drawY;
	float drawW;
	float drawH;
	float s1;
	float t1;
	float s2;
	float t2;
	uir_status_t st;

	if (!maskSpec || !maskSpec[0] || !(w > 0.0f) || !(h > 0.0f)) {
		return UIR_ERR_INVALID_ARG;
	}
	if (g_layerDepth > 0) {
		return UIR_ERR_WRONG_PHASE;
	}
	if (!UIR_LayerAvailable()) {
		return UIR_ERR_NOT_READY;
	}

	vp = UIR_CompositorViewport();
	if (!vp) {
		return UIR_ERR_NOT_READY;
	}

	UIR_BatchFlush();
	/* Added in OPM: layer RT begin changes GL scissor / target. */
	UIR_InvalidateAppliedClip();

	if (UIR_GradientIsBrush(maskSpec)) {
		/* Added in OPM: gradient brush → atlas; coverage is stretch over dest. */
		st = UIR_GradientEnsureShader(maskSpec, w, h, &shader, &imgW, &imgH);
		if (st != UIR_OK || !shader) {
			return (st == UIR_OK) ? UIR_ERR_NOT_READY : st;
		}
		fit = UIR_IMAGE_FIT_STRETCH;
	} else {
		if (!g_layerBackend.registerShaderNoMip) {
			return UIR_ERR_NOT_READY;
		}
		shader = g_layerBackend.registerShaderNoMip(maskSpec);
		if (!shader) {
			return UIR_ERR_NOT_READY;
		}
		if (g_layerBackend.getShaderSize) {
			g_layerBackend.getShaderSize(shader, &imgW, &imgH);
		}
	}
	if (imgW <= 0) {
		imgW = 1;
	}
	if (imgH <= 0) {
		imgH = 1;
	}

	UIR_ComputeImageRect(
		(float)imgW,
		(float)imgH,
		x,
		y,
		w,
		h,
		fit,
		&drawX,
		&drawY,
		&drawW,
		&drawH,
		&s1,
		&t1,
		&s2,
		&t2
	);

	uir_layer_bounds_to_scissor(vp, x, y, w, h, &sx, &sy, &sw, &sh);
	if (sw <= 0 || sh <= 0) {
		return UIR_ERR_INVALID_ARG;
	}

	if (!g_layerBackend.beginLayer(sx, sy, sw, sh, x, y, w, h)) {
		return UIR_ERR_NOT_READY;
	}

	g_maskShader = shader;
	g_maskDrawX = drawX;
	g_maskDrawY = drawY;
	g_maskDrawW = drawW;
	g_maskDrawH = drawH;
	g_maskS1 = s1;
	g_maskT1 = t1;
	g_maskS2 = s2;
	g_maskT2 = t2;
	g_layerDepth = 1;
	return UIR_OK;
}

void UIR_EndImageMask(void)
{
	if (g_layerDepth <= 0) {
		return;
	}

	UIR_BatchFlush();

	if (g_layerBackend.applyMask && g_maskShader) {
		g_layerBackend.applyMask(
			g_maskShader,
			g_maskDrawX,
			g_maskDrawY,
			g_maskDrawW,
			g_maskDrawH,
			g_maskS1,
			g_maskT1,
			g_maskS2,
			g_maskT2
		);
	}
	if (g_layerBackend.endLayer) {
		g_layerBackend.endLayer();
	}

	/* Added in OPM: leaving layer RT may reset GL scissor. */
	UIR_InvalidateAppliedClip();
	g_layerDepth = 0;
	g_maskShader = 0;
}
