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
 * Added in Omaha: the game's text is Windows-1252 (names, chat). The modern
 * UI's own text is UTF-8. These helpers read either encoding so the font can
 * draw both, and write typed text as the game's bytes before it leaves the UI.
 */

#pragma once

#include <stddef.h>

/* Code points of bytes 0x80..0x9F. 0 marks a byte with no character. */
static const unsigned short uir_cp1252_high[32] = {
	0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
	0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

static inline unsigned UIR_Cp1252ToCodepoint(unsigned char byte)
{
	if (byte >= 0x80 && byte < 0xA0) {
		return uir_cp1252_high[byte - 0x80];
	}
	return byte;
}

static inline unsigned char UIR_Cp1252FromCodepoint(unsigned codepoint)
{
	int i;

	if (!codepoint) {
		return 0;
	}
	if (codepoint < 0x80 || (codepoint >= 0xA0 && codepoint <= 0xFF)) {
		return (unsigned char)codepoint;
	}
	for (i = 0; i < 32; i++) {
		if (uir_cp1252_high[i] == codepoint) {
			return (unsigned char)(0x80 + i);
		}
	}
	return 0;
}

/*
 * Reads the character at text (not at the end) and returns how many bytes it
 * took. *byte is the character as a Windows-1252 byte.
 *
 * A UTF-8 sequence counts only when it is well formed and Windows-1252 has
 * that character. Every other byte is the Windows-1252 byte it already is.
 */
static inline int UIR_Cp1252Next(const char *text, unsigned char *byte)
{
	const unsigned char *s = (const unsigned char *)text;
	unsigned             codepoint = 0;
	int                  length = 0;

	if (s[0] >= 0xC2 && s[0] <= 0xDF && (s[1] & 0xC0) == 0x80) {
		codepoint = ((unsigned)(s[0] & 0x1F) << 6) | (unsigned)(s[1] & 0x3F);
		length = 2;
	} else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
		codepoint = ((unsigned)(s[0] & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (unsigned)(s[2] & 0x3F);
		length = codepoint >= 0x800 ? 3 : 0;
	}
	if (length) {
		const unsigned char found = UIR_Cp1252FromCodepoint(codepoint);

		if (found) {
			*byte = found;
			return length;
		}
	}
	*byte = s[0];
	return 1;
}

/* Writes text as the game's bytes. out may be text; the result is never longer. */
static inline void UIR_Cp1252FromText(const char *text, char *out, size_t outSize)
{
	size_t length = 0;

	if (!out || !outSize) {
		return;
	}
	while (text && *text && length + 1 < outSize) {
		unsigned char byte;

		text += UIR_Cp1252Next(text, &byte);
		out[length++] = (char)byte;
	}
	out[length] = 0;
}
