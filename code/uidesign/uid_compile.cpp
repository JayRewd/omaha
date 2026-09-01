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

#include "uid_compile.h"

#include "uid_binding.h"
#include "uid_document.h"
#include "uid_scrollbar.h"
#include "uid_shape.h"
#include "uid_value.h"

#include "../uirender/uir_gradient.h"

#include <cmath>
#include <cstring>
#include <string>

namespace {

bool LooksDeferredExpr(const std::string &v)
{
	return v.find('{') != std::string::npos;
}

void RebindSourcePaths(uid_document_t *doc)
{
	if (!doc) {
		return;
	}
	const char *owned = doc->sourceName.c_str();
	for (uid_node_def_t &node : doc->nodes) {
		node.source.path = owned;
		for (uid_action_handler_t &handler : node.handlers) {
			handler.loc.path = owned;
			for (uid_action_t &act : handler.actions) {
				act.loc.path = owned;
			}
		}
	}
	for (auto &kv : doc->definitions.shapes) {
		for (uid_path_def_t &path : kv.second.paths) {
			path.loc.path = owned;
		}
	}
	for (auto &kv : doc->definitions.templates) {
		for (uid_node_def_t &node : kv.second.nodes) {
			node.source.path = owned;
			for (uid_action_handler_t &handler : node.handlers) {
				handler.loc.path = owned;
				for (uid_action_t &act : handler.actions) {
					act.loc.path = owned;
				}
			}
		}
	}
}

bool ValidateLengthAttr(
	uid_diag_list_t *diags,
	const uid_source_location_t &loc,
	const char *attr,
	const std::string &value
)
{
	if (LooksDeferredExpr(value)) {
		return true;
	}
	uid_length_t len;
	std::string dm;
	if (!UID_ParseLength(value.c_str(), &len, &dm)) {
		if (diags) {
			diags->Error(loc, dm.empty() ? (std::string("invalid length for ") + attr) : dm);
		}
		return false;
	}
	if (len.unit == UID_LENGTH_PX || len.unit == UID_LENGTH_PERCENT) {
		if (!std::isfinite(static_cast<double>(len.value))) {
			if (diags) {
				diags->Error(loc, std::string("non-finite length for ") + attr);
			}
			return false;
		}
	}
	return true;
}

/* Added in Omaha: max-width / max-height must be definite px or % (not auto/fill). */
bool ValidateMaxLengthAttr(
	uid_diag_list_t *diags,
	const uid_source_location_t &loc,
	const char *attr,
	const std::string &value
)
{
	if (LooksDeferredExpr(value)) {
		return true;
	}
	uid_length_t len;
	std::string dm;
	if (!UID_ParseLength(value.c_str(), &len, &dm)) {
		if (diags) {
			diags->Error(loc, dm.empty() ? (std::string("invalid length for ") + attr) : dm);
		}
		return false;
	}
	if (len.unit != UID_LENGTH_PX && len.unit != UID_LENGTH_PERCENT) {
		if (diags) {
			diags->Error(loc, std::string(attr) + " must be px or % (not auto/fill)");
		}
		return false;
	}
	if (!std::isfinite(static_cast<double>(len.value))) {
		if (diags) {
			diags->Error(loc, std::string("non-finite length for ") + attr);
		}
		return false;
	}
	return true;
}

bool ValidateSidesAttr(
	uid_diag_list_t *diags,
	const uid_source_location_t &loc,
	const char *attr,
	const std::string &value
)
{
	if (LooksDeferredExpr(value)) {
		return true;
	}
	uid_sides_t sides;
	std::string dm;
	if (!UID_ParseSides(value.c_str(), &sides, &dm)) {
		if (diags) {
			diags->Error(loc, dm.empty() ? (std::string("invalid sides for ") + attr) : dm);
		}
		return false;
	}
	const uid_length_t *parts[4] = {&sides.top, &sides.right, &sides.bottom, &sides.left};
	for (const uid_length_t *p : parts) {
		if ((p->unit == UID_LENGTH_PX || p->unit == UID_LENGTH_PERCENT) &&
			!std::isfinite(static_cast<double>(p->value))) {
			if (diags) {
				diags->Error(loc, std::string("non-finite sides value for ") + attr);
			}
			return false;
		}
	}
	return true;
}

bool ValidateNumberAttr(
	uid_diag_list_t *diags,
	const uid_source_location_t &loc,
	const char *attr,
	const std::string &value
)
{
	if (LooksDeferredExpr(value)) {
		return true;
	}
	double n = 0.0;
	std::string dm;
	if (!UID_ParseNumber(value.c_str(), &n, &dm)) {
		if (diags) {
			diags->Error(loc, dm.empty() ? (std::string("invalid number for ") + attr) : dm);
		}
		return false;
	}
	if (!std::isfinite(n)) {
		if (diags) {
			diags->Error(loc, std::string("non-finite number for ") + attr);
		}
		return false;
	}
	return true;
}

bool ValidateColorAttr(
	uid_diag_list_t *diags,
	const uid_source_location_t &loc,
	const char *attr,
	const std::string &value
)
{
	if (LooksDeferredExpr(value)) {
		return true;
	}
	if (value.compare(0, 10, "cvar-rgba:") == 0) {
		return true;
	}
	/* Added in OPM: fill* attrs may be atlas gradient brushes. */
	const bool fillLike = attr && (std::strcmp(attr, "fill") == 0 || std::strcmp(attr, "hoverfill") == 0 ||
		std::strcmp(attr, "hover-fill") == 0 || std::strcmp(attr, "pressed-fill") == 0 ||
		std::strcmp(attr, "focus-fill") == 0 || std::strcmp(attr, "disabled-fill") == 0 ||
		std::strcmp(attr, "selected-fill") == 0);
	if (fillLike && UID_IsGradientBrush(value.c_str())) {
		uir_gradient_t g;
		if (UIR_GradientParse(value.c_str(), &g) != UIR_OK) {
			if (diags) {
				diags->Error(loc, std::string("invalid gradient for ") + attr);
			}
			return false;
		}
		return true;
	}
	uid_color_t c;
	std::string dm;
	if (!UID_ParseColor(value.c_str(), &c, &dm)) {
		if (diags) {
			diags->Error(loc, dm.empty() ? (std::string("invalid color for ") + attr) : dm);
		}
		return false;
	}
	return true;
}

bool ValidateBoolAttr(
	uid_diag_list_t *diags,
	const uid_source_location_t &loc,
	const char *attr,
	const std::string &value
)
{
	if (LooksDeferredExpr(value)) {
		return true;
	}
	bool b = false;
	std::string dm;
	if (!UID_ParseBool(value.c_str(), &b, &dm)) {
		if (diags) {
			diags->Error(loc, dm.empty() ? (std::string("invalid bool for ") + attr) : dm);
		}
		return false;
	}
	return true;
}

bool ValidateKnownAttrs(uid_diag_list_t *diags, const uid_node_def_t &node)
{
	bool ok = true;
	for (const auto &kv : node.properties.Attrs()) {
		const std::string &name = kv.first;
		const std::string &value = kv.second.value;

		if (name == "width" || name == "height" || name == "gap" || name == "font-size" ||
			name == "radius" || name == "stroke-width" || name == "translate-x" || name == "translate-y") {
			ok = ValidateLengthAttr(diags, node.source, name.c_str(), value) && ok;
		} else if (name == "max-width" || name == "max-height") {
			/* Added in Omaha */
			ok = ValidateMaxLengthAttr(diags, node.source, name.c_str(), value) && ok;
		} else if (name == "padding" || name == "margin") {
			ok = ValidateSidesAttr(diags, node.source, name.c_str(), value) && ok;
		} else if (name == "font-weight" || name == "tab-index" || name == "max-length" ||
				   name == "min" || name == "max" || name == "step") {
			ok = ValidateNumberAttr(diags, node.source, name.c_str(), value) && ok;
		} else if (name == "shape-rotation" || name == "rotation") {
			if (!LooksDeferredExpr(value)) {
				float deg = 0.0f;
				std::string dm;
				if (!UID_ParseRotationDeg(value.c_str(), &deg, &dm)) {
					if (diags) {
						diags->Error(node.source, dm.empty() ? "invalid rotation" : dm);
					}
					ok = false;
				}
			}
		} else if (name == "rotation-origin") {
			if (!LooksDeferredExpr(value)) {
				float ox = 0.0f;
				float oy = 0.0f;
				std::string dm;
				if (!UID_ParseRotationOrigin(value.c_str(), 100.0f, 100.0f, 1.0f, &ox, &oy, &dm)) {
					if (diags) {
						diags->Error(node.source, dm.empty() ? "invalid rotation-origin" : dm);
					}
					ok = false;
				}
			}
		} else if (name == "opacity") {
			if (!LooksDeferredExpr(value)) {
				double v = 0.0;
				std::string dm;
				if (!UID_ParseNumber(value.c_str(), &v, &dm) || v < 0.0 || v > 1.0) {
					if (diags) {
						diags->Error(node.source, dm.empty() ? "opacity must be 0.0..1.0" : dm);
					}
					ok = false;
				}
			}
		} else if (name == "left" || name == "top" || name == "right" || name == "bottom") {
			if (!LooksDeferredExpr(value)) {
				ok = ValidateNumberAttr(diags, node.source, name.c_str(), value) && ok;
			}
		} else if (name == "fill" || name == "color" || name == "hoverfill" ||
				   name == "pressed-fill" || name == "focus-fill" || name == "disabled-fill" ||
				   name == "selected-fill" ||
				   name == "hover-color" || name == "pressed-color" || name == "focus-color" ||
				   name == "disabled-color" || name == "stroke") {
			ok = ValidateColorAttr(diags, node.source, name.c_str(), value) && ok;
		} else if (name == "visible" || name == "enabled" || name == "drop-shadow" ||
				   name == "stroke-layout") {
			ok = ValidateBoolAttr(diags, node.source, name.c_str(), value) && ok;
		} else if (name == "background-fit" || name == "mask-fit" || name == "fit") {
			if (!LooksDeferredExpr(value)) {
				uid_image_fit_t fit;
				std::string dm;
				if (!UID_ParseImageFit(value.c_str(), &fit, &dm)) {
					if (diags) {
						diags->Error(node.source, dm.empty() ? (std::string("invalid ") + name) : dm);
					}
					ok = false;
				} else if (name == "mask-fit" && fit == UID_IMAGE_FIT_REPEAT) {
					if (diags) {
						diags->Error(node.source, "mask-fit does not support repeat");
					}
					ok = false;
				}
			}
		} else if (name == "background-scale" || name == "scale") {
			if (!LooksDeferredExpr(value)) {
				double v = 0.0;
				std::string dm;
				if (!UID_ParseNumber(value.c_str(), &v, &dm) || v <= 0.0) {
					if (diags) {
						diags->Error(node.source, dm.empty() ? (std::string(name) + " must be > 0") : dm);
					}
					ok = false;
				}
			}
		} else if (name == "type") {
			if (!LooksDeferredExpr(value) && node.kind == UID_NODE_CONTAINER) {
				uid_layout_axis_t axis;
				std::string dm;
				if (!UID_ParseAxis(value.c_str(), &axis, &dm)) {
					if (diags) {
						diags->Error(node.source, dm.empty() ? "invalid layout type" : dm);
					}
					ok = false;
				}
			}
		} else if (name == "halign" || name == "valign") {
			if (!LooksDeferredExpr(value)) {
				uid_align_t align;
				std::string dm;
				if (!UID_ParseAlign(value.c_str(), &align, &dm)) {
					if (diags) {
						diags->Error(node.source, dm.empty() ? "invalid align" : dm);
					}
					ok = false;
				}
			}
		} else if (name == "overflow") {
			if (!LooksDeferredExpr(value)) {
				uid_overflow_t overflow;
				std::string dm;
				if (!UID_ParseOverflow(value.c_str(), &overflow, &dm)) {
					if (diags) {
						diags->Error(node.source, dm.empty() ? "invalid overflow" : dm);
					}
					ok = false;
				}
			}
		} else if (name == "text-wrap") {
			if (!LooksDeferredExpr(value) && value != "none" && value != "word") {
				if (diags) {
					diags->Error(node.source, "text-wrap must be none or word");
				}
				ok = false;
			}
		} else if (name == "text-overflow") {
			/* Added in Omaha: paint-time ellipsis for single-line labels. */
			if (!LooksDeferredExpr(value) && value != "none" && value != "ellipsis") {
				if (diags) {
					diags->Error(node.source, "text-overflow must be none or ellipsis");
				}
				ok = false;
			}
		} else if (name == "line-height") {
			if (!LooksDeferredExpr(value)) {
				uid_length_t len;
				std::string dm;
				double mult = 0.0;
				if (!UID_ParseLength(value.c_str(), &len, &dm) && !UID_ParseNumber(value.c_str(), &mult, &dm)) {
					if (diags) {
						diags->Error(node.source, "invalid line-height");
					}
					ok = false;
				}
			}
		} else if (name == "scrollbar-edge") {
			if (!LooksDeferredExpr(value)) {
				uid_scrollbar_edge_t edge;
				std::string dm;
				if (!UID_ParseScrollbarEdge(value.c_str(), &edge, &dm)) {
					if (diags) {
						diags->Error(node.source, dm.empty() ? "invalid scrollbar-edge" : dm);
					}
					ok = false;
				}
			}
		}
	}

	if (node.kind == UID_NODE_CONTAINER) {
		std::string type;
		if (node.properties.Get("type", &type) && type == "overlap") {
			std::string halign;
			std::string valign;
			if ((node.properties.Get("halign", &halign) && !halign.empty()) ||
			    (node.properties.Get("valign", &valign) && !valign.empty())) {
				if (diags) {
					diags->Warning(
						node.source,
						"overlap containers position children via each child's halign/valign, not the parent's"
					);
				}
			}
		}

	}

	if (node.hasMin && !std::isfinite(node.minValue)) {
		if (diags) {
			diags->Error(node.source, "non-finite min");
		}
		ok = false;
	}
	if (node.hasMax && !std::isfinite(node.maxValue)) {
		if (diags) {
			diags->Error(node.source, "non-finite max");
		}
		ok = false;
	}
	if (node.hasStep && !std::isfinite(node.stepValue)) {
		if (diags) {
			diags->Error(node.source, "non-finite step");
		}
		ok = false;
	}

	/* Added in OPM: stroke-width requires stroke on the using element. */
	{
		std::string stroke;
		std::string strokeWidth;
		const bool hasStroke = node.properties.Get("stroke", &stroke) && !stroke.empty();
		const bool hasStrokeWidth = node.properties.Get("stroke-width", &strokeWidth) && !strokeWidth.empty();
		if (hasStrokeWidth && !hasStroke) {
			if (diags) {
				diags->Error(node.source, "stroke-width requires stroke");
			}
			ok = false;
		}
	}
	return ok;
}

bool ValidateBindingsAndActions(
	uid_document_t *doc,
	uid_diag_list_t *diags,
	const uid_node_def_t &node
)
{
	bool ok = true;

	std::string bind = node.bind;
	if (bind.empty()) {
		node.properties.Get("bind", &bind);
	}
	if (!bind.empty() && !LooksDeferredExpr(bind)) {
		std::string cvar;
		if (!UID_ParseCvarBind(bind.c_str(), &cvar) || cvar.empty()) {
			if (diags) {
				diags->Error(node.source, "invalid bind; expected cvar:name");
			}
			ok = false;
		}
	}

	for (const uid_action_handler_t &handler : node.handlers) {
		for (const uid_action_t &act : handler.actions) {
			if (act.kind == UID_NODE_SET) {
				if (act.target.empty()) {
					if (diags) {
						diags->Error(act.loc, "<set> requires target");
					}
					ok = false;
				} else if (doc->idIndex.find(act.target) == doc->idIndex.end()) {
					if (diags) {
						diags->Error(act.loc, "set target not found: " + act.target);
					}
					ok = false;
				}
			} else if (act.kind == UID_NODE_SET_CVAR) {
				if (act.name.empty()) {
					if (diags) {
						diags->Error(act.loc, "<set-cvar> requires name");
					}
					ok = false;
				}
			} else if (act.kind == UID_NODE_INVOKE) {
				if (act.name.empty()) {
					if (diags) {
						diags->Error(act.loc, "<invoke> requires nonempty name");
					}
					ok = false;
				}
			} else if (act.kind == UID_NODE_SHOW_MODAL) {
				if (act.name.empty()) {
					if (diags) {
						diags->Error(act.loc, "<show-modal> requires id");
					}
					ok = false;
				} else if (doc->definitions.modals.find(act.name) == doc->definitions.modals.end()) {
					if (diags) {
						diags->Error(act.loc, "unknown modal id: " + act.name);
					}
					ok = false;
				}
			} else if (act.kind == UID_NODE_HIDE_MODAL) {
				/* hide-modal always valid */
			}
		}
	}
	return ok;
}

bool ValidateShapeInstance(
	uid_document_t *doc,
	uid_diag_list_t *diags,
	const uid_node_def_t &node
)
{
	std::string shapeId = node.shapeId;
	if (shapeId.empty()) {
		node.properties.Get("shape", &shapeId);
	}
	/* Decorative shape= on ordinary boxes is optional; only shape instances must resolve. */
	if (node.kind != UID_NODE_SHAPE_INSTANCE) {
		if (!shapeId.empty() && doc->definitions.shapes.find(shapeId) == doc->definitions.shapes.end()) {
			if (diags) {
				diags->Error(node.source, "unknown shape: " + shapeId);
			}
			return false;
		}
		if (!shapeId.empty()) {
			auto sit = doc->definitions.shapes.find(shapeId);
			uid_property_set_t built;
			if (UID_BuildShapeInstanceProps(&sit->second, &node.properties, &built, diags, node.source) !=
				UID_OK) {
				return false;
			}
		}
		return true;
	}

	if (shapeId.empty()) {
		if (diags) {
			diags->Error(node.source, "shape instance missing shape id");
		}
		return false;
	}
	auto sit = doc->definitions.shapes.find(shapeId);
	if (sit == doc->definitions.shapes.end()) {
		if (diags) {
			diags->Error(node.source, "unknown shape: " + shapeId);
		}
		return false;
	}
	uid_property_set_t built;
	return UID_BuildShapeInstanceProps(&sit->second, &node.properties, &built, diags, node.source) ==
		UID_OK;
}

bool ValidateFontRef(uid_document_t *doc, uid_diag_list_t *diags, const uid_node_def_t &node)
{
	std::string fontId;
	if (!node.properties.Get("font", &fontId) || fontId.empty()) {
		return true;
	}
	if (doc->definitions.fonts.find(fontId) == doc->definitions.fonts.end()) {
		/* Allow unresolved font id only when registry is empty (minimal docs). */
		if (!doc->definitions.fonts.empty()) {
			if (diags) {
				diags->Error(node.source, "unknown font: " + fontId);
			}
			return false;
		}
	}
	return true;
}

bool ValidateImageRef(uid_document_t *doc, uid_diag_list_t *diags, const uid_node_def_t &node)
{
	std::string imageId;
	const char *propName = "background-image";
	if (node.kind == UID_NODE_IMAGE) {
		propName = "src";
		if (!node.properties.Get("src", &imageId) || imageId.empty()) {
			(void)node.properties.Get("background-image", &imageId);
		}
	} else if (!node.properties.Get("background-image", &imageId) || imageId.empty()) {
		return true;
	}
	if (imageId.empty()) {
		return true;
	}
	if (LooksDeferredExpr(imageId)) {
		return true;
	}
	if (imageId.find('/') != std::string::npos && imageId.find("..") == std::string::npos &&
		!imageId.empty() && imageId[0] != '/') {
		return true;
	}
	if (doc->definitions.images.find(imageId) == doc->definitions.images.end()) {
		if (!doc->definitions.images.empty()) {
			if (diags) {
				diags->Error(node.source, std::string("unknown ") + propName + ": " + imageId);
			}
			return false;
		}
	}
	return true;
}

/* Added in OPM: mask-image registry id, VFS path, or linear/radial gradient brush. */
bool ValidateMaskImageRef(uid_document_t *doc, uid_diag_list_t *diags, const uid_node_def_t &node)
{
	std::string imageId;
	if (!node.properties.Get("mask-image", &imageId) || imageId.empty()) {
		return true;
	}
	if (LooksDeferredExpr(imageId)) {
		return true;
	}
	if (UID_IsGradientBrush(imageId.c_str())) {
		return true;
	}
	if (imageId.find('/') != std::string::npos && imageId.find("..") == std::string::npos &&
		!imageId.empty() && imageId[0] != '/') {
		return true;
	}
	if (doc->definitions.images.find(imageId) == doc->definitions.images.end()) {
		if (!doc->definitions.images.empty()) {
			if (diags) {
				diags->Error(node.source, "unknown mask-image: " + imageId);
			}
			return false;
		}
	}
	return true;
}

/* Added in OPM */
bool ValidateSliderParts(
	uid_document_t *doc,
	uid_diag_list_t *diags,
	uid_node_id_t id,
	uid_node_kind_t parentKind
)
{
	const uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node) {
		return true;
	}

