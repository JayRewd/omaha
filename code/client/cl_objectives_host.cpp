/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

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

#include "cl_objectives_host.h"

#include "client.h"
#include "cl_uivars.h"

#include <cstring>

static uir_objective_row_t g_objectiveRows[UIR_OBJECTIVES_MAX_ROWS];
static int                 g_objectiveCount = 0;
static float               g_objectiveAlpha = 0.0f;
static uint64_t            g_objectiveRevision = 1;

static void UIR_Objectives_SyncCvars(void)
{
	static int   s_lastVisible = -1;
	static int   s_lastCount = -1;
	static float s_lastAlpha = -999.0f;
	const int    visible = g_objectiveCount > 0 ? 1 : 0;

	/* Changed in Omaha: skip identical objective store pushes (alpha animates separately). */
	if (visible == s_lastVisible && g_objectiveCount == s_lastCount &&
		g_objectiveAlpha == s_lastAlpha) {
		return;
	}
	s_lastVisible = visible;
	s_lastCount = g_objectiveCount;
	s_lastAlpha = g_objectiveAlpha;
	CL_UIVar_Set("ui_om_hud_objectives_visible", visible ? "1" : "0");
	CL_UIVar_SetValue("ui_om_hud_objectives_alpha", g_objectiveAlpha);
	CL_UIVar_SetValue("ui_om_hud_objectives_count", g_objectiveCount);
}

static uint64_t UIR_Objectives_ContentHash(void)
{
	/* FNV-1a over row text/flags only — alpha is a cvar, not collection content. */
	uint64_t h = 14695981039346656037ull;
	int      i;

	h ^= (uint64_t)g_objectiveCount;
	h *= 1099511628211ull;
	for (i = 0; i < g_objectiveCount; ++i) {
		const uir_objective_row_t *row = &g_objectiveRows[i];
		const char                *p = row->text;
		while (*p) {
			h ^= (uint64_t)(unsigned char)(*p++);
			h *= 1099511628211ull;
		}
		h ^= (uint64_t)row->hidden;
		h *= 1099511628211ull;
		h ^= (uint64_t)row->completed;
		h *= 1099511628211ull;
		h ^= (uint64_t)row->current;
		h *= 1099511628211ull;
		h ^= (uint64_t)row->highlight;
		h *= 1099511628211ull;
	}
	return h;
}

void UIR_Objectives_Clear(void)
{
	g_objectiveCount = 0;
	std::memset(g_objectiveRows, 0, sizeof(g_objectiveRows));
}

void UIR_Objectives_SetAlpha(float alpha)
{
	if (alpha < 0.0f) {
		alpha = 0.0f;
	} else if (alpha > 1.0f) {
		alpha = 1.0f;
	}
	g_objectiveAlpha = alpha;
}

void UIR_Objectives_AddRow(const uir_objective_row_t *row)
{
	if (!row || g_objectiveCount >= UIR_OBJECTIVES_MAX_ROWS) {
		return;
	}
	g_objectiveRows[g_objectiveCount++] = *row;
}

void UIR_Objectives_NotifyChanged(void)
{
	static uint64_t s_publishedHash = 0;
	const uint64_t  hash = UIR_Objectives_ContentHash();

	UIR_Objectives_SyncCvars();
	/* Fixed in Omaha: CG sync used to bump revision every frame with identical rows. */
	if (hash == s_publishedHash) {
		return;
	}
	s_publishedHash = hash;
	g_objectiveRevision++;
}

int UIR_Objectives_GetRowCount(void)
{
	return g_objectiveCount;
}

const uir_objective_row_t *UIR_Objectives_GetRow(int index)
{
	if (index < 0 || index >= g_objectiveCount) {
		return NULL;
	}
	return &g_objectiveRows[index];
}

uint64_t UIR_Objectives_GetRevision(void)
{
	return g_objectiveRevision;
}
