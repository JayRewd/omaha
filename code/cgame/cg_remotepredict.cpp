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

//
// cg_remotepredict.cpp -- client-only collision-aware prediction for remote players.
//
// Stock MOHAA servers do not lag-compensate. Bullet traces run against remotes at
// their current server positions when the shooter's usercmd is executed. Displaying
// remotes leadMs into the future therefore shows the geometrically correct aim
// point. This module re-simulates each visible remote forward each frame using the
// shared PM_StepSlideMove, seeded from the authoritative interpolated pose.
//

#include "cg_local.h"
#include "cg_remotepredict.h"
#include "../fgame/bg_local.h"

#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Cvars
// ---------------------------------------------------------------------------

static cvar_t *cg_remotePrediction;
static cvar_t *cg_remotePredictionScale;
static cvar_t *cg_remotePredictionMaxLead;
static cvar_t *cg_remotePredictionStepMsec;
static cvar_t *cg_remotePredictionSmooth;
static cvar_t *cg_remotePredictionSnapDist;
static cvar_t *cg_remotePredictionMaxDist;
static cvar_t *cg_remotePredictionMinSpeed;
static cvar_t *cg_remotePredictionDebug;
// Added in Omaha: public flesh logs showed over-lead when full interp was added to ping.
static cvar_t *cg_remotePredictionInterpWeight;
static cvar_t *cg_remotePredictionLeadKnee;
static cvar_t *cg_remotePredictionLeadKneeFrac;

// ---------------------------------------------------------------------------
// Frame / entity state
// ---------------------------------------------------------------------------

typedef struct {
    vec3_t   origin;
    vec3_t   velocity;
    vec3_t   mins;
    vec3_t   maxs;
    int      entityNum;
    int      gravity;
    qboolean grounded;
} rpSeed_t;

typedef struct {
    qboolean valid;
    int      lastUpdateTime;
    vec3_t   smoothOrigin;
    vec3_t   predOrigin;
    vec3_t   mins;
    vec3_t   maxs;
    vec3_t   lastSeedVelocity;
    float    confidence;
    int      histTime[2];
    vec3_t   histOrigin[2];
    int      lastSnapTime;
} rpEntity_t;


static rpEntity_t rp_entities[MAX_GENTITIES];

static qboolean rp_frameActive;
static int      rp_leadMsec;
static float    rp_pingSmoothed;
static int      rp_lastPingSnapTime;
static int      rp_debugPredictedCount;
static float    rp_debugDispSum;
static int      rp_lastDebugPrintTime;

static playerState_t rp_ps;
static pmove_t       rp_pm;

#define RP_MAX_STEPS           16
#define RP_STALE_MS            500
#define RP_HIST_MAX_GAP_MS     250
#define RP_PING_EMA_ALPHA      0.15f
// Changed in Omaha: drop lead sooner on velocity cuts (jukes); recover slightly slower.
#define RP_CONF_DROP_START     60.0f
#define RP_CONF_DROP_RANGE     300.0f
#define RP_CONF_RECOVER_MS     250.0f
#define RP_DEFAULT_GRAVITY     800
#define RP_BOUNDS_STALE_MS     100

// ---------------------------------------------------------------------------
// Trace shim matching pmove_t::trace exactly (bg_public.h)
// ---------------------------------------------------------------------------

static void CG_RP_Trace(
    trace_t     *results,
    const vec3_t start,
    const vec3_t mins,
    const vec3_t maxs,
    const vec3_t end,
    int          passEntityNum,
    int          contentMask,
    int          capsule,
    qboolean     traceDeep
)
{
    (void)capsule;
    (void)traceDeep;
    CG_Trace(results, start, mins, maxs, end, passEntityNum, contentMask, qtrue, qtrue, "RemotePredict");
}

// ---------------------------------------------------------------------------
// Per-entity helpers
// ---------------------------------------------------------------------------

static void CG_RP_InvalidateEntity(rpEntity_t *rp)
{
    memset(rp, 0, sizeof(*rp));
    rp->confidence = 1.0f;
}

