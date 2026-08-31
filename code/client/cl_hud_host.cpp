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

#include "client.h"
#include "cl_hud_host.h"
#include "cl_uirender.h"
#include "cl_ui.h"
#include "cl_inv.h"
#include "cl_objectives_host.h"
#include "../uirender/uir_compositor.h"
#include "../uirender/uir_image.h"
#include "../fgame/bg_public.h"

#include <algorithm>
#include <cmath>
#include <cctype>

extern clientGameExport_t *cge;

namespace {

struct WeaponSlotDef {
	int         bit;
	const char *texture;
	const char *textureEquipped;
};

static const WeaponSlotDef kWeaponSlots[] = {
	{WEAPON_CLASS_PISTOL, "textures/hud/weap_pistol", "textures/hud/weap_pistol_s"},
	{WEAPON_CLASS_RIFLE, "textures/hud/weap_rifle", "textures/hud/weap_rifle_s"},
	{WEAPON_CLASS_SMG, "textures/hud/weap_smg", "textures/hud/weap_smg_s"},
	{WEAPON_CLASS_MG, "textures/hud/weap_mg", "textures/hud/weap_mg_s"},
	{WEAPON_CLASS_GRENADE, "textures/hud/weap_grenade", "textures/hud/weap_grenade_s"},
	{WEAPON_CLASS_HEAVY, "textures/hud/weap_heavy", "textures/hud/weap_heavy_s"},
};

static int      g_lastWeaponsOwned = -1;
static int      g_lastWeaponsEquipped = 0xFFFF;
static int      g_weapHudHideTime = 0;
static int      g_itemHudHideTime = 0;
static qboolean g_weaponsBarShowing = qfalse;
static qboolean g_itemsBarShowing = qfalse;
static int      g_lastDamageDir = 0;
static int      g_damageFlashTime = 0;
static int      g_lastDamageHealth = -1;

/* Added in OPM: modern weapons-bar sticky caches (primary / sidearm / last gun). */
static char g_modernPrimaryName[MAX_QPATH];
static char g_modernSidearmName[MAX_QPATH];
static char g_modernLastGun[MAX_QPATH];
static int  g_modernPrimaryClip = 0;
static int  g_modernPrimaryAmmo = 0;
static int  g_modernSidearmClip = 0;
static int  g_modernSidearmAmmo = 0;
static int      g_modernLastGunClip = 0;
static int      g_modernLastGunAmmo = 0;
/* Added in OPM: sticky grenade inventory count (ammo pool can read 0 while unequipped). */
static int      g_modernGrenadeCount = 0;
/* Added in OPM: -2 = playing; -1 = free spectate; else followed client. */
static int      g_modernFollowClient = -2;

static const char *kWeaponSlotClassNames[] = {
	"pistol",
	"rifle",
	"smg",
	"mg",
	"grenade",
	"heavy",
};

static void UIR_Hud_SetCvar(const char *name, const char *value)
{
	if (!name || !value) {
		return;
	}
	Cvar_Set(name, value);
}

static void UIR_Hud_SetCvarInt(const char *name, int value)
{
	char buf[32];
	Com_sprintf(buf, sizeof(buf), "%d", value);
	UIR_Hud_SetCvar(name, buf);
}

static void UIR_Hud_SetCvarFrac(const char *name, float value)
{
	char buf[32];
	if (value < 0.0f) {
		value = 0.0f;
	} else if (value > 1.0f) {
		value = 1.0f;
	}
	Com_sprintf(buf, sizeof(buf), "%.3f", value);
	UIR_Hud_SetCvar(name, buf);
}

static void UIR_Hud_SetCvarAngleDeg(const char *name, float deg)
{
	char buf[32];
	Com_sprintf(buf, sizeof(buf), "%.1f", deg);
	UIR_Hud_SetCvar(name, buf);
}

static int UIR_Hud_HeadingSpinnerAngleTenths(int statValue)
{
	float frac = (float)statValue / 3600.0f;
	if (cge && cge->CG_EyeAngles) {
		vec3_t vViewAngles;
		cge->CG_EyeAngles(&vViewAngles);
		frac = (AngleSubtract(vViewAngles[1], frac * 360.0f + 180.0f) + 180.0f) / 360.0f;
		if (frac < 0.0f || frac > 1.0f) {
			frac = 0.0f;
		}
	}
	return (int)((frac * 360.0f - 180.0f) * 10.0f);
}

static int UIR_Hud_SpinnerAngleTenths(int statTenths)
{
	return statTenths - 1800;
}

static float UIR_Hud_CompassSpringAngleDeg(void)
{
	if (!cge || !cge->CG_EyeAngles) {
		return 0.0f;
	}

	static int   iLastCompassTime = -9999;
	static int   iLastTimeDelta = 0;
	static float fLastPitch = 0.0f;
	static float fLastYaw = 0.0f;
	static float fLastYawDelta = 0.0f;
	static float fYawOffset = 0.0f;
	static float fYawSpeed = 0.0f;
	static float fNeedleOffset = 0.0f;
	static float fNeedleSpeed = 0.0f;

	int    iTimeCount;
	int    iTimeDelta;
	float  fYawDelta;
	float  fYawDeltaDiff;
	vec3_t vViewAngles;

	cge->CG_EyeAngles(&vViewAngles);
	vViewAngles[1] -= cl.snap.ps.stats[STAT_COMPASSNORTH] / 182.0f;

	iTimeCount = uid.time - iLastCompassTime;
	if (iTimeCount <= 1000) {
		fYawDelta = AngleSubtract(vViewAngles[1], fLastYaw);
		if (fYawDelta > 0.1f) {
			if ((fYawDelta > 0.0f && fLastYawDelta < fYawDelta) || (fYawDelta < 0.0f && fLastYawDelta > fYawDelta)) {
				fYawSpeed += (fYawDelta - fLastYawDelta) * 0.75f;
				fYawOffset += (fYawDelta - fLastYawDelta) * 0.75f;
			}

			if (fYawSpeed > 90.0f) {
				fYawSpeed = 90.0f;
			} else if (fYawSpeed < 0.05f) {
				fYawSpeed = 0.05f;
			}
		}

		while (iTimeCount > 0) {
			iTimeDelta = iTimeCount;
			if (iTimeCount > 15) {
				iTimeDelta = 15;
			}
			iTimeCount -= iTimeDelta;

			if (fabs(fYawOffset) >= 0.1f || fabs(fYawSpeed) >= 0.01f) {
				fYawOffset += fYawSpeed * (float)iTimeDelta;
				if (fYawOffset > 0.0f) {
					fYawSpeed -= iTimeDelta * 0.00175f;
				} else if (fYawOffset < 0.0f) {
					fYawSpeed += iTimeDelta * 0.00175f;
				}

				if (fYawOffset > 40.0f) {
					fYawOffset = 40.0f;
					fYawSpeed  = 0.0f;
				} else if (fYawOffset < -40.0f) {
					fYawOffset = -40.0f;
					fYawSpeed  = 0.0f;
				}

				fYawSpeed -= iTimeDelta * 0.003f;
				if (fYawSpeed > 0.0f) {
					fYawSpeed -= iTimeDelta * 0.0004f;
				} else {
					fYawSpeed += iTimeDelta * 0.0004f;
					if (fYawSpeed > 0.0f) {
						fYawSpeed = 0.0f;
					}
				}
			} else {
				iTimeCount = 0;
				fYawOffset = 0.0f;
				fYawSpeed  = 0.0f;
			}
		}

		fLastYawDelta  = fYawDelta;
		iLastTimeDelta = uid.time - iLastCompassTime;
	} else {
		fLastYawDelta  = 0.0f;
		fYawOffset     = 0.0f;
		fYawSpeed      = 0.0f;
		iLastTimeDelta = 0;
	}

	fLastPitch       = vViewAngles[0];
	fLastYaw         = vViewAngles[1];
	iLastCompassTime = uid.time;

	if (iLastTimeDelta) {
		if (fabs(fLastYawDelta) > 0.1f) {
			fNeedleOffset -= fLastYawDelta;
		}

		iTimeCount = iLastTimeDelta;
		while (iTimeCount > 0) {
			iTimeDelta = iTimeCount;
			if (iTimeCount > 15) {
				iTimeDelta = 15;
			}
			iTimeCount -= iTimeDelta;

			if (fabs(fNeedleOffset) >= 0.1f || fabs(fNeedleSpeed) >= 0.01f) {
				fNeedleOffset += (float)iTimeDelta * fNeedleSpeed;
				if (fNeedleOffset > 180.0f) {
					fNeedleOffset -= 360.0f;
				} else if (fNeedleOffset < -180.0f) {
					fNeedleOffset += 360.0f;
				}

				if (fNeedleOffset > 0.0f) {
					fNeedleSpeed -= iTimeDelta * 0.00175f;
				} else if (fNeedleOffset < 0.0f) {
					fNeedleSpeed += iTimeDelta * 0.00175f;
				}

				fNeedleSpeed -= fNeedleSpeed * 0.0025f * (float)iTimeDelta;
				if (fNeedleSpeed > 0.0f) {
					fNeedleSpeed -= iTimeDelta * 0.00035f;
					if (fNeedleSpeed < 0.0f) {
						fNeedleSpeed = 0.0f;
					}
				} else {
					fNeedleSpeed += iTimeDelta * 0.00035f;
					if (fNeedleSpeed > 0.0f) {
						fNeedleSpeed = 0.0f;
					}
				}
			} else {
				iTimeCount    = 0;
				fNeedleOffset = 0.0f;
				fNeedleSpeed  = 0.0f;
			}
		}
	} else {
		fNeedleOffset = 0.0f;
		fNeedleSpeed  = 0.0f;
	}

	return anglemod(fNeedleOffset + fLastYaw);
}

/* Added in OPM: raw heading for modern tape compass (no classic needle spring bounce). */
static float UIR_Hud_CompassHeadingDeg(void)
{
	if (!cge || !cge->CG_EyeAngles) {
		return 0.0f;
	}

	vec3_t vViewAngles;
	cge->CG_EyeAngles(&vViewAngles);
	vViewAngles[1] -= cl.snap.ps.stats[STAT_COMPASSNORTH] / 182.0f;
	return anglemod(vViewAngles[1]);
}

static int UIR_Hud_WeaponSlotState(int owned, int equipped, int bit)
{
	if (!(owned & bit)) {
		return 3;
	}
	if (equipped & bit) {
		return 2;
	}
	return 1;
}

static int UIR_Hud_ItemSlotState(int owned, int equipped, int bit)
{
	if (!(owned & bit)) {
		return 3;
	}
	if (equipped & bit) {
		return 2;
	}
	return 1;
}

static void UIR_Hud_DrawImage(const char *path, float x, float y, float w, float h, float rotationDeg)
{
	UIR_ImageDrawClipped(path, x, y, w, h, NULL, 0, w, h, UIR_IMAGE_FIT_STRETCH, rotationDeg, 1.0f, 1.0f, NULL);
}

static const char *UIR_Hud_ItemIconPath(int handIndex, char *fallback, int fallbackSize)
{
	if (handIndex < 0 || handIndex >= 8 || cl.snap.ps.activeItems[handIndex] < 0) {
		return NULL;
	}
	const char *name = CL_ConfigString(CS_WEAPONS + cl.snap.ps.activeItems[handIndex]);
	inventory_item_t *item = CL_GetInvItemByName(&client_inv, name);
	if (item && item->bgshader) {
		const str shaderName = item->bgshader->GetName();
		if (shaderName.length() > 0) {
			return shaderName.c_str();
		}
	}
	if (fallback && fallbackSize > 0 && name && name[0]) {
		char sanitized[MAX_QPATH];
		int  j = 0;
		for (int i = 0; name[i] && j < (int)sizeof(sanitized) - 1; ++i) {
			const char c = name[i];
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
				sanitized[j++] = (char)tolower((unsigned char)c);
			}
		}
		sanitized[j] = '\0';
		if (sanitized[0]) {
			Com_sprintf(fallback, fallbackSize, "textures/hud/item_%s", sanitized);
			return fallback;
		}
	}
	return NULL;
}

