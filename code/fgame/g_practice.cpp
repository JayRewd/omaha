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

#include "g_practice.h"
#include "g_bot.h"
#include "g_utils.h"
#include "gamecvars.h"
#include "gamecmds.h"
#include "consoleevent.h"
#include "weaputils.h"
#include "dm_manager.h"
#include "weapon.h"
#include "ammo.h"
#include "item.h"
#include "huddraw.h"
#include "g_phys.h"
#include "bg_local.h"

#include "g_main.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <vector>

extern Event EV_Player_FreezeControls;

bool g_bPracticeCreateController = false;

PracticeManager practiceManager;


static const int   PRACTICE_DUMMY_HEALTH      = 30;
static const float PRACTICE_BOT_SHOT_DAMAGE  = 30.0f;
static const int   PRACTICE_MAX_BOTS          = 32;
static const int   PRACTICE_JOIN_RETRY_MS     = 500;
static const float PRACTICE_BOT_RESPAWN_DELAY = 0.5f;
static const int   PRACTICE_TIME_LIMIT_MS     = 30 * 60 * 1000; // Added in Omaha
static const float PRACTICE_PICK_RANGE        = 8192.0f;

// Added in Omaha: format elapsed ms as M:SS.t for server prints / HUD.
static void Practice_FormatTimeMs(int ms, char *out, size_t outSize)
{
	int totalSec;
	int tenths;
	int mins;
	int secs;

	if (ms < 0) {
		ms = 0;
	}
	totalSec = ms / 1000;
	tenths   = (ms % 1000) / 100;
	mins     = totalSec / 60;
	secs     = totalSec % 60;
	Com_sprintf(out, outSize, "%i:%02i.%i", mins, secs, tenths);
}
// Changed in Omaha: 48 was too tight — logs showed approaches at ~54u that never triggered.
static const float PRACTICE_GATE_RADIUS       = 96.0f;
// Brief LOS gaps (lean peek edges) must not restart the full reaction delay.
static const int   PRACTICE_SIGHT_GRACE_MS    = 400;
// Brief clear-shot flicker must not restart reaction; long gaps must.
static const int   PRACTICE_CLEAR_GRACE_MS    = 200;

// Added in Omaha: server-drawn presentation tuning (stock OPM clients see all of it).
static const float PRACTICE_STAND_R           = 28.0f;
static const float PRACTICE_STAND_DEDUP       = 8.0f;
// Shared world hex rings (engine PVS culls); 6 sides, same radius as the stand.
static const int   PRACTICE_STAND_RING_SEGS   = 6;
// Play-mode next-waypoint marker: green dynamic light only (no beam / pillar).
static const float PRACTICE_WAYPOINT_LIGHT_R      = 0.2f;
static const float PRACTICE_WAYPOINT_LIGHT_G      = 1.0f;
static const float PRACTICE_WAYPOINT_LIGHT_B      = 0.35f;
static const float PRACTICE_WAYPOINT_LIGHT_RADIUS = 180.0f;
static const float PRACTICE_AIM_SELECT_MIN    = 0.9945f; // ~6° cone around the crosshair
static const float PRACTICE_AIM_CUE_DIST      = 128.0f;
static const int   PRACTICE_NADE_TRAIL_MAX    = 96;
static const int   PRACTICE_NADE_LATENCY_MS   = 350;
static const int   PRACTICE_NADE_HOLD_MS      = 150;
static const int   PRACTICE_NADE_CAM_MAX_MS   = 20000;
static const int   PRACTICE_MAX_PREVIEWS      = 96;
// Keep entity slots free for players, projectiles and map scripts.
static const int   PRACTICE_ENTITY_RESERVE    = 192;
static const int   PRACTICE_HUD_REFRESH_MS    = 2500;
static const int   PRACTICE_HUD_REFRESH_BATCH = 8;
static const char *PRACTICE_HUD_FONT          = "verdana-14";
static const char *PRACTICE_PREVIEW_MODEL     = "models/player/german_wehrmacht_soldier.tik";

static int Practice_FreeEntitySlots(void)
{
    int used = 0;
    int i;

    for (i = 0; i < globals.num_entities; i++) {
        if (g_entities[i].inuse) {
            used++;
        }
    }
    return globals.max_entities - used;
}

static bool Practice_SpecEquals(const PracticeBeamSpec& a, const PracticeBeamSpec& b)
{
    return a.start == b.start && a.end == b.end && a.color == b.color && a.width == b.width && a.light == b.light;
}

static FuncBeam *Practice_SpawnBeam(int singleClient)
{
    FuncBeam *beam;

    if (Practice_FreeEntitySlots() <= PRACTICE_ENTITY_RESERVE) {
        return NULL;
    }

    beam = new FuncBeam;
    beam->edict->s.skinNum             = 0;
    beam->edict->s.surfaces[4]         = 1; // one straight segment
    beam->edict->s.bone_angles[0][0]   = 0; // min offset
    beam->edict->s.bone_angles[0][1]   = 0; // max offset (no jitter)
    beam->edict->s.alpha               = 1;
    if (singleClient >= 0) {
        beam->edict->r.svFlags |= SVF_SINGLECLIENT;
        beam->edict->r.singleClient = singleClient;
    }
    beam->hideModel();
    return beam;
}

static void Practice_UpdateBeam(FuncBeam *beam, const PracticeBeamSpec& spec)
{
    int r = (int)(spec.color[0] * 255.0f);
    int g = (int)(spec.color[1] * 255.0f);
    int b = (int)(spec.color[2] * 255.0f);
    int l = (int)(spec.light / CONSTANTLIGHT_RADIUS_SCALE);

    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    l = l < 0 ? 0 : (l > 255 ? 255 : l);

    beam->setOrigin(spec.start);
    VectorCopy(spec.end, beam->edict->s.origin2);
    beam->setScale(spec.width);
    beam->edict->s.constantLight = r | (g << 8) | (b << 16) | (l << 24);
    beam->showModel();
}

void PracticeBeamSet::Apply(const std::vector<PracticeBeamSpec>& specs)
{
    size_t i;

    while (m_beams.size() > specs.size()) {
        FuncBeam *beam = m_beams.back();
        if (beam) {
            beam->PostEvent(EV_Remove, 0);
        }
        m_beams.pop_back();
        m_last.pop_back();
    }

    for (i = 0; i < specs.size(); i++) {
        FuncBeam *beam;
        bool      fresh = false;

        if (i >= m_beams.size()) {
            beam = Practice_SpawnBeam(m_iSingleClient);
            if (!beam) {
                break;
            }
            m_beams.push_back(SafePtr<FuncBeam>(beam));
            m_last.push_back(PracticeBeamSpec());
            fresh = true;
        }

        beam = m_beams[i];
        if (!beam) {
            // Removed behind our back (level cleanup) — respawn into the same slot.
            beam = Practice_SpawnBeam(m_iSingleClient);
            if (!beam) {
                break;
            }
            m_beams[i] = beam;
            fresh      = true;
        }

        if (fresh || !Practice_SpecEquals(m_last[i], specs[i])) {
            Practice_UpdateBeam(beam, specs[i]);
            m_last[i] = specs[i];
        }
    }
}

void PracticeBeamSet::Clear(void)
{
    std::vector<PracticeBeamSpec> none;
    Apply(none);
}

void PracticeBeamSet::Forget(void)
{
    m_beams.clear();
    m_last.clear();
}

void PracticeClient::Reset(void)
{
    int i;

    player           = NULL;
    editButtons      = 0;
    lastNadeEnt      = -1;
    lastNadeCharge   = 1.0f;
    lastNadeOrigin   = vec_zero;
    lastThrowStand   = vec_zero;
    lastThrowAim     = vec_zero;
    lastThrowEye     = vec_zero;
    lastThrowFireAim = vec_zero;
    lastNadeStart    = vec_zero;
    lastThrowTime    = 0;
    lastThrowStance  = PRACTICE_STANCE_STAND;
    lastThrowLean    = PRACTICE_LEAN_NONE;
    nextReplayTime   = 0;
    lastNadeExploded = false;
    lastNadeModel[0] = 0;
    deathAtPlace     = false;
    deathSpawnOrigin = vec_zero;
    deathSpawnAngles = vec_zero;

    camera           = NULL;
    camAttached      = false;
    camStartTime     = 0;
    camExplodeTime   = 0;
    camRest          = vec_zero;
    camRestoreAngles = vec_zero;
    nadeTrail.clear();

    beams.Forget();
    activeThrow = -1;
    lockedThrow = -1;
    releaseSent = -2.0f;

    for (i = 0; i < PRACTICE_HUD_COUNT; i++) {
        hud[i] = PracticeHudElem();
    }
    hudRefreshIndex = 0;
    nextHudRefresh  = 0;
    feedback[0]     = 0;
    feedbackExpire  = 0;
    nextSoloNotice  = 0;
}

static void Practice_AddBeam(
    std::vector<PracticeBeamSpec>& out,
    const Vector&                  start,
    const Vector&                  end,
    const Vector&                  color,
    float                          width,
    float                          light = 0.0f
)
{
    PracticeBeamSpec spec;

    spec.start = start;
    spec.end   = end;
    spec.color = color;
    spec.width = width;
    spec.light = light;
    out.push_back(spec);
}

// Flat ring polyline on the floor.
static void Practice_AddRing(
    std::vector<PracticeBeamSpec>& out, const Vector& center, float radius, int segs, const Vector& color, float width
)
{
    int i;

    for (i = 0; i < segs; i++) {
        const float a0 = (float)i / (float)segs * (float)M_PI * 2.0f;
        const float a1 = (float)(i + 1) / (float)segs * (float)M_PI * 2.0f;
        Vector      p0(center[0] + cos(a0) * radius, center[1] + sin(a0) * radius, center[2] + 1.5f);
        Vector      p1(center[0] + cos(a1) * radius, center[1] + sin(a1) * radius, center[2] + 1.5f);

        Practice_AddBeam(out, p0, p1, color, width);
    }
}

static void Practice_AddPillar(
    std::vector<PracticeBeamSpec>& out,
    const Vector&                  base,
    float                          height,
    const Vector&                  color,
    float                          width,
    float                          light = 0.0f
)
{
    Practice_AddBeam(out, base, base + Vector(0, 0, height), color, width, light);
}

// Aim cue facing the throw eye: open-center X (precision pin); active is larger/hotter.
static void Practice_AddCue(std::vector<PracticeBeamSpec>& out, const PracticeCue& cue, bool active)
{
    Vector       forward = cue.forward;
    Vector       right;
    Vector       up;
    Vector       d1;
    Vector       d2;
    const float  inner = 1.5f;
    const float  outer = active ? 6.0f : 5.0f;
    const float  width = active ? 0.6f : 0.5f;
    const Vector col   = active ? Vector(1.0f, 0.9f, 0.15f) : Vector(0.9f, 0.9f, 0.95f);

    forward.normalize();
    forward.toAngles().AngleVectors(NULL, &right, &up);
    d1 = right + up;
    d2 = right - up;
    d1.normalize();
    d2.normalize();

    Practice_AddBeam(out, cue.pos + d1 * inner, cue.pos + d1 * outer, col, width);
    Practice_AddBeam(out, cue.pos - d1 * inner, cue.pos - d1 * outer, col, width);
    Practice_AddBeam(out, cue.pos + d2 * inner, cue.pos + d2 * outer, col, width);
    Practice_AddBeam(out, cue.pos - d2 * inner, cue.pos - d2 * outer, col, width);
}

// Added in Omaha: "Stand, Throw" / "Crouch, Lean left, Throw".
static void Practice_FormatThrowPose(const PracticeThrow& th, char *out, size_t outSize)
{
    const char *stance;

    if (th.stance == PRACTICE_STANCE_CROUCH) {
        stance = "Crouch";
    } else if (th.stance == PRACTICE_STANCE_JUMP) {
        stance = "Jump";
    } else {
        stance = "Stand";
    }
    if (th.lean == PRACTICE_LEAN_LEFT) {
        Com_sprintf(out, outSize, "%s, Lean left, Throw", stance);
    } else if (th.lean == PRACTICE_LEAN_RIGHT) {
        Com_sprintf(out, outSize, "%s, Lean right, Throw", stance);
    } else {
        Com_sprintf(out, outSize, "%s, Throw", stance);
    }
}

// Added in Omaha: angles spot pose for aim HUD (no "Throw").
static void Practice_FormatSpotPose(int stance, int lean, char *out, size_t outSize)
{
    const char *stanceName;

    if (stance == PRACTICE_STANCE_CROUCH) {
        stanceName = "Crouch";
    } else if (stance == PRACTICE_STANCE_JUMP) {
        stanceName = "Jump";
    } else {
        stanceName = "Stand";
    }
    if (lean == PRACTICE_LEAN_LEFT) {
        Com_sprintf(out, outSize, "%s, Lean left", stanceName);
    } else if (lean == PRACTICE_LEAN_RIGHT) {
        Com_sprintf(out, outSize, "%s, Lean right", stanceName);
    } else {
        Com_sprintf(out, outSize, "%s", stanceName);
    }
}

static bool Practice_IsHumanEntity(gentity_t *ent)
{
    return ent && ent->inuse && ent->client && ent->entity && ent->entity->IsSubclassOfPlayer() && !G_IsBot(ent);
}

Event EV_Practice_SpotAdd
(
    "practice_spot_add",
    EV_DEFAULT,
    NULL,
    NULL,
    "Add a clearing-angles bot spot at the player's pose.",
    EV_NORMAL
);
Event EV_Practice_SpotUndo
(
    "practice_spot_undo",
    EV_DEFAULT,
    NULL,
    NULL,
    "Remove the last placed clearing-angles bot spot.",
    EV_NORMAL
);
Event EV_Practice_StartSet
(
    "practice_start_set",
    EV_DEFAULT,
    NULL,
    NULL,
    "Set the player start for this practice scenario.",
    EV_NORMAL
);
Event EV_Practice_WaypointAdd
(
    "practice_waypoint_add",
    EV_DEFAULT,
    NULL,
    NULL,
    "Add a route waypoint at the player's origin.",
    EV_NORMAL
);
Event EV_Practice_ThrowSave
(
    "practice_throw_save",
    EV_DEFAULT,
    NULL,
    NULL,
    "Save the last grenade throw as a practice throw.",
    EV_NORMAL
);
Event EV_Practice_Weapon
(
    "practice_weapon",
    EV_DEFAULT,
    "s",
    "weapon",
    "Give the player a weapon without respawning.",
    EV_NORMAL
);
Event EV_Practice_Save
(
    "practice_save",
    EV_DEFAULT,
    NULL,
    NULL,
    "Write the current practice scenario to disk.",
    EV_NORMAL
);
Event EV_Practice_Meta
(
    "practice_meta",
    EV_DEFAULT,
    "sSI",
    "name author maxalive",
    "Set the scenario name, author and max alive bots (editor).",
    EV_NORMAL
);
Event EV_Practice_OrderBegin
(
    "practice_order_begin",
    EV_DEFAULT,
    "i",
    "count",
    "Begin a chunked node reorder (editor).",
    EV_NORMAL
);
Event EV_Practice_OrderAppend
(
    "practice_order_append",
    EV_DEFAULT,
    NULL,
    NULL,
    "Append node ids to a chunked reorder (editor).",
    EV_NORMAL
);
Event EV_Practice_OrderCommit
(
    "practice_order_commit",
    EV_DEFAULT,
    NULL,
    NULL,
    "Commit a chunked node reorder (editor).",
    EV_NORMAL
);

CLASS_DECLARATION(BotController, PracticeBotController, NULL) {
    {NULL, NULL}
};

CLASS_DECLARATION(Listener, PracticeManager, NULL) {
    {&EV_Practice_SpotAdd,       &PracticeManager::EventSpotAdd      },
    {&EV_Practice_SpotUndo,      &PracticeManager::EventSpotUndo     },
    {&EV_Practice_StartSet,      &PracticeManager::EventStartSet     },
    {&EV_Practice_WaypointAdd,   &PracticeManager::EventWaypointAdd  },
    {&EV_Practice_ThrowSave,     &PracticeManager::EventThrowSave    },
    {&EV_Practice_Weapon,        &PracticeManager::EventWeapon       },
    {&EV_Practice_Save,          &PracticeManager::EventSave         },
    {&EV_Practice_Meta,          &PracticeManager::EventMeta         },
    {&EV_Practice_OrderBegin,    &PracticeManager::EventOrderBegin   },
    {&EV_Practice_OrderAppend,   &PracticeManager::EventOrderAppend  },
    {&EV_Practice_OrderCommit,   &PracticeManager::EventOrderCommit  },
    {NULL,                       NULL                                }
};

static PracticeBotController *Practice_ControllerFor(Entity *ent)
{
    if (!ent) {
        return NULL;
    }
    return dynamic_cast<PracticeBotController *>(botManager.getControllerManager().findController(ent));
}

static void Practice_CollectControllers(std::vector<PracticeBotController *>& out)
{
    int i;

    out.clear();
    const Container<BotController *>& list = botManager.getControllerManager().getControllers();
    for (i = 1; i <= list.NumObjects(); i++) {
        PracticeBotController *controller = dynamic_cast<PracticeBotController *>(list.ObjectAt(i));
        if (controller) {
            out.push_back(controller);
        }
    }
}

