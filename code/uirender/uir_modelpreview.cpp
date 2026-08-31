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

#include "uir_modelpreview.h"

#include "../corepp/tiki.h"
#include "../qcommon/q_shared.h"
#include "../renderercommon/tr_types.h"
#include "../tiki/tiki_frame.h"
#include "../tiki/tiki_shared.h"
#include "../tiki/tiki_surface.h"
#include "../tiki/tiki_utility.h"

#include <string.h>

#define UIR_MP_SLOTS 8

typedef struct {
	int   model;
	char  anim[64];
	int   variant;          /* active clip within the alias group */
	int   requestedVariant; /* from XML; preserved across dontrepeat fallback */
	int   index;
	int   lastRealtime; /* ms; -1 => seed animTime from phase */
	float animTime;
} uir_mp_anim_slot_t;

static uir_modelpreview_backend_t g_mp;
static uir_mp_anim_slot_t         g_slots[UIR_MP_SLOTS];
static int                        g_slotsInited = 0;

static void uir_mp_bounds_with_load_origin(void *tiki, float modelScale, float outMins[3], float outMaxs[3])
{
	dtiki_t *pmdl = (dtiki_t *)tiki;
	float    loadDelta[3];

	if (!tiki || !g_mp.modelBounds || !outMins || !outMaxs) {
		return;
	}
	g_mp.modelBounds(tiki, modelScale, outMins, outMaxs);
	if (!pmdl) {
		return;
	}
	VectorScale(pmdl->load_origin, pmdl->load_scale * modelScale, loadDelta);
	UIR_ModelPreviewShiftBounds(outMins, outMaxs, loadDelta, outMins, outMaxs);
}

static int uir_mp_angles_nonzero(const float angles[3])
{
	if (!angles) {
		return 0;
	}
	return angles[0] != 0.0f || angles[1] != 0.0f || angles[2] != 0.0f;
}

static void uir_mp_set_idle_pose(void *tiki, refEntity_t *ent)
{
	int index;

	if (!tiki || !ent || !g_mp.animNumForNameVariant) {
		return;
	}

	index = g_mp.animNumForNameVariant(tiki, "idle", 0);
	if (index < 0) {
		return;
	}

	ent->frameInfo[0].index = index;
	ent->frameInfo[0].time = 0.0f;
	ent->frameInfo[0].weight = 1.0f;
	ent->actionWeight = 1.0f;
}

/* Added in OPM: mirror fgame Entity::SurfaceCommand for refEntity_t surface flags. */
static void uir_mp_surface_command(dtiki_t *tiki, refEntity_t *ent, const char *surf_name, const char *token)
{
	const char *current_surface_name;
	int         surface_num;
	int         mask;
	int         action;
	qboolean    do_all = qfalse;
	qboolean    mult = qfalse;
	int         numsurfaces;
	size_t      surf_len;

	if (!tiki || !ent || !surf_name || !token || !token[0]) {
		return;
	}

	surf_len = strlen(surf_name);
	if (surf_len > 0 && surf_name[surf_len - 1] == '*') {
		mult = qtrue;
		surface_num = 0;
	} else if (Q_stricmp(surf_name, "all")) {
		surface_num = TIKI_Surface_NameToNum(tiki, surf_name);
		if (surface_num < 0) {
			return;
		}
	} else {
		surface_num = 0;
		do_all = qtrue;
	}

	switch (token[0]) {
	case '+':
		action = 1;
		token++;
		break;
	case '-':
		action = 2;
		token++;
		break;
	default:
		action = 1;
		break;
	}

	if (!Q_stricmp(token, "skin1")) {
		mask = MDL_SURFACE_SKINOFFSET_BIT0;
	} else if (!Q_stricmp(token, "skin2")) {
		mask = MDL_SURFACE_SKINOFFSET_BIT1;
	} else if (!Q_stricmp(token, "nodraw")) {
		mask = MDL_SURFACE_NODRAW;
	} else if (!Q_stricmp(token, "crossfade")) {
		mask = MDL_SURFACE_CROSSFADE_SKINS;
	} else {
		return;
	}

	numsurfaces = TIKI_NumSurfaces(tiki);
	for (; surface_num < numsurfaces && surface_num < MAX_MODEL_SURFACES; surface_num++) {
		if (mult) {
			current_surface_name = TIKI_Surface_NumToName(tiki, surface_num);
			if (Q_stricmpn(current_surface_name, surf_name, (int)surf_len - 1) != 0) {
				continue;
			}
		}

		if (action == 1) {
			ent->surfaces[surface_num] |= mask;
		} else if (action == 2) {
			ent->surfaces[surface_num] &= ~mask;
		}

		if (!do_all && !mult) {
			break;
		}
	}
}