static void UIR_Hud_SyncWeaponsBar(void)
{
	const cvar_t *ui_weaponsbar = Cvar_Get("ui_weaponsbar", "1", 0);
	const cvar_t *ui_weaponsbartime = Cvar_Get("ui_weaponsbartime", "2500", 0);
	const int     weaponsStat = cl.snap.ps.stats[STAT_WEAPONS];
	const int     equippedStat = cl.snap.ps.stats[STAT_EQUIPPED_WEAPON];
	const int     owned = weaponsStat & 0x3F;
	const int     equipped = equippedStat & 0x3F;
	qboolean      show = qfalse;

	if (ui_weaponsbar && ui_weaponsbar->integer && !(cl.snap.ps.pm_flags & PMF_NO_WEAPONBAR)) {
		if (ui_weaponsbar->integer == 2) {
			g_weapHudHideTime = cls.realtime + (int)ui_weaponsbartime->value;
		} else if (ui_weaponsbar->integer != 3 && g_itemHudHideTime && g_itemsBarShowing) {
			g_weapHudHideTime = 0;
		}

		if (g_lastWeaponsOwned != weaponsStat || g_lastWeaponsEquipped != equippedStat) {
			const int ownedDiff = weaponsStat ^ g_lastWeaponsOwned & 0x3F;
			const int equippedDiff = (g_lastWeaponsEquipped ^ equippedStat) & 0x3F;

			if (ownedDiff || equippedDiff) {
				g_weapHudHideTime = cls.realtime + ui_weaponsbartime->integer;
			}
		}

		if (!g_weapHudHideTime) {
			show = qfalse;
		} else if (g_weapHudHideTime < cls.realtime || g_itemHudHideTime > g_weapHudHideTime) {
			g_weapHudHideTime = 0;
			show = qfalse;
		} else {
			show = qtrue;
		}
	} else {
		g_weapHudHideTime = 0;
	}

	g_lastWeaponsOwned = weaponsStat & 0x3F | g_lastWeaponsOwned & ~0x3F;
	g_lastWeaponsEquipped = equippedStat & 0x3F | g_lastWeaponsEquipped & ~0x3F;
	g_weaponsBarShowing = show;

	UIR_Hud_SetCvarInt("ui_om_hud_weapons_owned", owned);
	UIR_Hud_SetCvarInt("ui_om_hud_weapons_equipped", equipped);
	UIR_Hud_SetCvarInt("ui_om_hud_weapons_visible", show ? 1 : 0);
}

