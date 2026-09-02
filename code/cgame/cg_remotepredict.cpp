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
// Added in Omaha: multi-hypothesis coasts (straight / velocity-yaw curve / decel)
// blended by hard-coded ping-scheduled weights driven from smoothed ping.
//

#include "cg_local.h"
#include "cg_remotepredict.h"
#include "../fgame/bg_local.h"

#include <math.h>
#include <string.h>

// Set to 1 and rebuild cgame to expose cg_remotePredictionDebug (draw/stats).
// Shipping builds leave this at 0 — no cvar, no cheats path.
#ifndef CG_RP_DEBUG_DRAW
#define CG_RP_DEBUG_DRAW 0
#endif

// ---------------------------------------------------------------------------
// Cvars
// ---------------------------------------------------------------------------

static cvar_t *cg_remotePrediction;        // 0 off / 1 safe / 2 lead
static cvar_t *cg_remotePredictionMaxLead; // 0–150 ms ceiling

#if CG_RP_DEBUG_DRAW
static cvar_t *cg_remotePredictionDebug;
#endif

// Locked internals (not player cvars — retune requires rebuild).
#define RP_LEAD_SCALE           1.0f
#define RP_INTERP_WEIGHT        0.0f
#define RP_PING_SMOOTH_ALPHA    0.15f
#define RP_STEP_MSEC            16
#define RP_OUT_SMOOTH_MS        50
#define RP_SNAP_DIST            128.0f
#define RP_MAX_DIST             512.0f
#define RP_MIN_SPEED            16.0f
#define RP_BLEND_PING_LOW       40.0f
#define RP_BLEND_PING_MID       80.0f
#define RP_BLEND_PING_HIGH      140.0f
#define RP_BLEND_CURVE_W_LOW    0.55f
#define RP_BLEND_CURVE_W_MID    0.55f
#define RP_BLEND_CURVE_W_HIGH   0.48f
#define RP_BLEND_DECEL_W_LOW    0.04f
#define RP_BLEND_DECEL_W_MID    0.02f
#define RP_BLEND_DECEL_W_HIGH   0.01f
#define RP_MAX_YAW_RATE         4.0f
#define RP_DECEL_RATE           400.0f
#define RP_CURVE_STAB_MIN       0.35f
#define RP_DECEL_TREND_MIN      80.0f

// ---------------------------------------------------------------------------
// Frame / entity state
// ---------------------------------------------------------------------------

typedef enum {
    RP_COAST_STRAIGHT = 0,
    RP_COAST_CURVE,
    RP_COAST_DECEL
} rpCoastMode_t;

typedef struct {
    vec3_t   origin;
    vec3_t   velocity;
    vec3_t   mins;
    vec3_t   maxs;
    int      entityNum;
    int      gravity;
    qboolean grounded;
} rpSeed_t;

#define RP_HIST_SIZE 8

typedef struct {
    int    time;
    vec3_t origin;
    vec3_t trDelta;
    qboolean hasTrDelta;
} rpHistSample_t;

typedef struct {
    qboolean valid;
    int      lastUpdateTime;
    vec3_t   smoothOrigin;
    vec3_t   predOrigin;
    vec3_t   curveOrigin; // debug: pure curve coast
    vec3_t   mins;
    vec3_t   maxs;
    vec3_t   lastSeedVelocity;
    float    confidence;
    // Added in Omaha: snap history ring + motion estimator.
    rpHistSample_t hist[RP_HIST_SIZE];
    int            histCount;
    int            histHead; // next write index
    int            lastSnapTime;
    float          estOmega;      // rad/s horizontal velocity yaw-rate
    float          estSpeedTrend; // ups/s signed d|v_h|/dt (short)
    float          estSpeedTrendLong; // ups/s over ~3 snap gaps
    float          estStability;  // 0..1 curve reliability
    float          estTurnOnset;  // 0..1 recent |ω| rising vs older
    float          estOmegaAbs;   // |estOmega|
    float          estStop;       // 0..1 stop/slow cue (estimator only)
    float          estAccel;      // ups/s^2 short (≈ speedTrend)
    float          dbgWStraight;
    float          dbgWCurve;
    float          dbgWDecel;
    float          dbgCurveAff;
    float          dbgDecelAff;
    int            dbgRegime; // Added in Omaha: soft-mix dominant regime id
    qboolean       dbgAirborne;
} rpEntity_t;

static rpEntity_t rp_entities[MAX_GENTITIES];

static qboolean rp_frameActive;
static int      rp_leadMsec;
static float    rp_pingSmoothed;
static int      rp_lastPingSnapTime;
static int      rp_debugPredictedCount;
static float    rp_debugDispSum;
static int      rp_lastDebugPrintTime;
// Added in Omaha: frame-level blend weights from ping schedule (pre-gate).
static float    rp_schedWCurve;
static float    rp_schedWDecel;

static playerState_t rp_ps;
static pmove_t       rp_pm;

