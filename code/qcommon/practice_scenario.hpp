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

#include "q_shared.h"

#include <cstring>
#include <string>
#include <vector>

// Added in Omaha: hard caps for scenario parse / editor growth.
static const int PRACTICE_MAX_NODES  = 512;
static const int PRACTICE_MAX_THROWS = 256;
static const int PRACTICE_META_MAX_CHARS = 48;

enum PracticeCategory {
	PRACTICE_CAT_NONE   = 0,
	PRACTICE_CAT_ANGLES = 1,
	PRACTICE_CAT_NADES  = 2
};

enum PracticeStance {
	PRACTICE_STANCE_STAND  = 0,
	PRACTICE_STANCE_CROUCH = 1,
	// Added in Omaha: airborne throw (feet origin already elevated; eye uses stand height).
	PRACTICE_STANCE_JUMP   = 2
};

enum PracticeLean {
	PRACTICE_LEAN_NONE  = 0,
	PRACTICE_LEAN_LEFT  = 1,
	PRACTICE_LEAN_RIGHT = 2
};

enum PracticeNodeType {
	PRACTICE_NODE_START    = 0,
	PRACTICE_NODE_SPOT     = 1,
	PRACTICE_NODE_WAYPOINT = 2
};

struct PracticeVec3 {
	vec3_t v;

	PracticeVec3() { VectorClear(v); }
};

struct PracticeNode {
	// 0-based position in nodes; the start node is always 0. Rebuilt by RenumberNodes().
	int          id;
	int          type;
	PracticeVec3 origin;
	PracticeVec3 angles;
	int          stance;
	int          lean;

	PracticeNode()
		: id(0)
		, type(PRACTICE_NODE_SPOT)
		, stance(PRACTICE_STANCE_STAND)
		, lean(PRACTICE_LEAN_NONE)
	{}
};

struct PracticeSpot {
	PracticeVec3 origin;
	PracticeVec3 angles;
	int          stance;
	int          lean;
	int          unlockGate;
	int          nodeId;

	PracticeSpot()
		: stance(PRACTICE_STANCE_STAND)
		, lean(PRACTICE_LEAN_NONE)
		, unlockGate(0)
		, nodeId(0)
	{}
};

struct PracticeWaypoint {
	PracticeVec3 origin;
	int          nodeId;

	PracticeWaypoint()
		: nodeId(0)
	{}
};

struct PracticeThrow {
	PracticeVec3 stand;
	// Added in Omaha: camera/crosshair viewangles at throw (not weapon m_vViewAng).
	PracticeVec3 aim;
	// Added in Omaha: camera eye at throw (feet + viewheight + lean) — matches refdef.vieworg.
	PracticeVec3 eye;
	PracticeVec3 target;
	float        radius;
	// Added in Omaha: cook/charge fraction at throw (0..1); default 1 = full/uncooked release.
	float        charge;
	// Added in Omaha: body pose at throw (metadata / fallback cue reconstruction).
	int          stance;
	int          lean;
	bool         hasEye;
	bool         dummy;

	PracticeThrow()
		: radius(96.0f)
		, charge(1.0f)
		, stance(PRACTICE_STANCE_STAND)
		, lean(PRACTICE_LEAN_NONE)
		, hasEye(false)
		, dummy(true)
	{}
};

struct PracticeScenario {
	int                           version;
	int                           category;
	char                          map[MAX_QPATH];
	char                          name[MAX_QPATH];
	char                          author[MAX_QPATH];
	int                           maxAlive;
	// Added in Omaha: fastest angles clear time in ms (0 = none).
	int                           bestTimeMs;
	// Added in Omaha: difficulty the best time was set on (easy/medium/hard).
	char                          bestDifficulty[16];
	std::vector<PracticeNode>     nodes;
	std::vector<PracticeThrow>    throwsList;

	// Derived from nodes by RebuildDerived().
	PracticeVec3                  startOrigin;
	PracticeVec3                  startAngles;
	bool                          hasStart;
	std::vector<PracticeSpot>     spots;
	std::vector<PracticeWaypoint> waypoints;
	std::vector<PracticeVec3>     gates;

	// Set by PracticeScenario_Parse when stored node ids were missing or out of order.
	bool                          nodesRenumbered;

	PracticeScenario() { Clear(); }