static void UIR_Hud_SyncItemsBar(void)
{
	const cvar_t *ui_weaponsbar = Cvar_Get("ui_weaponsbar", "1", 0);
	const cvar_t *ui_itemsbar = Cvar_Get("ui_itemsbar", "1", 0);
	const cvar_t *ui_weaponsbartime = Cvar_Get("ui_weaponsbartime", "2500", 0);
	const int     weaponsStat = cl.snap.ps.stats[STAT_WEAPONS];
	const int     equippedStat = cl.snap.ps.stats[STAT_EQUIPPED_WEAPON];
	const int     owned = (weaponsStat >> 8) & 0xF;
	const int     equipped = (equippedStat >> 8) & 0xF;
	qboolean      show = qfalse;

	if (ui_weaponsbar && ui_weaponsbar->integer && ui_itemsbar && ui_itemsbar->integer) {
		if (ui_weaponsbar->integer == 3) {
			g_itemHudHideTime = cls.realtime + ui_weaponsbartime->integer;

			if (g_weapHudHideTime && g_weaponsBarShowing) {
				g_itemHudHideTime = 0;
			}
		}

		if (g_lastWeaponsOwned != weaponsStat || g_lastWeaponsEquipped != equippedStat) {
			const int ownedDiff = weaponsStat ^ g_lastWeaponsOwned & 0xF00;
			const int equippedDiff = (g_lastWeaponsEquipped ^ equippedStat) & 0xF00;

			if (ownedDiff || equippedDiff) {
				g_weapHudHideTime = cls.realtime + ui_weaponsbartime->integer;
			}
		}

		if (!g_itemHudHideTime) {
			show = qfalse;
		} else if (g_itemHudHideTime < cls.realtime || g_weapHudHideTime > g_itemHudHideTime) {
			g_itemHudHideTime = 0;
			show = qfalse;
		} else {
			show = qtrue;
		}
	} else {
		g_itemHudHideTime = 0;
	}

	g_lastWeaponsOwned = weaponsStat & 0xF00 | (g_lastWeaponsOwned & 0xF0);
	g_lastWeaponsEquipped = equippedStat & 0xF00 | (g_lastWeaponsEquipped & 0xF0);
	g_itemsBarShowing = show;

	UIR_Hud_SetCvarInt("ui_om_hud_items_owned", owned);
	UIR_Hud_SetCvarInt("ui_om_hud_items_equipped", equipped);
	UIR_Hud_SetCvarInt("ui_om_hud_items_visible", show ? 1 : 0);
}

