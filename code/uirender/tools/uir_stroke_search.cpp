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
 * Offline stroke QA matching main.xml allied-star paint:
 *   fill star + fill ring annulus, then stroke both with element stroke.
 * Scores (fail if any > 0 / below threshold):
 *   star_gap   — dark samples between white fill and green stroke at tips
 *   ring_flood — mid-ring samples that are green (white fill erased)
 *   ring_seam  — angular gaps on outer stroke ridge
 */

#include "uir_path.h"
#include "uir_svg.h"
#include "uir_viewport.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *kStarD =
	"M 11.961142 1.310257 L 14.335891 8.661963 L 22.020748 8.661963 "
	"L 15.803569 13.205566 L 18.17832 20.557281 L 11.961142 16.013676 "
	"L 5.743972 20.557281 L 8.118721 13.205566 L 1.901546 8.661963 "
	"L 9.586395 8.661963 Z";

static const char *kRingAnnulusD =
	"M 12 0 A 12 12 0 1 1 12 24 A 12 12 0 1 1 12 0 Z "
	"M 12 1.27658 A 10.72342 10.72342 0 1 0 12 22.72342 A 10.72342 10.72342 0 1 0 12 1.27658 Z";

static const char *kRingLobesD =
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
	"C 2.51775 3.34317 6.50757 0.301182 11.2412 0.00559011 Z";

enum { CH_EMPTY = 0, CH_FILL = 1, CH_STROKE = 2 };

struct Bitmap {
	int w = 0;
	int h = 0;
	std::vector<unsigned char> ch;

	void clear(int width, int height)
	{
		w = width;
		h = height;
		ch.assign((size_t)w * (size_t)h, CH_EMPTY);
	}

	unsigned char at(int x, int y) const
	{
		if (x < 0 || y < 0 || x >= w || y >= h) {
			return CH_EMPTY;
		}
		return ch[(size_t)y * (size_t)w + (size_t)x];
	}

	void stamp(float x, float y, float bw, float bh, unsigned char tag)
	{
		if (bw <= 0.0f || bh <= 0.0f) {
			return;
		}
		int x0 = (int)std::floor(x);
		int y0 = (int)std::floor(y);
		int x1 = (int)std::ceil(x + bw);
		int y1 = (int)std::ceil(y + bh);
		if (x0 < 0) {
			x0 = 0;
		}
		if (y0 < 0) {
			y0 = 0;
		}
		if (x1 > w) {
			x1 = w;
		}
		if (y1 > h) {
			y1 = h;
		}
		for (int py = y0; py < y1; py++) {
			for (int px = x0; px < x1; px++) {
				unsigned char &c = ch[(size_t)py * (size_t)w + (size_t)px];
				if (tag == CH_STROKE) {
					c = CH_STROKE;
				} else if (tag == CH_FILL && c != CH_STROKE) {
					c = CH_FILL;
				}
			}
		}
	}
};

struct SinkCtx {
	Bitmap *bm;
	unsigned char tag;
};

static void draw_box_sink(float x, float y, float w, float h, const uir_color_t *color, void *userdata)
{
	SinkCtx *ctx = (SinkCtx *)userdata;
	(void)color;
	ctx->bm->stamp(x, y, w, h, ctx->tag);
}

static int parse_map(
	const char *d,
	float flatness,
	float destSize,
	float pad,
	uir_path_t *mapped
)
{
	uir_path_t local;
	uir_parse_result_t pr = UIR_SvgParsePathD(d, flatness, &local);
	if (pr.status != UIR_OK) {
		return 0;
	}
	uir_viewbox_t vb = {0.0f, 0.0f, 24.0f, 24.0f};
	uir_rect_t dest = {pad, pad, destSize, destSize};
	uir_status_t st = UIR_SvgMapPathToRect(&local, &vb, &dest, UIR_FIT_STRETCH, mapped);
	UIR_PathFree(&local);
	return st == UIR_OK;
}

struct Score {
	int starGap;
	int ringFlood;
	int ringSeam;
	int fail;
};

