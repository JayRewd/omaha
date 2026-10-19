/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

/*
 * Changed in Omaha: forked from the previous sound-host header. Only snd_omaha.cpp includes this.
 */

#pragma once

#include "snd_renderer.h"

typedef int          S32;
typedef unsigned int U32;

/*
 * Changed in Omaha: smaller decode chunks, more queued buffers — same total
 * sample depth as 16×16384, but S_CodecReadStream per client frame is cheaper
 * (crop01 stream refill was ~490µs on 16384, ~127µs on 4096; 2048 targets ~60µs).
 */
#define MAX_STREAM_BUFFERS              128
#define MAX_BUFFER_SAMPLES              2048

/*
 * Savegames still serialize the fixed MAX_SOUNDSYSTEM_* channel table; at
 * runtime the voice pools and loop list grow past those baselines in chunks.
 */
#define SOUNDSYSTEM_VOICE_GROW_CHUNK 32

typedef enum {
    FADE_NONE,
    FADE_IN,
    FADE_OUT
} fade_t;

typedef struct {
    vec3_t vOrigin;
    vec3_t vRelativeOrigin;
    vec3_t vVelocity;

    sfx_t   *pSfx;
    qboolean bPlaying;

    int   iChannel;
    float fVolume;
    float fPitch;

    int      iStartTime;
    qboolean bInUse;
    qboolean bCombine;

    float fBaseVolume;
    float fMinDist;
    float fMaxDist;
    int   iFlags;

    /* Added in Omaha: next in-use slot with the same sfx, or -1. Chain order is ascending index. */
    int iNextSameSfx;
} omaha_loop_sound_t;

struct omaha_channel {
    sfx_t *pSfx;
    int    iEntNum;
    int    iEntChannel;
    vec3_t vOrigin;

    float fVolume;
    int   iBaseRate;
    float fNewPitchMult;
    float fMinDist;
    float fMaxDist;

    int iStartTime;
    int iTime;
    int iEndTime;
    int iFlags;
    int iPausedOffset;

    int song_number;

    fade_t fading;
    int    fade_time;
    int    fade_start_time;

    byte *bufferdata;

    /* Added in Omaha: renderer voice / sound ids. */
    omVoiceId_t voiceId;
    omSoundId_t bufferId;
    int         voiceKind; /* 0=3D, 1=2D, 2=2DStream */

    /* Added in Omaha: Miles-style loop block (sample frames; end < 0 = EOF). */
    int loopBlockStart;
    int loopBlockEnd;

    /* Added in Omaha: omaha-mixer obstruction state. */
    int   iNextObstructionCheck;
    float fObstruction;

    /* Added in Omaha: resolved sndCategory_t of the last 3D start (overlap ducking). */
    int iCategory;

    /* Added in Omaha: last route sent (bus / direct / dry). */
    int iSpatialRoute;

    /* Added in Omaha: params last handed to the mixer, so an unchanged frame skips it. */
    omVoiceParams_t cur;
    omVoiceParams_t sent;
    qboolean        sentOk;

    /* Added in Omaha: last obstruction trace endpoints (bitwise) and time. */
    vec3_t vTraceListener;
    vec3_t vTraceOrigin;
    int    iTraceTime;
    qboolean bTraceValid;

public:
    void         play();
    virtual void stop();
    void         pause();

    void  set_no_3d();
    void  set_3d();
    void  set_no_virtualization();
    void  set_virtualization();

    void flush_params();
    void set_gain(float gain);
    void set_velocity(float v0, float v1, float v2);
    void set_position(float v0, float v1, float v2);

    bool is_free();
    bool is_paused();
    bool is_playing();

    void         force_free();
    virtual bool set_sfx(sfx_t *pSfx);

    void start_sample();
    void stop_sample();
    void resume_sample();
    void end_sample();

    void set_sample_pan(S32 pan);
    void set_sample_playback_rate(S32 pan);
    S32  sample_playback_rate();

    S32          sample_volume();
    virtual U32  sample_offset();
    U32          sample_ms_offset();
    U32          sample_loop_count();
    virtual void set_sample_offset(U32 offset);
    void         set_sample_ms_offset(U32 offset);
    virtual void set_sample_loop_count(S32 count);
    void         set_sample_loop_block(S32 start_offset, S32 end_offset);

    U32 sample_status();

public:
    virtual void update();

    virtual U32 buffer_frequency() const;
};

