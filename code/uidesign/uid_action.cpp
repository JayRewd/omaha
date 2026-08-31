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

#include "uid_action.h"

#include "uid_invoke.h"
#include "uid_modal.h"
#include "uid_value.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

struct PendingSet {
	uid_node_id_t nodeId;
	std::string   property;
	std::string   value;
	bool          affectsVisibility;
};

bool PropertyAffectsVisibility(const std::string &property)
{
	return property == "visible";
}

bool PropertyAffectsLayout(const std::string &property)
{
	return property == "visible" || property == "width" || property == "height" ||
		property == "padding" || property == "margin" || property == "gap" ||
		property == "type" || property == "halign" || property == "valign" ||
		property == "overflow" || property == "scrollbar-edge" || property == "font" ||
		property == "font-size" || property == "font-weight";
}

bool IsKnownBoolProp(const std::string &property)
{
	return property == "visible" || property == "enabled";
}

bool IsKnownColorProp(const std::string &property)
{
	return property == "fill" || property == "color" || property == "hoverfill" ||
		property == "hover-fill" || property == "pressed-fill" || property == "focus-fill" ||
		property == "disabled-fill" || property == "selected-fill" || property == "hover-color" || property == "pressed-color" ||
		property == "focus-color" || property == "disabled-color";
}

bool IsKnownNumberProp(const std::string &property)
{
	return property == "font-weight" || property == "tab-index" || property == "max-length";
}

bool IsKnownLengthProp(const std::string &property)
{
	return property == "width" || property == "height" || property == "padding" ||
		property == "margin" || property == "gap" || property == "font-size" ||
		property == "radius";
}

bool PropertyExistsOnTarget(const uid_node_def_t &target, const std::string &property)
{
	if (property.empty()) {
		return false;
	}
	if (target.properties.Has(property.c_str())) {
		return true;
	}
	if (UID_BuiltinDefault(property.c_str()) != nullptr) {
		return true;
	}
	/* Known style overrides may be set for the first time via <set>. */
	return IsKnownColorProp(property) || IsKnownBoolProp(property) ||
		IsKnownNumberProp(property) || IsKnownLengthProp(property) ||
		property == "font" || property == "shape" || property == "type" ||
		property == "halign" || property == "valign" || property == "overflow" ||
		property == "scrollbar-edge";
}

bool ValidateTypedValue(const std::string &property, const std::string &value)
{
	if (IsKnownBoolProp(property)) {
		bool b = false;
		return UID_ParseBool(value.c_str(), &b, nullptr);
	}
	if (IsKnownColorProp(property)) {
		if ((property == "fill" || property == "hoverfill" || property == "hover-fill" ||
			 property == "pressed-fill" || property == "focus-fill" || property == "disabled-fill" ||
			 property == "selected-fill") &&
			UID_IsGradientBrush(value.c_str())) {
			return true;
		}
		uid_color_t c;
		return UID_ParseColor(value.c_str(), &c, nullptr);
	}
	if (IsKnownNumberProp(property)) {
		double n = 0.0;
		return UID_ParseNumber(value.c_str(), &n, nullptr);
	}
	if (IsKnownLengthProp(property)) {
		uid_length_t len;
		return UID_ParseLength(value.c_str(), &len, nullptr);
	}
	return true;
}

} // namespace

