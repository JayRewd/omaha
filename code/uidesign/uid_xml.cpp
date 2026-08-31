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

#include "uid_xml.h"

#include "uid_expr_bool.h"
#include "uid_value.h"
#include "../thirdparty/tinyxml2/tinyxml2.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <set>
#include <string>
#include <vector>

using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;
using tinyxml2::XMLNode;
using tinyxml2::XMLText;

namespace {

uid_source_location_t MakeLoc(const char *sourceName, const XMLNode *node)
{
	uid_source_location_t loc;
	loc.path = sourceName;
	loc.line = node ? node->GetLineNum() : 0;
	loc.column = 0;
	return loc;
}

bool ContainsDoctype(const char *xml, size_t size)
{
	if (!xml || size < 9) {
		return false;
	}
	/* Case-insensitive scan for "<!DOCTYPE". */
	const char *needle = "<!DOCTYPE";
	const size_t nlen = 9;
	for (size_t i = 0; i + nlen <= size; ++i) {
		bool match = true;
		for (size_t j = 0; j < nlen; ++j) {
			const char a = static_cast<char>(std::toupper(static_cast<unsigned char>(xml[i + j])));
			const char b = needle[j];
			if (a != b) {
				match = false;
				break;
			}
		}
		if (match) {
			return true;
		}
	}
	return false;
}

bool IsWhitespaceOnly(const char *text)
{
	if (!text) {
		return true;
	}
	for (const char *p = text; *p; ++p) {
		if (!std::isspace(static_cast<unsigned char>(*p))) {
			return false;
		}
	}
	return true;
}

std::string TrimText(const char *text)
{
	if (!text) {
		return std::string();
	}
	const char *begin = text;
	while (*begin && std::isspace(static_cast<unsigned char>(*begin))) {
		++begin;
	}
	const char *end = text + std::strlen(text);
	while (end > begin && std::isspace(static_cast<unsigned char>(end[-1]))) {
		--end;
	}
	return std::string(begin, end);
}

bool ParseCvarCompat(const char *raw, std::string *bindOut)
{
	if (!raw || !bindOut) {
		return false;
	}
	/* context="cvar(name)" / value="cvar(name)" */
	const char *p = raw;
	while (*p && std::isspace(static_cast<unsigned char>(*p))) {
		++p;
	}
	if (std::strncmp(p, "cvar(", 5) != 0) {
		return false;
	}
	p += 5;
	const char *close = std::strchr(p, ')');
	if (!close || close == p) {
		return false;
	}
	std::string name(p, close);
	*bindOut = "cvar:" + name;
	return true;
}

bool ParseEventName(const char *text, uid_event_kind_t *out)
{
	if (!text || !out) {
		return false;
	}
	if (std::strcmp(text, "click") == 0) {
		*out = UID_EVENT_CLICK;
		return true;
	}
	if (std::strcmp(text, "change") == 0) {
		*out = UID_EVENT_CHANGE;
		return true;
	}
	if (std::strcmp(text, "submit") == 0) {
		*out = UID_EVENT_SUBMIT;
		return true;
	}
	if (std::strcmp(text, "cancel") == 0) {
		*out = UID_EVENT_CANCEL;
		return true;
	}
	if (std::strcmp(text, "focus") == 0) {
		*out = UID_EVENT_FOCUS;
		return true;
	}
	if (std::strcmp(text, "blur") == 0) {
		*out = UID_EVENT_BLUR;
		return true;
	}
	if (std::strcmp(text, "dblclick") == 0) {
		*out = UID_EVENT_DBLCLICK;
		return true;
	}
	return false;
}

bool ParseCommitMode(const char *text, uid_commit_mode_t *out)
{
	if (!text || !out) {
		return false;
	}
	if (std::strcmp(text, "change") == 0) {
		*out = UID_COMMIT_CHANGE;
		return true;
	}
	if (std::strcmp(text, "submit") == 0) {
		*out = UID_COMMIT_SUBMIT;
		return true;
	}
	if (std::strcmp(text, "apply") == 0) {
		*out = UID_COMMIT_APPLY;
		return true;
	}
	return false;
}

bool ParsePropType(const char *text, uid_prop_type_t *out)
{
	if (!text || !out) {
		return false;
	}
	if (std::strcmp(text, "string") == 0) {
		*out = UID_PROP_STRING;
		return true;
	}
	if (std::strcmp(text, "number") == 0) {
		*out = UID_PROP_NUMBER;
		return true;
	}
	if (std::strcmp(text, "length") == 0) {
		*out = UID_PROP_LENGTH;
		return true;
	}
	if (std::strcmp(text, "color") == 0) {
		*out = UID_PROP_COLOR;
		return true;
	}
	if (std::strcmp(text, "boolean") == 0) {
		*out = UID_PROP_BOOLEAN;
		return true;
	}
	if (std::strcmp(text, "binding") == 0) {
		*out = UID_PROP_BINDING;
		return true;
	}
	if (std::strcmp(text, "identifier") == 0) {
		*out = UID_PROP_IDENTIFIER;
		return true;
	}
	return false;
}

bool IsCommonBoxAttr(const std::string &name)
{
	static const char *const kAttrs[] = {
		"id", "width", "height", "padding", "margin", "fill", "color", "shape", "radius",
		/* Added in Omaha: definite clamp after authored/intrinsic/fill resolve (px/% only). */
		"max-width", "max-height",
		"border", "border-top", "border-right", "border-bottom", "border-left",
		"stroke", "stroke-width", "stroke-layout", "shape-rotation", "rotation", "rotation-origin", "opacity",
		"crisp", /* Added in OPM: binary path coverage (no soft AA), e.g. crosshair */
		"translate-x", "translate-y", /* Added in OPM: post-flow layout offset */
		"background-image", "background-fit", "background-scale",
		"mask-image", "mask-fit", /* Added in OPM: soft coverage mask for subtree */
		"src", "fit", "scale", /* Added in OPM: leaf <image> (aliases of background-*) */
		"left", "top", "right", "bottom",
		"font", "font-size", "font-weight", "text-skew", "letter-spacing", "text-wrap", "line-height",
		"halign", "valign", /* Added in OPM: text/box align on labels and other box nodes */
		"drop-shadow",
		/* Added in OPM: paint-time marquee on labels (clip via parent overflow=hidden). */
		"marquee", "marquee-speed", "marquee-gap", "marquee-delay",
		"visible", "enabled",
		"text-cvar", "modal-role"
	};
	for (const char *a : kAttrs) {
		if (name == a) {
			return true;
		}
	}
	return false;
}

bool IsImageAttr(const std::string &name)
{
	/* Added in OPM: leaf <image> — common box + src/fit/scale (background-* accepted as aliases). */
	return IsCommonBoxAttr(name);
}

bool IsCommonControlAttr(const std::string &name)
{
	if (IsCommonBoxAttr(name)) {
		return true;
	}
		static const char *const kAttrs[] = {
		"bind", "commit", "value-type", "tab-index", "visible-if", "enabled-if", "set-value",
		"step-index", "set-index", "visible-if-index", "empty-label", "capture-label",
		"confirm-modal", "modal-cvar", "slot", "text-cvar", "modal-role",
		"hoverfill", "pressed-fill", "focus-fill", "disabled-fill", "selected-fill",
		"hover-color", "pressed-color", "focus-color", "disabled-color",
		/* Added in OPM: pack children like a container (flatten layout-only wrappers). */
		"type", "gap"
	};
	for (const char *a : kAttrs) {
		if (name == a) {
			return true;
		}
	}
	return false;
}

struct ParseContext;

static bool ParseModelFloatAttr(
	const char *text, float *out, ParseContext &ctx, const uid_source_location_t &loc, const char *attrName
);
static bool ParseModelVec3Attr(
	const char *text, float out[3], ParseContext &ctx, const uid_source_location_t &loc, const char *attrName
);
static bool ParseModelBboxAttr(
	const char *text, float mins[3], float maxs[3], ParseContext &ctx, const uid_source_location_t &loc
);

bool IsModelAttr(const std::string &name)
{
	if (IsCommonControlAttr(name)) {
		return true;
	}
	return name == "team" || name == "model" || name == "anim" || name == "anim-phase" || name == "anim-variant"
	    || name == "angles" || name == "fov" || name == "scale" || name == "offset" || name == "bbox"
	    || name == "bbox-from-model" || name == "framing-scale" || name == "color";
}

/* Added in OPM */
bool IsServerListAttr(const std::string &name)
{
	if (IsCommonBoxAttr(name)) {
		return true;
	}
	return name == "role";
}

bool IsSliderPartAttr(const std::string &name)
{
	/* Parts are chrome only — no bind/commit. */
	return IsCommonBoxAttr(name) || name == "hoverfill" || name == "pressed-fill" ||
		name == "focus-fill" || name == "disabled-fill" || name == "selected-fill" || name == "hover-color" ||
		name == "pressed-color" || name == "focus-color" || name == "disabled-color";
}

bool IsContainerAttr(const std::string &name)
{
	if (IsCommonBoxAttr(name)) {
		return true;
	}
	return name == "type" || name == "halign" || name == "valign" || name == "gap" || name == "overflow" ||
		name == "scrollbar" || name == "scrollbar-edge" || name == "role" ||
		name == "visible-if" || name == "enabled-if" || name == "source" || name == "wrap" || name == "scroll" || name == "index" ||
		name == "collection-display" || name == "default-index" ||
		name == "set-index" || name == "visible-if-index" || name == "hoverfill" || name == "pressed-fill" ||
		name == "focus-fill" || name == "disabled-fill" || name == "selected-fill" || name == "hover-color" || name == "pressed-color" ||
		name == "focus-color" || name == "disabled-color";
}

bool IsForeachAttr(const std::string &name)
{
	return name == "mode" || name == "count" || name == "row-height" || name == "lifetime"
		|| name == "fade-duration" || name == "source" || name == "wrap" || name == "scroll"
		|| name == "index" || IsCommonBoxAttr(name);
}

/* Added in OPM: per-item foreach template wrap inherits layout axis from <foreach>. */
static bool ForeachTemplateMainAxisAllFill(const uid_node_def_t &foreachNode, bool horizontalForeach)
{
	const char *mainProp = horizontalForeach ? "width" : "height";
	if (foreachNode.foreachTemplateRoot < 0 || foreachNode.foreachTemplateNodes.empty()) {
		return false;
	}
	const uid_node_def_t &wrap =
		foreachNode.foreachTemplateNodes[static_cast<size_t>(foreachNode.foreachTemplateRoot)];
	if (wrap.children.empty()) {
		return false;
	}
	for (uid_node_id_t c : wrap.children) {
		if (c < 0 || static_cast<size_t>(c) >= foreachNode.foreachTemplateNodes.size()) {
			return false;
		}
		const uid_node_def_t &child = foreachNode.foreachTemplateNodes[static_cast<size_t>(c)];
		std::string v;
		if (!child.properties.Get(mainProp, &v) || v != "fill") {
			return false;
		}
	}
	return true;
}

void CopyForeachTemplateLayout(const uid_node_def_t &foreachNode, uid_node_def_t *wrap)
{
	if (!wrap) {
		return;
	}
	static const char *const kLayoutProps[] = {
		"type", "halign", "valign", "gap", "padding", "margin", nullptr,
	};
	for (const char *const *p = kLayoutProps; *p; ++p) {
		std::string v;
		if (foreachNode.properties.Get(*p, &v) && !v.empty()) {
			wrap->properties.Set(*p, v.c_str());
		}
	}
	std::string axis;
	if (!foreachNode.properties.Get("type", &axis) || axis.empty()) {
		axis = "vertical";
	}
	if (axis == "horizontal") {
		std::string h;
		if (foreachNode.properties.Get("height", &h) && !h.empty()) {
			wrap->properties.Set("height", h.c_str());
		} else {
			wrap->properties.Set("height", "auto");
		}
		if (ForeachTemplateMainAxisAllFill(foreachNode, true)) {
			wrap->properties.Set("width", "fill");
		}
	} else {
		std::string w;
		if (foreachNode.properties.Get("width", &w) && !w.empty()) {
			wrap->properties.Set("width", w.c_str());
		} else {
			wrap->properties.Set("width", "auto");
		}
		if (ForeachTemplateMainAxisAllFill(foreachNode, false)) {
			wrap->properties.Set("height", "fill");
		}
	}
}

bool IsDefaultsAttr(const std::string &name)
{
	return IsContainerAttr(name) || name == "font" || name == "font-size" || name == "font-weight" ||
	       name == "fill" || name == "color" || name == "shape" || name == "visible" || name == "enabled" ||
	       name == "drop-shadow";
}

struct ParseContext;
bool ParseDefinitionChildren(ParseContext &ctx, XMLElement *parent);
bool ParseUiLibraryBytes(ParseContext &ctx, const std::string &resolvedPath, const char *xml, size_t size);
bool ParseImport(ParseContext &ctx, XMLElement *el);

struct ParseContext {
	const char           *sourceName;
	const uid_limits_t   *limits;
	const uid_parse_io_t *io;
	uid_document_t       *doc;
	uid_diag_list_t      *diags;
	int                   parsedNodeCount;
	bool                  failed;
	std::vector<std::string> importStack;
	int                   importFileCount;
	size_t                importBytesTotal;

	void Error(const uid_source_location_t &loc, const char *msg)
	{
		failed = true;
		if (diags) {
			diags->Error(loc, msg);
		}
	}

	void Warning(const uid_source_location_t &loc, const char *msg)
	{
		if (diags) {
			diags->Warning(loc, msg);
		}
	}

