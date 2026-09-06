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
	expect_true(count >= 1, "dot mode emits disc spans");
	expect_true(frame.dotRadius > cfg.dotRadius, "dot radius expands with dynamic spread");
}

#define XHAIR_MASK_W 16
#define XHAIR_MASK_H 16

static void mask_clear(char grid[XHAIR_MASK_H][XHAIR_MASK_W], int *ox, int *oy)
{
	int y;
	int x;

	for (y = 0; y < XHAIR_MASK_H; y++) {
		for (x = 0; x < XHAIR_MASK_W; x++) {
			grid[y][x] = ' ';
		}
	}
	*ox = XHAIR_MASK_W / 2;
	*oy = XHAIR_MASK_H / 2;
}

static void mask_stamp_rects(
	char grid[XHAIR_MASK_H][XHAIR_MASK_W],
	int ox,
	int oy,
	const xhair_rect_t *rects,
	int count,
	char ch
)
{
	int i;
	int px;
	int py;
	int x;

	for (i = 0; i < count; i++) {
		py = (int)rects[i].y - oy + XHAIR_MASK_H / 2;
		for (x = 0; x < (int)rects[i].w; x++) {
			px = (int)rects[i].x + x - ox + XHAIR_MASK_W / 2;
			if (px >= 0 && px < XHAIR_MASK_W && py >= 0 && py < XHAIR_MASK_H) {
				grid[py][px] = ch;
			}
		}
	}
}

static void mask_trim_compare(const char grid[XHAIR_MASK_H][XHAIR_MASK_W], const char *const *expect, const char *label)
{
	int minx = XHAIR_MASK_W;
	int maxx = -1;
	int miny = XHAIR_MASK_H;
	int maxy = -1;
	int x;
	int y;
	int row;
	int expectRows;

	for (y = 0; y < XHAIR_MASK_H; y++) {
		for (x = 0; x < XHAIR_MASK_W; x++) {
			if (grid[y][x] != ' ') {
				if (x < minx) {
					minx = x;
				}
				if (x > maxx) {
					maxx = x;
				}
				if (y < miny) {
					miny = y;
				}
				if (y > maxy) {
					maxy = y;
				}
			}
		}
	}
	expect_true(maxx >= minx && maxy >= miny, label);

	expectRows = 0;
	while (expect[expectRows] != NULL) {
		expectRows++;
	}
	expect_true(expectRows == (maxy - miny + 1), label);

	for (row = 0; row < expectRows; row++) {
		const char *e = expect[row];
		int          elen = (int)strlen(e);
		char         got[XHAIR_MASK_W + 1];
		int          i;

		expect_true(elen == (maxx - minx + 1), label);
		for (i = 0; i < elen; i++) {
			got[i] = grid[miny + row][minx + i];
		}
		got[elen] = '\0';
		if (strcmp(got, e) != 0) {
			fprintf(stderr, "FAIL: %s row %d got '%s' expected '%s'\n", label, row, got, e);
			g_failures++;
		}
	}
}

static void expect_dot_mask(float size, float outline, const char *const *expectFill, const char *const *expectCombined)
{
	xhair_config_t cfg;
	xhair_frame_t frame;
	xhair_rect_t fillRects[XHAIR_MAX_RECTS];
	xhair_rect_t outlineRects[XHAIR_MAX_RECTS];
	char grid[XHAIR_MASK_H][XHAIR_MASK_W];
	int ox;
	int oy;
	int fillCount;
	int outlineCount;
	char label[64];

	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = XHAIR_MODE_DOT;
	cfg.dotRadius = size;
	cfg.drawOutline = (outline > 0.0f) ? qtrue : qfalse;
	cfg.outline = outline;
	cfg.r = cfg.g = cfg.b = 1.0f;
	cfg.a = 1.0f;

	XHair_BuildFrame(&cfg, 100.0f, 100.0f, 1.0f, 1.0f, &frame);
	fillCount = XHair_EmitRects(&frame, XHAIR_PASS_FILL, fillRects, XHAIR_MAX_RECTS);
	outlineCount = XHair_EmitRects(&frame, XHAIR_PASS_OUTLINE, outlineRects, XHAIR_MAX_RECTS);

	mask_clear(grid, &ox, &oy);
	/* Origin for stamping: use snapped center as integer pixel reference. */
	{
		const int idiam = (int)floorf(size + 0.5f);
		if (idiam & 1) {
			ox = 100; /* floor(100)+0.5 → stamps relative to pixel 100 */
			oy = 100;
		} else {
			ox = 100;
			oy = 100;
		}
	}
	/*
	 * Rects are in absolute FB coords. Convert using the first fill rect's
	 * geometry: stamp with ox/oy = 100 so pixel 100 maps to mask center.
	 */
	ox = 100;
	oy = 100;
	mask_stamp_rects(grid, ox, oy, fillRects, fillCount, '#');
	snprintf(label, sizeof(label), "fill golden size=%.0f", size);
	mask_trim_compare(grid, expectFill, label);

	if (expectCombined) {
		mask_clear(grid, &ox, &oy);
		ox = 100;
		oy = 100;
		mask_stamp_rects(grid, ox, oy, outlineRects, outlineCount, '.');
		mask_stamp_rects(grid, ox, oy, fillRects, fillCount, '#');
		snprintf(label, sizeof(label), "two-circle golden size=%.0f outline=%.0f", size, outline);
		mask_trim_compare(grid, expectCombined, label);
	}
}

static void test_dot_golden_masks(void)
{
	/* Odd diameter → pixel-center; even → pixel-corner. Two-circle Euclidean. */
	static const char *fill4[] = {" ## ", "####", "####", " ## ", NULL};
	static const char *both4[] = {" .... ", "..##..", ".####.", ".####.", "..##..", " .... ", NULL};

	static const char *fill5[] = {" ### ", "#####", "#####", "#####", " ### ", NULL};
	static const char *both5[] = {
		"  ...  ", " .###. ", ".#####.", ".#####.", ".#####.", " .###. ", "  ...  ", NULL
	};

	static const char *fill6[] = {" #### ", "######", "######", "######", "######", " #### ", NULL};
	static const char *both6[] = {
		"  ....  ", " .####. ", ".######.", ".######.", ".######.", ".######.", " .####. ", "  ....  ",
		NULL
	};

	static const char *fill7[] = {"  ###  ", " ##### ", "#######", "#######", "#######", " ##### ", "  ###  ", NULL};

	expect_dot_mask(4.0f, 0.0f, fill4, NULL);
	expect_dot_mask(4.0f, 1.0f, fill4, both4);
	expect_dot_mask(5.0f, 0.0f, fill5, NULL);
	expect_dot_mask(5.0f, 1.0f, fill5, both5);
	expect_dot_mask(6.0f, 0.0f, fill6, NULL);
	expect_dot_mask(6.0f, 1.0f, fill6, both6);
	expect_dot_mask(7.0f, 0.0f, fill7, NULL);
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
	test_dot_golden_masks();
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
