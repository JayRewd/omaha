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

#pragma once

#include "playerbot.h"
#include "beam.h"
#include "camera.h"
#include "scriptslave.h"
#include "../qcommon/practice_scenario.hpp"

#include <string>
#include <vector>

class Player;
class Projectile;
class Entity;

extern cvar_t *g_practice;
extern cvar_t *g_practice_file;
extern cvar_t *g_practice_edit;
extern cvar_t *g_practice_difficulty;
extern cvar_t *g_practice_maxalive;

extern bool g_bPracticeCreateController;

class PracticeBotController : public BotController
{
public:
	CLASS_PROTOTYPE(PracticeBotController);

	PracticeBotController();

	void UpdateBotStates(void) override;
	void Spawned(void) override;
	void Killed(const Event& ev) override;

	void SetHome(const Vector& origin, const Vector& angles, int stance, int lean);
	void ClearHome(void);
	void SetDummy(bool dummy);
	void SetAllowFire(bool allowFire);

	bool          IsDummy(void) const { return m_bDummy; }
	bool          HasHome(void) const { return m_bHasHome; }
	const Vector& HomeOrigin(void) const { return m_vHomeOrigin; }
	const Vector& HomeAngles(void) const { return m_vHomeAngles; }
	// Added in Omaha: world-point LOS for death-respawn safety (no human entity required).
	bool SeesWorldPoint(const Vector& point) const;

private:
	// outSeeSrc: 1 = bot→player forward LOS, 2 = player→bot reverse only.
	bool   VisibleAimPoint(Player *human, Vector& out, int *outSeeSrc = NULL) const;
	Vector LeanedEyePosition(void) const;
	void   ApplyEyeOffset(void);

	Vector m_vHomeOrigin;
	Vector m_vHomeAngles;
	int    m_iStance;
	int    m_iLean;
	int    m_iSightTime;
	int    m_iLastSeeTime;
	int    m_iClearTime;
	int    m_iLastClearTime;
	int    m_iNextFireTime;
	int    m_iNextJoinTime;
	bool   m_bDummy;
	bool   m_bAllowFire;
	bool   m_bHasHome;
};

// Added in Omaha: one straight server-drawn marker segment (stock ET_BEAM, visible to stock clients).
struct PracticeBeamSpec {
	Vector start;
	Vector end;
	Vector color;
	float  width;
	float  light; // dlight radius, 0 = none

	PracticeBeamSpec()
		: width(1.0f)
		, light(0.0f)
	{}
};

// Added in Omaha: reusable FuncBeam pool; singleClient >= 0 sends the beams to that client only.
class PracticeBeamSet
{
public:
	PracticeBeamSet()
		: m_iSingleClient(-1)
	{}

	void SetSingleClient(int clientNum) { m_iSingleClient = clientNum; }
	void Apply(const std::vector<PracticeBeamSpec>& specs);
	void Clear(void);
	// Drop handles without touching entities (level teardown frees them).
	void Forget(void);

private:
	std::vector<SafePtr<FuncBeam> > m_beams;
	std::vector<PracticeBeamSpec>   m_last;
	int                             m_iSingleClient;
};

// Added in Omaha: stock per-client huddraw element block used by practice text.
static const int PRACTICE_HUD_BASE     = 220;
static const int PRACTICE_HUD_TITLE    = 0;
static const int PRACTICE_HUD_HELP     = 1;
static const int PRACTICE_HUD_HELP_MAX = 8;
static const int PRACTICE_HUD_STATUS   = 9;
static const int PRACTICE_HUD_AIM      = 10;
static const int PRACTICE_HUD_LIST     = 11;
static const int PRACTICE_HUD_LIST_MAX = 12;
static const int PRACTICE_HUD_FEEDBACK = 23;
static const int PRACTICE_HUD_COUNT    = 24;

struct PracticeHudElem {
	std::string text;
	std::string layout;
	bool        sent;
	bool        used;

	PracticeHudElem()
		: sent(false)
		, used(false)
	{}
};

struct PracticeNadeSample {
	int    time;
	Vector origin;
};

// Added in Omaha: aim cue world point for a saved throw (cached; world-only trace).
struct PracticeCue {
	Vector pos;
	Vector forward;
};

// Added in Omaha: angles-edit node under the crosshair (may be stacked with others).
enum PracticeAimedKind {
	PRACTICE_AIMED_START    = 0,
	PRACTICE_AIMED_WAYPOINT = 1,
	PRACTICE_AIMED_SPOT     = 2
};

struct PracticeAimedNode {
	int   kind;
	int   index; // scenario spot/waypoint index; -1 for start
	int   nodeId;
	float s; // fraction along the aim segment (near → far)
	char  pose[48];
};

