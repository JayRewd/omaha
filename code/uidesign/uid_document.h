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
#ifndef UID_DOCUMENT_H
#define UID_DOCUMENT_H

#include "uid_types.h"
#include "uid_value.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct uid_prop_decl_t {
	std::string    name;
	uid_prop_type_t type;
	bool           required;
	std::string    defaultValue;
};

struct uid_font_def_t {
	std::string id;
	std::string src;
	int         weight;
};

struct uid_image_def_t {
	std::string id;
	std::string src;
};

struct uid_path_def_t {
	std::string           fillExpr;
	std::string           d;
	uid_source_location_t loc;
};

struct uid_shape_def_t {
	std::string                 id;
	bool                        hasIntrinsicSize;
	float                       width;
	float                       height;
	std::string                 fit; /* contain/stretch optional */
	std::vector<uid_prop_decl_t> props;
	std::vector<uid_path_def_t> paths;
};

/* Added in Omaha: resolved shape path for paint (also cached on node state). */
struct uid_resolved_path_t {
	std::string d;
	uid_color_t fill;
};

struct uid_action_t {
	uid_node_kind_t       kind; /* SET / SET_CVAR / INVOKE */
	std::string           target;
	std::string           property;
	std::string           value;
	std::string           name;
	uid_source_location_t loc;
};

struct uid_action_handler_t {
	uid_event_kind_t           event;
	std::vector<uid_action_t>  actions;
	uid_source_location_t      loc;
};

struct uid_select_option_t {
	std::string value;
	std::string label;
};

/* Added in Omaha: host-backed collection item cached on scope nodes. */
struct uid_collection_entry_t {
	std::string                        key;
	std::string                        value;
	std::string                        label;
	std::map<std::string, std::string> fields;
};

struct uid_node_def_t {
	uid_node_kind_t                kind;
	uid_source_location_t          source;
	std::string                    id;
	uid_property_set_t             properties;
	std::string                    text;
	std::vector<uid_node_id_t>     children;
	std::vector<uid_action_handler_t> handlers;

	/* select */
	std::vector<uid_select_option_t> options;
	std::string                    optionSource;
	std::string                    appearance; /* empty|dropdown = overlay; cyclic = prev/next */
	std::string                    openModal; /* Added in Omaha: modal= id opens relative/fullscreen modal instead of procedural overlay */

	/* Added in Omaha: bind value transforms / visibility / two-button set-value */
	std::string                    valueType;  /* percent|invert-mouse|pitch-magnitude|cm360|display-mode */
	std::string                    visibleIf;  /* cvar:name=value (legacy) */
	std::string                    enabledIf;   /* cvar:name=value (legacy) */
	std::string                    visibleExpr; /* {bool expr} inner text */
	std::string                    enabledExpr; /* {bool expr} inner text */
	bool                           visibleExprBound;
	bool                           enabledExprBound;
	/* Added in Omaha: one-time migration of static visible="{...}" into visibleExpr. */
	bool                           visibleExprProbed;
	bool                           enabledExprProbed;
	/* Added in Omaha: style ternary exprs per property (inner of attr="{cond ? a : b}") */
	std::map<std::string, std::string> styleExprs;
	std::string                    setValue;   /* button writes this to bind on click */
	/* Added in Omaha: property values that are exactly {cvar:name} and sync each frame */
	std::map<std::string, std::string> cvarBoundProps;
	/* Added in Omaha: runtime numeric {expr} property bindings (not exact cvar passthrough) */
	std::map<std::string, std::string> exprBoundProps;
	/* Added in Omaha: cached "does this node need binding work?" bits. */
	unsigned                       bindingFlags;
	bool                           bindingFlagsValid;

	/* use */
	std::string                    templateId;
	bool                           deferredUse; /* template resolved after foreach expand */
	bool                           deferredUseExpanded;

	/* shape instance */
	std::string                    shapeId;

	/* model preview (Added in Omaha) */
	std::string                    modelPath;
	std::string                    team; /* optional allies|axis */
	std::string                    anim; /* optional; empty → team idle at paint */
	float                          animPhase; /* [0,1) of clip length when hasAnimPhase */
	bool                           hasAnimPhase;
	int                            animVariant;
	bool                           hasAnimVariant;
	float                          modelAngles[3];
	bool                           hasModelAngles;
	float                          modelOffset[3];
	bool                           hasModelOffset;
	float                          bboxMins[3];
	float                          bboxMaxs[3];
	bool                           hasBbox;
	bool                           bboxFromModel;
	float                          modelFov;
	bool                           hasModelFov;
	float                          modelScale;
	bool                           hasModelScale;
	float                          framingScale;
	bool                           hasFramingScale;
	float                          modelColor[4];
	bool                           hasModelColor;