static float Practice_Clamp01(float v)
{
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

// G_ClientDisconnect nulls edict->entity even when deferring EV_Remove during thinks,
// which leaves an active edict that crashes G_RunEntity. Remove safely instead.
static void Practice_RemoveBotSafe(gentity_t *ent)
{
	BotController *controller;

	if (!ent || !ent->entity) {
		return;
	}

	controller = botManager.getControllerManager().findController(ent->entity);
	if (controller) {
		botManager.getControllerManager().removeController(controller);
	}

	if (g_iInThinks) {
		if (ent->entity->IsSubclassOfPlayer()) {
			static_cast<Player *>(ent->entity)->Disconnect();
		}
		ent->entity->PostEvent(EV_Remove, 0);
		return;
	}

	G_RemoveBot(ent);
}

// Closest distance between segments [p1,q1] and [p2,q2]; outS is the fraction along the first.
static float Practice_SegmentDistance(const Vector& p1, const Vector& q1, const Vector& p2, const Vector& q2, float *outS)
{
    const float eps = 0.0001f;
    Vector      d1  = q1 - p1;
    Vector      d2  = q2 - p2;
    Vector      r   = p1 - p2;
    float       a   = DotProduct(d1, d1);
    float       e   = DotProduct(d2, d2);
    float       f   = DotProduct(d2, r);
    float       s;
    float       t;

    if (a <= eps && e <= eps) {
        s = 0.0f;
        t = 0.0f;
    } else if (a <= eps) {
        s = 0.0f;
        t = Practice_Clamp01(f / e);
    } else {
        float c = DotProduct(d1, r);
        if (e <= eps) {
            t = 0.0f;
            s = Practice_Clamp01(-c / a);
        } else {
            float b     = DotProduct(d1, d2);
            float denom = a * e - b * b;

            s = denom != 0.0f ? Practice_Clamp01((b * f - c * e) / denom) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = Practice_Clamp01(-c / a);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = Practice_Clamp01((b - c) / a);
            }
        }
    }

    if (outS) {
        *outS = s;
    }
    return ((p1 + d1 * s) - (p2 + d2 * t)).length();
}

// Capsule used by angles-edit aim collect (same geometry as former Practice_Pick).
struct PracticePickTarget {
    Vector bottom;
    Vector top;
    float  radius;
};

PracticeBotController::PracticeBotController()
    : m_iStance(PRACTICE_STANCE_STAND)
    , m_iLean(PRACTICE_LEAN_NONE)
    , m_iSightTime(0)
    , m_iLastSeeTime(0)
    , m_iClearTime(0)
    , m_iLastClearTime(0)
    , m_iNextFireTime(0)
    , m_iNextJoinTime(0)
    , m_bDummy(false)
    , m_bAllowFire(false)
    , m_bHasHome(false)
{
    m_vHomeOrigin = vec_zero;
    m_vHomeAngles = vec_zero;
}

void PracticeBotController::SetHome(const Vector& origin, const Vector& angles, int stance, int lean)
{
    m_vHomeOrigin     = origin;
    m_vHomeAngles     = angles;
    m_iStance         = stance;
    m_iLean           = lean;
    m_bHasHome        = true;
    m_iSightTime      = 0;
    m_iLastSeeTime    = 0;
    m_iClearTime      = 0;
    m_iLastClearTime  = 0;
    rotation.SetTargetAngles(m_vHomeAngles);
}

void PracticeBotController::ClearHome(void)
{
    m_bHasHome       = false;
    m_vHomeOrigin    = vec_zero;
    m_vHomeAngles    = vec_zero;
    m_iSightTime     = 0;
    m_iLastSeeTime   = 0;
    m_iClearTime     = 0;
    m_iLastClearTime = 0;
}

void PracticeBotController::SetDummy(bool dummy)
{
    m_bDummy = dummy;
}

void PracticeBotController::SetAllowFire(bool allowFire)
{
    m_bAllowFire = allowFire;
}

void PracticeBotController::Spawned(void)
{
    m_iSightTime     = 0;
    m_iLastSeeTime   = 0;
    m_iClearTime     = 0;
    m_iLastClearTime = 0;
    m_iNextFireTime  = 0;
    m_botCmd.buttons = 0;
    if (controlledEnt && controlledEnt->client) {
        Q_strncpyz(controlledEnt->client->pers.dm_primary, "rifle", sizeof(controlledEnt->client->pers.dm_primary));
    }
}

void PracticeBotController::Killed(const Event& ev)
{
    m_botCmd.buttons = 0;
    m_iSightTime     = 0;
    m_iClearTime     = 0;
    m_iLastClearTime = 0;
    (void)ev;
}

// World geometry that blocks bullets.
static const int PRACTICE_CLEAR_MASK =
    (CONTENTS_SOLID | CONTENTS_FENCE | CONTENTS_WEAPONCLIP | CONTENTS_BBOX | CONTENTS_UNKNOWN2 | CONTENTS_NOBOTCLIP);

// Added in Omaha: push a start point out of solid (lean-side first, then facing).
static Vector Practice_UnstickEye(const Vector& eye, const Vector& angles, Entity *bot, const Vector *leanRight)
{
    Vector  dir;
    Vector  start = eye;
    Vector  end;
    trace_t tr;
    int     pass;
    int     i;

    for (pass = 0; pass < 2; pass++) {
        if (pass == 0 && leanRight) {
            dir = *leanRight;
        } else {
            angles.AngleVectors(&dir);
        }
        start = eye;
        for (i = 0; i < 8; i++) {
            end = start + dir * 8.0f;
            tr  = G_Trace(start, vec_zero, vec_zero, end, bot, PRACTICE_CLEAR_MASK, qfalse, "PracticeBot::Unstick");
            if (!tr.startsolid && !tr.allsolid) {
                return start;
            }
            start = start + dir * 8.0f;
        }
    }
    return eye;
}

// Same solid blockers as sight, plus BODY so we can confirm a player hit.
// Fixed in Omaha: full MASK_SHOT includes TRIGGER/BBOX/UNKNOWN2 that false-block door gaps.
static const int PRACTICE_HIT_MASK = (MASK_CANSEE_NOENTS | CONTENTS_BODY);

// Added in Omaha: true only if a ray along angles hits the human entity (not empty space).
static bool Practice_ShotHitsHuman(
    const Vector& start, const Vector& angles, Player *bot, Player *human, int *outHitEnt
)
{
    Vector  forward;
    Vector  end;
    trace_t tr;
    float   dist;

    if (outHitEnt) {
        *outHitEnt = -1;
    }
    if (!bot || !human) {
        return false;
    }

    angles.AngleVectors(&forward);
    dist = (human->centroid - start).length() + 96.0f;
    if (dist < 128.0f) {
        dist = 128.0f;
    }
    end = start + forward * dist;
    tr  = G_Trace(start, vec_zero, vec_zero, end, bot, PRACTICE_HIT_MASK, qfalse, "PracticeBot::ClearShot");
    if (outHitEnt) {
        *outHitEnt = tr.entityNum;
    }
    return !tr.startsolid && !tr.allsolid && tr.entityNum == human->entnum;
}

// Added in Omaha: collect hittable aim points — bones first, then AABB samples.
static int Practice_CollectAimTargets(Player *human, Vector *out, int maxOut)
{
    static const char *const kTags[] = {"eyes bone", "tag_mouth", "tag_chest", "tag_torso", "tag_head"};
    orientation_t            tagOr;
    Vector                   humanView;
    float                    zTop;
    float                    zMid;
    float                    zLow;
    float                    xs[3];
    float                    ys[3];
    float                    zs[3];
    int                      n = 0;
    int                      i;
    int                      ix, iy, iz;

    if (!human || !out || maxOut <= 0) {
        return 0;
    }

    for (i = 0; i < (int)(sizeof(kTags) / sizeof(kTags[0])) && n < maxOut; i++) {
        if (human->GetTag(kTags[i], &tagOr)) {
            out[n++] = tagOr.origin;
        }
    }

    humanView = human->m_vViewPos;
    if ((humanView - human->origin).lengthSquared() < 1.0f) {
        humanView = human->EyePosition();
    }
    if (n < maxOut) {
        out[n++] = humanView;
    }
    if (n < maxOut) {
        out[n++] = human->EyePosition();
    }
    if (n < maxOut) {
        out[n++] = human->centroid;
    }

    zTop = human->maxs[2] > 8.0f ? human->maxs[2] - 2.0f : 56.0f;
    zMid = human->maxs[2] > 8.0f ? human->maxs[2] * 0.55f : 36.0f;
    zLow = human->maxs[2] > 8.0f ? human->maxs[2] * 0.25f : 18.0f;
    xs[0] = human->mins[0] * 0.9f;
    xs[1] = 0.0f;
    xs[2] = human->maxs[0] * 0.9f;
    ys[0] = human->mins[1] * 0.9f;
    ys[1] = 0.0f;
    ys[2] = human->maxs[1] * 0.9f;
    zs[0] = zTop;
    zs[1] = zMid;
    zs[2] = zLow;

    // Dense AABB lattice — shallow peeks only expose an edge/corner of the box.
    for (iz = 0; iz < 3 && n < maxOut; iz++) {
        for (iy = 0; iy < 3 && n < maxOut; iy++) {
            for (ix = 0; ix < 3 && n < maxOut; ix++) {
                out[n++] = human->origin + Vector(xs[ix], ys[iy], zs[iz]);
            }
        }
    }
    return n;
}

// Added in Omaha: combat aim points only — bones / view / centroid (no AABB lattice).
static int Practice_CollectCombatTargets(Player *human, Vector *out, int maxOut)
{
    static const char *const kTags[] = {"eyes bone", "tag_mouth", "tag_chest", "tag_torso", "tag_head"};
    orientation_t            tagOr;
    Vector                   humanView;
    int                      n = 0;
    int                      i;

    if (!human || !out || maxOut <= 0) {
        return 0;
    }
    for (i = 0; i < (int)(sizeof(kTags) / sizeof(kTags[0])) && n < maxOut; i++) {
        if (human->GetTag(kTags[i], &tagOr)) {
            out[n++] = tagOr.origin;
        }
    }
    humanView = human->m_vViewPos;
    if ((humanView - human->origin).lengthSquared() < 1.0f) {
        humanView = human->EyePosition();
    }
    if (n < maxOut) {
        out[n++] = humanView;
    }
    if (n < maxOut) {
        out[n++] = human->EyePosition();
    }
    if (n < maxOut) {
        out[n++] = human->centroid;
    }
    return n;
}

// Added in Omaha: aim only at combat targets with SightTrace + a ray that hits the player.
static bool Practice_FindClearAim(
    const Vector& fireStart,
    const Vector& geometric,
    int           lean,
    Player       *bot,
    Player       *human,
    Vector&       outAngles,
    int          *outHitEnt,
    float        *outMoved
)
{
    Vector targets[16];
    Vector start;
    Vector delta;
    Vector tryAng;
    int    n;
    int    ti;
    int    hitEnt = -1;
    float  moved  = 0.0f;

    if (outHitEnt) {
        *outHitEnt = -1;
    }
    if (outMoved) {
        *outMoved = 0.0f;
    }
    if (!bot || !human) {
        return false;
    }

    (void)lean;
    n         = Practice_CollectCombatTargets(human, targets, (int)(sizeof(targets) / sizeof(targets[0])));
    start     = Practice_UnstickEye(fireStart, geometric, bot, NULL);
    moved     = (start - fireStart).length();
    outAngles = geometric;
    if (outMoved) {
        *outMoved = moved;
    }

    for (ti = 0; ti < n; ti++) {
        // Fixed in Omaha: must actually see the aim point — bullet-only AABB clips were wallhacks.
        if (!G_SightTrace(
                start,
                vec_zero,
                vec_zero,
                targets[ti],
                bot,
                human,
                MASK_CANSEE_NOENTS,
                qfalse,
                "PracticeBot::ClearSight"
            )) {
            continue;
        }
        delta = targets[ti] - start;
        if (delta.lengthSquared() < 1.0f) {
            continue;
        }
        VectorNormalize(delta);
        vectoangles(delta, tryAng);
        if (Practice_ShotHitsHuman(start, tryAng, bot, human, &hitEnt)) {
            outAngles = tryAng;
            if (outHitEnt) {
                *outHitEnt = hitEnt;
            }
            return true;
        }
    }

    if (outHitEnt) {
        *outHitEnt = hitEnt;
    }
    return false;
}

bool PracticeBotController::VisibleAimPoint(Player *human, Vector& out, int *outSeeSrc) const
{
    Vector eye;
    Vector eyeNudge;
    Vector forward, right, up;
    Vector targets[48];
    Vector botPoints[5];
    Vector humanView;
    float  zTop;
    int    i;
    int    n;
    int    bi;

    if (outSeeSrc) {
        *outSeeSrc = 0;
    }
    if (!controlledEnt || !human) {
        return false;
    }
    if ((human->centroid - controlledEnt->centroid).lengthXY() > 8192.0f) {
        return false;
    }

    eye = LeanedEyePosition();
    // Corner peeks often bury the lateral-lean eye; a short forward probe finds the gap.
    // (Do not bake this into LeanedEyePosition — that broke all canSee in post-fix8.)
    m_vHomeAngles.AngleVectors(&forward, &right, &up);
    eyeNudge = eye + forward * 16.0f;

    zTop = human->maxs[2] > 8.0f ? human->maxs[2] - 4.0f : 56.0f;

    humanView = human->m_vViewPos;
    if ((humanView - human->origin).lengthSquared() < 1.0f) {
        humanView = human->EyePosition();
    }

    // Bones + dense AABB for awareness only (combat clear-shot uses combat targets).
    n = Practice_CollectAimTargets(human, targets, (int)(sizeof(targets) / sizeof(targets[0])));

    for (bi = 0; bi < 2; bi++) {
        const Vector& from = (bi == 0) ? eye : eyeNudge;
        for (i = 0; i < n; i++) {
            if (G_SightTrace(
                    from,
                    vec_zero,
                    vec_zero,
                    targets[i],
                    controlledEnt,
                    human,
                    MASK_CANSEE_NOENTS,
                    qfalse,
                    "PracticeBot::Aim"
                )) {
                out = targets[i];
                if (outSeeSrc) {
                    *outSeeSrc = 1;
                }
                return true;
            }
        }
    }

    // Reverse LOS: player can see bot — awareness only, must not arm clearShot alone.
    botPoints[0] = eye;
    botPoints[1] = eyeNudge;
    botPoints[2] = controlledEnt->centroid;
    botPoints[3] = controlledEnt->origin + Vector(0, 0, controlledEnt->viewheight);
    botPoints[4] = controlledEnt->origin + Vector(0, 0, zTop > 40.0f ? 56.0f : zTop);
    for (i = 0; i < 5; i++) {
        if (G_SightTrace(
                humanView,
                vec_zero,
                vec_zero,
                botPoints[i],
                human,
                controlledEnt,
                MASK_CANSEE_NOENTS,
                qfalse,
                "PracticeBot::AimReverse"
            )) {
            out = humanView;
            if (outSeeSrc) {
                *outSeeSrc = 2;
            }
            return true;
        }
    }
    return false;
}

// Added in Omaha: lean eye matches cgame first-person pivot (cg_view.c ~28.7).
// Spectate 0.65*angle / +12 extras were wrong — centerline and fat lean miss opposite ways.
Vector PracticeBotController::LeanedEyePosition(void) const
{
    Vector eye;
    Vector forward, right, up;
    Vector vStart;
    Vector vDelta;
    vec3_t vEnd;
    float  leanAngle;

    eye = controlledEnt->origin;
    eye[2] += controlledEnt->viewheight;

    if (m_iLean == PRACTICE_LEAN_NONE) {
        return eye;
    }

    leanAngle = 0.0f;
    if (controlledEnt->client) {
        leanAngle = controlledEnt->client->ps.fLeanAngle;
    }
    // Sight uses full lean immediately so corner spots work while fLeanAngle ramps up.
    if (m_iLean == PRACTICE_LEAN_LEFT && leanAngle > -40.0f) {
        leanAngle = -40.0f;
    } else if (m_iLean == PRACTICE_LEAN_RIGHT && leanAngle < 40.0f) {
        leanAngle = 40.0f;
    }

    m_vHomeAngles.AngleVectors(&forward, &right, &up);
    (void)right;
    (void)up;
    vStart = eye;
    vStart[2] -= 28.7f;
    vDelta = eye - vStart;
    RotatePointAroundVector(vEnd, forward, vDelta, leanAngle);
    eye = vStart + Vector(vEnd);
    return eye;
}

void PracticeBotController::ApplyEyeOffset(void)
{
    Vector eye = LeanedEyePosition();
    Vector ofs = eye - controlledEnt->origin;

    m_botEyes.ofs[0]    = (signed char)Q_clamp_int((int)ofs[0], -127, 127);
    m_botEyes.ofs[1]    = (signed char)Q_clamp_int((int)ofs[1], -127, 127);
    m_botEyes.ofs[2]    = (signed char)Q_clamp_int((int)ofs[2], -127, 127);
    m_botEyes.angles[0] = 0;
    m_botEyes.angles[1] = 0;
}

// Added in Omaha: same lean-eye + forward nudge as VisibleAimPoint, aimed at a world point
// (used to test whether a checkpoint is contested before death-respawn).
bool PracticeBotController::SeesWorldPoint(const Vector& point) const
{
    Vector eye;
    Vector eyeNudge;
    Vector forward, right, up;
    int    bi;

    if (!controlledEnt || !m_bHasHome) {
        return false;
    }
    if ((point - controlledEnt->centroid).lengthXY() > 8192.0f) {
        return false;
    }

    eye = LeanedEyePosition();
    m_vHomeAngles.AngleVectors(&forward, &right, &up);
    (void)right;
    (void)up;
    eyeNudge = eye + forward * 16.0f;

    for (bi = 0; bi < 2; bi++) {
        const Vector& from = (bi == 0) ? eye : eyeNudge;
        if (G_SightTrace(
                from,
                vec_zero,
                vec_zero,
                point,
                controlledEnt,
                NULL,
                MASK_CANSEE_NOENTS,
                qfalse,
                "PracticeBot::SeesWorldPoint"
            )) {
            return true;
        }
    }
    return false;
}

