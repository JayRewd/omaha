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
// Sound host selection and the public sound API entry points that forward to
// the active host (see snd_host.h).

#include "snd_local.h"
#include "client.h"

// Never NULL: stock entry points tolerate an uninitialized OpenAL host.
const sndHost_t *s_sndHost = &s_sndHostOpenAL;

#ifdef USE_OMAHA_SOUND
static cvar_t *s_backend;
#endif

qboolean S_Host_Init(void)
{
#ifdef USE_OMAHA_SOUND
    s_backend = Cvar_Get("s_backend", "omaha", CVAR_ARCHIVE | CVAR_SOUND_LATCH);
    if (Q_stricmp(s_backend->string, "openal")) {
        /*
         * Added in Omaha: surround/quad need Soft; clamp to headphones HRTF so a
         * leftover s_speaker_type 2/3 does not force a silent fall back to OpenAL
         * while the UI still says Omaha.
         */
        if (Cvar_VariableIntegerValue("s_speaker_type") >= 2) {
            Com_Printf("Sound: speaker_type %i (surround) is not supported by omaha.\n",
                       Cvar_VariableIntegerValue("s_speaker_type"));
            Cvar_Set("s_speaker_type", "1");
            Com_Printf("Sound: omaha does not support surround; using Headphones (HRTF).\n");
        }
        s_sndHost = &s_sndHostOmaha;
        if (s_sndHost->Init()) {
            return qtrue;
        }
        Com_Printf("Sound: omaha host unavailable, falling back to OpenAL.\n");
    }
#endif

    s_sndHost = &s_sndHostOpenAL;
    return s_sndHost->Init();
}

void MUSIC_NewSoundtrack(const char *name)
{
    s_sndHost->MusicNewSoundtrack(name);
}

void MUSIC_UpdateMood(int current, int fallback)
{
    s_sndHost->MusicUpdateMood(current, fallback);
}

void MUSIC_UpdateVolume(float volume, float fade_time)
{
    s_sndHost->MusicUpdateVolume(volume, fade_time);
}

const char *S_CurrentSoundtrack()
{
    return s_sndHost->CurrentSoundtrack();
}

void S_FadeSound(float fTime)
{
    s_sndHost->FadeSound(fTime);
}

qboolean S_IsSoundPlaying(int channelNumber, const char *name)
{
    return s_sndHost->IsSoundPlaying(channelNumber, name);
}

void S_TriggeredMusic_SetupHandle(const char *pszName, int iLoopCount, int iOffset, qboolean autostart)
{
    s_sndHost->TriggeredMusicSetupHandle(pszName, iLoopCount, iOffset, autostart);
}

void S_TriggeredMusic_PlayIntroMusic()
{
    s_sndHost->TriggeredMusicPlayIntroMusic();
}

void S_TriggeredMusic_Stop()
{
    s_sndHost->TriggeredMusicStop();
}

void S_SetupMovieAudio(const char *pszMovieName)
{
    s_sndHost->SetupMovieAudio(pszMovieName);
}

void S_StopMovieAudio()
{
    s_sndHost->StopMovieAudio();
}

int S_CurrentMoviePosition()
{
    return s_sndHost->CurrentMoviePosition();
}

void S_LoadData(soundsystemsavegame_t *pSave)
{
    s_sndHost->LoadData(pSave);
}

void S_SaveData(soundsystemsavegame_t *pSave)
{
    s_sndHost->SaveData(pSave);
}

void S_ReLoad(soundsystemsavegame_t *pSave)
{
    s_sndHost->ReLoad(pSave);
}

qboolean S_NeedFullRestart()
{
    return s_sndHost->NeedFullRestart();
}

void S_PrintInfo()
{
    s_sndHost->PrintInfo();
}

void S_DumpInfo()
{
    s_sndHost->DumpInfo();
}

qboolean S_Capture_Begin(void)
{
    return s_sndHost->CaptureBegin();
}

void S_Capture_End(void)
{
    s_sndHost->CaptureEnd();
}

void S_Capture_Frame(int aviFps)
{
    s_sndHost->CaptureFrame(aviFps);
}

qboolean S_Capture_Active(void)
{
    return s_sndHost->CaptureActive();
}

int S_Capture_SampleRate(void)
{
    return s_sndHost->CaptureSampleRate();
}
