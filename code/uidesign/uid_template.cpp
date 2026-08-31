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

#include "uid_template.h"

#include "uid_binding.h"
#include "uid_document.h"
#include "uid_expr.h"
#include "uid_expr_bool.h"
#include "uid_value.h"
#include "uid_vars.h"
#include "uid_xml.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

static const char *const kInheritedTextStyleKeys[] = {
	"font", "font-size", "font-weight", "color",
	"line-height", "text-skew", "letter-spacing", "text-wrap", "drop-shadow"
};

bool IsInheritedTextStyleKey(const std::string &name)
{
	for (const char *k : kInheritedTextStyleKeys) {
		if (name == k) {
			return true;
		}
	}
	return false;
}

void CopyInheritedTextStyle(const uid_property_set_t &from, uid_property_set_t *to)
{
	for (const char *k : kInheritedTextStyleKeys) {
		std::string v;
		if (from.Get(k, &v)) {
			to->Set(k, v);
		}
	}
}

/*
 * Cascade: built-in defaults → document <defaults> → inherited text style →
 * template/use instance values → explicit node attributes.
 */
void ResolveProperties(
	const uid_document_t *doc,
	const uid_property_set_t &inheritedTextStyle,
	const uid_property_set_t *templateProps,
	const uid_property_set_t &nodeAttrs,
	uid_property_set_t *out
)
{
	out->Clear();
	UID_ApplyBuiltinDefaults(out);

	for (const auto &kv : doc->definitions.defaults.Attrs()) {
		/*
		 * Do not stamp halign/valign from <defaults> onto every node.
		 * Those attrs are dual-purpose: container packing vs leaf text origin.
		 * Stamping "start" (as main.xml once did) forced button labels top-left
		 * and overrode kind-specific text defaults (button → center/center).
		 * Container packing still falls back via PropCStr(..., "start").
		 * Explicit node/template halign/valign continue to apply below.
		 */
		if (kv.first == "halign" || kv.first == "valign") {
			continue;
		}
		out->Set(kv.first.c_str(), kv.second.value);
	}

	for (const auto &kv : inheritedTextStyle.Attrs()) {
		if (IsInheritedTextStyleKey(kv.first)) {
			out->Set(kv.first.c_str(), kv.second.value);
		}
	}

	if (templateProps) {
		for (const auto &kv : templateProps->Attrs()) {
			out->Set(kv.first.c_str(), kv.second.value);
		}
	}

	for (const auto &kv : nodeAttrs.Attrs()) {
		out->Set(kv.first.c_str(), kv.second.value);
	}
}

bool TemplateHasLocalIds(const uid_template_def_t &tmpl)
{
	for (const uid_node_def_t &n : tmpl.nodes) {
		if (!n.id.empty()) {
			return true;
		}
	}
	return false;
}

void CollectLocalIds(const uid_template_def_t &tmpl, std::set<std::string> *out)
{
	out->clear();
	for (const uid_node_def_t &n : tmpl.nodes) {
		if (!n.id.empty()) {
			out->insert(n.id);
		}
	}
}

bool IsNumericAttrName(const std::string &name)
{
	return name == "width" || name == "height" || name == "gap" || name == "padding" ||
		name == "margin" || name == "font-size" || name == "font-weight" || name == "radius" ||
		name == "min" || name == "max" || name == "step" || name == "tab-index" ||
		name == "max-length" || name == "stroke-width";
}

bool IsSingleBraceExpr(const std::string &text, std::string *innerOut)
{
	size_t b = 0;
	while (b < text.size() && std::isspace(static_cast<unsigned char>(text[b]))) {
		++b;
	}
	size_t e = text.size();
	while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) {
		--e;
	}
	if (e <= b || text[b] != '{' || text[e - 1] != '}') {
		return false;
	}
	for (size_t i = b + 1; i + 1 < e; ++i) {
		if (text[i] == '{' || text[i] == '}') {
			return false;
		}
	}
	if (innerOut) {
		*innerOut = text.substr(b + 1, e - b - 2);
	}
	return true;
}

bool IsSimpleDottedPath(const std::string &expr, const char *prefix, std::string *keyOut)
{
	const size_t plen = std::strlen(prefix);
	size_t b = 0;
	while (b < expr.size() && std::isspace(static_cast<unsigned char>(expr[b]))) {
		++b;
	}
	if (expr.compare(b, plen, prefix) != 0) {
		return false;
	}
	size_t i = b + plen;
	if (i >= expr.size() ||
		!(std::isalpha(static_cast<unsigned char>(expr[i])) || expr[i] == '_')) {
		return false;
	}
	const size_t start = i;
	++i;
	while (i < expr.size() &&
		   (std::isalnum(static_cast<unsigned char>(expr[i])) || expr[i] == '_' || expr[i] == '-')) {
		++i;
	}
	size_t end = i;
	while (end < expr.size() && std::isspace(static_cast<unsigned char>(expr[end]))) {
		++end;
	}
	if (end != expr.size()) {
		return false;
	}
	if (keyOut) {
		*keyOut = expr.substr(start, i - start);
	}
	return true;
}

struct ContextLookupState {
	const uid_document_t     *doc;
	const uid_property_set_t *templateProps;
	const uid_property_set_t *parentProps;
};