// Added in Omaha: per-human practice state (nade mode supports several players at once).
struct PracticeClient {
	SafePtr<Player> player;
	int             editButtons;
	int             lastNadeEnt;
	float           lastNadeCharge;
	Vector          lastNadeOrigin;
	Vector          lastThrowStand;
	Vector          lastThrowAim; // camera/crosshair angles for aim cue
	Vector          lastThrowEye; // FP camera eye (pitch pivot + lean) for aim cue
	Vector          lastThrowFireAim; // weapon m_vViewAng for silent replay
	Vector          lastNadeStart; // projectile spawn point for replay
	int             lastThrowTime;
	int             lastThrowStance;
	int             lastThrowLean;
	int             nextReplayTime;
	bool            lastNadeExploded;
	char            lastNadeModel[MAX_QPATH];
	bool            deathAtPlace;
	Vector          deathSpawnOrigin;
	Vector          deathSpawnAngles;

	SafePtr<Camera>                 camera;
	bool                            camAttached;
	int                             camStartTime;
	int                             camExplodeTime;
	Vector                          camRest;
	Vector                          camRestoreAngles;
	std::vector<PracticeNadeSample> nadeTrail;

	PracticeBeamSet beams;
	int             activeThrow;
	int             lockedThrow;
	float           releaseSent;

	PracticeHudElem hud[PRACTICE_HUD_COUNT];
	int             hudRefreshIndex;
	int             nextHudRefresh;
	char            feedback[64];
	int             feedbackExpire;
	int             nextSoloNotice;

	PracticeClient() { Reset(); }
	void Reset(void);
};

class PracticeManager : public Listener
{
public:
	CLASS_PROTOTYPE(PracticeManager);

	PracticeManager();

	void Init(void);
	void Shutdown(void);
	void Frame(void);

	bool IsActive(void) const;
	bool IsEdit(void) const;
	int  Category(void) const { return m_scenario.category; }
	bool WantsPracticeController(void) const { return g_bPracticeCreateController; }
	bool ZeroSpread(Entity *ent) const;
	bool TrySpawnPlayer(Player *player);
	void OnPlayerSpawned(Player *player);
	void OnPlayerKilled(Player *player);
	void OnProjectileSpawned(Projectile *proj, Entity *owner);
	void OnProjectileExplode(Projectile *proj);
	// Added in Omaha: end chase when nade is removed without exploding (sky / OOB / etc).
	void OnProjectileGone(int slot);
	void FilterEditorInput(Player *player, usercmd_t *ucmd, const usereyes_t *eyes);
	Player *GetHuman(void);
	int     ReactionMs(void) const;
	int     FireIntervalMs(void) const;

	void EventSpotAdd(Event *ev);
	void EventSpotUndo(Event *ev);
	void EventStartSet(Event *ev);
	void EventWaypointAdd(Event *ev);
	void EventThrowSave(Event *ev);
	void EventWeapon(Event *ev);
	void EventSave(Event *ev);
	void EventMeta(Event *ev);
	void EventOrderBegin(Event *ev);
	void EventOrderAppend(Event *ev);
	void EventOrderCommit(Event *ev);

private:
	Player *HumanFromEvent(Event *ev);
	bool    MayEdit(Player *player);
	void    ClearOrderDraft(void);
	void    LoadScenario(void);
	void    Autosave(void);
	int     WantedBots(void) const;
	int     UnlockedSpotCount(void) const;
	bool    NextSpotLocked(void) const;
	void    EnsureBots(void);
	void    PlaceBots(void);
	void    BeginAnglesRun(void);
	void    EndAnglesRun(bool cleared);
	void    UpdateGates(Player *human);
	void    SyncWaypointObjective(void);
	bool    AssignNextSpot(PracticeBotController *controller);
	void    TeleportToSpot(Player *player, const vec3_t origin, const vec3_t angles);
	void    ForceViewAngles(Player *player, const vec3_t angles);
	void    PlaceHumanAtGate(Player *player, int gate);
	void    FreezeHuman(Player *player, float seconds);
	bool    GateExposedToBots(int gate) const;
	int     FindSafeDeathSpawnGate(void) const;
	void    ApplyLoadout(Player *player);
	void    StripGrenades(Player *player);
	void    RefillGrenades(Player *player);
	void    EquipRifle(Player *player);

	void AddSpot(Player *player);
	void AddWaypoint(Player *player);
	void SetStart(Player *player);
	void SaveLastThrow(Player *player);
	void ReplayLastThrow(Player *player);
	void EndNadeChase(int slot, const Vector& origin, int hit);
	// Added in Omaha: pistol alt-fire — snap to the in-ring throw whose aim cue you're looking at.
	bool SnapToThrowStand(Player *player);
	void EditPrimaryFire(Player *player, const Vector& eye, const Vector& end);
	void EditAltFire(Player *player, const Vector& eye, const Vector& end);
	void EditFeedback(Player *player, const char *fmt, ...) Q_PRINTF_FUNC(3, 4);
	void GameMessage(Player *player, const char *fmt, ...) Q_PRINTF_FUNC(3, 4);