static void UIR_Hud_SyncCompassCvars(void)
{
	const int objLeft = cl.snap.ps.stats[STAT_OBJECTIVELEFT];
	const int objRight = cl.snap.ps.stats[STAT_OBJECTIVERIGHT];
	const int objCenter = cl.snap.ps.stats[STAT_OBJECTIVECENTER];
	const int dmgStat = cl.snap.ps.stats[STAT_DAMAGEDIR];
	const int health = cl.snap.ps.stats[STAT_HEALTH];
	const int swMs = Cvar_VariableIntegerValue("ui_om_hud_stopwatch_ms");

	/*
	 * Fixed in OPM: retail headingspinner only retriggers when the damage-dir
	 * stat changes. Chip damage from the same direction leaves the stat unchanged
	 * so the fade never restarts. Also restart when health drops while a damage
	 * direction is active (covers repeated low-damage hits).
	 */
	if (dmgStat != g_lastDamageDir) {
		g_damageFlashTime = uid.time;
	} else if (dmgStat > 0 && g_lastDamageHealth >= 0 && health < g_lastDamageHealth) {
		g_damageFlashTime = uid.time;
	}
	g_lastDamageDir = dmgStat;
	g_lastDamageHealth = health;

	float damageAlpha = 0.0f;
	if (dmgStat > 0) {
		damageAlpha = 1.0f - (float)(uid.time - g_damageFlashTime) / 1500.0f;
		if (damageAlpha < 0.0f) {
			damageAlpha = 0.0f;
		} else if (damageAlpha > 1.0f) {
			damageAlpha = 1.0f;
		}
	}

	const float compassAngle = UIR_Hud_CompassSpringAngleDeg();
	UIR_Hud_SetCvarAngleDeg("ui_om_hud_compass_angle", compassAngle);
	/* Added in OPM: stable heading for modern scrolling compass. */
	UIR_Hud_SetCvarAngleDeg("ui_om_hud_compass_heading", UIR_Hud_CompassHeadingDeg());
	UIR_Hud_SetCvarAngleDeg("ui_om_hud_damage_angle", UIR_Hud_HeadingSpinnerAngleTenths(dmgStat) / 10.0f);
	UIR_Hud_SetCvarFrac("ui_om_hud_damage_alpha", damageAlpha);
	UIR_Hud_SetCvarAngleDeg("ui_om_hud_obj_arrow_angle", UIR_Hud_SpinnerAngleTenths(objCenter) / 10.0f);
	UIR_Hud_SetCvarAngleDeg("ui_om_hud_obj_left_angle", UIR_Hud_SpinnerAngleTenths(objLeft) / 10.0f);
	UIR_Hud_SetCvarAngleDeg("ui_om_hud_obj_right_angle", UIR_Hud_SpinnerAngleTenths(objRight) / 10.0f);
	UIR_Hud_SetCvarInt("ui_om_hud_obj_left_visible", objLeft != 1730 ? 1 : 0);
	UIR_Hud_SetCvarInt("ui_om_hud_obj_right_visible", objRight != 1870 ? 1 : 0);
	UIR_Hud_SetCvarInt("ui_om_hud_obj_arrow_visible", objCenter != 1800 ? 1 : 0);
	UIR_Hud_SetCvarAngleDeg("ui_om_hud_stopwatch_angle", swMs > 0 ? (swMs * 360.0f) / 60000.0f : 0.0f);
}

static void UIR_Hud_SyncFractionCvars(int bossHealth, int swMs, int swType)
{
	float fuseFrac = 0.0f;

	if (swMs > 0 && (swType == 1 || swType == 2)) {
		fuseFrac = std::min(1.0f, (float)swMs / 30000.0f);
	}

	UIR_Hud_SetCvarFrac("ui_om_hud_boss_frac", bossHealth > 0 ? (float)bossHealth / 100.0f : 0.0f);
	UIR_Hud_SetCvarFrac("ui_om_hud_fuse_frac", fuseFrac);
	UIR_Hud_SetCvarFrac("ui_om_hud_boss_right", bossHealth > 0 ? (float)bossHealth / 100.0f : 0.0f);
	UIR_Hud_SetCvarFrac("ui_om_hud_fuse_right", fuseFrac);
}

