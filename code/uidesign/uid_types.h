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
#ifndef UID_TYPES_H
#define UID_TYPES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

typedef enum {
	UID_OK = 0,
	UID_ERR_INVALID_ARG,
	UID_ERR_OVERFLOW,
	UID_ERR_PARSE,
	UID_ERR_VALIDATE,
	UID_ERR_IO,
	UID_ERR_LIMIT,
	UID_ERR_NOT_READY
} uid_result_t;

typedef enum {
	UID_SEVERITY_ERROR = 0,
	UID_SEVERITY_WARNING,
	UID_SEVERITY_INFO
} uid_severity_t;

typedef struct {
	const char *path; /* optional; may be nullptr */
	int         line;
	int         column;
} uid_source_location_t;

typedef enum {
	UID_NODE_CONTAINER = 0,
	UID_NODE_LABEL,
	UID_NODE_BUTTON,
	UID_NODE_INPUT,
	UID_NODE_TOGGLE,
	UID_NODE_SLIDER,
	UID_NODE_SLIDER_TRACK, /* Added in OPM: composed slider part */
	UID_NODE_SLIDER_RANGE, /* Added in OPM: composed slider part */
	UID_NODE_SLIDER_THUMB, /* Added in OPM: composed slider part */
	UID_NODE_SCROLLBAR,       /* Added in OPM: scroll container chrome root */
	UID_NODE_SCROLLBAR_TRACK, /* Added in OPM: scrollbar rail background */
	UID_NODE_SCROLLBAR_THUMB, /* Added in OPM: scrollbar draggable handle */
	UID_NODE_SELECT,
	UID_NODE_OPTION,
	UID_NODE_KEYBIND,
	UID_NODE_SHAPE_INSTANCE,
	UID_NODE_IMAGE,       /* Added in OPM: leaf bitmap (intrinsic size; not registry <images>) */
	UID_NODE_MODEL,       /* Added in OPM: player model preview */
	UID_NODE_SERVER_LIST, /* Added in OPM: host-drawn server browser region (deprecated) */
	UID_NODE_FOREACH,     /* Added in OPM: composable collection row template */
	UID_NODE_USE,
	UID_NODE_ON,
	UID_NODE_SET,
	UID_NODE_SET_CVAR,
	UID_NODE_INVOKE,
	UID_NODE_SHOW_MODAL, /* Added in OPM: sets ui_om_modal (or modal-cvar) */
	UID_NODE_HIDE_MODAL, /* Added in OPM: clears modal dispatch cvar */
	UID_NODE_PROP,
	UID_NODE_FONT_DEF,
	UID_NODE_SHAPE_DEF,
	UID_NODE_TEMPLATE_DEF,
	UID_NODE_PATH_DEF,
	UID_NODE_DEFAULTS,
	UID_NODE_CANVAS,
	UID_NODE_UI,
	UID_NODE_DEFINITIONS,
	UID_NODE_FONTS,
	UID_NODE_IMAGES,
	UID_NODE_SHAPES,
	UID_NODE_TEMPLATES,
	UID_NODE_PROPS
} uid_node_kind_t;

typedef enum {
	UID_AXIS_VERTICAL = 0,
	UID_AXIS_HORIZONTAL,
	UID_AXIS_OVERLAP
} uid_layout_axis_t;

typedef enum {
	UID_ALIGN_START = 0,
	UID_ALIGN_CENTER,
	UID_ALIGN_END,
	UID_ALIGN_EQUAL_SPACING,
	UID_ALIGN_SPACE_BETWEEN /* CSS space-between: free space only between children */
} uid_align_t;

typedef enum {
	UID_OVERFLOW_NONE = 0,
	UID_OVERFLOW_HIDDEN,
	UID_OVERFLOW_SCROLL
} uid_overflow_t;

/* Added in OPM: trailing-edge anchor for overflow=scroll scrollbar chrome. */
typedef enum {
	UID_SCROLLBAR_EDGE_CONTENT = 0, /* inside padding (content box trailing edge) */
	UID_SCROLLBAR_EDGE_BORDER       /* border box trailing edge */
} uid_scrollbar_edge_t;

/* Added in OPM: menu dispatcher backdrop mode (definitions menu-id documents). */
typedef enum {
	UID_MENU_BACKDROP_NONE = 0,
	UID_MENU_BACKDROP_MENU_MAP
} uid_menu_backdrop_t;

