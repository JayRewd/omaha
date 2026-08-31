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
 * Added in OPM: headless main-menu / HUD frame benchmark for UID CPU stages.
 * Loads a modern UI XML fixture, runs bind/layout/paint for N frames, and
 * optionally dumps a deterministic DRAW/CLIP call log for golden diffs.
 */

#include "../uid_backend.h"
#include "../uid_binding.h"
#include "../uid_compile.h"
#include "../uid_diag.h"
#include "../uid_document.h"
#include "../uid_layout.h"
#include "../uid_opt.h"
#include "../uid_template.h"
#include "../uid_types.h"
#include "../uid_widget.h"
#include "../uid_xml.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

/* Stubs for q_shared helpers used by this headless bench binary. */
extern "C" void Com_Error(int code, const char *fmt, ...)
{
	(void)code;
	(void)fmt;
	std::abort();
}

extern "C" void Com_Printf(const char *fmt, ...)
{
	(void)fmt;
}

#ifndef UID_TEST_FIXTURE_DIR
#define UID_TEST_FIXTURE_DIR "assets/main/ui/modern"
#endif

namespace {

struct FakeCvar {
	std::string value;
	int         flags = 0;
};

struct OrderedCall {
	std::string kind; /* DRAW or CLIP */
	std::string line;
};

struct FakeBackendState {
	std::map<std::string, FakeCvar> cvars;
	std::map<int, std::string>      bindings;
	std::set<std::string>           unknownCvars;
	std::vector<OrderedCall>        ordered;
	unsigned                        cvarEpoch = 1;
	int                             clipDepth = 0;
	int                             solidRects = 0;
	int                             fontDraws = 0;
	int                             imageDraws = 0;
	int                             pathDraws = 0;
	int                             gradientDraws = 0;
	int                             pushClips = 0;
	int                             popClips = 0;
	bool                            recordOrdered = false;
};

FakeBackendState *g_fake = nullptr;

void Record(const char *kind, const char *line)
{
	if (!g_fake || !g_fake->recordOrdered) {
		return;
	}
	g_fake->ordered.push_back({kind, line});
}

std::string ResolveTestImportDiskPath(const char *path)
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

long test_read_import_file(const char *path, void **buf)
{
	if (!path || !buf) {
		return -1;
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

void test_free_import_file(void *buf)
{
	std::free(buf);
}

uid_parse_io_t MakeTestParseIo(void)
{
	uid_parse_io_t io;
	io.readFile = test_read_import_file;
	io.freeFile = test_free_import_file;
	return io;
}

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
		g_fake->unknownCvars.insert(name);
		g_fake->cvars[name] = FakeCvar{"0", 0};
		it = g_fake->cvars.find(name);
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
	g_fake->cvars[name] = FakeCvar{value, 0};
	g_fake->cvarEpoch++;
	return true;
}

bool fake_cvarReset(const char *name)
{
	return fake_cvarWrite(name, "0");
}

unsigned fake_cvarEpoch(void)
{
	return g_fake ? g_fake->cvarEpoch : 0u;
}

bool fake_invoke(const char *, void *)
{
	return true;
}

void fake_drawRect(float x, float y, float w, float h, const float *rgba)
{
	if (!g_fake) {
		return;
	}
	g_fake->solidRects++;
	char buf[160];
	std::snprintf(buf, sizeof(buf), "rect %.1f %.1f %.1f %.1f a=%.2f", x, y, w, h, rgba ? rgba[3] : 0.0f);
	Record("DRAW", buf);
}

void fake_drawHostRegion(const char *role, float x, float y, float w, float h, void *)
{
	char buf[160];
	std::snprintf(buf, sizeof(buf), "host %s %.1f %.1f %.1f %.1f", role ? role : "", x, y, w, h);
	Record("DRAW", buf);
}

bool fake_getHiResScale(float *scaleX, float *scaleY)
{
	if (!scaleX || !scaleY) {
		return false;
	}
	*scaleX = 1.0f;
	*scaleY = 1.0f;
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
	if (!g_fake) {
		return;
	}
	g_fake->pushClips++;
	g_fake->clipDepth++;
	char buf[128];
	std::snprintf(buf, sizeof(buf), "push %.1f,%.1f,%.1f,%.1f", x, y, w, h);
	Record("CLIP", buf);
}

void fake_popClip(void)
{
	if (!g_fake || g_fake->clipDepth <= 0) {
		return;
	}
	g_fake->popClips++;
	g_fake->clipDepth--;
	Record("CLIP", "pop");
}

bool fake_beginShapeClip(
	float x,
	float y,
	float w,
	float h,
	const char *const *pathD,
	int pathCount,
	float,
	float,
	float
)
{
	char buf[160];
	std::snprintf(buf, sizeof(buf), "shapeclip %.1f,%.1f,%.1f,%.1f n=%d", x, y, w, h, pathCount);
	Record("DRAW", buf);
	if (pathD && pathCount > 0 && pathD[0] && pathD[0][0]) {
		Record("DRAW", pathD[0]);
	}
	return true;
}

void fake_endShapeClip(void)
{
	Record("DRAW", "shapeclip-end");
}

bool fake_beginImageMask(float x, float y, float w, float h, const char *vfsPath, int fit)
{
	char buf[192];
	std::snprintf(buf, sizeof(buf), "mask %.1f,%.1f,%.1f,%.1f fit=%d '%s'", x, y, w, h, fit, vfsPath ? vfsPath : "");
	Record("DRAW", buf);
	return true;
}

void fake_endImageMask(void)
{
	Record("DRAW", "mask-end");
}

float fake_fontMeasure(void *, const char *text)
{
	return text ? (float)std::strlen(text) * 8.0f : 0.0f;
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
	g_fake->fontDraws++;
	char buf[256];
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
	Record("DRAW", buf);
}

/* Added in Omaha: count rotated text draws in menu bench. */
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
	(void)rotationDeg;
	(void)pivotX;
	(void)pivotY;
	fake_fontDraw(nullptr, x, y, text, rgba, 0.0f);
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
	g_fake->pathDraws++;
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
	Record("DRAW", buf);
}

void fake_drawImage(
	const char *vfsPath,
	float x,
	float y,
	float w,
	float h,
	const char *const *clipPathD,
	int clipPathCount,
	float,
	float,
	int fit,
	float rotationDeg,
	float backgroundScale,
	const float *tintRgba
)
{
	if (!g_fake) {
		return;
	}
	g_fake->imageDraws++;
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
	Record("DRAW", buf);
}

bool fake_imageMeasure(const char *vfsPath, float *outW, float *outH)
{
	(void)vfsPath;
	if (!outW || !outH) {
		return false;
	}
	*outW = 32.0f;
	*outH = 32.0f;
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
	float,
	float,
	float rotationDeg,
	const float *tintRgba
)
{
	if (!g_fake) {
		return;
	}
	g_fake->gradientDraws++;
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
	Record("DRAW", buf);
}

int fake_queryOptions(const char *source, char **values, char **labels, int max)
{
	static char valueBuf[8][64];
	static char labelBuf[8][64];
	static const char *const displayMode[] = {"1", "Fullscreen", "2", "Borderless", "0", "Windowed"};
	if (!source || !values || !labels || max <= 0) {
		return 0;
	}
	if (std::strcmp(source, "display-mode") != 0) {
		return 0;
	}
	int n = 3;
	if (n > max) {
		n = max;
	}
	for (int i = 0; i < n; i++) {
		std::snprintf(valueBuf[i], sizeof(valueBuf[i]), "%s", displayMode[i * 2]);
		std::snprintf(labelBuf[i], sizeof(labelBuf[i]), "%s", displayMode[i * 2 + 1]);
		values[i] = valueBuf[i];
		labels[i] = labelBuf[i];
	}
	return n;
}

int fake_queryCollectionItems(const uid_collection_query_t *query, uid_collection_item_t *out, int max)
{
	static char keyBuf[8][16];
	static char valueBuf[8][32];
	static char labelBuf[8][32];
	if (!query || !query->source || !out || max <= 0) {
		return 0;
	}
	if (std::strcmp(query->source, "servers") == 0) {
		const int total = 2;
		if (query->outTotal) {
			*query->outTotal = total;
		}
		if (query->outRevision) {
			*query->outRevision = 1;
		}
		int written = 0;
		for (int i = 0; i < total && written < max; ++i) {
			std::snprintf(keyBuf[written], sizeof(keyBuf[written]), "%d", i);
			std::snprintf(valueBuf[written], sizeof(valueBuf[written]), "127.0.0.1:%d", 12203 + i);
			std::snprintf(labelBuf[written], sizeof(labelBuf[written]), "Server %d", i + 1);
			out[written].key = keyBuf[written];
			out[written].value = valueBuf[written];
			out[written].label = labelBuf[written];
			out[written].nfields = 0;
			out[written].fieldNames = nullptr;
			out[written].fieldValues = nullptr;
			out[written].flags = 0;
			written++;
		}
		return written;
	}
	if (query->outTotal) {
		*query->outTotal = 0;
	}
	if (query->outRevision) {
		*query->outRevision = 1;
	}
	return 0;
}

bool fake_keyNameToNum(const char *name, int *key)
{
	if (!name || !key || !name[0]) {
		return false;
	}
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
	b.cvarEpoch = fake_cvarEpoch;
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

void SeedDefaultCvars(FakeBackendState *st)
{
	st->cvars["ui_om_main_panel"] = FakeCvar{"play", 0};
	st->cvars["ui_om_settings_tab"] = FakeCvar{"input", 0};
	st->cvars["ui_om_modal"] = FakeCvar{"", 0};
	st->cvars["ui_om_pause_panel"] = FakeCvar{"root", 0};
	st->cvars["ui_om_servers_visible"] = FakeCvar{"0", 0};
	st->cvars["ui_om_servers_total"] = FakeCvar{"0", 0};
	st->cvars["ui_om_players_total"] = FakeCvar{"0", 0};
	st->cvars["ui_selected_server"] = FakeCvar{"", 0};
	st->cvars["ui_om_status_phase"] = FakeCvar{"READY", 0};
	st->cvars["ui_om_server_search"] = FakeCvar{"", 0};
	st->cvars["ui_om_browser_sort"] = FakeCvar{"players", 0};
	st->cvars["ui_om_browser_sort_asc"] = FakeCvar{"0", 0};
	st->cvars["ui_om_settings_search"] = FakeCvar{"", 0};
	st->cvars["cl_mouseAccel"] = FakeCvar{"0", 0};
	st->cvars["sensitivity"] = FakeCvar{"5", 0};
	st->cvars["m_filter"] = FakeCvar{"0", 0};
	st->cvars["cl_run"] = FakeCvar{"1", 0};
	st->cvars["cg_crosshair"] = FakeCvar{"1", 0};
	st->cvars["r_mode"] = FakeCvar{"-2", 0};
	st->cvars["r_fullscreen"] = FakeCvar{"1", 0};
	st->cvars["s_volume"] = FakeCvar{"0.8", 0};
	st->cvars["s_musicvolume"] = FakeCvar{"0.5", 0};
	st->cvars["name"] = FakeCvar{"Player", 0};
}

struct PhaseSamples {
	std::vector<long long> bindUs;
	std::vector<long long> layoutUs;
	std::vector<long long> paintUs;
	std::vector<long long> overlayUs;
	std::vector<long long> totalUs;
};

long long Percentile(std::vector<long long> v, double p)
{
	if (v.empty()) {
		return 0;
	}
	std::sort(v.begin(), v.end());
	const size_t idx = static_cast<size_t>(std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1));
	return v[idx];
}

double Avg(const std::vector<long long> &v)
{
	if (v.empty()) {
		return 0.0;
	}
	long long sum = 0;
	for (long long x : v) {
		sum += x;
	}
	return static_cast<double>(sum) / static_cast<double>(v.size());
}

void PrintPhase(const char *name, const std::vector<long long> &v)
{
	std::printf(
		"  %-10s avg=%7.3f med=%7.3f p95=%7.3f us\n",
		name,
		Avg(v),
		static_cast<double>(Percentile(v, 0.5)),
		static_cast<double>(Percentile(v, 0.95))
	);
}

void PrintUsage(const char *argv0)
{
	std::fprintf(
		stderr,
		"Usage: %s [--doc <rel>] [--frames N] [--warmup N] [--dump-log <file>] [--opt <hexmask>]\n"
		"  --doc default: main.xml (under UID_TEST_FIXTURE_DIR)\n"
		"  --opt 0xffffffff = all opts; 0 = none\n",
		argv0
	);
}

} // namespace