static void UIR_Hud_SyncSlotStateCvars(void)
{
	const int weaponsStat = cl.snap.ps.stats[STAT_WEAPONS];
	const int equippedStat = cl.snap.ps.stats[STAT_EQUIPPED_WEAPON];
	const int ownedWeapons = weaponsStat & 0x3F;
	const int equippedWeapons = equippedStat & 0x3F;
	const int ownedItems = (weaponsStat >> 8) & 0xF;
	const int equippedItems = (equippedStat >> 8) & 0xF;
	const int slotCount = (int)(sizeof(kWeaponSlots) / sizeof(kWeaponSlots[0]));

	for (int i = 0; i < slotCount; ++i) {
		char cvarName[64];
		Com_sprintf(
			cvarName,
			sizeof(cvarName),
			"ui_om_hud_weap_%s_state",
			kWeaponSlotClassNames[i]
		);
		UIR_Hud_SetCvarInt(
			cvarName,
			UIR_Hud_WeaponSlotState(ownedWeapons, equippedWeapons, kWeaponSlots[i].bit)
		);
	}

	for (int i = 0; i < 4; ++i) {
		const int     bit = (1 << i);
		char          fallback[MAX_QPATH];
		const char   *iconPath;
		const char   *itemName = "";
		char          imageCvar[48];
		char          stateCvar[48];
		char          nameCvar[48];

		Com_sprintf(imageCvar, sizeof(imageCvar), "ui_om_hud_item%d_image", i);
		Com_sprintf(stateCvar, sizeof(stateCvar), "ui_om_hud_item%d_state", i);
		Com_sprintf(nameCvar, sizeof(nameCvar), "ui_om_hud_item%d_name", i);

		iconPath = UIR_Hud_ItemIconPath(i + 2, fallback, sizeof(fallback));
		if (cl.snap.ps.activeItems[i + 2] >= 0) {
			const char *cfgName = CL_ConfigString(CS_WEAPONS + cl.snap.ps.activeItems[i + 2]);
			if (cfgName) {
				itemName = cfgName;
			}
		}

		UIR_Hud_SetCvar(imageCvar, iconPath ? iconPath : "");
		UIR_Hud_SetCvarInt(stateCvar, UIR_Hud_ItemSlotState(ownedItems, equippedItems, bit));
		UIR_Hud_SetCvar(nameCvar, itemName);
	}
}

static const int kModernPrimaryClassMask =
	WEAPON_CLASS_RIFLE | WEAPON_CLASS_SMG | WEAPON_CLASS_MG | WEAPON_CLASS_HEAVY;

static void UIR_Hud_CopyWeaponName(char *dst, size_t dstSize, const char *src)
{
	if (!dst || dstSize == 0) {
		return;
	}
	if (!src) {
		dst[0] = '\0';
		return;
	}
	Q_strncpyz(dst, src, dstSize);
}

/*
 * Added in OPM: retail snapshots only name the *equipped* gun (activeItems[ITEM_WEAPON]).
 * Unequipped ownership is class bits only. The server already prints the real item name
 * on pickup ("Picked Up <name>") — parse that on the client (no fgame/protocol change).
 */
static char g_modernPendingPickup[MAX_QPATH];
static int  g_modernPrevOwnSidearm = -1;
static int  g_modernPrevOwnPrimary = -1;

static qboolean UIR_Hud_IsGrenadeOrItemName(const char *name)
{
	if (!name || !name[0]) {
		return qtrue;
	}
	if (Q_stristr(name, "Grenade") || Q_stristr(name, "Stielhandgranate")) {
		return qtrue;
	}
	/* Inventory items also use "Picked Up" in some paths — ignore non-guns via CS. */
	for (int i = 0; i < MAX_WEAPONS; ++i) {
		const char *cs = CL_ConfigString(CS_WEAPONS + i);
		if (cs && cs[0] && !Q_stricmp(cs, name)) {
			return qfalse;
		}
	}
	return qtrue;
}

static void UIR_Hud_ClearModernWeaponsSticky(void)
{
	g_modernPrimaryName[0] = '\0';
	g_modernSidearmName[0] = '\0';
	g_modernLastGun[0] = '\0';
	g_modernPendingPickup[0] = '\0';
	g_modernPrimaryClip = 0;
	g_modernPrimaryAmmo = 0;
	g_modernSidearmClip = 0;
	g_modernSidearmAmmo = 0;
	g_modernLastGunClip = 0;
	g_modernLastGunAmmo = 0;
	g_modernGrenadeCount = 0;
	g_modernPrevOwnSidearm = -1;
	g_modernPrevOwnPrimary = -1;
}

static qboolean UIR_Hud_IsSidearmWeaponName(const char *name)
{
	inventory_item_t *item;

	if (!name || !name[0]) {
		return qfalse;
	}
	/* Retail pistol display names (modern-sidearm-weapons + common silenced). */
	if (!Q_stricmp(name, "Colt 45") || !Q_stricmp(name, "Walther P38")
		|| !Q_stricmp(name, "Hi-Standard Silenced")) {
		return qtrue;
	}
	item = CL_GetInvItemByName(&client_inv, name);
	if (item && item->ammoname.length() > 0 && !Q_stricmp(item->ammoname.c_str(), "pistol")) {
		return qtrue;
	}
	return qfalse;
}

static void UIR_Hud_SeedDefaultSidearmName(void)
{
	const int team = cl.snap.ps.stats[STAT_TEAM];
	if (g_modernSidearmName[0]) {
		return;
	}
	/* Loadout sidearm is never named in the snap until equipped/picked up. */
	if (team == TEAM_AXIS) {
		UIR_Hud_CopyWeaponName(g_modernSidearmName, sizeof(g_modernSidearmName), "Walther P38");
	} else {
		UIR_Hud_CopyWeaponName(g_modernSidearmName, sizeof(g_modernSidearmName), "Colt 45");
	}
}

static void UIR_Hud_ApplyPendingPickup(qboolean ownPrimary, qboolean ownSidearm)
{
	if (!g_modernPendingPickup[0]) {
		return;
	}

	const qboolean pendingSidearm = UIR_Hud_IsSidearmWeaponName(g_modernPendingPickup);
	const qboolean gainedSidearm = ownSidearm && g_modernPrevOwnSidearm == 0;
	const qboolean gainedPrimary = ownPrimary && g_modernPrevOwnPrimary == 0;

	/*
	 * Fixed in OPM: classify by weapon identity. Previously any pickup went to
	 * primary whenever ownPrimary was set, so picking up a Walther after a rifle
	 * renamed both sticky slots to the pistol.
	 */
	if (pendingSidearm) {
		if (ownSidearm) {
			UIR_Hud_CopyWeaponName(g_modernSidearmName, sizeof(g_modernSidearmName), g_modernPendingPickup);
		} else {
			return;
		}
	} else if (gainedSidearm && !gainedPrimary) {
		UIR_Hud_CopyWeaponName(g_modernSidearmName, sizeof(g_modernSidearmName), g_modernPendingPickup);
	} else if (ownPrimary) {
		UIR_Hud_CopyWeaponName(g_modernPrimaryName, sizeof(g_modernPrimaryName), g_modernPendingPickup);
	} else if (ownSidearm) {
		UIR_Hud_CopyWeaponName(g_modernSidearmName, sizeof(g_modernSidearmName), g_modernPendingPickup);
	} else {
		return;
	}

	g_modernPendingPickup[0] = '\0';
}