	/* server-list / host region (Added in Omaha) */
	std::string                    role; /* e.g. server-list */

	/* Added in Omaha: collection scope (source= on container) */
	std::string                    collectionSource;
	/* Added in Omaha: Phase 4.4 — 0 unresolved, 1 XML <sources>, 2 host provider. */
	int                            collectionSourceClass;
	int                            collectionHostId;
	std::string                    collectionDisplay; /* label|value for {item.display} */
	int                            collectionDefaultIndex;
	bool                           hasCollectionDefaultIndex;
	std::string                    indexBind;
	bool                           collectionWrap;
	bool                           collectionScroll;

	/* Added in Omaha: foreach row template */
	std::string                    foreachMode; /* all|selected|window */
	std::string                    foreachCountExpr; /* count="{...}" inner or full authored value */
	bool                           hasForeachCount;
	float                          foreachRowHeight;
	bool                           hasForeachRowHeight;
	/* Added in Omaha: opt-in row age / fade on <foreach> (ms). */
	bool                           hasForeachLifetime;
	int                            foreachLifetimeMs;
	int                            foreachFadeDurationMs;
	std::vector<uid_node_def_t>    foreachTemplateNodes;
	uid_node_id_t                  foreachTemplateRoot;
	uid_node_id_t                  foreachScopeId; /* scope for expanded instance */
	int                            foreachItemIndex;
	bool                           foreachGenerated;

	/* Added in Omaha: templated scrollbar chrome on overflow=scroll containers */
	std::string                    scrollbarTemplateId;
	bool                           scrollbarGenerated;

	/* Added in Omaha: index actions on buttons */
	int                            stepIndex;
	bool                           hasStepIndex;
	int                            setIndexValue;
	bool                           hasSetIndex;
	std::string                    visibleIfIndex;

	/* input */
	std::string                    inputType; /* text|number */

	/* keybind */
	std::string                    binding;
	std::string                    confirmModal; /* Added in Omaha: definitions modal id on conflict */
	std::string                    modalCvar;    /* Added in Omaha: dispatch cvar (default ui_om_modal) */
	int                            bindSlot;     /* Added in Omaha: 0 primary, 1 secondary */

	/* slider / numeric bounds */
	double                         minValue;
	double                         maxValue;
	double                         stepValue;
	bool                           hasMin;
	bool                           hasMax;
	bool                           hasStep;

	/* bind / commit */
	std::string                    bind;
	uid_commit_mode_t              commit;
	bool                           hasCommit;
};

/*
 * Template body is a separate node forest cloned on expand.
 * rootNode indexes into nodes (template-local IDs).
 */
struct uid_template_def_t {
	std::string                  id;
	std::vector<uid_prop_decl_t> props;
	uid_node_id_t                rootNode;
	std::vector<uid_node_def_t>  nodes;
};

/* Added in Omaha: XML-authored collection source definitions. */
struct uid_source_item_def_t {
	std::string                        value;
	std::string                        label;
	std::map<std::string, std::string> fields;
};

struct uid_source_def_t {
	std::string                    id;
	std::string                    defaultValue;
	std::vector<uid_source_item_def_t> items;
};

/* Added in Omaha: static design-token definitions (<vars><var id= value=>). */
struct uid_var_def_t {
	std::string id;
	std::string value;
};

/* Added in Omaha: modal definitions (non-rendering until mounted via cvar). */
struct uid_modal_def_t {
	std::string                  id;
	std::string                  type; /* empty = fullscreen dialog; relative = opener-anchored panel */
	uid_node_id_t                rootNode;
	std::vector<uid_node_def_t>  nodes;
};

struct uid_keybind_pending_t {
	bool          active;
	uid_node_id_t nodeId;
	int           slot;
	int           newKey;
	std::string   command;
};

struct uid_definition_tables_t {
	uid_property_set_t                      defaults;
	std::map<std::string, uid_font_def_t>   fonts;
	std::map<std::string, uid_image_def_t>  images;
	std::map<std::string, uid_shape_def_t>  shapes;
	std::map<std::string, uid_template_def_t> templates;
	std::map<std::string, uid_modal_def_t>   modals;
	std::map<std::string, uid_source_def_t>  sources;
	std::map<std::string, uid_var_def_t>     vars;
};