	bool ok = true;
	if (UID_IsSliderPartKind(node->kind)) {
		if (parentKind != UID_NODE_SLIDER) {
			if (diags) {
				diags->Error(node->source, "slider part must be a direct child of <slider>");
			}
			ok = false;
		}
		return ok;
	}

	if (node->kind == UID_NODE_SLIDER) {
		bool haveTrack = false;
		bool haveRange = false;
		bool haveThumb = false;
		for (uid_node_id_t c : node->children) {
			const uid_node_def_t *child = UID_GetNode(doc, c);
			if (!child) {
				continue;
			}
			if (child->kind == UID_NODE_SLIDER_TRACK) {
				if (haveTrack) {
					if (diags) {
						diags->Error(child->source, "duplicate <track> under <slider>");
					}
					ok = false;
				}
				haveTrack = true;
			} else if (child->kind == UID_NODE_SLIDER_RANGE) {
				if (haveRange) {
					if (diags) {
						diags->Error(child->source, "duplicate <range> under <slider>");
					}
					ok = false;
				}
				haveRange = true;
			} else if (child->kind == UID_NODE_SLIDER_THUMB) {
				if (haveThumb) {
					if (diags) {
						diags->Error(child->source, "duplicate <thumb> under <slider>");
					}
					ok = false;
				}
				haveThumb = true;
			}
		}
		if (!UID_SyncSliderBounds(const_cast<uid_node_def_t *>(node))) {
			if (diags) {
				diags->Error(node->source, "<slider> requires valid min, max, and step");
			}
			ok = false;
		}
	}

