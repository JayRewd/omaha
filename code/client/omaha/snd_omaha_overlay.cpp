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

// DESCRIPTION:
// Omaha sound host debug overlays, drawn from the 2D HUD pass.

#include "../snd_local.h"
#include "../client.h"
#include "snd_renderer.h"

static cvar_t *s_hf_duck_meter;

/*
 * HF duck meter: left = HP sidechain level + threshold line; right = shelf cut.
 * Uses DrawBox (not DrawStretchPic) so SetColor modulates correctly in MOH 2D.
 */
static void Om_MeterFillBox(float x, float y, float w, float h, const float *rgba)
{
	re.SetColor(rgba);
	re.DrawBox(x, y, w, h);
}

static void Om_MeterLabel(fontheader_t *font, float x, float y, const char *text, const float *rgba)
{
	if (!font || !text || !text[0]) {
		return;
	}
	re.SetColor(rgba);
	re.DrawString(font, text, x, y, -1, NULL);
}

static void Om_DrawMeterBar(
	fontheader_t *font, float x, float y, float meterW, float meterH, float t, const float *fillColor,
	const char *title, const char *topLabel, const char *botLabel
)
{
	vec4_t bg, edge, text;
	float  fillH;
	int    i;

	bg[0] = 0.08f;
	bg[1] = 0.08f;
	bg[2] = 0.1f;
	bg[3] = 0.75f;
	edge[0] = 0.85f;
	edge[1] = 0.85f;
	edge[2] = 0.9f;
	edge[3] = 1.0f;
	text[0] = text[1] = text[2] = 1.0f;
	text[3] = 1.0f;

	if (t < 0.0f) {
		t = 0.0f;
	} else if (t > 1.0f) {
		t = 1.0f;
	}
	fillH = t * meterH;

	Om_MeterFillBox(x, y, meterW, meterH, bg);
	if (fillH > 0.5f) {
		Om_MeterFillBox(x, y + meterH - fillH, meterW, fillH, fillColor);
	}
	/* Frame + quarter ticks */
	Om_MeterFillBox(x, y, meterW, 1.0f, edge);
	Om_MeterFillBox(x, y + meterH - 1.0f, meterW, 1.0f, edge);
	Om_MeterFillBox(x, y, 1.0f, meterH, edge);
	Om_MeterFillBox(x + meterW - 1.0f, y, 1.0f, meterH, edge);
	for (i = 1; i < 4; i++) {
		const float ty = y + (meterH * (float)i) / 4.0f;
		Om_MeterFillBox(x, ty, 5.0f, 1.0f, edge);
		Om_MeterFillBox(x + meterW - 5.0f, ty, 5.0f, 1.0f, edge);
	}

	Om_MeterLabel(font, x, y - 16.0f, title, text);
	Om_MeterLabel(font, x + meterW + 6.0f, y, topLabel, text);
	Om_MeterLabel(font, x + meterW + 6.0f, y + meterH - 14.0f, botLabel, text);
}

static void Om_DrawHfDuckMeter(void)
{
	static fontheader_t *s_meterFont;
	const float          meterH     = 500.0f;
	const float          meterW     = 18.0f;
	const float          margin     = 12.0f;
	const float          scFloorDb  = -60.0f;
	const float          scCeilDb   = 0.0f;
	float                gainDb, maxCut, duckT;
	float                scDb, threshDb, scT, threshT;
	float                y, leftX, rightX, threshY;
	vec4_t               duckFill, scFill, threshCol, liveCol;
	char                 topDuck[32], botDuck[16], topSc[16], botSc[16], thrLab[32], liveLab[32];

	/* Opt-in; the duck itself still needs s_mix + s_hf_duck. */
	if (!s_hf_duck_meter) {
		s_hf_duck_meter = Cvar_Get("s_hf_duck_meter", "0", 0);
	}
	if (!s_hf_duck_meter->integer) {
		return;
	}
	if (!Cvar_VariableIntegerValue("s_mix") || !Cvar_VariableIntegerValue("s_hf_duck")) {
		return;
	}

	if (!s_meterFont) {
		s_meterFont = re.LoadFont("verdana-14");
	}

	gainDb   = S_Omaha_HfDuckGainDb();
	maxCut   = S_Omaha_HfDuckMaxCutDb();
	scDb     = S_Omaha_HfDuckSidechainDb();
	threshDb = S_Omaha_HfDuckThreshDb();

	y = ((float)cls.glconfig.vidHeight - meterH) * 0.5f;
	if (y < margin + 20.0f) {
		y = margin + 20.0f;
	}
	leftX  = margin;
	rightX = (float)cls.glconfig.vidWidth - margin - meterW - 56.0f;

	scT = (scDb - scFloorDb) / (scCeilDb - scFloorDb);
	threshT = (threshDb - scFloorDb) / (scCeilDb - scFloorDb);
	if (threshT < 0.0f) {
		threshT = 0.0f;
	} else if (threshT > 1.0f) {
		threshT = 1.0f;
	}

	scFill[0] = 0.2f;
	scFill[1] = 0.7f;
	scFill[2] = 1.0f;
	scFill[3] = 0.9f;
	Com_sprintf(topSc, sizeof(topSc), "0 dB");
	Com_sprintf(botSc, sizeof(botSc), "-60");
	Om_DrawMeterBar(s_meterFont, leftX, y, meterW, meterH, scT, scFill, "HF SC", topSc, botSc);

	threshY = y + meterH - threshT * meterH;
	threshCol[0] = 1.0f;
	threshCol[1] = 0.85f;
	threshCol[2] = 0.1f;
	threshCol[3] = 1.0f;
	Om_MeterFillBox(leftX - 4.0f, threshY, meterW + 8.0f, 2.0f, threshCol);
	Com_sprintf(thrLab, sizeof(thrLab), "thr %.0f", threshDb);
	Om_MeterLabel(s_meterFont, leftX + meterW + 6.0f, threshY - 7.0f, thrLab, threshCol);

	liveCol[0] = 0.7f;
	liveCol[1] = 0.9f;
	liveCol[2] = 1.0f;
	liveCol[3] = 1.0f;
	Com_sprintf(liveLab, sizeof(liveLab), "%.1f dB", scDb);
	Om_MeterLabel(s_meterFont, leftX, y + meterH + 4.0f, liveLab, liveCol);

	if (maxCut < 0.05f) {
		duckT = 0.0f;
	} else {
		duckT = -gainDb / maxCut;
	}
	if (duckT < 0.0f) {
		duckT = 0.0f;
	} else if (duckT > 1.0f) {
		duckT = 1.0f;
	}
	duckFill[0] = 0.15f + 0.85f * duckT;
	duckFill[1] = 0.9f - 0.55f * duckT;
	duckFill[2] = 0.15f;
	duckFill[3] = 0.9f;
	Com_sprintf(topDuck, sizeof(topDuck), "-%.1f dB", maxCut);
	Com_sprintf(botDuck, sizeof(botDuck), "0");
	Om_DrawMeterBar(s_meterFont, rightX, y, meterW, meterH, duckT, duckFill, "SHELF", topDuck, botDuck);
	Com_sprintf(liveLab, sizeof(liveLab), "%.2f dB", gainDb);
	Om_MeterLabel(s_meterFont, rightX - 8.0f, y + meterH + 4.0f, liveLab, duckFill);

	re.SetColor(NULL);
}

void S_OMAHA_DrawOverlays(void)
{
	if (s_sndHost != &s_sndHostOmaha) {
		return;
	}
	Om_DrawHfDuckMeter();
}
