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

#include "uid_binding.h"
#include "uid_collection.h"
#include "uid_expr.h"
#include "uid_expr_bool.h"
#include "uid_modal.h"
#include "uid_opt.h"
#include "uid_profile.h"
#include "uid_value.h"
#include "uid_vars.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

/* Added in OPM: frame-scoped cvar read memo for SyncBindings. */
std::unordered_map<std::string, std::string> g_cvarMemo;
bool                                         g_cvarMemoActive = false;

enum {
	UID_BIND_F_VISIBLE_EXPR = 1u << 0,
	UID_BIND_F_ENABLED_EXPR = 1u << 1,
	UID_BIND_F_STYLE = 1u << 2,
	UID_BIND_F_CVAR_PROPS = 1u << 3,
	UID_BIND_F_EXPR_PROPS = 1u << 4,
	UID_BIND_F_BIND = 1u << 5,
	UID_BIND_F_OPTION_SOURCE = 1u << 6,
	UID_BIND_F_LABEL = 1u << 7,
	UID_BIND_F_KEYBIND = 1u << 8,
	UID_BIND_F_SELECT = 1u << 9
};

void MarkDirty(uid_document_t *doc, uid_dirty_flags_t flags)
{
	if (doc) {
		doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty | flags);
	}
}

bool TextSizeMayChange(uid_node_kind_t kind)
{
	switch (kind) {
	case UID_NODE_LABEL:
	case UID_NODE_BUTTON:
	case UID_NODE_INPUT:
	case UID_NODE_SELECT:
	case UID_NODE_KEYBIND:
		return true;
	default:
		return false;
	}
}

/* Added in OPM: keep leaf <image> src/fit/scale mirrored onto background-* for paint. */
void MirrorLeafImageProps(uid_node_def_t *node, const std::string &prop, const std::string &value)
{
	if (!node || node->kind != UID_NODE_IMAGE) {
		return;
	}
	if (prop == "src") {
		node->properties.Set("background-image", value.c_str());
	} else if (prop == "fit") {
		node->properties.Set("background-fit", value.c_str());
	} else if (prop == "scale") {
		node->properties.Set("background-scale", value.c_str());
	} else if (prop == "background-image") {
		node->properties.Set("src", value.c_str());
	} else if (prop == "background-fit") {
		node->properties.Set("fit", value.c_str());
	} else if (prop == "background-scale") {
		node->properties.Set("scale", value.c_str());
	}
}

void TrimInPlace(std::string *s)
{
	if (!s) {
		return;
	}
	size_t start = 0;
	while (start < s->size() && std::isspace(static_cast<unsigned char>((*s)[start]))) {
		++start;
	}
	size_t end = s->size();
	while (end > start && std::isspace(static_cast<unsigned char>((*s)[end - 1]))) {
		--end;
	}
	if (start == 0 && end == s->size()) {
		return;
	}
	*s = s->substr(start, end - start);
}

bool StagingBlocksSync(uid_document_t *doc, const uid_node_def_t &node, const uid_node_state_t &st)
{
	/* Never clobber an active text edit from an external cvar pulse. */
	if (st.focused && node.kind == UID_NODE_INPUT) {
		return true;
	}

	/* Added in OPM: slider drag stages runtime until pointer release write. */
	if (node.kind == UID_NODE_SLIDER && st.dragging) {
		return true;
	}

	/*
	 * Added in OPM: companion number inputs mirror staged slider values during
	 * drag — do not pull the old cvar over them until release.
	 */
	if (doc && node.kind == UID_NODE_INPUT && node.inputType == "number" && !node.bind.empty()) {
		for (size_t i = 0; i < doc->nodes.size() && i < doc->states.size(); ++i) {
			if (doc->nodes[i].kind == UID_NODE_SLIDER && doc->states[i].dragging &&
				doc->nodes[i].bind == node.bind) {
				return true;
			}
		}
	}

	/*
	 * commit=apply keeps UI-staged values until an explicit write/flush.
	 * After the first sync populates runtimeValue, further pulls are skipped
	 * so local edits remain until UID_WriteBinding / UID_WriteAllBindings.
	 */
	const uid_commit_mode_t mode = node.hasCommit ? node.commit : UID_COMMIT_CHANGE;
	if (mode == UID_COMMIT_APPLY) {
		return st.runtimeValue.hasValue;
	}
	if (mode == UID_COMMIT_SUBMIT) {
		if (st.focused) {
			return true;
		}
		if (!st.editBuffer.empty() && st.runtimeValue.hasValue &&
			st.editBuffer != st.runtimeValue.stringValue) {
			return true;
		}
	}
	return false;
}

void SetRuntimeIfChanged(
	uid_document_t *doc,
	uid_node_id_t id,
	uid_node_state_t *st,
	uid_node_kind_t kind,
	const std::string &value
)
{
	if (!st) {
		return;
	}
	if (st->runtimeValue.hasValue && st->runtimeValue.stringValue == value) {
		return;
	}
	st->runtimeValue.hasValue = true;
	st->runtimeValue.stringValue = value;
	if (TextSizeMayChange(kind)) {
		MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
	} else {
		MarkDirty(doc, UID_DIRTY_PAINT);
	}
	(void)id;
}

bool ReadCvarString(const uid_backend_t *backend, const char *name, std::string *out)
{
	if (!backend || !backend->cvarDescribe || !name || !out) {
		return false;
	}
	if (g_cvarMemoActive && UID_OptEnabled(UID_OPT_CVAR_MEMO)) {
		const auto it = g_cvarMemo.find(name);
		if (it != g_cvarMemo.end()) {
			*out = it->second;
			return true;
		}
	}
	char buf[1024];
	buf[0] = '\0';
	int flags = 0;
	if (!backend->cvarDescribe(name, &flags, buf, sizeof(buf))) {
		return false;
	}
	(void)flags;
	*out = buf;
	if (g_cvarMemoActive && UID_OptEnabled(UID_OPT_CVAR_MEMO)) {
		g_cvarMemo[name] = *out;
	}
	return true;
}

/* Added in OPM: invalidate memo when a cvar is written during sync. */
void InvalidateCvarMemo(const char *name)
{
	if (!name || !g_cvarMemoActive) {
		return;
	}
	g_cvarMemo.erase(name);
}

double ReadCvarNumber(const uid_backend_t *backend, const char *name, double fallback)
{
	std::string s;
	if (!ReadCvarString(backend, name, &s) || s.empty()) {
		return fallback;
	}
	char *end = nullptr;
	const double v = std::strtod(s.c_str(), &end);
	if (end == s.c_str()) {
		return fallback;
	}
	return v;
}

} // namespace

bool UID_ReadCvarString(const uid_backend_t *backend, const char *name, std::string *out)
{
	return ReadCvarString(backend, name, out);
}

double UID_ReadCvarNumber(const uid_backend_t *backend, const char *name, double fallback)
{
	return ReadCvarNumber(backend, name, fallback);
}

bool UID_ResolvePropString(const uid_backend_t *backend, const std::string &input, std::string *out)
{
	if (!out) {
		return false;
	}
	out->clear();
	if (!backend) {
		*out = input;
		return true;
	}

	size_t i = 0;
	while (i < input.size()) {
		if (input[i] != '{') {
			out->push_back(input[i++]);
			continue;
		}
		const size_t end = input.find('}', i + 1);
		if (end == std::string::npos) {
			*out = input;
			return false;
		}
		std::string inner = input.substr(i + 1, end - i - 1);
		size_t b = 0;
		while (b < inner.size() && std::isspace(static_cast<unsigned char>(inner[b]))) {
			++b;
		}
		size_t e = inner.size();
		while (e > b && std::isspace(static_cast<unsigned char>(inner[e - 1]))) {
			--e;
		}
		inner = inner.substr(b, e - b);
		if (inner.compare(0, 4, "cvar") == 0 && inner.size() > 5 && (inner[4] == ':' || inner[4] == '.')) {
			std::string val;
			if (!ReadCvarString(backend, inner.substr(5).c_str(), &val)) {
				return false;
			}
			out->append(val);
		} else {
			*out = input;
			return false;
		}
		i = end + 1;
	}
	return true;
}

bool UID_ResolveCvarRgba(const uid_backend_t *backend, const char *spec, uid_color_t *out)
{
	if (!backend || !spec || !out) {
		return false;
	}

	std::string names[4];
	{
		std::string s = spec;
		size_t start = 0;
		for (int ch = 0; ch < 4; ++ch) {
			const size_t comma = s.find(',', start);
			if (ch < 3 && comma == std::string::npos) {
				return false;
			}
			names[ch] = (ch < 3) ? s.substr(start, comma - start) : s.substr(start);
			size_t b = 0;
			while (b < names[ch].size() && std::isspace(static_cast<unsigned char>(names[ch][b]))) {
				++b;
			}
			size_t e = names[ch].size();
			while (e > b && std::isspace(static_cast<unsigned char>(names[ch][e - 1]))) {
				--e;
			}
			names[ch] = names[ch].substr(b, e - b);
			if (names[ch].empty()) {
				return false;
			}
			start = comma + 1;
		}
	}

	double rgba[4];
	for (int ch = 0; ch < 4; ++ch) {
		rgba[ch] = ReadCvarNumber(backend, names[ch].c_str(), -1.0);
		if (rgba[ch] < 0.0) {
			return false;
		}
	}

	out->r = static_cast<float>(rgba[0] / 255.0);
	out->g = static_cast<float>(rgba[1] / 255.0);
	out->b = static_cast<float>(rgba[2] / 255.0);
	out->a = static_cast<float>(rgba[3] / 255.0);
	return true;
}