	for (uid_node_id_t c : node->children) {
		ok = ValidateSliderParts(doc, diags, c, node->kind) && ok;
	}
	return ok;
}

/* Added in OPM */
bool ValidateScrollbarParts(
	uid_document_t *doc,
	uid_diag_list_t *diags,
	uid_node_id_t id,
	uid_node_kind_t parentKind
)
{
	const uid_node_def_t *node = UID_GetNode(doc, id);
	if (!node) {
		return true;
	}

	bool ok = true;
	if (UID_IsScrollbarPartKind(node->kind)) {
		if (parentKind != UID_NODE_SCROLLBAR) {
			if (diags) {
				diags->Error(node->source, "scrollbar part must be a direct child of <scrollbar>");
			}
			ok = false;
		}
		return ok;
	}

	if (node->kind == UID_NODE_SCROLLBAR) {
		bool haveTrack = false;
		bool haveThumb = false;
		for (uid_node_id_t c : node->children) {
			const uid_node_def_t *child = UID_GetNode(doc, c);
			if (!child) {
				continue;
			}
			if (child->kind == UID_NODE_SCROLLBAR_TRACK) {
				if (haveTrack) {
					if (diags) {
						diags->Error(child->source, "duplicate <track> under <scrollbar>");
					}
					ok = false;
				}
				haveTrack = true;
			} else if (child->kind == UID_NODE_SCROLLBAR_THUMB) {
				if (haveThumb) {
					if (diags) {
						diags->Error(child->source, "duplicate <thumb> under <scrollbar>");
					}
					ok = false;
				}
				haveThumb = true;
			}
		}
	}

	for (uid_node_id_t c : node->children) {
		ok = ValidateScrollbarParts(doc, diags, c, node->kind) && ok;
	}
	return ok;
}

