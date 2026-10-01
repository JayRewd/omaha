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

#include "uid_diag.h"

uid_diag_list_t::uid_diag_list_t(int maxDiagnostics)
	: m_maxDiagnostics(maxDiagnostics > 0 ? maxDiagnostics : 256)
	, m_truncated(false)
{
	/* Reserve so pathStorage.c_str() pointers stay stable for ReportDiags. */
	m_items.reserve(static_cast<size_t>(m_maxDiagnostics) + 1u);
}

void uid_diag_list_t::SetLimit(int maxDiagnostics)
{
	m_maxDiagnostics = maxDiagnostics > 0 ? maxDiagnostics : 256;
	if (m_items.capacity() < static_cast<size_t>(m_maxDiagnostics) + 1u) {
		m_items.reserve(static_cast<size_t>(m_maxDiagnostics) + 1u);
	}
}

void uid_diag_list_t::Clear()
{
	m_items.clear();
	m_truncated = false;
}

void uid_diag_list_t::Add(uid_severity_t severity, const uid_source_location_t &location, const char *message)
{
	AddInternal(severity, location, message ? message : "");
}

void uid_diag_list_t::Add(uid_severity_t severity, const uid_source_location_t &location, const std::string &message)
{
	AddInternal(severity, location, message);
}

void uid_diag_list_t::Error(const uid_source_location_t &location, const char *message)
{
	Add(UID_SEVERITY_ERROR, location, message);
}

void uid_diag_list_t::Error(const uid_source_location_t &location, const std::string &message)
{
	Add(UID_SEVERITY_ERROR, location, message);
}

void uid_diag_list_t::Warning(const uid_source_location_t &location, const char *message)
{
	Add(UID_SEVERITY_WARNING, location, message);
}

void uid_diag_list_t::Warning(const uid_source_location_t &location, const std::string &message)
{
	Add(UID_SEVERITY_WARNING, location, message);
}

void uid_diag_list_t::Info(const uid_source_location_t &location, const char *message)
{
	Add(UID_SEVERITY_INFO, location, message);
}

void uid_diag_list_t::Info(const uid_source_location_t &location, const std::string &message)
{
	Add(UID_SEVERITY_INFO, location, message);
}

bool uid_diag_list_t::HasErrors() const
{
	for (const uid_diag_t &item : m_items) {
		if (item.severity == UID_SEVERITY_ERROR) {
			return true;
		}
	}
	return false;
}

void uid_diag_list_t::AddInternal(uid_severity_t severity, const uid_source_location_t &location, const std::string &message)
{
	if (m_truncated) {
		return;
	}

	if (static_cast<int>(m_items.size()) >= m_maxDiagnostics) {
		/* Keep up to maxDiagnostics entries, then one truncation notice. */
		uid_diag_t trunc;
		trunc.severity = UID_SEVERITY_WARNING;
		trunc.pathStorage = location.path ? location.path : "";
		trunc.location.path = trunc.pathStorage.empty() ? nullptr : trunc.pathStorage.c_str();
		trunc.location.line = location.line;
		trunc.location.column = location.column;
		trunc.message = "diagnostic list truncated; additional messages discarded";
		m_items.push_back(trunc);
		m_truncated = true;
		return;
	}

	uid_diag_t item;
	item.severity = severity;
	item.pathStorage = location.path ? location.path : "";
	item.location.path = item.pathStorage.empty() ? nullptr : item.pathStorage.c_str();
	item.location.line = location.line;
	item.location.column = location.column;
	item.message = message;
	m_items.push_back(item);
	/* Rebind after push in case growth moved storage (should not with reserve). */
	uid_diag_t &stored = m_items.back();
	stored.location.path = stored.pathStorage.empty() ? nullptr : stored.pathStorage.c_str();
}