static void uir_mp_apply_surface_args(dtiki_t *tiki, refEntity_t *ent, int num_args, char **args)
{
	int j;

	if (!tiki || !ent || num_args < 3 || !args || Q_stricmp(args[0], "surface")) {
		return;
	}

	for (j = 2; j < num_args; j++) {
		uir_mp_surface_command(tiki, ent, args[1], args[j]);
	}
}

static void uir_mp_apply_surface_cmds(dtiki_t *tiki, refEntity_t *ent, dtikicmd_t *cmds, int num_cmds)
{
	int i;

	if (!tiki || !ent || !cmds || num_cmds <= 0) {
		return;
	}

	for (i = 0; i < num_cmds; i++) {
		uir_mp_apply_surface_args(tiki, ent, cmds[i].num_args, cmds[i].args);
	}
}

/*
 * Added in OPM: dropped world weapons hide clip/extra surfaces via server init
 * and idle entry commands (+nodraw). Bake previews must apply the same state.
 */
static void uir_mp_apply_weapon_surface_state(dtiki_t *tiki, refEntity_t *ent)
{
	tiki_cmd_t tikicmds;
	int        idleAnim;
	int        i;

	if (!tiki || !ent || !tiki->a) {
		return;
	}

	uir_mp_apply_surface_cmds(tiki, ent, tiki->a->server_initcmds, tiki->a->num_server_initcmds);

	idleAnim = -1;
	if (g_mp.animNumForNameVariant) {
		idleAnim = g_mp.animNumForNameVariant(tiki, "idle", 0);
	}
	if (idleAnim < 0) {
		return;
	}

	if (TIKI_Frame_Commands_Server(tiki, idleAnim, TIKI_FRAME_ENTRY, &tikicmds)) {
		for (i = 0; i < tikicmds.num_cmds; i++) {
			uir_mp_apply_surface_args(tiki, ent, tikicmds.cmds[i].num_args, tikicmds.cmds[i].args);
		}
	}
}

void UIR_ModelPreviewSetBackend(const uir_modelpreview_backend_t *backend)
{
	if (backend) {
		g_mp = *backend;
	} else {
		memset(&g_mp, 0, sizeof(g_mp));
	}
}

static void uir_mp_init_slots(void)
{
	int i;
	if (g_slotsInited) {
		return;
	}
	for (i = 0; i < UIR_MP_SLOTS; i++) {
		g_slots[i].model = -1;
		g_slots[i].index = -1;
		g_slots[i].variant = -1;
		g_slots[i].requestedVariant = -1;
		g_slots[i].anim[0] = '\0';
		g_slots[i].lastRealtime = -1;
		g_slots[i].animTime = 0.0f;
	}
	g_slotsInited = 1;
}

static int uir_mp_slot_key(int model, const char *anim, int instanceId, int animVariant)
{
	unsigned int key = (unsigned int)model;
	const char   *p;

	key = key * 31u + (unsigned int)instanceId;
	key = key * 31u + (unsigned int)animVariant;
	if (anim) {
		for (p = anim; *p; p++) {
			key = key * 31u + (unsigned char)*p;
		}
	}
	return (int)(key & 0x7fffffff);
}