uid_result_t UID_DispatchEvent(
	uid_document_t *doc,
	uid_node_id_t nodeId,
	uid_event_kind_t event,
	const uid_backend_t *backend
)
{
	if (!doc) {
		return UID_ERR_INVALID_ARG;
	}

	uid_node_def_t *node = UID_GetNode(doc, nodeId);
	if (!node) {
		return UID_ERR_INVALID_ARG;
	}

	std::vector<PendingSet> pendingSets;
	uid_dirty_flags_t dirty = UID_DIRTY_NONE;
	uid_result_t result = UID_OK;

	for (const uid_action_handler_t &handler : node->handlers) {
		if (handler.event != event) {
			continue;
		}

		pendingSets.clear();

		/* Prevalidate every SET: target, property existence, typed value. */
		bool setsOk = true;
		for (const uid_action_t &action : handler.actions) {
			if (action.kind != UID_NODE_SET) {
				continue;
			}
			auto it = doc->idIndex.find(action.target);
			if (it == doc->idIndex.end() || action.property.empty()) {
				setsOk = false;
				break;
			}
			const uid_node_def_t *target = UID_GetNode(doc, it->second);
			if (!target || !PropertyExistsOnTarget(*target, action.property)) {
				setsOk = false;
				break;
			}
			if (!ValidateTypedValue(action.property, action.value)) {
				setsOk = false;
				break;
			}
			PendingSet ps;
			ps.nodeId = it->second;
			ps.property = action.property;
			ps.value = action.value;
			ps.affectsVisibility = PropertyAffectsVisibility(action.property);
			pendingSets.push_back(ps);
		}

		/* Prevalidate engine actions for capability before any mutation. */
		bool engineOk = true;
		for (const uid_action_t &action : handler.actions) {
			if (action.kind == UID_NODE_SET_CVAR) {
				if (!backend || !backend->cvarWrite) {
					engineOk = false;
					break;
				}
				const char *cvarName = !action.name.empty() ? action.name.c_str() : action.target.c_str();
				if (!cvarName || !cvarName[0]) {
					engineOk = false;
					break;
				}
			} else if (action.kind == UID_NODE_INVOKE) {
				const char *invokeName = !action.name.empty() ? action.name.c_str() : action.target.c_str();
				if (!invokeName || !invokeName[0]) {
					engineOk = false;
					break;
				}
				if (!UID_HasInvoke(invokeName) && (!backend || !backend->invokeAction)) {
					engineOk = false;
					break;
				}
			} else if (action.kind == UID_NODE_SHOW_MODAL || action.kind == UID_NODE_HIDE_MODAL) {
				if (!backend || !backend->cvarWrite) {
					engineOk = false;
					break;
				}
			}
		}

		if (!setsOk || !engineOk) {
			result = UID_ERR_VALIDATE;
			continue; /* apply none of this handler's document sets */
		}

		for (const PendingSet &ps : pendingSets) {
			uid_node_def_t *target = UID_GetNode(doc, ps.nodeId);
			if (!target) {
				continue;
			}
			target->properties.Set(ps.property.c_str(), ps.value);
			if (ps.affectsVisibility) {
				dirty = static_cast<uid_dirty_flags_t>(
					dirty | UID_DIRTY_STRUCTURE | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT
				);
			} else if (PropertyAffectsLayout(ps.property)) {
				dirty = static_cast<uid_dirty_flags_t>(dirty | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT);
			} else {
				dirty = static_cast<uid_dirty_flags_t>(dirty | UID_DIRTY_PAINT);
			}
		}

		for (const uid_action_t &action : handler.actions) {
			if (action.kind == UID_NODE_SET) {
				continue;
			}
			if (action.kind == UID_NODE_SET_CVAR) {
				const char *cvarName = !action.name.empty() ? action.name.c_str() : action.target.c_str();
				if (!backend->cvarWrite(cvarName, action.value.c_str())) {
					result = UID_ERR_VALIDATE;
				} else {
					dirty = static_cast<uid_dirty_flags_t>(dirty | UID_DIRTY_BINDING | UID_DIRTY_PAINT);
				}
				continue;
			}
			if (action.kind == UID_NODE_INVOKE) {
				const char *invokeName = !action.name.empty() ? action.name.c_str() : action.target.c_str();
				bool invoked = UID_Invoke(invokeName);
				if (!invoked && backend && backend->invokeAction) {
					invoked = backend->invokeAction(invokeName, backend->userdata) ? true : false;
				}
				if (!invoked) {
					result = UID_ERR_VALIDATE;
				}
				continue;
			}
			if (action.kind == UID_NODE_SHOW_MODAL) {
				const char *cvarName = action.target.empty() ? UID_DefaultModalCvarName() : action.target.c_str();
				/* Added in OPM: remember opener for type=relative modal placement. */
				doc->modalOpenerNode = nodeId;
				if (!backend->cvarWrite(cvarName, action.name.c_str())) {
					result = UID_ERR_VALIDATE;
				} else {
					dirty = static_cast<uid_dirty_flags_t>(dirty | UID_DIRTY_BINDING | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT);
				}
				continue;
			}
			if (action.kind == UID_NODE_HIDE_MODAL) {
				const char *cvarName =
					!action.name.empty() ? action.name.c_str() : UID_DefaultModalCvarName();
				if (!backend->cvarWrite(cvarName, "")) {
					result = UID_ERR_VALIDATE;
				} else {
					dirty = static_cast<uid_dirty_flags_t>(dirty | UID_DIRTY_BINDING | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT);
				}
				continue;
			}
		}
	}

	doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty | dirty);
	return result;
}