	void Errorf(const uid_source_location_t &loc, const std::string &msg)
	{
		Error(loc, msg.c_str());
	}
};

static bool ParseModelFloatAttr(
	const char *text, float *out, ParseContext &ctx, const uid_source_location_t &loc, const char *attrName
)
{
	char *end = nullptr;

	if (!text || !text[0] || !out) {
		return false;
	}
	*out = std::strtof(text, &end);
	if (!end || end == text || *end != '\0') {
		ctx.Errorf(loc, std::string("<model> ") + attrName + " must be a number");
		return false;
	}
	return true;
}

static bool ParseModelVec3Attr(
	const char *text, float out[3], ParseContext &ctx, const uid_source_location_t &loc, const char *attrName
)
{
	char *end = nullptr;
	int   i;

	if (!text || !text[0] || !out) {
		return false;
	}
	for (i = 0; i < 3; i++) {
		out[i] = std::strtof(text, &end);
		if (!end || end == text) {
			ctx.Errorf(loc, std::string("<model> ") + attrName + " must be three numbers");
			return false;
		}
		while (*end == ' ') {
			end++;
		}
		text = end;
	}
	if (*text != '\0') {
		ctx.Errorf(loc, std::string("<model> ") + attrName + " must be three numbers");
		return false;
	}
	return true;
}

static bool ParseModelBboxAttr(
	const char *text, float mins[3], float maxs[3], ParseContext &ctx, const uid_source_location_t &loc
)
{
	char *end = nullptr;
	int   i;

	if (!text || !text[0] || !mins || !maxs) {
		return false;
	}
	for (i = 0; i < 3; i++) {
		mins[i] = std::strtof(text, &end);
		if (!end || end == text) {
			ctx.Error(loc, "<model> bbox must be six numbers: minX minY minZ maxX maxY maxZ");
			return false;
		}
		while (*end == ' ') {
			end++;
		}
		text = end;
	}
	for (i = 0; i < 3; i++) {
		maxs[i] = std::strtof(text, &end);
		if (!end || end == text) {
			ctx.Error(loc, "<model> bbox must be six numbers: minX minY minZ maxX maxY maxZ");
			return false;
		}
		while (*end == ' ') {
			end++;
		}
		text = end;
	}
	if (*text != '\0') {
		ctx.Error(loc, "<model> bbox must be six numbers: minX minY minZ maxX maxY maxZ");
		return false;
	}
	return true;
}

bool PathHasDotDot(const char *src)
{
	if (!src) {
		return false;
	}
	return std::strstr(src, "..") != nullptr;
}

bool PathHasBackslash(const char *src)
{
	if (!src) {
		return false;
	}
	return std::strchr(src, '\\') != nullptr;
}

bool ResolveImportPath(const char *baseVfsPath, const char *src, std::string *out)
{
	if (!src || !src[0] || !out) {
		return false;
	}
	if (PathHasDotDot(src) || PathHasBackslash(src)) {
		return false;
	}
	if (src[0] == '/') {
		*out = src + 1;
		return !out->empty();
	}
	/* VFS-rooted paths (menu imports) vs directory-relative (library imports). */
	if (std::strncmp(src, "ui/", 3) == 0) {
		*out = src;
		return true;
	}
	if (!baseVfsPath || !baseVfsPath[0]) {
		*out = src;
		return true;
	}
	std::string base(baseVfsPath);
	const size_t slash = base.rfind('/');
	if (slash == std::string::npos) {
		*out = src;
		return true;
	}
	*out = base.substr(0, slash + 1) + src;
	return true;
}

void WarnDefinitionOverride(ParseContext &ctx, const uid_source_location_t &loc, const char *kind, const char *id)
{
	ctx.Warning(loc, (std::string(kind) + " \"" + id + "\" overrides an earlier definition").c_str());
}

bool ConsumeNodeBudget(ParseContext &ctx, const uid_source_location_t &loc)
{
	++ctx.parsedNodeCount;
	if (ctx.parsedNodeCount > ctx.limits->maxParsedNodes) {
		ctx.Error(loc, "parsed node count exceeds limit");
		return false;
	}
	return true;
}

bool CheckDepth(ParseContext &ctx, int depth, const uid_source_location_t &loc)
{
	if (depth > ctx.limits->maxXmlDepth) {
		ctx.Error(loc, "XML depth exceeds limit");
		return false;
	}
	return true;
}

bool CheckIdLen(ParseContext &ctx, const std::string &id, const uid_source_location_t &loc)
{
	if (static_cast<int>(id.size()) > ctx.limits->maxIdLen) {
		ctx.Error(loc, "id exceeds maxIdLen");
		return false;
	}
	return true;
}

bool CheckTextLen(ParseContext &ctx, const std::string &text, const uid_source_location_t &loc)
{
	if (static_cast<int>(text.size()) > ctx.limits->maxTextBytes) {
		ctx.Error(loc, "text exceeds maxTextBytes");
		return false;
	}
	return true;
}

bool CheckPathLen(ParseContext &ctx, const std::string &path, const uid_source_location_t &loc)
{
	if (static_cast<int>(path.size()) > ctx.limits->maxPathBytes) {
		ctx.Error(loc, "path data exceeds maxPathBytes");
		return false;
	}
	return true;
}

/* ---- prop declarations ---- */

bool ParsePropDecls(ParseContext &ctx, XMLElement *propsEl, std::vector<uid_prop_decl_t> *out)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, propsEl);
	for (XMLElement *child = propsEl->FirstChildElement(); child; child = child->NextSiblingElement()) {
		if (std::strcmp(child->Name(), "prop") != 0) {
			ctx.Errorf(MakeLoc(ctx.sourceName, child), std::string("unknown element in <props>: ") + child->Name());
			return false;
		}
		const char *name = child->Attribute("name");
		const char *typeStr = child->Attribute("type");
		if (!name || !name[0] || !typeStr) {
			ctx.Error(MakeLoc(ctx.sourceName, child), "<prop> requires name and type");
			return false;
		}
		if (static_cast<int>(std::strlen(name)) > ctx.limits->maxPropNameLen) {
			ctx.Error(MakeLoc(ctx.sourceName, child), "prop name exceeds maxPropNameLen");
			return false;
		}
		uid_prop_type_t ptype;
		if (!ParsePropType(typeStr, &ptype)) {
			ctx.Error(MakeLoc(ctx.sourceName, child), "unknown prop type");
			return false;
		}
		uid_prop_decl_t decl;
		decl.name = name;
		decl.type = ptype;
		decl.required = false;
		const char *req = child->Attribute("required");
		if (req) {
			bool b = false;
			std::string dm;
			if (!UID_ParseBool(req, &b, &dm)) {
				ctx.Error(MakeLoc(ctx.sourceName, child), "invalid required boolean");
				return false;
			}
			decl.required = b;
		}
		const char *def = child->Attribute("default");
		if (def) {
			if (decl.required) {
				ctx.Error(MakeLoc(ctx.sourceName, child), "required prop cannot also have a default");
				return false;
			}
			decl.defaultValue = def;
		}
		/* Reject unknown attributes on <prop>. */
		for (const tinyxml2::XMLAttribute *attr = child->FirstAttribute(); attr; attr = attr->Next()) {
			const char *an = attr->Name();
			if (std::strcmp(an, "name") && std::strcmp(an, "type") && std::strcmp(an, "required") &&
			    std::strcmp(an, "default")) {
				ctx.Errorf(MakeLoc(ctx.sourceName, child), std::string("unknown attribute on <prop>: ") + an);
				return false;
			}
		}
		out->push_back(decl);
	}
	(void)loc;
	return true;
}

/* ---- actions / handlers ---- */

bool ParseUpdatePropShorthand(ParseContext &ctx, const char *actionAttr, uid_node_def_t *node, const uid_source_location_t &loc)
{
	ctx.Warning(loc, "action=\"UpdateProp(...)\" is compatibility shorthand; prefer <on><set/></on>");
	uid_action_handler_t handler;
	handler.event = UID_EVENT_CLICK;
	handler.loc = loc;

	const char *p = actionAttr;
	while (*p) {
		while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ';')) {
			++p;
		}
		if (!*p) {
			break;
		}
		if (std::strncmp(p, "UpdateProp", 10) != 0) {
			ctx.Error(loc, "action shorthand only supports UpdateProp");
			return false;
		}
		p += 10;
		while (*p && std::isspace(static_cast<unsigned char>(*p))) {
			++p;
		}
		if (*p != '(') {
			ctx.Error(loc, "malformed UpdateProp action");
			return false;
		}
		++p;
		std::string args[3];
		int argi = 0;
		std::string cur;
		while (*p && *p != ')') {
			if (*p == ',') {
				if (argi >= 3) {
					ctx.Error(loc, "UpdateProp expects three arguments");
					return false;
				}
				/* trim */
				size_t b = 0;
				while (b < cur.size() && std::isspace(static_cast<unsigned char>(cur[b]))) {
					++b;
				}
				size_t e = cur.size();
				while (e > b && std::isspace(static_cast<unsigned char>(cur[e - 1]))) {
					--e;
				}
				args[argi++] = cur.substr(b, e - b);
				cur.clear();
				++p;
				continue;
			}
			cur.push_back(*p++);
		}
		if (*p != ')') {
			ctx.Error(loc, "malformed UpdateProp action");
			return false;
		}
		++p;
		{
			size_t b = 0;
			while (b < cur.size() && std::isspace(static_cast<unsigned char>(cur[b]))) {
				++b;
			}
			size_t e = cur.size();
			while (e > b && std::isspace(static_cast<unsigned char>(cur[e - 1]))) {
				--e;
			}
			if (argi >= 3) {
				ctx.Error(loc, "UpdateProp expects three arguments");
				return false;
			}
			args[argi++] = cur.substr(b, e - b);
		}
		if (argi != 3) {
			ctx.Error(loc, "UpdateProp expects three arguments");
			return false;
		}
		if (static_cast<int>(handler.actions.size()) >= ctx.limits->maxActionsPerHandler) {
			ctx.Error(loc, "actions per handler exceed limit");
			return false;
		}
		uid_action_t act;
		act.kind = UID_NODE_SET;
		act.target = args[0];
		act.property = args[1];
		act.value = args[2];
		act.loc = loc;
		handler.actions.push_back(act);
	}

	if (handler.actions.empty()) {
		ctx.Error(loc, "empty action attribute");
		return false;
	}
	node->handlers.push_back(handler);
	return true;
}

bool ParseOnElement(ParseContext &ctx, XMLElement *onEl, uid_node_def_t *node)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, onEl);
	const char *eventName = onEl->Attribute("event");
	if (!eventName) {
		ctx.Error(loc, "<on> requires event");
		return false;
	}
	uid_event_kind_t event;
	if (!ParseEventName(eventName, &event)) {
		ctx.Error(loc, "unsupported event name");
		return false;
	}
	for (const tinyxml2::XMLAttribute *attr = onEl->FirstAttribute(); attr; attr = attr->Next()) {
		if (std::strcmp(attr->Name(), "event") != 0) {
			ctx.Errorf(loc, std::string("unknown attribute on <on>: ") + attr->Name());
			return false;
		}
	}

	uid_action_handler_t handler;
	handler.event = event;
	handler.loc = loc;

	for (XMLElement *child = onEl->FirstChildElement(); child; child = child->NextSiblingElement()) {
		if (static_cast<int>(handler.actions.size()) >= ctx.limits->maxActionsPerHandler) {
			ctx.Error(MakeLoc(ctx.sourceName, child), "actions per handler exceed limit");
			return false;
		}
		uid_action_t act;
		act.loc = MakeLoc(ctx.sourceName, child);
		const char *tag = child->Name();
		if (std::strcmp(tag, "set") == 0) {
			act.kind = UID_NODE_SET;
			const char *target = child->Attribute("target");
			const char *property = child->Attribute("property");
			const char *value = child->Attribute("value");
			if (!target || !property || !value) {
				ctx.Error(act.loc, "<set> requires target, property, and value");
				return false;
			}
			act.target = target;
			act.property = property;
			act.value = value;
		} else if (std::strcmp(tag, "set-cvar") == 0) {
			act.kind = UID_NODE_SET_CVAR;
			const char *name = child->Attribute("name");
			const char *value = child->Attribute("value");
			if (!name || !value) {
				ctx.Error(act.loc, "<set-cvar> requires name and value");
				return false;
			}
			act.name = name;
			act.value = value;
		} else if (std::strcmp(tag, "invoke") == 0) {
			act.kind = UID_NODE_INVOKE;
			const char *name = child->Attribute("name");
			if (!name) {
				ctx.Error(act.loc, "<invoke> requires name");
				return false;
			}
			act.name = name;
		} else if (std::strcmp(tag, "show-modal") == 0) {
			act.kind = UID_NODE_SHOW_MODAL;
			const char *id = child->Attribute("id");
			if (!id || !id[0]) {
				ctx.Error(act.loc, "<show-modal> requires id");
				return false;
			}
			act.name = id;
		} else if (std::strcmp(tag, "hide-modal") == 0) {
			act.kind = UID_NODE_HIDE_MODAL;
			const char *cvar = child->Attribute("cvar");
			if (cvar && cvar[0]) {
				act.name = cvar;
			}
		} else {
			ctx.Errorf(act.loc, std::string("unknown action element: ") + tag);
			return false;
		}
		handler.actions.push_back(act);
	}

	if (handler.actions.empty()) {
		ctx.Error(loc, "<on> requires at least one action");
		return false;
	}
	node->handlers.push_back(handler);
	return true;
}

/* ---- attribute collection ---- */

struct AttrBucket {
	uid_property_set_t props;
	std::string        id;
	std::string        bind;
	bool               hasBind = false;
	uid_commit_mode_t  commit = UID_COMMIT_CHANGE;
	bool               hasCommit = false;
	std::string        actionShorthand;
	std::map<std::string, std::string> custom; /* undeclared until validated */
};