// Added in OPM: sticky primary/sidearm/last-gun names + ammo for modern weapons bar.
static void UIR_Hud_SyncModernWeaponsBar(const char *weaponName, int clip, int ammo)
{
	const int pmFlags = cl.snap.ps.pm_flags;
	int followKey = -2;
	if (pmFlags & PMF_SPECTATING) {
		followKey = (pmFlags & PMF_CAMERA_VIEW) ? cl.snap.ps.stats[STAT_INFOCLIENT] : -1;
	}
	/* Added in OPM: drop sticky names when entering/leaving follow or swapping targets. */
	if (followKey != g_modernFollowClient) {
		UIR_Hud_ClearModernWeaponsSticky();
		g_modernFollowClient = followKey;
	}

	const int owned = cl.snap.ps.stats[STAT_WEAPONS] & 0x3F;
	const int equipped = cl.snap.ps.stats[STAT_EQUIPPED_WEAPON] & 0x3F;
	const qboolean ownPrimary = (owned & kModernPrimaryClassMask) != 0;
	const qboolean ownSidearm = (owned & WEAPON_CLASS_PISTOL) != 0;
	const qboolean equipGrenade = (equipped & WEAPON_CLASS_GRENADE) != 0;
	const qboolean equipPrimary = (equipped & kModernPrimaryClassMask) != 0;
	const qboolean equipSidearm = (equipped & WEAPON_CLASS_PISTOL) != 0;
	const qboolean hasActive = weaponName && weaponName[0];

	UIR_Hud_ApplyPendingPickup(ownPrimary, ownSidearm);

	if (hasActive && equipSidearm && !equipGrenade) {
		UIR_Hud_CopyWeaponName(g_modernSidearmName, sizeof(g_modernSidearmName), weaponName);
		UIR_Hud_CopyWeaponName(g_modernLastGun, sizeof(g_modernLastGun), weaponName);
		g_modernSidearmClip = clip;
		g_modernSidearmAmmo = ammo;
		g_modernLastGunClip = clip;
		g_modernLastGunAmmo = ammo;
	} else if (hasActive && equipPrimary && !equipGrenade) {
		UIR_Hud_CopyWeaponName(g_modernPrimaryName, sizeof(g_modernPrimaryName), weaponName);
		UIR_Hud_CopyWeaponName(g_modernLastGun, sizeof(g_modernLastGun), weaponName);
		g_modernPrimaryClip = clip;
		g_modernPrimaryAmmo = ammo;
		g_modernLastGunClip = clip;
		g_modernLastGunAmmo = ammo;
	}

	if (!ownPrimary) {
		g_modernPrimaryName[0] = '\0';
		g_modernPrimaryClip = 0;
		g_modernPrimaryAmmo = 0;
	}
	if (!ownSidearm) {
		g_modernSidearmName[0] = '\0';
		g_modernSidearmClip = 0;
		g_modernSidearmAmmo = 0;
	} else if (!g_modernSidearmName[0]) {
		/* Added in OPM: seed loadout pistol name (snap never names unequipped guns). */
		UIR_Hud_SeedDefaultSidearmName();
	}

	if (g_modernLastGun[0]) {
		const qboolean lastIsSidearm =
			g_modernSidearmName[0] && !Q_stricmp(g_modernLastGun, g_modernSidearmName);
		const qboolean lastIsPrimary =
			g_modernPrimaryName[0] && !Q_stricmp(g_modernLastGun, g_modernPrimaryName);
		if (!lastIsSidearm && !lastIsPrimary) {
			if (ownPrimary && g_modernPrimaryName[0]) {
				UIR_Hud_CopyWeaponName(g_modernLastGun, sizeof(g_modernLastGun), g_modernPrimaryName);
				g_modernLastGunClip = g_modernPrimaryClip;
				g_modernLastGunAmmo = g_modernPrimaryAmmo;
			} else if (ownSidearm && g_modernSidearmName[0]) {
				UIR_Hud_CopyWeaponName(g_modernLastGun, sizeof(g_modernLastGun), g_modernSidearmName);
				g_modernLastGunClip = g_modernSidearmClip;
				g_modernLastGunAmmo = g_modernSidearmAmmo;
			} else {
				g_modernLastGun[0] = '\0';
				g_modernLastGunClip = 0;
				g_modernLastGunAmmo = 0;
			}
		}
	} else if (ownPrimary && g_modernPrimaryName[0]) {
		UIR_Hud_CopyWeaponName(g_modernLastGun, sizeof(g_modernLastGun), g_modernPrimaryName);
		g_modernLastGunClip = g_modernPrimaryClip;
		g_modernLastGunAmmo = g_modernPrimaryAmmo;
	} else if (ownSidearm && g_modernSidearmName[0]) {
		UIR_Hud_CopyWeaponName(g_modernLastGun, sizeof(g_modernLastGun), g_modernSidearmName);
		g_modernLastGunClip = g_modernSidearmClip;
		g_modernLastGunAmmo = g_modernSidearmAmmo;
	}

	g_modernPrevOwnPrimary = ownPrimary ? 1 : 0;
	g_modernPrevOwnSidearm = ownSidearm ? 1 : 0;

	UIR_Hud_SetCvar("ui_om_hud_primary_name", g_modernPrimaryName);
	UIR_Hud_SetCvar("ui_om_hud_sidearm_name", g_modernSidearmName);
	UIR_Hud_SetCvar("ui_om_hud_last_gun", g_modernLastGun);
	UIR_Hud_SetCvarInt("ui_om_hud_last_gun_clip", g_modernLastGunClip);
	UIR_Hud_SetCvarInt("ui_om_hud_last_gun_ammo", g_modernLastGunAmmo);
}

} // namespace

