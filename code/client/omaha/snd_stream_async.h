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

// snd_stream_async.h -- decode-ahead worker for 2D sound streams (music, dialog).
//
// While attached, the worker thread owns the codec stream and decodes chunks
// ahead of playback; the main thread only pops finished PCM. Opening, closing,
// seeking and loop restarts stay on the main thread and require Detach first.

#pragma once

#include "../snd_codec.h"

typedef struct sndAsyncStream_s sndAsyncStream_t;

// Hand an open stream to the worker. Returns NULL when async decode is disabled
// (s_streamAsync 0) or the chunk does not fit; the caller keeps decoding inline.
sndAsyncStream_t *S_AsyncStream_Attach(snd_stream_t *stream, unsigned int chunkBytes);

// Next decoded chunk: > 0 bytes copied, 0 = not ready yet, -1 = end of stream.
// With wait set, blocks until the worker finishes the chunk it is decoding
// (use when playback is about to starve).
int S_AsyncStream_Pop(sndAsyncStream_t *job, void *out, unsigned int outSize, qboolean wait);

// Take the stream back. Returns the number of decoded bytes that were never
// popped (the codec position is that far past what the caller consumed).
unsigned int S_AsyncStream_Detach(sndAsyncStream_t *job);

// Stop the worker thread (sound shutdown). All jobs must be detached.
void S_AsyncStream_Shutdown(void);
