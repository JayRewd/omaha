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

#include "uir_debug.h"

#include <stdarg.h>
#include <stdio.h>

static int g_uirDebug = 0;

void UIR_DebugSetEnabled(int enabled)
{
	g_uirDebug = enabled ? 1 : 0;
}

int UIR_DebugEnabled(void)
{
	return g_uirDebug;
}

void UIR_DebugPrintf(const char *fmt, ...)
{
	va_list ap;

	if (!g_uirDebug || !fmt) {
		return;
	}
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void UIR_DebugDumpStats(const uir_stats_t *stats)
{
	if (!g_uirDebug || !stats) {
		return;
	}
	fprintf(
		stderr,
		"UIR stats: sampled=%d ss=%d runs=%d reject=%d boxes=%d fonts=%d previews=%d batches=%d batchVerts=%d batchTris=%d tessFallbacks=%d tessFailStatus=%d tessFailContours=%d tessSkip=%d tessLibFails=%d tessIn=%d tessOut=%d clipApplies=%d clipSkips=%d pathCacheHits=%d pathCacheMisses=%d meshCacheHits=%d meshCacheMisses=%d\n",
		stats->sampledPixels,
		stats->supersamples,
		stats->emittedRuns,
		stats->rejectedOversized,
		stats->drawBoxes,
		stats->fontRebuilds,
		stats->previewCount,
		stats->batches,
		stats->batchVerts,
		stats->batchTris,
		stats->tessFallbacks,
		stats->tessFallbackStatus,
		stats->tessFallbackContours,
		stats->tessSkippedContours,
		stats->tessLibFails,
		stats->tessContoursIn,
		stats->tessContoursOut,
		stats->clipApplies,
		stats->clipSkips,
		stats->pathCacheHits,
		stats->pathCacheMisses,
		stats->meshCacheHits,
		stats->meshCacheMisses
	);
}