typedef enum {
	UID_PROP_STRING = 0,
	UID_PROP_NUMBER,
	UID_PROP_LENGTH,
	UID_PROP_COLOR,
	UID_PROP_BOOLEAN,
	UID_PROP_BINDING,
	UID_PROP_IDENTIFIER
} uid_prop_type_t;

typedef enum {
	UID_EVENT_CLICK = 0,
	UID_EVENT_CHANGE,
	UID_EVENT_SUBMIT,
	UID_EVENT_CANCEL,
	UID_EVENT_FOCUS,
	UID_EVENT_BLUR,
	UID_EVENT_DBLCLICK
} uid_event_kind_t;

typedef enum {
	UID_COMMIT_CHANGE = 0,
	UID_COMMIT_SUBMIT,
	UID_COMMIT_APPLY
} uid_commit_mode_t;

typedef enum {
	UID_LENGTH_PX = 0,
	UID_LENGTH_PERCENT,
	UID_LENGTH_FILL,
	UID_LENGTH_AUTO
} uid_length_unit_t;

typedef enum {
	UID_DIRTY_NONE      = 0,
	UID_DIRTY_STRUCTURE = 1 << 0,
	UID_DIRTY_LAYOUT    = 1 << 1,
	UID_DIRTY_PAINT     = 1 << 2,
	UID_DIRTY_BINDING   = 1 << 3,
	UID_DIRTY_ALL       = (UID_DIRTY_STRUCTURE | UID_DIRTY_LAYOUT | UID_DIRTY_PAINT | UID_DIRTY_BINDING)
} uid_dirty_flags_t;

typedef int uid_node_id_t;
#define UID_INVALID_NODE_ID (-1)

typedef struct {
	float x;
	float y;
	float w;
	float h;
} uid_rect_t;

typedef struct {
	float r;
	float g;
	float b;
	float a;
} uid_color_t;

typedef enum {
	UID_IMAGE_FIT_STRETCH = 0,
	UID_IMAGE_FIT_REPEAT,
	UID_IMAGE_FIT_CONTAIN,
	UID_IMAGE_FIT_COVER
} uid_image_fit_t;

typedef struct {
	size_t maxXmlBytes;
	int    maxXmlDepth;
	int    maxParsedNodes;
	int    maxExpandedNodes;
	int    maxFonts;
	int    maxImages;
	int    maxShapes;
	int    maxTemplates;
	int    maxTemplateDepth;
	int    maxIdLen;
	int    maxPropNameLen;
	int    maxFontPathLen;
	int    maxPathBytes;
	int    maxExprBytes;
	int    maxExprNodes;
	int    maxTextBytes;
	int    maxActionsPerHandler;
	int    maxOptionsPerSelect;
	int    maxDiagnostics;
	int    maxImportDepth; /* Added in OPM */
	int    maxImportFiles; /* Added in OPM */
	int    maxVars;        /* Added in OPM */
} uid_limits_t;

static inline void UID_DefaultLimits(uid_limits_t *out)
{
	if (!out) {
		return;
	}
	out->maxXmlBytes          = 1024u * 1024u; /* 1 MiB */
	out->maxXmlDepth          = 64;
	out->maxParsedNodes       = 4096;
	out->maxExpandedNodes     = 8192;
	out->maxFonts             = 256;
	out->maxImages            = 256;
	out->maxShapes            = 256;
	out->maxTemplates         = 256;
	out->maxTemplateDepth     = 32;
	out->maxIdLen             = 128;
	out->maxPropNameLen       = 128;
	out->maxFontPathLen       = 256;
	out->maxPathBytes         = 65536;
	out->maxExprBytes         = 1024;
	out->maxExprNodes         = 128;
	out->maxTextBytes         = 65536;
	out->maxActionsPerHandler = 32;
	out->maxOptionsPerSelect  = 512;
	out->maxDiagnostics       = 256;
	out->maxImportDepth       = 16;
	out->maxImportFiles       = 64;
	out->maxVars              = 512;
}

typedef struct {
	float x;
	float y;
	int   buttons;
	int   wheel;
	bool  moved;
} uid_pointer_state_t;

#ifdef __cplusplus
}
#endif

#endif /* UID_TYPES_H */