static uir_mp_anim_slot_t *uir_mp_slot_for(int model, const char *anim, int instanceId, int animVariant)
{
	int i, freeIdx = -1;
	int slotKey;

	uir_mp_init_slots();
	slotKey = uir_mp_slot_key(model, anim, instanceId, animVariant);

	for (i = 0; i < UIR_MP_SLOTS; i++) {
		if (g_slots[i].model == slotKey) {
			return &g_slots[i];
		}
		if (g_slots[i].model == -1 && freeIdx < 0) {
			freeIdx = i;
		}
	}
	if (freeIdx < 0) {
		freeIdx = 0;
	}
	g_slots[freeIdx].model = slotKey;
	g_slots[freeIdx].index = -1;
	g_slots[freeIdx].variant = animVariant;
	g_slots[freeIdx].requestedVariant = animVariant;
	g_slots[freeIdx].lastRealtime = -1;
	g_slots[freeIdx].animTime = 0.0f;
	if (anim) {
		Q_strncpyz(g_slots[freeIdx].anim, anim, sizeof(g_slots[freeIdx].anim));
	} else {
		g_slots[freeIdx].anim[0] = '\0';
	}
	return &g_slots[freeIdx];
}

uir_status_t UIR_ModelPreviewDraw(const uir_rect_t *destPx, const uir_model_preview_params_t *params)
{
	refdef_t    rd;
	refEntity_t ent;
	float       fovX, fovY;
	float       origin[3];
	float       angles[3];
	float       scale;
	float       offset[3];
	float       bboxMins[3];
	float       bboxMaxs[3];
	float       modelScale;
	float       fovIn;
	int         time;
	void       *tiki;

	if (!destPx || !params || destPx->w <= 1.0f || destPx->h <= 1.0f) {
		return UIR_ERR_INVALID_ARG;
	}
	if (!g_mp.clearScene || !g_mp.addRefEntity || !g_mp.renderScene || !g_mp.anglesToAxis) {
		return UIR_ERR_NOT_READY;
	}

	fovIn = params->fov > 0.0f ? params->fov : UIR_MP_FOV_DEFAULT;
	if (UIR_ModelPreviewCalcFov((int)destPx->w, (int)destPx->h, fovIn, &fovX, &fovY) != UIR_OK) {
		return UIR_ERR_INVALID_ARG;
	}

	UIR_ModelPreviewComputeFraming(destPx->w, destPx->h, &scale, offset);
	if (params->hasFramingScale && params->framingScale > 0.0f) {
		scale = params->framingScale;
	}
	if (params->hasOffset) {
		offset[0] = params->offset[0];
		offset[1] = params->offset[1];
		offset[2] = params->offset[2];
	}

	if (params->hasBbox) {
		VectorCopy(params->bboxMins, bboxMins);
		VectorCopy(params->bboxMaxs, bboxMaxs);
	} else if (params->bboxFromModel && g_mp.modelGetHandle && g_mp.modelBounds) {
		tiki = g_mp.modelGetHandle(params->modelHandle);
		modelScale = params->modelScale > 0.0f ? params->modelScale : UIR_MP_MODEL_SCALE_DEFAULT;
		if (tiki) {
			g_mp.modelBounds(tiki, modelScale, bboxMins, bboxMaxs);
		} else {
			bboxMins[0] = bboxMins[1] = bboxMins[2] = 0.0f;
			bboxMaxs[0] = bboxMaxs[1] = bboxMaxs[2] = 0.0f;
		}
	} else {
		bboxMins[0] = bboxMins[1] = bboxMins[2] = 0.0f;
		bboxMaxs[0] = bboxMaxs[1] = bboxMaxs[2] = 0.0f;
	}

	UIR_ModelPreviewComputeOrigin(
		params->hasBbox || params->bboxFromModel ? bboxMins : NULL,
		params->hasBbox || params->bboxFromModel ? bboxMaxs : NULL,
		scale,
		origin
	);
	origin[0] += offset[0];
	origin[1] += offset[1];
	origin[2] += offset[2];

	modelScale = params->modelScale > 0.0f ? params->modelScale : UIR_MP_MODEL_SCALE_DEFAULT;
	origin[0] *= modelScale;
	origin[2] *= modelScale;

	if (params->hasAngles) {
		angles[0] = params->angles[0];
		angles[1] = params->angles[1];
		angles[2] = params->angles[2];
	} else {
		angles[0] = 10.0f;
		angles[1] = 180.0f;
		angles[2] = 0.0f;
	}

	time = params->serverTime ? params->serverTime : params->realtime;

	memset(&rd, 0, sizeof(rd));
	rd.x = (int)destPx->x;
	rd.y = (int)destPx->y;
	rd.width = (int)destPx->w;
	rd.height = (int)destPx->h;
	rd.fov_x = fovX;
	rd.fov_y = fovY;
	rd.time = time;
	rd.rdflags = RDF_HUD | RDF_NOWORLDMODEL;
	rd.viewaxis[0][0] = 1;
	rd.viewaxis[1][1] = 1;
	rd.viewaxis[2][2] = 1;

	memset(&ent, 0, sizeof(ent));
	ent.scale = modelScale;
	ent.hModel = params->modelHandle;
	ent.entityNumber = UIR_MP_ENTITY_BASE + (params->previewSlotId % UIR_MP_ENTITY_COUNT);
	VectorCopy(origin, ent.origin);
	g_mp.anglesToAxis(angles, ent.axis);

	if (params->hasColor) {
		ent.shaderRGBA[0] = (byte)(params->color[0] * 255);
		ent.shaderRGBA[1] = (byte)(params->color[1] * 255);
		ent.shaderRGBA[2] = (byte)(params->color[2] * 255);
		ent.shaderRGBA[3] = (byte)(params->color[3] * 255);
	} else {
		ent.shaderRGBA[0] = ent.shaderRGBA[1] = ent.shaderRGBA[2] = ent.shaderRGBA[3] = 255;
	}

	if (g_mp.modelGetHandle) {
		tiki = g_mp.modelGetHandle(params->modelHandle);
		ent.tiki = (struct dtiki_s *)tiki;
		if (!tiki) {
			return UIR_ERR_MISSING_ASSET;
		}
		if (params->animName && params->animName[0] && g_mp.animNumForNameVariant) {
			uir_mp_anim_slot_t *slot = uir_mp_slot_for(
				params->modelHandle,
				params->animName,
				params->instanceId,
				params->animVariant
			);
			if (Q_stricmp(slot->anim, params->animName) || slot->requestedVariant != params->animVariant) {
				Q_strncpyz(slot->anim, params->animName, sizeof(slot->anim));
				slot->requestedVariant = params->animVariant;
				slot->variant = params->animVariant;
				slot->index = -1;
				slot->lastRealtime = -1;
				slot->animTime = 0.0f;
			}
			if (slot->index < 0) {
				slot->index = g_mp.animNumForNameVariant(tiki, params->animName, params->animVariant);
				slot->lastRealtime = -1;
				slot->animTime = 0.0f;
			}

			if (slot->index >= 0) {
				float animLength = 0.0f;
				float animTime = 0.0f;

				if (g_mp.animTime) {
					animLength = g_mp.animTime(tiki, slot->index);
				}
				if (slot->lastRealtime < 0) {
					slot->animTime = UIR_ModelPreviewPhaseToAnimTime(params->animPhase, animLength);
					slot->lastRealtime = params->realtime;
				} else if (params->realtime >= slot->lastRealtime) {
					slot->animTime += (float)(params->realtime - slot->lastRealtime) * 0.001f;
					slot->lastRealtime = params->realtime;
					/* Fixed in OPM: dontrepeat (e.g. salute.skc) must not wrap —
					 * fall back to looping variant 0 of the same alias group. */
					if (animLength > 0.0f && slot->animTime >= animLength) {
						dtiki_t *ptiki = (dtiki_t *)tiki;
						int      flags = 0;

						if (ptiki && ptiki->a && slot->index < ptiki->a->num_anims
						    && ptiki->a->animdefs[slot->index]) {
							flags = ptiki->a->animdefs[slot->index]->flags;
						}
						if (flags & TAF_NOREPEAT) {
							int loopIdx = g_mp.animNumForNameVariant(tiki, params->animName, 0);

							if (loopIdx >= 0 && loopIdx != slot->index) {
								slot->index = loopIdx;
								slot->variant = 0;
								slot->animTime = 0.0f;
								if (g_mp.animTime) {
									animLength = g_mp.animTime(tiki, slot->index);
								}
							} else {
								slot->animTime = animLength - 0.001f;
								if (slot->animTime < 0.0f) {
									slot->animTime = 0.0f;
								}
							}
						} else {
							UIR_ModelPreviewWrapAnimTime(&slot->animTime, animLength);
						}
					}
				}
				animTime = slot->animTime;
				ent.frameInfo[0].index = slot->index;
				ent.frameInfo[0].time = animTime;
				ent.frameInfo[0].weight = 1.0f;
				ent.actionWeight = 1.0f;
			}
		}
	} else {
		return UIR_ERR_NOT_READY;
	}

	g_mp.clearScene();
	g_mp.addRefEntity(&ent, ENTITYNUM_NONE);
	g_mp.renderScene(&rd);
	return UIR_OK;
}

