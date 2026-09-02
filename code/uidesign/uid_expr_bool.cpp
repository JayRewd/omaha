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

#include "uid_expr_bool.h"

#include "uid_binding.h"
#include "uid_collection.h"
#include "uid_expr.h"
#include "uid_opt.h"
#include "uid_vars.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

enum uid_bool_val_kind_t {
	UID_BOOL_VAL_BOOL = 0,
	UID_BOOL_VAL_STRING,
	UID_BOOL_VAL_NUMBER
};

struct uid_bool_value_t {
	uid_bool_val_kind_t kind;
	bool                b;
	std::string         s;
	double              n;
};

struct uid_bool_parser_t {
	const char *src;
	size_t      len;
	size_t      pos;
	std::string *diag;
	bool        failed;
};

void BoolFail(uid_bool_parser_t *p, const char *msg)
{
	if (!p->failed) {
		p->failed = true;
		if (p->diag) {
			*p->diag = msg ? msg : "bool expression error";
		}
	}
}

void BoolSkipWs(uid_bool_parser_t *p)
{
	while (p->pos < p->len && std::isspace(static_cast<unsigned char>(p->src[p->pos]))) {
		++p->pos;
	}
}

bool BoolMatch(uid_bool_parser_t *p, const char *tok)
{
	BoolSkipWs(p);
	const size_t n = std::strlen(tok);
	if (p->pos + n > p->len) {
		return false;
	}
	if (std::strncmp(p->src + p->pos, tok, n) != 0) {
		return false;
	}
	p->pos += n;
	return true;
}

/* Match a keyword only when not a prefix of a longer ident (A-Za-z0-9_). */
bool BoolMatchWord(uid_bool_parser_t *p, const char *tok)
{
	BoolSkipWs(p);
	const size_t n = std::strlen(tok);
	if (p->pos + n > p->len) {
		return false;
	}
	if (std::strncmp(p->src + p->pos, tok, n) != 0) {
		return false;
	}
	if (p->pos + n < p->len) {
		const unsigned char next = static_cast<unsigned char>(p->src[p->pos + n]);
		if (std::isalnum(next) || next == '_') {
			return false;
		}
	}
	p->pos += n;
	return true;
}

bool BoolReadIdent(uid_bool_parser_t *p, std::string *out)
{
	BoolSkipWs(p);
	if (p->pos >= p->len) {
		return false;
	}
	const size_t start = p->pos;
	unsigned char c = static_cast<unsigned char>(p->src[p->pos]);
	if (!(std::isalpha(c) || c == '_')) {
		return false;
	}
	++p->pos;
	while (p->pos < p->len) {
		c = static_cast<unsigned char>(p->src[p->pos]);
		if (std::isalnum(c) || c == '_' || c == '.' || c == '-') {
			++p->pos;
		} else {
			break;
		}
	}
	*out = std::string(p->src + start, p->pos - start);
	if (out->empty() || out->back() == '.' || out->back() == '-' || out->find("..") != std::string::npos) {
		out->clear();
		return false;
	}
	return true;
}

bool BoolReadQuotedString(uid_bool_parser_t *p, std::string *out)
{
	BoolSkipWs(p);
	if (p->pos >= p->len) {
		return false;
	}
	const char quote = p->src[p->pos];
	if (quote != '"' && quote != '\'') {
		return false;
	}
	++p->pos;
	std::string s;
	while (p->pos < p->len) {
		const char c = p->src[p->pos++];
		if (c == quote) {
			*out = s;
			return true;
		}
		if (c == '\\' && p->pos < p->len) {
			s.push_back(p->src[p->pos++]);
		} else {
			s.push_back(c);
		}
	}
	BoolFail(p, "unterminated string literal");
	return false;
}

bool BoolReadBareWord(uid_bool_parser_t *p, std::string *out)
{
	BoolSkipWs(p);
	if (p->pos >= p->len) {
		return false;
	}
	const size_t start = p->pos;
	unsigned char c = static_cast<unsigned char>(p->src[p->pos]);
	if (!(std::isalnum(c) || c == '_' || c == '-')) {
		return false;
	}
	++p->pos;
	while (p->pos < p->len) {
		c = static_cast<unsigned char>(p->src[p->pos]);
		if (std::isalnum(c) || c == '_' || c == '-') {
			++p->pos;
		} else {
			break;
		}
	}
	*out = std::string(p->src + start, p->pos - start);
	return !out->empty();
}

bool BoolTryNumber(const std::string &s, double *out)
{
	if (s.empty()) {
		return false;
	}
	char *end = nullptr;
	const double v = std::strtod(s.c_str(), &end);
	if (end == s.c_str() || (end && *end != '\0')) {
		return false;
	}
	if (!std::isfinite(v)) {
		return false;
	}
	*out = v;
	return true;
}

