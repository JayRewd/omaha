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
 * PCM and ADPCM decode for the Omaha mixer.
 * IMA follows the IMA/DVI ADPCM WAV layout. MS ADPCM follows the WAV spec
 * (predictor header, adaptation table, coefficient pairs).
 */

#include "snd_adpcm_omaha.h"

#include <stdlib.h>
#include <string.h>

/* IMA/DVI step sizes. */
static const int kImaStep[89] = {
	7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,    31,
	34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,
	157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,
	724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
	3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22358, 24633, 27086, 29794, 32767
};

static const int kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

/* MS ADPCM adaptation and coefficient pairs (WAV spec). */
static const int kMsAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

static const int kMsCoef[7][2] = {
	{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}
};

static int Om_Clamp16(int v)
{
	if (v > 32767) {
		return 32767;
	}
	if (v < -32768) {
		return -32768;
	}
	return v;
}

static int Om_ImaNibble(int *sample, int *index, int nibble)
{
	int step = kImaStep[*index];
	int diff = step >> 3;

	if (nibble & 1) {
		diff += step >> 2;
	}
	if (nibble & 2) {
		diff += step >> 1;
	}
	if (nibble & 4) {
		diff += step;
	}
	if (nibble & 8) {
		diff = -diff;
	}
	*sample = Om_Clamp16(*sample + diff);
	*index += kImaIndex[nibble & 15];
	if (*index < 0) {
		*index = 0;
	}
	if (*index > 88) {
		*index = 88;
	}
	return *sample;
}

/*
 * WAVE DVI/IMA ADPCM: 4-byte header per channel, then 4-byte packets of 8
 * nibbles (low nibble first). Stereo interleaves those packets per channel.
 */
static int Om_ImaNibbleAt(const unsigned char *nibbles, int channels, int n)
{
	const int byteShift  = (n & 1) * 4;
	const int wordOffset = (n >> 1) & ~3;
	const int byteOffset = wordOffset * channels + ((n >> 1) & 3);
	return (nibbles[byteOffset] >> byteShift) & 0xf;
}

static qboolean Om_DecodeIma(const unsigned char *src, int sizeBytes, int channels, int samplesPerBlock, omPcmDecode_t *out)
{
	int blockBytes;
	int blocks, b, ch, n;
	float *dst;

	if (channels < 1 || channels > 2 || samplesPerBlock <= 1) {
		return qfalse;
	}
	blockBytes = ((samplesPerBlock - 1) / 2 + 4) * channels;
	if (blockBytes <= 0 || sizeBytes < blockBytes || (sizeBytes % blockBytes) != 0) {
		return qfalse;
	}
	blocks           = sizeBytes / blockBytes;
	out->channels    = channels;
	out->frames      = blocks * samplesPerBlock;
	out->interleaved = (float *)malloc((size_t)out->frames * (size_t)channels * sizeof(float));
	if (!out->interleaved) {
		return qfalse;
	}
	dst = out->interleaved;
	for (b = 0; b < blocks; b++) {
		const unsigned char *block = src + b * blockBytes;
		for (ch = 0; ch < channels; ch++) {
			int                  sample  = (int)(short)(block[ch * 4] | (block[ch * 4 + 1] << 8));
			int                  index   = block[ch * 4 + 2];
			const unsigned char *nibbles = block + (channels + ch) * 4;
			int                  frame0  = b * samplesPerBlock;

			if (index > 88) {
				index = 88;
			}
			dst[(frame0 * channels) + ch] = (float)sample * (1.0f / 32768.0f);
			for (n = 0; n < samplesPerBlock - 1; n++) {
				sample = Om_ImaNibble(&sample, &index, Om_ImaNibbleAt(nibbles, channels, n));
				dst[((frame0 + n + 1) * channels) + ch] = (float)sample * (1.0f / 32768.0f);
			}
		}
	}
	return qtrue;
}

static int Om_MsNibble(int nibble, int *s1, int *s2, int *delta, int coef0, int coef1)
{
	int pred;
	int n = nibble;
	if (n & 8) {
		n -= 16;
	}
	pred = ((*s1 * coef0) + (*s2 * coef1)) >> 8;
	pred = Om_Clamp16(pred + n * *delta);
	*s2  = *s1;
	*s1  = pred;
	*delta = (kMsAdapt[nibble & 15] * *delta) >> 8;
	if (*delta < 16) {
		*delta = 16;
	}
	return pred;
}

