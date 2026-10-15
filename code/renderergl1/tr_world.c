/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#include "tr_local.h"

#include <stdlib.h>


/*
=================
R_CullTriSurf

Returns true if the grid is completely culled away.
Also sets the clipped hint bit in tess
=================
*/
static qboolean	R_CullTriSurf( srfTriangles_t *cv ) {
	int 	boxCull;

	boxCull = R_CullLocalBox( cv->bounds );

	if ( boxCull == CULL_OUT ) {
		return qtrue;
	}
	return qfalse;
}

/*
=================
R_CullGrid

Returns true if the grid is completely culled away.
Also sets the clipped hint bit in tess
=================
*/
static qboolean	R_CullGrid( srfGridMesh_t *cv ) {
	int 	boxCull;
	int 	sphereCull;

	if ( r_nocurves->integer ) {
		return qtrue;
	}

	if (!tr.refdef.render_terrain) {
		return qtrue;
	}

	if ( tr.currentEntityNum != ENTITYNUM_WORLD ) {
		sphereCull = R_CullLocalPointAndRadius( cv->localOrigin, cv->meshRadius );
	} else {
		sphereCull = R_CullPointAndRadius( cv->localOrigin, cv->meshRadius );
	}
	boxCull = CULL_OUT;
	
	// check for trivial reject
	if ( sphereCull == CULL_OUT )
	{
		tr.pc.c_sphere_cull_patch_out++;
		return qtrue;
	}
	// check bounding box if necessary
	else if ( sphereCull == CULL_CLIP )
	{
		tr.pc.c_sphere_cull_patch_clip++;

		boxCull = R_CullLocalBox( cv->meshBounds );

		if ( boxCull == CULL_OUT ) 
		{
			tr.pc.c_box_cull_patch_out++;
			return qtrue;
		}
		else if ( boxCull == CULL_IN )
		{
			tr.pc.c_box_cull_patch_in++;
		}
		else
		{
			tr.pc.c_box_cull_patch_clip++;
		}
	}
	else
	{
		tr.pc.c_sphere_cull_patch_in++;
	}

	return qfalse;
}


/*
================
R_CullSurface

Tries to back face cull surfaces before they are lighted or
added to the sorting list.

This will also allow mirrors on both sides of a model without recursion.
================
*/
static qboolean	R_CullSurface( surfaceType_t *surface, shader_t *shader, int planeBits ) {
	srfSurfaceFace_t *sface;
	float			d;
	int				i;

	if ( r_nocull->integer ) {
		return qfalse;
	}

	if ( *surface == SF_GRID ) {
		return R_CullGrid( (srfGridMesh_t *)surface );
	}

	if ( *surface == SF_TRIANGLES ) {
		return R_CullTriSurf( (srfTriangles_t *)surface );
	}

	if ( *surface != SF_FACE ) {
		return qfalse;
	}

	sface = ( srfSurfaceFace_t * ) surface;

	/*
	 * Changed in Omaha: face plane cull before frustum-sphere. Backfaces are
	 * common; one DotProduct rejects them without up to four sphere tests.
	 * Sphere remains only when the leaf is still CLIP'd (planeBits & 15).
	 */
	if ( shader->cullType != CT_TWO_SIDED && r_facePlaneCull->integer ) {
		d = DotProduct( tr.ori.viewOrigin, sface->plane.normal );
		/* don't cull exactly on the plane — BSP/ICD/HW rounding gaps */
		if ( shader->cullType == CT_FRONT_SIDED ) {
			if ( d < sface->plane.dist - 8 ) {
				return qtrue;
			}
		} else {
			if ( d > sface->plane.dist + 8 ) {
				return qtrue;
			}
		}
	}

	if ( !shader->numDeforms && ( planeBits & 15 ) ) {
		for ( i = 0 ; i < 4 ; i++ ) {
			if ( DotProduct( sface->cullOrigin, tr.viewParms.frustum[i].normal ) - tr.viewParms.frustum[i].dist < -sface->cullRadius ) {
				return qtrue;
			}
		}
	}

	return qfalse;
}

/*
======================
R_FastDlightFace
======================
*/
static int R_FastDlightFace( srfSurfaceFace_t *face, int dlightBits ) {
	float		d;
	int			i;
	dlight_t	*dl;

	for ( i = 0 ; i < tr.refdef.num_dlights ; i++ ) {
		if ( ! ( dlightBits & ( 1 << i ) ) ) {
			continue;
		}
		dl = &tr.refdef.dlights[i];
		d = DotProduct( dl->transformed, face->plane.normal ) - face->plane.dist;
		if ( d < -dl->radius || d > dl->radius ) {
			// dlight doesn't reach the plane
			dlightBits &= ~( 1 << i );
		}
	}

	if ( !dlightBits ) {
		tr.pc.c_dlightSurfacesCulled++;
	}

	face->dlightBits[ tr.smpFrame ] = dlightBits;
	return dlightBits;
}

