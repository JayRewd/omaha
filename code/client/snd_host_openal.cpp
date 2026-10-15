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
// Stock OpenAL sound host as a sndHost_t. snd_openal_new.cpp is compiled
// unmodified through this file: the public entry points that snd_host.cpp
// dispatches are renamed for this translation unit only, so the stock source
// stays byte-identical to upstream and links next to other hosts.

#define MUSIC_NewSoundtrack             S_OPENAL_Host_MUSIC_NewSoundtrack
#define MUSIC_UpdateMood                S_OPENAL_Host_MUSIC_UpdateMood
#define MUSIC_UpdateVolume              S_OPENAL_Host_MUSIC_UpdateVolume
#define S_CurrentSoundtrack             S_OPENAL_Host_CurrentSoundtrack
#define S_FadeSound                     S_OPENAL_Host_FadeSound
#define S_IsSoundPlaying                S_OPENAL_Host_IsSoundPlaying
#define S_TriggeredMusic_SetupHandle    S_OPENAL_Host_TriggeredMusic_SetupHandle
#define S_TriggeredMusic_PlayIntroMusic S_OPENAL_Host_TriggeredMusic_PlayIntroMusic
#define S_TriggeredMusic_Stop           S_OPENAL_Host_TriggeredMusic_Stop
#define S_SetupMovieAudio               S_OPENAL_Host_SetupMovieAudio
#define S_StopMovieAudio                S_OPENAL_Host_StopMovieAudio
#define S_CurrentMoviePosition          S_OPENAL_Host_CurrentMoviePosition
#define S_LoadData                      S_OPENAL_Host_LoadData
#define S_SaveData                      S_OPENAL_Host_SaveData
#define S_ReLoad                        S_OPENAL_Host_ReLoad
#define S_NeedFullRestart               S_OPENAL_Host_NeedFullRestart
#define S_PrintInfo                     S_OPENAL_Host_PrintInfo
#define S_DumpInfo                      S_OPENAL_Host_DumpInfo

#include "snd_openal_new.cpp"

extern "C" const sndHost_t s_sndHostOpenAL = {
    "openal",

    S_OPENAL_Init,
    S_OPENAL_Shutdown,
    S_OPENAL_StartSound,
    S_OPENAL_AddLoopingSound,
    S_OPENAL_ClearLoopingSounds,
    S_OPENAL_StopSound,
    S_OPENAL_StopAllSounds,
    S_OPENAL_Respatialize,
    S_OPENAL_SetReverb,
    S_OPENAL_Update,
    S_OPENAL_GetMusicFilename,
    S_OPENAL_GetMusicLoopCount,
    S_OPENAL_GetMusicOffset,

    MUSIC_NewSoundtrack,
    MUSIC_UpdateMood,
    MUSIC_UpdateVolume,
    S_CurrentSoundtrack,
    S_FadeSound,
    S_IsSoundPlaying,
    S_TriggeredMusic_SetupHandle,
    S_TriggeredMusic_PlayIntroMusic,
    S_TriggeredMusic_Stop,
    S_SetupMovieAudio,
    S_StopMovieAudio,
    S_CurrentMoviePosition,
    S_LoadData,
    S_SaveData,
    S_ReLoad,
    S_NeedFullRestart,
    S_PrintInfo,
    S_DumpInfo,

    S_OPENAL_BeginAviLoopback,
    S_OPENAL_EndAviLoopback,
    S_OPENAL_AviCaptureFrame,
    S_OPENAL_AviLoopbackActive,
    S_OPENAL_AviSampleRate,
};
