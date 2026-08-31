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

#include "uid_menu_map_view.h"
#include "uid_document.h"

#include "../uirender/uir_menu_map_view.h"

#include <cstdlib>
#include <cstring>

namespace {

const uid_source_item_def_t *FindSourceItem(const uid_document_t *doc, const char *value)
{
	if (!doc) {
		return nullptr;
	}
	auto srcIt = doc->definitions.sources.find(UIR_MENU_MAP_VIEW_SOURCE_ID);
	if (srcIt == doc->definitions.sources.end()) {
		return nullptr;
	}
	const uid_source_def_t &src = srcIt->second;
	if (src.items.empty()) {
		return nullptr;
	}

	if (value && value[0]) {
		for (size_t i = 0; i < src.items.size(); ++i) {
			if (src.items[i].value == value) {
				return &src.items[i];
			}
		}
	}

	if (!src.defaultValue.empty()) {
		for (size_t i = 0; i < src.items.size(); ++i) {
			if (src.items[i].value == src.defaultValue) {
				return &src.items[i];
			}
		}
	}

	return &src.items[0];
}

const char *FieldOrEmpty(const std::map<std::string, std::string> &fields, const char *key)
{
	auto it = fields.find(key);
	if (it == fields.end()) {
		return "";
	}
	return it->second.c_str();
}

bool ParseFloat(const char *text, float *out)
{
	if (!text || !text[0] || !out) {
		return false;
	}
	char *end = nullptr;
	const double v = std::strtod(text, &end);
	if (end == text) {
		return false;
	}
	*out = static_cast<float>(v);
	return true;
}

bool ParseVieworg(const char *text, float out[3])
{
	if (!text || !text[0] || !out) {
		return false;
	}
	char buf[128];
	strncpy(buf, text, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	char *tok = buf;
	char *comma = std::strchr(tok, ',');
	if (!comma) {
		return false;
	}
	*comma = '\0';
	if (!ParseFloat(tok, &out[0])) {
		return false;
	}
	tok = comma + 1;
	comma = std::strchr(tok, ',');
	if (!comma) {
		return false;
	}
	*comma = '\0';
	if (!ParseFloat(tok, &out[1])) {
		return false;
	}
	return ParseFloat(comma + 1, &out[2]);
}

void FillFromItem(const uid_source_item_def_t &item, uir_menu_map_view_t *out)
{
	UIR_MenuMapViewSetDefaults(out);
	if (!item.value.empty()) {
		strncpy(out->id, item.value.c_str(), sizeof(out->id) - 1);
		out->id[sizeof(out->id) - 1] = '\0';
	}
	const char *bsp = FieldOrEmpty(item.fields, "bsp");
	if (bsp[0]) {
		strncpy(out->bsp, bsp, sizeof(out->bsp) - 1);
		out->bsp[sizeof(out->bsp) - 1] = '\0';
	}
	ParseVieworg(FieldOrEmpty(item.fields, "vieworg"), out->vieworg);
	ParseFloat(FieldOrEmpty(item.fields, "pitch"), &out->pitch);
	ParseFloat(FieldOrEmpty(item.fields, "yaw"), &out->yaw);
	ParseFloat(FieldOrEmpty(item.fields, "roll"), &out->roll);
	ParseFloat(FieldOrEmpty(item.fields, "fov"), &out->fov);
}

} // namespace

bool UID_ResolveMenuMapView(const uid_document_t *doc, const char *value, uir_menu_map_view_t *out)
{
	if (!out) {
		return false;
	}
	const uid_source_item_def_t *item = FindSourceItem(doc, value);
	if (!item) {
		UIR_MenuMapViewSetDefaults(out);
		return false;
	}
	FillFromItem(*item, out);
	return true;
}
