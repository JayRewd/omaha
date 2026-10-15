/*
===========================================================================
Copyright (C) 2015-2024 the OpenMoHAA team

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
// tr_light.c

#include "tr_local.h"

#define DLIGHT_AT_RADIUS 16
// at the edge of a dlight's influence, this amount of light will be added

#define DLIGHT_MINIMUM_RADIUS 16

// never calculate a range less than this to prevent huge light numbers

typedef struct {
    dlight_t *dl;
    float     power;
    vec3_t    origin;
} incidentLight_t;

typedef struct {
    int             dlightMap;
    int             allocated[LIGHTMAP_SIZE];
    byte            lightmap_buffer[LIGHTMAP_SIZE * LIGHTMAP_SIZE * 4];
    byte           *srcBase;
    byte           *dstBase;
    incidentLight_t lights[32];
    int             numLights;
} dlightInfo_t;

typedef struct {
    vec3_t point;
    int    s;
    int    t;
} patchLightBlock_t;

dlightInfo_t dli;

void     R_SetupEntityLightingGrid(trRefEntity_t *ent);
qboolean R_DlightSample(byte *src, const vec3_t vec, byte *dst);
qboolean R_AllocLMBlock(int w, int h, int *x, int *y);

/*
 * Added in Omaha: dlight texel cache + upload skip (r_dlightCache).
 *
 * A dlit face's texels depend only on its static lightmap and the ordered list of
 * lights reaching it (transformed origin, radius, color). At high fps the same
 * light set repeats for several frames (client time is in ms), for every dlight
 * — local or remote muzzle flashes alike — so the texels are cached per face.
 *
 * Every write into dli.lightmap_buffer is also recorded. When a scene's write
 * sequence matches the previous scene's (and that one matched the one before),
 * the buffer holds exactly what was last uploaded to each atlas, including
 * unallocated filler texels, so the glTexSubImage2D is skipped.
 */
#define DLIGHT_CACHE_SIZE       2048
#define DLIGHT_CACHE_WAYS       4
#define DLIGHT_CACHE_MAX_LIGHTS 8
#define DLIGHT_CACHE_POOL       (4 * 1024 * 1024)
#define DLIGHT_MAX_WRITES       1024
#define DLIGHT_MAX_ATLASES      15

typedef struct {
    const void   *surf;
    const byte   *lmData;
    int           w, h;
    int           numLights;
    int           overbrightShift;
    float         lights[DLIGHT_CACHE_MAX_LIGHTS][7];
    qboolean      added;
    qboolean      valid;
    unsigned int  serial;
    byte         *texels; // faces: the whole lmWidth x lmHeight block
    int          *ops;    // patches / terrain: recorded (byte offset, rgba) writes, in order
    int           numOps;
    int           maxOps;
    int           lastUse;
} dlightFaceCache_t;

typedef struct {
    const void  *surf;
    int          atlas;
    int          x, y;
    unsigned int serial;
} dlightWrite_t;

typedef struct {
    int numWrites;
    int h;
    int texnum;
} dlightUpload_t;

static dlightFaceCache_t s_dlCache[DLIGHT_CACHE_SIZE];
static int               s_dlPoolStore[DLIGHT_CACHE_POOL / sizeof(int)];
static byte *const       s_dlPool = (byte *)s_dlPoolStore;
static int               s_dlPoolUsed;
static unsigned int      s_dlSerial;
static int               s_dlScene;

static dlightWrite_t  s_dlWrites[2][DLIGHT_MAX_WRITES];
static int            s_dlNumWrites[2];
static qboolean       s_dlOverflow[2];
static dlightUpload_t s_dlUploads[2][DLIGHT_MAX_ATLASES];
static int            s_dlCur;
static qboolean       s_dlPrevStable;
static qboolean       s_dlPrefixMatch;

static dlightFaceCache_t *s_dlRecord;
static const byte        *s_dlRecordBase;

static void R_DlightCacheClear(void);

#define DLIGHT_MAX_INPLACE 1024

static srfSurfaceFace_t *s_ipFaces[DLIGHT_MAX_INPLACE];
static int               s_ipNumFaces;
static const world_t    *s_ipWorld;

void R_FlushDlightCache()
{
    // faces / lightmaps are being replaced; nothing left to restore
    s_ipNumFaces = 0;
    s_ipWorld    = NULL;
    R_DlightCacheClear();
}

static void R_DlightCacheClear(void)
{
    int i;

    Com_Memset(s_dlCache, 0, sizeof(s_dlCache));
    s_dlPoolUsed = 0;

    for (i = 0; i < 2; i++) {
        s_dlNumWrites[i] = 0;
        s_dlOverflow[i]  = qtrue;
        Com_Memset(s_dlUploads[i], 0, sizeof(s_dlUploads[i]));
    }
    s_dlPrevStable  = qfalse;
    s_dlPrefixMatch = qfalse;
}

static void R_DlightNoteWrite(const void *surf, int x, int y, unsigned int serial)
{
    const int      cur = s_dlCur;
    dlightWrite_t *w;
    int            n;

    if (!serial) {
        // uncached content: treat as never repeating
        s_dlOverflow[cur] = qtrue;
        s_dlPrefixMatch   = qfalse;
        return;
    }

    n = s_dlNumWrites[cur];
    if (n >= DLIGHT_MAX_WRITES) {
        s_dlOverflow[cur] = qtrue;
        s_dlPrefixMatch   = qfalse;
        return;
    }

    w         = &s_dlWrites[cur][n];
    w->surf   = surf;
    w->atlas  = dli.dlightMap;
    w->x      = x;
    w->y      = y;
    w->serial = serial;
    s_dlNumWrites[cur] = n + 1;

    if (s_dlPrefixMatch) {
        const dlightWrite_t *p = &s_dlWrites[!cur][n];
        if (n >= s_dlNumWrites[!cur] || p->surf != surf || p->atlas != w->atlas || p->x != x || p->y != y
            || p->serial != serial) {
            s_dlPrefixMatch = qfalse;
        }
    }
}

static void R_DlightBeginScene()
{
    const int prev = !s_dlCur;
    const int cur  = s_dlCur;

    // the scene that just ended becomes "previous"; stable if it repeated the one before
    s_dlPrevStable = !s_dlOverflow[cur] && !s_dlOverflow[prev] && s_dlNumWrites[cur] == s_dlNumWrites[prev]
                  && !memcmp(s_dlWrites[cur], s_dlWrites[prev], s_dlNumWrites[cur] * sizeof(dlightWrite_t));

    s_dlScene++;
    s_dlCur = prev;
    s_dlNumWrites[prev] = 0;
    s_dlOverflow[prev]  = !r_dlightCache->integer;
    Com_Memset(s_dlUploads[prev], 0, sizeof(s_dlUploads[prev]));
    s_dlPrefixMatch = r_dlightCache->integer != 0;
}

static dlightFaceCache_t *
R_DlightCacheFind(const void *srf, const byte *lmData, int w, int h, int maxOps, qboolean *hit)
{
    dlightFaceCache_t *e, *set;
    size_t             slot;
    int                k;

    *hit = qfalse;
    if (dli.numLights > DLIGHT_CACHE_MAX_LIGHTS) {
        return NULL;
    }

    slot = ((size_t)srf >> 4) ^ ((size_t)srf >> 14);
    set  = &s_dlCache[(slot & (DLIGHT_CACHE_SIZE / DLIGHT_CACHE_WAYS - 1)) * DLIGHT_CACHE_WAYS];
    e    = set;
    for (k = 0; k < DLIGHT_CACHE_WAYS; k++) {
        if (set[k].surf == srf) {
            e = &set[k];
            break;
        }
        if (set[k].lastUse < e->lastUse) {
            e = &set[k];
        }
    }
    e->lastUse = s_dlScene;

    if (e->surf == srf && e->valid && e->lmData == lmData && e->numLights == dli.numLights
        && e->overbrightShift == tr.overbrightShift) {
        for (k = 0; k < dli.numLights; k++) {
            const incidentLight_t *l = &dli.lights[k];
            const float           *c = e->lights[k];
            if (c[0] != l->origin[0] || c[1] != l->origin[1] || c[2] != l->origin[2] || c[3] != l->dl->radius
                || c[4] != l->dl->color[0] || c[5] != l->dl->color[1] || c[6] != l->dl->color[2]) {
                break;
            }
        }
        if (k == dli.numLights) {
            *hit = qtrue;
            return e;
        }
    }

    if (e->surf != srf || e->lmData != lmData || e->w != w || e->h != h || e->maxOps != maxOps) {
        const int size = maxOps ? maxOps * 2 * (int)sizeof(int) : w * h * 4;
        if (size > DLIGHT_CACHE_POOL / 16) {
            e->surf = NULL;
            return NULL;
        }
        if (s_dlPoolUsed + size > DLIGHT_CACHE_POOL) {
            R_DlightCacheClear();
            // the flush cleared the scene's write record; it can no longer match
            s_dlOverflow[s_dlCur] = qtrue;
        }
        e->surf   = srf;
        e->lmData = lmData;
        e->w      = w;
        e->h      = h;
        e->maxOps = maxOps;
        e->texels = maxOps ? NULL : &s_dlPool[s_dlPoolUsed];
        e->ops    = maxOps ? (int *)&s_dlPool[s_dlPoolUsed] : NULL;
        s_dlPoolUsed += size;
    }

    e->valid           = qtrue;
    e->numOps          = 0;
    e->numLights       = dli.numLights;
    e->overbrightShift = tr.overbrightShift;
    for (k = 0; k < dli.numLights; k++) {
        const incidentLight_t *l = &dli.lights[k];
        float                 *c = e->lights[k];
        c[0] = l->origin[0];
        c[1] = l->origin[1];
        c[2] = l->origin[2];
        c[3] = l->dl->radius;
        c[4] = l->dl->color[0];
        c[5] = l->dl->color[1];
        c[6] = l->dl->color[2];
    }
    e->serial = ++s_dlSerial;
    if (!e->serial) {
        e->serial = ++s_dlSerial;
    }

    return e;
}