static void CG_RP_PushHistory(rpEntity_t *rp, centity_t *cent)
{
    if (cent->snapShotTime == rp->lastSnapTime) {
        return;
    }

    if (rp->lastSnapTime != 0) {
        rp->histTime[0] = rp->histTime[1];
        VectorCopy(rp->histOrigin[1], rp->histOrigin[0]);
    }

    rp->histTime[1] = cent->snapShotTime;
    VectorCopy(cent->currentState.origin, rp->histOrigin[1]);
    rp->lastSnapTime = cent->snapShotTime;
}

static qboolean CG_RP_DerivedVelocity(const rpEntity_t *rp, vec3_t out)
{
    int   dt;
    float inv;

    if (rp->histTime[0] == 0 || rp->histTime[1] == 0) {
        return qfalse;
    }
    if (rp->histTime[1] <= rp->histTime[0]) {
        return qfalse;
    }

    dt = rp->histTime[1] - rp->histTime[0];
    if (dt > RP_HIST_MAX_GAP_MS) {
        return qfalse;
    }

    inv = 1000.0f / (float)dt;
    VectorSubtract(rp->histOrigin[1], rp->histOrigin[0], out);
    VectorScale(out, inv, out);
    return qtrue;
}

static qboolean CG_RP_BuildSeed(centity_t *cent, rpSeed_t *seed, rpEntity_t *rp)
{
    int      contents;
    float    speed;
    vec3_t   derived;
    trace_t  seedTrace;
    qboolean haveDerived;

    if (cent->currentState.eType != ET_PLAYER) {
        return qfalse;
    }
    if (!cg.snap || cent->currentState.number == cg.snap->ps.clientNum) {
        return qfalse;
    }
    if (!cent->currentValid || cent->teleported) {
        return qfalse;
    }
    if (cent->currentState.eFlags & EF_DEAD) {
        return qfalse;
    }
    if (cent->currentState.number < 0 || cent->currentState.number >= MAX_GENTITIES) {
        return qfalse;
    }
    if (!cent->currentState.solid || cent->currentState.solid == SOLID_BMODEL) {
        return qfalse;
    }

    memset(seed, 0, sizeof(*seed));
    seed->entityNum = cent->currentState.number;
    VectorCopy(cent->netLerpOrigin, seed->origin);
    IntegerToBoundingBox(cent->currentState.solid, seed->mins, seed->maxs);

    if (cent->interpolate && cg.nextSnap) {
        // Lerp velocity: (1-f)*cur + f*next
        seed->velocity[0] = cent->currentState.pos.trDelta[0]
                            + cg.frameInterpolation
                                  * (cent->nextState.pos.trDelta[0] - cent->currentState.pos.trDelta[0]);
        seed->velocity[1] = cent->currentState.pos.trDelta[1]
                            + cg.frameInterpolation
                                  * (cent->nextState.pos.trDelta[1] - cent->currentState.pos.trDelta[1]);
        seed->velocity[2] = cent->currentState.pos.trDelta[2]
                            + cg.frameInterpolation
                                  * (cent->nextState.pos.trDelta[2] - cent->currentState.pos.trDelta[2]);
    } else {
        VectorCopy(cent->currentState.pos.trDelta, seed->velocity);
    }

    haveDerived = CG_RP_DerivedVelocity(rp, derived);
    speed       = VectorLength(seed->velocity);
    if (speed < 1.0f && haveDerived && VectorLength(derived) >= cg_remotePredictionMinSpeed->value) {
        // Server likely has g_smoothClients 0 (trDelta zeroed); use origin deltas.
        VectorCopy(derived, seed->velocity);
        speed = VectorLength(seed->velocity);
    }

    if (speed < cg_remotePredictionMinSpeed->value) {
        return qfalse;
    }

    seed->grounded = (cent->currentState.groundEntityNum != ENTITYNUM_NONE) ? qtrue : qfalse;
    seed->gravity  = cg.snap->ps.gravity;
    if (seed->gravity <= 0) {
        seed->gravity = RP_DEFAULT_GRAVITY;
    }

    contents = CG_PointContents(seed->origin, seed->entityNum);
    if (contents & (CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_LAVA)) {
        return qfalse;
    }

    CG_Trace(
        &seedTrace,
        seed->origin,
        seed->mins,
        seed->maxs,
        seed->origin,
        seed->entityNum,
        MASK_PLAYERSOLID,
        qtrue,
        qtrue,
        "RemotePredictSeed"
    );
    if (seedTrace.allsolid) {
        return qfalse;
    }

    return qtrue;
}