bool BoolLookupPath(const uid_bool_lookup_ctx_t *ctx, const std::string &path, uid_bool_value_t *out)
{
	if (!ctx || !out || path.empty()) {
		return false;
	}

	if (path == "true") {
		out->kind = UID_BOOL_VAL_BOOL;
		out->b = true;
		return true;
	}
	if (path == "false") {
		out->kind = UID_BOOL_VAL_BOOL;
		out->b = false;
		return true;
	}

	if (path.rfind("cvar.", 0) == 0) {
		const std::string name = path.substr(5);
		std::string val;
		if (!ctx->backend || !UID_ReadCvarString(ctx->backend, name.c_str(), &val)) {
			val.clear();
		}
		out->kind = UID_BOOL_VAL_STRING;
		out->s = val;
		return true;
	}

	/* Added in OPM: item.count / item.last work even without a concrete item pointer. */
	if (path == "item.count") {
		out->kind = UID_BOOL_VAL_NUMBER;
		out->n = static_cast<double>(ctx->itemCount);
		return true;
	}
	if (path == "item.last") {
		out->kind = UID_BOOL_VAL_BOOL;
		out->b = (ctx->itemIndex >= 0 && ctx->itemCount > 0 &&
		          ctx->itemIndex == ctx->itemCount - 1);
		return true;
	}
	if (path == "item.index" && ctx->itemIndex >= 0) {
		out->kind = UID_BOOL_VAL_NUMBER;
		out->n = static_cast<double>(ctx->itemIndex);
		return true;
	}
	if (path == "item.selected" && ctx->itemIndex >= 0) {
		out->kind = UID_BOOL_VAL_BOOL;
		out->b = (ctx->itemIndex == ctx->selectedIndex);
		return true;
	}

	if (ctx->item) {
		if (path == "item.value") {
			out->kind = UID_BOOL_VAL_STRING;
			out->s = ctx->item->value;
			return true;
		}
		if (path == "item.label") {
			out->kind = UID_BOOL_VAL_STRING;
			out->s = ctx->item->label;
			return true;
		}
		if (path.rfind("item.field.", 0) == 0) {
			const std::string fname = path.substr(11);
			auto it = ctx->item->fields.find(fname);
			out->kind = UID_BOOL_VAL_STRING;
			out->s = (it != ctx->item->fields.end()) ? it->second : std::string();
			return true;
		}
	}

	if (path == "item.lifetime_alpha" && ctx->doc && ctx->nodeId >= 0) {
		out->kind = UID_BOOL_VAL_NUMBER;
		out->n = static_cast<double>(UID_EvalItemLifetimeAlpha(ctx->doc, ctx->nodeId));
		return true;
	}

	if (ctx->doc && ctx->nodeId >= 0 && static_cast<size_t>(ctx->nodeId) < ctx->doc->nodes.size()) {
		const uid_node_def_t &node = ctx->doc->nodes[static_cast<size_t>(ctx->nodeId)];
		/* Added in OPM: bind.selected — node's bind cvar matches set-value. */
		if (path == "bind.selected") {
			out->kind = UID_BOOL_VAL_BOOL;
			out->b = false;
			if (!node.bind.empty() && !node.setValue.empty() && ctx->backend) {
				std::string cvarName;
				const char *p = node.bind.c_str();
				if (std::strncmp(p, "cvar:", 5) == 0) {
					cvarName = p + 5;
				} else {
					cvarName = node.bind;
				}
				std::string have;
				if (UID_ReadCvarString(ctx->backend, cvarName.c_str(), &have)) {
					/*
					 * Fixed in Omaha: compare UI-space value after value-type
					 * transforms (e.g. invert-mouse maps ±m_pitch → 0/1).
					 */
					have = UID_TransformCvarToUi(node, have, ctx->backend);
					if (have == node.setValue) {
						out->b = true;
					} else {
						/*
						 * Fixed in Omaha: accept numeric equals ("1" vs "1.0") so
						 * Off/On highlights still match float-formatted cvars.
						 */
						char *endHave = nullptr;
						char *endWant = nullptr;
						const double a = std::strtod(have.c_str(), &endHave);
						const double b = std::strtod(node.setValue.c_str(), &endWant);
						if (endHave && endHave != have.c_str() && *endHave == '\0' && endWant &&
							endWant != node.setValue.c_str() && *endWant == '\0') {
							out->b = (std::fabs(a - b) < 1e-6);
						}
					}
				}
			}
			return true;
		}
		if (path == "bind.value") {
			out->kind = UID_BOOL_VAL_STRING;
			out->s.clear();
			if (!node.bind.empty() && ctx->backend) {
				std::string cvarName;
				const char *p = node.bind.c_str();
				if (std::strncmp(p, "cvar:", 5) == 0) {
					cvarName = p + 5;
				} else {
					cvarName = node.bind;
				}
				(void)UID_ReadCvarString(ctx->backend, cvarName.c_str(), &out->s);
			}
			return true;
		}
		/* Fill foreach item context once when not already populated. */
		if (ctx->itemIndex < 0 && node.foreachGenerated && node.foreachScopeId >= 0 &&
		    static_cast<size_t>(node.foreachScopeId) < ctx->doc->states.size()) {
			const uid_node_state_t &scopeSt = ctx->doc->states[static_cast<size_t>(node.foreachScopeId)];
			const int idx = node.foreachItemIndex;
			uid_bool_lookup_ctx_t itemCtx = *ctx;
			itemCtx.itemIndex = idx;
			itemCtx.itemCount = scopeSt.collectionItemCount;
			itemCtx.selectedIndex = scopeSt.collectionSelectedIndex;
			if (idx >= 0 && static_cast<size_t>(idx) < scopeSt.collectionItems.size()) {
				itemCtx.item = &scopeSt.collectionItems[static_cast<size_t>(idx)];
			}
			return BoolLookupPath(&itemCtx, path, out);
		}
	}

	return false;
}

void BoolCoerceString(const uid_bool_value_t &v, std::string *out)
{
	switch (v.kind) {
	case UID_BOOL_VAL_BOOL:
		*out = v.b ? "true" : "false";
		break;
	case UID_BOOL_VAL_NUMBER: {
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.6g", v.n);
		*out = buf;
		break;
	}
	default:
		*out = v.s;
		break;
	}
}

bool BoolCoerceNumber(const uid_bool_value_t &v, double *out)
{
	switch (v.kind) {
	case UID_BOOL_VAL_BOOL:
		*out = v.b ? 1.0 : 0.0;
		return true;
	case UID_BOOL_VAL_NUMBER:
		*out = v.n;
		return true;
	default:
		return BoolTryNumber(v.s, out);
	}
}