/*
======================
R_FastDlightGrid
======================
*/
static int R_FastDlightGrid( srfGridMesh_t *grid, int dlightBits ) {
	int			i;
	dlight_t	*dl;

	for ( i = 0 ; i < tr.refdef.num_dlights ; i++ ) {
		if ( ! ( dlightBits & ( 1 << i ) ) ) {
			continue;
		}
		dl = &tr.refdef.dlights[i];
		if ( dl->origin[0] - dl->radius > grid->meshBounds[1][0]
			|| dl->origin[0] + dl->radius < grid->meshBounds[0][0]
			|| dl->origin[1] - dl->radius > grid->meshBounds[1][1]
			|| dl->origin[1] + dl->radius < grid->meshBounds[0][1]
			|| dl->origin[2] - dl->radius > grid->meshBounds[1][2]
			|| dl->origin[2] + dl->radius < grid->meshBounds[0][2] ) {
			// dlight doesn't reach the bounds
			dlightBits &= ~( 1 << i );
		}
	}

	if ( !dlightBits ) {
		tr.pc.c_dlightSurfacesCulled++;
	}

	grid->dlightBits[ tr.smpFrame ] = dlightBits;
	return dlightBits;
}

/*
======================
R_FastDlightTerrain
======================
*/
static int R_FastDlightTerrain(cTerraPatchUnpacked_t* srf, int dlightBits) {
    dlight_t* dl;
    float* origin;
    int i;

    for (i = 0; i < tr.refdef.num_dlights; i++) {
        dl = &tr.refdef.dlights[i];
        origin = dl->transformed;

        if (srf->x0 - origin[0] > dl->radius
            || srf->x0 - origin[0] < -512.0 - dl->radius
            || srf->y0 - origin[1] > dl->radius
            || srf->y0 - origin[1] < -512.0 - dl->radius
            || srf->z0 - origin[2] > dl->radius
            || srf->z0 - origin[2] < -srf->zmax - dl->radius
            ) {
            // dlight doesn't reach the bounds
            dlightBits &= ~(1 << i);
        }
    }

    if (!dlightBits) {
        tr.pc.c_dlightSurfacesCulled++;
    }

    srf->drawinfo.dlightBits[tr.smpFrame] = dlightBits;
    return dlightBits;
}

/*
======================
R_DlightTrisurf
======================
*/
static int R_DlightTrisurf( srfTriangles_t *surf, int dlightBits ) {
	// FIXME: more dlight culling to trisurfs...
	surf->dlightBits[ tr.smpFrame ] = dlightBits;
	return dlightBits;
#if 0
	int			i;
	dlight_t	*dl;

	for ( i = 0 ; i < tr.refdef.num_dlights ; i++ ) {
		if ( ! ( dlightBits & ( 1 << i ) ) ) {
			continue;
		}
		dl = &tr.refdef.dlights[i];
		if ( dl->origin[0] - dl->radius > grid->meshBounds[1][0]
			|| dl->origin[0] + dl->radius < grid->meshBounds[0][0]
			|| dl->origin[1] - dl->radius > grid->meshBounds[1][1]
			|| dl->origin[1] + dl->radius < grid->meshBounds[0][1]
			|| dl->origin[2] - dl->radius > grid->meshBounds[1][2]
			|| dl->origin[2] + dl->radius < grid->meshBounds[0][2] ) {
			// dlight doesn't reach the bounds
			dlightBits &= ~( 1 << i );
		}
	}

	if ( !dlightBits ) {
		tr.pc.c_dlightSurfacesCulled++;
	}

	grid->dlightBits[ tr.smpFrame ] = dlightBits;
	return dlightBits;
#endif
}

/*
====================
R_DlightSurface

The given surface is going to be drawn, and it touches a leaf
that is touched by one or more dlights, so try to throw out
more dlights if possible.
====================
*/
static int R_DlightSurface( msurface_t *surf, int dlightBits ) {
	int dlightMap;

	dlightMap = 0;

	if ( *surf->data == SF_FACE ) {
		if (!r_fastdlights->integer) {
			if (surf->shader->lightmapIndex >= 0) {
				dlightMap = R_RealDlightFace((srfSurfaceFace_t*)surf->data, dlightBits);
			} else {
				((srfSurfaceFace_t*)surf->data)->dlightMap[tr.smpFrame] = 0;
				dlightMap = 0;
			}
			return dlightMap;
        } else {
			dlightBits = R_FastDlightFace((srfSurfaceFace_t*)surf->data, dlightBits);
		}
	} else if ( *surf->data == SF_GRID ) {
		if (!r_fastdlights->integer) {
			if (surf->shader->lightmapIndex >= 0) {
				dlightMap = R_RealDlightPatch((srfGridMesh_t*)surf->data, dlightBits);
			} else {
				((srfGridMesh_t*)surf->data)->dlightMap[tr.smpFrame] = 0.f;
				dlightMap = 0;
            }
            return dlightMap;
        } else {
            dlightBits = R_FastDlightGrid((srfGridMesh_t*)surf->data, dlightBits);
		}
	} else {
		return dlightMap;
	}

	if (dlightBits) {
		tr.pc.c_dlightSurfaces++;
		return 1;
	}

	return dlightMap;
}

/*
======================
R_DlightTerrain
======================
*/
int R_DlightTerrain(cTerraPatchUnpacked_t* surf, int dlightBits)
{
	int dlightMap;

    dlightMap = 0;

    if (r_fastdlights->integer)
    {
        if (R_FastDlightTerrain(surf, dlightBits)) {
            ++tr.pc.c_dlightSurfaces;
			dlightMap = 1;
        }
    } else if (surf->shader->lightmapIndex < 0) {
        surf->drawinfo.dlightMap[tr.smpFrame] = 0;
    } else {
		dlightMap = R_RealDlightTerrain(surf, dlightBits);
    }

    return dlightMap;
}

