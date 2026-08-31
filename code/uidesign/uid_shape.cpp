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

#include "uid_shape.h"

#include "uid_binding.h"
#include "uid_expr.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace {

struct ShapeLookupState {
	float parentWidth;
	float parentHeight;
	std::map<std::string, double> numbers;
	std::map<std::string, std::string> strings;
};

bool LookupNumber(void *userdata, const char *path, double *out)
{
	if (!userdata || !path || !out) {
		return false;
	}
	const ShapeLookupState *st = static_cast<const ShapeLookupState *>(userdata);

	if (std::strcmp(path, "parent.width") == 0 || std::strcmp(path, "parent.Width") == 0) {
		*out = st->parentWidth;
		return true;
	}
	if (std::strcmp(path, "parent.height") == 0 || std::strcmp(path, "parent.Height") == 0) {
		*out = st->parentHeight;
		return true;
	}

	auto it = st->numbers.find(path);
	if (it != st->numbers.end()) {
		*out = it->second;
		return true;
	}
	return false;
}

std::string NormalizeRefPath(const std::string &raw)
{
	/* parent.Width -> parent.width */
	std::string out = raw;
	const size_t dot = out.find('.');
	if (dot != std::string::npos && dot + 1 < out.size()) {
		std::string head = out.substr(0, dot + 1);
		std::string tail = out.substr(dot + 1);
		for (char &c : tail) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		out = head + tail;
	}
	return out;
}

bool SubstituteStringRefs(const std::string &input, const ShapeLookupState &st, std::string *out, std::string *diag)
{
	out->clear();
	size_t i = 0;
	while (i < input.size()) {
		if (input[i] != '{') {
			out->push_back(input[i++]);
			continue;
		}
		const size_t end = input.find('}', i + 1);
		if (end == std::string::npos) {
			if (diag) {
				*diag = "unclosed '{' in shape fill expression";
			}
			return false;
		}
		std::string inner = input.substr(i + 1, end - i - 1);
		/* Trim */
		size_t b = 0;
		while (b < inner.size() && std::isspace(static_cast<unsigned char>(inner[b]))) {
			++b;
		}
		size_t e = inner.size();
		while (e > b && std::isspace(static_cast<unsigned char>(inner[e - 1]))) {
			--e;
		}
		inner = inner.substr(b, e - b);
		const std::string path = NormalizeRefPath(inner);

		/* Prefer string props (colors); fall back to formatted numbers. */
		auto sit = st.strings.find(path);
		if (sit != st.strings.end()) {
			out->append(sit->second);
		} else if (path == "parent.width") {
			char buf[64];
			std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(st.parentWidth));
			out->append(buf);
		} else if (path == "parent.height") {
			char buf[64];
			std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(st.parentHeight));
			out->append(buf);
		} else {
			auto nit = st.numbers.find(path);
			if (nit != st.numbers.end()) {
				char buf[64];
				std::snprintf(buf, sizeof(buf), "%.6g", nit->second);
				out->append(buf);
			} else {
				if (diag) {
					*diag = "unknown shape fill reference: " + path;
				}
				return false;
			}
		}
		i = end + 1;
	}
	return true;
}

static bool TryResolveShapePropNumber(
	const std::string                  &raw,
	const uid_shape_resolve_params_t   *params,
	double                             *out
)
{
	if (!out) {
		return false;
	}
	std::string resolved = raw;
	if (params && params->backend) {
		std::string expanded;
		if (UID_ResolvePropString(params->backend, raw, &expanded)) {
			resolved = expanded;
		}
	}
	std::string dm;
	if (UID_ParseNumber(resolved.c_str(), out, &dm)) {
		return true;
	}
	if (params && params->doc && params->backend && params->nodeId >= 0 &&
	    raw.find('{') != std::string::npos) {
		return UID_EvalRuntimeNumericExpr(params->doc, params->nodeId, raw, params->backend, out);
	}
	return false;
}