// ---------------------------------------------------------------------------
// Forward coast via shared PM_StepSlideMove
// ---------------------------------------------------------------------------

static qboolean CG_RP_Coast(const rpSeed_t *seed, int leadMsec, vec3_t outOrigin)
{
    pmove_t *savedPm;
    pml_t    savedPml;
    int      stepMsec;
    int      remaining;
    int      steps;
    int      i;

    if (leadMsec <= 0) {
        VectorCopy(seed->origin, outOrigin);
        return qtrue;
    }

    memset(&rp_ps, 0, sizeof(rp_ps));
    VectorCopy(seed->origin, rp_ps.origin);
    VectorCopy(seed->velocity, rp_ps.velocity);
    rp_ps.gravity   = seed->gravity;
    rp_ps.clientNum = seed->entityNum;

    memset(&rp_pm, 0, sizeof(rp_pm));
    rp_pm.ps            = &rp_ps;
    rp_pm.trace         = CG_RP_Trace;
    rp_pm.pointcontents = CG_PointContents;
    rp_pm.tracemask     = MASK_PLAYERSOLID;
    VectorCopy(seed->mins, rp_pm.mins);
    VectorCopy(seed->maxs, rp_pm.maxs);

    // Save shared bg globals; local prediction also uses them.
    savedPm  = pm;
    savedPml = pml;
    pm       = &rp_pm;

    stepMsec = cg_remotePredictionStepMsec->integer;
    if (stepMsec < 4) {
        stepMsec = 4;
    }
    steps = (leadMsec + stepMsec - 1) / stepMsec;
    if (steps > RP_MAX_STEPS) {
        steps = RP_MAX_STEPS;
    }
    remaining = leadMsec;

    for (i = 0; i < steps; i++) {
        int      thisStep;
        vec3_t   point;
        trace_t  ground;

        thisStep = remaining / (steps - i);
        if (thisStep < 1) {
            thisStep = 1;
        }
        remaining -= thisStep;

        memset(&pml, 0, sizeof(pml));
        pml.frametime = thisStep * 0.001f;
        pml.msec      = thisStep;

        // Mini ground probe, mirroring PM_GroundTrace.
        VectorCopy(rp_ps.origin, point);
        point[2] -= 0.25f;
        rp_pm.trace(
            &ground,
            rp_ps.origin,
            rp_pm.mins,
            rp_pm.maxs,
            point,
            rp_ps.clientNum,
            rp_pm.tracemask,
            qtrue,
            qfalse
        );

        if (ground.fraction < 1.0f && ground.plane.normal[2] >= MIN_WALK_NORMAL && !ground.startsolid) {
            pml.groundPlane = qtrue;
            pml.walking     = qtrue;
            pml.groundTrace = ground;
        }

        // Coast: no friction and no acceleration. Assume the player is still
        // holding the same movement input; friction would systematically under-lead
        // players who are still running.
        PM_StepSlideMove(pml.walking ? qfalse : qtrue);

        {
            // Detect being trapped after a step (allsolid at current origin).
            trace_t trapped;
            rp_pm.trace(
                &trapped,
                rp_ps.origin,
                rp_pm.mins,
                rp_pm.maxs,
                rp_ps.origin,
                rp_ps.clientNum,
                rp_pm.tracemask,
                qtrue,
                qfalse
            );
            if (trapped.allsolid) {
                break;
            }
        }
    }

    VectorCopy(rp_ps.origin, outOrigin);

    pm  = savedPm;
    pml = savedPml;
    return qtrue;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void CG_RP_RegisterCvars(void)
{
    cg_remotePrediction         = cgi.Cvar_Get("cg_remotePrediction", "0", CVAR_ARCHIVE);
    // Changed in Omaha: default under-lead (measured fire disagree climbed with full lead).
    cg_remotePredictionScale    = cgi.Cvar_Get("cg_remotePredictionScale", "0.7", CVAR_ARCHIVE);
    cg_remotePredictionMaxLead  = cgi.Cvar_Get("cg_remotePredictionMaxLead", "200", CVAR_ARCHIVE);
    cg_remotePredictionStepMsec = cgi.Cvar_Get("cg_remotePredictionStepMsec", "16", CVAR_ARCHIVE);
    cg_remotePredictionSmooth   = cgi.Cvar_Get("cg_remotePredictionSmooth", "50", CVAR_ARCHIVE);
    cg_remotePredictionSnapDist = cgi.Cvar_Get("cg_remotePredictionSnapDist", "128", CVAR_ARCHIVE);
    cg_remotePredictionMaxDist  = cgi.Cvar_Get("cg_remotePredictionMaxDist", "512", CVAR_ARCHIVE);
    cg_remotePredictionMinSpeed = cgi.Cvar_Get("cg_remotePredictionMinSpeed", "16", CVAR_ARCHIVE);
    cg_remotePredictionDebug    = cgi.Cvar_Get("cg_remotePredictionDebug", "0", 0);
    // Added in Omaha: 0 = ping-only lead (public flesh hit skew preferred this over ping+full interp).
    cg_remotePredictionInterpWeight =
        cgi.Cvar_Get("cg_remotePredictionInterpWeight", "0", CVAR_ARCHIVE);
    // Added in Omaha: soft-cap base lead above knee before Scale (high-ping under-lead).
    cg_remotePredictionLeadKnee = cgi.Cvar_Get("cg_remotePredictionLeadKnee", "100", CVAR_ARCHIVE);
    cg_remotePredictionLeadKneeFrac =
        cgi.Cvar_Get("cg_remotePredictionLeadKneeFrac", "0.5", CVAR_ARCHIVE);

    cgi.Cvar_CheckRange(cg_remotePrediction, 0, 2, qtrue);
    cgi.Cvar_CheckRange(cg_remotePredictionScale, 0, 1.5f, qfalse);
    cgi.Cvar_CheckRange(cg_remotePredictionMaxLead, 0, 500, qtrue);
    cgi.Cvar_CheckRange(cg_remotePredictionStepMsec, 4, 50, qtrue);
    cgi.Cvar_CheckRange(cg_remotePredictionSmooth, 0, 500, qtrue);
    cgi.Cvar_CheckRange(cg_remotePredictionSnapDist, 0, 1024, qfalse);
    cgi.Cvar_CheckRange(cg_remotePredictionMaxDist, 0, 2048, qfalse);
    cgi.Cvar_CheckRange(cg_remotePredictionMinSpeed, 0, 200, qfalse);
    cgi.Cvar_CheckRange(cg_remotePredictionDebug, 0, 2, qtrue);
    cgi.Cvar_CheckRange(cg_remotePredictionInterpWeight, 0, 1.0f, qfalse);
    cgi.Cvar_CheckRange(cg_remotePredictionLeadKnee, 0, 500, qtrue);
    cgi.Cvar_CheckRange(cg_remotePredictionLeadKneeFrac, 0, 1.0f, qfalse);
}

void CG_RP_Init(void)
{
    int i;

    for (i = 0; i < MAX_GENTITIES; i++) {
        CG_RP_InvalidateEntity(&rp_entities[i]);
    }

    
    rp_frameActive         = qfalse;
    rp_leadMsec            = 0;
    rp_pingSmoothed        = 0.0f;
    rp_lastPingSnapTime    = 0;
    rp_debugPredictedCount = 0;
    rp_debugDispSum        = 0.0f;
    rp_lastDebugPrintTime  = 0;
}

/*
=================
CG_RP_BeginFrame

Lead derivation (mode 2):

  Seed remotes from the interpolated net pose at cg.time. A usercmd stamped
  now reaches the server after roughly one RTT. Public flesh-hit logs showed
  ping + full interp over-led (auth-only impacts >> pred-only), so the default
  is ping-only lead, optional interp weight, a soft knee above LeadKnee, then
  Scale under-lead.

  Stock MOHAA servers do no rewinding (no antilag in fgame), so this lead is
  the aim point rather than an error — within the limits of coast prediction.
=================
*/
void CG_RP_BeginFrame(void)
{
    float interpMs;
    float lead;
    float knee;
    float kneeFrac;
    float interpWeight;
    int   mode;
    int   rawPing;

    rp_frameActive         = qfalse;
    rp_leadMsec            = 0;
    rp_debugPredictedCount = 0;
    rp_debugDispSum        = 0.0f;

    mode = cg_remotePrediction->integer;
    if (mode <= 0) {
        return;
    }
    if (cg.demoPlayback || !cg.snap) {
        return;
    }
    if (cgs.gametype == GT_SINGLE_PLAYER) {
        return;
    }

    // Smooth ping on snapshot arrivals only (paced by ~20 Hz, not framerate).
    if (cg.snap->serverTime != rp_lastPingSnapTime) {
        rawPing = cg.snap->ping;
        if (rawPing < 0) {
            rawPing = 0;
        } else if (rawPing > 999) {
            rawPing = 999;
        }

        if (rp_lastPingSnapTime == 0) {
            rp_pingSmoothed = (float)rawPing;
        } else {
            rp_pingSmoothed =
                rp_pingSmoothed + RP_PING_EMA_ALPHA * ((float)rawPing - rp_pingSmoothed);
        }
        rp_lastPingSnapTime = cg.snap->serverTime;
    }

    interpMs = (float)(cg.latestSnapshotTime - cg.time);

    if (mode == 1) {
        // Safe: only cover overshoot past the newest data.
        lead = (float)(cg.time - cg.latestSnapshotTime);
        if (lead < 0.0f) {
            lead = 0.0f;
        }
    } else {
        // Mode 2: RTT lead; interp is optional (default weight 0 — see cvar).
        lead = rp_pingSmoothed;
        interpWeight = cg_remotePredictionInterpWeight->value;
        if (interpWeight < 0.0f) {
            interpWeight = 0.0f;
        } else if (interpWeight > 1.0f) {
            interpWeight = 1.0f;
        }
        if (interpMs > 0.0f && interpWeight > 0.0f) {
            lead += interpMs * interpWeight;
        }
    }


    // Soft knee: compress excess base lead above LeadKnee.
    if (mode == 2) {
        knee     = (float)cg_remotePredictionLeadKnee->integer;
        kneeFrac = cg_remotePredictionLeadKneeFrac->value;
        if (kneeFrac < 0.0f) {
            kneeFrac = 0.0f;
        } else if (kneeFrac > 1.0f) {
            kneeFrac = 1.0f;
        }
        if (knee > 0.0f && lead > knee) {
            lead = knee + (lead - knee) * kneeFrac;
        }
    }

    lead *= cg_remotePredictionScale->value;
    if (lead < 0.0f) {
        lead = 0.0f;
    }
    if (lead > (float)cg_remotePredictionMaxLead->integer) {
        lead = (float)cg_remotePredictionMaxLead->integer;
    }

    rp_leadMsec    = (int)(lead + 0.5f);
    rp_frameActive = qtrue;

    }

int CG_RP_GetLeadMsec(void)
{
    return rp_leadMsec;
}


void CG_RP_UpdateEntity(centity_t *cent)
{
    rpEntity_t *rp;
    rpSeed_t    seed;
    vec3_t      predOrigin;
    vec3_t      rawCoast;
    vec3_t      afterClamp;
    vec3_t      offset;
    vec3_t      finalOrigin;
    float       errLen;
    float       maxDist;
    float       alpha;
    float       tau;
    float       dt;
    float       confTarget;
    float       velDelta;
    int         lead;
    int         num;
    trace_t     reach;
    qboolean    clampHit;

    if (!rp_frameActive || !cent) {
        return;
    }

    num = cent->currentState.number;
    if (num < 0 || num >= MAX_GENTITIES) {
        return;
    }

    rp = &rp_entities[num];

    // Stale / teleport / lost entity: drop smoothed state.
    if (!cent->currentValid || cent->teleported
        || (cg.time - cent->snapShotTime > RP_STALE_MS && cent->snapShotTime != 0)) {
        CG_RP_InvalidateEntity(rp);
        return;
    }

    CG_RP_PushHistory(rp, cent);

    
    if (!CG_RP_BuildSeed(cent, &seed, rp)) {
        rp->valid = qfalse;
        return;
    }

    // Confidence: pull lead back when seed velocity changes sharply (juke).
    if (rp->valid) {
        VectorSubtract(seed.velocity, rp->lastSeedVelocity, offset);
        velDelta   = VectorLength(offset);
        confTarget = 1.0f - ((velDelta - RP_CONF_DROP_START) / RP_CONF_DROP_RANGE);
        if (confTarget < 0.0f) {
            confTarget = 0.0f;
        } else if (confTarget > 1.0f) {
            confTarget = 1.0f;
        }

        if (confTarget < rp->confidence) {
            rp->confidence = confTarget; // drop fast
        } else {
            dt = (float)cg.frametime;
            if (dt < 1.0f) {
                dt = 1.0f;
            }
            alpha = dt / RP_CONF_RECOVER_MS;
            if (alpha > 1.0f) {
                alpha = 1.0f;
            }
            rp->confidence += (confTarget - rp->confidence) * alpha;
        }
    } else {
        rp->confidence = 1.0f;
    }
    VectorCopy(seed.velocity, rp->lastSeedVelocity);

    lead = (int)((float)rp_leadMsec * rp->confidence + 0.5f);
    if (lead <= 0) {
        rp->valid = qfalse;
        return;
    }

    if (!CG_RP_Coast(&seed, lead, predOrigin)) {
        rp->valid = qfalse;
        return;
    }
    VectorCopy(predOrigin, rawCoast);

    // Max displacement clamp.
    clampHit = qfalse;
    VectorSubtract(predOrigin, cent->netLerpOrigin, offset);
    errLen  = VectorLength(offset);
    maxDist = cg_remotePredictionMaxDist->value;
    if (maxDist > 0.0f && errLen > maxDist) {
        VectorScale(offset, maxDist / errLen, offset);
        VectorAdd(cent->netLerpOrigin, offset, predOrigin);
        errLen    = maxDist;
        clampHit  = qtrue;
    }
    VectorCopy(predOrigin, afterClamp);

    // Output smoothing.
    tau = (float)cg_remotePredictionSmooth->integer;
    dt  = (float)cg.frametime;
    if (dt < 1.0f) {
        dt = 1.0f;
    }

    if (!rp->valid || cent->teleported || tau <= 0.0f
        || Distance(predOrigin, rp->smoothOrigin) > cg_remotePredictionSnapDist->value) {
        VectorCopy(predOrigin, rp->smoothOrigin);
    } else {
        alpha = 1.0f - (float)exp(-(double)(dt / tau));
        if (alpha < 0.0f) {
            alpha = 0.0f;
        } else if (alpha > 1.0f) {
            alpha = 1.0f;
        }
        VectorSubtract(predOrigin, rp->smoothOrigin, offset);
        VectorMA(rp->smoothOrigin, alpha, offset, rp->smoothOrigin);
    }

    // Reachability last: displayed box must be reachable from authoritative pose.
    CG_Trace(
        &reach,
        cent->netLerpOrigin,
        seed.mins,
        seed.maxs,
        rp->smoothOrigin,
        seed.entityNum,
        MASK_PLAYERSOLID,
        qtrue,
        qtrue,
        "RemotePredictReach"
    );
    if (reach.allsolid) {
        VectorCopy(cent->netLerpOrigin, finalOrigin);
    } else {
        VectorCopy(reach.endpos, finalOrigin);
    }

    // Do NOT modify lerpAngles — extrapolating view angles produces head-snapping
    // and buys nothing for hit geometry.
    VectorCopy(finalOrigin, cent->lerpOrigin);
    VectorCopy(finalOrigin, rp->predOrigin);
    VectorCopy(finalOrigin, rp->smoothOrigin);
    VectorCopy(seed.mins, rp->mins);
    VectorCopy(seed.maxs, rp->maxs);
    rp->valid          = qtrue;
    rp->lastUpdateTime = cg.time;

    rp_debugPredictedCount++;
    rp_debugDispSum += Distance(finalOrigin, cent->netLerpOrigin);

    }

qboolean CG_RP_GetPredictedBounds(int entityNum, vec3_t origin, vec3_t mins, vec3_t maxs)
{
    rpEntity_t *rp;

    if (entityNum < 0 || entityNum >= MAX_GENTITIES) {
        return qfalse;
    }

    rp = &rp_entities[entityNum];
    if (!rp->valid) {
        return qfalse;
    }
    if (cg.time - rp->lastUpdateTime > RP_BOUNDS_STALE_MS) {
        return qfalse;
    }

    VectorCopy(rp->predOrigin, origin);
    VectorCopy(rp->mins, mins);
    VectorCopy(rp->maxs, maxs);
    return qtrue;
}


// ---------------------------------------------------------------------------
// Debug visualization
// ---------------------------------------------------------------------------

static void CG_RP_DebugBox(const vec3_t origin, const vec3_t mins, const vec3_t maxs, float r, float g, float b)
{
    vec3_t c[8];
    int    i;

    // 8 corners of world-space AABB.
    for (i = 0; i < 8; i++) {
        c[i][0] = origin[0] + ((i & 1) ? maxs[0] : mins[0]);
        c[i][1] = origin[1] + ((i & 2) ? maxs[1] : mins[1]);
        c[i][2] = origin[2] + ((i & 4) ? maxs[2] : mins[2]);
    }

    // Bottom
    cgi.R_DebugLine(c[0], c[1], r, g, b, 1.0f);
    cgi.R_DebugLine(c[1], c[3], r, g, b, 1.0f);
    cgi.R_DebugLine(c[3], c[2], r, g, b, 1.0f);
    cgi.R_DebugLine(c[2], c[0], r, g, b, 1.0f);
    // Top
    cgi.R_DebugLine(c[4], c[5], r, g, b, 1.0f);
    cgi.R_DebugLine(c[5], c[7], r, g, b, 1.0f);
    cgi.R_DebugLine(c[7], c[6], r, g, b, 1.0f);
    cgi.R_DebugLine(c[6], c[4], r, g, b, 1.0f);
    // Verticals
    cgi.R_DebugLine(c[0], c[4], r, g, b, 1.0f);
    cgi.R_DebugLine(c[1], c[5], r, g, b, 1.0f);
    cgi.R_DebugLine(c[2], c[6], r, g, b, 1.0f);
    cgi.R_DebugLine(c[3], c[7], r, g, b, 1.0f);
}

void CG_RP_DebugDraw(void)
{
    int   i;
    float meanDisp;

    if (!cg_remotePredictionDebug || cg_remotePredictionDebug->integer <= 0) {
        return;
    }
    if (!rp_frameActive) {
        return;
    }

    for (i = 0; i < MAX_GENTITIES; i++) {
        rpEntity_t *rp;
        centity_t  *cent;
        vec3_t      a, b;

        rp = &rp_entities[i];
        if (!rp->valid) {
            continue;
        }
        if (cg.time - rp->lastUpdateTime > RP_BOUNDS_STALE_MS) {
            continue;
        }

        cent = &cg_entities[i];

        // White = authoritative, cyan = predicted.
        CG_RP_DebugBox(cent->netLerpOrigin, rp->mins, rp->maxs, 1.0f, 1.0f, 1.0f);
        CG_RP_DebugBox(cent->lerpOrigin, rp->mins, rp->maxs, 0.0f, 1.0f, 1.0f);

        VectorCopy(cent->netLerpOrigin, a);
        VectorCopy(cent->lerpOrigin, b);
        a[2] += (rp->mins[2] + rp->maxs[2]) * 0.5f;
        b[2] += (rp->mins[2] + rp->maxs[2]) * 0.5f;
        cgi.R_DebugLine(a, b, 1.0f, 1.0f, 0.0f, 1.0f);
    }

    if (cg_remotePredictionDebug->integer >= 2) {
        if (cg.time - rp_lastDebugPrintTime >= 500) {
            meanDisp = (rp_debugPredictedCount > 0) ? (rp_debugDispSum / (float)rp_debugPredictedCount) : 0.0f;
            cgi.Printf(
                "rp: ping=%.0f lead=%d ents=%d meanDisp=%.1f conf steps<=%d\n",
                rp_pingSmoothed,
                rp_leadMsec,
                rp_debugPredictedCount,
                meanDisp,
                RP_MAX_STEPS
            );
            rp_lastDebugPrintTime = cg.time;
        }
    }
}