namespace {

/* Added in OPM: cvar → UI runtime transforms. */
std::string TransformCvarToUi(
	const uid_node_def_t &node,
	const std::string &cvarValue,
	const uid_backend_t *backend
)
{
	if (node.valueType == "percent") {
		char *end = nullptr;
		const double v = std::strtod(cvarValue.c_str(), &end);
		if (end == cvarValue.c_str()) {
			return "0";
		}
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.6g", v * 100.0);
		return buf;
	}
	if (node.valueType == "invert-mouse") {
		char *end = nullptr;
		const double v = std::strtod(cvarValue.c_str(), &end);
		if (end == cvarValue.c_str()) {
			return "0";
		}
		return (v < 0.0) ? "1" : "0";
	}
	if (node.valueType == "cm360") {
		const double sens = std::strtod(cvarValue.c_str(), nullptr);
		const double dpi = ReadCvarNumber(backend, "ui_modernsettings_dpi", 800.0);
		const double yaw = ReadCvarNumber(backend, "m_yaw", 0.022);
		if (sens <= 0.0 || dpi <= 0.0 || yaw <= 0.0) {
			return "5";
		}
		const double cm = 360.0 * 2.54 / (dpi * sens * yaw);
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.6g", cm);
		return buf;
	}
	if (node.valueType == "display-mode") {
		const int fs = static_cast<int>(std::strtol(cvarValue.c_str(), nullptr, 10));
		std::string noborder = "0";
		ReadCvarString(backend, "r_noborder", &noborder);
		const int nb = static_cast<int>(std::strtol(noborder.c_str(), nullptr, 10));
		if (fs == 0) {
			return "0";
		}
		if (nb != 0) {
			return "2";
		}
		return "1";
	}
	return cvarValue;
}

/* Added in OPM: format slider/number display using authored step precision. */
static std::string FormatControlDisplayValue(const uid_node_def_t &node, const std::string &uiValue)
{
	if (node.kind != UID_NODE_SLIDER &&
		!(node.kind == UID_NODE_INPUT && node.inputType == "number")) {
		return uiValue;
	}
	if (!node.hasStep) {
		return uiValue;
	}

	double v = 0.0;
	if (!UID_ParseNumber(uiValue.c_str(), &v, nullptr)) {
		return uiValue;
	}

	char buf[64];
	if (!UID_FormatNumberForStep(
			v,
			node.hasMin ? node.minValue : 0.0,
			node.hasMax ? node.maxValue : v,
			node.stepValue,
			node.hasMin,
			node.hasMax,
			node.hasStep,
			buf,
			sizeof(buf))) {
		return uiValue;
	}
	return buf;
}

/* Added in OPM: UI runtime → cvar transforms. */
bool TransformUiToCvar(
	const uid_node_def_t &node,
	const std::string &uiValue,
	const uid_backend_t *backend,
	std::string *outPrimary,
	std::string *outNoborder /* optional for display-mode */
)
{
	if (!outPrimary) {
		return false;
	}
	if (node.valueType == "percent") {
		char *end = nullptr;
		const double v = std::strtod(uiValue.c_str(), &end);
		if (end == uiValue.c_str()) {
			return false;
		}
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.6g", v / 100.0);
		*outPrimary = buf;
		return true;
	}
	if (node.valueType == "invert-mouse") {
		const bool on = (uiValue == "1" || uiValue == "true" || uiValue == "on");
		double mag = 0.022;
		std::string cur;
		if (ReadCvarString(backend, "m_pitch", &cur)) {
			const double v = std::strtod(cur.c_str(), nullptr);
			if (v != 0.0) {
				mag = std::fabs(v);
			}
		}
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.6g", on ? -mag : mag);
		*outPrimary = buf;
		return true;
	}
	if (node.valueType == "cm360") {
		char *end = nullptr;
		const double cm = std::strtod(uiValue.c_str(), &end);
		if (end == uiValue.c_str() || cm <= 0.0) {
			return false;
		}
		const double dpi = ReadCvarNumber(backend, "ui_modernsettings_dpi", 800.0);
		const double yaw = ReadCvarNumber(backend, "m_yaw", 0.022);
		if (dpi <= 0.0 || yaw <= 0.0) {
			return false;
		}
		const double sens = 360.0 * 2.54 / (dpi * cm * yaw);
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.6g", sens);
		*outPrimary = buf;
		return true;
	}
	if (node.valueType == "display-mode") {
		const int mode = static_cast<int>(std::strtol(uiValue.c_str(), nullptr, 10));
		if (mode == 0) {
			*outPrimary = "0";
			if (outNoborder) {
				*outNoborder = "0";
			}
		} else if (mode == 2) {
			*outPrimary = "1";
			if (outNoborder) {
				*outNoborder = "1";
			}
		} else {
			*outPrimary = "1";
			if (outNoborder) {
				*outNoborder = "0";
			}
		}
		return true;
	}
	*outPrimary = uiValue;
	return true;
}

static void FillBoolLookupCtx(
	uid_bool_lookup_ctx_t             *ctx,
	uid_document_t                    *doc,
	uid_node_id_t                      nodeId,
	const uid_backend_t               *backend
)
{
	ctx->backend = backend;
	ctx->doc = doc;
	ctx->nodeId = nodeId;
	ctx->item = nullptr;
	ctx->itemIndex = -1;
	ctx->itemCount = 0;
	ctx->selectedIndex = -1;
	if (nodeId >= 0 && static_cast<size_t>(nodeId) < doc->nodes.size()) {
		const uid_node_def_t &node = doc->nodes[static_cast<size_t>(nodeId)];
		if (node.foreachGenerated && node.foreachScopeId >= 0 &&
		    static_cast<size_t>(node.foreachScopeId) < doc->states.size()) {
			const uid_node_state_t &scopeSt = doc->states[static_cast<size_t>(node.foreachScopeId)];
			const int idx = node.foreachItemIndex;
			ctx->itemIndex = idx;
			ctx->itemCount = scopeSt.collectionItemCount;
			ctx->selectedIndex = scopeSt.collectionSelectedIndex;
			if (idx >= 0 && static_cast<size_t>(idx) < scopeSt.collectionItems.size()) {
				ctx->item = &scopeSt.collectionItems[static_cast<size_t>(idx)];
			}
		}
	}
}

struct NumericLookupCtx {
	uid_bool_lookup_ctx_t boolCtx;
};

static bool NumericLookupPath(void *userdata, const char *path, double *out)
{
	if (!userdata || !path || !out) {
		return false;
	}
	NumericLookupCtx *ctx = static_cast<NumericLookupCtx *>(userdata);
	const uid_bool_lookup_ctx_t *bc = &ctx->boolCtx;

	if (std::strncmp(path, "cvar.", 5) == 0) {
		*out = ReadCvarNumber(bc->backend, path + 5, 0.0);
		return true;
	}
	if (std::strncmp(path, "var.", 4) == 0) {
		return bc->doc && UID_LookupVarNumber(bc->doc, path + 4, out);
	}
	if (std::strcmp(path, "item.count") == 0) {
		*out = static_cast<double>(bc->itemCount);
		return true;
	}
	if (std::strcmp(path, "item.index") == 0 && bc->itemIndex >= 0) {
		*out = static_cast<double>(bc->itemIndex);
		return true;
	}
	if (std::strcmp(path, "item.lifetime_alpha") == 0) {
		*out = static_cast<double>(UID_EvalItemLifetimeAlpha(bc->doc, bc->nodeId));
		return true;
	}
	if (bc->item && std::strncmp(path, "item.field.", 11) == 0) {
		const std::string fname(path + 11);
		auto it = bc->item->fields.find(fname);
		if (it != bc->item->fields.end()) {
			char *end = nullptr;
			const double v = std::strtod(it->second.c_str(), &end);
			if (end != it->second.c_str()) {
				*out = v;
				return true;
			}
		}
		return false;
	}
	return false;
}

static std::string FormatEvaluatedNumber(double value)
{
	char buf[64];
	if (std::fabs(value - std::floor(value)) < 1e-9) {
		std::snprintf(buf, sizeof(buf), "%.0f", value);
	} else {
		std::snprintf(buf, sizeof(buf), "%.15g", value);
	}
	return std::string(buf);
}

static bool ExtractBraceExprAndSuffix(const std::string &value, std::string *exprOut, std::string *suffixOut)
{
	if (!exprOut || !suffixOut) {
		return false;
	}
	exprOut->clear();
	suffixOut->clear();
	size_t start = value.find('{');
	if (start == std::string::npos) {
		return false;
	}
	const size_t end = value.find('}', start + 1);
	if (end == std::string::npos) {
		return false;
	}
	*exprOut = value.substr(start + 1, end - start - 1);
	if (end + 1 < value.size()) {
		*suffixOut = value.substr(end + 1);
	}
	return true;
}

static bool IsExprBoundPropValue(const std::string &value)
{
	if (value.find('{') == std::string::npos || value.find('}') == std::string::npos) {
		return false;
	}
	std::string cvarName;
	if (UID_ParseExactCvarBraceBinding(value, &cvarName)) {
		return false;
	}
	std::string expr;
	std::string suffix;
	if (!ExtractBraceExprAndSuffix(value, &expr, &suffix)) {
		return false;
	}
	if (expr.find('?') != std::string::npos) {
		return false;
	}
	return true;
}

static bool EvalRuntimeNumericExprImpl(
	uid_document_t      *doc,
	uid_node_id_t        nodeId,
	const std::string   &expr,
	const uid_backend_t *backend,
	double              *out
)
{
	if (!doc || !backend || !out || expr.empty()) {
		return false;
	}
	std::string inner = expr;
	std::string suffix;
	if (inner.front() == '{' && inner.back() == '}') {
		inner = inner.substr(1, inner.size() - 2);
	} else if (!ExtractBraceExprAndSuffix(expr, &inner, &suffix)) {
		inner = expr;
	}
	TrimInPlace(&inner);
	if (inner.empty()) {
		return false;
	}
	NumericLookupCtx ctx;
	FillBoolLookupCtx(&ctx.boolCtx, doc, nodeId, backend);
	uid_expr_limits_t lim;
	UID_DefaultExprLimits(&lim);
	std::string diag;
	return UID_EvalNumber(inner.c_str(), NumericLookupPath, &ctx, &lim, out, &diag);
}

static bool ResolveAllRuntimeNumericBraceExprs(
	uid_document_t      *doc,
	uid_node_id_t        nodeId,
	const std::string   &authored,
	const uid_backend_t *backend,
	std::string         *out
)
{
	if (!out) {
		return false;
	}
	if (authored.find('{') == std::string::npos) {
		return false;
	}

	std::string cur = authored;
	for (int guard = 0; guard < 64; ++guard) {
		const size_t start = cur.find('{');
		if (start == std::string::npos) {
			*out = cur;
			return true;
		}
		const size_t end = cur.find('}', start + 1);
		if (end == std::string::npos) {
			return false;
		}
		std::string expr = cur.substr(start + 1, end - start - 1);
		TrimInPlace(&expr);
		double value = 0.0;
		if (!EvalRuntimeNumericExprImpl(doc, nodeId, expr, backend, &value)) {
			return false;
		}
		const size_t after = end + 1;
		size_t       tokenEnd = after;
		while (tokenEnd < cur.size() && !std::isspace(static_cast<unsigned char>(cur[tokenEnd]))) {
			++tokenEnd;
		}
		const std::string unitSuffix = cur.substr(after, tokenEnd - after);
		const std::string replacement = FormatEvaluatedNumber(value) + unitSuffix;
		cur.replace(start, tokenEnd - start, replacement);
	}
	return false;
}

static bool ResolveRuntimeNumericPropValue(
	uid_document_t      *doc,
	uid_node_id_t        nodeId,
	const std::string   &authored,
	const uid_backend_t *backend,
	std::string         *out
)
{
	return ResolveAllRuntimeNumericBraceExprs(doc, nodeId, authored, backend, out);
}

static bool ExprLooksCvarPure(const std::string &expr)
{
	/* Added in OPM: cvar/var/literal-only exprs can be memoized on cvar epoch. */
	if (expr.find("item.") != std::string::npos) {
		return false;
	}
	if (expr.find("lifetime") != std::string::npos) {
		return false;
	}
	if (expr.find("hover") != std::string::npos || expr.find("focus") != std::string::npos
		|| expr.find("pressed") != std::string::npos) {
		return false;
	}
	/* Bare "index" / "collection." are host/runtime, not pure cvar. */
	if (expr.find("index") != std::string::npos || expr.find("collection.") != std::string::npos) {
		return false;
	}
	return true;
}

static bool EvalNodeBoolExpr(
	const std::string             &expr,
	uid_document_t                *doc,
	uid_node_id_t                  nodeId,
	const uid_backend_t           *backend
)
{
	if (expr.empty()) {
		return true;
	}
	uid_bool_lookup_ctx_t ctx;
	FillBoolLookupCtx(&ctx, doc, nodeId, backend);
	bool result = true;
	std::string diag;
	if (!UID_EvalBool(expr.c_str(), &ctx, nullptr, &result, &diag)) {
		return false;
	}
	return result;
}

static bool EvalNodeBoolExprCached(
	const std::string   &expr,
	uid_document_t      *doc,
	uid_node_id_t        nodeId,
	const uid_backend_t *backend,
	uid_node_state_t    *st,
	bool                 forVisible
)
{
	if (expr.empty()) {
		return true;
	}
	/* Only memoize when the host exposes a real cvar epoch (tests often omit it). */
	const bool canMemo = UID_OptEnabled(UID_OPT_EXPR_CACHE) && st && backend && backend->cvarEpoch
		&& ExprLooksCvarPure(expr);
	const unsigned epoch = canMemo ? backend->cvarEpoch() : 0u;
	if (canMemo) {
		if (forVisible && st->visibleCached && st->visibleEpoch == epoch) {
			return st->visibleCachedValue;
		}
		if (!forVisible && st->enabledCached && st->enabledEpoch == epoch) {
			return st->enabledCachedValue;
		}
	}
	const bool result = EvalNodeBoolExpr(expr, doc, nodeId, backend);
	if (canMemo) {
		if (forVisible) {
			st->visibleEpoch = epoch;
			st->visibleCached = true;
			st->visibleCachedValue = result;
		} else {
			st->enabledEpoch = epoch;
			st->enabledCached = true;
			st->enabledCachedValue = result;
		}
	}
	return result;
}

/* Added in OPM: sync style ternaries (any property) into resolved property values. */
static void SyncBoundStyleExprs(
	uid_document_t      *doc,
	uid_node_id_t        nodeId,
	uid_node_def_t      *node,
	const uid_backend_t *backend
)
{
	if (!doc || !node || node->styleExprs.empty()) {
		return;
	}

	uid_node_state_t *st = nullptr;
	if (nodeId >= 0 && static_cast<size_t>(nodeId) < doc->states.size()) {
		st = &doc->states[static_cast<size_t>(nodeId)];
	}
	bool allCvarPure = true;
	for (const auto &kv : node->styleExprs) {
		if (!kv.second.empty() && !ExprLooksCvarPure(kv.second)) {
			allCvarPure = false;
			break;
		}
	}
	const bool canMemo = UID_OptEnabled(UID_OPT_EXPR_CACHE) && allCvarPure && st && backend
		&& backend->cvarEpoch;
	const unsigned epoch = canMemo ? backend->cvarEpoch() : 0u;
	if (canMemo && st->styleExprCached && st->styleExprEpoch == epoch) {
		return;
	}

	static const char *kLayoutProps[] = {
		"width", "height", "gap", "margin", "padding", "font-size", "rotation", "rotation-origin",
		"translate-x", "translate-y",
		"src" /* Added in OPM: leaf <image> intrinsic size depends on src */
	};
	bool strokeLayout = true;
	{
		const char *sl = node->properties.GetCStr("stroke-layout", nullptr);
		if (sl && sl[0]) {
			(void)UID_ParseBool(sl, &strokeLayout, nullptr);
		}
	}
	uid_bool_lookup_ctx_t ctx;
	FillBoolLookupCtx(&ctx, doc, nodeId, backend);
	for (const auto &kv : node->styleExprs) {
		if (kv.second.empty()) {
			continue;
		}
		std::string resolved;
		std::string diag;
		if (!UID_EvalStyleTernary(kv.second.c_str(), &ctx, nullptr, &resolved, &diag)) {
			continue;
		}
		const char *cur = node->properties.GetCStr(kv.first.c_str(), nullptr);
		if (cur && resolved == cur) {
			continue;
		}
		node->properties.Set(kv.first.c_str(), resolved.c_str());
		/* Changed in OPM: hoverfill aliases share one resolved value. */
		if (kv.first == "hoverfill") {
			node->properties.Set("hover-fill", resolved.c_str());
		} else if (kv.first == "hover-fill") {
			node->properties.Set("hoverfill", resolved.c_str());
		}
		MirrorLeafImageProps(node, kv.first, resolved);
		uid_dirty_flags_t dirty = UID_DIRTY_PAINT;
		for (const char *lp : kLayoutProps) {
			if (kv.first == lp) {
				dirty = static_cast<uid_dirty_flags_t>(dirty | UID_DIRTY_LAYOUT);
				break;
			}
		}
		/* Changed in OPM: stroke layout dirty only when stroke-layout is true (default). */
		if (strokeLayout && (kv.first == "stroke" || kv.first == "stroke-width")) {
			dirty = static_cast<uid_dirty_flags_t>(dirty | UID_DIRTY_LAYOUT);
		}
		MarkDirty(doc, dirty);
	}

	if (canMemo) {
		st->styleExprEpoch = epoch;
		st->styleExprCached = true;
	} else if (st) {
		st->styleExprCached = false;
	}
}

std::string FormatKeybindKeyName(const char *name)
{
	if (!name || !name[0]) {
		return std::string();
	}
	if (name[1] == '\0') {
		const unsigned char c = static_cast<unsigned char>(name[0]);
		if (c >= 'a' && c <= 'z') {
			char buf[2];
			buf[0] = static_cast<char>(std::toupper(c));
			buf[1] = '\0';
			return buf;
		}
	}
	return name;
}

/*
 * Fixed in OPM: Quake/MOHAA letter binds are lowercase ASCII ('w' == 119).
 * Display labels may show "W"; never store uppercase letter keynums.
 */
int NormalizeBindKey(int key)
{
	if (key >= 'A' && key <= 'Z') {
		return key + ('a' - 'A');
	}
	return key;
}

int KeybindSlotIndex(const uid_node_def_t &node)
{
	return node.bindSlot != 0 ? 1 : 0;
}

const char *KeybindModalCvarName(const uid_node_def_t &node)
{
	if (!node.modalCvar.empty()) {
		return node.modalCvar.c_str();
	}
	return UID_DefaultModalCvarName();
}

bool ParseCapturedKey(const std::string &val, const uid_backend_t *backend, int *outKey)
{
	if (!outKey) {
		return false;
	}
	if (backend && backend->keyNameToNum && backend->keyNameToNum(val.c_str(), outKey)) {
		return true;
	}
	char *end = nullptr;
	const long n = std::strtol(val.c_str(), &end, 10);
	if (end && end != val.c_str() && *end == '\0') {
		*outKey = static_cast<int>(n);
		return true;
	}
	return false;
}

void SyncTextCvarLabel(
	uid_document_t *doc,
	uid_node_id_t id,
	uid_node_def_t *node,
	uid_node_state_t *st,
	const uid_backend_t *backend
)
{
	const char *tc = node->properties.GetCStr("text-cvar", nullptr);
	if (!tc || !tc[0]) {
		return;
	}
	std::string value;
	if (!ReadCvarString(backend, tc, &value)) {
		value.clear();
	}
	SetRuntimeIfChanged(doc, id, st, node->kind, value);
}

static void MigrateSettingsCvarValue(
	const std::string &cvarName,
	char *valueBuf,
	size_t valueBufSize,
	const uid_backend_t *backend
)
{
	if (!valueBuf || valueBufSize == 0 || !backend || !backend->cvarWrite) {
		return;
	}

	if (cvarName == "com_maxfps") {
		const int fps = static_cast<int>(std::strtol(valueBuf, nullptr, 10));
		if (fps != 125 && fps != 250 && fps != 500) {
			backend->cvarWrite("com_maxfps", "125");
			std::snprintf(valueBuf, valueBufSize, "%s", "125");
		}
		return;
	}

	if (cvarName == "in_mouse") {
		const int v = static_cast<int>(std::strtol(valueBuf, nullptr, 10));
		if (v == 0 || v < -1 || v > 1) {
			backend->cvarWrite("in_mouse", "1");
			std::snprintf(valueBuf, valueBufSize, "%s", "1");
		}
		return;
	}

	if (cvarName == "r_lodscale") {
		const double v = std::strtod(valueBuf, nullptr);
		static const double presets[] = {0.5, 1.1, 1.5};
		bool           found = false;
		for (double preset : presets) {
			if (std::fabs(v - preset) < 0.01) {
				found = true;
				break;
			}
		}
		if (!found) {
			double best = presets[1];
			double bestDist = std::fabs(v - best);
			for (double preset : presets) {
				const double dist = std::fabs(v - preset);
				if (dist < bestDist) {
					best = preset;
					bestDist = dist;
				}
			}
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%.1f", best);
			backend->cvarWrite("r_lodscale", buf);
			std::snprintf(valueBuf, valueBufSize, "%s", buf);
		}
	}
}

void SyncCvarBind(
	uid_document_t *doc,
	uid_node_id_t id,
	uid_node_def_t *node,
	uid_node_state_t *st,
	const uid_backend_t *backend,
	const std::string &cvarName
)
{
	if (!backend || !backend->cvarDescribe || !node || !st) {
		return;
	}
	if (StagingBlocksSync(doc, *node, *st)) {
		return;
	}

	char valueBuf[1024];
	valueBuf[0] = '\0';
	int flags = 0;
	if (!backend->cvarDescribe(cvarName.c_str(), &flags, valueBuf, sizeof(valueBuf))) {
		return;
	}
	(void)flags;
	MigrateSettingsCvarValue(cvarName, valueBuf, sizeof(valueBuf), backend);
	const std::string ui = TransformCvarToUi(*node, std::string(valueBuf), backend);
	SetRuntimeIfChanged(doc, id, st, node->kind, FormatControlDisplayValue(*node, ui));
}

void SyncKeybindDisplay(
	uid_document_t *doc,
	uid_node_id_t id,
	uid_node_def_t *node,
	uid_node_state_t *st,
	const uid_backend_t *backend
)
{
	if (!backend || !node || !st || node->binding.empty()) {
		return;
	}
	if (st->capturing) {
		return;
	}

	std::string display;
	int key1 = -1;
	int key2 = -1;
	if (backend->getKeysForCommand) {
		backend->getKeysForCommand(node->binding.c_str(), &key1, &key2);
	}

	/*
	 * Fixed in OPM: migrate corrupted uppercase letter binds (from display-label
	 * round-trip) down to lowercase so console bind w matches the settings UI.
	 */
	if (backend->setBinding && backend->getBinding) {
		auto migrateUpper = [&](int *keySlot) {
			if (!keySlot || *keySlot < 'A' || *keySlot > 'Z') {
				return;
			}
			const int upper = *keySlot;
			const int lower = NormalizeBindKey(upper);
			char lowerBind[256];
			lowerBind[0] = '\0';
			backend->getBinding(lower, lowerBind, sizeof(lowerBind));
			if (lowerBind[0] == '\0' || std::strcmp(lowerBind, node->binding.c_str()) == 0) {
				backend->setBinding(upper, "");
				backend->setBinding(lower, node->binding.c_str());
				*keySlot = lower;
			}
		};
		migrateUpper(&key1);
		migrateUpper(&key2);
	}

	const int slot = KeybindSlotIndex(*node);
	const int key = (slot == 1) ? key2 : key1;
	if (key >= 0 && backend->keyNumToName) {
		char name[64];
		name[0] = '\0';
		if (backend->keyNumToName(key, name, sizeof(name)) && name[0]) {
			display = FormatKeybindKeyName(name);
		}
	}

	if (display.empty()) {
		display = UID_KeybindEmptyLabel(*node);
	}
	SetRuntimeIfChanged(doc, id, st, node->kind, display);
}

void RefreshOptionSource(
	uid_document_t *doc,
	uid_node_def_t *node,
	const uid_backend_t *backend
)
{
	if (!doc || !node || !backend || !backend->queryOptions) {
		return;
	}
	if (node->optionSource.empty()) {
		return;
	}
	/* Cache: only query when options have not been populated yet. */
	if (!node->options.empty()) {
		return;
	}

	const int maxOpts = doc->limits.maxOptionsPerSelect > 0 ? doc->limits.maxOptionsPerSelect : 512;
	std::vector<char *> values(static_cast<size_t>(maxOpts), nullptr);
	std::vector<char *> labels(static_cast<size_t>(maxOpts), nullptr);
	const int n = backend->queryOptions(
		node->optionSource.c_str(),
		values.data(),
		labels.data(),
		maxOpts
	);
	if (n <= 0) {
		return;
	}

	const int count = n < maxOpts ? n : maxOpts;
	node->options.clear();
	node->options.reserve(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i) {
		uid_select_option_t opt;
		opt.value = values[static_cast<size_t>(i)] ? values[static_cast<size_t>(i)] : "";
		opt.label = labels[static_cast<size_t>(i)] ? labels[static_cast<size_t>(i)] : opt.value;
		node->options.push_back(opt);
	}
	MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
}

uid_result_t WriteCvarBind(
	uid_node_def_t *node,
	uid_node_state_t *st,
	const uid_backend_t *backend,
	const std::string &cvarName
)
{
	if (!backend || !backend->cvarWrite || !node || !st) {
		return UID_ERR_INVALID_ARG;
	}
	if (!st->runtimeValue.hasValue) {
		return UID_ERR_NOT_READY;
	}

	int flags = 0;
	char valueBuf[8];
	valueBuf[0] = '\0';
	if (backend->cvarDescribe) {
		if (backend->cvarDescribe(cvarName.c_str(), &flags, valueBuf, sizeof(valueBuf))) {
			if (flags & UID_CVAR_WRITE_DENIED) {
				return UID_ERR_VALIDATE;
			}
		}
	}

	std::string primary;
	std::string noborder;
	if (!TransformUiToCvar(*node, st->runtimeValue.stringValue, backend, &primary, &noborder)) {
		return UID_ERR_VALIDATE;
	}

	if (!backend->cvarWrite(cvarName.c_str(), primary.c_str())) {
		return UID_ERR_VALIDATE;
	}
	/* Added in OPM: display-mode also drives r_noborder. */
	if (node->valueType == "display-mode" && !noborder.empty()) {
		backend->cvarWrite("r_noborder", noborder.c_str());
	}
	return UID_OK;
}

uid_result_t CommitKeybindSlot(
	const char *command,
	int slot,
	int newKey,
	bool haveNewKey,
	const uid_backend_t *backend
)
{
	if (!backend || !backend->setBinding || !command || !command[0]) {
		return UID_ERR_INVALID_ARG;
	}
	if (!backend->getKeysForCommand) {
		return UID_ERR_NOT_READY;
	}

	if (haveNewKey) {
		newKey = NormalizeBindKey(newKey);
	}

	int key1 = -1;
	int key2 = -1;
	backend->getKeysForCommand(command, &key1, &key2);

	if (!haveNewKey) {
		if (slot == 0) {
			if (key1 >= 0) {
				backend->setBinding(key1, "");
			}
		} else {
			if (key2 >= 0) {
				backend->setBinding(key2, "");
			}
		}
		return UID_OK;
	}

	if (slot == 0) {
		if (key1 == newKey) {
			return UID_OK;
		}
		if (key2 == newKey) {
			if (key1 >= 0) {
				backend->setBinding(key1, "");
			}
			if (key2 >= 0) {
				backend->setBinding(key2, "");
			}
			backend->setBinding(newKey, command);
			return UID_OK;
		}
		if (key1 >= 0) {
			backend->setBinding(key1, "");
		}
		backend->setBinding(newKey, command);
		return UID_OK;
	}

	if (key2 == newKey) {
		return UID_OK;
	}
	if (key1 == newKey) {
		if (key2 >= 0) {
			backend->setBinding(key2, "");
		}
		return UID_OK;
	}
	if (key2 >= 0) {
		backend->setBinding(key2, "");
	}
	backend->setBinding(newKey, command);
	return UID_OK;
}

uid_result_t WriteKeybind(
	uid_node_def_t *node,
	uid_node_state_t *st,
	const uid_backend_t *backend
)
{
	if (!backend || !node || !st || node->binding.empty()) {
		return UID_ERR_INVALID_ARG;
	}
	if (!backend->setBinding) {
		return UID_ERR_NOT_READY;
	}

	/*
	 * Fixed in OPM: runtimeValue holds display labels ("W", "NONE"), not capture
	 * keynums. Capture commits via UID_TryCommitKeybindCapture. Only empty value
	 * means clear this slot (Backspace / Del).
	 */
	if (st->runtimeValue.hasValue && !st->runtimeValue.stringValue.empty()) {
		return UID_OK;
	}

	return CommitKeybindSlot(node->binding.c_str(), KeybindSlotIndex(*node), 0, false, backend);
}

uid_result_t TryCommitKeybindCaptureImpl(
	uid_document_t *doc,
	uid_node_id_t nodeId,
	int capturedKey,
	const uid_backend_t *backend
)
{
	if (!doc || !backend || nodeId == UID_INVALID_NODE_ID) {
		return UID_ERR_INVALID_ARG;
	}
	uid_node_def_t *node = UID_GetNode(doc, nodeId);
	if (!node || node->kind != UID_NODE_KEYBIND) {
		return UID_ERR_INVALID_ARG;
	}
	if (static_cast<size_t>(nodeId) >= doc->states.size()) {
		return UID_ERR_INVALID_ARG;
	}
	uid_node_state_t *st = &doc->states[static_cast<size_t>(nodeId)];

	char existing[256];
	existing[0] = '\0';
	if (backend->getBinding) {
		backend->getBinding(capturedKey, existing, sizeof(existing));
	}

	const bool conflict =
		existing[0] != '\0' && std::strcmp(existing, node->binding.c_str()) != 0;

	if (conflict && !node->confirmModal.empty() && backend->cvarWrite) {
		char keyName[64];
		keyName[0] = '\0';
		if (backend->keyNumToName) {
			backend->keyNumToName(capturedKey, keyName, sizeof(keyName));
		}
		const std::string displayKey = FormatKeybindKeyName(keyName);

		std::string message;
		if (!displayKey.empty() && existing[0]) {
			message = displayKey + " is already bound to " + existing + ". Overwrite?";
		} else if (!displayKey.empty()) {
			message = displayKey + " is already bound. Overwrite?";
		} else {
			message = "This key is already bound. Overwrite?";
		}

		char keyBuf[32];
		std::snprintf(keyBuf, sizeof(keyBuf), "%d", capturedKey);
		const int slot = KeybindSlotIndex(*node);
		const char *slotStr = slot == 1 ? "secondary" : "primary";

		backend->cvarWrite("ui_modal_message", message.c_str());
		backend->cvarWrite("ui_modal_bind_command", node->binding.c_str());
		backend->cvarWrite("ui_modal_bind_key", keyBuf);
		backend->cvarWrite("ui_modal_bind_slot", slotStr);
		backend->cvarWrite("ui_modal_bind_existing", existing);
		backend->cvarWrite("ui_modal_confirm_invoke", "modal-commit-keybind");

		doc->keybindPending.active = true;
		doc->keybindPending.nodeId = nodeId;
		doc->keybindPending.slot = slot;
		doc->keybindPending.newKey = capturedKey;
		doc->keybindPending.command = node->binding;

		backend->cvarWrite(KeybindModalCvarName(*node), node->confirmModal.c_str());
		SyncKeybindDisplay(doc, nodeId, node, st, backend);
		MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_BINDING | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
		return UID_OK;
	}

	if (const uid_result_t r = CommitKeybindSlot(
			node->binding.c_str(),
			KeybindSlotIndex(*node),
			capturedKey,
			true,
			backend
		);
		r != UID_OK) {
		return r;
	}
	SyncKeybindDisplay(doc, nodeId, node, st, backend);
	return UID_OK;
}

uid_result_t CommitKeybindFromModalCvarsImpl(uid_document_t *doc, const uid_backend_t *backend)
{
	if (!doc || !backend || !backend->cvarDescribe || !backend->setBinding) {
		return UID_ERR_INVALID_ARG;
	}

	char command[256];
	char keyBuf[64];
	char slotBuf[32];
	command[0] = '\0';
	keyBuf[0] = '\0';
	slotBuf[0] = '\0';
	int flags = 0;
	if (!backend->cvarDescribe("ui_modal_bind_command", &flags, command, sizeof(command))) {
		return UID_ERR_VALIDATE;
	}
	if (!backend->cvarDescribe("ui_modal_bind_key", &flags, keyBuf, sizeof(keyBuf))) {
		return UID_ERR_VALIDATE;
	}
	if (!backend->cvarDescribe("ui_modal_bind_slot", &flags, slotBuf, sizeof(slotBuf))) {
		slotBuf[0] = '\0';
	}

	int newKey = 0;
	if (!ParseCapturedKey(keyBuf, backend, &newKey)) {
		return UID_ERR_VALIDATE;
	}

	int slot = 0;
	if (std::strcmp(slotBuf, "secondary") == 0) {
		slot = 1;
	}

	const uid_result_t r = CommitKeybindSlot(command, slot, newKey, true, backend);
	if (r != UID_OK) {
		return r;
	}

	const uid_node_id_t pendingId = doc->keybindPending.nodeId;
	doc->keybindPending.active = false;

	if (pendingId != UID_INVALID_NODE_ID && static_cast<size_t>(pendingId) < doc->states.size()) {
		uid_node_def_t *node = UID_GetNode(doc, pendingId);
		uid_node_state_t *st = &doc->states[static_cast<size_t>(pendingId)];
		if (node) {
			SyncKeybindDisplay(doc, pendingId, node, st, backend);
		}
	}

	const size_t n = doc->nodes.size() < doc->states.size() ? doc->nodes.size() : doc->states.size();
	for (size_t i = 0; i < n; ++i) {
		if (doc->nodes[i].kind != UID_NODE_KEYBIND) {
			continue;
		}
		if (doc->nodes[i].binding == command) {
			SyncKeybindDisplay(
				doc,
				static_cast<uid_node_id_t>(i),
				&doc->nodes[i],
				&doc->states[i],
				backend
			);
		}
	}

	return UID_OK;
}

static std::string SubstituteAllItemFieldTokens(const uid_collection_entry_t *item, const std::string &input)
{
	if (!item || input.find("item.field.") == std::string::npos) {
		return input;
	}
	std::string out;
	out.reserve(input.size());
	for (size_t i = 0; i < input.size();) {
		if (input[i] != '{') {
			out.push_back(input[i++]);
			continue;
		}
		const size_t end = input.find('}', i + 1);
		if (end == std::string::npos) {
			out.push_back(input[i++]);
			continue;
		}
		const std::string key = input.substr(i + 1, end - i - 1);
		if (key.rfind("item.field.", 0) == 0) {
			const std::string fname = key.substr(11);
			auto it = item->fields.find(fname);
			if (it != item->fields.end()) {
				out += it->second;
			} else {
				out += input.substr(i, end - i + 1);
			}
		} else {
			out += input.substr(i, end - i + 1);
		}
		i = end + 1;
	}
	return out;
}

static void SyncExprBoundProps(
	uid_document_t      *doc,
	uid_node_id_t        nodeId,
	uid_node_def_t      *node,
	const uid_backend_t *backend
)
{
	if (!doc || !node || !backend) {
		return;
	}
	static const char *kLayoutProps[] = {
		"width", "height", "gap", "margin", "padding", "font-size", "rotation", "rotation-origin",
		"translate-x", "translate-y",
		"src" /* Added in OPM: leaf <image> intrinsic size depends on src */
	};
	for (const auto &kv : node->exprBoundProps) {
		uid_bool_lookup_ctx_t ctx;
		FillBoolLookupCtx(&ctx, doc, nodeId, backend);
		std::string authored = kv.second;
		if (ctx.item) {
			authored = SubstituteAllItemFieldTokens(ctx.item, authored);
		}

		std::string resolved;
		bool ok = ResolveRuntimeNumericPropValue(doc, nodeId, authored, backend, &resolved);
		if (!ok && authored.find('{') == std::string::npos) {
			resolved = authored;
			ok = !resolved.empty();
		}
		if (!ok) {
			/* String / ternary item.field props (color hex, font-weight ternary, …). */
			std::string expr;
			std::string suffix;
			if (authored.size() >= 2 && authored.front() == '{' && authored.back() == '}') {
				expr = authored.substr(1, authored.size() - 2);
			} else if (!ExtractBraceExprAndSuffix(authored, &expr, &suffix)) {
				continue;
			}
			TrimInPlace(&expr);
			if (expr.find('?') != std::string::npos) {
				std::string diag;
				if (!UID_EvalStyleTernary(expr.c_str(), &ctx, nullptr, &resolved, &diag)) {
					continue;
				}
				resolved += suffix;
				ok = true;
			} else if (ctx.item && expr.rfind("item.field.", 0) == 0) {
				const std::string fname = expr.substr(11);
				auto it = ctx.item->fields.find(fname);
				if (it == ctx.item->fields.end()) {
					continue;
				}
				resolved = it->second + suffix;
				ok = true;
			} else if (expr == "item.lifetime_alpha") {
				char buf[32];
				std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(UID_EvalItemLifetimeAlpha(doc, nodeId)));
				resolved = std::string(buf) + suffix;
				ok = true;
			}
		}
		if (!ok) {
			continue;
		}
		const char *want = resolved.c_str();
		const char *cur = node->properties.GetCStr(kv.first.c_str(), "");
		if (cur && std::strcmp(cur, want) == 0) {
			continue;
		}
		node->properties.Set(kv.first.c_str(), want);
		MirrorLeafImageProps(node, kv.first, resolved);
		uid_dirty_flags_t dirty = UID_DIRTY_PAINT;
		for (const char *lp : kLayoutProps) {
			if (kv.first == lp) {
				dirty = static_cast<uid_dirty_flags_t>(dirty | UID_DIRTY_LAYOUT);
				break;
			}
		}
		MarkDirty(doc, dirty);
	}
}

