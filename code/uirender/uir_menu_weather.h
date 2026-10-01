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
#ifndef UIR_MENU_WEATHER_H
#define UIR_MENU_WEATHER_H

#include "uir_map_env.h"
#include "uir_menuworld.h"

#ifdef __cplusplus
extern "C" {
#endif

void UIR_MenuWeatherAddToScene(
	const uir_map_env_t *env,
	const float vieworg[3],
	const float viewaxis[3][3],
	int cgRainEnabled,
	int realtime,
	const uir_menuworld_backend_t *backend);

/* Drop cached rain-beam shader handles after re.BeginRegistration. */
void UIR_MenuWeatherInvalidate(void);

#ifdef __cplusplus
}
#endif

#endif /* UIR_MENU_WEATHER_H */
