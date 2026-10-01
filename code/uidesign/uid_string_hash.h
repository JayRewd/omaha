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
#ifndef UID_STRING_HASH_H
#define UID_STRING_HASH_H

#include <functional>
#include <string>
#include <string_view>

/*
 * Added in Omaha: transparent string keys for C++17 std::map (std::less<>).
 * Avoids temporary std::string construction on find(const char*).
 * Prefer std::map over unordered_map here — heterogeneous unordered lookup is C++20.
 */

struct uid_cstring_less {
	using is_transparent = void;

	bool operator()(std::string_view a, std::string_view b) const noexcept
	{
		return a < b;
	}
	bool operator()(const char *a, std::string_view b) const noexcept
	{
		return std::string_view(a ? a : "") < b;
	}
	bool operator()(std::string_view a, const char *b) const noexcept
	{
		return a < std::string_view(b ? b : "");
	}
	bool operator()(const std::string &a, std::string_view b) const noexcept
	{
		return std::string_view(a) < b;
	}
	bool operator()(std::string_view a, const std::string &b) const noexcept
	{
		return a < std::string_view(b);
	}
	bool operator()(const std::string &a, const std::string &b) const noexcept
	{
		return a < b;
	}
	bool operator()(const char *a, const char *b) const noexcept
	{
		return std::string_view(a ? a : "") < std::string_view(b ? b : "");
	}
	bool operator()(const std::string &a, const char *b) const noexcept
	{
		return std::string_view(a) < std::string_view(b ? b : "");
	}
	bool operator()(const char *a, const std::string &b) const noexcept
	{
		return std::string_view(a ? a : "") < std::string_view(b);
	}
};

#endif /* UID_STRING_HASH_H */
