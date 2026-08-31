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

#include "uid_expr.h"
#include "uid_opt.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

enum uid_expr_op_t {
	UID_EXPR_LIT = 0,
	UID_EXPR_IDENT,
	UID_EXPR_NEG,
	UID_EXPR_POS,
	UID_EXPR_ADD,
	UID_EXPR_SUB,
	UID_EXPR_MUL,
	UID_EXPR_DIV,
	UID_EXPR_MOD,
	UID_EXPR_CALL /* Added in OPM: abs / min / max / clamp */
};

struct uid_expr_node_t {
	uid_expr_op_t     op;
	double            lit;
	std::string       ident;
	int               left;
	int               right;
	std::vector<int>  args; /* CALL argument node indices */
};

/* Added in OPM: whitelist arity for numeric helper calls. */
int UID_ExprCallArity(const char *name)
{
	if (!name) {
		return -1;
	}
	if (std::strcmp(name, "abs") == 0 || std::strcmp(name, "floor") == 0) {
		return 1;
	}
	if (std::strcmp(name, "min") == 0 || std::strcmp(name, "max") == 0) {
		return 2;
	}
	if (std::strcmp(name, "clamp") == 0) {
		return 3;
	}
	return -1;
}

struct uid_expr_parser_t {
	const char           *src;
	size_t                len;
	size_t                pos;
	int                   maxNodes;
	std::vector<uid_expr_node_t> nodes;
	std::string          *diag;
	bool                  failed;
};

void UID_ExprFail(uid_expr_parser_t *p, const char *msg)
{
	if (!p->failed) {
		p->failed = true;
		if (p->diag) {
			*p->diag = msg ? msg : "expression error";
		}
	}
}

void UID_ExprSkipWs(uid_expr_parser_t *p)
{
	while (p->pos < p->len && std::isspace(static_cast<unsigned char>(p->src[p->pos]))) {
		++p->pos;
	}
}

int UID_ExprAlloc(uid_expr_parser_t *p, uid_expr_op_t op)
{
	if (p->failed) {
		return -1;
	}
	if (static_cast<int>(p->nodes.size()) >= p->maxNodes) {
		UID_ExprFail(p, "expression exceeds node limit");
		return -1;
	}
	uid_expr_node_t node;
	node.op = op;
	node.lit = 0.0;
	node.left = -1;
	node.right = -1;
	node.args.clear();
	p->nodes.push_back(node);
	return static_cast<int>(p->nodes.size()) - 1;
}

int UID_ExprParseExpr(uid_expr_parser_t *p);

int UID_ExprParsePrimary(uid_expr_parser_t *p)
{
	UID_ExprSkipWs(p);
	if (p->failed || p->pos >= p->len) {
		UID_ExprFail(p, "unexpected end of expression");
		return -1;
	}

	const char c = p->src[p->pos];

	if (c == '(') {
		++p->pos;
		const int inner = UID_ExprParseExpr(p);
		UID_ExprSkipWs(p);
		if (p->pos >= p->len || p->src[p->pos] != ')') {
			UID_ExprFail(p, "missing closing parenthesis");
			return -1;
		}
		++p->pos;
		return inner;
	}

	if (c == '+' || c == '-') {
		const char sign = c;
		++p->pos;
		const int child = UID_ExprParsePrimary(p);
		if (child < 0) {
			return -1;
		}
		const int node = UID_ExprAlloc(p, sign == '-' ? UID_EXPR_NEG : UID_EXPR_POS);
		if (node < 0) {
			return -1;
		}
		p->nodes[static_cast<size_t>(node)].left = child;
		return node;
	}

	if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
		char   *end = nullptr;
		errno = 0;
		const double value = std::strtod(p->src + p->pos, &end);
		if (end == p->src + p->pos || errno == ERANGE) {
			UID_ExprFail(p, "invalid numeric literal");
			return -1;
		}
		const int node = UID_ExprAlloc(p, UID_EXPR_LIT);
		if (node < 0) {
			return -1;
		}
		p->nodes[static_cast<size_t>(node)].lit = value;
		p->pos = static_cast<size_t>(end - p->src);
		return node;
	}

	if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
		const size_t start = p->pos;
		++p->pos;
		while (p->pos < p->len) {
			const char ch = p->src[p->pos];
			if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.' || ch == '-') {
				++p->pos;
			} else {
				break;
			}
		}
		const size_t identEnd = p->pos;
		std::string identName(p->src + start, identEnd - start);
		UID_ExprSkipWs(p);
		if (p->pos < p->len && p->src[p->pos] == '(') {
			/* Added in OPM: whitelisted abs/min/max/clamp calls. */
			const int arity = UID_ExprCallArity(identName.c_str());
			if (arity < 0) {
				UID_ExprFail(p, "unknown function in expression");
				return -1;
			}
			++p->pos;
			std::vector<int> argNodes;
			UID_ExprSkipWs(p);
			if (arity == 0) {
				/* No zero-arity helpers today. */
			} else {
				for (int i = 0; i < arity; ++i) {
					if (i > 0) {
						UID_ExprSkipWs(p);
						if (p->pos >= p->len || p->src[p->pos] != ',') {
							UID_ExprFail(p, "expected comma between function arguments");
							return -1;
						}
						++p->pos;
					}
					const int arg = UID_ExprParseExpr(p);
					if (arg < 0) {
						return -1;
					}
					argNodes.push_back(arg);
				}
			}
			UID_ExprSkipWs(p);
			if (p->pos >= p->len || p->src[p->pos] != ')') {
				if (p->pos < p->len && p->src[p->pos] == ',') {
					UID_ExprFail(p, "too many function arguments");
				} else {
					UID_ExprFail(p, "missing closing parenthesis in function call");
				}
				return -1;
			}
			++p->pos;
			const int node = UID_ExprAlloc(p, UID_EXPR_CALL);
			if (node < 0) {
				return -1;
			}
			p->nodes[static_cast<size_t>(node)].ident = std::move(identName);
			p->nodes[static_cast<size_t>(node)].args = std::move(argNodes);
			return node;
		}

		const int node = UID_ExprAlloc(p, UID_EXPR_IDENT);
		if (node < 0) {
			return -1;
		}
		std::string &ident = p->nodes[static_cast<size_t>(node)].ident;
		ident = std::move(identName);
		if (ident.empty() || ident.back() == '.' || ident.back() == '-' || ident.find("..") != std::string::npos) {
			UID_ExprFail(p, "invalid property path");
			return -1;
		}
		return node;
	}

	UID_ExprFail(p, "unexpected token in expression");
	return -1;
}

