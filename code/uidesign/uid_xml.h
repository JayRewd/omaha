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
#ifndef UID_XML_H
#define UID_XML_H

#include "uid_diag.h"
#include "uid_document.h"
#include "uid_types.h"

#include <stddef.h>

/* Added in OPM: optional VFS reader for <import> resolution during parse. */
typedef struct uid_parse_io_s {
	long (*readFile)(const char *path, void **buf);
	void (*freeFile)(void *buf);
} uid_parse_io_t;

/*
 * Parse XML bytes into a NEW document. Does not mutate a live runtime document.
 * On failure, *outDoc is left empty/cleared and diagnostics describe the errors.
 * io may be NULL; <import> then fails at compile time.
 */
uid_result_t UID_ParseXml(
	const char *sourceName,
	const char *xml,
	size_t size,
	const uid_limits_t *limits,
	const uid_parse_io_t *io,
	uid_document_t *outDoc,
	uid_diag_list_t *diags
);

/* Added in OPM: sync collection/index fields from properties after template expand. */
void UID_ApplyCollectionAndIndexFields(uid_node_def_t *node);

/* Added in OPM: lightweight menu registration metadata from <definitions> attributes. */
typedef struct uid_menu_meta_s {
	char                 menuId[64];
	int                  drawOrder;
	uid_menu_backdrop_t  backdrop;
	bool                 valid;
} uid_menu_meta_t;

uid_result_t UID_PeekMenuMetadata(
	const char *vfsPath,
	const uid_parse_io_t *io,
	uid_menu_meta_t *out,
	uid_diag_list_t *diags
);

/* Added in OPM: HUD pack registration metadata from <definitions hud-id hud-label draw-order>. */
/* Changed in Omaha: pause-menu / scoreboard-menu companions are required on HUD packs. */
typedef struct uid_hud_meta_s {
	char hudId[64];
	char hudLabel[96];
	char pauseMenu[64];
	char scoreboardMenu[64];
	int  drawOrder;
	bool valid;
} uid_hud_meta_t;

uid_result_t UID_PeekHudMetadata(
	const char *vfsPath,
	const uid_parse_io_t *io,
	uid_hud_meta_t *out,
	uid_diag_list_t *diags
);

#endif /* UID_XML_H */
