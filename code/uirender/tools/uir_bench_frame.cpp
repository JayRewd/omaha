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

#include "../uir_viewport.h"
#include "../uir_path.h"
#include "../uir_svg.h"
#include "../uir_draw2d.h"
#include "../uir_batch.h"

#include "../qcommon/q_shared.h"

#include <cstdio>
#include <cstring>

extern "C" {
void QDECL Com_Printf(const char *msg, ...)
{
	(void)msg;
}
void QDECL Com_Error(int level, const char *error, ...)
{
	(void)level;
	(void)error;
}
}

static int g_boxDrawCount = 0;

static void bench_set_color(const float *rgba)
{
	(void)rgba;
}

static void bench_draw_box(float x, float y, float w, float h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	g_boxDrawCount++;
}

static void bench_skew_rect(int scaleX, int scaleY, const char *label)
{
	uir_viewport_t vp;
	uir_path_t path;
	uir_color_t fill = {0.12f, 0.12f, 0.12f, 0.85f};
	uir_color_t stroke = {1.0f, 1.0f, 1.0f, 1.0f};
	uir_draw2d_backend_t backend;
	uir_stats_t stats;
	uir_viewbox_t view = {0, 0, 350, 100};
	uir_rect_t dest = {0, 0, 350, 100};
	const char *d =
		"M 0 100 L 50 0 L 350 0 L 300 100 Z";
	uir_parse_result_t pr;

	memset(&backend, 0, sizeof(backend));
	backend.setColor = bench_set_color;
	backend.drawBox = bench_draw_box;
	UIR_Draw2D_SetBackend(&backend);

	UIR_ViewportMakeOrtho(0, 0, 350 * scaleX, 100 * scaleY, 0.0f, 350.0f, 0.0f, 100.0f, &vp);

	pr = UIR_SvgParsePathD(d, 0.25f, &path);
	if (pr.status != UIR_OK) {
		std::fprintf(stderr, "bench: parse failed\n");
		return;
	}

	g_boxDrawCount = 0;
	memset(&stats, 0, sizeof(stats));
	UIR_Draw2D_Path(&vp, &path, &fill, &stats, 0, 0);
	UIR_Draw2D_PathStroke(&vp, &path, &stroke, 1.0f, &stats, 0);
	std::printf(
		"%s %dx: sampled=%d runs=%d boxes=%d tessFallbacks=%d\n",
		label,
		scaleX,
		stats.sampledPixels,
		stats.emittedRuns,
		g_boxDrawCount,
		stats.tessFallbacks
	);
	UIR_PathFree(&path);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	bench_skew_rect(1, 1, "span-1x");
	bench_skew_rect(2, 2, "span-2x");
	return 0;
}