/* Added in OPM */
bool ValidateModelNode(uid_diag_list_t *diags, const uid_node_def_t &node)
{
	if (node.kind != UID_NODE_MODEL) {
		return true;
	}
	std::string team = node.team;
	if (team.empty()) {
		node.properties.Get("team", &team);
	}
	if (!team.empty() && team != "allies" && team != "axis") {
		if (diags) {
			diags->Error(node.source, "model team must be allies or axis");
		}
		return false;
	}
	if (team.empty() && node.modelPath.empty() && node.bind.empty()) {
		if (diags) {
			diags->Error(node.source, "model requires team, model=, or bind=");
		}
		return false;
	}
	if (node.hasAnimPhase && (node.animPhase < 0.0f || node.animPhase >= 1.0f)) {
		if (diags) {
			diags->Error(node.source, "model anim-phase must be in [0,1)");
		}
		return false;
	}
	if (node.hasAnimVariant && node.animVariant < 0) {
		if (diags) {
			diags->Error(node.source, "model anim-variant must be >= 0");
		}
		return false;
	}
	if (node.hasModelFov && (node.modelFov <= 0.0f || node.modelFov >= 180.0f)) {
		if (diags) {
			diags->Error(node.source, "model fov must be in (0,180)");
		}
		return false;
	}
	if (node.hasModelScale && node.modelScale <= 0.0f) {
		if (diags) {
			diags->Error(node.source, "model scale must be > 0");
		}
		return false;
	}
	if (node.hasBbox) {
		if (node.bboxMins[0] >= node.bboxMaxs[0] || node.bboxMins[1] >= node.bboxMaxs[1]
		    || node.bboxMins[2] >= node.bboxMaxs[2]) {
			if (diags) {
				diags->Error(node.source, "model bbox mins must be less than maxs on each axis");
			}
			return false;
		}
	}
	return true;
}