void PracticeBotController::UpdateBotStates(void)
{
    Player *human;
    Vector  aimPoint;
    bool    canSee;

    m_botCmd.serverTime  = level.svsTime;
    m_botCmd.forwardmove = 0;
    m_botCmd.rightmove   = 0;
    m_botCmd.upmove      = 0;
    m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT | BUTTON_RUN | BUTTON_USE | BUTTON_LEAN_LEFT
                          | BUTTON_LEAN_RIGHT);

    if (!controlledEnt || !controlledEnt->client) {
        return;
    }

    // Without a primary the join flow waits on a weapon-select menu a bot can't answer.
    if (!controlledEnt->client->pers.dm_primary[0]) {
        Q_strncpyz(controlledEnt->client->pers.dm_primary, "rifle", sizeof(controlledEnt->client->pers.dm_primary));
    }

    if (controlledEnt->GetTeam() == TEAM_NONE || controlledEnt->GetTeam() == TEAM_SPECTATOR) {
        float time = controlledEnt->entnum / 20.0f;
        // Force Allies so stock EquipWeapons never hands out a Kar98/Mauser.
        if (!controlledEnt->EventPending(EV_Player_JoinDMTeam)
            && !controlledEnt->EventPending(EV_Player_AutoJoinDMTeam)) {
            Event *join = new Event(EV_Player_JoinDMTeam);
            join->AddString("allies");
            controlledEnt->PostEvent(join, time);
        }
        return;
    }

    if (controlledEnt->IsSpectator()) {
        if (level.inttime >= m_iNextJoinTime) {
            m_iNextJoinTime = level.inttime + PRACTICE_JOIN_RETRY_MS;
            Event *ev       = new Event(EV_Player_PrimaryDMWeapon);
            ev->AddString(controlledEnt->client->pers.dm_primary);
            controlledEnt->ProcessEvent(ev);
        }
        return;
    }

    if (controlledEnt->IsDead()) {
        return;
    }

    // Parked (no home yet): stay inert until PlaceBots assigns a spot.
    if (!m_bHasHome) {
        return;
    }

    if (m_bHasHome && (controlledEnt->origin - m_vHomeOrigin).length() > 8.0f) {
        controlledEnt->setOrigin(m_vHomeOrigin);
        controlledEnt->velocity = vec_zero;
    }

    if (m_iStance == PRACTICE_STANCE_CROUCH) {
        m_botCmd.upmove = -127;
    }
    if (m_iLean == PRACTICE_LEAN_LEFT) {
        m_botCmd.buttons |= BUTTON_LEAN_LEFT;
    } else if (m_iLean == PRACTICE_LEAN_RIGHT) {
        m_botCmd.buttons |= BUTTON_LEAN_RIGHT;
    }

    ApplyEyeOffset();

    human = practiceManager.GetHuman();
    if (m_bDummy || !m_bAllowFire || !human || human->IsDead() || human->IsSpectator()) {
        m_iSightTime = 0;
        rotation.SetTargetAngles(m_vHomeAngles);
        rotation.TurnThink(m_botCmd, m_botEyes);
        return;
    }

    int seeSrc = 0;
    canSee     = VisibleAimPoint(human, aimPoint, &seeSrc);
    int clearShot = 0;
    if (canSee) {
        static Vector s_lastClearAng[MAX_CLIENTS];
        static int    s_haveLastClear[MAX_CLIENTS];
        Vector        fireStart;
        Vector        aimDelta;
        Vector        aimAngles;
        Vector        clearAngles;
        orientation_t eyesOr;
        const int     entIdx = controlledEnt->entnum;

        m_iLastSeeTime = level.inttime;
        if (!m_iSightTime) {
            m_iSightTime = level.inttime;
        }

        // Prefer eyes bone — same target a human peeker exposes first.
        if (human->GetTag("eyes bone", &eyesOr)) {
            aimPoint = eyesOr.origin;
        } else if ((human->m_vViewPos - human->origin).lengthSquared() > 1.0f) {
            aimPoint = human->m_vViewPos;
        }

        // Fire from real FP lean eye (diag-lean-pivot proved this hits the authored line).
        fireStart = LeanedEyePosition();
        aimDelta  = aimPoint - fireStart;
        if (aimDelta.lengthSquared() > 1.0f) {
            VectorNormalize(aimDelta);
            vectoangles(aimDelta, aimAngles);
        } else {
            aimAngles = m_vHomeAngles;
        }

        // Fixed in Omaha: reverse-only awareness must not aim/fire (player sees bot ≠ bot sees player).
        if (seeSrc == 2) {
            clearShot = 0;
            rotation.SetTargetAngles(m_vHomeAngles);
            if (entIdx >= 0 && entIdx < MAX_CLIENTS) {
                s_haveLastClear[entIdx] = 0;
            }
        } else if (Practice_FindClearAim(
                       fireStart, aimAngles, m_iLean, controlledEnt, human, clearAngles, NULL, NULL
                   )) {
            clearShot = 1;
            rotation.SetTargetAngles(clearAngles);
            if (entIdx >= 0 && entIdx < MAX_CLIENTS) {
                s_lastClearAng[entIdx]  = clearAngles;
                s_haveLastClear[entIdx] = 1;
            }
        } else if (
            entIdx >= 0 && entIdx < MAX_CLIENTS && s_haveLastClear[entIdx]
            && G_SightTrace(
                   fireStart,
                   vec_zero,
                   vec_zero,
                   aimPoint,
                   controlledEnt,
                   human,
                   MASK_CANSEE_NOENTS,
                   qfalse,
                   "PracticeBot::StickySight"
               )
            && Practice_ShotHitsHuman(fireStart, s_lastClearAng[entIdx], controlledEnt, human, NULL)
        ) {
            clearShot   = 1;
            clearAngles = s_lastClearAng[entIdx];
            rotation.SetTargetAngles(clearAngles);
        } else {
            // Fixed in Omaha: no clear shot → hold authored facing (do not track through walls).
            clearShot = 0;
            rotation.SetTargetAngles(m_vHomeAngles);
        }

        // Fixed in Omaha: reaction starts when a clear shot appears, not on early canSee
        // (reverse/edge LOS was burning the delay before the player was shootable).
        if (clearShot == 1) {
            m_iLastClearTime = level.inttime;
            if (!m_iClearTime) {
                m_iClearTime = level.inttime;
            }
        } else if (m_iClearTime && level.inttime - m_iLastClearTime > PRACTICE_CLEAR_GRACE_MS) {
            m_iClearTime = 0;
        }

        if (clearShot == 1 && m_iClearTime
            && level.inttime >= m_iClearTime + practiceManager.ReactionMs()) {
            const Vector  viewNow = controlledEnt->GetViewAngles();
            const Vector& tgtNow  = rotation.GetTargetAngles();
            const float   aimErr  = fabs(AngleSubtract(viewNow[YAW], tgtNow[YAW]));

            if (aimErr <= 2.5f) {
                if (!controlledEnt->GetActiveWeapon(WEAPON_MAIN)) {
                    Event ev(EV_Player_PrimaryDMWeapon);
                    ev.AddString("rifle");
                    controlledEnt->ProcessEvent(ev);
                }
                if (level.inttime >= m_iNextFireTime) {
                    m_botCmd.buttons |= BUTTON_ATTACKLEFT;
                    m_iNextFireTime = level.inttime + practiceManager.FireIntervalMs();
                }
            }
        }
    } else {
        if (m_iSightTime && level.inttime - m_iLastSeeTime > PRACTICE_SIGHT_GRACE_MS) {
            m_iSightTime = 0;
        }
        if (m_iClearTime && level.inttime - m_iLastClearTime > PRACTICE_CLEAR_GRACE_MS) {
            m_iClearTime = 0;
        }
        rotation.SetTargetAngles(m_vHomeAngles);
    }

    rotation.TurnThink(m_botCmd, m_botEyes);
}

PracticeManager::PracticeManager()
    : m_bLoaded(false)
    , m_bLoadOk(false)
    , m_bMapMismatch(false)
    , m_bComplete(false)
    , m_bRunActive(false)
    , m_bBotsPlaced(false)
    , m_bDeathRewind(false)
    , m_bNadeReplay(false)
    , m_bCueCacheDirty(true)
    , m_iNextSpot(0)
    , m_iCurrentGate(-1)
    , m_iDeathSpawnGate(0)
    , m_iRunStartTime(0)
    , m_iCompleteTime(0)
    , m_iAnglesOwner(-1)
    , m_iNextGlobalMarkers(0)
    , m_iNextPreviews(0)
    , m_iOrderExpected(0)
{
}

bool PracticeManager::IsActive(void) const
{
    if (!g_practice || g_gametype->integer != GT_FFA) {
        return false;
    }
    return Practice_CategoryFromName(g_practice->string) != PRACTICE_CAT_NONE;
}

bool PracticeManager::IsEdit(void) const
{
    return g_practice_edit && g_practice_edit->integer != 0;
}

int PracticeManager::ReactionMs(void) const
{
    const char *diff = g_practice_difficulty ? g_practice_difficulty->string : "medium";
    if (!Q_stricmp(diff, "hard")) {
        return 250;
    }
    if (!Q_stricmp(diff, "easy")) {
        return 750;
    }
    return 500;
}

int PracticeManager::FireIntervalMs(void) const
{
    const char *diff = g_practice_difficulty ? g_practice_difficulty->string : "medium";
    if (!Q_stricmp(diff, "hard")) {
        return 350;
    }
    if (!Q_stricmp(diff, "easy")) {
        return 750;
    }
    return 500;
}

// Changed in Omaha: angles mode is single-player — the claimed owner is "the" human.
// Nade mode is multi-player and uses per-client state instead of GetHuman.
Player *PracticeManager::GetHuman(void)
{
    int i;

    if (m_iAnglesOwner >= 0 && m_iAnglesOwner < game.maxclients && Practice_IsHumanEntity(&g_entities[m_iAnglesOwner])) {
        return static_cast<Player *>(g_entities[m_iAnglesOwner].entity);
    }
    if (Practice_IsHumanEntity(&g_entities[0])) {
        return static_cast<Player *>(g_entities[0].entity);
    }
    for (i = 1; i < game.maxclients; i++) {
        if (Practice_IsHumanEntity(&g_entities[i])) {
            return static_cast<Player *>(g_entities[i].entity);
        }
    }
    return NULL;
}

int PracticeManager::SlotFor(Player *player) const
{
    int slot;

    if (!player || !player->edict || !player->client) {
        return -1;
    }
    slot = player->edict->s.number;
    if (slot < 0 || slot >= game.maxclients || slot >= MAX_CLIENTS) {
        return -1;
    }
    return slot;
}

PracticeClient *PracticeManager::ClientFor(Player *player)
{
    const int slot = SlotFor(player);

    if (slot < 0 || G_IsBot(player->edict)) {
        return NULL;
    }
    return &m_clients[slot];
}

// Added in Omaha: listen-server local player — the only client that receives omp_* (release tick).
bool PracticeManager::IsOmahaHost(Player *player) const
{
    return player && player->edict && !dedicated->integer && player->edict->s.number == 0 && !G_IsBot(player->edict);
}

// Stock cgame prints "Unknown client game command" for omp_*, so never broadcast them.
void PracticeManager::SendOmahaCommand(const char *fmt, ...)
{
    char    buf[MAX_STRING_CHARS];
    va_list args;

    if (dedicated->integer || !Practice_IsHumanEntity(&g_entities[0])) {
        return;
    }
    va_start(args, fmt);
    Q_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    gi.SendServerCommand(0, "%s", buf);
}

bool PracticeManager::IsAnglesBystander(Player *player) const
{
    const int slot = SlotFor(player);

    return m_scenario.category == PRACTICE_CAT_ANGLES && m_iAnglesOwner >= 0 && slot >= 0 && slot != m_iAnglesOwner;
}

Player *PracticeManager::HumanFromEvent(Event *ev)
{
    // Changed in Omaha: no GetHuman fallback — edit/write cmds need a real console edict.
    if (ev && ev->isSubclassOf(ConsoleEvent)) {
        gentity_t *edict = static_cast<ConsoleEvent *>(ev)->GetConsoleEdict();
        if (edict && edict->entity && edict->entity->IsSubclassOfPlayer()) {
            return static_cast<Player *>(edict->entity);
        }
    }
    return NULL;
}

bool PracticeManager::MayEdit(Player *player)
{
    if (!IsActive() || !IsEdit() || !player || !player->edict || G_IsBot(player->edict)) {
        return false;
    }
    // Changed in Omaha: g_practice_edit opens editing to every connected player (private servers).
    if (IsAnglesBystander(player)) {
        EditFeedback(player, "Angles practice is single-player");
        return false;
    }
    return true;
}

void PracticeManager::ClearOrderDraft(void)
{
    m_orderDraft.clear();
    m_iOrderExpected = 0;
}

void PracticeManager::LoadScenario(void)
{
    char  err[256];
    char *buf = NULL;
    long  len;
    char  levelMap[MAX_QPATH];
    char  scenarioMap[MAX_QPATH];

    m_bLoaded         = false;
    m_bLoadOk         = false;
    m_bMapMismatch    = false;
    m_bComplete       = false;
    m_bRunActive      = false;
    m_bBotsPlaced     = false;
    m_bDeathRewind    = false;
    m_iNextSpot       = 0;
    m_iCurrentGate    = -1;
    m_iDeathSpawnGate = 0;
    m_bCueCacheDirty  = true;
    ClearOrderDraft();
    m_scenario.Clear();

    if (!g_practice_file || !g_practice_file->string[0]) {
        m_scenario.category = Practice_CategoryFromName(g_practice->string);
        Q_strncpyz(m_scenario.map, level.mapname.c_str(), sizeof(m_scenario.map));
        Q_strncpyz(m_scenario.name, "Untitled", sizeof(m_scenario.name));
        m_bLoaded = true;
        // No path — Autosave already no-ops; keep m_bLoadOk false.
        return;
    }

    if (!Practice_IsWritableScenarioPath(g_practice_file->string)) {
        gi.Printf("Practice: refusing unsafe g_practice_file path\n");
        m_scenario.category = Practice_CategoryFromName(g_practice->string);
        Q_strncpyz(m_scenario.map, level.mapname.c_str(), sizeof(m_scenario.map));
        m_bLoaded = true;
        return;
    }

    len = gi.FS_ReadFile(g_practice_file->string, (void **)&buf, qtrue);
    if (len <= 0 || !buf) {
        gi.Printf("Practice: could not read %s\n", g_practice_file->string);
        m_scenario.category = Practice_CategoryFromName(g_practice->string);
        Q_strncpyz(m_scenario.map, level.mapname.c_str(), sizeof(m_scenario.map));
        m_bLoaded = true;
        // Missing file in edit → allow first Autosave (create).
        m_bLoadOk = IsEdit();
        return;
    }

    if (!PracticeScenario_Parse(buf, &m_scenario, err, sizeof(err))) {
        gi.Printf("Practice: parse error in %s: %s\n", g_practice_file->string, err);
        m_scenario.category = Practice_CategoryFromName(g_practice->string);
        Q_strncpyz(m_scenario.map, level.mapname.c_str(), sizeof(m_scenario.map));
        gi.FS_FreeFile(buf);
        m_bLoaded = true;
        m_bLoadOk = false;
        return;
    }
    gi.FS_FreeFile(buf);

    if (m_scenario.category == PRACTICE_CAT_NONE) {
        m_scenario.category = Practice_CategoryFromName(g_practice->string);
    }
    if (!m_scenario.map[0]) {
        Q_strncpyz(m_scenario.map, level.mapname.c_str(), sizeof(m_scenario.map));
    }

    Q_strncpyz(levelMap, level.mapname.c_str(), sizeof(levelMap));
    Q_strncpyz(scenarioMap, m_scenario.map, sizeof(scenarioMap));
    COM_StripExtension(levelMap, levelMap, sizeof(levelMap));
    COM_StripExtension(scenarioMap, scenarioMap, sizeof(scenarioMap));
    if (Q_stricmp(levelMap, scenarioMap)) {
        m_bMapMismatch = true;
        gi.Printf(
            "Practice: scenario map '%s' does not match level '%s'\n",
            m_scenario.map,
            level.mapname.c_str()
        );
    }

    m_bLoaded = true;
    m_bLoadOk = true;

    // Persist rebuilt node ids so the file matches what the editor shows.
    if (IsEdit() && m_scenario.nodesRenumbered) {
        m_scenario.nodesRenumbered = false;
        Autosave();
    }
}

void PracticeManager::Autosave(void)
{
    std::string json;

    if (!IsActive()) {
        return;
    }
    if (!m_bLoadOk) {
        return;
    }
    if (!g_practice_file || !g_practice_file->string[0]) {
        gi.Printf("Practice: no g_practice_file set, not saving\n");
        return;
    }
    if (!Practice_IsWritableScenarioPath(g_practice_file->string)) {
        gi.Printf("Practice: refusing to write outside practice/\n");
        return;
    }
    if (!PracticeScenario_Dump(m_scenario, &json)) {
        return;
    }
    if (gi.FS_WriteFile(g_practice_file->string, json.c_str(), (int)json.size()) < 0) {
        gi.Printf("Practice: failed to write %s\n", g_practice_file->string);
        return;
    }
    // Changed in Omaha: cue/marker cache only — node labels are server huddraw now.
    m_bCueCacheDirty = true;
}

void PracticeManager::SetFeedback(int slot, const char *text, int durationMs)
{
    if (slot < 0 || slot >= MAX_CLIENTS) {
        return;
    }
    Q_strncpyz(m_clients[slot].feedback, text ? text : "", sizeof(m_clients[slot].feedback));
    m_clients[slot].feedbackExpire = level.inttime + durationMs;
}

void PracticeManager::EditFeedback(Player *player, const char *fmt, ...)
{
    char    buf[256];
    va_list args;

    if (!player || !player->edict) {
        return;
    }
    va_start(args, fmt);
    Q_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    gi.centerprintf(player->edict, "%s", buf);
}

// Added in Omaha: modern HUD game-messages panel (hudprint / gmbox).
void PracticeManager::GameMessage(Player *player, const char *fmt, ...)
{
    char    buf[256];
    va_list args;

    if (!player) {
        return;
    }
    va_start(args, fmt);
    Q_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    player->HUDPrint(buf);
}

void PracticeManager::TeleportToSpot(Player *player, const vec3_t origin, const vec3_t angles)
{
    Vector org(origin);
    Vector ang(angles);

    player->setOrigin(org + Vector(0, 0, 1));
    player->origin.copyTo(player->edict->s.origin2);
    player->edict->s.renderfx |= RF_FRAMELERP;
    player->velocity = vec_zero;
    player->setAngles(ang);
    player->SetViewAngles(ang);
    player->SetupView();
    VectorCopy(player->origin, player->client->ps.vEyePos);
    player->client->ps.vEyePos[2] += player->client->ps.viewheight;
}

