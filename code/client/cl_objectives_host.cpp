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

#include <cstring>

static uir_objective_row_t g_objectiveRows[UIR_OBJECTIVES_MAX_ROWS];
static int                 g_objectiveCount = 0;
static float               g_objectiveAlpha = 0.0f;
static uint64_t            g_objectiveRevision = 1;

static cvar_t *ui_om_hud_objectives_visible = NULL;
static cvar_t *ui_om_hud_objectives_alpha = NULL;
static cvar_t *ui_om_hud_objectives_count = NULL;

static void UIR_Objectives_EnsureCvars(void)
{
	if (ui_om_hud_objectives_visible) {
		return;
	}
	ui_om_hud_objectives_visible = Cvar_Get("ui_om_hud_objectives_visible", "0", CVAR_TEMP);
	ui_om_hud_objectives_alpha = Cvar_Get("ui_om_hud_objectives_alpha", "0", CVAR_TEMP);
	ui_om_hud_objectives_count = Cvar_Get("ui_om_hud_objectives_count", "0", CVAR_TEMP);
}

static void UIR_Objectives_SyncCvars(void)
{
	UIR_Objectives_EnsureCvars();
	Cvar_Set("ui_om_hud_objectives_visible", g_objectiveCount > 0 ? "1" : "0");
	Cvar_SetValue("ui_om_hud_objectives_alpha", g_objectiveAlpha);
	Cvar_SetValue("ui_om_hud_objectives_count", g_objectiveCount);
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
	UIR_Objectives_SyncCvars();
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