static void R_DlightRecordBegin(dlightFaceCache_t *e, const byte *base)
{
    s_dlRecord     = e;
    s_dlRecordBase = base;
}

static void R_DlightRecordEnd(int added)
{
    if (s_dlRecord) {
        s_dlRecord->added = added;
        s_dlRecord        = NULL;
    }
}

static void R_DlightRecordWrite(const byte *dst)
{
    dlightFaceCache_t *e = s_dlRecord;
    int               *op;

    if (e->numOps >= e->maxOps) {
        e->valid = qfalse;
        return;
    }
    op    = &e->ops[e->numOps * 2];
    op[0] = (int)(dst - s_dlRecordBase);
    Com_Memcpy(&op[1], dst, 4);
    e->numOps++;
}

static int R_DlightReplay(const dlightFaceCache_t *e, byte *base)
{
    const int *op = e->ops;
    int        n;

    for (n = 0; n < e->numOps; n++, op += 2) {
        Com_Memcpy(base + op[0], &op[1], 4);
    }
    return e->added;
}

/*
 * Added in Omaha: r_dlightInPlace. A lit face writes its block straight into its own
 * lightmap texture, so it stays in its normal (VBO, unsplit) world batch. Texel values
 * are the same as the dlight-atlas path; only bilinear neighbours at block edges
 * differ (its real lightmap neighbours instead of the atlas packing). Blocks no longer
 * lit at the end of a world scene are restored from the static lightmap.
 */
static byte s_ipScratch[LIGHTMAP_SIZE * LIGHTMAP_SIZE * 4];

static void R_DlightUploadFaceBlock(const srfSurfaceFace_t *srf, const byte *texels)
{
    GL_Bind(tr.lightmaps[srf->lmTexIndex]);
    qglTexSubImage2D(
        GL_TEXTURE_2D, 0, srf->lmTexX, srf->lmTexY, srf->lmWidth, srf->lmHeight, GL_RGBA, GL_UNSIGNED_BYTE, texels
    );
}

static qboolean R_DlightFaceInPlace(srfSurfaceFace_t *srf)
{
    dlightFaceCache_t *cached;
    const byte        *texels;
    qboolean           hit, added;
    unsigned int       serial;

    if (!r_dlightInPlace->integer || srf->lmTexIndex < 0 || srf->lmTexIndex >= tr.numLightmaps
        || !tr.lightmaps[srf->lmTexIndex] || !srf->lmData || r_lightmap->integer == 2) {
        return qfalse;
    }
    if (s_ipWorld != tr.world) {
        s_ipNumFaces = 0;
        s_ipWorld    = tr.world;
    }
    if (!srf->lmInPlaceDirty && s_ipNumFaces >= DLIGHT_MAX_INPLACE) {
        return qfalse;
    }

    cached = NULL;
    hit    = qfalse;
    if (r_dlightCache->integer) {
        cached = R_DlightCacheFind(srf, srf->lmData, srf->lmWidth, srf->lmHeight, 0, &hit);
    }

    tr.pc.c_dlightTexels += srf->lmWidth * srf->lmHeight;

    if (hit) {
        texels = cached->texels;
        added  = cached->added;
    } else {
        byte       *dst = cached ? cached->texels : s_ipScratch;
        const byte *src = srf->lmData;
        vec3_t      vec, vecStepS, vecStepT;
        int         i, j;

        texels = dst;

        VectorCopy(srf->lmVecs[0], vecStepS);
        VectorMA(srf->lmVecs[1], -srf->lmWidth, srf->lmVecs[0], vecStepT);
        VectorCopy(srf->lmOrigin, vec);

        added = qfalse;
        for (i = 0; i < srf->lmHeight; i++) {
            for (j = 0; j < srf->lmWidth; j++) {
                added |= R_DlightSample((byte *)src, vec, dst);
                VectorAdd(vec, vecStepS, vec);
                src += 3;
                dst += 4;
            }
            VectorAdd(vec, vecStepT, vec);
            src += (LIGHTMAP_SIZE - j) * 3;
        }

        if (cached) {
            cached->added = added;
        }
    }

    if (!added) {
        // nothing reached a texel: draw from the static lightmap (restored at scene end)
        return qtrue;
    }

    serial              = cached && cached->valid ? cached->serial : 0;
    srf->lmInPlaceScene = s_dlScene;
    if (!srf->lmInPlaceDirty) {
        s_ipFaces[s_ipNumFaces++] = srf;
        srf->lmInPlaceDirty       = qtrue;
        srf->lmInPlaceSerial      = 0;
    }
    if (!serial || serial != srf->lmInPlaceSerial) {
        R_DlightUploadFaceBlock(srf, texels);
        srf->lmInPlaceSerial = serial;
    }
    return qtrue;
}

/*
===============
R_RestoreInPlaceDlights

Added in Omaha: put back the static lightmap under faces that weren't lit in place
by the scene that just walked the world (all = every dirty face).
===============
*/
void R_RestoreInPlaceDlights(qboolean all)
{
    int i, n, k;

    if (s_ipWorld != tr.world) {
        s_ipNumFaces = 0;
        s_ipWorld    = tr.world;
        return;
    }

    n = 0;
    for (i = 0; i < s_ipNumFaces; i++) {
        srfSurfaceFace_t *f = s_ipFaces[i];
        const byte       *src;
        byte             *dst;

        if (!all && r_dlightInPlace->integer && f->lmInPlaceScene == s_dlScene) {
            s_ipFaces[n++] = f;
            continue;
        }

        f->lmInPlaceDirty  = qfalse;
        f->lmInPlaceSerial = 0;
        if (f->lmTexIndex < 0 || f->lmTexIndex >= tr.numLightmaps || !tr.lightmaps[f->lmTexIndex]) {
            continue;
        }

        dst = s_ipScratch;
        for (k = 0; k < f->lmHeight; k++) {
            int j;
            src = f->lmData + k * LIGHTMAP_SIZE * 3;
            for (j = 0; j < f->lmWidth; j++, src += 3, dst += 4) {
                R_ColorShiftLightingBytes((byte *)src, dst);
                dst[3] = 255;
            }
        }
        R_DlightUploadFaceBlock(f, s_ipScratch);
    }
    s_ipNumFaces = n;
}

/*
===============
R_RecursiveDlightPatch
===============
*/
static int R_RecursiveDlightPatch(patchLightBlock_t *plb)
{
    patchLightBlock_t cut[4];
    qboolean          added;
    int               index;

    if (plb[0].s < plb[1].s - 1) {
        cut[0] = plb[0];
        cut[2] = plb[2];

        VectorAdd(plb[0].point, plb[1].point, cut[1].point);
        VectorScale(cut[1].point, 0.5f, cut[1].point);
        VectorAdd(plb[2].point, plb[3].point, cut[3].point);
        VectorScale(cut[3].point, 0.5f, cut[3].point);

        cut[3].s = (plb[0].s + plb[1].s) >> 1;
        cut[1].s = cut[3].s;
        cut[1].t = plb[0].t;
        cut[3].t = plb[2].t;

        added = R_RecursiveDlightPatch(cut);

        cut[0] = cut[1];
        cut[2] = cut[3];

        index = 1;
    } else if (plb[0].t < plb[2].t - 1) {
        cut[0] = plb[0];
        cut[1] = plb[1];

        VectorAdd(plb[0].point, plb[2].point, cut[2].point);
        VectorScale(cut[2].point, 0.5f, cut[2].point);
        VectorAdd(plb[1].point, plb[3].point, cut[3].point);
        VectorScale(cut[3].point, 0.5f, cut[3].point);

        cut[3].t = (plb[0].t + plb[2].t) >> 1;
        cut[2].t = cut[3].t;
        cut[2].s = plb[0].s;
        cut[3].s = plb[1].s;

        added  = R_RecursiveDlightPatch(cut);
        cut[0] = cut[2];
        cut[1] = cut[3];

        index = 2;
    } else {
        return R_DlightSample(
            &dli.srcBase[plb[0].t * (LIGHTMAP_SIZE * 3) + plb[0].s * 3],
            plb[0].point,
            &dli.dstBase[plb[0].t * (LIGHTMAP_SIZE * 4) + plb[0].s * 4]
        );
    }

    cut[index] = plb[index];
    cut[3]     = plb[3];

    return R_RecursiveDlightPatch(cut) + added;
}

