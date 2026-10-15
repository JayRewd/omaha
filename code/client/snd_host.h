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
// Sound host table. A host owns MOH channel policy, music, streams and the
// output device. The stock OpenAL host (snd_openal_new.cpp, unchanged) is
// always built; the Omaha host (omaha/) is optional (USE_OMAHA_SOUND).
// s_backend picks one at S_Init; everything else in the client is host-neutral.
//
// Included from snd_local_new.h (needs soundsystemsavegame_t).

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sndHost_s {
    const char *name;

    // S_Driver_* (snd_dma_new.cpp)
    qboolean (*Init)(void);
    void (*Shutdown)(void);
    void (*StartSound)(
        const vec3_t origin,
        int          entnum,
        int          entchannel,
        sfxHandle_t  sfxHandle,
        float        volume,
        float        minDist,
        float        pitch,
        float        maxDist,
        qboolean     streamed
    );
    void (*AddLoopingSound)(
        const vec3_t origin,
        const vec3_t velocity,
        sfxHandle_t  sfxHandle,
        float        volume,
        float        minDist,
        float        maxDist,
        float        pitch,
        int          flags
    );
    void (*ClearLoopingSounds)(void);
    void (*StopSound)(int entnum, int channel);
    void (*StopAllSounds)(qboolean stopMusic);
    void (*Respatialize)(int entnum, const vec3_t head, const vec3_t axis[3]);
    void (*SetReverb)(int type, float level);
    void (*Update)(void);
    const char *(*GetMusicFilename)(void);
    int (*GetMusicLoopCount)(void);
    unsigned int (*GetMusicOffset)(void);

    // Public sound API implemented by the host (dispatched in snd_host.cpp)
    void (*MusicNewSoundtrack)(const char *name);
    void (*MusicUpdateMood)(int current, int fallback);
    void (*MusicUpdateVolume)(float volume, float fadeTime);
    const char *(*CurrentSoundtrack)(void);
    void (*FadeSound)(float time);
    qboolean (*IsSoundPlaying)(int channelNumber, const char *name);
    void (*TriggeredMusicSetupHandle)(const char *name, int loopCount, int offset, qboolean autostart);
    void (*TriggeredMusicPlayIntroMusic)(void);
    void (*TriggeredMusicStop)(void);
    void (*SetupMovieAudio)(const char *movieName);
    void (*StopMovieAudio)(void);
    int (*CurrentMoviePosition)(void);
    void (*LoadData)(soundsystemsavegame_t *save);
    void (*SaveData)(soundsystemsavegame_t *save);
    void (*ReLoad)(soundsystemsavegame_t *save);
    qboolean (*NeedFullRestart)(void);
    void (*PrintInfo)(void);
    void (*DumpInfo)(void);

    // Mixed PCM for AVI / MP4 demo export
    qboolean (*CaptureBegin)(void);
    void (*CaptureEnd)(void);
    void (*CaptureFrame)(int aviFps);
    qboolean (*CaptureActive)(void);
    int (*CaptureSampleRate)(void);
} sndHost_t;

extern const sndHost_t  s_sndHostOpenAL;
extern const sndHost_t  s_sndHostOmaha;
extern const sndHost_t *s_sndHost;

// Picks the host from s_backend and initializes it (omaha falls back to OpenAL).
qboolean S_Host_Init(void);

#ifdef __cplusplus
}
#endif
