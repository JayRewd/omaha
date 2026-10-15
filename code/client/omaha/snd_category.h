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

#pragma once

#include "../../qcommon/q_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Added in Omaha: mix-bus categories for client volume sliders. */
typedef enum {
	SND_CAT_AUTO = 0, /* unclassified (no alias / path rule matched) */
	SND_CAT_FIRE,
	SND_CAT_FOLEY,
	SND_CAT_IMPACTS,
	SND_CAT_EXPLOSIONS,
	SND_CAT_FOOTSTEPS,
	SND_CAT_BODYFALLS,
	SND_CAT_VOICE,
	SND_CAT_VEHICLES,
	SND_CAT_WORLD,
	SND_CAT_NONE /* uncontrolled (ambience / UI / music / leftovers) */
} sndCategory_t;

/*
==============
S_SoundCategory_ClassifyName

Dialogue path, then alias, then wav path; SND_CAT_AUTO when nothing matches
(cacheable per sample). aliasOrNull / sfxPath may be NULL.
==============
*/
sndCategory_t S_SoundCategory_ClassifyName(const char *aliasOrNull, const char *sfxPath);
/* CHAN_* fallback for samples ClassifyName leaves at SND_CAT_AUTO. */
sndCategory_t S_SoundCategory_FromChannel(int channel);

#ifdef __cplusplus
}
#endif