static qboolean Om_DecodeMs(const unsigned char *src, int sizeBytes, int channels, int samplesPerBlock, omPcmDecode_t *out)
{
	int blockBytes;
	int blocks, b, ch, n, i, pos;
	int s1[2], s2[2], delta[2], pred[2];
	float *dst;

	if (channels < 1 || channels > 2 || samplesPerBlock <= 2) {
		return qfalse;
	}
	blockBytes = 7 * channels + ((samplesPerBlock - 2) * channels + 1) / 2;
	if (blockBytes <= 0 || sizeBytes < blockBytes || (sizeBytes % blockBytes) != 0) {
		return qfalse;
	}
	blocks        = sizeBytes / blockBytes;
	out->channels = channels;
	out->frames   = blocks * samplesPerBlock;
	out->interleaved = (float *)malloc((size_t)out->frames * (size_t)channels * sizeof(float));
	if (!out->interleaved) {
		return qfalse;
	}
	dst = out->interleaved;
	i   = 0;
	for (b = 0; b < blocks; b++) {
		const unsigned char *block = src + b * blockBytes;
		pos                        = 0;
		for (ch = 0; ch < channels; ch++) {
			pred[ch]  = block[pos];
			if (pred[ch] > 6) {
				pred[ch] = 0;
			}
			delta[ch] = (int)(short)(block[pos + 1] | (block[pos + 2] << 8));
			s1[ch]    = (int)(short)(block[pos + 3] | (block[pos + 4] << 8));
			s2[ch]    = (int)(short)(block[pos + 5] | (block[pos + 6] << 8));
			pos += 7;
			if (delta[ch] < 16) {
				delta[ch] = 16;
			}
		}
		for (ch = 0; ch < channels; ch++) {
			dst[i * channels + ch] = (float)s2[ch] * (1.0f / 32768.0f);
		}
		i++;
		for (ch = 0; ch < channels; ch++) {
			dst[i * channels + ch] = (float)s1[ch] * (1.0f / 32768.0f);
		}
		i++;
		n = 2;
		while (n < samplesPerBlock && pos < blockBytes) {
			int byte = block[pos++];
			int hi   = byte >> 4;
			int lo   = byte & 15;
			ch       = 0;
			dst[i * channels + ch] =
				(float)Om_MsNibble(hi, &s1[ch], &s2[ch], &delta[ch], kMsCoef[pred[ch]][0], kMsCoef[pred[ch]][1])
				* (1.0f / 32768.0f);
			n++;
			if (channels == 1) {
				if (n >= samplesPerBlock) {
					break;
				}
				i++;
				dst[i * channels] =
					(float)Om_MsNibble(lo, &s1[0], &s2[0], &delta[0], kMsCoef[pred[0]][0], kMsCoef[pred[0]][1])
					* (1.0f / 32768.0f);
				n++;
				i++;
			} else {
				ch = 1;
				dst[i * channels + ch] =
					(float)Om_MsNibble(lo, &s1[ch], &s2[ch], &delta[ch], kMsCoef[pred[ch]][0], kMsCoef[pred[ch]][1])
					* (1.0f / 32768.0f);
				n++;
				i++;
			}
		}
	}
	return qtrue;
}

static qboolean Om_DecodePcm(omSampleFormat_t fmt, const void *data, int sizeBytes, omPcmDecode_t *out)
{
	int channels = (fmt == OM_FMT_STEREO8 || fmt == OM_FMT_STEREO16 || fmt == OM_FMT_STEREO_F32) ? 2 : 1;
	int i, n;
	float *dst;

	if (fmt == OM_FMT_MONO16 || fmt == OM_FMT_STEREO16) {
		n = sizeBytes / (2 * channels);
	} else if (fmt == OM_FMT_MONO_F32 || fmt == OM_FMT_STEREO_F32) {
		n = sizeBytes / (4 * channels);
	} else {
		n = sizeBytes / channels;
	}
	if (n <= 0) {
		return qfalse;
	}
	dst = (float *)malloc((size_t)n * (size_t)channels * sizeof(float));
	if (!dst) {
		return qfalse;
	}
	if (fmt == OM_FMT_MONO16 || fmt == OM_FMT_STEREO16) {
		const short *s = (const short *)data;
		for (i = 0; i < n * channels; i++) {
			dst[i] = (float)s[i] * (1.0f / 32768.0f);
		}
	} else if (fmt == OM_FMT_MONO_F32 || fmt == OM_FMT_STEREO_F32) {
		memcpy(dst, data, (size_t)n * (size_t)channels * sizeof(float));
	} else {
		const unsigned char *s = (const unsigned char *)data;
		for (i = 0; i < n * channels; i++) {
			dst[i] = (float)s[i] * (1.0f / 128.0f) - 1.0f;
		}
	}
	out->channels    = channels;
	out->frames      = n;
	out->interleaved = dst;
	return qtrue;
}

qboolean OmPcm_Decode(omSampleFormat_t fmt, const void *data, int sizeBytes, int blockAlign, omPcmDecode_t *out)
{
	int samples;

	memset(out, 0, sizeof(*out));
	if (!data || sizeBytes <= 0) {
		return qfalse;
	}
	switch (fmt) {
	case OM_FMT_MONO8:
	case OM_FMT_MONO16:
	case OM_FMT_STEREO8:
	case OM_FMT_STEREO16:
	case OM_FMT_MONO_F32:
	case OM_FMT_STEREO_F32:
		return Om_DecodePcm(fmt, data, sizeBytes, out);
	case OM_FMT_MONO_IMA4:
	case OM_FMT_STEREO_IMA4:
		samples = blockAlign > 1 ? blockAlign : 65;
		return Om_DecodeIma((const unsigned char *)data, sizeBytes, fmt == OM_FMT_STEREO_IMA4 ? 2 : 1, samples, out);
	case OM_FMT_MONO_MSADPCM:
	case OM_FMT_STEREO_MSADPCM:
		samples = blockAlign > 2 ? blockAlign : 64;
		return Om_DecodeMs((const unsigned char *)data, sizeBytes, fmt == OM_FMT_STEREO_MSADPCM ? 2 : 1, samples, out);
	default:
		return qfalse;
	}
}

void OmPcm_Free(omPcmDecode_t *pcm)
{
	if (!pcm) {
		return;
	}
	free(pcm->interleaved);
	pcm->interleaved = NULL;
	pcm->frames      = 0;
	pcm->channels    = 0;
}
