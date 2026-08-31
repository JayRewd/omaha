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

#include "../uir_fov.h"
#include "../uir_viewport.h"
#include "../uir_path.h"
#include "../uir_svg.h"
#include "../uir_font.h"
#include "../uir_gradient.h"
#include "../uir_image.h"
#include "../uir_modelpreview.h"
#include "../uir_menuworld.h"
#include "../uir_menu_map_view.h"
#include "../uir_map_env.h"
#include "../uir_menu_weather.h"
#include "../uir_compositor.h"
#include "../uir_draw2d.h"
#include "../uir_batch.h"
#include "../uir_tess.h"
#include "../../renderercommon/tr_types.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
void QDECL Com_Printf(const char *msg, ...)
{
	(void)msg;
}
void QDECL Com_Error(int level, const char *error, ...)
{
	(void)level;
	(void)error;
	std::exit(1);
}
}

static int g_failures = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
			g_failures++;                                                                                              \
		}                                                                                                              \
	} while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((double)(a) - (double)(b)) < (eps))


static int g_boxDrawCount = 0;

static void test_set_color(const float *rgba)
{
	(void)rgba;
}

static void test_draw_box(float x, float y, float w, float h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	g_boxDrawCount++;
}

static void test_set2d(
	int x,
	int y,
	int w,
	int h,
	float left,
	float right,
	float bottom,
	float top,
	float n,
	float f
)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)left;
	(void)right;
	(void)bottom;
	(void)top;
	(void)n;
	(void)f;
}

static void test_scissor(int x, int y, int w, int h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}


static void test_viewport()
{
	uir_viewport_t vp;
	float sx, sy, dx, dy;
	float x, y, w, h;

	CHECK(UIR_ViewportMake(0, 0, 800, 600, &vp) == UIR_OK);
	CHECK_NEAR(vp.scaleX, 1.0f, 1e-5);
	CHECK_NEAR(vp.invX, 1.0f, 1e-5);

	UIR_ViewportDrawToFb(&vp, 100.0f, 50.0f, &sx, &sy);
	CHECK_NEAR(sx, 100.0f, 1e-5);
	CHECK_NEAR(sy, 50.0f, 1e-5);
	UIR_ViewportFbToDraw(&vp, sx, sy, &dx, &dy);
	CHECK_NEAR(dx, 100.0f, 1e-5);
	CHECK_NEAR(dy, 50.0f, 1e-5);

	CHECK(UIR_ViewportMakeOrtho(10, 20, 200, 100, 0.0f, 100.0f, 0.0f, 50.0f, &vp) == UIR_OK);
	CHECK_NEAR(vp.scaleX, 2.0f, 1e-5);
	CHECK_NEAR(vp.scaleY, 2.0f, 1e-5);
	UIR_ViewportDrawToFb(&vp, 25.0f, 10.0f, &sx, &sy);
	CHECK_NEAR(sx, 10.0f + 50.0f, 1e-4);
	CHECK_NEAR(sy, 20.0f + 20.0f, 1e-4);

	CHECK(UIR_ViewportMake(0, 0, 0, 100, &vp) == UIR_ERR_INVALID_ARG);

	CHECK(UIR_ViewportMake(0, 0, 100, 100, &vp) == UIR_OK);
	x = 10.4f;
	y = 20.6f;
	w = 5.2f;
	h = 3.7f;
	UIR_ViewportSnapQuad(&vp, &x, &y, &w, &h);
	CHECK_NEAR(x, 10.0f, 1e-4);
	CHECK_NEAR(y, 21.0f, 1e-4);
}

/* Added in OPM: reference-resolution px scale (must match CL_UIR_PushUiPxScale). */
static void test_ref_px_scale()
{
	CHECK_NEAR(UIR_RefPxScale(1920, 1080), 1.0f, 1e-5);
	CHECK_NEAR(UIR_RefPxScale(3840, 2160), 2.0f, 1e-5);
	CHECK_NEAR(UIR_RefPxScale(1280, 720), 720.0f / 1080.0f, 1e-5);
	CHECK_NEAR(UIR_RefPxScale(3440, 1440), 1440.0f / 1080.0f, 1e-5);
	CHECK_NEAR(UIR_RefPxScale(1024, 768), 1024.0f / 1920.0f, 1e-5);
	CHECK_NEAR(UIR_RefPxScale(2560, 1440), 1440.0f / 1080.0f, 1e-5);
	CHECK_NEAR(UIR_RefPxScale(0, 1080), 1.0f, 1e-5);
	CHECK_NEAR(UIR_RefPxScale(1920, 0), 1.0f, 1e-5);
	CHECK_NEAR(UIR_RefPxScale(-1, 1080), 1.0f, 1e-5);
}

static void test_fov()
{
	float fx, fy;

	CHECK(UIR_CalcWorldFov(640, 480, 80.0f, &fx, &fy) == UIR_OK);
	CHECK_NEAR(fx, 80.0f, 1e-3);

	CHECK(UIR_CalcWorldFov(1920, 1080, 80.0f, &fx, &fy) == UIR_OK);
	CHECK(fx > 80.0f);

	CHECK(UIR_CalcPreviewFovY(100, 200, 30.0f, &fy) == UIR_OK);
	CHECK(fy > 30.0f);

	CHECK(UIR_CalcPreviewFovY(200, 100, 30.0f, &fy) == UIR_OK);
	CHECK(fy < 30.0f);

	CHECK(UIR_CalcWorldFov(0, 480, 80.0f, &fx, &fy) == UIR_ERR_INVALID_ARG);
}

struct BoxRec {
	float x, y, w, h;
	uir_color_t c;
};

static std::vector<BoxRec> g_boxes;

static void sink_box(float x, float y, float w, float h, const uir_color_t *color, void *userdata)
{
	(void)userdata;
	BoxRec b;
	b.x = x;
	b.y = y;
	b.w = w;
	b.h = h;
	b.c = *color;
	g_boxes.push_back(b);
}

static void test_path_fill()
{
	uir_viewport_t vp;
	uir_point_t tri[3] = {{10, 10}, {50, 10}, {30, 40}};
	uir_color_t white = {1, 1, 1, 1};
	uir_stats_t stats;
	uir_draw_sink_t sink;
	uir_path_t path;
	uir_contour_t *c;

	memset(&stats, 0, sizeof(stats));
	g_boxes.clear();
	sink.drawBox = sink_box;
	sink.userdata = NULL;
	sink.stats = &stats;

	CHECK(UIR_ViewportMake(0, 0, 64, 64, &vp) == UIR_OK);
	CHECK(UIR_FillPolygon(&vp, tri, 3, &white, &sink) == UIR_OK);
	CHECK(stats.emittedRuns > 0);
	CHECK(!g_boxes.empty());

	/* Concave chevron via even-odd path */
	UIR_PathInit(&path);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 5, 5);
	UIR_ContourAddPoint(c, 40, 5);
	UIR_ContourAddPoint(c, 40, 40);
	UIR_ContourAddPoint(c, 25, 20);
	UIR_ContourAddPoint(c, 5, 40);
	UIR_ContourClose(c);
	g_boxes.clear();
	memset(&stats, 0, sizeof(stats));
	CHECK(UIR_PathFill(&vp, &path, &white, &sink, 0) == UIR_OK);
	CHECK(stats.supersamples > 0);
	UIR_PathFree(&path);

	/* Added in OPM: crisp fill uses binary coverage (no soft AA supersamples). */
	UIR_PathInit(&path);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 5, 5);
	UIR_ContourAddPoint(c, 40, 5);
	UIR_ContourAddPoint(c, 40, 40);
	UIR_ContourAddPoint(c, 25, 20);
	UIR_ContourAddPoint(c, 5, 40);
	UIR_ContourClose(c);
	g_boxes.clear();
	memset(&stats, 0, sizeof(stats));
	CHECK(UIR_PathFill(&vp, &path, &white, &sink, 1) == UIR_OK);
	CHECK(stats.sampledPixels > 0);
	CHECK(stats.supersamples == 0);
	UIR_PathFree(&path);

	/* Hole: outer rect + inner rect, even-odd */
	UIR_PathInit(&path);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 2, 2);
	UIR_ContourAddPoint(c, 60, 2);
	UIR_ContourAddPoint(c, 60, 60);
	UIR_ContourAddPoint(c, 2, 60);
	UIR_ContourClose(c);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 20, 20);
	UIR_ContourAddPoint(c, 40, 20);
	UIR_ContourAddPoint(c, 40, 40);
	UIR_ContourAddPoint(c, 20, 40);
	UIR_ContourClose(c);
	g_boxes.clear();
	CHECK(UIR_PathFill(&vp, &path, &white, &sink, 0) == UIR_OK);
	UIR_PathFree(&path);
}

static void test_svg()
{
	const char *p;
	float v;
	uir_path_t path;
	uir_parse_result_t pr;
	uir_viewbox_t vb = {0, 0, 24, 24};
	uir_rect_t dest = {0, 0, 48, 48};
	uir_path_t mapped;
	float ox, oy;

	p = "4.88758e-7";
	CHECK(UIR_SvgParseFloat(&p, &v) == UIR_OK);
	CHECK_NEAR(v, 4.88758e-7f, 1e-12);
	CHECK(*p == '\0');

	p = "-12.5";
	CHECK(UIR_SvgParseFloat(&p, &v) == UIR_OK);
	CHECK_NEAR(v, -12.5f, 1e-5);

	p = "1e";
	CHECK(UIR_SvgParseFloat(&p, &v) == UIR_OK);
	CHECK_NEAR(v, 1.0f, 1e-5);
	/* trailing 'e' without exponent digits is not consumed as scientific */
	CHECK(*p == 'e');

	pr = UIR_SvgParsePathD("M0 0 L10 0 L10 10 Z", 0.25f, &path);
	CHECK(pr.status == UIR_OK);
	CHECK(path.contourCount == 1);
	CHECK(path.contours[0].closed);
	CHECK(path.contours[0].count >= 3);

	CHECK(UIR_SvgMapPathToRect(&path, &vb, &dest, UIR_FIT_CONTAIN, &mapped) == UIR_OK);
	CHECK(mapped.contourCount == 1);
	UIR_PathFree(&mapped);
	UIR_PathFree(&path);

	pr = UIR_SvgParsePathD("M1 1 C 1 8 8 8 8 1", 0.25f, &path);
	CHECK(pr.status == UIR_OK);
	CHECK(path.contours[0].count > 2);
	UIR_PathFree(&path);

	pr = UIR_SvgParsePolygonPoints("0,0 10,0 10,10", &path);
	CHECK(pr.status == UIR_OK);
	CHECK(path.contours[0].count == 3);
	UIR_PathFree(&path);

	UIR_SvgMapPointContain(12.0f, 12.0f, &vb, 100.0f, 200.0f, 24.0f, &ox, &oy);
	CHECK_NEAR(ox, 100.0f, 1e-4);
	CHECK_NEAR(oy, 200.0f, 1e-4);

	pr = UIR_SvgParsePathD("M0 0 garbage", 0.25f, &path);
	CHECK(pr.status == UIR_ERR_PARSE);
	UIR_PathFree(&path);

	/* Trailing junk after a closed subpath must fail. */
	pr = UIR_SvgParsePathD("M0 0 L1 0 L1 1 Z 1", 0.25f, &path);
	CHECK(pr.status == UIR_ERR_PARSE);
	UIR_PathFree(&path);
}