int UID_ExprParseMul(uid_expr_parser_t *p)
{
	int left = UID_ExprParsePrimary(p);
	if (left < 0) {
		return -1;
	}

	for (;;) {
		UID_ExprSkipWs(p);
		if (p->pos >= p->len) {
			break;
		}
		const char c = p->src[p->pos];
		if (c != '*' && c != '/' && c != '%') {
			break;
		}
		++p->pos;
		const int right = UID_ExprParsePrimary(p);
		if (right < 0) {
			return -1;
		}
		uid_expr_op_t op = UID_EXPR_MUL;
		if (c == '/') {
			op = UID_EXPR_DIV;
		} else if (c == '%') {
			op = UID_EXPR_MOD;
		}
		const int node = UID_ExprAlloc(p, op);
		if (node < 0) {
			return -1;
		}
		p->nodes[static_cast<size_t>(node)].left = left;
		p->nodes[static_cast<size_t>(node)].right = right;
		left = node;
	}
	return left;
}

int UID_ExprParseExpr(uid_expr_parser_t *p)
{
	int left = UID_ExprParseMul(p);
	if (left < 0) {
		return -1;
	}

	for (;;) {
		UID_ExprSkipWs(p);
		if (p->pos >= p->len) {
			break;
		}
		const char c = p->src[p->pos];
		if (c != '+' && c != '-') {
			break;
		}
		/* Disallow assignment-like tokens. */
		if (p->pos + 1 < p->len && p->src[p->pos + 1] == '=') {
			UID_ExprFail(p, "assignment is not allowed in expressions");
			return -1;
		}
		++p->pos;
		const int right = UID_ExprParseMul(p);
		if (right < 0) {
			return -1;
		}
		const int node = UID_ExprAlloc(p, c == '+' ? UID_EXPR_ADD : UID_EXPR_SUB);
		if (node < 0) {
			return -1;
		}
		p->nodes[static_cast<size_t>(node)].left = left;
		p->nodes[static_cast<size_t>(node)].right = right;
		left = node;
	}
	return left;
}