/*
======================
R_CheckDlightSurface
======================
*/
static int R_CheckDlightSurface(msurface_t* surf, int dlightBits)
{
    if (dlightBits && surf->frameCount != tr.frameCount)
    {
        surf->frameCount = tr.frameCount;
        return R_DlightSurface(surf, dlightBits);
    }

	if (dlightBits)
	{
		//
		// Added in 2.0
		//  Return the existing dlight map
		if (*surf->data == SF_FACE)
		{
			return ((srfSurfaceFace_t*)surf->data)->dlightMap[tr.smpFrame];
		}
		else if (*surf->data == SF_GRID)
		{
			return ((srfGridMesh_t*)surf->data)->dlightMap[tr.smpFrame];
		}
	}
	else
	{
		if (*surf->data == SF_FACE)
		{
			((srfSurfaceFace_t*)surf->data)->dlightMap[tr.smpFrame] = 0;
			((srfSurfaceFace_t*)surf->data)->dlightBits[tr.smpFrame] = 0;
		}
		else if (*surf->data == SF_GRID)
		{
			((srfGridMesh_t*)surf->data)->dlightMap[tr.smpFrame] = 0;
			((srfGridMesh_t*)surf->data)->dlightBits[tr.smpFrame] = 0;
		}
	}

    return 0;
}

/*
======================
R_CheckDlightTerrain
======================
*/
int R_CheckDlightTerrain(cTerraPatchUnpacked_t* surf, int dlightBits)
{
	if (surf->frameCount == tr.frameCount) {
		return surf->drawinfo.dlightMap[tr.smpFrame];
	}

	surf->frameCount = tr.frameCount;
	if (dlightBits) {
		return R_DlightTerrain(surf, dlightBits);
	}

	surf->drawinfo.dlightMap[tr.smpFrame] = 0;
	surf->drawinfo.dlightBits[tr.smpFrame] = 0;
	return 0;
}

/*
======================
R_AddWorldSurface
======================
*/
/*
 * Added in Omaha: face-only cull (plane then sphere) — skips grid/tri type tests
 * that R_CullSurface pays on every world face.
 */
static qboolean R_CullWorldFace( srfSurfaceFace_t *sface, shader_t *shader, int planeBits ) {
	float d;
	int   i;

	if ( r_nocull->integer ) {
		return qfalse;
	}

	if ( shader->cullType != CT_TWO_SIDED && r_facePlaneCull->integer ) {
		d = DotProduct( tr.ori.viewOrigin, sface->plane.normal );
		if ( shader->cullType == CT_FRONT_SIDED ) {
			if ( d < sface->plane.dist - 8 ) {
				return qtrue;
			}
		} else {
			if ( d > sface->plane.dist + 8 ) {
				return qtrue;
			}
		}
	}

	if ( !shader->numDeforms && ( planeBits & 15 ) ) {
		for ( i = 0; i < 4; i++ ) {
			if ( DotProduct( sface->cullOrigin, tr.viewParms.frustum[i].normal )
					 - tr.viewParms.frustum[i].dist
				 < -sface->cullRadius ) {
				return qtrue;
			}
		}
	}

	return qfalse;
}

/*
 * Added in Omaha: world leaf-walk replay. At high fps several frames share one
 * client ms, so the camera repeats exactly; the accepted leaves and added surfaces
 * are then identical and can be replayed instead of re-culled.
 */
typedef struct {
	mnode_t    *leaf; /* non-NULL: accepted leaf (bounds, sky node, terrain, decals, statics) */
	msurface_t *surf; /* non-NULL: world surface added to the draw list */
} worldReplayEvent_t;

typedef struct {
	world_t       *world;
	int            viewCluster;
	byte           areamask[MAX_MAP_AREA_BYTES];
	orientationr_t ori;
	float          frustum[5][4];
	int            extrafrustums;
	int            farplane_cull;
	int            flags[9];
} worldReplayKey_t;

static worldReplayEvent_t *s_wrEvents;
static int                 s_wrNumEvents;
static int                 s_wrCapEvents;
static qboolean            s_wrRecording;
static qboolean            s_wrValid;
static worldReplayKey_t    s_wrKey;

static void R_WorldLeafAccept( mnode_t *node );
static void R_WorldLeafExtras( mnode_t *node );
static void R_VisLeafCacheClear( void );

static void R_WorldReplayPush( mnode_t *leaf, msurface_t *surf ) {
	if ( s_wrNumEvents == s_wrCapEvents ) {
		int                 cap = s_wrCapEvents ? s_wrCapEvents * 2 : 4096;
		worldReplayEvent_t *ev  = (worldReplayEvent_t *)realloc( s_wrEvents, (size_t)cap * sizeof( *ev ) );
		if ( !ev ) {
			s_wrRecording = qfalse;
			return;
		}
		s_wrEvents    = ev;
		s_wrCapEvents = cap;
	}
	s_wrEvents[s_wrNumEvents].leaf = leaf;
	s_wrEvents[s_wrNumEvents].surf = surf;
	s_wrNumEvents++;
}

void R_WorldSurfCacheClear( void ) {
	free( s_wrEvents );
	s_wrEvents    = NULL;
	s_wrNumEvents = 0;
	s_wrCapEvents = 0;
	s_wrRecording = qfalse;
	s_wrValid     = qfalse;
	R_VisLeafCacheClear();
}