// Added in Omaha: stock ChooseSpawnPoint re-asserts viewangles for a few frames so
// client cmd_angles do not snap the look back after SetViewAngles.
// Body keeps yaw only — applying view pitch to setAngles pitches the model into the ground.
void PracticeManager::ForceViewAngles(Player *player, const vec3_t angles)
{
    Vector view(angles);
    Vector body(angles);
    int    i;

    if (!player) {
        return;
    }

    body[0] = 0.0f;
    body[2] = 0.0f;
    player->setAngles(body);
    player->SetViewAngles(view);
    for (i = 1; i <= 4; i++) {
        Event *ev = new Event(EV_SetViewangles);
        ev->AddVector(view);
        player->PostEvent(ev, level.frametime * i);
    }
}

// Added in Omaha: place the human at a gate position (start if gate is 0).
void PracticeManager::PlaceHumanAtGate(Player *player, int gate)
{
    vec3_t angles;

    if (!player) {
        return;
    }

    if (gate < 0) {
        gate = 0;
    }

    VectorCopy(m_scenario.startAngles.v, angles);
    if (gate > 0 && gate < (int)m_scenario.gates.size()) {
        if (gate + 1 < (int)m_scenario.gates.size()) {
            vec3_t dir;

            VectorSubtract(m_scenario.gates[gate + 1].v, m_scenario.gates[gate].v, dir);
            if (VectorLength(dir) > 1.0f) {
                vectoangles(dir, angles);
                angles[2] = 0.0f;
            }
        }
        TeleportToSpot(player, m_scenario.gates[gate].v, angles);
        return;
    }

    if (m_scenario.hasStart) {
        TeleportToSpot(player, m_scenario.startOrigin.v, m_scenario.startAngles.v);
    }
}

// Added in Omaha: block movement for a short death-respawn penalty.
void PracticeManager::FreezeHuman(Player *player, float seconds)
{
    Event *ev;

    if (!player || seconds <= 0.0f) {
        return;
    }

    player->CancelEventsOfType(EV_Player_FreezeControls);
    player->m_bFrozen = true;

    ev = new Event(EV_Player_FreezeControls);
    ev->AddInteger(0);
    player->PostEvent(ev, seconds);
}

// Added in Omaha: true if any alive placed bot can see stand/crouch eye at this gate.
bool PracticeManager::GateExposedToBots(int gate) const
{
    std::vector<PracticeBotController *> bots;
    Vector                               origin;
    Vector                               samples[3];
    int                                  i;
    int                                  s;

    if (gate < 0 || gate >= (int)m_scenario.gates.size()) {
        return false;
    }

    origin     = Vector(m_scenario.gates[gate].v);
    samples[0] = origin + Vector(0, 0, (float)DEFAULT_VIEWHEIGHT);
    samples[1] = origin + Vector(0, 0, (float)CROUCH_VIEWHEIGHT);
    samples[2] = origin + Vector(0, 0, 36.0f); // torso / centroid-ish

    Practice_CollectControllers(bots);

    for (i = 0; i < (int)bots.size(); i++) {
        Player *bot = bots[i]->getControlledEntity();

        if (!bots[i]->HasHome() || !bot || bot->IsDead() || bot->IsSpectator()) {
            continue;
        }
        for (s = 0; s < 3; s++) {
            if (bots[i]->SeesWorldPoint(samples[s])) {
                return true;
            }
        }
    }
    return false;
}

// Added in Omaha: walk back from the previous checkpoint until one is clear of bot LOS.
int PracticeManager::FindSafeDeathSpawnGate(void) const
{
    int gate;

    gate = m_iCurrentGate - 1;
    if (gate < 0) {
        gate = 0;
    }
    if (gate >= (int)m_scenario.gates.size()) {
        gate = (int)m_scenario.gates.size() - 1;
    }

    for (; gate > 0; gate--) {
        if (!GateExposedToBots(gate)) {
            return gate;
        }
    }
    return 0;
}

bool PracticeManager::TrySpawnPlayer(Player *player)
{
    PracticeBotController *controller;

    if (!IsActive() || !player) {
        return false;
    }

    controller = Practice_ControllerFor(player);
    if (controller) {
        if (!controller->HasHome()) {
            // Fixed in Omaha: park homeless bots off-map (no stock spawn, no spectator).
            // Spectator mode left bots assigned-but-invisible after PlaceBots.
            if (player->IsSpectator()) {
                player->EndSpectator();
            }
            player->setSolidType(SOLID_NOT);
            player->hideModel();
            player->setOrigin(Vector(0, 0, -20000));
            player->velocity = vec_zero;
            return true;
        }
        // No KillBox: a bot appearing on its spot must never telefrag the player.
        if (player->IsSpectator()) {
            player->EndSpectator();
        }
        player->showModel();
        player->setSolidType(SOLID_BBOX);
        TeleportToSpot(player, controller->HomeOrigin(), controller->HomeAngles());
        return true;
    }

    // Changed in Omaha: nade mode has no start marker — death respawns at death place only.
    // Flag stays set until OnPlayerSpawned so view angles can be re-asserted after Init.
    if (m_scenario.category == PRACTICE_CAT_NADES) {
        PracticeClient *pc = ClientFor(player);

        if (pc && pc->deathAtPlace) {
            TeleportToSpot(player, pc->deathSpawnOrigin, pc->deathSpawnAngles);
            return true;
        }
        return false;
    }

    if (m_scenario.hasStart) {
        TeleportToSpot(player, m_scenario.startOrigin.v, m_scenario.startAngles.v);
        return true;
    }

    return false;
}

void PracticeManager::StripGrenades(Player *player)
{
    static const char *names[] = {
        "m2frag_grenade",
        "mills_grenade",
        "steilhandgranate",
        "Russian_F1_grenade",
        "it_w_bomba",
        "M18_smoke_grenade",
        "nebelhandgranate",
        "RDG-1_Smoke_grenade",
        "it_w_bombabreda",
        NULL
    };
    int i;

    if (!player) {
        return;
    }

    for (i = 0; names[i]; i++) {
        if (player->HasItem(names[i])) {
            player->takeItem(names[i]);
        }
    }

    player->takeAmmoType("grenade");
    player->takeAmmoType("agrenade");
    player->takeAmmoType("smokegrenade");
    player->takeAmmoType("asmokegrenade");
}

void PracticeManager::RefillGrenades(Player *player)
{
    if (!player || player->IsDead() || player->IsSpectator()) {
        return;
    }
    player->GiveAmmo("grenade", 5, 5);
    player->GiveAmmo("agrenade", 5, 5);
}

void PracticeManager::EquipRifle(Player *player)
{
    Event *useEv;

    if (!player) {
        return;
    }

    // Always Garand — EquipWeapons("rifle") gives Kar98/Mauser on Axis.
    Q_strncpyz(player->client->pers.dm_primary, "rifle", sizeof(player->client->pers.dm_primary));
    player->FreeInventory();
    player->giveItem("weapons/m1_garand.tik");
    player->GiveAmmo("rifle", 200);
    useEv = new Event(EV_Sentient_UseItem);
    useEv->AddString("M1 Garand");
    player->PostEvent(useEv, 0.1f);
    StripGrenades(player);
}

void PracticeManager::ApplyLoadout(Player *player)
{
    PracticeBotController *controller;

    if (!player) {
        return;
    }

    controller = Practice_ControllerFor(player);

    if (controller && controller->IsDummy()) {
        player->FreeInventory();
        player->health     = PRACTICE_DUMMY_HEALTH;
        player->max_health = PRACTICE_DUMMY_HEALTH;
        return;
    }

    if (controller) {
        EquipRifle(player);
        return;
    }

    if (m_scenario.category == PRACTICE_CAT_ANGLES) {
        StripGrenades(player);
    }
}

void PracticeManager::OnPlayerSpawned(Player *player)
{
    if (!IsActive() || !player) {
        return;
    }

    ApplyLoadout(player);

    if (!Practice_ControllerFor(player) && !G_IsBot(player->edict)) {
        PracticeClient *pc = ClientFor(player);

        // Added in Omaha: angles is single-player — the first human to spawn claims the run.
        if (m_scenario.category == PRACTICE_CAT_ANGLES) {
            const int slot = SlotFor(player);

            if (m_iAnglesOwner < 0 || !Practice_IsHumanEntity(&g_entities[m_iAnglesOwner])) {
                m_iAnglesOwner = slot;
            }
            if (slot != m_iAnglesOwner) {
                // EnforceAnglesSolo moves bystanders back to spectator next frame.
                return;
            }
        }
        // Added in Omaha: first join / rejoin after clear or timeout starts a timed run.
        if (m_scenario.category == PRACTICE_CAT_ANGLES && !IsEdit()) {
            if (!m_bRunActive) {
                BeginAnglesRun();
            } else if (m_bDeathRewind) {
                // Changed in Omaha: death teleports to nearest prior gate clear of bot LOS.
                m_bDeathRewind = false;
                PlaceHumanAtGate(player, m_iDeathSpawnGate);
                FreezeHuman(player, 1.0f);
            }
        } else if (m_scenario.category == PRACTICE_CAT_NADES && pc && pc->deathAtPlace) {
            // Fixed in Omaha: restore look direction after respawn (Init zeros view; cmd_angles fight).
            pc->deathAtPlace = false;
            TeleportToSpot(player, pc->deathSpawnOrigin, pc->deathSpawnAngles);
            ForceViewAngles(player, pc->deathSpawnAngles);
        }
    }
}

bool PracticeManager::AssignNextSpot(PracticeBotController *controller)
{
    const PracticeSpot *spot;

    if (!controller || m_scenario.category != PRACTICE_CAT_ANGLES) {
        return false;
    }
    if (m_iCurrentGate < 0) {
        return false;
    }

    while (m_iNextSpot < (int)m_scenario.spots.size()) {
        spot = &m_scenario.spots[m_iNextSpot];
        if (spot->unlockGate > m_iCurrentGate) {
            return false;
        }
        m_iNextSpot++;
        controller->SetHome(spot->origin.v, spot->angles.v, spot->stance, spot->lean);
        controller->SetDummy(false);
        controller->SetAllowFire(!IsEdit());
        return true;
    }
    return false;
}

bool PracticeManager::NextSpotLocked(void) const
{
    if (m_iNextSpot >= (int)m_scenario.spots.size()) {
        return false;
    }
    return m_scenario.spots[m_iNextSpot].unlockGate > m_iCurrentGate;
}

int PracticeManager::UnlockedSpotCount(void) const
{
    int count = 0;
    int i;

    if (m_iCurrentGate < 0) {
        return 0;
    }
    for (i = 0; i < (int)m_scenario.spots.size(); i++) {
        if (m_scenario.spots[i].unlockGate <= m_iCurrentGate) {
            count++;
        }
    }
    return count;
}

// Added in Omaha: start (or restart) a timed angles attempt.
void PracticeManager::BeginAnglesRun(void)
{
    std::vector<PracticeBotController *> bots;
    int                                  i;

    if (m_bMapMismatch && !IsEdit()) {
        Player *human = GetHuman();
        if (human) {
            EditFeedback(human, "Scenario map mismatch");
        }
        return;
    }

    m_bRunActive      = true;
    m_iNextSpot       = 0;
    m_iCurrentGate    = 0;
    m_bComplete       = false;
    m_bDeathRewind    = false;
    m_iDeathSpawnGate = 0;
    m_iRunStartTime   = level.inttime;
    m_bBotsPlaced     = false;

    Practice_CollectControllers(bots);
    for (i = 0; i < (int)bots.size(); i++) {
        Player *bot = bots[i]->getControlledEntity();

        bots[i]->ClearHome();
        bots[i]->SetAllowFire(false);
        if (bot) {
            bot->CancelEventsOfType(EV_Player_Respawn);
        }
    }

    EnsureBots();
    Practice_CollectControllers(bots);
    for (i = 0; i < (int)bots.size(); i++) {
        Player *bot = bots[i]->getControlledEntity();

        if (!bot) {
            continue;
        }
        if (!AssignNextSpot(bots[i])) {
            continue;
        }
        if (bot->IsDead()) {
            bot->ProcessEvent(EV_Player_Respawn);
        } else if (!bot->IsSpectator()) {
            TeleportToSpot(bot, bots[i]->HomeOrigin(), bots[i]->HomeAngles());
        }
    }
    m_bBotsPlaced = true;

    SetFeedback(SlotFor(GetHuman()), "SCENARIO RESET", 2500);
    SyncWaypointObjective();
}

// Added in Omaha: end a timed angles attempt (clear or timeout) → spectator, optional high score.
void PracticeManager::EndAnglesRun(bool cleared)
{
    std::vector<PracticeBotController *> bots;
    Player                              *human;
    int                                  i;
    int                                  elapsed;
    char                                 timeBuf[32];

    if (!m_bRunActive && !m_bComplete) {
        // Idle already; still allow a cleared end if somehow flagged mid-frame.
        if (!cleared) {
            return;
        }
    }

    m_bRunActive   = false;
    m_bDeathRewind = false;

    Practice_CollectControllers(bots);
    for (i = 0; i < (int)bots.size(); i++) {
        Player *bot = bots[i]->getControlledEntity();

        bots[i]->ClearHome();
        bots[i]->SetAllowFire(false);
        if (bot) {
            bot->CancelEventsOfType(EV_Player_Respawn);
        }
    }

    level.m_vObjectiveLocation = vec_zero;

    human = GetHuman();
    if (human) {
        human->CancelEventsOfType(EV_Player_FreezeControls);
        human->m_bFrozen = false;
        if (!human->IsSpectator()) {
            // Same stock path as team → spectator (Player::Spectator event → SetTeam).
            human->SetTeam(TEAM_SPECTATOR);
        }
    }

    if (cleared) {
        elapsed = level.inttime - m_iRunStartTime;
        if (elapsed < 0) {
            elapsed = 0;
        }
        m_bComplete     = true;
        m_iCompleteTime = level.inttime;

        Practice_FormatTimeMs(elapsed, timeBuf, sizeof(timeBuf));
        SetFeedback(SlotFor(human), va("CLEARED  %s", timeBuf), 5000);
        // Changed in Omaha: score/end feedback via HUD game messages (not console-only).
        GameMessage(human, "Cleared in %s", timeBuf);

        if (m_scenario.bestTimeMs == 0 || elapsed < m_scenario.bestTimeMs) {
            const char *diff = g_practice_difficulty ? g_practice_difficulty->string : "medium";

            m_scenario.bestTimeMs = elapsed;
            // Added in Omaha: record which difficulty the best clear was played on.
            if (!Q_stricmp(diff, "hard")) {
                Q_strncpyz(m_scenario.bestDifficulty, "hard", sizeof(m_scenario.bestDifficulty));
            } else if (!Q_stricmp(diff, "easy")) {
                Q_strncpyz(m_scenario.bestDifficulty, "easy", sizeof(m_scenario.bestDifficulty));
            } else {
                Q_strncpyz(m_scenario.bestDifficulty, "medium", sizeof(m_scenario.bestDifficulty));
            }
            Autosave();
            GameMessage(human, "NEW HIGH SCORE! (%s)", m_scenario.bestDifficulty);
        } else {
            char bestBuf[32];

            Practice_FormatTimeMs(m_scenario.bestTimeMs, bestBuf, sizeof(bestBuf));
            if (m_scenario.bestDifficulty[0]) {
                GameMessage(human, "Best remains %s (%s)", bestBuf, m_scenario.bestDifficulty);
            } else {
                GameMessage(human, "Best remains %s", bestBuf);
            }
        }
    } else {
        m_bComplete = false;
        GameMessage(human, "Time limit expired (30:00)");
    }
}

void PracticeManager::OnPlayerKilled(Player *player)
{
    PracticeBotController *controller;
    Player                *human;

    if (!IsActive() || !player) {
        return;
    }

    controller = Practice_ControllerFor(player);

    if (controller) {
        // Added in Omaha: player heals a bit for each practice kill during an active run.
        human = GetHuman();
        if (human && !human->IsDead() && !human->IsSpectator() && !IsEdit() && m_bRunActive) {
            human->health += 25.0f;
            if (human->health > human->max_health) {
                human->health = human->max_health;
            }
        }

        if (m_scenario.category == PRACTICE_CAT_ANGLES && !IsEdit() && m_bRunActive) {
            if (AssignNextSpot(controller)) {
                player->PostEvent(EV_Player_Respawn, PRACTICE_BOT_RESPAWN_DELAY);
            } else {
                // Wait for a later waypoint gate, or end the run when the pool is exhausted.
                controller->ClearHome();
                // Fixed in Omaha: cancel pending respawn; Practice_BlockStockRespawn keeps
                // locked/exhausted bots dead until the next gate PlaceBots.
                player->CancelEventsOfType(EV_Player_Respawn);
                {
                    std::vector<PracticeBotController *> bots;
                    bool                                 anyAlive = false;
                    int                                  i;

                    Practice_CollectControllers(bots);
                    for (i = 0; i < (int)bots.size(); i++) {
                        Player *other = bots[i]->getControlledEntity();
                        // Fixed in Omaha: parked (no home) bots are alive off-map — ignore them
                        // or clear never fires after the last placed bot dies.
                        if (!bots[i]->HasHome() || !other || other == player || other->IsDead()
                            || other->IsSpectator()) {
                            continue;
                        }
                        anyAlive = true;
                        break;
                    }
                    if (!anyAlive && !NextSpotLocked() && m_iNextSpot >= (int)m_scenario.spots.size()) {
                        EndAnglesRun(true);
                    }
                }
            }
        } else if (m_scenario.category == PRACTICE_CAT_NADES && controller->IsDummy()) {
            player->PostEvent(EV_Player_Respawn, 1.5f);
        }
        return;
    }

    if (m_scenario.category == PRACTICE_CAT_NADES) {
        PracticeClient *pc = ClientFor(player);

        if (!pc) {
            return;
        }
        // Added in Omaha: nade play/edit — respawn at death origin + look (observe landing / continue editing).
        // Fixed in Omaha: store v_angle/ps.viewangles — at death m_vViewAng can be wrapped junk
        // (e.g. pitch -270 → EulerNormalize → 90 straight down) while v_angle stays correct.
        // Chase cam owns the view while active — keep the pre-throw look instead.
        pc->deathSpawnOrigin = player->origin;
        pc->deathSpawnAngles = pc->camera ? pc->camRestoreAngles : player->GetViewAngles();
        pc->deathAtPlace     = true;
        StopChaseCam(SlotFor(player), false);
        player->PostEvent(EV_Player_Respawn, 0.5f);
        return;
    }

    if (m_scenario.category == PRACTICE_CAT_ANGLES && !IsEdit() && m_bRunActive) {
        // Changed in Omaha: walk prior checkpoints until one has no bot LOS; progress unchanged.
        m_iDeathSpawnGate = FindSafeDeathSpawnGate();
        m_bDeathRewind    = true;
        player->PostEvent(EV_Player_Respawn, 0.15f);
    }
}

