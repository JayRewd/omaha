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

#include "uir_modelpreview.h"
#include "uir_fov.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define UIR_MP_HALF_ANGLE 0.268f
#define UIR_DEG2RAD(a) ((a) * (float)M_PI / 180.0f)

void UIR_ModelPreviewComputeFraming(float w, float h, float *outScale, float outOffset[3])
{
	float aspect;
	float scale;
	float dist;
	float halfFovY;
	float worldH;
	float tallBias;
	float zBias;

	if (outScale) {
		*outScale = 1.0f;
	}
	if (outOffset) {
		outOffset[0] = 60.0f;
		outOffset[1] = 0.0f;
		outOffset[2] = 0.0f;
	}
	if (!(w > 1.0f) || !(h > 1.0f)) {
		return;
	}

	aspect = h / w;
	if (aspect < 1.0f) {
		aspect = 1.0f;
	}
	scale = 1.0f / (aspect * 1.08f);

	dist = 96.0f * scale * 0.5f / UIR_MP_HALF_ANGLE;
	halfFovY = atanf(tanf(UIR_DEG2RAD(15.0f)) * (h / w));
	worldH = 2.0f * dist * tanf(halfFovY);
	tallBias = 1.0f - 1.0f / aspect;
	if (tallBias < 0.0f) {
		tallBias = 0.0f;
	}
	zBias = -worldH * (0.5f * tallBias + 0.04f);

	if (outScale) {
		*outScale = scale;
	}
	if (outOffset) {
		outOffset[0] = 60.0f;
		outOffset[1] = 0.0f;
		outOffset[2] = zBias;
	}
}

void UIR_ModelPreviewComputeOrigin(const float mins[3], const float maxs[3], float framingScale, float outOrigin[3])
{
	static const float kDefaultMins[3] = {-16, -16, 0};
	static const float kDefaultMaxs[3] = {16, 16, 96};
	const float       *useMins = mins ? mins : kDefaultMins;
	const float       *useMaxs = maxs ? maxs : kDefaultMaxs;
	float              height;

	if (!outOrigin) {
		return;
	}
	if (!(framingScale > 0.0f)) {
		framingScale = 1.0f;
	}
	height = useMaxs[2] - useMins[2];
	if (height < useMaxs[1] - useMins[1]) {
		height = useMaxs[1] - useMins[1];
	}
	if (height < useMaxs[0] - useMins[0]) {
		height = useMaxs[0] - useMins[0];
	}
	outOrigin[1] = (useMins[1] + useMaxs[1]) * 0.5f;
	outOrigin[2] = (useMins[2] + useMaxs[2]) * -0.5f;
	outOrigin[0] = height * framingScale * 0.5f / UIR_MP_HALF_ANGLE;
}

float UIR_ModelPreviewBBoxExtent(const float mins[3], const float maxs[3])
{
	static const float kDefaultMins[3] = {-16, -16, 0};
	static const float kDefaultMaxs[3] = {16, 16, 96};
	const float       *useMins = mins ? mins : kDefaultMins;
	const float       *useMaxs = maxs ? maxs : kDefaultMaxs;
	float              height;

	height = useMaxs[2] - useMins[2];
	if (height < useMaxs[1] - useMins[1]) {
		height = useMaxs[1] - useMins[1];
	}
	if (height < useMaxs[0] - useMins[0]) {
		height = useMaxs[0] - useMins[0];
	}
	return height;
}

void UIR_ModelPreviewShiftBounds(
	const float mins[3], const float maxs[3], const float delta[3], float outMins[3], float outMaxs[3]
)
{
	int i;

	if (!mins || !maxs || !outMins || !outMaxs) {
		return;
	}
	for (i = 0; i < 3; i++) {
		outMins[i] = mins[i] + (delta ? delta[i] : 0.0f);
		outMaxs[i] = maxs[i] + (delta ? delta[i] : 0.0f);
	}
}

