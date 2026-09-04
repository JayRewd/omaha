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

#include "uid_profile.h"

#include <chrono>
#include <cstring>

namespace {

using prof_clock = std::chrono::steady_clock;

static int                    g_enabled;
static prof_clock::time_point g_starts[UID_PROF_COUNT];
static int                    g_active[UID_PROF_COUNT];

static uid_prof_timings_t g_load;
static uid_prof_timings_t g_frame;

/* Nested sample stack (ui_total → legacy_ui → hud_layer / disconnected_main). */
static uid_prof_timings_t g_frameStack[4];
static int                g_frameStackDepth;
static int                g_counts[UID_PROF_CNT_COUNT];
static int                g_countingNew;

static void ClearTimings(uid_prof_timings_t *t)
{
	if (!t) {
		return;
	}
	std::memset(t, 0, sizeof(*t));
}

static void FinishLoadTotals(uid_prof_timings_t *t)
{
	if (!t) {
		return;
	}
	t->totalUs = t->us[UID_PROF_LOAD_READ] + t->us[UID_PROF_LOAD_PARSE] + t->us[UID_PROF_LOAD_EXPAND]
		+ t->us[UID_PROF_LOAD_COMPILE] + t->us[UID_PROF_LOAD_ADOPT] + t->us[UID_PROF_LEGACY_LOAD];
}

static void FinishFrameTotals(uid_prof_timings_t *t)
{
	/*
	 * Non-overlapping pipeline sum. Nested detail phases (frame_paint_*,
	 * frame_foreach_window, frame_collection_cull) are reported but not
	 * added again — they run inside host_* / FRAME_BIND.
	 */
	if (!t) {
		return;
	}
	t->totalUs = t->us[UID_PROF_FRAME_BIND] + t->us[UID_PROF_FRAME_LAYOUT] + t->us[UID_PROF_FRAME_POINTER]
		+ t->us[UID_PROF_HOST_WORLD] + t->us[UID_PROF_HOST_CHROME] + t->us[UID_PROF_HOST_PREVIEWS]
		+ t->us[UID_PROF_HOST_OVERLAY] + t->us[UID_PROF_HOST_BATCH_FLUSH]
		+ t->us[UID_PROF_LEGACY_EVENTS] + t->us[UID_PROF_LEGACY_VIEW3D] + t->us[UID_PROF_LEGACY_URC]
		+ t->us[UID_PROF_LEGACY_MISC];
}

static int IsLoadPhase(uid_prof_phase_t phase)
{
	return phase <= UID_PROF_LEGACY_LOAD;
}

static void SnapshotActiveIntoFrame(uid_prof_timings_t *dst)
{
	const prof_clock::time_point now = prof_clock::now();
	int                          i;

	if (!dst) {
		return;
	}
	for (i = 0; i < UID_PROF_COUNT; ++i) {
		if (!g_active[i] || IsLoadPhase(static_cast<uid_prof_phase_t>(i))) {
			continue;
		}
		dst->us[i] += std::chrono::duration_cast<std::chrono::microseconds>(now - g_starts[i]).count();
		g_starts[i] = now;
	}
}

static void RestartActivePhases(void)
{
	const prof_clock::time_point now = prof_clock::now();
	int                          i;

	for (i = 0; i < UID_PROF_COUNT; ++i) {
		if (g_active[i]) {
			g_starts[i] = now;
		}
	}
}

} // namespace

void UID_ProfileSetEnabled(int enabled)
{
	g_enabled = enabled ? 1 : 0;
}

int UID_ProfileEnabled(void)
{
	return g_enabled;
}

const char *UID_ProfilePhaseName(uid_prof_phase_t phase)
{
	switch (phase) {
	case UID_PROF_LOAD_READ:
		return "load_read";
	case UID_PROF_LOAD_PARSE:
		return "load_parse";
	case UID_PROF_LOAD_EXPAND:
		return "load_expand";
	case UID_PROF_LOAD_COMPILE:
		return "load_compile";
	case UID_PROF_LOAD_ADOPT:
		return "load_adopt";
	case UID_PROF_LEGACY_LOAD:
		return "legacy_load";
	case UID_PROF_FRAME_BIND:
		return "frame_bind";
	case UID_PROF_FRAME_LAYOUT:
		return "frame_layout";
	case UID_PROF_FRAME_POINTER:
		return "frame_pointer";
	case UID_PROF_FRAME_PAINT_CHROME:
		return "frame_paint_chrome";
	case UID_PROF_FRAME_PAINT_OVERLAY:
		return "frame_paint_overlay";
	case UID_PROF_FRAME_FOREACH_WINDOW:
		return "frame_foreach_window";
	case UID_PROF_FRAME_COLLECTION_CULL:
		return "frame_collection_cull";
	case UID_PROF_HOST_WORLD:
		return "host_world";
	case UID_PROF_HOST_CHROME:
		return "host_chrome";
	case UID_PROF_HOST_PREVIEWS:
		return "host_previews";
	case UID_PROF_HOST_OVERLAY:
		return "host_overlay";
	case UID_PROF_HOST_BATCH_FLUSH:
		return "host_batch_flush";
	case UID_PROF_LEGACY_EVENTS:
		return "legacy_events";
	case UID_PROF_LEGACY_VIEW3D:
		return "legacy_view3d";
	case UID_PROF_LEGACY_URC:
		return "legacy_urc";
	case UID_PROF_LEGACY_MISC:
		return "legacy_misc";
	default:
		return "unknown";
	}
}