void IngestPropMaps(
	ShapeLookupState                 *st,
	const uid_property_set_t         *parentProps,
	const uid_property_set_t         *shapeProps,
	float                             uiPxScale,
	const uid_shape_resolve_params_t *params
)
{
	const float pxScale = (uiPxScale > 0.0f) ? uiPxScale : 1.0f;
	const uid_backend_t *backend = params ? params->backend : nullptr;
	auto ingest = [&](const char *prefix, const uid_property_set_t *props) {
		if (!props) {
			return;
		}
		for (const auto &kv : props->Attrs()) {
			std::string resolved = kv.second.value;
			if (backend) {
				std::string expanded;
				if (UID_ResolvePropString(backend, kv.second.value, &expanded)) {
					resolved = expanded;
				}
			}
			const std::string path = std::string(prefix) + kv.first;
			st->strings[path] = resolved;

			uid_length_t len;
			std::string dm;
			if (UID_ParseLength(resolved.c_str(), &len, &dm)) {
				if (len.unit == UID_LENGTH_PX) {
					/* Added in OPM: shape prop px are CSS-like DIPs. */
					st->numbers[path] = static_cast<double>(len.value) * static_cast<double>(pxScale);
				} else if (len.unit == UID_LENGTH_PERCENT) {
					/* Percent of parent width by convention for shape props. */
					st->numbers[path] = st->parentWidth * (len.value / 100.0);
				}
			} else {
				double num = 0.0;
				if (params && TryResolveShapePropNumber(kv.second.value, params, &num)) {
					st->numbers[path] = num;
				} else if (UID_ParseNumber(resolved.c_str(), &num, &dm)) {
					st->numbers[path] = num;
				}
			}
		}
	};

	ingest("parent.", parentProps);
	ingest("shape.", shapeProps);

	auto formatCvarRgba = [&](const std::string &raw, std::string *out) -> bool {
		uid_color_t c{};
		if (!(backend && raw.compare(0, 10, "cvar-rgba:") == 0 &&
			  UID_ResolveCvarRgba(backend, raw.c_str() + 10, &c))) {
			return false;
		}
		char buf[32];
		std::snprintf(
			buf,
			sizeof(buf),
			"#%02X%02X%02X%02X",
			static_cast<unsigned>(std::min(255.0f, c.r * 255.0f + 0.5f)),
			static_cast<unsigned>(std::min(255.0f, c.g * 255.0f + 0.5f)),
			static_cast<unsigned>(std::min(255.0f, c.b * 255.0f + 0.5f)),
			static_cast<unsigned>(std::min(255.0f, c.a * 255.0f + 0.5f))
		);
		*out = buf;
		return true;
	};

	if (parentProps) {
		std::string fill;
		if (parentProps->Get("fill", &fill)) {
			std::string formatted;
			if (formatCvarRgba(fill, &formatted)) {
				st->strings["parent.fill"] = formatted;
			} else {
				st->strings["parent.fill"] = fill;
			}
		}
		std::string color;
		if (parentProps->Get("color", &color)) {
			st->strings["parent.color"] = color;
		}
	}

	/* Added in OPM: optional overrides without cloning parent prop maps. */
	if (params) {
		if (params->fillOverride) {
			std::string raw = params->fillOverride;
			std::string formatted;
			if (formatCvarRgba(raw, &formatted)) {
				st->strings["parent.fill"] = formatted;
			} else {
				st->strings["parent.fill"] = raw;
			}
		}
		if (params->strokeOverride) {
			std::string raw = params->strokeOverride;
			std::string formatted;
			if (formatCvarRgba(raw, &formatted)) {
				st->strings["parent.stroke"] = formatted;
			} else {
				st->strings["parent.stroke"] = raw;
			}
		}
		if (params->strokeWidthOverride) {
			const std::string raw = params->strokeWidthOverride;
			st->strings["parent.stroke-width"] = raw;
			uid_length_t len;
			std::string dm;
			if (UID_ParseLength(raw.c_str(), &len, &dm)) {
				if (len.unit == UID_LENGTH_PX) {
					st->numbers["parent.stroke-width"] =
						static_cast<double>(len.value) * static_cast<double>(pxScale);
				} else if (len.unit == UID_LENGTH_PERCENT) {
					st->numbers["parent.stroke-width"] = st->parentWidth * (len.value / 100.0);
				}
			} else {
				double num = 0.0;
				if (TryResolveShapePropNumber(raw, params, &num)) {
					st->numbers["parent.stroke-width"] = num;
				} else if (UID_ParseNumber(raw.c_str(), &num, &dm)) {
					st->numbers["parent.stroke-width"] = num;
				}
			}
		}
	}
}

} // namespace

