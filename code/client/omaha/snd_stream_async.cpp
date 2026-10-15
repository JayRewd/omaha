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

// snd_stream_async.cpp -- decode-ahead worker for 2D sound streams.

#include "snd_stream_async.h"

#include <condition_variable>
#include <mutex>
#include <string.h>
#include <thread>

#define ASYNC_STREAM_MAX_JOBS   16
#define ASYNC_STREAM_RING       8
#define ASYNC_STREAM_CHUNK_SIZE (2048 * 2 * 2)

struct sndAsyncStream_s {
    snd_stream_t *stream;
    unsigned int  chunkBytes;
    unsigned int  sizes[ASYNC_STREAM_RING];
    byte          data[ASYNC_STREAM_RING][ASYNC_STREAM_CHUNK_SIZE];
    int           head;  // next chunk to pop (main thread)
    int           count; // decoded chunks waiting
    unsigned int  lost;  // bytes decoded after dead was set (codec advanced, never queued)
    bool          eof;
    bool          busy;  // worker is inside S_CodecReadStream for this job
    bool          dead;  // never decode again (detaching, or handles closed by FS_Shutdown)
};

static std::mutex              s_asyncMutex;
static std::condition_variable s_asyncWork;
static std::condition_variable s_asyncIdle;
static std::thread             s_asyncThread;
static bool                    s_asyncQuit;
static sndAsyncStream_t       *s_asyncJobs[ASYNC_STREAM_MAX_JOBS];
static int                     s_asyncNext;
static cvar_t                 *s_streamAsync;

static sndAsyncStream_t *S_AsyncStream_NextJob(void)
{
    int i;

    for (i = 0; i < ASYNC_STREAM_MAX_JOBS; i++) {
        sndAsyncStream_t *job = s_asyncJobs[(s_asyncNext + i) % ASYNC_STREAM_MAX_JOBS];

        if (job && !job->dead && !job->eof && job->count < ASYNC_STREAM_RING) {
            s_asyncNext = (s_asyncNext + i + 1) % ASYNC_STREAM_MAX_JOBS;
            return job;
        }
    }
    return NULL;
}

static void S_AsyncStream_Worker(void)
{
    std::unique_lock<std::mutex> lock(s_asyncMutex);

    while (!s_asyncQuit) {
        sndAsyncStream_t *job = S_AsyncStream_NextJob();
        int               slot, bytes;

        if (!job) {
            s_asyncWork.wait(lock);
            continue;
        }

        // The tail slot is outside [head, head + count), so the main thread
        // never reads it while we decode into it unlocked.
        slot      = (job->head + job->count) % ASYNC_STREAM_RING;
        job->busy = true;
        lock.unlock();

        bytes = S_CodecReadStream(job->stream, job->chunkBytes, job->data[slot]);

        lock.lock();
        job->busy = false;
        if (job->dead) {
            if (bytes > 0) {
                job->lost += (unsigned int)bytes;
            }
        } else if (bytes > 0) {
            job->sizes[slot] = (unsigned int)bytes;
            job->count++;
        } else {
            job->eof = true;
        }
        s_asyncIdle.notify_all();
    }
}

// FS_Shutdown closes every open handle; keep the worker off them from now on.
static void S_AsyncStream_FSShutdown(void)
{
    std::unique_lock<std::mutex> lock(s_asyncMutex);
    int                          i;

    for (i = 0; i < ASYNC_STREAM_MAX_JOBS; i++) {
        if (s_asyncJobs[i]) {
            s_asyncJobs[i]->dead = true;
        }
    }
    for (i = 0; i < ASYNC_STREAM_MAX_JOBS; i++) {
        while (s_asyncJobs[i] && s_asyncJobs[i]->busy) {
            s_asyncIdle.wait(lock);
        }
    }
}

sndAsyncStream_t *S_AsyncStream_Attach(snd_stream_t *stream, unsigned int chunkBytes)
{
    sndAsyncStream_t *job;
    int               i;

    if (!s_streamAsync) {
        s_streamAsync = Cvar_Get("s_streamAsync", "1", CVAR_ARCHIVE);
    }
    if (!s_streamAsync->integer || !stream || !chunkBytes || chunkBytes > ASYNC_STREAM_CHUNK_SIZE) {
        return NULL;
    }

    job             = new sndAsyncStream_t;
    job->stream     = stream;
    job->chunkBytes = chunkBytes;
    job->head       = 0;
    job->count      = 0;
    job->lost       = 0;
    job->eof        = false;
    job->busy       = false;
    job->dead       = false;

    {
        std::lock_guard<std::mutex> lock(s_asyncMutex);

        for (i = 0; i < ASYNC_STREAM_MAX_JOBS; i++) {
            if (!s_asyncJobs[i]) {
                s_asyncJobs[i] = job;
                break;
            }
        }
        if (i == ASYNC_STREAM_MAX_JOBS) {
            delete job;
            return NULL;
        }

        if (!s_asyncThread.joinable()) {
            s_asyncQuit   = false;
            s_asyncThread = std::thread(S_AsyncStream_Worker);
        }
        fs_shutdownHook = S_AsyncStream_FSShutdown;
    }
    s_asyncWork.notify_one();

    return job;
}

int S_AsyncStream_Pop(sndAsyncStream_t *job, void *out, unsigned int outSize, qboolean wait)
{
    unsigned int size;
    int          slot;

    {
        std::unique_lock<std::mutex> lock(s_asyncMutex);

        while (wait && !job->count && !job->eof && !job->dead && !s_asyncQuit) {
            s_asyncIdle.wait(lock);
        }
        if (!job->count) {
            // dead here means FS_Shutdown closed the handle: end the stream like a failed read
            return (job->eof || job->dead) ? -1 : 0;
        }
        slot = job->head;
    }

    // The head slot stays ours until count is decremented below.
    size = job->sizes[slot];
    if (size > outSize) {
        size = outSize;
    }
    memcpy(out, job->data[slot], size);

    {
        std::lock_guard<std::mutex> lock(s_asyncMutex);

        job->head = (job->head + 1) % ASYNC_STREAM_RING;
        job->count--;
    }
    s_asyncWork.notify_one();

    return (int)size;
}

unsigned int S_AsyncStream_Detach(sndAsyncStream_t *job)
{
    unsigned int discarded = 0;
    int          i;

    {
        std::unique_lock<std::mutex> lock(s_asyncMutex);

        job->dead = true;
        while (job->busy) {
            s_asyncIdle.wait(lock);
        }
        discarded = job->lost;
        for (i = 0; i < job->count; i++) {
            discarded += job->sizes[(job->head + i) % ASYNC_STREAM_RING];
        }
        for (i = 0; i < ASYNC_STREAM_MAX_JOBS; i++) {
            if (s_asyncJobs[i] == job) {
                s_asyncJobs[i] = NULL;
            }
        }
    }

    delete job;
    return discarded;
}

void S_AsyncStream_Shutdown(void)
{
    if (!s_asyncThread.joinable()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(s_asyncMutex);
        s_asyncQuit = true;
    }
    s_asyncWork.notify_all();
    s_asyncThread.join();
}