	void Clear()
	{
		version   = 2;
		category  = PRACTICE_CAT_NONE;
		map[0]    = 0;
		name[0]   = 0;
		author[0] = 0;
		maxAlive  = 32;
		bestTimeMs = 0;
		bestDifficulty[0] = 0;
		nodes.clear();
		throwsList.clear();
		VectorClear(startOrigin.v);
		VectorClear(startAngles.v);
		hasStart = false;
		spots.clear();
		waypoints.clear();
		gates.clear();
		nodesRenumbered = false;
	}

	// Start node first (at most one), then ids = position. Returns true if anything changed.
	bool RenumberNodes()
	{
		bool changed = false;
		int  startIndex = -1;
		int  i;

		for (i = 0; i < (int)nodes.size(); i++) {
			if (nodes[i].type != PRACTICE_NODE_START) {
				continue;
			}
			if (startIndex < 0) {
				startIndex = i;
			} else {
				nodes.erase(nodes.begin() + i);
				i--;
				changed = true;
			}
		}
		if (startIndex > 0) {
			PracticeNode start = nodes[startIndex];

			nodes.erase(nodes.begin() + startIndex);
			nodes.insert(nodes.begin(), start);
			changed = true;
		}
		for (i = 0; i < (int)nodes.size(); i++) {
			if (nodes[i].id != i) {
				nodes[i].id = i;
				changed     = true;
			}
		}
		return changed;
	}

	// order lists every non-start node id exactly once, in the new sequence.
	bool ApplyNodeOrder(const std::vector<int> &order)
	{
		std::vector<PracticeNode> reordered;
		std::vector<bool>         used(nodes.size(), false);
		int                       firstMovable = 0;
		size_t                    i;

		RenumberNodes();
		if (!nodes.empty() && nodes[0].type == PRACTICE_NODE_START) {
			reordered.push_back(nodes[0]);
			firstMovable = 1;
		}
		if ((int)order.size() != (int)nodes.size() - firstMovable) {
			return false;
		}
		for (i = 0; i < order.size(); i++) {
			const int id = order[i];

			if (id < firstMovable || id >= (int)nodes.size() || used[id]) {
				return false;
			}
			used[id] = true;
			reordered.push_back(nodes[id]);
		}
		nodes = reordered;
		RebuildDerived();
		return true;
	}

	void RebuildDerived()
	{
		PracticeVec3 gate0;
		int          spotUnlockGate = 0;
		bool         inWaypointRun  = false;

		RenumberNodes();
		VectorClear(startOrigin.v);
		VectorClear(startAngles.v);
		hasStart = false;
		spots.clear();
		waypoints.clear();
		gates.clear();

		gates.push_back(gate0);

		for (const PracticeNode &node : nodes) {
			if (node.type == PRACTICE_NODE_START) {
				VectorCopy(node.origin.v, startOrigin.v);
				VectorCopy(node.angles.v, startAngles.v);
				VectorCopy(node.origin.v, gates[0].v);
				hasStart        = true;
				spotUnlockGate  = 0;
				inWaypointRun   = false;
			} else if (node.type == PRACTICE_NODE_WAYPOINT) {
				PracticeWaypoint wp;
				PracticeVec3     g;
				int              gateIndex;

				VectorCopy(node.origin.v, wp.origin.v);
				wp.nodeId = node.id;
				VectorCopy(node.origin.v, g.v);
				waypoints.push_back(wp);
				gates.push_back(g);
				gateIndex = (int)gates.size() - 1;
				// Changed in Omaha: consecutive waypoints without spots share one unlock —
				// the first of the run spawns the next enemy cycle; later WPs stay route-only.
				if (!inWaypointRun) {
					inWaypointRun  = true;
					spotUnlockGate = gateIndex;
				}
			} else if (node.type == PRACTICE_NODE_SPOT) {
				PracticeSpot spot;

				VectorCopy(node.origin.v, spot.origin.v);
				VectorCopy(node.angles.v, spot.angles.v);
				spot.stance     = node.stance;
				spot.lean       = node.lean;
				spot.unlockGate = spotUnlockGate;
				spot.nodeId     = node.id;
				spots.push_back(spot);
				inWaypointRun = false;
			}
		}
	}