bool BoolValuesEqual(const uid_bool_value_t &a, const uid_bool_value_t &b)
{
	std::string sa;
	std::string sb;
	BoolCoerceString(a, &sa);
	BoolCoerceString(b, &sb);
	return sa == sb;
}

static bool BoolIContains(const std::string &haystack, const std::string &needle)
{
	if (needle.empty()) {
		return true;
	}
	if (haystack.empty()) {
		return false;
	}
	for (size_t i = 0; i < haystack.size(); ++i) {
		size_t hi = i;
		size_t ni = 0;
		while (hi < haystack.size() && ni < needle.size() &&
		       std::tolower(static_cast<unsigned char>(haystack[hi])) ==
			       std::tolower(static_cast<unsigned char>(needle[ni]))) {
			++hi;
			++ni;
		}
		if (ni == needle.size()) {
			return true;
		}
	}
	return false;
}

static bool BoolParseStringValue(uid_bool_parser_t *p, const uid_bool_lookup_ctx_t *ctx, uid_bool_value_t *out)
{
	if (!p || !out) {
		return false;
	}
	BoolSkipWs(p);
	std::string tok;
	if (p->pos < p->len && (p->src[p->pos] == '"' || p->src[p->pos] == '\'')) {
		if (!BoolReadQuotedString(p, &tok)) {
			return false;
		}
		out->kind = UID_BOOL_VAL_STRING;
		out->s = tok;
		return true;
	}
	if (!BoolReadIdent(p, &tok)) {
		if (!BoolReadBareWord(p, &tok)) {
			BoolFail(p, "expected string value in bool expression");
			return false;
		}
		out->kind = UID_BOOL_VAL_STRING;
		out->s = tok;
		return true;
	}
	if (!BoolLookupPath(ctx, tok, out)) {
		out->kind = UID_BOOL_VAL_STRING;
		out->s = tok;
	}
	return true;
}

bool BoolEvalCompare(
	const uid_bool_value_t &left,
	const uid_bool_value_t &right,
	const char *op,
	bool *out
)
{
	if (!std::strcmp(op, "==")) {
		*out = BoolValuesEqual(left, right);
		return true;
	}
	if (!std::strcmp(op, "!=")) {
		*out = !BoolValuesEqual(left, right);
		return true;
	}

	double ln = 0.0;
	double rn = 0.0;
	if (!BoolCoerceNumber(left, &ln) || !BoolCoerceNumber(right, &rn)) {
		return false;
	}
	if (!std::strcmp(op, "<")) {
		*out = ln < rn;
	} else if (!std::strcmp(op, "<=")) {
		*out = ln <= rn;
	} else if (!std::strcmp(op, ">")) {
		*out = ln > rn;
	} else if (!std::strcmp(op, ">=")) {
		*out = ln >= rn;
	} else {
		return false;
	}
	return true;
}

bool BoolParsePrimary(uid_bool_parser_t *p, const uid_bool_lookup_ctx_t *ctx, bool *out);
bool BoolParseCompare(uid_bool_parser_t *p, const uid_bool_lookup_ctx_t *ctx, bool *out);
bool BoolParseNot(uid_bool_parser_t *p, const uid_bool_lookup_ctx_t *ctx, bool *out);
bool BoolParseAnd(uid_bool_parser_t *p, const uid_bool_lookup_ctx_t *ctx, bool *out);
bool BoolParseOr(uid_bool_parser_t *p, const uid_bool_lookup_ctx_t *ctx, bool *out);

/* Added in OPM: numeric lookup for arithmetic in bool compares (cvar/item paths only when numeric). */
static bool BoolNumericLookup(void *userdata, const char *path, double *out)
{
	if (!userdata || !path || !out) {
		return false;
	}
	const uid_bool_lookup_ctx_t *ctx = static_cast<const uid_bool_lookup_ctx_t *>(userdata);
	uid_bool_value_t val{};
	if (!BoolLookupPath(ctx, path, &val)) {
		return false;
	}
	return BoolCoerceNumber(val, out);
}

static bool BoolWordAt(const char *src, size_t i, size_t len, const char *word)
{
	const size_t n = std::strlen(word);
	if (i + n > len || std::strncmp(src + i, word, n) != 0) {
		return false;
	}
	if (i > 0) {
		const unsigned char prev = static_cast<unsigned char>(src[i - 1]);
		if (std::isalnum(prev) || prev == '_' || prev == '.') {
			return false;
		}
	}
	if (i + n < len) {
		const unsigned char next = static_cast<unsigned char>(src[i + n]);
		if (std::isalnum(next) || next == '_') {
			return false;
		}
	}
	return true;
}

/*
 * Fixed in OPM: compare operands may be numeric expressions (+ - * / % and paths),
 * so visible="{cvar.health > item.index * 10}" works. Non-numeric string compares
 * still use the legacy atomic-token path.
 */