bool UID_ExprEvalNode(
	const std::vector<uid_expr_node_t> &nodes,
	int nodeIndex,
	uid_expr_lookup_fn lookup,
	void *userdata,
	double *out,
	std::string *diagMessage
)
{
	if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= nodes.size()) {
		if (diagMessage) {
			*diagMessage = "invalid expression node";
		}
		return false;
	}

	const uid_expr_node_t &node = nodes[static_cast<size_t>(nodeIndex)];
	switch (node.op) {
	case UID_EXPR_LIT:
		*out = node.lit;
		return true;
	case UID_EXPR_IDENT: {
		if (!lookup) {
			if (diagMessage) {
				*diagMessage = "missing expression lookup callback";
			}
			return false;
		}
		double value = 0.0;
		if (!lookup(userdata, node.ident.c_str(), &value)) {
			if (diagMessage) {
				*diagMessage = std::string("unknown property path: ") + node.ident;
			}
			return false;
		}
		*out = value;
		return true;
	}
	case UID_EXPR_NEG: {
		double child = 0.0;
		if (!UID_ExprEvalNode(nodes, node.left, lookup, userdata, &child, diagMessage)) {
			return false;
		}
		*out = -child;
		return true;
	}
	case UID_EXPR_POS: {
		return UID_ExprEvalNode(nodes, node.left, lookup, userdata, out, diagMessage);
	}
	case UID_EXPR_CALL: {
		/* Added in OPM: evaluate whitelisted numeric helpers. */
		std::vector<double> vals;
		vals.reserve(node.args.size());
		for (int argIdx : node.args) {
			double v = 0.0;
			if (!UID_ExprEvalNode(nodes, argIdx, lookup, userdata, &v, diagMessage)) {
				return false;
			}
			vals.push_back(v);
		}
		double result = 0.0;
		if (node.ident == "abs") {
			if (vals.size() != 1) {
				if (diagMessage) {
					*diagMessage = "abs expects 1 argument";
				}
				return false;
			}
			result = std::fabs(vals[0]);
		} else if (node.ident == "floor") {
			/* Added in OPM: truncate toward -inf for MM:SS and similar HUD math. */
			if (vals.size() != 1) {
				if (diagMessage) {
					*diagMessage = "floor expects 1 argument";
				}
				return false;
			}
			result = std::floor(vals[0]);
		} else if (node.ident == "min") {
			if (vals.size() != 2) {
				if (diagMessage) {
					*diagMessage = "min expects 2 arguments";
				}
				return false;
			}
			result = std::fmin(vals[0], vals[1]);
		} else if (node.ident == "max") {
			if (vals.size() != 2) {
				if (diagMessage) {
					*diagMessage = "max expects 2 arguments";
				}
				return false;
			}
			result = std::fmax(vals[0], vals[1]);
		} else if (node.ident == "clamp") {
			if (vals.size() != 3) {
				if (diagMessage) {
					*diagMessage = "clamp expects 3 arguments";
				}
				return false;
			}
			double lo = vals[1];
			double hi = vals[2];
			if (lo > hi) {
				const double tmp = lo;
				lo = hi;
				hi = tmp;
			}
			result = std::fmin(std::fmax(vals[0], lo), hi);
		} else {
			if (diagMessage) {
				*diagMessage = std::string("unknown function: ") + node.ident;
			}
			return false;
		}
		if (!std::isfinite(result)) {
			if (diagMessage) {
				*diagMessage = "non-finite expression result";
			}
			return false;
		}
		*out = result;
		return true;
	}
	case UID_EXPR_ADD:
	case UID_EXPR_SUB:
	case UID_EXPR_MUL:
	case UID_EXPR_DIV:
	case UID_EXPR_MOD: {
		double lhs = 0.0;
		double rhs = 0.0;
		if (!UID_ExprEvalNode(nodes, node.left, lookup, userdata, &lhs, diagMessage)) {
			return false;
		}
		if (!UID_ExprEvalNode(nodes, node.right, lookup, userdata, &rhs, diagMessage)) {
			return false;
		}
		double result = 0.0;
		switch (node.op) {
		case UID_EXPR_ADD:
			result = lhs + rhs;
			break;
		case UID_EXPR_SUB:
			result = lhs - rhs;
			break;
		case UID_EXPR_MUL:
			result = lhs * rhs;
			break;
		case UID_EXPR_DIV:
			if (rhs == 0.0) {
				if (diagMessage) {
					*diagMessage = "division by zero";
				}
				return false;
			}
			result = lhs / rhs;
			break;
		case UID_EXPR_MOD:
			if (rhs == 0.0) {
				if (diagMessage) {
					*diagMessage = "modulo by zero";
				}
				return false;
			}
			result = std::fmod(lhs, rhs);
			break;
		default:
			break;
		}
		if (!std::isfinite(result)) {
			if (diagMessage) {
				*diagMessage = "non-finite expression result";
			}
			return false;
		}
		*out = result;
		return true;
	}
	default:
		if (diagMessage) {
			*diagMessage = "unsupported expression operator";
		}
		return false;
	}
}

