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

/*
 * Added in Omaha: headless stub for UI design/bench targets that link
 * uir_compositor without the full model-preview backend.
 */

#include "../uir_modelpreview.h"

void UIR_ModelPreviewSetBackend(const uir_modelpreview_backend_t *backend)
{
	(void)backend;
}

uir_status_t UIR_ModelPreviewDraw(const uir_rect_t *destPx, const uir_model_preview_params_t *params)
{
	(void)destPx;
	(void)params;
	return UIR_OK;
}
