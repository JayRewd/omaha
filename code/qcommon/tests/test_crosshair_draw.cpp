/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "crosshair_draw.h"

#include "qcommon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static void expect_true(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		g_failures++;
	}
}

static void test_open_rect_count(void)
{
	xhair_config_t cfg;
	xhair_frame_t frame;
	xhair_rect_t rects[XHAIR_MAX_RECTS];
	int count;

	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = XHAIR_MODE_OPEN;
	cfg.style = 4;
	cfg.gap = 2.0f;
	cfg.length = 3.0f;
	cfg.thickness = 1.0f;
	cfg.r = cfg.g = cfg.b = 1.0f;
	cfg.a = 1.0f;
	cfg.drawOutline = qtrue;
	cfg.outline = 1.0f;

	XHair_BuildFrame(&cfg, 320.0f, 240.0f, 1.0f, 1.0f, &frame);
	count = XHair_EmitRects(&frame, XHAIR_PASS_FILL, rects, XHAIR_MAX_RECTS);
	expect_true(count == 4, "open crosshair emits four arms");

	cfg.centerDot = qtrue;
	XHair_BuildFrame(&cfg, 320.0f, 240.0f, 1.0f, 1.0f, &frame);
	count = XHair_EmitRects(&frame, XHAIR_PASS_FILL, rects, XHAIR_MAX_RECTS);
	expect_true(count == 5, "open crosshair with center dot emits square dot");
}

static void test_t_style(void)
{
	xhair_config_t cfg;
	xhair_frame_t frame;
	xhair_rect_t rects[XHAIR_MAX_RECTS];
	int count;

	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = XHAIR_MODE_OPEN;
	cfg.style = 4;
	cfg.gap = 2.0f;
	cfg.length = 3.0f;
	cfg.thickness = 1.0f;
	cfg.tStyle = qtrue;
	cfg.r = cfg.g = cfg.b = 1.0f;
	cfg.a = 1.0f;

	XHair_BuildFrame(&cfg, 320.0f, 240.0f, 1.0f, 1.0f, &frame);
	count = XHair_EmitRects(&frame, XHAIR_PASS_FILL, rects, XHAIR_MAX_RECTS);
	expect_true(count == 3, "T-style open crosshair emits three arms");
}

static void test_dot_mode(void)
{
	xhair_config_t cfg;
	xhair_frame_t frame;
	xhair_rect_t rects[XHAIR_MAX_RECTS];
	int count;

	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = XHAIR_MODE_DOT;
	cfg.dotRadius = 2.0f;
	cfg.dynamicEnabled = qtrue;
	cfg.dynamicSpreadPx = 4.0f;
	cfg.dynamicScale = 1.0f;
	cfg.length = 3.0f;
	cfg.r = cfg.g = cfg.b = 1.0f;
	cfg.a = 1.0f;

	XHair_BuildFrame(&cfg, 320.0f, 240.0f, 1.0f, 1.0f, &frame);
	count = XHair_EmitRects(&frame, XHAIR_PASS_FILL, rects, XHAIR_MAX_RECTS);
	expect_true(count >= 3, "dot mode emits scanline circle");
	expect_true(frame.dotRadius > cfg.dotRadius, "dot radius expands with dynamic spread");
}

static void test_dynamic_scale(void)
{
	xhair_config_t cfg;
	xhair_frame_t frame;

	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = XHAIR_MODE_OPEN;
	cfg.gap = 0.0f;
	cfg.length = 5.0f;
	cfg.thickness = 1.0f;
	cfg.dynamicEnabled = qtrue;
	cfg.dynamicSpreadPx = 10.0f;
	cfg.dynamicScale = 0.5f;
	cfg.r = cfg.g = cfg.b = 1.0f;
	cfg.a = 1.0f;

	XHair_BuildFrame(&cfg, 320.0f, 240.0f, 1.0f, 1.0f, &frame);
	expect_true(frame.dynamicSpreadPx == 5.0f, "dynamic scale halves spread contribution");
}

static void test_dynamic_open_gap(void)
{
	xhair_config_t cfg;
	xhair_frame_t frame;

	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = XHAIR_MODE_OPEN;
	cfg.gap = -1.0f;
	cfg.length = 5.0f;
	cfg.thickness = 1.0f;
	cfg.dynamicEnabled = qtrue;
	cfg.dynamicSpreadPx = 6.0f;
	cfg.dynamicScale = 1.0f;
	cfg.r = cfg.g = cfg.b = 1.0f;
	cfg.a = 1.0f;

	XHair_BuildFrame(&cfg, 960.0f, 540.0f, 1.0f, 1.0f, &frame);
	expect_true(frame.gap == -1.0f, "open gap base unchanged in frame");
	expect_true(frame.dynamicSpreadPx == 6.0f, "dynamic spread preserved for emit");
}

static void test_dynamic_split_rect_count(void)
{
	xhair_config_t cfg;
	xhair_frame_t frame;
	xhair_rect_t rects[XHAIR_MAX_RECTS];
	int count;

	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = XHAIR_MODE_OPEN;
	cfg.gap = 0.0f;
	cfg.length = 5.0f;
	cfg.thickness = 1.0f;
	cfg.dynamicEnabled = qtrue;
	cfg.dynamicSpreadPx = 12.0f;
	cfg.dynamicScale = 1.0f;
	cfg.r = cfg.g = cfg.b = 1.0f;
	cfg.a = 1.0f;

	XHair_BuildFrame(&cfg, 320.0f, 240.0f, 1.0f, 1.0f, &frame);
	count = XHair_EmitRects(&frame, XHAIR_PASS_FILL, rects, XHAIR_MAX_RECTS);
	expect_true(count == 4, "dynamic open crosshair emits only the four main arms");
}

static void test_color_preset(void)
{
	float r;
	float g;
	float b;
	float a;

	XHair_ResolveColor(4, 0.0f, 0.0f, 0.0f, qtrue, 200, &r, &g, &b, &a);
	expect_true(r > 0.9f && g < 0.1f && b < 0.1f, "color preset 4 resolves to red");
	expect_true(a > 0.75f && a < 0.85f, "alpha scaling uses 0-255 input");
}

static void test_recoil_offset(void)
{
	xhair_config_t cfg;
	xhair_frame_t frame;

	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = XHAIR_MODE_DOT;
	cfg.dotRadius = 2.0f;
	cfg.r = cfg.g = cfg.b = 1.0f;
	cfg.a = 1.0f;

	XHair_BuildFrame(&cfg, 100.0f, 200.0f, 1.0f, 1.0f, &frame);
	expect_true(frame.cx == 100.0f && frame.cy == 200.0f, "frame preserves draw center");
}

int main(void)
{
	test_open_rect_count();
	test_t_style();
	test_dot_mode();
	test_dynamic_scale();
	test_dynamic_open_gap();
	test_dynamic_split_rect_count();
	test_color_preset();
	test_recoil_offset();

	if (g_failures > 0) {
		fprintf(stderr, "%d crosshair_draw test(s) failed\n", g_failures);
		return EXIT_FAILURE;
	}

	printf("test_crosshair_draw: all tests passed\n");
	return EXIT_SUCCESS;
}