/*
 * Added in Omaha: dlight bits for a world surface that passed culling. With no
 * leaf dlights, still clear stale face/grid dlightMap/Bits (tess reads those, not
 * only the sort key). Shared by the leaf walk and its replay.
 */
static int R_WorldSurfaceDlights( msurface_t *surf, int dlightBits ) {
	if ( dlightBits ) {
		return R_CheckDlightSurface( surf, dlightBits );
	}

	if ( *surf->data == SF_FACE ) {
		srfSurfaceFace_t *f = (srfSurfaceFace_t *)surf->data;
		if ( f->dlightMap[tr.smpFrame] | f->dlightBits[tr.smpFrame] ) {
			f->dlightMap[tr.smpFrame]  = 0;
			f->dlightBits[tr.smpFrame] = 0;
		}
	} else if ( *surf->data == SF_GRID ) {
		srfGridMesh_t *g = (srfGridMesh_t *)surf->data;
		if ( g->dlightMap[tr.smpFrame] | g->dlightBits[tr.smpFrame] ) {
			g->dlightMap[tr.smpFrame]  = 0;
			g->dlightBits[tr.smpFrame] = 0;
		}
	}
	return 0;
}

static void R_AddWorldSurface( msurface_t *surf, int dlightBits, int planeBits ) {
	shader_t *shader;

	//if ( surf->viewCount == tr.viewCount ) {
	//	return;		// already in this view
	//}

	surf->viewCount = tr.viewCount;
	// FIXME: bmodel fog?

	shader = surf->shader;

	// try to cull before dlighting or adding
	/* Changed in Omaha: hot path for SF_FACE (vast majority of world marksurfs). */
	if ( *surf->data == SF_FACE ) {
		if ( R_CullWorldFace( (srfSurfaceFace_t *)surf->data, shader, planeBits ) ) {
			return;
		}
	} else if ( R_CullSurface( surf->data, shader, planeBits ) ) {
		return;
	}

	dlightBits = R_WorldSurfaceDlights( surf, dlightBits );

	if ( s_wrRecording ) {
		R_WorldReplayPush( NULL, surf );
	}

	if ( shader && shader->isPortalSky ) {
		// Sky portal
		R_Sky_AddSurf( surf );
		return;
	}

	R_AddDrawSurf( surf->data, shader, dlightBits );
}

/*
=============================================================

	BRUSH MODELS

=============================================================
*/

/*
=================
R_AddBrushModelSurfaces
=================
*/
void R_AddBrushModelSurfaces ( trRefEntity_t *ent ) {
	bmodel_t	*bmodel;
	int			clip;
	model_t		*pModel;
	int			i;

	pModel = R_GetModelByHandle( ent->e.hModel );

	bmodel = pModel->d.bmodel;

	clip = R_CullLocalBox( bmodel->bounds );
	if ( clip == CULL_OUT ) {
		return;
	}
	
	R_DlightBmodel( bmodel );

	for ( i = 0 ; i < bmodel->numSurfaces ; i++ ) {
		R_AddWorldSurface( bmodel->firstSurface + i, tr.currentEntity->needDlights, 0 );
	}
}


/*
=============================================================

	WORLD MODEL

=============================================================
*/

void R_GetInlineModelBounds(int iIndex, vec3_t vMins, vec3_t vMaxs)
{
	bmodel_t* bmodel;

	bmodel = &tr.world->bmodels[iIndex];
	VectorCopy(bmodel->bounds[0], vMins);
	VectorCopy(bmodel->bounds[1], vMaxs);
}

/* Added in Omaha: flat PVS leaf list — skip BSP internal-node BoxOnPlaneSide walks
 * that dominated R_RecursiveWorldNode cost when the frustum kept planeBits CLIP. */
static mnode_t **s_visLeaves;
static int       s_numVisLeaves;
static int       s_visLeavesCap;
static int       s_visLeavesVisCount;

/*
 * Added in Omaha: runs of consecutive PVS leaves (BSP order, spatially coherent)
 * with their union bounds. A leaf box lies inside its group box, and
 * BoxOnPlaneSide is monotonic in the box corners, so a group that is fully
 * outside (or fully inside) a frustum plane gives the same answer for every
 * leaf in it — same accepted leaves, planeBits and draw order as testing each.
 */
#define VIS_LEAF_GROUP 8

typedef struct {
	vec3_t mins;
	vec3_t maxs;
	int    first;
	int    count;
} visLeafGroup_t;

static visLeafGroup_t *s_visGroups;
static int             s_numVisGroups;
static int             s_visGroupsCap;

/* Fixed in Omaha: the list holds mnode_t pointers into the loaded world. A map
 * reload (demo rewind) reallocates nodes while tr.world keeps the same slot
 * address and tr.visCount may not advance, so the stale list walked the old
 * world: inline brush surfaces leaked in as world (ghost props near origin). */
static void R_VisLeafCacheClear( void ) {
	s_numVisLeaves      = 0;
	s_numVisGroups      = 0;
	s_visLeavesVisCount = -1;
}

