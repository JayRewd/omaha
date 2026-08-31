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

#include "uid_value.h"
#include "uid_opt.h"

#include "../uirender/uir_gradient.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstring>

static void UID_SetDiag(std::string *diagMessage, const char *msg)
{
	if (diagMessage) {
		*diagMessage = msg ? msg : "";
	}
}

static const char *UID_SkipWs(const char *p)
{
	while (p && *p && std::isspace(static_cast<unsigned char>(*p))) {
		++p;
	}
	return p;
}

static bool UID_EqIgnoreCase(const char *a, const char *b)
{
	if (!a || !b) {
		return false;
	}
	while (*a && *b) {
		const unsigned char ca = static_cast<unsigned char>(*a++);
		const unsigned char cb = static_cast<unsigned char>(*b++);
		if (std::tolower(ca) != std::tolower(cb)) {
			return false;
		}
	}
	return *a == '\0' && *b == '\0';
}

static bool UID_ParseLengthToken(const char *begin, const char *end, uid_length_t *out, std::string *diagMessage)
{
	if (!begin || begin >= end || !out) {
		UID_SetDiag(diagMessage, "empty length");
		return false;
	}

	/* Added in OPM: trim without heap allocation. */
	while (begin < end && std::isspace(static_cast<unsigned char>(*begin))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
		--end;
	}
	if (begin >= end) {
		UID_SetDiag(diagMessage, "empty length");
		return false;
	}

	char stackBuf[128];
	const size_t len = static_cast<size_t>(end - begin);
	if (len >= sizeof(stackBuf)) {
		UID_SetDiag(diagMessage, "length token too long");
		return false;
	}
	std::memcpy(stackBuf, begin, len);
	stackBuf[len] = '\0';

	if (UID_EqIgnoreCase(stackBuf, "auto")) {
		out->unit = UID_LENGTH_AUTO;
		out->value = 0.0f;
		return true;
	}
	if (UID_EqIgnoreCase(stackBuf, "fill")) {
		out->unit = UID_LENGTH_FILL;
		out->value = 0.0f;
		return true;
	}

	char *parseEnd = nullptr;
	errno = 0;
	const double value = std::strtod(stackBuf, &parseEnd);
	if (parseEnd == stackBuf || errno == ERANGE || !std::isfinite(value)) {
		UID_SetDiag(diagMessage, "invalid length number");
		return false;
	}

	const char *unit = UID_SkipWs(parseEnd);
	if (!*unit) {
		out->unit = UID_LENGTH_PX;
		out->value = static_cast<float>(value);
		return true;
	}
	if (UID_EqIgnoreCase(unit, "px")) {
		out->unit = UID_LENGTH_PX;
		out->value = static_cast<float>(value);
		return true;
	}
	if (std::strcmp(unit, "%") == 0) {
		out->unit = UID_LENGTH_PERCENT;
		out->value = static_cast<float>(value);
		return true;
	}

	UID_SetDiag(diagMessage, "unknown length unit");
	return false;
}

bool UID_ParseLength(const char *text, uid_length_t *out, std::string *diagMessage)
{
	if (!text || !out) {
		UID_SetDiag(diagMessage, "null length argument");
		return false;
	}
	const char *end = text + std::strlen(text);
	return UID_ParseLengthToken(text, end, out, diagMessage);
}