/*
===============
R_RealDlightPatch
===============
*/
int R_RealDlightPatch(srfGridMesh_t *srf, int dlightBits)
{
    int         x, y;
    int         i, j;
    int         i2, j2;
    int         steps[2][2];
    dlight_t   *dl;
    qboolean    added;
    float      *origin;
    drawVert_t *dv;
    dlightFaceCache_t *cached;
    qboolean           hit;

    if (!srf->lmHeight || !srf->lmWidth) {
        srf->dlightMap[tr.smpFrame] = 0;
        return 0;
    }

    dli.numLights = 0;

    for (i = 0; i < tr.refdef.num_dlights; i++) {
        if (!(dlightBits & (1 << i))) {
            continue;
        }

        dl = &tr.refdef.dlights[i];
        if (dl->origin[0] + dl->radius < srf->meshBounds[0][0] || dl->origin[0] - dl->radius > srf->meshBounds[1][0]
            || dl->origin[1] + dl->radius < srf->meshBounds[0][1] || dl->origin[1] - dl->radius > srf->meshBounds[1][1]
            || dl->origin[2] + dl->radius < srf->meshBounds[0][2]
            || dl->origin[2] - dl->radius > srf->meshBounds[1][2]) {
            dlightBits &= ~(1 << i);
            continue;
        }

        VectorCopy(dl->transformed, dli.lights[dli.numLights].origin);
        dli.lights[dli.numLights].dl    = dl;
        dli.lights[dli.numLights].power = 1.f / dl->radius;
        dli.numLights++;
    }

    if (!dli.numLights) {
        srf->dlightMap[tr.smpFrame] = 0;
        return 0;
    }

    if (!R_AllocLMBlock(srf->lmWidth, srf->lmHeight, &x, &y)) {
        srf->dlightMap[tr.smpFrame] = 0;
        return 0;
    }

    dli.srcBase = srf->lmData;
    dli.dstBase = &dli.lightmap_buffer[y * 4 * LIGHTMAP_SIZE + x * 4];

    srf->lightmapOffset[0] = (float)x / LIGHTMAP_SIZE - (float)srf->lmX / LIGHTMAP_SIZE;
    srf->lightmapOffset[1] = (float)y / LIGHTMAP_SIZE - (float)srf->lmY / LIGHTMAP_SIZE;

    tr.pc.c_dlightSurfaces++;
    tr.pc.c_dlightTexels += srf->lmWidth * srf->lmHeight;

    added = qfalse;

    // Added in Omaha: replay the writes the subdivision made for this light set
    cached = NULL;
    hit    = qfalse;
    if (r_dlightCache->integer) {
        cached = R_DlightCacheFind(
            srf,
            srf->lmData,
            srf->lmWidth,
            srf->lmHeight,
            2 * (srf->lmWidth + 1) * (srf->lmHeight + 1) + 4 * srf->width * srf->height,
            &hit
        );
    }

    if (hit) {
        added = R_DlightReplay(cached, dli.dstBase);
        goto patchDone;
    }

    R_DlightRecordBegin(cached, dli.dstBase);

    if (srf->verts[srf->width - 1].lightmap[0] != srf->verts[0].lightmap[0]) {
        if (srf->verts[srf->width - 1].lightmap[0] < srf->verts[0].lightmap[0]) {
            steps[0][0] = 0;
            steps[0][1] = -1;
        } else {
            steps[0][0] = 0;
            steps[0][1] = 1;
        }

        if (srf->verts[srf->width * (srf->height - 1)].lightmap[1] < srf->verts[0].lightmap[1]) {
            steps[1][0] = -1;
            steps[1][1] = 0;
        } else {
            steps[1][0] = 1;
            steps[1][1] = 0;
        }
    } else {
        if (srf->verts[srf->width * (srf->height - 1)].lightmap[0] < srf->verts[0].lightmap[0]) {
            steps[0][0] = -1;
            steps[0][1] = 0;
        } else {
            steps[0][0] = 1;
            steps[0][1] = 0;
        }

        if (srf->verts[srf->width - 1].lightmap[1] < srf->verts[0].lightmap[1]) {
            steps[1][0] = 0;
            steps[1][1] = -1;
        } else {
            steps[1][0] = 0;
            steps[1][1] = 1;
        }
    }

    for (i = 0; i < srf->height; i++) {
        for (j = 0; j < srf->width; j++) {
            patchLightBlock_t plb[4];

            //
            // Vert 1
            //

            dv = &srf->verts[i * srf->width + j];

            VectorCopy(dv->xyz, plb[0].point);
            plb[0].s = (int)(dv->lightmap[0] * LIGHTMAP_SIZE) - srf->lmX;
            plb[0].t = (int)(dv->lightmap[1] * LIGHTMAP_SIZE) - srf->lmY;

            //
            // Vert 2
            //

            i2 = Q_clamp_int(steps[0][0] + i, 0, srf->height - 1);
            j2 = Q_clamp_int(steps[0][1] + j, 0, srf->width - 1);
            dv = &srf->verts[i2 * srf->width + j2];

            VectorCopy(dv->xyz, plb[1].point);
            plb[1].s = (int)(dv->lightmap[0] * LIGHTMAP_SIZE) - srf->lmX;
            plb[1].t = (int)(dv->lightmap[1] * LIGHTMAP_SIZE) - srf->lmY;

            //
            // Vert 3
            //

            i2 = Q_clamp_int(steps[1][0] + i, 0, srf->height - 1);
            j2 = Q_clamp_int(steps[1][1] + j, 0, srf->width - 1);
            dv = &srf->verts[i2 * srf->width + j2];

            VectorCopy(dv->xyz, plb[2].point);
            plb[2].s = (int)(dv->lightmap[0] * LIGHTMAP_SIZE) - srf->lmX;
            plb[2].t = (int)(dv->lightmap[1] * LIGHTMAP_SIZE) - srf->lmY;

            //
            // Vert 4
            //

            i2 = Q_clamp_int(steps[1][0] + steps[0][0] + i, 0, srf->height - 1);
            j2 = Q_clamp_int(steps[1][1] + steps[0][1] + j, 0, srf->width - 1);
            dv = &srf->verts[i2 * srf->width + j2];

            VectorCopy(dv->xyz, plb[3].point);
            plb[3].s = (int)(dv->lightmap[0] * LIGHTMAP_SIZE) - srf->lmX;
            plb[3].t = (int)(dv->lightmap[1] * LIGHTMAP_SIZE) - srf->lmY;

            added |= R_RecursiveDlightPatch(plb);
        }
    }

    R_DlightRecordEnd(added);

patchDone:
    R_DlightNoteWrite(srf, x, y, cached && cached->valid ? cached->serial : 0);

    if (!added) {
        srf->dlightMap[tr.smpFrame] = 0;
        return 0;
    }

    for (i = 0; i < srf->lmWidth; i++) {
        dli.allocated[x + i] = srf->lmHeight + y;
    }

    srf->dlightMap[tr.smpFrame] = dli.dlightMap + 1;
    return srf->dlightMap[tr.smpFrame];
}