#define RP_MAX_STEPS           16
#define RP_STALE_MS            500
#define RP_HIST_MAX_GAP_MS     250
// Changed in Omaha: drop lead sooner on velocity cuts (jukes); recover slightly slower.
#define RP_CONF_DROP_START     60.0f
#define RP_CONF_DROP_RANGE     300.0f
#define RP_CONF_RECOVER_MS     250.0f
#define RP_DEFAULT_GRAVITY     800
#define RP_BOUNDS_STALE_MS     100
#define RP_MIN_HORIZ_SPEED     8.0f
#define RP_OMEGA_SAMPLES       4

static float CG_RP_ClampFloat(float v, float lo, float hi);
static float CG_RP_Lerp(float a, float b, float t);

// Soft continuous mixer (replaces hard regime floors). Regime ids kept for overlap logs.
typedef enum {
    RP_REGIME_SCHEDULE = 0,
    RP_REGIME_JUKE     = 1, // unused (retired)
    RP_REGIME_CURVE    = 2,
    RP_REGIME_DECEL    = 3,
    RP_REGIME_AIR      = 4,
    RP_REGIME_COUNT
} rpRegime_t;

// Ping-scaled soft boost targets (Scale 1.0 retune).
#define RP_SOFT_CURVE_BOOST_LOW  0.58f
#define RP_SOFT_CURVE_BOOST_MID  0.72f
#define RP_SOFT_CURVE_BOOST_HIGH 0.84f
#define RP_SOFT_DECEL_BOOST_LOW  0.20f
#define RP_SOFT_DECEL_BOOST_MID  0.28f
#define RP_SOFT_DECEL_BOOST_HIGH 0.34f
static float CG_RP_Smoothstep(float edge0, float edge1, float x)
{
    float t;
    float lo = edge0;
    float hi = edge1;
    float ascending = 1.0f;

    if (edge1 < edge0) {
        lo        = edge1;
        hi        = edge0;
        ascending = 0.0f;
    }
    if (hi <= lo) {
        return (x >= hi) ? 1.0f : 0.0f;
    }
    t = CG_RP_ClampFloat((x - lo) / (hi - lo), 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    return ascending ? t : (1.0f - t);
}

static float CG_RP_LookupPingKnot(float ping, float vLow, float vMid, float vHigh)
{
    float pLow  = RP_BLEND_PING_LOW;
    float pMid  = RP_BLEND_PING_MID;
    float pHigh = RP_BLEND_PING_HIGH;
    float t;

    if (pMid < pLow) {
        pMid = pLow;
    }
    if (pHigh < pMid) {
        pHigh = pMid;
    }

    if (ping <= pLow) {
        return vLow;
    }
    if (ping >= pHigh) {
        return vHigh;
    }
    if (ping <= pMid) {
        t = (pMid > pLow) ? ((ping - pLow) / (pMid - pLow)) : 0.0f;
        return CG_RP_Lerp(vLow, vMid, t);
    }
    t = (pHigh > pMid) ? ((ping - pMid) / (pHigh - pMid)) : 0.0f;
    return CG_RP_Lerp(vMid, vHigh, t);
}

static void CG_RP_LookupRegimeBoosts(float ping, float *outCurveBoost, float *outDecelBoost)
{
    *outCurveBoost = CG_RP_LookupPingKnot(
        ping, RP_SOFT_CURVE_BOOST_LOW, RP_SOFT_CURVE_BOOST_MID, RP_SOFT_CURVE_BOOST_HIGH
    );
    *outDecelBoost = CG_RP_LookupPingKnot(
        ping, RP_SOFT_DECEL_BOOST_LOW, RP_SOFT_DECEL_BOOST_MID, RP_SOFT_DECEL_BOOST_HIGH
    );
}

// Soft feature mixer: continuous affinities lift schedule weights toward ping-scaled
// boosts (no hard floors / mutual hard zeros). Returns dominant regime for logs.
static int CG_RP_ApplySoftMixer(
    qboolean grounded,
    float    stability,
    float    omega,
    float    speedTrend,
    float    speedTrendLong,
    float    turnOnset,
    float    ping,
    float    schedCurve,
    float    schedDecel,
    float    stabMin,
    float    trendMin,
    float   *outWS,
    float   *outWC,
    float   *outWD,
    float   *outCurveAff,
    float   *outDecelAff
)
{
    float wCurve = schedCurve;
    float wDecel = schedDecel;
    float wStraight;
    float wSum;
    float absOm;
    float curveBoost;
    float decelBoost;
    float stabFrac;
    float curveAff;
    float decelAff;
    float slowMag;
    float slowMagLong;
    int   regime = RP_REGIME_SCHEDULE;

    if (outCurveAff) {
        *outCurveAff = 0.0f;
    }
    if (outDecelAff) {
        *outDecelAff = 0.0f;
    }

    if (!grounded) {
        *outWS = 1.0f;
        *outWC = 0.0f;
        *outWD = 0.0f;
        return RP_REGIME_AIR;
    }

    CG_RP_LookupRegimeBoosts(ping, &curveBoost, &decelBoost);
    absOm = (float)fabs((double)omega);

    // Soft schedule base (sqrt stab gate; soft decel enable).
    if (stability <= stabMin) {
        stabFrac = 0.0f;
        wCurve   = 0.0f;
    } else {
        stabFrac = (stability - stabMin) / Q_max(1.0f - stabMin, 0.01f);
        stabFrac = CG_RP_ClampFloat(stabFrac, 0.0f, 1.0f);
        wCurve *= (float)sqrt((double)stabFrac);
    }
    {
        float decelGate;

        // Soft enable as trend goes more negative past trendMin.
        if (speedTrend >= -trendMin * 0.25f) {
            decelGate = 0.0f;
        } else if (speedTrend <= -trendMin) {
            decelGate = 1.0f;
        } else {
            decelGate = (-speedTrend - trendMin * 0.25f) / Q_max(trendMin * 0.75f, 1.0f);
            decelGate = CG_RP_ClampFloat(decelGate, 0.0f, 1.0f);
            decelGate = decelGate * decelGate * (3.0f - 2.0f * decelGate);
        }
        wDecel *= decelGate;
    }

    // Continuous affinities — steeper than v1 (v1 under-boosted vs hard v3 floors).
    // Align soft ramps with former hard gates (stab≥0.55, |ω|≥1.5) but keep continuity.
    curveAff = CG_RP_Smoothstep(0.45f, 0.62f, stability) * CG_RP_Smoothstep(0.8f, 1.8f, absOm);
    curveAff = CG_RP_ClampFloat(curveAff * (1.0f + 0.20f * turnOnset), 0.0f, 1.0f);
    // Lift mid affinities so soft mix reaches boost sooner (sqrt).
    curveAff = (float)sqrt((double)curveAff);

    slowMag     = CG_RP_ClampFloat((-speedTrend - trendMin) / 500.0f, 0.0f, 1.0f);
    slowMagLong = CG_RP_ClampFloat((-speedTrendLong - trendMin) / 500.0f, 0.0f, 1.0f);
    if (speedTrend > -trendMin * 0.5f && speedTrendLong > -trendMin * 0.5f) {
        slowMag     = 0.0f;
        slowMagLong = 0.0f;
    }
    slowMag  = Q_max(slowMag, slowMagLong * 0.85f);
    decelAff = slowMag * CG_RP_Smoothstep(0.50f, 0.18f, stability);
    decelAff = (float)sqrt((double)CG_RP_ClampFloat(decelAff, 0.0f, 1.0f));
    // Mild soft competition (v1 0.55/0.45 was too aggressive).
    {
        float cKeep = 1.0f - 0.25f * decelAff;
        float dKeep = 1.0f - 0.20f * curveAff;
        curveAff *= cKeep;
        decelAff *= dKeep;
    }

    // Soft lift toward ping-scaled boost targets.
    wCurve = wCurve + curveAff * (curveBoost - wCurve);
    wDecel = wDecel + decelAff * (decelBoost - wDecel);

    wStraight = 1.0f - wCurve - wDecel;
    if (wStraight < 0.0f) {
        wSum = wCurve + wDecel;
        if (wSum > 1e-4f) {
            wCurve /= wSum;
            wDecel /= wSum;
        }
        wStraight = 0.0f;
    }

    if (curveAff >= 0.35f && curveAff >= decelAff) {
        regime = RP_REGIME_CURVE;
    } else if (decelAff >= 0.35f) {
        regime = RP_REGIME_DECEL;
    }

    if (outCurveAff) {
        *outCurveAff = curveAff;
    }
    if (outDecelAff) {
        *outDecelAff = decelAff;
    }

    *outWS = wStraight;
    *outWC = wCurve;
    *outWD = wDecel;
    return regime;
}

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
// Helpers
// ---------------------------------------------------------------------------

static float CG_RP_ClampFloat(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static float CG_RP_Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

static void CG_RP_InvalidateEntity(rpEntity_t *rp)
{
    memset(rp, 0, sizeof(*rp));
    rp->confidence   = 1.0f;
    rp->estStability = 0.0f;
}

// Piecewise-linear 3-knot schedule on smoothed ping → curve/decel weights.
static void CG_RP_LookupBlendWeights(float ping, float *outCurve, float *outDecel)
{
    float pLow  = RP_BLEND_PING_LOW;
    float pMid  = RP_BLEND_PING_MID;
    float pHigh = RP_BLEND_PING_HIGH;
    float cLow  = RP_BLEND_CURVE_W_LOW;
    float cMid  = RP_BLEND_CURVE_W_MID;
    float cHigh = RP_BLEND_CURVE_W_HIGH;
    float dLow  = RP_BLEND_DECEL_W_LOW;
    float dMid  = RP_BLEND_DECEL_W_MID;
    float dHigh = RP_BLEND_DECEL_W_HIGH;
    float t;
    float curve;
    float decel;
    float sum;

    // Keep knots ordered.
    if (pMid < pLow) {
        pMid = pLow;
    }
    if (pHigh < pMid) {
        pHigh = pMid;
    }

    if (ping <= pLow) {
        curve = cLow;
        decel = dLow;
    } else if (ping >= pHigh) {
        curve = cHigh;
        decel = dHigh;
    } else if (ping <= pMid) {
        t     = (pMid > pLow) ? ((ping - pLow) / (pMid - pLow)) : 0.0f;
        curve = CG_RP_Lerp(cLow, cMid, t);
        decel = CG_RP_Lerp(dLow, dMid, t);
    } else {
        t     = (pHigh > pMid) ? ((ping - pMid) / (pHigh - pMid)) : 0.0f;
        curve = CG_RP_Lerp(cMid, cHigh, t);
        decel = CG_RP_Lerp(dMid, dHigh, t);
    }

    curve = CG_RP_ClampFloat(curve, 0.0f, 1.0f);
    decel = CG_RP_ClampFloat(decel, 0.0f, 1.0f);
    sum   = curve + decel;
    if (sum > 1.0f) {
        curve /= sum;
        decel /= sum;
    }

    *outCurve = curve;
    *outDecel = decel;
}

static int CG_RP_HistIndex(const rpEntity_t *rp, int age)
{
    // age 0 = most recent sample, age 1 = previous, ...
    int idx;

    if (age < 0 || age >= rp->histCount) {
        return -1;
    }
    idx = rp->histHead - 1 - age;
    while (idx < 0) {
        idx += RP_HIST_SIZE;
    }
    return idx;
}

static qboolean CG_RP_SampleHorizVel(const rpHistSample_t *a, const rpHistSample_t *b, vec3_t outHoriz, float *outSpeed)
{
    int   dt;
    float inv;
    vec3_t delta;

    if (a->hasTrDelta && VectorLength(a->trDelta) >= 1.0f) {
        outHoriz[0] = a->trDelta[0];
        outHoriz[1] = a->trDelta[1];
        outHoriz[2] = 0.0f;
        *outSpeed   = VectorLength(outHoriz);
        return (*outSpeed >= RP_MIN_HORIZ_SPEED) ? qtrue : qfalse;
    }

    if (!b || a->time <= b->time) {
        return qfalse;
    }
    dt = a->time - b->time;
    if (dt <= 0 || dt > RP_HIST_MAX_GAP_MS) {
        return qfalse;
    }

    VectorSubtract(a->origin, b->origin, delta);
    inv         = 1000.0f / (float)dt;
    outHoriz[0] = delta[0] * inv;
    outHoriz[1] = delta[1] * inv;
    outHoriz[2] = 0.0f;
    *outSpeed   = VectorLength(outHoriz);
    return (*outSpeed >= RP_MIN_HORIZ_SPEED) ? qtrue : qfalse;
}

static void CG_RP_UpdateEstimator(rpEntity_t *rp)
{
    int   i;
    int   nOmega;
    int   idx0, idx1, idx2, idx3;
    float omegaSum;
    float omegaAbsSum;
    float omegaRecent;
    float omegaOlder;
    float dtSec;
    float sp0, sp1, spLong;
    float spRecent, spPrev;
    float yaw0, yaw1;
    float dyaw;
    float omega;
    float consistency;
    float magFactor;
    float speedConsist;
    float absOmega;
    vec3_t v0, v1, vLong;
    const rpHistSample_t *s0;
    const rpHistSample_t *s1;
    const rpHistSample_t *s2;
    const rpHistSample_t *s3;
    const rpHistSample_t *sOlder;

    rp->estOmega          = 0.0f;
    rp->estOmegaAbs       = 0.0f;
    rp->estSpeedTrend     = 0.0f;
    rp->estSpeedTrendLong = 0.0f;
    rp->estStability      = 0.0f;
    rp->estTurnOnset      = 0.0f;
    rp->estStop           = 0.0f;
    rp->estAccel          = 0.0f;
    spRecent              = 0.0f;
    spPrev                = 0.0f;

    if (rp->histCount < 2) {
        return;
    }

    // Speed trend from the two most recent samples.
    idx0 = CG_RP_HistIndex(rp, 0);
    idx1 = CG_RP_HistIndex(rp, 1);
    if (idx0 < 0 || idx1 < 0) {
        return;
    }
    s0     = &rp->hist[idx0];
    s1     = &rp->hist[idx1];
    idx2   = CG_RP_HistIndex(rp, 2);
    sOlder = (idx2 >= 0) ? &rp->hist[idx2] : NULL;
    idx3   = CG_RP_HistIndex(rp, 3);
    s3     = (idx3 >= 0) ? &rp->hist[idx3] : NULL;

    if (CG_RP_SampleHorizVel(s0, s1, v0, &sp0) && CG_RP_SampleHorizVel(s1, sOlder, v1, &sp1)) {
        spRecent = sp0;
        spPrev   = sp1;
        dtSec    = (s0->time - s1->time) * 0.001f;
        if (dtSec > 0.001f) {
            rp->estSpeedTrend = (sp0 - sp1) / dtSec;
        }
    }

    // Longer trend: newest vel vs ~3 gaps back.
    if (s3 && CG_RP_SampleHorizVel(s0, s1, v0, &sp0) && CG_RP_SampleHorizVel(s3, NULL, vLong, &spLong)) {
        dtSec = (s0->time - s3->time) * 0.001f;
        if (dtSec > 0.001f) {
            rp->estSpeedTrendLong = (sp0 - spLong) / dtSec;
        }
    } else if (sOlder && CG_RP_SampleHorizVel(s0, s1, v0, &sp0)
               && CG_RP_SampleHorizVel(sOlder, s3, vLong, &spLong)) {
        dtSec = (s0->time - sOlder->time) * 0.001f;
        if (dtSec > 0.001f) {
            rp->estSpeedTrendLong = (sp0 - spLong) / dtSec;
        }
    }

    // Omega: average over recent samples.
    nOmega      = 0;
    omegaSum    = 0.0f;
    omegaAbsSum = 0.0f;
    omegaRecent = 0.0f;
    omegaOlder  = 0.0f;

    for (i = 0; i < RP_OMEGA_SAMPLES; i++) {
        idx0 = CG_RP_HistIndex(rp, i);
        idx1 = CG_RP_HistIndex(rp, i + 1);
        idx2 = CG_RP_HistIndex(rp, i + 2);
        if (idx0 < 0 || idx1 < 0) {
            break;
        }
        s0 = &rp->hist[idx0];
        s1 = &rp->hist[idx1];
        s2 = (idx2 >= 0) ? &rp->hist[idx2] : NULL;

        if (!CG_RP_SampleHorizVel(s0, s1, v0, &sp0)) {
            break;
        }
        if (!CG_RP_SampleHorizVel(s1, s2, v1, &sp1)) {
            break;
        }

        yaw0 = atan2(v0[1], v0[0]);
        yaw1 = atan2(v1[1], v1[0]);
        dyaw = yaw0 - yaw1;
        while (dyaw > (float)M_PI) {
            dyaw -= (float)(2.0 * M_PI);
        }
        while (dyaw < (float)(-M_PI)) {
            dyaw += (float)(2.0 * M_PI);
        }

        dtSec = (s0->time - s1->time) * 0.001f;
        if (dtSec < 0.001f) {
            break;
        }
        omega = dyaw / dtSec;

        omegaSum += omega;
        omegaAbsSum += fabs(omega);
        if (i == 0) {
            omegaRecent = fabs(omega);
        }
        if (i >= 2) {
            omegaOlder += fabs(omega);
        }
        nOmega++;
    }

    if (nOmega <= 0) {
        return;
    }

    rp->estOmega    = omegaSum / (float)nOmega;
    rp->estOmegaAbs = fabs(rp->estOmega);
    rp->estAccel    = rp->estSpeedTrend;

    // Turn onset: recent |ω| rising vs older samples.
    if (nOmega >= 3) {
        float olderAvg = omegaOlder / (float)Q_max(nOmega - 2, 1);
        if (olderAvg < 1e-3f) {
            rp->estTurnOnset = CG_RP_ClampFloat(omegaRecent / 2.0f, 0.0f, 1.0f);
        } else {
            rp->estTurnOnset = CG_RP_ClampFloat((omegaRecent - olderAvg) / Q_max(olderAvg, 0.5f), 0.0f, 1.0f);
        }
    }

    // Stop/slow cue kept for overlap logs only (mixer stop path rejected).
    {
        float slow = CG_RP_Smoothstep(48.0f, 10.0f, spRecent);
        float cut  = CG_RP_ClampFloat((-rp->estSpeedTrend) / 500.0f, 0.0f, 1.0f);
        float cutL = CG_RP_ClampFloat((-rp->estSpeedTrendLong) / 500.0f, 0.0f, 1.0f);
        rp->estStop = CG_RP_ClampFloat(slow * Q_max(cut, cutL), 0.0f, 1.0f);
    }

    // Stability: directional agreement * soft magnitude * speed agreement.
    consistency = 0.0f;
    if (omegaAbsSum > 1e-4f) {
        consistency = fabs(omegaSum) / omegaAbsSum;
    }

    absOmega     = rp->estOmegaAbs;
    magFactor    = CG_RP_ClampFloat((absOmega - 0.3f) / 2.0f, 0.0f, 1.0f);
    speedConsist = 1.0f;
    if (spRecent > 1.0f && spPrev > 1.0f) {
        float ratio = spRecent / spPrev;
        if (ratio > 1.0f) {
            ratio = 1.0f / ratio;
        }
        speedConsist = CG_RP_ClampFloat((ratio - 0.55f) / 0.45f, 0.35f, 1.0f);
    }
    rp->estStability = consistency * magFactor * speedConsist;
    if (nOmega >= 2) {
        rp->estStability = CG_RP_ClampFloat(rp->estStability * 1.1f, 0.0f, 1.0f);
    }
}

static void CG_RP_PushHistory(rpEntity_t *rp, centity_t *cent)
{
    rpHistSample_t *s;
    float           trSpeed;

    if (cent->snapShotTime == rp->lastSnapTime) {
        return;
    }

    s       = &rp->hist[rp->histHead];
    s->time = cent->snapShotTime;
    VectorCopy(cent->currentState.origin, s->origin);
    VectorCopy(cent->currentState.pos.trDelta, s->trDelta);
    trSpeed        = VectorLength(s->trDelta);
    s->hasTrDelta  = (trSpeed >= 1.0f) ? qtrue : qfalse;

    rp->histHead = (rp->histHead + 1) % RP_HIST_SIZE;
    if (rp->histCount < RP_HIST_SIZE) {
        rp->histCount++;
    }
    rp->lastSnapTime = cent->snapShotTime;

    CG_RP_UpdateEstimator(rp);
}

static qboolean CG_RP_DerivedVelocity(const rpEntity_t *rp, vec3_t out)
{
    int                    idx0, idx1;
    const rpHistSample_t  *a;
    const rpHistSample_t  *b;
    int                    dt;
    float                  inv;
    vec3_t                 delta;

    idx0 = CG_RP_HistIndex(rp, 0);
    idx1 = CG_RP_HistIndex(rp, 1);
    if (idx0 < 0 || idx1 < 0) {
        return qfalse;
    }

    a = &rp->hist[idx0];
    b = &rp->hist[idx1];
    if (a->time <= b->time) {
        return qfalse;
    }

    dt = a->time - b->time;
    if (dt > RP_HIST_MAX_GAP_MS) {
        return qfalse;
    }

    inv = 1000.0f / (float)dt;
    VectorSubtract(a->origin, b->origin, delta);
    VectorScale(delta, inv, out);
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
    if (speed < 1.0f && haveDerived && VectorLength(derived) >= RP_MIN_SPEED) {
        // Server likely has g_smoothClients 0 (trDelta zeroed); use origin deltas.
        VectorCopy(derived, seed->velocity);
        speed = VectorLength(seed->velocity);
    }

    if (speed < RP_MIN_SPEED) {
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
// Forward coast via shared PM_StepSlideMove (straight / curve / decel)
// ---------------------------------------------------------------------------

static void CG_RP_ApplyCoastVelocityStep(rpCoastMode_t mode, float omega, float decelRate, float frametime)
{
    float hx, hy, hs, c, s, nx, ny;
    float maxOmega;

    if (frametime <= 0.0f) {
        return;
    }

    if (mode == RP_COAST_CURVE) {
        maxOmega = RP_MAX_YAW_RATE;
        if (maxOmega < 0.0f) {
            maxOmega = 0.0f;
        }
        omega = CG_RP_ClampFloat(omega, -maxOmega, maxOmega);
        if (fabs(omega) > 1e-5f) {
            hx = rp_ps.velocity[0];
            hy = rp_ps.velocity[1];
            hs = sqrt(hx * hx + hy * hy);
            if (hs >= RP_MIN_HORIZ_SPEED) {
                c  = cos(omega * frametime);
                s  = sin(omega * frametime);
                nx = hx * c - hy * s;
                ny = hx * s + hy * c;
                rp_ps.velocity[0] = nx;
                rp_ps.velocity[1] = ny;
            }
        }
    } else if (mode == RP_COAST_DECEL) {
        if (decelRate > 0.0f) {
            hx = rp_ps.velocity[0];
            hy = rp_ps.velocity[1];
            hs = sqrt(hx * hx + hy * hy);
            if (hs > 1.0f) {
                float newHs = hs - decelRate * frametime;
                if (newHs < 0.0f) {
                    newHs = 0.0f;
                }
                rp_ps.velocity[0] = hx * (newHs / hs);
                rp_ps.velocity[1] = hy * (newHs / hs);
            }
        }
    }
}

static qboolean CG_RP_Coast(
    const rpSeed_t *seed,
    int             leadMsec,
    rpCoastMode_t   mode,
    float           omega,
    float           decelRate,
    vec3_t          outOrigin
)
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

    stepMsec = RP_STEP_MSEC;
    if (stepMsec < 4) {
        stepMsec = 4;
    }
    steps = (leadMsec + stepMsec - 1) / stepMsec;
    if (steps > RP_MAX_STEPS) {
        steps = RP_MAX_STEPS;
    }
    remaining = leadMsec;

    for (i = 0; i < steps; i++) {
        int     thisStep;
        vec3_t  point;
        trace_t ground;

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

        // Evolve horizontal velocity for curve/decel before the slide step.
        CG_RP_ApplyCoastVelocityStep(mode, omega, decelRate, pml.frametime);

        // Coast: no PM friction/accel. Straight assumes continued run input;
        // friction would systematically under-lead players who are still running.
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
    // Player-facing only. Blend/lead tuning is compile-time locked above.
    cg_remotePrediction        = cgi.Cvar_Get("cg_remotePrediction", "0", CVAR_ARCHIVE);
    cg_remotePredictionMaxLead = cgi.Cvar_Get("cg_remotePredictionMaxLead", "150", CVAR_ARCHIVE);
#if CG_RP_DEBUG_DRAW
    cg_remotePredictionDebug = cgi.Cvar_Get("cg_remotePredictionDebug", "0", 0);
    cgi.Cvar_CheckRange(cg_remotePredictionDebug, 0, 3, qtrue);
#endif
    cgi.Cvar_CheckRange(cg_remotePrediction, 0, 2, qtrue);
    cgi.Cvar_CheckRange(cg_remotePredictionMaxLead, 0, 150, qtrue);
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
    rp_schedWCurve         = 0.0f;
    rp_schedWDecel         = 0.0f;
}

/*
=================
CG_RP_BeginFrame

Lead derivation (mode 2):

  Seed remotes from the interpolated net pose at cg.time. A usercmd stamped
  now reaches the server after roughly one RTT. Public flesh-hit logs showed
  ping + full interp over-led (auth-only impacts >> pred-only), so the default
  is ping-only lead at Scale 1.0, then MaxLead (player cvar 0–150). Lead tracks
  ping adaptively from 0 up to the configured ceiling. Soft-mix and the blend
  schedule are compile-time locked.

  Stock MOHAA servers do no rewinding (no antilag in fgame), so this lead is
  the aim point rather than an error — within the limits of coast prediction.
=================
*/
void CG_RP_BeginFrame(void)
{
    float interpMs;
    float lead;
    float pingAlpha;
    int   mode;
    int   rawPing;

    rp_frameActive         = qfalse;
    rp_leadMsec            = 0;
    rp_debugPredictedCount = 0;
    rp_debugDispSum        = 0.0f;
    rp_schedWCurve         = 0.0f;
    rp_schedWDecel         = 0.0f;

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

        pingAlpha = RP_PING_SMOOTH_ALPHA;
        if (pingAlpha < 0.01f) {
            pingAlpha = 0.01f;
        } else if (pingAlpha > 1.0f) {
            pingAlpha = 1.0f;
        }

        if (rp_lastPingSnapTime == 0) {
            rp_pingSmoothed = (float)rawPing;
        } else {
            rp_pingSmoothed = rp_pingSmoothed + pingAlpha * ((float)rawPing - rp_pingSmoothed);
        }
        rp_lastPingSnapTime = cg.snap->serverTime;
    }

    // Locked ping schedules for hypothesis blend.
    CG_RP_LookupBlendWeights(rp_pingSmoothed, &rp_schedWCurve, &rp_schedWDecel);

    mode = cg_remotePrediction->integer;
    if (mode <= 0) {
        return;
    }

    interpMs = (float)(cg.latestSnapshotTime - cg.time);

    if (mode == 1) {
        // Safe: only cover overshoot past the newest data.
        lead = (float)(cg.time - cg.latestSnapshotTime);
        if (lead < 0.0f) {
            lead = 0.0f;
        }
    } else {
        // Mode 2: ping-only lead (interp weight locked at 0).
        lead = rp_pingSmoothed;
        if (interpMs > 0.0f && RP_INTERP_WEIGHT > 0.0f) {
            lead += interpMs * RP_INTERP_WEIGHT;
        }
    }

    lead *= RP_LEAD_SCALE;
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
    vec3_t      originStraight;
    vec3_t      originCurve;
    vec3_t      originDecel;
    vec3_t      offset;
    vec3_t      finalOrigin;
    float       errLen;
    float       maxDist;
    float       alpha;
    float       tau;
    float       dt;
    float       confTarget;
    float       velDelta;
    float       wStraight;
    float       wCurve;
    float       wDecel;
    float       stabMin;
    float       trendMin;
    float       decelRate;
    int         lead;
    int         num;
    trace_t     reach;
    qboolean    needCurve;
    qboolean    needDecel;

    if (!rp_frameActive) {
        return;
    }
    if (!cent) {
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

    // Soft mixer on top of ping schedule (continuous affinities, no hard floors).
    wCurve          = rp_schedWCurve;
    wDecel          = rp_schedWDecel;
    rp->dbgAirborne = seed.grounded ? qfalse : qtrue;
    stabMin         = RP_CURVE_STAB_MIN;
    trendMin        = RP_DECEL_TREND_MIN;
    rp->dbgRegime   = CG_RP_ApplySoftMixer(
        seed.grounded,
        rp->estStability,
        rp->estOmega,
        rp->estSpeedTrend,
        rp->estSpeedTrendLong,
        rp->estTurnOnset,
        rp_pingSmoothed,
        wCurve,
        wDecel,
        stabMin,
        trendMin,
        &wStraight,
        &wCurve,
        &wDecel,
        &rp->dbgCurveAff,
        &rp->dbgDecelAff
    );

    rp->dbgWStraight = wStraight;
    rp->dbgWCurve    = wCurve;
    rp->dbgWDecel    = wDecel;

    needCurve = (wCurve > 0.001f) ? qtrue : qfalse;
    needDecel = (wDecel > 0.001f) ? qtrue : qfalse;

    lead = (int)((float)rp_leadMsec * rp->confidence + 0.5f);
    if (lead <= 0) {
        rp->valid = qfalse;
        return;
    }

    if (!CG_RP_Coast(&seed, lead, RP_COAST_STRAIGHT, 0.0f, 0.0f, originStraight)) {
        rp->valid = qfalse;
        return;
    }
    VectorCopy(originStraight, predOrigin);
    VectorCopy(originStraight, originCurve);
    VectorCopy(originStraight, originDecel);

    if (needCurve) {
        CG_RP_Coast(&seed, lead, RP_COAST_CURVE, rp->estOmega, 0.0f, originCurve);
    }
    VectorCopy(originCurve, rp->curveOrigin);

    if (needDecel) {
        decelRate = RP_DECEL_RATE;
        if (decelRate < 0.0f) {
            decelRate = 0.0f;
        }
        CG_RP_Coast(&seed, lead, RP_COAST_DECEL, 0.0f, decelRate, originDecel);
    }

    // Origin blend of hypotheses.
    predOrigin[0] =
        originStraight[0] * wStraight + originCurve[0] * wCurve + originDecel[0] * wDecel;
    predOrigin[1] =
        originStraight[1] * wStraight + originCurve[1] * wCurve + originDecel[1] * wDecel;
    predOrigin[2] =
        originStraight[2] * wStraight + originCurve[2] * wCurve + originDecel[2] * wDecel;

    // Max displacement clamp.
    VectorSubtract(predOrigin, cent->netLerpOrigin, offset);
    errLen  = VectorLength(offset);
    maxDist = RP_MAX_DIST;
    if (maxDist > 0.0f && errLen > maxDist) {
        VectorScale(offset, maxDist / errLen, offset);
        VectorAdd(cent->netLerpOrigin, offset, predOrigin);
    }

    // Output smoothing.
    tau = (float)RP_OUT_SMOOTH_MS;
    dt  = (float)cg.frametime;
    if (dt < 1.0f) {
        dt = 1.0f;
    }

    if (!rp->valid || cent->teleported || tau <= 0.0f
        || Distance(predOrigin, rp->smoothOrigin) > RP_SNAP_DIST) {
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
// Debug visualization (compile-time only — set CG_RP_DEBUG_DRAW to 1 and rebuild)
// ---------------------------------------------------------------------------

#if CG_RP_DEBUG_DRAW

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

        // White = authoritative, cyan = blended predicted.
        CG_RP_DebugBox(cent->netLerpOrigin, rp->mins, rp->maxs, 1.0f, 1.0f, 1.0f);
        CG_RP_DebugBox(cent->lerpOrigin, rp->mins, rp->maxs, 0.0f, 1.0f, 1.0f);

        // Debug >= 3: magenta = pure curve coast (chord vs arc compare).
        if (cg_remotePredictionDebug->integer >= 3 && rp->dbgWCurve > 0.001f) {
            CG_RP_DebugBox(rp->curveOrigin, rp->mins, rp->maxs, 1.0f, 0.0f, 1.0f);
            VectorCopy(cent->netLerpOrigin, a);
            VectorCopy(rp->curveOrigin, b);
            a[2] += (rp->mins[2] + rp->maxs[2]) * 0.5f;
            b[2] += (rp->mins[2] + rp->maxs[2]) * 0.5f;
            cgi.R_DebugLine(a, b, 1.0f, 0.4f, 1.0f, 1.0f);
        }

        VectorCopy(cent->netLerpOrigin, a);
        VectorCopy(cent->lerpOrigin, b);
        a[2] += (rp->mins[2] + rp->maxs[2]) * 0.5f;
        b[2] += (rp->mins[2] + rp->maxs[2]) * 0.5f;
        cgi.R_DebugLine(a, b, 1.0f, 1.0f, 0.0f, 1.0f);
    }

    if (cg_remotePredictionDebug->integer >= 2) {
        if (cg.time - rp_lastDebugPrintTime >= 500) {
            // Pick first valid entity for detailed weights if any.
            float wS = 0.0f, wC = 0.0f, wD = 0.0f, om = 0.0f, st = 0.0f, stab = 0.0f;
            int   air = 0;
            for (i = 0; i < MAX_GENTITIES; i++) {
                if (rp_entities[i].valid
                    && cg.time - rp_entities[i].lastUpdateTime <= RP_BOUNDS_STALE_MS) {
                    wS   = rp_entities[i].dbgWStraight;
                    wC   = rp_entities[i].dbgWCurve;
                    wD   = rp_entities[i].dbgWDecel;
                    om   = rp_entities[i].estOmega;
                    st   = rp_entities[i].estSpeedTrend;
                    stab = rp_entities[i].estStability;
                    air  = rp_entities[i].dbgAirborne ? 1 : 0;
                    break;
                }
            }

            meanDisp = (rp_debugPredictedCount > 0) ? (rp_debugDispSum / (float)rp_debugPredictedCount)
                                                    : 0.0f;
            cgi.Printf(
                "rp: ping=%.0f lead=%d ents=%d meanDisp=%.1f schedC=%.2f schedD=%.2f "
                "wS=%.2f wC=%.2f wD=%.2f om=%.2f st=%.0f stab=%.2f air=%d\n",
                rp_pingSmoothed,
                rp_leadMsec,
                rp_debugPredictedCount,
                meanDisp,
                rp_schedWCurve,
                rp_schedWDecel,
                wS,
                wC,
                wD,
                om,
                st,
                stab,
                air
            );
            rp_lastDebugPrintTime = cg.time;
        }
    }
}

#else // !CG_RP_DEBUG_DRAW

void CG_RP_DebugDraw(void) {}

#endif // CG_RP_DEBUG_DRAW