bool LookupContextNumber(void *userdata, const char *path, double *out)
{
	if (!userdata || !path || !out) {
		return false;
	}
	const ContextLookupState *st = static_cast<const ContextLookupState *>(userdata);
	if (std::strncmp(path, "var.", 4) == 0) {
		return st->doc && UID_LookupVarNumber(st->doc, path + 4, out);
	}
	const uid_property_set_t *bag = nullptr;
	const char *key = nullptr;
	if (std::strncmp(path, "template.", 9) == 0) {
		bag = st->templateProps;
		key = path + 9;
	} else if (std::strncmp(path, "parent.", 7) == 0) {
		bag = st->parentProps;
		key = path + 7;
	} else {
		return false;
	}
	if (!bag || !key || !*key) {
		return false;
	}
	std::string v;
	if (!bag->Get(key, &v)) {
		return false;
	}
	std::string dm;
	if (UID_ParseNumber(v.c_str(), out, &dm)) {
		return true;
	}
	uid_length_t len;
	if (UID_ParseLength(v.c_str(), &len, &dm) && len.unit == UID_LENGTH_PX) {
		*out = static_cast<double>(len.value);
		return true;
	}
	return false;
}

std::string FormatNumberLocaleIndependent(double v)
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.6g", v);
	return std::string(buf);
}

/* Match '}' for the '{' at openIdx; supports nesting. */
size_t FindMatchingBraceEnd(const std::string &input, size_t openIdx)
{
	if (openIdx >= input.size() || input[openIdx] != '{') {
		return std::string::npos;
	}
	int depth = 0;
	for (size_t j = openIdx; j < input.size(); ++j) {
		if (input[j] == '{') {
			++depth;
		} else if (input[j] == '}') {
			--depth;
			if (depth == 0) {
				return j;
			}
		}
	}
	return std::string::npos;
}

/*
 * Added in OPM: bake template./parent. idents inside a mixed brace expr when
 * the whole expression cannot evaluate at expand (e.g. still contains cvar.).
 * Runtime lookup has no template bag - without this, mixed exprs break.
 * Nested {embeds} are left alone; non-numeric template props are left intact.
 */
bool RewriteContextIdentsInExpr(
	const std::string        &inner,
	const ContextLookupState &st,
	std::string              *out,
	std::string              *diag
)
{
	out->clear();
	size_t i = 0;
	while (i < inner.size()) {
		if (inner[i] == '{') {
			const size_t end = FindMatchingBraceEnd(inner, i);
			if (end == std::string::npos) {
				if (diag) {
					*diag = "unclosed '{' in template context reference";
				}
				return false;
			}
			out->append(inner, i, end - i + 1);
			i = end + 1;
			continue;
		}
		const unsigned char c = static_cast<unsigned char>(inner[i]);
		if (!(std::isalpha(c) || c == '_')) {
			out->push_back(inner[i++]);
			continue;
		}
		const size_t start = i;
		++i;
		while (i < inner.size()) {
			const unsigned char ch = static_cast<unsigned char>(inner[i]);
			if (std::isalnum(ch) || ch == '_' || ch == '.' || ch == '-') {
				++i;
			} else {
				break;
			}
		}
		const std::string ident = inner.substr(start, i - start);
		if (ident.compare(0, 9, "template.") == 0 || ident.compare(0, 7, "parent.") == 0) {
			double num = 0.0;
			if (LookupContextNumber(const_cast<ContextLookupState *>(&st), ident.c_str(), &num)) {
				out->append(FormatNumberLocaleIndependent(num));
				continue;
			}
			if (ident.compare(0, 9, "template.") == 0) {
				const char *key = ident.c_str() + 9;
				std::string v;
				if (st.templateProps && st.templateProps->Get(key, &v)) {
					/*
					 * Changed in OPM: inline string template props in bool/style
					 * exprs (e.g. visible="{template.slider-visible}") so gates
					 * passed as props evaluate at runtime. Strip outer braces.
					 */
					std::string braceInner;
					if (UID_ParseBraceBoolExpr(v.c_str(), &braceInner)) {
						out->append(braceInner);
					} else {
						out->append(v);
					}
					continue;
				}
				if (diag) {
					*diag = "unknown template prop: " + std::string(key);
				}
				return false;
			}
			/* parent. may resolve later - leave intact. */
		}
		out->append(ident);
	}
	return true;
}

/*
 * Lexical {template.*} (and available {parent.*}) embeds during expand.
 * Unknown parent/shape embeds are left intact for later layout/shape phases.
 */