void UIR_ModelPreviewAxisTransformBounds(
	const float mins[3], const float maxs[3], const float axis[3][3], float outMins[3], float outMaxs[3]
)
{
	int   i, j, k, n;
	float corner[3];
	float rotated[3];
	float useMins[3];
	float useMaxs[3];

	if (!mins || !maxs || !axis || !outMins || !outMaxs) {
		return;
	}

	useMins[0] = mins[0];
	useMins[1] = mins[1];
	useMins[2] = mins[2];
	useMaxs[0] = maxs[0];
	useMaxs[1] = maxs[1];
	useMaxs[2] = maxs[2];

	outMins[0] = outMins[1] = outMins[2] = 1e30f;
	outMaxs[0] = outMaxs[1] = outMaxs[2] = -1e30f;

	/* Entity transform: R*p = p0*axis[0] + p1*axis[1] + p2*axis[2] (not VectorRotate). */
	for (i = 0; i < 2; i++) {
		corner[0] = i ? useMaxs[0] : useMins[0];
		for (j = 0; j < 2; j++) {
			corner[1] = j ? useMaxs[1] : useMins[1];
			for (k = 0; k < 2; k++) {
				corner[2] = k ? useMaxs[2] : useMins[2];
				rotated[0] =
					axis[0][0] * corner[0] + axis[1][0] * corner[1] + axis[2][0] * corner[2];
				rotated[1] =
					axis[0][1] * corner[0] + axis[1][1] * corner[1] + axis[2][1] * corner[2];
				rotated[2] =
					axis[0][2] * corner[0] + axis[1][2] * corner[1] + axis[2][2] * corner[2];
				for (n = 0; n < 3; n++) {
					if (rotated[n] < outMins[n]) {
						outMins[n] = rotated[n];
					}
					if (rotated[n] > outMaxs[n]) {
						outMaxs[n] = rotated[n];
					}
				}
			}
		}
	}
}

void UIR_ModelPreviewComputeOriginShared(
	const float mins[3],
	const float maxs[3],
	float framingScale,
	float sharedExtent,
	const float axis[3][3],
	float outOrigin[3]
)
{
	static const float kDefaultMins[3] = {-16, -16, 0};
	static const float kDefaultMaxs[3] = {16, 16, 96};
	const float       *useMins = mins ? mins : kDefaultMins;
	const float       *useMaxs = maxs ? maxs : kDefaultMaxs;
	float              height;
	float              rmins[3];
	float              rmaxs[3];

	if (!outOrigin) {
		return;
	}
	if (!(framingScale > 0.0f)) {
		framingScale = 1.0f;
	}

	if (axis) {
		UIR_ModelPreviewAxisTransformBounds(useMins, useMaxs, axis, rmins, rmaxs);
		/* True mid on view axes; distance still from unoriented/shared extent. */
		outOrigin[1] = (rmins[1] + rmaxs[1]) * -0.5f;
		outOrigin[2] = (rmins[2] + rmaxs[2]) * -0.5f;
	} else {
		outOrigin[1] = (useMins[1] + useMaxs[1]) * 0.5f;
		outOrigin[2] = (useMins[2] + useMaxs[2]) * -0.5f;
	}

	if (sharedExtent > 0.0f) {
		height = sharedExtent;
	} else {
		height = UIR_ModelPreviewBBoxExtent(useMins, useMaxs);
	}
	outOrigin[0] = height * framingScale * 0.5f / UIR_MP_HALF_ANGLE;
}

uir_status_t UIR_ModelPreviewCalcFov(int w, int h, float fovXIn, float *fovX, float *fovY)
{
	float fov;

	if (!fovX || !fovY) {
		return UIR_ERR_INVALID_ARG;
	}
	fov = fovXIn > 0.0f ? fovXIn : UIR_MP_FOV_DEFAULT;
	*fovX = fov;
	return UIR_CalcPreviewFovY(w, h, fov, fovY);
}

void UIR_ModelPreviewWrapAnimTime(float *animTime, float animLength)
{
	if (!animTime || animLength <= 0.0f) {
		return;
	}
	while (*animTime >= animLength) {
		*animTime -= animLength;
	}
	while (*animTime < 0.0f) {
		*animTime += animLength;
	}
}

float UIR_ModelPreviewPhaseToAnimTime(float animPhase, float animLength)
{
	float t;

	if (animLength <= 0.0f) {
		return 0.0f;
	}
	t = animPhase * animLength;
	UIR_ModelPreviewWrapAnimTime(&t, animLength);
	return t;
}
