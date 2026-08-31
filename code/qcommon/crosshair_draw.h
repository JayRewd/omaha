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

#pragma once

#include "q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	XHAIR_MODE_NONE = 0,
	XHAIR_MODE_OPEN,
	XHAIR_MODE_SOLID,
	XHAIR_MODE_DOT
} xhair_mode_t;

typedef enum {
	XHAIR_PASS_OUTLINE = 0,
	XHAIR_PASS_FILL
} xhair_pass_t;

typedef struct {
	xhair_mode_t mode;
	int style;
	float gap;
	float length;
	float thickness;
	float dotRadius;
	float solidSize;
	float solidThickness;
	float r;
	float g;
	float b;
	float a;
	float outline;
	qboolean drawOutline;
	qboolean tStyle;
	qboolean centerDot;
	qboolean dynamicEnabled;
	float splitDist;
	float splitRatio;
	float splitInnerAlphaMod;
	float splitOuterAlphaMod;
	float dynamicSpreadPx;
	float dynamicScale;
} xhair_config_t;

typedef struct {
	xhair_mode_t mode;
	float cx;
	float cy;
	float scaleX;
	float scaleY;
	float gap;
	float length;
	float thickness;
	float dotRadius;
	float solidSize;
	float r;
	float g;
	float b;
	float a;
	float outline;
	qboolean drawOutline;
	qboolean tStyle;
	qboolean centerDot;
	qboolean dynamicEnabled;
	int style;
	float splitDist;
	float splitRatio;
	float splitInnerAlphaMod;
	float splitOuterAlphaMod;
	float dynamicSpreadPx;
	float dynamicScale;
} xhair_frame_t;

typedef struct {
	float x;
	float y;
	float w;
	float h;
	float r;
	float g;
	float b;
	float a;
} xhair_rect_t;

#define XHAIR_MAX_RECTS 64

xhair_mode_t XHair_ParseMode(const char *mode);

void XHair_ResolveColor(int preset, float customR, float customG, float customB, int useAlpha, int alpha,
                        float *outR, float *outG, float *outB, float *outA);

void XHair_ApplyStyleDynamic(xhair_config_t *cfg);

void XHair_BuildFrame(const xhair_config_t *cfg, float cx, float cy, float scaleX, float scaleY, xhair_frame_t *out);

int XHair_EmitRects(const xhair_frame_t *frame, xhair_pass_t pass, xhair_rect_t *out, int maxOut);

#if !defined(CGAME_DLL) && !defined(GAME_DLL)
void XHair_ReadConfigFromCvars(xhair_config_t *cfg);
void XHair_RegisterClientCvars(void);
void XHair_ClientSyncClAliases(void);
#endif

#ifdef __cplusplus
}
#endif