bool SubstituteContextRefs(
	const uid_document_t     *doc,
	const std::string        &input,
	const uid_property_set_t *templateProps,
	const uid_property_set_t *parentProps,
	const uid_expr_limits_t  *exprLim,
	bool                      preferNumber,
	std::string              *out,
	std::string              *diag
)
{
	out->clear();
	ContextLookupState st{doc, templateProps, parentProps};
	size_t i = 0;
	while (i < input.size()) {
		if (input[i] != '{') {
			out->push_back(input[i++]);
			continue;
		}
		const size_t end = FindMatchingBraceEnd(input, i);
		if (end == std::string::npos) {
			if (diag) {
				*diag = "unclosed '{' in template context reference";
			}
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

		std::string key;
		if (IsSimpleDottedPath(inner, "template.", &key)) {
			std::string value;
			if (!templateProps || !templateProps->Get(key.c_str(), &value)) {
				if (diag) {
					*diag = "unknown template prop: " + key;
				}
				return false;
			}
			if (preferNumber) {
				double num = 0.0;
				std::string dm;
				if (UID_ParseNumber(value.c_str(), &num, &dm)) {
					out->append(FormatNumberLocaleIndependent(num));
				} else {
					out->append(value);
				}
			} else {
				out->append(value);
			}
			i = end + 1;
			continue;
		}
		if (IsSimpleDottedPath(inner, "parent.", &key)) {
			std::string value;
			if (parentProps && parentProps->Get(key.c_str(), &value)) {
				out->append(value);
			} else {
				out->push_back('{');
				out->append(inner);
				out->push_back('}');
			}
			i = end + 1;
			continue;
		}

		if (exprLim) {
			double num = 0.0;
			std::string dm;
			if (UID_EvalNumber(inner.c_str(), LookupContextNumber, &st, exprLim, &num, &dm)) {
				out->append(FormatNumberLocaleIndependent(num));
				i = end + 1;
				continue;
			}
		}

		/*
		 * Added in OPM: expand nested {template.} embeds first, then bake any
		 * remaining bare numeric template./parent. idents for mixed cvar exprs.
		 */
		std::string nestedOut;
		if (!SubstituteContextRefs(
				doc, inner, templateProps, parentProps, exprLim, preferNumber, &nestedOut, diag
			)) {
			return false;
		}
		if (exprLim) {
			double num = 0.0;
			std::string dm;
			if (UID_EvalNumber(nestedOut.c_str(), LookupContextNumber, &st, exprLim, &num, &dm)) {
				out->append(FormatNumberLocaleIndependent(num));
				i = end + 1;
				continue;
			}
		}
		std::string rewritten;
		if (!RewriteContextIdentsInExpr(nestedOut, st, &rewritten, diag)) {
			return false;
		}
		out->push_back('{');
		out->append(rewritten);
		out->push_back('}');
		i = end + 1;
	}
	return true;
}

void CanonicalizeBind(std::string *bind)
{
	if (!bind || bind->empty()) {
		return;
	}
	std::string cvarName;
	if (UID_ParseCvarBind(bind->c_str(), &cvarName)) {
		*bind = "cvar:" + cvarName;
	}
}

struct ExpandContext {
	uid_document_t  *doc;
	uid_diag_list_t *diags;
	std::vector<uid_node_def_t> outNodes;
	std::unordered_map<std::string, uid_node_id_t> idIndex;
	std::set<std::string> expandStack;
	bool failed;

	void Error(const uid_source_location_t &loc, const char *msg)
	{
		failed = true;
		if (diags) {
			diags->Error(loc, msg);
		}
	}

	void Errorf(const uid_source_location_t &loc, const std::string &msg)
	{
		Error(loc, msg.c_str());
	}

	bool RegisterId(const std::string &id, uid_node_id_t nodeId, const uid_source_location_t &loc)
	{
		if (id.empty()) {
			return true;
		}
		if (static_cast<int>(id.size()) > doc->limits.maxIdLen) {
			Error(loc, "expanded id exceeds maxIdLen");
			return false;
		}
		if (!idIndex.emplace(id, nodeId).second) {
			Errorf(loc, "duplicate expanded id: " + id);
			return false;
		}
		return true;
	}

	bool CheckExpandedBudget(const uid_source_location_t &loc)
	{
		if (static_cast<int>(outNodes.size()) >= doc->limits.maxExpandedNodes) {
			Error(loc, "expanded node count exceeds limit");
			return false;
		}
		return true;
	}
};

void ApplyContextToNode(
	ExpandContext &ctx,
	uid_node_def_t *node,
	const uid_property_set_t *templateProps,
	const uid_property_set_t *parentProps,
	const std::set<std::string> *localIds,
	const std::string &useId
);

void ApplyContextToForeachTemplateTree(
	ExpandContext &ctx,
	std::vector<uid_node_def_t> *nodes,
	uid_node_id_t nodeId,
	const uid_property_set_t *templateProps,
	const uid_property_set_t *parentProps,
	const std::set<std::string> *localIds,
	const std::string &useId
)
{
	if (!nodes || nodeId < 0 || static_cast<size_t>(nodeId) >= nodes->size()) {
		return;
	}
	uid_node_def_t &node = (*nodes)[static_cast<size_t>(nodeId)];
	ApplyContextToNode(ctx, &node, templateProps, parentProps, localIds, useId);
	for (uid_node_id_t childId : node.children) {
		ApplyContextToForeachTemplateTree(
			ctx, nodes, childId, templateProps, parentProps, localIds, useId
		);
	}
}

void ApplyContextToNode(
	ExpandContext &ctx,
	uid_node_def_t *node,
	const uid_property_set_t *templateProps,
	const uid_property_set_t *parentProps,
	const std::set<std::string> *localIds,
	const std::string &useId
)
{
	if (!node || (!templateProps && !parentProps)) {
		return;
	}

	uid_expr_limits_t exprLim;
	exprLim.maxExprBytes = ctx.doc->limits.maxExprBytes;
	exprLim.maxExprNodes = ctx.doc->limits.maxExprNodes;

	std::string diag;
	std::string resolved;
	if (!node->text.empty()) {
		if (!SubstituteContextRefs(ctx.doc,node->text, templateProps, parentProps, &exprLim, false, &resolved, &diag)) {
			ctx.Errorf(node->source, diag);
			return;
		}
		node->text = resolved;
	}

	uid_property_set_t resolvedProps;
	for (const auto &kv : node->properties.Attrs()) {
		std::string value = kv.second.value;
		if (value.find('{') != std::string::npos) {
			const bool preferNumber = IsNumericAttrName(kv.first);
			std::string inner;
			if (preferNumber && IsSingleBraceExpr(value, &inner)) {
				ContextLookupState st{ctx.doc, templateProps, parentProps};
				double num = 0.0;
				if (UID_EvalNumber(inner.c_str(), LookupContextNumber, &st, &exprLim, &num, &diag)) {
					value = FormatNumberLocaleIndependent(num);
				} else if (!SubstituteContextRefs(ctx.doc,
							   value, templateProps, parentProps, &exprLim, true, &resolved, &diag
						   )) {
					ctx.Errorf(node->source, diag);
					return;
				} else {
					value = resolved;
				}
			} else if (!SubstituteContextRefs(ctx.doc,
						   value, templateProps, parentProps, &exprLim, preferNumber, &resolved, &diag
					   )) {
				ctx.Errorf(node->source, diag);
				return;
			} else {
				value = resolved;
			}
		}
		resolvedProps.Set(kv.first.c_str(), value);
	}
	node->properties = resolvedProps;

	if (!node->bind.empty()) {
		if (!SubstituteContextRefs(ctx.doc,node->bind, templateProps, parentProps, &exprLim, false, &resolved, &diag)) {
			ctx.Errorf(node->source, diag);
			return;
		}
		node->bind = resolved;
	} else {
		std::string b;
		if (node->properties.Get("bind", &b)) {
			node->bind = b;
		}
	}

	/* Added in OPM: brace-expand select appearance / optionSource like bind. */
	if (!node->appearance.empty() && node->appearance.find('{') != std::string::npos) {
		if (!SubstituteContextRefs(ctx.doc,node->appearance, templateProps, parentProps, &exprLim, false, &resolved, &diag)) {
			ctx.Errorf(node->source, diag);
			return;
		}
		node->appearance = resolved;
	}
	if (!node->optionSource.empty() && node->optionSource.find('{') != std::string::npos) {
		if (!SubstituteContextRefs(ctx.doc,node->optionSource, templateProps, parentProps, &exprLim, false, &resolved, &diag)) {
			ctx.Errorf(node->source, diag);
			return;
		}
		node->optionSource = resolved;
	}
	if (!node->valueType.empty() && node->valueType.find('{') != std::string::npos) {
		if (!SubstituteContextRefs(ctx.doc,node->valueType, templateProps, parentProps, &exprLim, false, &resolved, &diag)) {
			ctx.Errorf(node->source, diag);
			return;
		}
		node->valueType = resolved;
	}
	if (!node->visibleIf.empty() && node->visibleIf.find('{') != std::string::npos) {
		if (!SubstituteContextRefs(ctx.doc,node->visibleIf, templateProps, parentProps, &exprLim, false, &resolved, &diag)) {
			ctx.Errorf(node->source, diag);
			return;
		}
		node->visibleIf = resolved;
		if (node->visibleExpr.empty()) {
			UID_VisibleIfToBoolExpr(resolved, &node->visibleExpr);
		}
	}
	/* Added in OPM: brace-expand bool/style expression fields with {template.*}. */
	if (!node->visibleExpr.empty() && node->visibleExpr.find('{') != std::string::npos) {
		if (!SubstituteContextRefs(ctx.doc,node->visibleExpr, templateProps, parentProps, &exprLim, false, &resolved, &diag)) {
			ctx.Errorf(node->source, diag);
			return;
		}
		node->visibleExpr = resolved;
	}
	if (!node->enabledExpr.empty() && node->enabledExpr.find('{') != std::string::npos) {
		if (!SubstituteContextRefs(ctx.doc,node->enabledExpr, templateProps, parentProps, &exprLim, false, &resolved, &diag)) {
			ctx.Errorf(node->source, diag);
			return;
		}
		node->enabledExpr = resolved;
	}
	for (auto &kv : node->styleExprs) {
		if (kv.second.empty() || kv.second.find('{') == std::string::npos) {
			continue;
		}
		if (!SubstituteContextRefs(ctx.doc, kv.second, templateProps, parentProps, &exprLim, false, &resolved, &diag)) {
			ctx.Errorf(node->source, diag);
			return;
		}
		kv.second = resolved;
	}
	/*
	 * Added in OPM: style/bool exprs store the brace-stripped body. Bake bare
	 * template./parent. idents (mixed with cvar.) the same as braced attrs.
	 */
	if (templateProps || parentProps) {
		ContextLookupState bareSt{ctx.doc, templateProps, parentProps};
		auto bakeBare = [&](std::string *expr) -> bool {
			if (!expr || expr->empty()) {
				return true;
			}
			if (expr->find("template.") == std::string::npos && expr->find("parent.") == std::string::npos) {
				return true;
			}
			std::string rewritten;
			if (!RewriteContextIdentsInExpr(*expr, bareSt, &rewritten, &diag)) {
				ctx.Errorf(node->source, diag);
				return false;
			}
			*expr = rewritten;
			return true;
		};
		if (!bakeBare(&node->visibleExpr) || !bakeBare(&node->enabledExpr)) {
			return;
		}
		for (auto &kv : node->styleExprs) {
			if (!bakeBare(&kv.second)) {
				return;
			}
		}
	}
	if (!node->setValue.empty() && node->setValue.find('{') != std::string::npos) {
		if (!SubstituteContextRefs(ctx.doc,node->setValue, templateProps, parentProps, &exprLim, false, &resolved, &diag)) {
			ctx.Errorf(node->source, diag);
			return;
		}
		node->setValue = resolved;
	}
	/* Added in OPM: brace-expand keybind binding="{template.*}" like bind. */
	if (!node->binding.empty() && node->binding.find('{') != std::string::npos) {
		if (!SubstituteContextRefs(ctx.doc,node->binding, templateProps, parentProps, &exprLim, false, &resolved, &diag)) {
			ctx.Errorf(node->source, diag);
			return;
		}
		node->binding = resolved;
	}

	for (uid_action_handler_t &handler : node->handlers) {
		for (uid_action_t &act : handler.actions) {
			const uid_source_location_t &actLoc = act.loc.path ? act.loc : node->source;
			if (!act.value.empty() && act.value.find('{') != std::string::npos) {
				if (!SubstituteContextRefs(ctx.doc,
						act.value, templateProps, parentProps, &exprLim, false, &resolved, &diag
					)) {
					ctx.Errorf(actLoc, diag);
					return;
				}
				act.value = resolved;
			}
			if (!act.target.empty() && act.target.find('{') != std::string::npos) {
				if (!SubstituteContextRefs(ctx.doc,
						act.target, templateProps, parentProps, &exprLim, false, &resolved, &diag
					)) {
					ctx.Errorf(actLoc, diag);
					return;
				}
				act.target = resolved;
			}
			if (!act.name.empty() && act.name.find('{') != std::string::npos) {
				if (!SubstituteContextRefs(ctx.doc,
						act.name, templateProps, parentProps, &exprLim, false, &resolved, &diag
					)) {
					ctx.Errorf(actLoc, diag);
					return;
				}
				act.name = resolved;
			}
			if (act.kind == UID_NODE_SET && localIds && !useId.empty() && !act.target.empty()) {
				if (localIds->count(act.target)) {
					act.target = useId + "." + act.target;
				}
			}
		}
	}

	if (!node->foreachTemplateNodes.empty() && node->foreachTemplateRoot >= 0) {
		ApplyContextToForeachTemplateTree(
			ctx,
			&node->foreachTemplateNodes,
			node->foreachTemplateRoot,
			templateProps,
			parentProps,
			localIds,
			useId
		);
	}
}

uid_node_id_t ExpandNode(
	ExpandContext &ctx,
	const std::vector<uid_node_def_t> &srcNodes,
	uid_node_id_t srcId,
	const uid_property_set_t &inheritedTextStyle,
	const uid_property_set_t *templateProps,
	const uid_property_set_t *parentProps,
	const std::set<std::string> *localIds,
	const std::string &useId,
	const std::string &idPrefix,
	int depth
);

uid_property_set_t BuildTemplatePropValues(
	ExpandContext &ctx,
	const uid_template_def_t &tmpl,
	const uid_node_def_t &useNode
)
{
	uid_property_set_t props;
	for (const uid_prop_decl_t &decl : tmpl.props) {
		std::string v;
		if (useNode.properties.Get(decl.name.c_str(), &v)) {
			props.Set(decl.name.c_str(), v);
		} else if (decl.required) {
			ctx.Errorf(useNode.source, "missing required template prop: " + decl.name);
		} else if (!decl.defaultValue.empty()) {
			props.Set(decl.name.c_str(), decl.defaultValue);
		}
	}
	/* bind on use maps to template.bind when declared */
	if (!useNode.bind.empty()) {
		std::string bind = useNode.bind;
		CanonicalizeBind(&bind);
		props.Set("bind", bind);
	} else {
		std::string v;
		if (useNode.properties.Get("bind", &v)) {
			CanonicalizeBind(&v);
			props.Set("bind", v);
		}
	}
	/*
	 * Fixed in OPM: literal commit= on <use> may live on hasCommit only; still
	 * feed template.commit so nested sliders honor submit vs change.
	 */
	{
		std::string v;
		if (!props.Get("commit", &v) || v.empty()) {
			if (useNode.hasCommit) {
				const char *mode = "change";
				if (useNode.commit == UID_COMMIT_SUBMIT) {
					mode = "submit";
				} else if (useNode.commit == UID_COMMIT_APPLY) {
					mode = "apply";
				}
				props.Set("commit", mode);
			}
		}
	}
	return props;
}

uid_property_set_t BuildUseLayoutOverlay(const uid_node_def_t &useNode)
{
	uid_property_set_t layout;
	static const char *const kLayout[] = {
		"width", "height", "padding", "margin", "fill", "color", "shape", "radius",
		"font", "font-size", "font-weight", "text-skew", "letter-spacing", "line-height",
		"text-wrap", "drop-shadow", "visible", "enabled",
		"type", "halign", "valign", "gap", "overflow",
		"bind", "commit", "value-type", "tab-index", "visible-if",
		"hoverfill", "pressed-fill", "focus-fill", "disabled-fill", "selected-fill",
		"hover-color", "pressed-color", "focus-color", "disabled-color"
	};
	for (const char *k : kLayout) {
		std::string v;
		if (useNode.properties.Get(k, &v)) {
			layout.Set(k, v);
		}
	}
	if (!useNode.bind.empty()) {
		layout.Set("bind", useNode.bind);
	}
	return layout;
}

uid_node_id_t ExpandUse(
	ExpandContext &ctx,
	const uid_node_def_t &useNode,
	const uid_property_set_t &inheritedTextStyle,
	const uid_property_set_t *outerParentProps,
	const std::string &idPrefix,
	int depth
)
{
	if (depth > ctx.doc->limits.maxTemplateDepth) {
		ctx.Error(useNode.source, "template expansion depth exceeds limit");
		return UID_INVALID_NODE_ID;
	}

	auto tit = ctx.doc->definitions.templates.find(useNode.templateId);
	if (tit == ctx.doc->definitions.templates.end()) {
		ctx.Errorf(useNode.source, "unknown template: " + useNode.templateId);
		return UID_INVALID_NODE_ID;
	}
	const uid_template_def_t &tmpl = tit->second;

	if (ctx.expandStack.count(tmpl.id)) {
		ctx.Errorf(useNode.source, "cyclic template expansion: " + tmpl.id);
		return UID_INVALID_NODE_ID;
	}

	std::string useId = useNode.id;
	if (!idPrefix.empty() && !useNode.id.empty()) {
		useId = idPrefix + "." + useNode.id;
	} else if (!idPrefix.empty() && useNode.id.empty()) {
		useId = idPrefix;
	}

	if (TemplateHasLocalIds(tmpl) && useId.empty()) {
		ctx.Error(useNode.source, "template with local ids requires <use id>");
		return UID_INVALID_NODE_ID;
	}

	uid_property_set_t templateProps = BuildTemplatePropValues(ctx, tmpl, useNode);
	if (ctx.failed) {
		return UID_INVALID_NODE_ID;
	}
	uid_property_set_t useLayout = BuildUseLayoutOverlay(useNode);

	std::set<std::string> localIds;
	CollectLocalIds(tmpl, &localIds);

	/* Parent context for {parent.*} inside the template is the use overlay. */
	uid_property_set_t parentProps = useLayout;
	if (outerParentProps) {
		for (const auto &kv : outerParentProps->Attrs()) {
			if (!parentProps.Has(kv.first.c_str())) {
				parentProps.Set(kv.first.c_str(), kv.second.value);
			}
		}
	}

	uid_property_set_t nextInherit = inheritedTextStyle;
	CopyInheritedTextStyle(useNode.properties, &nextInherit);

	ctx.expandStack.insert(tmpl.id);
	const uid_node_id_t root = ExpandNode(
		ctx,
		tmpl.nodes,
		tmpl.rootNode,
		nextInherit,
		&templateProps,
		&parentProps,
		&localIds,
		useId,
		useId,
		depth + 1
	);
	ctx.expandStack.erase(tmpl.id);

	if (root == UID_INVALID_NODE_ID || ctx.failed) {
		return UID_INVALID_NODE_ID;
	}

	uid_node_def_t &rootNode = ctx.outNodes[static_cast<size_t>(root)];
	uid_property_set_t merged;
	ResolveProperties(ctx.doc, inheritedTextStyle, &templateProps, rootNode.properties, &merged);
	for (const auto &kv : useLayout.Attrs()) {
		merged.Set(kv.first.c_str(), kv.second.value);
	}
	rootNode.properties = merged;

	if (!useNode.visibleIf.empty()) {
		rootNode.visibleIf = useNode.visibleIf;
	}
	if (!useNode.enabledIf.empty()) {
		rootNode.enabledIf = useNode.enabledIf;
	}
	/* Added in OPM: forward brace expr attrs from <use> onto expanded root. */
	if (!useNode.visibleExpr.empty()) {
		if (!rootNode.visibleExpr.empty()) {
			/*
			 * Changed in OPM: parenthesize both sides so use-site gates stay
			 * effective when the template root uses `or` (e.g. search filter).
			 * Without parens, `gate and search == '' or icontains(...)` ignores
			 * the gate when icontains matches the empty needle.
			 */
			rootNode.visibleExpr =
				"(" + useNode.visibleExpr + ") and (" + rootNode.visibleExpr + ")";
		} else {
			rootNode.visibleExpr = useNode.visibleExpr;
		}
		rootNode.visibleExprBound = useNode.visibleExprBound;
	}
	if (!useNode.enabledExpr.empty()) {
		if (!rootNode.enabledExpr.empty()) {
			/* Changed in OPM: parenthesize when combining enabled exprs (same as visible). */
			rootNode.enabledExpr =
				"(" + useNode.enabledExpr + ") and (" + rootNode.enabledExpr + ")";
		} else {
			rootNode.enabledExpr = useNode.enabledExpr;
		}
		rootNode.enabledExprBound = useNode.enabledExprBound;
	}
	/* Added in OPM: forward style ternaries from <use> onto expanded root. */
	for (const auto &kv : useNode.styleExprs) {
		rootNode.styleExprs[kv.first] = kv.second;
	}

	if (rootNode.kind == UID_NODE_SLIDER && !UID_SyncSliderBounds(&rootNode)) {
		ctx.Error(useNode.source, "<slider> requires valid min, max, and step after template expand");
		return UID_INVALID_NODE_ID;
	}

	if (!useId.empty()) {
		if (rootNode.id.empty()) {
			rootNode.id = useId;
		}
		if (rootNode.id != useId) {
			if (!ctx.RegisterId(useId, root, useNode.source)) {
				return UID_INVALID_NODE_ID;
			}
		} else if (!ctx.idIndex.count(useId)) {
			if (!ctx.RegisterId(useId, root, useNode.source)) {
				return UID_INVALID_NODE_ID;
			}
		}
	}

	return root;
}

uid_node_id_t ExpandNode(
	ExpandContext &ctx,
	const std::vector<uid_node_def_t> &srcNodes,
	uid_node_id_t srcId,
	const uid_property_set_t &inheritedTextStyle,
	const uid_property_set_t *templateProps,
	const uid_property_set_t *parentProps,
	const std::set<std::string> *localIds,
	const std::string &useId,
	const std::string &idPrefix,
	int depth
)
{
	if (srcId < 0 || static_cast<size_t>(srcId) >= srcNodes.size()) {
		ctx.Error(uid_source_location_t{ctx.doc->sourceName.c_str(), 0, 0}, "invalid source node id");
		return UID_INVALID_NODE_ID;
	}

	const uid_node_def_t &src = srcNodes[static_cast<size_t>(srcId)];

	if (src.kind == UID_NODE_USE) {
		if (src.deferredUse) {
			if (!ctx.CheckExpandedBudget(src.source)) {
				return UID_INVALID_NODE_ID;
			}
			uid_node_def_t dst = src;
			dst.children.clear();
			const uid_node_id_t newId = static_cast<uid_node_id_t>(ctx.outNodes.size());
			ctx.outNodes.push_back(dst);
			return newId;
		}
		uid_node_def_t useNode = src;
		if (templateProps || parentProps) {
			ApplyContextToNode(ctx, &useNode, templateProps, parentProps, nullptr, std::string());
			if (ctx.failed) {
				return UID_INVALID_NODE_ID;
			}
		}
		return ExpandUse(ctx, useNode, inheritedTextStyle, parentProps, idPrefix, depth);
	}

	if (!ctx.CheckExpandedBudget(src.source)) {
		return UID_INVALID_NODE_ID;
	}

	uid_node_def_t dst = src;
	dst.children.clear();

	std::string expandedId = src.id;
	if (!src.id.empty() && !idPrefix.empty()) {
		expandedId = idPrefix + "." + src.id;
	} else if (!src.id.empty()) {
		expandedId = src.id;
	}
	dst.id = expandedId;

	ResolveProperties(ctx.doc, inheritedTextStyle, nullptr, src.properties, &dst.properties);

	if (!src.bind.empty()) {
		dst.bind = src.bind;
		dst.properties.Set("bind", src.bind);
	} else {
		std::string b;
		if (dst.properties.Get("bind", &b)) {
			dst.bind = b;
		}
	}

	if (templateProps || parentProps) {
		ApplyContextToNode(ctx, &dst, templateProps, parentProps, localIds, useId);
		if (ctx.failed) {
			return UID_INVALID_NODE_ID;
		}
	}

	if (!dst.bind.empty()) {
		CanonicalizeBind(&dst.bind);
		dst.properties.Set("bind", dst.bind);
	}

	/* Added in OPM: resolve deferred commit="{template.*}" after expand. */
	{
		std::string commitStr;
		if (dst.properties.Get("commit", &commitStr) && !commitStr.empty()) {
			uid_commit_mode_t mode = UID_COMMIT_CHANGE;
			if (commitStr == "change") {
				mode = UID_COMMIT_CHANGE;
			} else if (commitStr == "submit") {
				mode = UID_COMMIT_SUBMIT;
			} else if (commitStr == "apply") {
				mode = UID_COMMIT_APPLY;
			} else if (commitStr.find('{') != std::string::npos) {
				/* Optional template prop omitted — leave default change. */
				mode = UID_COMMIT_CHANGE;
			} else {
				ctx.Error(dst.source, "invalid commit mode after template expand");
				return UID_INVALID_NODE_ID;
			}
			dst.commit = mode;
			dst.hasCommit = true;
			dst.properties.Set("commit", commitStr == "change" || commitStr == "submit" || commitStr == "apply"
				? commitStr
				: "change");
		}
	}

	/* Added in OPM: resolve deferred slider/input min/max/step after {template.*} expand. */
	if (dst.kind == UID_NODE_SLIDER && !UID_SyncSliderBounds(&dst)) {
		ctx.Error(dst.source, "<slider> requires valid min, max, and step after template expand");
		return UID_INVALID_NODE_ID;
	}
	if (dst.kind == UID_NODE_INPUT && !UID_SyncInputBounds(&dst)) {
		ctx.Error(dst.source, "invalid input min/max/step after template expand");
		return UID_INVALID_NODE_ID;
	}
	if (dst.kind == UID_NODE_SELECT) {
		if (!dst.optionSource.empty() && !dst.options.empty()) {
			ctx.Error(dst.source, "<select> cannot combine source with static options after template expand");
			return UID_INVALID_NODE_ID;
		}
		if (dst.optionSource.empty() && dst.options.empty()) {
			ctx.Error(dst.source, "<select> requires source or options after template expand");
			return UID_INVALID_NODE_ID;
		}
	}

	const uid_node_id_t newId = static_cast<uid_node_id_t>(ctx.outNodes.size());
	ctx.outNodes.push_back(dst);

	if (!ctx.RegisterId(expandedId, newId, src.source)) {
		return UID_INVALID_NODE_ID;
	}

	uid_property_set_t childInherit = inheritedTextStyle;
	CopyInheritedTextStyle(ctx.outNodes[static_cast<size_t>(newId)].properties, &childInherit);

	uid_node_def_t *stored = &ctx.outNodes[static_cast<size_t>(newId)];
	for (uid_node_id_t childSrc : src.children) {
		const uid_node_id_t childDst = ExpandNode(
			ctx,
			srcNodes,
			childSrc,
			childInherit,
			templateProps,
			parentProps,
			localIds,
			useId,
			idPrefix,
			depth
		);
		if (childDst == UID_INVALID_NODE_ID || ctx.failed) {
			return UID_INVALID_NODE_ID;
		}
		stored = &ctx.outNodes[static_cast<size_t>(newId)];
		stored->children.push_back(childDst);
	}

	return newId;
}

} // namespace

