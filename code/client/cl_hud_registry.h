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
#ifndef CL_HUD_REGISTRY_H
#define CL_HUD_REGISTRY_H

#ifdef __cplusplus
extern "C" {
#endif

#define CL_HUD_LEGACY_ID "legacy"

void     CL_UIMenu_ReloadHudRegistry(void);
void     CL_UIMenu_ListHuds_f(void);
int      CL_UIMenu_HudCount(void);
qboolean CL_UIMenu_HudExists(const char *hudId);
const char *CL_UIMenu_HudPath(const char *hudId);
const char *CL_UIMenu_HudLabel(const char *hudId);
int        CL_UIMenu_HudDrawOrder(const char *hudId);
qboolean   CL_UIMenu_HudIsBuiltinLegacy(const char *hudId);
void       CL_UIMenu_HudEntryAt(int index, const char **outId, const char **outLabel, const char **outPath);
uint64_t   CL_UIMenu_HudRegistryRevision(void);
void       CL_UIMenu_ValidateHudCvar(void);

#ifdef __cplusplus
}
#endif

#endif /* CL_HUD_REGISTRY_H */