	int SpotNodeIndex(int spotIndex) const
	{
		int spotCount = 0;
		int i;

		for (i = 0; i < (int)nodes.size(); i++) {
			if (nodes[i].type != PRACTICE_NODE_SPOT) {
				continue;
			}
			if (spotCount == spotIndex) {
				return i;
			}
			spotCount++;
		}
		return -1;
	}

	bool RemoveSpotAt(int spotIndex)
	{
		int nodeIndex = SpotNodeIndex(spotIndex);

		if (nodeIndex < 0) {
			return false;
		}
		nodes.erase(nodes.begin() + nodeIndex);
		RebuildDerived();
		return true;
	}

	int WaypointNodeIndex(int waypointIndex) const
	{
		int waypointCount = 0;
		int i;

		for (i = 0; i < (int)nodes.size(); i++) {
			if (nodes[i].type != PRACTICE_NODE_WAYPOINT) {
				continue;
			}
			if (waypointCount == waypointIndex) {
				return i;
			}
			waypointCount++;
		}
		return -1;
	}

	// Added in Omaha: remove a route waypoint by derived-list index (editor alt-fire).
	bool RemoveWaypointAt(int waypointIndex)
	{
		int nodeIndex = WaypointNodeIndex(waypointIndex);

		if (nodeIndex < 0) {
			return false;
		}
		nodes.erase(nodes.begin() + nodeIndex);
		RebuildDerived();
		return true;
	}

	bool PopLastSpot()
	{
		int i;

		for (i = (int)nodes.size() - 1; i >= 0; i--) {
			if (nodes[i].type == PRACTICE_NODE_SPOT) {
				nodes.erase(nodes.begin() + i);
				RebuildDerived();
				return true;
			}
		}
		return false;
	}
};

inline void Practice_CopyVec(vec3_t dst, const vec3_t src)
{
	VectorCopy(src, dst);
}

inline const char *Practice_CategoryName(int category)
{
	if (category == PRACTICE_CAT_NADES) {
		return "nades";
	}
	if (category == PRACTICE_CAT_ANGLES) {
		return "angles";
	}
	return "none";
}

inline int Practice_CategoryFromName(const char *name)
{
	if (!name) {
		return PRACTICE_CAT_NONE;
	}
	if (!Q_stricmp(name, "nades") || !Q_stricmp(name, "grenade") || !Q_stricmp(name, "grenades")) {
		return PRACTICE_CAT_NADES;
	}
	if (!Q_stricmp(name, "angles") || !Q_stricmp(name, "clearing")) {
		return PRACTICE_CAT_ANGLES;
	}
	return PRACTICE_CAT_NONE;
}

inline int Practice_StanceFromName(const char *name)
{
	if (name && !Q_stricmp(name, "crouch")) {
		return PRACTICE_STANCE_CROUCH;
	}
	if (name && !Q_stricmp(name, "jump")) {
		return PRACTICE_STANCE_JUMP;
	}
	return PRACTICE_STANCE_STAND;
}

inline const char *Practice_StanceName(int stance)
{
	if (stance == PRACTICE_STANCE_CROUCH) {
		return "crouch";
	}
	if (stance == PRACTICE_STANCE_JUMP) {
		return "jump";
	}
	return "stand";
}

// Added in Omaha: FP eye Z offset for a stored throw pose (matches bg viewheights).
// Jump uses stand height because stand.origin is already elevated mid-air.
inline float Practice_ViewHeightForStance(int stance)
{
	if (stance == PRACTICE_STANCE_CROUCH) {
		return 48.0f; // CROUCH_VIEWHEIGHT
	}
	return 82.0f; // DEFAULT_VIEWHEIGHT (stand + jump)
}

// Added in Omaha: throw-stand volume — XY disk, mostly upward (no floor bleed-through).
// standDown: tiny slack for feet sitting a hair below saved stand Z (float / uneven ground).
inline bool Practice_InThrowStand(
	const vec3_t playerOrigin,
	const vec3_t stand,
	float        standR    = 28.0f,
	float        standUp   = 96.0f,
	float        standDown = 8.0f
)
{
	vec3_t delta;
	float  dz;

	VectorSubtract(playerOrigin, stand, delta);
	dz       = delta[2];
	delta[2] = 0.0f;
	if (VectorLength(delta) >= standR) {
		return false;
	}
	// Fixed in Omaha: was infinite Z — stands one floor up/down falsely activated.
	// Fixed in Omaha: dz >= 0 rejected standing throws by ~0.01–0.2u (needed a jump to enter).
	if (dz < -standDown || dz > standUp) {
		return false;
	}
	return true;
}