static void RemapSubtreeNodeIds(std::vector<uid_node_def_t> *nodes, size_t baseOffset)
{
	if (!nodes || baseOffset == 0) {
		return;
	}
	auto remapId = [baseOffset](uid_node_id_t id) -> uid_node_id_t {
		if (id == UID_INVALID_NODE_ID) {
			return UID_INVALID_NODE_ID;
		}
		return static_cast<uid_node_id_t>(baseOffset + static_cast<size_t>(id));
	};
	for (uid_node_def_t &node : *nodes) {
		for (uid_node_id_t &child : node.children) {
			child = remapId(child);
		}
		if (node.foreachTemplateRoot != UID_INVALID_NODE_ID) {
			node.foreachTemplateRoot = remapId(node.foreachTemplateRoot);
		}
		if (node.foreachScopeId != UID_INVALID_NODE_ID) {
			node.foreachScopeId = remapId(node.foreachScopeId);
		}
	}
}

uid_node_id_t UID_CloneTemplateRoot(
	uid_document_t *doc,
	const char *templateId,
	const uid_node_def_t &propSource,
	uid_diag_list_t *diags
)
{
	if (!doc || !templateId || !templateId[0]) {
		return UID_INVALID_NODE_ID;
	}

	ExpandContext ctx;
	ctx.doc = doc;
	ctx.diags = diags;
	ctx.failed = false;

	uid_node_def_t useNode;
	if (propSource.kind == UID_NODE_USE) {
		useNode = propSource;
		useNode.templateId = templateId;
		useNode.children.clear();
		useNode.foreachTemplateNodes.clear();
		useNode.deferredUse = false;
		useNode.deferredUseExpanded = false;
	} else {
		UID_InitNodeDef(&useNode);
		useNode.source = propSource.source;
		useNode.templateId = templateId;
		/*
		 * Do not copy the scroll container's layout attrs (width/height/fill/…) onto
		 * the generated chrome — BuildUseLayoutOverlay would override the template's
		 * width="8px" with width="100%" and the rail becomes ~container height wide.
		 */
		if (!propSource.id.empty()) {
			useNode.id = propSource.id + ".__scrollbar";
		} else {
			useNode.id = "__scrollbar";
		}
	}

	uid_property_set_t inherit;
	CopyInheritedTextStyle(doc->definitions.defaults, &inherit);

	const uid_node_id_t root = ExpandUse(ctx, useNode, inherit, nullptr, std::string(), 0);
	if (ctx.failed || root == UID_INVALID_NODE_ID) {
		return UID_INVALID_NODE_ID;
	}

	const size_t base = doc->nodes.size();
	RemapSubtreeNodeIds(&ctx.outNodes, base);

	for (uid_node_def_t &node : ctx.outNodes) {
		doc->nodes.push_back(std::move(node));
		doc->states.emplace_back();
		UID_InitNodeState(&doc->states.back());
	}

	for (const auto &kv : ctx.idIndex) {
		const uid_node_id_t newId =
			static_cast<uid_node_id_t>(base + static_cast<size_t>(kv.second));
		doc->idIndex.emplace(kv.first, newId);
	}

	return static_cast<uid_node_id_t>(base + static_cast<size_t>(root));
}