bool UID_ParseDurationMs(const char *text, int *outMs, std::string *diagMessage)
{
	if (!text || !outMs) {
		UID_SetDiag(diagMessage, "null duration argument");
		return false;
	}

	const char *t = UID_SkipWs(text);
	if (!*t) {
		UID_SetDiag(diagMessage, "empty duration");
		return false;
	}

	std::string trimmed(t);
	while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
		trimmed.pop_back();
	}

	char *parseEnd = nullptr;
	errno = 0;
	const double value = std::strtod(trimmed.c_str(), &parseEnd);
	if (parseEnd == trimmed.c_str() || errno == ERANGE || !std::isfinite(value) || value < 0.0) {
		UID_SetDiag(diagMessage, "invalid duration number");
		return false;
	}

	const char *unit = UID_SkipWs(parseEnd);
	double ms = value;
	if (!*unit || UID_EqIgnoreCase(unit, "s")) {
		ms = value * 1000.0;
	} else if (UID_EqIgnoreCase(unit, "ms")) {
		ms = value;
	} else {
		UID_SetDiag(diagMessage, "unknown duration unit (use s or ms)");
		return false;
	}

	if (ms > static_cast<double>(0x7fffffff)) {
		UID_SetDiag(diagMessage, "duration too large");
		return false;
	}
	*outMs = static_cast<int>(ms + 0.5);
	return true;
}

bool UID_ParseSides(const char *text, uid_sides_t *out, std::string *diagMessage)
{
	if (!text || !out) {
		UID_SetDiag(diagMessage, "null sides argument");
		return false;
	}

	uid_length_t parts[4];
	int          count = 0;
	const char  *p = text;

	while (*p) {
		p = UID_SkipWs(p);
		if (!*p) {
			break;
		}
		const char *start = p;
		while (*p && !std::isspace(static_cast<unsigned char>(*p))) {
			++p;
		}
		if (count >= 4) {
			UID_SetDiag(diagMessage, "sides accept 1, 2, or 4 values");
			return false;
		}
		if (!UID_ParseLengthToken(start, p, &parts[count], diagMessage)) {
			return false;
		}
		++count;
	}

	if (count == 0) {
		UID_SetDiag(diagMessage, "empty sides value");
		return false;
	}
	if (count == 3) {
		UID_SetDiag(diagMessage, "sides reject 3 values; use 1, 2, or 4");
		return false;
	}
	if (count == 1) {
		out->top = out->right = out->bottom = out->left = parts[0];
		return true;
	}
	if (count == 2) {
		out->top = out->bottom = parts[0];
		out->right = out->left = parts[1];
		return true;
	}

	out->top = parts[0];
	out->right = parts[1];
	out->bottom = parts[2];
	out->left = parts[3];
	return true;
}

static int UID_HexNibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return 10 + (c - 'a');
	}
	if (c >= 'A' && c <= 'F') {
		return 10 + (c - 'A');
	}
	return -1;
}

static bool UID_HexByte(const char *p, int *out)
{
	const int hi = UID_HexNibble(p[0]);
	const int lo = UID_HexNibble(p[1]);
	if (hi < 0 || lo < 0) {
		return false;
	}
	*out = (hi << 4) | lo;
	return true;
}

bool UID_ParseColor(const char *text, uid_color_t *out, std::string *diagMessage)
{
	if (!text || !out) {
		UID_SetDiag(diagMessage, "null color argument");
		return false;
	}

	const char *p = UID_SkipWs(text);
	if (*p != '#') {
		UID_SetDiag(diagMessage, "color must be #RRGGBB or #RRGGBBAA");
		return false;
	}
	++p;

	const size_t len = std::strlen(p);
	/* Allow trailing whitespace after hex digits. */
	size_t hexLen = 0;
	while (hexLen < len && !std::isspace(static_cast<unsigned char>(p[hexLen]))) {
		++hexLen;
	}
	for (size_t i = hexLen; i < len; ++i) {
		if (!std::isspace(static_cast<unsigned char>(p[i]))) {
			UID_SetDiag(diagMessage, "trailing garbage in color");
			return false;
		}
	}

	if (hexLen != 6 && hexLen != 8) {
		UID_SetDiag(diagMessage, "color must be #RRGGBB or #RRGGBBAA");
		return false;
	}

	int r = 0, g = 0, b = 0, a = 255;
	if (!UID_HexByte(p + 0, &r) || !UID_HexByte(p + 2, &g) || !UID_HexByte(p + 4, &b)) {
		UID_SetDiag(diagMessage, "invalid hex digits in color");
		return false;
	}
	if (hexLen == 8 && !UID_HexByte(p + 6, &a)) {
		UID_SetDiag(diagMessage, "invalid hex digits in color");
		return false;
	}

	out->r = static_cast<float>(r) / 255.0f;
	out->g = static_cast<float>(g) / 255.0f;
	out->b = static_cast<float>(b) / 255.0f;
	out->a = static_cast<float>(a) / 255.0f;
	return true;
}