/*
===============
R_RealDlightFace
===============
*/
int R_RealDlightFace(srfSurfaceFace_t *srf, int dlightBits)
{
    int       x, y;
    byte     *src, *dst;
    int       i, j;
    vec3_t    vec;
    vec3_t    vecStepS, vecStepT;
    dlight_t *dl;
    float     d;
    qboolean  added;
    float    *origin;
    dlightFaceCache_t *cached;
    qboolean           hit;

    if (!srf->lmHeight || !srf->lmWidth) {
        srf->dlightMap[tr.smpFrame] = 0;
        return 0;
    }

    dli.numLights = 0;

    for (i = 0; i < tr.refdef.num_dlights; i++) {
        if (!(dlightBits & (1 << i))) {
            continue;
        }

        dl = &tr.refdef.dlights[i];

        d = DotProduct(dl->transformed, srf->plane.normal) - srf->plane.dist;

        if ((d < 0.f && !r_dlightBacks->integer) || (d > dl->radius || d < -dl->radius)) {
            // dlight doesn't reach the plane
            dlightBits &= ~(1 << i);
        } else {
            VectorCopy(dl->transformed, dli.lights[dli.numLights].origin);
            dli.lights[dli.numLights].dl    = dl;
            dli.lights[dli.numLights].power = 1.0 / dl->radius;
            dli.numLights++;
        }
    }

    if (!dli.numLights) {
        srf->dlightMap[tr.smpFrame] = 0;
        return 0;
    }

    if (R_DlightFaceInPlace(srf)) {
        srf->dlightMap[tr.smpFrame] = 0;
        return 0;
    }

    if (!R_AllocLMBlock(srf->lmWidth, srf->lmHeight, &x, &y)) {
        srf->dlightMap[tr.smpFrame] = 0;
        return 0;
    }

    src = srf->lmData;
    dst = &dli.lightmap_buffer[y * 4 * LIGHTMAP_SIZE + x * 4];

    srf->lightmapOffset[0] = (float)x / LIGHTMAP_SIZE - (float)srf->lmX / LIGHTMAP_SIZE;
    srf->lightmapOffset[1] = (float)y / LIGHTMAP_SIZE - (float)srf->lmY / LIGHTMAP_SIZE;

    tr.pc.c_dlightSurfaces++;
    tr.pc.c_dlightTexels += srf->lmWidth * srf->lmHeight;

    // Added in Omaha: serve repeated light sets from the texel cache
    cached = NULL;
    hit    = qfalse;
    if (r_dlightCache->integer) {
        cached = R_DlightCacheFind(srf, srf->lmData, srf->lmWidth, srf->lmHeight, 0, &hit);
    }


    if (hit) {
        for (i = 0; i < srf->lmHeight; i++) {
            Com_Memcpy(dst + i * LIGHTMAP_SIZE * 4, cached->texels + i * srf->lmWidth * 4, srf->lmWidth * 4);
        }
        added = cached->added;
    } else {
        byte *dst0 = dst;

        VectorCopy(srf->lmVecs[0], vecStepS);
        VectorMA(srf->lmVecs[1], -srf->lmWidth, srf->lmVecs[0], vecStepT);

        added = qfalse;
        VectorCopy(srf->lmOrigin, vec);

        for (i = 0; i < srf->lmHeight; i++) {
            for (j = 0; j < srf->lmWidth; j++) {
                added |= R_DlightSample(src, vec, dst);
                VectorAdd(vec, vecStepS, vec);
                src += 3;
                dst += 4;
            }

            VectorAdd(vec, vecStepT, vec);
            src += (LIGHTMAP_SIZE - j) * 3;
            dst += (LIGHTMAP_SIZE - j) * 4;
        }

        if (cached) {
            for (i = 0; i < srf->lmHeight; i++) {
                Com_Memcpy(cached->texels + i * srf->lmWidth * 4, dst0 + i * LIGHTMAP_SIZE * 4, srf->lmWidth * 4);
            }
            cached->added = added;
        }
    }


    R_DlightNoteWrite(srf, x, y, cached && cached->valid ? cached->serial : 0);

    if (!added) {
        srf->dlightMap[tr.smpFrame] = 0;
        return 0;
    }

    for (i = 0; i < srf->lmWidth; i++) {
        dli.allocated[x + i] = srf->lmHeight + y;
    }

    srf->dlightMap[tr.smpFrame] = dli.dlightMap + 1;
    return srf->dlightMap[tr.smpFrame];
}

/*
===============
R_RealDlightTerrain
===============
*/
int R_RealDlightTerrain(cTerraPatchUnpacked_t *srf, int dlightBits)
{
    dlight_t *dl;
    int       x, y;
    byte     *dst, *src;
    vec3_t    vec;
    float     delta, dist;
    float    *origin;
    int       i, j, k;
    int       di, dj;
    qboolean  added;
    float     lmScale;
    int       lumelsPerHeight;
    float     heightPerLumelSquared;
    float     z00, z01;
    float     z10, z11;
    dlightFaceCache_t *cached;
    qboolean           hit;

    dli.numLights = 0;

    for (i = 0; i < tr.refdef.num_dlights; i++) {
        dl = &tr.refdef.dlights[i];

        if (dl->radius < srf->x0 - dl->transformed[0] || -512.f - dl->radius > srf->x0 - dl->transformed[0]
            || -512.f - dl->radius > srf->y0 - dl->transformed[1] || dl->radius < srf->y0 - dl->transformed[1]) {
            continue;
        }

        if (dl->radius > 128.f) {
            dist = 0.f;

            delta = dl->transformed[0] - srf->x0;
            if (delta <= 0.f) {
                dist += delta * delta;
            } else {
                delta -= 512.f;
                if (delta > 0.f) {
                    dist += delta * delta;
                }
            }

            delta = dl->transformed[1] - srf->y0;
            if (delta <= 0.f) {
                dist += delta * delta;
            } else {
                delta -= 512.f;
                if (delta > 0.f) {
                    dist += delta * delta;
                }
            }

            delta = dl->transformed[2] - srf->z0;
            if (delta <= 0.f) {
                dist += delta * delta;
            } else {
                delta -= srf->zmax;
                if (delta > 0.f) {
                    dist += delta * delta;
                }
            }

            if (dl->radius * dl->radius < dist) {
                continue;
            }
        }

        VectorCopy(dl->origin, dli.lights[dli.numLights].origin);
        dli.lights[dli.numLights].dl    = dl;
        dli.lights[dli.numLights].power = 1.f / dl->radius;
        dli.numLights++;
    }

    if (!dli.numLights) {
        srf->drawinfo.dlightMap[tr.smpFrame] = 0;
        return 0;
    }

    if (!R_AllocLMBlock(srf->drawinfo.lmapSize, srf->drawinfo.lmapSize, &x, &y)) {
        srf->drawinfo.dlightMap[tr.smpFrame] = 0;
        return 0;
    }

    src = srf->drawinfo.lmData;
    dst = &dli.lightmap_buffer[y * 4 * LIGHTMAP_SIZE + x * 4];

    srf->drawinfo.lmapX = x / (float)LIGHTMAP_SIZE;
    srf->drawinfo.lmapY = y / (float)LIGHTMAP_SIZE;

    tr.pc.c_dlightSurfaces++;
    tr.pc.c_dlightTexels += srf->drawinfo.lmapSize * srf->drawinfo.lmapSize;

    added = qfalse;

    // Added in Omaha: replay the writes made for this light set
    cached = NULL;
    hit    = qfalse;
    if (r_dlightCache->integer) {
        cached = R_DlightCacheFind(
            srf,
            srf->drawinfo.lmData,
            srf->drawinfo.lmapSize,
            srf->drawinfo.lmapSize,
            srf->drawinfo.lmapSize * srf->drawinfo.lmapSize + 64,
            &hit
        );
    }

    if (hit) {
        added = R_DlightReplay(cached, dst);
        goto terrainDone;
    }

    R_DlightRecordBegin(cached, dst);

    lumelsPerHeight = 64.f / srf->drawinfo.lmapStep;
    if (lumelsPerHeight == 1) {
        vec[1] = srf->y0;

        for (i = 0; i < 9; i++) {
            vec[0] = srf->x0;

            for (j = 0; j < 9; j++) {
                vec[2] = srf->z0 + (int)(srf->heightmap[0] << 1);
                added |= R_DlightSample(src, vec, dst);
                src += 3;
                dst += 4;
                vec[0] += srf->drawinfo.lmapStep;
            }

            src += (LIGHTMAP_SIZE - 9) * 3;
            dst += (LIGHTMAP_SIZE - 9) * 4;
            vec[1] += srf->drawinfo.lmapStep;
        }
    } else if (lumelsPerHeight == 2) {
        vec[1] = srf->y0;

        k = 0;

        for (j = 0; j < 8; j++) {
            vec[0] = srf->x0;

            for (i = 0; i < 8; i++) {
                z00    = (int)srf->heightmap[k];
                vec[2] = srf->z0 + z00 + z00;
                added |= R_DlightSample(src, vec, dst);
                src += 3;
                dst += 4;
                // increase lightmap step
                vec[0] += srf->drawinfo.lmapStep;
                k++;

                z01    = (int)srf->heightmap[k];
                vec[2] = srf->z0 + z00 + z01;
                added |= R_DlightSample(src, vec, dst);
                src += 3;
                dst += 4;
                vec[0] += srf->drawinfo.lmapStep;
            }

            vec[2] = srf->z0 + z01 + z01;
            added |= R_DlightSample(src, vec, dst);
            src += (LIGHTMAP_SIZE - 16) * 3;
            dst += (LIGHTMAP_SIZE - 16) * 4;
            k -= 8;

            vec[0] = srf->x0;
            vec[1] += srf->drawinfo.lmapStep;

            for (i = 0; i < 8; i++) {
                z00    = (int)srf->heightmap[k] + (int)srf->heightmap[k + 9];
                vec[2] = srf->z0 + z00;
                added |= R_DlightSample(src, vec, dst);
                src += 3;
                dst += 4;
                k++;
                vec[0] += srf->drawinfo.lmapStep;

                z01    = (int)srf->heightmap[k] + (int)srf->heightmap[k + 9];
                vec[2] = srf->z0 + (z00 + z01) * 0.5f;
                added |= R_DlightSample(src, vec, dst);
                src += 3;
                dst += 4;
                vec[0] += srf->drawinfo.lmapStep;
            }

            vec[2] = srf->z0 + z01;
            added |= R_DlightSample(src, vec, dst);
            src += (LIGHTMAP_SIZE - 16) * 3;
            dst += (LIGHTMAP_SIZE - 16) * 4;
            k++;
            vec[0] += srf->drawinfo.lmapStep;
            vec[1] += srf->drawinfo.lmapStep;
        }

        vec[0] = srf->x0;

        for (i = 0; i < 8; i++) {
            z00    = (int)srf->heightmap[k];
            vec[2] = srf->z0 + z00 + z00;
            k++;
            added |= R_DlightSample(src, vec, dst);
            src += 3;
            dst += 4;
            vec[0] += srf->drawinfo.lmapStep;

            z01    = (int)srf->heightmap[k];
            vec[2] = srf->z0 + z00 + z01;
            added |= R_DlightSample(src, vec, dst);
            src += 3;
            dst += 4;
            vec[0] += srf->drawinfo.lmapStep;
        }

        vec[2] = srf->z0 + z01 + z01;
        added |= R_DlightSample(src, vec, dst);
        vec[0] += srf->drawinfo.lmapStep;
        vec[1] += srf->drawinfo.lmapStep;
    } else {
        vec[1] = srf->y0;

        for (j = 0; j < 8; j++) {
            dj = 0;

            for (;;) {
                if (j == 7) {
                    if (dj >= lumelsPerHeight + 1) {
                        break;
                    }
                } else if (dj >= lumelsPerHeight) {
                    break;
                }

                vec[0] = srf->x0;

                for (i = 0; i < 8; i++) {
                    for (;;) {
                        if (i == 7) {
                            if (di >= lumelsPerHeight + 1) {
                                break;
                            }
                        } else if (dj >= lumelsPerHeight) {
                            break;
                        }

                        di  = 0;
                        z00 = (int)srf->heightmap[9 * j + i];
                        z01 = (int)srf->heightmap[9 * (j + 1) + i];
                        z10 = (int)srf->heightmap[9 * j + (i + 1)];
                        z11 = (int)srf->heightmap[9 * (j + 1) + (i + 1)];

                        heightPerLumelSquared = 2.f / (lumelsPerHeight * lumelsPerHeight);

                        vec[2] =
                            srf->z0
                            + (dj * (di * z11) + (lumelsPerHeight - dj) * (z10 * di) + z01 * (lumelsPerHeight - di) * dj
                               + z00 * (lumelsPerHeight - di) * (lumelsPerHeight - dj))
                                  * heightPerLumelSquared;

                        added |= R_DlightSample(src, vec, dst);
                        src += 3;
                        dst += 4;

                        vec[0] += srf->drawinfo.lmapStep;
                    }
                }

                src += (LIGHTMAP_SIZE - 1 - (8 * lumelsPerHeight)) * 3;
                dst += (LIGHTMAP_SIZE - 1 - (8 * lumelsPerHeight)) * 4;
                vec[1] += srf->drawinfo.lmapStep;
            }
        }
    }

    R_DlightRecordEnd(added);

terrainDone:
    R_DlightNoteWrite(srf, x, y, cached && cached->valid ? cached->serial : 0);

    if (!added) {
        srf->drawinfo.dlightMap[tr.smpFrame] = 0;
        return 0;
    }

    for (i = 0; i < srf->drawinfo.lmapSize; i++) {
        dli.allocated[x + i] = srf->drawinfo.lmapSize + y;
    }

    lmScale = (1.0 / LIGHTMAP_SIZE) / srf->drawinfo.lmapStep;
    srf->drawinfo.lmapX -= (srf->x0 * lmScale - (0.5 / LIGHTMAP_SIZE));
    srf->drawinfo.lmapY -= (srf->y0 * lmScale - (0.5 / LIGHTMAP_SIZE));

    srf->drawinfo.dlightMap[tr.smpFrame] = dli.dlightMap + 1;
    return srf->drawinfo.dlightMap[tr.smpFrame];
}

