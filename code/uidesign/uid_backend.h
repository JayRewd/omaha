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
#ifndef UID_BACKEND_H
#define UID_BACKEND_H

#include "uid_types.h"

#include <stddef.h>

#ifdef __cplusplus
#include <cstdint>
extern "C" {
#else
#include <stdbool.h>
#include <stdint.h>
#endif

/* Added in OPM: dynamic collection items for foreach / composable lists. */
typedef struct uid_collection_item_s {
	const char *key;
	const char *value;
	const char *label;
	int         nfields;
	const char **fieldNames;
	const char **fieldValues;
	uint32_t    flags;
} uid_collection_item_t;

typedef struct uid_collection_query_s {
	const char *source;
	int         offset;
	int         limit;
	int        *outTotal;
	uint64_t   *outRevision;
} uid_collection_query_t;

/* Added in OPM: descriptor for compositor model previews (modern <model> tag). */
typedef struct uid_model_preview_desc_s {
	float       x, y, w, h;
	const char *model;
	const char *anim;
	const char *team;
	const char *instanceKey;
	int         animVariant;
	float       animPhase;
	int         hasAnimPhase;
	float       angles[3];
	int         hasAngles;
	float       offset[3];
	int         hasOffset;
	float       bboxMins[3];
	float       bboxMaxs[3];
	int         hasBbox;
	float       fov;
	int         hasFov;
	float       scale;
	int         hasScale;
	float       framingScale;
	int         hasFramingScale;
	float       color[4];
	int         hasColor;
	int         bboxFromModel;
	void       *userdata;
} uid_model_preview_desc_t;

/*
 * Injected host capabilities for the UI design runtime.
 * All function pointers are optional except alloc/free for unit tests.
 * Production adapters fill VFS, cvar, keybind, font, and draw entry points.
 */
typedef struct uid_backend_s {
	void *(*alloc)(size_t size);
	void (*free)(void *ptr);

	long (*readFile)(const char *path, void **buf);
	void (*freeFile)(void *buf);

	/* cvar */
	bool (*cvarDescribe)(const char *name, int *flags, char *valueBuf, size_t valueBufSize);
	bool (*cvarWrite)(const char *name, const char *value);
	bool (*cvarReset)(const char *name);
	/* Added in OPM: monotonic epoch bumped when any cvar value changes. */
	unsigned (*cvarEpoch)(void);

	/* keybind */
	bool (*keyNameToNum)(const char *name, int *key);
	bool (*keyNumToName)(int key, char *out, size_t outSize);
	bool (*getBinding)(int key, char *out, size_t outSize);
	bool (*setBinding)(int key, const char *binding);
	int (*findConflicts)(const char *binding, int *keysOut, int maxKeys);
	bool (*getKeysForCommand)(const char *command, int *key1, int *key2);

	/* options */
	int (*queryOptions)(const char *source, char **values, char **labels, int max);

	int (*queryCollectionItems)(const uid_collection_query_t *query, uid_collection_item_t *out, int max);

	/* named actions */
	bool (*invokeAction)(const char *name, void *userdata);

	/* fonts via UIR */
	void *(*fontResolve)(const char *vfsPath, float logicalPx, float fbScale);
	float (*fontMeasure)(void *font, const char *text);
	float (*fontAscent)(void *font); /* typographic ascent (px); used for cap-optical valign */
	void (*fontDraw)(void *font, float x, float y, const char *text, const float *rgba, float tracking);
	/*
	 * Optional CSS skewX-style shear. skewTan = tan(degrees); originY is the
	 * transform origin in draw space (typically content vertical center).
	 * When null, host uses fontDraw with no shear.
	 */
	void (*fontDrawSkewed)(
		void *font,
		float x,
		float y,
		const char *text,
		const float *rgba,
		float skewTan,
		float originY,
		float tracking
	);

	/* draw */
	void (*drawSolidRect)(float x, float y, float w, float h, const float *rgba);
	/*
	 * svgD is in viewBox space (viewW x viewH). Dest is draw-space (x,y,w,h).
	 * Pass viewW/viewH <= 0 to use dest w/h as the viewBox (1:1 path units).
	 * fillRgba NULL or a<=0 skips fill; strokeRgba NULL skips stroke.
	 */
	void (*drawPath)(
		const char *svgD,
		float x,
		float y,
		float w,
		float h,
		float viewW,
		float viewH,
		const float *fillRgba,
		const float *strokeRgba,
		float strokeWidthPx,
		float rotationDeg,
		int crisp /* Added in OPM: binary coverage / no soft AA */
	);
	/*
	 * Bitmap background from the image registry (.png / .tga).
	 * clipPathD/viewW/viewH follow drawPath semantics; pass clipPathCount=0 for
	 * axis-aligned scissor-only clipping (plain rectangles).
	 */
	void (*drawImage)(
		const char *vfsPath,
		float x,
		float y,
		float w,
		float h,
		const char *const *clipPathD,
		int clipPathCount,
		float viewW,
		float viewH,
		int fit,
		float rotationDeg,
		float backgroundScale,
		const float *tintRgba
	);
	/*
	 * Added in OPM: texel size for leaf <image> intrinsic / aspect layout.
	 * outW/outH are native shader texels (aspect = outW/outH). Optional.
	 */
	bool (*imageMeasure)(const char *vfsPath, float *outW, float *outH);
	/*
	 * Added in OPM: atlas-baked gradient fill (linear/radial brush string).
	 * clipPathD semantics match drawImage; tintRgba applies opacity/modulate.
	 */
	void (*drawGradient)(
		const char *brush,
		float x,
		float y,
		float w,
		float h,
		const char *const *clipPathD,
		int clipPathCount,
		float viewW,
		float viewH,
		float rotationDeg,
		const float *tintRgba
	);
	void (*pushClip)(float x, float y, float w, float h);
	void (*popClip)(void);

	/*
	 * Added in OPM: clip subsequent draws to SVG shape path(s) (stencil when available).
	 * beginShapeClip returns true if a clip was activated (pair with endShapeClip).
	 * Nested clips are rejected (returns false) — outer clip remains active.
	 */
	bool (*beginShapeClip)(
		float x,
		float y,
		float w,
		float h,
		const char *const *pathD,
		int pathCount,
		float viewW,
		float viewH,
		float rotationDeg
	);
	void (*endShapeClip)(void);

	/*
	 * Added in OPM: soft mask coverage for subsequent draws (UI layer RT).
	 * beginImageMask returns true if activated (pair with endImageMask). Nested masks fail.
	 * vfsPathOrBrush is a VFS image path or linear(...)/radial(...) gradient brush.
	 * fit matches uid_image_fit_t / uir_image_fit_t (stretch/contain/cover; gradients force stretch).
	 */
	bool (*beginImageMask)(float x, float y, float w, float h, const char *vfsPathOrBrush, int fit);
	void (*endImageMask)(void);

	/*
	 * Added in OPM: queue a 3D model preview for the chrome→preview compositor phase.
	 * Rect is in logical draw units; the client converts to FB pixels if needed.
	 */
	void (*queueModelPreview)(const uid_model_preview_desc_t *desc);

	/* Added in OPM: host-owned region paint (e.g. server-list). Rect is logical. */
	void (*drawHostRegion)(const char *role, float x, float y, float w, float h, void *userdata);

	/* Added in OPM: hi-res UI scale and framebuffer size for crosshair preview parity. */
	bool (*getHiResScale)(float *scaleX, float *scaleY);
	bool (*getFramebufferSize)(int *width, int *height);

	/*
	 * Added in OPM: pointer routed into a host region. localX/Y are relative to the
	 * region border box. Returns true if the event was consumed.
	 */
	bool (*hostRegionPointer)(
		const char *role,
		float localX,
		float localY,
		int buttons,
		int wheel,
		void *userdata
	);

	/* diagnostics */
	void (*diag)(int severity, const char *path, int line, const char *msg, void *userdata);

	void *userdata;
} uid_backend_t;

#ifdef __cplusplus
}
#endif

#endif /* UID_BACKEND_H */