bool UID_IsGradientBrush(const char *text)
{
	return UIR_GradientIsBrush(text) != 0;
}

bool UID_IsFillPaint(const char *text)
{
	if (!text || !text[0]) {
		return false;
	}
	if (std::strncmp(text, "cvar-rgba:", 10) == 0) {
		return true;
	}
	if (UID_IsGradientBrush(text)) {
		return true;
	}
	uid_color_t c{};
	return UID_ParseColor(text, &c, nullptr);
}

bool UID_ParseBool(const char *text, bool *out, std::string *diagMessage)
{
	if (!text || !out) {
		UID_SetDiag(diagMessage, "null bool argument");
		return false;
	}

	/* Added in OPM: compare trimmed C string without heap allocation. */
	const char *p = UID_SkipWs(text);
	const char *end = p + std::strlen(p);
	while (end > p && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
		--end;
	}
	const size_t len = static_cast<size_t>(end - p);
	if (len == 4 && (p[0] == 't' || p[0] == 'T') && (p[1] == 'r' || p[1] == 'R') && (p[2] == 'u' || p[2] == 'U')
		&& (p[3] == 'e' || p[3] == 'E')) {
		*out = true;
		return true;
	}
	if (len == 5 && (p[0] == 'f' || p[0] == 'F') && (p[1] == 'a' || p[1] == 'A') && (p[2] == 'l' || p[2] == 'L')
		&& (p[3] == 's' || p[3] == 'S') && (p[4] == 'e' || p[4] == 'E')) {
		*out = false;
		return true;
	}
	/* Exact match path for lowercase (fast common case). */
	if (len == 4 && std::strncmp(p, "true", 4) == 0) {
		*out = true;
		return true;
	}
	if (len == 5 && std::strncmp(p, "false", 5) == 0) {
		*out = false;
		return true;
	}

	UID_SetDiag(diagMessage, "boolean must be true or false");
	return false;
}

bool UID_ParseNumber(const char *text, double *out, std::string *diagMessage)
{
	if (!text || !out) {
		UID_SetDiag(diagMessage, "null number argument");
		return false;
	}

	const char *p = UID_SkipWs(text);
	char       *end = nullptr;
	errno = 0;
	const double value = std::strtod(p, &end);
	if (end == p || errno == ERANGE || !std::isfinite(value)) {
		UID_SetDiag(diagMessage, "invalid number");
		return false;
	}
	end = const_cast<char *>(UID_SkipWs(end));
	if (*end) {
		UID_SetDiag(diagMessage, "trailing garbage in number");
		return false;
	}

	*out = value;
	return true;
}

static int UID_DecimalPlacesForStep(double step)
{
	if (!(step > 0.0) || !std::isfinite(step)) {
		return 0;
	}

	int   places = 0;
	double scaled = step;
	while (places < 12 && std::fabs(scaled - std::round(scaled)) > 1e-6) {
		scaled *= 10.0;
		++places;
	}
	return places;
}

static double UID_SnapToStep(
	double val,
	double minV,
	double maxV,
	double step,
	bool hasMin,
	bool hasMax,
	bool hasStep
)
{
	if (hasMin) {
		val = std::max(minV, val);
	}
	if (hasMax) {
		val = std::min(maxV, val);
	}
	if (hasStep && step > 0.0 && hasMin) {
		val = minV + std::round((val - minV) / step) * step;
		if (hasMin) {
			val = std::max(minV, val);
		}
		if (hasMax) {
			val = std::min(maxV, val);
		}
	}
	return val;
}