static void SyncForeachItemFieldText(
	uid_document_t      *doc,
	uid_node_id_t        nodeId,
	uid_node_def_t      *node,
	uid_node_state_t    *st,
	const uid_backend_t *backend
)
{
	if (!doc || !node || !st || !backend || !node->foreachGenerated) {
		return;
	}
	if (node->text.find("{item.field.") == std::string::npos) {
		return;
	}
	uid_bool_lookup_ctx_t ctx;
	FillBoolLookupCtx(&ctx, doc, nodeId, backend);
	if (!ctx.item) {
		return;
	}

	std::string out;
	out.reserve(node->text.size());
	for (size_t i = 0; i < node->text.size();) {
		if (node->text[i] != '{') {
			out.push_back(node->text[i++]);
			continue;
		}
		const size_t end = node->text.find('}', i + 1);
		if (end == std::string::npos) {
			out.push_back(node->text[i++]);
			continue;
		}
		const std::string key = node->text.substr(i + 1, end - i - 1);
		if (key.rfind("item.field.", 0) == 0) {
			const std::string fname = key.substr(11);
			auto it = ctx.item->fields.find(fname);
			if (it != ctx.item->fields.end()) {
				out += it->second;
			}
		} else {
			out += node->text.substr(i, end - i + 1);
		}
		i = end + 1;
	}

	if (st->runtimeValue.hasValue && st->runtimeValue.stringValue == out) {
		return;
	}
	st->runtimeValue.hasValue = true;
	st->runtimeValue.stringValue = out;
	uid_dirty_flags_t dirty = UID_DIRTY_PAINT;
	if (TextSizeMayChange(node->kind)) {
		dirty = static_cast<uid_dirty_flags_t>(dirty | UID_DIRTY_LAYOUT);
	}
	MarkDirty(doc, dirty);
}