void PracticeManager::OnProjectileSpawned(Projectile *proj, Entity *owner)
{
    Player         *human;
    PracticeClient *pc;
    int             slot;

    if (!IsActive() || m_scenario.category != PRACTICE_CAT_NADES || !proj || !owner) {
        return;
    }
    // Replay spawns have weap == NULL; live throws must be grenade-class.
    if (proj->weap && !(proj->weap->GetWeaponClass() & WEAPON_CLASS_GRENADE)) {
        return;
    }
    if (!proj->weap && !m_bNadeReplay) {
        return;
    }

    // Changed in Omaha: every human tracks their own throw (hostable multi-player nade practice).
    if (!owner->IsSubclassOfPlayer()) {
        return;
    }
    human = static_cast<Player *>(owner);
    pc    = ClientFor(human);
    slot  = SlotFor(human);
    if (!pc || Practice_ControllerFor(human)) {
        return;
    }

    pc->lastNadeEnt = proj->entnum;

    if (m_bNadeReplay) {
        // Silent rethrow: keep original stand/aim for further replays; no chase cam.
        return;
    }

    pc->lastThrowStand = human->origin;
    // Fixed in Omaha: cue uses camera/crosshair (GetViewAngles + FP pitch pivot + lean),
    // not weapon m_vViewPos/m_vViewAng. Bare feet+viewheight sits below the real camera when looking up.
    pc->lastThrowAim     = human->GetViewAngles();
    pc->lastThrowFireAim = human->m_vViewAng;
    pc->lastNadeStart    = proj->origin;
    pc->lastNadeOrigin   = proj->origin;
    pc->lastNadeCharge   = proj->charge_fraction;
    pc->lastThrowTime    = level.inttime;
    pc->lastNadeExploded = false;
    // Added in Omaha: capture pose for aim-cue eye (crouch / jump / lean combinations).
    pc->lastThrowLean = PRACTICE_LEAN_NONE;
    if (human->client && human->client->ps.fLeanAngle > 5.0f) {
        pc->lastThrowLean = PRACTICE_LEAN_RIGHT;
    } else if (human->client && human->client->ps.fLeanAngle < -5.0f) {
        pc->lastThrowLean = PRACTICE_LEAN_LEFT;
    }
    // Fixed in Omaha: jump before crouch — grenade throw anims call modheight "duck" and
    // briefly set viewheight to CROUCH_VIEWHEIGHT even on a jump throw.
    if (human->client
        && ((human->client->ps.pm_flags & PMF_VIEW_JUMP_START) || !human->client->ps.walking
            || human->client->ps.groundEntityNum == ENTITYNUM_NONE)) {
        pc->lastThrowStance = PRACTICE_STANCE_JUMP;
    } else if (
        human->client
        && ((human->client->ps.pm_flags & PMF_DUCKED)
            || human->client->ps.viewheight <= (int)CROUCH_VIEWHEIGHT)
    ) {
        pc->lastThrowStance = PRACTICE_STANCE_CROUCH;
    } else {
        pc->lastThrowStance = PRACTICE_STANCE_STAND;
    }
    {
        const float viewh =
            human->client ? (float)human->client->ps.viewheight : (float)human->viewheight;
        const float leanAng = human->client ? human->client->ps.fLeanAngle : 0.0f;
        vec3_t      eye;

        Practice_CameraEye(eye, human->origin, pc->lastThrowAim, viewh, leanAng);
        pc->lastThrowEye = eye;
    }
    if (proj->model.length()) {
        Q_strncpyz(pc->lastNadeModel, proj->model.c_str(), sizeof(pc->lastNadeModel));
    }

    // Changed in Omaha: server Camera chase (stock clients follow ps.camera_*). The listen
    // host's cg_practice_nadecam opt-out is readable here; remote players always get it.
    if (IsOmahaHost(human)) {
        cvar_t *nadecam = gi.Cvar_Get("cg_practice_nadecam", "1", 0);
        if (nadecam && !nadecam->integer) {
            return;
        }
    }

    StopChaseCam(slot, false);
    {
        Camera *cam = new Camera;

        cam->setOrigin(pc->lastThrowEye);
        cam->setAngles(pc->lastThrowAim);
        pc->camera           = cam;
        pc->camAttached      = false;
        pc->camStartTime     = level.inttime;
        pc->camExplodeTime   = 0;
        pc->camRestoreAngles = pc->lastThrowAim;
        pc->nadeTrail.clear();
    }

    // Added in Omaha: lock the thrower in place for the chase cam.
    {
        Event *ev;

        human->CancelEventsOfType(EV_Player_FreezeControls);
        human->m_bFrozen = true;
        human->velocity  = vec_zero;
        // Safety unfreeze if explode never arrives.
        ev = new Event(EV_Player_FreezeControls);
        ev->AddInteger(0);
        human->PostEvent(ev, 20.0f);
    }
}

void PracticeManager::EndNadeChase(int slot, const Vector& origin, int hit)
{
    PracticeClient *pc;
    Player         *human;

    if (slot < 0 || slot >= MAX_CLIENTS) {
        return;
    }
    pc                   = &m_clients[slot];
    pc->lastNadeEnt      = -1;
    pc->lastNadeOrigin   = origin;
    pc->lastNadeExploded = true;

    human = pc->player;
    if (!human || !human->client) {
        return;
    }

    SetFeedback(slot, hit > 0 ? "NADE HIT" : (hit < 0 ? "NADE LOST" : "NADE MISS"), 2000);

    if (pc->camera) {
        pc->camExplodeTime = level.inttime;
        pc->camRest        = origin;
        if (!pc->camAttached) {
            StopChaseCam(slot, false);
        }

        // Changed in Omaha: release chase freeze once the camera hold ends.
        {
            Event *ev;

            human->CancelEventsOfType(EV_Player_FreezeControls);
            ev = new Event(EV_Player_FreezeControls);
            ev->AddInteger(0);
            human->PostEvent(ev, PRACTICE_NADE_HOLD_MS / 1000.0f);
        }
    }
}

void PracticeManager::OnProjectileExplode(Projectile *proj)
{
    int hit = 0;
    int slot;
    int i;

    if (!IsActive() || !proj || m_scenario.category != PRACTICE_CAT_NADES) {
        return;
    }
    for (slot = 0; slot < game.maxclients && slot < MAX_CLIENTS; slot++) {
        if (m_clients[slot].lastNadeEnt == proj->entnum && m_clients[slot].player) {
            break;
        }
    }
    if (slot >= game.maxclients || slot >= MAX_CLIENTS) {
        return;
    }

    for (i = 0; i < (int)m_scenario.throwsList.size(); i++) {
        const PracticeThrow& th = m_scenario.throwsList[i];
        Vector               delta;

        delta = m_clients[slot].lastThrowStand - Vector(th.stand.v);
        if (delta.length() > 48.0f) {
            continue;
        }
        delta = proj->origin - Vector(th.target.v);
        if (delta.length() <= th.radius) {
            hit = 1;
            break;
        }
    }
    EndNadeChase(slot, proj->origin, hit);
}

void PracticeManager::OnProjectileGone(int slot)
{
    if (!IsActive() || m_scenario.category != PRACTICE_CAT_NADES || slot < 0 || slot >= MAX_CLIENTS
        || m_clients[slot].lastNadeEnt < 0) {
        return;
    }

    // Fixed in Omaha: sky / OOB / remove-without-explode never called Explode, so chase
    // cam + freeze stayed until the 20s safety timeout. Treat last known origin as a miss.
    EndNadeChase(slot, m_clients[slot].lastNadeOrigin, -1);
}

// Added in Omaha: follow the live nade (trail sample + explode/lost detection).
void PracticeManager::UpdateNadeTracking(int slot)
{
    PracticeClient *pc = &m_clients[slot];
    Entity         *nade = NULL;
    const int       ent  = pc->lastNadeEnt;

    if (ent < 0) {
        return;
    }
    if (ent < globals.num_entities && g_entities[ent].inuse && g_entities[ent].entity
        && g_entities[ent].entity->isSubclassOf(Projectile)) {
        nade = g_entities[ent].entity;
    }
    if (!nade) {
        OnProjectileGone(slot);
        return;
    }

    pc->lastNadeOrigin = nade->origin;
    if (pc->camera) {
        PracticeNadeSample sample;

        sample.time   = level.inttime;
        sample.origin = nade->origin;
        pc->nadeTrail.push_back(sample);
        if ((int)pc->nadeTrail.size() > PRACTICE_NADE_TRAIL_MAX) {
            pc->nadeTrail.erase(pc->nadeTrail.begin());
        }
    }
}

// Sample the recorded nade path at an earlier time (camera rides behind the tip).
static bool Practice_TrailSampleAt(const std::vector<PracticeNadeSample>& trail, int atTime, Vector& out)
{
    size_t i;

    if (trail.empty()) {
        return false;
    }
    if (atTime <= trail[0].time) {
        out = trail[0].origin;
        return true;
    }
    for (i = 1; i < trail.size(); i++) {
        const PracticeNadeSample& a = trail[i - 1];
        const PracticeNadeSample& b = trail[i];

        if (atTime <= b.time) {
            const int span = b.time - a.time;
            if (span <= 0) {
                out = b.origin;
            } else {
                const float t = (float)(atTime - a.time) / (float)span;
                out           = a.origin + (b.origin - a.origin) * t;
            }
            return true;
        }
    }
    out = trail.back().origin;
    return true;
}

void PracticeManager::UpdateChaseCam(int slot)
{
    PracticeClient *pc     = &m_clients[slot];
    Player         *player = pc->player;
    Camera         *cam    = pc->camera;
    Vector          pos;
    Vector          look;
    Vector          dir;

    if (!cam) {
        return;
    }
    if (!player || !player->client || player->IsDead() || player->IsSpectator()
        || level.inttime - pc->camStartTime > PRACTICE_NADE_CAM_MAX_MS) {
        StopChaseCam(slot, !!player);
        return;
    }
    if (pc->camExplodeTime && level.inttime - pc->camExplodeTime >= PRACTICE_NADE_HOLD_MS) {
        StopChaseCam(slot, true);
        return;
    }
    if (pc->nadeTrail.size() < 2 || !Practice_TrailSampleAt(pc->nadeTrail, level.inttime - PRACTICE_NADE_LATENCY_MS, pos)) {
        return;
    }

    pos += Vector(0, 0, 24);
    look = pc->camExplodeTime ? pc->camRest : pc->lastNadeOrigin;
    dir  = look - pos;
    if (dir.length() >= 1.0f) {
        cam->setAngles(dir.toAngles());
    }
    cam->setOrigin(pos);

    if (!pc->camAttached) {
        player->SetCamera(cam, 0);
        player->CameraCut();
        pc->camAttached = true;
    }
}

void PracticeManager::StopChaseCam(int slot, bool restoreView)
{
    PracticeClient *pc;
    Player         *player;
    Camera         *cam;

    if (slot < 0 || slot >= MAX_CLIENTS) {
        return;
    }
    pc     = &m_clients[slot];
    player = pc->player;
    cam    = pc->camera;

    if (player && player->client && pc->camAttached) {
        player->SetCamera(NULL, 0);
        player->CameraCut();
        if (restoreView) {
            ForceViewAngles(player, pc->camRestoreAngles);
        }
    }
    if (cam) {
        cam->PostEvent(EV_Remove, 0);
    }
    pc->camera         = NULL;
    pc->camAttached    = false;
    pc->camExplodeTime = 0;
    pc->nadeTrail.clear();
}

int PracticeManager::WantedBots(void) const
{
    int want = 0;

    // Added in Omaha: edit mode uses client-side pose ghosts, not live bots.
    if (IsEdit()) {
        return 0;
    }

    // Changed in Omaha: nade practice uses throw targets/cues only — no dummy bots.
    if (m_scenario.category != PRACTICE_CAT_ANGLES) {
        return 0;
    }

    {
        int maxAlive = m_scenario.maxAlive;

        // Added in Omaha: no bot refill while idle after clear/timeout.
        if (!m_bRunActive) {
            return 0;
        }

        // Added in Omaha: g_practice_maxalive > 0 overrides the scenario value.
        if (g_practice_maxalive && g_practice_maxalive->integer > 0) {
            maxAlive = g_practice_maxalive->integer;
        }
        if (maxAlive < 1) {
            maxAlive = 1;
        } else if (maxAlive > PRACTICE_MAX_BOTS) {
            maxAlive = PRACTICE_MAX_BOTS;
        }

        // Pool tracks unlocked spots (capped by maxAlive). Homeless bots are parked
        // off-map — do not shrink the pool when a bot clears its home on death.
        want = UnlockedSpotCount();
        if (want > maxAlive) {
            want = maxAlive;
        }
    }

    if (want > PRACTICE_MAX_BOTS) {
        want = PRACTICE_MAX_BOTS;
    }
    return want;
}

void PracticeManager::EnsureBots(void)
{
    std::vector<PracticeBotController *> bots;
    int                                  want;

    if (!m_bLoaded) {
        return;
    }

    want = WantedBots();
    Practice_CollectControllers(bots);

    while ((int)bots.size() > want) {
        Player    *bot = bots.back()->getControlledEntity();
        gentity_t *ent = bot ? bot->edict : NULL;

        bots.pop_back();
        if (ent) {
            Practice_RemoveBotSafe(ent);
        }
    }

    if ((int)bots.size() < want) {
        int have = (int)bots.size();

        g_bPracticeCreateController = true;
        while (have < want) {
            if (!G_AddBot()) {
                break;
            }
            have++;
        }
        g_bPracticeCreateController = false;
        m_bBotsPlaced = false;
    }

    if (!m_bBotsPlaced) {
        PlaceBots();
    }
}

void PracticeManager::PlaceBots(void)
{
    std::vector<PracticeBotController *> bots;
    int                                  i;

    m_bBotsPlaced = true;
    // Added in Omaha: do not place bots when scenario map does not match the level (play).
    if (m_bMapMismatch && !IsEdit()) {
        Player *human = GetHuman();
        if (human) {
            EditFeedback(human, "Scenario map mismatch");
        }
        return;
    }
    Practice_CollectControllers(bots);

    if (m_scenario.category == PRACTICE_CAT_ANGLES) {
        if (m_iRunStartTime == 0) {
            m_iRunStartTime = level.inttime;
        }
        for (i = 0; i < (int)bots.size(); i++) {
            Player *bot = bots[i]->getControlledEntity();

            if (!bot) {
                continue;
            }
            // Keep existing homes when growing the pool after a gate unlock.
            if (bots[i]->HasHome()) {
                // Fixed in Omaha: recover bots left spectator/dead/parked despite a home.
                if (bot->IsSpectator()) {
                    bot->EndSpectator();
                    bot->ProcessEvent(EV_Player_Respawn);
                } else if (bot->IsDead()) {
                    bot->ProcessEvent(EV_Player_Respawn);
                } else {
                    bot->showModel();
                    bot->setSolidType(SOLID_BBOX);
                    TeleportToSpot(bot, bots[i]->HomeOrigin(), bots[i]->HomeAngles());
                }
                continue;
            }
            if (!AssignNextSpot(bots[i])) {
                continue;
            }
            if (bot->IsSpectator()) {
                bot->EndSpectator();
                bot->ProcessEvent(EV_Player_Respawn);
            } else if (bot->IsDead()) {
                bot->ProcessEvent(EV_Player_Respawn);
            } else {
                bot->showModel();
                bot->setSolidType(SOLID_BBOX);
                TeleportToSpot(bot, bots[i]->HomeOrigin(), bots[i]->HomeAngles());
            }
        }
        return;
    }

    if (m_scenario.category == PRACTICE_CAT_NADES) {
        int dummyIndex = 0;
        for (i = 0; i < (int)bots.size(); i++) {
            Player *bot = bots[i]->getControlledEntity();

            while (dummyIndex < (int)m_scenario.throwsList.size() && !m_scenario.throwsList[dummyIndex].dummy) {
                dummyIndex++;
            }
            if (dummyIndex >= (int)m_scenario.throwsList.size()) {
                break;
            }
            bots[i]->SetDummy(true);
            bots[i]->SetAllowFire(false);
            bots[i]->SetHome(
                m_scenario.throwsList[dummyIndex].target.v, vec_zero, PRACTICE_STANCE_STAND, PRACTICE_LEAN_NONE
            );
            if (bot && !bot->IsDead() && !bot->IsSpectator()) {
                TeleportToSpot(bot, m_scenario.throwsList[dummyIndex].target.v, vec_zero);
            }
            dummyIndex++;
        }
    }
}