/*
===============
R_TransformDlights

Transforms the origins of an array of dlights.
Used by both the front end (for DlightBmodel) and
the back end (before doing the lighting calculation)
===============
*/
void R_TransformDlights(int count, dlight_t *dl, orientationr_t *ori)
{
    int    i;
    vec3_t temp;

    for (i = 0; i < count; i++, dl++) {
        VectorSubtract(dl->origin, ori->origin, temp);
        dl->transformed[0] = DotProduct(temp, ori->axis[0]);
        dl->transformed[1] = DotProduct(temp, ori->axis[1]);
        dl->transformed[2] = DotProduct(temp, ori->axis[2]);
    }
}

/*
=============
R_DlightBmodel

Determine which dynamic lights may effect this bmodel
=============
*/
void R_DlightBmodel(bmodel_t *bmodel)
{
    int         i, j;
    dlight_t   *dl;
    int         mask;
    msurface_t *surf;

    // transform all the lights
    R_TransformDlights(tr.refdef.num_dlights, tr.refdef.dlights, &tr.ori);

    mask = 0;
    for (i = 0; i < tr.refdef.num_dlights; i++) {
        dl = &tr.refdef.dlights[i];

        // see if the point is close enough to the bounds to matter
        for (j = 0; j < 3; j++) {
            if (dl->transformed[j] - bmodel->bounds[1][j] > dl->radius) {
                break;
            }
            if (bmodel->bounds[0][j] - dl->transformed[j] > dl->radius) {
                break;
            }
        }
        if (j < 3) {
            continue;
        }

        // we need to check this light
        mask |= 1 << i;
    }

    tr.currentEntity->needDlights = (mask != 0);

    // set the dlight bits in all the surfaces
    for (i = 0; i < bmodel->numSurfaces; i++) {
        surf = bmodel->firstSurface + i;

        if (*surf->data == SF_FACE) {
            ((srfSurfaceFace_t *)surf->data)->dlightBits[tr.smpFrame] = mask;
        } else if (*surf->data == SF_GRID) {
            ((srfGridMesh_t *)surf->data)->dlightBits[tr.smpFrame] = mask;
        } else if (*surf->data == SF_TRIANGLES) {
            ((srfTriangles_t *)surf->data)->dlightBits[tr.smpFrame] = mask;
        }
    }
}

/*
===============
R_GetLightGridPalettedColor
===============
*/
static byte *R_GetLightGridPalettedColor(int iColor)
{
    return &tr.world->lightGridPalette[iColor * 3];
}