int main(int argc, char **argv)
{
	const char *docRel = "main.xml";
	int         frames = 600;
	int         warmup = 30;
	const char *dumpLog = nullptr;
	unsigned    optMask = UID_OPT_ALL;

	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--doc") == 0 && i + 1 < argc) {
			docRel = argv[++i];
		} else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			frames = std::atoi(argv[++i]);
		} else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
			warmup = std::atoi(argv[++i]);
		} else if (std::strcmp(argv[i], "--dump-log") == 0 && i + 1 < argc) {
			dumpLog = argv[++i];
		} else if (std::strcmp(argv[i], "--opt") == 0 && i + 1 < argc) {
			optMask = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
		} else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
			PrintUsage(argv[0]);
			return 0;
		} else {
			std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
			PrintUsage(argv[0]);
			return 1;
		}
	}
	if (frames < 1) {
		frames = 1;
	}
	if (warmup < 0) {
		warmup = 0;
	}

	UID_SetOptFlags(optMask);

	const std::string path = std::string(UID_TEST_FIXTURE_DIR) + "/" + docRel;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) {
		std::fprintf(stderr, "failed to open %s\n", path.c_str());
		return 1;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string xml(static_cast<size_t>(sz), '\0');
	if (std::fread(xml.data(), 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
		std::fclose(f);
		std::fprintf(stderr, "failed to read %s\n", path.c_str());
		return 1;
	}
	std::fclose(f);

	uid_limits_t lim;
	UID_DefaultLimits(&lim);
	uid_document_t *doc = UID_CreateDocument();
	uid_diag_list_t diags(lim.maxDiagnostics);
	FakeBackendState st;
	SeedDefaultCvars(&st);
	uid_backend_t be = MakeFakeBackend(&st);
	uid_parse_io_t parseIo = MakeTestParseIo();

	const std::string vfsName = std::string("ui/modern/") + docRel;
	if (UID_ParseXml(vfsName.c_str(), xml.c_str(), xml.size(), &lim, &parseIo, doc, &diags) != UID_OK) {
		std::fprintf(stderr, "parse failed for %s\n", vfsName.c_str());
		for (const uid_diag_t &d : diags.Items()) {
			std::fprintf(stderr, "  %s\n", d.message.c_str());
		}
		UID_DestroyDocument(doc);
		return 1;
	}
	if (UID_ExpandDocument(doc, &diags) != UID_OK) {
		std::fprintf(stderr, "expand failed\n");
		UID_DestroyDocument(doc);
		return 1;
	}
	if (UID_CompileDocument(doc, &diags) != UID_OK) {
		std::fprintf(stderr, "compile failed\n");
		UID_DestroyDocument(doc);
		return 1;
	}

	PhaseSamples samples;
	const int totalFrames = warmup + frames;
	for (int frame = 0; frame < totalFrames; ++frame) {
		const bool timed = frame >= warmup;
		const bool last = (frame == totalFrames - 1);

		st.solidRects = 0;
		st.fontDraws = 0;
		st.imageDraws = 0;
		st.pathDraws = 0;
		st.gradientDraws = 0;
		st.pushClips = 0;
		st.popClips = 0;
		st.ordered.clear();
		st.recordOrdered = last && dumpLog != nullptr;

		using clock = std::chrono::steady_clock;
		const auto t0 = clock::now();
		UID_SyncBindings(doc, &be);
		const auto t1 = clock::now();

		long long layoutUs = 0;
		if (doc->dirty & (UID_DIRTY_STRUCTURE | UID_DIRTY_LAYOUT)) {
			const auto l0 = clock::now();
			UID_LayoutDocument(doc, 1920, 1080, 1.0f, 1.0f, &be, &diags);
			const auto l1 = clock::now();
			layoutUs = std::chrono::duration_cast<std::chrono::microseconds>(l1 - l0).count();
		}
		const auto t2 = clock::now();
		UID_PaintChrome(doc, &be);
		const auto t3 = clock::now();
		UID_PaintOverlay(doc, &be);
		const auto t4 = clock::now();

		const long long bindUs =
			std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
		const long long paintUs =
			std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
		const long long overlayUs =
			std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3).count();
		const long long totalUs = bindUs + layoutUs + paintUs + overlayUs;

		if (timed) {
			samples.bindUs.push_back(bindUs);
			samples.layoutUs.push_back(layoutUs);
			samples.paintUs.push_back(paintUs);
			samples.overlayUs.push_back(overlayUs);
			samples.totalUs.push_back(totalUs);
		}
	}

	std::printf(
		"uid_bench_menu doc=%s nodes=%zu frames=%d warmup=%d opt=0x%08x\n",
		docRel,
		doc->nodes.size(),
		frames,
		warmup,
		optMask
	);
	PrintPhase("bind", samples.bindUs);
	PrintPhase("layout", samples.layoutUs);
	PrintPhase("paint", samples.paintUs);
	PrintPhase("overlay", samples.overlayUs);
	PrintPhase("total", samples.totalUs);
	std::printf(
		"  last_calls solid=%d font=%d image=%d path=%d gradient=%d pushClip=%d popClip=%d\n",
		st.solidRects,
		st.fontDraws,
		st.imageDraws,
		st.pathDraws,
		st.gradientDraws,
		st.pushClips,
		st.popClips
	);

	if (!st.unknownCvars.empty()) {
		std::printf("  unknown_cvars_auto_zeroed=%zu:", st.unknownCvars.size());
		size_t shown = 0;
		for (const auto &name : st.unknownCvars) {
			if (shown >= 24) {
				std::printf(" ...");
				break;
			}
			std::printf(" %s", name.c_str());
			++shown;
		}
		std::printf("\n");
	}

	if (dumpLog) {
		FILE *out = std::fopen(dumpLog, "wb");
		if (!out) {
			std::fprintf(stderr, "failed to write %s\n", dumpLog);
			UID_DestroyDocument(doc);
			return 1;
		}
		std::fprintf(out, "# DRAW\n");
		for (const auto &c : st.ordered) {
			if (c.kind == "DRAW") {
				std::fprintf(out, "%s\n", c.line.c_str());
			}
		}
		std::fprintf(out, "# CLIP\n");
		for (const auto &c : st.ordered) {
			if (c.kind == "CLIP") {
				std::fprintf(out, "%s\n", c.line.c_str());
			}
		}
		std::fclose(out);
		std::printf("  dumped_log=%s ordered=%zu\n", dumpLog, st.ordered.size());
	}

	UID_DestroyDocument(doc);
	return 0;
}