const char *UID_ProfileCounterName(uid_prof_counter_t counter)
{
	switch (counter) {
	case UID_PROF_CNT_NEW:
		return "new";
	case UID_PROF_CNT_CVAR_SET:
		return "cvar_set";
	case UID_PROF_CNT_CVAR_DESCRIBE:
		return "cvar_describe";
	case UID_PROF_CNT_STRTOD:
		return "strtod";
	case UID_PROF_CNT_SNPRINTF:
		return "snprintf";
	default:
		return "unknown";
	}
}

void UID_ProfileResetLoad(void)
{
	ClearTimings(&g_load);
}

void UID_ProfileResetFrame(void)
{
	ClearTimings(&g_frame);
	g_frameStackDepth = 0;
	std::memset(g_counts, 0, sizeof(g_counts));
}

void UID_ProfilePushFrame(void)
{
	if (!g_enabled) {
		return;
	}
	SnapshotActiveIntoFrame(&g_frame);
	if (g_frameStackDepth < static_cast<int>(sizeof(g_frameStack) / sizeof(g_frameStack[0]))) {
		g_frameStack[g_frameStackDepth++] = g_frame;
	}
	ClearTimings(&g_frame);
	std::memset(g_counts, 0, sizeof(g_counts));
	RestartActivePhases();
}

void UID_ProfilePopFrame(void)
{
	if (!g_enabled) {
		return;
	}
	if (g_frameStackDepth > 0) {
		g_frame = g_frameStack[--g_frameStackDepth];
	} else {
		ClearTimings(&g_frame);
	}
	std::memset(g_counts, 0, sizeof(g_counts));
	RestartActivePhases();
}

void UID_ProfileBegin(uid_prof_phase_t phase)
{
	if (!g_enabled || phase < 0 || phase >= UID_PROF_COUNT) {
		return;
	}
	g_starts[phase] = prof_clock::now();
	g_active[phase] = 1;
}

void UID_ProfileEnd(uid_prof_phase_t phase)
{
	long long           us;
	uid_prof_timings_t *dst;

	if (!g_enabled || phase < 0 || phase >= UID_PROF_COUNT || !g_active[phase]) {
		return;
	}
	us = std::chrono::duration_cast<std::chrono::microseconds>(prof_clock::now() - g_starts[phase]).count();
	g_active[phase] = 0;

	if (IsLoadPhase(phase)) {
		dst = &g_load;
	} else {
		dst = &g_frame;
	}
	dst->us[phase] += us;
}

void UID_ProfileSetLoadLabel(const char *label)
{
	if (!label) {
		g_load.label[0] = '\0';
		return;
	}
	std::strncpy(g_load.label, label, sizeof(g_load.label) - 1);
	g_load.label[sizeof(g_load.label) - 1] = '\0';
}

void UID_ProfileSetFrameLabel(const char *label)
{
	if (!label) {
		g_frame.label[0] = '\0';
		return;
	}
	std::strncpy(g_frame.label, label, sizeof(g_frame.label) - 1);
	g_frame.label[sizeof(g_frame.label) - 1] = '\0';
}

void UID_ProfileSetFrameMeta(int layoutRan, int nodeCount)
{
	/* layoutRan: 0=none 1=full 2=scoped; keep the higher code if nested samples. */
	if (layoutRan > g_frame.layoutRan) {
		g_frame.layoutRan = layoutRan;
	}
	g_frame.nodeCount += nodeCount;
}

void UID_ProfileNoteLayoutDirty(int nodeId, const char *kind, const char *reason)
{
	if (!g_enabled) {
		return;
	}
	g_frame.layoutDirtyHits++;
	if (g_frame.layoutDirtyHits > 1) {
		return;
	}
	g_frame.layoutDirtyNodeId = nodeId;
	if (kind && kind[0]) {
		std::strncpy(g_frame.layoutDirtyKind, kind, sizeof(g_frame.layoutDirtyKind) - 1);
		g_frame.layoutDirtyKind[sizeof(g_frame.layoutDirtyKind) - 1] = '\0';
	} else {
		g_frame.layoutDirtyKind[0] = '\0';
	}
	if (reason && reason[0]) {
		std::strncpy(g_frame.layoutDirtyReason, reason, sizeof(g_frame.layoutDirtyReason) - 1);
		g_frame.layoutDirtyReason[sizeof(g_frame.layoutDirtyReason) - 1] = '\0';
	} else {
		g_frame.layoutDirtyReason[0] = '\0';
	}
}

void UID_ProfileSetSubmitStats(int batches, int batchVerts, int batchTris, int clipApplies, int clipSkips)
{
	if (!g_enabled) {
		return;
	}
	g_frame.batches = batches;
	g_frame.batchVerts = batchVerts;
	g_frame.batchTris = batchTris;
	g_frame.clipApplies = clipApplies;
	g_frame.clipSkips = clipSkips;
}

void UID_ProfileCountReset(void)
{
	std::memset(g_counts, 0, sizeof(g_counts));
}

void UID_ProfileCountInc(uid_prof_counter_t counter)
{
	if (!g_enabled || counter < 0 || counter >= UID_PROF_CNT_COUNT) {
		return;
	}
	g_counts[counter]++;
}

void UID_ProfileCountBeginNew(void)
{
	g_countingNew = g_enabled ? 1 : 0;
}

void UID_ProfileCountEndNew(void)
{
	g_countingNew = 0;
}

int UID_ProfileCountingNew(void)
{
	return g_countingNew;
}

void UID_ProfileCaptureLoad(uid_prof_timings_t *out)
{
	if (!out) {
		return;
	}
	FinishLoadTotals(&g_load);
	*out = g_load;
}

void UID_ProfileCaptureFrame(uid_prof_timings_t *out)
{
	int i;

	if (!out) {
		return;
	}
	FinishFrameTotals(&g_frame);
	*out = g_frame;
	for (i = 0; i < UID_PROF_CNT_COUNT; ++i) {
		out->counts[i] = g_counts[i];
	}
}