static void test_fill_rule_smoke()
{
	uir_viewport_t vp;
	uir_color_t white = {1, 1, 1, 1};
	uir_stats_t statsEven, statsNonZero;
	uir_draw_sink_t sink;
	uir_path_t path;
	uir_contour_t *c;
	size_t evenBoxes, nzBoxes;

	CHECK(UIR_ViewportMake(0, 0, 64, 64, &vp) == UIR_OK);

	UIR_PathInit(&path);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 2, 2);
	UIR_ContourAddPoint(c, 60, 2);
	UIR_ContourAddPoint(c, 60, 60);
	UIR_ContourAddPoint(c, 2, 60);
	UIR_ContourClose(c);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 20, 20);
	UIR_ContourAddPoint(c, 40, 20);
	UIR_ContourAddPoint(c, 40, 40);
	UIR_ContourAddPoint(c, 20, 40);
	UIR_ContourClose(c);

	g_boxes.clear();
	memset(&statsEven, 0, sizeof(statsEven));
	sink.drawBox = sink_box;
	sink.userdata = NULL;
	sink.stats = &statsEven;
	path.fillRule = UIR_FILL_EVEN_ODD;
	CHECK(UIR_PathFill(&vp, &path, &white, &sink, 0) == UIR_OK);
	evenBoxes = g_boxes.size();

	g_boxes.clear();
	memset(&statsNonZero, 0, sizeof(statsNonZero));
	sink.stats = &statsNonZero;
	path.fillRule = UIR_FILL_NON_ZERO;
	CHECK(UIR_PathFill(&vp, &path, &white, &sink, 0) == UIR_OK);
	nzBoxes = g_boxes.size();

	/* Same-winding hole: even-odd punches a hole; nonzero keeps coverage. */
	CHECK(evenBoxes > 0);
	CHECK(nzBoxes > 0);
	CHECK(statsEven.supersamples > 0);
	CHECK(statsNonZero.supersamples > 0);
	CHECK(evenBoxes != nzBoxes || statsEven.emittedRuns != statsNonZero.emittedRuns);
	UIR_PathFree(&path);

	/*
	 * Scissor Y conversion (top-left FB → OpenGL bottom-left) lives in
	 * uir_compositor.c as a static helper and is not exported — no unit test here.
	 */
}

static void test_model_preview_helpers()
{
	float origin[3];
	float offset[3];
	float scale;
	float fovX, fovY;
	const float defaultMins[3] = {-16, -16, 0};
	const float defaultMaxs[3] = {16, 16, 96};
	const float customMins[3] = {-8, -8, 0};
	const float customMaxs[3] = {8, 8, 48};

	UIR_ModelPreviewComputeOrigin(defaultMins, defaultMaxs, 1.0f, origin);
	CHECK(origin[0] > 0.0f);
	CHECK_NEAR(origin[1], 0.0f, 1e-4);
	CHECK_NEAR(origin[2], -48.0f, 1e-4);

	/* Worked example from Artifacts/UI/menu-model-draw.md §8 */
	UIR_ModelPreviewComputeFraming(226.5f, 450.0f, &scale, offset);
	CHECK_NEAR(scale, 0.466f, 0.005f);
	CHECK_NEAR(offset[0], 60.0f, 1e-4);
	CHECK_NEAR(offset[1], 0.0f, 1e-4);
	CHECK(offset[2] < 0.0f);
	UIR_ModelPreviewComputeOrigin(defaultMins, defaultMaxs, scale, origin);
	CHECK_NEAR(origin[0], 83.5f, 1.0f);
	CHECK_NEAR(origin[2], -48.0f, 1e-4);

	UIR_ModelPreviewComputeOrigin(customMins, customMaxs, 1.0f, origin);
	CHECK(origin[0] > 0.0f);
	CHECK_NEAR(origin[2], -24.0f, 1e-4);

	{
		float shiftedMins[3];
		float shiftedMaxs[3];
		const float delta[3] = {4.0f, -2.0f, 1.0f};

		UIR_ModelPreviewShiftBounds(customMins, customMaxs, delta, shiftedMins, shiftedMaxs);
		CHECK_NEAR(shiftedMins[0], -4.0f, 1e-4);
		CHECK_NEAR(shiftedMaxs[2], 49.0f, 1e-4);
	}

	/* Added in OPM: wide weapon bbox (500x100 bake framing). */
	{
		const float rifleMins[3] = {-40.0f, -4.0f, -2.0f};
		const float rifleMaxs[3] = {40.0f, 4.0f, 2.0f};
		float       wideScale;
		float       wideOffset[3];

		UIR_ModelPreviewComputeFraming(500.0f, 100.0f, &wideScale, wideOffset);
		CHECK(wideScale > 0.0f && wideScale <= 1.0f);
		UIR_ModelPreviewComputeOrigin(rifleMins, rifleMaxs, wideScale, origin);
		CHECK(origin[0] > 0.0f);
		CHECK_NEAR(origin[1], 0.0f, 1e-4);
		CHECK_NEAR(origin[2], 0.0f, 1e-4);
	}

	CHECK(UIR_ModelPreviewCalcFov(100, 200, 0.0f, &fovX, &fovY) == UIR_OK);
	CHECK_NEAR(fovX, 30.0f, 1e-4);
	CHECK(fovY > fovX);

	CHECK(UIR_ModelPreviewCalcFov(200, 100, 0.0f, &fovX, &fovY) == UIR_OK);
	CHECK(fovY < fovX);

	CHECK(UIR_ModelPreviewCalcFov(100, 200, 45.0f, &fovX, &fovY) == UIR_OK);
	CHECK_NEAR(fovX, 45.0f, 1e-4);

	{
		float animTime = 4.9f;
		UIR_ModelPreviewWrapAnimTime(&animTime, 2.0f);
		CHECK_NEAR(animTime, 0.9f, 1e-4);
	}
	{
		float animTime = 10.5f;
		UIR_ModelPreviewWrapAnimTime(&animTime, 3.0f);
		CHECK_NEAR(animTime, 1.5f, 1e-4);
	}
	{
		float animTime = -0.5f;
		UIR_ModelPreviewWrapAnimTime(&animTime, 2.0f);
		CHECK_NEAR(animTime, 1.5f, 1e-4);
	}
	CHECK_NEAR(UIR_ModelPreviewPhaseToAnimTime(0.0f, 4.0f), 0.0f, 1e-4);
	CHECK_NEAR(UIR_ModelPreviewPhaseToAnimTime(0.5f, 4.0f), 2.0f, 1e-4);
	CHECK_NEAR(UIR_ModelPreviewPhaseToAnimTime(1.25f, 4.0f), 1.0f, 1e-4);
	CHECK_NEAR(UIR_ModelPreviewPhaseToAnimTime(-0.25f, 4.0f), 3.0f, 1e-4);
	CHECK_NEAR(UIR_ModelPreviewPhaseToAnimTime(0.5f, 0.0f), 0.0f, 1e-4);
}

static void test_font_math()
{
	float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
	UIR_FontInsetUVs(&u0, &v0, &u1, &v1, 100, 100);
	CHECK(u0 > 0.0f && v0 > 0.0f);
	CHECK(u1 < 1.0f && v1 < 1.0f);
	CHECK_NEAR(UIR_FontEmScale(48.0f, 1000), 0.048f, 1e-5);
	CHECK_NEAR(UIR_FontEmScale(48.0f, 0), 0.0f, 1e-8);
}