// Added in Omaha: drive the retail compass objective wedge to the next waypoint.
// Changed in Omaha: also place a green dynamic light there (play mode; no beam).
void PracticeManager::SyncWaypointObjective(void)
{
    int    next;
    Vector lightOrigin;
    Entity *light;
    float  r, g, b, radius;

    if (!IsActive() || IsEdit() || !m_bRunActive || m_scenario.category != PRACTICE_CAT_ANGLES) {
        level.m_vObjectiveLocation = vec_zero;
        if (m_waypointLight) {
            m_waypointLight->PostEvent(EV_Remove, 0);
            m_waypointLight = NULL;
        }
        return;
    }
    // m_iCurrentGate is the last gate passed (0 = start). Next marker is waypoints[gate].
    next = m_iCurrentGate;
    if (next < 0 || next >= (int)m_scenario.waypoints.size()) {
        level.m_vObjectiveLocation = vec_zero;
        if (m_waypointLight) {
            m_waypointLight->PostEvent(EV_Remove, 0);
            m_waypointLight = NULL;
        }
        return;
    }

    level.m_vObjectiveLocation = Vector(m_scenario.waypoints[next].origin.v);
    lightOrigin                = level.m_vObjectiveLocation;
    lightOrigin[2] += 24.0f;

    light = m_waypointLight;
    if (!light) {
        if (Practice_FreeEntitySlots() <= PRACTICE_ENTITY_RESERVE) {
            return;
        }
        light = new Entity;
        light->setSolidType(SOLID_NOT);
        light->setMoveType(MOVETYPE_NONE);
        light->edict->s.renderfx |= RF_DONTDRAW;
        // Fixed in Omaha: modelindex 0 entities are skipped unless SVF_SENDPVS (sv_snapshot).
        light->edict->r.svFlags &= ~SVF_NOCLIENT;
        light->edict->r.svFlags |= SVF_SENDPVS;
        m_waypointLight = light;
    }
    light->setOrigin(lightOrigin);
    r      = PRACTICE_WAYPOINT_LIGHT_R;
    g      = PRACTICE_WAYPOINT_LIGHT_G;
    b      = PRACTICE_WAYPOINT_LIGHT_B;
    radius = PRACTICE_WAYPOINT_LIGHT_RADIUS;
    G_SetConstantLight(&light->edict->s.constantLight, &r, &g, &b, &radius);
}

void PracticeManager::UpdateGates(Player *human)
{
    Vector delta;
    int    nextGate;

    if (!human || IsEdit() || !m_bRunActive || m_scenario.category != PRACTICE_CAT_ANGLES) {
        SyncWaypointObjective();
        return;
    }
    if (human->IsDead() || human->IsSpectator()) {
        SyncWaypointObjective();
        return;
    }

    if (m_iCurrentGate < 0) {
        m_iCurrentGate = 0;
        m_bBotsPlaced  = false;
        SyncWaypointObjective();
        return;
    }

    // Changed in Omaha: consume every gate currently in radius (stacked WPs advance in one frame).
    while (true) {
        nextGate = m_iCurrentGate + 1;
        if (nextGate >= (int)m_scenario.gates.size()) {
            break;
        }

        delta = human->origin - Vector(m_scenario.gates[nextGate].v);
        if (delta.length() >= PRACTICE_GATE_RADIUS) {
            break;
        }

        m_iCurrentGate = nextGate;
        m_bBotsPlaced  = false;
    }
    SyncWaypointObjective();
}

void PracticeManager::Frame(void)
{
    std::vector<PracticeBotController *> bots;
    Player                              *human;
    int                                  i;

    if (!IsActive()) {
        return;
    }

    if (!m_bLoaded) {
        LoadScenario();
        SyncWaypointObjective();
    }
    if (m_bCueCacheDirty) {
        RebuildCueCache();
    }

    SyncClients();
    if (m_scenario.category == PRACTICE_CAT_ANGLES) {
        EnforceAnglesSolo();
    }

    // Leave the human in spectator so they can pick team + weapon via stock menus.
    human = GetHuman();
    UpdateGates(human);
    EnsureBots();

    // Changed in Omaha: per-human upkeep + stock-client presentation.
    for (i = 0; i < game.maxclients && i < MAX_CLIENTS; i++) {
        Player *player = m_clients[i].player;

        if (!player) {
            continue;
        }
        if (!player->IsDead() && !player->IsSpectator()) {
            if (m_scenario.category == PRACTICE_CAT_ANGLES) {
                StripGrenades(player);
            } else if (m_scenario.category == PRACTICE_CAT_NADES) {
                RefillGrenades(player);
            }
        }
        if (m_scenario.category == PRACTICE_CAT_NADES) {
            UpdateNadeTracking(i);
            UpdateChaseCam(i);
        }
        UpdateClientMarkers(i);
        UpdateClientHud(i);
    }
    UpdateGlobalMarkers();
    UpdatePreviews();

    // Added in Omaha: 30-minute timed angles runs fail into spectator with no save.
    if (m_scenario.category == PRACTICE_CAT_ANGLES && !IsEdit() && m_bRunActive && human && !human->IsDead()
        && !human->IsSpectator() && level.inttime >= m_iRunStartTime + PRACTICE_TIME_LIMIT_MS) {
        EndAnglesRun(false);
    }

    Practice_CollectControllers(bots);
    for (i = 0; i < (int)bots.size(); i++) {
        Player *bot = bots[i]->getControlledEntity();

        bots[i]->SetAllowFire(!bots[i]->IsDummy());
        if (!bot || bot->IsDead() || bot->IsSpectator()) {
            continue;
        }
    }
}

void PracticeManager::AddSpot(Player *player)
{
    PracticeNode node;

    if ((int)m_scenario.nodes.size() >= PRACTICE_MAX_NODES) {
        EditFeedback(player, "Node limit reached (%i)", PRACTICE_MAX_NODES);
        return;
    }

    node.type = PRACTICE_NODE_SPOT;
    VectorCopy(player->origin, node.origin.v);
    VectorCopy(player->GetViewAngles(), node.angles.v);
    // Crouch viewheight is 48; the old <40 check never recorded crouch.
    if (player->client
        && ((player->client->ps.pm_flags & PMF_DUCKED) || player->viewheight <= (int)CROUCH_VIEWHEIGHT)) {
        node.stance = PRACTICE_STANCE_CROUCH;
    } else {
        node.stance = PRACTICE_STANCE_STAND;
    }
    if (player->client && player->client->ps.fLeanAngle > 5.0f) {
        node.lean = PRACTICE_LEAN_RIGHT;
    } else if (player->client && player->client->ps.fLeanAngle < -5.0f) {
        node.lean = PRACTICE_LEAN_LEFT;
    }
    m_scenario.nodes.push_back(node);
    m_scenario.RebuildDerived();
    m_bBotsPlaced = false;
    EditFeedback(player, "Spot %i added", (int)m_scenario.spots.size());
    Autosave();
}

void PracticeManager::AddWaypoint(Player *player)
{
    PracticeNode node;

    if ((int)m_scenario.nodes.size() >= PRACTICE_MAX_NODES) {
        EditFeedback(player, "Node limit reached (%i)", PRACTICE_MAX_NODES);
        return;
    }

    node.type = PRACTICE_NODE_WAYPOINT;
    VectorCopy(player->origin, node.origin.v);
    m_scenario.nodes.push_back(node);
    m_scenario.RebuildDerived();
    EditFeedback(player, "Waypoint %i added", (int)m_scenario.waypoints.size());
    Autosave();
}

void PracticeManager::SetStart(Player *player)
{
    PracticeNode node;
    int          i;
    bool         replaced = false;

    node.type = PRACTICE_NODE_START;
    VectorCopy(player->origin, node.origin.v);
    VectorCopy(player->GetViewAngles(), node.angles.v);

    for (i = 0; i < (int)m_scenario.nodes.size(); i++) {
        if (m_scenario.nodes[i].type == PRACTICE_NODE_START) {
            m_scenario.nodes[i] = node;
            replaced            = true;
            break;
        }
    }
    if (!replaced) {
        m_scenario.nodes.insert(m_scenario.nodes.begin(), node);
    }
    m_scenario.RebuildDerived();
    EditFeedback(player, "Start set");
    Autosave();
}

void PracticeManager::SaveLastThrow(Player *player)
{
    PracticeThrow   th;
    PracticeClient *pc = ClientFor(player);

    if (!pc || !pc->lastThrowTime || !pc->lastNadeExploded) {
        EditFeedback(player, "Throw a grenade first");
        return;
    }
    if ((int)m_scenario.throwsList.size() >= PRACTICE_MAX_THROWS) {
        EditFeedback(player, "Throw limit reached (%i)", PRACTICE_MAX_THROWS);
        return;
    }
    VectorCopy(pc->lastThrowStand, th.stand.v);
    VectorCopy(pc->lastThrowAim, th.aim.v);
    VectorCopy(pc->lastThrowEye, th.eye.v);
    VectorCopy(pc->lastNadeOrigin, th.target.v);
    th.radius = 96.0f;
    th.charge = pc->lastNadeCharge;
    th.stance = pc->lastThrowStance;
    th.lean   = pc->lastThrowLean;
    th.hasEye = true;
    th.dummy  = true;
    m_scenario.throwsList.push_back(th);
    pc->lastThrowTime = 0;
    m_bBotsPlaced     = false;
    m_bCueCacheDirty  = true;
    EditFeedback(player, "Throw %i saved", (int)m_scenario.throwsList.size());
    Autosave();
}

// Added in Omaha: rethrow last nade from its original spawn without chase cam.
void PracticeManager::ReplayLastThrow(Player *player)
{
    Vector          dir;
    PracticeClient *pc                = ClientFor(player);
    const int       kReplayCooldownMs = 750;

    if (!pc || player->IsDead() || player->IsSpectator()) {
        return;
    }
    if (!pc->lastNadeModel[0] || !pc->lastThrowTime) {
        EditFeedback(player, "Throw a grenade first");
        return;
    }
    // Added in Omaha: one live practice nade at a time — no replay spam mid-flight.
    if (pc->lastNadeEnt >= 0) {
        EditFeedback(player, "Wait for the grenade");
        return;
    }
    if (level.inttime < pc->nextReplayTime) {
        EditFeedback(player, "Wait before replaying");
        return;
    }

    // Replay uses weapon fire angles; cue storage is camera look.
    pc->lastThrowFireAim.AngleVectors(&dir);

    m_bNadeReplay = true;
    ProjectileAttack(pc->lastNadeStart, dir, player, str(pc->lastNadeModel), pc->lastNadeCharge, 0, NULL);
    m_bNadeReplay      = false;
    pc->nextReplayTime = level.inttime + kReplayCooldownMs;
    EditFeedback(player, "Replaying throw");
}

void PracticeManager::EditPrimaryFire(Player *player, const Vector& eye, const Vector& end)
{
    (void)eye;
    (void)end;

    // Changed in Omaha: nades primary saves last throw; angles primary adds a spot.
    if (m_scenario.category == PRACTICE_CAT_NADES) {
        SaveLastThrow(player);
        return;
    }

    if (m_scenario.category == PRACTICE_CAT_ANGLES) {
        AddSpot(player);
    }
}

// Added in Omaha: view ray for angles-edit aim HUD (matches FilterEditorInput length + solid clamp).
void PracticeManager::AimPickRay(Player *player, Vector& eye, Vector& end) const
{
    Vector  viewAng;
    Vector  forward;
    vec3_t  viewEye;
    float   viewh;
    float   leanAng;
    trace_t tr;

    eye = vec_zero;
    end = vec_zero;
    if (!player || !player->client) {
        return;
    }

    viewAng = player->GetViewAngles();
    viewh   = (float)player->client->ps.viewheight;
    leanAng = player->client->ps.fLeanAngle;
    Practice_CameraEye(viewEye, player->origin, viewAng, viewh, leanAng);
    eye = viewEye;
    viewAng.AngleVectors(&forward);
    end = eye + forward * PRACTICE_PICK_RANGE;
    tr  = G_Trace(eye, vec_zero, vec_zero, end, player, CONTENTS_SOLID, qfalse, "PracticeManager::AimPickRay");
    end = tr.endpos;
}

// Added in Omaha: every angles node the aim ray hits (stacked spot+waypoint); selected = alt-fire remove.
void PracticeManager::CollectAimedAnglesNodes(
    const Vector&                   eye,
    const Vector&                   end,
    std::vector<PracticeAimedNode>& out,
    int                            *outSelected
) const
{
    struct Candidate {
        PracticePickTarget cap;
        PracticeAimedNode  node;
    };
    std::vector<Candidate> cands;
    int                    i;

    out.clear();
    if (outSelected) {
        *outSelected = -1;
    }
    if (m_scenario.category != PRACTICE_CAT_ANGLES) {
        return;
    }

    if (m_scenario.hasStart) {
        Candidate c;

        c.cap.bottom = Vector(m_scenario.startOrigin.v);
        c.cap.top    = c.cap.bottom + Vector(0, 0, 72.0f);
        c.cap.radius = 20.0f;
        c.node.kind  = PRACTICE_AIMED_START;
        c.node.index = -1;
        c.node.nodeId = 0;
        c.node.s      = 0.0f;
        Q_strncpyz(c.node.pose, "Start", sizeof(c.node.pose));
        cands.push_back(c);
    }
    for (i = 0; i < (int)m_scenario.waypoints.size(); i++) {
        Candidate c;

        c.cap.bottom  = Vector(m_scenario.waypoints[i].origin.v);
        c.cap.top     = c.cap.bottom + Vector(0, 0, 96.0f);
        c.cap.radius  = 28.0f;
        c.node.kind   = PRACTICE_AIMED_WAYPOINT;
        c.node.index  = i;
        c.node.nodeId = m_scenario.waypoints[i].nodeId;
        c.node.s      = 0.0f;
        Q_strncpyz(c.node.pose, "Waypoint", sizeof(c.node.pose));
        cands.push_back(c);
    }
    for (i = 0; i < (int)m_scenario.spots.size(); i++) {
        Candidate c;
        float     height;

        // Ghosts stay on placement origin; lean is bone-only on the preview.
        height        = (m_scenario.spots[i].stance == PRACTICE_STANCE_CROUCH) ? 48.0f : 72.0f;
        c.cap.bottom  = Vector(m_scenario.spots[i].origin.v);
        c.cap.top     = c.cap.bottom + Vector(0, 0, height);
        c.cap.radius  = 20.0f;
        c.node.kind   = PRACTICE_AIMED_SPOT;
        c.node.index  = i;
        c.node.nodeId = m_scenario.spots[i].nodeId;
        c.node.s      = 0.0f;
        Practice_FormatSpotPose(m_scenario.spots[i].stance, m_scenario.spots[i].lean, c.node.pose, sizeof(c.node.pose));
        cands.push_back(c);
    }

    for (i = 0; i < (int)cands.size(); i++) {
        float s;
        float dist;

        // Standing inside a marker (e.g. the spot just placed) must not select it.
        if (Practice_SegmentDistance(eye, eye, cands[i].cap.bottom, cands[i].cap.top, NULL) <= cands[i].cap.radius) {
            continue;
        }
        dist = Practice_SegmentDistance(eye, end, cands[i].cap.bottom, cands[i].cap.top, &s);
        if (dist <= cands[i].cap.radius) {
            cands[i].node.s = s;
            out.push_back(cands[i].node);
        }
    }

    std::sort(out.begin(), out.end(), [](const PracticeAimedNode& a, const PracticeAimedNode& b) { return a.s < b.s; });

    // Remove target matches legacy Practice_Pick: nearest along the ray among spots/waypoints only.
    if (outSelected) {
        float bestS = 2.0f;
        int   best  = -1;

        for (i = 0; i < (int)out.size(); i++) {
            if (out[i].kind == PRACTICE_AIMED_START) {
                continue;
            }
            if (out[i].s < bestS) {
                bestS = out[i].s;
                best  = i;
            }
        }
        *outSelected = best;
    }
}

void PracticeManager::EditAltFire(Player *player, const Vector& eye, const Vector& end)
{
    if (m_scenario.category == PRACTICE_CAT_NADES) {
        // Changed in Omaha: remove the highlighted (aimed) cue — same pick as snap and the server markers.
        int removeAt;

        (void)eye;
        (void)end;
        removeAt = FindAimedThrow(player);
        if (removeAt >= 0) {
            m_scenario.throwsList.erase(m_scenario.throwsList.begin() + removeAt);
            m_bBotsPlaced    = false;
            m_bCueCacheDirty = true;
            EditFeedback(player, "Throw removed (%i left)", (int)m_scenario.throwsList.size());
            Autosave();
        } else {
            EditFeedback(player, "Stand in ring, aim at the cue, alt-fire to remove");
        }
        return;
    }

    // Changed in Omaha: primary alt-fire removes the nearest aimed spot or waypoint (same pick as HUD ">").
    {
        std::vector<PracticeAimedNode> hits;
        int                            selected = -1;

        CollectAimedAnglesNodes(eye, end, hits, &selected);
        if (selected >= 0 && selected < (int)hits.size()) {
            const PracticeAimedNode& hit = hits[selected];

            if (hit.kind == PRACTICE_AIMED_SPOT && m_scenario.RemoveSpotAt(hit.index)) {
                m_bBotsPlaced = false;
                EditFeedback(player, "Spot removed (%i left)", (int)m_scenario.spots.size());
                Autosave();
                return;
            }
            if (hit.kind == PRACTICE_AIMED_WAYPOINT && m_scenario.RemoveWaypointAt(hit.index)) {
                m_bBotsPlaced = false;
                EditFeedback(player, "Waypoint removed (%i left)", (int)m_scenario.waypoints.size());
                Autosave();
                return;
            }
        }
        EditFeedback(player, "Aim at a ghost or waypoint and alt-fire to remove");
    }
}

// Added in Omaha: world-only cue positions (shared by marker drawing, snap and remove).
void PracticeManager::RebuildCueCache(void)
{
    // Stand hex rings live in the global beam set — refresh them with the cue cache.
    m_iNextGlobalMarkers = 0;
    int i;

    m_cueCache.resize(m_scenario.throwsList.size());
    for (i = 0; i < (int)m_scenario.throwsList.size(); i++) {
        const PracticeThrow& th = m_scenario.throwsList[i];
        vec3_t               eye;
        vec3_t               fwd;
        vec3_t               end;
        trace_t              tr;
        float                aimDist;

        Practice_CameraEye(
            eye, th.stand.v, th.aim.v, Practice_ViewHeightForStance(th.stance), Practice_LeanAngleForLean(th.lean)
        );
        AngleVectors(th.aim.v, fwd, NULL, NULL);
        VectorMA(eye, PRACTICE_AIM_CUE_DIST, fwd, end);
        tr = G_Trace(Vector(eye), vec_zero, vec_zero, Vector(end), NULL, MASK_SOLID, qfalse, "PracticeCueCache");
        aimDist = PRACTICE_AIM_CUE_DIST * tr.fraction;
        if (aimDist > 12.0f) {
            aimDist -= 4.0f;
        }
        if (aimDist < 24.0f) {
            aimDist = 24.0f;
        }
        m_cueCache[i].pos     = Vector(eye) + Vector(fwd) * aimDist;
        m_cueCache[i].forward = Vector(fwd);
    }
    m_bCueCacheDirty = false;
}