static void UID_TrimTrailingFractionZeros(char *s)
{
	if (!s) {
		return;
	}
	char *dot = std::strchr(s, '.');
	if (!dot) {
		return;
	}
	char *end = dot + 1;
	while (*end) {
		++end;
	}
	while (end > dot + 1 && end[-1] == '0') {
		--end;
	}
	if (end == dot + 1) {
		*dot = '\0';
	} else {
		*end = '\0';
	}
}

bool UID_FormatNumberForStep(
	double value,
	double minV,
	double maxV,
	double step,
	bool hasMin,
	bool hasMax,
	bool hasStep,
	char *buf,
	size_t bufSize
)
{
	if (!buf || bufSize == 0) {
		return false;
	}

	const double val = UID_SnapToStep(value, minV, maxV, step, hasMin, hasMax, hasStep);
	if (hasStep && step > 0.0) {
		const int decimals = UID_DecimalPlacesForStep(step);
		std::snprintf(buf, bufSize, "%.*f", decimals, val);
		UID_TrimTrailingFractionZeros(buf);
	} else {
		std::snprintf(buf, bufSize, "%.6g", val);
	}
	return true;
}

bool UID_ParseRotationDeg(const char *text, float *outDeg, std::string *diagMessage)
{
	if (!text || !outDeg) {
		UID_SetDiag(diagMessage, "null rotation argument");
		return false;
	}

	const char *p = UID_SkipWs(text);
	if (!*p) {
		UID_SetDiag(diagMessage, "empty rotation value");
		return false;
	}

	char *end = nullptr;
	const double deg = std::strtod(p, &end);
	if (end == p || !std::isfinite(deg)) {
		UID_SetDiag(diagMessage, "invalid rotation degrees");
		return false;
	}
	p = UID_SkipWs(end);
	if (p[0] == 'd' && p[1] == 'e' && p[2] == 'g') {
		p = UID_SkipWs(p + 3);
	}
	if (*p) {
		UID_SetDiag(diagMessage, "trailing garbage in rotation value");
		return false;
	}

	*outDeg = static_cast<float>(deg);
	return true;
}

bool UID_ParseRotationOrigin(
	const char *text,
	float       boxW,
	float       boxH,
	float       uiPxScale,
	float      *outX,
	float      *outY,
	std::string *diagMessage
)
{
	if (!text || !outX || !outY) {
		UID_SetDiag(diagMessage, "null rotation-origin argument");
		return false;
	}

	const char *p = UID_SkipWs(text);
	if (!*p) {
		*outX = boxW * 0.5f;
		*outY = boxH * 0.5f;
		return true;
	}

	uid_length_t xLen{};
	std::string dm;
	const char *start = p;
	while (*p && !std::isspace(static_cast<unsigned char>(*p))) {
		++p;
	}
	if (!UID_ParseLengthToken(start, p, &xLen, &dm)) {
		UID_SetDiag(diagMessage, dm.empty() ? "invalid rotation-origin x" : dm.c_str());
		return false;
	}
	p = UID_SkipWs(p);
	if (!*p) {
		UID_SetDiag(diagMessage, "rotation-origin requires x and y");
		return false;
	}
	start = p;
	while (*p && !std::isspace(static_cast<unsigned char>(*p))) {
		++p;
	}
	uid_length_t yLen{};
	if (!UID_ParseLengthToken(start, p, &yLen, &dm)) {
		UID_SetDiag(diagMessage, dm.empty() ? "invalid rotation-origin y" : dm.c_str());
		return false;
	}
	p = UID_SkipWs(p);
	if (*p) {
		UID_SetDiag(diagMessage, "trailing garbage in rotation-origin");
		return false;
	}

	if (xLen.unit == UID_LENGTH_PERCENT) {
		*outX = boxW * static_cast<float>(xLen.value) / 100.0f;
	} else {
		*outX = xLen.value * uiPxScale;
	}
	if (yLen.unit == UID_LENGTH_PERCENT) {
		*outY = boxH * static_cast<float>(yLen.value) / 100.0f;
	} else {
		*outY = yLen.value * uiPxScale;
	}
	return true;
}