uid_node_id_t FindParentOfChild(const uid_document_t *doc, uid_node_id_t childId)
{
	if (!doc || childId < 0) {
		return UID_INVALID_NODE_ID;
	}
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		for (uid_node_id_t c : doc->nodes[i].children) {
			if (c == childId) {
				return static_cast<uid_node_id_t>(i);
			}
		}
	}
	return UID_INVALID_NODE_ID;
}

void UID_ExpandDeferredUses(uid_document_t *doc, uid_diag_list_t *diags)
{
	if (!doc) {
		return;
	}

	std::vector<uid_node_id_t> pending;
	pending.reserve(doc->nodes.size());
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		const uid_node_def_t &node = doc->nodes[i];
		if (node.kind != UID_NODE_USE || !node.deferredUse || node.deferredUseExpanded) {
			continue;
		}
		if (node.templateId.empty() || node.templateId.find('{') != std::string::npos) {
			continue;
		}
		if (doc->definitions.templates.find(node.templateId) == doc->definitions.templates.end()) {
			continue;
		}
		pending.push_back(static_cast<uid_node_id_t>(i));
	}

	for (uid_node_id_t useId : pending) {
		if (useId < 0 || static_cast<size_t>(useId) >= doc->nodes.size()) {
			continue;
		}
		uid_node_def_t &node = doc->nodes[static_cast<size_t>(useId)];
		if (node.kind != UID_NODE_USE || !node.deferredUse || node.deferredUseExpanded) {
			continue;
		}

		const uid_node_id_t parentId = FindParentOfChild(doc, useId);
		if (parentId < 0) {
			continue;
		}

		const uid_node_def_t useCopy = doc->nodes[static_cast<size_t>(useId)];
		const std::string templateId = useCopy.templateId;
		const uid_node_id_t expanded = UID_CloneTemplateRoot(doc, templateId.c_str(), useCopy, diags);
		if (expanded < 0) {
			continue;
		}

		uid_node_def_t &parent = doc->nodes[static_cast<size_t>(parentId)];
		for (uid_node_id_t &childId : parent.children) {
			if (childId == useId) {
				childId = expanded;
				break;
			}
		}
		uid_node_def_t &useNode = doc->nodes[static_cast<size_t>(useId)];
		useNode.deferredUseExpanded = true;
		useNode.properties.Set("visible", "false");
		doc->dirty = static_cast<uid_dirty_flags_t>(
			doc->dirty | UID_DIRTY_STRUCTURE | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT
		);
	}
}