/* Added in OPM: client-only unequipped-gun naming from the retail pickup print. */
void UIR_Hud_NotifyPickedUpWeapon(const char *message)
{
	const char *p;
	char        name[MAX_QPATH];
	int         len;

	if (!message || !message[0] || !CL_UIR_UseModernHudPack()) {
		return;
	}

	p = message;
	while (*p == ' ' || *p == '\t') {
		p++;
	}

	/* LV_ConvertString("Picked Up <item>") — English prefix; name is the CS weapon string. */
	if (Q_stricmpn(p, "Picked Up ", 10) != 0) {
		return;
	}
	p += 10;
	while (*p == ' ' || *p == '\t') {
		p++;
	}

	Q_strncpyz(name, p, sizeof(name));
	len = (int)strlen(name);
	while (len > 0 && (name[len - 1] == '\n' || name[len - 1] == '\r' || name[len - 1] == ' ')) {
		name[--len] = '\0';
	}
	if (!name[0] || UIR_Hud_IsGrenadeOrItemName(name)) {
		return;
	}

	UIR_Hud_CopyWeaponName(g_modernPendingPickup, sizeof(g_modernPendingPickup), name);
}

void UIR_Hud_Sync(void)
{
	char buf[256];
	const char *weaponName = "";

	if (!CL_UIR_UseModernHudPack() || clc.state != CA_ACTIVE || !cl.snap.valid) {
		/* Added in OPM: clear sniper overlay gate when HUD sync is inactive. */
		UIR_Hud_SetCvarInt("ui_om_hud_sniper_zoom", 0);
		return;
	}

	if (cge && cge->CG_SyncModernHudCvars) {
		cge->CG_SyncModernHudCvars();
	}

	const int pmFlags = cl.snap.ps.pm_flags;
	const qboolean following =
		(pmFlags & PMF_SPECTATING) != 0 && (pmFlags & PMF_CAMERA_VIEW) != 0
		&& cl.snap.ps.stats[STAT_INFOCLIENT] >= 0;

	int health = cl.snap.ps.stats[STAT_HEALTH];
	int maxHealth = std::max(1, cl.snap.ps.stats[STAT_MAXHEALTH]);
	/*
	 * Fixed in OPM: chase-spectate health. Prefer followed-player combat stats
	 * (CopyHudCombatStats → STAT_HEALTH). If the server has not copied combat
	 * yet, fall back to STAT_INFOCLIENT_HEALTH (percent while following).
	 */
	if (following) {
		const int infoHealth = cl.snap.ps.stats[STAT_INFOCLIENT_HEALTH];
		if ((cl.snap.ps.stats[STAT_WEAPONS] & 0x3F) == 0 && infoHealth > 0) {
			health = infoHealth;
			maxHealth = 100;
		}
	}
	const int clip = cl.snap.ps.stats[STAT_CLIPAMMO];
	const int ammo = cl.snap.ps.stats[STAT_AMMO];
	const int maxClip = std::max(1, cl.snap.ps.stats[STAT_MAXCLIPAMMO]);
	const int maxAmmo = std::max(1, cl.snap.ps.stats[STAT_MAXAMMO]);
	const int team = cl.snap.ps.stats[STAT_TEAM];
	const int bossHealth = cl.snap.ps.stats[STAT_BOSSHEALTH];
	const int swMs = Cvar_VariableIntegerValue("ui_om_hud_stopwatch_ms");
	const int swType = Cvar_VariableIntegerValue("ui_om_hud_stopwatch_type");

	UIR_Hud_SetCvarInt("ui_om_hud_health", health);
	UIR_Hud_SetCvarInt("ui_om_hud_max_health", maxHealth);
	Com_sprintf(buf, sizeof(buf), "%.3f", (float)health / (float)maxHealth);
	UIR_Hud_SetCvar("ui_om_hud_health_frac", buf);
	UIR_Hud_SetCvarFrac("ui_om_hud_health_top", 1.0f - (float)health / (float)maxHealth);
	UIR_Hud_SetCvarInt("ui_om_hud_clip", clip);
	UIR_Hud_SetCvarInt("ui_om_hud_ammo", ammo);
	UIR_Hud_SetCvarInt("ui_om_hud_max_clip", maxClip);
	UIR_Hud_SetCvarInt("ui_om_hud_max_ammo", maxAmmo);
	UIR_Hud_SetCvarInt("ui_om_hud_team", team);
	UIR_Hud_SetCvarInt("ui_om_hud_in_zoom", cl.snap.ps.stats[STAT_INZOOM]);
	/*
	 * Added in OPM: sniper scope overlay (not Spy Camera / Binoculars).
	 * Same FOV gate as retail CG_DrawZoomOverlay zoom types 0/1.
	 */
	{
		int sniperZoom = 0;
		const int inZoom = cl.snap.ps.stats[STAT_INZOOM];
		if (inZoom > 0 && inZoom <= 30) {
			const char *wpn = "";
			if (cl.snap.ps.activeItems[ITEM_WEAPON] >= 0) {
				wpn = CL_ConfigString(CS_WEAPONS + cl.snap.ps.activeItems[ITEM_WEAPON]);
			}
			if (!wpn) {
				wpn = "";
			}
			if (Q_stricmp(wpn, "Spy Camera") != 0 && Q_stricmp(wpn, "Binoculars") != 0) {
				sniperZoom = 1;
			}
		}
		UIR_Hud_SetCvarInt("ui_om_hud_sniper_zoom", sniperZoom);
	}
	UIR_Hud_SetCvarInt("ui_om_hud_compass_north", cl.snap.ps.stats[STAT_COMPASSNORTH]);
	UIR_Hud_SetCvarInt("ui_om_hud_damage_dir", cl.snap.ps.stats[STAT_DAMAGEDIR]);
	UIR_Hud_SetCvarInt("ui_om_hud_objective_left", cl.snap.ps.stats[STAT_OBJECTIVELEFT]);
	UIR_Hud_SetCvarInt("ui_om_hud_objective_right", cl.snap.ps.stats[STAT_OBJECTIVERIGHT]);
	UIR_Hud_SetCvarInt("ui_om_hud_objective_center", cl.snap.ps.stats[STAT_OBJECTIVECENTER]);
	UIR_Hud_SetCvarInt("ui_om_hud_boss_health", bossHealth);
	/* Changed in OPM: fold legacy suppress flags into hud_root visibility. */
	{
		int show = Cvar_VariableIntegerValue("cg_hud");
		if (!show || (cl.snap.ps.pm_flags & PMF_NO_HUD) || (cl.snap.ps.pm_flags & PMF_INTERMISSION)
			|| UI_LetterboxActive()) {
			show = 0;
		}
		UIR_Hud_SetCvarInt("ui_om_hud_show", show);
	}

	UIR_Hud_SyncWeaponsBar();
	UIR_Hud_SyncItemsBar();

	if (cl.snap.ps.activeItems[ITEM_WEAPON] >= 0) {
		weaponName = CL_ConfigString(CS_WEAPONS + cl.snap.ps.activeItems[ITEM_WEAPON]);
		UIR_Hud_SetCvar("ui_om_hud_active_weapon", weaponName ? weaponName : "");
	} else {
		UIR_Hud_SetCvar("ui_om_hud_active_weapon", "");
	}

	/*
	 * Added in OPM: grenade inventory count for modern HUD panel (client-only).
	 * Prefer ammo pool ("grenade" / "agrenade" / name match). When equipped,
	 * STAT_AMMO is live (maxclip 1 folds clip into ammo). Sticky keeps the last
	 * good count while owned but unequipped — pool reads can briefly hit 0 on
	 * weapon switch, which used to hide the grenade panel.
	 */
	{
		int nadeCount = 0;
		int unusedMax = 0;
		int n = 0;
		int grenadeMax = 0;
		const int ownedWeapons = cl.snap.ps.stats[STAT_WEAPONS] & 0x3F;
		const qboolean ownGrenade = (ownedWeapons & WEAPON_CLASS_GRENADE) != 0;
		const qboolean equipGrenade =
			(cl.snap.ps.stats[STAT_EQUIPPED_WEAPON] & WEAPON_CLASS_GRENADE) != 0;

		CL_AmmoCount("grenade", &n, &unusedMax);
		nadeCount = n;
		grenadeMax = unusedMax;
		CL_AmmoCount("agrenade", &n, &unusedMax);
		if (n > nadeCount) {
			nadeCount = n;
		}
		if (unusedMax > grenadeMax) {
			grenadeMax = unusedMax;
		}
		for (int i = 0; i < (int)ARRAY_LEN(cl.snap.ps.ammo_name_index); ++i) {
			const int index = cl.snap.ps.ammo_name_index[i];
			const char *ammoName;
			if (index <= 0 || index >= MAX_CONFIGSTRINGS) {
				continue;
			}
			ammoName = CL_ConfigString(index);
			if (!ammoName || !ammoName[0]) {
				continue;
			}
			if (Q_stristr(ammoName, "riflegrenade")) {
				continue;
			}
			if (!Q_stristr(ammoName, "grenade") && Q_stricmp(ammoName, "agrenade")) {
				continue;
			}
			if (cl.snap.ps.ammo_amount[i] > nadeCount) {
				nadeCount = cl.snap.ps.ammo_amount[i];
			}
			if (cl.snap.ps.max_ammo_amount[i] > grenadeMax) {
				grenadeMax = cl.snap.ps.max_ammo_amount[i];
			}
		}
		/*
		 * Fixed in OPM: while a grenade is equipped, STAT_AMMO often equals
		 * pool+clip even when the pool was not decremented for the loaded
		 * round (shows 6 with max 5). Prefer STAT_AMMO only when it does not
		 * exceed the ammo-type max.
		 */
		if (equipGrenade && ammo > nadeCount) {
			nadeCount = ammo;
		}
		if (grenadeMax > 0 && nadeCount > grenadeMax) {
			nadeCount = grenadeMax;
		}

		if (!ownGrenade) {
			g_modernGrenadeCount = 0;
		} else if (nadeCount > 0 || equipGrenade) {
			g_modernGrenadeCount = nadeCount;
		}
		UIR_Hud_SetCvarInt("ui_om_hud_grenade_count", g_modernGrenadeCount);
	}

	UIR_Hud_SyncCompassCvars();

	UIR_Hud_SyncFractionCvars(bossHealth, swMs, swType);
	UIR_Hud_SyncSlotStateCvars();
	UIR_Hud_SyncModernWeaponsBar(weaponName ? weaponName : "", clip, ammo);
}