bool UID_ParseAlign(const char *text, uid_align_t *out, std::string *diagMessage)
{
	if (!text || !out) {
		UID_SetDiag(diagMessage, "null align argument");
		return false;
	}

	const char *p = UID_SkipWs(text);
	std::string token(p);
	while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
		token.pop_back();
	}

	if (token == "start") {
		*out = UID_ALIGN_START;
		return true;
	}
	if (token == "center") {
		*out = UID_ALIGN_CENTER;
		return true;
	}
	if (token == "end") {
		*out = UID_ALIGN_END;
		return true;
	}
	if (token == "equal-spacing" || token == "equal_spacing") {
		*out = UID_ALIGN_EQUAL_SPACING;
		return true;
	}
	if (token == "space-between" || token == "space_between") {
		*out = UID_ALIGN_SPACE_BETWEEN;
		return true;
	}

	UID_SetDiag(diagMessage, "align must be start, center, end, equal-spacing, or space-between");
	return false;
}

bool UID_ParseOverflow(const char *text, uid_overflow_t *out, std::string *diagMessage)
{
	if (!text || !out) {
		UID_SetDiag(diagMessage, "null overflow argument");
		return false;
	}

	const char *p = UID_SkipWs(text);
	std::string token(p);
	while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
		token.pop_back();
	}

	if (token == "none") {
		*out = UID_OVERFLOW_NONE;
		return true;
	}
	if (token == "hidden") {
		*out = UID_OVERFLOW_HIDDEN;
		return true;
	}
	if (token == "scroll") {
		*out = UID_OVERFLOW_SCROLL;
		return true;
	}

	UID_SetDiag(diagMessage, "overflow must be none, hidden, or scroll");
	return false;
}

bool UID_ParseImageFit(const char *text, uid_image_fit_t *out, std::string *diagMessage)
{
	if (!text || !out) {
		UID_SetDiag(diagMessage, "null background-fit argument");
		return false;
	}

	const char *p = UID_SkipWs(text);
	std::string token(p);
	while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
		token.pop_back();
	}

	if (token == "stretch") {
		*out = UID_IMAGE_FIT_STRETCH;
		return true;
	}
	if (token == "repeat") {
		*out = UID_IMAGE_FIT_REPEAT;
		return true;
	}
	if (token == "contain") {
		*out = UID_IMAGE_FIT_CONTAIN;
		return true;
	}
	if (token == "cover") {
		*out = UID_IMAGE_FIT_COVER;
		return true;
	}

	UID_SetDiag(diagMessage, "background-fit must be stretch, repeat, contain, or cover");
	return false;
}

bool UID_ParseScrollbarEdge(const char *text, uid_scrollbar_edge_t *out, std::string *diagMessage)
{
	if (!text || !out) {
		UID_SetDiag(diagMessage, "null scrollbar-edge argument");
		return false;
	}

	const char *p = UID_SkipWs(text);
	std::string token(p);
	while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
		token.pop_back();
	}

	if (token == "content") {
		*out = UID_SCROLLBAR_EDGE_CONTENT;
		return true;
	}
	if (token == "border") {
		*out = UID_SCROLLBAR_EDGE_BORDER;
		return true;
	}

	UID_SetDiag(diagMessage, "scrollbar-edge must be content or border");
	return false;
}

bool UID_ParseAxis(const char *text, uid_layout_axis_t *out, std::string *diagMessage)
{
	if (!text || !out) {
		UID_SetDiag(diagMessage, "null axis argument");
		return false;
	}

	const char *p = UID_SkipWs(text);
	std::string token(p);
	while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
		token.pop_back();
	}

	if (token == "vertical") {
		*out = UID_AXIS_VERTICAL;
		return true;
	}
	if (token == "horizontal") {
		*out = UID_AXIS_HORIZONTAL;
		return true;
	}
	if (token == "overlap") {
		*out = UID_AXIS_OVERLAP;
		return true;
	}

	UID_SetDiag(diagMessage, "axis/type must be vertical, horizontal, or overlap");
	return false;
}

