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

#include "uid_vars.h"

#include "uid_value.h"

#include <cctype>
#include <string>
#include <vector>

namespace {

bool IsVarIdChar(unsigned char c)
{
	return std::isalnum(c) || c == '_' || c == '-';
}

bool ParseVarRefInner(const std::string &inner, std::string *idOut)
{
	if (!idOut) {
		return false;
	}
	idOut->clear();
	size_t b = 0;
	while (b < inner.size() && std::isspace(static_cast<unsigned char>(inner[b]))) {
		++b;
	}
	size_t e = inner.size();
	while (e > b && std::isspace(static_cast<unsigned char>(inner[e - 1]))) {
		--e;
	}
	if (e <= b) {
		return false;
	}
	const std::string trimmed = inner.substr(b, e - b);
	if (trimmed.rfind("var.", 0) == 0 || trimmed.rfind("var:", 0) == 0) {
		const std::string id = trimmed.substr(4);
		if (id.empty() || (!std::isalpha(static_cast<unsigned char>(id[0])) && id[0] != '_')) {
			return false;
		}
		for (char c : id) {
			if (!IsVarIdChar(static_cast<unsigned char>(c))) {
				return false;
			}
		}
		*idOut = id;
		return true;
	}
	return false;
}

bool SubstituteVarRefsInString(
	const uid_document_t *doc,
	const std::string    &input,
	std::string          *out,
	std::string          *unknownVarOut
)
{
	if (!out) {
		return false;
	}
	out->clear();
	if (unknownVarOut) {
		unknownVarOut->clear();
	}
	size_t i = 0;
	while (i < input.size()) {
		if (input[i] != '{') {
			out->push_back(input[i++]);
			continue;
		}
		const size_t end = input.find('}', i + 1);
		if (end == std::string::npos) {
			out->push_back(input[i++]);
			continue;
		}
		std::string varId;
		const std::string inner = input.substr(i + 1, end - i - 1);
		if (ParseVarRefInner(inner, &varId)) {
			std::string value;
			if (!UID_LookupVar(doc, varId.c_str(), &value)) {
				if (unknownVarOut) {
					*unknownVarOut = varId;
				}
				return false;
			}
			out->append(value);
			i = end + 1;
			continue;
		}
		out->push_back('{');
		out->append(inner);
		out->push_back('}');
		i = end + 1;
	}
	return true;
}

bool SubstituteVarRefsInPropertySet(
	const uid_document_t *doc,
	uid_property_set_t   *props,
	std::string          *unknownVarOut
)
{
	if (!doc || !props) {
		return false;
	}
	for (const auto &kv : props->Attrs()) {
		if (kv.second.value.find('{') == std::string::npos) {
			continue;
		}
		std::string resolved;
		std::string unknown;
		if (!SubstituteVarRefsInString(doc, kv.second.value, &resolved, &unknown)) {
			if (unknownVarOut) {
				*unknownVarOut = unknown;
			}
			return false;
		}
		props->Set(kv.first.c_str(), resolved);
	}
	return true;
}

/*
 * Fixed in OPM: also walk foreachTemplateNodes — they are not in doc->nodes until
 * ExpandForeach clones them, so stroke="{var.fill-divider}" etc. never resolved.
 */
bool SubstituteVarRefsInNodeList(
	const uid_document_t       *doc,
	std::vector<uid_node_def_t> *nodes,
	std::string                *unknownVarOut
)
{
	if (!doc || !nodes) {
		return false;
	}
	for (uid_node_def_t &node : *nodes) {
		if (!SubstituteVarRefsInPropertySet(doc, &node.properties, unknownVarOut)) {
			return false;
		}
		if (!node.foreachTemplateNodes.empty()) {
			if (!SubstituteVarRefsInNodeList(doc, &node.foreachTemplateNodes, unknownVarOut)) {
				return false;
			}
		}
	}
	return true;
}

bool ReportUnknownVar(uid_document_t *doc, uid_diag_list_t *diags, const std::string &unknown)
{
	if (diags) {
		diags->Error(
			uid_source_location_t{doc->sourceName.c_str(), 0, 0},
			"unknown design var: " + unknown
		);
	}
	return false;
}

} // namespace

bool UID_LookupVar(const uid_document_t *doc, const char *name, std::string *valueOut)
{
	if (!doc || !name || !name[0] || !valueOut) {
		return false;
	}
	auto it = doc->definitions.vars.find(name);
	if (it == doc->definitions.vars.end()) {
		return false;
	}
	*valueOut = it->second.value;
	return true;
}

bool UID_LookupVarNumber(const uid_document_t *doc, const char *name, double *out)
{
	if (!doc || !name || !name[0] || !out) {
		return false;
	}
	std::string value;
	if (!UID_LookupVar(doc, name, &value)) {
		return false;
	}
	std::string dm;
	if (UID_ParseNumber(value.c_str(), out, &dm)) {
		return true;
	}
	uid_length_t len;
	if (UID_ParseLength(value.c_str(), &len, &dm) && len.unit == UID_LENGTH_PX) {
		*out = static_cast<double>(len.value);
		return true;
	}
	return false;
}

uid_result_t UID_ResolveDocumentVars(uid_document_t *doc, uid_diag_list_t *diags)
{
	if (!doc) {
		return UID_ERR_INVALID_ARG;
	}

	std::string unknown;
	if (!SubstituteVarRefsInPropertySet(doc, &doc->definitions.defaults, &unknown)) {
		ReportUnknownVar(doc, diags, unknown);
		return UID_ERR_VALIDATE;
	}

	for (auto &kv : doc->definitions.templates) {
		if (!SubstituteVarRefsInNodeList(doc, &kv.second.nodes, &unknown)) {
			ReportUnknownVar(doc, diags, unknown);
			return UID_ERR_VALIDATE;
		}
	}
	for (auto &kv : doc->definitions.modals) {
		if (!SubstituteVarRefsInNodeList(doc, &kv.second.nodes, &unknown)) {
			ReportUnknownVar(doc, diags, unknown);
			return UID_ERR_VALIDATE;
		}
	}
	if (!SubstituteVarRefsInNodeList(doc, &doc->nodes, &unknown)) {
		ReportUnknownVar(doc, diags, unknown);
		return UID_ERR_VALIDATE;
	}

	return UID_OK;
}