uir_status_t UIR_ModelBakeBuildScene(
	const uir_model_bake_params_t *params, void *outRefdef, void *outEntity
)
{
	refdef_t    *rd;
	refEntity_t *ent;
	float        fovX, fovY;
	float        origin[3];
	float        scale;
	float        offset[3];
	float        bboxMins[3];
	float        bboxMaxs[3];
	float        modelScale;
	float        fovIn;
	void        *tiki;

	if (!params || !outRefdef || !outEntity || !params->modelHandle) {
		return UIR_ERR_INVALID_ARG;
	}
	if (params->width <= 0 || params->height <= 0) {
		return UIR_ERR_INVALID_ARG;
	}
	if (!g_mp.anglesToAxis || !g_mp.modelGetHandle) {
		return UIR_ERR_NOT_READY;
	}

	rd = (refdef_t *)outRefdef;
	ent = (refEntity_t *)outEntity;

	fovIn = params->fov > 0.0f ? params->fov : UIR_MP_FOV_DEFAULT;
	if (UIR_ModelPreviewCalcFov(params->width, params->height, fovIn, &fovX, &fovY) != UIR_OK) {
		return UIR_ERR_INVALID_ARG;
	}

	{
		float framingOffset[3];

		UIR_ModelPreviewComputeFraming((float)params->width, (float)params->height, &scale, framingOffset);
		if (params->framingScale > 0.0f) {
			scale = params->framingScale;
		}
		offset[0] = framingOffset[0] + params->offset[0];
		offset[1] = framingOffset[1] + params->offset[1];
		offset[2] = framingOffset[2] + params->offset[2];
	}

	tiki = g_mp.modelGetHandle(params->modelHandle);
	modelScale = params->modelScale > 0.0f ? params->modelScale : 1.0f;
	if (tiki && g_mp.modelBounds) {
		uir_mp_bounds_with_load_origin(tiki, modelScale, bboxMins, bboxMaxs);
	} else {
		bboxMins[0] = bboxMins[1] = bboxMins[2] = 0.0f;
		bboxMaxs[0] = bboxMaxs[1] = bboxMaxs[2] = 0.0f;
	}

	{
		/*
		 * Changed in OPM: sharedExtent fixes camera distance across a bake group.
		 * Bounds include TIKI load_origin (tr_model.cpp). Use unrotated bounds center
		 * for origin Y/Z; entity axis handles rotation (oriented center drifted off-screen).
		 */
		UIR_ModelPreviewComputeOriginShared(
			bboxMins,
			bboxMaxs,
			scale,
			params->sharedExtent > 0.0f ? params->sharedExtent : 0.0f,
			NULL,
			origin
		);
	}
	origin[0] += offset[0];
	origin[1] += offset[1];
	origin[2] += offset[2];

	memset(rd, 0, sizeof(*rd));
	rd->width = params->width;
	rd->height = params->height;
	rd->fov_x = fovX;
	rd->fov_y = fovY;
	rd->rdflags = RDF_HUD | RDF_NOWORLDMODEL;
	rd->viewaxis[0][0] = 1;
	rd->viewaxis[1][1] = 1;
	rd->viewaxis[2][2] = 1;

	memset(ent, 0, sizeof(*ent));
	(void)modelScale;
	ent->scale = 1.0f;
	ent->hModel = params->modelHandle;
	ent->entityNumber = UIR_MP_ENTITY_BASE;
	VectorCopy(origin, ent->origin);
	g_mp.anglesToAxis(params->angles, ent->axis);
	ent->shaderRGBA[0] = ent->shaderRGBA[1] = ent->shaderRGBA[2] = ent->shaderRGBA[3] = 255;

	ent->tiki = (struct dtiki_s *)tiki;
	if (!tiki) {
		return UIR_ERR_MISSING_ASSET;
	}

	/* World weapon TIKIs need idle pose; bind pose breaks skinned submeshes (colt45, mp44, …). */
	uir_mp_set_idle_pose(tiki, ent);
	uir_mp_apply_weapon_surface_state((dtiki_t *)tiki, ent);

	return UIR_OK;
}

