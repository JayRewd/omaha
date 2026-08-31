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
#ifndef UID_VALUE_H
#define UID_VALUE_H

#include "uid_types.h"

#include <cstdint>
#include <map>
#include <string>

struct uid_length_t {
	uid_length_unit_t unit;
	float             value; /* unused for fill/auto */
};

struct uid_sides_t {
	uid_length_t top;
	uid_length_t right;
	uid_length_t bottom;
	uid_length_t left;
};

/* Added in OPM: which enum family GetEnumCached last parsed for this entry. */
typedef enum uid_prop_enum_kind_e {
	UID_PROP_ENUM_NONE = 0,
	UID_PROP_ENUM_ALIGN,
	UID_PROP_ENUM_OVERFLOW,
	UID_PROP_ENUM_IMAGE_FIT,
	UID_PROP_ENUM_AXIS,
	UID_PROP_ENUM_SCROLLBAR_EDGE
} uid_prop_enum_kind_t;

/* Added in OPM: parsed-value memo so paint/layout stop re-parsing text each frame. */
enum {
	UID_PROP_CACHE_COLOR = 1u << 0,
	UID_PROP_CACHE_LENGTH = 1u << 1,
	UID_PROP_CACHE_NUMBER = 1u << 2,
	UID_PROP_CACHE_BOOL = 1u << 3,
	UID_PROP_CACHE_ENUM = 1u << 4
};

struct uid_prop_entry_t {
	std::string          value;
	mutable uint32_t     cacheValid = 0; /* bitmask per parsed kind */
	mutable uint32_t     cacheOk = 0;    /* parse succeeded per kind */
	mutable uid_color_t  color{};
	mutable uid_length_t length{};
	mutable double       number = 0.0;
	mutable bool         boolean = false;
	mutable int          enumValue = 0;
	mutable uint8_t      enumKind = 0;
};

/* Optional diagnostic message out-parameter may be nullptr. */
bool UID_ParseLength(const char *text, uid_length_t *out, std::string *diagMessage);
bool UID_ParseSides(const char *text, uid_sides_t *out, std::string *diagMessage);
/* Added in OPM: duration for foreach lifetime / fade ("5", "5s", "500ms"). */
bool UID_ParseDurationMs(const char *text, int *outMs, std::string *diagMessage);

bool UID_ParseColor(const char *text, uid_color_t *out, std::string *diagMessage);
/* Added in OPM: fill may be #RRGGBB(AA) or linear(...)/radial(...) atlas brush. */
bool UID_IsGradientBrush(const char *text);
bool UID_IsFillPaint(const char *text); /* color, cvar-rgba, or gradient brush */
bool UID_ParseBool(const char *text, bool *out, std::string *diagMessage);
bool UID_ParseNumber(const char *text, double *out, std::string *diagMessage);
/* Added in OPM: snap/format numeric control values using authored step. */
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
);
/* Added in OPM: shape-rotation degrees ("90", "90deg", "-45deg"). */
bool UID_ParseRotationDeg(const char *text, float *outDeg, std::string *diagMessage);
bool UID_ParseRotationOrigin(
	const char *text,
	float       boxW,
	float       boxH,
	float       uiPxScale,
	float      *outX,
	float      *outY,
	std::string *diagMessage
);
bool UID_ParseAlign(const char *text, uid_align_t *out, std::string *diagMessage);
bool UID_ParseOverflow(const char *text, uid_overflow_t *out, std::string *diagMessage);
bool UID_ParseImageFit(const char *text, uid_image_fit_t *out, std::string *diagMessage);
bool UID_ParseScrollbarEdge(const char *text, uid_scrollbar_edge_t *out, std::string *diagMessage);
bool UID_ParseAxis(const char *text, uid_layout_axis_t *out, std::string *diagMessage);

/*
 * Lowercases and remaps compatibility aliases:
 * fontsize/fontweight -> font-size/font-weight, Width/Height -> width/height.
 * Returns true when the canonical name differs from the input (alias used).
 */
bool UID_NormalizeAttrName(const char *raw, std::string *canonicalOut);

class uid_property_set_t {
public:
	void Clear();
	bool Empty() const { return m_attrs.empty(); }
	size_t Size() const { return m_attrs.size(); }

	bool Has(const char *name) const;
	bool Get(const char *name, std::string *valueOut) const;
	const char *GetCStr(const char *name, const char *fallback = nullptr) const;

	void Set(const char *name, const char *value);
	void Set(const char *name, const std::string &value);
	/* Added in OPM: later keys override earlier ones. */
	void MergeFrom(const uid_property_set_t &other);

	const std::map<std::string, uid_prop_entry_t> &Attrs() const { return m_attrs; }
	/* Added in OPM: bumps on Set/Clear/MergeFrom for shape resolve cache keys. */
	unsigned Version() const { return m_version; }

	/* Added in OPM: typed accessors with per-entry parse memo (Stage 3). */
	bool GetColorCached(const char *name, uid_color_t *out) const;
	bool GetLengthCached(const char *name, uid_length_t *out) const;
	bool GetNumberCached(const char *name, double *out) const;
	bool GetBoolCached(const char *name, bool *out) const;
	bool GetEnumCached(const char *name, uid_prop_enum_kind_t kind, int *out) const;

private:
	std::map<std::string, uid_prop_entry_t> m_attrs;
	unsigned                                m_version = 0;
};

/* Built-in defaults: transparent, visible, enabled, vertical, start, zero spacing, overflow none, auto size. */
const char *UID_BuiltinDefault(const char *canonicalName);
/* Added in OPM: builtin defaults are constant, so parse them once. */
const uid_prop_entry_t *UID_BuiltinDefaultParsed(const char *canonicalName);
void        UID_ApplyBuiltinDefaults(uid_property_set_t *out);

#endif /* UID_VALUE_H */