static void test_path_stroke()
{
	uir_viewport_t vp;
	uir_color_t white = {1, 1, 1, 1};
	uir_stats_t stats;
	uir_draw_sink_t sink;
	uir_path_t path;
	uir_path_t stroke;
	uir_contour_t *c;
	float areaThin = 0.0f;
	float areaThick = 0.0f;
	size_t i;

	memset(&stats, 0, sizeof(stats));
	g_boxes.clear();
	sink.drawBox = sink_box;
	sink.userdata = NULL;
	sink.stats = &stats;
	CHECK(UIR_ViewportMake(0, 0, 128, 128, &vp) == UIR_OK);

	/* Open horizontal segment — round caps extend past endpoints */
	UIR_PathInit(&path);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 20, 64);
	UIR_ContourAddPoint(c, 100, 64);
	CHECK(UIR_BuildStrokePath(&path, 4.0f, &stroke) == UIR_OK);
	CHECK(stroke.contourCount == 1);
	CHECK(stroke.contours[0].closed == 1);
	CHECK(stroke.contours[0].count >= 4);
	/* Round caps: stroke polygon extends ~half-width past the open endpoints. */
	{
		float minX = 1e30f, maxX = -1e30f;
		int pi;
		for (pi = 0; pi < stroke.contours[0].count; pi++) {
			float px = stroke.contours[0].points[pi].x;
			if (px < minX) {
				minX = px;
			}
			if (px > maxX) {
				maxX = px;
			}
		}
		CHECK(minX < 20.0f - 1.0f);
		CHECK(maxX > 100.0f + 1.0f);
	}
	g_boxes.clear();
	memset(&stats, 0, sizeof(stats));
	CHECK(UIR_PathStroke(&vp, &path, &white, 4.0f, &sink, 0) == UIR_OK);
	CHECK(stats.emittedRuns > 0);
	CHECK(!g_boxes.empty());
	for (i = 0; i < g_boxes.size(); i++) {
		areaThin += g_boxes[i].w * g_boxes[i].h;
	}
	UIR_PathFree(&stroke);

	g_boxes.clear();
	memset(&stats, 0, sizeof(stats));
	CHECK(UIR_PathStroke(&vp, &path, &white, 8.0f, &sink, 0) == UIR_OK);
	for (i = 0; i < g_boxes.size(); i++) {
		areaThick += g_boxes[i].w * g_boxes[i].h;
	}
	CHECK(areaThick > areaThin * 1.5f);
	UIR_PathFree(&path);

	/* Closed square → outside-aligned annulus (outer offset + path as inner) */
	UIR_PathInit(&path);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 20, 20);
	UIR_ContourAddPoint(c, 80, 20);
	UIR_ContourAddPoint(c, 80, 80);
	UIR_ContourAddPoint(c, 20, 80);
	UIR_ContourClose(c);
	CHECK(UIR_BuildStrokePath(&path, 4.0f, &stroke) == UIR_OK);
	CHECK(stroke.contourCount == 2);
	CHECK(stroke.contours[0].closed == 1);
	CHECK(stroke.contours[1].closed == 1);
	/* Outside align: outer ring extends a full width past the path edge. */
	{
		float minX = 1e30f;
		int pi;
		for (pi = 0; pi < stroke.contours[0].count; pi++) {
			float px = stroke.contours[0].points[pi].x;
			if (px < minX) {
				minX = px;
			}
		}
		CHECK(minX < 20.0f - 2.0f);
	}
	g_boxes.clear();
	memset(&stats, 0, sizeof(stats));
	CHECK(UIR_PathStroke(&vp, &path, &white, 4.0f, &sink, 0) == UIR_OK);
	CHECK(!g_boxes.empty());
	UIR_PathFree(&stroke);
	UIR_PathFree(&path);

	/* Acute corner: round join still builds one open strip */
	UIR_PathInit(&path);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 10, 10);
	UIR_ContourAddPoint(c, 64, 64);
	UIR_ContourAddPoint(c, 20, 60);
	CHECK(UIR_BuildStrokePath(&path, 6.0f, &stroke) == UIR_OK);
	CHECK(stroke.contourCount == 1);
	UIR_PathFree(&stroke);
	UIR_PathFree(&path);

	/* Allied-star ring: continuous annulus (outer+inner) strokes without seam gaps */
	{
		const char *rings =
			"M 12 0 A 12 12 0 1 1 12 24 A 12 12 0 1 1 12 0 Z "
			"M 12 1.27658 A 10.72342 10.72342 0 1 0 12 22.72342 A 10.72342 10.72342 0 1 0 12 1.27658 Z";
		uir_parse_result_t pr;
		uir_viewbox_t vb = {0, 0, 24, 24};
		uir_rect_t dest = {0, 0, 64, 64};
		uir_path_t local;
		uir_path_t mapped;

		pr = UIR_SvgParsePathD(rings, 0.25f, &local);
		CHECK(pr.status == UIR_OK);
		CHECK(local.contourCount == 2);
		CHECK(UIR_SvgMapPathToRect(&local, &vb, &dest, UIR_FIT_STRETCH, &mapped) == UIR_OK);
		CHECK(UIR_BuildStrokePath(&mapped, 2.0f, &stroke) == UIR_OK);
		/* 2 circles × (outer+inner) = 4 contours */
		CHECK(stroke.contourCount == 4);
		g_boxes.clear();
		CHECK(UIR_PathStroke(&vp, &mapped, &white, 2.0f, &sink, 0) == UIR_OK);
		CHECK(!g_boxes.empty());
		UIR_PathFree(&stroke);
		UIR_PathFree(&mapped);
		UIR_PathFree(&local);
	}
}