/*
===============
R_GetLightingGridValue
===============
*/
void R_GetLightingGridValue(const vec3_t vPos, vec3_t vLight)
{
    byte  *pColor;
    int    iBaseOffset;
    int    i;
    int    iOffset;
    int    iRowPos;
    int    iData;
    int    iLen;
    int    iGridPos[3];
    int    iArrayXStep;
    float  fV;
    float  fFrac[3];
    float  fOMFrac[3];
    float  fWeight, fWeight2;
    float  fTotalFactor;
    int    iCurData;
    vec3_t vLightOrigin;
    byte  *pCurData;

    if (!tr.world || !tr.world->lightGridData || !tr.world->lightGridOffsets) {
        vLight[0] = vLight[1] = vLight[2] = tr.identityLightByte;
        return;
    }

    VectorSubtract(vPos, tr.world->lightGridMins, vLightOrigin);

    for (i = 0; i < 3; i++) {
        fV          = vLightOrigin[i] * tr.world->lightGridOOSize[i];
        iGridPos[i] = floor(fV);
        fFrac[i]    = fV - iGridPos[i];
        fOMFrac[i]  = 1.0 - fFrac[i];

        if (iGridPos[i] < 0) {
            iGridPos[i] = 0;
        } else if (iGridPos[i] > tr.world->lightGridBounds[i] - 2) {
            iGridPos[i] = tr.world->lightGridBounds[i] - 2;
        }
    }

    fTotalFactor = 0;
    iArrayXStep  = tr.world->lightGridBounds[1];
    iBaseOffset  = tr.world->lightGridBounds[0] + iGridPos[1] + iArrayXStep * iGridPos[0];
    VectorClear(vLight);

    for (i = 0; i < 4; i++) {
        qboolean bContinue = qfalse;

        switch (i) {
        case 0:
            fWeight  = fOMFrac[0] * fOMFrac[1] * fOMFrac[2];
            fWeight2 = fOMFrac[0] * fOMFrac[1] * fFrac[2];
            iOffset  = tr.world->lightGridOffsets[iBaseOffset] + (tr.world->lightGridOffsets[iGridPos[0]] << 8);
            break;
        case 1:
            fWeight  = fOMFrac[0] * fFrac[1] * fOMFrac[2];
            fWeight2 = fOMFrac[0] * fFrac[1] * fFrac[2];
            iOffset  = tr.world->lightGridOffsets[iBaseOffset + 1] + (tr.world->lightGridOffsets[iGridPos[0]] << 8);
            break;
        case 2:
            fWeight  = fFrac[0] * fOMFrac[1] * fOMFrac[2];
            fWeight2 = fFrac[0] * fOMFrac[1] * fFrac[2];
            iOffset  = tr.world->lightGridOffsets[iBaseOffset + iArrayXStep]
                    + (tr.world->lightGridOffsets[iGridPos[0] + 1] << 8);
            break;
        case 3:
            fWeight  = fFrac[0] * fFrac[1] * fOMFrac[2];
            fWeight2 = fFrac[0] * fFrac[1] * fFrac[2];
            iOffset  = tr.world->lightGridOffsets[iBaseOffset + iArrayXStep + 1]
                    + (tr.world->lightGridOffsets[iGridPos[0] + 1] << 8);
            break;
        }

        iRowPos  = iGridPos[2];
        pCurData = &tr.world->lightGridData[iOffset];
        iData    = 0;

        while (1) {
            while (1) {
                iCurData = (char)pCurData[iData];
                iData++;
                if (iCurData >= 0) {
                    break;
                }

                iLen = -iCurData;
                if (iLen > iRowPos) {
                    iData += iRowPos;

                    if (pCurData[iData]) {
                        pColor = R_GetLightGridPalettedColor(pCurData[iData]);
                        VectorMA(vLight, fWeight, pColor, vLight);
                        fTotalFactor += fWeight;
                    }

                    iData++;
                    if (iLen - 1 == iRowPos) {
                        iData++;
                    }

                    if (pCurData[iData]) {
                        pColor = R_GetLightGridPalettedColor(pCurData[iData]);
                        VectorMA(vLight, fWeight2, pColor, vLight);
                        fTotalFactor += fWeight2;
                    }

                    bContinue = qtrue;
                    break;
                }

                iRowPos -= iLen;
                iData += iLen;
            }

            if (bContinue) {
                break;
            }

            iLen = iCurData + 2;
            if (iLen - 1 >= iRowPos) {
                break;
            }

            iRowPos -= iLen;
            iData++;
        }

        if (bContinue) {
            continue;
        }

        if (iLen - 1 > iRowPos) {
            if (!pCurData[iData]) {
                continue;
            }

            pColor = R_GetLightGridPalettedColor(pCurData[iData]);
            VectorMA(vLight, fWeight + fWeight2, pColor, vLight);
            fTotalFactor += fWeight + fWeight2;
        } else {
            if (pCurData[iData]) {
                pColor = R_GetLightGridPalettedColor(pCurData[iData]);
                VectorMA(vLight, fWeight, pColor, vLight);
                fTotalFactor += fWeight;
            }

            iData += 2;
            if (pCurData[iData]) {
                pColor = R_GetLightGridPalettedColor(pCurData[iData]);
                VectorMA(vLight, fWeight2, pColor, vLight);
                fTotalFactor += fWeight2;
            }
        }
    }

    if (fTotalFactor > 0.0 && fTotalFactor < 0.99) {
        VectorScale(vLight, 1.0 / fTotalFactor, vLight);
    }

    if (fTotalFactor) {
        if (vLight[0] > 255.0 || vLight[1] > 255.0 || vLight[2] > 255.0) {
            float t;
            // normalize color values
            t = 255.0 / Q_max(vLight[0], Q_max(vLight[1], vLight[2]));
            VectorScale(vLight, t, vLight);
        }
    } else {
        vLight[0] = vLight[1] = vLight[2] = tr.identityLightByte;
    }
}

/*
===============
R_GetLightingGridValueFast
===============
*/
void R_GetLightingGridValueFast(const vec3_t vPos, vec3_t vLight)
{
    byte      *pColor;
    int        i;
    int        iRowPos;
    int        iGridPos[3];
    int        iArrayXStep;
    float      fV[3];
    vec3_t     vLightOrigin;
    int        iData;
    int        iLen;
    int        iBaseOffset;
    byte      *pCurData;
    int        iOrder;
    int        iToggle;
    int        iSample[8];
    float      fDist[3];
    int        j, k;
    static int iSearch[8][8] = {
        {0, 4, 2, 6, 1, 5, 3, 7},
        {0, 2, 4, 6, 1, 3, 5, 7},
        {0, 0, 0, 0, 0, 0, 0, 0},
        {0, 2, 1, 3, 4, 6, 5, 7},
        {0, 4, 1, 5, 2, 6, 3, 7},
        {0, 0, 0, 0, 0, 0, 0, 0},
        {0, 1, 4, 5, 2, 3, 6, 7},
        {0, 1, 2, 3, 4, 5, 6, 7}
    };

    if (!tr.world->lightGridData || !tr.world->lightGridOffsets) {
        vLight[0] = vLight[1] = vLight[2] = tr.identityLightByte;
        return;
    }

    iOrder  = 0;
    iToggle = 0;

    for (i = 0; i < 3; i++) {
        fV[i] =
            (tr.world->lightGridSize[i] * 0.5 + vPos[i] - tr.world->lightGridMins[i]) * tr.world->lightGridOOSize[i];
        iGridPos[i] = floor(fV[i]);
        fV[i] -= iGridPos[i];

        if (iGridPos[i] < 0) {
            iGridPos[i] = 0;
            fV[i]       = 0.0;
        } else if (iGridPos[i] > tr.world->lightGridBounds[i] - 2) {
            iGridPos[i] = tr.world->lightGridBounds[i] - 2;
            fV[i]       = 1.0;
        }

        fDist[i] = 0.5 - fV[i];
        if (fDist[i] < 0) {
            fDist[i] = -fDist[i];
            iToggle |= 1 << i;
        }
    }

    if (fDist[1] >= fDist[0]) {
        iOrder = 1;
    }
    if (fDist[2] >= fDist[0]) {
        iOrder |= 2;
    }
    if (fDist[2] >= fDist[1]) {
        iOrder |= 4;
    }

    for (i = 0; i < 8; i++) {
        iSample[i] = -1;
    }

    iArrayXStep = tr.world->lightGridBounds[1];
    iBaseOffset = iGridPos[1] + tr.world->lightGridBounds[0] + iGridPos[0] * tr.world->lightGridBounds[1];

    for (i = 0; i < 8; i++) {
        j = iSearch[iOrder][i] ^ iToggle;

        if (iSample[j] < 0) {
            switch (j & 3) {
            case 0:
                pCurData =
                    &tr.world->lightGridData
                         [tr.world->lightGridOffsets[iGridPos[0]] * 256 + tr.world->lightGridOffsets[iBaseOffset]];
                break;
            case 1:
                pCurData =
                    &tr.world->lightGridData
                         [tr.world->lightGridOffsets[iGridPos[0]] * 256 + tr.world->lightGridOffsets[iBaseOffset + 1]];
                break;
            case 2:
                pCurData = &tr.world->lightGridData
                                [tr.world->lightGridOffsets[iGridPos[0] + 1] * 256
                                 + tr.world->lightGridOffsets[iBaseOffset + iArrayXStep]];
                break;
            case 3:
            default:
                pCurData = &tr.world->lightGridData
                                [tr.world->lightGridOffsets[iGridPos[0] + 1] * 256
                                 + tr.world->lightGridOffsets[iBaseOffset + iArrayXStep + 1]];
                break;
            }

            k = j & ~1;

            iRowPos = iGridPos[2];
            iData   = 0;

            while (1) {
                qboolean bShouldBreak = qfalse;

                for (; (char)pCurData[iData] >= 0; iData += 2, iRowPos -= iLen) {
                    iLen = (char)pCurData[iData] + 2;

                    if (iLen - 1 < iRowPos) {
                        continue;
                    }

                    if (iLen - 1 > iRowPos) {
                        iSample[k + 1] = pCurData[iData + 1];
                        iSample[k]     = pCurData[iData + 1];
                    } else {
                        iSample[k]     = pCurData[iData + 1];
                        iSample[k + 1] = pCurData[iData + 3];
                    }

                    bShouldBreak = qtrue;
                    break;
                }

                if (bShouldBreak) {
                    break;
                }

                if (-(char)pCurData[iData] > iRowPos) {
                    iSample[k] = pCurData[iRowPos + iData + 1];

                    if ((-(char)pCurData[iData] - 1) == iRowPos) {
                        iSample[k + 1] = pCurData[iRowPos + iData + 3];
                    } else {
                        iSample[k + 1] = pCurData[iRowPos + iData + 2];
                    }
                    break;
                }

                iRowPos -= -(char)pCurData[iData];
                iData += -(char)pCurData[iData] + 1;
            }
        }

        if (iSample[j] > 0) {
            pColor    = R_GetLightGridPalettedColor(iSample[j]);
            vLight[0] = pColor[0];
            vLight[1] = pColor[1];
            vLight[2] = pColor[2];
            return;
        }
    }

    vLight[0] = vLight[1] = vLight[2] = tr.identityLightByte;
}