bool CollectAttrs(
	ParseContext &ctx,
	XMLElement *el,
	bool (*isBuiltin)(const std::string &),
	bool allowCustom,
	const std::set<std::string> *declaredProps,
	AttrBucket *out
)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
	for (const tinyxml2::XMLAttribute *attr = el->FirstAttribute(); attr; attr = attr->Next()) {
		std::string canonical;
		const bool aliased = UID_NormalizeAttrName(attr->Name(), &canonical);
		if (aliased) {
			ctx.Warning(loc, ("attribute alias normalized: " + std::string(attr->Name()) + " -> " + canonical).c_str());
		}

		const char *rawValue = attr->Value() ? attr->Value() : "";

		if (canonical == "id") {
			out->id = rawValue;
			if (!CheckIdLen(ctx, out->id, loc)) {
				return false;
			}
			continue;
		}
		if (canonical == "action") {
			out->actionShorthand = rawValue;
			continue;
		}
		if (canonical == "commit") {
			/* Added in OPM: defer brace/template commit until expand (like bind). */
			if (std::strchr(rawValue, '{') != nullptr) {
				out->props.Set("commit", rawValue);
				continue;
			}
			if (!ParseCommitMode(rawValue, &out->commit)) {
				ctx.Error(loc, "invalid commit mode");
				return false;
			}
			out->hasCommit = true;
			/* Fixed in OPM: template uses read commit from properties (BuildTemplatePropValues). */
			out->props.Set("commit", rawValue);
			continue;
		}
		if (canonical == "context" || (canonical == "value" && !out->hasBind && ParseCvarCompat(rawValue, &out->bind))) {
			std::string bind;
			if (ParseCvarCompat(rawValue, &bind)) {
				ctx.Warning(loc, (canonical + "=\"cvar(...)\" is compatibility syntax; prefer bind=\"cvar:name\"").c_str());
				out->bind = bind;
				out->hasBind = true;
				continue;
			}
			if (canonical == "context") {
				ctx.Error(loc, "invalid context attribute");
				return false;
			}
			/* fall through: value as custom/builtin */
		}
		if (canonical == "bind") {
			out->bind = rawValue;
			out->hasBind = true;
			out->props.Set("bind", rawValue);
			continue;
		}

		if (isBuiltin(canonical)) {
			out->props.Set(canonical.c_str(), rawValue);
			continue;
		}

		/* Known structural attrs handled by callers (template, shape, type, min, ...). */
		static const char *const kStructural[] = {
			"template", "shape", "type", "min", "max", "step", "placeholder", "max-length",
			"true-value", "false-value", "source", "appearance", "modal", "binding", "fit", "width", "height",
			"label", "halign", "valign", "gap", "overflow", "radius", "font", "font-size",
			"font-weight", "text-skew", "shape-rotation", "letter-spacing", "event", "target", "property", "name", "required", "default",
		"version", "weight", "src", "value", "value-type", "visible-if", "set-value",
		"wrap", "scroll", "index", "step-index", "set-index", "visible-if-index", "mode", "row-height",
		"lifetime", "fade-duration",
		"border", "border-top", "border-right", "border-bottom", "border-left"
		};
		bool structural = false;
		for (const char *s : kStructural) {
			if (canonical == s) {
				structural = true;
				break;
			}
		}
		if (structural) {
			/* Leave for element-specific handling; also stash in props when useful. */
			out->props.Set(canonical.c_str(), rawValue);
			continue;
		}

		if (allowCustom) {
			if (declaredProps) {
				if (declaredProps->find(canonical) == declaredProps->end()) {
					ctx.Errorf(loc, "undeclared property attribute: " + canonical);
					return false;
				}
			}
			out->custom[canonical] = rawValue;
			out->props.Set(canonical.c_str(), rawValue);
			continue;
		}

		ctx.Errorf(loc, "unknown attribute: " + canonical);
		return false;
	}
	return true;
}

/*
 * When an element has shape="<id>", allow that definition's <props>
 * (e.g. chamfer, radius) as instance attributes — same as <shape> instances.
 * Without this, <button shape="chamfer-secondary" chamfer="24px"> fails parse
 * and the whole design refuses to load.
 */
bool CollectAttrsForStyledBox(
	ParseContext &ctx,
	XMLElement *el,
	bool (*isBuiltin)(const std::string &),
	AttrBucket *out
)
{
	std::set<std::string> declared;
	bool                  haveDeclared = false;
	const char           *shapeId = el->Attribute("shape");
	if (shapeId && shapeId[0] && ctx.doc) {
		auto sit = ctx.doc->definitions.shapes.find(shapeId);
		if (sit != ctx.doc->definitions.shapes.end()) {
			for (const uid_prop_decl_t &p : sit->second.props) {
				declared.insert(p.name);
			}
			haveDeclared = !declared.empty();
		}
	}
	return CollectAttrs(
		ctx,
		el,
		isBuiltin,
		haveDeclared,
		haveDeclared ? &declared : nullptr,
		out
	);
}

void ApplyCommonControlFields(AttrBucket &attrs, uid_node_def_t *node)
{
	if (!attrs.id.empty()) {
		node->id = attrs.id;
	}
	node->properties = attrs.props;
	if (attrs.hasBind) {
		node->bind = attrs.bind;
		node->properties.Set("bind", attrs.bind.c_str());
	}
	if (attrs.hasCommit) {
		node->commit = attrs.commit;
		node->hasCommit = true;
	}
	/* Added in OPM: promote extended control attrs onto the node. */
	std::string tmp;
	if (node->properties.Get("value-type", &tmp)) {
		node->valueType = (tmp == "none") ? std::string() : tmp;
	}
	if (node->properties.Get("visible-if", &tmp)) {
		node->visibleIf = tmp;
		if (node->visibleExpr.empty()) {
			UID_VisibleIfToBoolExpr(tmp, &node->visibleExpr);
		}
	}
	if (node->properties.Get("enabled-if", &tmp)) {
		node->enabledIf = tmp;
		if (node->enabledExpr.empty()) {
			UID_VisibleIfToBoolExpr(tmp, &node->enabledExpr);
		}
	}
	if (node->properties.Get("visible", &tmp)) {
		std::string inner;
		if (UID_ParseBraceBoolExpr(tmp.c_str(), &inner)) {
			node->visibleExpr = inner;
			node->visibleExprBound = true;
		}
	}
	if (node->properties.Get("enabled", &tmp)) {
		std::string inner;
		if (UID_ParseBraceBoolExpr(tmp.c_str(), &inner)) {
			node->enabledExpr = inner;
			node->enabledExprBound = true;
		}
	}
	/* Added in OPM: style ternaries on any property (except visible/enabled bool exprs). */
	node->styleExprs.clear();
	for (const auto &kv : node->properties.Attrs()) {
		if (kv.first == "visible" || kv.first == "enabled") {
			continue;
		}
		std::string inner;
		if (UID_ParseBraceBoolExpr(kv.second.value.c_str(), &inner) &&
		    inner.find('?') != std::string::npos) {
			node->styleExprs[kv.first] = inner;
		}
	}
	if (node->properties.Get("set-value", &tmp)) {
		node->setValue = tmp;
	}
}

void ApplyScrollbarFields(uid_node_def_t *node)
{
	if (!node) {
		return;
	}
	std::string tmp;
	if (node->properties.Get("scrollbar", &tmp) && !tmp.empty()) {
		node->scrollbarTemplateId = tmp;
	}
}

bool IsScrollbarAttr(const std::string &name)
{
	return IsCommonBoxAttr(name) || name == "axis";
}

/* Forward decls */
bool ParseRenderable(
	ParseContext &ctx,
	XMLElement *el,
	int depth,
	std::vector<uid_node_def_t> *outNodes,
	uid_node_id_t *outId,
	bool insideTemplate
);
bool ParseSliderPart(
	ParseContext &ctx,
	XMLElement *el,
	int depth,
	std::vector<uid_node_def_t> *outNodes,
	uid_node_id_t *outId
);
bool ParseScrollbarPart(
	ParseContext &ctx,
	XMLElement *el,
	int depth,
	std::vector<uid_node_def_t> *outNodes,
	uid_node_id_t *outId,
	bool insideTemplate
);
uid_node_kind_t SliderPartKindFromTag(const char *tag);
uid_node_kind_t ScrollbarPartKindFromTag(const char *tag);

bool ParseChildren(
	ParseContext &ctx,
	XMLElement *parent,
	int depth,
	uid_node_id_t parentId,
	std::vector<uid_node_def_t> *outNodes,
	bool allowTextAsLabel,
	bool allowOptions,
	bool allowOn,
	bool insideTemplate
)
{
	auto parentNode = [&]() -> uid_node_def_t & {
		return (*outNodes)[static_cast<size_t>(parentId)];
	};

	std::string accumulatedText;
	for (XMLNode *child = parent->FirstChild(); child; child = child->NextSibling()) {
		if (const XMLText *text = child->ToText()) {
			const char *v = text->Value();
			if (!IsWhitespaceOnly(v)) {
				if (!accumulatedText.empty()) {
					accumulatedText.push_back(' ');
				}
				accumulatedText += TrimText(v);
			}
			continue;
		}
		XMLElement *el = child->ToElement();
		if (!el) {
			continue;
		}
		const char *tag = el->Name();
		if (allowOn && std::strcmp(tag, "on") == 0) {
			/* Flush pending text before handlers. */
			if (!accumulatedText.empty()) {
				if (!CheckTextLen(ctx, accumulatedText, MakeLoc(ctx.sourceName, parent))) {
					return false;
				}
				if (allowTextAsLabel && parentNode().kind == UID_NODE_CONTAINER) {
					if (!ConsumeNodeBudget(ctx, MakeLoc(ctx.sourceName, parent))) {
						return false;
					}
					uid_node_def_t label;
					UID_InitNodeDef(&label);
					label.kind = UID_NODE_LABEL;
					label.source = MakeLoc(ctx.sourceName, parent);
					label.text = accumulatedText;
					const uid_node_id_t lid = static_cast<uid_node_id_t>(outNodes->size());
					outNodes->push_back(label);
					parentNode().children.push_back(lid);
				} else {
					parentNode().text = accumulatedText;
				}
				accumulatedText.clear();
			}
			if (!ParseOnElement(ctx, el, &parentNode())) {
				return false;
			}
			continue;
		}
		if (allowOptions && std::strcmp(tag, "option") == 0) {
			if (static_cast<int>(parentNode().options.size()) >= ctx.limits->maxOptionsPerSelect) {
				ctx.Error(MakeLoc(ctx.sourceName, el), "options exceed maxOptionsPerSelect");
				return false;
			}
			const char *value = el->Attribute("value");
			if (!value) {
				ctx.Error(MakeLoc(ctx.sourceName, el), "<option> requires value");
				return false;
			}
			uid_select_option_t opt;
			opt.value = value;
			opt.label = TrimText(el->GetText());
			parentNode().options.push_back(opt);
			continue;
		}

		/* Added in OPM: slider chrome parts (track / range / thumb). */
		if (parentNode().kind == UID_NODE_SLIDER && UID_IsSliderPartKind(SliderPartKindFromTag(tag))) {
			const uid_node_kind_t partKind = SliderPartKindFromTag(tag);
			for (uid_node_id_t existing : parentNode().children) {
				if (existing < 0 || static_cast<size_t>(existing) >= outNodes->size()) {
					continue;
				}
				if ((*outNodes)[static_cast<size_t>(existing)].kind == partKind) {
					ctx.Errorf(MakeLoc(ctx.sourceName, el), std::string("duplicate slider part: ") + tag);
					return false;
				}
			}
			uid_node_id_t partId = UID_INVALID_NODE_ID;
			if (!ParseSliderPart(ctx, el, depth + 1, outNodes, &partId)) {
				return false;
			}
			parentNode().children.push_back(partId);
			continue;
		}

		/* Added in OPM: scrollbar chrome parts (track / thumb). */
		if (parentNode().kind == UID_NODE_SCROLLBAR && UID_IsScrollbarPartKind(ScrollbarPartKindFromTag(tag))) {
			const uid_node_kind_t partKind = ScrollbarPartKindFromTag(tag);
			for (uid_node_id_t existing : parentNode().children) {
				if (existing < 0 || static_cast<size_t>(existing) >= outNodes->size()) {
					continue;
				}
				if ((*outNodes)[static_cast<size_t>(existing)].kind == partKind) {
					ctx.Errorf(MakeLoc(ctx.sourceName, el), std::string("duplicate scrollbar part: ") + tag);
					return false;
				}
			}
			uid_node_id_t partId = UID_INVALID_NODE_ID;
			if (!ParseScrollbarPart(ctx, el, depth + 1, outNodes, &partId, insideTemplate)) {
				return false;
			}
			parentNode().children.push_back(partId);
			continue;
		}

		/* Flush text as implicit label before next element when in container. */
		if (!accumulatedText.empty() && allowTextAsLabel && parentNode().kind == UID_NODE_CONTAINER) {
			if (!CheckTextLen(ctx, accumulatedText, MakeLoc(ctx.sourceName, parent))) {
				return false;
			}
			if (!ConsumeNodeBudget(ctx, MakeLoc(ctx.sourceName, parent))) {
				return false;
			}
			uid_node_def_t label;
			UID_InitNodeDef(&label);
			label.kind = UID_NODE_LABEL;
			label.source = MakeLoc(ctx.sourceName, parent);
			label.text = accumulatedText;
			const uid_node_id_t lid = static_cast<uid_node_id_t>(outNodes->size());
			outNodes->push_back(label);
			parentNode().children.push_back(lid);
			accumulatedText.clear();
		} else if (!accumulatedText.empty() && !allowTextAsLabel) {
			if (!CheckTextLen(ctx, accumulatedText, MakeLoc(ctx.sourceName, parent))) {
				return false;
			}
			parentNode().text = accumulatedText;
			accumulatedText.clear();
		}

		uid_node_id_t childId = UID_INVALID_NODE_ID;
		if (!ParseRenderable(ctx, el, depth + 1, outNodes, &childId, insideTemplate)) {
			return false;
		}
		parentNode().children.push_back(childId);
	}

	if (!accumulatedText.empty()) {
		if (!CheckTextLen(ctx, accumulatedText, MakeLoc(ctx.sourceName, parent))) {
			return false;
		}
		if (allowTextAsLabel && parentNode().kind == UID_NODE_CONTAINER) {
			if (!ConsumeNodeBudget(ctx, MakeLoc(ctx.sourceName, parent))) {
				return false;
			}
			uid_node_def_t label;
			UID_InitNodeDef(&label);
			label.kind = UID_NODE_LABEL;
			label.source = MakeLoc(ctx.sourceName, parent);
			label.text = accumulatedText;
			const uid_node_id_t lid = static_cast<uid_node_id_t>(outNodes->size());
			outNodes->push_back(label);
			parentNode().children.push_back(lid);
		} else {
			parentNode().text = accumulatedText;
		}
	}
	return true;
}

bool ValidateSliderBounds(ParseContext &ctx, uid_node_def_t *node, const uid_source_location_t &loc)
{
	if (!UID_SyncSliderBounds(node)) {
		ctx.Error(loc, "<slider> requires valid min, max, and step");
		return false;
	}
	return true;
}

bool SliderBoundsDeferred(const uid_node_def_t &node)
{
	std::string v;
	if (node.properties.Get("min", &v) && v.find('{') != std::string::npos) {
		return true;
	}
	if (node.properties.Get("max", &v) && v.find('{') != std::string::npos) {
		return true;
	}
	if (node.properties.Get("step", &v) && v.find('{') != std::string::npos) {
		return true;
	}
	return false;
}

uid_node_kind_t SliderPartKindFromTag(const char *tag)
{
	if (!tag) {
		return UID_NODE_CONTAINER;
	}
	if (std::strcmp(tag, "track") == 0) {
		return UID_NODE_SLIDER_TRACK;
	}
	if (std::strcmp(tag, "range") == 0) {
		return UID_NODE_SLIDER_RANGE;
	}
	if (std::strcmp(tag, "thumb") == 0) {
		return UID_NODE_SLIDER_THUMB;
	}
	return UID_NODE_CONTAINER;
}