/* Added in OPM: border* attrs removed; use stroke or compositional dividers. */
bool ValidateBorderAttrsRemoved(uid_diag_list_t *diags, const uid_node_def_t &node)
{
	static const char *const kBorderAttrs[] = {
		"border", "border-top", "border-right", "border-bottom", "border-left",
	};
	bool ok = true;
	for (const char *attr : kBorderAttrs) {
		std::string value;
		if (node.properties.Get(attr, &value) && !value.empty()) {
			if (diags) {
				diags->Error(
					node.source,
					std::string(attr) +
						" is removed; use stroke/stroke-width for outlines or a 1px child divider"
				);
			}
			ok = false;
		}
	}
	return ok;
}

/* Added in OPM: implicit rectangle shape when definitions omit it. */
void InjectBuiltinRectangleShape(uid_document_t *doc)
{
	if (!doc || doc->definitions.shapes.count("rectangle")) {
		return;
	}

	uid_shape_def_t shape;
	shape.id = "rectangle";
	shape.hasIntrinsicSize = false;
	shape.width = 0.0f;
	shape.height = 0.0f;

	uid_prop_decl_t radiusProp;
	radiusProp.name = "radius";
	radiusProp.type = UID_PROP_LENGTH;
	radiusProp.required = false;
	radiusProp.defaultValue = "0px";
	shape.props.push_back(radiusProp);

	uid_path_def_t path;
	path.fillExpr = "{parent.fill}";
	path.d =
		"M {shape.radius} 0\n"
		"L {parent.width - shape.radius} 0\n"
		"L {parent.width} {shape.radius}\n"
		"L {parent.width} {parent.height - shape.radius}\n"
		"L {parent.width - shape.radius} {parent.height}\n"
		"L {shape.radius} {parent.height}\n"
		"L 0 {parent.height - shape.radius}\n"
		"L 0 {shape.radius} Z";
	path.loc = uid_source_location_t{doc->sourceName.c_str(), 0, 0};
	shape.paths.push_back(path);

	doc->definitions.shapes.emplace("rectangle", std::move(shape));
}

