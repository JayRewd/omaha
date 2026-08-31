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
#ifndef UIR_MENUWORLD_H
#define UIR_MENUWORLD_H

#include "uir_menu_map_view.h"
#include "uir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	UIR_MW_UNLOADED = 0,
	UIR_MW_SWITCHING,
	UIR_MW_READY,
	UIR_MW_UNAVAILABLE,
	UIR_MW_NEEDS_RELOAD
} uir_menuworld_state_t;

typedef struct {
	/* Renderer */
	void (*clearWorld)(void);
	void (*loadMenuWorld)(const char *name);
	int (*loadMenuWorldStaged)(const char *name);
	void (*commitMenuWorld)(void);
	void (*cancelMenuWorldStaging)(void);
	int (*hasActiveWorld)(void);
	void (*setWorldVisData)(const unsigned char *vis);
	void (*clearScene)(void);
	void (*renderScene)(const void *refdef); /* refdef_t* */
	void (*setColor)(const float *rgba);
	void (*drawBox)(float x, float y, float w, float h);
	void (*set2DWindow)(int x, int y, int w, int h, float left, float right, float bottom, float top, float n, float f);
	void (*scissor)(int x, int y, int w, int h);

	/* Collision / FS */
	int (*fileExists)(const char *path);
	void (*cmLoadMap)(const char *name, int clientload, int *checksum);
	const unsigned char *(*cmVisibilityPointer)(void);
	const char *(*cmEntityString)(void);
	void (*cmModelBoundsFromName)(const char *name, float mins[3], float maxs[3]);
	long (*readFile)(const char *path, void **buffer);
	void (*freeFile)(void *buffer);

	/* Renderer helpers for menu weather */
	int (*registerShader)(const char *name);
	void (*addPolyToScene)(int shader, int numVerts, const void *verts, int renderfx);

	/* Math */
	void (*anglesToAxis)(const float angles[3], float axis[3][3]);

	float (*cvarFloat)(const char *name, float fallback);
	int (*cvarInteger)(const char *name, int fallback);

	void (*printf)(const char *fmt, ...);
} uir_menuworld_backend_t;

void UIR_MenuWorldSetBackend(const uir_menuworld_backend_t *backend);
void UIR_MenuWorldShutdown(void);
/* Fixed in OPM: clearWorld only while menu UI owns the renderer world slot. */
void UIR_MenuWorldReleaseOwnership(void);
void UIR_MenuWorldMarkNeedsReload(void);
uir_menuworld_state_t UIR_MenuWorldState(void);

/* Added in OPM: select active catalog view (from sources.xml via host). */
void UIR_MenuWorldSetDesiredView(const uir_menu_map_view_t *view);

/* Lazy load / advance staged switch; safe to call every frame. */
uir_status_t UIR_MenuWorldEnsureLoaded(void);

/*
 * Draw backdrop into dest rect (framebuffer pixels). Uses cls-independent
 * realtime passed by caller. On unavailable, draws solid fallback box.
 */
uir_status_t UIR_MenuWorldDraw(const uir_rect_t *destPx, int realtime);

#ifdef __cplusplus
}
#endif

#endif /* UIR_MENUWORLD_H */
