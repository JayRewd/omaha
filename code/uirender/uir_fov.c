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

#include "uir_fov.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define UIR_DEG2RAD(a) ((a) * (float)M_PI / 180.0f)
#define UIR_RAD2DEG(a) ((a) * 180.0f / (float)M_PI)

uir_status_t UIR_CalcWorldFov(int width, int height, float baseFovX_4_3, float *outFovX, float *outFovY)
{
	float aspect;
	float fovRatio;
	float fovX;
	float viewDistance;
	float fovY;

	if (!outFovX || !outFovY || width <= 0 || height <= 0 || !(baseFovX_4_3 > 0.0f)) {
		return UIR_ERR_INVALID_ARG;
	}

	aspect = (float)width / (float)height;
	fovRatio = aspect * (3.0f / 4.0f);

	if (fabsf(fovRatio - 1.0f) < 1e-5f) {
		fovX = baseFovX_4_3;
	} else {
		fovX = 2.0f * UIR_RAD2DEG(atanf(tanf(UIR_DEG2RAD(baseFovX_4_3 * 0.5f)) * fovRatio));
	}

	viewDistance = (float)width / tanf(UIR_DEG2RAD(fovX * 0.5f));
	fovY = 2.0f * UIR_RAD2DEG(atan2f((float)height, viewDistance));

	*outFovX = fovX;
	*outFovY = fovY;
	return UIR_OK;
}

uir_status_t UIR_CalcPreviewFovY(int width, int height, float fovX, float *outFovY)
{
	if (!outFovY || width <= 0 || height <= 0 || !(fovX > 0.0f)) {
		return UIR_ERR_INVALID_ARG;
	}

	/* fov_y = 2 * degrees(atan(tan(radians(fov_x/2)) * (h/w))) */
	*outFovY = 2.0f * UIR_RAD2DEG(atanf(tanf(UIR_DEG2RAD(fovX * 0.5f)) * ((float)height / (float)width)));
	return UIR_OK;
}
