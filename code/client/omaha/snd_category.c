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

#include "snd_category.h"

static qboolean S_Cat_HasPrefix(const char *s, const char *prefix)
{
	size_t n;

	if (!s || !prefix) {
		return qfalse;
	}
	n = strlen(prefix);
	return (qboolean)(Q_stricmpn(s, prefix, (int)n) == 0);
}

static qboolean S_Cat_PathHas(const char *path, const char *needle)
{
	return (qboolean)(path && needle && Q_stristr(path, needle) != NULL);
}

static qboolean S_Cat_IsVehicleAlias(const char *alias)
{
	if (!alias) {
		return qfalse;
	}
	if (Q_stristr(alias, "vehicle") || Q_stristr(alias, "tread_snd") || S_Cat_HasPrefix(alias, "airplane")
	    || S_Cat_HasPrefix(alias, "airstrike") || S_Cat_HasPrefix(alias, "armoredtrain")) {
		return qtrue;
	}
	if (S_Cat_HasPrefix(alias, "tank_") || S_Cat_HasPrefix(alias, "jeep_") || S_Cat_HasPrefix(alias, "king_")
	    || S_Cat_HasPrefix(alias, "sdkfz_") || S_Cat_HasPrefix(alias, "opel") || S_Cat_HasPrefix(alias, "higgins")
	    || S_Cat_HasPrefix(alias, "stuka") || S_Cat_HasPrefix(alias, "flak_") || S_Cat_HasPrefix(alias, "aagun_")
	    || S_Cat_HasPrefix(alias, "bmwbike_") || S_Cat_HasPrefix(alias, "nebelwerfer_")
	    || S_Cat_HasPrefix(alias, "granatwerfer_") || S_Cat_HasPrefix(alias, "bohler_")
	    || S_Cat_HasPrefix(alias, "damage_vehicle") || S_Cat_HasPrefix(alias, "damage_tank")) {
		return qtrue;
	}
	/* Vehicle doors live under mechanics/ paths — catch by alias prefix + snd_door. */
	if (Q_stristr(alias, "snd_door")
	    && (S_Cat_HasPrefix(alias, "tank") || S_Cat_HasPrefix(alias, "jeep") || S_Cat_HasPrefix(alias, "king")
	        || S_Cat_HasPrefix(alias, "sdkfz") || S_Cat_HasPrefix(alias, "opel") || S_Cat_HasPrefix(alias, "aagun")
	        || S_Cat_HasPrefix(alias, "flak") || S_Cat_HasPrefix(alias, "bmwbike") || S_Cat_HasPrefix(alias, "bohler")
	        || S_Cat_HasPrefix(alias, "nebelwerfer") || S_Cat_HasPrefix(alias, "granatwerfer"))) {
		return qtrue;
	}
	return qfalse;
}

static sndCategory_t S_Cat_FromAlias(const char *alias)
{
	if (!alias || !alias[0]) {
		return SND_CAT_AUTO;
	}

	if (S_Cat_HasPrefix(alias, "snd_step_") || S_Cat_HasPrefix(alias, "snd_landing_")
	    || S_Cat_HasPrefix(alias, "dog_fs") || !Q_stricmp(alias, "boot_click1")) {
		return SND_CAT_FOOTSTEPS;
	}
	if (Q_stristr(alias, "bodyfall") || S_Cat_HasPrefix(alias, "snd_bodyfall_")) {
		return SND_CAT_BODYFALLS;
	}
	if (S_Cat_HasPrefix(alias, "snd_bh_") || S_Cat_HasPrefix(alias, "snd_b_zing") || !Q_stricmp(alias, "snd_ping")
	    || Q_stristr(alias, "supression") || Q_stristr(alias, "suppression")
	    || Q_stristr(alias, "hedgehog_ambient")) {
		return SND_CAT_IMPACTS;
	}
	if (Q_stristr(alias, "fake_snd_fire") || Q_stristr(alias, "_snd_fire") || Q_stristr(alias, "_fire1")
	    || Q_stristr(alias, "_fire2") || Q_stristr(alias, "_fire3") || Q_stristr(alias, "_fire4")
	    || Q_stristr(alias, "_fire5") || !Q_stricmp(alias, "jeep_snd_fire") || S_Cat_HasPrefix(alias, "nebelwerfer_launch")
	    || (S_Cat_HasPrefix(alias, "flak") && Q_stristr(alias, "fire"))
	    || S_Cat_HasPrefix(alias, "tank_snd_fire")) {
		return SND_CAT_FIRE;
	}
	if (Q_stristr(alias, "reload") || Q_stristr(alias, "boltpull") || Q_stristr(alias, "_snd_bolt")
	    || Q_stristr(alias, "_bolt_npc") || Q_stristr(alias, "pullout") || Q_stristr(alias, "putaway")
	    || Q_stristr(alias, "noammo") || Q_stristr(alias, "dryfire") || Q_stristr(alias, "rechamber")
	    || Q_stristr(alias, "gundrop") || Q_stristr(alias, "pistolwhip") || Q_stristr(alias, "pistolhit")
	    || Q_stristr(alias, "_whip") || !Q_stricmp(alias, "snd_binoculars") || !Q_stricmp(alias, "snd_papers")
	    || S_Cat_HasPrefix(alias, "heavy_drop") || S_Cat_HasPrefix(alias, "mg_drop")
	    || S_Cat_HasPrefix(alias, "pistol_drop") || S_Cat_HasPrefix(alias, "rifle_drop")
	    || S_Cat_HasPrefix(alias, "smg_drop") || S_Cat_HasPrefix(alias, "grenade_drop")
	    || S_Cat_HasPrefix(alias, "pistol_hit")
	    || S_Cat_HasPrefix(alias, "rifle_hit") || S_Cat_HasPrefix(alias, "pistol_whip")
	    || S_Cat_HasPrefix(alias, "rifle_whip")) {
		return SND_CAT_FOLEY;
	}
	if (Q_stristr(alias, "explode") || Q_stristr(alias, "explosion") || S_Cat_HasPrefix(alias, "arty_")
	    || S_Cat_HasPrefix(alias, "bazooka_exp") || S_Cat_HasPrefix(alias, "grenade_")
	    || S_Cat_HasPrefix(alias, "drop_bomb") || S_Cat_HasPrefix(alias, "short_circuit")
	    || S_Cat_HasPrefix(alias, "stonecrash") || S_Cat_HasPrefix(alias, "exp_")
	    || Q_stristr(alias, "higgins_explosion") || S_Cat_HasPrefix(alias, "flakk88_hit")
	    || Q_stristr(alias, "grenade_bounce") || Q_stristr(alias, "grenade_exp")) {
		return SND_CAT_EXPLOSIONS;
	}
	if (S_Cat_HasPrefix(alias, "punch") || S_Cat_HasPrefix(alias, "swing")) {
		return SND_CAT_FIRE;
	}
	if (S_Cat_IsVehicleAlias(alias)) {
		return SND_CAT_VEHICLES;
	}
	if (Q_stristr(alias, "pickup")) {
		return SND_CAT_WORLD;
	}

	return SND_CAT_AUTO;
}