static Score score_bitmap(const Bitmap &bm, float cx, float cy, float s, float strokeW)
{
	Score sc{};
	float rOuter = 12.0f * s;
	float rInner = 10.72342f * s;
	float rMid = 0.5f * (rOuter + rInner);

	/* Star tip angles (point-up star in y-down: tip near -90° / 270°). */
	const float tipDeg[5] = {270.0f, 342.0f, 54.0f, 126.0f, 198.0f};
	for (int t = 0; t < 5; t++) {
		float ang = tipDeg[t] * (float)M_PI / 180.0f;
		/* Sample just outside expected fill tip along ray — should be stroke, not empty. */
		for (float r = 9.5f * s; r <= 12.2f * s; r += 0.35f * s) {
			int x = (int)std::floor(cx + std::cos(ang) * r);
			int y = (int)std::floor(cy + std::sin(ang) * r);
			unsigned char c = bm.at(x, y);
			unsigned char in = bm.at(
				(int)std::floor(cx + std::cos(ang) * (r - 0.6f * s)),
				(int)std::floor(cy + std::sin(ang) * (r - 0.6f * s))
			);
			unsigned char out = bm.at(
				(int)std::floor(cx + std::cos(ang) * (r + 0.6f * s)),
				(int)std::floor(cy + std::sin(ang) * (r + 0.6f * s))
			);
			/* Gap: fill on one side, stroke/empty mismatch with empty between. */
			if (c == CH_EMPTY && in == CH_FILL && (out == CH_STROKE || out == CH_FILL)) {
				sc.starGap++;
			}
			if (c == CH_EMPTY && in == CH_FILL && out == CH_EMPTY) {
				/* tip end: fill then empty without stroke collar */
				if (r > 10.0f * s) {
					sc.starGap++;
				}
			}
		}
	}

	/* Mid-ring: white fill should remain (not flooded by overlapping strokes). */
	int midN = 0;
	int midFlood = 0;
	for (int i = 0; i < 72; i++) {
		float ang = (float)i * (float)M_PI / 36.0f;
		int x = (int)std::floor(cx + std::cos(ang) * rMid);
		int y = (int)std::floor(cy + std::sin(ang) * rMid);
		unsigned char c = bm.at(x, y);
		midN++;
		if (c == CH_STROKE) {
			midFlood++;
		}
	}
	sc.ringFlood = midFlood;
	/* Fail flood if majority of mid-ring is stroke. */
	if (midN > 0 && midFlood * 2 > midN) {
		sc.fail = 1;
	}

	/* Outer ridge continuity (stroke present across ± band). */
	for (int i = 0; i < 360; i++) {
		float ang = (float)i * (float)M_PI / 180.0f;
		int hit = 0;
		for (int k = 0; k < 5; k++) {
			float t = (float)k / 4.0f - 0.5f;
			float r = rOuter + t * strokeW * 0.9f;
			int x = (int)std::floor(cx + std::cos(ang) * r);
			int y = (int)std::floor(cy + std::sin(ang) * r);
			if (bm.at(x, y) == CH_STROKE) {
				hit = 1;
				break;
			}
		}
		if (!hit) {
			sc.ringSeam++;
		}
	}
	/* Count only interior seam runs (ignore nothing if fully broken). */
	if (sc.ringSeam > 0 && sc.ringSeam < 360) {
		sc.fail = 1;
	} else {
		sc.ringSeam = 0; /* all-or-nothing covered: treat full miss separately via flood */
	}
	if (sc.starGap > 0) {
		sc.fail = 1;
	}
	return sc;
}

enum CandId {
	CAND_OUTSIDE = 0, /* current default */
	CAND_CENTERLINE,
	CAND_LOBES_OUTSIDE,
	CAND_COUNT
};

static const char *cand_name(int id)
{
	static const char *n[] = {"outside_annulus", "centerline_annulus", "lobes_outside"};
	return n[id];
}