static bool BoolParseNumericOperand(
	uid_bool_parser_t             *p,
	const uid_bool_lookup_ctx_t   *ctx,
	const uid_expr_limits_t       *limits,
	double                        *out
)
{
	if (!p || !ctx || !out) {
		return false;
	}
	BoolSkipWs(p);
	const size_t start = p->pos;
	if (start >= p->len) {
		return false;
	}

	int depth = 0;
	size_t i = start;
	while (i < p->len) {
		const char c = p->src[i];
		if (c == '"' || c == '\'') {
			const char quote = c;
			++i;
			while (i < p->len) {
				if (p->src[i] == '\\' && i + 1 < p->len) {
					i += 2;
					continue;
				}
				if (p->src[i] == quote) {
					++i;
					break;
				}
				++i;
			}
			continue;
		}
		if (c == '(') {
			++depth;
			++i;
			continue;
		}
		if (c == ')') {
			if (depth == 0) {
				break;
			}
			--depth;
			++i;
			continue;
		}
		if (depth == 0) {
			if (BoolWordAt(p->src, i, p->len, "and") || BoolWordAt(p->src, i, p->len, "or")) {
				break;
			}
			if (i + 1 < p->len) {
				const char n1 = p->src[i + 1];
				if ((c == '=' && n1 == '=') || (c == '!' && n1 == '=') ||
				    (c == '<' && n1 == '=') || (c == '>' && n1 == '=')) {
					break;
				}
			}
			if (c == '<' || c == '>') {
				break;
			}
		}
		++i;
	}

	while (i > start && std::isspace(static_cast<unsigned char>(p->src[i - 1]))) {
		--i;
	}
	if (i <= start) {
		return false;
	}

	const std::string piece(p->src + start, i - start);
	uid_expr_limits_t localLimits;
	if (!limits) {
		UID_DefaultExprLimits(&localLimits);
		limits = &localLimits;
	}
	std::string diag;
	if (!UID_EvalNumber(
			piece.c_str(),
			BoolNumericLookup,
			const_cast<uid_bool_lookup_ctx_t *>(ctx),
			limits,
			out,
			&diag
		)) {
		return false;
	}
	p->pos = i;
	return true;
}

static bool BoolParseCompareOp(uid_bool_parser_t *p, const char **opOut)
{
	BoolSkipWs(p);
	static const char *const ops[] = {"==", "!=", "<=", ">=", "<", ">", nullptr};
	for (int i = 0; ops[i]; ++i) {
		if (BoolMatch(p, ops[i])) {
			*opOut = ops[i];
			return true;
		}
	}
	return false;
}

bool BoolParseCompare(uid_bool_parser_t *p, const uid_bool_lookup_ctx_t *ctx, bool *out)
{
	BoolSkipWs(p);
	const size_t save = p->pos;

	/* Prefer arithmetic-capable numeric operands when both sides evaluate as numbers. */
	{
		double leftN = 0.0;
		double rightN = 0.0;
		const char *op = nullptr;
		if (BoolParseNumericOperand(p, ctx, nullptr, &leftN) && BoolParseCompareOp(p, &op) &&
		    BoolParseNumericOperand(p, ctx, nullptr, &rightN)) {
			uid_bool_value_t left{};
			uid_bool_value_t right{};
			left.kind = UID_BOOL_VAL_NUMBER;
			left.n = leftN;
			right.kind = UID_BOOL_VAL_NUMBER;
			right.n = rightN;
			if (BoolEvalCompare(left, right, op, out)) {
				return true;
			}
			BoolFail(p, "invalid numeric comparison");
			return false;
		}
		p->pos = save;
		p->failed = false;
		if (p->diag) {
			p->diag->clear();
		}
	}

	std::string leftTok;
	uid_bool_value_t left;
	left.kind = UID_BOOL_VAL_STRING;

	if (p->pos < p->len && (p->src[p->pos] == '"' || p->src[p->pos] == '\'')) {
		if (!BoolReadQuotedString(p, &leftTok)) {
			return false;
		}
		left.s = leftTok;
	} else if (!BoolReadIdent(p, &leftTok)) {
		if (!BoolReadBareWord(p, &leftTok)) {
			return BoolParsePrimary(p, ctx, out);
		}
		left.s = leftTok;
	} else if (!BoolLookupPath(ctx, leftTok, &left)) {
		left.kind = UID_BOOL_VAL_STRING;
		left.s = leftTok;
	}

	const char *op = nullptr;
	if (!BoolParseCompareOp(p, &op)) {
		p->pos = save;
		return BoolParsePrimary(p, ctx, out);
	}

	BoolSkipWs(p);
	uid_bool_value_t right;
	right.kind = UID_BOOL_VAL_STRING;
	std::string rightTok;
	if (p->pos < p->len && (p->src[p->pos] == '"' || p->src[p->pos] == '\'')) {
		if (!BoolReadQuotedString(p, &rightTok)) {
			return false;
		}
		right.s = rightTok;
	} else if (!BoolReadIdent(p, &rightTok)) {
		if (!BoolReadBareWord(p, &rightTok)) {
			BoolFail(p, "expected comparison operand");
			return false;
		}
		right.s = rightTok;
	} else if (!BoolLookupPath(ctx, rightTok, &right)) {
		right.kind = UID_BOOL_VAL_STRING;
		right.s = rightTok;
	}

	if (!BoolEvalCompare(left, right, op, out)) {
		BoolFail(p, "invalid comparison operands");
		return false;
	}
	return true;
}