// In-ring throw whose cue sits inside the ~6° crosshair cone, or -1.
int PracticeManager::FindAimedThrow(Player *player)
{
    int    i;
    int    best      = -1;
    float  bestScore = PRACTICE_AIM_SELECT_MIN;
    Vector viewAng;
    Vector forward;
    vec3_t viewEye;
    float  viewh;
    float  leanAng;

    if (!player || !player->client || m_scenario.category != PRACTICE_CAT_NADES) {
        return -1;
    }
    if (m_bCueCacheDirty || m_cueCache.size() != m_scenario.throwsList.size()) {
        RebuildCueCache();
    }

    viewAng = player->GetViewAngles();
    viewh   = (float)player->client->ps.viewheight;
    leanAng = player->client->ps.fLeanAngle;
    Practice_CameraEye(viewEye, player->origin, viewAng, viewh, leanAng);
    viewAng.AngleVectors(&forward);

    for (i = 0; i < (int)m_scenario.throwsList.size(); i++) {
        Vector toCue;
        float  dist;
        float  score;

        if (!Practice_InThrowStand(player->origin, m_scenario.throwsList[i].stand.v, PRACTICE_STAND_R)) {
            continue;
        }
        toCue = m_cueCache[i].pos - Vector(viewEye);
        dist  = toCue.normalize();
        if (dist < 1.0f) {
            continue;
        }
        score = DotProduct(toCue, forward);
        if (score > bestScore) {
            bestScore = score;
            best      = i;
        }
    }
    return best;
}

bool PracticeManager::SnapToThrowStand(Player *player)
{
    const int best = FindAimedThrow(player);

    if (best < 0) {
        return false;
    }

    TeleportToSpot(player, m_scenario.throwsList[best].stand.v, m_scenario.throwsList[best].aim.v);
    ForceViewAngles(player, m_scenario.throwsList[best].aim.v);
    EditFeedback(player, "Snapped to throw %i", best + 1);
    return true;
}

void PracticeManager::FilterEditorInput(Player *player, usercmd_t *ucmd, const usereyes_t *eyes)
{
    Weapon *weap;
    int     pressed;
    int     wclass;
    Vector  eye;
    Vector  forward;
    Vector  end;
    trace_t         trace;
    bool            pistol;
    PracticeClient *pc;

    if (!IsActive() || !player || !ucmd || !eyes || G_IsBot(player->edict)) {
        return;
    }
    pc = ClientFor(player);
    if (!pc) {
        return;
    }

    pressed         = ucmd->buttons & ~pc->editButtons;
    pc->editButtons = ucmd->buttons;

    if (player->IsDead() || player->IsSpectator()) {
        return;
    }

    weap = player->GetActiveWeapon(WEAPON_MAIN);
    if (!weap) {
        return;
    }
    wclass = weap->GetWeaponClass();
    if (wclass & WEAPON_CLASS_THROWABLE) {
        return;
    }

    pistol = (wclass & WEAPON_CLASS_PISTOL) != 0;

    // Changed in Omaha: nade play+edit — pistol left replays, pistol right snaps to aimed cue.
    if (m_scenario.category == PRACTICE_CAT_NADES && pistol) {
        ucmd->buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        if (pressed & BUTTON_ATTACKRIGHT) {
            if (!SnapToThrowStand(player)) {
                EditFeedback(player, "Stand in a ring and aim at a cue to snap");
            }
        } else if (pressed & BUTTON_ATTACKLEFT) {
            ReplayLastThrow(player);
        }
        return;
    }

    if (!IsEdit()) {
        return;
    }

    ucmd->buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
    if (!(pressed & (BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT))) {
        return;
    }

    eye = player->origin + Vector(eyes->ofs[0], eyes->ofs[1], eyes->ofs[2]);
    Vector(eyes->angles[0], eyes->angles[1], 0).AngleVectors(&forward);
    end   = eye + forward * PRACTICE_PICK_RANGE;
    trace = G_Trace(eye, vec_zero, vec_zero, end, player, CONTENTS_SOLID, qfalse, "PracticeManager::FilterEditorInput");
    end   = trace.endpos;

    // Changed in Omaha: nade edit — rifle saves/removes; pistol handled above for play+edit.
    if (m_scenario.category == PRACTICE_CAT_NADES) {
        if (pressed & BUTTON_ATTACKLEFT) {
            EditPrimaryFire(player, eye, end);
        } else {
            EditAltFire(player, eye, end);
        }
        return;
    }

    if (pressed & BUTTON_ATTACKLEFT) {
        if (pistol) {
            AddWaypoint(player);
        } else {
            EditPrimaryFire(player, eye, end);
        }
    } else if (pistol) {
        SetStart(player);
    } else {
        EditAltFire(player, eye, end);
    }
}

void PracticeManager::EventSpotAdd(Event *ev)
{
    Player *player = HumanFromEvent(ev);

    if (!MayEdit(player) || m_scenario.category != PRACTICE_CAT_ANGLES) {
        return;
    }
    AddSpot(player);
}

void PracticeManager::EventSpotUndo(Event *ev)
{
    Player *player = HumanFromEvent(ev);

    if (!MayEdit(player) || m_scenario.spots.empty()) {
        return;
    }
    if (!m_scenario.PopLastSpot()) {
        return;
    }
    m_bBotsPlaced = false;
    EditFeedback(player, "Spot removed (%i left)", (int)m_scenario.spots.size());
    Autosave();
}

void PracticeManager::EventStartSet(Event *ev)
{
    Player *player = HumanFromEvent(ev);

    if (!MayEdit(player) || m_scenario.category != PRACTICE_CAT_ANGLES) {
        return;
    }
    SetStart(player);
}

void PracticeManager::EventWaypointAdd(Event *ev)
{
    Player *player = HumanFromEvent(ev);

    if (!MayEdit(player) || m_scenario.category != PRACTICE_CAT_ANGLES) {
        return;
    }
    AddWaypoint(player);
}

void PracticeManager::EventThrowSave(Event *ev)
{
    Player *player = HumanFromEvent(ev);

    if (!MayEdit(player) || m_scenario.category != PRACTICE_CAT_NADES) {
        return;
    }
    SaveLastThrow(player);
}

void PracticeManager::EventWeapon(Event *ev)
{
    Player     *player = HumanFromEvent(ev);
    const char *name;
    static const char *const kPrimaries[] = {
        "rifle", "sniper", "smg", "mg", "heavy", "shotgun", NULL
    };
    int i;

    if (!IsActive() || !player || !player->client || ev->NumArgs() < 1) {
        return;
    }
    name = ev->GetString(1);
    for (i = 0; kPrimaries[i]; i++) {
        if (!Q_stricmp(name, kPrimaries[i])) {
            break;
        }
    }
    if (!kPrimaries[i]) {
        return;
    }
    Q_strncpyz(player->client->pers.dm_primary, kPrimaries[i], sizeof(player->client->pers.dm_primary));
    player->EquipWeapons();
    if (m_scenario.category == PRACTICE_CAT_ANGLES) {
        StripGrenades(player);
    }
}

void PracticeManager::EventSave(Event *ev)
{
    Player *player = HumanFromEvent(ev);

    if (!MayEdit(player)) {
        return;
    }
    Autosave();
}

void PracticeManager::EventMeta(Event *ev)
{
    Player *player = HumanFromEvent(ev);
    char    name[PRACTICE_META_MAX_CHARS + 1];
    char    author[PRACTICE_META_MAX_CHARS + 1];
    int     maxAlive;

    if (!MayEdit(player) || ev->NumArgs() < 1) {
        return;
    }
    Practice_SanitizeMetaField(ev->GetString(1), name, sizeof(name));
    Q_strncpyz(m_scenario.name, name, sizeof(m_scenario.name));
    if (ev->NumArgs() >= 2) {
        Practice_SanitizeMetaField(ev->GetString(2), author, sizeof(author));
        Q_strncpyz(m_scenario.author, author, sizeof(m_scenario.author));
    }
    if (ev->NumArgs() >= 3) {
        maxAlive = ev->GetInteger(3);
        if (maxAlive < 1) {
            maxAlive = 1;
        } else if (maxAlive > PRACTICE_MAX_BOTS) {
            maxAlive = PRACTICE_MAX_BOTS;
        }
        m_scenario.maxAlive = maxAlive;
    }
    Autosave();
}

void PracticeManager::EventOrderBegin(Event *ev)
{
    Player *player = HumanFromEvent(ev);
    int     count;

    if (!MayEdit(player) || ev->NumArgs() < 1) {
        return;
    }
    count = ev->GetInteger(1);
    ClearOrderDraft();
    if (count < 0 || count > PRACTICE_MAX_NODES) {
        EditFeedback(player, "Node order rejected (bad count)");
        return;
    }
    m_iOrderExpected = count;
    m_orderDraft.reserve((size_t)count);
}

void PracticeManager::EventOrderAppend(Event *ev)
{
    Player *player = HumanFromEvent(ev);
    int     i;

    if (!MayEdit(player) || m_iOrderExpected <= 0) {
        return;
    }
    for (i = 1; i <= ev->NumArgs(); i++) {
        if ((int)m_orderDraft.size() >= m_iOrderExpected) {
            EditFeedback(player, "Node order rejected (too many ids)");
            ClearOrderDraft();
            return;
        }
        m_orderDraft.push_back(ev->GetInteger(i));
    }
}

void PracticeManager::EventOrderCommit(Event *ev)
{
    Player *player = HumanFromEvent(ev);

    if (!MayEdit(player)) {
        return;
    }
    if (m_iOrderExpected <= 0 || (int)m_orderDraft.size() != m_iOrderExpected) {
        EditFeedback(player, "Node order rejected (incomplete)");
        ClearOrderDraft();
        return;
    }
    if (!m_scenario.ApplyNodeOrder(m_orderDraft)) {
        EditFeedback(player, "Node order rejected (scenario changed?)");
        ClearOrderDraft();
        return;
    }
    ClearOrderDraft();
    m_bBotsPlaced = false;
    Autosave();
}

void PracticeManager::Init(void)
{
    m_bLoaded             = false;
    m_bLoadOk             = false;
    m_bMapMismatch        = false;
    m_bComplete           = false;
    m_bRunActive          = false;
    m_bBotsPlaced         = false;
    m_bDeathRewind        = false;
    m_bNadeReplay         = false;
    m_bCueCacheDirty      = true;
    m_iNextSpot           = 0;
    m_iCurrentGate        = -1;
    m_iDeathSpawnGate     = 0;
    m_iRunStartTime       = 0;
    m_iCompleteTime       = 0;
    m_iAnglesOwner        = -1;
    m_iNextGlobalMarkers  = 0;
    m_iNextPreviews       = 0;
    ClearOrderDraft();
    ForgetPresentation();
    G_CreateMaster("practice", this);
}

void PracticeManager::Shutdown(void)
{
    m_bLoaded  = false;
    m_bLoadOk  = false;
    ClearOrderDraft();
    m_scenario.Clear();
    ForgetPresentation();
    level.m_vObjectiveLocation = vec_zero;
}

// Level teardown frees the entities itself — just drop handles and per-client state.
void PracticeManager::ForgetPresentation(void)
{
    int i;

    for (i = 0; i < MAX_CLIENTS; i++) {
        m_clients[i].Reset();
    }
    m_globalBeams.Forget();
    m_waypointLight = NULL;
    m_previews.clear();
    m_previewKeys.clear();
    m_previewIds.clear();
    m_cueCache.clear();
}

// Added in Omaha: bind client slots to the current human entities (reset on (re)connect).
void PracticeManager::SyncClients(void)
{
    int i;

    for (i = 0; i < MAX_CLIENTS; i++) {
        Player *player = NULL;

        if (i < game.maxclients && Practice_IsHumanEntity(&g_entities[i])) {
            player = static_cast<Player *>(g_entities[i].entity);
            if (Practice_ControllerFor(player)) {
                player = NULL;
            }
        }
        if ((Player *)m_clients[i].player != player || (!player && m_clients[i].camera)) {
            ResetClient(i);
            m_clients[i].player = player;
            m_clients[i].beams.SetSingleClient(i);
        }
    }

    if (m_iAnglesOwner >= 0 && !m_clients[m_iAnglesOwner].player) {
        m_iAnglesOwner = -1;
    }
}

void PracticeManager::ResetClient(int slot)
{
    PracticeClient *pc = &m_clients[slot];

    StopChaseCam(slot, false);
    pc->beams.Clear();
    pc->Reset();
}

// Added in Omaha: angles mode is single-player — other humans stay spectating.
void PracticeManager::EnforceAnglesSolo(void)
{
    int i;

    if (m_iAnglesOwner < 0) {
        for (i = 0; i < game.maxclients && i < MAX_CLIENTS; i++) {
            Player *player = m_clients[i].player;
            if (player && !player->IsSpectator()) {
                m_iAnglesOwner = i;
                break;
            }
        }
    }

    for (i = 0; i < game.maxclients && i < MAX_CLIENTS; i++) {
        Player *player = m_clients[i].player;

        if (!player || i == m_iAnglesOwner || player->IsSpectator()) {
            continue;
        }
        player->SetTeam(TEAM_SPECTATOR);
        if (level.inttime >= m_clients[i].nextSoloNotice) {
            m_clients[i].nextSoloNotice = level.inttime + 3000;
            EditFeedback(player, "Angles practice is single-player");
        }
    }
}

float PracticeManager::ChargeFill(Player *player, float charge) const
{
    float   minT = 0.6f;
    float   maxT = 4.0f;
    float   fill;
    Weapon *weap = player ? player->GetActiveWeapon(WEAPON_MAIN) : NULL;

    // Same mapping as the Omaha charge bar: cook seconds between min and max charge time.
    if (weap && (weap->GetWeaponClass() & WEAPON_CLASS_GRENADE)) {
        const float mn = weap->GetMinChargeTime(FIRE_PRIMARY);
        const float mx = weap->GetMaxChargeTime(FIRE_PRIMARY);
        if (mx > mn && mx > 0.0f) {
            minT = mn;
            maxT = mx;
        }
    }
    fill = (charge * maxT - minT) / (maxT - minT);
    return Practice_Clamp01(fill);
}

void PracticeManager::FormatThrowLine(Player *player, const PracticeThrow& th, char *out, size_t outSize) const
{
    char pose[48];

    Practice_FormatThrowPose(th, pose, sizeof(pose));
    Com_sprintf(out, outSize, "%s - %i%%", pose, (int)(ChargeFill(player, th.charge) * 100.0f + 0.5f));
}

void PracticeManager::UpdateClientMarkers(int slot)
{
    PracticeClient               *pc     = &m_clients[slot];
    Player                       *player = pc->player;
    std::vector<PracticeBeamSpec> specs;
    int                           selected = -1;
    int                           i;

    if (!player || m_scenario.category != PRACTICE_CAT_NADES || player->IsDead() || player->IsSpectator()) {
        pc->activeThrow = -1;
        pc->lockedThrow = -1;
        pc->beams.Apply(specs);
        return;
    }
    if (m_bCueCacheDirty || m_cueCache.size() != m_scenario.throwsList.size()) {
        RebuildCueCache();
    }

    // Keep the throw selected at release while the chase cam looks away from the cue.
    if (pc->camera && pc->lockedThrow >= 0 && pc->lockedThrow < (int)m_scenario.throwsList.size()) {
        selected = pc->lockedThrow;
    } else {
        selected        = FindAimedThrow(player);
        pc->lockedThrow = selected;
    }
    pc->activeThrow = selected;

    // Stand rings are shared world entities (UpdateGlobalMarkers); client set is cues + landing only.
    for (i = 0; i < (int)m_scenario.throwsList.size(); i++) {
        if (i != selected && !Practice_InThrowStand(player->origin, m_scenario.throwsList[i].stand.v, PRACTICE_STAND_R)) {
            continue;
        }
        Practice_AddCue(specs, m_cueCache[i], i == selected);
    }

    if (selected >= 0) {
        const PracticeThrow& th = m_scenario.throwsList[selected];
        const Vector         target(th.target.v);
        const Vector         landCol(1.0f, 0.35f, 0.1f);
        float                radius = th.radius;

        if (radius < 32.0f) {
            radius = 32.0f;
        } else if (radius > 96.0f) {
            radius = 96.0f;
        }
        Practice_AddRing(specs, target, radius, 12, landCol, 2.0f);
        Practice_AddPillar(specs, target, 72.0f, landCol, 3.0f, 150.0f);
    }

    pc->beams.Apply(specs);

    // Omaha host only: charge-bar release tick.
    if (IsOmahaHost(player)) {
        const float release = selected >= 0 ? m_scenario.throwsList[selected].charge : -1.0f;
        if (release != pc->releaseSent) {
            pc->releaseSent = release;
            SendOmahaCommand("omp_release %.3f", release);
        }
    }
}