uid_result_t UID_BuildShapeInstanceProps(
	const uid_shape_def_t *shape,
	const uid_property_set_t *instanceProps,
	uid_property_set_t *outProps,
	uid_diag_list_t *diags,
	const uid_source_location_t &loc
)
{
	if (!shape || !outProps) {
		return UID_ERR_INVALID_ARG;
	}

	outProps->Clear();
	bool ok = true;
	for (const uid_prop_decl_t &decl : shape->props) {
		std::string v;
		if (instanceProps && instanceProps->Get(decl.name.c_str(), &v)) {
			outProps->Set(decl.name.c_str(), v);
		} else if (decl.required && decl.defaultValue.empty()) {
			if (diags) {
				diags->Error(loc, "missing required shape prop: " + decl.name);
			}
			ok = false;
		} else if (!decl.defaultValue.empty()) {
			outProps->Set(decl.name.c_str(), decl.defaultValue);
		}
	}
	return ok ? UID_OK : UID_ERR_VALIDATE;
}

uid_result_t UID_ResolveShape(
	const uid_shape_def_t *shape,
	const uid_shape_resolve_params_t *params,
	std::vector<uid_resolved_path_t> *outPaths,
	uid_diag_list_t *diags
)
{
	if (!shape || !params || !outPaths) {
		return UID_ERR_INVALID_ARG;
	}

	outPaths->clear();

	uid_limits_t defaultLimits;
	UID_DefaultLimits(&defaultLimits);
	const uid_limits_t *lim = params->limits ? params->limits : &defaultLimits;

	uid_expr_limits_t exprLim;
	exprLim.maxExprBytes = lim->maxExprBytes;
	exprLim.maxExprNodes = lim->maxExprNodes;

	/* Apply declaration defaults for missing instance props (e.g. radius). */
	uid_property_set_t mergedShapeProps;
	uid_source_location_t loc{};
	loc.path = nullptr;
	loc.line = 0;
	loc.column = 0;
	if (UID_BuildShapeInstanceProps(shape, params->shapeProps, &mergedShapeProps, diags, loc) != UID_OK) {
		return UID_ERR_VALIDATE;
	}
	/* Also keep any extra instance attrs that expressions may reference. */
	if (params->shapeProps) {
		for (const auto &kv : params->shapeProps->Attrs()) {
			if (!mergedShapeProps.Has(kv.first.c_str())) {
				mergedShapeProps.Set(kv.first.c_str(), kv.second.value);
			}
		}
	}

	ShapeLookupState st;
	st.parentWidth = params->parentWidth;
	st.parentHeight = params->parentHeight;
	IngestPropMaps(&st, params->parentProps, &mergedShapeProps, params->uiPxScale, params);

	for (const uid_path_def_t &path : shape->paths) {
		uid_resolved_path_t resolved;
		std::memset(&resolved.fill, 0, sizeof(resolved.fill));

		std::string fillResolved;
		std::string diagMsg;
		if (!SubstituteStringRefs(path.fillExpr, st, &fillResolved, &diagMsg)) {
			if (diags) {
				diags->Error(path.loc, diagMsg.c_str());
			}
			return UID_ERR_VALIDATE;
		}
		if (fillResolved == "none") {
			resolved.fill.r = 0.0f;
			resolved.fill.g = 0.0f;
			resolved.fill.b = 0.0f;
			resolved.fill.a = 0.0f;
		} else if (!UID_ParseColor(fillResolved.c_str(), &resolved.fill, &diagMsg)) {
			if (diags) {
				diags->Error(path.loc, diagMsg.empty() ? "invalid shape fill color" : diagMsg.c_str());
			}
			return UID_ERR_VALIDATE;
		}

		if (!UID_InterpolateString(path.d.c_str(), LookupNumber, &st, &exprLim, &resolved.d, &diagMsg)) {
			if (diags) {
				diags->Error(path.loc, diagMsg.empty() ? "shape path expression failed" : diagMsg.c_str());
			}
			return UID_ERR_VALIDATE;
		}

		if (static_cast<int>(resolved.d.size()) > lim->maxPathBytes) {
			if (diags) {
				diags->Error(path.loc, "resolved path exceeds maxPathBytes");
			}
			return UID_ERR_LIMIT;
		}

		outPaths->push_back(std::move(resolved));
	}

	return UID_OK;
}