static void test_path_rotate()
{
	uir_path_t src;
	uir_path_t rotated;
	uir_contour_t *c;

	UIR_PathInit(&src);
	CHECK(UIR_PathBeginContour(&src, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 0.0f, 0.0f);
	UIR_ContourAddPoint(c, 10.0f, 0.0f);
	UIR_ContourAddPoint(c, 10.0f, 10.0f);
	UIR_ContourAddPoint(c, 0.0f, 10.0f);
	UIR_ContourClose(c);

	CHECK(UIR_PathRotate(&src, 5.0f, 5.0f, 90.0f, &rotated) == UIR_OK);
	CHECK(rotated.contourCount == 1);
	CHECK(rotated.contours[0].count == 4);
	/* (10,0) around (5,5) by +90° → (10,10) */
	CHECK_NEAR(rotated.contours[0].points[1].x, 10.0f, 1e-3f);
	CHECK_NEAR(rotated.contours[0].points[1].y, 10.0f, 1e-3f);
	/* (10,10) → (0,10) */
	CHECK_NEAR(rotated.contours[0].points[2].x, 0.0f, 1e-3f);
	CHECK_NEAR(rotated.contours[0].points[2].y, 10.0f, 1e-3f);

	UIR_PathFree(&rotated);
	UIR_PathFree(&src);
}

static void test_image_fit_math(void)
{
	float x, y, w, h, s1, t1, s2, t2;

	UIR_ComputeImageRect(100.0f, 50.0f, 0.0f, 0.0f, 200.0f, 100.0f, UIR_IMAGE_FIT_STRETCH,
		&x, &y, &w, &h, &s1, &t1, &s2, &t2);
	CHECK_NEAR(x, 0.0f, 1e-3f);
	CHECK_NEAR(w, 200.0f, 1e-3f);
	CHECK_NEAR(h, 100.0f, 1e-3f);
	CHECK_NEAR(s2, 1.0f, 1e-3f);

	UIR_ComputeImageRect(100.0f, 100.0f, 0.0f, 0.0f, 200.0f, 100.0f, UIR_IMAGE_FIT_CONTAIN,
		&x, &y, &w, &h, &s1, &t1, &s2, &t2);
	CHECK_NEAR(w, 100.0f, 1e-3f);
	CHECK_NEAR(h, 100.0f, 1e-3f);
	CHECK_NEAR(x, 50.0f, 1e-3f);

	UIR_ComputeImageRect(100.0f, 100.0f, 0.0f, 0.0f, 200.0f, 100.0f, UIR_IMAGE_FIT_COVER,
		&x, &y, &w, &h, &s1, &t1, &s2, &t2);
	CHECK_NEAR(w, 200.0f, 1e-3f);
	CHECK_NEAR(h, 200.0f, 1e-3f);
	CHECK_NEAR(y, -50.0f, 1e-3f);
	CHECK_NEAR(s1, 0.0f, 1e-3f);
	CHECK_NEAR(t1, 0.25f, 1e-3f);
	CHECK_NEAR(s2, 1.0f, 1e-3f);
	CHECK_NEAR(t2, 0.75f, 1e-3f);
}

static float g_tilePicW = 0.0f;
static float g_tilePicH = 0.0f;

struct StretchDrawRec {
	float x;
	float y;
	float w;
	float h;
	float s1;
	float t1;
	float s2;
	float t2;
};

static std::vector<StretchDrawRec> g_stretchDrawLog;
static std::vector<StretchDrawRec> g_triangleDrawLog;

struct ScissorRec {
	int x;
	int y;
	int w;
	int h;
};

static std::vector<ScissorRec> g_scissorLog;

static int mock_register_shader(const char *path)
{
	(void)path;
	return 1;
}

static void mock_get_shader_size_repeat(int shader, int *width, int *height)
{
	(void)shader;
	if (width) {
		*width = 64;
	}
	if (height) {
		*height = 32;
	}
}

static void mock_get_shader_size_rotate(int shader, int *width, int *height)
{
	(void)shader;
	if (width) {
		*width = 128;
	}
	if (height) {
		*height = 128;
	}
}

static void mock_draw_stretch_pic_log(
	float x,
	float y,
	float w,
	float h,
	float s1,
	float t1,
	float s2,
	float t2,
	int shader
)
{
	(void)shader;
	g_stretchDrawLog.push_back({x, y, w, h, s1, t1, s2, t2});
}

static void mock_draw_triangle_pic_log(const float points[3][2], const float texCoords[3][2], int shader)
{
	(void)shader;
	(void)texCoords;
	g_triangleDrawLog.push_back({points[0][0], points[0][1], 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
}

static void mock_draw_tile_pic(float x, float y, float w, float h, int shader)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)shader;
}

static void mock_set_color(const float *rgba)
{
	(void)rgba;
}

static int g_lastScissorX = 0;
static int g_lastScissorY = 0;
static int g_lastScissorW = 0;
static int g_lastScissorH = 0;

static void test_scissor_record(int x, int y, int w, int h)
{
	g_scissorLog.push_back({x, y, w, h});
	g_lastScissorX = x;
	g_lastScissorY = y;
	g_lastScissorW = w;
	g_lastScissorH = h;
}

static void test_image_repeat_tiling(void)
{
	uir_image_backend_t backend;
	uir_viewport_t      vp;
	uir_draw2d_backend_t d2d;

	std::memset(&backend, 0, sizeof(backend));
	backend.registerShaderNoMip = mock_register_shader;
	backend.getShaderSize = mock_get_shader_size_repeat;
	backend.setColor = mock_set_color;
	backend.drawStretchPic = mock_draw_stretch_pic_log;
	backend.drawTilePic = mock_draw_tile_pic;

	UIR_CompositorReset();
	UIR_ViewportMake(0, 0, 640, 480, &vp);
	UIR_BeginOverlayFrame(&vp, 0);
	std::memset(&d2d, 0, sizeof(d2d));
	d2d.scissor = test_scissor_record;
	UIR_Draw2D_SetBackend(&d2d);
	UIR_ImageSetBackend(&backend);
	g_stretchDrawLog.clear();

	CHECK(
		UIR_ImageDrawClipped(
			"textures/hud/clip_rifle",
			0.0f,
			0.0f,
			64.0f,
			128.0f,
			NULL,
			0,
			64.0f,
			128.0f,
			UIR_IMAGE_FIT_REPEAT,
			0.0f,
			1.0f,
			1.0f,
			NULL
		) == UIR_OK
	);
	CHECK(g_stretchDrawLog.size() == 1);
	CHECK_NEAR(g_stretchDrawLog[0].y, 0.0f, 1e-3f);
	CHECK_NEAR(g_stretchDrawLog[0].s2, 1.0f, 1e-3f);
	CHECK_NEAR(g_stretchDrawLog[0].t2, 4.0f, 1e-3f);

	g_stretchDrawLog.clear();
	CHECK(
		UIR_ImageDrawClipped(
			"textures/hud/clip_rifle",
			0.0f,
			0.0f,
			64.0f,
			100.0f,
			NULL,
			0,
			64.0f,
			100.0f,
			UIR_IMAGE_FIT_REPEAT,
			0.0f,
			1.0f,
			1.0f,
			NULL
		) == UIR_OK
	);
	CHECK(g_stretchDrawLog.size() == 1);
	CHECK_NEAR(g_stretchDrawLog[0].h, 100.0f, 1e-3f);
	CHECK_NEAR(g_stretchDrawLog[0].t2, 3.125f, 1e-3f);

	g_stretchDrawLog.clear();
	CHECK(
		UIR_ImageDrawClipped(
			"textures/hud/clip_rifle",
			0.0f,
			0.0f,
			76.8f,
			153.6f,
			NULL,
			0,
			76.8f,
			153.6f,
			UIR_IMAGE_FIT_REPEAT,
			0.0f,
			1.2f,
			1.0f,
			NULL
		) == UIR_OK
	);
	CHECK(g_stretchDrawLog.size() == 1);
	CHECK_NEAR(g_stretchDrawLog[0].s2, 1.0f, 1e-3f);
	CHECK_NEAR(g_stretchDrawLog[0].t2, 4.0f, 1e-3f);

	g_stretchDrawLog.clear();
	CHECK(
		UIR_ImageDrawClipped(
			"textures/hud/clip_rifle",
			0.0f,
			0.0f,
			64.0f,
			128.0f,
			NULL,
			0,
			64.0f,
			128.0f,
			UIR_IMAGE_FIT_REPEAT,
			0.0f,
			1.0f,
			2.0f,
			NULL
		) == UIR_OK
	);
	CHECK(g_stretchDrawLog.size() == 1);
	CHECK_NEAR(g_stretchDrawLog[0].s2, 0.5f, 1e-3f);
	CHECK_NEAR(g_stretchDrawLog[0].t2, 2.0f, 1e-3f);

	UIR_ImageShutdown();
}

static void test_image_rotation_quad(void)
{
	uir_image_backend_t backend;
	uir_viewport_t      vp;
	uir_draw2d_backend_t d2d;

	std::memset(&backend, 0, sizeof(backend));
	backend.registerShaderNoMip = mock_register_shader;
	backend.getShaderSize = mock_get_shader_size_rotate;
	backend.setColor = mock_set_color;
	backend.drawStretchPic = mock_draw_stretch_pic_log;
	backend.drawTrianglePic = mock_draw_triangle_pic_log;

	UIR_CompositorReset();
	UIR_ViewportMake(0, 0, 640, 480, &vp);
	UIR_BeginOverlayFrame(&vp, 0);
	std::memset(&d2d, 0, sizeof(d2d));
	d2d.scissor = test_scissor_record;
	UIR_Draw2D_SetBackend(&d2d);
	UIR_ImageSetBackend(&backend);
	g_triangleDrawLog.clear();

	CHECK(
		UIR_ImageDrawClipped(
			"textures/hud/compassface",
			0.0f,
			0.0f,
			128.0f,
			128.0f,
			NULL,
			0,
			128.0f,
			128.0f,
			UIR_IMAGE_FIT_STRETCH,
			90.0f,
			1.0f,
			1.0f,
			NULL
		) == UIR_OK
	);
	CHECK(g_triangleDrawLog.size() >= 1);
	CHECK_NEAR(g_triangleDrawLog[0].x, 128.0f, 0.01f);
	CHECK_NEAR(g_triangleDrawLog[0].y, 0.0f, 0.01f);

	UIR_ImageShutdown();
}

static void test_image_axis_aligned_clip_scissor(void)
{
	uir_image_backend_t backend;
	uir_viewport_t      vp;
	uir_draw2d_backend_t d2d;
	static const char *clipPath = "M 0 64 L 64 64 L 64 128 L 0 128 Z";
	const char *clipPaths[1];

	std::memset(&backend, 0, sizeof(backend));
	backend.registerShaderNoMip = mock_register_shader;
	backend.getShaderSize = mock_get_shader_size_repeat;
	backend.setColor = mock_set_color;
	backend.drawStretchPic = mock_draw_stretch_pic_log;
	backend.drawTrianglePic = mock_draw_triangle_pic_log;

	UIR_CompositorReset();
	UIR_ViewportMake(0, 0, 640, 480, &vp);
	UIR_BeginOverlayFrame(&vp, 0);
	std::memset(&d2d, 0, sizeof(d2d));
	d2d.scissor = test_scissor_record;
	UIR_Draw2D_SetBackend(&d2d);
	UIR_ImageSetBackend(&backend);
	g_triangleDrawLog.clear();
	g_stretchDrawLog.clear();
	g_scissorLog.clear();
	clipPaths[0] = clipPath;

	CHECK(
		UIR_ImageDrawClipped(
			"textures/hud/clip_rifle",
			0.0f,
			0.0f,
			64.0f,
			128.0f,
			clipPaths,
			1,
			64.0f,
			128.0f,
			UIR_IMAGE_FIT_STRETCH,
			0.0f,
			1.0f,
			1.0f,
			NULL
		) == UIR_OK
	);
	CHECK(g_triangleDrawLog.empty());
	{
		bool foundClip = false;
		for (const ScissorRec &rec : g_scissorLog) {
			if (rec.w == 64 && rec.h == 64 && rec.y == 352) {
				foundClip = true;
				break;
			}
		}
		CHECK(foundClip);
	}

	UIR_ImageShutdown();
}

static int g_mwStagedCalls = 0;
static int g_mwLoadCalls = 0;
static int g_mwCommitCalls = 0;
static int g_mwHasWorld = 0;

static int mock_load_menu_world_staged(const char *name)
{
	(void)name;
	g_mwStagedCalls++;
	return 1;
}

static void mock_load_menu_world(const char *name)
{
	(void)name;
	g_mwLoadCalls++;
	g_mwHasWorld = 1;
}

static int mock_has_active_world(void)
{
	return g_mwHasWorld;
}

static int mock_file_exists(const char *path)
{
	(void)path;
	return 1;
}

static void mock_cm_load_map(const char *name, int clientload, int *checksum)
{
	(void)name;
	(void)clientload;
	if (checksum) {
		*checksum = 0;
	}
}

static void mock_clear_world(void)
{
	g_mwHasWorld = 0;
}

static void mock_clear_scene(void)
{
}

static void mock_render_scene(const void *refdef)
{
	(void)refdef;
}

static void mock_angles_to_axis(const float angles[3], float axis[3][3])
{
	(void)angles;
	(void)axis;
}

static void mock_draw_box(float x, float y, float w, float h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}

static void mock_commit_menu_world(void)
{
	g_mwCommitCalls++;
	g_mwHasWorld = 1;
}

static void test_menu_world_view_switch()
{
	uir_menuworld_backend_t mw;
	uir_menu_map_view_t viewA;
	uir_menu_map_view_t viewB;

	memset(&mw, 0, sizeof(mw));
	mw.loadMenuWorld = mock_load_menu_world;
	mw.loadMenuWorldStaged = mock_load_menu_world_staged;
	mw.commitMenuWorld = mock_commit_menu_world;
	mw.fileExists = mock_file_exists;
	mw.cmLoadMap = mock_cm_load_map;
	mw.hasActiveWorld = mock_has_active_world;
	mw.clearWorld = mock_clear_world;
	mw.clearScene = mock_clear_scene;
	mw.renderScene = mock_render_scene;
	mw.anglesToAxis = mock_angles_to_axis;
	mw.setColor = mock_set_color;
	mw.drawBox = mock_draw_box;

	UIR_MenuWorldShutdown();
	UIR_MenuWorldSetBackend(&mw);

	g_mwStagedCalls = 0;
	g_mwLoadCalls = 0;
	g_mwCommitCalls = 0;
	g_mwHasWorld = 0;

	UIR_MenuMapViewSetDefaults(&viewA);
	UIR_MenuMapViewSetDefaults(&viewB);
	strncpy(viewB.bsp, "maps/dm/other.bsp", sizeof(viewB.bsp) - 1);
	viewB.bsp[sizeof(viewB.bsp) - 1] = '\0';
	viewB.pitch = 5.0f;

	UIR_MenuWorldSetDesiredView(&viewA);
	CHECK(UIR_MenuWorldEnsureLoaded() == UIR_OK);
	CHECK(g_mwLoadCalls == 1);
	CHECK(g_mwStagedCalls == 0);
	CHECK(g_mwCommitCalls == 0);

	g_mwStagedCalls = 0;
	g_mwCommitCalls = 0;
	viewA.pitch = 12.0f;
	UIR_MenuWorldSetDesiredView(&viewA);
	CHECK(UIR_MenuWorldEnsureLoaded() == UIR_OK);
	CHECK(g_mwStagedCalls == 0);
	CHECK(g_mwCommitCalls == 0);
	CHECK(g_mwLoadCalls == 1);

	g_mwStagedCalls = 0;
	g_mwCommitCalls = 0;
	UIR_MenuWorldSetDesiredView(&viewB);
	CHECK(UIR_MenuWorldEnsureLoaded() == UIR_OK);
	CHECK(g_mwStagedCalls == 0);
	CHECK(g_mwCommitCalls == 0);
	CHECK(g_mwLoadCalls == 2);

	UIR_MenuWorldShutdown();
}

static void mock_cm_model_bounds_from_name(const char *name, float mins[3], float maxs[3])
{
	if (name && name[0] == '*' && name[1] == '1') {
		mins[0] = -100.0f;
		mins[1] = -100.0f;
		mins[2] = 0.0f;
		maxs[0] = 100.0f;
		maxs[1] = 100.0f;
		maxs[2] = 200.0f;
		return;
	}
	mins[0] = mins[1] = mins[2] = 0.0f;
	maxs[0] = maxs[1] = maxs[2] = 0.0f;
}

static void test_map_env_parse_entities_classname_after_model()
{
	static const char kEnts[] =
		"{\n"
		"\"model\" \"*1\"\n"
		"\"origin\" \"10 20 30\"\n"
		"\"classname\" \"func_rain\"\n"
		"}\n";
	uir_map_env_backend_t backend;
	uir_map_env_t         env;

	memset(&backend, 0, sizeof(backend));
	backend.cmModelBoundsFromName = mock_cm_model_bounds_from_name;
	memset(&env, 0, sizeof(env));
	UIR_MapEnvParseEntities(kEnts, &backend, &env);

	CHECK(env.numRainVolumes == 1);
}

static void test_map_env_parse_entities()
{
	static const char kEnts[] =
		"{\n"
		"\"classname\" \"worldspawn\"\n"
		"\"farplane\" \"7000\"\n"
		"\"farplane_color\" \".5 .4 .2\"\n"
		"}\n"
		"{\n"
		"\"classname\" \"func_rain\"\n"
		"\"model\" \"*1\"\n"
		"\"origin\" \"10 20 30\"\n"
		"}\n";
	uir_map_env_backend_t backend;
	uir_map_env_t         env;

	memset(&backend, 0, sizeof(backend));
	backend.cmModelBoundsFromName = mock_cm_model_bounds_from_name;
	UIR_MapEnvParseEntities(kEnts, &backend, &env);

	CHECK(env.hasFarplane);
	CHECK_NEAR(env.farplane, 7000.0, 1e-3);
	CHECK_NEAR(env.farplane_bias, 1260.0, 1e-3);
	CHECK_NEAR(env.farplane_color[0], 0.5, 1e-3);
	CHECK(env.numRainVolumes == 1);
}

static void test_map_env_parse_script()
{
	static const char kScr[] =
		R"(snow:
level.rain_shader = "textures/snow0"
level.rain_numshaders = 12
level.rain_density = ".2"
level.rain_speed = "32"
)";
	uir_weather_params_t params;

	UIR_MapEnvParseScript(kScr, &params);
	CHECK_NEAR(params.density, 0.2, 1e-3);
	CHECK(params.numshaders == 12);
	CHECK(std::strcmp(params.shader[0], "textures/snow0") == 0);
	CHECK(std::strcmp(params.shader[11], "textures/snow11") == 0);
}