static void R_RebuildVisLeafGroups( void ) {
	int i, j;

	if ( s_visGroupsCap * VIS_LEAF_GROUP < s_numVisLeaves ) {
		if ( s_visGroups ) {
			free( s_visGroups );
		}
		s_visGroupsCap = ( s_visLeavesCap + VIS_LEAF_GROUP - 1 ) / VIS_LEAF_GROUP;
		s_visGroups    = (visLeafGroup_t *)malloc( (size_t)s_visGroupsCap * sizeof( *s_visGroups ) );
	}

	s_numVisGroups = 0;
	for ( i = 0; i < s_numVisLeaves; i += VIS_LEAF_GROUP ) {
		visLeafGroup_t *g = &s_visGroups[s_numVisGroups++];

		g->first = i;
		g->count = s_numVisLeaves - i < VIS_LEAF_GROUP ? s_numVisLeaves - i : VIS_LEAF_GROUP;
		VectorCopy( s_visLeaves[i]->mins, g->mins );
		VectorCopy( s_visLeaves[i]->maxs, g->maxs );
		for ( j = 1; j < g->count; j++ ) {
			AddPointToBounds( s_visLeaves[i + j]->mins, g->mins, g->maxs );
			AddPointToBounds( s_visLeaves[i + j]->maxs, g->mins, g->maxs );
		}
	}
}

static void R_RebuildVisLeafList( void ) {
	int      i;
	mnode_t *node;

	if ( !tr.world ) {
		s_numVisLeaves      = 0;
		s_numVisGroups      = 0;
		s_visLeavesVisCount = -1;
		return;
	}

	if ( s_visLeavesCap < tr.world->numnodes ) {
		if ( s_visLeaves ) {
			free( s_visLeaves );
		}
		s_visLeavesCap = tr.world->numnodes;
		s_visLeaves    = (mnode_t **)malloc( (size_t)s_visLeavesCap * sizeof( mnode_t * ) );
	}

	s_numVisLeaves = 0;
	for ( i = 0, node = tr.world->nodes; i < tr.world->numnodes; i++, node++ ) {
		if ( node->visframe != tr.visCount ) {
			continue;
		}
		if ( node->contents == -1 ) {
			continue; /* decision node */
		}
		s_visLeaves[s_numVisLeaves++] = node;
	}
	s_visLeavesVisCount = tr.visCount;
	R_RebuildVisLeafGroups();
}

/*
 * Changed in Omaha: cull dlights against leaf AABB once (recursive walk did this
 * while descending; flat walk must do it per leaf).
 */
static int R_WorldLeafDlights( const mnode_t *node, int dlightBits ) {
	int      di;
	dlight_t *dl;

	if ( !dlightBits ) {
		return 0;
	}
	for ( di = 0; di < tr.refdef.num_dlights; di++ ) {
		if ( !( dlightBits & ( 1 << di ) ) ) {
			continue;
		}
		dl = &tr.refdef.dlights[di];
		if ( dl->origin[0] - dl->radius > node->maxs[0]
			 || dl->origin[0] + dl->radius < node->mins[0]
			 || dl->origin[1] - dl->radius > node->maxs[1]
			 || dl->origin[1] + dl->radius < node->mins[1]
			 || dl->origin[2] - dl->radius > node->maxs[2]
			 || dl->origin[2] + dl->radius < node->mins[2] ) {
			dlightBits &= ~( 1 << di );
		}
	}
	return dlightBits;
}

/*
================
R_AddWorldLeafSurfaces

Frustum-cull one PVS leaf and add its marksurfaces (was RecursiveWorldNode leaf body).
Changed in Omaha: knownInBits are planes its leaf group is already fully in front of.
================
*/
static void R_AddWorldLeafSurfaces( mnode_t *node, int dlightBits, int knownInBits ) {
	int         planeBits;
	int         c;
	msurface_t *surf, **mark;
	int         r;

	planeBits = ( tr.viewParms.fog.extrafrustums ? 31 : 15 ) & ~knownInBits;

	if ( !r_nocull->integer ) {
		if ( planeBits & 1 ) {
			r = BoxOnPlaneSide( node->mins, node->maxs, &tr.viewParms.frustum[0] );
			if ( r == 2 ) {
				return;
			}
			if ( r == 1 ) {
				planeBits &= ~1;
			}
		}
		if ( planeBits & 2 ) {
			r = BoxOnPlaneSide( node->mins, node->maxs, &tr.viewParms.frustum[1] );
			if ( r == 2 ) {
				return;
			}
			if ( r == 1 ) {
				planeBits &= ~2;
			}
		}
		if ( planeBits & 4 ) {
			r = BoxOnPlaneSide( node->mins, node->maxs, &tr.viewParms.frustum[2] );
			if ( r == 2 ) {
				return;
			}
			if ( r == 1 ) {
				planeBits &= ~4;
			}
		}
		if ( planeBits & 8 ) {
			r = BoxOnPlaneSide( node->mins, node->maxs, &tr.viewParms.frustum[3] );
			if ( r == 2 ) {
				return;
			}
			if ( r == 1 ) {
				planeBits &= ~8;
			}
		}
		if ( ( planeBits & 16 )
			 && ( tr.viewParms.farplane_cull < FARPLANE_CULL_PORTALSKY || tr.viewParms.isPortalSky ) ) {
			r = BoxOnPlaneSide( node->mins, node->maxs, &tr.viewParms.frustum[4] );
			if ( r == 2 ) {
				return;
			}
			if ( r == 1 ) {
				planeBits &= ~16;
			}
		}
	}


	if ( s_wrRecording ) {
		R_WorldReplayPush( node, NULL );
	}
	R_WorldLeafAccept( node );

	dlightBits = R_WorldLeafDlights( node, dlightBits );

	if ( r_drawbrushes->integer ) {
		const int worldSurfCount =
			(tr.world && tr.world->numBmodels > 0) ? tr.world->bmodels[0].numSurfaces : 0;
		mark = node->firstmarksurface;
		c    = node->nummarksurfaces;
		while ( c-- ) {
			surf = *mark;
			/* Fixed in Omaha: leaf marklists include inline brush-model surfaces
			 * (func_crate / func_barrel). Their verts are local-space; drawing them
			 * as world (identity ori) parks ghosts near the map origin. Those
			 * surfaces are only valid via R_AddBrushModelSurfaces. */
			if (worldSurfCount > 0) {
				const int surfIndex = (int)(surf - tr.world->surfaces);
				if (surfIndex >= worldSurfCount) {
					mark++;
					continue;
				}
			}
			if ( surf->viewCount != tr.viewCount ) {
				R_AddWorldSurface( surf, dlightBits, planeBits );
			}
			mark++;
		}
	}

	R_WorldLeafExtras( node );
}