bool UID_NormalizeAttrName(const char *raw, std::string *canonicalOut)
{
	if (!raw || !canonicalOut) {
		return false;
	}

	std::string lower;
	lower.reserve(std::strlen(raw));
	for (const char *p = raw; *p; ++p) {
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
	}

	std::string canonical = lower;
	bool        aliased = false;

	if (lower == "fontsize") {
		canonical = "font-size";
		aliased = true;
	} else if (lower == "fontweight") {
		canonical = "font-weight";
		aliased = true;
	} else if (lower == "width" && std::strcmp(raw, "width") != 0) {
		/* Width / WIDTH -> width */
		canonical = "width";
		aliased = (std::strcmp(raw, "width") != 0);
	} else if (lower == "height" && std::strcmp(raw, "height") != 0) {
		canonical = "height";
		aliased = true;
	}

	/* Any non-canonical casing of a kebab name is an alias for diagnostics. */
	if (!aliased && lower != raw) {
		aliased = true;
		canonical = lower;
	}

	*canonicalOut = canonical;
	return aliased;
}

void uid_property_set_t::Clear()
{
	m_attrs.clear();
	++m_version;
}

bool uid_property_set_t::Has(const char *name) const
{
	if (!name) {
		return false;
	}
	return m_attrs.find(name) != m_attrs.end();
}

bool uid_property_set_t::Get(const char *name, std::string *valueOut) const
{
	if (!name || !valueOut) {
		return false;
	}
	const auto it = m_attrs.find(name);
	if (it == m_attrs.end()) {
		return false;
	}
	*valueOut = it->second.value;
	return true;
}

const char *uid_property_set_t::GetCStr(const char *name, const char *fallback) const
{
	if (!name) {
		return fallback;
	}
	const auto it = m_attrs.find(name);
	if (it == m_attrs.end()) {
		return fallback;
	}
	return it->second.value.c_str();
}

void uid_property_set_t::Set(const char *name, const char *value)
{
	if (!name) {
		return;
	}
	uid_prop_entry_t &e = m_attrs[name];
	e.value = value ? value : "";
	e.cacheValid = 0;
	e.cacheOk = 0;
	++m_version;
}

void uid_property_set_t::Set(const char *name, const std::string &value)
{
	if (!name) {
		return;
	}
	uid_prop_entry_t &e = m_attrs[name];
	e.value = value;
	e.cacheValid = 0;
	e.cacheOk = 0;
	++m_version;
}

void uid_property_set_t::MergeFrom(const uid_property_set_t &other)
{
	for (const auto &kv : other.m_attrs) {
		uid_prop_entry_t &e = m_attrs[kv.first];
		e.value = kv.second.value;
		e.cacheValid = 0;
		e.cacheOk = 0;
	}
	++m_version;
}

const char *UID_BuiltinDefault(const char *canonicalName)
{
	if (!canonicalName) {
		return nullptr;
	}

	/* Documented built-ins: transparent, visible, enabled, vertical, start-aligned,
	 * zero spacing, overflow none, intrinsic/auto size. */
	if (std::strcmp(canonicalName, "fill") == 0) {
		return "#00000000";
	}
	if (std::strcmp(canonicalName, "visible") == 0) {
		return "true";
	}
	if (std::strcmp(canonicalName, "enabled") == 0) {
		return "true";
	}
	if (std::strcmp(canonicalName, "type") == 0) {
		return "vertical";
	}
	if (std::strcmp(canonicalName, "halign") == 0) {
		return "start";
	}
	if (std::strcmp(canonicalName, "valign") == 0) {
		return "start";
	}
	if (std::strcmp(canonicalName, "gap") == 0) {
		return "0";
	}
	if (std::strcmp(canonicalName, "padding") == 0) {
		return "0";
	}
	if (std::strcmp(canonicalName, "margin") == 0) {
		return "0";
	}
	if (std::strcmp(canonicalName, "overflow") == 0) {
		return "none";
	}
	if (std::strcmp(canonicalName, "width") == 0) {
		return "auto";
	}
	if (std::strcmp(canonicalName, "height") == 0) {
		return "auto";
	}
	return nullptr;
}

