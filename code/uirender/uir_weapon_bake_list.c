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

#include "uir_weapon_bake_list.h"

#include <stddef.h>

/*
 * Base MOHAA multiplayer world weapon TIKIs (Player::EquipWeapons_ver8).
 * Angles: world rest side-on (pitch 0, yaw 90, roll ±90) from weapon.cpp.
 */
#define A_GUN 0.0f, 90.0f, 90.0f
#define A_PIST 0.0f, 90.0f, -90.0f
#define A_NADE 0.0f, 90.0f, 0.0f
#define A_MISC 0.0f, 90.0f, 0.0f
#define O0 0.0f, 0.0f, 0.0f

static const uir_weapon_bake_entry_t g_weaponBakeList[] = {
	{"models/weapons/m1_garand.tik", {A_GUN}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_GUN},
	{"models/weapons/kar98.tik", {A_GUN}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_GUN},
	{"models/weapons/springfield.tik", {A_GUN}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_GUN},
	{"models/weapons/KAR98sniper.tik", {A_GUN}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_GUN},
	{"models/weapons/thompsonsmg.tik", {A_GUN}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_GUN},
	{"models/weapons/mp40.tik", {A_GUN}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_GUN},
	{"models/weapons/bar.tik", {A_GUN}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_GUN},
	{"models/weapons/mp44.tik", {A_GUN}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_GUN},
	{"models/weapons/bazooka.tik", {A_GUN}, {O0}, 0.0f, 1, 0, 1, UIR_BAKE_GUN},
	{"models/weapons/panzerschreck.tik", {A_GUN}, {O0}, 0.0f, 1, 0, 1, UIR_BAKE_GUN},
	{"models/weapons/shotgun.tik", {A_GUN}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_GUN},
	{"models/weapons/colt45.tik", {A_PIST}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_GUN},
	{"models/weapons/p38.tik", {A_PIST}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_GUN},
	{"models/weapons/m2frag_grenade.tik", {A_NADE}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_GRENADE},
	{"models/weapons/steilhandgranate.tik", {A_NADE}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_GRENADE},
	{"models/items/binoculars.tik", {A_MISC}, {O0}, 0.0f, 1, 0, 0, UIR_BAKE_OTHER},
};

int UIR_WeaponBakeEntryCount(void)
{
	return (int)(sizeof(g_weaponBakeList) / sizeof(g_weaponBakeList[0]));
}

const uir_weapon_bake_entry_t *UIR_WeaponBakeEntry(int index)
{
	if (index < 0 || index >= UIR_WeaponBakeEntryCount()) {
		return NULL;
	}
	return &g_weaponBakeList[index];
}