/* Added in OPM: collection source ids must resolve via XML definitions or host providers. */
bool IsHostCollectionSource(const char *sourceId)
{
	if (!sourceId || !sourceId[0]) {
		return false;
	}
	return std::strcmp(sourceId, "servers") == 0 || std::strcmp(sourceId, "video-modes") == 0 ||
		std::strcmp(sourceId, "display-refresh") == 0 || /* Added in Omaha: SDL refresh rates. */
		std::strcmp(sourceId, "scoreboard") == 0 || std::strcmp(sourceId, "hud-packs") == 0 ||
		std::strcmp(sourceId, "hitmarker-sounds") == 0 || /* Added in Omaha: hitmarker wav picker. */
		std::strcmp(sourceId, "hud-objectives") == 0 || std::strcmp(sourceId, "hud-messages") == 0 ||
		std::strcmp(sourceId, "hud-game-messages") == 0 ||
		std::strcmp(sourceId, "hud-chat") == 0 || std::strcmp(sourceId, "hud-kill-feed") == 0 ||
		std::strcmp(sourceId, "vote-options") == 0; /* Added in OPM: dm_pause vote list. */
}

bool ValidateCollectionSources(uid_document_t *doc, uid_diag_list_t *diags)
{
	if (!doc) {
		return true;
	}
	bool ok = true;
	for (const uid_node_def_t &node : doc->nodes) {
		if (node.collectionSource.empty()) {
			continue;
		}
		if (doc->definitions.sources.count(node.collectionSource) > 0) {
			continue;
		}
		if (IsHostCollectionSource(node.collectionSource.c_str())) {
			continue;
		}
		if (diags) {
			diags->Error(node.source, "unknown collection source: " + node.collectionSource);
		}
		ok = false;
	}
	for (const uid_node_def_t &node : doc->nodes) {
		if (node.kind != UID_NODE_SELECT || node.openModal.empty()) {
			continue;
		}
		if (doc->definitions.modals.find(node.openModal) == doc->definitions.modals.end()) {
			if (diags) {
				diags->Error(node.source, "unknown modal id on <select modal>: " + node.openModal);
			}
			ok = false;
		}
	}
	return ok;
}

} // namespace

