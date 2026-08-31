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
#ifndef UIR_LAYER_H
#define UIR_LAYER_H

#include "uir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int (*available)(void);
	int (*beginLayer)(int fbX, int fbY, int fbW, int fbH, float uiX, float uiY, float uiW, float uiH);
	void (*applyMask)(int shader, float x, float y, float w, float h, float s1, float t1, float s2, float t2);
	void (*endLayer)(void);
	int (*registerShaderNoMip)(const char *path);
	void (*getShaderSize)(int shader, int *width, int *height);
} uir_layer_backend_t;

void UIR_LayerSetBackend(const uir_layer_backend_t *backend);
int UIR_LayerAvailable(void);

/*
 * Added in OPM: begin soft mask coverage for subsequent draws (full subtree).
 * maskSpec is either a VFS image path or a linear(...)/radial(...) gradient brush.
 * Uses a UI-only layer RT when available; returns UIR_ERR_NOT_READY / WRONG_PHASE otherwise.
 * Pair with UIR_EndImageMask. Nested calls fail.
 */
uir_status_t UIR_BeginImageMask(
	float x,
	float y,
	float w,
	float h,
	const char *maskSpec,
	uir_image_fit_t fit
);
void UIR_EndImageMask(void);

#ifdef __cplusplus
}
#endif

#endif /* UIR_LAYER_H */