// Added in Omaha: authored lean degrees for aim-cue / ghost (full lean, not ramp).
inline float Practice_LeanAngleForLean(int lean)
{
	if (lean == PRACTICE_LEAN_LEFT) {
		return -40.0f;
	}
	if (lean == PRACTICE_LEAN_RIGHT) {
		return 40.0f;
	}
	return 0.0f;
}

// Added in Omaha: FP camera eye matching CG_OffsetFirstPersonView (pitch pivot + lean).
// Aim cue must use this — bare feet+viewheight sits well below the real camera when looking up.
inline void Practice_CameraEye(vec3_t eye, const vec3_t stand, const vec3_t viewAngles, float viewheight, float leanAngle)
{
	vec3_t pivot;
	vec3_t forward;
	vec3_t left;
	vec3_t vStart;
	vec3_t vDelta;
	vec3_t vEnd;

	VectorCopy(stand, eye);
	eye[2] += viewheight;

	// Pitch pivot (cg_view.c CG_OffsetFirstPersonView): rotate about left by pitch*0.4.
	pivot[0] = viewAngles[0];
	pivot[1] = viewAngles[1];
	pivot[2] = 0.0f;
	AngleVectorsLeft(pivot, forward, left, NULL);

	VectorCopy(eye, vStart);
	if (viewAngles[0] > 0.0f) {
		vStart[2] -= viewheight * 0.4f;
	} else {
		vStart[2] -= viewheight * 0.2f;
	}
	VectorSubtract(eye, vStart, vDelta);
	RotatePointAroundVector(vEnd, left, vDelta, viewAngles[0] * 0.4f);
	VectorAdd(vStart, vEnd, eye);

	// Lean pivot about view forward (same 28.7 arc as CG_FirstPersonApplyLean).
	if (leanAngle != 0.0f) {
		VectorCopy(eye, vStart);
		vStart[2] -= 28.7f;
		VectorSubtract(eye, vStart, vDelta);
		RotatePointAroundVector(vEnd, forward, vDelta, leanAngle);
		VectorAdd(vStart, vEnd, eye);
	}
}

inline int Practice_LeanFromName(const char *name)
{
	if (!name) {
		return PRACTICE_LEAN_NONE;
	}
	if (!Q_stricmp(name, "left")) {
		return PRACTICE_LEAN_LEFT;
	}
	if (!Q_stricmp(name, "right")) {
		return PRACTICE_LEAN_RIGHT;
	}
	return PRACTICE_LEAN_NONE;
}

inline const char *Practice_LeanName(int lean)
{
	if (lean == PRACTICE_LEAN_LEFT) {
		return "left";
	}
	if (lean == PRACTICE_LEAN_RIGHT) {
		return "right";
	}
	return "none";
}

inline const char *Practice_NodeTypeName(int type)
{
	if (type == PRACTICE_NODE_START) {
		return "start";
	}
	if (type == PRACTICE_NODE_WAYPOINT) {
		return "waypoint";
	}
	return "spot";
}

inline int Practice_NodeTypeFromName(const char *name)
{
	if (!name) {
		return PRACTICE_NODE_SPOT;
	}
	if (!Q_stricmp(name, "start")) {
		return PRACTICE_NODE_START;
	}
	if (!Q_stricmp(name, "waypoint")) {
		return PRACTICE_NODE_WAYPOINT;
	}
	return PRACTICE_NODE_SPOT;
}

// Added in Omaha: cbuf / path validators (implemented in practice_scenario.cpp).
bool Practice_IsSafeToken(const char *s);
bool Practice_IsSafeDifficulty(const char *s);
bool Practice_IsSafeCategory(const char *s);
bool Practice_IsPracticeScenarioPath(const char *qpath);
bool Practice_IsWritableScenarioPath(const char *qpath);
void Practice_SanitizeMetaField(const char *in, char *out, size_t outSize);

// practice/<map>/<name>.json — map may contain slashes (dm/mohdm6).
void PracticeScenario_MakePath(const char *map, const char *name, char *out, size_t outSize);
bool PracticeScenario_Parse(const char *text, PracticeScenario *out, char *err, size_t errSize);
bool PracticeScenario_Dump(const PracticeScenario &in, std::string *out);