float UIR_ModelBakeExtent(int modelHandle, float modelScale, const float angles[3])
{
	float mins[3];
	float maxs[3];
	float rmins[3];
	float rmaxs[3];
	float axis[3][3];
	void *tiki;
	float scale;

	if (!modelHandle || !g_mp.modelGetHandle || !g_mp.modelBounds) {
		return 0.0f;
	}
	tiki = g_mp.modelGetHandle(modelHandle);
	if (!tiki) {
		return 0.0f;
	}
	scale = modelScale > 0.0f ? modelScale : 1.0f;
	uir_mp_bounds_with_load_origin(tiki, scale, mins, maxs);
	if (angles && g_mp.anglesToAxis) {
		g_mp.anglesToAxis(angles, axis);
		UIR_ModelPreviewAxisTransformBounds(mins, maxs, axis, rmins, rmaxs);
		return UIR_ModelPreviewBBoxExtent(rmins, rmaxs);
	}
	return UIR_ModelPreviewBBoxExtent(mins, maxs);
}

uir_status_t UIR_ModelBakeToPNG(const uir_model_bake_params_t *params)
{
	refdef_t    rd;
	refEntity_t ent;
	uir_status_t st;

	if (!params || !params->outPath || !params->outPath[0]) {
		return UIR_ERR_INVALID_ARG;
	}
	if (!g_mp.clearScene || !g_mp.addRefEntity || !g_mp.exportModelPreviewPNG) {
		return UIR_ERR_NOT_READY;
	}

	st = UIR_ModelBakeBuildScene(params, &rd, &ent);
	if (st != UIR_OK) {
		return st;
	}

	/* Batch bakes share entity 1008 and one frame_skel_index; force idle pose each time. */
	if (g_mp.forceUpdatePose) {
		g_mp.forceUpdatePose(&ent);
	}

	g_mp.clearScene();
	g_mp.addRefEntity(&ent, ENTITYNUM_NONE);
	if (!g_mp.exportModelPreviewPNG || !g_mp.exportModelPreviewPNG(&rd, params->outPath)) {
		return UIR_ERR_NOT_READY;
	}
	return UIR_OK;
}