struct uid_optional_value_t {
	bool        hasValue;
	std::string stringValue;
};

/* Added in Omaha: Stage 3 computed paint style (ui_style_cache). */
struct uid_computed_style_t {
	unsigned    propsVersion;
	unsigned    scaleEpoch;
	unsigned    cvarEpoch;
	unsigned    interactionKey;
	bool        valid;
	/* Added in Omaha: only invalidate on cvarEpoch when paint reads live cvars. */
	bool        dependsOnCvar;

	bool        hasFill;
	bool        hasGradient;
	uid_color_t fill;
	std::string gradientBrush;

	bool        hasStroke;
	uid_color_t stroke;
	float       strokeWidthPx;

	char        shapeName[64];
	bool        rectShape;
	bool        isEdgeClip;

	float       pathRotationDeg;
	float       bgRotationDeg;

	uid_color_t textColor;
};

struct uid_node_state_t {
	bool                hovered;
	bool                pressed;
	bool                focused;
	bool                effectivelyEnabled;
	uid_optional_value_t runtimeValue;
	/* Added in Omaha: commit=apply staged by user input (not auto sync/fallback). */
	bool                applyUserEdited;
	std::string         editBuffer;
	std::string         preEditValue; /* Escape restores this for inputs */
	size_t              caretCodepoint;
	size_t              anchorCodepoint;
	float               scrollX;
	float               scrollY;
	float               contentExtentW;
	float               contentExtentH;
	uid_rect_t          marginBox;
	uid_rect_t          borderBox;
	uid_rect_t          contentBox;
	uid_rect_t          effectiveClip;
	bool                capturing;     /* keybind capture mode */
	bool                overlayOpen;   /* legacy select flag; relative modals use ui_om_modal */
	bool                dragging;      /* slider drag */
	int                 highlightIndex; /* select highlight */

	/* Added in Omaha: collection scope runtime */
	std::vector<uid_collection_entry_t> collectionItems;
	int                 collectionSelectedIndex;
	int                 collectionItemCount;
	uint64_t            collectionRevision;
	int                 collectionScrollOffset;
	/* Added in Omaha: syncFrameCounter when RefreshCollectionScope last ran. */
	int                 collectionRefreshFrame;

	/* Added in Omaha: foreach expansion cache (skip rebuild when unchanged). */
	uint64_t            foreachExpandSig;
	/*
	 * Fixed in Omaha: scope identity the last expand consumed. Skip paths must
	 * prove the rows match the scope as it is now, not merely that the host did
	 * not change this frame (hidden-while-loaded and selection-only changes).
	 */
	bool                foreachStamped;
	uint64_t            foreachStampRev;
	int                 foreachStampCount;
	int                 foreachStampSize;
	int                 foreachStampSel;
	int                 foreachStampScroll;
	/* Added in Omaha: first-seen time (doc updateTimeMs) per item.key when lifetime is set. */
	std::unordered_map<std::string, int> foreachAppearAtMs;
	/* Added in Omaha: per-row opacity mul from foreach lifetime fade (wrap roots). */
	float               lifetimeOpacityMul;
	/* Added in Omaha: Phase 4.3 — row wrapper has a valid LIVE_OPACITY cache. */
	bool                liveOpacityCached;

	/* Added in Omaha: scrollbar chrome layout / drag (on scroll container). */
	uid_rect_t          scrollbarTrackRect;
	uid_rect_t          scrollbarThumbRect;
	bool                scrollbarDragging;
	float               scrollbarDragOffset;
	bool                scrollbarVisible;

	/* Added in Omaha: per-node text measurement cache (layout + paint alignment). */
	void               *cachedFont;
	float               cachedTextWidth;
	uint64_t            cachedTextKey;     /* font id + weight + size + scales (ResolveFont) */
	uint64_t            cachedMeasureKey;  /* text + font params (measure cache) */
	/* Added in Omaha: cvar-pure visible/enabled result memo keyed by cvar epoch. */
	unsigned            visibleEpoch;
	bool                visibleCached;
	bool                visibleCachedValue;
	unsigned            enabledEpoch;
	bool                enabledCached;
	bool                enabledCachedValue;
	/* Added in Omaha: skip SyncBoundStyleExprs when all styleExprs are cvar-pure and epoch matches. */
	unsigned            styleExprEpoch;
	bool                styleExprCached;

