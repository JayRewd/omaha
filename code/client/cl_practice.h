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

#pragma once

void CL_Practice_Refresh(void);
void CL_Practice_Play(int edit);
qboolean CL_Practice_New(void); // Added in Omaha: qfalse if create refused (e.g. name exists)
void CL_Practice_StartHost(void);
int  CL_Practice_QueryScenarios(
     int offset, int limit, int *outTotal, uint64_t *outRevision, struct uid_collection_item_s *out, int max
 );
int CL_Practice_QueryMaps(
	int offset, int limit, int *outTotal, uint64_t *outRevision, struct uid_collection_item_s *out, int max
);

// Added in Omaha: in-game scenario editor (cl_practice_editor.cpp).
void CL_PracticeEditor_Init(void);
void CL_PracticeEditor_Frame(void);
int  CL_PracticeEditor_QueryNodes(
	 int offset, int limit, int *outTotal, uint64_t *outRevision, struct uid_collection_item_s *out, int max
 );