/*
================
R_WorldLeafAccept / R_WorldLeafExtras

Added in Omaha: per-leaf side effects of the world walk, shared with the replay.
================
*/
static void R_WorldLeafAccept( mnode_t *node ) {
	tr.pc.c_leafs++;

	if ( node->mins[0] < tr.viewParms.visBounds[0][0] ) {
		tr.viewParms.visBounds[0][0] = node->mins[0];
	}
	if ( node->mins[1] < tr.viewParms.visBounds[0][1] ) {
		tr.viewParms.visBounds[0][1] = node->mins[1];
	}
	if ( node->mins[2] < tr.viewParms.visBounds[0][2] ) {
		tr.viewParms.visBounds[0][2] = node->mins[2];
	}
	if ( node->maxs[0] > tr.viewParms.visBounds[1][0] ) {
		tr.viewParms.visBounds[1][0] = node->maxs[0];
	}
	if ( node->maxs[1] > tr.viewParms.visBounds[1][1] ) {
		tr.viewParms.visBounds[1][1] = node->maxs[1];
	}
	if ( node->maxs[2] > tr.viewParms.visBounds[1][2] ) {
		tr.viewParms.visBounds[1][2] = node->maxs[2];
	}

	tr.portalsky.cntNode = node;
}

static void R_WorldLeafExtras( mnode_t *node ) {
	if ( r_drawterrain->integer && tr.refdef.render_terrain && !tr.viewParms.isPortalSky ) {
		int i;
		for ( i = 0; i < node->numTerraPatches; i++ ) {
			R_MarkTerrainPatch( tr.world->visTerraPatches[node->firstTerraPatch + i] );
		}
	}

	if ( r_drawstaticdecals->integer ) {
		if ( node->pFirstMarkFragment ) {
			R_AddPermanentMarkFragmentSurfaces( node->pFirstMarkFragment, node->iNumMarkFragment );
		}
	}

	if ( r_drawstaticmodels->integer ) {
		int i;
		for ( i = 0; i < node->numStaticModels; i++ ) {
			tr.world->visStaticModels[node->firstStaticModel + i]->visCount = tr.visCount;
		}
	}
}

/*
================
R_AddWorldSurfacesFromVisLeaves

Added in Omaha: replace BSP recursion with a linear PVS-leaf pass.
================
*/
static void R_AddWorldSurfacesFromVisLeaves( int dlightBits ) {
	int i, j;

	if ( s_visLeavesVisCount != tr.visCount || !s_visLeaves ) {
		R_RebuildVisLeafList();
	}

	if ( !r_worldLeafGroups->integer || r_nocull->integer ) {
		for ( i = 0; i < s_numVisLeaves; i++ ) {
			R_AddWorldLeafSurfaces( s_visLeaves[i], dlightBits, 0 );
		}
		return;
	}

	/* Added in Omaha: group pre-cull; same plane set and order as R_AddWorldLeafSurfaces */
	for ( i = 0; i < s_numVisGroups; i++ ) {
		const visLeafGroup_t *g        = &s_visGroups[i];
		const int             numPlanes = tr.viewParms.fog.extrafrustums ? 5 : 4;
		int                   knownIn   = 0;
		qboolean              out       = qfalse;

		for ( j = 0; j < numPlanes; j++ ) {
			int r;

			if ( j == 4 && !( tr.viewParms.farplane_cull < FARPLANE_CULL_PORTALSKY || tr.viewParms.isPortalSky ) ) {
				break;
			}
			r = BoxOnPlaneSide( g->mins, g->maxs, &tr.viewParms.frustum[j] );
			if ( r == 2 ) {
				out = qtrue;
				break;
			}
			if ( r == 1 ) {
				knownIn |= 1 << j;
			}
		}
		if ( out ) {
			continue;
		}
		for ( j = 0; j < g->count; j++ ) {
			R_AddWorldLeafSurfaces( s_visLeaves[g->first + j], dlightBits, knownIn );
		}
	}
}