bool ParseSliderPart(
	ParseContext &ctx,
	XMLElement *el,
	int depth,
	std::vector<uid_node_def_t> *outNodes,
	uid_node_id_t *outId
)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
	if (!CheckDepth(ctx, depth, loc)) {
		return false;
	}
	if (!ConsumeNodeBudget(ctx, loc)) {
		return false;
	}

	const uid_node_kind_t kind = SliderPartKindFromTag(el->Name());
	if (!UID_IsSliderPartKind(kind)) {
		ctx.Errorf(loc, std::string("unknown slider part: ") + el->Name());
		return false;
	}

	uid_node_def_t node;
	UID_InitNodeDef(&node);
	node.kind = kind;
	node.source = loc;

	AttrBucket attrs;
	if (!CollectAttrs(ctx, el, IsSliderPartAttr, false, nullptr, &attrs)) {
		return false;
	}
	ApplyCommonControlFields(attrs, &node);

	for (XMLElement *child = el->FirstChildElement(); child; child = child->NextSiblingElement()) {
		ctx.Errorf(MakeLoc(ctx.sourceName, child), std::string("unexpected child in slider part: ") + child->Name());
		return false;
	}

	const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
	outNodes->push_back(node);
	*outId = self;
	return !ctx.failed;
}

uid_node_kind_t ScrollbarPartKindFromTag(const char *tag)
{
	if (!tag) {
		return UID_NODE_CONTAINER;
	}
	if (std::strcmp(tag, "track") == 0) {
		return UID_NODE_SCROLLBAR_TRACK;
	}
	if (std::strcmp(tag, "thumb") == 0) {
		return UID_NODE_SCROLLBAR_THUMB;
	}
	return UID_NODE_CONTAINER;
}

bool ParseScrollbarPart(
	ParseContext &ctx,
	XMLElement *el,
	int depth,
	std::vector<uid_node_def_t> *outNodes,
	uid_node_id_t *outId,
	bool insideTemplate
)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
	if (!CheckDepth(ctx, depth, loc)) {
		return false;
	}
	if (!ConsumeNodeBudget(ctx, loc)) {
		return false;
	}

	const uid_node_kind_t kind = ScrollbarPartKindFromTag(el->Name());
	if (!UID_IsScrollbarPartKind(kind)) {
		ctx.Errorf(loc, std::string("unknown scrollbar part: ") + el->Name());
		return false;
	}

	uid_node_def_t node;
	UID_InitNodeDef(&node);
	node.kind = kind;
	node.source = loc;

	AttrBucket attrs;
	if (!CollectAttrs(ctx, el, IsSliderPartAttr, false, nullptr, &attrs)) {
		return false;
	}
	ApplyCommonControlFields(attrs, &node);

	const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
	outNodes->push_back(node);
	if (!ParseChildren(ctx, el, depth, self, outNodes, true, false, false, insideTemplate)) {
		return false;
	}
	*outId = self;
	return !ctx.failed;
}