bool BoolParsePrimary(uid_bool_parser_t *p, const uid_bool_lookup_ctx_t *ctx, bool *out)
{
	BoolSkipWs(p);
	if (p->pos >= p->len) {
		BoolFail(p, "unexpected end of bool expression");
		return false;
	}

	if (BoolMatch(p, "(")) {
		if (!BoolParseOr(p, ctx, out)) {
			return false;
		}
		BoolSkipWs(p);
		if (!BoolMatch(p, ")")) {
			BoolFail(p, "missing closing parenthesis");
			return false;
		}
		return true;
	}

	if (BoolMatchWord(p, "true")) {
		*out = true;
		return true;
	}
	if (BoolMatchWord(p, "false")) {
		*out = false;
		return true;
	}

	std::string tok;
	if (p->src[p->pos] == '"' || p->src[p->pos] == '\'') {
		if (!BoolReadQuotedString(p, &tok)) {
			return false;
		}
		uid_bool_value_t lv;
		lv.kind = UID_BOOL_VAL_STRING;
		lv.s = tok;
		uid_bool_value_t rv;
		rv.kind = UID_BOOL_VAL_STRING;
		rv.s = tok;
		return BoolEvalCompare(lv, rv, "==", out);
	}

	if (!BoolReadIdent(p, &tok)) {
		if (!BoolReadBareWord(p, &tok)) {
			BoolFail(p, "expected value in bool expression");
			return false;
		}
		uid_bool_value_t lv;
		lv.kind = UID_BOOL_VAL_STRING;
		lv.s = tok;
		uid_bool_value_t rv;
		rv.kind = UID_BOOL_VAL_STRING;
		rv.s = tok;
		return BoolEvalCompare(lv, rv, "==", out);
	}

	if (tok == "icontains" || tok == "contains") {
		BoolSkipWs(p);
		if (!BoolMatch(p, "(")) {
			BoolFail(p, "expected '(' after icontains/contains");
			return false;
		}
		uid_bool_value_t hay;
		uid_bool_value_t needle;
		if (!BoolParseStringValue(p, ctx, &hay)) {
			return false;
		}
		BoolSkipWs(p);
		if (!BoolMatch(p, ",")) {
			BoolFail(p, "expected ',' in icontains/contains");
			return false;
		}
		if (!BoolParseStringValue(p, ctx, &needle)) {
			return false;
		}
		BoolSkipWs(p);
		if (!BoolMatch(p, ")")) {
			BoolFail(p, "expected ')' after icontains/contains arguments");
			return false;
		}
		std::string hayS;
		std::string needleS;
		BoolCoerceString(hay, &hayS);
		BoolCoerceString(needle, &needleS);
		*out = BoolIContains(hayS, needleS);
		return true;
	}

	uid_bool_value_t val;
	if (!BoolLookupPath(ctx, tok, &val)) {
		BoolFail(p, "unknown bool lookup path");
		return false;
	}
	if (val.kind == UID_BOOL_VAL_BOOL) {
		*out = val.b;
		return true;
	}
	std::string s;
	BoolCoerceString(val, &s);
	if (s == "true") {
		*out = true;
		return true;
	}
	if (s == "false" || s.empty()) {
		*out = false;
		return true;
	}
	double n = 0.0;
	if (BoolTryNumber(s, &n)) {
		*out = n != 0.0;
		return true;
	}
	*out = true;
	return true;
}

bool BoolParseNot(uid_bool_parser_t *p, const uid_bool_lookup_ctx_t *ctx, bool *out)
{
	BoolSkipWs(p);
	if (BoolMatch(p, "!")) {
		bool v = false;
		if (!BoolParseNot(p, ctx, &v)) {
			return false;
		}
		*out = !v;
		return true;
	}
	return BoolParseCompare(p, ctx, out);
}

bool BoolParseAnd(uid_bool_parser_t *p, const uid_bool_lookup_ctx_t *ctx, bool *out)
{
	if (!BoolParseNot(p, ctx, out)) {
		return false;
	}
	for (;;) {
		BoolSkipWs(p);
		/* Changed in OPM: word ops replace C-style && / || in UI bool exprs. */
		if (BoolMatch(p, "&&")) {
			BoolFail(p, "use 'and'/'or' instead of '&&'/'||'");
			return false;
		}
		if (!BoolMatchWord(p, "and")) {
			break;
		}
		bool rhs = false;
		if (!BoolParseNot(p, ctx, &rhs)) {
			return false;
		}
		*out = *out && rhs;
	}
	return true;
}

bool BoolParseOr(uid_bool_parser_t *p, const uid_bool_lookup_ctx_t *ctx, bool *out)
{
	if (!BoolParseAnd(p, ctx, out)) {
		return false;
	}
	for (;;) {
		BoolSkipWs(p);
		if (BoolMatch(p, "||")) {
			BoolFail(p, "use 'and'/'or' instead of '&&'/'||'");
			return false;
		}
		if (!BoolMatchWord(p, "or")) {
			break;
		}
		bool rhs = false;
		if (!BoolParseAnd(p, ctx, &rhs)) {
			return false;
		}
		*out = *out || rhs;
	}
	return true;
}

} // namespace

bool UID_ParseBraceBoolExpr(const char *attrValue, std::string *outInner)
{
	if (!attrValue || !outInner) {
		return false;
	}
	const char *p = attrValue;
	while (*p && std::isspace(static_cast<unsigned char>(*p))) {
		++p;
	}
	if (*p != '{') {
		return false;
	}
	++p;
	const char *start = p;
	int depth = 1;
	while (*p) {
		if (*p == '{') {
			++depth;
		} else if (*p == '}') {
			--depth;
			if (depth == 0) {
				outInner->assign(start, static_cast<size_t>(p - start));
				return true;
			}
		}
		++p;
	}
	return false;
}

bool UID_VisibleIfToBoolExpr(const std::string &visibleIf, std::string *outExpr)
{
	if (!outExpr || visibleIf.empty()) {
		return false;
	}
	const char *p = visibleIf.c_str();
	if (std::strncmp(p, "cvar:", 5) == 0) {
		p += 5;
	}
	const char *neq = std::strstr(p, "!=");
	if (neq && neq > p) {
		std::string name(p, static_cast<size_t>(neq - p));
		std::string want(neq + 2);
		while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
			name.pop_back();
		}
		while (!want.empty() && std::isspace(static_cast<unsigned char>(want.front()))) {
			want.erase(want.begin());
		}
		*outExpr = "cvar." + name + " != " + want;
		return true;
	}
	const char *eq = std::strchr(p, '=');
	if (!eq || eq == p) {
		return false;
	}
	std::string name(p, static_cast<size_t>(eq - p));
	std::string want(eq + 1);
	while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
		name.pop_back();
	}
	while (!want.empty() && std::isspace(static_cast<unsigned char>(want.front()))) {
		want.erase(want.begin());
	}
	*outExpr = "cvar." + name + " == " + want;
	return true;
}