static int g_polyCount = 0;

static int mock_weather_register_shader(const char *name)
{
	(void)name;
	return 42;
}

static void mock_add_poly_to_scene(int shader, int numVerts, const void *verts, int renderfx)
{
	(void)shader;
	(void)numVerts;
	(void)verts;
	(void)renderfx;
	g_polyCount++;
}

static void test_menu_weather_skip_without_density()
{
	uir_map_env_t          env;
	uir_menuworld_backend_t backend;
	float                  vieworg[3] = {0, 0, 0};
	float                  viewaxis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

	memset(&env, 0, sizeof(env));
	UIR_WeatherParamsSetDefaults(&env.weather);
	memset(&backend, 0, sizeof(backend));
	backend.registerShader = mock_weather_register_shader;
	backend.addPolyToScene = mock_add_poly_to_scene;

	g_polyCount = 0;
	UIR_MenuWeatherAddToScene(&env, vieworg, viewaxis, 1, 1000, &backend);
	CHECK(g_polyCount == 0);
}

static void test_menu_weather_spawns_from_script_density()
{
	uir_map_env_t          env;
	uir_menuworld_backend_t backend;
	float                  vieworg[3] = {1053.49f, -2820.07f, 201.90f};
	float                  viewaxis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

	memset(&env, 0, sizeof(env));
	UIR_WeatherParamsSetDefaults(&env.weather);
	env.weather.density = 0.2f;
	env.weather.min_dist = 1800.0f;
	env.weather.length = 2.0f;
	env.weather.speed = 32.0f;
	Q_strncpyz(env.weather.shader[0], "textures/snow0", sizeof(env.weather.shader[0]));
	memset(&backend, 0, sizeof(backend));
	backend.registerShader = mock_weather_register_shader;
	backend.addPolyToScene = mock_add_poly_to_scene;

	g_polyCount = 0;
	UIR_MenuWeatherAddToScene(&env, vieworg, viewaxis, 1, 1000, &backend);
	CHECK(g_polyCount > 0);
}

static void test_gpu_tess_and_batch()
{
	uir_viewport_t vp;
	uir_path_t path;
	uir_point_t pts[4];
	uir_color_t rgba = {0.2f, 0.4f, 1.0f, 0.9f};
	uir_vert_t verts[UIR_BATCH_MAX_VERTS];
	unsigned short idx[UIR_BATCH_MAX_INDEXES];
	int nv = 0;
	int ni = 0;
	uir_status_t st;

	CHECK(sizeof(uir_vert_t) == sizeof(ui2dVert_t));

	CHECK(UIR_ViewportMakeOrtho(0, 0, 700, 200, 0.0f, 350.0f, 0.0f, 100.0f, &vp) == UIR_OK);

	pts[0].x = 0.0f;
	pts[0].y = 100.0f;
	pts[1].x = 50.0f;
	pts[1].y = 0.0f;
	pts[2].x = 350.0f;
	pts[2].y = 0.0f;
	pts[3].x = 300.0f;
	pts[3].y = 100.0f;

	UIR_PathInit(&path);
	{
		uir_contour_t *contour = NULL;
		CHECK(UIR_PathBeginContour(&path, &contour) == UIR_OK);
		for (int i = 0; i < 4; i++) {
			UIR_ContourAddPoint(contour, pts[i].x, pts[i].y);
		}
		UIR_ContourClose(contour);
	}

	st = UIR_TessFillPath(&vp, &path, &rgba, 1.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni);
	CHECK(st == UIR_OK);
	CHECK(nv >= 8);
	CHECK(ni >= 6);
	{
		int hasGradient = 0;
		for (int i = 0; i < nv; i++) {
			if (verts[i].a > 0 && verts[i].a < 255) {
				hasGradient = 1;
				break;
			}
		}
		CHECK(hasGradient);
	}

	st = UIR_TessFillPath(&vp, &path, &rgba, 0.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni);
	CHECK(st == UIR_OK);
	CHECK(nv >= 4);
	CHECK(ni >= 6);

	UIR_PathFree(&path);
}

static int test_tess_cover_grid(
	const uir_vert_t *verts,
	int nv,
	const unsigned short *idx,
	int ni,
	float px,
	float py
);

static uir_status_t test_parse_shape_path(
	const char *d,
	float viewW,
	float viewH,
	float destW,
	float destH,
	uir_path_t *out
)
{
	uir_viewbox_t vb = {0.0f, 0.0f, viewW, viewH};
	uir_rect_t dest = {0.0f, 0.0f, destW, destH};
	float sx = (viewW > 1e-6f) ? (destW / viewW) : 1.0f;
	float sy = (viewH > 1e-6f) ? (destH / viewH) : 1.0f;
	float scale = sx < sy ? sx : sy;
	float flatness = (scale > 1e-6f) ? (0.25f / scale) : 0.25f;
	uir_path_t local;
	uir_parse_result_t pr;

	if (flatness < 0.01f) {
		flatness = 0.01f;
	}
	if (flatness > 0.5f) {
		flatness = 0.5f;
	}

	pr = UIR_SvgParsePathD(d, flatness, &local);
	if (pr.status != UIR_OK) {
		return pr.status;
	}
	uir_status_t st = UIR_SvgMapPathToRect(&local, &vb, &dest, UIR_FIT_STRETCH, out);
	UIR_PathFree(&local);
	return st;
}

static int test_path_coverage_matches(
	const uir_viewport_t *vp,
	const uir_path_t *path,
	const uir_vert_t *verts,
	int nv,
	const unsigned short *idx,
	int ni,
	float gridStep
)
{
	float minX = 1e30f;
	float minY = 1e30f;
	float maxX = -1e30f;
	float maxY = -1e30f;
	int mismatches = 0;

	for (int c = 0; c < path->contourCount; c++) {
		for (int i = 0; i < path->contours[c].count; i++) {
			float px = path->contours[c].points[i].x;
			float py = path->contours[c].points[i].y;
			if (px < minX) {
				minX = px;
			}
			if (py < minY) {
				minY = py;
			}
			if (px > maxX) {
				maxX = px;
			}
			if (py > maxY) {
				maxY = py;
			}
		}
	}

	for (float py = minY + gridStep * 0.5f; py < maxY; py += gridStep) {
		for (float px = minX + gridStep * 0.5f; px < maxX; px += gridStep) {
			int cpu = UIR_PathContainsPoint(path, px, py);
			int gpu = test_tess_cover_grid(verts, nv, idx, ni, px, py);
			if (cpu != gpu) {
				mismatches++;
			}
		}
	}

	(void)vp;
	return mismatches;
}

static void test_icon_coverage(const char *name, const char *d, int destW, int destH, float viewW, float viewH, int expectContours)
{
	uir_viewport_t vp;
	uir_path_t path;
	uir_color_t white = {1, 1, 1, 1};
	uir_vert_t verts[UIR_BATCH_MAX_VERTS];
	unsigned short idx[UIR_BATCH_MAX_INDEXES];
	int nv = 0;
	int ni = 0;
	uir_stats_t stats;
	uir_status_t st;

	memset(&stats, 0, sizeof(stats));
	CHECK(UIR_ViewportMake(0, 0, destW, destH, &vp) == UIR_OK);
	CHECK(test_parse_shape_path(d, viewW, viewH, (float)destW, (float)destH, &path) == UIR_OK);
	if (expectContours > 0) {
		CHECK(path.contourCount == expectContours);
	}

	UIR_TessSetStats(&stats);
	st = UIR_TessFillPath(&vp, &path, &white, 0.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni);
	UIR_TessSetStats(NULL);
	CHECK(st == UIR_OK);
	CHECK(nv > 0);
	CHECK(ni >= 3);
	CHECK(test_path_coverage_matches(&vp, &path, verts, nv, idx, ni, (destW <= 35) ? 2.0f : 4.0f) == 0);
	if (expectContours > 0) {
		CHECK(stats.tessContoursIn >= expectContours);
	}
	(void)name;
	UIR_PathFree(&path);
}

static void test_icon_coverage_suite()
{
	/* Synced with assets/main/ui/modern/lib/shapes.xml — update when shapes change. */
	test_icon_coverage(
		"exit-circle",
		"M 1 8 a 6 6 0 0 1 8.5 -5.4 a 0.8 0.8 0 0 1 -0.6 1.3 a 4.5 4.5 0 1 0 0 8.2 a 0.8 0.8 0 1 1 0.6 1.3 A 6 6 0 0 1 1 8",
		32,
		32,
		16.0f,
		16.0f,
		1
	);
	test_icon_coverage(
		"exit-arrow",
		"M 11.2 4.7 a 1 1 0 0 0 0 1 l 1.4 1.6 H 6.8 a 0.8 0.8 0 0 0 0 1.5 h 5.8 l -1.4 1.4 a 0.8 0.8 0 0 0 1.1 1 l 2.5 -2.7 a 1 1 0 0 0 0 -1 l -2.5 -2.8 a 1 1 0 0 0 -1 0",
		32,
		32,
		16.0f,
		16.0f,
		1
	);
	test_icon_coverage(
		"allied-star",
		"M 11.961142 1.310257 L 14.335891 8.661963 L 22.020748 8.661963 "
		"L 15.803569 13.205566 L 18.17832 20.557281 L 11.961142 16.013676 "
		"L 5.743972 20.557281 L 8.118721 13.205566 L 1.901546 8.661963 "
		"L 9.586395 8.661963 Z",
		35,
		35,
		24.0f,
		24.0f,
		1
	);
	test_icon_coverage(
		"allied-ring",
		"M 1.27658 11.9844 C 1.27658 15.2947 2.76875 18.2548 5.11423 20.223 L 4.40278 21.2872 "
		"C 1.71518 19.0848 0 15.7384 0 11.991 C 0 10.9261 0.138494 9.89363 0.398469 8.91047 "
		"L 1.60943 9.31641 C 1.39214 10.1693 1.2766 11.0633 1.2766 11.9844 Z "
		"M 11.957 22.7139 C 14.0322 22.7139 15.9693 22.1193 17.6085 21.0904 L 18.4339 22.1299 "
		"C 16.5745 23.3141 14.3674 24 12 24 C 9.62834 24 7.41733 23.3115 5.55576 22.1232 "
		"L 6.26374 21.0641 C 7.91173 22.1092 9.86412 22.7139 11.957 22.7139 Z "
		"M 22.6374 11.9844 C 22.6374 11.0891 22.5284 10.2194 22.3226 9.38813 L 23.6166 8.96843 "
		"C 23.8669 9.9341 24 10.947 24 11.991 C 24 15.7428 22.2809 19.0927 19.5881 21.2948 "
		"L 18.762 20.2544 C 21.1291 18.2864 22.6374 15.3123 22.6374 11.9844 Z "
		"M 21.8893 8.03171 C 20.4051 4.27119 16.8659 1.5539 12.6637 1.2781 L 12.6637 0 "
		"C 17.4599 0.26164 21.5063 3.34173 23.178 7.61382 Z "
		"M 11.2412 1.27871 C 7.06854 1.55608 3.55095 4.24081 2.05206 7.963 L 0.844001 7.5581 "
		"C 2.51775 3.34317 6.50757 0.301182 11.2412 0.00559011 Z",
		35,
		35,
		24.0f,
		24.0f,
		5
	);
	test_icon_coverage(
		"favorite",
		"M63.9 24.3a2 2 0 0 0-1.6-1.4l-19.7-3-8.8-18.7a2 2 0 0 0-3.6 0l-8.8 18.7-19.7 3a2 2 0 0 0-1.1 3.4L14.9 41l-3.4 20.7a2 2 0 0 0 3 2L32 54l17.6 9.7a2 2 0 0 0 2 0 2 2 0 0 0 1-2L49 41l14.3-14.7a2 2 0 0 0 .5-2",
		64,
		64,
		64.0f,
		64.0f,
		1
	);
}

