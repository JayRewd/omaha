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
#ifndef UID_DIAG_H
#define UID_DIAG_H

#include "uid_types.h"

#include <string>
#include <vector>

struct uid_diag_t {
	uid_severity_t        severity;
	uid_source_location_t location;
	std::string           pathStorage; /* owns location.path; import stacks may free */
	std::string           message;
};

class uid_diag_list_t {
public:
	explicit uid_diag_list_t(int maxDiagnostics = 256);

	void SetLimit(int maxDiagnostics);
	int  Limit() const { return m_maxDiagnostics; }

	void Clear();
	bool Empty() const { return m_items.empty(); }
	size_t Size() const { return m_items.size(); }
	bool Truncated() const { return m_truncated; }

	const std::vector<uid_diag_t> &Items() const { return m_items; }

	void Add(uid_severity_t severity, const uid_source_location_t &location, const char *message);
	void Add(uid_severity_t severity, const uid_source_location_t &location, const std::string &message);
	void Error(const uid_source_location_t &location, const char *message);
	void Error(const uid_source_location_t &location, const std::string &message);
	void Warning(const uid_source_location_t &location, const char *message);
	void Warning(const uid_source_location_t &location, const std::string &message);
	void Info(const uid_source_location_t &location, const char *message);
	void Info(const uid_source_location_t &location, const std::string &message);

	bool HasErrors() const;

private:
	void AddInternal(uid_severity_t severity, const uid_source_location_t &location, const std::string &message);

	std::vector<uid_diag_t> m_items;
	int                     m_maxDiagnostics;
	bool                    m_truncated;
};

#endif /* UID_DIAG_H */