	/*
	 * Added in Omaha Stage 6b: skip full bind body when cvar-pure work is unchanged.
	 * bindSyncEpoch tracks Cvar_GlobalModCount; bindPrimaryModCount / labelCvarModCount
	 * allow per-cvar early-outs when other HUD cvars still churn the global epoch.
	 */
	unsigned            bindSyncEpoch;
	bool                bindSyncCached;
	unsigned            bindPrimaryModCount;
	unsigned            labelCvarModCount;
	unsigned            cvarPropsModStamp;
	/*
	 * Added in Omaha: foreach-generated nodes skip item.* text/prop rebind while
	 * the enclosing collection scope revision and this row's foreachItemIndex are
	 * unchanged. Field refresh bumps revision; mode=selected rebind updates index
	 * in place without expand teardown.
	 */
	uint64_t            itemBindRevision;
	int                 itemBindItemIndex;
	/* Added in Omaha: Phase 4.4 — last collectionRevision consumed by bind deps. */
	uint64_t            bindDepsSeenCollectionRev;
	/*
	 * Fixed in Omaha: last collectionSelectedIndex consumed by bind deps. Selection
	 * steps rebind mode=selected rows in place without a revision bump.
	 */
	int                 bindDepsSeenCollectionSel;
	/* Added in Omaha: Phase 4.4 — last RefreshCollectionScope was a no-op this frame. */
	bool                collectionRefreshUnchanged;

	/* Added in Omaha: cached UID_ResolveShape output. */
	std::vector<uid_resolved_path_t> cachedShapePaths;
	unsigned long long               cachedShapeKey;
	bool                             cachedShapeValid;

	/*
	 * Added in Omaha: last LayoutNode inputs (Stage 2 scoped layout). Valid after a
	 * successful layout of this node; used to re-run LayoutNode from a boundary.
	 */
	float               layoutMarginX;
	float               layoutMarginY;
	float               layoutMarginW;
	float               layoutMarginH;
	float               layoutPercentBaseW;
	float               layoutPercentBaseH;
	uid_rect_t          layoutParentClip;
	bool                layoutAncestorVisible;
	bool                layoutAncestorEnabled;
	bool                layoutInputsValid;
	/* Added in Omaha: last LayoutNode authored size units were non-auto (px/%/fill). */
	bool                layoutSizedW;
	bool                layoutSizedH;

	/* Added in Omaha: Stage 3 computed style cache. */
	uid_computed_style_t computedStyle;
};

/* Per-document pointer/modifier edge state (owned; not a global map). */
struct uid_input_scratch_t {
	bool          shiftDown;
	int           lastButtons;
	uid_node_id_t pressNode;
	unsigned int  lastClickTime;
	uid_node_id_t lastClickNode;
	float         lastClickX;
	float         lastClickY;
};

struct uid_document_t {
	std::string                                  sourceName;
	/* Added in Omaha: registerable menu metadata from <definitions menu-id draw-order backdrop>. */
	bool                                         hasMenuMeta;
	std::string                                  menuId;
	int                                          drawOrder;
	uid_menu_backdrop_t                          menuBackdrop;
	/* Added in Omaha: optional canvas pointer="{bool expr}"; empty = no cursor ownership. */
	std::string                                  pointerExpr;
	uid_limits_t                                 limits;
	uid_definition_tables_t                      definitions;
	std::vector<uid_node_def_t>                  nodes; /* canvas / expanded runtime tree */
	uid_node_id_t                                rootNode;
	std::vector<uid_node_state_t>                states;
	std::unordered_map<std::string, uid_node_id_t> idIndex;
	uid_dirty_flags_t                            dirty;
	bool                                         expanded;
	float                                        lastFbScale; /* from last UID_LayoutDocument */
	float                                        lastUiPxScale; /* authored px multiplier; Added in Omaha */
	int                                          lastLogicalW; /* viewport width from last layout */
	int                                          lastLogicalH; /* viewport height from last layout */
	uid_input_scratch_t                          inputScratch;
	/* Added in Omaha: last UID_Update realtime (ms) for foreach lifetime / fade. */
	int                                          updateTimeMs;
	/* Added in Omaha: skip UID_ApplyCollectionAndIndexFields until structure changes. */
	bool                                         collectionFieldsApplied;
	/* Added in Omaha: incremented each UID_SyncBindings for collection refresh stamps. */
	int                                          syncFrameCounter;
	/* Added in Omaha: translate-x/y deltas applied without full-document layout. */
	struct translate_delta_t {
		uid_node_id_t nodeId;
		float         dx;
		float         dy;
	};
	std::vector<translate_delta_t>               pendingTranslateDeltas;