static void SyncCvarBoundProps(
	uid_document_t      *doc,
	uid_node_id_t        nodeId,
	uid_node_def_t      *node,
	const uid_backend_t *backend
)
{
	if (!doc || !node || !backend) {
		return;
	}
	static const char *kLayoutProps[] = {
		"width", "height", "gap", "margin", "padding", "font-size", "rotation-origin",
		"translate-x", "translate-y",
		"src" /* Added in OPM: leaf <image> intrinsic size depends on src */
	};
	for (const auto &kv : node->cvarBoundProps) {
		std::string cvarName;
		if (!UID_ParseExactCvarBraceBinding(kv.second, &cvarName)) {
			continue;
		}
		std::string val;
		if (!ReadCvarString(backend, cvarName.c_str(), &val) || val.empty()) {
			const char *authored = kv.second.c_str();
			const char *cur = node->properties.GetCStr(kv.first.c_str(), "");
			if (cur && std::strcmp(cur, authored) == 0) {
				continue;
			}
			node->properties.Set(kv.first.c_str(), authored);
			MirrorLeafImageProps(node, kv.first, authored);
			uid_dirty_flags_t dirty = UID_DIRTY_PAINT;
			for (const char *lp : kLayoutProps) {
				if (kv.first == lp) {
					dirty = static_cast<uid_dirty_flags_t>(dirty | UID_DIRTY_LAYOUT);
					break;
				}
			}
			MarkDirty(doc, dirty);
			continue;
		}
		const char *want = val.c_str();
		const char *cur = node->properties.GetCStr(kv.first.c_str(), "");
		if (cur && std::strcmp(cur, want) == 0) {
			continue;
		}
		node->properties.Set(kv.first.c_str(), want);
		MirrorLeafImageProps(node, kv.first, val);
		uid_dirty_flags_t dirty = UID_DIRTY_PAINT;
		for (const char *lp : kLayoutProps) {
			if (kv.first == lp) {
				dirty = static_cast<uid_dirty_flags_t>(dirty | UID_DIRTY_LAYOUT);
				break;
			}
		}
		MarkDirty(doc, dirty);
	}
}

