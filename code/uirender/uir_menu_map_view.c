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

#include "uir_menu_map_view.h"

#include <string.h>

void UIR_MenuMapViewSetDefaults(uir_menu_map_view_t *out)
{
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	Q_strncpyz(out->id, UIR_MENU_MAP_VIEW_DEFAULT_ID, sizeof(out->id));
	Q_strncpyz(out->bsp, UIR_MENU_MAP_VIEW_DEFAULT_BSP, sizeof(out->bsp));
	out->vieworg[0] = 947.23f;
	out->vieworg[1] = -649.70f;
	out->vieworg[2] = -68.50f;
	out->pitch = -31.15f;
	out->yaw = -67.03f;
	out->roll = 0.0f;
	out->fov = 80.0f;
}