static void test_shapes_xml_all()
{
	struct shape_case_t {
		const char *d;
		int destW;
		int destH;
		float viewW;
		float viewH;
	} cases[] = {
		{"M 20.8 8.8 L 11.2 20 L 20.8 31.2 Z", 64, 40, 32.0f, 40.0f},
		{"M 0 20 L 128 0 L 128 3.84 L 64 20 L 128 37.6 L 128 40 Z", 128, 40, 128.0f, 40.0f},
		{"M 0 40 L 12 0 L 200 0 L 188 40 Z", 120, 40, 200.0f, 40.0f},
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uir_viewport_t vp;
		uir_path_t path;
		uir_color_t white = {1, 1, 1, 1};
		uir_vert_t verts[UIR_BATCH_MAX_VERTS];
		unsigned short idx[UIR_BATCH_MAX_INDEXES];
		int nv = 0;
		int ni = 0;

		CHECK(UIR_ViewportMake(0, 0, cases[i].destW, cases[i].destH, &vp) == UIR_OK);
		CHECK(test_parse_shape_path(cases[i].d, cases[i].viewW, cases[i].viewH, (float)cases[i].destW, (float)cases[i].destH, &path) == UIR_OK);
		CHECK(UIR_TessFillPath(&vp, &path, &white, 0.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
		CHECK(test_path_coverage_matches(&vp, &path, verts, nv, idx, ni, 4.0f) == 0);
		CHECK(UIR_TessStrokePath(&vp, &path, &white, 2.0f, 0, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
		CHECK(nv > 0);
		UIR_PathFree(&path);
	}
}

static float test_tess_max_edge(const uir_vert_t *verts, int nv, const unsigned short *idx, int ni)
{
	float maxE = 0.0f;
	int i;

	for (i = 0; i + 2 < ni; i += 3) {
		int t;
		for (t = 0; t < 3; t++) {
			int a = idx[i + t];
			int b = idx[i + (t + 1) % 3];
			float dx = verts[b].x - verts[a].x;
			float dy = verts[b].y - verts[a].y;
			float len = sqrtf(dx * dx + dy * dy);

			if (len > maxE) {
				maxE = len;
			}
		}
	}
	(void)nv;
	return maxE;
}

static int test_tess_has_fringe_aa(const uir_vert_t *verts, int nv, const unsigned short *idx, int ni)
{
	int i;

	for (i = 0; i < nv; i++) {
		if (verts[i].a > 0 && verts[i].a < 255) {
			return 1;
		}
	}
	for (i = 0; i + 2 < ni; i += 3) {
		int a0 = idx[i + 0];
		int a1 = idx[i + 1];
		int a2 = idx[i + 2];
		unsigned char va0 = verts[a0].a;
		unsigned char va1 = verts[a1].a;
		unsigned char va2 = verts[a2].a;
		int has0 = (va0 == 0 || va1 == 0 || va2 == 0);
		int has255 = (va0 == 255 || va1 == 255 || va2 == 255);

		if (has0 && has255 && !(va0 == va1 && va1 == va2)) {
			return 1;
		}
	}
	return 0;
}

static float test_tess_max_fringe_edge(const uir_vert_t *verts, int nv, const unsigned short *idx, int ni)
{
	float maxE = 0.0f;
	int i;

	(void)nv;
	for (i = 0; i + 2 < ni; i += 3) {
		int t;
		int a0 = idx[i + 0];
		int a1 = idx[i + 1];
		int a2 = idx[i + 2];
		unsigned char va0 = verts[a0].a;
		unsigned char va1 = verts[a1].a;
		unsigned char va2 = verts[a2].a;
		int has0;
		int has255;

		if ((va0 == 0 || va0 == 255) && (va1 == 0 || va1 == 255) && (va2 == 0 || va2 == 255)) {
			has0 = (va0 == 0 || va1 == 0 || va2 == 0);
			has255 = (va0 == 255 || va1 == 255 || va2 == 255);
			if (!(has0 && has255)) {
				continue;
			}
		}
		for (t = 0; t < 3; t++) {
			int a = idx[i + t];
			int b = idx[i + (t + 1) % 3];
			float dx = verts[b].x - verts[a].x;
			float dy = verts[b].y - verts[a].y;
			float len = sqrtf(dx * dx + dy * dy);

			if (len > maxE) {
				maxE = len;
			}
		}
	}
	return maxE;
}

static float test_tess_max_fringe_outward_extent(
	const uir_vert_t *verts,
	int nv,
	float fringePx
)
{
	float minX = 1e9f;
	float minY = 1e9f;
	float maxX = -1e9f;
	float maxY = -1e9f;
	float maxOut = 0.0f;
	int i;

	for (i = 0; i < nv; i++) {
		if (verts[i].a == 255) {
			if (verts[i].x < minX) {
				minX = verts[i].x;
			}
			if (verts[i].y < minY) {
				minY = verts[i].y;
			}
			if (verts[i].x > maxX) {
				maxX = verts[i].x;
			}
			if (verts[i].y > maxY) {
				maxY = verts[i].y;
			}
		}
	}
	if (minX > maxX || minY > maxY) {
		return 0.0f;
	}
	for (i = 0; i < nv; i++) {
		if (verts[i].a > 0 && verts[i].a < 255) {
			float out = 0.0f;

			if (verts[i].x < minX) {
				out = minX - verts[i].x;
			} else if (verts[i].x > maxX) {
				out = verts[i].x - maxX;
			}
			if (verts[i].y < minY) {
				if (minY - verts[i].y > out) {
					out = minY - verts[i].y;
				}
			} else if (verts[i].y > maxY) {
				if (verts[i].y - maxY > out) {
					out = verts[i].y - maxY;
				}
			}
			if (out > maxOut) {
				maxOut = out;
			}
		}
	}
	(void)fringePx;
	return maxOut;
}

static void test_fringe_implicit_close_no_long_edges()
{
	uir_viewport_t vp;
	uir_path_t path;
	uir_contour_t *c;
	uir_color_t blue = {0.2f, 0.5f, 1.0f, 1.0f};
	uir_vert_t verts[UIR_BATCH_MAX_VERTS];
	unsigned short idx[UIR_BATCH_MAX_INDEXES];
	int nv = 0;
	int ni = 0;
	float maxE;

	CHECK(UIR_ViewportMake(0, 0, 2560, 1440, &vp) == UIR_OK);
	UIR_PathInit(&path);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 100.0f, 100.0f);
	UIR_ContourAddPoint(c, 1700.0f, 100.0f);
	UIR_ContourAddPoint(c, 1700.0f, 500.0f);
	UIR_ContourAddPoint(c, 100.0f, 500.0f);
	CHECK(UIR_TessFillPath(&vp, &path, &blue, 2.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
	maxE = test_tess_max_edge(verts, nv, idx, ni);
	CHECK(maxE < 100.0f);
	UIR_PathFree(&path);
}

static void test_tess_fill_settings_divider_no_long_diagonal()
{
	uir_viewport_t vp;
	uir_path_t path;
	uir_contour_t *c;
	uir_color_t divider = {1.0f, 1.0f, 1.0f, 26.0f / 255.0f};
	uir_vert_t verts[UIR_BATCH_MAX_VERTS];
	unsigned short idx[UIR_BATCH_MAX_INDEXES];
	int nv = 0;
	int ni = 0;
	float maxE;

	CHECK(UIR_ViewportMake(0, 0, 2560, 1440, &vp) == UIR_OK);
	UIR_PathInit(&path);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 0.0f, 64.0f);
	UIR_ContourAddPoint(c, 1718.0f, 64.0f);
	UIR_ContourAddPoint(c, 1718.0f, 65.0f);
	UIR_ContourAddPoint(c, 0.0f, 65.0f);
	CHECK(UIR_TessFillPath(&vp, &path, &divider, 0.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
	maxE = test_tess_max_edge(verts, nv, idx, ni);
	CHECK(maxE < 100.0f);
	UIR_PathFree(&path);
}

static void test_fringe_wide_row_has_aa_no_bowtie()
{
	uir_viewport_t vp;
	uir_path_t path;
	uir_contour_t *c;
	uir_color_t blue = {0.2f, 0.5f, 1.0f, 1.0f};
	uir_vert_t verts[UIR_BATCH_MAX_VERTS];
	unsigned short idx[UIR_BATCH_MAX_INDEXES];
	int nv = 0;
	int ni = 0;
	int i;
	float maxE;

	CHECK(UIR_ViewportMake(0, 0, 2560, 1440, &vp) == UIR_OK);
	UIR_PathInit(&path);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 10.0f, 10.0f);
	UIR_ContourAddPoint(c, 1448.0f, 10.0f);
	UIR_ContourAddPoint(c, 1448.0f, 46.0f);
	UIR_ContourAddPoint(c, 10.0f, 46.0f);
	CHECK(UIR_TessFillPath(&vp, &path, &blue, 2.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
	CHECK(test_tess_has_fringe_aa(verts, nv, idx, ni));
	maxE = test_tess_max_fringe_edge(verts, nv, idx, ni);
	CHECK(maxE < 120.0f);
	UIR_PathFree(&path);
}

static void test_fringe_exit_icon_no_long_edges()
{
	uir_viewport_t vp;
	uir_path_t path;
	uir_color_t white = {1, 1, 1, 1};
	uir_vert_t verts[UIR_BATCH_MAX_VERTS];
	unsigned short idx[UIR_BATCH_MAX_INDEXES];
	int nv = 0;
	int ni = 0;
	float maxE;

	CHECK(UIR_ViewportMake(0, 0, 32, 32, &vp) == UIR_OK);
	CHECK(
		test_parse_shape_path(
			"M 1 8 a 6 6 0 0 1 8.5 -5.4 a 0.8 0.8 0 0 1 -0.6 1.3 a 4.5 4.5 0 1 0 0 8.2 a 0.8 0.8 0 1 1 0.6 1.3 A 6 6 0 0 1 1 8 "
			"M 11.2 4.7 a 1 1 0 0 0 0 1 l 1.4 1.6 H 6.8 a 0.8 0.8 0 0 0 0 1.5 h 5.8 l -1.4 1.4 a 0.8 0.8 0 0 0 1.1 1 l 2.5 -2.7 a 1 1 0 0 0 0 -1 l -2.5 -2.8 a 1 1 0 0 0 -1 0",
			16.0f,
			16.0f,
			32.0f,
			32.0f,
			&path
		) == UIR_OK
	);
	CHECK(UIR_TessFillPath(&vp, &path, &white, 2.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
	maxE = test_tess_max_fringe_edge(verts, nv, idx, ni);
	CHECK(maxE < 120.0f);
	CHECK(test_tess_max_fringe_outward_extent(verts, nv, 2.0f) <= 1.5f);
	UIR_PathFree(&path);
}

static void test_fringe_allied_ring_no_long_quads()
{
	uir_viewport_t vp;
	uir_path_t ring5;
	uir_color_t white = {1, 1, 1, 1};
	uir_vert_t verts[UIR_BATCH_MAX_VERTS];
	unsigned short idx[UIR_BATCH_MAX_INDEXES];
	uir_parse_result_t pr;
	int nv = 0;
	int ni = 0;
	float maxE;

	CHECK(UIR_ViewportMake(0, 0, 35, 35, &vp) == UIR_OK);
	pr = UIR_SvgParsePathD(
		"M 1.27658 11.9844 C 1.27658 15.2947 2.76875 18.2548 5.11423 20.223 L 4.40278 21.2872 "
		"C 1.71518 19.0848 0 15.7384 0 11.991 C 0 10.9261 0.138494 9.89363 0.398469 8.91047 "
		"L 1.60943 9.31641 C 1.39214 10.1693 1.2766 11.0633 1.2766 11.9844 Z "
		"M 11.957 22.7139 C 14.0322 22.7139 15.9693 22.1193 17.6085 21.0904 L 18.4339 22.1299 "
		"C 16.5745 23.3141 14.3674 24 12 24 C 9.62834 24 7.41733 23.3115 5.55576 22.1232 "
		"L 6.26374 21.0641 C 7.91173 22.1092 9.86412 22.7139 11.957 22.7139 Z "
		"M 22.6374 11.9844 C 22.6374 11.0891 22.5284 10.2194 22.3226 9.38813 L 23.6166 8.96843 "
		"C 23.8669 9.9341 24 10.947 24 11.991 C 24 15.7428 22.2809 19.0927 19.5881 21.2948 "
		"L 18.762 20.2544 C 21.1291 18.2864 22.6374 15.3123 22.6374 11.9844 Z "
		"M 21.8893 8.03171 C 20.4051 4.27119 16.8659 1.5539 12.6637 1.2781 L 12.6637 0 "
		"C 17.4599 0.26164 21.5063 3.34173 23.178 7.61382 Z "
		"M 11.2412 1.27871 C 7.06854 1.55608 3.55095 4.24081 2.05206 7.963 L 0.844001 7.5581 "
		"C 2.51775 3.34317 6.50757 0.301182 11.2412 0.00559011 Z",
		0.25f,
		&ring5
	);
	CHECK(pr.status == UIR_OK);
	CHECK(UIR_TessFillPath(&vp, &ring5, &white, 2.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
	maxE = test_tess_max_edge(verts, nv, idx, ni);
	CHECK(maxE < 150.0f);
	UIR_PathFree(&ring5);
}

static void test_fringe_large_panel_has_aa_no_bowtie()
{
	uir_viewport_t vp;
	uir_path_t path;
	uir_contour_t *c;
	uir_color_t blue = {0.2f, 0.5f, 1.0f, 1.0f};
	uir_vert_t verts[UIR_BATCH_MAX_VERTS];
	unsigned short idx[UIR_BATCH_MAX_INDEXES];
	int nv = 0;
	int ni = 0;
	float maxE;

	CHECK(UIR_ViewportMake(0, 0, 2560, 1440, &vp) == UIR_OK);
	UIR_PathInit(&path);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 10.0f, 10.0f);
	UIR_ContourAddPoint(c, 1448.0f, 10.0f);
	UIR_ContourAddPoint(c, 1448.0f, 200.0f);
	UIR_ContourAddPoint(c, 10.0f, 200.0f);
	CHECK(UIR_TessFillPath(&vp, &path, &blue, 2.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
	CHECK(test_tess_has_fringe_aa(verts, nv, idx, ni));
	maxE = test_tess_max_fringe_edge(verts, nv, idx, ni);
	CHECK(maxE < 120.0f);
	UIR_PathFree(&path);
}

static void test_skew_rect_edge_profile()
{
	uir_viewport_t vp;
	uir_path_t path;
	uir_color_t white = {1, 1, 1, 1};
	uir_vert_t verts[UIR_BATCH_MAX_VERTS];
	unsigned short idx[UIR_BATCH_MAX_INDEXES];
	int nv = 0;
	int ni = 0;
	float avgScale;
	float fringeFbPx;

	CHECK(UIR_ViewportMake(0, 0, 120, 40, &vp) == UIR_OK);
	CHECK(test_parse_shape_path("M 0 40 L 12 0 L 200 0 L 188 40 Z", 200.0f, 40.0f, 120.0f, 40.0f, &path) == UIR_OK);
	avgScale = 0.5f * (vp.scaleX + vp.scaleY);
	fringeFbPx = UIR_TessDefaultFringeFbPx(avgScale);
	CHECK(UIR_TessFillPath(&vp, &path, &white, fringeFbPx, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
	CHECK(test_tess_has_fringe_aa(verts, nv, idx, ni));
	UIR_PathFree(&path);
}

static void test_stroke_1px_diagonal()
{
	uir_viewport_t vp;
	uir_path_t path;
	uir_contour_t *c;
	uir_color_t white = {1, 1, 1, 1};
	uir_vert_t verts[UIR_BATCH_MAX_VERTS];
	unsigned short idx[UIR_BATCH_MAX_INDEXES];
	int nv = 0;
	int ni = 0;

	CHECK(UIR_ViewportMake(0, 0, 64, 64, &vp) == UIR_OK);
	UIR_PathInit(&path);
	CHECK(UIR_PathBeginContour(&path, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 4.0f, 60.0f);
	UIR_ContourAddPoint(c, 60.0f, 4.0f);
	CHECK(UIR_TessStrokePath(&vp, &path, &white, 1.0f, 0, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
	CHECK(nv > 0);
	CHECK(test_tess_has_fringe_aa(verts, nv, idx, ni));
	UIR_PathFree(&path);
}

static void test_fringe_wider_at_2x_scale()
{
	uir_viewport_t vp1;
	uir_viewport_t vp2;
	uir_path_t path;
	uir_color_t white = {1, 1, 1, 1};
	uir_vert_t verts1[UIR_BATCH_MAX_VERTS];
	uir_vert_t verts2[UIR_BATCH_MAX_VERTS];
	unsigned short idx1[UIR_BATCH_MAX_INDEXES];
	unsigned short idx2[UIR_BATCH_MAX_INDEXES];
	int nv1 = 0;
	int ni1 = 0;
	int nv2 = 0;
	int ni2 = 0;
	float maxR1 = 0.0f;
	float maxR2 = 0.0f;
	float cx = 32.0f;
	float cy = 32.0f;

	CHECK(test_parse_shape_path(
		      "M 11.961142 1.310257 L 14.335891 8.661963 L 22.020748 8.661963 "
		      "L 15.803569 13.205566 L 18.17832 20.557281 L 11.961142 16.013676 "
		      "L 5.743972 20.557281 L 8.118721 13.205566 L 1.901546 8.661963 "
		      "L 9.586395 8.661963 Z",
		      24.0f,
		      24.0f,
		      64.0f,
		      64.0f,
		      &path
	      ) == UIR_OK);
	CHECK(UIR_ViewportMake(0, 0, 64, 64, &vp1) == UIR_OK);
	CHECK(UIR_ViewportMake(0, 0, 128, 128, &vp2) == UIR_OK);
	CHECK(UIR_TessFillPath(&vp1, &path, &white, 1.25f, verts1, UIR_BATCH_MAX_VERTS, &nv1, idx1, UIR_BATCH_MAX_INDEXES, &ni1) == UIR_OK);
	CHECK(UIR_TessFillPath(&vp2, &path, &white, 2.5f, verts2, UIR_BATCH_MAX_VERTS, &nv2, idx2, UIR_BATCH_MAX_INDEXES, &ni2) == UIR_OK);
	for (int i = 0; i < nv1; i++) {
		if (verts1[i].a == 0) {
			float dx = verts1[i].x - cx;
			float dy = verts1[i].y - cy;
			float r = std::sqrt(dx * dx + dy * dy);
			if (r > maxR1) {
				maxR1 = r;
			}
		}
	}
	cx = 64.0f;
	cy = 64.0f;
	for (int i = 0; i < nv2; i++) {
		if (verts2[i].a == 0) {
			float dx = verts2[i].x - cx;
			float dy = verts2[i].y - cy;
			float r = std::sqrt(dx * dx + dy * dy);
			if (r > maxR2) {
				maxR2 = r;
			}
		}
	}
	CHECK(maxR2 >= maxR1);
	UIR_PathFree(&path);
}

static int test_point_in_tri(
	float px,
	float py,
	float ax,
	float ay,
	float bx,
	float by,
	float cx,
	float cy
)
{
	float abx = bx - ax;
	float aby = by - ay;
	float bcx = cx - bx;
	float bcy = cy - by;
	float cax = ax - cx;
	float cay = ay - cy;
	float apx = px - ax;
	float apy = py - ay;
	float bpx = px - bx;
	float bpy = py - by;
	float cpx = px - cx;
	float cpy = py - cy;
	float s1 = abx * apy - aby * apx;
	float s2 = bcx * bpy - bcy * bpx;
	float s3 = cax * cpy - cay * cpx;
	int ccw = (s1 >= -1e-5f && s2 >= -1e-5f && s3 >= -1e-5f);
	int cw = (s1 <= 1e-5f && s2 <= 1e-5f && s3 <= 1e-5f);
	return ccw || cw;
}

static int test_tess_cover_grid(
	const uir_vert_t *verts,
	int nv,
	const unsigned short *idx,
	int ni,
	float px,
	float py
)
{
	int i;

	(void)nv;
	for (i = 0; i + 2 < ni; i += 3) {
		const uir_vert_t *a = &verts[idx[i + 0]];
		const uir_vert_t *b = &verts[idx[i + 1]];
		const uir_vert_t *c = &verts[idx[i + 2]];
		if (test_point_in_tri(px, py, a->x, a->y, b->x, b->y, c->x, c->y)) {
			return 1;
		}
	}
	return 0;
}

static int test_point_in_star(const uir_path_t *path, float px, float py)
{
	int c;
	int wn = 0;

	for (c = 0; c < path->contourCount; c++) {
		const uir_contour_t *contour = &path->contours[c];
		int i;
		if (contour->count < 3) {
			continue;
		}
		for (i = 0; i < contour->count; i++) {
			float xi = contour->points[i].x;
			float yi = contour->points[i].y;
			float xj = contour->points[(i + 1) % contour->count].x;
			float yj = contour->points[(i + 1) % contour->count].y;
			if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi + 1e-30f) + xi)) {
				wn += (yi < yj) ? 1 : -1;
			}
		}
	}
	return wn != 0;
}

static void test_tess_star_and_multi_contour()
{
	uir_viewport_t vp;
	uir_color_t white = {1, 1, 1, 1};
	uir_vert_t verts[UIR_BATCH_MAX_VERTS];
	unsigned short idx[UIR_BATCH_MAX_INDEXES];
	uir_path_t star;
	uir_path_t starRev;
	uir_path_t ring5;
	uir_path_t holePath;
	uir_contour_t *c;
	uir_parse_result_t pr;
	int nv, ni;
	int matches = 0;
	int mismatches = 0;
	int sx, sy;

	CHECK(UIR_ViewportMake(0, 0, 35, 35, &vp) == UIR_OK);

	pr = UIR_SvgParsePathD(
		"M 11.961142 1.310257 L 14.335891 8.661963 L 22.020748 8.661963 "
		"L 15.803569 13.205566 L 18.17832 20.557281 L 11.961142 16.013676 "
		"L 5.743972 20.557281 L 8.118721 13.205566 L 1.901546 8.661963 "
		"L 9.586395 8.661963 Z",
		0.25f,
		&star
	);
	CHECK(pr.status == UIR_OK);
	CHECK(UIR_TessFillPath(&vp, &star, &white, 0.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
	CHECK(nv >= 10);
	CHECK(ni >= 24);

	UIR_PathInit(&starRev);
	CHECK(UIR_PathBeginContour(&starRev, &c) == UIR_OK);
	for (int i = star.contours[0].count - 1; i >= 0; i--) {
		UIR_ContourAddPoint(c, star.contours[0].points[i].x, star.contours[0].points[i].y);
	}
	UIR_ContourClose(c);
	CHECK(UIR_TessFillPath(&vp, &starRev, &white, 0.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);

	CHECK(UIR_TessFillPath(&vp, &star, &white, 0.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);

	for (sy = 4; sy < 31; sy += 4) {
		for (sx = 4; sx < 31; sx += 4) {
			float px = (float)sx;
			float py = (float)sy;
			int cpu = test_point_in_star(&star, px, py);
			if (test_tess_cover_grid(verts, nv, idx, ni, px, py) == cpu) {
				matches++;
			} else {
				mismatches++;
			}
		}
	}
	CHECK(mismatches == 0);
	CHECK(matches > 0);
	CHECK(test_tess_cover_grid(verts, nv, idx, ni, 12.0f, 4.0f));

	pr = UIR_SvgParsePathD(
		"M 1.27658 11.9844 C 1.27658 15.2947 2.76875 18.2548 5.11423 20.223 L 4.40278 21.2872 "
		"C 1.71518 19.0848 0 15.7384 0 11.991 C 0 10.9261 0.138494 9.89363 0.398469 8.91047 "
		"L 1.60943 9.31641 C 1.39214 10.1693 1.2766 11.0633 1.2766 11.9844 Z "
		"M 11.957 22.7139 C 14.0322 22.7139 15.9693 22.1193 17.6085 21.0904 L 18.4339 22.1299 "
		"C 16.5745 23.3141 14.3674 24 12 24 C 9.62834 24 7.41733 23.3115 5.55576 22.1232 "
		"L 6.26374 21.0641 C 7.91173 22.1092 9.86412 22.7139 11.957 22.7139 Z "
		"M 22.6374 11.9844 C 22.6374 11.0891 22.5284 10.2194 22.3226 9.38813 L 23.6166 8.96843 "
		"C 23.8669 9.9341 24 10.947 24 11.991 C 24 15.7428 22.2809 19.0927 19.5881 21.2948 "
		"L 18.762 20.2544 C 21.1291 18.2864 22.6374 15.3123 22.6374 11.9844 Z "
		"M 21.8893 8.03171 C 20.4051 4.27119 16.8659 1.5539 12.6637 1.2781 L 12.6637 0 "
		"C 17.4599 0.26164 21.5063 3.34173 23.178 7.61382 Z "
		"M 11.2412 1.27871 C 7.06854 1.55608 3.55095 4.24081 2.05206 7.963 L 0.844001 7.5581 "
		"C 2.51775 3.34317 6.50757 0.301182 11.2412 0.00559011 Z",
		0.25f,
		&ring5
	);
	CHECK(pr.status == UIR_OK);
	CHECK(ring5.contourCount == 5);
	CHECK(UIR_TessFillPath(&vp, &ring5, &white, 0.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
	CHECK(UIR_TessStrokePath(&vp, &ring5, &white, 1.0f, 0, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
	CHECK(nv > 0);
	CHECK(ni > 0);

	UIR_PathInit(&holePath);
	CHECK(UIR_PathBeginContour(&holePath, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 2, 2);
	UIR_ContourAddPoint(c, 60, 2);
	UIR_ContourAddPoint(c, 60, 60);
	UIR_ContourAddPoint(c, 2, 60);
	UIR_ContourClose(c);
	CHECK(UIR_PathBeginContour(&holePath, &c) == UIR_OK);
	UIR_ContourAddPoint(c, 20, 20);
	UIR_ContourAddPoint(c, 40, 20);
	UIR_ContourAddPoint(c, 40, 40);
	UIR_ContourAddPoint(c, 20, 40);
	UIR_ContourClose(c);
	holePath.fillRule = UIR_FILL_EVEN_ODD;
	CHECK(UIR_TessFillPath(&vp, &holePath, &white, 0.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);
	holePath.fillRule = UIR_FILL_NON_ZERO;
	CHECK(UIR_TessFillPath(&vp, &holePath, &white, 0.0f, verts, UIR_BATCH_MAX_VERTS, &nv, idx, UIR_BATCH_MAX_INDEXES, &ni) == UIR_OK);

	UIR_PathFree(&holePath);
	UIR_PathFree(&ring5);
	UIR_PathFree(&starRev);
	UIR_PathFree(&star);
}

static void test_tess_shapes_acceptance()
{
	test_shapes_xml_all();
}

static void test_gradient_parse_and_raster()
{
	uir_gradient_t g;
	unsigned char strip[256 * 4];
	unsigned char rgba2d[64 * 64 * 4];

	CHECK(UIR_GradientIsBrush("linear(180deg, #00000000, #FFFFFFFF)"));
	CHECK(UIR_GradientIsBrush(" radial(50% 50%, #FFFFFFFF, #00000000)"));
	CHECK(!UIR_GradientIsBrush("#FF0000FF"));
	CHECK(UIR_GradientParse("not-a-brush", &g) == UIR_ERR_PARSE);
	CHECK(UIR_GradientParse("linear(180deg, #00000000, #FFFFFFFF)", &g) == UIR_OK);
	CHECK(g.kind == UIR_GRADIENT_LINEAR);
	CHECK(g.stopCount == 2);
	CHECK(UIR_GradientRasterize(&g, 1, 256, strip) == UIR_OK);
	/* 180deg = to bottom: first texel near top stop (transparent), last near opaque. */
	CHECK(strip[3] < 8);
	CHECK(strip[255 * 4 + 3] > 240);

	CHECK(UIR_GradientParse("linear(90deg, #FF0000FF 0%, #0000FFFF 100%)", &g) == UIR_OK);
	CHECK(UIR_GradientRasterize(&g, 256, 1, strip) == UIR_OK);
	CHECK(strip[0] > 240 && strip[2] < 16);
	CHECK(strip[255 * 4 + 0] < 16 && strip[255 * 4 + 2] > 240);

	CHECK(UIR_GradientParse("radial(50% 50%, #FFFFFFFF 0%, #FFFFFF00 100%)", &g) == UIR_OK);
	CHECK(UIR_GradientRasterize(&g, 64, 64, rgba2d) == UIR_OK);
	{
		const unsigned char *center = &rgba2d[(32 * 64 + 32) * 4];
		const unsigned char *corner = &rgba2d[0];
		CHECK(center[3] > 200);
		CHECK(corner[3] < 40);
	}
}

int main()
{
	test_viewport();
	test_ref_px_scale();
	test_fov();
	test_path_fill();
	test_path_stroke();
	test_svg();
	test_path_rotate();
	test_fill_rule_smoke();
	test_model_preview_helpers();
	test_menu_world_view_switch();
	test_map_env_parse_entities();
	test_map_env_parse_entities_classname_after_model();
	test_map_env_parse_script();
	test_menu_weather_skip_without_density();
	test_menu_weather_spawns_from_script_density();
	test_font_math();
	test_image_fit_math();
	test_image_repeat_tiling();
	test_image_rotation_quad();
	test_gpu_tess_and_batch();
	test_tess_star_and_multi_contour();
	test_icon_coverage_suite();
	test_skew_rect_edge_profile();
	test_fringe_implicit_close_no_long_edges();
	test_tess_fill_settings_divider_no_long_diagonal();
	test_fringe_wide_row_has_aa_no_bowtie();
	test_fringe_exit_icon_no_long_edges();
	test_fringe_allied_ring_no_long_quads();
	test_fringe_large_panel_has_aa_no_bowtie();
	test_stroke_1px_diagonal();
	test_fringe_wider_at_2x_scale();
	test_tess_shapes_acceptance();
	test_image_axis_aligned_clip_scissor();
	test_gradient_parse_and_raster();

	if (g_failures) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("uirender tests ok\n");
	return 0;
}