/*
===============
R_GetLightingForDecal
===============
*/
void R_GetLightingForDecal(vec3_t vLight, const vec3_t vFacing, const vec3_t vOrigin)
{
    float fMax;

    R_GetLightingGridValue(vOrigin, vLight);

    if (!tr.overbrightShift) {
        return;
    }

    VectorScale(vLight, tr.overbrightMult, vLight);

    if (vLight[0] > 255.0 || vLight[1] > 255.0 || vLight[2] > 255.0) {
        float scale = 255.0 / Q_max(vLight[0], Q_max(vLight[1], vLight[2]));
        VectorScale(vLight, scale, vLight);
    }
}

/*
===============
R_GetLightingForSmoke
===============
*/
void R_GetLightingForSmoke(vec3_t vLight, const vec3_t vOrigin)
{
    int       i;
    dlight_t *dl;
    float     power;
    vec3_t    dir;
    float     d;

    if (r_smoothsmokelight->integer) {
        R_GetLightingGridValue(vOrigin, vLight);
    } else {
        R_GetLightingGridValueFast(vOrigin, vLight);
    }

    for (i = 0; i < backEnd.refdef.num_dlights; i++) {
        dl = &backEnd.refdef.dlights[i];
        VectorSubtract(dl->origin, vOrigin, dir);
        d = VectorLengthSquared(dir);

        power = dl->radius * dl->radius;
        if (power >= d) {
            d = dl->radius * 7500.0 / d;
            VectorMA(vLight, d, dl->color, vLight);
        }
    }

    if (tr.overbrightShift) {
        vLight[0] = tr.overbrightMult * vLight[0];
        vLight[1] = tr.overbrightMult * vLight[1];
        vLight[2] = tr.overbrightMult * vLight[2];
    }

    // normalize
    if (vLight[0] > 255.0 || vLight[1] > 255.0 || vLight[2] > 255.0) {
        float scale = 255.0 / Q_max(vLight[0], Q_max(vLight[1], vLight[2]));
        VectorScale(vLight, scale, vLight);
    }

    vLight[0] /= 255.0;
    vLight[1] /= 255.0;
    vLight[2] /= 255.0;
}

/*
===============
RB_GetEntityGridLighting
===============
*/
static int RB_GetEntityGridLighting()
{
    int       iColor;
    int       i;
    dlight_t *dl;
    float     power;
    vec3_t    vLight;
    vec3_t    dir;
    float     d;
    float    *lightOrigin;

    lightOrigin = backEnd.currentSphere->traceOrigin;
    if (!(backEnd.refdef.rdflags & RDF_NOWORLDMODEL) && tr.world->lightGridData) {
        R_GetLightingGridValue(backEnd.currentSphere->traceOrigin, vLight);
    } else {
        vLight[0] = vLight[1] = vLight[2] = tr.identityLight * 150.0;
    }

    for (i = 0; i < backEnd.refdef.num_dlights; i++) {
        dl = &backEnd.refdef.dlights[i];
        VectorSubtract(dl->origin, lightOrigin, dir);
        d = VectorLengthSquared(dir);

        power = dl->radius * dl->radius;
        if (power >= d) {
            d = dl->radius * 7500.0 / d;
            VectorMA(vLight, d, dl->color, vLight);
        }
    }

    if (tr.overbrightShift) {
        vLight[0] = tr.overbrightMult * vLight[0];
        vLight[1] = tr.overbrightMult * vLight[1];
        vLight[2] = tr.overbrightMult * vLight[2];
    }

    // normalize
    if (vLight[0] > 255.0 || vLight[1] > 255.0 || vLight[2] > 255.0) {
        float scale = 255.0 / Q_max(vLight[0], Q_max(vLight[1], vLight[2]));
        VectorScale(vLight, scale, vLight);
    }

    // clamp ambient
    for (i = 0; i < 3; i++) {
        if (vLight[i] > tr.identityLightByte) {
            vLight[i] = tr.identityLightByte;
        }
    }

    // save out the byte packet version
    ((byte *)&iColor)[0] = myftol(vLight[0]);
    ((byte *)&iColor)[1] = myftol(vLight[1]);
    ((byte *)&iColor)[2] = myftol(vLight[2]);
    ((byte *)&iColor)[3] = 0xff;

    return iColor;

#if 0
	int				i;
	dlight_t* dl;
	float			power;
	vec3_t			dir;
	float			d;
	vec3_t			lightDir;
	vec3_t			lightOrigin;
	int				ambientlightInt = 0;
	trRefEntity_t	*ent = backEnd.currentEntity;
	trRefdef_t		*refdef = &backEnd.refdef;

	//
	// trace a sample point down to find ambient light
	//
	if (ent->e.renderfx & RF_LIGHTING_ORIGIN) {
		// seperate lightOrigins are needed so an object that is
		// sinking into the ground can still be lit, and so
		// multi-part models can be lit identically
		VectorCopy(ent->e.lightingOrigin, lightOrigin);
	}
	else {
		VectorCopy(ent->e.origin, lightOrigin);
	}

	// if NOWORLDMODEL, only use dynamic lights (menu system, etc)
	if (!(refdef->rdflags & RDF_NOWORLDMODEL)
		&& tr.world->lightGridData) {
		R_SetupEntityLightingGrid(ent);
	}
	else {
		ent->ambientLight[0] = ent->ambientLight[1] =
			ent->ambientLight[2] = tr.identityLight * 150;
		ent->directedLight[0] = ent->directedLight[1] =
			ent->directedLight[2] = tr.identityLight * 150;
		VectorCopy(tr.sunDirection, ent->lightDir);
	}

	// bonus items and view weapons have a fixed minimum add
	if (1 /* ent->e.renderfx & RF_MINLIGHT */) {
		// give everything a minimum light add
		ent->ambientLight[0] += tr.identityLight * 32;
		ent->ambientLight[1] += tr.identityLight * 32;
		ent->ambientLight[2] += tr.identityLight * 32;
	}

	//
	// modify the light by dynamic lights
	//
	d = VectorLength(ent->directedLight);
	VectorScale(ent->lightDir, d, lightDir);

	for (i = 0; i < refdef->num_dlights; i++) {
		dl = &refdef->dlights[i];
		VectorSubtract(dl->origin, lightOrigin, dir);
		d = VectorNormalize(dir);

		power = DLIGHT_AT_RADIUS * (dl->radius * dl->radius);
		if (d < DLIGHT_MINIMUM_RADIUS) {
			d = DLIGHT_MINIMUM_RADIUS;
		}
		d = power / (d * d);

		VectorMA(ent->directedLight, d, dl->color, ent->directedLight);
		VectorMA(lightDir, d, dir, lightDir);
	}

	// clamp ambient
	for (i = 0; i < 3; i++) {
		if (ent->ambientLight[i] > tr.identityLightByte) {
			ent->ambientLight[i] = tr.identityLightByte;
		}
	}

	// save out the byte packet version
	((byte*)&ambientlightInt)[0] = myftol(ent->ambientLight[0]);
	((byte*)&ambientlightInt)[1] = myftol(ent->ambientLight[1]);
	((byte*)&ambientlightInt)[2] = myftol(ent->ambientLight[2]);
	((byte*)&ambientlightInt)[3] = 0xff;

	// transform the direction to local space
	VectorNormalize(lightDir);
	ent->lightDir[0] = DotProduct(lightDir, ent->e.axis[0]);
	ent->lightDir[1] = DotProduct(lightDir, ent->e.axis[1]);
	ent->lightDir[2] = DotProduct(lightDir, ent->e.axis[2]);

	return ambientlightInt;
#endif
}