struct omaha_channel_two_d_stream : public omaha_channel {
private:
    char         fileName[64];
    void        *streamHandle;
    unsigned int bufferSizes[MAX_STREAM_BUFFERS]; // Added in Omaha: byte size of each queued chunk
    unsigned int lastBitsPerSample;               // Added in Omaha: bits * channels of the last chunk
    unsigned int currentBuf;
    unsigned int sampleLoopCount;
    unsigned int sampleLooped;
    unsigned int streamNextOffset;
    bool         streaming;
    struct sndAsyncStream_s *asyncJob; // Added in Omaha: decode-ahead worker owns streamHandle while set

public:
    omaha_channel_two_d_stream();
    ~omaha_channel_two_d_stream();

    void stop() override;
    bool set_sfx(sfx_t *pSfx) override;
    void set_sample_loop_count(S32 count) override;
    void update() override;
    U32  sample_offset() override;
    void set_sample_offset(U32 offset) override;

    bool queue_stream(const char *fileName);

protected:
    U32 buffer_frequency() const override;

private:
    void clear_stream();
    void queueChunk(const void *data, unsigned int size, const void *streamHandle); // Added in Omaha
    void attachAsync();            // Added in Omaha
    unsigned int detachAsync();    // Added in Omaha

    unsigned int getQueueLength() const;
    unsigned int getCurrentStreamPosition() const;
    unsigned int getBitsPerSample() const;
};

struct omaha_movie_channel : public omaha_channel {

};

struct omaha_internal_t {
    omaha_channel              chan_3D[MAX_SOUNDSYSTEM_CHANNELS_3D];
    omaha_channel              chan_2D[MAX_SOUNDSYSTEM_CHANNELS_2D];
    omaha_channel_two_d_stream chan_2D_stream[MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM];
    omaha_channel_two_d_stream chan_song[MAX_SOUNDSYSTEM_SONGS];
    omaha_channel_two_d_stream chan_mp3;
    omaha_channel_two_d_stream chan_trig_music;
    omaha_channel_two_d_stream chan_movie;

    // Pointers to channels (baseline slots; overflow appended at runtime).
    omaha_channel *channel[MAX_SOUNDSYSTEM_CHANNELS];

    /* Added in Omaha: growable overflow beyond the baseline fixed pools. */
    omaha_channel            **overflow_3D;
    int                         overflow_3D_count;
    int                         overflow_3D_capacity;
    omaha_channel            **overflow_2D;
    int                         overflow_2D_count;
    int                         overflow_2D_capacity;
    omaha_channel_two_d_stream **overflow_2D_stream;
    int                         overflow_2D_stream_count;
    int                         overflow_2D_stream_capacity;
    omaha_channel            **channel_extra;
    int                         channel_extra_count;
    int                         channel_extra_capacity;
    int                         position_channel_count; /* baseline + overflow */

    omaha_loop_sound_t *loop_sounds;
    int                  loop_sounds_count;
    int                  loop_sounds_capacity;

    omaha_channel movieChannel;
    sfx_t          movieSFX;

    char tm_filename[MAX_RES_NAME];
    int  tm_loopcount;
};

#ifdef __cplusplus
extern "C" {
#endif

    qboolean S_OMAHA_Init();
    void     S_OMAHA_Shutdown();
    void     S_OMAHA_StartSound(
            const vec3_t vOrigin,
            int          iEntNum,
            int          iEntChannel,
            sfxHandle_t  sfxHandle,
            float        fVolume,
            float        fMinDist,
            float        fPitch,
            float        fMaxDist,
            qboolean     bStreamed
        );
    void S_OMAHA_AddLoopingSound(
        const vec3_t vOrigin,
        const vec3_t vVelocity,
        sfxHandle_t  sfxHandle,
        float        fVolume,
        float        fMinDist,
        float        fMaxDist,
        float        fPitch,
        int          iFlags
    );
    void S_OMAHA_ClearLoopingSounds();
    void S_OMAHA_StopSound(int iEntNum, int iEntChannel);
    void S_OMAHA_StopAllSounds(qboolean bStopMusic);
    void S_OMAHA_Respatialize(int iEntNum, const vec3_t vHeadPos, const vec3_t vAxis[3]);
    void S_OMAHA_SetReverb(int iType, float fLevel);
    void S_OMAHA_Update();

    const char  *S_OMAHA_GetMusicFilename();
    int          S_OMAHA_GetMusicLoopCount();
    unsigned int S_OMAHA_GetMusicOffset();

#ifdef __cplusplus
}
#endif