uid_result_t UID_ExpandDocument(uid_document_t *doc, uid_diag_list_t *diags)
{
	if (!doc) {
		return UID_ERR_INVALID_ARG;
	}

	if (doc->expanded) {
		return UID_OK;
	}

	const uid_result_t varResult = UID_ResolveDocumentVars(doc, diags);
	if (varResult != UID_OK) {
		return varResult;
	}

	ExpandContext ctx;
	ctx.doc = doc;
	ctx.diags = diags;
	ctx.failed = false;

	uid_property_set_t inherit;
	CopyInheritedTextStyle(doc->definitions.defaults, &inherit);

	uid_node_id_t newRoot = UID_INVALID_NODE_ID;
	if (doc->rootNode != UID_INVALID_NODE_ID) {
		newRoot = ExpandNode(
			ctx,
			doc->nodes,
			doc->rootNode,
			inherit,
			nullptr,
			nullptr,
			nullptr,
			std::string(),
			std::string(),
			0
		);
		if (ctx.failed || newRoot == UID_INVALID_NODE_ID) {
			return UID_ERR_VALIDATE;
		}
	}

	doc->nodes = std::move(ctx.outNodes);
	doc->rootNode = newRoot;
	doc->idIndex = std::move(ctx.idIndex);
	doc->states.clear();
	doc->states.resize(doc->nodes.size());
	for (uid_node_state_t &st : doc->states) {
		UID_InitNodeState(&st);
	}
	for (uid_node_def_t &node : doc->nodes) {
		UID_ApplyCollectionAndIndexFields(&node);
	}
	doc->expanded = true;
	doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty | UID_DIRTY_STRUCTURE | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT);
	return UID_OK;
}