bool uid_property_set_t::GetColorCached(const char *name, uid_color_t *out) const
{
	if (!name || !out) {
		return false;
	}
	const auto it = m_attrs.find(name);
	if (it == m_attrs.end()) {
		return false;
	}
	const uid_prop_entry_t &e = it->second;
	if (UID_OptEnabled(UID_OPT_PARSE_CACHE) && (e.cacheValid & UID_PROP_CACHE_COLOR)) {
		if (e.cacheOk & UID_PROP_CACHE_COLOR) {
			*out = e.color;
			return true;
		}
		return false;
	}
	const bool ok = UID_ParseColor(e.value.c_str(), &e.color, nullptr);
	e.cacheValid |= UID_PROP_CACHE_COLOR;
	if (ok) {
		e.cacheOk |= UID_PROP_CACHE_COLOR;
		*out = e.color;
	} else {
		e.cacheOk &= ~UID_PROP_CACHE_COLOR;
	}
	return ok;
}

bool uid_property_set_t::GetLengthCached(const char *name, uid_length_t *out) const
{
	if (!name || !out) {
		return false;
	}
	const auto it = m_attrs.find(name);
	if (it == m_attrs.end()) {
		return false;
	}
	const uid_prop_entry_t &e = it->second;
	if (UID_OptEnabled(UID_OPT_PARSE_CACHE) && (e.cacheValid & UID_PROP_CACHE_LENGTH)) {
		if (e.cacheOk & UID_PROP_CACHE_LENGTH) {
			*out = e.length;
			return true;
		}
		return false;
	}
	const bool ok = UID_ParseLength(e.value.c_str(), &e.length, nullptr);
	e.cacheValid |= UID_PROP_CACHE_LENGTH;
	if (ok) {
		e.cacheOk |= UID_PROP_CACHE_LENGTH;
		*out = e.length;
	} else {
		e.cacheOk &= ~UID_PROP_CACHE_LENGTH;
	}
	return ok;
}

bool uid_property_set_t::GetNumberCached(const char *name, double *out) const
{
	if (!name || !out) {
		return false;
	}
	const auto it = m_attrs.find(name);
	if (it == m_attrs.end()) {
		return false;
	}
	const uid_prop_entry_t &e = it->second;
	if (UID_OptEnabled(UID_OPT_PARSE_CACHE) && (e.cacheValid & UID_PROP_CACHE_NUMBER)) {
		if (e.cacheOk & UID_PROP_CACHE_NUMBER) {
			*out = e.number;
			return true;
		}
		return false;
	}
	const bool ok = UID_ParseNumber(e.value.c_str(), &e.number, nullptr);
	e.cacheValid |= UID_PROP_CACHE_NUMBER;
	if (ok) {
		e.cacheOk |= UID_PROP_CACHE_NUMBER;
		*out = e.number;
	} else {
		e.cacheOk &= ~UID_PROP_CACHE_NUMBER;
	}
	return ok;
}

bool uid_property_set_t::GetBoolCached(const char *name, bool *out) const
{
	if (!name || !out) {
		return false;
	}
	const auto it = m_attrs.find(name);
	if (it == m_attrs.end()) {
		return false;
	}
	const uid_prop_entry_t &e = it->second;
	if (UID_OptEnabled(UID_OPT_PARSE_CACHE) && (e.cacheValid & UID_PROP_CACHE_BOOL)) {
		if (e.cacheOk & UID_PROP_CACHE_BOOL) {
			*out = e.boolean;
			return true;
		}
		return false;
	}
	const bool ok = UID_ParseBool(e.value.c_str(), &e.boolean, nullptr);
	e.cacheValid |= UID_PROP_CACHE_BOOL;
	if (ok) {
		e.cacheOk |= UID_PROP_CACHE_BOOL;
		*out = e.boolean;
	} else {
		e.cacheOk &= ~UID_PROP_CACHE_BOOL;
	}
	return ok;
}

