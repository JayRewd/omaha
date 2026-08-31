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
#ifndef UIR_WEAPON_BAKE_LIST_H
#define UIR_WEAPON_BAKE_LIST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Added in OPM: gun vs grenade vs misc for shared-scale bake groups. */
typedef enum {
	UIR_BAKE_GUN = 0,
	UIR_BAKE_GRENADE,
	UIR_BAKE_OTHER
} uir_bake_kind_t;

typedef struct {
	const char     *modelPath;
	float           angles[3]; /* pitch yaw roll; used when hasAngles */
	float           offset[3]; /* added on top of wide-rect framing offset */
	float           framingScale; /* >0 => per-entry scale; 0 => group CLI default */
	int             hasAngles;
	int             hasOffset;
	int             excludeSharedPool; /* heavy/outlier: bake at group scale but skip max-extent pass */
	uir_bake_kind_t kind;
} uir_weapon_bake_entry_t;

int UIR_WeaponBakeEntryCount(void);
const uir_weapon_bake_entry_t *UIR_WeaponBakeEntry(int index);

#ifdef __cplusplus
}
#endif

#endif /* UIR_WEAPON_BAKE_LIST_H */