/* Added in OPM: max joined label text so huge servers cannot blow buffers. */
constexpr size_t kJoinMaxChars = 2048;

static bool JoinReadBareId(const char *s, size_t len, size_t *pos, std::string *out)
{
	if (!s || !pos || !out) {
		return false;
	}
	while (*pos < len && std::isspace(static_cast<unsigned char>(s[*pos]))) {
		++(*pos);
	}
	if (*pos >= len) {
		return false;
	}
	const size_t start = *pos;
	unsigned char c = static_cast<unsigned char>(s[*pos]);
	if (!(std::isalnum(c) || c == '_' || c == '-')) {
		return false;
	}
	++(*pos);
	while (*pos < len) {
		c = static_cast<unsigned char>(s[*pos]);
		if (std::isalnum(c) || c == '_' || c == '-') {
			++(*pos);
		} else {
			break;
		}
	}
	*out = std::string(s + start, *pos - start);
	return !out->empty();
}

static bool JoinReadQuoted(const char *s, size_t len, size_t *pos, std::string *out)
{
	if (!s || !pos || !out) {
		return false;
	}
	while (*pos < len && std::isspace(static_cast<unsigned char>(s[*pos]))) {
		++(*pos);
	}
	if (*pos >= len) {
		return false;
	}
	const char quote = s[*pos];
	if (quote != '"' && quote != '\'') {
		return false;
	}
	++(*pos);
	std::string acc;
	while (*pos < len) {
		const char c = s[(*pos)++];
		if (c == quote) {
			*out = acc;
			return true;
		}
		if (c == '\\' && *pos < len) {
			acc.push_back(s[(*pos)++]);
		} else {
			acc.push_back(c);
		}
	}
	return false;
}