/*
================
R_BuildWorldReplayKey

Added in Omaha: everything the leaf walk's accept/cull decisions read. Same key
means the same leaves and surfaces in the same order. Dlights are not part of
the key: the replay re-runs the per-leaf and per-surface dlight steps.
================
*/
static void R_BuildWorldReplayKey( worldReplayKey_t *key ) {
	int i;

	memset( key, 0, sizeof( *key ) );
	key->world       = tr.world;
	key->viewCluster = tr.viewCluster;
	memcpy( key->areamask, tr.refdef.areamask, sizeof( key->areamask ) );
	key->ori = tr.viewParms.ori;
	for ( i = 0; i < 5; i++ ) {
		VectorCopy( tr.viewParms.frustum[i].normal, key->frustum[i] );
		key->frustum[i][3] = tr.viewParms.frustum[i].dist;
	}
	key->extrafrustums = tr.viewParms.fog.extrafrustums;
	key->farplane_cull = tr.viewParms.farplane_cull;
	key->flags[0]      = r_nocull->integer;
	key->flags[1]      = r_facePlaneCull->integer;
	key->flags[2]      = r_drawbrushes->integer;
	key->flags[3]      = r_drawterrain->integer && tr.refdef.render_terrain;
	key->flags[4]      = r_drawstaticdecals->integer;
	key->flags[5]      = r_drawstaticmodels->integer;
	key->flags[6]      = r_novis->integer;
	key->flags[7]      = tr.refdef.rdflags;
	key->flags[8]      = tr.numShaders;
}

/*
================
R_ReplayWorldLeafWalk

Added in Omaha: re-issue the recorded walk without frustum, plane or sphere culls;
dlight masks and per-surface dlight state are recomputed for this frame.
================
*/
static void R_ReplayWorldLeafWalk( int dlightBits ) {
	const worldReplayEvent_t *ev  = s_wrEvents;
	const worldReplayEvent_t *end = s_wrEvents + s_wrNumEvents;
	int                       leafDlights = 0;

	for ( ; ev < end; ev++ ) {
		msurface_t *surf;
		int         surfDlights;

		if ( ev->leaf ) {
			R_WorldLeafAccept( ev->leaf );
			leafDlights = R_WorldLeafDlights( ev->leaf, dlightBits );
			R_WorldLeafExtras( ev->leaf );
			continue;
		}

		surf            = ev->surf;
		surf->viewCount = tr.viewCount;
		surfDlights     = R_WorldSurfaceDlights( surf, leafDlights );
		if ( surf->shader && surf->shader->isPortalSky ) {
			R_Sky_AddSurf( surf );
		} else {
			R_AddDrawSurf( surf->data, surf->shader, surfDlights );
		}
	}
}

/*
================
R_AddWorldSurfacesCached

Added in Omaha: main-view leaf walk, replayed when the view repeats exactly.
================
*/
static void R_AddWorldSurfacesCached( void ) {
	worldReplayKey_t key;
	const int        dlightBits = ( 1 << tr.refdef.num_dlights ) - 1;

	if ( !r_worldSurfCache->integer || tr.viewParms.isPortalSky || r_lockpvs->integer ) {
		s_wrValid = qfalse;
		R_AddWorldSurfacesFromVisLeaves( dlightBits );
		return;
	}

	R_BuildWorldReplayKey( &key );
	if ( s_wrValid && !memcmp( &key, &s_wrKey, sizeof( key ) ) ) {
		R_ReplayWorldLeafWalk( dlightBits );
		return;
	}

	s_wrNumEvents = 0;
	s_wrRecording = qtrue;
	R_AddWorldSurfacesFromVisLeaves( dlightBits );
	s_wrValid     = s_wrRecording;
	s_wrRecording = qfalse;
	s_wrKey       = key;
}

int R_SphereInLeafs(const vec3_t p, float r, mnode_t** nodes, int nMaxNodes) {
	mnode_t* nodestack[1024];
	int iNodeStackPos;
	mnode_t* pCurNode;
	int nFoundNodes;

	iNodeStackPos = 0;
	pCurNode = tr.world->nodes;
	nFoundNodes = 0;

	while (1)
    {
        cplane_t* plane;
		float d;

		while (pCurNode->contents == -1)
		{
			plane = pCurNode->plane;
			if (plane->type >= PLANE_NON_AXIAL) {
				d = DotProduct(p, plane->normal) - plane->dist;
			} else {
				d = p[plane->type] - plane->dist;
			}

            if (d < r) {
                if (d > -r) {
                    nodestack[iNodeStackPos++] = pCurNode->children[0];
                }
                pCurNode = pCurNode->children[1];
            } else {
                pCurNode = pCurNode->children[0];
			}
		}

		nodes[nFoundNodes++] = pCurNode;
		if (!iNodeStackPos || nFoundNodes == nMaxNodes) {
			break;
		}

		iNodeStackPos--;
        pCurNode = nodestack[iNodeStackPos];
	}

	return nFoundNodes;
}
/*
===============
R_PointInLeaf
===============
*/
mnode_t *R_PointInLeaf( const vec3_t p ) {
	mnode_t		*node;
	float		d;
	cplane_t	*plane;
	
	if ( !tr.world ) {
		ri.Error (ERR_DROP, "R_PointInLeaf: bad model");
	}
	
	node = tr.world->nodes;
	while( 1 ) {
		if (node->contents != -1) {
			break;
		}
		plane = node->plane;
		d = DotProduct (p,plane->normal) - plane->dist;
		if (d > 0) {
			node = node->children[0];
		} else {
			node = node->children[1];
		}
	}
	
	return node;
}