/* Compatibility wrapper for call sites that still pass a parser. */
bool UID_ExprEvalNode(
	const uid_expr_parser_t *p,
	int nodeIndex,
	uid_expr_lookup_fn lookup,
	void *userdata,
	double *out,
	std::string *diagMessage
)
{
	if (!p) {
		if (diagMessage) {
			*diagMessage = "null expression parser";
		}
		return false;
	}
	return UID_ExprEvalNode(p->nodes, nodeIndex, lookup, userdata, out, diagMessage);
}

void UID_FormatNumberInvariant(double value, std::string *out)
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.15g", value);
	for (char *p = buf; *p; ++p) {
		if (*p == ',') {
			*p = '.';
		}
	}
	*out = buf;
}

} // namespace

void UID_DefaultExprLimits(uid_expr_limits_t *out)
{
	if (!out) {
		return;
	}
	out->maxExprBytes = 1024;
	out->maxExprNodes = 128;
}

bool UID_EvalNumber(
	const char *expr,
	uid_expr_lookup_fn lookup,
	void *userdata,
	const uid_expr_limits_t *limits,
	double *out,
	std::string *diagMessage
)
{
	if (!expr || !out) {
		if (diagMessage) {
			*diagMessage = "null expression argument";
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
			*diagMessage = "expression exceeds byte limit";
		}
		return false;
	}

	/* Reject assignment and other disallowed tokens early. */
	if (std::strchr(expr, '=')) {
		if (diagMessage) {
			*diagMessage = "assignment is not allowed in expressions";
		}
		return false;
	}

	/* Added in OPM: cache parsed AST keyed by expression text. */
	struct AstCacheEntry {
		std::vector<uid_expr_node_t> nodes;
		int                         root;
	};
	static std::unordered_map<std::string, AstCacheEntry> s_astCache;
	const bool useCache = UID_OptEnabled(UID_OPT_EXPR_CACHE) != 0;

	uid_expr_parser_t parser;
	parser.src = expr;
	parser.len = len;
	parser.pos = 0;
	parser.maxNodes = limits->maxExprNodes > 0 ? limits->maxExprNodes : 128;
	parser.diag = diagMessage;
	parser.failed = false;

	if (useCache) {
		const auto it = s_astCache.find(expr);
		if (it != s_astCache.end()) {
			/* Added in OPM: evaluate cached AST by const ref (no vector copy). */
			return UID_ExprEvalNode(it->second.nodes, it->second.root, lookup, userdata, out, diagMessage);
		}
	}

	const int root = UID_ExprParseExpr(&parser);
	UID_ExprSkipWs(&parser);
	if (parser.failed || root < 0) {
		return false;
	}
	if (parser.pos < parser.len) {
		if (diagMessage) {
			*diagMessage = "trailing garbage in expression";
		}
		return false;
	}

	if (useCache) {
		if (s_astCache.size() >= 512) {
			s_astCache.clear();
		}
		AstCacheEntry entry;
		entry.nodes = parser.nodes;
		entry.root = root;
		s_astCache.emplace(expr, std::move(entry));
	}

	return UID_ExprEvalNode(parser.nodes, root, lookup, userdata, out, diagMessage);
}

bool UID_InterpolateString(
	const char *text,
	uid_expr_lookup_fn lookup,
	void *userdata,
	const uid_expr_limits_t *limits,
	std::string *out,
	std::string *diagMessage
)
{
	if (!text || !out) {
		if (diagMessage) {
			*diagMessage = "null interpolate argument";
		}
		return false;
	}

	out->clear();
	const size_t len = std::strlen(text);
	size_t       i = 0;

	while (i < len) {
		if (text[i] != '{') {
			out->push_back(text[i++]);
			continue;
		}

		/* Find matching '}' (expressions do not nest braces). */
		size_t j = i + 1;
		bool   found = false;
		while (j < len) {
			if (text[j] == '}') {
				found = true;
				break;
			}
			if (text[j] == '{') {
				if (diagMessage) {
					*diagMessage = "nested braces are not allowed in interpolated text";
				}
				return false;
			}
			++j;
		}
		if (!found) {
			if (diagMessage) {
				*diagMessage = "unclosed expression brace";
			}
			return false;
		}

		const std::string expr(text + i + 1, j - (i + 1));
		double            value = 0.0;
		if (!UID_EvalNumber(expr.c_str(), lookup, userdata, limits, &value, diagMessage)) {
			return false;
		}

		std::string formatted;
		UID_FormatNumberInvariant(value, &formatted);
		out->append(formatted);
		i = j + 1;
	}

	return true;
}
