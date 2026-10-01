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

#include "uid_invoke.h"

#include "../qcommon/q_shared.h"

#include <cstring>

namespace {

struct uid_invoke_entry_t {
	char          name[UID_INVOKE_MAX_NAME];
	uid_invoke_fn fn;
	void         *userdata;
	bool          used;
};

uid_invoke_entry_t g_invokes[UID_INVOKE_MAX_ENTRIES];

int FindInvokeIndex(const char *name)
{
	if (!name || !name[0]) {
		return -1;
	}
	for (int i = 0; i < UID_INVOKE_MAX_ENTRIES; ++i) {
		if (g_invokes[i].used && Q_stricmp(g_invokes[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

} // namespace

bool UID_RegisterInvoke(const char *name, uid_invoke_fn fn, void *userdata)
{
	if (!name || !name[0] || !fn) {
		return false;
	}
	if (std::strlen(name) >= UID_INVOKE_MAX_NAME) {
		return false;
	}

	const int existing = FindInvokeIndex(name);
	if (existing >= 0) {
		g_invokes[existing].fn = fn;
		g_invokes[existing].userdata = userdata;
		return true;
	}

	for (int i = 0; i < UID_INVOKE_MAX_ENTRIES; ++i) {
		if (!g_invokes[i].used) {
			std::strncpy(g_invokes[i].name, name, sizeof(g_invokes[i].name) - 1);
			g_invokes[i].name[sizeof(g_invokes[i].name) - 1] = '\0';
			g_invokes[i].fn = fn;
			g_invokes[i].userdata = userdata;
			g_invokes[i].used = true;
			return true;
		}
	}
	return false;
}

bool UID_UnregisterInvoke(const char *name)
{
	const int idx = FindInvokeIndex(name);
	if (idx < 0) {
		return false;
	}
	g_invokes[idx].used = false;
	g_invokes[idx].name[0] = '\0';
	g_invokes[idx].fn = nullptr;
	g_invokes[idx].userdata = nullptr;
	return true;
}

void UID_ClearInvokes(void)
{
	for (int i = 0; i < UID_INVOKE_MAX_ENTRIES; ++i) {
		g_invokes[i].used = false;
		g_invokes[i].name[0] = '\0';
		g_invokes[i].fn = nullptr;
		g_invokes[i].userdata = nullptr;
	}
}

bool UID_HasInvoke(const char *name)
{
	return FindInvokeIndex(name) >= 0;
}

bool UID_Invoke(const char *name)
{
	const int idx = FindInvokeIndex(name);
	if (idx < 0 || !g_invokes[idx].fn) {
		return false;
	}
	return g_invokes[idx].fn(g_invokes[idx].userdata);
}
