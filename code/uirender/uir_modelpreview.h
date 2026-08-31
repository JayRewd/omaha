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
#ifndef UIR_MODELPREVIEW_H
#define UIR_MODELPREVIEW_H

#include "uir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UIR_MP_MODEL_SCALE_DEFAULT 1.5f
#define UIR_MP_FOV_DEFAULT         30.0f
/* Added in OPM: skeletor cache slots for menu previews (below ENTITYNUM_WORLD). */
#define UIR_MP_ENTITY_BASE 1008
#define UIR_MP_ENTITY_COUNT 14

typedef struct {
	float angles[3];     /* pitch yaw roll; default (10,180,0) */
	float offset[3];     /* view-space depth bias; default (60,0,0) */
	float framingScale;  /* default 1.0; reduce for tall rects */
	float modelScale;    /* 0 => UIR_MP_MODEL_SCALE_DEFAULT */
	float animPhase;     /* initial phase in [0,1) of clip length */
	int   animVariant;   /* TAF_RANDOM group index; default 0 */
	int   hasAngles;
	int   hasOffset;
	int   hasFramingScale;
	int   hasBbox;
	int   hasFov;
	int   hasColor;
	int   bboxFromModel;
	float bboxMins[3];
	float bboxMaxs[3];
	float fov;           /* horizontal FOV degrees; 0 => UIR_MP_FOV_DEFAULT */
	int   instanceId;    /* per-preview identity for anim slot separation */
	int   previewSlotId; /* compositor queue index; stable renderer entity slot */
	int   modelHandle;   /* qhandle_t */
	const char *animName;
	float color[4];
	int   serverTime;    /* 0 => use realtime */
	int   realtime;
} uir_model_preview_params_t;

typedef struct {
	void (*clearScene)(void);
	void (*addRefEntity)(const void *ent, int parentEntityNumber);
	void (*renderScene)(const void *refdef);
	void (*set2DWindow)(int x, int y, int w, int h, float left, float right, float bottom, float top, float n, float f);
	void (*scissor)(int x, int y, int w, int h);
	void (*anglesToAxis)(const float angles[3], float axis[3][3]);
	void *(*modelGetHandle)(int model); /* returns dtiki_t* */
	int (*animNumForNameVariant)(void *tiki, const char *name, int variant);
	float (*animTime)(void *tiki, int index);
	void (*modelBounds)(void *tiki, float scale, float mins[3], float maxs[3]);
	void (*forceUpdatePose)(void *ent);
	int (*exportModelPreviewPNG)(const void *refdef, const char *vfsPath);
} uir_modelpreview_backend_t;

void UIR_ModelPreviewSetBackend(const uir_modelpreview_backend_t *backend);

/* Pure helpers (menu-model-draw.md framing + origin). */
void UIR_ModelPreviewComputeFraming(float w, float h, float *outScale, float outOffset[3]);
void UIR_ModelPreviewComputeOrigin(const float mins[3], const float maxs[3], float framingScale, float outOrigin[3]);
/* Added in OPM: max axis extent of a bbox (for shared bake camera distance). */
float UIR_ModelPreviewBBoxExtent(const float mins[3], const float maxs[3]);
/* Added in OPM: AABB of mins/maxs after entity-style axis transform (columns). */
void UIR_ModelPreviewAxisTransformBounds(
	const float mins[3], const float maxs[3], const float axis[3][3], float outMins[3], float outMaxs[3]
);
/* Added in OPM: shift AABB by delta (e.g. TIKI load_origin * load_scale). */
void UIR_ModelPreviewShiftBounds(
	const float mins[3], const float maxs[3], const float delta[3], float outMins[3], float outMaxs[3]
);
/* Added in OPM: center from (optionally oriented) mins/maxs; sharedExtent > 0 fixes cam distance. */
void UIR_ModelPreviewComputeOriginShared(
	const float mins[3],
	const float maxs[3],
	float framingScale,
	float sharedExtent,
	const float axis[3][3],
	float outOrigin[3]
);
uir_status_t UIR_ModelPreviewCalcFov(int w, int h, float fovXIn, float *fovX, float *fovY);

/* Added in OPM: testable loop wrap for menu model preview animation time. */
void UIR_ModelPreviewWrapAnimTime(float *animTime, float animLength);

/* Added in OPM: map [0,1) phase onto clip seconds (wraps; length<=0 => 0). */
float UIR_ModelPreviewPhaseToAnimTime(float animPhase, float animLength);

uir_status_t UIR_ModelPreviewDraw(const uir_rect_t *destPx, const uir_model_preview_params_t *params);

/* Added in OPM: transparent PNG model bake (weapon icons, etc.). */
typedef struct {
	int         modelHandle;
	float       angles[3]; /* pitch yaw roll */
	float       offset[3];
	float       modelScale;
	float       framingScale; /* 0 => auto from aspect */
	float       sharedExtent; /* >0 => shared camera distance (relative sizes) */
	float       fov;          /* 0 => UIR_MP_FOV_DEFAULT */
	int         width;
	int         height;
	const char *outPath;
} uir_model_bake_params_t;

/* Added in OPM: max ModelBounds extent at modelScale (0 if unavailable).
 * When angles != NULL, uses oriented AABB extent (matches bake framing). */
float UIR_ModelBakeExtent(int modelHandle, float modelScale, const float angles[3]);

uir_status_t UIR_ModelBakeBuildScene(
	const uir_model_bake_params_t *params, void *outRefdef, void *outEntity
);
uir_status_t UIR_ModelBakeToPNG(const uir_model_bake_params_t *params);

#ifdef __cplusplus
}
#endif

#endif /* UIR_MODELPREVIEW_H */