static bool JoinExpectChar(const char *s, size_t len, size_t *pos, char want)
{
	if (!s || !pos) {
		return false;
	}
	while (*pos < len && std::isspace(static_cast<unsigned char>(s[*pos]))) {
		++(*pos);
	}
	if (*pos >= len || s[*pos] != want) {
		return false;
	}
	++(*pos);
	return true;
}

static std::string JoinFieldValue(const uid_collection_entry_t &item, const std::string &field)
{
	if (field == "label") {
		return item.label;
	}
	if (field == "value" || field == "key") {
		return field == "key" ? item.key : item.value;
	}
	auto it = item.fields.find(field);
	if (it != item.fields.end()) {
		return it->second;
	}
	return std::string();
}

/*
 * Added in OPM: join(source, field, "sep"[, boolFilter]) → string for label braces.
 * Filter is evaluated per row with item.field.* bound to that row.
 */
static bool EvalJoinCall(
	uid_document_t *doc,
	uid_node_id_t nodeId,
	const std::string &call,
	const uid_backend_t *backend,
	std::string *out
)
{
	if (!doc || !backend || !out) {
		return false;
	}
	out->clear();
	const char *s = call.c_str();
	const size_t len = call.size();
	size_t pos = 0;
	while (pos < len && std::isspace(static_cast<unsigned char>(s[pos]))) {
		++pos;
	}
	if (len - pos < 5 || std::strncmp(s + pos, "join", 4) != 0) {
		return false;
	}
	pos += 4;
	if (!JoinExpectChar(s, len, &pos, '(')) {
		return false;
	}

	std::string sourceId;
	std::string fieldId;
	std::string sep;
	if (!JoinReadBareId(s, len, &pos, &sourceId) || !JoinExpectChar(s, len, &pos, ',') ||
	    !JoinReadBareId(s, len, &pos, &fieldId) || !JoinExpectChar(s, len, &pos, ',') ||
	    !JoinReadQuoted(s, len, &pos, &sep)) {
		return false;
	}

	std::string filter;
	while (pos < len && std::isspace(static_cast<unsigned char>(s[pos]))) {
		++pos;
	}
	if (pos < len && s[pos] == ',') {
		++pos;
		while (pos < len && std::isspace(static_cast<unsigned char>(s[pos]))) {
			++pos;
		}
		/* Remainder until the matching ')' at depth 0. */
		int depth = 0;
		const size_t filterStart = pos;
		while (pos < len) {
			const char c = s[pos];
			if (c == '(') {
				++depth;
			} else if (c == ')') {
				if (depth == 0) {
					break;
				}
				--depth;
			} else if ((c == '"' || c == '\'') && depth >= 0) {
				const char q = c;
				++pos;
				while (pos < len && s[pos] != q) {
					if (s[pos] == '\\' && pos + 1 < len) {
						pos += 2;
					} else {
						++pos;
					}
				}
				if (pos < len) {
					++pos;
				}
				continue;
			}
			++pos;
		}
		filter = std::string(s + filterStart, pos - filterStart);
		TrimInPlace(&filter);
	}
	if (!JoinExpectChar(s, len, &pos, ')')) {
		return false;
	}
	while (pos < len && std::isspace(static_cast<unsigned char>(s[pos]))) {
		++pos;
	}
	if (pos != len) {
		return false;
	}

	std::vector<uid_collection_entry_t> items;
	if (!UID_FetchCollectionEntries(doc, backend, sourceId.c_str(), &items)) {
		return false;
	}

	uid_bool_lookup_ctx_t ctx;
	FillBoolLookupCtx(&ctx, doc, nodeId, backend);
	ctx.itemCount = static_cast<int>(items.size());
	ctx.selectedIndex = -1;

	std::string joined;
	joined.reserve(256);
	for (size_t i = 0; i < items.size(); ++i) {
		ctx.item = &items[i];
		ctx.itemIndex = static_cast<int>(i);
		if (!filter.empty()) {
			bool keep = false;
			std::string diag;
			if (!UID_EvalBool(filter.c_str(), &ctx, nullptr, &keep, &diag) || !keep) {
				continue;
			}
		}
		const std::string piece = JoinFieldValue(items[i], fieldId);
		if (piece.empty()) {
			continue;
		}
		if (!joined.empty()) {
			joined += sep;
		}
		joined += piece;
		if (joined.size() >= kJoinMaxChars) {
			joined.resize(kJoinMaxChars);
			break;
		}
	}
	*out = joined;
	return true;
}

/* Added in OPM: label braces may be join(...) strings or numeric embeds. */
static bool ResolveAllRuntimeLabelBraceExprs(
	uid_document_t *doc,
	uid_node_id_t nodeId,
	const std::string &authored,
	const uid_backend_t *backend,
	std::string *out
)
{
	if (!out) {
		return false;
	}
	if (authored.find('{') == std::string::npos) {
		return false;
	}

	std::string cur = authored;
	for (int guard = 0; guard < 64; ++guard) {
		const size_t start = cur.find('{');
		if (start == std::string::npos) {
			*out = cur;
			return true;
		}
		const size_t end = cur.find('}', start + 1);
		if (end == std::string::npos) {
			return false;
		}
		std::string expr = cur.substr(start + 1, end - start - 1);
		TrimInPlace(&expr);

		std::string replacement;
		if (expr.size() >= 5 && expr.compare(0, 4, "join") == 0 &&
		    (expr.size() == 4 || expr[4] == '(' || std::isspace(static_cast<unsigned char>(expr[4])))) {
			if (!EvalJoinCall(doc, nodeId, expr, backend, &replacement)) {
				return false;
			}
		} else {
			double value = 0.0;
			if (!EvalRuntimeNumericExprImpl(doc, nodeId, expr, backend, &value)) {
				return false;
			}
			const size_t after = end + 1;
			size_t       tokenEnd = after;
			while (tokenEnd < cur.size() && !std::isspace(static_cast<unsigned char>(cur[tokenEnd]))) {
				++tokenEnd;
			}
			const std::string unitSuffix = cur.substr(after, tokenEnd - after);
			replacement = FormatEvaluatedNumber(value) + unitSuffix;
			cur.replace(start, tokenEnd - start, replacement);
			continue;
		}
		cur.replace(start, end - start + 1, replacement);
	}
	return false;
}

/* Added in OPM: evaluate {expr} embeds in label text (e.g. floor(cvar…/60) for MM:SS). */
static void SyncInterpolatedLabelText(
	uid_document_t *doc,
	uid_node_id_t id,
	uid_node_def_t *node,
	uid_node_state_t *st,
	const uid_backend_t *backend
)
{
	if (!doc || !node || !st || !backend) {
		return;
	}
	if (node->text.empty() || node->text.find('{') == std::string::npos) {
		return;
	}
	/* Exact {cvar.name} passthrough — keep as string, not numeric format. */
	std::string cvarName;
	if (UID_ParseExactCvarBraceBinding(node->text, &cvarName)) {
		std::string value;
		if (!ReadCvarString(backend, cvarName.c_str(), &value)) {
			value.clear();
		}
		SetRuntimeIfChanged(doc, id, st, node->kind, value);
		return;
	}
	/* Foreach item.field text is handled by SyncForeachItemFieldText. */
	if (node->foreachGenerated && node->text.find("{item.field.") != std::string::npos &&
	    node->text.find("cvar.") == std::string::npos &&
	    node->text.find("join(") == std::string::npos) {
		return;
	}
	std::string resolved;
	if (!ResolveAllRuntimeLabelBraceExprs(doc, id, node->text, backend, &resolved)) {
		return;
	}
	SetRuntimeIfChanged(doc, id, st, node->kind, resolved);
}

} // namespace