static sndCategory_t S_Cat_FromPath(const char *path)
{
	if (!path || !path[0]) {
		return SND_CAT_AUTO;
	}

	if (S_Cat_HasPrefix(path, "sound/prom/hitmarkers/")) {
		return SND_CAT_NONE;
	}
	if (S_Cat_PathHas(path, "sound/weapons/fire/") || S_Cat_PathHas(path, "/weapons/fire/")) {
		return SND_CAT_FIRE;
	}
	if (S_Cat_PathHas(path, "sound/weapons/impact/") || S_Cat_PathHas(path, "/weapons/impact/")
	    || S_Cat_PathHas(path, "sound/weapons/zings/") || S_Cat_PathHas(path, "/weapons/zings/")) {
		return SND_CAT_IMPACTS;
	}
	if (S_Cat_PathHas(path, "sound/weapons/explo/") || S_Cat_PathHas(path, "/weapons/explo/")) {
		return SND_CAT_EXPLOSIONS;
	}
	if (S_Cat_PathHas(path, "sound/weapons/foley/") || S_Cat_PathHas(path, "/weapons/foley/")) {
		return SND_CAT_FOLEY;
	}
	if (S_Cat_PathHas(path, "sound/vehicle/") || S_Cat_PathHas(path, "/vehicle/")) {
		return SND_CAT_VEHICLES;
	}
	if (S_Cat_PathHas(path, "/characters/")) {
		if (S_Cat_PathHas(path, "body_fall") || S_Cat_PathHas(path, "bodyfall")) {
			return SND_CAT_BODYFALLS;
		}
		if (S_Cat_PathHas(path, "fs_") || S_Cat_PathHas(path, "body_mvmt") || S_Cat_PathHas(path, "foot")) {
			return SND_CAT_FOOTSTEPS;
		}
		if (S_Cat_PathHas(path, "dog_")) {
			return SND_CAT_VOICE;
		}
	}
	if (S_Cat_PathHas(path, "/mechanics/") || S_Cat_PathHas(path, "/doors/") || S_Cat_PathHas(path, "/items/")
	    || S_Cat_PathHas(path, "/props/")) {
		return SND_CAT_WORLD;
	}
	if (S_Cat_PathHas(path, "/amb/") || S_Cat_PathHas(path, "/ambient/") || S_Cat_PathHas(path, "/weather/")
	    || S_Cat_PathHas(path, "/amb_stereo/")) {
		return SND_CAT_NONE;
	}
	if (S_Cat_PathHas(path, "/dialogue/") || S_Cat_PathHas(path, "/dialog/")) {
		return SND_CAT_VOICE;
	}

	return SND_CAT_AUTO;
}

/*
==============
S_SoundCategory_FromChannel
==============
*/
sndCategory_t S_SoundCategory_FromChannel(int channel)
{
	switch (channel) {
	case CHAN_DIALOG:
	case CHAN_DIALOG_SECONDARY:
	case CHAN_VOICE:
		return SND_CAT_VOICE;
	case CHAN_WEAPON:
		return SND_CAT_FIRE;
	case CHAN_ITEM:
		return SND_CAT_WORLD;
	case CHAN_BODY:
		return SND_CAT_BODYFALLS;
	case CHAN_MENU:
	case CHAN_LOCAL:
	case CHAN_LOCAL_SOUND:
	case CHAN_ANNOUNCER:
		return SND_CAT_NONE;
	default:
		return SND_CAT_NONE;
	}
}

/*
==============
S_SoundCategory_ClassifyName
==============
*/
sndCategory_t S_SoundCategory_ClassifyName(const char *aliasOrNull, const char *sfxPath)
{
	sndCategory_t cat;

	/* Dialogue files are voice whatever the alias says (airstrike_*, dfr_reload*). */
	if (S_Cat_PathHas(sfxPath, "/dialogue/") || S_Cat_PathHas(sfxPath, "/dialog/")) {
		return SND_CAT_VOICE;
	}

	cat = S_Cat_FromAlias(aliasOrNull);
	if (cat != SND_CAT_AUTO) {
		return cat;
	}

	return S_Cat_FromPath(sfxPath);
}