	/* Added in Omaha: cvar-dispatched modal overlay (nodes appended to nodes/states). */
	std::string                                  activeModalId;
	size_t                                       modalOverlayBase;
	uid_node_id_t                                modalRootNode;
	/* Added in Omaha: node that triggered show-modal / select modal= (relative placement anchor). */
	uid_node_id_t                                modalOpenerNode;
	uid_keybind_pending_t                        keybindPending;

	/* Added in Omaha: nodes that contributed UID_DIRTY_LAYOUT (Stage 2 scoped layout). */
	std::vector<uid_node_id_t>                   dirtyLayoutNodes;

	/* Added in Omaha Stage 4: retained chrome paint list (opaque; uid_paint.cpp). */
	void                                        *paintList;
	/* Added in Omaha: Phase 4.1 per-region retained paint (opaque vector in uid_paint.cpp). */
	std::vector<uid_node_id_t>                   parentOf;
	std::vector<int>                             regionOf;
	bool                                         regionsStale;
	void                                        *paintRegions;
	/* Added in Omaha: Phase 4.4 — cvar dependency index for targeted bind sync. */
	std::vector<std::string>                     depCvars;
	std::vector<std::vector<uid_node_id_t>>      depNodes;
	std::vector<unsigned>                        depLastMod;
	std::vector<void *>                          depCvarPtrs;
	std::vector<unsigned char>                   depAffectsVisible;
	/* Added in Omaha: Phase 4.4 — cvar only appears in translate-x/y. */
	std::vector<unsigned char>                   depTranslateOnly;
	std::vector<uid_node_id_t>                   impureNodes;
	bool                                         bindDepsStale;
	bool                                         bindDepsWarm;
	size_t                                       bindDepsNodeCount;
	/* Added in Omaha: Phase 4.4 — reused per-frame scratch (avoid heap in FRAME_BIND). */
	std::vector<unsigned char>                   bindTouchedScratch;
	std::vector<unsigned char>                   visMemoScratch;
};

void UID_InitNodeDef(uid_node_def_t *node);
void UID_InitNodeState(uid_node_state_t *state);

uid_document_t *UID_CreateDocument(void);
void            UID_DestroyDocument(uid_document_t *doc);
void            UID_ClearDocument(uid_document_t *doc);

/*
 * Added in Omaha: OR dirty flags; when LAYOUT is newly set, attribute the first
 * transition for ui_profile (nodeId may be UID_INVALID_NODE_ID; reason optional).
 */
void UID_MarkDirty(uid_document_t *doc, uid_dirty_flags_t flags, uid_node_id_t nodeId, const char *reason);
const char *UID_NodeKindName(uid_node_kind_t kind);

/* Look up an expanded (or parsed canvas) node by id string. */
uid_node_def_t *UID_GetNodeById(uid_document_t *doc, const char *id);
const uid_node_def_t *UID_GetNodeById(const uid_document_t *doc, const char *id);

uid_node_def_t *UID_GetNode(uid_document_t *doc, uid_node_id_t id);
const uid_node_def_t *UID_GetNode(const uid_document_t *doc, uid_node_id_t id);

/*
 * Resolve a font definition by id, preferring an entry whose weight matches
 * requestedWeight. Falls back to the id match, then any weight match, then null.
 */
const uid_font_def_t *UID_FindFontDef(const uid_document_t *doc, const char *fontId, int requestedWeight);

/* Added in Omaha: slider/input min/max/step from properties (after template expand). */
bool UID_IsSliderPartKind(uid_node_kind_t kind);
bool UID_IsScrollbarPartKind(uid_node_kind_t kind);
bool UID_SyncSliderBounds(uid_node_def_t *node);
bool UID_SyncInputBounds(uid_node_def_t *node);
uid_node_id_t UID_FindChildOfKind(const uid_document_t *doc, uid_node_id_t parent, uid_node_kind_t kind);

#endif /* UID_DOCUMENT_H */