bool UID_EvalRuntimeNumericExpr(
	uid_document_t      *doc,
	uid_node_id_t        nodeId,
	const std::string   &expr,
	const uid_backend_t *backend,
	double              *out
)
{
	return EvalRuntimeNumericExprImpl(doc, nodeId, expr, backend, out);
}

uid_result_t UID_TryCommitKeybindCapture(
	uid_document_t *doc,
	uid_node_id_t nodeId,
	int capturedKey,
	const uid_backend_t *backend
)
{
	return TryCommitKeybindCaptureImpl(doc, nodeId, capturedKey, backend);
}

uid_result_t UID_CommitKeybindFromModalCvars(uid_document_t *doc, const uid_backend_t *backend)
{
	return CommitKeybindFromModalCvarsImpl(doc, backend);
}

static void TrimCvarBraceBinding(std::string *s)
{
	if (!s) {
		return;
	}
	size_t b = 0;
	while (b < s->size() && std::isspace(static_cast<unsigned char>((*s)[b]))) {
		++b;
	}
	size_t e = s->size();
	while (e > b && std::isspace(static_cast<unsigned char>((*s)[e - 1]))) {
		--e;
	}
	*s = s->substr(b, e - b);
}

bool UID_ParseExactCvarBraceBinding(const std::string &value, std::string *cvarNameOut)
{
	if (!cvarNameOut) {
		return false;
	}
	cvarNameOut->clear();
	std::string trimmed = value;
	TrimCvarBraceBinding(&trimmed);
	if (trimmed.size() < 3 || trimmed.front() != '{' || trimmed.back() != '}') {
		return false;
	}
	std::string inner = trimmed.substr(1, trimmed.size() - 2);
	TrimCvarBraceBinding(&inner);
	if (inner.size() < 6 || inner.compare(0, 4, "cvar") != 0) {
		return false;
	}
	if (inner[4] != ':' && inner[4] != '.') {
		return false;
	}
	std::string name = inner.substr(5);
	TrimCvarBraceBinding(&name);
	if (name.empty()) {
		return false;
	}
	/*
	 * Fixed in OPM: only exact {cvar.name} / {cvar:name} — not style ternaries
	 * that begin with cvar. (e.g. "{cvar.a != cvar.b ? …}"). Those must stay on
	 * the styleExprs path; treating them as cvar binds made SyncCvarBoundProps
	 * restore the unresolved ternary after SyncBoundStyleExprs.
	 */
	for (char ch : name) {
		const unsigned char c = static_cast<unsigned char>(ch);
		if (std::isalnum(c) || ch == '_' || ch == '-' || ch == '.') {
			continue;
		}
		return false;
	}
	*cvarNameOut = name;
	return true;
}

void UID_RegisterCvarBoundProps(uid_node_def_t *node)
{
	if (!node) {
		return;
	}
	node->bindingFlagsValid = false;
	static const char *kProps[] = {
		"width", "height", "gap", "margin", "padding", "font-size", "rotation", "rotation-origin",
		"translate-x", "translate-y",
		"opacity", "background-image", "mask-image", "color", "fill", "left", "top", "right", "bottom",
		"background-scale",
		/* Added in OPM: leaf <image> */
		"src", "fit", "scale"
	};
	node->cvarBoundProps.clear();
	node->exprBoundProps.clear();
	for (const char *prop : kProps) {
		std::string value;
		if (!node->properties.Get(prop, &value) || value.empty()) {
			continue;
		}
		if (std::strcmp(prop, "background-image") == 0 || std::strcmp(prop, "mask-image") == 0 ||
			std::strcmp(prop, "src") == 0) {
			std::string cvarName;
			if (UID_ParseExactCvarBraceBinding(value, &cvarName)) {
				node->cvarBoundProps[prop] = value;
			} else if (value.find("item.field.") != std::string::npos) {
				node->exprBoundProps[prop] = value;
			} else if (value.find("item.lifetime_alpha") != std::string::npos) {
				node->exprBoundProps[prop] = value;
			} else if (IsExprBoundPropValue(value)) {
				node->exprBoundProps[prop] = value;
			}
			continue;
		}
		std::string cvarName;
		if (UID_ParseExactCvarBraceBinding(value, &cvarName)) {
			node->cvarBoundProps[prop] = value;
		} else if (value.find("item.field.") != std::string::npos) {
			/* Added in OPM: keep authored item.field placeholders (incl. ternaries)
			 * so SyncExprBoundProps can refresh them after same-key collection updates. */
			node->exprBoundProps[prop] = value;
		} else if (value.find("item.lifetime_alpha") != std::string::npos) {
			node->exprBoundProps[prop] = value;
		} else if (IsExprBoundPropValue(value)) {
			node->exprBoundProps[prop] = value;
		}
	}
}

bool UID_ParseCvarBind(const char *bind, std::string *cvarNameOut)
{
	if (!bind || !cvarNameOut) {
		return false;
	}

	const char *p = bind;
	while (*p && std::isspace(static_cast<unsigned char>(*p))) {
		++p;
	}
	if (!*p) {
		return false;
	}

	/* Canonical: cvar:name */
	if (std::strncmp(p, "cvar:", 5) == 0) {
		std::string name(p + 5);
		TrimInPlace(&name);
		if (name.empty()) {
			return false;
		}
		*cvarNameOut = name;
		return true;
	}

	/* Compat: cvar(name) */
	if (std::strncmp(p, "cvar(", 5) == 0) {
		p += 5;
		const char *close = std::strchr(p, ')');
		if (!close || close == p) {
			return false;
		}
		std::string name(p, close);
		TrimInPlace(&name);
		if (name.empty()) {
			return false;
		}
		*cvarNameOut = name;
		return true;
	}

	return false;
}

bool UID_ParseItemFieldBind(const char *bind, std::string *fieldNameOut)
{
	if (!bind || !fieldNameOut) {
		return false;
	}

	const char *p = bind;
	while (*p && std::isspace(static_cast<unsigned char>(*p))) {
		++p;
	}
	if (!*p) {
		return false;
	}

	/* Canonical: item.field:name */
	if (std::strncmp(p, "item.field:", 11) == 0) {
		std::string name(p + 11);
		TrimInPlace(&name);
		if (name.empty()) {
			return false;
		}
		*fieldNameOut = name;
		return true;
	}

	/* Compat: item.field.name (path style) */
	if (std::strncmp(p, "item.field.", 11) == 0) {
		std::string name(p + 11);
		TrimInPlace(&name);
		if (name.empty() || name.find('{') != std::string::npos) {
			return false;
		}
		*fieldNameOut = name;
		return true;
	}

	return false;
}