bool uid_property_set_t::GetEnumCached(const char *name, uid_prop_enum_kind_t kind, int *out) const
{
	if (!name || !out || kind == UID_PROP_ENUM_NONE) {
		return false;
	}
	const auto it = m_attrs.find(name);
	if (it == m_attrs.end()) {
		return false;
	}
	const uid_prop_entry_t &e = it->second;
	if (UID_OptEnabled(UID_OPT_PARSE_CACHE) && (e.cacheValid & UID_PROP_CACHE_ENUM) && e.enumKind == static_cast<uint8_t>(kind)) {
		if (e.cacheOk & UID_PROP_CACHE_ENUM) {
			*out = e.enumValue;
			return true;
		}
		return false;
	}
	bool ok = false;
	int value = 0;
	switch (kind) {
	case UID_PROP_ENUM_ALIGN: {
		uid_align_t a = UID_ALIGN_START;
		ok = UID_ParseAlign(e.value.c_str(), &a, nullptr);
		value = static_cast<int>(a);
		break;
	}
	case UID_PROP_ENUM_OVERFLOW: {
		uid_overflow_t o = UID_OVERFLOW_NONE;
		ok = UID_ParseOverflow(e.value.c_str(), &o, nullptr);
		value = static_cast<int>(o);
		break;
	}
	case UID_PROP_ENUM_IMAGE_FIT: {
		uid_image_fit_t f = UID_IMAGE_FIT_STRETCH;
		ok = UID_ParseImageFit(e.value.c_str(), &f, nullptr);
		value = static_cast<int>(f);
		break;
	}
	case UID_PROP_ENUM_AXIS: {
		uid_layout_axis_t ax = UID_AXIS_VERTICAL;
		ok = UID_ParseAxis(e.value.c_str(), &ax, nullptr);
		value = static_cast<int>(ax);
		break;
	}
	case UID_PROP_ENUM_SCROLLBAR_EDGE: {
		uid_scrollbar_edge_t edge = UID_SCROLLBAR_EDGE_CONTENT;
		ok = UID_ParseScrollbarEdge(e.value.c_str(), &edge, nullptr);
		value = static_cast<int>(edge);
		break;
	}
	default:
		break;
	}
	e.enumKind = static_cast<uint8_t>(kind);
	e.enumValue = value;
	e.cacheValid |= UID_PROP_CACHE_ENUM;
	if (ok) {
		e.cacheOk |= UID_PROP_CACHE_ENUM;
		*out = value;
	} else {
		e.cacheOk &= ~UID_PROP_CACHE_ENUM;
	}
	return ok;
}

const uid_prop_entry_t *UID_BuiltinDefaultParsed(const char *canonicalName)
{
	static std::map<std::string, uid_prop_entry_t> s_parsed;
	const char *raw = UID_BuiltinDefault(canonicalName);
	if (!raw) {
		return nullptr;
	}
	auto it = s_parsed.find(canonicalName);
	if (it != s_parsed.end()) {
		return &it->second;
	}
	uid_prop_entry_t e;
	e.value = raw;
	auto inserted = s_parsed.emplace(canonicalName, std::move(e));
	return &inserted.first->second;
}

void UID_ApplyBuiltinDefaults(uid_property_set_t *out)
{
	if (!out) {
		return;
	}
	/*
	 * Changed in OPM: do not stamp halign/valign onto every node.
	 * Layout still falls back to "start" via UID_BuiltinDefault / PropCStr.
	 * Leaf text (buttons, etc.) uses kind-specific defaults when unset.
	 */
	static const char *const kNames[] = {
		"fill", "visible", "enabled", "type",
		"gap", "padding", "margin", "overflow", "width", "height"
	};
	for (const char *name : kNames) {
		out->Set(name, UID_BuiltinDefault(name));
	}
}