	// Added in Omaha: stock-client presentation (entities, Camera, huddraw).
	int             SlotFor(Player *player) const;
	PracticeClient *ClientFor(Player *player);
	void            SyncClients(void);
	void            ResetClient(int slot);
	void            EnforceAnglesSolo(void);
	bool            IsAnglesBystander(Player *player) const;
	void            RebuildCueCache(void);
	int             FindAimedThrow(Player *player);
	// Added in Omaha: angles-edit aim ray hits (stacked spots/waypoints); selected = alt-fire remove.
	void            CollectAimedAnglesNodes(
					   const Vector&                   eye,
					   const Vector&                   end,
					   std::vector<PracticeAimedNode>& out,
					   int                            *outSelected
				   ) const;
	void            AimPickRay(Player *player, Vector& eye, Vector& end) const;
	float           ChargeFill(Player *player, float charge) const;
	void            FormatThrowLine(Player *player, const PracticeThrow& th, char *out, size_t outSize) const;
	void            UpdateNadeTracking(int slot);
	void            UpdateChaseCam(int slot);
	void            StopChaseCam(int slot, bool restoreView);
	void            UpdateClientMarkers(int slot);
	void            UpdateGlobalMarkers(void);
	void            UpdatePreviews(void);
	void            UpdateClientHud(int slot);
	void            HudSet(int slot, int elem, const char *text, int x, int y, int halign, int valign, const float *color);
	void            SetFeedback(int slot, const char *text, int durationMs);
	bool            IsOmahaHost(Player *player) const;
	void            SendOmahaCommand(const char *fmt, ...) Q_PRINTF_FUNC(2, 3);
	void            ForgetPresentation(void);

	PracticeScenario m_scenario;
	bool             m_bLoaded;
	bool             m_bLoadOk; // Added in Omaha: file parse/create succeeded — gates Autosave
	bool             m_bMapMismatch; // Added in Omaha: scenario map != level.mapname
	bool             m_bComplete;
	bool             m_bRunActive; // Added in Omaha: timed angles attempt in progress
	bool             m_bBotsPlaced;
	bool             m_bDeathRewind; // Added in Omaha: next human spawn is a death position penalty
	bool             m_bNadeReplay; // Added in Omaha: silent rethrow — no chase cam
	bool             m_bCueCacheDirty;
	int              m_iNextSpot;
	int              m_iCurrentGate;
	int              m_iDeathSpawnGate; // Added in Omaha: LOS-safe prior gate (progress unchanged)
	int              m_iRunStartTime;
	int              m_iCompleteTime;
	int              m_iAnglesOwner; // Added in Omaha: the one human allowed to play angles mode
	int              m_iNextGlobalMarkers;
	int              m_iNextPreviews;
	std::vector<int> m_orderDraft; // Added in Omaha: chunked practice_order_*
	int              m_iOrderExpected;

	PracticeClient                    m_clients[MAX_CLIENTS];
	std::vector<PracticeCue>          m_cueCache;
	PracticeBeamSet                   m_globalBeams;
	// Added in Omaha: green dlight on the next angles waypoint (play mode only).
	SafePtr<Entity>                   m_waypointLight;
	// Fixed in Omaha: one entity per spotIndex (stable); never rebind a model to another spot.
	std::vector<SafePtr<ScriptModel> > m_previews;
	std::vector<std::string>           m_previewKeys;
	std::vector<int>                   m_previewIds; // parallel; -1 = start, else spots[] index
};

extern PracticeManager practiceManager;

bool Practice_IsActive(void);
bool Practice_WantsPracticeController(void);
bool  Practice_ZeroSpread(Entity *ent);
bool  Practice_IsPracticeBot(Entity *ent);
// Added in Omaha: practice owns bot respawn (PlaceBots / AssignNextSpot) — block stock DM force-respawn.
bool  Practice_BlockStockRespawn(Player *player);
float Practice_BotShotDamage(void);
bool  Practice_TrySpawnPlayer(Player *player);
void Practice_Init(void);
void Practice_Shutdown(void);
void Practice_Frame(void);
void Practice_OnPlayerSpawned(Player *player);
void Practice_OnPlayerKilled(Player *player);
void Practice_OnProjectileSpawned(Projectile *proj, Entity *owner);
void Practice_OnProjectileExplode(Projectile *proj);
void Practice_FilterEditorInput(Player *player, usercmd_t *ucmd, const usereyes_t *eyes);
