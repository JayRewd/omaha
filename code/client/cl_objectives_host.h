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
#pragma once

#include "../qcommon/q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UIR_OBJECTIVES_MAX_ROWS 20

typedef struct {
	char text[256];
	int  hidden;
	int  completed;
	int  current;
	int  highlight;
} uir_objective_row_t;

void UIR_Objectives_Clear(void);
void UIR_Objectives_SetAlpha(float alpha);
void UIR_Objectives_AddRow(const uir_objective_row_t *row);
void UIR_Objectives_NotifyChanged(void);
int  UIR_Objectives_GetRowCount(void);
const uir_objective_row_t *UIR_Objectives_GetRow(int index);
uint64_t UIR_Objectives_GetRevision(void);

#ifdef __cplusplus
}
#endif