void PracticeManager::UpdateGlobalMarkers(void)
{
    std::vector<PracticeBeamSpec> specs;
    int                           i;

    if (level.inttime < m_iNextGlobalMarkers) {
        return;
    }
    m_iNextGlobalMarkers = level.inttime + 250;

    // Shared world beams (not SVF_SINGLECLIENT) — snapshot PVS decides who sees them.
    if (m_scenario.category == PRACTICE_CAT_NADES) {
        const Vector        ringCol(0.15f, 0.75f, 1.0f);
        std::vector<Vector> stands;

        for (i = 0; i < (int)m_scenario.throwsList.size(); i++) {
            const Vector stand(m_scenario.throwsList[i].stand.v);
            bool         dup = false;
            size_t       j;

            for (j = 0; j < stands.size(); j++) {
                if ((stands[j] - stand).length() < PRACTICE_STAND_DEDUP) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            stands.push_back(stand);
            Practice_AddRing(specs, stand, PRACTICE_STAND_R, PRACTICE_STAND_RING_SEGS, ringCol, 1.5f);
        }
    } else if (m_scenario.category == PRACTICE_CAT_ANGLES && IsEdit()) {
        // Changed in Omaha: edit-only hex rings like nade stands (no pillars, no dedupe).
        // Play mode uses the compass objective light only — no beams.
        const Vector wayCol(0.2f, 1.0f, 0.35f);
        const Vector startCol(1.0f, 1.0f, 1.0f);

        if (m_scenario.hasStart) {
            Practice_AddRing(
                specs, Vector(m_scenario.startOrigin.v), PRACTICE_STAND_R, PRACTICE_STAND_RING_SEGS, startCol, 1.5f
            );
        }
        for (i = 0; i < (int)m_scenario.waypoints.size(); i++) {
            Practice_AddRing(
                specs,
                Vector(m_scenario.waypoints[i].origin.v),
                PRACTICE_STAND_R,
                PRACTICE_STAND_RING_SEGS,
                wayCol,
                1.5f
            );
        }
    }

    m_globalBeams.Apply(specs);
}

// Added in Omaha: posed player-model previews for angles spots (edit only; stance, yaw, lean kept).
// Fixed in Omaha: bind each ScriptModel to a stable spotIndex so distance LOD never teleports models.
void PracticeManager::UpdatePreviews(void)
{
    struct PreviewWant {
        Vector origin;
        float  yaw;
        int    stance;
        int    lean;
        int    spotIndex; // -1 = start; else scenario.spots index
    };
    std::vector<PreviewWant>            want;
    std::vector<std::pair<float, int> > order;
    std::vector<char>                   keep;
    Player                             *human;
    size_t                              i;

    if (level.inttime < m_iNextPreviews) {
        return;
    }
    m_iNextPreviews = level.inttime + 500;

    if (m_scenario.category == PRACTICE_CAT_ANGLES && IsEdit()) {
        human = GetHuman();
        if (m_scenario.hasStart) {
            PreviewWant w;
            w.origin    = Vector(m_scenario.startOrigin.v);
            w.yaw       = m_scenario.startAngles.v[YAW];
            w.stance    = PRACTICE_STANCE_STAND;
            w.lean      = PRACTICE_LEAN_NONE;
            w.spotIndex = -1;
            want.push_back(w);
        }
        for (i = 0; i < m_scenario.spots.size(); i++) {
            const float dist = human ? (human->origin - Vector(m_scenario.spots[i].origin.v)).length() : 0.0f;
            order.push_back(std::make_pair(dist, (int)i));
        }
        std::sort(order.begin(), order.end());
        for (i = 0; i < order.size() && (int)want.size() < PRACTICE_MAX_PREVIEWS; i++) {
            const PracticeSpot& spot = m_scenario.spots[order[i].second];
            PreviewWant         w;

            w.origin    = Vector(spot.origin.v);
            w.yaw       = spot.angles.v[YAW];
            w.stance    = spot.stance;
            w.lean      = spot.lean;
            w.spotIndex = order[i].second;
            want.push_back(w);
        }
    }

    // Drop entities whose spot left the LOD set (stable ids — never reassign to another spot).
    keep.assign(m_previews.size(), 0);
    for (i = 0; i < want.size(); i++) {
        size_t j;
        for (j = 0; j < m_previewIds.size(); j++) {
            if (m_previewIds[j] == want[i].spotIndex) {
                keep[j] = 1;
                break;
            }
        }
    }
    for (i = m_previews.size(); i > 0; i--) {
        const size_t j = i - 1;
        if (keep[j]) {
            continue;
        }
        if (m_previews[j]) {
            m_previews[j]->PostEvent(EV_Remove, 0);
        }
        m_previews.erase(m_previews.begin() + (long)j);
        m_previewKeys.erase(m_previewKeys.begin() + (long)j);
        m_previewIds.erase(m_previewIds.begin() + (long)j);
    }

    for (i = 0; i < want.size(); i++) {
        static const char *const kStandAnims[]  = {"rifle_stand_idle", "unarmed_stand_idle", "idle", NULL};
        static const char *const kCrouchAnims[] = {"rifle_crouch_idle", "unarmed_crouch_idle", "crouch_idle", NULL};
        const PreviewWant&       w              = want[i];
        ScriptModel             *model;
        str                      key;
        int                      a;
        size_t                   slot;
        size_t                   j;

        slot = m_previews.size();
        for (j = 0; j < m_previewIds.size(); j++) {
            if (m_previewIds[j] == w.spotIndex) {
                slot = j;
                break;
            }
        }
        if (slot >= m_previews.size()) {
            if (Practice_FreeEntitySlots() <= PRACTICE_ENTITY_RESERVE) {
                break;
            }
            m_previews.push_back(SafePtr<ScriptModel>(NULL));
            m_previewKeys.push_back(std::string());
            m_previewIds.push_back(w.spotIndex);
            slot = m_previews.size() - 1;
        }

        model = m_previews[slot];
        if (!model) {
            if (Practice_FreeEntitySlots() <= PRACTICE_ENTITY_RESERVE) {
                break;
            }
            model = new ScriptModel;
            model->setModel(PRACTICE_PREVIEW_MODEL);
            model->setSolidType(SOLID_NOT);
            model->setMoveType(MOVETYPE_NONE);
            if (model->edict->tiki) {
                model->SetControllerTag(HEAD_TAG, gi.Tag_NumForName(model->edict->tiki, "Bip01 Head"));
                model->SetControllerTag(TORSO_TAG, gi.Tag_NumForName(model->edict->tiki, "Bip01 Spine2"));
                model->SetControllerTag(ARMS_TAG, gi.Tag_NumForName(model->edict->tiki, "Bip01 Spine1"));
                model->SetControllerTag(PELVIS_TAG, gi.Tag_NumForName(model->edict->tiki, "Bip01 Pelvis"));
            }
            // Place before show so the first snapshot is not at world origin.
            model->setOrigin(w.origin);
            model->setAngles(Vector(0, w.yaw, 0));
            model->showModel();
            m_previews[slot]    = model;
            m_previewKeys[slot] = std::string();
        }

        key = va("%.1f %.1f %.1f %.1f %i %i", w.origin[0], w.origin[1], w.origin[2], w.yaw, w.stance, w.lean);
        if (m_previewKeys[slot] == key.c_str()) {
            continue;
        }
        m_previewKeys[slot] = key.c_str();

        model->setOrigin(w.origin);
        model->setAngles(Vector(0, w.yaw, 0));
        if (model->edict->tiki) {
            const char *const *anims = (w.stance == PRACTICE_STANCE_CROUCH) ? kCrouchAnims : kStandAnims;
            const float        lean  = Practice_LeanAngleForLean(w.lean);

            for (a = 0; anims[a]; a++) {
                const int anim = gi.Anim_NumForName(model->edict->tiki, anims[a]);
                if (anim >= 0) {
                    model->NewAnim(anim);
                    break;
                }
            }
            // Mirror PmoveAdjustAngleSettings lean rolls (pelvis / head / torso / arms).
            model->SetControllerAngles(PELVIS_TAG, Vector(0, 0, lean * 0.8f));
            model->SetControllerAngles(HEAD_TAG, Vector(0, 0, lean * 0.6f));
            model->SetControllerAngles(TORSO_TAG, Vector(0, 0, lean * 0.2f * -0.1f));
            model->SetControllerAngles(ARMS_TAG, Vector(0, 0, lean * 0.2f * 1.1f));
        }
    }
}

// Added in Omaha: stock per-client huddraw text. Sends only what changed; CGM is unreliable,
// so used elements are re-sent round-robin every PRACTICE_HUD_REFRESH_MS.
void PracticeManager::HudSet(int slot, int elem, const char *text, int x, int y, int halign, int valign, const float *color)
{
    PracticeHudElem& e    = m_clients[slot].hud[elem];
    const int        info = PRACTICE_HUD_BASE + elem;
    const int        w    = (halign == HUD_ALIGN_X_LEFT) ? 0 : (int)strlen(text ? text : "") * 7;
    char             layout[96];

    if (!text) {
        text = "";
    }
    if (!e.used && !text[0]) {
        return;
    }

    Com_sprintf(
        layout,
        sizeof(layout),
        "%i %i %i %i %i %.2f %.2f %.2f",
        x,
        y,
        w,
        halign,
        valign,
        color[0],
        color[1],
        color[2]
    );
    if (!e.sent || e.layout != layout) {
        float rgb[3];

        rgb[0] = color[0];
        rgb[1] = color[1];
        rgb[2] = color[2];
        iHudDrawFont(slot, info, PRACTICE_HUD_FONT);
        iHudDrawAlign(slot, info, halign, valign);
        iHudDrawRect(slot, info, x, y, w, 14);
        iHudDrawVirtualSize(slot, info, 0);
        iHudDrawColor(slot, info, rgb);
        iHudDrawAlpha(slot, info, 1.0f);
        e.layout = layout;
        e.sent   = true;
        e.used   = true;
        e.text   = "\x01";
    }
    if (e.text != text) {
        iHudDrawString(slot, info, text);
        e.text = text;
    }
}

void PracticeManager::UpdateClientHud(int slot)
{
    static const float kTitleCol[3] = {1.0f, 0.85f, 0.2f};
    static const float kTextCol[3]  = {1.0f, 1.0f, 1.0f};
    static const float kAimCol[3]   = {1.0f, 0.95f, 0.35f};
    PracticeClient    *pc           = &m_clients[slot];
    Player            *player       = pc->player;
    const char        *title        = "";
    const char        *help[PRACTICE_HUD_HELP_MAX];
    char               status[64];
    char               aim[96];
    char               list[PRACTICE_HUD_LIST_MAX][80];
    int                helpCount = 0;
    int                listCount = 0;
    bool               inGame;
    int                i;

    if (!player) {
        return;
    }

    // Periodic resend of used elements (lost CGM packets would otherwise leave stale text).
    if (level.inttime >= pc->nextHudRefresh) {
        int n;

        pc->nextHudRefresh = level.inttime + PRACTICE_HUD_REFRESH_MS / (PRACTICE_HUD_COUNT / PRACTICE_HUD_REFRESH_BATCH);
        for (n = 0; n < PRACTICE_HUD_REFRESH_BATCH; n++) {
            PracticeHudElem& e = pc->hud[pc->hudRefreshIndex];
            if (e.used) {
                e.sent = false;
            }
            pc->hudRefreshIndex = (pc->hudRefreshIndex + 1) % PRACTICE_HUD_COUNT;
        }
    }

    inGame    = !player->IsDead() && !player->IsSpectator();
    status[0] = 0;
    aim[0]    = 0;

    if (IsAnglesBystander(player)) {
        title = "ANGLES PRACTICE";
        Q_strncpyz(status, "Single-player mode: spectating", sizeof(status));
    } else if (m_scenario.category == PRACTICE_CAT_NADES) {
        if (IsEdit()) {
            title             = "NADE EDITOR";
            help[helpCount++] = "GRENADE: throw (grenade alt stays cook)";
            help[helpCount++] = "RIFLE/SMG FIRE: save last throw";
            help[helpCount++] = "RIFLE/SMG ALT FIRE: aim cue to remove (stand in ring)";
            help[helpCount++] = "PISTOL FIRE: replay last throw";
            help[helpCount++] = "PISTOL ALT: snap to aimed cue";
            Com_sprintf(status, sizeof(status), "THROWS SAVED: %i", (int)m_scenario.throwsList.size());
        } else {
            title             = "NADE PRACTICE";
            help[helpCount++] = "PISTOL FIRE: replay last throw";
            help[helpCount++] = "PISTOL ALT: snap to aimed cue";
        }

        if (inGame && !pc->camera) {
            for (i = 0; i < (int)m_scenario.throwsList.size() && listCount < PRACTICE_HUD_LIST_MAX; i++) {
                char line[72];

                if (!Practice_InThrowStand(player->origin, m_scenario.throwsList[i].stand.v, PRACTICE_STAND_R)) {
                    continue;
                }
                FormatThrowLine(player, m_scenario.throwsList[i], line, sizeof(line));
                Com_sprintf(list[listCount], sizeof(list[listCount]), "%i. %s", listCount + 1, line);
                listCount++;
            }
            if (pc->activeThrow >= 0 && pc->activeThrow < (int)m_scenario.throwsList.size()) {
                FormatThrowLine(player, m_scenario.throwsList[pc->activeThrow], aim, sizeof(aim));
            }
        }
    } else if (m_scenario.category == PRACTICE_CAT_ANGLES && IsEdit()) {
        title             = "ANGLES EDITOR";
        help[helpCount++] = "PRIMARY FIRE: add spot";
        help[helpCount++] = "PRIMARY ALT FIRE: remove ghost or waypoint";
        help[helpCount++] = "PISTOL FIRE: add waypoint";
        help[helpCount++] = "PISTOL ALT FIRE: set start";
        if (IsOmahaHost(player)) {
            help[helpCount++] = "ESC > SCENARIO EDITOR: name, node order";
        }
        if (!m_scenario.hasStart) {
            Q_strncpyz(status, "NO START SET", sizeof(status));
        }

        // Added in Omaha: aimed node stack (stock iHudDraw) — ">" marks alt-fire remove target.
        if (inGame) {
            std::vector<PracticeAimedNode> hits;
            Vector                         eye;
            Vector                         end;
            int                            selected = -1;
            int                            n;

            AimPickRay(player, eye, end);
            CollectAimedAnglesNodes(eye, end, hits, &selected);
            for (n = 0; n < (int)hits.size() && listCount < PRACTICE_HUD_LIST_MAX; n++) {
                const PracticeAimedNode& hit  = hits[n];
                const char               mark = (n == selected) ? '>' : ' ';

                if (hit.kind == PRACTICE_AIMED_SPOT) {
                    Com_sprintf(
                        list[listCount], sizeof(list[listCount]), "%c %i Spot - %s", mark, hit.nodeId, hit.pose
                    );
                } else {
                    // Start pose is "Start"; waypoint pose is "Waypoint".
                    Com_sprintf(list[listCount], sizeof(list[listCount]), "%c %i %s", mark, hit.nodeId, hit.pose);
                }
                listCount++;
            }
        }
    }

    HudSet(slot, PRACTICE_HUD_TITLE, title, 12, 106, HUD_ALIGN_X_LEFT, HUD_ALIGN_Y_TOP, kTitleCol);
    for (i = 0; i < PRACTICE_HUD_HELP_MAX; i++) {
        HudSet(
            slot,
            PRACTICE_HUD_HELP + i,
            i < helpCount ? help[i] : "",
            12,
            120 + i * 12,
            HUD_ALIGN_X_LEFT,
            HUD_ALIGN_Y_TOP,
            kTextCol
        );
    }
    HudSet(slot, PRACTICE_HUD_STATUS, status, 12, 120 + helpCount * 12 + 4, HUD_ALIGN_X_LEFT, HUD_ALIGN_Y_TOP, kTextCol);

    // Changed in Omaha: aimed-cue pose + charge sits just above the crosshair, not on the cue.
    HudSet(slot, PRACTICE_HUD_AIM, aim, 0, -48, HUD_ALIGN_X_CENTER, HUD_ALIGN_Y_CENTER, kAimCol);

    for (i = 0; i < PRACTICE_HUD_LIST_MAX; i++) {
        HudSet(
            slot,
            PRACTICE_HUD_LIST + i,
            i < listCount ? list[i] : "",
            0,
            96 + i * 14,
            HUD_ALIGN_X_CENTER,
            HUD_ALIGN_Y_TOP,
            kTextCol
        );
    }

    HudSet(
        slot,
        PRACTICE_HUD_FEEDBACK,
        pc->feedbackExpire > level.inttime ? pc->feedback : "",
        0,
        80,
        HUD_ALIGN_X_CENTER,
        HUD_ALIGN_Y_TOP,
        kTextCol
    );
}

bool PracticeManager::ZeroSpread(Entity *ent) const
{
    if (!IsActive() || !ent) {
        return false;
    }
    return Practice_ControllerFor(ent) != NULL;
}

void Practice_Init(void)
{
    practiceManager.Init();
}

void Practice_Shutdown(void)
{
    practiceManager.Shutdown();
}

void Practice_Frame(void)
{
    practiceManager.Frame();
}

bool Practice_IsActive(void)
{
    return practiceManager.IsActive();
}

bool Practice_WantsPracticeController(void)
{
    return g_bPracticeCreateController;
}

bool Practice_ZeroSpread(Entity *ent)
{
    return practiceManager.ZeroSpread(ent);
}

bool Practice_IsPracticeBot(Entity *ent)
{
    return Practice_ControllerFor(ent) != NULL;
}

bool Practice_BlockStockRespawn(Player *player)
{
    if (!practiceManager.IsActive() || !player || !G_IsBot(player->edict)) {
        return false;
    }
    // Practice PlaceBots / AssignNextSpot own bot respawns; stock DM must not.
    return Practice_ControllerFor(player) != NULL;
}

float Practice_BotShotDamage(void)
{
    return PRACTICE_BOT_SHOT_DAMAGE;
}

bool Practice_TrySpawnPlayer(Player *player)
{
    return practiceManager.TrySpawnPlayer(player);
}

void Practice_OnPlayerSpawned(Player *player)
{
    practiceManager.OnPlayerSpawned(player);
}

void Practice_OnPlayerKilled(Player *player)
{
    practiceManager.OnPlayerKilled(player);
}

void Practice_OnProjectileSpawned(Projectile *proj, Entity *owner)
{
    practiceManager.OnProjectileSpawned(proj, owner);
}

void Practice_OnProjectileExplode(Projectile *proj)
{
    practiceManager.OnProjectileExplode(proj);
}

void Practice_FilterEditorInput(Player *player, usercmd_t *ucmd, const usereyes_t *eyes)
{
    practiceManager.FilterEditorInput(player, ucmd, eyes);
}