/*
===============
RB_SetupEntityGridLighting
===============
*/
void RB_SetupEntityGridLighting()
{
    trRefEntity_t *ent;
    int            iColor;

    if (backEnd.currentEntity->bLightGridCalculated) {
        return;
    }

    for (ent = backEnd.currentEntity; ent->e.parentEntity != ENTITYNUM_NONE;
         ent = &backEnd.refdef.entities[ent->e.parentEntity]) {
        trRefEntity_t *newref = &backEnd.refdef.entities[ent->e.parentEntity];
        if (newref == ent) {
            assert(!"backEnd.refdef.entities[ent->e.parentEntity] refers to itself\n");
            iColor = newref->iGridLighting;
            break;
        }

        if (newref->bLightGridCalculated) {
            iColor = newref->iGridLighting;
            break;
        }
    }

    if (ent->e.parentEntity == ENTITYNUM_NONE) {
        iColor = RB_GetEntityGridLighting();
    }

    ent = backEnd.currentEntity;
    for (;;) {
        ent->bLightGridCalculated = qtrue;
        ent->iGridLighting        = iColor;
        if (ent->e.parentEntity == ENTITYNUM_NONE) {
            break;
        }

        if (ent == &backEnd.refdef.entities[ent->e.parentEntity]) {
            assert(!"backEnd.refdef.entities[ent->e.parentEntity] refers to itself\n");
            break;
        }

        ent = &backEnd.refdef.entities[ent->e.parentEntity];
    }
}

/*
===============
RB_SetupStaticModelGridLighting
===============
*/
void RB_SetupStaticModelGridLighting(trRefdef_t *refdef, cStaticModelUnpacked_t *ent, const vec3_t lightOrigin)
{
    int       iColor;
    int       i;
    dlight_t *dl;
    float     power;
    vec3_t    vLight;
    vec3_t    dir;
    float     d;

    if (ent->bLightGridCalculated) {
        return;
    }
    ent->bLightGridCalculated = qtrue;

    if (!(refdef->rdflags & RDF_NOWORLDMODEL) && tr.world->lightGridData) {
        R_GetLightingGridValue(lightOrigin, vLight);
    } else {
        vLight[0] = vLight[1] = vLight[2] = tr.identityLight * 150.0;
    }

    for (i = 0; i < refdef->num_dlights; i++) {
        dl = &refdef->dlights[i];
        VectorSubtract(dl->origin, lightOrigin, dir);
        d = VectorLengthSquared(dir);

        power = dl->radius * dl->radius;
        if (power >= d) {
            d = dl->radius * 7500.0 / d;
            VectorMA(vLight, d, dl->color, vLight);
        }
    }

    if (tr.overbrightShift) {
        vLight[0] = tr.overbrightMult * vLight[0];
        vLight[1] = tr.overbrightMult * vLight[1];
        vLight[2] = tr.overbrightMult * vLight[2];
    }

    // normalize
    if (vLight[0] > 255.0 || vLight[1] > 255.0 || vLight[2] > 255.0) {
        float scale = 255.0 / Q_max(vLight[0], Q_max(vLight[1], vLight[2]));
        VectorScale(vLight, scale, vLight);
    }

    // clamp ambient
    for (i = 0; i < 3; i++) {
        if (vLight[i] > tr.identityLightByte) {
            vLight[i] = tr.identityLightByte;
        }
    }

    // save out the byte packet version
    ((byte *)&ent->iGridLighting)[0] = myftol(vLight[0]);
    ((byte *)&ent->iGridLighting)[1] = myftol(vLight[1]);
    ((byte *)&ent->iGridLighting)[2] = myftol(vLight[2]);
    ((byte *)&ent->iGridLighting)[3] = 0xff;
}

/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/

extern cvar_t *r_ambientScale;
extern cvar_t *r_directedScale;

/*
===============
LogLight
===============
*/
static void LogLight(trRefEntity_t *ent)
{
    int max1, max2;

    if (!(ent->e.renderfx & RF_FIRST_PERSON)) {
        return;
    }

    max1 = ent->ambientLight[0];
    if (ent->ambientLight[1] > max1) {
        max1 = ent->ambientLight[1];
    } else if (ent->ambientLight[2] > max1) {
        max1 = ent->ambientLight[2];
    }

    max2 = ent->directedLight[0];
    if (ent->directedLight[1] > max2) {
        max2 = ent->directedLight[1];
    } else if (ent->directedLight[2] > max2) {
        max2 = ent->directedLight[2];
    }

    ri.Printf(PRINT_ALL, "amb:%i  dir:%i\n", max1, max2);
}

/*
===============
R_ClearRealDlights
===============
*/
void R_ClearRealDlights()
{
    memset(dli.allocated, 0, sizeof(dli.allocated));
    dli.dlightMap = 0;
    R_DlightBeginScene();
}

/*
===============
R_UploadDlights
===============
*/
void R_UploadDlights()
{
    int i, h;

    if (!tr.pc.c_dlightSurfaces) {
        return;
    }

    h = 0;
    for (i = 0; i < LIGHTMAP_SIZE; i++) {
        if (h < dli.allocated[i]) {
            h = dli.allocated[i];
        }
    }

    if (h) {
        if (h > LIGHTMAP_SIZE) {
            ri.Error(ERR_DROP, "R_UploadDlights: bad allocated height");
        }

        image_t              *image = tr.dlightImages[dli.dlightMap];
        dlightUpload_t       *up    = &s_dlUploads[s_dlCur][dli.dlightMap];
        const dlightUpload_t *prev  = &s_dlUploads[!s_dlCur][dli.dlightMap];

        up->numWrites = s_dlNumWrites[s_dlCur];
        up->h         = h;
        up->texnum    = image->texnum;

        // Added in Omaha: skip the upload when the atlas already holds this exact content
        if (!(s_dlPrevStable && s_dlPrefixMatch && !s_dlOverflow[s_dlCur] && prev->numWrites == up->numWrites
              && prev->h == h && prev->texnum == up->texnum)) {
            GL_Bind(image);
            qglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, LIGHTMAP_SIZE, h, GL_RGBA, GL_UNSIGNED_BYTE, dli.lightmap_buffer);
        }

        tr.pc.c_dlightMaps++;
        memset(dli.allocated, 0, sizeof(dli.allocated));
    }
}

/*
===============
R_AllocLMBlock
===============
*/
qboolean R_AllocLMBlock(int w, int h, int *x, int *y)
{
    int i, j;
    int best, best2;

    for (;;) {
        best = LIGHTMAP_SIZE;
        for (i = 0; i < LIGHTMAP_SIZE - w; i++) {
            best2 = 0;

            for (j = 0; j < w && dli.allocated[i + j] < best; j++) {
                if (best2 < dli.allocated[i + j]) {
                    best2 = dli.allocated[i + j];
                }
            }

            if (j == w) {
                *x   = i;
                *y   = best2;
                best = best2;
                // Added in Omaha: nothing can beat a zero-height window
                if (!best && w > 0) {
                    break;
                }
            }
        }

        if (h + best <= LIGHTMAP_SIZE) {
            break;
        }

        if (dli.dlightMap == 14) {
            return qfalse;
        }

        R_UploadDlights();
        dli.dlightMap++;
    }

    return qtrue;
}

/*
===============
R_DlightSample
===============
*/
qboolean R_DlightSample(byte *src, const vec3_t vec, byte *dst)
{
    int      r, g, b;
    int      k;
    qboolean added;
    vec3_t   dir;
    float    add;

    added = qfalse;
    r     = src[0];
    g     = src[1];
    b     = src[2];

    for (k = 0; k < dli.numLights; k++) {
        incidentLight_t *light = &dli.lights[k];

        VectorSubtract(vec, light->origin, dir);
        // Added in Omaha: skip the sqrt when clearly outside the radius (margin keeps it exact)
        if (DotProduct(dir, dir) > light->dl->radius * light->dl->radius * 1.001f) {
            continue;
        }
        add = VectorLength(dir) * light->power;
        if (add <= 1.f) {
            float t = (1.0 - add) * (1.0 - add) * 375.0;

            r = (int)(t * light->dl->color[0] + (float)r);
            g = (int)(t * light->dl->color[1] + (float)g);
            b = (int)(t * light->dl->color[2] + (float)b);
            // light was added
            added = qtrue;
        }
    }

    if (tr.overbrightShift) {
        r <<= tr.overbrightShift & 0xFF;
        g <<= tr.overbrightShift & 0xFF;
        b <<= tr.overbrightShift & 0xFF;
    }

    if (r > 0xFF || g > 0xFF || b > 0xFF) {
        float t;
        // normalize color values
        t = 255.0 / (float)Q_max(r, Q_max(g, b));

        r = (int)((float)r * t);
        g = (int)((float)g * t);
        b = (int)((float)b * t);
    }

    dst[0] = (byte)r;
    dst[1] = (byte)g;
    dst[2] = (byte)b;
    dst[3] = -1;

    if (s_dlRecord) {
        R_DlightRecordWrite(dst);
    }

    return added;
}

int R_LightForPoint(vec3_t point, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir) {
    // Stub
    return 0;
}
