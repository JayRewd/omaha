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
#ifndef UIR_MENU_MAP_VIEW_H
#define UIR_MENU_MAP_VIEW_H

#include "../qcommon/q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UIR_MENU_MAP_VIEW_SOURCE_ID "menu-map-views"

/* Built-in Remagen defaults when XML source is missing. */
#define UIR_MENU_MAP_VIEW_DEFAULT_ID   "remagen"
#define UIR_MENU_MAP_VIEW_DEFAULT_BSP  "maps/dm/mohdm3.bsp"

typedef struct {
	char  id[64];
	char  bsp[MAX_QPATH];
	float vieworg[3];
	float pitch;
	float yaw;
	float roll;
	float fov;
} uir_menu_map_view_t;

void UIR_MenuMapViewSetDefaults(uir_menu_map_view_t *out);

#ifdef __cplusplus
}
#endif

#endif /* UIR_MENU_MAP_VIEW_H */
