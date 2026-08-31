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

#include "uid_action.h"
#include "uid_binding.h"
#include "uid_collection.h"
#include "uid_compile.h"
#include "uid_diag.h"
#include "uid_document.h"
#include "uid_expr.h"
#include "uid_expr_bool.h"
#include "uid_input.h"
#include "uid_invoke.h"
#include "uid_layout.h"
#include "uid_modal.h"
#include "uid_menu_map_view.h"
#include "uid_runtime.h"
#include "uid_template.h"
#include "uid_value.h"
#include "uid_widget.h"
#include "uid_xml.h"
#include "cl_killfeed.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Stub for q_shared Q_strncpyz used by kill-feed classify in this test binary. */
extern "C" void Com_Error(int code, const char *fmt, ...)
{
	va_list args;
	(void)code;
	va_start(args, fmt);
	std::vfprintf(stderr, fmt, args);
	va_end(args);
	std::fputc('\n', stderr);
	std::abort();
}

#include "../uirender/uir_menu_map_view.h"
#include "../uirender/uir_viewport.h"
#include "../uirender/uir_compositor.h"
#include "../uirender/uir_draw2d.h"
#include "../uirender/uir_image.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifndef UID_TEST_FIXTURE_DIR
#define UID_TEST_FIXTURE_DIR "assets/main/ui/modern"
#endif

static std::map<std::string, std::string> g_testImportFiles;

static std::string ResolveTestImportDiskPath(const char *path)
{
	if (!path || !path[0]) {
		return std::string();
	}
	const char *prefix = "ui/modern/";
	if (std::strncmp(path, prefix, std::strlen(prefix)) == 0) {
		return std::string(UID_TEST_FIXTURE_DIR) + "/" + (path + std::strlen(prefix));
	}
	return std::string("assets/main/") + path;
}

static long test_read_import_file(const char *path, void **buf)
{
	if (!path || !buf) {
		return -1;
	}
	auto mapped = g_testImportFiles.find(path);
	if (mapped != g_testImportFiles.end()) {
		const std::string &data = mapped->second;
		void *copy = std::malloc(data.size());
		if (!copy) {
			return -1;
		}
		std::memcpy(copy, data.data(), data.size());
		*buf = copy;
		return static_cast<long>(data.size());
	}

	const std::string diskPath = ResolveTestImportDiskPath(path);
	FILE *f = std::fopen(diskPath.c_str(), "rb");
	if (!f) {
		return -1;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	if (sz < 0) {
		std::fclose(f);
		return -1;
	}
	std::fseek(f, 0, SEEK_SET);
	void *data = std::malloc(static_cast<size_t>(sz));
	if (!data) {
		std::fclose(f);
		return -1;
	}
	if (std::fread(data, 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
		std::free(data);
		std::fclose(f);
		return -1;
	}
	std::fclose(f);
	*buf = data;
	return sz;
}

static void test_free_import_file(void *buf)
{
	std::free(buf);
}

static uid_parse_io_t MakeTestParseIo(void)
{
	uid_parse_io_t io;
	io.readFile = test_read_import_file;
	io.freeFile = test_free_import_file;
	return io;
}

static int g_failures = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
			g_failures++;                                                                                              \
		}                                                                                                              \
	} while (0)

#define CHECK_EQ_F(a, b, eps)                                                                                          \
	do {                                                                                                               \
		double _a = (double)(a);                                                                                       \
		double _b = (double)(b);                                                                                       \
		if (std::fabs(_a - _b) > (eps)) {                                                                              \
			std::fprintf(stderr, "FAIL %s:%d: %g != %g (eps %g)\n", __FILE__, __LINE__, _a, _b, (double)(eps));         \
			g_failures++;                                                                                              \
		}                                                                                                              \
	} while (0)

namespace {

struct FakeCvar {
	std::string value;
	int         flags;
};

struct FakeBackendState {
	std::map<std::string, FakeCvar> cvars;
	std::map<int, std::string>      bindings;
	std::vector<std::string>        drawLog;
	std::vector<std::string>        fontDrawLog;
	std::vector<std::string>        invokes;
	std::vector<std::string>        imageDrawLog;
	std::vector<std::string>        hostRegionLog;
	std::vector<std::string>        clipRects;
	/* Added in OPM: vfs path → texel size for leaf <image> layout tests. */
	std::map<std::string, std::pair<float, float>> imageSizes;
	int                             clipDepth = 0;
	int                             shapeClipBegins = 0;
	int                             shapeClipEnds = 0;
	int                             shapeClipDepth = 0;
	std::vector<std::string>        shapeClipPaths;
	int                             imageMaskBegins = 0;
	int                             imageMaskEnds = 0;
	int                             imageMaskDepth = 0;
	std::vector<std::string>        imageMaskLog;
	float                           lastPathRotation = 0.0f;
	/* Added in OPM: mutable host feed for foreach lifetime tests. */
	struct LifetimeItem {
		std::string key;
		std::string text;
	};
	std::vector<LifetimeItem> lifetimeItems;
	uint64_t                 lifetimeRevision = 1;
	bool                     lifetimeFeedActive = false;
	/* Added in OPM: per-byte advance for caret/layout measure tests (default 8). */
	float                    fontMeasurePx = 8.0f;
};

FakeBackendState *g_fake = nullptr;

void *fake_alloc(size_t n)
{
	return std::malloc(n);
}
void fake_free(void *p)
{
	std::free(p);
}

bool fake_cvarDescribe(const char *name, int *flags, char *valueBuf, size_t valueBufSize)
{
	if (!g_fake || !name || !valueBuf || valueBufSize == 0) {
		return false;
	}
	auto it = g_fake->cvars.find(name);
	if (it == g_fake->cvars.end()) {
		return false;
	}
	if (flags) {
		*flags = it->second.flags;
	}
	std::snprintf(valueBuf, valueBufSize, "%s", it->second.value.c_str());
	return true;
}

bool fake_cvarWrite(const char *name, const char *value)
{
	if (!g_fake || !name || !value) {
		return false;
	}
	auto it = g_fake->cvars.find(name);
	if (it == g_fake->cvars.end()) {
		/* Added in OPM: allow creating companion cvars (e.g. r_noborder). */
		g_fake->cvars[name] = FakeCvar{value, 0};
		return true;
	}
	if (it->second.flags & UID_CVAR_WRITE_DENIED) {
		return false;
	}
	it->second.value = value;
	return true;
}

bool fake_cvarReset(const char *name)
{
	return fake_cvarWrite(name, "0");
}

bool fake_invoke(const char *name, void *)
{
	if (!g_fake || !name) {
		return false;
	}
	g_fake->invokes.push_back(name);
	return true;
}

void fake_drawRect(float x, float y, float w, float h, const float *rgba)
{
	char buf[128];
	std::snprintf(buf, sizeof(buf), "rect %.1f %.1f %.1f %.1f a=%.2f", x, y, w, h, rgba ? rgba[3] : 0.0f);
	g_fake->drawLog.push_back(buf);
}

void fake_drawHostRegion(const char *role, float x, float y, float w, float h, void *)
{
	char buf[160];
	std::snprintf(buf, sizeof(buf), "host %s %.1f %.1f %.1f %.1f", role ? role : "", x, y, w, h);
	if (g_fake) {
		g_fake->hostRegionLog.push_back(buf);
	}
}

bool fake_getHiResScale(float *scaleX, float *scaleY)
{
	if (!scaleX || !scaleY) {
		return false;
	}
	*scaleX = 2.0f;
	*scaleY = 2.0f;
	return true;
}

bool fake_getFramebufferSize(int *width, int *height)
{
	if (!width || !height) {
		return false;
	}
	*width = 1920;
	*height = 1080;
	return true;
}

void fake_pushClip(float x, float y, float w, float h)
{
	if (g_fake) {
		char buf[128];
		std::snprintf(buf, sizeof(buf), "clip %.1f,%.1f,%.1f,%.1f", x, y, w, h);
		g_fake->clipRects.push_back(buf);
		g_fake->clipDepth++;
	}
}

void fake_popClip(void)
{
	if (g_fake && g_fake->clipDepth > 0) {
		g_fake->clipDepth--;
	}
}

bool fake_beginShapeClip(
	float x,
	float y,
	float w,
	float h,
	const char *const *pathD,
	int pathCount,
	float viewW,
	float viewH,
	float rotationDeg
)
{
	(void)viewW;
	(void)viewH;
	(void)rotationDeg;
	if (!g_fake || !pathD || pathCount <= 0 || g_fake->shapeClipDepth > 0) {
		return false;
	}
	char buf[160];
	std::snprintf(buf, sizeof(buf), "shapeclip %.1f,%.1f,%.1f,%.1f n=%d", x, y, w, h, pathCount);
	g_fake->shapeClipPaths.push_back(buf);
	if (pathD[0] && pathD[0][0]) {
		g_fake->shapeClipPaths.push_back(pathD[0]);
	}
	g_fake->shapeClipBegins++;
	g_fake->shapeClipDepth++;
	return true;
}

void fake_endShapeClip(void)
{
	if (g_fake && g_fake->shapeClipDepth > 0) {
		g_fake->shapeClipEnds++;
		g_fake->shapeClipDepth--;
	}
}

bool fake_beginImageMask(float x, float y, float w, float h, const char *vfsPath, int fit)
{
	if (!g_fake || !vfsPath || !vfsPath[0] || g_fake->imageMaskDepth > 0) {
		return false;
	}
	char buf[192];
	std::snprintf(
		buf,
		sizeof(buf),
		"mask %.1f,%.1f,%.1f,%.1f fit=%d '%s'",
		x,
		y,
		w,
		h,
		fit,
		vfsPath
	);
	g_fake->imageMaskLog.push_back(buf);
	g_fake->imageMaskBegins++;
	g_fake->imageMaskDepth++;
	return true;
}

void fake_endImageMask(void)
{
	if (g_fake && g_fake->imageMaskDepth > 0) {
		g_fake->imageMaskEnds++;
		g_fake->imageMaskDepth--;
		g_fake->imageMaskLog.push_back("mask-end");
	}
}

float fake_fontMeasure(void *, const char *text)
{
	const float px = (g_fake && g_fake->fontMeasurePx > 0.0f) ? g_fake->fontMeasurePx : 8.0f;
	return text ? (float)std::strlen(text) * px : 0.0f;
}

void *fake_fontResolve(const char *, float, float)
{
	static int s_fontToken = 1;
	return &s_fontToken;
}

void fake_fontDraw(void *, float x, float y, const char *text, const float *rgba, float)
{
	if (!g_fake) {
		return;
	}
	char buf[192];
	if (rgba) {
		std::snprintf(
			buf,
			sizeof(buf),
			"text %.1f,%.1f '%s' rgba=%.2f,%.2f,%.2f,%.2f",
			x,
			y,
			text ? text : "",
			rgba[0],
			rgba[1],
			rgba[2],
			rgba[3]
		);
	} else {
		std::snprintf(buf, sizeof(buf), "text %.1f,%.1f '%s'", x, y, text ? text : "");
	}
	g_fake->fontDrawLog.push_back(buf);
}

/* Added in Omaha: log rotated text draws for unit tests. */
void fake_fontDrawRotated(
	void *,
	float x,
	float y,
	const char *text,
	const float *rgba,
	float tracking,
	float rotationDeg,
	float pivotX,
	float pivotY
)
{
	(void)tracking;
	if (!g_fake) {
		return;
	}
	char buf[256];
	if (rgba) {
		std::snprintf(
			buf,
			sizeof(buf),
			"textrot %.1f,%.1f '%s' rot=%.1f pivot=%.1f,%.1f rgba=%.2f,%.2f,%.2f,%.2f",
			x,
			y,
			text ? text : "",
			rotationDeg,
			pivotX,
			pivotY,
			rgba[0],
			rgba[1],
			rgba[2],
			rgba[3]
		);
	} else {
		std::snprintf(
			buf,
			sizeof(buf),
			"textrot %.1f,%.1f '%s' rot=%.1f pivot=%.1f,%.1f",
			x,
			y,
			text ? text : "",
			rotationDeg,
			pivotX,
			pivotY
		);
	}
	g_fake->fontDrawLog.push_back(buf);
}

void fake_drawPath(
	const char *svgD,
	float x,
	float y,
	float w,
	float h,
	float viewW,
	float viewH,
	const float *fillRgba,
	const float *strokeRgba,
	float strokeWidthPx,
	float rotationDeg,
	int crisp
)
{
	if (!g_fake) {
		return;
	}
	g_fake->lastPathRotation = rotationDeg;
	char buf[512];
	std::snprintf(
		buf,
		sizeof(buf),
		"path d='%s' xywh=%.1f,%.1f,%.1f,%.1f view=%.1fx%.1f fill_a=%.2f stroke_a=%.2f stroke_w=%.2f rot=%.1f crisp=%d",
		svgD ? svgD : "",
		x,
		y,
		w,
		h,
		viewW,
		viewH,
		fillRgba ? fillRgba[3] : 0.0f,
		strokeRgba ? strokeRgba[3] : 0.0f,
		strokeWidthPx,
		rotationDeg,
		crisp
	);
	g_fake->drawLog.push_back(buf);
}

void fake_drawImage(
	const char *vfsPath,
	float x,
	float y,
	float w,
	float h,
	const char *const *clipPathD,
	int clipPathCount,
	float viewW,
	float viewH,
	int fit,
	float rotationDeg,
	float backgroundScale,
	const float *tintRgba
)
{
	(void)viewW;
	(void)viewH;
	if (!g_fake) {
		return;
	}
	char buf[512];
	std::snprintf(
		buf,
		sizeof(buf),
		"image '%s' xywh=%.1f,%.1f,%.1f,%.1f fit=%d clips=%d rot=%.1f bg_scale=%.2f tint_a=%.2f first='%s'",
		vfsPath ? vfsPath : "",
		x,
		y,
		w,
		h,
		fit,
		clipPathCount,
		rotationDeg,
		backgroundScale,
		tintRgba ? tintRgba[3] : 1.0f,
		(clipPathCount > 0 && clipPathD && clipPathD[0]) ? clipPathD[0] : ""
	);
	g_fake->imageDrawLog.push_back(buf);
}

/* Added in OPM: fake texel size for leaf <image> intrinsic layout. */
bool fake_imageMeasure(const char *vfsPath, float *outW, float *outH)
{
	if (!g_fake || !vfsPath || !outW || !outH) {
		return false;
	}
	const auto it = g_fake->imageSizes.find(vfsPath);
	if (it == g_fake->imageSizes.end()) {
		return false;
	}
	*outW = it->second.first;
	*outH = it->second.second;
	return true;
}

void fake_drawGradient(
	const char *brush,
	float x,
	float y,
	float w,
	float h,
	const char *const *clipPathD,
	int clipPathCount,
	float viewW,
	float viewH,
	float rotationDeg,
	const float *tintRgba
)
{
	(void)viewW;
	(void)viewH;
	if (!g_fake) {
		return;
	}
	char buf[512];
	std::snprintf(
		buf,
		sizeof(buf),
		"gradient '%s' xywh=%.1f,%.1f,%.1f,%.1f clips=%d rot=%.1f tint_a=%.2f first='%s'",
		brush ? brush : "",
		x,
		y,
		w,
		h,
		clipPathCount,
		rotationDeg,
		tintRgba ? tintRgba[3] : 1.0f,
		(clipPathCount > 0 && clipPathD && clipPathD[0]) ? clipPathD[0] : ""
	);
	g_fake->drawLog.push_back(buf);
}

static int g_edgeClipShader = 1;
static int g_edgeClipScissorX = 0;
static int g_edgeClipScissorY = 0;
static int g_edgeClipScissorW = 0;
static int g_edgeClipScissorH = 0;
static bool g_edgeClipScissorOk = false;

static int edge_clip_register_shader(const char *path)
{
	(void)path;
	return g_edgeClipShader;
}

static void edge_clip_shader_size(int shader, int *width, int *height)
{
	(void)shader;
	if (width) {
		*width = 64;
	}
	if (height) {
		*height = 32;
	}
}

static void edge_clip_set_color(const float *rgba)
{
	(void)rgba;
}

static void edge_clip_draw_stretch(
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
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)s1;
	(void)t1;
	(void)s2;
	(void)t2;
	(void)shader;
}

static void edge_clip_test_scissor(int x, int y, int w, int h)
{
	g_edgeClipScissorX = x;
	g_edgeClipScissorY = y;
	g_edgeClipScissorW = w;
	g_edgeClipScissorH = h;
	if (w == 64 && h == 64 && y == 352) {
		g_edgeClipScissorOk = true;
	}
}

static void fake_drawImageUir(
	const char *vfsPath,
	float x,
	float y,
	float w,
	float h,
	const char *const *clipPathD,
	int clipPathCount,
	float viewW,
	float viewH,
	int fit,
	float rotationDeg,
	float backgroundScale,
	const float *tintRgba
)
{
	fake_drawImage(
		vfsPath,
		x,
		y,
		w,
		h,
		clipPathD,
		clipPathCount,
		viewW,
		viewH,
		fit,
		rotationDeg,
		backgroundScale,
		tintRgba
	);
	(void)UIR_ImageDrawClipped(
		vfsPath,
		x,
		y,
		w,
		h,
		clipPathD,
		clipPathCount,
		viewW,
		viewH,
		static_cast<uir_image_fit_t>(fit),
		rotationDeg,
		1.0f,
		backgroundScale,
		nullptr
	);
}

/* Added in OPM: host option arrays for cyclic source= tests. */
int fake_queryOptions(const char *source, char **values, char **labels, int max)
{
	static char valueBuf[8][64];
	static char labelBuf[8][64];
	static const char *const displayMode[] = {"1", "Fullscreen", "2", "Borderless", "0", "Windowed"};
	int i;
	int n;

	if (!source || !values || !labels || max <= 0) {
		return 0;
	}
	if (std::strcmp(source, "display-mode") != 0) {
		return 0;
	}
	n = 3;
	if (n > max) {
		n = max;
	}
	for (i = 0; i < n; i++) {
		std::snprintf(valueBuf[i], sizeof(valueBuf[i]), "%s", displayMode[i * 2]);
		std::snprintf(labelBuf[i], sizeof(labelBuf[i]), "%s", displayMode[i * 2 + 1]);
		values[i] = valueBuf[i];
		labels[i] = labelBuf[i];
	}
	return n;
}

uint64_t g_fakeServersRevision = 1;
uint64_t g_fakeScoreboardRevision = 1;

int fake_queryCollectionItems(const uid_collection_query_t *query, uid_collection_item_t *out, int max)
{
	static char keyBuf[8][16];
	static char valueBuf[8][32];
	static char labelBuf[8][32];
	static char favBuf[8][4];
	static char nameBuf[8][32];
	static char mapBuf[8][32];
	static char playersBuf[8][16];
	static char gametypeBuf[8][16];
	static char pingBuf[8][8];
	static const char *fieldNames[7];
	static const char *fieldValues[7];
	if (!query || !query->source || !out || max <= 0) {
		return 0;
	}
	if (std::strcmp(query->source, "servers") == 0) {
		const int total = 2;
		if (query->outTotal) {
			*query->outTotal = total;
		}
		if (query->outRevision) {
			*query->outRevision = g_fakeServersRevision;
		}
		int written = 0;
		for (int i = 0; i < total && written < max; ++i) {
			std::snprintf(keyBuf[written], sizeof(keyBuf[written]), "%d", i);
			std::snprintf(valueBuf[written], sizeof(valueBuf[written]), "127.0.0.1:%d", 12203 + i);
			std::snprintf(labelBuf[written], sizeof(labelBuf[written]), "Server %d", i + 1);
			std::snprintf(favBuf[written], sizeof(favBuf[written]), i == 0 ? "*" : "o");
			std::snprintf(nameBuf[written], sizeof(nameBuf[written]), "Server %d", i + 1);
			std::snprintf(mapBuf[written], sizeof(mapBuf[written]), "mohdm3");
			std::snprintf(playersBuf[written], sizeof(playersBuf[written]), "4/16");
			std::snprintf(gametypeBuf[written], sizeof(gametypeBuf[written]), "DM");
			std::snprintf(pingBuf[written], sizeof(pingBuf[written]), "%d", 32 + i);
			static char favFillBuf[8][16];
			std::snprintf(
				favFillBuf[written],
				sizeof(favFillBuf[written]),
				i == 0 ? "#F4C430EB" : "#EBF0F559"
			);
			fieldNames[0] = "favorite";
			fieldNames[1] = "favorite_fill";
			fieldNames[2] = "name";
			fieldNames[3] = "map";
			fieldNames[4] = "players";
			fieldNames[5] = "gametype";
			fieldNames[6] = "ping";
			fieldValues[0] = favBuf[written];
			fieldValues[1] = favFillBuf[written];
			fieldValues[2] = nameBuf[written];
			fieldValues[3] = mapBuf[written];
			fieldValues[4] = playersBuf[written];
			fieldValues[5] = gametypeBuf[written];
			fieldValues[6] = pingBuf[written];
			out[written].key = keyBuf[written];
			out[written].value = valueBuf[written];
			out[written].label = labelBuf[written];
			out[written].nfields = 7;
			out[written].fieldNames = fieldNames;
			out[written].fieldValues = fieldValues;
			out[written].flags = 0;
			written++;
		}
		return written;
	}
	if (std::strcmp(query->source, "scoreboard") == 0) {
		static char kindBuf[4][16];
		static char slotBuf[4][8];
		static char nameBuf[4][64];
		static char killsBuf[4][16];
		static char deathsBuf[4][16];
		static char kdBuf[4][16];
		static char timeBuf[4][32];
		static char pingBuf[4][16];
		static char textColorBuf[4][16];
		static char rowFillBuf[4][16];
		static char isHeaderBuf[4][4];
		static char isSpectatorBuf[4][4];
		static const char *fieldNames[12] = {
			"kind", "slot", "name", "kills", "deaths", "kd", "time", "ping", "text_color", "row_fill", "is_header",
			"is_spectator",
		};
		static const char *fieldValues[4][12];
		const int total = 3;
		if (query->outTotal) {
			*query->outTotal = total;
		}
		if (query->outRevision) {
			*query->outRevision = g_fakeScoreboardRevision;
		}
		int written = 0;
		for (int i = 0; i < total && written < max; ++i) {
			if (i == 0) {
				std::snprintf(kindBuf[written], sizeof(kindBuf[written]), "header");
				std::snprintf(slotBuf[written], sizeof(slotBuf[written]), "");
				std::snprintf(nameBuf[written], sizeof(nameBuf[written]), "ALLIED");
				std::snprintf(killsBuf[written], sizeof(killsBuf[written]), "");
				std::snprintf(deathsBuf[written], sizeof(deathsBuf[written]), "");
				std::snprintf(kdBuf[written], sizeof(kdBuf[written]), "");
				std::snprintf(timeBuf[written], sizeof(timeBuf[written]), "");
				std::snprintf(pingBuf[written], sizeof(pingBuf[written]), "");
				std::snprintf(textColorBuf[written], sizeof(textColorBuf[written]), "#88CC88FF");
				std::snprintf(rowFillBuf[written], sizeof(rowFillBuf[written]), "#00000059");
				std::snprintf(isHeaderBuf[written], sizeof(isHeaderBuf[written]), "1");
				std::snprintf(isSpectatorBuf[written], sizeof(isSpectatorBuf[written]), "0");
			} else if (i == 1) {
				std::snprintf(kindBuf[written], sizeof(kindBuf[written]), "player");
				std::snprintf(slotBuf[written], sizeof(slotBuf[written]), "1");
				std::snprintf(nameBuf[written], sizeof(nameBuf[written]), "PlayerOne");
				std::snprintf(killsBuf[written], sizeof(killsBuf[written]), "10");
				std::snprintf(deathsBuf[written], sizeof(deathsBuf[written]), "4");
				std::snprintf(kdBuf[written], sizeof(kdBuf[written]), "2.50");
				std::snprintf(timeBuf[written], sizeof(timeBuf[written]), "12:34");
				std::snprintf(pingBuf[written], sizeof(pingBuf[written]), "32");
				std::snprintf(textColorBuf[written], sizeof(textColorBuf[written]), "#FFFFFFFF");
				std::snprintf(rowFillBuf[written], sizeof(rowFillBuf[written]), "#00000000");
				std::snprintf(isHeaderBuf[written], sizeof(isHeaderBuf[written]), "0");
				std::snprintf(isSpectatorBuf[written], sizeof(isSpectatorBuf[written]), "0");
			} else {
				std::snprintf(kindBuf[written], sizeof(kindBuf[written]), "spacer");
				std::snprintf(slotBuf[written], sizeof(slotBuf[written]), "");
				std::snprintf(nameBuf[written], sizeof(nameBuf[written]), "");
				std::snprintf(killsBuf[written], sizeof(killsBuf[written]), "");
				std::snprintf(deathsBuf[written], sizeof(deathsBuf[written]), "");
				std::snprintf(kdBuf[written], sizeof(kdBuf[written]), "");
				std::snprintf(timeBuf[written], sizeof(timeBuf[written]), "");
				std::snprintf(pingBuf[written], sizeof(pingBuf[written]), "");
				std::snprintf(textColorBuf[written], sizeof(textColorBuf[written]), "");
				std::snprintf(rowFillBuf[written], sizeof(rowFillBuf[written]), "");
				std::snprintf(isHeaderBuf[written], sizeof(isHeaderBuf[written]), "0");
				std::snprintf(isSpectatorBuf[written], sizeof(isSpectatorBuf[written]), "0");
			}
			fieldValues[written][0] = kindBuf[written];
			fieldValues[written][1] = slotBuf[written];
			fieldValues[written][2] = nameBuf[written];
			fieldValues[written][3] = killsBuf[written];
			fieldValues[written][4] = deathsBuf[written];
			fieldValues[written][5] = kdBuf[written];
			fieldValues[written][6] = timeBuf[written];
			fieldValues[written][7] = pingBuf[written];
			fieldValues[written][8] = textColorBuf[written];
			fieldValues[written][9] = rowFillBuf[written];
			fieldValues[written][10] = isHeaderBuf[written];
			fieldValues[written][11] = isSpectatorBuf[written];
			std::snprintf(keyBuf[written], sizeof(keyBuf[written]), "%d", i);
			out[written].key = keyBuf[written];
			out[written].value = nameBuf[written];
			out[written].label = nameBuf[written];
			out[written].nfields = 12;
			out[written].fieldNames = fieldNames;
			out[written].fieldValues = fieldValues[written];
			out[written].flags = 0;
			written++;
		}
		return written;
	}
	/* Added in OPM: tiny host feed for join() label aggregate tests. */
	if (std::strcmp(query->source, "join-demo") == 0) {
		static char nameBuf[4][32];
		static char specBuf[4][4];
		static const char *fieldNames[2] = {"name", "is_spectator"};
		static const char *fieldValues[4][2];
		static char keyBufLocal[4][8];
		const int total = 3;
		if (query->outTotal) {
			*query->outTotal = total;
		}
		if (query->outRevision) {
			*query->outRevision = 1;
		}
		const char *names[] = {"Alice", "Bob", "Carol"};
		const char *specs[] = {"1", "0", "1"};
		int written = 0;
		for (int i = 0; i < total && written < max; ++i) {
			std::snprintf(keyBufLocal[written], sizeof(keyBufLocal[written]), "%d", i);
			std::snprintf(nameBuf[written], sizeof(nameBuf[written]), "%s", names[i]);
			std::snprintf(specBuf[written], sizeof(specBuf[written]), "%s", specs[i]);
			fieldValues[written][0] = nameBuf[written];
			fieldValues[written][1] = specBuf[written];
			out[written].key = keyBufLocal[written];
			out[written].value = keyBufLocal[written];
			out[written].label = nameBuf[written];
			out[written].nfields = 2;
			out[written].fieldNames = fieldNames;
			out[written].fieldValues = fieldValues[written];
			out[written].flags = 0;
			written++;
		}
		return written;
	}
	if (std::strcmp(query->source, "hud-objectives") == 0) {
		static char textBuf[2][64];
		static char hiddenBuf[2][8];
		static char completedBuf[2][8];
		static char currentBuf[2][8];
		static char highlightBuf[2][8];
		static const char *objFieldNames[5];
		static const char *objFieldValues[5];
		const int total = 1;
		if (query->outTotal) {
			*query->outTotal = total;
		}
		if (query->outRevision) {
			*query->outRevision = 1;
		}
		if (max <= 0) {
			return 0;
		}
		std::snprintf(textBuf[0], sizeof(textBuf[0]), "Secure the bridge");
		std::snprintf(hiddenBuf[0], sizeof(hiddenBuf[0]), "0");
		std::snprintf(completedBuf[0], sizeof(completedBuf[0]), "0");
		std::snprintf(currentBuf[0], sizeof(currentBuf[0]), "1");
		std::snprintf(highlightBuf[0], sizeof(highlightBuf[0]), "1");
		objFieldNames[0] = "text";
		objFieldNames[1] = "hidden";
		objFieldNames[2] = "completed";
		objFieldNames[3] = "current";
		objFieldNames[4] = "highlight";
		objFieldValues[0] = textBuf[0];
		objFieldValues[1] = hiddenBuf[0];
		objFieldValues[2] = completedBuf[0];
		objFieldValues[3] = currentBuf[0];
		objFieldValues[4] = highlightBuf[0];
		std::snprintf(keyBuf[0], sizeof(keyBuf[0]), "0");
		out[0].key = keyBuf[0];
		out[0].value = keyBuf[0];
		out[0].label = textBuf[0];
		out[0].nfields = 5;
		out[0].fieldNames = objFieldNames;
		out[0].fieldValues = objFieldValues;
		out[0].flags = 0;
		return 1;
	}
	if (std::strcmp(query->source, "hud-messages") == 0) {
		static char msgTextBuf[5][64];
		static char msgColorBuf[5][16];
		static char msgAlphaBuf[5][8];
		static char msgBoldBuf[5][4];
		static const char *msgFieldNames[4] = {"text", "color", "alpha", "bold"};
		static const char *msgFieldValues[5][4];
		if (g_fake && g_fake->lifetimeFeedActive) {
			const int total = static_cast<int>(g_fake->lifetimeItems.size());
			if (query->outTotal) {
				*query->outTotal = total;
			}
			if (query->outRevision) {
				*query->outRevision = g_fake->lifetimeRevision;
			}
			int written = 0;
			for (int i = 0; i < total && written < max; ++i) {
				const auto &item = g_fake->lifetimeItems[static_cast<size_t>(i)];
				std::snprintf(msgTextBuf[written], sizeof(msgTextBuf[written]), "%s", item.text.c_str());
				std::snprintf(msgColorBuf[written], sizeof(msgColorBuf[written]), "#FF8080FF");
				std::snprintf(msgAlphaBuf[written], sizeof(msgAlphaBuf[written]), "1.00");
				std::snprintf(msgBoldBuf[written], sizeof(msgBoldBuf[written]), "0");
				msgFieldValues[written][0] = msgTextBuf[written];
				msgFieldValues[written][1] = msgColorBuf[written];
				msgFieldValues[written][2] = msgAlphaBuf[written];
				msgFieldValues[written][3] = msgBoldBuf[written];
				std::snprintf(keyBuf[written], sizeof(keyBuf[written]), "%s", item.key.c_str());
				out[written].key = keyBuf[written];
				out[written].value = msgTextBuf[written];
				out[written].label = msgTextBuf[written];
				out[written].nfields = 4;
				out[written].fieldNames = msgFieldNames;
				out[written].fieldValues = msgFieldValues[written];
				out[written].flags = 0;
				written++;
			}
			return written;
		}
		const int total = 5;
		if (query->outTotal) {
			*query->outTotal = total;
		}
		if (query->outRevision) {
			*query->outRevision = 42;
		}
		int written = 0;
		for (int i = 0; i < total && written < max; ++i) {
			std::snprintf(msgTextBuf[written], sizeof(msgTextBuf[written]), "kill line %d", i + 1);
			std::snprintf(msgColorBuf[written], sizeof(msgColorBuf[written]), "#FF8080FF");
			std::snprintf(msgAlphaBuf[written], sizeof(msgAlphaBuf[written]), "%.2f", 1.0f - i * 0.1f);
			std::snprintf(msgBoldBuf[written], sizeof(msgBoldBuf[written]), i == 0 ? "1" : "0");
			msgFieldValues[written][0] = msgTextBuf[written];
			msgFieldValues[written][1] = msgColorBuf[written];
			msgFieldValues[written][2] = msgAlphaBuf[written];
			msgFieldValues[written][3] = msgBoldBuf[written];
			std::snprintf(keyBuf[written], sizeof(keyBuf[written]), "%d", i);
			out[written].key = keyBuf[written];
			out[written].value = msgTextBuf[written];
			out[written].label = msgTextBuf[written];
			out[written].nfields = 4;
			out[written].fieldNames = msgFieldNames;
			out[written].fieldValues = msgFieldValues[written];
			out[written].flags = 0;
			written++;
		}
		return written;
	}
	if (std::strcmp(query->source, "hud-game-messages") == 0) {
		static char gmTextBuf[4][64];
		static char gmColorBuf[4][16];
		static char gmAlphaBuf[4][8];
		static char gmBoldBuf[4][4];
		static const char *gmFieldNames[4] = {"text", "color", "alpha", "bold"};
		static const char *gmFieldValues[4][4];
		const int total = 4;
		if (query->outTotal) {
			*query->outTotal = total;
		}
		if (query->outRevision) {
			*query->outRevision = 43;
		}
		int written = 0;
		for (int i = 0; i < total && written < max; ++i) {
			std::snprintf(gmTextBuf[written], sizeof(gmTextBuf[written]), "game line %d", i + 1);
			std::snprintf(gmColorBuf[written], sizeof(gmColorBuf[written]), "#80FF80FF");
			std::snprintf(gmAlphaBuf[written], sizeof(gmAlphaBuf[written]), "0.80");
			std::snprintf(gmBoldBuf[written], sizeof(gmBoldBuf[written]), "0");
			gmFieldValues[written][0] = gmTextBuf[written];
			gmFieldValues[written][1] = gmColorBuf[written];
			gmFieldValues[written][2] = gmAlphaBuf[written];
			gmFieldValues[written][3] = gmBoldBuf[written];
			std::snprintf(keyBuf[written], sizeof(keyBuf[written]), "%d", i);
			out[written].key = keyBuf[written];
			out[written].value = gmTextBuf[written];
			out[written].label = gmTextBuf[written];
			out[written].nfields = 4;
			out[written].fieldNames = gmFieldNames;
			out[written].fieldValues = gmFieldValues[written];
			out[written].flags = 0;
			written++;
		}
		return written;
	}
	/* Added in OPM: hud-chat and hud-kill-feed fake providers. */
	if (std::strcmp(query->source, "hud-chat") == 0) {
		static char chatTextBuf[5][64];
		static char chatColorBuf[5][16];
		static char chatAlphaBuf[5][8];
		static char chatBoldBuf[5][4];
		static const char *chatFieldNames[4] = {"text", "color", "alpha", "bold"};
		static const char *chatFieldValues[5][4];
		const int total = 3;
		if (query->outTotal) {
			*query->outTotal = total;
		}
		if (query->outRevision) {
			*query->outRevision = 44;
		}
		int written = 0;
		for (int i = 0; i < total && written < max; ++i) {
			std::snprintf(chatTextBuf[written], sizeof(chatTextBuf[written]), "chat line %d", i + 1);
			std::snprintf(chatColorBuf[written], sizeof(chatColorBuf[written]), "#FFFFFFFF");
			std::snprintf(chatAlphaBuf[written], sizeof(chatAlphaBuf[written]), "1.00");
			std::snprintf(chatBoldBuf[written], sizeof(chatBoldBuf[written]), "1");
			chatFieldValues[written][0] = chatTextBuf[written];
			chatFieldValues[written][1] = chatColorBuf[written];
			chatFieldValues[written][2] = chatAlphaBuf[written];
			chatFieldValues[written][3] = chatBoldBuf[written];
			std::snprintf(keyBuf[written], sizeof(keyBuf[written]), "chat_%d", i);
			out[written].key = keyBuf[written];
			out[written].value = chatTextBuf[written];
			out[written].label = chatTextBuf[written];
			out[written].nfields = 4;
			out[written].fieldNames = chatFieldNames;
			out[written].fieldValues = chatFieldValues[written];
			out[written].flags = 0;
			written++;
		}
		return written;
	}
	if (std::strcmp(query->source, "hud-kill-feed") == 0) {
		static char killKiller[6][32];
		static char killVictim[6][32];
		static char killWeapon[6][16];
		static char killKillerTeam[6][16];
		static char killVictimTeam[6][16];
		static char killIconTeam[6][16];
		static char killKind[6][16];
		static char killText[6][64];
		static char killColor[6][16];
		static char killHs[6][4];
		static char killFr[6][4];
		static const char *killFieldNames[11] = {
			"killer",
			"victim",
			"weapon_class",
			"killer_team",
			"victim_team",
			"icon_team",
			"headshot",
			"kill_kind",
			"friendly",
			"text",
			"color"
		};
		static const char *killFieldValues[6][11];
		const int total = 2;
		if (query->outTotal) {
			*query->outTotal = total;
		}
		if (query->outRevision) {
			*query->outRevision = 45;
		}
		int written = 0;
		for (int i = 0; i < total && written < max; ++i) {
			std::snprintf(killKiller[written], sizeof(killKiller[written]), "Killer%d", i + 1);
			std::snprintf(killVictim[written], sizeof(killVictim[written]), "Victim%d", i + 1);
			std::snprintf(killWeapon[written], sizeof(killWeapon[written]), i == 0 ? "sniper" : "smg");
			std::snprintf(killKillerTeam[written], sizeof(killKillerTeam[written]), i == 0 ? "allies" : "axis");
			std::snprintf(killVictimTeam[written], sizeof(killVictimTeam[written]), i == 0 ? "axis" : "allies");
			std::snprintf(killIconTeam[written], sizeof(killIconTeam[written]), killKillerTeam[written]);
			std::snprintf(killKind[written], sizeof(killKind[written]), "player");
			std::snprintf(
				killText[written],
				sizeof(killText[written]),
				"%s %s %s",
				killKiller[written],
				killWeapon[written],
				killVictim[written]
			);
			std::snprintf(killColor[written], sizeof(killColor[written]), "#FF8080FF");
			std::snprintf(killHs[written], sizeof(killHs[written]), i == 0 ? "1" : "0");
			std::snprintf(killFr[written], sizeof(killFr[written]), "0");
			killFieldValues[written][0] = killKiller[written];
			killFieldValues[written][1] = killVictim[written];
			killFieldValues[written][2] = killWeapon[written];
			killFieldValues[written][3] = killKillerTeam[written];
			killFieldValues[written][4] = killVictimTeam[written];
			killFieldValues[written][5] = killIconTeam[written];
			killFieldValues[written][6] = killHs[written];
			killFieldValues[written][7] = killKind[written];
			killFieldValues[written][8] = killFr[written];
			killFieldValues[written][9] = killText[written];
			killFieldValues[written][10] = killColor[written];
			std::snprintf(keyBuf[written], sizeof(keyBuf[written]), "kill_%d", i);
			out[written].key = keyBuf[written];
			out[written].value = keyBuf[written];
			out[written].label = killText[written];
			out[written].nfields = 11;
			out[written].fieldNames = killFieldNames;
			out[written].fieldValues = killFieldValues[written];
			out[written].flags = 0;
			written++;
		}
		return written;
	}
	if (std::strcmp(query->source, "display-mode") != 0) {
		return 0;
	}
	static const char *const values[] = {"1", "2", "0"};
	static const char *const labels[] = {"Fullscreen", "Borderless", "Windowed"};
	const int total = 3;
	if (query->outTotal) {
		*query->outTotal = total;
	}
	if (query->outRevision) {
		*query->outRevision = 1;
	}
	int offset = query->offset > 0 ? query->offset : 0;
	int written = 0;
	for (int i = offset; i < total && written < max; i++) {
		std::snprintf(keyBuf[written], sizeof(keyBuf[written]), "%d", i);
		out[written].key = keyBuf[written];
		out[written].value = values[i];
		out[written].label = labels[i];
		out[written].nfields = 0;
		out[written].fieldNames = nullptr;
		out[written].fieldValues = nullptr;
		out[written].flags = 0;
		written++;
	}
	return written;
}

bool fake_keyNameToNum(const char *name, int *key)
{
	if (!name || !key || !name[0]) {
		return false;
	}
	/* Match Key_StringToKeynum: single printable char is that keynum. */
	if (!name[1]) {
		*key = static_cast<unsigned char>(name[0]);
		return true;
	}
	char *end = nullptr;
	const long n = std::strtol(name, &end, 10);
	if (!end || end == name || *end != '\0') {
		return false;
	}
	*key = static_cast<int>(n);
	return true;
}

bool fake_keyNumToName(int key, char *out, size_t outSize)
{
	if (!out || outSize == 0) {
		return false;
	}
	/* Match Key_KeynumToString for printable ASCII letters. */
	if (key > 32 && key < 127 && key != '"' && key != ';') {
		if (outSize < 2) {
			return false;
		}
		out[0] = static_cast<char>(key);
		out[1] = '\0';
		return true;
	}
	std::snprintf(out, outSize, "%d", key);
	return true;
}

bool fake_getBinding(int key, char *out, size_t outSize)
{
	if (!g_fake || !out || outSize == 0) {
		return false;
	}
	auto it = g_fake->bindings.find(key);
	if (it == g_fake->bindings.end()) {
		out[0] = '\0';
		return true;
	}
	std::snprintf(out, outSize, "%s", it->second.c_str());
	return true;
}

bool fake_setBinding(int key, const char *binding)
{
	if (!g_fake) {
		return false;
	}
	if (!binding || !binding[0]) {
		g_fake->bindings.erase(key);
		return true;
	}
	g_fake->bindings[key] = binding;
	return true;
}

int fake_findConflicts(const char *binding, int *keysOut, int maxKeys)
{
	int n = 0;
	if (!g_fake || !binding || !keysOut || maxKeys <= 0) {
		return 0;
	}
	for (const auto &kv : g_fake->bindings) {
		if (kv.second == binding) {
			if (n < maxKeys) {
				keysOut[n] = kv.first;
			}
			n++;
		}
	}
	return n > maxKeys ? maxKeys : n;
}

bool fake_getKeysForCommand(const char *command, int *key1, int *key2)
{
	if (!g_fake || !command || !key1 || !key2) {
		return false;
	}
	*key1 = -1;
	*key2 = -1;
	for (const auto &kv : g_fake->bindings) {
		if (kv.second == command) {
			if (*key1 < 0) {
				*key1 = kv.first;
			} else {
				*key2 = kv.first;
				break;
			}
		}
	}
	return true;
}

uid_backend_t MakeFakeBackend(FakeBackendState *state)
{
	uid_backend_t b;
	std::memset(&b, 0, sizeof(b));
	g_fake = state;
	b.alloc = fake_alloc;
	b.free = fake_free;
	b.cvarDescribe = fake_cvarDescribe;
	b.cvarWrite = fake_cvarWrite;
	b.cvarReset = fake_cvarReset;
	b.invokeAction = fake_invoke;
	b.drawSolidRect = fake_drawRect;
	b.drawPath = fake_drawPath;
	b.drawImage = fake_drawImage;
	b.imageMeasure = fake_imageMeasure;
	b.drawGradient = fake_drawGradient;
	b.pushClip = fake_pushClip;
	b.popClip = fake_popClip;
	b.beginShapeClip = fake_beginShapeClip;
	b.endShapeClip = fake_endShapeClip;
	b.beginImageMask = fake_beginImageMask;
	b.endImageMask = fake_endImageMask;
	b.fontMeasure = fake_fontMeasure;
	b.fontResolve = fake_fontResolve;
	b.fontDraw = fake_fontDraw;
	b.fontDrawRotated = fake_fontDrawRotated;
	b.queryOptions = fake_queryOptions;
	b.queryCollectionItems = fake_queryCollectionItems;
	b.keyNameToNum = fake_keyNameToNum;
	b.keyNumToName = fake_keyNumToName;
	b.getBinding = fake_getBinding;
	b.setBinding = fake_setBinding;
	b.findConflicts = fake_findConflicts;
	b.getKeysForCommand = fake_getKeysForCommand;
	b.drawHostRegion = fake_drawHostRegion;
	b.getHiResScale = fake_getHiResScale;
	b.getFramebufferSize = fake_getFramebufferSize;
	b.userdata = state;
	return b;
}

const char *kMinimal = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts>
      <font id="body" src="fonts/Oswald-Medium.ttf" weight="400"/>
    </fonts>
  </definitions>
  <canvas/>
</ui>
)";

const char *kLayoutDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"/>
  </definitions>
  <canvas>
    <container id="root" type="horizontal" width="100%" height="100%" gap="10px" padding="10px"
               halign="equal-spacing" valign="center">
      <container id="a" width="50px" height="20px" fill="#FF0000FF"/>
      <container id="b" width="fill" height="20px" fill="#00FF00FF"/>
      <container id="c" width="50px" height="20px" fill="#0000FFFF"/>
    </container>
  </canvas>
</ui>
)";

const char *kTemplateDoc = R"UID(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <templates>
      <template id="row">
        <props>
          <prop name="label" type="string" required="true"/>
          <prop name="bind" type="binding" required="true"/>
        </props>
        <container id="inner" type="vertical" width="100px" height="40px">
          <label id="lbl" width="fill">{template.label}</label>
          <toggle id="tog" bind="{template.bind}" width="40px" height="20px"/>
        </container>
      </template>
    </templates>
  </definitions>
  <canvas>
    <use id="u1" template="row" label="Hello" bind="cvar(cg_fov)"/>
  </canvas>
</ui>
)UID";

const char *kBindDoc = R"UID(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="200px" height="200px">
      <toggle id="t" bind="cvar:r_fullscreen" width="40px" height="20px"/>
      <button id="go">
        Go
        <on event="click">
          <set-cvar name="cg_fov" value="100"/>
          <invoke name="apply-video"/>
        </on>
      </button>
    </container>
  </canvas>
</ui>
)UID";

/* Added in OPM: select modal= opens type=relative modal (no procedural overlay paint). */
const char *kOverlayDoc = R"UID(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <modals>
      <modal id="mode-list" type="relative">
        <container type="overlap" width="100%" height="100%" fill="#00000000">
          <button id="dismiss" width="100%" height="100%" fill="#00000000">
            <on event="click"><hide-modal/></on>
          </button>
          <container id="panel" role="relative-panel" type="vertical" width="100%" height="auto"
                     overflow="scroll" fill="#101010FF">
            <button width="100%" height="28px">Alpha</button>
            <button width="100%" height="28px">Beta</button>
          </container>
        </container>
      </modal>
    </modals>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="200px" height="100px">
      <select id="mode" width="100px" height="24px" modal="mode-list">
        <option value="a" label="Alpha"/>
        <option value="b" label="Beta"/>
      </select>
    </container>
  </canvas>
</ui>
)UID";

const char *kCyclicDoc = R"UID(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="280px" height="80px">
      <select id="mode" appearance="cyclic" width="240px" height="auto"
              bind="cvar:r_fullscreen" commit="change"
              fill="#080A0CB8" color="#EBF0F5EB">
        <option value="1">Fullscreen</option>
        <option value="1">Borderless</option>
        <option value="0">Windowed</option>
      </select>
    </container>
  </canvas>
</ui>
)UID";

const char *kBadDoctype = R"(
<!DOCTYPE ui SYSTEM "x.dtd">
<ui version="1"><definitions/><canvas/></ui>
)";

void TestValues(void)
{
	uid_length_t len;
	uid_color_t  col;
	uid_sides_t  sides;
	bool         b;
	CHECK(UID_ParseLength("12px", &len, nullptr));
	CHECK(len.unit == UID_LENGTH_PX);
	CHECK_EQ_F(len.value, 12.0, 1e-5);
	CHECK(UID_ParseLength("50%", &len, nullptr));
	CHECK(len.unit == UID_LENGTH_PERCENT);
	CHECK(UID_ParseLength("fill", &len, nullptr));
	CHECK(len.unit == UID_LENGTH_FILL);
	CHECK(UID_ParseLength("nan", &len, nullptr) == false);
	CHECK(UID_ParseLength("nanpx", &len, nullptr) == false);
	CHECK(UID_ParseColor("#RRGGBB", &col, nullptr) == false);
	CHECK(UID_ParseColor("#FF0000", &col, nullptr));
	CHECK_EQ_F(col.r, 1.0, 1e-5);
	CHECK_EQ_F(col.a, 1.0, 1e-5);
	CHECK(UID_ParseColor("#0000FF80", &col, nullptr));
	CHECK_EQ_F(col.a, 128.0 / 255.0, 1e-3);
	CHECK(UID_ParseBool("true", &b, nullptr) && b);
	CHECK(UID_ParseSides("1px 2px", &sides, nullptr));
	CHECK_EQ_F(sides.top.value, 1.0, 1e-5);
	CHECK_EQ_F(sides.left.value, 2.0, 1e-5);
	CHECK(!UID_ParseSides("1px 2px 3px", &sides, nullptr));
	std::string n;
	CHECK(UID_NormalizeAttrName("fontsize", &n));
	CHECK(n == "font-size");
	double nanProbe = 0.0;
	CHECK(!UID_ParseNumber("nan", &nanProbe, nullptr));
	CHECK(!UID_ParseNumber("inf", &nanProbe, nullptr));
	CHECK(!UID_ParseLength("nanpx", &len, nullptr));

	float rotDeg = 0.0f;
	CHECK(UID_ParseRotationDeg("90", &rotDeg, nullptr));
	CHECK_EQ_F(rotDeg, 90.0, 1e-5);
	CHECK(UID_ParseRotationDeg("90deg", &rotDeg, nullptr));
	CHECK_EQ_F(rotDeg, 90.0, 1e-5);
	CHECK(UID_ParseRotationDeg("-45", &rotDeg, nullptr));
	CHECK_EQ_F(rotDeg, -45.0, 1e-5);
	CHECK(!UID_ParseRotationDeg("nan", &rotDeg, nullptr));

	float ox = 0.0f;
	float oy = 0.0f;
	CHECK(UID_ParseRotationOrigin("50% 50%", 16.0f, 16.0f, 1.0f, &ox, &oy, nullptr));
	CHECK_EQ_F(ox, 8.0, 1e-5);
	CHECK_EQ_F(oy, 8.0, 1e-5);
	CHECK(UID_ParseRotationOrigin("8px 58px", 16.0f, 16.0f, 1.0f, &ox, &oy, nullptr));
	CHECK_EQ_F(ox, 8.0, 1e-5);
	CHECK_EQ_F(oy, 58.0, 1e-5);

	std::string cvarName;
	CHECK(UID_ParseExactCvarBraceBinding("{cvar.ui_om_hud_clip_top}", &cvarName));
	CHECK(cvarName == "ui_om_hud_clip_top");
	CHECK(UID_ParseExactCvarBraceBinding("{cvar:ui_om_hud_health}", &cvarName));
	CHECK(cvarName == "ui_om_hud_health");
	CHECK(!UID_ParseExactCvarBraceBinding("textures/hud/clip_rifle", &cvarName));
	/* Fixed in OPM: style ternaries that start with cvar. are not exact cvar binds. */
	CHECK(!UID_ParseExactCvarBraceBinding(
		"{cvar.ui_om_hud_last_gun != cvar.ui_om_hud_primary_name ? var.fill-panel : var.fill-transparent}",
		&cvarName
	));
	CHECK(!UID_ParseExactCvarBraceBinding("{cvar.a == on ? #FF0000FF : #00000000}", &cvarName));

	int ms = 0;
	CHECK(UID_ParseDurationMs("6s", &ms, nullptr) && ms == 6000);
	CHECK(UID_ParseDurationMs("1.5s", &ms, nullptr) && ms == 1500);
	CHECK(UID_ParseDurationMs("500ms", &ms, nullptr) && ms == 500);
	CHECK(UID_ParseDurationMs("2", &ms, nullptr) && ms == 2000);
	CHECK(!UID_ParseDurationMs("5m", &ms, nullptr));
	CHECK(!UID_ParseDurationMs("-1s", &ms, nullptr));
}

void TestExpr(void)
{
	struct Ctx {
		double w, h, r;
	} ctx{100, 50, 8};
	auto lookup = [](void *ud, const char *path, double *out) -> bool {
		Ctx *c = (Ctx *)ud;
		if (!path || !out) {
			return false;
		}
		if (std::strcmp(path, "parent.width") == 0) {
			*out = c->w;
			return true;
		}
		if (std::strcmp(path, "parent.height") == 0) {
			*out = c->h;
			return true;
		}
		if (std::strcmp(path, "shape.radius") == 0) {
			*out = c->r;
			return true;
		}
		return false;
	};
	double v = 0;
	uid_expr_limits_t lim;
	UID_DefaultExprLimits(&lim);
	CHECK(UID_EvalNumber("parent.width - shape.radius", lookup, &ctx, &lim, &v, nullptr));
	CHECK_EQ_F(v, 92.0, 1e-5);
	std::string s;
	CHECK(UID_InterpolateString("M {shape.radius} 0", lookup, &ctx, &lim, &s, nullptr));
	CHECK(s.find("8") != std::string::npos);
}

void TestDynamicBackgroundImageCompile(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
    <container id="b" width="64px" height="64px" background-image="textures/hud/healthback"/>
    <container id="c" width="64px" height="64px" background-image="{cvar.ui_om_hud_health_frac}"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("dyn_bg.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	UID_DestroyDocument(doc);
}

void TestCvarDotPropResolve(void)
{
	FakeBackendState st;
	st.cvars["ui_om_hud_clip_top"] = FakeCvar{"0.25", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	std::string out;
	CHECK(UID_ResolvePropString(&be, "{cvar.ui_om_hud_clip_top}", &out));
	CHECK(out == "0.25");
}

void TestModuloExpression(void)
{
	uid_expr_limits_t lim;
	UID_DefaultExprLimits(&lim);
	double v = 0.0;
	CHECK(UID_EvalNumber("10 % 3", nullptr, nullptr, &lim, &v, nullptr));
	CHECK_EQ_F(v, 1.0, 1e-5);
	CHECK(UID_EvalNumber("7 % 2", nullptr, nullptr, &lim, &v, nullptr));
	CHECK_EQ_F(v, 1.0, 1e-5);
}

/* Added in OPM: whitelisted abs/min/max/clamp numeric helpers. */
void TestNumericHelperFunctions(void)
{
	uid_expr_limits_t lim;
	UID_DefaultExprLimits(&lim);
	double v = 0.0;
	std::string diag;

	CHECK(UID_EvalNumber("abs(-3)", nullptr, nullptr, &lim, &v, nullptr));
	CHECK_EQ_F(v, 3.0, 1e-5);
	CHECK(UID_EvalNumber("abs(4.5)", nullptr, nullptr, &lim, &v, nullptr));
	CHECK_EQ_F(v, 4.5, 1e-5);

	CHECK(UID_EvalNumber("floor(2.9)", nullptr, nullptr, &lim, &v, nullptr));
	CHECK_EQ_F(v, 2.0, 1e-5);
	CHECK(UID_EvalNumber("floor(125 / 60)", nullptr, nullptr, &lim, &v, nullptr));
	CHECK_EQ_F(v, 2.0, 1e-5);

	CHECK(UID_EvalNumber("min(3, 7)", nullptr, nullptr, &lim, &v, nullptr));
	CHECK_EQ_F(v, 3.0, 1e-5);
	CHECK(UID_EvalNumber("max(3, 7)", nullptr, nullptr, &lim, &v, nullptr));
	CHECK_EQ_F(v, 7.0, 1e-5);

	CHECK(UID_EvalNumber("clamp(5, 0, 10)", nullptr, nullptr, &lim, &v, nullptr));
	CHECK_EQ_F(v, 5.0, 1e-5);
	CHECK(UID_EvalNumber("clamp(-2, 0, 10)", nullptr, nullptr, &lim, &v, nullptr));
	CHECK_EQ_F(v, 0.0, 1e-5);
	CHECK(UID_EvalNumber("clamp(20, 0, 10)", nullptr, nullptr, &lim, &v, nullptr));
	CHECK_EQ_F(v, 10.0, 1e-5);
	CHECK(UID_EvalNumber("clamp(5, 10, 0)", nullptr, nullptr, &lim, &v, nullptr));
	CHECK_EQ_F(v, 5.0, 1e-5);

	struct Ctx {
		double bearing;
	} ctx{135.0};
	auto lookup = [](void *userdata, const char *path, double *out) -> bool {
		auto *c = static_cast<Ctx *>(userdata);
		if (std::strcmp(path, "cvar.bearing") == 0) {
			*out = c->bearing;
			return true;
		}
		return false;
	};
	CHECK(UID_EvalNumber("clamp(cvar.bearing, -90, 90)", lookup, &ctx, &lim, &v, nullptr));
	CHECK_EQ_F(v, 90.0, 1e-5);
	CHECK(UID_EvalNumber("clamp(cvar.bearing, -90, 90) * 2", lookup, &ctx, &lim, &v, nullptr));
	CHECK_EQ_F(v, 180.0, 1e-5);

	diag.clear();
	CHECK(!UID_EvalNumber("foo(1)", nullptr, nullptr, &lim, &v, &diag));
	CHECK(diag.find("unknown function") != std::string::npos);
	diag.clear();
	CHECK(!UID_EvalNumber("abs(1, 2)", nullptr, nullptr, &lim, &v, &diag));
	CHECK(!diag.empty());
	diag.clear();
	CHECK(!UID_EvalNumber("min(1)", nullptr, nullptr, &lim, &v, &diag));
	CHECK(!diag.empty());
}

void TestRuntimeNumericExprBinding(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <container id="dyn" width="{cvar.ui_test_a + cvar.ui_test_b}px" height="8px"
                 top="{(cvar.ui_test_max - cvar.ui_test_cur) / cvar.ui_test_max}"/>
    </container>
  </canvas>
</ui>
)";
	FakeBackendState st;
	st.cvars["ui_test_a"] = FakeCvar{"10", 0};
	st.cvars["ui_test_b"] = FakeCvar{"20", 0};
	st.cvars["ui_test_max"] = FakeCvar{"32", 0};
	st.cvars["ui_test_cur"] = FakeCvar{"31", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("runtime_num.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	UID_SyncBindings(doc, &be);
	uid_node_id_t dynId = doc->idIndex.count("dyn") ? doc->idIndex["dyn"] : UID_INVALID_NODE_ID;
	CHECK(dynId >= 0);
	const char *width = doc->nodes[static_cast<size_t>(dynId)].properties.GetCStr("width", "");
	CHECK(std::strcmp(width, "30px") == 0);
	const char *top = doc->nodes[static_cast<size_t>(dynId)].properties.GetCStr("top", "");
	CHECK(std::strcmp(top, "0.03125") == 0);
	st.cvars["ui_test_a"] = FakeCvar{"5", 0};
	UID_SyncBindings(doc, &be);
	width = doc->nodes[static_cast<size_t>(dynId)].properties.GetCStr("width", "");
	CHECK(std::strcmp(width, "25px") == 0);
	UID_DestroyDocument(doc);
}

void TestForeachCountExpansion(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <foreach id="bullets" count="{cvar.ui_test_count}">
        <container id="bullet" width="4px" height="4px"/>
      </foreach>
    </container>
  </canvas>
</ui>
)";
	FakeBackendState st;
	st.cvars["ui_test_count"] = FakeCvar{"3", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("foreach_count.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	uid_node_id_t foreachId = doc->idIndex.count("bullets") ? doc->idIndex["bullets"] : UID_INVALID_NODE_ID;
	CHECK(foreachId >= 0);
	CHECK(doc->nodes[static_cast<size_t>(foreachId)].hasForeachCount);
	CHECK(doc->nodes[static_cast<size_t>(foreachId)].foreachTemplateRoot >= 0);
	CHECK(!doc->nodes[static_cast<size_t>(foreachId)].foreachTemplateNodes.empty());
	double countVal = 0.0;
	CHECK(UID_EvalRuntimeNumericExpr(
		doc,
		foreachId,
		doc->nodes[static_cast<size_t>(foreachId)].foreachCountExpr,
		&be,
		&countVal
	));
	CHECK_EQ_F(countVal, 3.0, 1e-5);
	UID_SyncBindings(doc, &be);
	const size_t childCount = doc->nodes[static_cast<size_t>(foreachId)].children.size();
	CHECK(childCount == 3);
	st.cvars["ui_test_count"] = FakeCvar{"1", 0};
	UID_SyncBindings(doc, &be);
	CHECK(doc->nodes[static_cast<size_t>(foreachId)].children.size() == 1);
	UID_DestroyDocument(doc);
}

/* Added in OPM: {var.*} on foreach template props (stroke, etc.) must resolve. */
void TestForeachTemplateVarStrokeResolve(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <vars>
      <var id="fill-divider" value="#AABBCCDD"/>
    </vars>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="horizontal" width="100%" height="100%">
      <foreach id="pips" count="2" type="horizontal" gap="4px">
        <container id="pip" width="32px" height="16px"
                   stroke="{var.fill-divider}" stroke-width="1px"
                   fill="#FFFFFFFF"/>
      </foreach>
    </container>
  </canvas>
</ui>
)";
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("foreach_var_stroke.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());

	uid_node_id_t foreachId = doc->idIndex.count("pips") ? doc->idIndex["pips"] : UID_INVALID_NODE_ID;
	CHECK(foreachId >= 0);
	const uid_node_def_t &fn = doc->nodes[static_cast<size_t>(foreachId)];
	CHECK(fn.foreachTemplateRoot >= 0);
	CHECK(!fn.foreachTemplateNodes.empty());
	const char *tmplStroke =
		fn.foreachTemplateNodes[static_cast<size_t>(fn.foreachTemplateRoot)].properties.GetCStr("stroke", "");
	/* Wrap root may not hold stroke — check template child. */
	bool tmplOk = tmplStroke && std::strcmp(tmplStroke, "#AABBCCDD") == 0;
	if (!tmplOk && !fn.foreachTemplateNodes[static_cast<size_t>(fn.foreachTemplateRoot)].children.empty()) {
		const uid_node_id_t childTmpl =
			fn.foreachTemplateNodes[static_cast<size_t>(fn.foreachTemplateRoot)].children[0];
		tmplStroke = fn.foreachTemplateNodes[static_cast<size_t>(childTmpl)].properties.GetCStr("stroke", "");
		tmplOk = tmplStroke && std::strcmp(tmplStroke, "#AABBCCDD") == 0;
	}
	CHECK(tmplOk);

	UID_SyncBindings(doc, &be);
	CHECK(doc->nodes[static_cast<size_t>(foreachId)].children.size() == 2);
	bool any = false;
	for (uid_node_id_t c : doc->nodes[static_cast<size_t>(foreachId)].children) {
		const uid_node_def_t *n = &doc->nodes[static_cast<size_t>(c)];
		const char *stroke = n->properties.GetCStr("stroke", "");
		if ((!stroke || std::strcmp(stroke, "#AABBCCDD") != 0) && !n->children.empty()) {
			n = &doc->nodes[static_cast<size_t>(n->children[0])];
			stroke = n->properties.GetCStr("stroke", "");
		}
		CHECK(stroke && std::strcmp(stroke, "#AABBCCDD") == 0);
		any = true;
	}
	CHECK(any);
	UID_DestroyDocument(doc);
}

void TestHudAmmoCatalogCompile(void)
{
	g_testImportFiles.clear();
	g_testImportFiles["ui/modern/lib/hud_ammo.xml"] = R"(<ui-library version="1">
  <sources>
    <source id="classic-ammo-defs">
      <item value="MP40" label="MP40" texture="textures/hud/clip_pistol"
            tile-width="32" row-height="16" lanes="2" stagger-x="4" stagger-y="8" use-ammo="0"/>
      <item value="Walther P38" label="Walther P38" texture="textures/hud/clip_pistol"
            tile-width="32" row-height="16" lanes="1" stagger-x="0" stagger-y="0" use-ammo="0"/>
    </source>
  </sources>
  <templates>
    <template id="classic-ammo-clip-single-clip">
      <container width="{item.field.tile-width}px"
                 height="{cvar.ui_om_hud_max_clip * item.field.row-height}px"
                 top="{(cvar.ui_om_hud_max_clip - cvar.ui_om_hud_clip) / cvar.ui_om_hud_max_clip}"
                 visible="{item.field.lanes == 1}"/>
    </template>
    <template id="classic-ammo-panel">
      <container source="classic-ammo-defs" bind="cvar:ui_om_hud_active_weapon">
        <foreach mode="selected">
          <use template="classic-ammo-clip-single-clip"/>
        </foreach>
      </container>
    </template>
  </templates>
</ui-library>
)";

	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <import src="ui/modern/lib/hud_ammo.xml"/>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <use template="classic-ammo-panel"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	CHECK(UID_ParseXml("hud_ammo_panel.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(doc->definitions.sources.count("classic-ammo-defs") == 1);
	CHECK(doc->definitions.templates.count("classic-ammo-panel") == 1);
	g_testImportFiles.clear();
	UID_DestroyDocument(doc);
}

void TestAmmoClipTopSync(void)
{
	g_testImportFiles.clear();
	g_testImportFiles["ui/modern/lib/hud_ammo.xml"] = R"(<ui-library version="1">
  <sources>
    <source id="classic-ammo-defs">
      <item value="Walther P38" label="Walther P38" texture="textures/hud/clip_pistol"
            tile-width="32" row-height="16" lanes="1" stagger-x="0" stagger-y="0" use-ammo="0"/>
    </source>
  </sources>
  <templates>
    <template id="classic-ammo-clip-single-clip">
      <container width="{item.field.tile-width}px"
                 height="{cvar.ui_om_hud_max_clip * item.field.row-height}px"
                 background-image="{item.field.texture}" background-fit="repeat"
                 shape="edge-clip" left="0"
                 top="{(cvar.ui_om_hud_max_clip - cvar.ui_om_hud_clip) / cvar.ui_om_hud_max_clip}"
                 right="1" bottom="1"
                 visible="{item.field.lanes == 1}"/>
    </template>
    <template id="classic-ammo-panel">
      <container source="classic-ammo-defs" bind="cvar:ui_om_hud_active_weapon">
        <foreach mode="selected">
          <use template="classic-ammo-clip-single-clip"/>
        </foreach>
      </container>
    </template>
  </templates>
</ui-library>
)";

	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <import src="ui/modern/lib/hud_ammo.xml"/>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <use template="classic-ammo-panel"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	FakeBackendState st;
	st.cvars["ui_om_hud_active_weapon"] = FakeCvar{"Walther P38", 0};
	st.cvars["ui_om_hud_max_clip"] = FakeCvar{"8", 0};
	st.cvars["ui_om_hud_clip"] = FakeCvar{"6", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("ammo_top_sync.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	bool foundClip = false;
	for (const uid_node_def_t &n : doc->nodes) {
		if (!n.foreachGenerated || n.properties.GetCStr("shape", nullptr) == nullptr) {
			continue;
		}
		const char *shape = n.properties.GetCStr("shape", "");
		if (std::strcmp(shape, "edge-clip") != 0) {
			continue;
		}
		const char *top = n.properties.GetCStr("top", nullptr);
		CHECK(top != nullptr);
		double topVal = 0.0;
		CHECK(UID_ParseNumber(top, &topVal, nullptr));
		CHECK_EQ_F(topVal, 0.25, 1e-5);
		const char *bg = n.properties.GetCStr("background-image", nullptr);
		CHECK(bg != nullptr);
		CHECK(std::strcmp(bg, "textures/hud/clip_pistol") == 0);
		foundClip = true;
	}
	CHECK(foundClip);

	g_testImportFiles.clear();
	UID_DestroyDocument(doc);
}


void TestModernWeaponsBarImageFieldSync(void)
{
	g_testImportFiles.clear();
	g_testImportFiles["ui/modern/lib/hud_modern_img.xml"] = R"(<ui-library version="1">
  <images>
    <image id="modernhud-m1-garand" src="ui/modern/textures/modernhud/m1_garand"/>
    <image id="modernhud-colt45" src="ui/modern/textures/modernhud/colt45"/>
  </images>
  <sources>
    <source id="modern-primary-weapons">
      <item value="M1 Garand" label="M1 Garand" image="modernhud-m1-garand"/>
    </source>
    <source id="modern-sidearm-weapons">
      <item value="Colt 45" label="Colt 45" image="modernhud-colt45"/>
    </source>
  </sources>
  <templates>
    <template id="modern-weapons-bar">
      <container type="overlap" width="192px" height="64px">
        <container width="192px" height="64px" source="modern-primary-weapons" bind="cvar:ui_om_hud_last_gun">
          <foreach mode="selected" width="192px" height="64px">
            <container width="192px" height="64px" background-image="{item.field.image}" background-fit="contain"/>
          </foreach>
        </container>
        <container width="192px" height="64px" source="modern-sidearm-weapons" bind="cvar:ui_om_hud_last_gun">
          <foreach mode="selected" width="192px" height="64px">
            <container width="192px" height="64px" background-image="{item.field.image}" background-fit="contain"/>
          </foreach>
        </container>
      </container>
    </template>
  </templates>
</ui-library>
)";
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <import src="ui/modern/lib/hud_modern_img.xml"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <use id="weapons_bar" template="modern-weapons-bar"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	FakeBackendState st;
	st.cvars["ui_om_hud_last_gun"] = FakeCvar{"M1 Garand", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("modern_img.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	bool found = false;
	for (const uid_node_def_t &n : doc->nodes) {
		if (!n.foreachGenerated) {
			continue;
		}
		const char *bg = n.properties.GetCStr("background-image", nullptr);
		if (!bg) {
			continue;
		}
		std::printf("bg='%s' exprBound=%zu\n", bg, n.exprBoundProps.size());
		if (std::strcmp(bg, "modernhud-m1-garand") == 0) {
			found = true;
		}
	}
	CHECK(found);

	g_testImportFiles.clear();
	UID_DestroyDocument(doc);
}

void TestAmmoStaggerLanesSync(void)
{
	g_testImportFiles.clear();
	g_testImportFiles["ui/modern/lib/hud_ammo.xml"] = R"(<ui-library version="1">
  <sources>
    <source id="classic-ammo-defs">
      <item value="MP40" label="MP40" texture="textures/hud/clip_pistol"
            tile-width="32" row-height="16" lanes="2" stagger-x="4" stagger-y="8" use-ammo="0"/>
    </source>
  </sources>
  <templates>
    <template id="classic-ammo-clip-stagger">
      <container type="overlap" width="{item.field.tile-width}px"
                 height="{((cvar.ui_om_hud_max_clip + (cvar.ui_om_hud_max_clip % 2)) / 2) * item.field.row-height}px"
                 visible="{item.field.lanes == 2}">
        <container width="{item.field.tile-width}px"
                   height="{((cvar.ui_om_hud_max_clip + (cvar.ui_om_hud_max_clip % 2)) / 2) * item.field.row-height}px"
                   margin="{item.field.stagger-y}px 0 0 {item.field.stagger-x}px"
                   background-image="{item.field.texture}" background-fit="repeat"
                   shape="edge-clip" left="0"
                   top="{((cvar.ui_om_hud_max_clip - cvar.ui_om_hud_clip) - ((cvar.ui_om_hud_max_clip - cvar.ui_om_hud_clip) % 2)) / cvar.ui_om_hud_max_clip}"
                   right="1" bottom="1"/>
        <container width="{item.field.tile-width}px"
                   height="{((cvar.ui_om_hud_max_clip + (cvar.ui_om_hud_max_clip % 2)) / 2) * item.field.row-height}px"
                   background-image="{item.field.texture}" background-fit="repeat"
                   shape="edge-clip" left="0"
                   top="{((cvar.ui_om_hud_max_clip - cvar.ui_om_hud_clip) + ((cvar.ui_om_hud_max_clip - cvar.ui_om_hud_clip) % 2)) / cvar.ui_om_hud_max_clip}"
                   right="1" bottom="1"/>
      </container>
    </template>
    <template id="classic-ammo-panel">
      <container source="classic-ammo-defs" bind="cvar:ui_om_hud_active_weapon">
        <foreach mode="selected">
          <use template="classic-ammo-clip-stagger"/>
        </foreach>
      </container>
    </template>
  </templates>
</ui-library>
)";

	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <import src="ui/modern/lib/hud_ammo.xml"/>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <use template="classic-ammo-panel"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	FakeBackendState st;
	st.cvars["ui_om_hud_active_weapon"] = FakeCvar{"MP40", 0};
	st.cvars["ui_om_hud_max_clip"] = FakeCvar{"32", 0};
	st.cvars["ui_om_hud_clip"] = FakeCvar{"31", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("ammo_stagger_sync.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	int staggerLaneCount = 0;
	int marginLaneCount = 0;
	for (const uid_node_def_t &n : doc->nodes) {
		if (!n.foreachGenerated || n.properties.GetCStr("shape", nullptr) == nullptr) {
			continue;
		}
		const char *shape = n.properties.GetCStr("shape", "");
		if (std::strcmp(shape, "edge-clip") != 0) {
			continue;
		}
		++staggerLaneCount;
		const char *bg = n.properties.GetCStr("background-image", nullptr);
		CHECK(bg != nullptr);
		CHECK(std::strcmp(bg, "textures/hud/clip_pistol") == 0);
		const char *margin = n.properties.GetCStr("margin", nullptr);
		if (margin && std::strcmp(margin, "8px 0 0 4px") == 0) {
			++marginLaneCount;
			const char *top = n.properties.GetCStr("top", nullptr);
			CHECK(top != nullptr);
			double topVal = 0.0;
			CHECK(UID_ParseNumber(top, &topVal, nullptr));
			CHECK_EQ_F(topVal, 0.0, 1e-5);
		} else {
			const char *top = n.properties.GetCStr("top", nullptr);
			CHECK(top != nullptr);
			double topVal = 0.0;
			CHECK(UID_ParseNumber(top, &topVal, nullptr));
			CHECK_EQ_F(topVal, 0.0625, 1e-5);
		}
	}
	CHECK(staggerLaneCount == 2);
	CHECK(marginLaneCount == 1);

	g_testImportFiles.clear();
	UID_DestroyDocument(doc);
}

void TestExprBoundWidthMultiply(void)
{
	g_testImportFiles.clear();
	g_testImportFiles["ui/modern/lib/hud_ammo.xml"] = R"(<ui-library version="1">
  <sources>
    <source id="classic-ammo-defs">
      <item value="Walther P38" label="Walther P38" texture="textures/hud/clip_pistol"
            tile-width="32" row-height="16" lanes="1" stagger-x="0" stagger-y="0" use-ammo="0"/>
    </source>
  </sources>
  <templates>
    <template id="classic-ammo-clip-single-clip">
      <container width="{item.field.tile-width * 2}px"
                 height="{cvar.ui_om_hud_max_clip * item.field.row-height * 2}px"
                 background-image="{item.field.texture}" background-fit="repeat"
                 shape="edge-clip" left="0"
                 top="{(cvar.ui_om_hud_max_clip - cvar.ui_om_hud_clip) / cvar.ui_om_hud_max_clip}"
                 right="1" bottom="1"
                 visible="{item.field.lanes == 1 and item.field.use-ammo == 0}"/>
    </template>
    <template id="classic-ammo-panel">
      <container source="classic-ammo-defs" bind="cvar:ui_om_hud_active_weapon">
        <foreach mode="selected">
          <use template="classic-ammo-clip-single-clip"/>
        </foreach>
      </container>
    </template>
  </templates>
</ui-library>
)";

	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <import src="ui/modern/lib/hud_ammo.xml"/>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <use template="classic-ammo-panel"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	FakeBackendState st;
	st.cvars["ui_om_hud_active_weapon"] = FakeCvar{"Walther P38", 0};
	st.cvars["ui_om_hud_max_clip"] = FakeCvar{"8", 0};
	st.cvars["ui_om_hud_clip"] = FakeCvar{"6", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("ammo_width_mul.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	bool foundClip = false;
	for (const uid_node_def_t &n : doc->nodes) {
		if (!n.foreachGenerated || n.properties.GetCStr("shape", nullptr) == nullptr) {
			continue;
		}
		if (std::strcmp(n.properties.GetCStr("shape", ""), "edge-clip") != 0) {
			continue;
		}
		const char *width = n.properties.GetCStr("width", nullptr);
		const char *height = n.properties.GetCStr("height", nullptr);
		const char *bg = n.properties.GetCStr("background-image", nullptr);
		CHECK(width != nullptr);
		CHECK(height != nullptr);
		CHECK(bg != nullptr);
		CHECK(std::strcmp(width, "64px") == 0);
		CHECK(std::strcmp(height, "256px") == 0);
		CHECK(std::strcmp(bg, "textures/hud/clip_pistol") == 0);
		foundClip = true;
	}
	CHECK(foundClip);

	g_testImportFiles.clear();
	UID_DestroyDocument(doc);
}

void TestExprBoundMarginDoubleMultiply(void)
{
	g_testImportFiles.clear();
	g_testImportFiles["ui/modern/lib/hud_ammo.xml"] = R"(<ui-library version="1">
  <sources>
    <source id="classic-ammo-defs">
      <item value="MP40" label="MP40" texture="textures/hud/clip_pistol"
            tile-width="32" row-height="16" lanes="2" stagger-x="4" stagger-y="8" use-ammo="0"/>
    </source>
  </sources>
  <templates>
    <template id="classic-ammo-clip-stagger">
      <container type="overlap" width="{item.field.tile-width * 2}px"
                 height="{((cvar.ui_om_hud_max_clip + (cvar.ui_om_hud_max_clip % 2)) / 2) * item.field.row-height * 2}px"
                 visible="{item.field.lanes == 2}">
        <container width="{item.field.tile-width * 2}px"
                   height="{((cvar.ui_om_hud_max_clip + (cvar.ui_om_hud_max_clip % 2)) / 2) * item.field.row-height * 2}px"
                   margin="{item.field.stagger-y * 2}px 0 0 {item.field.stagger-x * 2}px"
                   background-image="{item.field.texture}" background-fit="repeat"
                   shape="edge-clip" left="0"
                   top="{((cvar.ui_om_hud_max_clip - cvar.ui_om_hud_clip) - ((cvar.ui_om_hud_max_clip - cvar.ui_om_hud_clip) % 2)) / cvar.ui_om_hud_max_clip}"
                   right="1" bottom="1"/>
        <container width="{item.field.tile-width * 2}px"
                   height="{((cvar.ui_om_hud_max_clip + (cvar.ui_om_hud_max_clip % 2)) / 2) * item.field.row-height * 2}px"
                   background-image="{item.field.texture}" background-fit="repeat"
                   shape="edge-clip" left="0"
                   top="{((cvar.ui_om_hud_max_clip - cvar.ui_om_hud_clip) + ((cvar.ui_om_hud_max_clip - cvar.ui_om_hud_clip) % 2)) / cvar.ui_om_hud_max_clip}"
                   right="1" bottom="1"/>
      </container>
    </template>
    <template id="classic-ammo-panel">
      <container source="classic-ammo-defs" bind="cvar:ui_om_hud_active_weapon">
        <foreach mode="selected">
          <use template="classic-ammo-clip-stagger"/>
        </foreach>
      </container>
    </template>
  </templates>
</ui-library>
)";

	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <import src="ui/modern/lib/hud_ammo.xml"/>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <use template="classic-ammo-panel"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	FakeBackendState st;
	st.cvars["ui_om_hud_active_weapon"] = FakeCvar{"MP40", 0};
	st.cvars["ui_om_hud_max_clip"] = FakeCvar{"32", 0};
	st.cvars["ui_om_hud_clip"] = FakeCvar{"31", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("ammo_margin_mul.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	int marginLaneCount = 0;
	for (const uid_node_def_t &n : doc->nodes) {
		if (!n.foreachGenerated || n.properties.GetCStr("shape", nullptr) == nullptr) {
			continue;
		}
		if (std::strcmp(n.properties.GetCStr("shape", ""), "edge-clip") != 0) {
			continue;
		}
		const char *margin = n.properties.GetCStr("margin", nullptr);
		if (margin && std::strcmp(margin, "16px 0 0 8px") == 0) {
			++marginLaneCount;
		}
	}
	CHECK(marginLaneCount == 1);

	g_testImportFiles.clear();
	UID_DestroyDocument(doc);
}

/* Added in OPM: auto container sizes to fixed children + padding + stroke (modern weapons_bar). */
void TestWeaponsBarAutoChildrenPadding(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"/>
  </definitions>
  <canvas>
    <container id="hud_bottom_row" type="horizontal" width="100%" height="100px">
      <container id="weapons_bar" type="vertical" width="auto" height="auto"
                 gap="0" padding="12px 12px 0px 12px"
                 fill="#00000040" stroke="#FFFFFFFF" stroke-width="1px">
        <container id="weapons_bar_inner" type="horizontal" width="300px" height="100px" fill="#FF000040"/>
        <container id="weapons_bar_outer" type="horizontal" width="300px" height="100px" fill="#00FF0040"/>
      </container>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("weapons_bar_auto.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 1280, 720, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_node_id_t barId = doc->idIndex.at("weapons_bar");
	const uid_node_id_t innerId = doc->idIndex.at("weapons_bar_inner");
	const uid_node_id_t outerId = doc->idIndex.at("weapons_bar_outer");
	CHECK(barId >= 0 && innerId >= 0 && outerId >= 0);

	const uid_rect_t &bar = doc->states[static_cast<size_t>(barId)].borderBox;
	const uid_rect_t &content = doc->states[static_cast<size_t>(barId)].contentBox;
	const uid_rect_t &inner = doc->states[static_cast<size_t>(innerId)].borderBox;
	const uid_rect_t &outer = doc->states[static_cast<size_t>(outerId)].borderBox;

	CHECK_EQ_F(bar.w, 326.0f, 0.5f);
	CHECK_EQ_F(bar.h, 214.0f, 0.5f);
	CHECK_EQ_F(inner.x, content.x, 0.5f);
	CHECK_EQ_F(inner.y, content.y, 0.5f);
	CHECK_EQ_F(inner.w, 300.0f, 0.5f);
	CHECK_EQ_F(inner.h, 100.0f, 0.5f);
	CHECK_EQ_F(outer.x, content.x, 0.5f);
	CHECK_EQ_F(outer.y, inner.y + inner.h, 0.5f);
	CHECK_EQ_F(outer.w, 300.0f, 0.5f);
	CHECK_EQ_F(outer.h, 100.0f, 0.5f);

	UID_DestroyDocument(doc);
}

/* Added in OPM: stroke-layout=false keeps auto size at children+padding only. */
void TestStrokeLayoutFalseAutoSize(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"/>
  </definitions>
  <canvas>
    <container id="hud_bottom_row" type="horizontal" width="100%" height="100px">
      <container id="weapons_bar" type="vertical" width="auto" height="auto"
                 gap="0" padding="12px 12px 0px 12px"
                 fill="#00000040" stroke="#FFFFFFFF" stroke-width="1px"
                 stroke-layout="false">
        <container id="weapons_bar_inner" type="horizontal" width="300px" height="100px" fill="#FF000040"/>
        <container id="weapons_bar_outer" type="horizontal" width="300px" height="100px" fill="#00FF0040"/>
      </container>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("weapons_bar_stroke_layout_false.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 1280, 720, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_node_id_t barId = doc->idIndex.at("weapons_bar");
	CHECK(barId >= 0);
	const uid_rect_t &bar = doc->states[static_cast<size_t>(barId)].borderBox;
	const uid_rect_t &content = doc->states[static_cast<size_t>(barId)].contentBox;
	CHECK_EQ_F(bar.w, 324.0f, 0.5f);
	CHECK_EQ_F(bar.h, 212.0f, 0.5f);
	CHECK_EQ_F(content.w, 300.0f, 0.5f);
	CHECK_EQ_F(content.h, 200.0f, 0.5f);

	UID_DestroyDocument(doc);
}

/* Added in OPM: non-rect shape clips descendants during paint (layout unchanged). */
void TestShapeClipsChildrenPaint(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"/>
    <shapes>
      <shape id="skew-rect">
        <props>
          <prop name="skewl" type="length" default="12px"/>
          <prop name="skewr" type="length" default="12px"/>
        </props>
        <path fill="{parent.fill}"
          d="M 0 {parent.height} L {shape.skewl} 0 L {parent.width} 0 L {parent.width - shape.skewr} {parent.height} Z"/>
      </shape>
    </shapes>
  </definitions>
  <canvas>
    <container id="bar" type="vertical" width="auto" height="auto" padding="12px"
               fill="#00000080" shape="skew-rect" skewl="50px" skewr="50px">
      <container id="inner" width="300px" height="100px" fill="#FF000080"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("shape_child_clip.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 800, 600, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_PaintChrome(doc, &be);

	CHECK(st.shapeClipBegins >= 1);
	CHECK(st.shapeClipEnds >= 1);
	CHECK(st.shapeClipDepth == 0);
	bool sawSkewPath = false;
	for (const std::string &s : st.shapeClipPaths) {
		if (s.find("skewl") != std::string::npos || s.find("50") != std::string::npos ||
		    s.find("M 0") != std::string::npos) {
			sawSkewPath = true;
		}
	}
	CHECK(sawSkewPath);

	UID_DestroyDocument(doc);
}

/* Added in OPM: mask-image wraps background + children; nested mask fails. */
void TestImageMaskPaint(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"/>
    <images>
      <image id="soft-mask" src="ui/modern/textures/panel.png"/>
    </images>
  </definitions>
  <canvas>
    <container id="outer" width="200px" height="100px" fill="#FF000080"
               mask-image="soft-mask" mask-fit="stretch">
      <container id="inner" width="100px" height="50px" fill="#00FF0080"
                 mask-image="soft-mask"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("image_mask.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 800, 600, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_PaintChrome(doc, &be);

	CHECK(st.imageMaskBegins == 1);
	CHECK(st.imageMaskEnds == 1);
	CHECK(st.imageMaskDepth == 0);
	CHECK(!st.imageMaskLog.empty());
	CHECK(st.imageMaskLog.front().find("ui/modern/textures/panel.png") != std::string::npos);
	CHECK(st.imageMaskLog.front().find("mask ") == 0);
	bool sawFill = false;
	for (const std::string &s : st.drawLog) {
		if (s.find("rect") != std::string::npos || s.find("path") != std::string::npos) {
			sawFill = true;
			break;
		}
	}
	CHECK(sawFill);
	/* Nested begin failed: only one begin/end pair. */
	int maskEnds = 0;
	for (const std::string &s : st.imageMaskLog) {
		if (s == "mask-end") {
			maskEnds++;
		}
	}
	CHECK(maskEnds == 1);

	UID_DestroyDocument(doc);
}

void TestImageMaskUnknownRejected(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <images>
      <image id="panel" src="ui/modern/textures/panel.png"/>
    </images>
  </definitions>
  <canvas>
    <container id="c" width="64px" height="64px" mask-image="missing-mask" fill="#FFFFFFFF"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);

	CHECK(UID_ParseXml("image_mask_unknown.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) != UID_OK);
	bool saw = false;
	for (const auto &d : diags.Items()) {
		if (d.message.find("unknown mask-image") != std::string::npos) {
			saw = true;
		}
	}
	CHECK(saw);
	UID_DestroyDocument(doc);
}

void TestImageMaskNullHooksSafe(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <images>
      <image id="soft-mask" src="ui/modern/textures/panel.png"/>
    </images>
  </definitions>
  <canvas>
    <container id="c" width="64px" height="64px" mask-image="soft-mask" fill="#FF0000FF"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);
	be.beginImageMask = nullptr;
	be.endImageMask = nullptr;

	CHECK(UID_ParseXml("image_mask_null.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 800, 600, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_PaintChrome(doc, &be);
	CHECK(st.imageMaskBegins == 0);
	UID_DestroyDocument(doc);
}

/* Added in OPM: mask-image accepts linear/radial gradient brushes. */
void TestImageMaskGradientBrush(void)
{
	static const char *kDoc = R"XML(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"/>
  </definitions>
  <canvas>
    <container id="c" width="120px" height="60px" fill="#FF0000FF"
               mask-image="linear(90deg, #FFFFFFFF, #FFFFFF00)"/>
  </canvas>
</ui>
)XML";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("image_mask_grad.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_LayoutDocument(doc, 800, 600, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_PaintChrome(doc, &be);

	CHECK(st.imageMaskBegins == 1);
	CHECK(st.imageMaskEnds == 1);
	CHECK(!st.imageMaskLog.empty());
	CHECK(st.imageMaskLog.front().find("linear(90deg") != std::string::npos);

	UID_DestroyDocument(doc);
}

void TestImageMaskRadialBrushCompile(void)
{
	static const char *kDoc = R"XML(
<ui version="1">
  <definitions/>
  <canvas>
    <container id="c" width="64px" height="64px"
               mask-image="radial(50% 50%, #FFFFFFFF 0%, #FFFFFF00 100%)"
               fill="#00FF00FF"/>
  </canvas>
</ui>
)XML";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);

	CHECK(UID_ParseXml("image_mask_radial.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	UID_DestroyDocument(doc);
}

void TestAutoFillAutoRow(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="auto" height="auto"/></definitions>
  <canvas>
    <container id="row" type="horizontal" width="100%" height="40px" gap="0">
      <container id="left" width="50px" height="auto" fill="#FF0000FF"/>
      <container id="center" width="fill" height="auto" fill="#00FF00FF"/>
      <container id="right" width="50px" height="auto" fill="#0000FFFF"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("auto_fill_auto.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_rect_t &left = doc->states[static_cast<size_t>(doc->idIndex.at("left"))].borderBox;
	const uid_rect_t &center = doc->states[static_cast<size_t>(doc->idIndex.at("center"))].borderBox;
	const uid_rect_t &right = doc->states[static_cast<size_t>(doc->idIndex.at("right"))].borderBox;

	CHECK_EQ_F(left.x, 0.0f, 0.5f);
	CHECK_EQ_F(left.w, 50.0f, 0.5f);
	CHECK_EQ_F(center.x, 50.0f, 0.5f);
	CHECK_EQ_F(center.w, 300.0f, 0.5f);
	CHECK_EQ_F(right.x, 350.0f, 0.5f);
	CHECK_EQ_F(right.w, 50.0f, 0.5f);

	UID_DestroyDocument(doc);
}

void TestAutoWrapperWithInternalFill(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="auto" height="auto"/></definitions>
  <canvas>
    <container id="row" type="horizontal" width="100%" height="40px" gap="0">
      <container id="left" width="40px" height="40px"/>
      <container id="inner" type="horizontal" width="auto" height="40px" gap="0">
        <container id="a" width="40px" height="40px"/>
        <container id="fill" width="fill" height="40px"/>
        <container id="b" width="40px" height="40px"/>
      </container>
      <container id="right" width="40px" height="40px"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("auto_wrapper_fill.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_rect_t &inner = doc->states[static_cast<size_t>(doc->idIndex.at("inner"))].borderBox;
	const uid_rect_t &fill = doc->states[static_cast<size_t>(doc->idIndex.at("fill"))].borderBox;
	const uid_rect_t &left = doc->states[static_cast<size_t>(doc->idIndex.at("left"))].borderBox;
	const uid_rect_t &right = doc->states[static_cast<size_t>(doc->idIndex.at("right"))].borderBox;

	CHECK(inner.w > 200.0f);
	CHECK(fill.w > 0.0f);
	CHECK(left.x + left.w <= inner.x + 0.5f);
	CHECK(inner.x + inner.w <= right.x + 0.5f);

	UID_DestroyDocument(doc);
}

void TestAutoParentExpandsForFill(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="auto" height="auto"/></definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <container id="parent" type="horizontal" width="auto" height="40px" gap="0">
        <container id="a" width="40px" height="40px"/>
        <container id="fill" width="fill" height="40px"/>
        <container id="b" width="40px" height="40px"/>
      </container>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("auto_parent_fill.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 500, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_rect_t &root = doc->states[static_cast<size_t>(doc->idIndex.at("root"))].contentBox;
	const uid_rect_t &parent = doc->states[static_cast<size_t>(doc->idIndex.at("parent"))].borderBox;
	const uid_rect_t &fill = doc->states[static_cast<size_t>(doc->idIndex.at("fill"))].borderBox;

	CHECK_EQ_F(parent.w, root.w, 0.5f);
	CHECK(fill.w > 0.0f);

	UID_DestroyDocument(doc);
}

/* Fixed in OPM: horizontal height=auto + width=fill children must not steal vertical fill. */
void TestHorizontalAutoHeightNotPromotedInVerticalParent(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="auto" height="auto"/></definitions>
  <canvas>
    <container id="panel" type="vertical" width="100%" height="100%" gap="0">
      <container id="tabs" width="100%" height="48px"/>
      <container id="pages" type="vertical" width="100%" height="fill"/>
      <container id="actions" type="horizontal" width="100%" height="auto"
                 padding="12px 16px" gap="4px">
        <container id="btn" width="80px" height="40px"/>
        <container id="search_wrap" type="horizontal" width="fill" height="auto" padding="1px">
          <container id="search" width="fill" height="40px"/>
        </container>
        <container id="apply" width="80px" height="40px"/>
      </container>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("settings_actions_height.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) ==
	      UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 800, 600, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_rect_t &actions = doc->states[static_cast<size_t>(doc->idIndex.at("actions"))].borderBox;
	const uid_rect_t &pages = doc->states[static_cast<size_t>(doc->idIndex.at("pages"))].borderBox;
	const uid_rect_t &search = doc->states[static_cast<size_t>(doc->idIndex.at("search"))].borderBox;

	CHECK(actions.h < 100.0f);
	CHECK_EQ_F(actions.h, 66.0f, 1.0f);
	CHECK(pages.h > 400.0f);
	CHECK(search.w > 100.0f);

	UID_DestroyDocument(doc);
}

void TestForeachDefaultWidthAutoInHorizontalPanel(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="auto" height="auto"/></definitions>
  <canvas>
    <container id="panel" type="horizontal" width="auto" height="40px" gap="4px">
      <foreach id="clips" count="{cvar.ui_test_count}" type="vertical" gap="0">
        <container width="32px" height="32px"/>
      </foreach>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_test_count"] = FakeCvar{"1", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("foreach_auto_panel.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	uid_node_id_t foreachId = UID_INVALID_NODE_ID;
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		if (doc->nodes[i].kind == UID_NODE_FOREACH && doc->nodes[i].id == "clips") {
			foreachId = static_cast<uid_node_id_t>(i);
			break;
		}
	}
	CHECK(foreachId >= 0);
	const uid_node_def_t *fn = UID_GetNode(doc, foreachId);
	CHECK(fn != nullptr);
	CHECK(fn->foreachTemplateRoot >= 0);
	CHECK(fn->foreachTemplateNodes[static_cast<size_t>(fn->foreachTemplateRoot)].properties.GetCStr("width", "") &&
	      std::strcmp(
	          fn->foreachTemplateNodes[static_cast<size_t>(fn->foreachTemplateRoot)].properties.GetCStr("width", ""),
	          "auto"
	      ) == 0);

	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 2560, 1440, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_rect_t &panel = doc->states[static_cast<size_t>(doc->idIndex.at("panel"))].borderBox;
	CHECK(panel.w < 200.0f);
	CHECK(panel.w >= 32.0f);

	UID_DestroyDocument(doc);
}

void TestAmmoPanelBottomRowLayoutMaxClip32(void)
{
	g_testImportFiles.clear();
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <import src="ui/modern/lib/hud_ammo.xml"/>
    <defaults type="vertical" width="auto" height="auto"/>
  </definitions>
  <canvas>
    <container id="root" type="overlap" width="100%" height="100%">
      <container id="hud_bottom_row" type="horizontal" width="100%" height="auto" gap="0" padding="0">
        <container id="hud_bottom_left" width="200px" height="200px"/>
        <container id="hud_bottom_center" type="vertical" width="fill" height="auto"/>
        <container id="hud_bottom_end" type="vertical" width="auto" height="auto" padding="0 16px 0 0">
          <use template="classic-ammo-panel"/>
        </container>
      </container>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	FakeBackendState st;
	st.cvars["ui_om_hud_active_weapon"] = FakeCvar{"BAR", 0};
	st.cvars["ui_om_hud_max_clip"] = FakeCvar{"32", 0};
	st.cvars["ui_om_hud_clip"] = FakeCvar{"32", 0};
	st.cvars["ui_om_hud_ammo"] = FakeCvar{"200", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("ammo_bottom_row.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 2560, 1440, 1.0f, 1.0f, &be, &diags) == UID_OK);

	auto centerIt = doc->idIndex.find("hud_bottom_center");
	auto endIt = doc->idIndex.find("hud_bottom_end");
	CHECK(centerIt != doc->idIndex.end());
	CHECK(endIt != doc->idIndex.end());
	const uid_node_state_t *centerSt = &doc->states[static_cast<size_t>(centerIt->second)];
	const uid_node_state_t *endSt = &doc->states[static_cast<size_t>(endIt->second)];

	CHECK(centerSt->borderBox.w > 2200.0f);
	CHECK(endSt->borderBox.w < 800.0f);
	CHECK(endSt->borderBox.x > 2300.0f);

	UID_DestroyDocument(doc);
}

void TestEdgeClipShapePaint(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <shapes>
      <shape id="edge-clip">
        <props>
          <prop name="left" type="number" default="0"/>
          <prop name="top" type="number" default="0"/>
          <prop name="right" type="number" default="1"/>
          <prop name="bottom" type="number" default="1"/>
        </props>
        <path fill="none"
          d="M {parent.width * shape.left} {parent.height * shape.top}
             L {parent.width * shape.right} {parent.height * shape.top}
             L {parent.width * shape.right} {parent.height * shape.bottom}
             L {parent.width * shape.left} {parent.height * shape.bottom}
             Z"/>
      </shape>
    </shapes>
    <images>
      <image id="tile" src="textures/hud/clip_rifle"/>
    </images>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <container id="bar" width="64px" height="128px" background-image="tile"
                 background-fit="stretch" shape="edge-clip" left="0" top="0.5" right="1" bottom="1"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	UIR_CompositorReset();
	g_edgeClipScissorOk = false;
	{
		uir_viewport_t       vp;
		uir_draw2d_backend_t d2d;
		uir_image_backend_t  image;
		std::memset(&d2d, 0, sizeof(d2d));
		std::memset(&image, 0, sizeof(image));
		UIR_ViewportMake(0, 0, 640, 480, &vp);
		UIR_BeginOverlayFrame(&vp, 0);
		d2d.scissor = edge_clip_test_scissor;
		UIR_Draw2D_SetBackend(&d2d);
		image.registerShaderNoMip = edge_clip_register_shader;
		image.getShaderSize = edge_clip_shader_size;
		image.setColor = edge_clip_set_color;
		image.drawStretchPic = edge_clip_draw_stretch;
		UIR_ImageSetBackend(&image);
	}
	be.drawImage = fake_drawImageUir;

	CHECK(UID_ParseXml("edge_clip.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 640, 480, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_PaintChrome(doc, &be);
	CHECK(st.imageDrawLog.size() >= 1);
	const std::string &img = st.imageDrawLog.back();
	CHECK(img.find("clips=1") != std::string::npos);
	CHECK(img.find("xywh=0.0,0.0,64.0,128.0") != std::string::npos);
	CHECK(img.find("64") != std::string::npos);
	CHECK(g_edgeClipScissorOk);
	UIR_ImageShutdown();
	UID_DestroyDocument(doc);
}

void TestCvarBoundPropertySync(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="100%" height="100%"/></definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <container id="n" width="64px" height="64px" top="{cvar.ui_test_top}"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_test_top"] = FakeCvar{"0.25", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("cvar_sync.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	uid_node_def_t *node = UID_GetNodeById(doc, "n");
	CHECK(node != nullptr);

	UID_SyncBindings(doc, &be);
	std::string topVal;
	CHECK(node->properties.Get("top", &topVal));
	CHECK(topVal == "0.25");

	st.cvars["ui_test_top"] = FakeCvar{"0.75", 0};
	UID_SyncBindings(doc, &be);
	CHECK(node->properties.Get("top", &topVal));
	CHECK(topVal == "0.75");

	st.cvars.erase("ui_test_top");
	UID_SyncBindings(doc, &be);
	CHECK(node->properties.Get("top", &topVal));
	CHECK(topVal == "{cvar.ui_test_top}");
	UID_DestroyDocument(doc);
}

void TestOpacityInheritancePaint(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <images><image id="tile" src="textures/hud/clip_rifle"/></images>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%" opacity="0.5">
      <container id="child" width="64px" height="64px" opacity="0.5" background-image="tile"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("opacity.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 640, 480, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_PaintChrome(doc, &be);
	CHECK(st.imageDrawLog.size() >= 1);
	CHECK(st.imageDrawLog.back().find("tint_a=0.25") != std::string::npos);
	UID_DestroyDocument(doc);
}

static void AssertClassicHudAnchors(uid_document_t *doc, int w, int h)
{
	bool compassOk = false;
	bool healthOk = false;
	bool killFeedOk = false;
	bool weaponsBarOk = false;

	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		const uid_node_def_t &n = doc->nodes[i];
		const uid_rect_t &box = doc->states[i].borderBox;
		std::string bg;
		if (n.properties.Get("background-image", &bg) && bg == "hud-healthmeter") {
			if (box.w >= 15.0f && box.h >= 63.0f) {
				healthOk = true;
				CHECK_EQ_F(box.w, 16.0, 1.0);
				CHECK_EQ_F(box.h, 64.0, 1.0);
			}
		}
		if (box.w >= 127.0f && box.w <= 129.0f && box.h >= 127.0f && box.h <= 129.0f) {
			std::string bg;
			if (n.properties.Get("background-image", &bg) && bg == "hud-compass-back") {
				compassOk = true;
			}
		}
		if (n.id == "hud_kill_feed_slot") {
			killFeedOk = true;
			CHECK_EQ_F(box.w, 378.0, 1.0);
			CHECK_EQ_F(box.h, 120.0, 1.0);
		}
		std::string src;
		if (n.collectionSource == "hud-messages" || (n.properties.Get("source", &src) && src == "hud-messages")) {
			if (box.w >= 377.0f && box.w <= 379.0f && box.h >= 119.0f && box.h <= 121.0f) {
				killFeedOk = true;
			}
		}
	}
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		const uid_node_def_t &n = doc->nodes[i];
		std::string widthStr;
		std::string heightStr;
		if (n.properties.Get("width", &widthStr) && widthStr == "384px" &&
			n.properties.Get("height", &heightStr) && heightStr == "64px") {
			const uid_rect_t &box = doc->states[i].borderBox;
			if (box.w >= 383.0f && box.w <= 385.0f && box.h >= 63.0f && box.h <= 65.0f) {
				weaponsBarOk = true;
			}
		}
	}
	CHECK(compassOk);
	CHECK(healthOk);
	CHECK(killFeedOk);
	(void)w;
	(void)h;
	(void)weaponsBarOk;
}

void TestClassicHudLayoutGeometry(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/huds/classic.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	FakeBackendState st;
	st.cvars["ui_om_hud_show"] = FakeCvar{"1", 0};
	st.cvars["ui_om_hud_health_top"] = FakeCvar{"0.5", 0};
	st.cvars["ui_om_hud_clip_top"] = FakeCvar{"0.5", 0};
	st.cvars["ui_compass"] = FakeCvar{"1", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("ui/modern/huds/classic.xml", xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);

	for (const uid_node_def_t &n : doc->nodes) {
		for (const auto &kv : n.properties.Attrs()) {
			CHECK(kv.first.find("aspect-ratio") == std::string::npos);
			CHECK(kv.second.value.find("aspect-ratio") == std::string::npos);
		}
		const char *role = n.properties.GetCStr("role", nullptr);
		if (role && role[0]) {
			CHECK(std::strcmp(role, "server-list") == 0);
		}
	}

	const int sizes[][2] = {{640, 480}, {1280, 720}, {2560, 1080}};
	for (const auto &szPair : sizes) {
		UID_SyncBindings(doc, &be);
		CHECK(UID_LayoutDocument(doc, szPair[0], szPair[1], 1.0f, 1.0f, &be, &diags) == UID_OK);
		AssertClassicHudAnchors(doc, szPair[0], szPair[1]);
	}
	UID_DestroyDocument(doc);
}

void TestMessageCollectionPaint(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <templates>
      <template id="row">
        <label font="body" font-size="14px" color="{item.field.color}">
          {item.field.text}
        </label>
      </template>
    </templates>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <container id="feed" type="vertical" width="378px" height="120px" source="hud-messages">
        <foreach mode="all"><use template="row"/></foreach>
      </container>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("msg.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 640, 480, 1.0f, 1.0f, &be, &diags) == UID_OK);
	int foreachRows = 0;
	for (const uid_node_def_t &n : doc->nodes) {
		if (n.foreachGenerated && n.foreachScopeId >= 0) {
			foreachRows++;
		}
	}
	CHECK(foreachRows >= 5);
	UID_PaintChrome(doc, &be);
	UID_DestroyDocument(doc);
}

static int CountForeachWrapChildren(const uid_document_t *doc, uid_node_id_t foreachId)
{
	if (!doc || foreachId < 0 || static_cast<size_t>(foreachId) >= doc->nodes.size()) {
		return 0;
	}
	return static_cast<int>(doc->nodes[static_cast<size_t>(foreachId)].children.size());
}

static uid_node_id_t FindFirstForeach(const uid_document_t *doc)
{
	if (!doc) {
		return UID_INVALID_NODE_ID;
	}
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		if (doc->nodes[i].kind == UID_NODE_FOREACH) {
			return static_cast<uid_node_id_t>(i);
		}
	}
	return UID_INVALID_NODE_ID;
}

void TestForeachLifetimeFade(void)
{
	/* Parse: fade-duration alone is rejected; lifetime alone is OK. */
	{
		static const char *kBad = R"(
<ui version="1"><definitions/><canvas>
  <container source="lifetime-feed"><foreach mode="all" fade-duration="1s"><label>x</label></foreach></container>
</canvas></ui>
)";
		uid_limits_t lim;
		UID_DefaultLimits(&lim);
		uid_document_t *doc = UID_CreateDocument();
		uid_diag_list_t diags(lim.maxDiagnostics);
		CHECK(UID_ParseXml("bad_fade.xml", kBad, std::strlen(kBad), &lim, nullptr, doc, &diags) != UID_OK);
		CHECK(diags.HasErrors());
		UID_DestroyDocument(doc);
	}
	{
		static const char *kOk = R"(
<ui version="1"><definitions/><canvas>
  <container source="lifetime-feed"><foreach mode="all" lifetime="6s"><label>x</label></foreach></container>
</canvas></ui>
)";
		uid_limits_t lim;
		UID_DefaultLimits(&lim);
		uid_document_t *doc = UID_CreateDocument();
		uid_diag_list_t diags(lim.maxDiagnostics);
		CHECK(UID_ParseXml("ok_life.xml", kOk, std::strlen(kOk), &lim, nullptr, doc, &diags) == UID_OK);
		uid_node_id_t fid = FindFirstForeach(doc);
		CHECK(fid != UID_INVALID_NODE_ID);
		CHECK(doc->nodes[static_cast<size_t>(fid)].hasForeachLifetime);
		CHECK(doc->nodes[static_cast<size_t>(fid)].foreachLifetimeMs == 6000);
		CHECK(doc->nodes[static_cast<size_t>(fid)].foreachFadeDurationMs == 0);
		UID_DestroyDocument(doc);
	}

	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <container id="feed" type="vertical" width="200px" height="120px" source="hud-messages">
        <foreach mode="all" lifetime="1000ms" fade-duration="400ms">
          <label font="body" font-size="14px">{item.field.text}</label>
        </foreach>
      </container>
      <container id="plain" type="vertical" width="200px" height="120px" source="hud-messages">
        <foreach mode="all">
          <label font="body" font-size="14px">{item.field.text}</label>
        </foreach>
      </container>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);
	st.lifetimeFeedActive = true;
	st.lifetimeItems.push_back({"msg_1", "one"});
	st.lifetimeItems.push_back({"msg_2", "two"});

	CHECK(UID_ParseXml("life.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);

	uid_node_id_t lifeForeach = UID_INVALID_NODE_ID;
	uid_node_id_t plainForeach = UID_INVALID_NODE_ID;
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		if (doc->nodes[i].kind != UID_NODE_FOREACH) {
			continue;
		}
		if (doc->nodes[i].hasForeachLifetime) {
			lifeForeach = static_cast<uid_node_id_t>(i);
		} else {
			plainForeach = static_cast<uid_node_id_t>(i);
		}
	}
	CHECK(lifeForeach != UID_INVALID_NODE_ID);
	CHECK(plainForeach != UID_INVALID_NODE_ID);

	doc->updateTimeMs = 1000;
	UID_SyncBindings(doc, &be);
	CHECK(CountForeachWrapChildren(doc, lifeForeach) == 2);
	CHECK(CountForeachWrapChildren(doc, plainForeach) == 2);
	{
		const uid_node_id_t wrap = doc->nodes[static_cast<size_t>(lifeForeach)].children[0];
		CHECK_EQ_F(doc->states[static_cast<size_t>(wrap)].lifetimeOpacityMul, 1.0, 1e-4);
	}
	const uint64_t solidSig = doc->states[static_cast<size_t>(lifeForeach)].foreachExpandSig;

	/* Same-key field refresh must not rebuild during solid phase. */
	st.lifetimeItems[0].text = "one-updated";
	st.lifetimeRevision++;
	doc->updateTimeMs = 1200;
	UID_SyncBindings(doc, &be);
	CHECK(doc->states[static_cast<size_t>(lifeForeach)].foreachExpandSig == solidSig);
	CHECK(CountForeachWrapChildren(doc, lifeForeach) == 2);

	/* Fade window: opacity declines, no rebuild. */
	doc->updateTimeMs = 1000 + 700; /* age 700; fade starts at 600 */
	UID_SyncBindings(doc, &be);
	CHECK(doc->states[static_cast<size_t>(lifeForeach)].foreachExpandSig == solidSig);
	CHECK(CountForeachWrapChildren(doc, lifeForeach) == 2);
	{
		const uid_node_id_t wrap = doc->nodes[static_cast<size_t>(lifeForeach)].children[0];
		const float alpha = doc->states[static_cast<size_t>(wrap)].lifetimeOpacityMul;
		CHECK(alpha < 0.999f && alpha > 0.001f);
	}

	/* Past lifetime: rows drop from lifetime foreach only. */
	doc->updateTimeMs = 1000 + 1000;
	UID_SyncBindings(doc, &be);
	lifeForeach = FindFirstForeach(doc);
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		if (doc->nodes[i].kind == UID_NODE_FOREACH && doc->nodes[i].hasForeachLifetime) {
			lifeForeach = static_cast<uid_node_id_t>(i);
		}
		if (doc->nodes[i].kind == UID_NODE_FOREACH && !doc->nodes[i].hasForeachLifetime) {
			plainForeach = static_cast<uid_node_id_t>(i);
		}
	}
	CHECK(CountForeachWrapChildren(doc, lifeForeach) == 0);
	CHECK(CountForeachWrapChildren(doc, plainForeach) == 2);

	/* No-lifetime: host remove drops immediately. */
	st.lifetimeItems.clear();
	st.lifetimeItems.push_back({"msg_2", "two"});
	st.lifetimeRevision++;
	doc->updateTimeMs = 1000 + 1000;
	UID_SyncBindings(doc, &be);
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		if (doc->nodes[i].kind == UID_NODE_FOREACH && !doc->nodes[i].hasForeachLifetime) {
			plainForeach = static_cast<uid_node_id_t>(i);
		}
	}
	CHECK(CountForeachWrapChildren(doc, plainForeach) == 1);

	UID_DestroyDocument(doc);
}

void TestParseMinimal(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("minimal.xml", kMinimal, std::strlen(kMinimal), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	UID_DestroyDocument(doc);
}

void TestRejectDoctype(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("bad.xml", kBadDoctype, std::strlen(kBadDoctype), &lim, nullptr, doc, &diags) != UID_OK);
	CHECK(diags.HasErrors());
	UID_DestroyDocument(doc);
}

const char *kCanvasPointerDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas pointer="{cvar.ui_om_scoreboard_disable_cursor != 1 and (cvar.ui_om_intermission == 1 or cvar.ui_om_spectator == 1 or cvar.ui_om_scoreboard_cursor == 1)}">
    <container id="root" width="100%" height="100%"/>
  </canvas>
</ui>
)";

const char *kCanvasPointerTrueDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas pointer="true">
    <container id="root" width="100%" height="100%"/>
  </canvas>
</ui>
)";

const char *kCanvasUnknownAttrDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas foo="bar">
    <container id="root" width="100%" height="100%"/>
  </canvas>
</ui>
)";

/* Added in OPM: canvas pointer="{bool expr}" menu cursor ownership. */
void TestCanvasPointerAttr(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	{
		uid_document_t *doc = UID_CreateDocument();
		uid_diag_list_t diags(lim.maxDiagnostics);
		CHECK(UID_ParseXml("pointer.xml", kCanvasPointerDoc, std::strlen(kCanvasPointerDoc), &lim, nullptr, doc, &diags)
		      == UID_OK);
		CHECK(!diags.HasErrors());
		CHECK(doc->pointerExpr
		      == "cvar.ui_om_scoreboard_disable_cursor != 1 and (cvar.ui_om_intermission == 1 or cvar.ui_om_spectator == "
		         "1 or cvar.ui_om_scoreboard_cursor == 1)");
		UID_DestroyDocument(doc);
	}
	{
		uid_document_t *doc = UID_CreateDocument();
		uid_diag_list_t diags(lim.maxDiagnostics);
		CHECK(UID_ParseXml("pointer_true.xml", kCanvasPointerTrueDoc, std::strlen(kCanvasPointerTrueDoc), &lim, nullptr, doc,
		                   &diags)
		      == UID_OK);
		CHECK(!diags.HasErrors());
		CHECK(doc->pointerExpr == "true");
		UID_DestroyDocument(doc);
	}
	{
		uid_document_t *doc = UID_CreateDocument();
		uid_diag_list_t diags(lim.maxDiagnostics);
		CHECK(UID_ParseXml("pointer_bad.xml", kCanvasUnknownAttrDoc, std::strlen(kCanvasUnknownAttrDoc), &lim, nullptr, doc,
		                   &diags)
		      != UID_OK);
		CHECK(diags.HasErrors());
		UID_DestroyDocument(doc);
	}
}

const char *kButtonChamferPropDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"/>
    <shapes>
      <shape id="skew-rect">
        <props>
          <prop name="skewl" type="length" default="12px"/>
          <prop name="skewr" type="length" default="12px"/>
        </props>
        <path fill="{parent.fill}"
              d="M 0 {parent.height} L {shape.skewl} 0 L {parent.width} 0
                 L {parent.width - shape.skewr} {parent.height} Z"/>
      </shape>
    </shapes>
  </definitions>
  <canvas>
    <button id="btn" width="120px" height="40px" shape="skew-rect" skewr="24px" skewl="0"
            fill="#1A6FD4FF">REFRESH</button>
  </canvas>
</ui>
)";

void TestButtonShapeDeclaredProp(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("skew.xml", kButtonChamferPropDoc, std::strlen(kButtonChamferPropDoc), &lim, nullptr, doc, &diags) ==
	      UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	uid_node_def_t *btn = UID_GetNodeById(doc, "btn");
	CHECK(btn != nullptr);
	if (btn) {
		const char *sr = btn->properties.GetCStr("skewr", nullptr);
		CHECK(sr != nullptr && std::strcmp(sr, "24px") == 0);
		const char *sl = btn->properties.GetCStr("skewl", nullptr);
		CHECK(sl != nullptr && std::strcmp(sl, "0") == 0);
	}
	UID_DestroyDocument(doc);
}

void TestTemplateExpand(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("tpl.xml", kTemplateDoc, std::strlen(kTemplateDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_GetNodeById(doc, "u1") != nullptr);
	uid_node_def_t *lbl = UID_GetNodeById(doc, "u1.lbl");
	CHECK(lbl != nullptr);
	if (lbl) {
		CHECK(lbl->text == "Hello");
		CHECK(lbl->text.find("{template.") == std::string::npos);
	}
	uid_node_def_t *tog = UID_GetNodeById(doc, "u1.tog");
	CHECK(tog != nullptr);
	if (tog) {
		CHECK(tog->bind == "cvar:cg_fov");
		CHECK(tog->bind.find("{template.") == std::string::npos);
	}
	UID_DestroyDocument(doc);
}

void TestKeybindTemplateExpand(void)
{
	static const char kDoc[] = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <templates>
      <template id="kbrow">
        <props>
          <prop name="binding" type="string" required="true"/>
        </props>
        <keybind id="kb" binding="{template.binding}"/>
      </template>
    </templates>
  </definitions>
  <canvas>
    <use id="u1" template="kbrow" binding="+forward"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("kb.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	uid_node_def_t *kb = UID_GetNodeById(doc, "u1.kb");
	CHECK(kb != nullptr);
	if (kb) {
		CHECK(kb->binding == "+forward");
		CHECK(kb->binding.find("{template.") == std::string::npos);
	}
	UID_DestroyDocument(doc);
}

void TestKeybindConflictModal(void)
{
	static const char kDoc[] = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <modals>
      <modal id="overwrite">
        <container id="modal_root" type="vertical" width="100%" height="100%" halign="center" valign="center">
          <button id="modal_yes" modal-role="confirm">YES</button>
        </container>
      </modal>
    </modals>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <keybind id="kb_fwd" binding="+forward" confirm-modal="overwrite"/>
      <keybind id="kb_back" binding="+back"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);
	st.cvars["ui_om_modal"] = FakeCvar{"", 0};
	st.cvars["ui_modal_message"] = FakeCvar{"", 0};
	st.cvars["ui_modal_bind_command"] = FakeCvar{"", 0};
	st.cvars["ui_modal_bind_key"] = FakeCvar{"", 0};
	st.cvars["ui_modal_bind_slot"] = FakeCvar{"", 0};
	st.bindings[119] = "+back";
	CHECK(UID_ParseXml("kbconf.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	uid_node_id_t fwdId = -1;
	for (const auto &kv : doc->idIndex) {
		if (kv.first == "kb_fwd") {
			fwdId = kv.second;
		}
	}
	CHECK(fwdId >= 0);
	if (fwdId >= 0) {
		doc->states[static_cast<size_t>(fwdId)].capturing = true;
		CHECK(UID_TryCommitKeybindCapture(doc, fwdId, 119, &be) == UID_OK);
		CHECK(st.cvars["ui_om_modal"].value == "overwrite");
		CHECK(st.cvars["ui_modal_bind_command"].value == "+forward");
		CHECK(st.cvars["ui_modal_bind_key"].value == "119");
		UID_SyncBindings(doc, &be);
		CHECK(UID_IsModalActive(doc));
		CHECK(UID_GetNodeById(doc, "modal_yes") != nullptr);
		UID_LayoutDocument(doc, 800, 600, 1.0f, 1.0f, &be, &diags);
		const uid_node_def_t *yes = UID_GetNodeById(doc, "modal_yes");
		if (yes) {
			uid_node_id_t yesId = -1;
			for (const auto &kv : doc->idIndex) {
				if (kv.first == "modal_yes") {
					yesId = kv.second;
				}
			}
			if (yesId >= 0) {
				const uid_node_state_t &yst = doc->states[static_cast<size_t>(yesId)];
				CHECK(yst.borderBox.w > 0.0f);
				CHECK(yst.borderBox.x > 100.0f);
			}
		}
	}
	UID_DestroyDocument(doc);
}

void TestWriteAllBindingsPreservesKeybinds(void)
{
	static const char kDoc[] = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="100%" height="100%"/></definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <keybind id="kb_fwd" binding="+forward" slot="primary"/>
      <keybind id="kb_fwd_sec" binding="+forward" slot="secondary"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.bindings[119] = "+forward";
	uid_backend_t be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("flushkb.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(st.bindings.count(119) > 0);
	CHECK(UID_WriteAllBindings(doc, &be) == UID_OK);
	CHECK(st.bindings.count(119) > 0);
	CHECK(st.bindings[119] == "+forward");
	UID_DestroyDocument(doc);
}

void TestKeybindLowercaseAndDisplay(void)
{
	static const char kDoc[] = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="100%" height="100%"/></definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <keybind id="kb_fwd" binding="+forward" slot="primary"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("kblower.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);

	uid_node_id_t fwdId = doc->idIndex["kb_fwd"];
	CHECK(fwdId >= 0);

	/* Capture lowercase 'w' (119); must store on 119, never 'W' (87). */
	CHECK(UID_TryCommitKeybindCapture(doc, fwdId, 'w', &be) == UID_OK);
	CHECK(st.bindings.count('w') > 0);
	CHECK(st.bindings['w'] == "+forward");
	CHECK(st.bindings.count('W') == 0);

	UID_SyncBindings(doc, &be);
	{
		uid_node_state_t *stNode = &doc->states[static_cast<size_t>(fwdId)];
		CHECK(stNode->runtimeValue.hasValue);
		CHECK(stNode->runtimeValue.stringValue == "W");
	}

	/* Display label "W" must not rewrite the bind when WriteBinding is called. */
	CHECK(UID_WriteBinding(doc, fwdId, &be) == UID_OK);
	CHECK(st.bindings.count('w') > 0);
	CHECK(st.bindings['w'] == "+forward");
	CHECK(st.bindings.count('W') == 0);

	/* Uppercase capture keynum is normalized to lowercase. */
	st.bindings.clear();
	CHECK(UID_TryCommitKeybindCapture(doc, fwdId, 'W', &be) == UID_OK);
	CHECK(st.bindings.count('w') > 0);
	CHECK(st.bindings['w'] == "+forward");
	CHECK(st.bindings.count('W') == 0);

	UID_DestroyDocument(doc);
}

void TestKeybindMigrateUppercase(void)
{
	static const char kDoc[] = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="100%" height="100%"/></definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <keybind id="kb_fwd" binding="+forward" slot="primary"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	/* Corrupted state: command bound to uppercase W. */
	st.bindings['W'] = "+forward";
	uid_backend_t be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("kbmig.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(st.bindings.count('w') > 0);
	CHECK(st.bindings['w'] == "+forward");
	CHECK(st.bindings.count('W') == 0);
	UID_DestroyDocument(doc);
}

void TestKeybindCaptureMouseWheelKeys(void)
{
	static const char kDoc[] = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="100%" height="100%"/></definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <keybind id="kb_atk" binding="+attackprimary" slot="primary"/>
    </container>
  </canvas>
</ui>
)";
	/* Engine keycodes: K_MOUSE1..5 then K_MWHEELDOWN/UP — use high nums like production. */
	const int kMouse1 = 178;
	const int kMWheelUp = 184;
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("kbmouse.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	uid_node_id_t atkId = doc->idIndex["kb_atk"];
	CHECK(atkId >= 0);
	CHECK(UID_TryCommitKeybindCapture(doc, atkId, kMouse1, &be) == UID_OK);
	CHECK(st.bindings.count(kMouse1) > 0);
	CHECK(st.bindings[kMouse1] == "+attackprimary");
	st.bindings.clear();
	CHECK(UID_TryCommitKeybindCapture(doc, atkId, kMWheelUp, &be) == UID_OK);
	CHECK(st.bindings.count(kMWheelUp) > 0);
	CHECK(st.bindings[kMWheelUp] == "+attackprimary");
	UID_DestroyDocument(doc);
}

void TestModalCvarDispatch(void)
{
	static const char kDoc[] = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <modals>
      <modal id="test_modal">
        <container id="modal_root" type="vertical" width="100%" height="100%">
          <label id="modal_msg" width="100%" text-cvar="ui_modal_message"/>
        </container>
      </modal>
    </modals>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);
	st.cvars["ui_om_modal"] = FakeCvar{"", 0};
	st.cvars["ui_modal_message"] = FakeCvar{"hello", 0};
	CHECK(UID_ParseXml("modal.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	st.cvars["ui_om_modal"].value = "test_modal";
	UID_SyncBindings(doc, &be);
	CHECK(UID_IsModalActive(doc));
	CHECK(UID_GetNodeById(doc, "modal_msg") != nullptr);
	st.cvars["ui_om_modal"].value = "";
	UID_SyncBindings(doc, &be);
	CHECK(!UID_IsModalActive(doc));
	UID_DestroyDocument(doc);
}

void TestLayoutFill(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("lay.xml", kLayoutDoc, std::strlen(kLayoutDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);
	uid_node_def_t *a = UID_GetNodeById(doc, "a");
	uid_node_def_t *b = UID_GetNodeById(doc, "b");
	uid_node_def_t *c = UID_GetNodeById(doc, "c");
	CHECK(a && b && c);
	if (a && b && c) {
		uid_node_id_t aid = -1, bid = -1, cid = -1;
		for (auto &kv : doc->idIndex) {
			if (kv.first == "a") {
				aid = kv.second;
			}
			if (kv.first == "b") {
				bid = kv.second;
			}
			if (kv.first == "c") {
				cid = kv.second;
			}
		}
		CHECK(aid >= 0 && bid >= 0 && cid >= 0);
		if (aid >= 0 && bid >= 0 && cid >= 0) {
			/* Golden: padding 10 → content 180; gaps 20; fixed 100 → fill 60. */
			CHECK_EQ_F(doc->states[(size_t)aid].borderBox.w, 50.0, 0.5);
			CHECK_EQ_F(doc->states[(size_t)cid].borderBox.w, 50.0, 0.5);
			CHECK_EQ_F(doc->states[(size_t)bid].borderBox.w, 60.0, 1.0);
			CHECK_EQ_F(doc->states[(size_t)aid].borderBox.h, 20.0, 0.5);
			CHECK(doc->states[(size_t)aid].borderBox.x < doc->states[(size_t)bid].borderBox.x);
			CHECK(doc->states[(size_t)bid].borderBox.x < doc->states[(size_t)cid].borderBox.x);
		}
	}
	UID_DestroyDocument(doc);
}

/*
 * Nested H/V containers, gap, and cross/main alignment geometry.
 * These are the primitives the full menu depends on.
 */
const char *kAlignLabDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"
              font="body" font-size="14px" color="#FFFFFFFF"/>
    <fonts>
      <font id="body" src="fonts/Oswald-Medium.ttf" weight="500"/>
    </fonts>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%" padding="10px" gap="10px">
      <container id="row_center" type="horizontal" width="100%" height="40px" gap="8px"
                 halign="start" valign="center" fill="#111111FF">
        <container id="rc_a" width="20px" height="20px" fill="#FF0000FF"/>
        <container id="rc_b" width="20px" height="10px" fill="#00FF00FF"/>
      </container>
      <container id="col_center" type="vertical" width="100px" height="100px" gap="4px"
                 halign="center" valign="start" fill="#222222FF">
        <container id="cc_a" width="40px" height="20px" fill="#0000FFFF"/>
        <container id="cc_b" width="20px" height="20px" fill="#FFFF00FF"/>
      </container>
      <container id="row_end" type="horizontal" width="200px" height="30px" gap="0"
                 halign="end" valign="end" fill="#333333FF">
        <container id="re_a" width="30px" height="10px" fill="#FF00FFFF"/>
      </container>
      <container id="row_even" type="horizontal" width="200px" height="20px" gap="0"
                 halign="equal-spacing" valign="start" fill="#444444FF">
        <container id="re2_a" width="20px" height="20px" fill="#AAAAAAAA"/>
        <container id="re2_b" width="20px" height="20px" fill="#BBBBBBBB"/>
      </container>
      <button id="btn_center" width="120px" height="40px" font-size="16px" fill="#1A6FD4FF">Join</button>
      <label id="lbl_start" width="100px" height="30px" font-size="14px">Name</label>
    </container>
  </canvas>
</ui>
)";

uid_node_id_t NodeId(uid_document_t *doc, const char *id)
{
	auto it = doc->idIndex.find(id);
	return it != doc->idIndex.end() ? it->second : -1;
}

void TestNestedAlignAndGap(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("align.xml", kAlignLabDoc, std::strlen(kAlignLabDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 400, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t rowCenter = NodeId(doc, "row_center");
	uid_node_id_t rcA = NodeId(doc, "rc_a");
	uid_node_id_t rcB = NodeId(doc, "rc_b");
	uid_node_id_t colCenter = NodeId(doc, "col_center");
	uid_node_id_t ccA = NodeId(doc, "cc_a");
	uid_node_id_t ccB = NodeId(doc, "cc_b");
	uid_node_id_t rowEnd = NodeId(doc, "row_end");
	uid_node_id_t reA = NodeId(doc, "re_a");
	uid_node_id_t rowEven = NodeId(doc, "row_even");
	uid_node_id_t re2A = NodeId(doc, "re2_a");
	uid_node_id_t re2B = NodeId(doc, "re2_b");
	CHECK(rowCenter >= 0 && rcA >= 0 && rcB >= 0);
	CHECK(colCenter >= 0 && ccA >= 0 && ccB >= 0);
	CHECK(rowEnd >= 0 && reA >= 0 && rowEven >= 0 && re2A >= 0 && re2B >= 0);

	if (rowCenter >= 0 && rcA >= 0 && rcB >= 0) {
		const uid_rect_t &row = doc->states[(size_t)rowCenter].contentBox;
		const uid_rect_t &a = doc->states[(size_t)rcA].borderBox;
		const uid_rect_t &b = doc->states[(size_t)rcB].borderBox;
		/* valign=center in a 40px-tall row */
		CHECK_EQ_F(a.y, row.y + (row.h - 20.0) * 0.5, 0.6);
		CHECK_EQ_F(b.y, row.y + (row.h - 10.0) * 0.5, 0.6);
		/* gap 8 between a and b */
		CHECK_EQ_F(b.x, a.x + a.w + 8.0, 0.6);
	}

	if (colCenter >= 0 && ccA >= 0 && ccB >= 0) {
		const uid_rect_t &col = doc->states[(size_t)colCenter].contentBox;
		const uid_rect_t &a = doc->states[(size_t)ccA].borderBox;
		const uid_rect_t &b = doc->states[(size_t)ccB].borderBox;
		/* halign=center in a 100px-wide column */
		CHECK_EQ_F(a.x, col.x + (col.w - 40.0) * 0.5, 0.6);
		CHECK_EQ_F(b.x, col.x + (col.w - 20.0) * 0.5, 0.6);
		CHECK_EQ_F(b.y, a.y + a.h + 4.0, 0.6);
	}

	if (rowEnd >= 0 && reA >= 0) {
		const uid_rect_t &row = doc->states[(size_t)rowEnd].contentBox;
		const uid_rect_t &a = doc->states[(size_t)reA].borderBox;
		CHECK_EQ_F(a.x, row.x + row.w - 30.0, 0.6);
		CHECK_EQ_F(a.y, row.y + row.h - 10.0, 0.6);
	}

	if (rowEven >= 0 && re2A >= 0 && re2B >= 0) {
		const uid_rect_t &row = doc->states[(size_t)rowEven].contentBox;
		const uid_rect_t &a = doc->states[(size_t)re2A].borderBox;
		const uid_rect_t &b = doc->states[(size_t)re2B].borderBox;
		/* space-evenly: free=160 over 3 slots → 53.333… */
		const double slot = (200.0 - 40.0) / 3.0;
		CHECK_EQ_F(a.x, row.x + slot, 1.0);
		CHECK_EQ_F(b.x, a.x + a.w + slot, 1.0);
	}

	UID_DestroyDocument(doc);
}

const char *kSpaceBetweenDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"/>
  </definitions>
  <canvas>
    <container id="row_sb" type="horizontal" width="300px" height="40px" gap="0"
               halign="space-between" valign="center">
      <container id="sb_a" width="40px" height="20px" fill="#FF0000FF"/>
      <container id="sb_b" width="40px" height="20px" fill="#00FF00FF"/>
      <container id="sb_c" width="40px" height="20px" fill="#0000FFFF"/>
    </container>
  </canvas>
</ui>
)";

void TestSpaceBetween(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("sb.xml", kSpaceBetweenDoc, std::strlen(kSpaceBetweenDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t row = NodeId(doc, "row_sb");
	uid_node_id_t a = NodeId(doc, "sb_a");
	uid_node_id_t b = NodeId(doc, "sb_b");
	uid_node_id_t c = NodeId(doc, "sb_c");
	CHECK(row >= 0 && a >= 0 && b >= 0 && c >= 0);
	if (row >= 0 && a >= 0 && b >= 0 && c >= 0) {
		const uid_rect_t &r = doc->states[(size_t)row].contentBox;
		const uid_rect_t &ra = doc->states[(size_t)a].borderBox;
		const uid_rect_t &rb = doc->states[(size_t)b].borderBox;
		const uid_rect_t &rc = doc->states[(size_t)c].borderBox;
		/* free = 300 - 120 = 180 over 2 between-slots → 90 */
		const double slot = (300.0 - 120.0) / 2.0;
		CHECK_EQ_F(ra.x, r.x, 0.75);
		CHECK_EQ_F(rb.x, ra.x + ra.w + slot, 0.75);
		CHECK_EQ_F(rc.x, rb.x + rb.w + slot, 0.75);
		CHECK_EQ_F(rc.x + rc.w, r.x + r.w, 0.75);
	}
	UID_DestroyDocument(doc);
}

void TestButtonTextCentered(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("align.xml", kAlignLabDoc, std::strlen(kAlignLabDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 400, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t btn = NodeId(doc, "btn_center");
	uid_node_id_t lbl = NodeId(doc, "lbl_start");
	CHECK(btn >= 0 && lbl >= 0);
	if (btn < 0 || lbl < 0) {
		UID_DestroyDocument(doc);
		return;
	}

	st.fontDrawLog.clear();
	UID_PaintChrome(doc, &be);

	const uid_rect_t &bb = doc->states[(size_t)btn].contentBox;
	const uid_rect_t &lb = doc->states[(size_t)lbl].contentBox;
	/* "Join" = 4 chars * 8px = 32; font-size 16 → cap-centric center in 120x40 */
	const double expectBtnX = bb.x + (bb.w - 32.0) * 0.5;
	const double expectBtnY = bb.y + bb.h * 0.5 - 16.0 * 0.62;
	bool foundBtn = false;
	bool foundLbl = false;
	for (const std::string &line : st.fontDrawLog) {
		float x = 0.0f, y = 0.0f;
		char  text[64];
		if (std::sscanf(line.c_str(), "text %f,%f '%63[^']'", &x, &y, text) == 3) {
			if (std::strcmp(text, "Join") == 0) {
				foundBtn = true;
				CHECK_EQ_F(x, expectBtnX, 0.75);
				CHECK_EQ_F(y, expectBtnY, 0.75);
			}
			if (std::strcmp(text, "Name") == 0) {
				foundLbl = true;
				/* label: start + vertical center, font-size 14 */
				CHECK_EQ_F(x, lb.x, 0.75);
				CHECK_EQ_F(y, lb.y + lb.h * 0.5 - 14.0 * 0.62, 0.75);
			}
		}
	}
	CHECK(foundBtn);
	CHECK(foundLbl);
	UID_DestroyDocument(doc);
}

/* Document <defaults halign/valign=start> must not pin nested button text top-left. */
const char *kNestedTextAlignDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"
              font="body" font-size="14px" color="#FFFFFFFF"
              halign="start" valign="start"/>
    <fonts>
      <font id="body" src="fonts/Oswald-Medium.ttf" weight="500"/>
    </fonts>
  </definitions>
  <canvas>
    <container type="vertical" width="100%" height="100%" gap="8px" padding="0">
      <container id="header" type="horizontal" width="400px" height="52px" gap="0" padding="0">
        <container id="nav" type="horizontal" width="auto" height="100%" gap="0" padding="0 0 0 12px">
          <button id="play" width="120px" height="100%" padding="0 20px" font-size="22px">PLAY</button>
        </container>
      </container>
      <container id="actions" type="horizontal" width="400px" height="auto" padding="12px 16px" gap="4px"
                 valign="center">
        <button id="refresh" width="100px" height="40px" padding="0 18px" font-size="16px">REFRESH</button>
      </container>
      <container id="explicit" type="horizontal" width="400px" height="52px">
        <button id="play_start" width="120px" height="100%" padding="0 20px" font-size="22px"
                valign="start" halign="center">TOP</button>
      </container>
    </container>
  </canvas>
</ui>
)";

void TestNestedButtonTextCenter(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("nest.xml", kNestedTextAlignDoc, std::strlen(kNestedTextAlignDoc), &lim, nullptr, doc, &diags) ==
	      UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 800, 400, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t play = NodeId(doc, "play");
	uid_node_id_t refresh = NodeId(doc, "refresh");
	uid_node_id_t playStart = NodeId(doc, "play_start");
	CHECK(play >= 0 && refresh >= 0 && playStart >= 0);

	/* Hostile defaults must not leave valign=start on buttons. */
	if (play >= 0) {
		const char *v = doc->nodes[(size_t)play].properties.GetCStr("valign", nullptr);
		CHECK(v == nullptr);
	}

	st.fontDrawLog.clear();
	UID_PaintChrome(doc, &be);

	auto findText = [&](const char *want, float *ox, float *oy) -> bool {
		for (const std::string &line : st.fontDrawLog) {
			float x = 0.0f, y = 0.0f;
			char  text[64];
			if (std::sscanf(line.c_str(), "text %f,%f '%63[^']'", &x, &y, text) == 3 &&
			    std::strcmp(text, want) == 0) {
				*ox = x;
				*oy = y;
				return true;
			}
		}
		return false;
	};

	if (play >= 0) {
		const uid_rect_t &bb = doc->states[(size_t)play].contentBox;
		CHECK(bb.h > 40.0); /* height=100% of 52px header, not intrinsic text */
		float x = 0.0f, y = 0.0f;
		CHECK(findText("PLAY", &x, &y));
		const double expectY = bb.y + bb.h * 0.5 - 22.0 * 0.62;
		CHECK_EQ_F(y, expectY, 1.0);
		const double mid = bb.y + bb.h * 0.5;
		/* Glyph top should be near mid (not flush to content top). */
		CHECK(y > bb.y + 4.0);
		CHECK(std::fabs((y + 22.0 * 0.62) - mid) < 1.5);
	}

	if (refresh >= 0) {
		const uid_rect_t &bb = doc->states[(size_t)refresh].contentBox;
		float x = 0.0f, y = 0.0f;
		CHECK(findText("REFRESH", &x, &y));
		CHECK_EQ_F(y, bb.y + bb.h * 0.5 - 16.0 * 0.62, 1.0);
	}

	if (playStart >= 0) {
		const uid_rect_t &bb = doc->states[(size_t)playStart].contentBox;
		float x = 0.0f, y = 0.0f;
		CHECK(findText("TOP", &x, &y));
		CHECK_EQ_F(y, bb.y, 0.75); /* explicit start */
	}

	UID_DestroyDocument(doc);
}

const char *kPaddingFillDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%" padding="20px" gap="0">
      <container id="row" type="horizontal" width="100%" height="60px" gap="10px" padding="10px" fill="#111111FF">
        <container id="f1" width="fill" height="100%" fill="#FF0000FF"/>
        <container id="f2" width="fill" height="100%" fill="#00FF00FF"/>
        <container id="f3" width="fill" height="100%" fill="#0000FFFF"/>
      </container>
      <container id="mrow" type="horizontal" width="300px" height="40px" gap="0">
        <container id="m1" width="40px" height="100%" fill="#FF0000FF"/>
        <container id="m2" width="40px" height="100%" margin="0 20px" fill="#00FF00FF"/>
        <container id="m3" width="40px" height="100%" fill="#0000FFFF"/>
      </container>
    </container>
  </canvas>
</ui>
)";

void TestPaddingFillAndMargin(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("pad.xml", kPaddingFillDoc, std::strlen(kPaddingFillDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t root = NodeId(doc, "root");
	uid_node_id_t row = NodeId(doc, "row");
	uid_node_id_t f1 = NodeId(doc, "f1");
	uid_node_id_t f2 = NodeId(doc, "f2");
	uid_node_id_t f3 = NodeId(doc, "f3");
	uid_node_id_t m1 = NodeId(doc, "m1");
	uid_node_id_t m2 = NodeId(doc, "m2");
	uid_node_id_t m3 = NodeId(doc, "m3");
	CHECK(root >= 0 && row >= 0 && f1 >= 0 && f2 >= 0 && f3 >= 0);
	CHECK(m1 >= 0 && m2 >= 0 && m3 >= 0);

	if (root >= 0 && row >= 0) {
		const uid_rect_t &r = doc->states[(size_t)root].borderBox;
		const uid_rect_t &c = doc->states[(size_t)root].contentBox;
		CHECK_EQ_F(r.x, 0.0, 0.1);
		CHECK_EQ_F(c.x, 20.0, 0.1);
		CHECK_EQ_F(c.y, 20.0, 0.1);
		CHECK_EQ_F(c.w, 360.0, 0.1);
		const uid_rect_t &rowB = doc->states[(size_t)row].borderBox;
		CHECK_EQ_F(rowB.x, 20.0, 0.1);
		CHECK_EQ_F(rowB.y, 20.0, 0.1);
		CHECK_EQ_F(rowB.w, 360.0, 0.1);
	}

	if (f1 >= 0 && f2 >= 0 && f3 >= 0 && row >= 0) {
		const uid_rect_t &rc = doc->states[(size_t)row].contentBox;
		const uid_rect_t &a = doc->states[(size_t)f1].borderBox;
		const uid_rect_t &b = doc->states[(size_t)f2].borderBox;
		const uid_rect_t &c = doc->states[(size_t)f3].borderBox;
		/* content 360-20pad=340? row w=360, pad 10 → content 340; gaps 20; fill each (340-20)/3=106.666 */
		CHECK_EQ_F(rc.w, 340.0, 0.5);
		CHECK_EQ_F(a.w, 106.666, 1.0);
		CHECK_EQ_F(b.w, 106.666, 1.0);
		CHECK_EQ_F(c.w, 106.666, 1.0);
		CHECK_EQ_F(a.h, rc.h, 0.5);
		CHECK_EQ_F(b.x, a.x + a.w + 10.0, 0.75);
		CHECK_EQ_F(c.x, b.x + b.w + 10.0, 0.75);
	}

	if (m1 >= 0 && m2 >= 0 && m3 >= 0) {
		const uid_rect_t &a = doc->states[(size_t)m1].borderBox;
		const uid_rect_t &b = doc->states[(size_t)m2].borderBox;
		const uid_rect_t &c = doc->states[(size_t)m3].borderBox;
		/* m2 has margin 0 20 → 20px each side */
		CHECK_EQ_F(b.x, a.x + a.w + 20.0, 0.75);
		CHECK_EQ_F(c.x, b.x + b.w + 20.0, 0.75);
	}

	UID_DestroyDocument(doc);
}


void TestMainXmlLoads(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/main.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	uid_result_t status = UID_ParseXml(
		"ui/modern/main.xml",
		xml.c_str(),
		xml.size(),
		&lim,
		&parseIo,
		doc,
		&diags
	);
	if (diags.HasErrors()) {
		for (const auto &d : diags.Items()) {
			std::fprintf(
				stderr,
				"parse: %s:%d: %s\n",
				d.location.path ? d.location.path : "?",
				d.location.line,
				d.message.c_str()
			);
		}
	}
	CHECK(status == UID_OK);
	CHECK(!diags.HasErrors());
	status = UID_ExpandDocument(doc, &diags);
	if (diags.HasErrors()) {
		for (const auto &d : diags.Items()) {
			std::fprintf(
				stderr,
				"expand: %s:%d: %s\n",
				d.location.path ? d.location.path : "?",
				d.location.line,
				d.message.c_str()
			);
		}
	}
	CHECK(status == UID_OK);
	CHECK(!diags.HasErrors());
	status = UID_CompileDocument(doc, &diags);
	if (diags.HasErrors()) {
		for (const auto &d : diags.Items()) {
			std::fprintf(
				stderr,
				"compile: %s:%d: %s\n",
				d.location.path ? d.location.path : "?",
				d.location.line,
				d.message.c_str()
			);
		}
	}
	CHECK(status == UID_OK);
	CHECK(!diags.HasErrors());

	/* Added in OPM: player-model selects must wire modal= to relative modal defs. */
	CHECK(doc->definitions.modals.count("player-model-allies") == 1);
	CHECK(doc->definitions.modals.count("player-model-axis") == 1);
	CHECK(doc->definitions.modals["player-model-allies"].type == "relative");
	uid_node_id_t alliesSel = doc->idIndex.count("select_allies_model") ? doc->idIndex["select_allies_model"] : -1;
	uid_node_id_t axisSel = doc->idIndex.count("select_axis_model") ? doc->idIndex["select_axis_model"] : -1;
	CHECK(alliesSel >= 0);
	CHECK(axisSel >= 0);
	if (alliesSel >= 0) {
		CHECK(doc->nodes[(size_t)alliesSel].openModal == "player-model-allies");
	}
	if (axisSel >= 0) {
		CHECK(doc->nodes[(size_t)axisSel].openModal == "player-model-axis");
	}

	UID_DestroyDocument(doc);
}

/* Added in OPM: click-release on select modal= must mount relative modal with items. */
void TestSelectModalClickOpen(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <sources>
      <source id="models" default="a">
        <item value="a" label="Alpha"/>
        <item value="b" label="Beta"/>
        <item value="c" label="Charlie"/>
      </source>
    </sources>
    <modals>
      <modal id="model-pop" type="relative">
        <container type="overlap" width="100%" height="100%" fill="#00000000">
          <button id="dismiss" width="100%" height="100%" fill="#00000000">
            <on event="click"><hide-modal/></on>
          </button>
          <container id="panel" role="relative-panel" type="vertical" width="100%" height="auto"
                     overflow="scroll" source="models" bind="cvar:ui_model" commit="change"
                     fill="#101010FF">
            <foreach mode="all" type="vertical" width="100%" height="auto" gap="0">
              <button width="100%" height="28px" set-index="{item.index}">
                {item.display}
                <on event="click"><hide-modal/></on>
              </button>
            </foreach>
          </container>
        </container>
      </modal>
    </modals>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="400px" height="200px" valign="start" padding="20px">
      <select id="mode" width="160px" height="28px" source="models" modal="model-pop"
              bind="cvar:ui_model" commit="change" fill="#202020FF"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_om_modal"] = FakeCvar{"", 0};
	st.cvars["ui_model"] = FakeCvar{"a", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("select_modal.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t sid = NodeId(doc, "mode");
	CHECK(sid >= 0);
	if (sid < 0) {
		UID_DestroyDocument(doc);
		return;
	}
	CHECK(doc->nodes[(size_t)sid].openModal == "model-pop");
	const uid_node_state_t &sst = doc->states[(size_t)sid];
	CHECK(sst.borderBox.w > 1.0f);
	CHECK(sst.borderBox.h > 1.0f);
	CHECK(sst.effectivelyEnabled);

	const uid_node_id_t hit = UID_HitTest(doc, sst.borderBox.x + sst.borderBox.w * 0.5f,
										  sst.borderBox.y + sst.borderBox.h * 0.5f, true);
	CHECK(hit == sid);

	uid_pointer_state_t ptr{};
	ptr.x = sst.borderBox.x + sst.borderBox.w * 0.5f;
	ptr.y = sst.borderBox.y + sst.borderBox.h * 0.5f;
	ptr.buttons = 0;
	UID_HandlePointer(doc, &ptr, 0, &be);
	ptr.buttons = UID_POINTER_BUTTON_LEFT;
	UID_HandlePointer(doc, &ptr, 1, &be);
	ptr.buttons = 0;
	UID_HandlePointer(doc, &ptr, 2, &be);

	CHECK(st.cvars["ui_om_modal"].value == "model-pop");
	CHECK(doc->modalOpenerNode == sid);

	UID_SyncBindings(doc, &be);
	CHECK(UID_IsModalActive(doc));
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t panelId = NodeId(doc, "panel");
	CHECK(panelId >= 0);
	if (panelId >= 0) {
		const uid_node_state_t &panel = doc->states[(size_t)panelId];
		CHECK(panel.borderBox.w > 1.0f);
		CHECK(panel.borderBox.h > 20.0f);
		CHECK(doc->nodes[(size_t)panelId].collectionSource == "models");
		CHECK(doc->states[(size_t)panelId].collectionItemCount == 3);
		bool hasRows = false;
		for (uid_node_id_t c : doc->nodes[(size_t)panelId].children) {
			if (c >= 0 && static_cast<size_t>(c) < doc->nodes.size() &&
				doc->nodes[(size_t)c].kind == UID_NODE_FOREACH &&
				!doc->nodes[(size_t)c].children.empty()) {
				hasRows = true;
				break;
			}
		}
		CHECK(hasRows);
	}

	UID_DestroyDocument(doc);
}

/* Added in OPM: full main.xml Allies select click mounts player-model-allies. */
void TestMainSelectModalClick(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/main.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_om_modal"] = FakeCvar{"", 0};
	st.cvars["ui_om_main_panel"] = FakeCvar{"play", 0};
	st.cvars["ui_dm_playermodel_set"] = FakeCvar{"american_army", 0};
	st.cvars["ui_dm_playergermanmodel_set"] = FakeCvar{"german_wehrmacht_soldier", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("ui/modern/main.xml", xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 1280, 720, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t sid = NodeId(doc, "select_allies_model");
	CHECK(sid >= 0);
	if (sid < 0) {
		UID_DestroyDocument(doc);
		return;
	}
	const uid_node_state_t &sst = doc->states[(size_t)sid];
	CHECK(sst.borderBox.w > 1.0f);
	CHECK(sst.borderBox.h > 1.0f);
	CHECK(sst.effectivelyEnabled);

	const float cx = sst.borderBox.x + sst.borderBox.w * 0.5f;
	const float cy = sst.borderBox.y + sst.borderBox.h * 0.5f;
	const uid_node_id_t hit = UID_HitTest(doc, cx, cy, true);
	CHECK(hit == sid);

	uid_pointer_state_t ptr{};
	ptr.x = cx;
	ptr.y = cy;
	ptr.buttons = 0;
	UID_HandlePointer(doc, &ptr, 0, &be);
	ptr.buttons = UID_POINTER_BUTTON_LEFT;
	UID_HandlePointer(doc, &ptr, 1, &be);
	ptr.buttons = 0;
	UID_HandlePointer(doc, &ptr, 2, &be);
	CHECK(st.cvars["ui_om_modal"].value == "player-model-allies");

	UID_SyncBindings(doc, &be);
	CHECK(UID_IsModalActive(doc));
	CHECK(UID_LayoutDocument(doc, 1280, 720, 1.0f, 1.0f, &be, &diags) == UID_OK);

	/* Find relative-panel by role (no stable id in production assets). */
	uid_node_id_t panelId = UID_INVALID_NODE_ID;
	const uid_node_id_t modalRoot = UID_GetModalRoot(doc);
	if (modalRoot != UID_INVALID_NODE_ID) {
		std::vector<uid_node_id_t> stack = {modalRoot};
		while (!stack.empty()) {
			uid_node_id_t id = stack.back();
			stack.pop_back();
			const uid_node_def_t *n = UID_GetNode(doc, id);
			if (!n) {
				continue;
			}
			if (n->role == "relative-panel") {
				panelId = id;
				break;
			}
			for (uid_node_id_t c : n->children) {
				stack.push_back(c);
			}
		}
	}
	CHECK(panelId >= 0);
	if (panelId >= 0) {
		CHECK(doc->states[(size_t)panelId].collectionItemCount > 0);
		CHECK(doc->states[(size_t)panelId].borderBox.h > 20.0f);
		bool hasRows = false;
		for (uid_node_id_t c : doc->nodes[(size_t)panelId].children) {
			if (c >= 0 && static_cast<size_t>(c) < doc->nodes.size() &&
				doc->nodes[(size_t)c].kind == UID_NODE_FOREACH &&
				doc->nodes[(size_t)c].children.size() > 0) {
				hasRows = true;
				break;
			}
		}
		CHECK(hasRows);
	}

	UID_DestroyDocument(doc);
}

void TestOverlayPaint(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	st.cvars["ui_om_modal"] = FakeCvar{"", 0};
	CHECK(UID_ParseXml("ov.xml", kOverlayDoc, std::strlen(kOverlayDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);
	uid_node_id_t sid = doc->idIndex.count("mode") ? doc->idIndex["mode"] : -1;
	CHECK(sid >= 0);
	if (sid >= 0) {
		/* Relative modal paints in overlay (above 3D model previews), not chrome. */
		st.drawLog.clear();
		UID_PaintOverlay(doc, &be);
		CHECK(st.drawLog.empty());

		doc->modalOpenerNode = sid;
		st.cvars["ui_om_modal"].value = "mode-list";
		UID_SyncBindings(doc, &be);
		CHECK(UID_IsModalActive(doc));
		CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);
		st.drawLog.clear();
		UID_PaintOverlay(doc, &be);
		CHECK(!st.drawLog.empty());
		CHECK(UID_GetNodeById(doc, "panel") != nullptr);
	}
	UID_DestroyDocument(doc);
}

/* Added in OPM: appearance=cyclic steps/wraps; Enter does not open overlay. */
void TestCyclicSelect(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["r_fullscreen"] = FakeCvar{"1", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("cyclic.xml", kCyclicDoc, std::strlen(kCyclicDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 280, 80, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t sid = doc->idIndex.count("mode") ? doc->idIndex["mode"] : -1;
	CHECK(sid >= 0);
	if (sid < 0) {
		UID_DestroyDocument(doc);
		return;
	}
	uid_node_def_t *node = UID_GetNode(doc, sid);
	CHECK(node != nullptr);
	CHECK(node->appearance == "cyclic");
	CHECK(node->options.size() == 3);

	UID_SyncBindings(doc, &be);
	uid_node_state_t &nst = doc->states[(size_t)sid];
	nst.highlightIndex = 0;
	CHECK(nst.runtimeValue.hasValue);
	CHECK(nst.runtimeValue.stringValue == "1");

	UID_SetFocus(doc, sid, &be);
	CHECK(UID_HandleKey(doc, UID_KEY_ENTER, true, 0, &be));
	CHECK(!nst.overlayOpen);

	CHECK(UID_HandleKey(doc, UID_KEY_LEFTARROW, true, 0, &be));
	CHECK(nst.highlightIndex == 2);
	CHECK(nst.runtimeValue.stringValue == "0");
	CHECK(st.cvars["r_fullscreen"].value == "0");
	CHECK(!nst.overlayOpen);

	CHECK(UID_HandleKey(doc, UID_KEY_RIGHTARROW, true, 0, &be));
	CHECK(nst.highlightIndex == 0);
	CHECK(nst.runtimeValue.stringValue == "1");

	CHECK(UID_HandleKey(doc, UID_KEY_RIGHTARROW, true, 0, &be));
	CHECK(nst.highlightIndex == 1);
	CHECK(UID_NodeDisplayText(doc, sid) == "Borderless");

	/* Click left chevron column → previous option (Borderless → Fullscreen). */
	uid_pointer_state_t ptr{};
	ptr.x = nst.borderBox.x + 8.0f;
	ptr.y = nst.borderBox.y + nst.borderBox.h * 0.5f;
	ptr.buttons = 0;
	UID_HandlePointer(doc, &ptr, 0, &be);
	ptr.buttons = UID_POINTER_BUTTON_LEFT;
	UID_HandlePointer(doc, &ptr, 0, &be);
	ptr.buttons = 0;
	UID_HandlePointer(doc, &ptr, 0, &be);
	CHECK(nst.highlightIndex == 0);
	CHECK(nst.runtimeValue.stringValue == "1");
	CHECK(!nst.overlayOpen);

	st.drawLog.clear();
	UID_PaintChrome(doc, &be);
	CHECK(!st.drawLog.empty());
	nst.overlayOpen = true;
	st.drawLog.clear();
	UID_PaintOverlay(doc, &be);
	CHECK(st.drawLog.empty());

	UID_DestroyDocument(doc);

	/* Unknown appearance rejected. */
	const char *badApp = R"UID(
<ui version="1">
  <definitions><defaults type="vertical" width="100%" height="100%"/></definitions>
  <canvas>
    <select appearance="wheel" width="100px" height="24px">
      <option value="a">A</option>
    </select>
  </canvas>
</ui>
)UID";
	doc = UID_CreateDocument();
	uid_diag_list_t badDiags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("bad_app.xml", badApp, std::strlen(badApp), &lim, nullptr, doc, &badDiags) != UID_OK ||
		  badDiags.HasErrors());
	UID_DestroyDocument(doc);
}

/* Added in OPM: cyclic select filled from source= + queryOptions. */
void TestCyclicSelectSource(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="body" src="fonts/x.ttf" weight="400"/></fonts>
  </definitions>
  <canvas>
    <select id="mode" appearance="cyclic" width="240px" height="auto"
            bind="cvar:r_fullscreen" source="display-mode" value-type="display-mode"
            commit="change" fill="#080A0CB8" color="#FFFFFFFF"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["r_fullscreen"] = FakeCvar{"1", 0};
	st.cvars["r_noborder"] = FakeCvar{"0", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("src.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 280, 80, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t sid = doc->idIndex.count("mode") ? doc->idIndex["mode"] : -1;
	CHECK(sid >= 0);
	UID_SyncBindings(doc, &be);
	uid_node_def_t *node = UID_GetNode(doc, sid);
	CHECK(node != nullptr);
	CHECK(node->options.size() == 3);
	CHECK(doc->states[(size_t)sid].runtimeValue.stringValue == "1");

	/* Step to Borderless (value 2) → fullscreen=1 noborder=1. */
	UID_SetFocus(doc, sid, &be);
	CHECK(UID_HandleKey(doc, UID_KEY_RIGHTARROW, true, 0, &be));
	CHECK(doc->states[(size_t)sid].runtimeValue.stringValue == "2");
	CHECK(st.cvars["r_fullscreen"].value == "1");
	CHECK(st.cvars["r_noborder"].value == "1");

	UID_DestroyDocument(doc);
}

/* Added in OPM: Off/On + Sensitivity Mode two-button groups. */
void TestSettingsOnOffButtons(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="body" src="fonts/x.ttf" weight="400"/><font id="control" src="fonts/x.ttf" weight="600"/></fonts>
  </definitions>
  <canvas>
    <container type="vertical" width="100%" height="100%" gap="8px">
      <button id="off" bind="cvar:m_filter" set-value="0" commit="change"
              fill="{bind.selected ? #1A6FD4FF : #00000073}" pressed-fill="#0F5FC4FF" color="#FFFFFFFF">Off</button>
      <button id="on" bind="cvar:m_filter" set-value="1" commit="change"
              fill="{bind.selected ? #1A6FD4FF : #00000073}" pressed-fill="#0F5FC4FF" color="#FFFFFFFF">On</button>
      <button id="sens" bind="cvar:ui_modernsettings_sensitivity_mode" set-value="sensitivity" commit="change"
              fill="{bind.selected ? #1A6FD4FF : #00000073}" pressed-fill="#0F5FC4FF" color="#FFFFFFFF">Sensitivity</button>
      <button id="cm" bind="cvar:ui_modernsettings_sensitivity_mode" set-value="cm360" commit="change"
              fill="{bind.selected ? #1A6FD4FF : #00000073}" pressed-fill="#0F5FC4FF" color="#FFFFFFFF">cm/360</button>
      <container id="dpi_row" type="horizontal" width="100%" height="40px"
                 visible="{cvar.ui_modernsettings_sensitivity_mode == cm360}">
        <label>DPI</label>
      </container>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["m_filter"] = FakeCvar{"0", 0};
	st.cvars["ui_modernsettings_sensitivity_mode"] = FakeCvar{"sensitivity", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("onoff.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	uid_node_id_t onId = doc->idIndex["on"];
	uid_node_id_t sensId = doc->idIndex["sens"];
	uid_node_id_t cmId = doc->idIndex["cm"];
	uid_node_id_t dpiId = doc->idIndex["dpi_row"];

	CHECK(UID_NodeDisplayText(doc, sensId) == "Sensitivity");
	CHECK(UID_NodeDisplayText(doc, cmId) == "cm/360");

	CHECK(doc->states[(size_t)onId].runtimeValue.stringValue == "0");
	{
		const char *vis = doc->nodes[(size_t)dpiId].properties.GetCStr("visible", "true");
		CHECK(vis && (std::strcmp(vis, "false") == 0 || std::strcmp(vis, "0") == 0));
	}

	UID_SetFocus(doc, onId, &be);
	CHECK(UID_HandleKey(doc, UID_KEY_ENTER, true, 0, &be));
	CHECK(st.cvars["m_filter"].value == "1");
	CHECK(doc->states[(size_t)onId].runtimeValue.stringValue == "1");

	uid_color_t fill{};
	CHECK(UID_ResolveFillColor(doc, onId, &fill));
	CHECK(fill.b > 0.5f); /* bind.selected fill blue */

	UID_SetFocus(doc, cmId, &be);
	CHECK(UID_HandleKey(doc, UID_KEY_ENTER, true, 0, &be));
	CHECK(st.cvars["ui_modernsettings_sensitivity_mode"].value == "cm360");
	{
		const char *vis = doc->nodes[(size_t)dpiId].properties.GetCStr("visible", "true");
		CHECK(vis && std::strcmp(vis, "true") == 0);
	}

	UID_DestroyDocument(doc);
}

/* Added in Omaha: bind.selected matches numeric cvar strings; peer click refreshes fill. */
void TestBindSelectedNumericMatch(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="control" src="fonts/x.ttf" weight="600"/></fonts>
  </definitions>
  <canvas>
    <container type="horizontal" width="100%" height="40px" gap="8px">
      <button id="off" bind="cvar:s_doppler" set-value="0" commit="change"
              fill="{bind.selected ? #1A6FD4FF : #00000073}" color="#FFFFFFFF">Off</button>
      <button id="on" bind="cvar:s_doppler" set-value="1" commit="change"
              fill="{bind.selected ? #1A6FD4FF : #00000073}" color="#FFFFFFFF">On</button>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["s_doppler"] = FakeCvar{"1.0", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("bind_selected_num.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 320, 80, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	uid_node_id_t offId = doc->idIndex["off"];
	uid_node_id_t onId = doc->idIndex["on"];
	uid_color_t   fillOn{};
	uid_color_t   fillOff{};
	CHECK(UID_ResolveFillColor(doc, onId, &fillOn));
	CHECK(UID_ResolveFillColor(doc, offId, &fillOff));
	CHECK(fillOn.b > 0.5f);
	CHECK(fillOff.b < 0.5f);

	/* Simulate a stale idle fill + same-value click (cvar already 1.0). */
	doc->nodes[static_cast<size_t>(onId)].properties.Set("fill", "#00000073");
	doc->states[static_cast<size_t>(onId)].styleExprCached = true;
	doc->states[static_cast<size_t>(onId)].styleExprEpoch = 1u;
	doc->states[static_cast<size_t>(offId)].styleExprCached = true;
	doc->states[static_cast<size_t>(offId)].styleExprEpoch = 1u;
	UID_SetFocus(doc, onId, &be);
	CHECK(UID_HandleKey(doc, UID_KEY_ENTER, true, 0, &be));
	CHECK(st.cvars["s_doppler"].value == "1.0" || st.cvars["s_doppler"].value == "1");
	CHECK(UID_ResolveFillColor(doc, onId, &fillOn));
	CHECK(fillOn.b > 0.5f);

	UID_DestroyDocument(doc);
}

/* Added in OPM: use-site visible AND template search `or` must keep parent gate. */
void TestUseVisibleAndSearchOrPrecedence(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="body" src="fonts/x.ttf" weight="400"/></fonts>
    <templates>
      <template id="settings-slider-row">
        <props>
          <prop name="label" type="string" required="true"/>
        </props>
        <container id="row" type="vertical" width="100%" height="40px"
                   visible="{cvar.ui_om_settings_search == '' or icontains('{template.label}', cvar.ui_om_settings_search)}">
          <label>{template.label}</label>
        </container>
      </template>
    </templates>
  </definitions>
  <canvas>
    <use id="cm360" template="settings-slider-row" label="cm/360"
         visible="{cvar.ui_modernsettings_sensitivity_mode == cm360}"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_om_settings_search"] = FakeCvar{"", 0};
	st.cvars["ui_modernsettings_sensitivity_mode"] = FakeCvar{"sensitivity", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("vis_and.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	uid_node_id_t rowId = doc->idIndex["cm360"];
	CHECK(rowId >= 0);
	{
		const std::string &expr = doc->nodes[(size_t)rowId].visibleExpr;
		CHECK(expr.find("(") != std::string::npos);
		CHECK(expr.find(") and (") != std::string::npos);
		const char *vis = doc->nodes[(size_t)rowId].properties.GetCStr("visible", "true");
		CHECK(vis && (std::strcmp(vis, "false") == 0 || std::strcmp(vis, "0") == 0));
	}

	st.cvars["ui_modernsettings_sensitivity_mode"].value = "cm360";
	UID_SyncBindings(doc, &be);
	{
		const char *vis = doc->nodes[(size_t)rowId].properties.GetCStr("visible", "false");
		CHECK(vis && std::strcmp(vis, "true") == 0);
	}

	st.cvars["ui_modernsettings_sensitivity_mode"].value = "sensitivity";
	st.cvars["ui_om_settings_search"].value = "cm";
	UID_SyncBindings(doc, &be);
	{
		const char *vis = doc->nodes[(size_t)rowId].properties.GetCStr("visible", "true");
		CHECK(vis && (std::strcmp(vis, "false") == 0 || std::strcmp(vis, "0") == 0));
	}

	UID_DestroyDocument(doc);
}

/* Added in OPM: percent / invert-mouse / cm360 value-types. */
void TestValueTypeTransforms(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="body" src="fonts/x.ttf" weight="400"/></fonts>
  </definitions>
  <canvas>
    <container type="vertical" width="100%" height="100%">
      <slider id="vol" bind="cvar:s_volume" value-type="percent" min="0" max="100" step="1" commit="change"/>
      <button id="inv" bind="cvar:m_pitch" value-type="invert-mouse" set-value="1" commit="change">On</button>
      <slider id="cm" bind="cvar:sensitivity" value-type="cm360" min="1" max="200" step="0.1" commit="change"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["s_volume"] = FakeCvar{"0.5", 0};
	st.cvars["m_pitch"] = FakeCvar{"0.022", 0};
	st.cvars["sensitivity"] = FakeCvar{"5", 0};
	st.cvars["ui_modernsettings_dpi"] = FakeCvar{"800", 0};
	st.cvars["m_yaw"] = FakeCvar{"0.022", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("vt.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	uid_node_id_t vol = doc->idIndex["vol"];
	uid_node_id_t inv = doc->idIndex["inv"];
	uid_node_id_t cm = doc->idIndex["cm"];
	CHECK(doc->states[(size_t)vol].runtimeValue.stringValue == "50");
	CHECK(doc->states[(size_t)inv].runtimeValue.stringValue == "0");

	doc->states[(size_t)vol].runtimeValue.stringValue = "80";
	CHECK(UID_WriteBinding(doc, vol, &be) == UID_OK);
	CHECK(st.cvars["s_volume"].value == "0.8");

	doc->states[(size_t)inv].runtimeValue.stringValue = "1";
	CHECK(UID_WriteBinding(doc, inv, &be) == UID_OK);
	CHECK(st.cvars["m_pitch"].value[0] == '-');

	/* cm360 round-trip: write UI cm, read back sens. */
	const double cmVal = 360.0 * 2.54 / (800.0 * 5.0 * 0.022);
	CHECK(doc->states[(size_t)cm].runtimeValue.stringValue == "10.4");

	doc->states[(size_t)cm].runtimeValue.stringValue = "10";
	CHECK(UID_WriteBinding(doc, cm, &be) == UID_OK);
	const double sens = std::strtod(st.cvars["sensitivity"].value.c_str(), nullptr);
	const double expect = 360.0 * 2.54 / (800.0 * 10.0 * 0.022);
	CHECK_EQ_F(sens, expect, 1e-4);

	(void)cmVal;
	UID_DestroyDocument(doc);
}

/* Added in OPM: slider/number display precision follows authored step. */
void TestSteppedNumberDisplay(void)
{
	char buf[64];

	CHECK(UID_FormatNumberForStep(1.0, 0.1, 30.0, 0.1, true, true, true, buf, sizeof(buf)));
	CHECK(std::strcmp(buf, "1") == 0);

	CHECK(UID_FormatNumberForStep(1.000000, 0.1, 30.0, 0.1, true, true, true, buf, sizeof(buf)));
	CHECK(std::strcmp(buf, "1") == 0);

	CHECK(UID_FormatNumberForStep(0.022, 0.001, 1.0, 0.001, true, true, true, buf, sizeof(buf)));
	CHECK(std::strcmp(buf, "0.022") == 0);

	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="body" src="fonts/x.ttf" weight="400"/></fonts>
  </definitions>
  <canvas>
    <container type="vertical" width="100%" height="100%">
      <slider id="sens" bind="cvar:sensitivity" min="0.1" max="30" step="0.1" commit="change">
        <track height="100%" fill="#00000099" shape="rectangle"/>
        <range fill="#ffffff54" shape="rectangle"/>
        <thumb width="2px" height="100%" fill="#1A6FD4FF" shape="rectangle"/>
      </slider>
      <input id="sens_num" type="number" bind="cvar:sensitivity" min="0.1" max="30" step="0.1" commit="change"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["sensitivity"] = FakeCvar{"1.000000", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("step.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	uid_node_id_t sens = doc->idIndex["sens"];
	uid_node_id_t sensNum = doc->idIndex["sens_num"];
	CHECK(doc->states[(size_t)sens].runtimeValue.stringValue == "1");
	CHECK(doc->states[(size_t)sensNum].runtimeValue.stringValue == "1");

	UID_DestroyDocument(doc);
}

/* Added in OPM: enabled-if="cvar:name=value" toggles node enabled. */
void TestEnabledIf(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="body" src="fonts/x.ttf" weight="400"/></fonts>
  </definitions>
  <canvas>
    <container type="vertical" width="100%" height="100%">
      <button id="apply" enabled="{cvar.ui_om_settings_tab == video}"
              fill="#1A6FD4FF" disabled-fill="#FFFFFF0F" color="#FFFFFFFF" disabled-color="#EBF0F559">Apply</button>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_om_settings_tab"] = FakeCvar{"mouse", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("enabled.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t applyId = doc->idIndex["apply"];
	const char *enabled = doc->nodes[(size_t)applyId].properties.GetCStr("enabled", "true");
	CHECK(enabled && std::strcmp(enabled, "false") == 0);
	CHECK(!UID_NodeEffectivelyEnabled(doc, applyId));

	st.cvars["ui_om_settings_tab"].value = "video";
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);
	enabled = doc->nodes[(size_t)applyId].properties.GetCStr("enabled", "false");
	CHECK(enabled && std::strcmp(enabled, "true") == 0);
	CHECK(UID_NodeEffectivelyEnabled(doc, applyId));

	UID_DestroyDocument(doc);
}

/* Added in OPM: brace bool expressions on visible/enabled. */
void TestBoolExpr(void)
{
	bool result = false;
	std::string diag;
	uid_expr_limits_t lim;
	UID_DefaultExprLimits(&lim);

	FakeBackendState st;
	st.cvars["ui_om_main_panel"] = FakeCvar{"play", 0};
	st.cvars["ui_om_settings_tab"] = FakeCvar{"video", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_bool_lookup_ctx_t ctx;
	ctx.backend = &be;
	ctx.doc = nullptr;
	ctx.nodeId = UID_INVALID_NODE_ID;
	ctx.item = nullptr;
	ctx.itemIndex = -1;
	ctx.itemCount = 0;
	ctx.selectedIndex = -1;

	CHECK(UID_EvalBool("cvar.ui_om_main_panel == play", &ctx, &lim, &result, &diag) && result);
	CHECK(UID_EvalBool("cvar.ui_om_main_panel == settings", &ctx, &lim, &result, &diag) && !result);
	CHECK(UID_EvalBool("cvar.ui_om_settings_tab == video and cvar.ui_om_main_panel == play", &ctx, &lim, &result, &diag) && result);
	CHECK(UID_EvalBool("cvar.ui_om_settings_tab == video or cvar.ui_om_main_panel == settings", &ctx, &lim, &result, &diag) && result);
	CHECK(UID_EvalBool("!false", &ctx, &lim, &result, &diag) && result);
	/* Changed in OPM: C-style && / || are rejected in favor of and / or. */
	CHECK(!UID_EvalBool("true && false", &ctx, &lim, &result, &diag));
	CHECK(diag.find("and") != std::string::npos || diag.find("or") != std::string::npos);
	CHECK(!UID_EvalBool("true || false", &ctx, &lim, &result, &diag));

	CHECK(UID_EvalBool("icontains('Sensitivity', 'sens')", &ctx, &lim, &result, &diag) && result);
	CHECK(UID_EvalBool("icontains('Sensitivity', 'GAMMA')", &ctx, &lim, &result, &diag) && !result);
	CHECK(UID_EvalBool("icontains('Sensitivity', '')", &ctx, &lim, &result, &diag) && result);
	st.cvars["ui_om_settings_search"] = FakeCvar{"gam", 0};
	CHECK(UID_EvalBool("cvar.ui_om_settings_search == '' or icontains('Gamma', cvar.ui_om_settings_search)", &ctx, &lim, &result, &diag) && result);
	CHECK(UID_EvalBool("cvar.ui_om_settings_search == '' or icontains('Sensitivity', cvar.ui_om_settings_search)", &ctx, &lim, &result, &diag) && !result);

	std::string inner;
	CHECK(UID_ParseBraceBoolExpr("{cvar.ui_om_main_panel == play}", &inner));
	CHECK(inner == "cvar.ui_om_main_panel == play");

	std::string migrated;
	CHECK(UID_VisibleIfToBoolExpr("cvar:ui_om_settings_tab=video", &migrated));
	CHECK(migrated == "cvar.ui_om_settings_tab == video");
	CHECK(UID_VisibleIfToBoolExpr("cvar:ui_om_settings_tab!=video", &migrated));
	CHECK(migrated == "cvar.ui_om_settings_tab != video");

	CHECK(UID_VisibleIfIndexToBoolExpr("selected", &migrated));
	CHECK(migrated == "item.selected");
	CHECK(UID_VisibleIfIndexToBoolExpr("not-last", &migrated));
	CHECK(migrated == "!item.last");

	uid_collection_entry_t item;
	item.value = "a";
	item.label = "A";
	ctx.item = &item;
	ctx.itemIndex = 1;
	ctx.itemCount = 3;
	ctx.selectedIndex = 1;
	CHECK(UID_EvalBool("item.selected", &ctx, &lim, &result, &diag) && result);
	CHECK(UID_EvalBool("!item.last", &ctx, &lim, &result, &diag) && result);
	ctx.itemIndex = 2;
	CHECK(UID_EvalBool("item.last", &ctx, &lim, &result, &diag) && result);
	CHECK(UID_EvalBool("!item.selected", &ctx, &lim, &result, &diag) && result);

	item.fields["lanes"] = "1";
	item.fields["use-ammo"] = "0";
	CHECK(UID_EvalBool("item.field.lanes == 1 and item.field.use-ammo == 0", &ctx, &lim, &result, &diag) && result);
	item.fields["use-ammo"] = "1";
	CHECK(UID_EvalBool("item.field.lanes == 1 and item.field.use-ammo == 1", &ctx, &lim, &result, &diag) && result);
	CHECK(UID_EvalBool("item.field.lanes == 1 and item.field.use-ammo == 0", &ctx, &lim, &result, &diag) && !result);
	item.fields["lanes"] = "2";
	CHECK(UID_EvalBool("item.field.lanes == 2", &ctx, &lim, &result, &diag) && result);

	st.cvars["ui_om_hud_active_weapon"] = FakeCvar{"Walther P38", 0};
	CHECK(UID_EvalBool("cvar.ui_om_hud_active_weapon != ''", &ctx, &lim, &result, &diag) && result);
	st.cvars["ui_om_hud_active_weapon"] = FakeCvar{"", 0};
	CHECK(UID_EvalBool("cvar.ui_om_hud_active_weapon != ''", &ctx, &lim, &result, &diag) && !result);

	/* Added in OPM: classic HUD MP visibility gates. */
	st.cvars["ui_om_hud_stopwatch_ms"] = FakeCvar{"0", 0};
	CHECK(UID_EvalBool("cvar.ui_om_hud_stopwatch_ms > 0", &ctx, &lim, &result, &diag) && !result);
	st.cvars["ui_om_hud_stopwatch_ms"] = FakeCvar{"15000", 0};
	CHECK(UID_EvalBool("cvar.ui_om_hud_stopwatch_ms > 0", &ctx, &lim, &result, &diag) && result);

	/* Added in OPM: arithmetic in bool compare operands (visible/style conditions). */
	st.cvars["ui_om_hud_health"] = FakeCvar{"55", 0};
	ctx.itemIndex = 5;
	ctx.itemCount = 10;
	CHECK(UID_EvalBool("cvar.ui_om_hud_health > item.index * 10", &ctx, &lim, &result, &diag) && result);
	ctx.itemIndex = 6;
	CHECK(UID_EvalBool("cvar.ui_om_hud_health > item.index * 10", &ctx, &lim, &result, &diag) && !result);
	CHECK(UID_EvalBool("(cvar.ui_om_hud_health - item.index * 10) > 0", &ctx, &lim, &result, &diag) && !result);
	ctx.itemIndex = 4;
	CHECK(UID_EvalBool("cvar.ui_om_hud_health >= (item.index + 1) * 10", &ctx, &lim, &result, &diag) && result);
	/* String compares must still win for non-numeric cvars. */
	st.cvars["ui_om_hud_last_gun"] = FakeCvar{"StG 44", 0};
	st.cvars["ui_om_hud_primary_name"] = FakeCvar{"StG 44", 0};
	CHECK(UID_EvalBool("cvar.ui_om_hud_last_gun == cvar.ui_om_hud_primary_name", &ctx, &lim, &result, &diag) && result);
	st.cvars["ui_om_hud_primary_name"] = FakeCvar{"Walther P38", 0};
	CHECK(UID_EvalBool("cvar.ui_om_hud_last_gun == cvar.ui_om_hud_primary_name", &ctx, &lim, &result, &diag) && !result);

	st.cvars["cg_gametype"] = FakeCvar{"1", 0};
	st.cvars["ui_om_hud_team"] = FakeCvar{"3", 0};
	CHECK(UID_EvalBool("cvar.cg_gametype > 1 and cvar.ui_om_hud_team == 3", &ctx, &lim, &result, &diag) && !result);
	st.cvars["cg_gametype"] = FakeCvar{"2", 0};
	CHECK(UID_EvalBool("cvar.cg_gametype > 1 and cvar.ui_om_hud_team == 3", &ctx, &lim, &result, &diag) && result);

	st.cvars["ui_compass"] = FakeCvar{"0", 0};
	CHECK(UID_EvalBool("cvar.ui_compass > 0", &ctx, &lim, &result, &diag) && !result);
	st.cvars["ui_compass"] = FakeCvar{"1", 0};
	CHECK(UID_EvalBool("cvar.ui_compass > 0", &ctx, &lim, &result, &diag) && result);

	st.cvars["ui_om_hud_obj_left_visible"] = FakeCvar{"0", 0};
	CHECK(UID_EvalBool("cvar.ui_om_hud_obj_left_visible > 0", &ctx, &lim, &result, &diag) && !result);
	st.cvars["ui_om_hud_obj_left_visible"] = FakeCvar{"1", 0};
	CHECK(UID_EvalBool("cvar.ui_om_hud_obj_left_visible > 0", &ctx, &lim, &result, &diag) && result);

	st.cvars["ui_om_hud_time_message"] = FakeCvar{"", 0};
	CHECK(UID_EvalBool("cvar.ui_om_hud_time_message != ''", &ctx, &lim, &result, &diag) && !result);
	st.cvars["ui_om_hud_time_message"] = FakeCvar{"Time Left:  5:00", 0};
	CHECK(UID_EvalBool("cvar.ui_om_hud_time_message != ''", &ctx, &lim, &result, &diag) && result);

	std::string styleVal;
	ctx.itemIndex = 1;
	CHECK(UID_EvalStyleTernary("item.selected ? #1A6FD4FF : #00000000", &ctx, &lim, &styleVal, &diag));
	CHECK(styleVal == "#1A6FD4FF");
	CHECK(UID_EvalStyleTernary("item.last ? 5px : 3px", &ctx, &lim, &styleVal, &diag));
	CHECK(styleVal == "3px");

	/* Added in OPM: nested style ternaries in else branch. */
	st.cvars["ui_om_hud_health"] = FakeCvar{"55", 0};
	ctx.itemIndex = 3;
	ctx.itemCount = 10;
	CHECK(UID_EvalStyleTernary(
		"cvar.ui_om_hud_health > item.index * 10 and cvar.ui_om_hud_health < 40 ? #FF0000FF : "
		"cvar.ui_om_hud_health > item.index * 10 ? #00FF00FF : #111111FF",
		&ctx,
		&lim,
		&styleVal,
		&diag
	));
	CHECK(styleVal == "#00FF00FF");
	ctx.itemIndex = 6;
	CHECK(UID_EvalStyleTernary(
		"cvar.ui_om_hud_health > item.index * 10 and cvar.ui_om_hud_health < 40 ? #FF0000FF : "
		"cvar.ui_om_hud_health > item.index * 10 ? #00FF00FF : #111111FF",
		&ctx,
		&lim,
		&styleVal,
		&diag
	));
	CHECK(styleVal == "#111111FF");
	st.cvars["ui_om_hud_health"] = FakeCvar{"30", 0};
	ctx.itemIndex = 1;
	CHECK(UID_EvalStyleTernary(
		"cvar.ui_om_hud_health > item.index * 10 and cvar.ui_om_hud_health < 40 ? #FF0000FF : "
		"cvar.ui_om_hud_health > item.index * 10 ? #00FF00FF : #111111FF",
		&ctx,
		&lim,
		&styleVal,
		&diag
	));
	CHECK(styleVal == "#FF0000FF");
}

void TestVisibleBraceExpr(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="body" src="fonts/x.ttf" weight="400"/></fonts>
  </definitions>
  <canvas>
    <container type="vertical" width="100%" height="100%">
      <button id="play_panel" visible="{cvar.ui_om_main_panel == play}">Play</button>
      <button id="settings_panel" visible="{cvar.ui_om_main_panel == settings}">Settings</button>
      <button id="apply" enabled="{cvar.ui_om_settings_tab == video}"
              fill="#1A6FD4FF" disabled-fill="#FFFFFF0F" color="#FFFFFFFF" disabled-color="#EBF0F559">Apply</button>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_om_main_panel"] = FakeCvar{"play", 0};
	st.cvars["ui_om_settings_tab"] = FakeCvar{"mouse", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("brace.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	uid_node_id_t playPanel = doc->idIndex["play_panel"];
	uid_node_id_t settingsPanel = doc->idIndex["settings_panel"];
	uid_node_id_t applyId = doc->idIndex["apply"];
	const char *vis = doc->nodes[(size_t)playPanel].properties.GetCStr("visible", "false");
	CHECK(vis && std::strcmp(vis, "true") == 0);
	vis = doc->nodes[(size_t)settingsPanel].properties.GetCStr("visible", "true");
	CHECK(vis && std::strcmp(vis, "false") == 0);
	const char *enabled = doc->nodes[(size_t)applyId].properties.GetCStr("enabled", "true");
	CHECK(enabled && std::strcmp(enabled, "false") == 0);

	st.cvars["ui_om_main_panel"].value = "settings";
	st.cvars["ui_om_settings_tab"].value = "video";
	UID_SyncBindings(doc, &be);
	vis = doc->nodes[(size_t)playPanel].properties.GetCStr("visible", "true");
	CHECK(vis && std::strcmp(vis, "false") == 0);
	vis = doc->nodes[(size_t)settingsPanel].properties.GetCStr("visible", "false");
	CHECK(vis && std::strcmp(vis, "true") == 0);
	enabled = doc->nodes[(size_t)applyId].properties.GetCStr("enabled", "false");
	CHECK(enabled && std::strcmp(enabled, "true") == 0);

	UID_DestroyDocument(doc);
}

void TestBindingsAndActions(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["r_fullscreen"] = FakeCvar{"0", 0};
	st.cvars["cg_fov"] = FakeCvar{"90", 0};
	st.cvars["rom_var"] = FakeCvar{"1", UID_CVAR_ROM};
	uid_backend_t be = MakeFakeBackend(&st);

	std::string name;
	CHECK(UID_ParseCvarBind("cvar:cg_fov", &name) && name == "cg_fov");
	CHECK(UID_ParseCvarBind("cvar(cg_fov)", &name) && name == "cg_fov");
	CHECK(UID_ParseItemFieldBind("item.field:weapon_class", &name) && name == "weapon_class");
	CHECK(UID_ParseItemFieldBind("item.field.weapon_class", &name) && name == "weapon_class");
	CHECK(!UID_ParseItemFieldBind("cvar:weapon_class", &name));
	CHECK(!UID_ParseCvarBind("item.field:weapon_class", &name));

	CHECK(UID_ParseXml("bind.xml", kBindDoc, std::strlen(kBindDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	uid_node_id_t tid = doc->idIndex.count("t") ? doc->idIndex["t"] : -1;
	CHECK(tid >= 0);
	if (tid >= 0) {
		CHECK(doc->states[(size_t)tid].runtimeValue.hasValue);
	}

	/* ROM refuse */
	uid_node_def_t fakeNode;
	UID_InitNodeDef(&fakeNode);
	fakeNode.bind = "cvar:rom_var";
	doc->nodes.push_back(fakeNode);
	doc->states.push_back(uid_node_state_t{});
	UID_InitNodeState(&doc->states.back());
	doc->states.back().runtimeValue.hasValue = true;
	doc->states.back().runtimeValue.stringValue = "2";
	uid_node_id_t rid = (uid_node_id_t)(doc->nodes.size() - 1);
	CHECK(UID_WriteBinding(doc, rid, &be) != UID_OK);
	CHECK(st.cvars["rom_var"].value == "1");

	uid_node_id_t go = doc->idIndex.count("go") ? doc->idIndex["go"] : -1;
	CHECK(go >= 0);
	if (go >= 0) {
		CHECK(UID_DispatchEvent(doc, go, UID_EVENT_CLICK, &be) == UID_OK);
		CHECK(st.cvars["cg_fov"].value == "100");
		CHECK(!st.invokes.empty() && st.invokes[0] == "apply-video");
	}

	UID_DestroyDocument(doc);
}

void TestRuntimeLifecycle(void)
{
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	uid_runtime_t   *rt = UID_Create(&be, nullptr);
	CHECK(rt != nullptr);
	CHECK(UID_LoadMemory(rt, "mem.xml", kMinimal, std::strlen(kMinimal)) == UID_OK);
	CHECK(UID_HasDocument(rt));
	/* bad reload keeps prior */
	CHECK(UID_LoadMemory(rt, "bad.xml", kBadDoctype, std::strlen(kBadDoctype)) != UID_OK);
	CHECK(UID_HasDocument(rt));
	UID_SetSurface(rt, 640, 480, 1280, 960);
	uid_pointer_state_t ptr{};
	UID_Update(rt, 0, &ptr);
	UID_DrawChrome(rt);
	UID_DrawOverlay(rt);
	UID_Deactivate(rt);
	UID_Destroy(rt);
}

void TestSettingsFixtureFile(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/examples/settings.xml";
	FILE       *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		std::fprintf(stderr, "missing fixture: %s\n", path.c_str());
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	CHECK(UID_ParseXml(
		"ui/modern/examples/settings.xml",
		xml.c_str(),
		xml.size(),
		&lim,
		&parseIo,
		doc,
		&diags
	) == UID_OK);
	if (diags.HasErrors()) {
		for (const auto &d : diags.Items()) {
			std::fprintf(stderr, "diag: %s\n", d.message.c_str());
		}
	}
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	if (diags.HasErrors()) {
		for (const auto &d : diags.Items()) {
			std::fprintf(stderr, "compile diag: %s\n", d.message.c_str());
		}
	}
	CHECK(UID_GetNodeById(doc, "fov-setting") != nullptr);
	uid_node_def_t *fovLbl = nullptr;
	uid_node_def_t *fovInput = nullptr;
	for (uid_node_def_t &n : doc->nodes) {
		if (n.kind == UID_NODE_LABEL && n.text == "Field of view") {
			fovLbl = &n;
		}
		if (n.kind == UID_NODE_INPUT && n.bind == "cvar:cg_fov") {
			fovInput = &n;
		}
	}
	CHECK(fovLbl != nullptr);
	CHECK(fovInput != nullptr);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	CHECK(UID_LayoutDocument(doc, 1280, 720, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_DestroyDocument(doc);
}

/* Added in OPM: authored px scales; % of canvas does not. */
void TestUiPxScale(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%" gap="0">
      <container id="bar" width="50%" height="52px" fill="#FFFFFFFF"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("pxscale.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 800, 600, 1.0f, 1.0f, &be, &diags) == UID_OK);
	uid_node_id_t barId = NodeId(doc, "bar");
	CHECK(barId >= 0);
	if (barId >= 0) {
		CHECK_EQ_F(doc->states[(size_t)barId].borderBox.h, 52.0, 0.5);
		CHECK_EQ_F(doc->states[(size_t)barId].borderBox.w, 400.0, 0.5);
	}
	doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty | UID_DIRTY_LAYOUT);
	CHECK(UID_LayoutDocument(doc, 800, 600, 1.0f, 2.0f, &be, &diags) == UID_OK);
	if (barId >= 0) {
		CHECK_EQ_F(doc->states[(size_t)barId].borderBox.h, 104.0, 0.75);
		CHECK_EQ_F(doc->states[(size_t)barId].borderBox.w, 400.0, 0.5);
	}
	UID_DestroyDocument(doc);
}

/* Added in OPM: composed slider track/range/thumb layout from value. */
void TestComposedSlider(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <slider id="s" width="200px" height="28px" bind="cvar:test_slider" min="0" max="100" step="1">
        <track id="tr" height="6px" fill="#FFFFFF26"/>
        <range id="rg" fill="#1A6FD4FF"/>
        <thumb id="th" width="14px" height="14px" fill="#FFFFFFFF"/>
      </slider>
      <slider id="legacy" width="200px" height="28px" bind="cvar:test_legacy" min="0" max="1" step="0.1"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("composed_slider.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	if (diags.HasErrors()) {
		for (const auto &d : diags.Items()) {
			std::fprintf(stderr, "parse diag: %s\n", d.message.c_str());
		}
	}
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	if (diags.HasErrors()) {
		for (const auto &d : diags.Items()) {
			std::fprintf(stderr, "compile diag: %s\n", d.message.c_str());
		}
	}

	uid_node_id_t sliderId = NodeId(doc, "s");
	CHECK(sliderId >= 0);
	if (sliderId >= 0) {
		uid_node_state_t *sst = &doc->states[(size_t)sliderId];
		sst->runtimeValue.hasValue = true;
		sst->runtimeValue.stringValue = "50";
	}
	CHECK(UID_LayoutDocument(doc, 800, 600, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t trackId = NodeId(doc, "tr");
	uid_node_id_t rangeId = NodeId(doc, "rg");
	uid_node_id_t thumbId = NodeId(doc, "th");
	CHECK(trackId >= 0);
	CHECK(rangeId >= 0);
	CHECK(thumbId >= 0);
	if (trackId >= 0 && rangeId >= 0) {
		const float trackW = doc->states[(size_t)trackId].borderBox.w;
		const float rangeW = doc->states[(size_t)rangeId].borderBox.w;
		CHECK(trackW > 0.0f);
		CHECK_EQ_F(rangeW, trackW * 0.5f, 1.0);
	}
	if (thumbId >= 0) {
		CHECK_EQ_F(doc->states[(size_t)thumbId].borderBox.w, 14.0, 0.5);
		CHECK_EQ_F(doc->states[(size_t)thumbId].borderBox.h, 14.0, 0.5);
	}
	uid_node_id_t legacyId = NodeId(doc, "legacy");
	CHECK(legacyId >= 0);
	if (legacyId >= 0) {
		CHECK(doc->nodes[(size_t)legacyId].children.empty());
		CHECK(doc->states[(size_t)legacyId].borderBox.w > 0.0f);
	}
	UID_DestroyDocument(doc);
}

/* Added in OPM: focused text caret X uses fontMeasure of the prefix, not 8px/codepoint. */
void TestInputCaretUsesFontMeasure(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"/>
    <fonts><font id="body" src="fonts/x.ttf" weight="400"/></fonts>
  </definitions>
  <canvas>
    <input id="chat" type="text" width="200px" height="24px" font="body" font-size="16px"
           color="#FFFFFFFF" fill="#00000000" padding="0"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.fontMeasurePx = 4.0f; /* narrower than the old hardcoded 8px fallback */
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("input_caret.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t id = NodeId(doc, "chat");
	CHECK(id >= 0);
	if (id < 0) {
		UID_DestroyDocument(doc);
		return;
	}

	UID_SetFocus(doc, id, &be);
	uid_node_state_t &nst = doc->states[(size_t)id];
	nst.editBuffer = "hello";
	nst.caretCodepoint = 5;
	nst.anchorCodepoint = 5;

	st.drawLog.clear();
	UID_PaintChrome(doc, &be);

	const float expectX = nst.contentBox.x + 5.0f * st.fontMeasurePx;
	const float wrongX = nst.contentBox.x + 5.0f * 8.0f;
	bool sawCaret = false;
	for (const std::string &line : st.drawLog) {
		float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f, a = 0.0f;
		if (std::sscanf(line.c_str(), "rect %f %f %f %f a=%f", &x, &y, &w, &h, &a) != 5) {
			continue;
		}
		if (std::fabs(w - 2.0f) > 0.6f) {
			continue; /* caret is 2px wide */
		}
		CHECK_EQ_F(x, expectX, 0.6);
		CHECK(std::fabs(x - wrongX) > 1.0f);
		CHECK_EQ_F(h, nst.contentBox.h * 0.65f, 0.6);
		sawCaret = true;
		break;
	}
	CHECK(sawCaret);

	UID_DestroyDocument(doc);
}

/* Added in OPM: number <input> min/max/step may use {template.*} like <slider>. */
void TestTemplateInputBounds(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"/>
    <templates>
      <template id="numrow">
        <props>
          <prop name="bind" type="binding" required="true"/>
          <prop name="min" type="number" required="true"/>
          <prop name="max" type="number" required="true"/>
          <prop name="step" type="number" required="true"/>
        </props>
        <container type="horizontal" width="200px" height="28px">
          <input id="n" type="number" width="56px" height="28px"
                 bind="{template.bind}" min="{template.min}" max="{template.max}" step="{template.step}"/>
        </container>
      </template>
    </templates>
  </definitions>
  <canvas>
    <use id="row" template="numrow" bind="cvar:test_num" min="0.5" max="2.5" step="0.05"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("tmpl_input_bounds.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	if (diags.HasErrors()) {
		for (const auto &d : diags.Items()) {
			std::fprintf(stderr, "diag: %s\n", d.message.c_str());
		}
	}
	CHECK(!diags.HasErrors());
	uid_node_id_t inputId = NodeId(doc, "row.n");
	CHECK(inputId >= 0);
	if (inputId >= 0) {
		const uid_node_def_t *n = UID_GetNode(doc, inputId);
		CHECK(n != nullptr);
		if (n) {
			CHECK(n->hasMin);
			CHECK(n->hasMax);
			CHECK(n->hasStep);
			CHECK_EQ_F(n->minValue, 0.5, 1e-6);
			CHECK_EQ_F(n->maxValue, 2.5, 1e-6);
			CHECK_EQ_F(n->stepValue, 0.05, 1e-6);
		}
	}
	UID_DestroyDocument(doc);
}

/* Added in OPM: border* attrs are compile errors after stroke-only migration. */
void TestBorderAttrRejected(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container width="100px" height="40px" border="1px #FFFFFFFF"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("border.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) != UID_OK);
	CHECK(diags.HasErrors());
	UID_DestroyDocument(doc);
}

/* Added in OPM: built-in rectangle shape when definitions omit it. */
void TestBuiltinRectangleShape(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%" padding="0" margin="0" gap="0"/>
  </definitions>
  <canvas>
    <container id="box" width="100px" height="40px" fill="#808080FF"
               stroke="#FFFFFFFF" stroke-width="1px"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("builtin_rect.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(doc->definitions.shapes.find("rectangle") == doc->definitions.shapes.end());
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(doc->definitions.shapes.find("rectangle") != doc->definitions.shapes.end());
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_PaintChrome(doc, &be);
	CHECK(!st.drawLog.empty());
	UID_DestroyDocument(doc);
}

/* Added in OPM: shape-rotation compile validation. */
void TestShapeRotationRejected(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container width="40px" height="40px" shape-rotation="inf"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("rot_bad.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) != UID_OK);
	CHECK(diags.HasErrors());
	UID_DestroyDocument(doc);
}

/* Added in OPM: shape-rotation is paint-only; layout unchanged, drawPath gets rotation. */
void TestShapeRotationPaint(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"/>
    <shapes>
      <shape id="star" width="24px" height="24px">
        <path fill="{parent.fill}"
              d="M 12 0 L 15 9 L 24 9 L 17 14 L 20 23 L 12 18 L 4 23 L 7 14 L 0 9 L 9 9 Z"/>
      </shape>
    </shapes>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <shape id="plain" shape="star" width="40px" height="40px" fill="#FFFFFFFF"/>
      <shape id="rot" shape="star" width="40px" height="40px" fill="#FFFFFFFF" shape-rotation="90"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("rot_paint.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_node_id_t plainId = NodeId(doc, "plain");
	const uid_node_id_t rotId = NodeId(doc, "rot");
	CHECK(plainId >= 0 && rotId >= 0);
	CHECK_EQ_F(doc->states[(size_t)plainId].borderBox.w, doc->states[(size_t)rotId].borderBox.w, 0.5f);
	CHECK_EQ_F(doc->states[(size_t)plainId].borderBox.h, doc->states[(size_t)rotId].borderBox.h, 0.5f);

	st.drawLog.clear();
	st.lastPathRotation = 0.0f;
	UID_PaintChrome(doc, &be);
	bool sawRot90 = false;
	for (const std::string &line : st.drawLog) {
		if (line.find("rot=90.0") != std::string::npos) {
			sawRot90 = true;
		}
	}
	CHECK(sawRot90);
	CHECK(std::fabs(st.lastPathRotation - 90.0f) < 1e-3f);
	UID_DestroyDocument(doc);
}

/* Added in Omaha: label rotation paints via fontDrawRotated around border-box center. */
void TestTextRotationPaint(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"
              font="body" font-size="14px" color="#FFFFFFFF"/>
    <fonts>
      <font id="body" src="fonts/Oswald-Medium.ttf" weight="400"/>
    </fonts>
  </definitions>
  <canvas>
    <container type="vertical" width="200px" height="100px" padding="0" gap="0">
      <label id="plain" width="80px" height="40px" font-size="14px" color="#FFFFFFFF"
             halign="center" valign="center">Hi</label>
      <label id="rot" width="80px" height="40px" font-size="14px" color="#FFFFFFFF"
             halign="center" valign="center" rotation="90">Hi</label>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("text_rot_paint.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_node_id_t plainId = NodeId(doc, "plain");
	const uid_node_id_t rotId = NodeId(doc, "rot");
	CHECK(plainId >= 0 && rotId >= 0);
	CHECK_EQ_F(doc->states[(size_t)plainId].borderBox.w, doc->states[(size_t)rotId].borderBox.w, 0.5f);
	CHECK_EQ_F(doc->states[(size_t)plainId].borderBox.h, doc->states[(size_t)rotId].borderBox.h, 0.5f);

	st.fontDrawLog.clear();
	UID_PaintChrome(doc, &be);

	bool sawPlain = false;
	bool sawRot = false;
	float rotDeg = 0.0f;
	float pivotX = 0.0f;
	float pivotY = 0.0f;
	for (const std::string &line : st.fontDrawLog) {
		if (line.find("textrot ") == 0 && line.find("'Hi'") != std::string::npos) {
			sawRot = true;
			CHECK(
				std::sscanf(line.c_str(), "textrot %*f,%*f '%*[^']' rot=%f pivot=%f,%f", &rotDeg, &pivotX, &pivotY) ==
				3
			);
		} else if (line.find("text ") == 0 && line.find("'Hi'") != std::string::npos) {
			sawPlain = true;
		}
	}
	CHECK(sawPlain);
	CHECK(sawRot);
	CHECK(std::fabs(rotDeg - 90.0f) < 1e-3f);
	const uid_rect_t &bb = doc->states[(size_t)rotId].borderBox;
	CHECK_EQ_F(pivotX, bb.x + bb.w * 0.5f, 0.5f);
	CHECK_EQ_F(pivotY, bb.y + bb.h * 0.5f, 0.5f);
	UID_DestroyDocument(doc);
}

/* Added in OPM: rotated rectangles route through drawPath, not solid rect fast path. */
void TestRotatedRectangleUsesPath(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"/>
  </definitions>
  <canvas>
    <container id="box" width="100px" height="40px" fill="#808080FF"
               stroke="#FFFFFFFF" stroke-width="1px" shape-rotation="90"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("rot_rect.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_PaintChrome(doc, &be);

	bool sawPath = false;
	bool sawSolidRect = false;
	for (const std::string &line : st.drawLog) {
		if (line.find("path ") == 0) {
			sawPath = true;
			CHECK(line.find("rot=90.0") != std::string::npos);
		}
		if (line.find("rect ") == 0) {
			sawSolidRect = true;
		}
	}
	CHECK(sawPath);
	CHECK(!sawSolidRect);
	UID_DestroyDocument(doc);
}

/* Added in OPM: nested shape containers inside buttons get laid out and painted. */
void TestButtonNestedShapeChild(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"/>
    <shapes>
      <shape id="tri">
        <path fill="{parent.fill}"
              d="M 0 0 L {parent.width} 0 L {parent.width} {parent.height * 0.5} L 0 {parent.height * 0.5} Z"/>
      </shape>
    </shapes>
  </definitions>
  <canvas>
    <button id="prev" width="32px" height="28px" fill="#00000047" shape="rectangle">
      <container id="icon" width="100%" height="100%" shape="tri" fill="#FFFFFFFF"/>
    </button>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("btn_shape.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_node_id_t iconId = NodeId(doc, "icon");
	CHECK(iconId >= 0);
	CHECK(doc->states[(size_t)iconId].borderBox.w > 8.0f);
	CHECK(doc->states[(size_t)iconId].borderBox.h > 8.0f);

	UID_PaintChrome(doc, &be);
	bool sawPath = false;
	for (const std::string &line : st.drawLog) {
		if (line.find("path ") == 0 && line.find("fill_a=1.00") != std::string::npos) {
			sawPath = true;
		}
	}
	CHECK(sawPath);
	UID_DestroyDocument(doc);
}

/* Added in OPM: width=auto on icon buttons includes child shapes and padding. */
void TestButtonAutoWidthIconPadding(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"/>
    <shapes>
      <shape id="icon-box">
        <path fill="{parent.fill}" d="M 0 0 L {parent.width} 0 L {parent.width} {parent.height} L 0 {parent.height} Z"/>
      </shape>
    </shapes>
  </definitions>
  <canvas>
    <button id="exit" width="auto" height="40px" padding="0 18px" halign="center" valign="center">
      <shape id="icon" shape="icon-box" width="32px" height="32px" fill="#FFFFFFFF"/>
    </button>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("btn_auto_icon.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_node_id_t exitId = NodeId(doc, "exit");
	const uid_node_id_t iconId = NodeId(doc, "icon");
	CHECK(exitId >= 0 && iconId >= 0);

	const uid_rect_t &btn = doc->states[(size_t)exitId].borderBox;
	const uid_rect_t &content = doc->states[(size_t)exitId].contentBox;
	const uid_rect_t &icon = doc->states[(size_t)iconId].borderBox;
	CHECK_EQ_F(btn.w, 68.0f, 0.5f);
	CHECK_EQ_F(content.w, 32.0f, 0.5f);
	CHECK_EQ_F(icon.w, 32.0f, 0.5f);
	CHECK_EQ_F(icon.x, content.x, 0.5f);
	CHECK_EQ_F(icon.x + icon.w, content.x + content.w, 0.5f);
	UID_DestroyDocument(doc);
}

/* Added in OPM: settings-cyclic template paints foreach label and chevron paths. */
void TestSettingsCyclicTemplatePaint(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"/>
    <fonts><font id="control" src="fonts/Oswald-SemiBold.ttf" weight="600"/></fonts>
    <shapes>
      <shape id="triangle-left">
        <path fill="{parent.fill}"
              d="M {parent.width * 0.65} {parent.height * 0.22}
                 L {parent.width * 0.35} {parent.height * 0.5}
                 L {parent.width * 0.65} {parent.height * 0.78}
                 Z"/>
      </shape>
    </shapes>
    <sources>
      <source id="display-mode" default="1">
        <item value="1" label="Fullscreen"/>
        <item value="2" label="Borderless"/>
        <item value="0" label="Windowed"/>
      </source>
    </sources>
    <templates>
      <template id="settings-cyclic">
        <props>
          <prop name="bind" type="binding" required="true"/>
          <prop name="source" type="string" required="true"/>
        </props>
        <container type="horizontal" width="240px" height="40px" gap="0"
                   source="{template.source}" bind="{template.bind}" wrap="true">
          <button width="32px" height="100%" shape="rectangle" fill="#00000047">
            <container width="100%" height="100%" shape="triangle-left" fill="#FFFFFFFF"/>
          </button>
          <container type="vertical" width="fill" height="100%" gap="4px" valign="start"
                     fill="#080A0CB8" stroke="#FFFFFF29" stroke-width="1px" padding="6px 8px 4px 8px">
            <foreach mode="selected" width="fill" height="fill">
              <label width="fill" height="fill" halign="center" valign="center"
                     font="control" font-size="15px" color="#EBF0F5EB">{item.display}</label>
            </foreach>
            <foreach mode="all" type="horizontal" width="fill" height="5px" gap="2px" valign="center">
              <container width="fill"
                         height="{item.selected ? 5px : 3px}"
                         fill="{item.selected ? #1A6FD4FF : #FFFFFF38}"/>
            </foreach>
          </container>
          <button width="32px" height="100%" shape="rectangle" fill="#00000047">
            <container width="100%" height="100%" shape="triangle-left" fill="#FFFFFFFF" shape-rotation="180"/>
          </button>
        </container>
      </template>
    </templates>
  </definitions>
  <canvas>
    <use template="settings-cyclic" source="display-mode" bind="cvar:r_fullscreen" width="240px" height="40px"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["r_fullscreen"] = FakeCvar{"1", 0};
	st.cvars["r_noborder"] = FakeCvar{"0", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("cyclic_tpl.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 320, 80, 1.0f, 1.0f, &be, &diags) == UID_OK);

	bool foundResolvedLabel = false;
	for (const uid_node_def_t &n : doc->nodes) {
		if (n.kind == UID_NODE_LABEL &&
		    (n.text == "Fullscreen" || n.text == "Borderless" || n.text == "Windowed")) {
			foundResolvedLabel = true;
		}
	}
	CHECK(foundResolvedLabel);

	UID_PaintChrome(doc, &be);

	bool sawLabel = false;
	bool sawChevron = false;
	bool sawIndicatorBar = false;
	for (const std::string &line : st.fontDrawLog) {
		if (line.find("Fullscreen") != std::string::npos || line.find("Borderless") != std::string::npos ||
		    line.find("Windowed") != std::string::npos) {
			sawLabel = true;
		}
	}
	for (const std::string &line : st.drawLog) {
		if (line.find("path ") == 0 && line.find("fill_a=1.00") != std::string::npos) {
			sawChevron = true;
		}
		if (line.find("rect ") == 0) {
			float x = 0.0f;
			float y = 0.0f;
			float w = 0.0f;
			float h = 0.0f;
			float a = 0.0f;
			if (std::sscanf(line.c_str(), "rect %f %f %f %f a=%f", &x, &y, &w, &h, &a) == 5 && w > 8.0f &&
			    h >= 2.0f && h <= 6.0f && a > 0.2f) {
				sawIndicatorBar = true;
			}
		}
	}
	CHECK(sawLabel);
	CHECK(sawChevron);
	CHECK(sawIndicatorBar);
	UID_DestroyDocument(doc);
}

/* Added in OPM: stroke/stroke-width on the using element drill into drawPath. */
void TestElementStroke(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"/>
    <shapes>
      <shape id="hairline">
        <path fill="#00000000"
              d="M 0 {parent.height * 0.5} L {parent.width} {parent.height * 0.5}"/>
      </shape>
    </shapes>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <button id="line" shape="hairline" width="200px" height="28px"
              fill="#00000000" stroke="#1A6FD4FF" stroke-width="2px"/>
      <button id="box" width="100px" height="40px" fill="#808080FF"
              stroke="#FFFFFFFF" stroke-width="1px"/>
    </container>
  </canvas>
</ui>
)";
	const char *badXml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto"/>
  </definitions>
  <canvas>
    <button id="bad" width="40px" height="20px" stroke-width="2px"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("element_stroke.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());

	CHECK(UID_LayoutDocument(doc, 400, 300, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_PaintChrome(doc, &be);

	bool sawLineStroke = false;
	bool sawBoxStrokeFrame = false;
	int boxEdgeRects = 0;
	for (const std::string &line : st.drawLog) {
		if (line.find("stroke_w=2.00") != std::string::npos && line.find("stroke_a=1.00") != std::string::npos) {
			sawLineStroke = true;
		}
		/* Box 100x40 + 1px stroke → four opaque edge quads (size includes stroke). */
		if (line.find("rect ") == 0 && line.find("a=1.00") != std::string::npos) {
			/* Top/bottom full-width 100x1, or side 1x38 */
			if (line.find("100.0 1.0") != std::string::npos || line.find("1.0 38.0") != std::string::npos) {
				boxEdgeRects++;
			}
		}
	}
	sawBoxStrokeFrame = (boxEdgeRects >= 4);
	CHECK(sawLineStroke);
	CHECK(sawBoxStrokeFrame);
	UID_DestroyDocument(doc);

	doc = UID_CreateDocument();
	uid_diag_list_t badDiags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("bad_stroke.xml", badXml, std::strlen(badXml), &lim, nullptr, doc, &badDiags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &badDiags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &badDiags) != UID_OK || badDiags.HasErrors());
	CHECK(badDiags.HasErrors());
	UID_DestroyDocument(doc);
}

/* Added in OPM: label/button drop-shadow paints five black offset passes before main text. */
void TestDropShadow(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"
              font="body" font-size="14px" color="#FFFFFFFF"/>
    <fonts>
      <font id="body" src="fonts/Oswald-Medium.ttf" weight="500"/>
    </fonts>
  </definitions>
  <canvas>
    <container type="vertical" width="400px" height="120px" gap="8px" padding="0">
      <label id="lbl" drop-shadow="true" width="200px" height="30px" font-size="14px" color="#FF0000FF">Hello</label>
      <button id="btn" drop-shadow="true" width="120px" height="40px" font-size="16px" color="#00FF00FF">Join</button>
    </container>
  </canvas>
</ui>
)";

	uid_limits_t lim {};
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("drop_shadow.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 120, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t lbl = NodeId(doc, "lbl");
	uid_node_id_t btn = NodeId(doc, "btn");
	CHECK(lbl >= 0 && btn >= 0);
	if (lbl < 0 || btn < 0) {
		UID_DestroyDocument(doc);
		return;
	}

	st.fontDrawLog.clear();
	st.clipRects.clear();
	UID_PaintChrome(doc, &be);

	const uid_rect_t &lblClip = doc->states[static_cast<size_t>(lbl)].effectiveClip;
	bool              sawExpandedClip = false;
	for (const std::string &line : st.clipRects) {
		float cx = 0.0f;
		float cy = 0.0f;
		float cw = 0.0f;
		float ch = 0.0f;
		if (std::sscanf(line.c_str(), "clip %f,%f,%f,%f", &cx, &cy, &cw, &ch) == 4) {
			if (cw > lblClip.w + 0.5f || ch > lblClip.h + 0.5f || cx < lblClip.x - 0.5f ||
			    cy < lblClip.y - 0.5f) {
				sawExpandedClip = true;
			}
		}
	}
	CHECK(sawExpandedClip);

	struct DrawEntry {
		float x = 0.0f;
		float y = 0.0f;
		char  text[64] = {};
		float rgba[4] = {};
		bool  hasRgba = false;
	};

	auto parseDraw = [](const std::string &line, DrawEntry *out) -> bool {
		if (!out) {
			return false;
		}
		int n = std::sscanf(
			line.c_str(),
			"text %f,%f '%63[^']' rgba=%f,%f,%f,%f",
			&out->x,
			&out->y,
			out->text,
			&out->rgba[0],
			&out->rgba[1],
			&out->rgba[2],
			&out->rgba[3]
		);
		if (n == 7) {
			out->hasRgba = true;
			return true;
		}
		n = std::sscanf(line.c_str(), "text %f,%f '%63[^']'", &out->x, &out->y, out->text);
		return n == 3;
	};

	std::vector<DrawEntry> helloDraws;
	std::vector<DrawEntry> joinDraws;
	for (const std::string &line : st.fontDrawLog) {
		DrawEntry entry;
		if (!parseDraw(line, &entry)) {
			continue;
		}
		if (std::strcmp(entry.text, "Hello") == 0) {
			helloDraws.push_back(entry);
		} else if (std::strcmp(entry.text, "Join") == 0) {
			joinDraws.push_back(entry);
		}
	}

	CHECK(helloDraws.size() == 6);
	CHECK(joinDraws.size() == 6);

	auto checkShadowPasses = [](const std::vector<DrawEntry> &draws, float mainX, float mainY) {
		CHECK(draws.size() == 6);
		const DrawEntry &main = draws.back();
		CHECK_EQ_F(main.x, mainX, 0.75);
		CHECK_EQ_F(main.y, mainY, 0.75);
		CHECK(main.hasRgba);
		CHECK(main.rgba[0] > 0.9f || main.rgba[1] > 0.9f);

		static const struct {
			float dx;
			float dy;
			float a;
		} kPasses[] = {
			{0.0f, -1.0f, 0.50f},
			{0.0f, 1.0f, 0.50f},
			{-1.0f, 0.0f, 0.50f},
			{1.0f, 0.0f, 0.50f},
			{1.5f, 1.5f, 0.33f},
		};
		for (size_t i = 0; i < 5; ++i) {
			const DrawEntry &shadow = draws[i];
			CHECK(shadow.hasRgba);
			CHECK_EQ_F(shadow.rgba[0], 0.0f, 0.01f);
			CHECK_EQ_F(shadow.rgba[1], 0.0f, 0.01f);
			CHECK_EQ_F(shadow.rgba[2], 0.0f, 0.01f);
			CHECK_EQ_F(shadow.rgba[3], kPasses[i].a, 0.02f);
			CHECK_EQ_F(shadow.x, main.x + kPasses[i].dx, 0.75);
			CHECK_EQ_F(shadow.y, main.y + kPasses[i].dy, 0.75);
		}
	};

	const uid_rect_t &lb = doc->states[(size_t)lbl].contentBox;
	const uid_rect_t &bb = doc->states[(size_t)btn].contentBox;
	const double expectLblY = lb.y + lb.h * 0.5 - 14.0 * 0.62;
	const double expectBtnX = bb.x + (bb.w - 32.0) * 0.5;
	const double expectBtnY = bb.y + bb.h * 0.5 - 16.0 * 0.62;
	checkShadowPasses(helloDraws, lb.x, (float)expectLblY);
	checkShadowPasses(joinDraws, (float)expectBtnX, (float)expectBtnY);

	UID_DestroyDocument(doc);
}

/* Added in OPM: parent containers cascade extended text-style props to descendants. */
void TestTextStyleInheritance(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"
              font="body" font-size="14px" color="#FFFFFFFF"/>
    <fonts>
      <font id="body" src="fonts/Oswald-Medium.ttf" weight="500"/>
    </fonts>
    <templates>
      <template id="row">
        <label id="inner" width="200px" height="30px">Child</label>
      </template>
    </templates>
  </definitions>
  <canvas>
    <container id="panel" type="vertical" width="300px" height="120px"
               font-size="18px" line-height="1.6" text-wrap="word" text-skew="6"
               letter-spacing="1px" drop-shadow="true" color="#AABBCCDD">
      <label id="lbl" width="200px" height="30px">Hello</label>
      <label id="override" width="200px" height="30px" color="#FF0000FF">Red</label>
      <use id="row" template="row" font-size="20px"/>
    </container>
  </canvas>
</ui>
)";

	uid_limits_t lim {};
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("inherit.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);

	const uid_node_def_t *lbl = UID_GetNodeById(doc, "lbl");
	const uid_node_def_t *overrideLbl = UID_GetNodeById(doc, "override");
	const uid_node_def_t *inner = UID_GetNodeById(doc, "row.inner");
	CHECK(lbl != nullptr);
	CHECK(overrideLbl != nullptr);
	CHECK(inner != nullptr);
	if (!lbl || !overrideLbl || !inner) {
		UID_DestroyDocument(doc);
		return;
	}

	CHECK(std::strcmp(lbl->properties.GetCStr("font-size", ""), "18px") == 0);
	CHECK(std::strcmp(lbl->properties.GetCStr("line-height", ""), "1.6") == 0);
	CHECK(std::strcmp(lbl->properties.GetCStr("text-wrap", ""), "word") == 0);
	CHECK(std::strcmp(lbl->properties.GetCStr("text-skew", ""), "6") == 0);
	CHECK(std::strcmp(lbl->properties.GetCStr("letter-spacing", ""), "1px") == 0);
	CHECK(std::strcmp(lbl->properties.GetCStr("drop-shadow", ""), "true") == 0);
	CHECK(std::strcmp(lbl->properties.GetCStr("color", ""), "#AABBCCDD") == 0);

	CHECK(std::strcmp(overrideLbl->properties.GetCStr("font-size", ""), "18px") == 0);
	CHECK(std::strcmp(overrideLbl->properties.GetCStr("color", ""), "#FF0000FF") == 0);

	CHECK(std::strcmp(inner->properties.GetCStr("font-size", ""), "20px") == 0);
	CHECK(std::strcmp(inner->properties.GetCStr("drop-shadow", ""), "true") == 0);

	UID_DestroyDocument(doc);
}

/* Added in OPM: drop-shadow inherited from parent container paints on child label. */
void TestInheritedDropShadowPaint(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto" padding="0" margin="0" gap="0"
              overflow="none" fill="#00000000" visible="true" enabled="true"
              font="body" font-size="14px" color="#FFFFFFFF"/>
    <fonts>
      <font id="body" src="fonts/Oswald-Medium.ttf" weight="500"/>
    </fonts>
  </definitions>
  <canvas>
    <container type="vertical" width="400px" height="80px" drop-shadow="true">
      <label id="lbl" width="200px" height="30px" font-size="14px" color="#FF0000FF">Hello</label>
    </container>
  </canvas>
</ui>
)";

	uid_limits_t lim {};
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("inherit_shadow.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 80, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t lbl = NodeId(doc, "lbl");
	CHECK(lbl >= 0);
	if (lbl < 0) {
		UID_DestroyDocument(doc);
		return;
	}

	st.fontDrawLog.clear();
	UID_PaintChrome(doc, &be);

	int helloCount = 0;
	for (const std::string &line : st.fontDrawLog) {
		if (line.find("'Hello'") != std::string::npos) {
			++helloCount;
		}
	}
	CHECK(helloCount == 6);

	UID_DestroyDocument(doc);
}

void TestDesignVarsParseAndResolve(void)
{
	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <vars>
      <var id="space-md" value="16px"/>
      <var id="text-body" value="14px"/>
      <var id="color-primary" value="#EBF0F5E6"/>
    </vars>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="{var.space-md}" height="32px"
               gap="{var.space-md}" font-size="{var.text-body}" color="{var.color-primary}">
      <label id="lbl" width="fill" height="20px">Hi</label>
    </container>
  </canvas>
</ui>)";

	uid_limits_t lim {};
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("vars.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(doc->definitions.vars.size() == 3);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);

	const uid_node_def_t *root = UID_GetNodeById(doc, "root");
	const uid_node_def_t *lbl = UID_GetNodeById(doc, "lbl");
	CHECK(root != nullptr && lbl != nullptr);
	if (!root || !lbl) {
		UID_DestroyDocument(doc);
		return;
	}
	CHECK(std::strcmp(root->properties.GetCStr("width", ""), "16px") == 0);
	CHECK(std::strcmp(root->properties.GetCStr("gap", ""), "16px") == 0);
	CHECK(std::strcmp(root->properties.GetCStr("font-size", ""), "14px") == 0);
	CHECK(std::strcmp(root->properties.GetCStr("color", ""), "#EBF0F5E6") == 0);
	CHECK(std::strcmp(lbl->properties.GetCStr("font-size", ""), "14px") == 0);
	CHECK(std::strcmp(lbl->properties.GetCStr("color", ""), "#EBF0F5E6") == 0);

	UID_DestroyDocument(doc);
}

void TestDesignVarsUnknownRejected(void)
{
	static const char kDoc[] = R"(<ui version="1">
  <definitions/>
  <canvas>
    <container id="root" width="{var.missing}" height="32px"/>
  </canvas>
</ui>)";

	uid_limits_t lim {};
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("vars_bad.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) != UID_OK);
	CHECK(diags.HasErrors());
	UID_DestroyDocument(doc);
}

void TestDesignVarsNumericLayout(void)
{
	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <vars>
      <var id="base" value="20"/>
    </vars>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="{var.base * 2}px" height="40px"/>
  </canvas>
</ui>)";

	uid_limits_t lim {};
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t    be = MakeFakeBackend(&st);
	CHECK(UID_ParseXml("vars_num.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 640, 480, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_node_def_t *root = UID_GetNodeById(doc, "root");
	CHECK(root != nullptr);
	if (!root) {
		UID_DestroyDocument(doc);
		return;
	}
	CHECK_EQ_F(doc->states[(size_t)NodeId(doc, "root")].borderBox.w, 40.0, 0.5);

	UID_DestroyDocument(doc);
}

void TestDesignVarsImportMerge(void)
{
	g_testImportFiles.clear();
	g_testImportFiles["ui/modern/lib/menu_tokens.xml"] = R"(<ui-library version="1">
  <vars>
    <var id="space-md" value="12px"/>
  </vars>
</ui-library>
)";

	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <import src="ui/modern/lib/menu_tokens.xml"/>
    <vars>
      <var id="space-md" value="16px"/>
    </vars>
  </definitions>
  <canvas>
    <container id="root" width="{var.space-md}" height="32px"/>
  </canvas>
</ui>)";

	uid_limits_t lim {};
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	CHECK(UID_ParseXml("vars_import.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(std::strcmp(doc->definitions.vars["space-md"].value.c_str(), "16px") == 0);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	const uid_node_def_t *root = UID_GetNodeById(doc, "root");
	CHECK(root != nullptr);
	if (root) {
		CHECK(std::strcmp(root->properties.GetCStr("width", ""), "16px") == 0);
	}
	UID_DestroyDocument(doc);
	g_testImportFiles.clear();
}

/* Added in OPM: composable foreach cyclic (step-index + mode=selected). */
void TestComposableCyclicForeach(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="control" src="fonts/x.ttf" weight="600"/></fonts>
    <sources>
      <source id="display-mode" default="1">
        <item value="1" label="Fullscreen"/>
        <item value="2" label="Borderless"/>
        <item value="0" label="Windowed"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="scope" type="horizontal" width="280px" height="40px" gap="0"
               source="display-mode" bind="cvar:r_fullscreen" wrap="true" value-type="display-mode">
      <button id="prev" width="32px" height="100%" step-index="-1">‹</button>
      <container width="fill" height="100%" halign="center" valign="center">
        <foreach mode="selected">
          <label id="lbl" font="control" font-size="15px">{item.label}</label>
        </foreach>
      </container>
      <button id="next" width="32px" height="100%" step-index="1">›</button>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["r_fullscreen"] = FakeCvar{"1", 0};
	st.cvars["r_noborder"] = FakeCvar{"0", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("foreach_cyclic.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 320, 80, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t lbl = doc->idIndex.count("lbl") ? doc->idIndex["lbl"] : UID_INVALID_NODE_ID;
	CHECK(lbl >= 0);
	CHECK(UID_GetNode(doc, lbl)->text == "Fullscreen");

	uid_node_id_t scopeId = doc->idIndex.count("scope") ? doc->idIndex["scope"] : UID_INVALID_NODE_ID;
	CHECK(scopeId >= 0);
	CHECK(UID_StepCollectionIndex(doc, scopeId, 1, &be));
	UID_SyncBindings(doc, &be);
	lbl = doc->idIndex.count("lbl") ? doc->idIndex["lbl"] : UID_INVALID_NODE_ID;
	CHECK(lbl >= 0);
	CHECK(UID_GetNode(doc, lbl)->text == "Borderless");

	UID_DestroyDocument(doc);
}

/* Added in Omaha: commit=apply cyclic stages until WriteAllBindings (settings-apply). */
void TestCyclicCommitApplyStagesUntilFlush(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="control" src="fonts/x.ttf" weight="600"/></fonts>
    <sources>
      <source id="picmip" default="1">
        <item value="0" label="Highest"/>
        <item value="1" label="High"/>
        <item value="2" label="Medium"/>
        <item value="3" label="Low"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="scope" type="horizontal" width="280px" height="40px" gap="0"
               source="picmip" bind="cvar:r_picmip" wrap="true" commit="apply">
      <button id="prev" width="32px" height="100%" step-index="-1">‹</button>
      <container width="fill" height="100%" halign="center" valign="center">
        <foreach mode="selected">
          <label id="lbl" font="control" font-size="15px">{item.label}</label>
        </foreach>
      </container>
      <button id="next" width="32px" height="100%" step-index="1">›</button>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["r_picmip"] = FakeCvar{"1", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("cyclic_apply.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 320, 80, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t scopeId = doc->idIndex.count("scope") ? doc->idIndex["scope"] : UID_INVALID_NODE_ID;
	CHECK(scopeId >= 0);
	CHECK(UID_StepCollectionIndex(doc, scopeId, 1, &be));
	UID_SyncBindings(doc, &be);
	/* Staged: UI moved, cvar unchanged until flush. */
	CHECK(st.cvars["r_picmip"].value == "1");
	uid_node_id_t lbl = doc->idIndex.count("lbl") ? doc->idIndex["lbl"] : UID_INVALID_NODE_ID;
	CHECK(lbl >= 0);
	CHECK(UID_GetNode(doc, lbl)->text == "Medium");

	CHECK(UID_WriteAllBindings(doc, &be) == UID_OK);
	CHECK(st.cvars["r_picmip"].value == "2");

	UID_DestroyDocument(doc);
}

/* Added in OPM: vertical list row click via set-index on foreach row. */
void TestComposableVerticalList(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="body" src="fonts/x.ttf" weight="400"/></fonts>
    <sources>
      <source id="display-mode" default="1">
        <item value="1" label="Fullscreen"/>
        <item value="2" label="Borderless"/>
        <item value="0" label="Windowed"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="scope" type="vertical" width="200px" height="120px" source="display-mode"
               bind="cvar:r_fullscreen" value-type="display-mode">
      <foreach mode="all">
        <button width="100%" height="28px" set-index="{item.index}">{item.label}</button>
      </foreach>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["r_fullscreen"] = FakeCvar{"1", 0};
	st.cvars["r_noborder"] = FakeCvar{"0", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("foreach_list.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 240, 160, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t scopeId = doc->idIndex.count("scope") ? doc->idIndex["scope"] : UID_INVALID_NODE_ID;
	CHECK(scopeId >= 0);
	(void)UID_SetCollectionIndex(doc, scopeId, 2, &be);
	UID_SyncBindings(doc, &be);
	CHECK(st.cvars["r_fullscreen"].value == "0");
	CHECK(st.cvars["r_noborder"].value == "0");

	UID_DestroyDocument(doc);
}

/* Added in OPM: XML-authored collection sources + default selection + tab/indicator patterns. */
void TestXmlCollectionSource(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <sources>
      <source id="colors" default="red">
        <item value="red" label="Red"/>
        <item value="green" label="Green"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="scope" type="vertical" width="120px" height="80px" source="colors" bind="cvar:ui_color"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("xml_src.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	uid_node_id_t scopeId = doc->idIndex.count("scope") ? doc->idIndex["scope"] : UID_INVALID_NODE_ID;
	CHECK(scopeId >= 0);
	CHECK(doc->states[static_cast<size_t>(scopeId)].collectionItems.size() == 2);
	CHECK(doc->states[static_cast<size_t>(scopeId)].collectionSelectedIndex == 0);
	CHECK(st.cvars["ui_color"].value == "red");
	UID_DestroyDocument(doc);
}

void TestSourceDefaultNoMatch(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <sources>
      <source id="colors" default="green">
        <item value="red" label="Red"/>
        <item value="green" label="Green"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="scope" type="vertical" width="120px" height="80px" source="colors" bind="cvar:ui_color"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_color"] = FakeCvar{"purple", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("xml_default.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	uid_node_id_t scopeId = doc->idIndex.count("scope") ? doc->idIndex["scope"] : UID_INVALID_NODE_ID;
	CHECK(scopeId >= 0);
	CHECK(doc->states[static_cast<size_t>(scopeId)].collectionSelectedIndex == 1);
	CHECK(st.cvars["ui_color"].value == "green");
	UID_DestroyDocument(doc);
}

void TestCollectionNoDefaultUnset(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="scope" type="vertical" width="120px" height="80px" source="servers"
               bind="cvar:ui_selected_server"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("no_default.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	uid_node_id_t scopeId = doc->idIndex.count("scope") ? doc->idIndex["scope"] : UID_INVALID_NODE_ID;
	CHECK(scopeId >= 0);
	CHECK(doc->states[static_cast<size_t>(scopeId)].collectionSelectedIndex == -1);
	CHECK(st.cvars["ui_selected_server"].value.empty());
	UID_DestroyDocument(doc);
}

void TestCollectionDefaultIndex(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <sources>
      <source id="colors">
        <item value="red" label="Red"/>
        <item value="green" label="Green"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="scope" type="vertical" width="120px" height="80px" source="colors"
               bind="cvar:ui_color" default-index="0"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("default_idx.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	uid_node_id_t scopeId = doc->idIndex.count("scope") ? doc->idIndex["scope"] : UID_INVALID_NODE_ID;
	CHECK(scopeId >= 0);
	CHECK(doc->states[static_cast<size_t>(scopeId)].collectionSelectedIndex == 0);
	CHECK(st.cvars["ui_color"].value == "red");
	UID_DestroyDocument(doc);
}

void TestButtonDblClick(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="100%" height="100%"/></definitions>
  <canvas>
    <button id="btn" width="80px" height="32px">
      <on event="dblclick"><invoke name="test-dblclick"/></on>
    </button>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);
	uid_node_id_t btnId;

	CHECK(UID_ParseXml("dblclick.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);
	btnId = doc->idIndex.count("btn") ? doc->idIndex["btn"] : UID_INVALID_NODE_ID;
	CHECK(btnId >= 0);
	{
		const uid_node_state_t &bst = doc->states[static_cast<size_t>(btnId)];
		uid_pointer_state_t ptr{};
		ptr.x = bst.borderBox.x + bst.borderBox.w * 0.5f;
		ptr.y = bst.borderBox.y + bst.borderBox.h * 0.5f;
		ptr.buttons = UID_POINTER_BUTTON_LEFT;
		UID_HandlePointer(doc, &ptr, 100, &be);
		ptr.buttons = 0;
		UID_HandlePointer(doc, &ptr, 150, &be);
		CHECK(st.invokes.empty());
		ptr.buttons = UID_POINTER_BUTTON_LEFT;
		UID_HandlePointer(doc, &ptr, 400, &be);
		ptr.buttons = 0;
		UID_HandlePointer(doc, &ptr, 450, &be);
		CHECK(st.invokes.size() == 1);
		CHECK(st.invokes[0] == "test-dblclick");
	}
	UID_DestroyDocument(doc);
}

void TestTabBarForeach(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="horizontal" width="100%" height="100%"/>
    <sources>
      <source id="tabs" default="a">
        <item value="a" label="A"/>
        <item value="b" label="B"/>
        <item value="c" label="C"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="tabs" type="horizontal" width="240px" height="32px" source="tabs" bind="cvar:ui_tab">
      <foreach mode="all" type="horizontal" width="100%" height="100%" gap="0">
        <button set-index="{item.index}" width="fill" height="100%"
                fill="{item.selected ? #1A6FD4FF : #00000000}">{item.label}</button>
        <container width="1px" height="100%" fill="#FFFFFF38" visible="{!item.last}"/>
      </foreach>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_tab"] = FakeCvar{"b", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("tab_bar.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 320, 64, 1.0f, 1.0f, &be, &diags) == UID_OK);

	int selectedButtons = 0;
	int separators = 0;
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		const uid_node_def_t &n = doc->nodes[i];
		if (n.kind == UID_NODE_BUTTON && n.text == "B") {
			const char *vis = n.properties.GetCStr("visible", "true");
			if (vis && std::strcmp(vis, "false") != 0) {
				selectedButtons++;
			}
		}
		if (n.foreachItemIndex >= 0 && !n.visibleExpr.empty() && n.visibleExpr.find("item.last") != std::string::npos) {
			const char *vis = n.properties.GetCStr("visible", "true");
			if (vis && std::strcmp(vis, "false") != 0) {
				separators++;
			}
		}
	}
	CHECK(selectedButtons == 1);
	CHECK(separators == 2);
	UID_DestroyDocument(doc);
}

void TestCollectionDisplayValue(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <sources>
      <source id="fps" default="125">
        <item value="125" label="125 FPS"/>
        <item value="250" label="250 FPS"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="scope" type="vertical" width="160px" height="40px" source="fps"
               bind="cvar:com_maxfps" collection-display="value">
      <foreach mode="selected">
        <label id="val" font="control" font-size="14px">{item.display}</label>
      </foreach>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["com_maxfps"] = FakeCvar{"125", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("display_val.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 200, 64, 1.0f, 1.0f, &be, &diags) == UID_OK);
	uid_node_id_t lbl = doc->idIndex.count("val") ? doc->idIndex["val"] : UID_INVALID_NODE_ID;
	CHECK(lbl >= 0);
	CHECK(UID_GetNode(doc, lbl)->text == "125");
	UID_DestroyDocument(doc);
}

void TestHostCollectionFallback(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="control" src="fonts/x.ttf" weight="600"/></fonts>
  </definitions>
  <canvas>
    <container id="scope" type="vertical" width="100%" height="120px" source="servers"
               bind="cvar:ui_selected_server">
      <foreach mode="window">
        <label width="100%" height="28px">{item.label}</label>
      </foreach>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_selected_server"] = FakeCvar{"127.0.0.1:12203", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("host_src.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	uid_node_id_t scopeId = doc->idIndex.count("scope") ? doc->idIndex["scope"] : UID_INVALID_NODE_ID;
	CHECK(scopeId >= 0);
	CHECK(doc->states[static_cast<size_t>(scopeId)].collectionItems.size() == 2);
	if (!doc->states[static_cast<size_t>(scopeId)].collectionItems.empty()) {
		CHECK(doc->states[static_cast<size_t>(scopeId)].collectionItems[0].fields.size() == 7);
	}
	UID_DestroyDocument(doc);
}

/* Added in OPM: foreach re-expand must not use invalidated scope state after node compaction. */
void TestForeachTemplateWrapLayout(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="100%" height="100%"/></definitions>
  <canvas>
    <foreach id="list" mode="all" type="horizontal" width="100%" height="40px" gap="2px" valign="center">
      <button width="40px" height="100%"/>
      <container width="2px" height="100%" fill="#FFFFFFFF"/>
    </foreach>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("foreach_wrap.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	uid_node_id_t id = UID_INVALID_NODE_ID;
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		if (doc->nodes[i].kind == UID_NODE_FOREACH && doc->nodes[i].id == "list") {
			id = static_cast<uid_node_id_t>(i);
			break;
		}
	}
	CHECK(id >= 0);
	const uid_node_def_t *fn = UID_GetNode(doc, id);
	CHECK(fn != nullptr);
	CHECK(fn->foreachTemplateNodes.size() >= 1);
	CHECK(fn->foreachTemplateNodes[0].properties.GetCStr("type", "vertical") &&
	      std::strcmp(fn->foreachTemplateNodes[0].properties.GetCStr("type", "vertical"), "horizontal") == 0);
	CHECK(fn->foreachTemplateNodes[0].properties.GetCStr("height", "") &&
	      std::strcmp(fn->foreachTemplateNodes[0].properties.GetCStr("height", ""), "40px") == 0);
	CHECK(!fn->foreachTemplateNodes[0].properties.GetCStr("width", "") ||
	      std::strcmp(fn->foreachTemplateNodes[0].properties.GetCStr("width", ""), "fill") != 0);

	static const char *kIndicatorDoc = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="100%" height="100%"/></definitions>
  <canvas>
    <foreach id="ticks" mode="all" type="horizontal" width="200px" height="5px" gap="2px">
      <container width="fill"
                 height="{item.selected ? 5px : 3px}"
                 fill="{item.selected ? #1A6FD4FF : #FFFFFF38}"/>
    </foreach>
  </canvas>
</ui>
)";
	uid_document_t *doc2 = UID_CreateDocument();
	uid_diag_list_t diags2(lim.maxDiagnostics);
	CHECK(UID_ParseXml("foreach_ticks.xml", kIndicatorDoc, std::strlen(kIndicatorDoc), &lim, nullptr, doc2, &diags2) ==
	      UID_OK);
	for (size_t i = 0; i < doc2->nodes.size(); ++i) {
		if (doc2->nodes[i].kind == UID_NODE_FOREACH && doc2->nodes[i].id == "ticks") {
			const uid_node_def_t *ticks = &doc2->nodes[i];
			CHECK(ticks->foreachTemplateNodes.size() >= 1);
			CHECK(ticks->foreachTemplateNodes[0].properties.GetCStr("width", "") &&
			      std::strcmp(ticks->foreachTemplateNodes[0].properties.GetCStr("width", ""), "fill") == 0);
			break;
		}
	}
	UID_DestroyDocument(doc2);
	UID_DestroyDocument(doc);
}

void TestForeachReexpandWithFields(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="control" src="fonts/x.ttf" weight="600"/></fonts>
  </definitions>
  <canvas>
    <container id="scope" type="vertical" width="100%" height="120px" source="servers"
               bind="cvar:ui_selected_server">
      <foreach mode="window">
        <container type="horizontal" width="100%" height="36px" set-index="{item.index}">
          <label width="35%" font="control" font-size="16px">{item.field.name}</label>
          <label width="15%" font="control" font-size="16px">{item.field.map}</label>
        </container>
      </foreach>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_selected_server"] = FakeCvar{"127.0.0.1:12203", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	g_fakeServersRevision = 1;
	CHECK(UID_ParseXml("foreach_reexpand.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	g_fakeServersRevision = 2;
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 640, 480, 1.0f, 1.0f, &be, &diags) == UID_OK);

	UID_DestroyDocument(doc);
}

void TestCollectionSelectedFill(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="100%" height="100%"/></definitions>
  <canvas>
    <container id="scope" type="vertical" width="100%" height="120px" source="servers"
               bind="cvar:ui_selected_server">
      <foreach mode="window">
        <container width="100%" height="36px" set-index="{item.index}"
                    fill="{item.selected ? #1A6FD4FF : #00000000}"/>
      </foreach>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_selected_server"] = FakeCvar{"127.0.0.1:12203", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("selected_fill.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 640, 480, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_node_id_t scopeId = doc->idIndex.count("scope") ? doc->idIndex["scope"] : UID_INVALID_NODE_ID;
	CHECK(scopeId >= 0);
	const int sel = doc->states[static_cast<size_t>(scopeId)].collectionSelectedIndex;
	int selectedHits = 0;
	int unselectedHits = 0;
	uid_color_t fill{};
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		const uid_node_def_t &n = doc->nodes[i];
		if (n.foreachScopeId != scopeId || n.foreachItemIndex < 0) {
			continue;
		}
		if (n.styleExprs.find("fill") == n.styleExprs.end()) {
			continue;
		}
		CHECK(UID_ResolveFillColor(doc, static_cast<uid_node_id_t>(i), &fill));
		if (n.foreachItemIndex == sel) {
			CHECK(fill.b > 0.5f);
			selectedHits++;
		} else {
			CHECK(fill.a < 0.01f);
			unselectedHits++;
		}
	}
	CHECK(selectedHits == 1);
	CHECK(unselectedHits >= 1);

	UID_DestroyDocument(doc);
}

/* Added in OPM: mode=window sized from viewport/row-height; scrollY drives offset; synthetic extent. */
void TestWindowForeachScroll(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <sources>
      <source id="rows">
        <item value="0" label="r0"/>
        <item value="1" label="r1"/>
        <item value="2" label="r2"/>
        <item value="3" label="r3"/>
        <item value="4" label="r4"/>
        <item value="5" label="r5"/>
        <item value="6" label="r6"/>
        <item value="7" label="r7"/>
        <item value="8" label="r8"/>
        <item value="9" label="r9"/>
        <item value="10" label="r10"/>
        <item value="11" label="r11"/>
        <item value="12" label="r12"/>
        <item value="13" label="r13"/>
        <item value="14" label="r14"/>
        <item value="15" label="r15"/>
        <item value="16" label="r16"/>
        <item value="17" label="r17"/>
        <item value="18" label="r18"/>
        <item value="19" label="r19"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="scope" type="vertical" width="200px" height="100%" source="rows" bind="cvar:ui_row">
      <container id="scroller" type="vertical" width="100%" height="100px" overflow="scroll" gap="0">
        <foreach id="list" mode="window" row-height="36px">
          <label width="100%" height="36px">{item.label}</label>
        </foreach>
      </container>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_row"] = FakeCvar{"0", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("window_scroll.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);
	/* Second sync picks up contentBox.h for window visible count. */
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t scopeId = doc->idIndex.count("scope") ? doc->idIndex["scope"] : UID_INVALID_NODE_ID;
	uid_node_id_t scrollId = doc->idIndex.count("scroller") ? doc->idIndex["scroller"] : UID_INVALID_NODE_ID;
	uid_node_id_t listId = doc->idIndex.count("list") ? doc->idIndex["list"] : UID_INVALID_NODE_ID;
	CHECK(scopeId >= 0 && scrollId >= 0 && listId >= 0);

	/* viewport 100 / row 36 → ceil=3 + overscan 2 → 5. */
	const int child0 = static_cast<int>(doc->nodes[static_cast<size_t>(listId)].children.size());
	CHECK(child0 >= 3 && child0 <= 6);
	CHECK_EQ_F(doc->states[static_cast<size_t>(scrollId)].contentExtentH, 20.0f * 36.0f, 0.5);

	doc->states[static_cast<size_t>(scrollId)].scrollY = 36.0f * 4.0f;
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);
	listId = doc->idIndex.count("list") ? doc->idIndex["list"] : UID_INVALID_NODE_ID;
	CHECK(listId >= 0);
	CHECK(doc->states[static_cast<size_t>(scopeId)].collectionScrollOffset == 4);

	int minIdx = 999;
	int maxIdx = -1;
	for (uid_node_id_t c : doc->nodes[static_cast<size_t>(listId)].children) {
		if (c < 0 || static_cast<size_t>(c) >= doc->nodes.size()) {
			continue;
		}
		const int idx = doc->nodes[static_cast<size_t>(c)].foreachItemIndex;
		if (idx < minIdx) {
			minIdx = idx;
		}
		if (idx > maxIdx) {
			maxIdx = idx;
		}
	}
	CHECK(minIdx == 4);
	CHECK(maxIdx >= 4);

	UID_StepCollectionIndex(doc, scopeId, 10, &be);
	UID_SyncBindings(doc, &be);
	CHECK(doc->states[static_cast<size_t>(scopeId)].collectionSelectedIndex >= 10);
	CHECK(doc->states[static_cast<size_t>(scopeId)].collectionScrollOffset
		  <= doc->states[static_cast<size_t>(scopeId)].collectionSelectedIndex);

	UID_DestroyDocument(doc);
}

/* Added in Omaha: max-height / max-width clamps + scroll-sibling shrink. */
void TestMaxHeightShortContent(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="panel" type="vertical" width="200px" height="auto" max-height="80%" gap="0">
      <container id="a" width="100%" height="40px"/>
      <container id="b" width="100%" height="40px"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("maxh_short.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 200, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t panelId = doc->idIndex.count("panel") ? doc->idIndex["panel"] : UID_INVALID_NODE_ID;
	CHECK(panelId >= 0);
	CHECK_EQ_F(doc->states[static_cast<size_t>(panelId)].borderBox.h, 80.0f, 0.5);

	UID_DestroyDocument(doc);
}

void TestMaxHeightScrollShrink(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="panel" type="vertical" width="200px" height="auto" max-height="100px" gap="0">
      <container id="hdr" width="100%" height="20px"/>
      <container id="list" type="vertical" width="100%" height="auto" overflow="scroll" gap="0">
        <container width="100%" height="40px"/>
        <container width="100%" height="40px"/>
        <container width="100%" height="40px"/>
        <container width="100%" height="40px"/>
      </container>
      <container id="ftr" width="100%" height="20px"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("maxh_scroll.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 200, 400, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t panelId = doc->idIndex.count("panel") ? doc->idIndex["panel"] : UID_INVALID_NODE_ID;
	uid_node_id_t listId = doc->idIndex.count("list") ? doc->idIndex["list"] : UID_INVALID_NODE_ID;
	uid_node_id_t hdrId = doc->idIndex.count("hdr") ? doc->idIndex["hdr"] : UID_INVALID_NODE_ID;
	uid_node_id_t ftrId = doc->idIndex.count("ftr") ? doc->idIndex["ftr"] : UID_INVALID_NODE_ID;
	CHECK(panelId >= 0 && listId >= 0 && hdrId >= 0 && ftrId >= 0);

	CHECK_EQ_F(doc->states[static_cast<size_t>(panelId)].borderBox.h, 100.0f, 0.5);
	CHECK_EQ_F(doc->states[static_cast<size_t>(hdrId)].borderBox.h, 20.0f, 0.5);
	CHECK_EQ_F(doc->states[static_cast<size_t>(ftrId)].borderBox.h, 20.0f, 0.5);
	CHECK_EQ_F(doc->states[static_cast<size_t>(listId)].borderBox.h, 60.0f, 0.5);
	CHECK(doc->states[static_cast<size_t>(listId)].contentExtentH + 0.5f >
	      doc->states[static_cast<size_t>(listId)].contentBox.h);

	UID_DestroyDocument(doc);
}

void TestMaxHeightWindowForeachIntrinsic(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <sources>
      <source id="rows">
        <item value="0" label="r0"/>
        <item value="1" label="r1"/>
        <item value="2" label="r2"/>
        <item value="3" label="r3"/>
        <item value="4" label="r4"/>
        <item value="5" label="r5"/>
        <item value="6" label="r6"/>
        <item value="7" label="r7"/>
        <item value="8" label="r8"/>
        <item value="9" label="r9"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="panel" type="vertical" width="200px" height="auto" max-height="100px" gap="0"
               source="rows" bind="cvar:ui_row">
      <container id="scroller" type="vertical" width="100%" height="auto" overflow="scroll" gap="0">
        <foreach id="list" mode="window" row-height="36px">
          <label width="100%" height="36px">{item.label}</label>
        </foreach>
      </container>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_row"] = FakeCvar{"0", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("maxh_window.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 200, 400, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 200, 400, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t panelId = doc->idIndex.count("panel") ? doc->idIndex["panel"] : UID_INVALID_NODE_ID;
	uid_node_id_t scrollId = doc->idIndex.count("scroller") ? doc->idIndex["scroller"] : UID_INVALID_NODE_ID;
	CHECK(panelId >= 0 && scrollId >= 0);
	CHECK_EQ_F(doc->states[static_cast<size_t>(panelId)].borderBox.h, 100.0f, 0.5);
	CHECK_EQ_F(doc->states[static_cast<size_t>(scrollId)].contentExtentH, 10.0f * 36.0f, 0.5);
	CHECK(doc->states[static_cast<size_t>(scrollId)].contentExtentH + 0.5f >
	      doc->states[static_cast<size_t>(scrollId)].contentBox.h);

	UID_DestroyDocument(doc);
}

void TestMaxHeightCompileReject(void)
{
	static const char *kFill = R"(
<ui version="1">
  <definitions/>
  <canvas>
    <container id="panel" width="100px" height="auto" max-height="fill"/>
  </canvas>
</ui>
)";
	static const char *kAuto = R"(
<ui version="1">
  <definitions/>
  <canvas>
    <container id="panel" width="100px" height="auto" max-height="auto"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);

	{
		uid_document_t *doc = UID_CreateDocument();
		uid_diag_list_t diags(lim.maxDiagnostics);
		CHECK(UID_ParseXml("maxh_fill.xml", kFill, std::strlen(kFill), &lim, nullptr, doc, &diags) == UID_OK);
		CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
		CHECK(UID_CompileDocument(doc, &diags) != UID_OK);
		UID_DestroyDocument(doc);
	}
	{
		uid_document_t *doc = UID_CreateDocument();
		uid_diag_list_t diags(lim.maxDiagnostics);
		CHECK(UID_ParseXml("maxh_auto.xml", kAuto, std::strlen(kAuto), &lim, nullptr, doc, &diags) == UID_OK);
		CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
		CHECK(UID_CompileDocument(doc, &diags) != UID_OK);
		UID_DestroyDocument(doc);
	}
}

void TestMaxWidthClamp(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="panel" type="horizontal" width="auto" max-width="100px" height="40px" gap="0">
      <container width="80px" height="100%"/>
      <container width="80px" height="100%"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("maxw.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t panelId = doc->idIndex.count("panel") ? doc->idIndex["panel"] : UID_INVALID_NODE_ID;
	CHECK(panelId >= 0);
	CHECK_EQ_F(doc->states[static_cast<size_t>(panelId)].borderBox.w, 100.0f, 0.5);

	UID_DestroyDocument(doc);
}

/* Added in OPM: hidden panels skip Expand/Refresh; reveal expands same SyncBindings. */
void TestCollectionVisibilityCull(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <sources>
      <source id="rows">
        <item value="a" label="A"/>
        <item value="b" label="B"/>
        <item value="c" label="C"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%">
      <container id="panel_a" type="vertical" width="100%" height="100%"
                 visible="{cvar.ui_panel == a}" source="rows" bind="cvar:ui_sel_a">
        <foreach id="list_a" mode="all">
          <label width="100%" height="20px">{item.label}</label>
        </foreach>
      </container>
      <container id="panel_b" type="vertical" width="100%" height="100%"
                 visible="{cvar.ui_panel == b}" source="rows" bind="cvar:ui_sel_b">
        <foreach id="list_b" mode="all">
          <label width="100%" height="20px">{item.label}</label>
        </foreach>
      </container>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_panel"] = FakeCvar{"a", 0};
	st.cvars["ui_sel_a"] = FakeCvar{"a", 0};
	st.cvars["ui_sel_b"] = FakeCvar{"a", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("coll_cull.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	uid_node_id_t listA = doc->idIndex.count("list_a") ? doc->idIndex["list_a"] : UID_INVALID_NODE_ID;
	uid_node_id_t listB = doc->idIndex.count("list_b") ? doc->idIndex["list_b"] : UID_INVALID_NODE_ID;
	CHECK(listA >= 0 && listB >= 0);
	CHECK(doc->nodes[static_cast<size_t>(listA)].children.size() == 3);
	CHECK(doc->nodes[static_cast<size_t>(listB)].children.empty());

	st.cvars["ui_panel"].value = "b";
	UID_SyncBindings(doc, &be);
	listA = doc->idIndex.count("list_a") ? doc->idIndex["list_a"] : UID_INVALID_NODE_ID;
	listB = doc->idIndex.count("list_b") ? doc->idIndex["list_b"] : UID_INVALID_NODE_ID;
	CHECK(listA >= 0 && listB >= 0);
	/* A stays warm (children kept); B expands same frame. */
	CHECK(doc->nodes[static_cast<size_t>(listA)].children.size() == 3);
	CHECK(doc->nodes[static_cast<size_t>(listB)].children.size() == 3);

	UID_DestroyDocument(doc);
}

void TestStrokeStyleTernary(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="100%" height="100%"/></definitions>
  <canvas>
    <container id="box" width="100px" height="40px"
               fill="#112233FF"
               stroke="{cvar.ui_selected == on ? #FF0000FF : #00000000}"
               stroke-width="1px"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_selected"] = FakeCvar{"on", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("stroke_ternary.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);

	const uid_node_id_t boxId = doc->idIndex.count("box") ? doc->idIndex["box"] : UID_INVALID_NODE_ID;
	CHECK(boxId >= 0);
	CHECK(doc->nodes[static_cast<size_t>(boxId)].styleExprs.count("stroke") == 1);

	UID_SyncBindings(doc, &be);
	CHECK(std::strcmp(doc->nodes[static_cast<size_t>(boxId)].properties.GetCStr("stroke", ""), "#FF0000FF") == 0);

	st.cvars["ui_selected"].value = "off";
	UID_SyncBindings(doc, &be);
	CHECK(std::strcmp(doc->nodes[static_cast<size_t>(boxId)].properties.GetCStr("stroke", ""), "#00000000") == 0);

	UID_DestroyDocument(doc);
}

void TestCvarNeqStyleFillSurvivesSync(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="tab" width="100px" height="40px"
               fill="{cvar.ui_om_hud_last_gun != cvar.ui_om_hud_primary_name ? #112233FF : #00000000}"
               stroke="#FFFFFF1A" stroke-width="1px"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_om_hud_last_gun"] = FakeCvar{"Kar98k", 0};
	st.cvars["ui_om_hud_primary_name"] = FakeCvar{"Kar98k", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("cvar_neq_fill.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);

	const uid_node_id_t tabId = doc->idIndex.count("tab") ? doc->idIndex["tab"] : UID_INVALID_NODE_ID;
	CHECK(tabId >= 0);
	uid_node_def_t &tab = doc->nodes[static_cast<size_t>(tabId)];
	CHECK(tab.styleExprs.count("fill") == 1);
	CHECK(tab.cvarBoundProps.count("fill") == 0);

	UID_SyncBindings(doc, &be);
	CHECK(std::strcmp(tab.properties.GetCStr("fill", ""), "#00000000") == 0);

	st.cvars["ui_om_hud_last_gun"].value = "Pistol";
	UID_SyncBindings(doc, &be);
	CHECK(std::strcmp(tab.properties.GetCStr("fill", ""), "#112233FF") == 0);

	UID_DestroyDocument(doc);
}

void TestFavoriteButtonInvoke(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions><defaults type="vertical" width="100%" height="100%"/></definitions>
  <canvas>
    <button id="fav_btn" width="100%" height="36px">
      <on event="click">
        <set-cvar name="ui_browser_favorite_target" value="127.0.0.1:12203"/>
        <invoke name="toggle-server-favorite"/>
      </on>
    </button>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_browser_favorite_target"] = FakeCvar{"", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("favorite_btn.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	uid_node_id_t favId = doc->idIndex.count("fav_btn") ? doc->idIndex["fav_btn"] : UID_INVALID_NODE_ID;
	CHECK(favId >= 0);
	CHECK(UID_DispatchEvent(doc, favId, UID_EVENT_CLICK, &be) == UID_OK);
	CHECK(!st.invokes.empty());
	CHECK(st.invokes[0] == "toggle-server-favorite");
	CHECK(st.cvars["ui_browser_favorite_target"].value == "127.0.0.1:12203");

	UID_DestroyDocument(doc);
}

void TestInlineSettingsPages(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/main.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_om_main_panel"] = FakeCvar{"settings", 0};
	st.cvars["ui_om_settings_tab"] = FakeCvar{"input", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("ui/modern/main.xml", xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	size_t keybindCount = 0;
	for (const uid_node_def_t &node : doc->nodes) {
		if (node.kind == UID_NODE_KEYBIND) {
			keybindCount++;
		}
	}
	CHECK(keybindCount > 0);

	UID_DestroyDocument(doc);
}

void TestSettingsTabVisibility(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/main.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_om_main_panel"] = FakeCvar{"settings", 0};
	st.cvars["ui_om_settings_tab"] = FakeCvar{"input", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("ui/modern/main.xml", xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	uid_node_id_t inputPage = doc->idIndex["settings_page_input"];
	uid_node_id_t videoPage = doc->idIndex["settings_page_video"];
	CHECK(inputPage >= 0);
	CHECK(videoPage >= 0);
	const char *vis = doc->nodes[(size_t)inputPage].properties.GetCStr("visible", "false");
	CHECK(vis && std::strcmp(vis, "true") == 0);
	vis = doc->nodes[(size_t)videoPage].properties.GetCStr("visible", "true");
	CHECK(vis && std::strcmp(vis, "false") == 0);

	st.cvars["ui_om_settings_tab"].value = "video";
	UID_SyncBindings(doc, &be);
	vis = doc->nodes[(size_t)inputPage].properties.GetCStr("visible", "true");
	CHECK(vis && std::strcmp(vis, "false") == 0);
	vis = doc->nodes[(size_t)videoPage].properties.GetCStr("visible", "false");
	CHECK(vis && std::strcmp(vis, "true") == 0);

	UID_DestroyDocument(doc);
}

static void RegisterTestSettingsCvars(FakeBackendState &st)
{
	static const char *const kNames[] = {
		"fps", "cg_fov", "cg_drawviewmodel", "cg_hud", "cg_rain", "cg_marks_add", "cg_shadows",
		"cg_effectdetail", "vss_draw", "com_blood", "in_mouse", "r_lodscale", "r_noborder",
		"sensitivity", "m_pitch", "m_yaw", "m_filter", "cl_mouseAccel", "cl_run", "cg_zoomSensitivity",
		"r_mode", "r_fullscreen", "r_swapInterval", "com_maxfps", "r_gamma", "r_colorbits", "r_texturebits",
		"ui_scale", "ui_om_menu_map_view", "r_picmip", "r_textureMode", "r_ext_compressed_textures",
		"r_fastentlight", "r_entlightmap", "r_flares", "r_drawstaticdecals",
		"s_initsound", "s_volume", "s_musicvolume", "s_speaker_type", "s_khz", "s_milesdriver",
		"s_doppler", "s_reverb", "s_mixahead", "s_muteWhenMinimized", "s_muteWhenUnfocused",
		"ui_om_hud", "ui_weaponsbar", "cg_crosshair_mode", "cg_autoswitch", "ui_legacy",
		"ui_om_settings_search", "ui_om_settings_tab", "ui_om_main_panel",
		"ui_modernsettings_dpi", "ui_modernsettings_sensitivity_mode",
		"cg_crosshaircolor_r", "cg_crosshaircolor_g", "cg_crosshaircolor_b", "cg_crosshairalpha",
		"cg_crosshair_drawoutline", "cg_crosshair_outlinethickness", "cg_crosshairusealpha",
		"cg_crosshair_solid_size", "cg_crosshairthickness", "cg_crosshairsize",
		"cg_crosshairgap", "cg_crosshairdot", "cg_crosshair_dot_size", "cg_crosshair_dynamic",
		"cg_crosshair_dynamic", "cg_crosshair_dynamic_movement", "cg_crosshair_friendly_warning",
		"cg_crosshaircolor", "cg_crosshair_recoil",
		"cg_crosshair_dynamic_splitdist", "cg_crosshair_dynamic_maxdist_splitratio",
		nullptr,
	};

	for (int i = 0; kNames[i]; ++i) {
		if (st.cvars.find(kNames[i]) == st.cvars.end()) {
			st.cvars[kNames[i]] = FakeCvar{"0", 0};
		}
	}
}

static std::string FindRowCvarBind(const uid_document_t *doc, uid_node_id_t root)
{
	const uid_node_def_t &node = doc->nodes[static_cast<size_t>(root)];
	std::string            name;

	if (!node.bind.empty() && UID_ParseCvarBind(node.bind.c_str(), &name)) {
		return name;
	}
	for (uid_node_id_t child : node.children) {
		name = FindRowCvarBind(doc, child);
		if (!name.empty()) {
			return name;
		}
	}
	return "";
}

void TestSettingsCvarBindsResolve(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/main.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	RegisterTestSettingsCvars(st);
	st.cvars["ui_om_main_panel"] = FakeCvar{"settings", 0};
	st.cvars["ui_om_settings_tab"] = FakeCvar{"input", 0};
	st.cvars["com_maxfps"] = FakeCvar{"85", 0};
	st.cvars["r_lodscale"] = FakeCvar{"1.0", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("ui/modern/main.xml", xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		const uid_node_def_t &node = doc->nodes[i];
		if (node.visibleExpr.find("icontains") == std::string::npos) {
			continue;
		}
		const std::string cvarName = FindRowCvarBind(doc, static_cast<uid_node_id_t>(i));
		if (cvarName.empty()) {
			continue;
		}
		char valueBuf[256];
		valueBuf[0] = '\0';
		CHECK(fake_cvarDescribe(cvarName.c_str(), nullptr, valueBuf, sizeof(valueBuf)));
	}

	CHECK(st.cvars["com_maxfps"].value == "125");
	CHECK(st.cvars["r_lodscale"].value == "1.1");

	UID_DestroyDocument(doc);
}

void TestSettingsSearchFilter(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/main.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	RegisterTestSettingsCvars(st);
	st.cvars["ui_om_main_panel"] = FakeCvar{"settings", 0};
	st.cvars["ui_om_settings_tab"] = FakeCvar{"input", 0};
	st.cvars["ui_modernsettings_sensitivity_mode"] = FakeCvar{"sensitivity", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("ui/modern/main.xml", xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	uid_node_id_t sensRow = UID_INVALID_NODE_ID;
	uid_node_id_t gammaRow = UID_INVALID_NODE_ID;
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		const std::string &expr = doc->nodes[i].visibleExpr;
		if (expr.find("icontains('Sensitivity'") != std::string::npos &&
		    expr.find("{template") == std::string::npos) {
			sensRow = static_cast<uid_node_id_t>(i);
		} else if (FindRowCvarBind(doc, static_cast<uid_node_id_t>(i)) == "r_gamma") {
			gammaRow = static_cast<uid_node_id_t>(i);
		}
	}
	CHECK(sensRow >= 0);
	CHECK(gammaRow >= 0);

	st.cvars["ui_om_settings_search"].value = "gamma";
	UID_SyncBindings(doc, &be);
	const char *vis = doc->nodes[(size_t)sensRow].properties.GetCStr("visible", "true");
	CHECK(vis && std::strcmp(vis, "false") == 0);
	vis = doc->nodes[(size_t)gammaRow].properties.GetCStr("visible", "false");
	CHECK(vis && std::strcmp(vis, "true") == 0);

	st.cvars["ui_om_settings_tab"].value = "video";
	UID_SyncBindings(doc, &be);
	vis = doc->nodes[(size_t)gammaRow].properties.GetCStr("visible", "false");
	CHECK(vis && std::strcmp(vis, "true") == 0);

	st.cvars["ui_om_settings_search"].value = "";
	UID_SyncBindings(doc, &be);
	st.cvars["ui_om_settings_tab"].value = "input";
	UID_SyncBindings(doc, &be);
	vis = doc->nodes[(size_t)sensRow].properties.GetCStr("visible", "false");
	CHECK(vis && std::strcmp(vis, "true") == 0);

	UID_DestroyDocument(doc);
}

void TestRouteTabBar(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="horizontal" width="100%" height="100%"/>
    <sources>
      <source id="menu-panels" default="play">
        <item value="play" label="PLAY"/>
        <item value="settings" label="SETTINGS"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="menu_nav_tabs" type="horizontal" width="320px" height="32px"
               source="menu-panels" bind="cvar:ui_om_main_panel">
      <foreach mode="all" type="horizontal" width="100%" height="100%" gap="0" valign="center">
        <button set-index="{item.index}"
                width="auto" height="100%" padding="0 20px"
                fill="{item.selected ? #1A6FD4FF : #FFFFFF00}"
                color="{item.selected ? #FFFFFFFF : #EBF0F5B8}">{item.label}</button>
        <container width="2px" height="100%" fill="#FFFFFF61" visible="{!item.last}"/>
      </foreach>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_om_main_panel"] = FakeCvar{"play", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("route_tab.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 400, 64, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	uid_node_id_t scopeId = doc->idIndex["menu_nav_tabs"];
	CHECK(scopeId >= 0);
	CHECK(doc->states[(size_t)scopeId].collectionItemCount == 2);
	CHECK(doc->states[(size_t)scopeId].collectionSelectedIndex == 0);

	UID_DestroyDocument(doc);
}

void TestMainPanelVisibility(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/main.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["ui_om_main_panel"] = FakeCvar{"play", 0};
	st.cvars["ui_om_settings_tab"] = FakeCvar{"input", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("ui/modern/main.xml", xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);

	uid_node_id_t playPanel = doc->idIndex["panel_play"];
	uid_node_id_t settingsPanel = doc->idIndex["panel_settings"];
	const char *vis = doc->nodes[(size_t)playPanel].properties.GetCStr("visible", "false");
	CHECK(vis && std::strcmp(vis, "true") == 0);
	vis = doc->nodes[(size_t)settingsPanel].properties.GetCStr("visible", "true");
	CHECK(vis && std::strcmp(vis, "false") == 0);

	st.cvars["ui_om_main_panel"].value = "settings";
	UID_SyncBindings(doc, &be);
	vis = doc->nodes[(size_t)playPanel].properties.GetCStr("visible", "true");
	CHECK(vis && std::strcmp(vis, "false") == 0);
	vis = doc->nodes[(size_t)settingsPanel].properties.GetCStr("visible", "false");
	CHECK(vis && std::strcmp(vis, "true") == 0);

	UID_DestroyDocument(doc);
}

void TestInvokeRegistry(void)
{
	static int s_navCount = 0;
	struct Local {
		static bool NavHandler(void *userdata)
		{
			(void)userdata;
			s_navCount++;
			return true;
		}
	};

	UID_ClearInvokes();
	CHECK(!UID_HasInvoke("test-nav"));
	CHECK(UID_RegisterInvoke("test-nav", Local::NavHandler, nullptr));
	CHECK(UID_HasInvoke("test-nav"));
	CHECK(UID_Invoke("test-nav"));
	CHECK(s_navCount == 1);
	CHECK(!UID_Invoke("missing"));
	UID_UnregisterInvoke("test-nav");
	CHECK(!UID_HasInvoke("test-nav"));
}

void TestMainXmlRuntime(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/main.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["r_fullscreen"] = FakeCvar{"1", 0};
	st.cvars["r_noborder"] = FakeCvar{"0", 0};
	st.cvars["ui_selected_server"] = FakeCvar{"127.0.0.1:12203", 0};
	uid_backend_t be = MakeFakeBackend(&st);

	uid_parse_io_t parseIo = MakeTestParseIo();
	CHECK(UID_ParseXml("ui/modern/main.xml", xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	g_fakeServersRevision = 1;
	UID_SyncBindings(doc, &be);
	g_fakeServersRevision = 2;
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 1280, 720, 1.0f, 1.0f, &be, &diags) == UID_OK);

	UID_DestroyDocument(doc);
}

/* Added in OPM: type=relative modal panel flips/clamps/scrolls vs opener. */
static void OpenRelativeFromOpener(
	uid_document_t *doc,
	FakeBackendState *st,
	uid_backend_t *be,
	uid_diag_list_t *diags,
	uid_node_id_t openerId,
	int logicalW,
	int logicalH
)
{
	doc->modalOpenerNode = openerId;
	st->cvars["ui_om_modal"].value = "pop";
	UID_SyncBindings(doc, be);
	CHECK(UID_IsModalActive(doc));
	CHECK(UID_LayoutDocument(doc, logicalW, logicalH, 1.0f, 1.0f, be, diags) == UID_OK);
}

void TestOverlayViewportBounds(void)
{
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);
	st.cvars["ui_om_modal"] = FakeCvar{"", 0};

	/* Near bottom: panel flips above opener. */
	{
		const char *flipXml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <modals>
      <modal id="pop" type="relative">
        <container type="overlap" width="100%" height="100%" fill="#00000000">
          <button width="100%" height="100%" fill="#00000000"><on event="click"><hide-modal/></on></button>
          <container id="panel" role="relative-panel" type="vertical" width="100%" height="auto" overflow="scroll" fill="#101010FF">
            <button width="100%" height="28px">A</button>
            <button width="100%" height="28px">B</button>
            <button width="100%" height="28px">C</button>
            <button width="100%" height="28px">D</button>
          </container>
        </container>
      </modal>
    </modals>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="200px" height="80px" valign="end">
      <select id="mode" width="100px" height="24px" modal="pop">
        <option value="a" label="A"/>
        <option value="b" label="B"/>
        <option value="c" label="C"/>
        <option value="d" label="D"/>
      </select>
    </container>
  </canvas>
</ui>
)";
		uid_document_t *doc = UID_CreateDocument();
		CHECK(UID_ParseXml("flip.xml", flipXml, std::strlen(flipXml), &lim, nullptr, doc, &diags) == UID_OK);
		CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
		CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
		CHECK(UID_LayoutDocument(doc, 200, 80, 1.0f, 1.0f, &be, &diags) == UID_OK);
		uid_node_id_t sid = NodeId(doc, "mode");
		CHECK(sid >= 0);
		OpenRelativeFromOpener(doc, &st, &be, &diags, sid, 200, 80);
		uid_node_id_t panelId = NodeId(doc, "panel");
		CHECK(panelId >= 0);
		const uid_node_state_t &opener = doc->states[(size_t)sid];
		const uid_node_state_t &panel = doc->states[(size_t)panelId];
		CHECK(panel.borderBox.y + panel.borderBox.h <= 80.0f + 0.5f);
		CHECK(panel.borderBox.y + panel.borderBox.h <= opener.borderBox.y + 0.5f);
		CHECK_EQ_F(panel.borderBox.w, opener.borderBox.w, 0.5);
		UID_DestroyDocument(doc);
		st.cvars["ui_om_modal"].value = "";
	}

	/* Near right edge: panel X clamped into viewport. */
	{
		const char *clampInline = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <modals>
      <modal id="pop" type="relative">
        <container type="overlap" width="100%" height="100%" fill="#00000000">
          <button width="100%" height="100%" fill="#00000000"><on event="click"><hide-modal/></on></button>
          <container id="panel" role="relative-panel" type="vertical" width="100%" height="auto" overflow="scroll" fill="#101010FF">
            <button width="100%" height="28px">A</button>
            <button width="100%" height="28px">B</button>
          </container>
        </container>
      </modal>
    </modals>
  </definitions>
  <canvas>
    <container id="root" type="horizontal" width="200px" height="80px" halign="end">
      <select id="mode" width="120px" height="24px" modal="pop">
        <option value="a" label="A"/>
        <option value="b" label="B"/>
      </select>
    </container>
  </canvas>
</ui>
)";
		uid_document_t *doc = UID_CreateDocument();
		CHECK(UID_ParseXml("clamp.xml", clampInline, std::strlen(clampInline), &lim, nullptr, doc, &diags) == UID_OK);
		CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
		CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
		CHECK(UID_LayoutDocument(doc, 200, 80, 1.0f, 1.0f, &be, &diags) == UID_OK);
		uid_node_id_t sid = NodeId(doc, "mode");
		CHECK(sid >= 0);
		OpenRelativeFromOpener(doc, &st, &be, &diags, sid, 200, 80);
		uid_node_id_t panelId = NodeId(doc, "panel");
		CHECK(panelId >= 0);
		const uid_node_state_t &panel = doc->states[(size_t)panelId];
		CHECK(panel.borderBox.x + panel.borderBox.w <= 200.0f + 0.5f);
		CHECK(panel.borderBox.x >= -0.5f);
		UID_DestroyDocument(doc);
		st.cvars["ui_om_modal"].value = "";
	}

	/* Soft max ~5 rows; wheel scrolls the relative-panel. */
	{
		std::string rows;
		for (int i = 0; i < 12; ++i) {
			rows += "<button width=\"100%\" height=\"28px\">R" + std::to_string(i) + "</button>";
		}
		std::string capXml =
			R"(<ui version="1"><definitions><defaults type="vertical" width="100%" height="100%"/><modals><modal id="pop" type="relative"><container type="overlap" width="100%" height="100%" fill="#00000000"><button width="100%" height="100%" fill="#00000000"><on event="click"><hide-modal/></on></button><container id="panel" role="relative-panel" type="vertical" width="100%" height="auto" overflow="scroll" fill="#101010FF">)" +
			rows +
			R"(</container></container></modal></modals></definitions><canvas><container id="root" type="vertical" width="400px" height="400px" valign="start"><button id="opener" width="160px" height="28px"><on event="click"><show-modal id="pop"/></on>Open</button></container></canvas></ui>)";
		uid_document_t *doc = UID_CreateDocument();
		CHECK(UID_ParseXml("cap.xml", capXml.c_str(), capXml.size(), &lim, nullptr, doc, &diags) == UID_OK);
		CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
		CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
		CHECK(UID_LayoutDocument(doc, 400, 400, 1.0f, 1.0f, &be, &diags) == UID_OK);
		uid_node_id_t openerId = NodeId(doc, "opener");
		CHECK(openerId >= 0);
		OpenRelativeFromOpener(doc, &st, &be, &diags, openerId, 400, 400);
		uid_node_id_t panelId = NodeId(doc, "panel");
		CHECK(panelId >= 0);
		uid_node_state_t &panel = doc->states[(size_t)panelId];
		CHECK(panel.borderBox.h + 0.5f < panel.contentExtentH);
		CHECK(panel.borderBox.h <= 28.0f * 5.0f + 0.5f);
		const float scrollBefore = panel.scrollY;
		uid_pointer_state_t ptr{};
		ptr.x = panel.borderBox.x + panel.borderBox.w * 0.5f;
		ptr.y = panel.borderBox.y + panel.borderBox.h * 0.5f;
		ptr.wheel = -1;
		UID_HandlePointer(doc, &ptr, 0, &be);
		CHECK(panel.scrollY > scrollBefore);
		const float scrolled = panel.scrollY;
		/* Relayout must not wipe scroll (measure pass used to clamp scrollY to 0). */
		CHECK(UID_LayoutDocument(doc, 400, 400, 1.0f, 1.0f, &be, &diags) == UID_OK);
		CHECK(panel.scrollY + 0.5f >= scrolled);
		UID_DestroyDocument(doc);
		st.cvars["ui_om_modal"].value = "";
	}

	/* Resize viewport keeps panel on-screen. */
	{
		const char *flipInline = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <modals>
      <modal id="pop" type="relative">
        <container type="overlap" width="100%" height="100%" fill="#00000000">
          <button width="100%" height="100%" fill="#00000000"><on event="click"><hide-modal/></on></button>
          <container id="panel" role="relative-panel" type="vertical" width="100%" height="auto" overflow="scroll" fill="#101010FF">
            <button width="100%" height="28px">A</button>
            <button width="100%" height="28px">B</button>
            <button width="100%" height="28px">C</button>
            <button width="100%" height="28px">D</button>
          </container>
        </container>
      </modal>
    </modals>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="200px" height="80px" valign="end">
      <select id="mode" width="100px" height="24px" modal="pop">
        <option value="a" label="A"/>
        <option value="b" label="B"/>
        <option value="c" label="C"/>
        <option value="d" label="D"/>
      </select>
    </container>
  </canvas>
</ui>
)";
		uid_document_t *doc = UID_CreateDocument();
		CHECK(UID_ParseXml("resize.xml", flipInline, std::strlen(flipInline), &lim, nullptr, doc, &diags) == UID_OK);
		CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
		CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
		CHECK(UID_LayoutDocument(doc, 200, 80, 1.0f, 1.0f, &be, &diags) == UID_OK);
		uid_node_id_t sid = NodeId(doc, "mode");
		CHECK(sid >= 0);
		OpenRelativeFromOpener(doc, &st, &be, &diags, sid, 200, 80);
		CHECK(UID_LayoutDocument(doc, 200, 50, 1.0f, 1.0f, &be, &diags) == UID_OK);
		uid_node_id_t panelId = NodeId(doc, "panel");
		CHECK(panelId >= 0);
		const uid_node_state_t &panel = doc->states[(size_t)panelId];
		CHECK(panel.borderBox.y + panel.borderBox.h <= 50.0f + 0.5f);
		CHECK(panel.borderBox.y >= -0.5f);
		UID_DestroyDocument(doc);
	}
}

/* Scroll containers compile when default template is absent (engine fallback chrome). */
void TestScrollbarMissingDefaultTemplate(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%" padding="0" margin="0" gap="0"/>
  </definitions>
  <canvas>
    <container id="scroller" type="vertical" width="120px" height="60px" overflow="scroll" gap="0">
      <container width="100%" height="50px"/>
      <container width="100%" height="50px"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("legacy_scroll.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	const uid_node_id_t scrollerId = NodeId(doc, "scroller");
	CHECK(scrollerId >= 0);
	if (scrollerId >= 0) {
		CHECK(UID_FindChildOfKind(doc, scrollerId, UID_NODE_SCROLLBAR) < 0);
	}
	UID_DestroyDocument(doc);
}

/* Added in OPM: templated scrollbar chrome on overflow=scroll containers. */
void TestScrollbarTemplated(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%" padding="0" margin="0" gap="0"/>
    <templates>
      <template id="scrollbar-default">
        <scrollbar axis="vertical" width="8px">
          <track fill="#000000FF" shape="rectangle"/>
          <thumb id="thumb" width="8px" halign="center" valign="start"
                 fill="#1A6FD4FF" shape="rectangle"/>
        </scrollbar>
      </template>
    </templates>
  </definitions>
  <canvas>
    <container id="scroller" type="vertical" width="200px" height="100px" overflow="scroll"
               scrollbar="scrollbar-default" gap="0">
      <container width="100%" height="40px" fill="#FF0000FF"/>
      <container width="100%" height="40px" fill="#00FF00FF"/>
      <container width="100%" height="40px" fill="#0000FFFF"/>
      <container width="100%" height="40px" fill="#FFFF00FF"/>
      <container width="100%" height="40px" fill="#FF00FFFF"/>
    </container>
  </canvas>
</ui>
)";
	const char *defaultXml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%" padding="0" margin="0" gap="0"/>
    <templates>
      <template id="scrollbar-default">
        <scrollbar axis="vertical" width="8px">
          <track fill="#000000FF" shape="rectangle"/>
          <thumb width="8px" halign="center" valign="start" fill="#1A6FD4FF" shape="rectangle"/>
        </scrollbar>
      </template>
    </templates>
  </definitions>
  <canvas>
    <container id="auto" type="vertical" width="120px" height="60px" overflow="scroll" gap="0">
      <container width="100%" height="50px"/>
      <container width="100%" height="50px"/>
    </container>
  </canvas>
</ui>
)";
	const char *noOverflowXml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%" padding="0" margin="0" gap="0"/>
    <templates>
      <template id="scrollbar-default">
        <scrollbar axis="vertical" width="8px">
          <track fill="#000000FF" shape="rectangle"/>
          <thumb width="8px" halign="center" valign="start" fill="#1A6FD4FF" shape="rectangle"/>
        </scrollbar>
      </template>
    </templates>
  </definitions>
  <canvas>
    <container id="fits" type="vertical" width="200px" height="100px" overflow="scroll"
               scrollbar="scrollbar-default" gap="0">
      <container width="100%" height="40px" fill="#FF0000FF"/>
      <container width="100%" height="40px" fill="#00FF00FF"/>
    </container>
  </canvas>
</ui>
)";
	const char *tallMinXml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%" padding="0" margin="0" gap="0"/>
    <templates>
      <template id="scrollbar-default">
        <scrollbar axis="vertical" width="8px">
          <track fill="#000000FF" shape="rectangle"/>
          <thumb width="8px" halign="center" valign="start" fill="#1A6FD4FF" shape="rectangle"/>
        </scrollbar>
      </template>
    </templates>
  </definitions>
  <canvas>
    <container id="tall" type="vertical" width="200px" height="100px" overflow="scroll"
               scrollbar="scrollbar-default" gap="0">
      <container width="100%" height="500px"/>
      <container width="100%" height="500px"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("scrollbar.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	uid_node_id_t scrollerId = NodeId(doc, "scroller");
	uid_node_id_t chromeId = -1;
	CHECK(scrollerId >= 0);
	if (scrollerId >= 0) {
		CHECK(doc->nodes[(size_t)scrollerId].scrollbarTemplateId == "scrollbar-default");
	}
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	if (diags.HasErrors()) {
		for (const auto &d : diags.Items()) {
			std::fprintf(stderr, "scrollbar compile: %s\n", d.message.c_str());
		}
	}
	CHECK(!diags.HasErrors());
	if (scrollerId >= 0) {
		chromeId = UID_FindChildOfKind(doc, scrollerId, UID_NODE_SCROLLBAR);
		CHECK(chromeId >= 0);
		CHECK(doc->nodes[(size_t)chromeId].scrollbarGenerated);
	}

	CHECK(UID_LayoutDocument(doc, 400, 300, 1.0f, 1.0f, &be, &diags) == UID_OK);
	if (scrollerId >= 0) {
		uid_node_state_t *cst = &doc->states[(size_t)scrollerId];
		CHECK(cst->scrollbarVisible);
		CHECK(cst->scrollbarTrackRect.w > 0.0f);
		CHECK(cst->scrollbarTrackRect.w <= 16.0f); /* template width="8px", not container width */
		CHECK(cst->scrollbarThumbRect.h > 0.0f);
		/* Proportional thumb: viewport/content × track (5×40px content in 100px viewport → ~0.5). */
		{
			const float expected =
				cst->scrollbarTrackRect.h *
				(cst->contentBox.h / std::max(cst->contentExtentH, 1.0f));
			CHECK(std::fabs(cst->scrollbarThumbRect.h - expected) < 1.5f);
		}
		if (chromeId >= 0) {
			const uid_node_id_t thumbId = UID_FindChildOfKind(doc, chromeId, UID_NODE_SCROLLBAR_THUMB);
			if (thumbId >= 0) {
				const uid_node_state_t *thumbSt = &doc->states[(size_t)thumbId];
				CHECK(thumbSt->borderBox.w >= 6.0f);
				CHECK(thumbSt->borderBox.h >= 10.0f);
				/* Rail lives inside scroller content box on the right edge. */
				CHECK(thumbSt->borderBox.x + thumbSt->borderBox.w <= cst->contentBox.x + cst->contentBox.w + 1.0f);
			}
		}
		const float thumbY0 = cst->scrollbarThumbRect.y;

		cst->scrollY = 50.0f;
		CHECK(UID_LayoutDocument(doc, 400, 300, 1.0f, 1.0f, &be, &diags) == UID_OK);
		CHECK(cst->scrollbarThumbRect.y > thumbY0);

		const float thumbMidX = cst->scrollbarThumbRect.x + cst->scrollbarThumbRect.w * 0.5f;
		const float thumbMidY = cst->scrollbarThumbRect.y + cst->scrollbarThumbRect.h * 0.5f;
		uid_pointer_state_t ptr{};
		ptr.x = thumbMidX;
		ptr.y = thumbMidY;
		ptr.buttons = UID_POINTER_BUTTON_LEFT;
		UID_HandlePointer(doc, &ptr, 0, &be);
		ptr.y = thumbMidY + 20.0f;
		UID_HandlePointer(doc, &ptr, 0, &be);
		ptr.buttons = 0;
		UID_HandlePointer(doc, &ptr, 0, &be);
		CHECK(cst->scrollY > 50.0f);
	}
	UID_DestroyDocument(doc);

	doc = UID_CreateDocument();
	uid_diag_list_t diags2(lim.maxDiagnostics);
	CHECK(UID_ParseXml("scrollbar_default.xml", defaultXml, std::strlen(defaultXml), &lim, nullptr, doc, &diags2) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags2) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags2) == UID_OK);
	if (diags2.HasErrors()) {
		for (const auto &d : diags2.Items()) {
			std::fprintf(stderr, "scrollbar default compile: %s\n", d.message.c_str());
		}
	}
	CHECK(!diags2.HasErrors());
	scrollerId = NodeId(doc, "auto");
	CHECK(scrollerId >= 0);
	if (scrollerId >= 0) {
		CHECK(UID_FindChildOfKind(doc, scrollerId, UID_NODE_SCROLLBAR) >= 0);
	}
	UID_DestroyDocument(doc);

	/* No overflow: content fits viewport → chrome hidden, empty rects. */
	doc = UID_CreateDocument();
	uid_diag_list_t diags3(lim.maxDiagnostics);
	CHECK(UID_ParseXml("scrollbar_no_overflow.xml", noOverflowXml, std::strlen(noOverflowXml), &lim, nullptr, doc, &diags3) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags3) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags3) == UID_OK);
	CHECK(!diags3.HasErrors());
	CHECK(UID_LayoutDocument(doc, 400, 300, 1.0f, 1.0f, &be, &diags3) == UID_OK);
	scrollerId = NodeId(doc, "fits");
	CHECK(scrollerId >= 0);
	if (scrollerId >= 0) {
		uid_node_state_t *cst = &doc->states[(size_t)scrollerId];
		CHECK(!cst->scrollbarVisible);
		CHECK(cst->scrollbarTrackRect.w == 0.0f);
		CHECK(cst->scrollbarTrackRect.h == 0.0f);
		CHECK(cst->scrollbarThumbRect.w == 0.0f);
		CHECK(cst->scrollbarThumbRect.h == 0.0f);
		const uid_node_id_t chromeIdFits = UID_FindChildOfKind(doc, scrollerId, UID_NODE_SCROLLBAR);
		CHECK(chromeIdFits >= 0);
		if (chromeIdFits >= 0) {
			const uid_node_state_t *chromeSt = &doc->states[(size_t)chromeIdFits];
			CHECK(chromeSt->borderBox.w == 0.0f);
			CHECK(chromeSt->borderBox.h == 0.0f);
		}
	}
	UID_DestroyDocument(doc);

	/* Very tall content: thumb clamped to min ~20 authored px. */
	doc = UID_CreateDocument();
	uid_diag_list_t diags4(lim.maxDiagnostics);
	CHECK(UID_ParseXml("scrollbar_tall_min.xml", tallMinXml, std::strlen(tallMinXml), &lim, nullptr, doc, &diags4) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags4) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags4) == UID_OK);
	CHECK(!diags4.HasErrors());
	CHECK(UID_LayoutDocument(doc, 400, 300, 1.0f, 1.0f, &be, &diags4) == UID_OK);
	scrollerId = NodeId(doc, "tall");
	CHECK(scrollerId >= 0);
	if (scrollerId >= 0) {
		uid_node_state_t *cst = &doc->states[(size_t)scrollerId];
		CHECK(cst->scrollbarVisible);
		CHECK(cst->scrollbarThumbRect.h >= 19.0f);
		CHECK(cst->scrollbarThumbRect.h <= 21.0f);
	}
	UID_DestroyDocument(doc);
}

/* Added in OPM: scrollbar-edge content vs border on padded scroll containers. */
void TestScrollbarEdge(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%" padding="0" margin="0" gap="0"/>
    <templates>
      <template id="scrollbar-default">
        <scrollbar axis="vertical" width="8px">
          <track fill="#000000FF" shape="rectangle"/>
          <thumb width="8px" halign="center" valign="start" fill="#1A6FD4FF" shape="rectangle"/>
        </scrollbar>
      </template>
    </templates>
  </definitions>
  <canvas>
    <container type="horizontal" width="100%" height="100%" gap="0">
      <container id="content_edge" type="vertical" width="200px" height="100px" padding="12px"
                 overflow="scroll" scrollbar="scrollbar-default" gap="0">
        <container width="100%" height="40px"/>
        <container width="100%" height="40px"/>
        <container width="100%" height="40px"/>
        <container width="100%" height="40px"/>
        <container width="100%" height="40px"/>
      </container>
      <container id="border_edge" type="vertical" width="200px" height="100px" padding="12px"
                 overflow="scroll" scrollbar="scrollbar-default" scrollbar-edge="border" gap="0">
        <container width="100%" height="40px"/>
        <container width="100%" height="40px"/>
        <container width="100%" height="40px"/>
        <container width="100%" height="40px"/>
        <container width="100%" height="40px"/>
      </container>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("scrollbar_edge.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());

	const uid_node_id_t contentId = NodeId(doc, "content_edge");
	const uid_node_id_t borderId = NodeId(doc, "border_edge");
	CHECK(contentId >= 0);
	CHECK(borderId >= 0);

	CHECK(UID_LayoutDocument(doc, 400, 300, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_state_t *contentSt = &doc->states[(size_t)contentId];
	uid_node_state_t *borderSt = &doc->states[(size_t)borderId];
	CHECK(contentSt->scrollbarVisible);
	CHECK(borderSt->scrollbarVisible);

	const float contentTrailing = contentSt->scrollbarTrackRect.x + contentSt->scrollbarTrackRect.w;
	const float borderTrailing = borderSt->scrollbarTrackRect.x + borderSt->scrollbarTrackRect.w;
	CHECK(contentTrailing <= contentSt->contentBox.x + contentSt->contentBox.w + 1.0f);
	CHECK(borderTrailing <= borderSt->borderBox.x + borderSt->borderBox.w + 1.0f);
	CHECK(borderTrailing > contentTrailing + 8.0f); /* 12px padding minus rail width */

	if (borderId >= 0) {
		const uid_node_id_t chromeId = UID_FindChildOfKind(doc, borderId, UID_NODE_SCROLLBAR);
		if (chromeId >= 0) {
			const uid_node_id_t trackId = UID_FindChildOfKind(doc, chromeId, UID_NODE_SCROLLBAR_TRACK);
			if (trackId >= 0) {
				const uid_node_state_t *trackSt = &doc->states[(size_t)trackId];
				CHECK(trackSt->borderBox.w > 0.0f);
				CHECK(trackSt->effectiveClip.w > 0.0f);
			}
		}
	}

	UID_DestroyDocument(doc);
}

/* Wheel over interactive child scrolls overflow=scroll panel (settings-style). */
void TestScrollWheelOverScrollContainer(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%" padding="0" margin="0" gap="0"/>
    <templates>
      <template id="scrollbar-default">
        <scrollbar axis="vertical" width="8px">
          <track fill="#000000FF" shape="rectangle"/>
          <thumb width="8px" halign="center" valign="start" fill="#1A6FD4FF" shape="rectangle"/>
        </scrollbar>
      </template>
    </templates>
  </definitions>
  <canvas>
    <container id="scroller" type="vertical" width="200px" height="100px" padding="12px"
               overflow="scroll" scrollbar="scrollbar-default" scrollbar-edge="border" gap="0">
      <button id="row0" width="100%" height="40px">Row 0</button>
      <button id="row1" width="100%" height="40px">Row 1</button>
      <button id="row2" width="100%" height="40px">Row 2</button>
      <button id="row3" width="100%" height="40px">Row 3</button>
      <button id="row4" width="100%" height="40px">Row 4</button>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("wheel_scroll.xml", xml, std::strlen(xml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_LayoutDocument(doc, 400, 300, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_node_id_t scrollerId = NodeId(doc, "scroller");
	const uid_node_id_t rowId = NodeId(doc, "row0");
	CHECK(scrollerId >= 0);
	CHECK(rowId >= 0);
	if (scrollerId >= 0 && rowId >= 0) {
		uid_node_state_t *scrollerSt = &doc->states[(size_t)scrollerId];
		const uid_node_state_t *rowSt = &doc->states[(size_t)rowId];
		CHECK(scrollerSt->scrollbarVisible);
		CHECK(scrollerSt->scrollY == 0.0f);

		uid_pointer_state_t ptr{};
		ptr.x = rowSt->borderBox.x + rowSt->borderBox.w * 0.5f;
		ptr.y = rowSt->borderBox.y + rowSt->borderBox.h * 0.5f;
		ptr.wheel = -1;
		UID_HandlePointer(doc, &ptr, 0, &be);
		CHECK(scrollerSt->scrollY > 0.0f);
	}
	UID_DestroyDocument(doc);
}

void TestMenuMapViewResolve(void)
{
	static const char kXml[] = R"(<ui version="1">
  <definitions>
    <sources>
      <source id="menu-map-views" default="remagen">
        <item value="remagen" label="Remagen"
              bsp="maps/dm/mohdm3.bsp"
              vieworg="947.23,-649.70,-68.50"
              pitch="-31.15" yaw="-67.03" roll="0"
              fov="80"/>
        <item value="alt" label="Alt"
              bsp="maps/dm/other.bsp"
              vieworg="1,2,3"
              pitch="10" yaw="20" roll="30"
              fov="70"/>
      </source>
    </sources>
  </definitions>
  <canvas/>
</ui>)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uir_menu_map_view_t view;

	CHECK(UID_ParseXml("menu_map_view.xml", kXml, std::strlen(kXml), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());

	CHECK(UID_ResolveMenuMapView(doc, "remagen", &view));
	CHECK(std::strcmp(view.id, "remagen") == 0);
	CHECK(std::strcmp(view.bsp, "maps/dm/mohdm3.bsp") == 0);
	CHECK_EQ_F(view.vieworg[0], 947.23f, 1e-3f);
	CHECK_EQ_F(view.pitch, -31.15f, 1e-3f);
	CHECK_EQ_F(view.fov, 80.0f, 1e-3f);

	CHECK(UID_ResolveMenuMapView(doc, "alt", &view));
	CHECK(std::strcmp(view.bsp, "maps/dm/other.bsp") == 0);
	CHECK_EQ_F(view.vieworg[2], 3.0f, 1e-3f);
	CHECK_EQ_F(view.yaw, 20.0f, 1e-3f);

	CHECK(UID_ResolveMenuMapView(doc, "missing", &view));
	CHECK(std::strcmp(view.id, "remagen") == 0);

	UID_DestroyDocument(doc);
}

void TestImportHappyPath(void)
{
	g_testImportFiles.clear();
	g_testImportFiles["ui/modern/lib/mini.xml"] = R"(<ui-library version="1">
  <templates>
    <template id="mini-label">
      <props>
        <prop name="label" type="string" required="true"/>
      </props>
      <label width="auto" height="auto">{template.label}</label>
    </template>
  </templates>
</ui-library>
)";

	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <import src="ui/modern/lib/mini.xml"/>
  </definitions>
  <canvas>
    <use template="mini-label" label="hello" width="100px" height="20px"/>
  </canvas>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	CHECK(UID_ParseXml("ui/modern/main.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(doc->definitions.templates.count("mini-label") == 1);
	UID_DestroyDocument(doc);
	g_testImportFiles.clear();
}

void TestImportRelativePath(void)
{
	g_testImportFiles.clear();
	g_testImportFiles["ui/modern/lib/child/theme.xml"] = R"(<ui-library version="1">
  <fonts>
    <font id="body" src="fonts/Oswald-Medium.ttf" weight="500"/>
  </fonts>
</ui-library>
)";
	g_testImportFiles["ui/modern/lib/parent.xml"] = R"(<ui-library version="1">
  <import src="child/theme.xml"/>
</ui-library>
)";

	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <import src="ui/modern/lib/parent.xml"/>
  </definitions>
  <canvas/>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	CHECK(UID_ParseXml("ui/modern/main.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(doc->definitions.fonts.count("body") == 1);
	UID_DestroyDocument(doc);
	g_testImportFiles.clear();
}

void TestImportCycleRejected(void)
{
	g_testImportFiles.clear();
	g_testImportFiles["ui/modern/a.xml"] = R"(<ui-library version="1">
  <import src="ui/modern/b.xml"/>
</ui-library>
)";
	g_testImportFiles["ui/modern/b.xml"] = R"(<ui-library version="1">
  <import src="ui/modern/a.xml"/>
</ui-library>
)";

	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <import src="ui/modern/a.xml"/>
  </definitions>
  <canvas/>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	CHECK(UID_ParseXml("ui/modern/main.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) != UID_OK);
	CHECK(diags.HasErrors());
	UID_DestroyDocument(doc);
	g_testImportFiles.clear();
}

void TestImportDotDotRejected(void)
{
	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <import src="../secret.xml"/>
  </definitions>
  <canvas/>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	CHECK(UID_ParseXml("ui/modern/main.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) != UID_OK);
	UID_DestroyDocument(doc);
}

void TestImportOverrideWarning(void)
{
	g_testImportFiles.clear();
	g_testImportFiles["ui/modern/lib/base.xml"] = R"(<ui-library version="1">
  <templates>
    <template id="row">
      <label width="auto" height="auto">imported</label>
    </template>
  </templates>
</ui-library>
)";

	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <import src="ui/modern/lib/base.xml"/>
    <templates>
      <template id="row">
        <label width="auto" height="auto">local</label>
      </template>
    </templates>
  </definitions>
  <canvas/>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	CHECK(UID_ParseXml("ui/modern/main.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) == UID_OK);
	bool sawOverride = false;
	for (const uid_diag_t &d : diags.Items()) {
		if (d.severity == UID_SEVERITY_WARNING && d.message.find("overrides") != std::string::npos) {
			sawOverride = true;
			break;
		}
	}
	CHECK(sawOverride);
	CHECK(doc->definitions.templates["row"].nodes.size() > 0);
	UID_DestroyDocument(doc);
	g_testImportFiles.clear();
}

void TestImportDefaultsMerge(void)
{
	g_testImportFiles.clear();
	g_testImportFiles["ui/modern/lib/theme.xml"] = R"(<ui-library version="1">
  <defaults fill="#FF0000FF" font-size="12px"/>
</ui-library>
)";

	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <import src="ui/modern/lib/theme.xml"/>
    <defaults font-size="16px" color="#FFFFFFFF"/>
  </definitions>
  <canvas/>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	CHECK(UID_ParseXml("ui/modern/main.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(std::strcmp(doc->definitions.defaults.GetCStr("fill", ""), "#FF0000FF") == 0);
	CHECK(std::strcmp(doc->definitions.defaults.GetCStr("font-size", ""), "16px") == 0);
	CHECK(std::strcmp(doc->definitions.defaults.GetCStr("color", ""), "#FFFFFFFF") == 0);
	UID_DestroyDocument(doc);
	g_testImportFiles.clear();
}

void TestImportRequiresIo(void)
{
	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <import src="ui/modern/lib/theme.xml"/>
  </definitions>
  <canvas/>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("ui/modern/main.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) != UID_OK);
	UID_DestroyDocument(doc);
}

void TestImageRegistryParse(void)
{
	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <images>
      <image id="panel" src="ui/modern/textures/panel.png"/>
      <image id="logo" src="ui/modern/textures/logo.tga"/>
    </images>
  </definitions>
  <canvas/>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("test.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(doc->definitions.images.size() == 2);
	CHECK(doc->definitions.images.count("panel") == 1);
	CHECK(doc->definitions.images.count("logo") == 1);
	UID_DestroyDocument(doc);
}

void TestImageRegistryRejectsJpg(void)
{
	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <images>
      <image id="bad" src="textures/foo.jpg"/>
    </images>
  </definitions>
  <canvas/>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("test.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) != UID_OK);
	UID_DestroyDocument(doc);
}

void TestBackgroundImagePaint(void)
{
	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <shapes>
      <shape id="skew-rect">
        <props>
          <prop name="skewl" type="length" default="12px"/>
          <prop name="skewr" type="length" default="12px"/>
        </props>
        <path fill="{parent.fill}"
          d="M 0 {parent.height} L {shape.skewl} 0 L {parent.width} 0 L {parent.width - shape.skewr} {parent.height} Z"/>
      </shape>
    </shapes>
    <images>
      <image id="panel" src="ui/modern/textures/panel.png"/>
    </images>
  </definitions>
  <canvas>
    <container id="root" width="200px" height="120px" shape="skew-rect" skewl="12px" skewr="12px"
               background-image="panel" background-fit="cover" fill="#00000000"/>
  </canvas>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("test.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);

	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);
	CHECK(UID_LayoutDocument(doc, 800, 600, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_PaintChrome(doc, &be);

	CHECK(!st.imageDrawLog.empty());
	CHECK(st.imageDrawLog[0].find("fit=3") != std::string::npos); /* cover */
	CHECK(st.imageDrawLog[0].find("clips=1") != std::string::npos);

	UID_DestroyDocument(doc);
}

void TestBackgroundImageRectFastPath(void)
{
	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <images>
      <image id="panel" src="ui/modern/textures/panel.png"/>
    </images>
  </definitions>
  <canvas>
    <container id="root" width="100px" height="50px" background-image="panel" background-fit="stretch" fill="#00000000"/>
  </canvas>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("test.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);

	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);
	CHECK(UID_LayoutDocument(doc, 400, 300, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_PaintChrome(doc, &be);

	CHECK(!st.imageDrawLog.empty());
	CHECK(st.imageDrawLog[0].find("clips=0") != std::string::npos);

	UID_DestroyDocument(doc);
}

/* Added in OPM: leaf <image> parse, aspect auto width, paint fit=contain. */
void TestLeafImageAspectLayout(void)
{
	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <images>
      <image id="nade" src="ui/modern/textures/nade.png"/>
      <image id="rifle" src="ui/modern/textures/rifle.png"/>
    </images>
  </definitions>
  <canvas>
    <container id="root" type="horizontal" width="400px" height="40px" gap="4px" fill="#00000000">
      <image id="icon_nade" src="nade" height="20px" width="auto" fit="contain"/>
      <image id="icon_rifle" src="rifle" height="20px" width="auto"/>
      <image id="icon_fixed" src="nade" width="40px" height="20px" fit="contain"/>
    </container>
  </canvas>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("leaf_image.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);

	FakeBackendState st;
	st.imageSizes["ui/modern/textures/nade.png"] = {23.0f, 40.0f};
	st.imageSizes["ui/modern/textures/rifle.png"] = {144.0f, 40.0f};
	uid_backend_t be = MakeFakeBackend(&st);
	CHECK(UID_LayoutDocument(doc, 800, 600, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const uid_node_id_t nadeId = doc->idIndex.at("icon_nade");
	const uid_node_id_t rifleId = doc->idIndex.at("icon_rifle");
	const uid_node_id_t fixedId = doc->idIndex.at("icon_fixed");
	CHECK_EQ_F(doc->states[(size_t)nadeId].borderBox.h, 20.0, 0.5);
	CHECK_EQ_F(doc->states[(size_t)nadeId].borderBox.w, 20.0 * (23.0 / 40.0), 0.75);
	CHECK_EQ_F(doc->states[(size_t)rifleId].borderBox.h, 20.0, 0.5);
	CHECK_EQ_F(doc->states[(size_t)rifleId].borderBox.w, 20.0 * (144.0 / 40.0), 0.75);
	CHECK_EQ_F(doc->states[(size_t)fixedId].borderBox.w, 40.0, 0.5);
	CHECK_EQ_F(doc->states[(size_t)fixedId].borderBox.h, 20.0, 0.5);

	UID_PaintChrome(doc, &be);
	CHECK(st.imageDrawLog.size() >= 3);
	CHECK(st.imageDrawLog[0].find("fit=2") != std::string::npos); /* contain */

	UID_DestroyDocument(doc);
}

/* Added in OPM: nested auto parents must inherit aspect-derived image width
 * (killfeed: overlap → source → foreach → image height=20 width=auto). */
void TestLeafImageNestedIntrinsicWidth(void)
{
	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <images>
      <image id="kf" src="ui/modern/textures/kf.png"/>
    </images>
  </definitions>
  <canvas>
    <container id="row" type="horizontal" width="400px" height="40px" gap="6px" fill="#00000000">
      <container id="wrap" type="overlap" width="auto" height="20px" fill="#00000000">
        <container id="src" width="auto" height="20px" fill="#00000000">
          <image id="icon" src="kf" height="20px" width="auto" fit="contain"/>
        </container>
      </container>
      <label id="victim" font-size="14px" width="auto" height="auto">Victim</label>
    </container>
  </canvas>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("leaf_image_nested.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);

	FakeBackendState st;
	st.imageSizes["ui/modern/textures/kf.png"] = {144.0f, 40.0f};
	uid_backend_t be = MakeFakeBackend(&st);
	CHECK(UID_LayoutDocument(doc, 800, 600, 1.0f, 1.0f, &be, &diags) == UID_OK);

	const float expectW = 20.0f * (144.0f / 40.0f);
	const uid_node_id_t iconId = doc->idIndex.at("icon");
	const uid_node_id_t wrapId = doc->idIndex.at("wrap");
	const uid_node_id_t victimId = doc->idIndex.at("victim");
	CHECK_EQ_F(doc->states[(size_t)iconId].borderBox.w, expectW, 0.75);
	CHECK_EQ_F(doc->states[(size_t)wrapId].borderBox.w, expectW, 0.75);
	/* Victim must start immediately after wrap + gap, not after full 144px tex width. */
	const float wrapRight = doc->states[(size_t)wrapId].borderBox.x + doc->states[(size_t)wrapId].borderBox.w;
	CHECK_EQ_F(doc->states[(size_t)victimId].borderBox.x, wrapRight + 6.0, 1.0);

	UID_DestroyDocument(doc);
}

void TestLeafImageUnknownRejected(void)
{
	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <images>
      <image id="panel" src="ui/modern/textures/panel.png"/>
    </images>
  </definitions>
  <canvas>
    <image src="missing-icon" width="20px" height="20px"/>
  </canvas>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("leaf_bad.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) != UID_OK);
	UID_DestroyDocument(doc);
}

void TestLeafImageItemFieldBind(void)
{
	static const char kDoc[] = R"(<ui version="1">
  <definitions>
    <images>
      <image id="modernhud-nade" src="ui/modern/textures/nade.png"/>
      <image id="modernhud-rifle" src="ui/modern/textures/rifle.png"/>
    </images>
    <sources>
      <source id="kf">
        <item value="nade" label="Nade" image="modernhud-nade"/>
        <item value="rifle" label="Rifle" image="modernhud-rifle"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="root" type="horizontal" width="400px" height="40px" fill="#00000000"
               source="kf" bind="cvar:ui_test_kf">
      <foreach mode="selected" width="auto" height="20px">
        <image src="{item.field.image}" height="20px" width="auto"/>
      </foreach>
    </container>
  </canvas>
</ui>)";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("leaf_kf.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);

	FakeBackendState st;
	st.cvars["ui_test_kf"] = FakeCvar{"nade", 0};
	st.imageSizes["ui/modern/textures/nade.png"] = {23.0f, 40.0f};
	st.imageSizes["ui/modern/textures/rifle.png"] = {144.0f, 40.0f};
	uid_backend_t be = MakeFakeBackend(&st);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 800, 600, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_PaintChrome(doc, &be);
	CHECK(!st.imageDrawLog.empty());
	CHECK(st.imageDrawLog[0].find("nade.png") != std::string::npos);

	UID_DestroyDocument(doc);
}

/* Added in OPM: atlas gradient fill paints via drawGradient; shape clip; no solid fill. */
void TestGradientFillPaint(void)
{
	static const char kDoc[] = R"XML(<ui version="1">
  <definitions>
    <shapes>
      <shape id="skew-rect">
        <props>
          <prop name="skewl" type="length" default="12px"/>
          <prop name="skewr" type="length" default="12px"/>
        </props>
        <path fill="{parent.fill}"
          d="M 0 {parent.height} L {shape.skewl} 0 L {parent.width} 0 L {parent.width - shape.skewr} {parent.height} Z"/>
      </shape>
    </shapes>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100%" height="100%" gap="8px" fill="#00000000">
      <container id="grad" width="200px" height="80px" shape="skew-rect" skewl="12px" skewr="12px"
                 fill="linear(180deg, #00000000, #000000B3)"
                 stroke="#FFFFFFFF" stroke-width="1px"/>
      <container id="tern" width="100px" height="40px"
                 fill="{cvar.ui_test_on == 1 ? linear(90deg, #FF0000FF, #0000FFFF) : #00FF00FF}"/>
    </container>
  </canvas>
</ui>)XML";

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	CHECK(UID_ParseXml("test.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);

	FakeBackendState st;
	st.cvars["ui_test_on"] = FakeCvar{"1", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	CHECK(UID_LayoutDocument(doc, 800, 600, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	UID_PaintChrome(doc, &be);

	bool sawGrad = false;
	bool sawGradClip = false;
	bool sawSolidFillOnGrad = false;
	for (const std::string &line : st.drawLog) {
		if (line.find("gradient 'linear(180deg") != std::string::npos) {
			sawGrad = true;
			if (line.find("clips=1") != std::string::npos) {
				sawGradClip = true;
			}
		}
		if (line.find("path ") != std::string::npos && line.find("fill_a=") != std::string::npos) {
			/* Stroke-only path for the skew panel: fill alpha should be 0. */
			if (line.find("fill_a=0.00") == std::string::npos &&
				line.find("xywh=0.0,0.0,200.0") != std::string::npos) {
				sawSolidFillOnGrad = true;
			}
		}
		if (line.find("gradient 'linear(90deg") != std::string::npos) {
			sawGrad = true;
		}
	}
	CHECK(sawGrad);
	CHECK(sawGradClip);
	CHECK(!sawSolidFillOnGrad);

	st.drawLog.clear();
	st.cvars["ui_test_on"] = FakeCvar{"0", 0};
	UID_SyncBindings(doc, &be);
	UID_PaintChrome(doc, &be);
	bool sawSolidRect = false;
	bool sawTernGrad = false;
	for (const std::string &line : st.drawLog) {
		if (line.find("gradient 'linear(90deg") != std::string::npos) {
			sawTernGrad = true;
		}
		if (line.find("rect ") == 0 && line.find("100.0") != std::string::npos) {
			sawSolidRect = true;
		}
	}
	CHECK(sawSolidRect);
	CHECK(!sawTernGrad);

	UID_DestroyDocument(doc);
}

/* Added in OPM: crosshair settings preview paints via shape instances + cvar-backed props. */
void TestCrosshairDisplayShape(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto"/>
    <shapes>
      <shape id="crosshair-dot" width="64px" height="64px">
        <props>
          <prop name="size" type="length" default="4px"/>
        </props>
        <path fill="{parent.fill}"
              d="M {parent.width * 0.5 - shape.size * 0.5}
                 {parent.height * 0.5 - shape.size * 0.5}
                 L {parent.width * 0.5 + shape.size * 0.5}
                 {parent.height * 0.5 - shape.size * 0.5}
                 L {parent.width * 0.5 + shape.size * 0.5}
                 {parent.height * 0.5 + shape.size * 0.5}
                 L {parent.width * 0.5 - shape.size * 0.5}
                 {parent.height * 0.5 + shape.size * 0.5} Z"/>
      </shape>
    </shapes>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="120px" height="120px" halign="center" valign="center">
      <shape shape="crosshair-dot" width="64px" height="64px"
             fill="cvar-rgba:cg_crosshaircolor_r,cg_crosshaircolor_g,cg_crosshaircolor_b,cg_crosshairalpha"
             stroke="#000000FF" stroke-width="{cvar:cg_crosshair_outlinethickness}px"
             size="{cvar:cg_crosshair_dot_size}px"
             crisp="true"
             visible="{cvar.cg_crosshair_mode == dot}"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["cg_crosshair_mode"] = FakeCvar{"dot", 0};
	st.cvars["cg_crosshaircolor_r"] = FakeCvar{"255", 0};
	st.cvars["cg_crosshaircolor_g"] = FakeCvar{"255", 0};
	st.cvars["cg_crosshaircolor_b"] = FakeCvar{"255", 0};
	st.cvars["cg_crosshairalpha"] = FakeCvar{"255", 0};
	st.cvars["cg_crosshair_outlinethickness"] = FakeCvar{"1", 0};
	st.cvars["cg_crosshair_dot_size"] = FakeCvar{"4", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("xhair_shape.xml", xml, std::strlen(xml), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 120, 120, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_PaintChrome(doc, &be);

	bool sawPath = false;
	for (const std::string &line : st.drawLog) {
		if (line.find("path ") == 0 && line.find("fill_a=1.00") != std::string::npos &&
		    line.find("stroke_a=1.00") != std::string::npos &&
		    line.find("crisp=1") != std::string::npos) {
			sawPath = true;
		}
	}
	CHECK(sawPath);

	UID_DestroyDocument(doc);
}

/*
 * Added in OPM: intrinsic shape px props must not take uiPxScale twice
 * (layout box × view→dest stretch already applies DIP).
 */
void TestIntrinsicShapePropScale(void)
{
	const char *xml = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="auto" height="auto"/>
    <shapes>
      <shape id="bar" width="64px" height="64px">
        <props>
          <prop name="thickness" type="length" default="1px"/>
        </props>
        <path fill="{parent.fill}"
              d="M 0 {parent.height * 0.5 - shape.thickness * 0.5}
                 L {parent.width} {parent.height * 0.5 - shape.thickness * 0.5}
                 L {parent.width} {parent.height * 0.5 + shape.thickness * 0.5}
                 L 0 {parent.height * 0.5 + shape.thickness * 0.5} Z"/>
      </shape>
    </shapes>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="200px" height="200px" halign="center" valign="center">
      <shape id="xhair" shape="bar" width="64px" height="64px" fill="#FFFFFFFF"
             thickness="{cvar:cg_crosshairthickness}px"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.cvars["cg_crosshairthickness"] = FakeCvar{"1", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("intrinsic_scale.xml", xml, std::strlen(xml), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);

	/* uiPxScale=2 → 64px instance lays out to 128×128. */
	CHECK(UID_LayoutDocument(doc, 400, 400, 1.0f, 2.0f, &be, &diags) == UID_OK);
	uid_node_id_t shapeId = NodeId(doc, "xhair");
	CHECK(shapeId >= 0);
	CHECK_EQ_F(doc->states[(size_t)shapeId].borderBox.w, 128.0, 0.75);
	CHECK_EQ_F(doc->states[(size_t)shapeId].borderBox.h, 128.0, 0.75);

	st.drawLog.clear();
	UID_PaintChrome(doc, &be);

	bool saw = false;
	for (const std::string &line : st.drawLog) {
		if (line.find("path ") != 0) {
			continue;
		}
		float x = 0, y = 0, w = 0, h = 0, viewW = 0, viewH = 0;
		if (std::sscanf(
				line.c_str(),
				"path d='%*[^']' xywh=%f,%f,%f,%f view=%fx%f",
				&x,
				&y,
				&w,
				&h,
				&viewW,
				&viewH
			) < 6) {
			continue;
		}
		CHECK_EQ_F(w, 128.0, 0.75);
		CHECK_EQ_F(viewW, 64.0, 0.75);
		/*
		 * Path is in viewBox space. thickness=1 authored → local height 1.0
		 * (not 2.0). Draw-space bar = 1 * (128/64) = 2, not uiPxScale²=4.
		 */
		const size_t d0 = line.find("d='");
		const size_t d1 = (d0 != std::string::npos) ? line.find('\'', d0 + 3) : std::string::npos;
		CHECK(d0 != std::string::npos && d1 != std::string::npos);
		const std::string d = line.substr(d0 + 3, d1 - (d0 + 3));
		float y0 = 0, y1 = 0;
		CHECK(std::sscanf(d.c_str(), "M %*g %g L %*g %*g L %*g %g", &y0, &y1) == 2);
		const float localThickness = std::fabs(y1 - y0);
		CHECK_EQ_F(localThickness, 1.0, 0.15);
		const float drawThickness = localThickness * (w / viewW);
		CHECK_EQ_F(drawThickness, 2.0, 0.2);
		saw = true;
	}
	CHECK(saw);

	/* Same geometry at uiPxScale=1: local 1, draw 1. */
	doc->dirty = static_cast<uid_dirty_flags_t>(doc->dirty | UID_DIRTY_LAYOUT);
	CHECK(UID_LayoutDocument(doc, 400, 400, 1.0f, 1.0f, &be, &diags) == UID_OK);
	CHECK_EQ_F(doc->states[(size_t)shapeId].borderBox.w, 64.0, 0.75);
	st.drawLog.clear();
	UID_PaintChrome(doc, &be);
	saw = false;
	for (const std::string &line : st.drawLog) {
		if (line.find("path ") != 0) {
			continue;
		}
		float x = 0, y = 0, w = 0, h = 0, viewW = 0, viewH = 0;
		if (std::sscanf(
				line.c_str(),
				"path d='%*[^']' xywh=%f,%f,%f,%f view=%fx%f",
				&x,
				&y,
				&w,
				&h,
				&viewW,
				&viewH
			) < 6) {
			continue;
		}
		CHECK_EQ_F(w, 64.0, 0.75);
		const size_t d0 = line.find("d='");
		const size_t d1 = (d0 != std::string::npos) ? line.find('\'', d0 + 3) : std::string::npos;
		CHECK(d0 != std::string::npos && d1 != std::string::npos);
		const std::string d = line.substr(d0 + 3, d1 - (d0 + 3));
		float y0 = 0, y1 = 0;
		CHECK(std::sscanf(d.c_str(), "M %*g %g L %*g %*g L %*g %g", &y0, &y1) == 2);
		CHECK_EQ_F(std::fabs(y1 - y0), 1.0, 0.15);
		CHECK_EQ_F(std::fabs(y1 - y0) * (w / viewW), 1.0, 0.2);
		saw = true;
	}
	CHECK(saw);

	UID_DestroyDocument(doc);
}

static void TestMenuMetadataParse(void)
{
	const char *xml =
		R"(<ui version="1">
  <definitions menu-id="stats" draw-order="6" backdrop="menu-map">
    <fonts/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100px" height="100px"/>
  </canvas>
</ui>)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("menu_meta.xml", xml, std::strlen(xml), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(doc->hasMenuMeta);
	CHECK(doc->menuId == "stats");
	CHECK(doc->drawOrder == 6);
	CHECK(doc->menuBackdrop == UID_MENU_BACKDROP_MENU_MAP);

	UID_DestroyDocument(doc);
}

static void TestMenuMetadataDrawOrderBounds(void)
{
	const char *xml =
		R"(<ui version="1">
  <definitions menu-id="bad" draw-order="10">
    <fonts/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100px" height="100px"/>
  </canvas>
</ui>)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("menu_bad.xml", xml, std::strlen(xml), &lim, &parseIo, doc, &diags) != UID_OK);
	CHECK(diags.HasErrors());

	UID_DestroyDocument(doc);
}

static void TestMenuMetadataMissingDrawOrder(void)
{
	const char *xml =
		R"(<ui version="1">
  <definitions menu-id="orphan">
    <fonts/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100px" height="100px"/>
  </canvas>
</ui>)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("menu_orphan.xml", xml, std::strlen(xml), &lim, &parseIo, doc, &diags) != UID_OK);
	CHECK(diags.HasErrors());

	UID_DestroyDocument(doc);
}

static void TestMenuMetadataPeekFixtures(void)
{
	struct FixtureCase {
		const char *vfsPath;
		const char *menuId;
		int         drawOrder;
		uid_menu_backdrop_t backdrop;
	};

	const FixtureCase cases[] = {
		{"ui/modern/main.xml", "main", 8, UID_MENU_BACKDROP_MENU_MAP},
		{"ui/modern/menus/scoreboard.xml", "scoreboard", 3, UID_MENU_BACKDROP_NONE},
	};

	uid_parse_io_t parseIo = MakeTestParseIo();
	for (const FixtureCase &tc : cases) {
		uid_menu_meta_t meta;
		CHECK(UID_PeekMenuMetadata(tc.vfsPath, &parseIo, &meta, NULL) == UID_OK);
		CHECK(meta.valid);
		CHECK(std::strcmp(meta.menuId, tc.menuId) == 0);
		CHECK(meta.drawOrder == tc.drawOrder);
		CHECK(meta.backdrop == tc.backdrop);
	}
}

static void TestOverlapContainerLayout(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="overlap" width="200px" height="200px" padding="0">
      <container id="layer_a" width="100%" height="100%" fill="#FF0000FF"/>
      <container id="layer_b" width="100%" height="100%" fill="#00FF00FF"/>
      <container id="layer_c" width="100%" height="100%" fill="#0000FFFF"/>
      <container id="corner" width="40px" height="30px" halign="end" valign="end" margin="0 8px 8px 0" fill="#FFFFFFFF"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("overlap.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_LayoutDocument(doc, 400, 400, 1.0f, 1.0f, &be, &diags) == UID_OK);
	CHECK(!diags.HasErrors());

	uid_node_id_t root = NodeId(doc, "root");
	uid_node_id_t layerA = NodeId(doc, "layer_a");
	uid_node_id_t layerB = NodeId(doc, "layer_b");
	uid_node_id_t layerC = NodeId(doc, "layer_c");
	uid_node_id_t corner = NodeId(doc, "corner");
	CHECK(root >= 0 && layerA >= 0 && layerB >= 0 && layerC >= 0 && corner >= 0);

	const uid_rect_t &rootBox = doc->states[(size_t)root].contentBox;
	const uid_rect_t &a = doc->states[(size_t)layerA].contentBox;
	const uid_rect_t &b = doc->states[(size_t)layerB].contentBox;
	const uid_rect_t &c = doc->states[(size_t)layerC].contentBox;
	CHECK_EQ_F(a.x, rootBox.x, 0.5);
	CHECK_EQ_F(a.y, rootBox.y, 0.5);
	CHECK_EQ_F(a.w, rootBox.w, 0.5);
	CHECK_EQ_F(a.h, rootBox.h, 0.5);
	CHECK_EQ_F(b.x, a.x, 0.5);
	CHECK_EQ_F(b.y, a.y, 0.5);
	CHECK_EQ_F(b.w, a.w, 0.5);
	CHECK_EQ_F(b.h, a.h, 0.5);
	CHECK_EQ_F(c.x, a.x, 0.5);
	CHECK_EQ_F(c.y, a.y, 0.5);

	const uid_rect_t &cornerBox = doc->states[(size_t)corner].borderBox;
	CHECK_EQ_F(cornerBox.w, 40.0, 0.5);
	CHECK_EQ_F(cornerBox.h, 30.0, 0.5);
	CHECK_EQ_F(cornerBox.x, rootBox.x + rootBox.w - 40.0 - 8.0, 0.5);
	CHECK_EQ_F(cornerBox.y, rootBox.y + rootBox.h - 30.0 - 8.0, 0.5);

	UID_DestroyDocument(doc);
}

static void TestOverlapPercentSizeWithMargin(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="overlap" width="640px" height="480px" padding="0">
      <container id="urc_rect" width="32.5%" height="41.6667%"
                 margin="28.3333% 0 0 33.75%" fill="#FFFFFFFF"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("overlap_percent_margin.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 640, 480, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t rect = NodeId(doc, "urc_rect");
	CHECK(rect >= 0);
	const uid_rect_t &box = doc->states[(size_t)rect].borderBox;
	CHECK_EQ_F(box.x, 216.0, 0.5);
	CHECK_EQ_F(box.y, 136.0, 0.5);
	CHECK_EQ_F(box.w, 208.0, 0.5);
	CHECK_EQ_F(box.h, 200.0, 0.5);

	UID_DestroyDocument(doc);
}

static void TestOverlapCompassTemplateLayout(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <templates>
      <template id="classic-compass-panel">
        <container type="overlap" width="128px" height="128px" padding="0" fill="#00000000">
          <container id="back" width="100%" height="100%" fill="#101010FF"/>
          <container id="face" width="100%" height="100%" fill="#202020FF"/>
          <container id="damage" width="100%" height="100%" fill="#303030FF"/>
        </container>
      </template>
    </templates>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="200px" height="200px">
      <use id="compass" template="classic-compass-panel"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("compass_overlap.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 400, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t face = NodeId(doc, "compass.face");
	uid_node_id_t damage = NodeId(doc, "compass.damage");
	uid_node_id_t back = NodeId(doc, "compass.back");
	CHECK(face >= 0 && damage >= 0 && back >= 0);

	const uid_rect_t &f = doc->states[(size_t)face].contentBox;
	const uid_rect_t &d = doc->states[(size_t)damage].contentBox;
	const uid_rect_t &b = doc->states[(size_t)back].contentBox;
	CHECK_EQ_F(f.w, 128.0, 0.5);
	CHECK_EQ_F(f.h, 128.0, 0.5);
	CHECK_EQ_F(d.x, f.x, 0.5);
	CHECK_EQ_F(d.y, f.y, 0.5);
	CHECK_EQ_F(b.w, f.w, 0.5);
	CHECK_EQ_F(b.h, f.h, 0.5);

	UID_DestroyDocument(doc);
}

/* Added in OPM: template.* idents bake inside mixed runtime exprs (cvar + props). */
static void TestTemplatePropsInMixedRuntimeExpr(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <templates>
      <template id="tape-box">
        <props>
          <prop name="width" type="length" default="200px"/>
          <prop name="fov-deg" type="number" default="100"/>
        </props>
        <container id="root" type="overlap" width="{template.width}" height="40px" overflow="hidden" padding="0">
          <container id="marker" width="10px" height="10px"
                     translate-x="{template.width / 2 + cvar.ui_test_bearing * (template.width / template.fov-deg)}px"
                     fill="#FFFFFFFF"/>
        </container>
      </template>
    </templates>
  </definitions>
  <canvas>
    <use id="box" template="tape-box" width="200px" fov-deg="100"/>
  </canvas>
</ui>
)";
	FakeBackendState st;
	st.cvars["ui_test_bearing"] = FakeCvar{"10", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);

	CHECK(UID_ParseXml("template_mixed_expr.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());

	uid_node_id_t marker = NodeId(doc, "box.marker");
	CHECK(marker >= 0);
	std::string tx;
	CHECK(doc->nodes[(size_t)marker].properties.Get("translate-x", &tx));
	CHECK(tx.find("template.") == std::string::npos);
	CHECK(tx.find("cvar.ui_test_bearing") != std::string::npos);

	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t root = NodeId(doc, "box.root");
	CHECK(root >= 0);
	const uid_rect_t &rootBox = doc->states[(size_t)root].contentBox;
	const uid_rect_t &markerBox = doc->states[(size_t)marker].borderBox;
	/* half_w(100) + bearing(10) * px_per_deg(2) = 120 */
	CHECK_EQ_F(markerBox.x, rootBox.x + 120.0f, 0.5);

	st.cvars["ui_test_bearing"] = FakeCvar{"-5", 0};
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);
	const uid_rect_t &markerBox2 = doc->states[(size_t)marker].borderBox;
	CHECK_EQ_F(markerBox2.x, rootBox.x + 90.0f, 0.5);

	UID_DestroyDocument(doc);
}

/* Added in OPM: translate-x/y post-flow offset; siblings ignore it; parent clip still applies. */
static void TestTranslateOffsetLayout(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="viewport" type="overlap" width="200px" height="40px" overflow="hidden" padding="0">
      <container id="tape" width="400px" height="40px" translate-x="-50px" fill="#101010FF"/>
      <container id="marker" width="10px" height="10px" translate-x="{cvar.ui_test_tx}px" translate-y="5px"
                 fill="#FFFFFFFF"/>
      <container id="fixed" width="10px" height="10px" fill="#00FF00FF"/>
    </container>
  </canvas>
</ui>
)";
	FakeBackendState st;
	st.cvars["ui_test_tx"] = FakeCvar{"80", 0};
	uid_backend_t be = MakeFakeBackend(&st);
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);

	CHECK(UID_ParseXml("translate_offset.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t viewport = NodeId(doc, "viewport");
	uid_node_id_t tape = NodeId(doc, "tape");
	uid_node_id_t marker = NodeId(doc, "marker");
	uid_node_id_t fixed = NodeId(doc, "fixed");
	CHECK(viewport >= 0 && tape >= 0 && marker >= 0 && fixed >= 0);

	const uid_rect_t &vp = doc->states[(size_t)viewport].contentBox;
	const uid_rect_t &tapeBox = doc->states[(size_t)tape].borderBox;
	const uid_rect_t &markerBox = doc->states[(size_t)marker].borderBox;
	const uid_rect_t &fixedBox = doc->states[(size_t)fixed].borderBox;

	CHECK_EQ_F(tapeBox.x, vp.x - 50.0f, 0.5);
	CHECK_EQ_F(fixedBox.x, vp.x, 0.5);
	CHECK_EQ_F(markerBox.x, vp.x + 80.0f, 0.5);
	CHECK_EQ_F(markerBox.y, vp.y + 5.0f, 0.5);

	const uid_rect_t &vpClip = doc->states[(size_t)viewport].effectiveClip;
	CHECK_EQ_F(vpClip.w, 200.0f, 0.5);
	CHECK_EQ_F(vpClip.h, 40.0f, 0.5);
	CHECK(tapeBox.x < vpClip.x);
	CHECK(doc->states[(size_t)tape].effectiveClip.x >= vpClip.x - 0.5f);

	st.cvars["ui_test_tx"] = FakeCvar{"20", 0};
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 400, 200, 1.0f, 1.0f, &be, &diags) == UID_OK);
	const uid_rect_t &markerBox2 = doc->states[(size_t)marker].borderBox;
	CHECK_EQ_F(markerBox2.x, vp.x + 20.0f, 0.5);

	UID_DestroyDocument(doc);
}

static void TestOverlapCenterChildLayout(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="overlap" width="200px" height="200px" padding="0">
      <container id="centered" width="40px" height="30px" halign="center" valign="center" fill="#FFFFFFFF"/>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("overlap_center.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(UID_LayoutDocument(doc, 400, 400, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t root = NodeId(doc, "root");
	uid_node_id_t centered = NodeId(doc, "centered");
	CHECK(root >= 0 && centered >= 0);

	const uid_rect_t &rootBox = doc->states[(size_t)root].contentBox;
	const uid_rect_t &childBox = doc->states[(size_t)centered].borderBox;
	CHECK_EQ_F(childBox.w, 40.0, 0.5);
	CHECK_EQ_F(childBox.h, 30.0, 0.5);
	CHECK_EQ_F(childBox.x, rootBox.x + (rootBox.w - 40.0) * 0.5f, 0.5);
	CHECK_EQ_F(childBox.y, rootBox.y + (rootBox.h - 30.0) * 0.5f, 0.5);

	UID_DestroyDocument(doc);
}

static void TestHudPackMetadataPeekFixtures(void)
{
	struct FixtureCase {
		const char *vfsPath;
		const char *hudId;
		const char *hudLabel;
		const char *pauseMenu;
		const char *scoreboardMenu;
		int         drawOrder;
	};

	const FixtureCase cases[] = {
		{"ui/modern/huds/classic.xml", "classic", "Classic", "dm_pause", "scoreboard", 4},
		{"ui/modern/huds/modern.xml", "modern", "Modern", "dm_pause_modern", "scoreboard", 4},
		{"ui/modern/huds/competitive.xml", "competitive", "Competitive", "dm_pause_modern", "scoreboard", 4},
	};

	uid_parse_io_t parseIo = MakeTestParseIo();
	for (const FixtureCase &tc : cases) {
		uid_hud_meta_t meta;
		CHECK(UID_PeekHudMetadata(tc.vfsPath, &parseIo, &meta, NULL) == UID_OK);
		CHECK(meta.valid);
		CHECK(std::strcmp(meta.hudId, tc.hudId) == 0);
		CHECK(std::strcmp(meta.hudLabel, tc.hudLabel) == 0);
		CHECK(std::strcmp(meta.pauseMenu, tc.pauseMenu) == 0);
		CHECK(std::strcmp(meta.scoreboardMenu, tc.scoreboardMenu) == 0);
		CHECK(meta.drawOrder == tc.drawOrder);
	}

	uid_menu_meta_t menuMeta;
	CHECK(UID_PeekMenuMetadata("ui/modern/huds/classic.xml", &parseIo, &menuMeta, NULL) != UID_OK);
}

static void TestClassicHudPackLoads(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/huds/classic.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("ui/modern/huds/classic.xml", xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) == UID_OK);
	if (diags.HasErrors()) {
		for (const auto &d : diags.Items()) {
			std::fprintf(
				stderr,
				"classic hud parse: %s:%d: %s\n",
				d.location.path ? d.location.path : "?",
				d.location.line,
				d.message.c_str()
			);
		}
	}
	CHECK(!diags.HasErrors());
	CHECK(!doc->hasMenuMeta);
	CHECK(doc->drawOrder == 4);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	UID_DestroyDocument(doc);
}

static void TestModernHudPackLoads(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/huds/modern.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("ui/modern/huds/modern.xml", xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) == UID_OK);
	if (diags.HasErrors()) {
		for (const auto &d : diags.Items()) {
			std::fprintf(
				stderr,
				"modern hud parse: %s:%d: %s\n",
				d.location.path ? d.location.path : "?",
				d.location.line,
				d.message.c_str()
			);
		}
	}
	CHECK(!diags.HasErrors());
	CHECK(!doc->hasMenuMeta);
	CHECK(doc->drawOrder == 4);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	UID_DestroyDocument(doc);
}

static void TestHudAndMenuIdMutuallyExclusive(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions menu-id="x" hud-id="y" hud-label="Y" draw-order="4">
    <defaults type="vertical" width="100%" height="100%"/>
  </definitions>
  <canvas>
    <container id="root" type="vertical" width="100px" height="100px"/>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("hud_menu_xor.xml", kDoc, std::strlen(kDoc), &lim, &parseIo, doc, &diags) != UID_OK);
	CHECK(diags.HasErrors());
	UID_DestroyDocument(doc);
}

static void TestScoreboardMenuLoads(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/menus/scoreboard.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("ui/modern/menus/scoreboard.xml", xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	UID_DestroyDocument(doc);
}

static void TestDmPauseMenuLoadsAndMapsUrcRects(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/menus/dm_pause.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);
	st.cvars["ui_om_pause_panel"] = FakeCvar{"root", 0};
	st.cvars["cg_gametype"] = FakeCvar{"2", 2};

	CHECK(UID_ParseXml("ui/modern/menus/dm_pause.xml", xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 640, 480, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t rootPanel = NodeId(doc, "pause_root_panel");
	uid_node_id_t mainHit = NodeId(doc, "pause_root_main");
	CHECK(rootPanel >= 0 && mainHit >= 0);
	const uid_rect_t &rootBox = doc->states[(size_t)rootPanel].borderBox;
	const uid_rect_t &mainBox = doc->states[(size_t)mainHit].borderBox;
	CHECK_EQ_F(rootBox.x, 216.0, 0.75);
	CHECK_EQ_F(rootBox.y, 136.0, 0.75);
	CHECK_EQ_F(rootBox.w, 208.0, 0.75);
	CHECK_EQ_F(rootBox.h, 200.0, 0.75);
	CHECK_EQ_F(mainBox.x, 224.0, 0.75);
	CHECK_EQ_F(mainBox.y, 144.0, 0.75);
	CHECK_EQ_F(mainBox.w, 192.0, 0.75);
	CHECK_EQ_F(mainBox.h, 32.0, 0.75);

	st.cvars["ui_om_pause_panel"].value = "team";
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 640, 480, 1.0f, 1.0f, &be, &diags) == UID_OK);
	uid_node_id_t teamPanel = NodeId(doc, "pause_team_panel");
	uid_node_id_t alliesHit = NodeId(doc, "pause_team_allies");
	uid_node_id_t axisHit = NodeId(doc, "pause_team_axis");
	CHECK(teamPanel >= 0 && alliesHit >= 0 && axisHit >= 0);
	const uid_rect_t &teamBox = doc->states[(size_t)teamPanel].borderBox;
	const uid_rect_t &alliesBox = doc->states[(size_t)alliesHit].borderBox;
	const uid_rect_t &axisBox = doc->states[(size_t)axisHit].borderBox;
	CHECK_EQ_F(teamBox.x, 160.0, 0.75);
	CHECK_EQ_F(teamBox.y, 80.0, 0.75);
	CHECK_EQ_F(teamBox.w, 320.0, 0.75);
	CHECK_EQ_F(teamBox.h, 304.0, 0.75);
	CHECK_EQ_F(alliesBox.x, 168.0, 0.75);
	CHECK_EQ_F(alliesBox.y, 88.0, 0.75);
	CHECK_EQ_F(alliesBox.w, 152.0, 0.75);
	CHECK_EQ_F(alliesBox.h, 240.0, 0.75);
	CHECK_EQ_F(axisBox.x, 320.0, 0.75);
	CHECK_EQ_F(axisBox.y, 88.0, 0.75);
	CHECK_EQ_F(axisBox.w, 152.0, 0.75);
	CHECK_EQ_F(axisBox.h, 240.0, 0.75);

	UID_DestroyDocument(doc);
}

static void TestDmPauseModernMenuLoads(void)
{
	std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/menus/dm_pause_modern.xml";
	FILE *f = std::fopen(path.c_str(), "rb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml((size_t)sz, '\0');
	CHECK(std::fread(xml.data(), 1, (size_t)sz, f) == (size_t)sz);
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	uid_parse_io_t parseIo = MakeTestParseIo();

	CHECK(UID_ParseXml("ui/modern/menus/dm_pause_modern.xml", xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	UID_DestroyDocument(doc);
}

static void TestScoreboardCollectionForeach(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="control" src="fonts/x.ttf" weight="600"/></fonts>
  </definitions>
  <canvas>
    <container id="scope" type="vertical" width="100%" height="120px" source="scoreboard">
      <foreach mode="all">
        <container type="horizontal" width="100%" height="32px"
                   visible="{item.field.kind != spacer}">
          <label id="hdr" width="100%" font="control" font-size="15px"
                 visible="{item.field.is_header == 1}">{item.field.name}</label>
          <label id="kd" width="20%" font="control" font-size="14px"
                 visible="{item.field.is_header == 0}">{item.field.kd}</label>
        </container>
      </foreach>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	g_fakeScoreboardRevision = 1;
	CHECK(UID_ParseXml("scoreboard_foreach.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	uid_node_id_t scopeId = doc->idIndex.count("scope") ? doc->idIndex["scope"] : UID_INVALID_NODE_ID;
	CHECK(scopeId >= 0);
	CHECK(doc->states[static_cast<size_t>(scopeId)].collectionItems.size() == 3);
	if (!doc->states[static_cast<size_t>(scopeId)].collectionItems.empty()) {
		CHECK(doc->states[static_cast<size_t>(scopeId)].collectionItems[1].fields.size() == 12);
		CHECK(doc->states[static_cast<size_t>(scopeId)].collectionItems[1].fields["kd"] == "2.50");
	}
	g_fakeScoreboardRevision = 2;
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 640, 480, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_DestroyDocument(doc);
}

static void TestScoreboardForeachUseTemplate(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="control" src="fonts/x.ttf" weight="600"/></fonts>
    <templates>
      <template id="scoreboard-row">
        <container type="horizontal" width="100%" height="32px"
                   visible="{item.field.is_spectator != 1}">
          <label id="name" width="fill" font="control" font-size="14px">{item.field.name}</label>
          <label id="kd" width="20%" font="control" font-size="14px">{item.field.kd}</label>
        </container>
      </template>
    </templates>
  </definitions>
  <canvas>
    <container id="scope" type="vertical" width="100%" height="120px" source="scoreboard">
      <foreach mode="all">
        <use template="scoreboard-row"/>
      </foreach>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	g_fakeScoreboardRevision = 3;
	CHECK(UID_ParseXml("scoreboard_foreach_use.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	CHECK(doc->definitions.templates.count("scoreboard-row") == 1);
	UID_SyncBindings(doc, &be);
	uid_node_id_t scopeId = doc->idIndex.count("scope") ? doc->idIndex["scope"] : UID_INVALID_NODE_ID;
	uid_node_id_t foreachId = UID_INVALID_NODE_ID;
	for (size_t ni = 0; ni < doc->nodes.size(); ++ni) {
		if (doc->nodes[ni].kind == UID_NODE_FOREACH) {
			foreachId = static_cast<uid_node_id_t>(ni);
			break;
		}
	}
	CHECK(scopeId >= 0);
	CHECK(foreachId >= 0);
	CHECK(doc->nodes[static_cast<size_t>(foreachId)].children.size() == 3);
	for (uid_node_id_t childId : doc->nodes[static_cast<size_t>(foreachId)].children) {
		CHECK(doc->nodes[static_cast<size_t>(childId)].kind != UID_NODE_USE);
	}
	CHECK(UID_LayoutDocument(doc, 640, 480, 1.0f, 1.0f, &be, &diags) == UID_OK);
	bool foundPlayer = false;
	bool foundKd = false;
	for (size_t i = 0; i < doc->nodes.size(); ++i) {
		const uid_node_state_t &st = doc->states[i];
		if (!st.runtimeValue.hasValue) {
			continue;
		}
		if (st.runtimeValue.stringValue == "PlayerOne") {
			foundPlayer = true;
		}
		if (st.runtimeValue.stringValue == "2.50") {
			foundKd = true;
		}
	}
	CHECK(foundPlayer);
	CHECK(foundKd);
	UID_DestroyDocument(doc);
}

static void TestScoreboardKdFormat(void)
{
	char buf[16];
	auto formatKd = [](int kills, int deaths, char *out, int outSize) {
		if (deaths < 1) {
			deaths = 1;
		}
		std::snprintf(out, static_cast<size_t>(outSize), "%.2f", static_cast<double>(kills) / deaths);
	};
	formatKd(10, 4, buf, sizeof(buf));
	CHECK(std::strcmp(buf, "2.50") == 0);
	formatKd(5, 0, buf, sizeof(buf));
	CHECK(std::strcmp(buf, "5.00") == 0);
	formatKd(0, 0, buf, sizeof(buf));
	CHECK(std::strcmp(buf, "0.00") == 0);
}

/* Added in OPM: join(source, field, sep[, filter]) in label text. */
static void TestJoinCollectionLabel(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="control" src="fonts/x.ttf" weight="600"/></fonts>
  </definitions>
  <canvas>
    <container type="vertical" width="400px" height="40px">
      <label id="joined" width="100%" height="100%" font="control" font-size="14px">
        Spectators: {join(join-demo, name, ", ", item.field.is_spectator == 1)}
      </label>
      <label id="all" width="100%" height="auto" font="control" font-size="14px">
        {join(join-demo, name, " | ")}
      </label>
      <label id="mixed" width="100%" height="auto" font="control" font-size="14px">
        n={floor(3.9)} {join(join-demo, name, ",", item.field.is_spectator == 0)}
      </label>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("join_label.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	UID_SyncBindings(doc, &be);

	uid_node_id_t joinedId = doc->idIndex.count("joined") ? doc->idIndex["joined"] : UID_INVALID_NODE_ID;
	uid_node_id_t allId = doc->idIndex.count("all") ? doc->idIndex["all"] : UID_INVALID_NODE_ID;
	uid_node_id_t mixedId = doc->idIndex.count("mixed") ? doc->idIndex["mixed"] : UID_INVALID_NODE_ID;
	CHECK(joinedId >= 0 && allId >= 0 && mixedId >= 0);
	CHECK(doc->states[static_cast<size_t>(joinedId)].runtimeValue.stringValue == "Spectators: Alice, Carol");
	CHECK(doc->states[static_cast<size_t>(allId)].runtimeValue.stringValue == "Alice | Bob | Carol");
	CHECK(doc->states[static_cast<size_t>(mixedId)].runtimeValue.stringValue == "n=3 Bob");

	UID_DestroyDocument(doc);
}

/* Added in OPM: windowed-foreach overscan must not paint row dividers below the scroll viewport. */
static void TestWindowForeachOverscanPaintCull(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <sources>
      <source id="rows">
        <item value="0" label="r0"/><item value="1" label="r1"/><item value="2" label="r2"/>
        <item value="3" label="r3"/><item value="4" label="r4"/><item value="5" label="r5"/>
        <item value="6" label="r6"/><item value="7" label="r7"/><item value="8" label="r8"/>
        <item value="9" label="r9"/><item value="10" label="r10"/><item value="11" label="r11"/>
        <item value="12" label="r12"/><item value="13" label="r13"/><item value="14" label="r14"/>
        <item value="15" label="r15"/><item value="16" label="r16"/><item value="17" label="r17"/>
        <item value="18" label="r18"/><item value="19" label="r19"/>
      </source>
    </sources>
  </definitions>
  <canvas>
    <container id="scope" type="vertical" width="200px" height="100%" source="rows">
      <container id="scroller" type="vertical" width="100%" height="100px" overflow="scroll" gap="0"
                 fill="#00000000">
        <foreach mode="window" row-height="29px">
          <container type="vertical" width="100%" height="29px" overflow="hidden">
            <container width="100%" height="28px" fill="#FFFFFFFF"/>
            <container width="100%" height="1px" fill="#808080FF"/>
          </container>
        </foreach>
      </container>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("window_overscan_cull.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);
	UID_SyncBindings(doc, &be);
	CHECK(UID_LayoutDocument(doc, 200, 100, 1.0f, 1.0f, &be, &diags) == UID_OK);

	uid_node_id_t scrollId = doc->idIndex.count("scroller") ? doc->idIndex["scroller"] : UID_INVALID_NODE_ID;
	CHECK(scrollId >= 0);
	const uid_rect_t &vp = doc->states[static_cast<size_t>(scrollId)].contentBox;
	CHECK(vp.h > 0.0f);

	st.drawLog.clear();
	UID_PaintChrome(doc, &be);

	const float bottom = vp.y + vp.h;
	int below = 0;
	for (const std::string &line : st.drawLog) {
		if (line.compare(0, 4, "rect") != 0) {
			continue;
		}
		float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f, a = 0.0f;
		if (std::sscanf(line.c_str(), "rect %f %f %f %f a=%f", &x, &y, &w, &h, &a) != 5) {
			continue;
		}
		if (y >= bottom - 0.5f) {
			++below;
		}
	}
	CHECK(below == 0);

	UID_DestroyDocument(doc);
}

/* Added in OPM: label marquee paint offset when text overflows the content box. */
static void TestMarqueeLabelPaint(void)
{
	static const char *kDoc = R"(
<ui version="1">
  <definitions>
    <defaults type="vertical" width="100%" height="100%"/>
    <fonts><font id="control" src="fonts/x.ttf" weight="600"/></fonts>
  </definitions>
  <canvas>
    <container type="vertical" width="100%" height="100%" gap="8px">
      <container id="clip" type="vertical" width="80px" height="24px" overflow="hidden">
        <label id="ticker" width="100%" height="100%" font="control" font-size="14px"
               marquee="horizontal" marquee-speed="40px" marquee-gap="16px">
          ABCDEFGHIJKLMNOPQRSTUVWXYZ
        </label>
      </container>
      <label id="fits" width="400px" height="24px" font="control" font-size="14px"
             marquee="horizontal" marquee-speed="40px">Hi</label>
    </container>
  </canvas>
</ui>
)";
	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	st.fontMeasurePx = 8.0f;
	uid_backend_t be = MakeFakeBackend(&st);

	CHECK(UID_ParseXml("marquee_label.xml", kDoc, std::strlen(kDoc), &lim, nullptr, doc, &diags) == UID_OK);
	CHECK(UID_ExpandDocument(doc, &diags) == UID_OK);
	CHECK(UID_CompileDocument(doc, &diags) == UID_OK);
	CHECK(!diags.HasErrors());
	UID_SyncBindings(doc, &be);
	doc->updateTimeMs = 0;
	CHECK(UID_LayoutDocument(doc, 640, 480, 1.0f, 1.0f, &be, &diags) == UID_OK);

	st.fontDrawLog.clear();
	UID_PaintChrome(doc, &be);
	float x0 = -1.0f;
	for (const std::string &line : st.fontDrawLog) {
		if (line.find("'ABCDEFGHIJKLMNOPQRSTUVWXYZ'") != std::string::npos) {
			float x = 0.0f, y = 0.0f;
			CHECK(std::sscanf(line.c_str(), "text %f,%f", &x, &y) == 2);
			x0 = x;
			break;
		}
	}
	CHECK(x0 >= 0.0f);

	doc->updateTimeMs = 1000; /* 40px/s → 40px leftward */
	st.fontDrawLog.clear();
	UID_PaintChrome(doc, &be);
	float x1 = x0;
	int copies = 0;
	bool saw = false;
	for (const std::string &line : st.fontDrawLog) {
		if (line.find("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos) {
			float x = 0.0f, y = 0.0f;
			CHECK(std::sscanf(line.c_str(), "text %f,%f", &x, &y) == 2);
			if (!saw) {
				x1 = x;
				saw = true;
			}
			++copies;
		}
	}
	CHECK(saw);
	CHECK(x1 < x0 - 30.0f); /* ~40px left */
	CHECK(copies >= 2);     /* seamless second copy */

	st.fontDrawLog.clear();
	/* Short label must not marquee. */
	uid_node_id_t fitsId = doc->idIndex.count("fits") ? doc->idIndex["fits"] : UID_INVALID_NODE_ID;
	CHECK(fitsId >= 0);
	doc->updateTimeMs = 5000;
	UID_PaintChrome(doc, &be);
	float fitsX0 = -1.0f;
	float fitsX1 = -1.0f;
	for (const std::string &line : st.fontDrawLog) {
		if (line.find("'Hi'") != std::string::npos) {
			float x = 0.0f, y = 0.0f;
			CHECK(std::sscanf(line.c_str(), "text %f,%f", &x, &y) == 2);
			if (fitsX0 < 0.0f) {
				fitsX0 = x;
			} else {
				fitsX1 = x;
			}
		}
	}
	CHECK(fitsX0 >= 0.0f);
	CHECK(fitsX1 < 0.0f); /* single draw, no loop copy */

	UID_DestroyDocument(doc);
}

/* Added in OPM: printdeathmsg phrase classifier fixtures. */
static void TestKillFeedClassify(void)
{
	char weapon[32];
	char kind[16];
	int  headshot = 0;
	int  friendly = 0;

	CL_KillFeed_Classify(
		"was sniped by", "x", 'p', weapon, sizeof(weapon), kind, sizeof(kind), &headshot, &friendly
	);
	CHECK(std::strcmp(weapon, "sniper") == 0);
	CHECK(std::strcmp(kind, "player") == 0);
	CHECK(headshot == 0);
	CHECK(friendly == 0);

	CL_KillFeed_Classify(
		"was perforated by",
		"'s' SMG in the head",
		'p',
		weapon,
		sizeof(weapon),
		kind,
		sizeof(kind),
		&headshot,
		&friendly
	);
	CHECK(std::strcmp(weapon, "smg") == 0);
	CHECK(headshot == 1);

	CL_KillFeed_Classify(
		"was gunned down by", "x in the helmet", 'P', weapon, sizeof(weapon), kind, sizeof(kind), &headshot, &friendly
	);
	CHECK(std::strcmp(weapon, "pistol") == 0);
	CHECK(headshot == 1);
	CHECK(friendly == 1);

	CL_KillFeed_Classify(
		"was shot by", "x", 'p', weapon, sizeof(weapon), kind, sizeof(kind), &headshot, &friendly
	);
	CHECK(std::strcmp(weapon, "unknown") == 0);
	CHECK(headshot == 0);

	CL_KillFeed_Classify(
		"took himself out of commision", "x", 's', weapon, sizeof(weapon), kind, sizeof(kind), &headshot, &friendly
	);
	CHECK(std::strcmp(kind, "suicide") == 0);

	CL_KillFeed_Classify(
		"caught a rocket", "x", 'w', weapon, sizeof(weapon), kind, sizeof(kind), &headshot, &friendly
	);
	CHECK(std::strcmp(kind, "world") == 0);
	CHECK(std::strcmp(weapon, "rocket") == 0);

	CL_KillFeed_Classify(
		"tripped on", "'s grenade", 'p', weapon, sizeof(weapon), kind, sizeof(kind), &headshot, &friendly
	);
	CHECK(std::strcmp(weapon, "grenade") == 0);
}

} // namespace

int main(void)
{
	TestValues();
	TestExpr();
	TestParseMinimal();
	TestDynamicBackgroundImageCompile();
	TestCvarDotPropResolve();
	TestModuloExpression();
	TestNumericHelperFunctions();
	TestRuntimeNumericExprBinding();
	TestForeachCountExpansion();
	TestForeachTemplateVarStrokeResolve();
	TestHudAmmoCatalogCompile();
	TestAmmoClipTopSync();
	TestModernWeaponsBarImageFieldSync();
	TestAmmoStaggerLanesSync();
	TestExprBoundWidthMultiply();
	TestExprBoundMarginDoubleMultiply();
	TestWeaponsBarAutoChildrenPadding();
	TestStrokeLayoutFalseAutoSize();
	TestShapeClipsChildrenPaint();
	TestImageMaskPaint();
	TestImageMaskUnknownRejected();
	TestImageMaskNullHooksSafe();
	TestImageMaskGradientBrush();
	TestImageMaskRadialBrushCompile();
	TestAutoFillAutoRow();
	TestAutoWrapperWithInternalFill();
	TestAutoParentExpandsForFill();
	TestHorizontalAutoHeightNotPromotedInVerticalParent();
	TestForeachDefaultWidthAutoInHorizontalPanel();
	TestAmmoPanelBottomRowLayoutMaxClip32();
	TestEdgeClipShapePaint();
	TestCvarBoundPropertySync();
	TestOpacityInheritancePaint();
	TestRejectDoctype();
	TestCanvasPointerAttr();
	TestButtonShapeDeclaredProp();
	TestTemplateExpand();
	TestKeybindTemplateExpand();
	TestKeybindConflictModal();
	TestWriteAllBindingsPreservesKeybinds();
	TestKeybindLowercaseAndDisplay();
	TestKeybindMigrateUppercase();
	TestKeybindCaptureMouseWheelKeys();
	TestModalCvarDispatch();
	TestLayoutFill();
	TestNestedAlignAndGap();
	TestSpaceBetween();
	TestButtonTextCentered();
	TestNestedButtonTextCenter();
	TestPaddingFillAndMargin();
	TestMainXmlLoads();
	TestSelectModalClickOpen();
	TestMainSelectModalClick();
	TestOverlayPaint();
	TestOverlayViewportBounds();
	TestCyclicSelect();
	TestCyclicSelectSource();
	TestComposableCyclicForeach();
	TestCyclicCommitApplyStagesUntilFlush();
	TestComposableVerticalList();
	TestXmlCollectionSource();
	TestSourceDefaultNoMatch();
	TestCollectionNoDefaultUnset();
	TestCollectionDefaultIndex();
	TestButtonDblClick();
	TestTabBarForeach();
	TestInlineSettingsPages();
	TestSettingsTabVisibility();
	TestSettingsCvarBindsResolve();
	TestSettingsSearchFilter();
	TestRouteTabBar();
	TestMainPanelVisibility();
	TestInvokeRegistry();
	TestForeachTemplateWrapLayout();
	TestCollectionDisplayValue();
	TestHostCollectionFallback();
	TestForeachReexpandWithFields();
	TestCollectionSelectedFill();
	TestWindowForeachScroll();
	TestMaxHeightShortContent();
	TestMaxHeightScrollShrink();
	TestMaxHeightWindowForeachIntrinsic();
	TestMaxHeightCompileReject();
	TestMaxWidthClamp();
	TestCollectionVisibilityCull();
	TestStrokeStyleTernary();
	TestCvarNeqStyleFillSurvivesSync();
	TestFavoriteButtonInvoke();
	TestMainXmlRuntime();
	TestSettingsOnOffButtons();
	TestBindSelectedNumericMatch();
	TestUseVisibleAndSearchOrPrecedence();
	TestValueTypeTransforms();
	TestSteppedNumberDisplay();
	TestEnabledIf();
	TestBoolExpr();
	TestVisibleBraceExpr();
	TestBindingsAndActions();
	TestRuntimeLifecycle();
	TestSettingsFixtureFile();
	TestUiPxScale();
	TestComposedSlider();
	TestTemplateInputBounds();
	TestInputCaretUsesFontMeasure();
	TestElementStroke();
	TestDropShadow();
	TestTextStyleInheritance();
	TestInheritedDropShadowPaint();
	TestDesignVarsParseAndResolve();
	TestDesignVarsUnknownRejected();
	TestDesignVarsNumericLayout();
	TestDesignVarsImportMerge();
	TestBorderAttrRejected();
	TestBuiltinRectangleShape();
	TestShapeRotationRejected();
	TestShapeRotationPaint();
	TestTextRotationPaint();
	TestRotatedRectangleUsesPath();
	TestButtonNestedShapeChild();
	TestButtonAutoWidthIconPadding();
	TestSettingsCyclicTemplatePaint();
	TestScrollbarTemplated();
	TestScrollbarEdge();
	TestScrollWheelOverScrollContainer();
	TestScrollbarMissingDefaultTemplate();
	TestMenuMapViewResolve();
	TestImportHappyPath();
	TestImportRelativePath();
	TestImportCycleRejected();
	TestImportDotDotRejected();
	TestImportOverrideWarning();
	TestImportDefaultsMerge();
	TestImportRequiresIo();
	TestImageRegistryParse();
	TestImageRegistryRejectsJpg();
	TestBackgroundImagePaint();
	TestBackgroundImageRectFastPath();
	TestLeafImageAspectLayout();
	TestLeafImageNestedIntrinsicWidth();
	TestLeafImageUnknownRejected();
	TestLeafImageItemFieldBind();
	TestGradientFillPaint();
	TestCrosshairDisplayShape();
	TestIntrinsicShapePropScale();
	TestMenuMetadataParse();
	TestMenuMetadataDrawOrderBounds();
	TestMenuMetadataMissingDrawOrder();
	TestMenuMetadataPeekFixtures();
	TestHudPackMetadataPeekFixtures();
	TestOverlapContainerLayout();
	TestOverlapPercentSizeWithMargin();
	TestOverlapCenterChildLayout();
	TestOverlapCompassTemplateLayout();
	TestTemplatePropsInMixedRuntimeExpr();
	TestTranslateOffsetLayout();
	TestClassicHudPackLoads();
	TestModernHudPackLoads();
	TestClassicHudLayoutGeometry();
	TestMessageCollectionPaint();
	TestForeachLifetimeFade();
	TestHudAndMenuIdMutuallyExclusive();
	TestScoreboardMenuLoads();
	TestDmPauseMenuLoadsAndMapsUrcRects();
	TestDmPauseModernMenuLoads();
	TestScoreboardCollectionForeach();
	TestScoreboardForeachUseTemplate();
	TestScoreboardKdFormat();
	TestJoinCollectionLabel();
	TestWindowForeachOverscanPaintCull();
	TestMarqueeLabelPaint();
	TestKillFeedClassify();

	if (g_failures) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("test_uir_design: ok\n");
	return 0;
}