bool UID_VisibleIfIndexToBoolExpr(const std::string &visibleIfIndex, std::string *outExpr)
{
	if (!outExpr || visibleIfIndex.empty()) {
		return false;
	}
	std::string key = visibleIfIndex;
	if (key.rfind("index:", 0) == 0) {
		key = key.substr(6);
	}
	if (key == "selected") {
		*outExpr = "item.selected";
		return true;
	}
	if (key == "not-selected") {
		*outExpr = "!item.selected";
		return true;
	}
	if (key == "not-last") {
		*outExpr = "!item.last";
		return true;
	}
	if (key == "all") {
		*outExpr = "true";
		return true;
	}
	if (key.rfind("equals=", 0) == 0) {
		*outExpr = "item.index == " + key.substr(7);
		return true;
	}
	return false;
}

static void TrimStyleLiteral(std::string *s)
{
	if (!s) {
		return;
	}
	while (!s->empty() && std::isspace(static_cast<unsigned char>(s->front()))) {
		s->erase(s->begin());
	}
	while (!s->empty() && std::isspace(static_cast<unsigned char>(s->back()))) {
		s->pop_back();
	}
}

bool UID_EvalStyleTernary(
	const char                  *expr,
	const uid_bool_lookup_ctx_t *ctx,
	const uid_expr_limits_t     *limits,
	std::string                 *outValue,
	std::string                 *diagMessage
)
{
	if (!expr || !outValue) {
		if (diagMessage) {
			*diagMessage = "null style ternary argument";
		}
		return false;
	}

	uid_expr_limits_t localLimits;
	if (!limits) {
		UID_DefaultExprLimits(&localLimits);
		limits = &localLimits;
	}

	const size_t len = std::strlen(expr);
	if (static_cast<int>(len) > limits->maxExprBytes) {
		if (diagMessage) {
			*diagMessage = "style ternary exceeds byte limit";
		}
		return false;
	}

	/* Added in OPM: cache cond/then/else split keyed by expression text. */
	struct StyleTernarySplit {
		std::string cond;
		std::string thenLit;
		std::string elseLit;
	};
	static std::unordered_map<std::string, StyleTernarySplit> s_styleSplitCache;
	const bool useCache = UID_OptEnabled(UID_OPT_EXPR_CACHE) != 0;

	const StyleTernarySplit *split = nullptr;
	StyleTernarySplit localSplit;
	if (useCache) {
		const auto it = s_styleSplitCache.find(expr);
		if (it != s_styleSplitCache.end()) {
			split = &it->second;
		}
	}
	if (!split) {
		int depth = 0;
		size_t qPos = static_cast<size_t>(-1);
		size_t cPos = static_cast<size_t>(-1);
		for (size_t i = 0; i < len; ++i) {
			const char ch = expr[i];
			if (ch == '(') {
				++depth;
			} else if (ch == ')') {
				--depth;
			} else if (depth == 0 && ch == '?' && qPos == static_cast<size_t>(-1)) {
				qPos = i;
			} else if (depth == 0 && ch == ':' && qPos != static_cast<size_t>(-1) &&
					   cPos == static_cast<size_t>(-1)) {
				cPos = i;
				break;
			}
		}
		if (qPos == static_cast<size_t>(-1) || cPos == static_cast<size_t>(-1) || cPos <= qPos) {
			if (diagMessage) {
				*diagMessage = "style ternary requires boolExpr ? literal : literal";
			}
			return false;
		}

		localSplit.cond.assign(expr, qPos);
		localSplit.thenLit.assign(expr + qPos + 1, cPos - qPos - 1);
		localSplit.elseLit.assign(expr + cPos + 1);
		TrimStyleLiteral(&localSplit.cond);
		TrimStyleLiteral(&localSplit.thenLit);
		TrimStyleLiteral(&localSplit.elseLit);
		if (localSplit.cond.empty() || localSplit.thenLit.empty() || localSplit.elseLit.empty()) {
			if (diagMessage) {
				*diagMessage = "empty style ternary operand";
			}
			return false;
		}
		if (useCache) {
			if (s_styleSplitCache.size() >= 512) {
				s_styleSplitCache.clear();
			}
			auto em = s_styleSplitCache.emplace(expr, localSplit);
			split = &em.first->second;
		} else {
			split = &localSplit;
		}
	}

	auto resolveStyleBranch = [&](const std::string &branch) -> std::string {
		std::string lit = branch;
		TrimStyleLiteral(&lit);
		/* Added in OPM: nested ternaries in then/else (e.g. a ? x : b ? y : z). */
		if (lit.find('?') != std::string::npos) {
			std::string nested;
			std::string nestedDiag;
			if (UID_EvalStyleTernary(lit.c_str(), ctx, limits, &nested, &nestedDiag)) {
				return nested;
			}
		}
		if (!ctx || !ctx->doc) {
			return lit;
		}
		std::string varId;
		if (lit.rfind("var.", 0) == 0) {
			varId = lit.substr(4);
		} else if (lit.rfind("var:", 0) == 0) {
			varId = lit.substr(4);
		} else {
			return lit;
		}
		std::string value;
		if (UID_LookupVar(ctx->doc, varId.c_str(), &value)) {
			return value;
		}
		return lit;
	};

	bool result = false;
	if (!UID_EvalBool(split->cond.c_str(), ctx, limits, &result, diagMessage)) {
		return false;
	}
	*outValue = resolveStyleBranch(result ? split->thenLit : split->elseLit);
	return true;
}

