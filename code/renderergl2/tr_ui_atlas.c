/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2024-2026 openmohaa contributors

This file is part of Quake III Arena source code / openmohaa.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
/* Added in OPM: dynamic RGBA atlas materials for modern UI (GL2). */
#include "tr_local.h"

qhandle_t RE_RegisterShaderFromImage(const char *name, int lightmapIndex, image_t *image, qboolean mipRawImage);

/*
====================
RE_UpdateUIAtlas
====================
*/
qboolean RE_UpdateUIAtlas(qhandle_t hShader, const byte *rgba, int width, int height)
{
	shader_t *sh;
	image_t  *image;

	if (!rgba || width <= 0 || height <= 0) {
		return qfalse;
	}

	sh = R_GetShaderByHandle(hShader);
	if (!sh || sh == tr.defaultShader || sh->defaultShader) {
		return qfalse;
	}

	if (!sh->stages[0] || !sh->stages[0]->bundle[0].image[0]) {
		return qfalse;
	}

	image = sh->stages[0]->bundle[0].image[0];
	if (image->width != width || image->height != height) {
		return qfalse;
	}

	R_IssuePendingRenderCommands();
	GL_BindToTMU(image, 0);
	qglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	return qtrue;
}

/*
====================
RE_CreateUIAtlas

Creates (or updates) a clamp / no-mip / no-picmip linear RGBA atlas shader.
====================
*/
qhandle_t RE_CreateUIAtlas(const char *name, const byte *rgba, int width, int height)
{
	shader_t  *sh;
	image_t   *image;
	qhandle_t  hShader;
	imgFlags_t flags;

	if (!name || !name[0] || !rgba || width <= 0 || height <= 0) {
		return 0;
	}

	if (strlen(name) >= MAX_QPATH) {
		ri.Printf(PRINT_WARNING, "RE_CreateUIAtlas: name too long '%s'\n", name);
		return 0;
	}

	sh = R_FindShaderByName(name);
	if (sh && sh != tr.defaultShader && !sh->defaultShader) {
		if (sh->stages[0] && sh->stages[0]->bundle[0].image[0]) {
			image = sh->stages[0]->bundle[0].image[0];
			if (image->width == width && image->height == height) {
				if (!RE_UpdateUIAtlas(sh->index, rgba, width, height)) {
					return 0;
				}
				return sh->index;
			}
			/* Existing atlas with a different size cannot be resized in place. */
			return 0;
		}
	}

	R_IssuePendingRenderCommands();
	flags = (imgFlags_t)(IMGFLAG_CLAMPTOEDGE | IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE);
	image = R_CreateImage(name, (byte *)rgba, width, height, IMGTYPE_COLORALPHA, flags, 0);
	if (!image) {
		return 0;
	}

	hShader = RE_RegisterShaderFromImage(name, LIGHTMAP_2D, image, qfalse);
	return hShader;
}