/*
==============
R_ClusterPVS
==============
*/
static const byte *R_ClusterPVS (int cluster) {
	if (!tr.world || !tr.world->vis || cluster < 0 || cluster >= tr.world->numClusters ) {
		return tr.world->novis;
	}

	return tr.world->vis + cluster * tr.world->clusterBytes;
}

/*
=================
R_inPVS
=================
*/
qboolean R_inPVS( const vec3_t p1, const vec3_t p2 ) {
	mnode_t *leaf;
	byte	*vis;

	leaf = R_PointInLeaf( p1 );
	vis = ri.CM_ClusterPVS( leaf->cluster );
	leaf = R_PointInLeaf( p2 );

	if ( !(vis[leaf->cluster>>3] & (1<<(leaf->cluster&7))) ) {
		return qfalse;
	}
	return qtrue;
}

/*
===============
R_MarkLeaves

Mark the leaves and nodes that are in the PVS for the current
cluster
===============
*/
static void R_MarkLeaves (void) {
	const byte	*vis;
	mnode_t	*leaf, *parent;
	int		i;
	int		cluster;

	// lockpvs lets designers walk around to determine the
	// extent of the current pvs
	if ( r_lockpvs->integer ) {
		return;
	}

	// current viewcluster
	leaf = R_PointInLeaf( tr.viewParms.pvsOrigin );
	cluster = leaf->cluster;

	// if the cluster is the same and the area visibility matrix
	// hasn't changed, we don't need to mark everything again
	//
	// Fixed in Omaha: viewCluster == -1 is the "force remark" sentinel used after
	// map/menu-world loads. Cameras outside the world also report cluster -1, so
	// treating (-1 == -1) as a hit skips marking forever (Algiers / Southern France
	// menu backdrops: brushes vanish, static models still draw).
	if ( tr.viewCluster == cluster && tr.viewCluster != -1
		&& !tr.refdef.areamaskModified
		&& !r_showcluster->modified ) {
		return;
	}

	if ( r_showcluster->modified || r_showcluster->integer ) {
		r_showcluster->modified = qfalse;
		if ( r_showcluster->integer ) {
			ri.Printf( PRINT_ALL, "cluster:%i  area:%i\n", cluster, leaf->area );
		}
	}

	tr.visCount++;
	tr.viewCluster = cluster;

	if ( r_novis->integer || tr.viewCluster == -1 ) {
		for (i=0 ; i<tr.world->numnodes ; i++) {
			if (tr.world->nodes[i].contents != CONTENTS_SOLID) {
				tr.world->nodes[i].visframe = tr.visCount;
			}
		}
		R_RebuildVisLeafList(); /* Added in Omaha */
		return;
	}

	vis = R_ClusterPVS (tr.viewCluster);
	
	for (i=0,leaf=tr.world->nodes ; i<tr.world->numnodes ; i++, leaf++) {
		cluster = leaf->cluster;
		if ( cluster < 0 || cluster >= tr.world->numClusters ) {
			continue;
		}

		// check general pvs
		if ( !(vis[cluster>>3] & (1<<(cluster&7))) ) {
			continue;
		}

		// check for door connection
		if ( (tr.refdef.areamask[leaf->area>>3] & (1<<(leaf->area&7)) ) ) {
			continue;		// not visible
		}

		parent = leaf;
		do {
			if (parent->visframe == tr.visCount)
				break;
			parent->visframe = tr.visCount;
			parent = parent->parent;
		} while (parent);
	}

	R_RebuildVisLeafList(); /* Added in Omaha */
}


/*
=============
R_AddWorldSurfaces
=============
*/
void R_AddWorldSurfaces (void) {
	if (!r_drawworld->integer) {
		return;
	}

	if (tr.refdef.rdflags & RDF_NOWORLDMODEL) {
		return;
	}

	tr.currentEntityNum = ENTITYNUM_WORLD;
	tr.shiftedEntityNum = tr.currentEntityNum << QSORT_ENTITYNUM_SHIFT;

	if (r_drawterrain->integer && tr.refdef.render_terrain && !tr.viewParms.isPortalSky) {
		R_TerrainPrepareFrame();
	}

	// determine which leaves are in the PVS / areamask
	R_MarkLeaves();

	// clear out the visible min/max
	ClearBounds(tr.viewParms.visBounds[0], tr.viewParms.visBounds[1]);

	// perform frustum culling and add all the potentially visible surfaces
	if (tr.refdef.num_dlights > 32) {
		tr.refdef.num_dlights = 32;
	}
	R_TransformDlights(tr.refdef.num_dlights, tr.refdef.dlights, &tr.viewParms.world);
	/* Changed in Omaha: flat PVS leaf pass (was R_RecursiveWorldNode), replayed
	 * when the view repeats */
	R_AddWorldSurfacesCached();

	if (r_drawterrain->integer && tr.refdef.render_terrain && !tr.viewParms.isPortalSky) {
		R_AddTerrainSurfaces();
	}
	if (r_drawstaticmodels->integer) {
		R_AddStaticModelSurfaces();
	}

	if (g_bInfostaticmodels) {
		g_bInfostaticmodels = 0;
		R_PrintInfoStaticModels();
	}

	R_UpdateLevelMarksSystem();
}
