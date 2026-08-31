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

#include "uid_opt.h"

/* Added in OPM: default all optimizations on. */
static unsigned g_uidOptFlags = UID_OPT_ALL;

void UID_SetOptFlags(unsigned flags)
{
	g_uidOptFlags = flags;
}

unsigned UID_OptFlags(void)
{
	return g_uidOptFlags;
}

int UID_OptEnabled(unsigned flag)
{
	return (g_uidOptFlags & flag) != 0 ? 1 : 0;
}