void UID_SyncBindings(uid_document_t *doc, const uid_backend_t *backend)
{
	if (!doc || !backend) {
		return;
	}

	++doc->syncFrameCounter;

	g_cvarMemo.clear();
	g_cvarMemoActive = UID_OptEnabled(UID_OPT_CVAR_MEMO) != 0;

	UID_SyncModals(doc, backend);

	auto ensureBindingFlags = [](uid_node_def_t *node) {
		if (!node || node->bindingFlagsValid) {
			return;
		}
		unsigned flags = 0;
		if (!node->visibleExpr.empty()) {
			flags |= UID_BIND_F_VISIBLE_EXPR;
		}
		if (!node->enabledExpr.empty()) {
			flags |= UID_BIND_F_ENABLED_EXPR;
		}
		if (!node->styleExprs.empty()) {
			flags |= UID_BIND_F_STYLE;
		}
		if (!node->cvarBoundProps.empty()) {
			flags |= UID_BIND_F_CVAR_PROPS;
		}
		if (!node->exprBoundProps.empty()) {
			flags |= UID_BIND_F_EXPR_PROPS;
		}
		if (!node->bind.empty()) {
			flags |= UID_BIND_F_BIND;
		}
		if (!node->optionSource.empty()) {
			flags |= UID_BIND_F_OPTION_SOURCE;
		}
		if (node->kind == UID_NODE_LABEL) {
			flags |= UID_BIND_F_LABEL;
		}
		if (node->kind == UID_NODE_KEYBIND) {
			flags |= UID_BIND_F_KEYBIND;
		}
		if (node->kind == UID_NODE_SELECT) {
			flags |= UID_BIND_F_SELECT;
		}
		node->bindingFlags = flags;
		node->bindingFlagsValid = true;
	};

	auto probeVisibleEnabled = [](uid_node_def_t *node) {
		if (!node->visibleExprProbed) {
			node->visibleExprProbed = true;
			if (node->visibleExpr.empty()) {
				std::string vis;
				if (node->properties.Get("visible", &vis)) {
					std::string inner;
					if (UID_ParseBraceBoolExpr(vis.c_str(), &inner)) {
						node->visibleExpr = inner;
						node->bindingFlagsValid = false;
					}
				}
			}
		}
		if (!node->enabledExprProbed) {
			node->enabledExprProbed = true;
			if (node->enabledExpr.empty()) {
				std::string en;
				if (node->properties.Get("enabled", &en)) {
					std::string inner;
					if (UID_ParseBraceBoolExpr(en.c_str(), &inner)) {
						node->enabledExpr = inner;
						node->bindingFlagsValid = false;
					}
				}
			}
		}
	};

	/*
	 * Added in OPM: apply visibleExpr before SyncCollections so visibility-aware
	 * collection cull sees this frame's panel visibility (not last frame).
	 * Always recurse so hidden panels update before a same-frame reveal.
	 */
	UID_ProfileBegin(UID_PROF_FRAME_COLLECTION_CULL);
	auto applyVisibility = [&](auto &self, uid_node_id_t id) -> void {
		if (id < 0 || static_cast<size_t>(id) >= doc->nodes.size() || static_cast<size_t>(id) >= doc->states.size()) {
			return;
		}
		uid_node_def_t *node = &doc->nodes[static_cast<size_t>(id)];
		uid_node_state_t *st = &doc->states[static_cast<size_t>(id)];
		probeVisibleEnabled(node);
		ensureBindingFlags(node);
		if (node->bindingFlags & UID_BIND_F_VISIBLE_EXPR) {
			const bool show = EvalNodeBoolExprCached(node->visibleExpr, doc, id, backend, st, true);
			const char *cur = node->properties.GetCStr("visible", "");
			if (!cur || std::strcmp(cur, show ? "true" : "false") != 0) {
				node->properties.Set("visible", show ? "true" : "false");
				MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
			}
		}
		for (uid_node_id_t c : node->children) {
			self(self, c);
		}
	};
	if (doc->rootNode != UID_INVALID_NODE_ID) {
		applyVisibility(applyVisibility, doc->rootNode);
	}
	if (UID_IsModalActive(doc)) {
		const uid_node_id_t modalRoot = UID_GetModalRoot(doc);
		if (modalRoot != UID_INVALID_NODE_ID) {
			applyVisibility(applyVisibility, modalRoot);
		}
	}
	UID_ProfileEnd(UID_PROF_FRAME_COLLECTION_CULL);

	UID_SyncCollections(doc, backend);

	auto syncOneNodeBody = [&](uid_document_t *d, uid_node_id_t id, uid_node_def_t *node, uid_node_state_t *st) {
		ensureBindingFlags(node);
		const unsigned flags = node->bindingFlags;

		if (flags & UID_BIND_F_VISIBLE_EXPR) {
			const bool show = EvalNodeBoolExprCached(node->visibleExpr, d, id, backend, st, true);
			const char *cur = node->properties.GetCStr("visible", "");
			if (!cur || std::strcmp(cur, show ? "true" : "false") != 0) {
				node->properties.Set("visible", show ? "true" : "false");
				MarkDirty(d, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
			}
		}

		if (flags & UID_BIND_F_ENABLED_EXPR) {
			const bool on = EvalNodeBoolExprCached(node->enabledExpr, d, id, backend, st, false);
			const char *cur = node->properties.GetCStr("enabled", "");
			if (!cur || std::strcmp(cur, on ? "true" : "false") != 0) {
				node->properties.Set("enabled", on ? "true" : "false");
				MarkDirty(d, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
			}
		}

		if (flags & UID_BIND_F_STYLE) {
			SyncBoundStyleExprs(d, id, node, backend);
		}
		if (flags & UID_BIND_F_CVAR_PROPS) {
			SyncCvarBoundProps(d, id, node, backend);
		}
		if (flags & UID_BIND_F_EXPR_PROPS) {
			SyncExprBoundProps(d, id, node, backend);
		}

		if ((flags & UID_BIND_F_SELECT) && (flags & UID_BIND_F_OPTION_SOURCE)) {
			RefreshOptionSource(d, node, backend);
		}

		if (flags & UID_BIND_F_LABEL) {
			SyncTextCvarLabel(d, id, node, st, backend);
			SyncForeachItemFieldText(d, id, node, st, backend);
			SyncInterpolatedLabelText(d, id, node, st, backend);
		}

		if (flags & UID_BIND_F_KEYBIND) {
			SyncKeybindDisplay(d, id, node, st, backend);
			return;
		}

		if (flags & UID_BIND_F_BIND) {
			std::string cvarName;
			if (UID_ParseCvarBind(node->bind.c_str(), &cvarName)) {
				SyncCvarBind(d, id, node, st, backend, cvarName);
			}
		}
	};

	auto syncRecursive = [&](auto &self, uid_node_id_t id, bool ancestorVisible) -> void {
		if (id < 0 || static_cast<size_t>(id) >= doc->nodes.size() || static_cast<size_t>(id) >= doc->states.size()) {
			return;
		}
		uid_node_def_t *node = &doc->nodes[static_cast<size_t>(id)];
		uid_node_state_t *st = &doc->states[static_cast<size_t>(id)];

		probeVisibleEnabled(node);
		ensureBindingFlags(node);

		/* Always evaluate this node's visibility first. */
		if (node->bindingFlags & UID_BIND_F_VISIBLE_EXPR) {
			const bool show = EvalNodeBoolExprCached(node->visibleExpr, doc, id, backend, st, true);
			const char *cur = node->properties.GetCStr("visible", "");
			if (!cur || std::strcmp(cur, show ? "true" : "false") != 0) {
				node->properties.Set("visible", show ? "true" : "false");
				MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
			}
		}

		bool selfVisible = ancestorVisible;
		{
			const char *vis = node->properties.GetCStr("visible", "true");
			bool v = true;
			if (vis && UID_ParseBool(vis, &v, nullptr)) {
				selfVisible = ancestorVisible && v;
			} else {
				selfVisible = ancestorVisible;
			}
		}

		if (!selfVisible) {
			return;
		}

		/* Visible: run remaining sync (skip redoing visibleExpr). */
		unsigned flags = node->bindingFlags;
		if (flags & UID_BIND_F_ENABLED_EXPR) {
			const bool on = EvalNodeBoolExprCached(node->enabledExpr, doc, id, backend, st, false);
			const char *cur = node->properties.GetCStr("enabled", "");
			if (!cur || std::strcmp(cur, on ? "true" : "false") != 0) {
				node->properties.Set("enabled", on ? "true" : "false");
				MarkDirty(doc, static_cast<uid_dirty_flags_t>(UID_DIRTY_LAYOUT | UID_DIRTY_PAINT));
			}
		}
		if (flags & UID_BIND_F_STYLE) {
			SyncBoundStyleExprs(doc, id, node, backend);
		}
		if (flags & UID_BIND_F_CVAR_PROPS) {
			SyncCvarBoundProps(doc, id, node, backend);
		}
		if (flags & UID_BIND_F_EXPR_PROPS) {
			SyncExprBoundProps(doc, id, node, backend);
		}
		if ((flags & UID_BIND_F_SELECT) && (flags & UID_BIND_F_OPTION_SOURCE)) {
			RefreshOptionSource(doc, node, backend);
		}
		if (flags & UID_BIND_F_LABEL) {
			SyncTextCvarLabel(doc, id, node, st, backend);
			SyncForeachItemFieldText(doc, id, node, st, backend);
			SyncInterpolatedLabelText(doc, id, node, st, backend);
		}
		if (flags & UID_BIND_F_KEYBIND) {
			SyncKeybindDisplay(doc, id, node, st, backend);
		} else if (flags & UID_BIND_F_BIND) {
			std::string cvarName;
			if (UID_ParseCvarBind(node->bind.c_str(), &cvarName)) {
				SyncCvarBind(doc, id, node, st, backend, cvarName);
			}
		}

		for (uid_node_id_t c : node->children) {
			self(self, c, true);
		}
	};

	auto syncAllNodesFlat = [&]() {
		const size_t n = doc->nodes.size() < doc->states.size() ? doc->nodes.size() : doc->states.size();
		for (size_t i = 0; i < n; ++i) {
			uid_node_def_t *node = &doc->nodes[i];
			uid_node_state_t *st = &doc->states[i];
			probeVisibleEnabled(node);
			syncOneNodeBody(doc, static_cast<uid_node_id_t>(i), node, st);
		}
	};

	if (UID_OptEnabled(UID_OPT_BIND_CULL)) {
		if (doc->rootNode != UID_INVALID_NODE_ID) {
			syncRecursive(syncRecursive, doc->rootNode, true);
		}
		if (UID_IsModalActive(doc)) {
			const uid_node_id_t modalRoot = UID_GetModalRoot(doc);
			if (modalRoot != UID_INVALID_NODE_ID) {
				syncRecursive(syncRecursive, modalRoot, true);
			}
		}
	} else {
		syncAllNodesFlat();
	}

	g_cvarMemoActive = false;
	g_cvarMemo.clear();
	doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty & ~UID_DIRTY_BINDING);
}

uid_result_t UID_WriteBinding(uid_document_t *doc, uid_node_id_t nodeId, const uid_backend_t *backend)
{
	if (!doc || !backend) {
		return UID_ERR_INVALID_ARG;
	}

	uid_node_def_t *node = UID_GetNode(doc, nodeId);
	if (!node) {
		return UID_ERR_INVALID_ARG;
	}
	if (nodeId < 0 || static_cast<size_t>(nodeId) >= doc->states.size()) {
		return UID_ERR_INVALID_ARG;
	}
	uid_node_state_t *st = &doc->states[static_cast<size_t>(nodeId)];

	if (node->kind == UID_NODE_KEYBIND) {
		return WriteKeybind(node, st, backend);
	}

	if (node->bind.empty()) {
		return UID_ERR_INVALID_ARG;
	}

	std::string cvarName;
	if (!UID_ParseCvarBind(node->bind.c_str(), &cvarName)) {
		return UID_ERR_VALIDATE;
	}

	/*
	 * Commit modes share the same write path: CHANGE callers write on each
	 * accepted edit; SUBMIT/APPLY callers write when flushing staged values.
	 * Staging is preserved on sync (see StagingBlocksSync), not here.
	 */
	(void)node->commit;
	return WriteCvarBind(node, st, backend, cvarName);
}

uid_result_t UID_WriteAllBindings(uid_document_t *doc, const uid_backend_t *backend)
{
	if (!doc || !backend) {
		return UID_ERR_INVALID_ARG;
	}

	uid_result_t worst = UID_OK;
	const size_t n = doc->nodes.size() < doc->states.size() ? doc->nodes.size() : doc->states.size();
	for (size_t i = 0; i < n; ++i) {
		const uid_node_def_t &node = doc->nodes[i];
		bool shouldWrite = false;
		/*
		 * Keybind nodes commit on capture (UID_TryCommitKeybindCapture) or explicit
		 * clear — not via bulk flush. runtimeValue holds display labels, not keys.
		 */
		if (!node.bind.empty() && node.hasCommit && node.commit == UID_COMMIT_APPLY) {
			shouldWrite = true;
		}
		if (!shouldWrite) {
			continue;
		}
		const uid_result_t r = UID_WriteBinding(doc, static_cast<uid_node_id_t>(i), backend);
		if (r != UID_OK && worst == UID_OK) {
			worst = r;
		}
	}
	return worst;
}

std::string UID_TransformCvarToUi(
	const uid_node_def_t &node,
	const std::string &cvarValue,
	const uid_backend_t *backend
)
{
	return TransformCvarToUi(node, cvarValue, backend);
}

std::string UID_KeybindEmptyLabel(const uid_node_def_t &node)
{
	const char *label = node.properties.GetCStr("empty-label", nullptr);
	return (label && label[0]) ? label : "NONE";
}

std::string UID_KeybindCaptureLabel(const uid_node_def_t &node)
{
	const char *label = node.properties.GetCStr("capture-label", nullptr);
	return (label && label[0]) ? label : "Press a key...";
}
