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
#ifndef UIR_BATCH_H
#define UIR_BATCH_H

#include "uir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UIR_BATCH_MAX_VERTS   4096
#define UIR_BATCH_MAX_INDEXES 12288

typedef struct {
	float x;
	float y;
	float s;
	float t;
	unsigned char r;
	unsigned char g;
	unsigned char b;
	unsigned char a;
} uir_vert_t;

typedef struct {
	int  (*supported)(void);
	int  (*canBatchShader)(int shader);
	void (*draw)(const uir_vert_t *v, int nv, const unsigned short *idx, int ni, int shader);
	int  (*targetAvailable)(void);
	int  (*targetSamples)(void);
	int  (*beginTarget)(void);
	void (*endTarget)(void);
} uir_batch_backend_t;

void UIR_BatchSetBackend(const uir_batch_backend_t *backend);
void UIR_BatchSetEnabled(int enabled);
int  UIR_BatchEnabled(void);
void UIR_BatchSetFringe(int enabled);
int  UIR_BatchFringeEnabled(void);
void UIR_BatchBeginFrame(uir_stats_t *stats);
void UIR_BatchFlush(void);
void UIR_BatchTargetBegin(void);
void UIR_BatchTargetEnd(void);

uir_status_t UIR_BatchQuad(
	int shader,
	float x,
	float y,
	float w,
	float h,
	float s0,
	float t0,
	float s1,
	float t1,
	const uir_color_t *rgba
);

uir_status_t UIR_BatchQuadSkewed(
	int shader,
	float x,
	float y,
	float w,
	float h,
	float s0,
	float t0,
	float s1,
	float t1,
	const uir_color_t *rgba,
	float skewTan,
	float originY
);

/* Added in Omaha: rotate axis-aligned quad around pivot (clockwise degrees). */
uir_status_t UIR_BatchQuadRotated(
	int shader,
	float x,
	float y,
	float w,
	float h,
	float s0,
	float t0,
	float s1,
	float t1,
	const uir_color_t *rgba,
	float rotationDeg,
	float pivotX,
	float pivotY
);

uir_status_t UIR_BatchTriangles(
	int shader,
	const uir_vert_t *v,
	int nv,
	const unsigned short *idx,
	int ni
);

#ifdef __cplusplus
}
#endif

#endif /* UIR_BATCH_H */