bool ParseRenderable(
	ParseContext &ctx,
	XMLElement *el,
	int depth,
	std::vector<uid_node_def_t> *outNodes,
	uid_node_id_t *outId,
	bool insideTemplate
)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
	if (!CheckDepth(ctx, depth, loc)) {
		return false;
	}
	if (!ConsumeNodeBudget(ctx, loc)) {
		return false;
	}

	const char *tag = el->Name();
	uid_node_def_t node;
	UID_InitNodeDef(&node);
	node.source = loc;

	/* Compatibility: <template template="id"> as <use>. */
	bool compatUse = false;
	if (std::strcmp(tag, "template") == 0 && el->Attribute("template")) {
		compatUse = true;
		ctx.Warning(loc, "<template template=\"...\"> is compatibility syntax; prefer <use template=\"...\">");
		tag = "use";
	}

	if (std::strcmp(tag, "scrollbar") == 0) {
		node.kind = UID_NODE_SCROLLBAR;
		AttrBucket attrs;
		if (!CollectAttrsForStyledBox(ctx, el, IsScrollbarAttr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);
		const char *axis = el->Attribute("axis");
		if (axis && axis[0]) {
			node.properties.Set("axis", axis);
		} else {
			node.properties.Set("axis", "vertical");
		}
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		if (!ParseChildren(ctx, el, depth, self, outNodes, false, false, false, insideTemplate)) {
			return false;
		}
		*outId = self;
		return true;
	}

	if (std::strcmp(tag, "container") == 0) {
		node.kind = UID_NODE_CONTAINER;
		AttrBucket attrs;
		if (!CollectAttrsForStyledBox(ctx, el, IsContainerAttr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);
		{
			const char *role = el->Attribute("role");
			if (role && role[0]) {
				node.role = role;
				node.properties.Set("role", role);
			}
		}
		::UID_ApplyCollectionAndIndexFields(&node);
		ApplyScrollbarFields(&node);
		if (!attrs.actionShorthand.empty()) {
			ctx.Error(loc, "action attribute is not valid on <container>");
			return false;
		}
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		if (!ParseChildren(ctx, el, depth, self, outNodes, true, false, true, insideTemplate)) {
			return false;
		}
		*outId = self;
		return true;
	}

	if (std::strcmp(tag, "label") == 0) {
		node.kind = UID_NODE_LABEL;
		AttrBucket attrs;
		if (!CollectAttrsForStyledBox(ctx, el, IsCommonBoxAttr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		if (!ParseChildren(ctx, el, depth, self, outNodes, false, false, false, insideTemplate)) {
			return false;
		}
		*outId = self;
		return true;
	}

	if (std::strcmp(tag, "button") == 0) {
		node.kind = UID_NODE_BUTTON;
		AttrBucket attrs;
		if (!CollectAttrsForStyledBox(ctx, el, IsCommonControlAttr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);
		::UID_ApplyCollectionAndIndexFields(&node);
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		if (!attrs.actionShorthand.empty()) {
			if (!ParseUpdatePropShorthand(ctx, attrs.actionShorthand.c_str(), &(*outNodes)[static_cast<size_t>(self)], loc)) {
				return false;
			}
		}
		if (!ParseChildren(ctx, el, depth, self, outNodes, false, false, true, insideTemplate)) {
			return false;
		}
		*outId = self;
		return true;
	}

	if (std::strcmp(tag, "foreach") == 0) {
		node.kind = UID_NODE_FOREACH;
		AttrBucket attrs;
		if (!CollectAttrs(ctx, el, IsForeachAttr, false, nullptr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);
		/* Added in OPM: allow source= on <foreach> (same as parent container scope). */
		::UID_ApplyCollectionAndIndexFields(&node);
		std::string tmp;
		if (node.properties.Get("mode", &tmp) && !tmp.empty()) {
			node.foreachMode = tmp;
		}
		const char *countAttr = el->Attribute("count");
		if (countAttr && countAttr[0]) {
			node.hasForeachCount = true;
			node.foreachCountExpr = countAttr;
		}
		const char *rowHeight = el->Attribute("row-height");
		if (rowHeight && rowHeight[0]) {
			uid_length_t len;
			if (!UID_ParseLength(rowHeight, &len, nullptr) || len.unit != UID_LENGTH_PX) {
				ctx.Error(loc, "<foreach> row-height must be a px length");
				return false;
			}
			node.foreachRowHeight = len.value;
			node.hasForeachRowHeight = true;
		}
		const char *lifetimeAttr = el->Attribute("lifetime");
		const char *fadeAttr = el->Attribute("fade-duration");
		if (fadeAttr && fadeAttr[0] && !(lifetimeAttr && lifetimeAttr[0])) {
			ctx.Error(loc, "<foreach> fade-duration requires lifetime");
			return false;
		}
		if (lifetimeAttr && lifetimeAttr[0]) {
			int lifetimeMs = 0;
			if (!UID_ParseDurationMs(lifetimeAttr, &lifetimeMs, nullptr)) {
				ctx.Error(loc, "<foreach> lifetime must be a duration (e.g. 5s, 500ms)");
				return false;
			}
			int fadeMs = 0;
			if (fadeAttr && fadeAttr[0]) {
				if (!UID_ParseDurationMs(fadeAttr, &fadeMs, nullptr)) {
					ctx.Error(loc, "<foreach> fade-duration must be a duration (e.g. 1.5s, 500ms)");
					return false;
				}
			}
			if (fadeMs > lifetimeMs) {
				fadeMs = lifetimeMs;
			}
			node.hasForeachLifetime = true;
			node.foreachLifetimeMs = lifetimeMs;
			node.foreachFadeDurationMs = fadeMs;
		}
		uid_node_def_t wrap;
		UID_InitNodeDef(&wrap);
		wrap.kind = UID_NODE_CONTAINER;
		wrap.source = loc;
		node.foreachTemplateNodes.push_back(wrap);
		const uid_node_id_t wrapId = 0;
		if (!ParseChildren(ctx, el, depth, wrapId, &node.foreachTemplateNodes, true, false, true, insideTemplate)) {
			return false;
		}
		if (node.foreachTemplateNodes[0].children.empty()) {
			ctx.Error(loc, "<foreach> requires at least one child template");
			return false;
		}
		node.foreachTemplateRoot = wrapId;
		CopyForeachTemplateLayout(node, &node.foreachTemplateNodes[0]);
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		*outId = self;
		return true;
	}

	if (std::strcmp(tag, "input") == 0) {
		node.kind = UID_NODE_INPUT;
		AttrBucket attrs;
		if (!CollectAttrsForStyledBox(ctx, el, IsCommonControlAttr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);
		const char *type = el->Attribute("type");
		if (type) {
			if (std::strcmp(type, "text") != 0 && std::strcmp(type, "number") != 0) {
				ctx.Error(loc, "input type must be text or number");
				return false;
			}
			node.inputType = type;
		} else {
			node.inputType = "text";
		}
		node.properties.Set("type", node.inputType.c_str());
		/* min/max/step already stashed in properties by CollectAttrs; allow {template.*} like <slider>. */
		if (SliderBoundsDeferred(node)) {
			if (!insideTemplate) {
				ctx.Error(loc, "<input> min/max/step expressions only allowed inside templates");
				return false;
			}
		} else if (!UID_SyncInputBounds(&node)) {
			ctx.Error(loc, "invalid input min/max/step");
			return false;
		}
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		if (!ParseChildren(ctx, el, depth, self, outNodes, false, false, true, insideTemplate)) {
			return false;
		}
		*outId = self;
		return true;
	}

	if (std::strcmp(tag, "toggle") == 0) {
		node.kind = UID_NODE_TOGGLE;
		AttrBucket attrs;
		if (!CollectAttrsForStyledBox(ctx, el, IsCommonControlAttr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		if (!ParseChildren(ctx, el, depth, self, outNodes, false, false, true, insideTemplate)) {
			return false;
		}
		*outId = self;
		return true;
	}

	if (std::strcmp(tag, "slider") == 0) {
		node.kind = UID_NODE_SLIDER;
		AttrBucket attrs;
		if (!CollectAttrsForStyledBox(ctx, el, IsCommonControlAttr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);
		const char *minS = el->Attribute("min");
		const char *maxS = el->Attribute("max");
		const char *stepS = el->Attribute("step");
		if (minS) {
			node.properties.Set("min", minS);
		}
		if (maxS) {
			node.properties.Set("max", maxS);
		}
		if (stepS) {
			node.properties.Set("step", stepS);
		}
		if (SliderBoundsDeferred(node)) {
			if (!insideTemplate) {
				ctx.Error(loc, "<slider> min/max/step expressions only allowed inside templates");
				return false;
			}
		} else if (!ValidateSliderBounds(ctx, &node, loc)) {
			return false;
		}
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		if (!ParseChildren(ctx, el, depth, self, outNodes, false, false, true, insideTemplate)) {
			return false;
		}
		*outId = self;
		return true;
	}

	if (std::strcmp(tag, "select") == 0) {
		node.kind = UID_NODE_SELECT;
		AttrBucket attrs;
		if (!CollectAttrsForStyledBox(ctx, el, IsCommonControlAttr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);
		const char *source = el->Attribute("source");
		if (source) {
			node.optionSource = source;
		}
		/* Added in OPM: appearance=cyclic|dropdown (default dropdown overlay). */
		const char *appearance = el->Attribute("appearance");
		if (appearance && appearance[0]) {
			if (std::strcmp(appearance, "cyclic") != 0 && std::strcmp(appearance, "dropdown") != 0) {
				ctx.Error(loc, "<select> appearance must be cyclic or dropdown");
				return false;
			}
			node.appearance = appearance;
		}
		/* Added in OPM: modal= opens a definitions modal instead of procedural overlay. */
		const char *modalAttr = el->Attribute("modal");
		if (modalAttr && modalAttr[0]) {
			node.openModal = modalAttr;
		}
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		if (!ParseChildren(ctx, el, depth, self, outNodes, false, true, true, insideTemplate)) {
			return false;
		}
		uid_node_def_t &stored = (*outNodes)[static_cast<size_t>(self)];
		if (!stored.optionSource.empty() && !stored.options.empty()) {
			ctx.Error(loc, "<select> cannot combine source with static options");
			return false;
		}
		/* Added in OPM: allow option-less select inside templates (options/source filled at use/expand). */
		if (!insideTemplate && stored.optionSource.empty() && stored.options.empty()) {
			ctx.Error(loc, "<select> requires source or options");
			return false;
		}
		*outId = self;
		return true;
	}

	if (std::strcmp(tag, "keybind") == 0) {
		node.kind = UID_NODE_KEYBIND;
		AttrBucket attrs;
		if (!CollectAttrsForStyledBox(ctx, el, IsCommonControlAttr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);
		const char *binding = el->Attribute("binding");
		if (!binding || !binding[0]) {
			ctx.Error(loc, "<keybind> requires binding");
			return false;
		}
		node.binding = binding;
		const char *slotAttr = el->Attribute("slot");
		if (slotAttr && std::strcmp(slotAttr, "secondary") == 0) {
			node.bindSlot = 1;
		}
		const char *confirmModal = el->Attribute("confirm-modal");
		if (confirmModal && confirmModal[0]) {
			node.confirmModal = confirmModal;
		}
		const char *modalCvar = el->Attribute("modal-cvar");
		if (modalCvar && modalCvar[0]) {
			node.modalCvar = modalCvar;
		}
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		if (!ParseChildren(ctx, el, depth, self, outNodes, false, false, true, insideTemplate)) {
			return false;
		}
		*outId = self;
		return true;
	}

	if (std::strcmp(tag, "shape") == 0) {
		node.kind = UID_NODE_SHAPE_INSTANCE;
		const char *shapeId = el->Attribute("shape");
		if (!shapeId || !shapeId[0]) {
			ctx.Error(loc, "<shape> instance requires shape=\"<definition-id>\"");
			return false;
		}
		node.shapeId = shapeId;
		std::set<std::string> declared;
		auto sit = ctx.doc->definitions.shapes.find(shapeId);
		if (sit == ctx.doc->definitions.shapes.end()) {
			if (!insideTemplate) {
				ctx.Errorf(loc, std::string("unknown shape definition: ") + shapeId);
				return false;
			}
		} else {
			for (const uid_prop_decl_t &p : sit->second.props) {
				declared.insert(p.name);
			}
		}
		AttrBucket attrs;
		if (!CollectAttrs(ctx, el, IsCommonBoxAttr, true, sit != ctx.doc->definitions.shapes.end() ? &declared : nullptr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);
		node.properties.Set("shape", shapeId);
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		/* Shape instances should not have element children. */
		for (XMLElement *child = el->FirstChildElement(); child; child = child->NextSiblingElement()) {
			ctx.Errorf(MakeLoc(ctx.sourceName, child), std::string("unexpected child in shape instance: ") + child->Name());
			return false;
		}
		*outId = self;
		return !ctx.failed;
	}

	/* Added in OPM: leaf bitmap with intrinsic size (not definitions <images>). */
	if (std::strcmp(tag, "image") == 0) {
		node.kind = UID_NODE_IMAGE;
		AttrBucket attrs;
		if (!CollectAttrs(ctx, el, IsImageAttr, false, nullptr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);

		std::string src;
		std::string fit;
		std::string scale;
		(void)attrs.props.Get("src", &src);
		if (src.empty()) {
			(void)attrs.props.Get("background-image", &src);
		}
		(void)attrs.props.Get("fit", &fit);
		if (fit.empty()) {
			(void)attrs.props.Get("background-fit", &fit);
		}
		(void)attrs.props.Get("scale", &scale);
		if (scale.empty()) {
			(void)attrs.props.Get("background-scale", &scale);
		}

		if (src.empty()) {
			ctx.Error(loc, "<image> requires src (or background-image)");
			return false;
		}
		if (fit.empty()) {
			fit = "contain";
		}

		node.properties.Set("src", src.c_str());
		node.properties.Set("fit", fit.c_str());
		if (!scale.empty()) {
			node.properties.Set("scale", scale.c_str());
		}
		/* Keep background-* mirrors so shared paint helpers can read either. */
		node.properties.Set("background-image", src.c_str());
		node.properties.Set("background-fit", fit.c_str());
		if (!scale.empty()) {
			node.properties.Set("background-scale", scale.c_str());
		}

		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		for (XMLElement *child = el->FirstChildElement(); child; child = child->NextSiblingElement()) {
			ctx.Errorf(MakeLoc(ctx.sourceName, child), std::string("unexpected child in <image>: ") + child->Name());
			return false;
		}
		*outId = self;
		return !ctx.failed;
	}

	/* Added in OPM: player model preview leaf */
	if (std::strcmp(tag, "model") == 0) {
		node.kind = UID_NODE_MODEL;
		AttrBucket attrs;
		if (!CollectAttrs(ctx, el, IsModelAttr, false, nullptr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);
		const char *team = el->Attribute("team");
		if (team && team[0]) {
			if (std::strcmp(team, "allies") != 0 && std::strcmp(team, "axis") != 0) {
				ctx.Error(loc, "<model> team must be allies or axis");
				return false;
			}
			node.team = team;
			node.properties.Set("team", team);
		}
		const char *modelPath = el->Attribute("model");
		if (modelPath && modelPath[0]) {
			node.modelPath = modelPath;
			node.properties.Set("model", modelPath);
		}
		const char *anim = el->Attribute("anim");
		if (anim && anim[0]) {
			node.anim = anim;
			node.properties.Set("anim", anim);
		}
		const char *animVariant = el->Attribute("anim-variant");
		if (animVariant && animVariant[0]) {
			char *end = nullptr;
			const long variant = std::strtol(animVariant, &end, 10);
			if (!end || end == animVariant || *end != '\0' || variant < 0) {
				ctx.Error(loc, "<model> anim-variant must be a non-negative integer");
				return false;
			}
			node.animVariant = static_cast<int>(variant);
			node.hasAnimVariant = true;
			node.properties.Set("anim-variant", animVariant);
		}
		const char *animPhase = el->Attribute("anim-phase");
		if (animPhase && animPhase[0]) {
			char *end = nullptr;
			const float phase = std::strtof(animPhase, &end);
			if (!end || end == animPhase || *end != '\0') {
				ctx.Error(loc, "<model> anim-phase must be a number in [0,1)");
				return false;
			}
			node.animPhase = phase;
			node.hasAnimPhase = true;
			node.properties.Set("anim-phase", animPhase);
		}
		const char *angles = el->Attribute("angles");
		if (angles && angles[0]) {
			if (!ParseModelVec3Attr(angles, node.modelAngles, ctx, loc, "angles")) {
				return false;
			}
			node.hasModelAngles = true;
			node.properties.Set("angles", angles);
		}
		const char *fov = el->Attribute("fov");
		if (fov && fov[0]) {
			if (!ParseModelFloatAttr(fov, &node.modelFov, ctx, loc, "fov")) {
				return false;
			}
			node.hasModelFov = true;
			node.properties.Set("fov", fov);
		}
		const char *scale = el->Attribute("scale");
		if (scale && scale[0]) {
			if (!ParseModelFloatAttr(scale, &node.modelScale, ctx, loc, "scale")) {
				return false;
			}
			node.hasModelScale = true;
			node.properties.Set("scale", scale);
		}
		const char *offset = el->Attribute("offset");
		if (offset && offset[0]) {
			if (!ParseModelVec3Attr(offset, node.modelOffset, ctx, loc, "offset")) {
				return false;
			}
			node.hasModelOffset = true;
			node.properties.Set("offset", offset);
		}
		const char *bbox = el->Attribute("bbox");
		if (bbox && bbox[0]) {
			if (!ParseModelBboxAttr(bbox, node.bboxMins, node.bboxMaxs, ctx, loc)) {
				return false;
			}
			node.hasBbox = true;
			node.properties.Set("bbox", bbox);
		}
		const char *bboxFromModel = el->Attribute("bbox-from-model");
		if (bboxFromModel && bboxFromModel[0]) {
			if (std::strcmp(bboxFromModel, "1") == 0 || std::strcmp(bboxFromModel, "true") == 0) {
				node.bboxFromModel = true;
			} else if (std::strcmp(bboxFromModel, "0") == 0 || std::strcmp(bboxFromModel, "false") == 0) {
				node.bboxFromModel = false;
			} else {
				ctx.Error(loc, "<model> bbox-from-model must be 0, 1, true, or false");
				return false;
			}
			node.properties.Set("bbox-from-model", bboxFromModel);
		}
		const char *framingScale = el->Attribute("framing-scale");
		if (framingScale && framingScale[0]) {
			if (!ParseModelFloatAttr(framingScale, &node.framingScale, ctx, loc, "framing-scale")) {
				return false;
			}
			node.hasFramingScale = true;
			node.properties.Set("framing-scale", framingScale);
		}
		const char *color = el->Attribute("color");
		if (color && color[0]) {
			uid_color_t parsed {};
			if (!UID_ParseColor(color, &parsed, nullptr)) {
				ctx.Error(loc, "<model> color must be #RRGGBB or #RRGGBBAA");
				return false;
			}
			node.modelColor[0] = parsed.r;
			node.modelColor[1] = parsed.g;
			node.modelColor[2] = parsed.b;
			node.modelColor[3] = parsed.a;
			node.hasModelColor = true;
			node.properties.Set("color", color);
		}
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		for (XMLElement *child = el->FirstChildElement(); child; child = child->NextSiblingElement()) {
			ctx.Errorf(MakeLoc(ctx.sourceName, child), std::string("unexpected child in <model>: ") + child->Name());
			return false;
		}
		*outId = self;
		return !ctx.failed;
	}

	/* Added in OPM: host-drawn server browser region */
	if (std::strcmp(tag, "server-list") == 0) {
		node.kind = UID_NODE_SERVER_LIST;
		AttrBucket attrs;
		if (!CollectAttrs(ctx, el, IsServerListAttr, false, nullptr, &attrs)) {
			return false;
		}
		ApplyCommonControlFields(attrs, &node);
		const char *role = el->Attribute("role");
		node.role = (role && role[0]) ? role : "server-list";
		node.properties.Set("role", node.role.c_str());
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		for (XMLElement *child = el->FirstChildElement(); child; child = child->NextSiblingElement()) {
			ctx.Errorf(
				MakeLoc(ctx.sourceName, child),
				std::string("unexpected child in <server-list>: ") + child->Name()
			);
			return false;
		}
		*outId = self;
		return !ctx.failed;
	}

	if (std::strcmp(tag, "use") == 0 || compatUse) {
		node.kind = UID_NODE_USE;
		const char *templateId = el->Attribute("template");
		if (!templateId || !templateId[0]) {
			ctx.Error(loc, "<use> requires template=\"<definition-id>\"");
			return false;
		}
		node.templateId = templateId;
		if (std::strchr(templateId, '{') != nullptr) {
			node.deferredUse = true;
		}
		std::set<std::string> declared;
		auto tit = ctx.doc->definitions.templates.find(templateId);
		if (tit == ctx.doc->definitions.templates.end()) {
			/* Template may be forward-referenced only if still parsing templates — canvas must resolve. */
			if (!insideTemplate && !node.deferredUse) {
				ctx.Errorf(loc, std::string("unknown template definition: ") + templateId);
				return false;
			}
		} else {
			for (const uid_prop_decl_t &p : tit->second.props) {
				declared.insert(p.name);
			}
		}
		AttrBucket attrs;
		const bool haveDecls = tit != ctx.doc->definitions.templates.end();
		if (!CollectAttrs(ctx, el, IsCommonBoxAttr, true, haveDecls ? &declared : nullptr, &attrs)) {
			return false;
		}
		/* Also allow declared prop names even if CollectAttrs structural set absorbed some. */
		ApplyCommonControlFields(attrs, &node);
		node.properties.Set("template", templateId);
		if (haveDecls) {
			for (const uid_prop_decl_t &p : tit->second.props) {
				if (p.required && !node.properties.Has(p.name.c_str())) {
					ctx.Errorf(loc, "missing required template prop: " + p.name);
					return false;
				}
			}
			/* Reject undeclared custom attrs already handled; also reject attrs that are neither builtin nor declared. */
			for (const auto &kv : node.properties.Attrs()) {
				const std::string &n = kv.first;
				if (n == "template" || IsCommonBoxAttr(n) || IsCommonControlAttr(n)) {
					continue;
				}
				if (n == "type" || n == "halign" || n == "valign" || n == "gap" || n == "overflow") {
					continue;
				}
				if (declared.find(n) == declared.end()) {
					ctx.Errorf(loc, "undeclared template property: " + n);
					return false;
				}
			}
		}
		const uid_node_id_t self = static_cast<uid_node_id_t>(outNodes->size());
		outNodes->push_back(node);
		for (XMLElement *child = el->FirstChildElement(); child; child = child->NextSiblingElement()) {
			ctx.Errorf(MakeLoc(ctx.sourceName, child), std::string("unexpected child in <use>: ") + child->Name());
			return false;
		}
		*outId = self;
		return true;
	}

	ctx.Errorf(loc, std::string("unknown element: ") + el->Name());
	return false;
}

/* ---- definitions ---- */

bool ParseDefaults(ParseContext &ctx, XMLElement *el)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
	if (el->FirstChildElement()) {
		ctx.Error(loc, "<defaults> cannot have children");
		return false;
	}
	if (el->Attribute("id")) {
		ctx.Error(loc, "<defaults> cannot have an id");
		return false;
	}
	AttrBucket attrs;
	if (!CollectAttrs(ctx, el, IsDefaultsAttr, false, nullptr, &attrs)) {
		return false;
	}
	ctx.doc->definitions.defaults.MergeFrom(attrs.props);
	return true;
}

bool ParseVars(ParseContext &ctx, XMLElement *varsEl)
{
	for (XMLElement *el = varsEl->FirstChildElement(); el; el = el->NextSiblingElement()) {
		if (std::strcmp(el->Name(), "var") != 0) {
			ctx.Errorf(MakeLoc(ctx.sourceName, el), std::string("unknown element in <vars>: ") + el->Name());
			return false;
		}
		const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
		if (el->FirstChildElement()) {
			ctx.Error(loc, "<var> cannot have children");
			return false;
		}
		const char *id = el->Attribute("id");
		const char *value = el->Attribute("value");
		if (!id || !value || !value[0]) {
			ctx.Error(loc, "<var> requires id and non-empty value");
			return false;
		}
		if (!CheckIdLen(ctx, id, loc)) {
			return false;
		}
		for (const tinyxml2::XMLAttribute *attr = el->FirstAttribute(); attr; attr = attr->Next()) {
			if (std::strcmp(attr->Name(), "id") != 0 && std::strcmp(attr->Name(), "value") != 0) {
				ctx.Error(loc, "unknown attribute on <var>");
				return false;
			}
		}
		if (ctx.doc->definitions.vars.count(id)) {
			WarnDefinitionOverride(ctx, loc, "var", id);
		} else if (static_cast<int>(ctx.doc->definitions.vars.size()) >= ctx.limits->maxVars) {
			ctx.Error(loc, "var count exceeds limit");
			return false;
		}
		uid_var_def_t varDef;
		varDef.id = id;
		varDef.value = value;
		ctx.doc->definitions.vars[id] = std::move(varDef);
	}
	return true;
}

bool ParseFonts(ParseContext &ctx, XMLElement *fontsEl)
{
	std::set<std::pair<std::string, int>> seenWeight;
	for (XMLElement *el = fontsEl->FirstChildElement(); el; el = el->NextSiblingElement()) {
		if (std::strcmp(el->Name(), "font") != 0) {
			ctx.Errorf(MakeLoc(ctx.sourceName, el), std::string("unknown element in <fonts>: ") + el->Name());
			return false;
		}
		const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
		const char *id = el->Attribute("id");
		const char *src = el->Attribute("src");
		const char *weightStr = el->Attribute("weight");
		if (!id || !src || !weightStr) {
			ctx.Error(loc, "<font> requires id, src, and weight");
			return false;
		}
		if (!CheckIdLen(ctx, id, loc)) {
			return false;
		}
		if (static_cast<int>(std::strlen(src)) > ctx.limits->maxFontPathLen) {
			ctx.Error(loc, "font src exceeds maxFontPathLen");
			return false;
		}
		if (PathHasDotDot(src)) {
			ctx.Error(loc, "font src must not contain '..'");
			return false;
		}
		double weightNum = 0.0;
		std::string dm;
		if (!UID_ParseNumber(weightStr, &weightNum, &dm)) {
			ctx.Error(loc, "invalid font weight");
			return false;
		}
		const int weight = static_cast<int>(weightNum);
		if (!seenWeight.insert({id, weight}).second) {
			ctx.Error(loc, "duplicate font (id, weight)");
			return false;
		}
		if (ctx.doc->definitions.fonts.count(id)) {
			WarnDefinitionOverride(ctx, loc, "font", id);
		} else if (static_cast<int>(ctx.doc->definitions.fonts.size()) >= ctx.limits->maxFonts) {
			ctx.Error(loc, "font count exceeds limit");
			return false;
		}
		uid_font_def_t font;
		font.id = id;
		font.src = src;
		font.weight = weight;
		ctx.doc->definitions.fonts[id] = font;
	}
	return true;
}

static bool ImageSrcHasAllowedExt(const char *src)
{
	if (!src || !src[0]) {
		return false;
	}
	/* Last path component: allow Quake shader names (no extension) or .png/.tga. */
	const char *base = src;
	for (const char *p = src; *p; ++p) {
		if (*p == '/' || *p == '\\') {
			base = p + 1;
		}
	}
	if (!base[0]) {
		return false;
	}
	const char *dot = std::strrchr(base, '.');
	if (!dot || !dot[1]) {
		return true;
	}
	return ::strcasecmp(dot, ".png") == 0 || ::strcasecmp(dot, ".tga") == 0;
}

bool ParseImages(ParseContext &ctx, XMLElement *imagesEl)
{
	for (XMLElement *el = imagesEl->FirstChildElement(); el; el = el->NextSiblingElement()) {
		if (std::strcmp(el->Name(), "image") != 0) {
			ctx.Errorf(MakeLoc(ctx.sourceName, el), std::string("unknown element in <images>: ") + el->Name());
			return false;
		}
		const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
		const char *id = el->Attribute("id");
		const char *src = el->Attribute("src");
		if (!id || !src) {
			ctx.Error(loc, "<image> requires id and src");
			return false;
		}
		if (!CheckIdLen(ctx, id, loc)) {
			return false;
		}
		if (static_cast<int>(std::strlen(src)) > ctx.limits->maxFontPathLen) {
			ctx.Error(loc, "image src exceeds maxFontPathLen");
			return false;
		}
		if (PathHasDotDot(src)) {
			ctx.Error(loc, "image src must not contain '..'");
			return false;
		}
		if (!ImageSrcHasAllowedExt(src)) {
			ctx.Error(loc, "image src must be extensionless or end with .png or .tga");
			return false;
		}
		if (ctx.doc->definitions.images.count(id)) {
			WarnDefinitionOverride(ctx, loc, "image", id);
		} else if (static_cast<int>(ctx.doc->definitions.images.size()) >= ctx.limits->maxImages) {
			ctx.Error(loc, "image count exceeds limit");
			return false;
		}
		uid_image_def_t image;
		image.id = id;
		image.src = src;
		ctx.doc->definitions.images[id] = image;
	}
	return true;
}

bool ParseShapeDef(ParseContext &ctx, XMLElement *el)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
	const char *id = el->Attribute("id");
	if (!id || !id[0]) {
		ctx.Error(loc, "<shape> definition requires id");
		return false;
	}
	if (!CheckIdLen(ctx, id, loc)) {
		return false;
	}
	if (ctx.doc->definitions.shapes.count(id)) {
		WarnDefinitionOverride(ctx, loc, "shape", id);
	} else if (static_cast<int>(ctx.doc->definitions.shapes.size()) >= ctx.limits->maxShapes) {
		ctx.Error(loc, "shape count exceeds limit");
		return false;
	}

	uid_shape_def_t shape;
	shape.id = id;
	shape.hasIntrinsicSize = false;
	shape.width = 0.0f;
	shape.height = 0.0f;
	const char *w = el->Attribute("width");
	const char *h = el->Attribute("height");
	const char *fit = el->Attribute("fit");
	if ((w && !h) || (!w && h)) {
		ctx.Error(loc, "shape intrinsic size requires both width and height");
		return false;
	}
	if (w && h) {
		uid_length_t lw, lh;
		std::string dm;
		if (!UID_ParseLength(w, &lw, &dm) || lw.unit != UID_LENGTH_PX ||
		    !UID_ParseLength(h, &lh, &dm) || lh.unit != UID_LENGTH_PX) {
			ctx.Error(loc, "shape intrinsic width/height must be px lengths");
			return false;
		}
		shape.hasIntrinsicSize = true;
		shape.width = lw.value;
		shape.height = lh.value;
	}
	if (fit) {
		shape.fit = fit;
	}

	XMLElement *child = el->FirstChildElement();
	if (child && std::strcmp(child->Name(), "props") == 0) {
		if (!ParsePropDecls(ctx, child, &shape.props)) {
			return false;
		}
		child = child->NextSiblingElement();
	}
	if (!child) {
		ctx.Error(loc, "shape requires at least one <path>");
		return false;
	}
	for (; child; child = child->NextSiblingElement()) {
		if (std::strcmp(child->Name(), "path") != 0) {
			ctx.Errorf(MakeLoc(ctx.sourceName, child), std::string("unexpected element in <shape>: ") + child->Name());
			return false;
		}
		const char *fill = child->Attribute("fill");
		const char *d = child->Attribute("d");
		if (!fill || !d) {
			ctx.Error(MakeLoc(ctx.sourceName, child), "<path> requires fill and d");
			return false;
		}
		if (!CheckPathLen(ctx, d, MakeLoc(ctx.sourceName, child))) {
			return false;
		}
		uid_path_def_t path;
		path.fillExpr = fill;
		path.d = d;
		path.loc = MakeLoc(ctx.sourceName, child);
		shape.paths.push_back(path);
	}

	ctx.doc->definitions.shapes[id] = std::move(shape);
	return true;
}

bool ParseShapes(ParseContext &ctx, XMLElement *shapesEl)
{
	for (XMLElement *el = shapesEl->FirstChildElement(); el; el = el->NextSiblingElement()) {
		if (std::strcmp(el->Name(), "shape") != 0) {
			ctx.Errorf(MakeLoc(ctx.sourceName, el), std::string("unknown element in <shapes>: ") + el->Name());
			return false;
		}
		if (!ParseShapeDef(ctx, el)) {
			return false;
		}
	}
	return true;
}

bool ParseTemplateDef(ParseContext &ctx, XMLElement *el)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
	const char *id = el->Attribute("id");
	if (!id || !id[0]) {
		ctx.Error(loc, "<template> definition requires id");
		return false;
	}
	if (!CheckIdLen(ctx, id, loc)) {
		return false;
	}
	if (ctx.doc->definitions.templates.count(id)) {
		WarnDefinitionOverride(ctx, loc, "template", id);
	} else if (static_cast<int>(ctx.doc->definitions.templates.size()) >= ctx.limits->maxTemplates) {
		ctx.Error(loc, "template count exceeds limit");
		return false;
	}

	uid_template_def_t tmpl;
	tmpl.id = id;
	tmpl.rootNode = UID_INVALID_NODE_ID;

	XMLElement *child = el->FirstChildElement();
	if (child && std::strcmp(child->Name(), "props") == 0) {
		if (!ParsePropDecls(ctx, child, &tmpl.props)) {
			return false;
		}
		child = child->NextSiblingElement();
	}
	if (!child) {
		ctx.Error(loc, "template requires exactly one renderable root");
		return false;
	}
	if (child->NextSiblingElement()) {
		ctx.Error(loc, "template requires exactly one renderable root");
		return false;
	}

	uid_node_id_t rootId = UID_INVALID_NODE_ID;
	if (!ParseRenderable(ctx, child, 1, &tmpl.nodes, &rootId, true)) {
		return false;
	}
	tmpl.rootNode = rootId;
	ctx.doc->definitions.templates[id] = std::move(tmpl);
	return true;
}

bool ParseTemplates(ParseContext &ctx, XMLElement *templatesEl)
{
	for (XMLElement *el = templatesEl->FirstChildElement(); el; el = el->NextSiblingElement()) {
		if (std::strcmp(el->Name(), "template") != 0) {
			ctx.Errorf(MakeLoc(ctx.sourceName, el), std::string("unknown element in <templates>: ") + el->Name());
			return false;
		}
		if (!ParseTemplateDef(ctx, el)) {
			return false;
		}
	}
	return true;
}

bool ParseModalDef(ParseContext &ctx, XMLElement *el)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
	const char *id = el->Attribute("id");
	if (!id || !id[0]) {
		ctx.Error(loc, "<modal> definition requires id");
		return false;
	}
	if (!CheckIdLen(ctx, id, loc)) {
		return false;
	}
	if (ctx.doc->definitions.modals.count(id)) {
		WarnDefinitionOverride(ctx, loc, "modal", id);
	} else if (static_cast<int>(ctx.doc->definitions.modals.size()) >= ctx.limits->maxTemplates) {
		ctx.Error(loc, "modal count exceeds limit");
		return false;
	}

	uid_modal_def_t modal;
	modal.id = id;
	modal.type.clear();
	modal.rootNode = UID_INVALID_NODE_ID;

	/* Added in OPM: type=relative → opener-anchored panel (role=relative-panel). */
	const char *type = el->Attribute("type");
	if (type && type[0]) {
		if (std::strcmp(type, "relative") != 0) {
			ctx.Error(loc, "<modal> type must be relative when set");
			return false;
		}
		modal.type = type;
	}

	XMLElement *child = el->FirstChildElement();
	if (!child) {
		ctx.Error(loc, "modal requires exactly one renderable root");
		return false;
	}
	if (child->NextSiblingElement()) {
		ctx.Error(loc, "modal requires exactly one renderable root");
		return false;
	}

	uid_node_id_t rootId = UID_INVALID_NODE_ID;
	if (!ParseRenderable(ctx, child, 1, &modal.nodes, &rootId, true)) {
		return false;
	}
	modal.rootNode = rootId;

	if (modal.type == "relative") {
		int panelCount = 0;
		for (const uid_node_def_t &n : modal.nodes) {
			if (n.role == "relative-panel") {
				++panelCount;
			}
		}
		if (panelCount != 1) {
			ctx.Error(loc, "<modal type=\"relative\"> requires exactly one role=\"relative-panel\"");
			return false;
		}
	}

	ctx.doc->definitions.modals[id] = std::move(modal);
	return true;
}

bool ParseModals(ParseContext &ctx, XMLElement *modalsEl)
{
	for (XMLElement *el = modalsEl->FirstChildElement(); el; el = el->NextSiblingElement()) {
		if (std::strcmp(el->Name(), "modal") != 0) {
			ctx.Errorf(MakeLoc(ctx.sourceName, el), std::string("unknown element in <modals>: ") + el->Name());
			return false;
		}
		if (!ParseModalDef(ctx, el)) {
			return false;
		}
	}
	return true;
}

bool ParseSourceItem(ParseContext &ctx, XMLElement *el, uid_source_item_def_t *out)
{
	if (!el || !out) {
		return false;
	}
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
	const char *value = el->Attribute("value");
	const char *label = el->Attribute("label");
	if (!label || !label[0]) {
		ctx.Error(loc, "<item> requires label");
		return false;
	}
	out->value = value ? value : "";
	out->label = label;
	out->fields.clear();
	for (const tinyxml2::XMLAttribute *attr = el->FirstAttribute(); attr; attr = attr->Next()) {
		const char *name = attr->Name();
		if (std::strcmp(name, "value") == 0 || std::strcmp(name, "label") == 0) {
			continue;
		}
		out->fields[name] = attr->Value() ? attr->Value() : "";
	}
	return true;
}

bool ParseSourceDef(ParseContext &ctx, XMLElement *el)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
	const char *id = el->Attribute("id");
	if (!id || !id[0]) {
		ctx.Error(loc, "<source> requires id");
		return false;
	}
	if (!CheckIdLen(ctx, id, loc)) {
		return false;
	}
	if (ctx.doc->definitions.sources.count(id)) {
		WarnDefinitionOverride(ctx, loc, "source", id);
	} else if (static_cast<int>(ctx.doc->definitions.sources.size()) >= ctx.limits->maxOptionsPerSelect) {
		ctx.Error(loc, "source count exceeds limit");
		return false;
	}

	uid_source_def_t src;
	src.id = id;
	const char *def = el->Attribute("default");
	src.defaultValue = def ? def : "";
	src.items.clear();
	for (XMLElement *child = el->FirstChildElement(); child; child = child->NextSiblingElement()) {
		if (std::strcmp(child->Name(), "item") != 0) {
			ctx.Errorf(MakeLoc(ctx.sourceName, child), std::string("unknown element in <source>: ") + child->Name());
			return false;
		}
		uid_source_item_def_t item;
		if (!ParseSourceItem(ctx, child, &item)) {
			return false;
		}
		src.items.push_back(item);
	}
	if (src.items.empty()) {
		ctx.Error(loc, "<source> requires at least one <item>");
		return false;
	}
	ctx.doc->definitions.sources[id] = std::move(src);
	return true;
}

bool ParseSources(ParseContext &ctx, XMLElement *sourcesEl)
{
	for (XMLElement *el = sourcesEl->FirstChildElement(); el; el = el->NextSiblingElement()) {
		if (std::strcmp(el->Name(), "source") != 0) {
			ctx.Errorf(MakeLoc(ctx.sourceName, el), std::string("unknown element in <sources>: ") + el->Name());
			return false;
		}
		if (!ParseSourceDef(ctx, el)) {
			return false;
		}
	}
	return true;
}

bool ParseDefinitionChildren(ParseContext &ctx, XMLElement *parent)
{
	bool sawDefaults = false;
	bool sawFonts = false;
	bool sawImages = false;
	bool sawShapes = false;
	bool sawTemplates = false;
	bool sawModals = false;
	bool sawSources = false;
	bool sawVars = false;

	for (XMLElement *el = parent->FirstChildElement(); el; el = el->NextSiblingElement()) {
		const char *name = el->Name();
		if (std::strcmp(name, "import") == 0) {
			if (!ParseImport(ctx, el)) {
				return false;
			}
		} else if (std::strcmp(name, "defaults") == 0) {
			if (sawDefaults) {
				ctx.Error(MakeLoc(ctx.sourceName, el), "duplicate <defaults>");
				return false;
			}
			sawDefaults = true;
			if (!ParseDefaults(ctx, el)) {
				return false;
			}
		} else if (std::strcmp(name, "fonts") == 0) {
			if (sawFonts) {
				ctx.Error(MakeLoc(ctx.sourceName, el), "duplicate <fonts>");
				return false;
			}
			sawFonts = true;
			if (!ParseFonts(ctx, el)) {
				return false;
			}
		} else if (std::strcmp(name, "images") == 0) {
			if (sawImages) {
				ctx.Error(MakeLoc(ctx.sourceName, el), "duplicate <images>");
				return false;
			}
			sawImages = true;
			if (!ParseImages(ctx, el)) {
				return false;
			}
		} else if (std::strcmp(name, "shapes") == 0) {
			if (sawShapes) {
				ctx.Error(MakeLoc(ctx.sourceName, el), "duplicate <shapes>");
				return false;
			}
			sawShapes = true;
			if (!ParseShapes(ctx, el)) {
				return false;
			}
		} else if (std::strcmp(name, "templates") == 0) {
			if (sawTemplates) {
				ctx.Error(MakeLoc(ctx.sourceName, el), "duplicate <templates>");
				return false;
			}
			sawTemplates = true;
			if (!ParseTemplates(ctx, el)) {
				return false;
			}
		} else if (std::strcmp(name, "modals") == 0) {
			if (sawModals) {
				ctx.Error(MakeLoc(ctx.sourceName, el), "duplicate <modals>");
				return false;
			}
			sawModals = true;
			if (!ParseModals(ctx, el)) {
				return false;
			}
		} else if (std::strcmp(name, "sources") == 0) {
			if (sawSources) {
				ctx.Error(MakeLoc(ctx.sourceName, el), "duplicate <sources>");
				return false;
			}
			sawSources = true;
			if (!ParseSources(ctx, el)) {
				return false;
			}
		} else if (std::strcmp(name, "vars") == 0) {
			if (sawVars) {
				ctx.Error(MakeLoc(ctx.sourceName, el), "duplicate <vars>");
				return false;
			}
			sawVars = true;
			if (!ParseVars(ctx, el)) {
				return false;
			}
		} else {
			ctx.Errorf(MakeLoc(ctx.sourceName, el), std::string("unknown element: ") + name);
			return false;
		}
	}
	return true;
}

bool ParseUiLibraryBytes(ParseContext &ctx, const std::string &resolvedPath, const char *xml, size_t size)
{
	const uid_source_location_t baseLoc = {resolvedPath.c_str(), 0, 0};

	if (size > ctx.limits->maxXmlBytes) {
		ctx.Error(baseLoc, "imported XML size exceeds maxXmlBytes");
		return false;
	}
	if (ctx.importBytesTotal + size > ctx.limits->maxXmlBytes) {
		ctx.Error(baseLoc, "cumulative imported XML exceeds maxXmlBytes");
		return false;
	}
	ctx.importBytesTotal += size;

	if (ContainsDoctype(xml, size)) {
		ctx.Error(baseLoc, "DOCTYPE / DTD declarations are not allowed in imported library");
		return false;
	}

	XMLDocument xmlDoc;
	const tinyxml2::XMLError err = xmlDoc.Parse(xml, size);
	if (err != tinyxml2::XML_SUCCESS) {
		uid_source_location_t loc = baseLoc;
		loc.line = xmlDoc.ErrorLineNum();
		ctx.Error(loc, xmlDoc.ErrorStr() ? xmlDoc.ErrorStr() : "XML parse error in import");
		return false;
	}

	XMLElement *root = xmlDoc.RootElement();
	if (!root || std::strcmp(root->Name(), "ui-library") != 0) {
		ctx.Error(MakeLoc(resolvedPath.c_str(), root), "imported file root must be <ui-library>");
		return false;
	}

	const char *version = root->Attribute("version");
	if (!version || std::strcmp(version, "1") != 0) {
		ctx.Error(MakeLoc(resolvedPath.c_str(), root), "<ui-library> requires version=\"1\"");
		return false;
	}
	for (const tinyxml2::XMLAttribute *attr = root->FirstAttribute(); attr; attr = attr->Next()) {
		if (std::strcmp(attr->Name(), "version") != 0) {
			ctx.Error(MakeLoc(resolvedPath.c_str(), root), "unknown attribute on <ui-library>");
			return false;
		}
	}

	const char *savedSource = ctx.sourceName;
	ctx.importStack.push_back(resolvedPath);
	ctx.sourceName = ctx.importStack.back().c_str();

	const bool ok = ParseDefinitionChildren(ctx, root);

	ctx.importStack.pop_back();
	if (!ctx.importStack.empty()) {
		ctx.sourceName = ctx.importStack.back().c_str();
	} else {
		ctx.sourceName = savedSource;
	}

	return ok;
}

bool ParseImport(ParseContext &ctx, XMLElement *el)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, el);
	if (el->FirstChildElement()) {
		ctx.Error(loc, "<import> cannot have children");
		return false;
	}

	const char *src = el->Attribute("src");
	for (const tinyxml2::XMLAttribute *attr = el->FirstAttribute(); attr; attr = attr->Next()) {
		if (std::strcmp(attr->Name(), "src") != 0) {
			ctx.Error(loc, "unknown attribute on <import>");
			return false;
		}
	}
	if (!src || !src[0]) {
		ctx.Error(loc, "<import> requires src");
		return false;
	}
	if (!ctx.io || !ctx.io->readFile || !ctx.io->freeFile) {
		ctx.Error(loc, "<import> requires a file reader (load via UID_LoadFile)");
		return false;
	}

	std::string resolved;
	if (!ResolveImportPath(ctx.sourceName, src, &resolved)) {
		ctx.Error(loc, "invalid import src");
		return false;
	}

	for (const std::string &seen : ctx.importStack) {
		if (seen == resolved) {
			ctx.Error(loc, "import cycle detected");
			return false;
		}
	}
	if (ctx.sourceName && resolved == ctx.sourceName) {
		ctx.Error(loc, "import cycle detected");
		return false;
	}

	if (static_cast<int>(ctx.importStack.size()) >= ctx.limits->maxImportDepth) {
		ctx.Error(loc, "import depth exceeds limit");
		return false;
	}
	if (ctx.importFileCount >= ctx.limits->maxImportFiles) {
		ctx.Error(loc, "import file count exceeds limit");
		return false;
	}
	++ctx.importFileCount;

	void *buf = nullptr;
	const long bytes = ctx.io->readFile(resolved.c_str(), &buf);
	if (bytes < 0 || !buf) {
		ctx.Error(loc, "failed to read import");
		return false;
	}

	const bool ok = ParseUiLibraryBytes(ctx, resolved, static_cast<const char *>(buf), static_cast<size_t>(bytes));
	ctx.io->freeFile(buf);
	return ok;
}

bool ParseDefinitions(ParseContext &ctx, XMLElement *defs)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, defs);
	const char *menuId = nullptr;
	const char *hudId = nullptr;
	const char *hudLabel = nullptr;
	const char *drawOrderStr = nullptr;
	const char *backdropStr = nullptr;
	const char *pauseMenu = nullptr;
	const char *scoreboardMenu = nullptr;

	for (const tinyxml2::XMLAttribute *attr = defs->FirstAttribute(); attr; attr = attr->Next()) {
		const char *name = attr->Name();
		if (std::strcmp(name, "menu-id") == 0) {
			menuId = attr->Value();
		} else if (std::strcmp(name, "hud-id") == 0) {
			hudId = attr->Value();
		} else if (std::strcmp(name, "hud-label") == 0) {
			hudLabel = attr->Value();
		} else if (std::strcmp(name, "draw-order") == 0) {
			drawOrderStr = attr->Value();
		} else if (std::strcmp(name, "backdrop") == 0) {
			backdropStr = attr->Value();
		} else if (std::strcmp(name, "pause-menu") == 0) {
			/* Added in Omaha: HUD companion pause menu-id. */
			pauseMenu = attr->Value();
		} else if (std::strcmp(name, "scoreboard-menu") == 0) {
			/* Added in Omaha: HUD companion scoreboard menu-id. */
			scoreboardMenu = attr->Value();
		} else {
			ctx.Error(loc, (std::string("unknown attribute on <definitions>: ") + name).c_str());
			return false;
		}
	}

	const bool hasMenuId = menuId && menuId[0];
	const bool hasHudId = hudId && hudId[0];
	if (hasMenuId && hasHudId) {
		ctx.Error(loc, "<definitions> cannot set both menu-id and hud-id");
		return false;
	}

	if (hasMenuId) {
		if (pauseMenu && pauseMenu[0]) {
			ctx.Error(loc, "<definitions pause-menu> requires hud-id");
			return false;
		}
		if (scoreboardMenu && scoreboardMenu[0]) {
			ctx.Error(loc, "<definitions scoreboard-menu> requires hud-id");
			return false;
		}
		if (!drawOrderStr || !drawOrderStr[0]) {
			ctx.Error(loc, "<definitions menu-id> requires draw-order");
			return false;
		}
		const int drawOrder = std::atoi(drawOrderStr);
		if (drawOrder < 0 || drawOrder > 9) {
			ctx.Error(loc, "draw-order must be 0-9");
			return false;
		}
		uid_menu_backdrop_t backdrop = UID_MENU_BACKDROP_NONE;
		if (backdropStr && backdropStr[0]) {
			if (!std::strcmp(backdropStr, "none")) {
				backdrop = UID_MENU_BACKDROP_NONE;
			} else if (!std::strcmp(backdropStr, "menu-map")) {
				backdrop = UID_MENU_BACKDROP_MENU_MAP;
			} else {
				ctx.Error(loc, "backdrop must be none or menu-map");
				return false;
			}
		}
		ctx.doc->hasMenuMeta = true;
		ctx.doc->menuId = menuId;
		ctx.doc->drawOrder = drawOrder;
		ctx.doc->menuBackdrop = backdrop;
	} else if (hasHudId) {
		if (!hudLabel || !hudLabel[0]) {
			ctx.Error(loc, "<definitions hud-id> requires hud-label");
			return false;
		}
		if (!drawOrderStr || !drawOrderStr[0]) {
			ctx.Error(loc, "<definitions hud-id> requires draw-order");
			return false;
		}
		if (!pauseMenu || !pauseMenu[0]) {
			ctx.Error(loc, "<definitions hud-id> requires pause-menu");
			return false;
		}
		if (!scoreboardMenu || !scoreboardMenu[0]) {
			ctx.Error(loc, "<definitions hud-id> requires scoreboard-menu");
			return false;
		}
		const int drawOrder = std::atoi(drawOrderStr);
		if (drawOrder < 0 || drawOrder > 9) {
			ctx.Error(loc, "draw-order must be 0-9");
			return false;
		}
		if (backdropStr && backdropStr[0] && std::strcmp(backdropStr, "none") != 0) {
			ctx.Error(loc, "HUD pack backdrop must be none or omitted");
			return false;
		}
		/* Added in OPM: HUD packs are not menu-registry entries; meta validated here only. */
		ctx.doc->drawOrder = drawOrder;
	} else if (drawOrderStr && drawOrderStr[0]) {
		ctx.Error(loc, "<definitions draw-order> requires menu-id or hud-id");
		return false;
	} else if (hudLabel && hudLabel[0]) {
		ctx.Error(loc, "<definitions hud-label> requires hud-id");
		return false;
	} else if (pauseMenu && pauseMenu[0]) {
		ctx.Error(loc, "<definitions pause-menu> requires hud-id");
		return false;
	} else if (scoreboardMenu && scoreboardMenu[0]) {
		ctx.Error(loc, "<definitions scoreboard-menu> requires hud-id");
		return false;
	}

	return ParseDefinitionChildren(ctx, defs);
}

bool ParseCanvas(ParseContext &ctx, XMLElement *canvas)
{
	const uid_source_location_t loc = MakeLoc(ctx.sourceName, canvas);
	ctx.doc->pointerExpr.clear();
	for (const tinyxml2::XMLAttribute *attr = canvas->FirstAttribute(); attr; attr = attr->Next()) {
		const char *name = attr->Name();
		const char *value = attr->Value();
		if (std::strcmp(name, "pointer") != 0) {
			ctx.Error(loc, (std::string("unknown attribute on <canvas>: ") + name).c_str());
			return false;
		}
		if (!value || !value[0]) {
			ctx.Error(loc, "<canvas pointer> requires a bool expression or true/false");
			return false;
		}
		std::string inner;
		if (UID_ParseBraceBoolExpr(value, &inner)) {
			ctx.doc->pointerExpr = inner;
		} else if (!std::strcmp(value, "true") || !std::strcmp(value, "1")) {
			ctx.doc->pointerExpr = "true";
		} else if (!std::strcmp(value, "false") || !std::strcmp(value, "0")) {
			ctx.doc->pointerExpr = "false";
		} else {
			ctx.Error(loc, "<canvas pointer> must be {bool expr}, true, or false");
			return false;
		}
	}

	XMLElement *root = canvas->FirstChildElement();
	if (!root) {
		ctx.doc->rootNode = UID_INVALID_NODE_ID;
		return true;
	}
	if (root->NextSiblingElement()) {
		ctx.Error(loc, "<canvas> may contain at most one renderable root");
		return false;
	}
	uid_node_id_t rootId = UID_INVALID_NODE_ID;
	if (!ParseRenderable(ctx, root, 1, &ctx.doc->nodes, &rootId, false)) {
		return false;
	}
	ctx.doc->rootNode = rootId;
	return true;
}

} // namespace

void UID_ApplyCollectionAndIndexFields(uid_node_def_t *node)
{
	if (!node) {
		return;
	}
	std::string tmp;
	if (node->kind != UID_NODE_SELECT && node->properties.Get("source", &tmp) && !tmp.empty()) {
		node->collectionSource = tmp;
	}
	if (node->properties.Get("wrap", &tmp)) {
		node->collectionWrap = (tmp == "true" || tmp == "1");
	}
	if (node->properties.Get("scroll", &tmp)) {
		node->collectionScroll = (tmp == "true" || tmp == "1");
	}
	if (node->properties.Get("index", &tmp) && !tmp.empty()) {
		node->indexBind = tmp;
	}
	if (node->properties.Get("step-index", &tmp) && !tmp.empty()) {
		node->hasStepIndex = true;
		node->stepIndex = std::atoi(tmp.c_str());
	}
	if (node->properties.Get("set-index", &tmp) && !tmp.empty()) {
		node->hasSetIndex = true;
		if (tmp == "{item.index}") {
			node->setIndexValue = -1;
		} else {
			node->setIndexValue = std::atoi(tmp.c_str());
		}
	}
	if (node->properties.Get("visible-if-index", &tmp) && !tmp.empty()) {
		node->visibleIfIndex = tmp;
		/* Added in OPM: migrate visible-if-index to brace bool expr. */
		if (node->visibleExpr.empty()) {
			UID_VisibleIfIndexToBoolExpr(tmp, &node->visibleExpr);
		}
	}
	if (node->properties.Get("collection-display", &tmp) && !tmp.empty()) {
		node->collectionDisplay = tmp;
	}
	if (node->properties.Get("default-index", &tmp) && !tmp.empty()) {
		node->hasCollectionDefaultIndex = true;
		node->collectionDefaultIndex = std::atoi(tmp.c_str());
	}
}

uid_result_t UID_ParseXml(
	const char *sourceName,
	const char *xml,
	size_t size,
	const uid_limits_t *limits,
	const uid_parse_io_t *io,
	uid_document_t *outDoc,
	uid_diag_list_t *diags
)
{
	if (!xml || !outDoc) {
		return UID_ERR_INVALID_ARG;
	}

	uid_limits_t defaultLimits;
	UID_DefaultLimits(&defaultLimits);
	const uid_limits_t *lim = limits ? limits : &defaultLimits;

	if (diags) {
		diags->SetLimit(lim->maxDiagnostics);
		diags->Clear();
	}

	UID_ClearDocument(outDoc);
	outDoc->limits = *lim;
	outDoc->sourceName = sourceName ? sourceName : "";

	uid_source_location_t baseLoc;
	baseLoc.path = sourceName;
	baseLoc.line = 0;
	baseLoc.column = 0;

	if (size > lim->maxXmlBytes) {
		if (diags) {
			diags->Error(baseLoc, "XML size exceeds maxXmlBytes");
		}
		return UID_ERR_LIMIT;
	}

	if (ContainsDoctype(xml, size)) {
		if (diags) {
			diags->Error(baseLoc, "DOCTYPE / DTD declarations are not allowed");
		}
		return UID_ERR_PARSE;
	}

	XMLDocument xmlDoc;
	const tinyxml2::XMLError err = xmlDoc.Parse(xml, size);
	if (err != tinyxml2::XML_SUCCESS) {
		if (diags) {
			uid_source_location_t loc = baseLoc;
			loc.line = xmlDoc.ErrorLineNum();
			diags->Error(loc, xmlDoc.ErrorStr() ? xmlDoc.ErrorStr() : "XML parse error");
		}
		return UID_ERR_PARSE;
	}

	XMLElement *root = xmlDoc.RootElement();
	if (!root || std::strcmp(root->Name(), "ui") != 0) {
		if (diags) {
			diags->Error(MakeLoc(sourceName, root), "root element must be <ui>");
		}
		return UID_ERR_VALIDATE;
	}

	const char *version = root->Attribute("version");
	if (!version || std::strcmp(version, "1") != 0) {
		if (diags) {
			diags->Error(MakeLoc(sourceName, root), "<ui> requires version=\"1\"");
		}
		return UID_ERR_VALIDATE;
	}
	for (const tinyxml2::XMLAttribute *attr = root->FirstAttribute(); attr; attr = attr->Next()) {
		if (std::strcmp(attr->Name(), "version") != 0) {
			if (diags) {
				diags->Error(MakeLoc(sourceName, root), "unknown attribute on <ui>");
			}
			return UID_ERR_VALIDATE;
		}
	}

	XMLElement *definitions = nullptr;
	XMLElement *canvas = nullptr;
	for (XMLElement *child = root->FirstChildElement(); child; child = child->NextSiblingElement()) {
		if (std::strcmp(child->Name(), "definitions") == 0) {
			if (definitions) {
				if (diags) {
					diags->Error(MakeLoc(sourceName, child), "duplicate <definitions>");
				}
				return UID_ERR_VALIDATE;
			}
			definitions = child;
		} else if (std::strcmp(child->Name(), "canvas") == 0) {
			if (canvas) {
				if (diags) {
					diags->Error(MakeLoc(sourceName, child), "duplicate <canvas>");
				}
				return UID_ERR_VALIDATE;
			}
			canvas = child;
		} else {
			if (diags) {
				diags->Error(MakeLoc(sourceName, child), "unknown child of <ui>");
			}
			return UID_ERR_VALIDATE;
		}
	}

	if (!definitions || !canvas) {
		if (diags) {
			diags->Error(MakeLoc(sourceName, root), "<ui> requires exactly one <definitions> and one <canvas>");
		}
		return UID_ERR_VALIDATE;
	}

	/* Prefer definitions before canvas; still accept either order if both present. */
	if (definitions->GetLineNum() > canvas->GetLineNum()) {
		if (diags) {
			diags->Warning(MakeLoc(sourceName, definitions), "<definitions> should appear before <canvas>");
		}
	}

	ParseContext ctx;
	ctx.sourceName = sourceName;
	ctx.limits = lim;
	ctx.io = io;
	ctx.doc = outDoc;
	ctx.diags = diags;
	ctx.parsedNodeCount = 0;
	ctx.failed = false;
	ctx.importFileCount = 0;
	ctx.importBytesTotal = size;

	if (!ParseDefinitions(ctx, definitions) || ctx.failed) {
		UID_ClearDocument(outDoc);
		return UID_ERR_VALIDATE;
	}
	if (!ParseCanvas(ctx, canvas) || ctx.failed) {
		UID_ClearDocument(outDoc);
		return UID_ERR_VALIDATE;
	}

	outDoc->dirty = UID_DIRTY_STRUCTURE;
	outDoc->expanded = false;
	return UID_OK;
}

static bool ReadMenuMetaFromDefinitions(
	const char *sourceName,
	const XMLElement *defs,
	uid_menu_meta_t *out,
	uid_diag_list_t *diags
)
{
	if (!defs || !out) {
		return false;
	}
	const uid_source_location_t loc = MakeLoc(sourceName, defs);
	const char *menuId = defs->Attribute("menu-id");
	const char *drawOrderStr = defs->Attribute("draw-order");
	const char *backdropStr = defs->Attribute("backdrop");

	std::memset(out, 0, sizeof(*out));
	out->backdrop = UID_MENU_BACKDROP_NONE;

	if (!menuId || !menuId[0]) {
		return false;
	}
	if (!drawOrderStr || !drawOrderStr[0]) {
		if (diags) {
			diags->Error(loc, "<definitions menu-id> requires draw-order");
		}
		return false;
	}
	const int drawOrder = std::atoi(drawOrderStr);
	if (drawOrder < 0 || drawOrder > 9) {
		if (diags) {
			diags->Error(loc, "draw-order must be 0-9");
		}
		return false;
	}
	if (backdropStr && backdropStr[0]) {
		if (!std::strcmp(backdropStr, "none")) {
			out->backdrop = UID_MENU_BACKDROP_NONE;
		} else if (!std::strcmp(backdropStr, "menu-map")) {
			out->backdrop = UID_MENU_BACKDROP_MENU_MAP;
		} else {
			if (diags) {
				diags->Error(loc, "backdrop must be none or menu-map");
			}
			return false;
		}
	}

	std::strncpy(out->menuId, menuId, sizeof(out->menuId) - 1);
	out->menuId[sizeof(out->menuId) - 1] = '\0';
	out->drawOrder = drawOrder;
	out->valid = true;
	return true;
}

uid_result_t UID_PeekMenuMetadata(
	const char *vfsPath,
	const uid_parse_io_t *io,
	uid_menu_meta_t *out,
	uid_diag_list_t *diags
)
{
	if (!vfsPath || !vfsPath[0] || !out) {
		return UID_ERR_INVALID_ARG;
	}

	std::memset(out, 0, sizeof(*out));
	out->backdrop = UID_MENU_BACKDROP_NONE;

	if (!io || !io->readFile || !io->freeFile) {
		return UID_ERR_NOT_READY;
	}

	void *buf = nullptr;
	const long size = io->readFile(vfsPath, &buf);
	if (size < 0 || !buf) {
		return UID_ERR_IO;
	}

	tinyxml2::XMLDocument doc;
	const tinyxml2::XMLError err = doc.Parse(static_cast<const char *>(buf), static_cast<size_t>(size));
	io->freeFile(buf);
	if (err != tinyxml2::XML_SUCCESS) {
		return UID_ERR_VALIDATE;
	}

	const XMLElement *root = doc.RootElement();
	if (!root || std::strcmp(root->Name(), "ui") != 0) {
		return UID_ERR_VALIDATE;
	}
	const char *version = root->Attribute("version");
	if (!version || std::strcmp(version, "1") != 0) {
		return UID_ERR_VALIDATE;
	}

	const XMLElement *definitions = nullptr;
	const XMLElement *canvas = nullptr;
	for (const XMLElement *child = root->FirstChildElement(); child; child = child->NextSiblingElement()) {
		if (std::strcmp(child->Name(), "definitions") == 0) {
			definitions = child;
		} else if (std::strcmp(child->Name(), "canvas") == 0) {
			canvas = child;
		}
	}
	if (!definitions || !canvas) {
		return UID_ERR_VALIDATE;
	}

	if (!ReadMenuMetaFromDefinitions(vfsPath, definitions, out, diags)) {
		return UID_ERR_VALIDATE;
	}
	return UID_OK;
}

static bool ReadHudMetaFromDefinitions(
	const char *sourceName,
	const XMLElement *defs,
	uid_hud_meta_t *out,
	uid_diag_list_t *diags
)
{
	if (!defs || !out) {
		return false;
	}
	const uid_source_location_t loc = MakeLoc(sourceName, defs);
	const char                 *hudId = defs->Attribute("hud-id");
	const char                 *hudLabel = defs->Attribute("hud-label");
	const char                 *drawOrderStr = defs->Attribute("draw-order");
	const char                 *pauseMenu = defs->Attribute("pause-menu");
	const char                 *scoreboardMenu = defs->Attribute("scoreboard-menu");

	std::memset(out, 0, sizeof(*out));

	if (!hudId || !hudId[0]) {
		return false;
	}
	if (!hudLabel || !hudLabel[0]) {
		if (diags) {
			diags->Error(loc, "<definitions hud-id> requires hud-label");
		}
		return false;
	}
	if (!drawOrderStr || !drawOrderStr[0]) {
		if (diags) {
			diags->Error(loc, "<definitions hud-id> requires draw-order");
		}
		return false;
	}
	/* Added in Omaha: companions required so host routing stays HUD-file-defined. */
	if (!pauseMenu || !pauseMenu[0]) {
		if (diags) {
			diags->Error(loc, "<definitions hud-id> requires pause-menu");
		}
		return false;
	}
	if (!scoreboardMenu || !scoreboardMenu[0]) {
		if (diags) {
			diags->Error(loc, "<definitions hud-id> requires scoreboard-menu");
		}
		return false;
	}
	const int drawOrder = std::atoi(drawOrderStr);
	if (drawOrder < 0 || drawOrder > 9) {
		if (diags) {
			diags->Error(loc, "draw-order must be 0-9");
		}
		return false;
	}

	std::strncpy(out->hudId, hudId, sizeof(out->hudId) - 1);
	out->hudId[sizeof(out->hudId) - 1] = '\0';
	std::strncpy(out->hudLabel, hudLabel, sizeof(out->hudLabel) - 1);
	out->hudLabel[sizeof(out->hudLabel) - 1] = '\0';
	std::strncpy(out->pauseMenu, pauseMenu, sizeof(out->pauseMenu) - 1);
	out->pauseMenu[sizeof(out->pauseMenu) - 1] = '\0';
	std::strncpy(out->scoreboardMenu, scoreboardMenu, sizeof(out->scoreboardMenu) - 1);
	out->scoreboardMenu[sizeof(out->scoreboardMenu) - 1] = '\0';
	out->drawOrder = drawOrder;
	out->valid = true;
	return true;
}

uid_result_t UID_PeekHudMetadata(
	const char *vfsPath,
	const uid_parse_io_t *io,
	uid_hud_meta_t *out,
	uid_diag_list_t *diags
)
{
	if (!vfsPath || !vfsPath[0] || !out) {
		return UID_ERR_INVALID_ARG;
	}

	std::memset(out, 0, sizeof(*out));

	if (!io || !io->readFile || !io->freeFile) {
		return UID_ERR_NOT_READY;
	}

	void *buf = nullptr;
	const long size = io->readFile(vfsPath, &buf);
	if (size < 0 || !buf) {
		return UID_ERR_IO;
	}

	tinyxml2::XMLDocument doc;
	const tinyxml2::XMLError err = doc.Parse(static_cast<const char *>(buf), static_cast<size_t>(size));
	io->freeFile(buf);
	if (err != tinyxml2::XML_SUCCESS) {
		return UID_ERR_VALIDATE;
	}

	const XMLElement *root = doc.RootElement();
	if (!root || std::strcmp(root->Name(), "ui") != 0) {
		return UID_ERR_VALIDATE;
	}
	const char *version = root->Attribute("version");
	if (!version || std::strcmp(version, "1") != 0) {
		return UID_ERR_VALIDATE;
	}

	const XMLElement *definitions = nullptr;
	const XMLElement *canvas = nullptr;
	for (const XMLElement *child = root->FirstChildElement(); child; child = child->NextSiblingElement()) {
		if (std::strcmp(child->Name(), "definitions") == 0) {
			definitions = child;
		} else if (std::strcmp(child->Name(), "canvas") == 0) {
			canvas = child;
		}
	}
	if (!definitions || !canvas) {
		return UID_ERR_VALIDATE;
	}

	if (!ReadHudMetaFromDefinitions(vfsPath, definitions, out, diags)) {
		return UID_ERR_VALIDATE;
	}
	return UID_OK;
}