uid_result_t UID_CompileDocument(uid_document_t *doc, uid_diag_list_t *diags)
{
	if (!doc) {
		return UID_ERR_INVALID_ARG;
	}

	RebindSourcePaths(doc);

	InjectBuiltinRectangleShape(doc);

	if (UID_ExpandScrollbars(doc, diags) != UID_OK) {
		return UID_ERR_VALIDATE;
	}

	bool ok = true;
	for (uid_node_def_t &node : doc->nodes) {
		ok = ValidateBorderAttrsRemoved(diags, node) && ok;
		ok = ValidateKnownAttrs(diags, node) && ok;
		ok = ValidateBindingsAndActions(doc, diags, node) && ok;
		ok = ValidateShapeInstance(doc, diags, node) && ok;
		ok = ValidateFontRef(doc, diags, node) && ok;
		ok = ValidateImageRef(doc, diags, node) && ok;
		ok = ValidateMaskImageRef(doc, diags, node) && ok;
		ok = ValidateModelNode(diags, node) && ok;
		UID_RegisterCvarBoundProps(&node);
	}
	if (doc->rootNode != UID_INVALID_NODE_ID) {
		ok = ValidateSliderParts(doc, diags, doc->rootNode, UID_NODE_CANVAS) && ok;
		ok = ValidateScrollbarParts(doc, diags, doc->rootNode, UID_NODE_CANVAS) && ok;
	}
	ok = ValidateCollectionSources(doc, diags) && ok;

	/* Shape definition paths must parse as bounded expressions later; ensure props are sane. */
	for (const auto &kv : doc->definitions.shapes) {
		const uid_shape_def_t &shape = kv.second;
		for (const uid_prop_decl_t &decl : shape.props) {
			if (decl.required && decl.defaultValue.empty()) {
				/* Valid declaration; instances checked above. */
				continue;
			}
			if (!decl.defaultValue.empty() && !LooksDeferredExpr(decl.defaultValue)) {
				if (decl.type == UID_PROP_LENGTH) {
					ok = ValidateLengthAttr(diags, uid_source_location_t{doc->sourceName.c_str(), 0, 0},
											decl.name.c_str(), decl.defaultValue) &&
						ok;
				} else if (decl.type == UID_PROP_NUMBER) {
					ok = ValidateNumberAttr(diags, uid_source_location_t{doc->sourceName.c_str(), 0, 0},
											decl.name.c_str(), decl.defaultValue) &&
						ok;
				} else if (decl.type == UID_PROP_COLOR) {
					ok = ValidateColorAttr(diags, uid_source_location_t{doc->sourceName.c_str(), 0, 0},
										   decl.name.c_str(), decl.defaultValue) &&
						ok;
				} else if (decl.type == UID_PROP_BOOLEAN) {
					ok = ValidateBoolAttr(diags, uid_source_location_t{doc->sourceName.c_str(), 0, 0},
										  decl.name.c_str(), decl.defaultValue) &&
						ok;
				}
			}
		}
	}

	if (!ok || (diags && diags->HasErrors())) {
		return UID_ERR_VALIDATE;
	}
	return UID_OK;
}