bool UID_EvalBool(
	const char                  *expr,
	const uid_bool_lookup_ctx_t *ctx,
	const uid_expr_limits_t     *limits,
	bool                        *out,
	std::string                 *diagMessage
)
{
	if (!expr || !out) {
		if (diagMessage) {
			*diagMessage = "null bool expression argument";
		}
		return false;
	}

	uid_expr_limits_t localLimits;
	if (!limits) {
		UID_DefaultExprLimits(&localLimits);
		limits = &localLimits;
	}

	const size_t len = std::strlen(expr);
	if (static_cast<int>(len) > limits->maxExprBytes) {
		if (diagMessage) {
			*diagMessage = "bool expression exceeds byte limit";
		}
		return false;
	}

	/*
	 * Added in OPM: cache a lightweight "program" for common bool shapes so we
	 * skip recursive-descent reparse. Complex exprs fall back to full parse once
	 * and store a SLOW sentinel that reuses the text path (still avoids split work
	 * for style ternaries via the sibling cache).
	 *
	 * Fast shapes:
	 *   path                         (truthy lookup)
	 *   path == lit / != / etc.      (single compare)
	 *   !path
	 *   path and path / path or path (two-operand chains flattened at compile)
	 */
	enum BoolProgOp : int {
		BP_SLOW = -1,
		BP_TRUE = 0,
		BP_FALSE,
		BP_TRUTHY,   /* a = path */
		BP_NOT,      /* unary on a */
		BP_CMP,      /* a op b ; op in c */
		BP_AND2,     /* a and b */
		BP_OR2       /* a or b */
	};
	struct BoolProg {
		int         op;
		std::string a;
		std::string b;
		std::string c; /* compare op */
	};
	static std::unordered_map<std::string, BoolProg> s_boolProgCache;
	const bool useCache = UID_OptEnabled(UID_OPT_EXPR_CACHE) != 0;

	auto evalTruthyPath = [&](const std::string &path, bool *bout) -> bool {
		uid_bool_value_t val;
		if (!BoolLookupPath(ctx, path, &val)) {
			if (diagMessage) {
				*diagMessage = "unknown bool lookup path";
			}
			return false;
		}
		if (val.kind == UID_BOOL_VAL_BOOL) {
			*bout = val.b;
			return true;
		}
		std::string s;
		BoolCoerceString(val, &s);
		if (s == "true") {
			*bout = true;
			return true;
		}
		if (s == "false" || s.empty()) {
			*bout = false;
			return true;
		}
		double n = 0.0;
		if (BoolTryNumber(s, &n)) {
			*bout = n != 0.0;
			return true;
		}
		*bout = true;
		return true;
	};

	auto evalCmp = [&](const std::string &leftPath, const std::string &op, const std::string &rightTok,
					   bool rightIsPath, bool *bout) -> bool {
		uid_bool_value_t left;
		if (!BoolLookupPath(ctx, leftPath, &left)) {
			left.kind = UID_BOOL_VAL_STRING;
			left.s = leftPath;
		}
		uid_bool_value_t right;
		if (rightIsPath) {
			if (!BoolLookupPath(ctx, rightTok, &right)) {
				right.kind = UID_BOOL_VAL_STRING;
				right.s = rightTok;
			}
		} else {
			right.kind = UID_BOOL_VAL_STRING;
			right.s = rightTok;
			double rn = 0.0;
			if (BoolTryNumber(rightTok, &rn)) {
				right.kind = UID_BOOL_VAL_NUMBER;
				right.n = rn;
			}
		}
		if (!BoolEvalCompare(left, right, op.c_str(), bout)) {
			if (diagMessage) {
				*diagMessage = "invalid comparison operands";
			}
			return false;
		}
		return true;
	};

	auto tryCompileSimple = [&](const char *e, BoolProg *prog) -> bool {
		std::string s(e);
		TrimStyleLiteral(&s);
		if (s.empty()) {
			return false;
		}
		if (s == "true") {
			prog->op = BP_TRUE;
			return true;
		}
		if (s == "false") {
			prog->op = BP_FALSE;
			return true;
		}
		/* Reject functions / parens / multi-ops / arithmetic — fall back to slow parse. */
		if (s.find('(') != std::string::npos || s.find(')') != std::string::npos ||
			s.find("icontains") != std::string::npos || s.find("contains") != std::string::npos ||
			s.find("&&") != std::string::npos || s.find("||") != std::string::npos ||
			s.find('*') != std::string::npos || s.find('/') != std::string::npos ||
			s.find('+') != std::string::npos) {
			return false;
		}
		if (s[0] == '!') {
			std::string rest = s.substr(1);
			TrimStyleLiteral(&rest);
			if (rest.empty() || rest.find(' ') != std::string::npos ||
				rest.find("and") != std::string::npos || rest.find("or") != std::string::npos ||
				rest.find('=') != std::string::npos || rest.find('<') != std::string::npos ||
				rest.find('>') != std::string::npos || rest.find('!') != std::string::npos) {
				return false;
			}
			prog->op = BP_NOT;
			prog->a = rest;
			return true;
		}
		/* path and path  /  path or path (exactly one connector word) */
		{
			const char *connectors[] = {" and ", " or ", nullptr};
			for (int ci = 0; connectors[ci]; ++ci) {
				const size_t pos = s.find(connectors[ci]);
				if (pos == std::string::npos) {
					continue;
				}
				if (s.find(connectors[ci], pos + 1) != std::string::npos) {
					return false; /* multi-chain → slow */
				}
				std::string left = s.substr(0, pos);
				std::string right = s.substr(pos + std::strlen(connectors[ci]));
				TrimStyleLiteral(&left);
				TrimStyleLiteral(&right);
				if (left.empty() || right.empty()) {
					return false;
				}
				if (left.find(' ') != std::string::npos || right.find(' ') != std::string::npos ||
					left.find('=') != std::string::npos || right.find('=') != std::string::npos ||
					left.find('<') != std::string::npos || right.find('<') != std::string::npos ||
					left.find('>') != std::string::npos || right.find('>') != std::string::npos ||
					left.find('!') != std::string::npos || right.find('!') != std::string::npos) {
					return false;
				}
				prog->op = (ci == 0) ? BP_AND2 : BP_OR2;
				prog->a = left;
				prog->b = right;
				return true;
			}
		}
		/* path CMP literal_or_path */
		{
			static const char *const ops[] = {"==", "!=", "<=", ">=", "<", ">", nullptr};
			for (int i = 0; ops[i]; ++i) {
				const size_t pos = s.find(ops[i]);
				if (pos == std::string::npos) {
					continue;
				}
				std::string left = s.substr(0, pos);
				std::string right = s.substr(pos + std::strlen(ops[i]));
				TrimStyleLiteral(&left);
				TrimStyleLiteral(&right);
				if (left.empty() || right.empty()) {
					return false;
				}
				if (left.find(' ') != std::string::npos || left.find('=') != std::string::npos) {
					return false;
				}
				bool rightIsPath = false;
				if (right.size() >= 2 &&
					((right.front() == '"' && right.back() == '"') ||
					 (right.front() == '\'' && right.back() == '\''))) {
					right = right.substr(1, right.size() - 2);
					rightIsPath = false;
				} else if (right.find('.') != std::string::npos ||
						   right.rfind("cvar.", 0) == 0 || right.rfind("item.", 0) == 0) {
					/* path-looking RHS (e.g. cvar.foo / item.bar) */
					rightIsPath = true;
				} else {
					/* bare word or number literal */
					rightIsPath = false;
				}
				prog->op = BP_CMP;
				prog->a = left;
				prog->b = right;
				prog->c = ops[i];
				/* reuse unused field: store rightIsPath in op via sentinel on empty d —
				 * BoolProg has no bool; pack into c as "op\\0path" — simpler: use leading
				 * marker on b. Instead set c to op and use a prefix on b. */
				if (rightIsPath) {
					prog->b.insert(prog->b.begin(), '\1'); /* path marker */
				}
				return true;
			}
		}
		/* bare path */
		if (s.find(' ') == std::string::npos && s.find('=') == std::string::npos &&
			s.find('<') == std::string::npos && s.find('>') == std::string::npos &&
			s.find('!') == std::string::npos) {
			prog->op = BP_TRUTHY;
			prog->a = s;
			return true;
		}
		return false;
	};

	if (useCache) {
		const auto it = s_boolProgCache.find(expr);
		if (it != s_boolProgCache.end()) {
			const BoolProg &prog = it->second;
			if (prog.op == BP_SLOW) {
				/* fall through to full parse */
			} else if (prog.op == BP_TRUE) {
				*out = true;
				return true;
			} else if (prog.op == BP_FALSE) {
				*out = false;
				return true;
			} else if (prog.op == BP_TRUTHY) {
				return evalTruthyPath(prog.a, out);
			} else if (prog.op == BP_NOT) {
				bool v = false;
				if (!evalTruthyPath(prog.a, &v)) {
					return false;
				}
				*out = !v;
				return true;
			} else if (prog.op == BP_CMP) {
				bool rightIsPath = false;
				std::string right = prog.b;
				if (!right.empty() && right[0] == '\1') {
					rightIsPath = true;
					right.erase(right.begin());
				}
				return evalCmp(prog.a, prog.c, right, rightIsPath, out);
			} else if (prog.op == BP_AND2) {
				bool l = false;
				bool r = false;
				if (!evalTruthyPath(prog.a, &l) || !evalTruthyPath(prog.b, &r)) {
					return false;
				}
				*out = l && r;
				return true;
			} else if (prog.op == BP_OR2) {
				bool l = false;
				bool r = false;
				if (!evalTruthyPath(prog.a, &l) || !evalTruthyPath(prog.b, &r)) {
					return false;
				}
				*out = l || r;
				return true;
			}
		} else {
			BoolProg prog{};
			if (tryCompileSimple(expr, &prog)) {
				if (s_boolProgCache.size() >= 512) {
					s_boolProgCache.clear();
				}
				s_boolProgCache.emplace(expr, prog);
				/* Re-enter via cache hit path */
				return UID_EvalBool(expr, ctx, limits, out, diagMessage);
			}
			if (s_boolProgCache.size() >= 512) {
				s_boolProgCache.clear();
			}
			BoolProg slow{};
			slow.op = BP_SLOW;
			s_boolProgCache.emplace(expr, std::move(slow));
		}
	}

	uid_bool_parser_t parser;
	parser.src = expr;
	parser.len = len;
	parser.pos = 0;
	parser.diag = diagMessage;
	parser.failed = false;

	if (!BoolParseOr(&parser, ctx, out)) {
		return false;
	}
	BoolSkipWs(&parser);
	if (parser.pos < parser.len) {
		if (diagMessage) {
			*diagMessage = "trailing garbage in bool expression";
		}
		return false;
	}
	return !parser.failed;
}