static int run_cell(int cand, int logoPx, float strokeW, float uiScale, int csv)
{
	uir_stroke_opts_t opts;
	UIR_StrokeOptsInit(&opts);
	const char *ringD = kRingAnnulusD;
	if (cand == CAND_CENTERLINE) {
		opts.alignOutside = 0;
	} else if (cand == CAND_LOBES_OUTSIDE) {
		ringD = kRingLobesD;
		opts.alignOutside = 1;
	}

	float destSize = (float)logoPx * uiScale;
	float flatness = 0.25f * 24.0f / destSize;
	if (flatness < 0.01f) {
		flatness = 0.01f;
	}
	if (flatness > 0.25f) {
		flatness = 0.25f;
	}
	float pad = strokeW * uiScale + 4.0f;
	int bmp = (int)std::ceil(destSize + pad * 2.0f);

	uir_path_t starMapped;
	uir_path_t ringMapped;
	uir_path_t starStroke;
	uir_path_t ringStroke;
	if (!parse_map(kStarD, flatness, destSize, pad, &starMapped)) {
		return -1;
	}
	if (!parse_map(ringD, flatness, destSize, pad, &ringMapped)) {
		UIR_PathFree(&starMapped);
		return -1;
	}

	float widthDraw = strokeW * uiScale;
	if (UIR_BuildStrokePathOpts(&starMapped, widthDraw, &opts, &starStroke) != UIR_OK ||
	    UIR_BuildStrokePathOpts(&ringMapped, widthDraw, &opts, &ringStroke) != UIR_OK) {
		UIR_PathFree(&starMapped);
		UIR_PathFree(&ringMapped);
		return -1;
	}

	Bitmap bm;
	bm.clear(bmp, bmp);
	uir_viewport_t vp;
	UIR_ViewportMake(0, 0, bmp, bmp, &vp);
	uir_color_t white = {1, 1, 1, 1};
	uir_color_t green = {0, 1, 0, 1};
	SinkCtx ctx{&bm, CH_FILL};
	uir_draw_sink_t sink;
	sink.drawBox = draw_box_sink;
	sink.userdata = &ctx;
	sink.stats = nullptr;

	/* Paint order matches client: stroke then fill so interiors stay visible. */
	ctx.tag = CH_STROKE;
	UIR_PathFill(&vp, &starStroke, &green, &sink, 0);
	UIR_PathFill(&vp, &ringStroke, &green, &sink, 0);
	ctx.tag = CH_FILL;
	UIR_PathFill(&vp, &starMapped, &white, &sink, 0);
	UIR_PathFill(&vp, &ringMapped, &white, &sink, 0);

	float cx = pad + destSize * 0.5f;
	float cy = pad + destSize * 0.5f;
	float s = destSize / 24.0f;
	Score sc = score_bitmap(bm, cx, cy, s, widthDraw);

	if (csv) {
		std::printf(
			"%s,%d,%.3f,%.3f,%d,%d,%d,%s\n",
			cand_name(cand),
			logoPx,
			strokeW,
			uiScale,
			sc.starGap,
			sc.ringFlood,
			sc.ringSeam,
			sc.fail ? "FAIL" : "PASS"
		);
	}

	UIR_PathFree(&starStroke);
	UIR_PathFree(&ringStroke);
	UIR_PathFree(&starMapped);
	UIR_PathFree(&ringMapped);
	return sc.fail ? 1 : 0;
}

int main(int argc, char **argv)
{
	int quick = 0;
	int csv = 1;
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--quick") == 0) {
			quick = 1;
		} else if (std::strcmp(argv[i], "--no-csv") == 0) {
			csv = 0;
		}
	}

	/* Logo box px ≈ header logo; ui_scale mimics cvar; extra scales cover windowed/fullscreen. */
	std::vector<int> sizes = quick ? std::vector<int>{36, 64, 96} : std::vector<int>{36, 52, 64, 96, 128, 192};
	std::vector<float> widths = quick ? std::vector<float>{2.0f} : std::vector<float>{1.0f, 2.0f, 3.0f};
	std::vector<float> scales =
		quick ? std::vector<float>{1.0f, 1.5f} : std::vector<float>{1.0f, 1.25f, 1.5f, 2.0f};

	if (csv) {
		std::printf("candidate,logo_px,stroke_w,ui_scale,star_gap,ring_flood,ring_seam,result\n");
	}

	int winner = -1;
	for (int cand = 0; cand < CAND_COUNT; cand++) {
		int failed = 0;
		int cells = 0;
		for (int sz : sizes) {
			for (float w : widths) {
				for (float sc : scales) {
					int r = run_cell(cand, sz, w, sc, csv);
					cells++;
					if (r != 0) {
						failed = 1;
					}
				}
			}
		}
		std::fprintf(
			stderr,
			"# candidate %s cells=%d %s\n",
			cand_name(cand),
			cells,
			failed ? "FAIL" : "PASS"
		);
		if (!failed && winner < 0) {
			winner = cand;
			break;
		}
	}

	if (winner >= 0) {
		std::fprintf(stderr, "# WINNER %s\n", cand_name(winner));
		return 0;
	}
	std::fprintf(stderr, "# NO PASSING CANDIDATE\n");
	return 1;
}
