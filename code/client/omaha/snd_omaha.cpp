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
 * Changed in Omaha: forked from snd_openal_new.cpp as the Project: Omaha sound
 * host (MOH channel policy, music, streams, mix buses, overlap/pocket ducking,
 * growable voices). Voice I/O goes through the miniaudio mixer in
 * snd_render_omaha.cpp; there is no OpenAL dependency. The core selects this
 * host through s_sndHostOmaha (snd_host.h); the stock OpenAL host stays as is.
 */

#include "../snd_local.h"
#include "snd_omaha.h"
#include "../client.h"
#include "../../server/server.h"
#include "../snd_codec.h"
#include "snd_stream_async.h"
#include "snd_category.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#define MAX_MUSIC_SONGS 16

// Host-internal; the public sound API reaches these through s_sndHostOmaha.
static void S_OMAHA_FadeSound(float fTime);
static float S_OMAHA_GetBaseVolume(void);
static qboolean S_OMAHA_NeedFullRestart(void);
static void S_OMAHA_PrintInfo(void);
static void S_OMAHA_DumpInfo(void);
static void S_OMAHA_MUSIC_Pause(void);
static void S_OMAHA_MUSIC_Unpause(void);
static void S_OMAHA_PauseSound(void);
static void S_OMAHA_UnpauseSound(void);
static void S_OMAHA_callbackServer(int entnum, int channel_number, const char *name);
static void S_OMAHA_StopLoopingSound(omaha_loop_sound_t *pLoopSound);
static void S_OMAHA_StopLoopingSounds(void);
static void S_OMAHA_AddLoopSounds(const vec3_t vTempAxis);
static qboolean S_OMAHA_IsSoundPlaying(int channel_number, const char *sfxName);
static void S_OMAHA_SaveData(soundsystemsavegame_t *pSave);
static void S_OMAHA_ReLoad(soundsystemsavegame_t *pSave);
static void S_OMAHA_LoadData(soundsystemsavegame_t *pSave);
static qboolean S_OMAHA_MUSIC_LoadSoundtrackFile(const char *filename);
static qboolean S_OMAHA_MUSIC_SongValid(const char *mood);
static qboolean S_OMAHA_MUSIC_Loaded(void);
static void S_OMAHA_Music_Update(void);
static void S_OMAHA_MUSIC_SongEnded(void);
static void S_OMAHA_MUSIC_NewSoundtrack(const char *name);
static void S_OMAHA_MUSIC_UpdateMood(int current, int fallback);
static void S_OMAHA_MUSIC_UpdateVolume(float volume, float fade_time);
static void S_OMAHA_MUSIC_StopAllSongs(void);
static void S_OMAHA_MUSIC_FreeAllSongs(void);
static qboolean S_OMAHA_MUSIC_Playing(void);
static int S_OMAHA_MUSIC_FindSong(const char *name);
static void S_OMAHA_loadsoundtrack(void);
static const char *S_OMAHA_CurrentSoundtrack(void);
static void S_OMAHA_PlaySong(void);
static int S_OMAHA_MUSIC_CurrentSongChannel(void);
static void S_OMAHA_MUSIC_StopChannel(int channel_number);
static qboolean S_OMAHA_MUSIC_PlaySong(const char *alias);
static void S_OMAHA_MUSIC_UpdateMusicVolumes(void);
static void S_OMAHA_MUSIC_CheckForStoppedSongs(void);
static void S_OMAHA_TriggeredMusic_SetupHandle(const char *pszName, int iLoopCount, int iOffset, qboolean autostart);
static void S_OMAHA_TriggeredMusic_Start(void);
static void S_OMAHA_TriggeredMusic_StartLoop(void);
static void S_OMAHA_TriggeredMusic_Stop(void);
static void S_OMAHA_TriggeredMusic_Volume(void);
static void S_OMAHA_TriggeredMusic_Pause(void);
static void S_OMAHA_TriggeredMusic_Unpause(void);
static void S_OMAHA_TriggeredMusic_PlayIntroMusic(void);
static void S_OMAHA_StopMovieAudio(void);
static void S_OMAHA_SetupMovieAudio(const char *pszMovieName);
static int S_OMAHA_CurrentMoviePosition(void);

static int   s_iReverbType         = 0;
static float s_fReverbLevel        = 0;
static bool  s_bReverbChanged      = false;
static bool  s_bFading             = false;
static float s_fFadeVolume         = 1.f;
static float s_fVelocityScale      = 0.005f;
/* Added in Omaha: omaha-mixer obstruction gate and cached listener origin. */
static cvar_t *s_obstruction;
/* Added in Omaha: gate for OM_MIX_* category scales (overlap/pocket stay on). */
static cvar_t *s_mix;
/*
 * Added in Omaha: force-load short combat VO (pain/death) so they can take the
 * 3D / HRTF path. Retail marks them streamed dialog; on load failure keep 2D.
 */
static cvar_t *s_pain_hrtf;
static vec3_t  s_vObstructionListener;
/* Changed in Omaha: mix / overlap / pocket knobs are compile-time (no console cvars). */
static constexpr float OM_MIX_FIRE_LOCAL   = 0.6f;
static constexpr float OM_MIX_FIRE_OTHER   = 0.65f;
static constexpr float OM_MIX_FOLEY        = 0.7f;
static constexpr float OM_MIX_IMPACTS      = 0.6f;
static constexpr float OM_MIX_EXPLOSIONS   = 0.75f;
static constexpr float OM_MIX_FOOTSTEPS    = 1.35f;
static constexpr float OM_MIX_BODYFALLS    = 1.f;
static constexpr float OM_MIX_VOICE        = 0.9f;
static constexpr float OM_MIX_VEHICLES     = 0.8f;
static constexpr float OM_MIX_WORLD        = 0.8f;
static constexpr int   OM_OVERLAP_ENABLE   = 1;
static constexpr float OM_OVERLAP_TAU      = 70.f;
static constexpr float OM_OVERLAP_POWER    = 0.7f;
static constexpr float OM_OVERLAP_SPREAD   = 2.f;
static constexpr float OM_OVERLAP_FLOOR    = 0.1f;
static constexpr int   OM_POCKET_ENABLE    = 1;
static constexpr float OM_POCKET_POWER     = 0.7f;
static constexpr float OM_POCKET_MIN       = 0.35f;
static constexpr float OM_POCKET_FIRE_R    = 512.f;
static constexpr float OM_POCKET_FIRE_TAU  = 80.f;
static constexpr float OM_POCKET_EXPL_R    = 512.f;
static constexpr float OM_POCKET_EXPL_TAU  = 120.f;
/* Changed in Omaha: obstruction trace interval fixed (was s_obstruction_cal_time). */
static constexpr int OM_OBSTRUCT_CAL_MS = 100;

static constexpr float s_fVolumeGain = 1.f; // = 84.f;
/* Miles sample volume was 0–127 after ×84; full-knob content ceiling = 127/84. */
static constexpr float OM_CONTENT_VOLUME_MAX = 127.f / 84.f;

static cvar_t *s_milesdriver;
static cvar_t *s_openaldevice;
static cvar_t *s_reverb;
static cvar_t *s_show_cpu;
static cvar_t *s_show_num_active_sounds;
static cvar_t *s_speaker_type;
// Added in Omaha: backend-neutral device name; s_openaldevice remains an alias
static cvar_t *s_snddevice;

static const char *S_OMAHA_DeviceName(void)
{
    if (s_snddevice && s_snddevice->string[0]) {
        return s_snddevice->string;
    }
    return s_openaldevice ? s_openaldevice->string : "";
}

static vec3_t       vec_zero          = {0, 0, 0};
static bool         al_initialized    = false;
static bool         al_use_reverb     = false;
static float        al_current_volume = 0;
static unsigned int al_frequency      = 22050;

static qboolean music_active            = qfalse;
static int      music_current_mood      = 0;
static int      music_fallback_mood     = 0;
static float    old_music_volume        = 1.f;
static float    music_volume            = 1.f;
static float    new_music_volume        = 1.f;
static float    music_volume_fade_time  = 0;
static long int music_volume_start_time = 0;
static int      music_volume_direction  = 0;
static int      music_volume_changed    = 0;
static int      music_loaded            = 0;
static int      music_numsongs          = 0;
static int      music_currentsong       = 0;

static song_t           music_songs[MAX_MUSIC_SONGS];
static omaha_internal_t omaha;
static float            s_fFadeStartTime;
static float            s_fFadeStopTime;
static char             current_soundtrack[128];

static void S_OMAHA_PlayMP3();
static void S_OMAHA_StopMP3();
static void S_OMAHA_Pitch();
static int
S_OMAHA_SpatializeStereoSound(const vec3_t listener_origin, const vec3_t listener_left, const vec3_t origin);
static void S_OMAHA_ApplyListenerGain(qboolean bForce);
static void S_OMAHA_reverb(int iChannel, int iReverbType, float fReverbLevel);
static void S_OMAHA_UpdateObstruction(
    omaha_channel *pChannel, const vec3_t vListener, const vec3_t vOrigin, bool bWorld, bool bForce
);
static int               S_OMAHA_SpatialRoute(const omaha_channel *pChannel, bool bWorld);
static sndSampleFormat_t S_SndFmtFromWidthChannels(float width, int channels);

/*
==============
S_OMAHA_NukeChannel
==============
*/
static void S_OMAHA_NukeChannel(omaha_channel *channel)
{
    if (!channel) {
        return;
    }

    if (g_sndRenderer && channel->voiceId != SND_VOICE_NONE) {
        g_sndRenderer->DestroyVoice(channel->voiceId);
        channel->voiceId = SND_VOICE_NONE;
    }

    if (channel->bufferdata) {
        delete[] channel->bufferdata;
    }
}

/*
==============
S_OMAHA_NukeContext
==============
*/
static void S_OMAHA_NukeContext()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS; ++i) {
        S_OMAHA_NukeChannel(omaha.channel[i]);
    }

    /* sfx->buffer holds a renderer buffer index (+1); the renderer frees its buffers. */
    for (i = 0; i < s_numSfx; i++) {
        s_knownSfx[i].buffer = 0;
    }
    omaha.movieSFX.buffer = 0;
    S_Renderer_Shutdown();
}

/*
==============
S_OMAHA_ApplyFrequency
==============
*/
static void S_OMAHA_ApplyFrequency(void)
{
    int khz = 44;

    if (s_khz) {
        khz = s_khz->integer;
    }
    switch (khz) {
    case 11:
        al_frequency = 11025;
        break;
    default:
    case 22:
        al_frequency = 22050;
        break;
    case 44:
        al_frequency = 44100;
        break;
    }
}

/*
==============
S_OMAHA_InitChannel
==============
*/
static bool S_OMAHA_InitChannel(int idx, omaha_channel *chan)
{
    if (idx >= 0 && idx < MAX_SOUNDSYSTEM_CHANNELS) {
        omaha.channel[idx] = chan;
    }
    VectorClear(chan->vOrigin);
    chan->fVolume         = 1.0;
    chan->fNewPitchMult   = 0.0;
    chan->fMinDist        = 0.0;
    chan->fMaxDist        = 0.0;
    chan->pSfx            = 0;
    chan->iEntNum         = 0;
    chan->iEntChannel     = 0;
    chan->iBaseRate       = 0;
    chan->iStartTime      = 0;
    chan->iTime           = 0;
    chan->iEndTime        = 0;
    chan->iFlags          = 0;
    chan->iPausedOffset   = 0;
    chan->bufferdata      = 0;
    chan->fading          = FADE_NONE;
    chan->fade_time       = 0;
    chan->fade_start_time = 0;
    chan->song_number     = 0;
    chan->voiceId         = SND_VOICE_NONE;
    chan->bufferId        = SND_BUFFER_NONE;
    chan->voiceKind       = 0;
    chan->loopBlockStart  = 0;
    chan->loopBlockEnd    = -1;
    chan->iNextObstructionCheck = 0;
    chan->fObstruction          = 0;
    chan->iCategory             = (int)SND_CAT_NONE;
    chan->iSpatialRoute         = -1;

    chan->voiceId = g_sndRenderer->CreateVoice();
    if (chan->voiceId == SND_VOICE_NONE) {
        Com_Printf("Sound: CreateVoice failed for channel %i\n", idx);
        return false;
    }
    g_sndRenderer->SetParami(chan->voiceId, SND_PARAM_SOURCE_RELATIVE, 1);
    return true;
}

/*
==============
S_OMAHA_Init
==============
*/
qboolean S_OMAHA_Init()
{
    int               i;
    sndDeviceConfig_t cfg;

    if (al_initialized) {
        Com_DPrintf("S_OMAHA_Init(): Called when sound is already initialized!\n");
        return qtrue;
    }

    s_milesdriver            = Cvar_Get("s_milesdriver", "auto", CVAR_SOUND_LATCH | CVAR_ARCHIVE);
    s_openaldevice           = Cvar_Get("s_openaldevice", "", CVAR_SOUND_LATCH);
    s_snddevice              = Cvar_Get("s_device", "", CVAR_SOUND_LATCH | CVAR_ARCHIVE);
    s_reverb                 = Cvar_Get("s_reverb", "0", CVAR_SOUND_LATCH | CVAR_ARCHIVE);
    s_show_cpu               = Cvar_Get("s_show_cpu", "0", 0);
    s_show_num_active_sounds = Cvar_Get("s_show_num_active_sounds", "0", 0);
    s_show_sounds            = Cvar_Get("s_show_sounds", "0", 0);
    /* Changed in Omaha: latch — surround layouts switch to the OpenAL host on restart. */
    s_speaker_type           = Cvar_Get("s_speaker_type", "0", CVAR_ARCHIVE | CVAR_SOUND_LATCH);
    /* Added in Omaha: world-trace obstruction on the omaha mixer (1 = on). */
    s_obstruction = Cvar_Get("s_obstruction", "1", CVAR_ARCHIVE);
    /* Added in Omaha: 1 = OM_MIX_* bus gains; 0 = unity (overlap/pocket unchanged). */
    s_mix         = Cvar_Get("s_mix", "1", CVAR_ARCHIVE);
    /* Added in Omaha: 1 = force-load pain/death VO for 3D/HRTF (0 = retail streamed 2D). */
    s_pain_hrtf   = Cvar_Get("s_pain_hrtf", "1", CVAR_ARCHIVE);
    /* Added in Omaha: SOFA HRTF set for omaha headphones (s_speaker_type 1). */
    Cvar_Get("s_hrtf_file", SND_HRTF_DEFAULT_FILE, CVAR_ARCHIVE | CVAR_SOUND_LATCH);
    /* Added in Omaha: developer A/B — force live SOFA DIRECT queries (skip bake runtime). */
    Cvar_Get("s_hrtf_live_query", "0", CVAR_SOUND_LATCH);

    if (!Cvar_Get("s_initsound", "1", 0)->integer) {
        Com_Printf("Sound: s_initsound set to zero...disabling audio.\n");
        return true;
    }

    /*
     * The omaha mixer renders stereo and headphones (its own HRTF) only.
     * 5.1 and quad need Soft's surround output modes — keep the error and fall
     * back to headphones HRTF instead of declining the omaha host.
     */
    if (s_speaker_type->integer >= 2) {
        Com_Printf("Sound: speaker_type %i (surround) is not supported by omaha.\n", s_speaker_type->integer);
        Cvar_Set("s_speaker_type", "1");
        Com_Printf("Sound: omaha does not support surround; using Headphones (HRTF).\n");
    }

    memset(&cfg, 0, sizeof(cfg));
    S_OMAHA_ApplyFrequency();
    cfg.sampleRate  = (int)al_frequency;
    cfg.deviceName  = S_OMAHA_DeviceName();
    cfg.speakerType = s_speaker_type->integer;
    cfg.wantCapture = qfalse;

    if (!S_Renderer_Init(&cfg)) {
        return qfalse;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS; i++) {
        omaha.channel[i] = NULL;
    }

    /* Added in Omaha: growable loop list + overflow voice pools. */
    omaha.overflow_3D                 = NULL;
    omaha.overflow_3D_count           = 0;
    omaha.overflow_3D_capacity        = 0;
    omaha.overflow_2D                 = NULL;
    omaha.overflow_2D_count           = 0;
    omaha.overflow_2D_capacity        = 0;
    omaha.overflow_2D_stream          = NULL;
    omaha.overflow_2D_stream_count    = 0;
    omaha.overflow_2D_stream_capacity = 0;
    omaha.channel_extra               = NULL;
    omaha.channel_extra_count         = 0;
    omaha.channel_extra_capacity      = 0;
    omaha.position_channel_count      = MAX_SOUNDSYSTEM_POSITION_CHANNELS;
    omaha.loop_sounds_capacity        = MAX_SOUNDSYSTEM_LOOP_SOUNDS;
    omaha.loop_sounds_count           = MAX_SOUNDSYSTEM_LOOP_SOUNDS;
    omaha.loop_sounds                 = (omaha_loop_sound_t *)Z_Malloc(omaha.loop_sounds_capacity * sizeof(*omaha.loop_sounds));
    memset(omaha.loop_sounds, 0, omaha.loop_sounds_capacity * sizeof(*omaha.loop_sounds));

    s_speaker_type->modified = false;

    al_use_reverb = false;
    if (s_reverb->integer) {
        STUB_DESC("reenable reverb support later.");

        if (al_use_reverb) {
            S_OMAHA_SetReverb(s_iReverbType, s_fReverbLevel);
        } else {
            Com_Printf("Sound: No reverb support. Reverb is disabled.\n");
        }
    }
    s_reverb->modified = false;

    /* Changed in Omaha: omaha master is post-limiter (see S_OMAHA_ApplyListenerGain). */
    S_OMAHA_ApplyListenerGain(qtrue);

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_3D; i++) {
        if (!S_OMAHA_InitChannel(i, &omaha.chan_3D[i])) {
            return false;
        }
        omaha.chan_3D[i].voiceKind = 0;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_2D; i++) {
        if (!S_OMAHA_InitChannel(i + MAX_SOUNDSYSTEM_CHANNELS_3D, &omaha.chan_2D[i])) {
            return false;
        }
        omaha.chan_2D[i].voiceKind = 1;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM; i++) {
        if (!S_OMAHA_InitChannel(
                i + MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D, &omaha.chan_2D_stream[i]
            )) {
            return false;
        }
        omaha.chan_2D_stream[i].voiceKind = 2;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        if (!S_OMAHA_InitChannel(
                i + MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM,
                &omaha.chan_song[i]
            )) {
            return false;
        }
    }

    if (!S_OMAHA_InitChannel(SOUNDSYSTEM_CHANNEL_MP3_ID, &omaha.chan_mp3)) {
        return false;
    }

    if (!S_OMAHA_InitChannel(SOUNDSYSTEM_CHANNEL_TRIGGER_MUSIC_ID, &omaha.chan_trig_music)) {
        return false;
    }

    if (!S_OMAHA_InitChannel(SOUNDSYSTEM_CHANNEL_MOVIE_ID, &omaha.chan_movie)) {
        return false;
    }

    //
    // Disable sound virtualization on special channels
    //  (triggered music, ambient sounds...)
    //
    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        omaha.chan_song[i].set_no_virtualization();
    }
    omaha.chan_mp3.set_no_virtualization();
    omaha.chan_trig_music.set_no_virtualization();
    omaha.chan_movie.set_no_virtualization();

    Cmd_AddCommand("playmp3", S_OMAHA_PlayMP3);
    Cmd_AddCommand("stopmp3", S_OMAHA_StopMP3);
    Cmd_AddCommand("loadsoundtrack", S_OMAHA_loadsoundtrack);
    Cmd_AddCommand("playsong", S_OMAHA_PlaySong);
    Cmd_AddCommand("pitch", S_OMAHA_Pitch);
    Cmd_AddCommand("tmstart", S_OMAHA_TriggeredMusic_Start);
    Cmd_AddCommand("tmstartloop", S_OMAHA_TriggeredMusic_StartLoop);
    Cmd_AddCommand("tmstop", S_OMAHA_TriggeredMusic_Stop);
    // Added in 2.0
    Cmd_AddCommand("tmvolume", S_OMAHA_TriggeredMusic_Volume);

    S_OMAHA_ClearLoopingSounds();
    load_sfx_info();
    al_initialized = true;

    // Added in OPM
    S_CodecInit();

    return true;
}

/*
==============
S_OMAHA_Shutdown
==============
*/
void S_OMAHA_Shutdown()
{
    int i;

    if (!al_initialized) {
        Com_DPrintf("S_OMAHA_Shutdown(): Called when sound is NOT initialized!\n");
        return;
    }

    S_OMAHA_StopAllSounds(true);

    Cmd_RemoveCommand("playmp3");
    Cmd_RemoveCommand("stopmp3");
    Cmd_RemoveCommand("loadsoundtrack");
    Cmd_RemoveCommand("playsong");
    Cmd_RemoveCommand("pitch");
    Cmd_RemoveCommand("tmstart");
    Cmd_RemoveCommand("tmstartloop");
    Cmd_RemoveCommand("tmstop");
    // Added in 2.0
    Cmd_RemoveCommand("tmvolume");

    /* Added in Omaha: free growable overflow voices (Z_Free rejects NULL). */
    for (i = 0; i < omaha.channel_extra_count; i++) {
        S_OMAHA_NukeChannel(omaha.channel_extra[i]);
        delete omaha.channel_extra[i];
    }
    if (omaha.channel_extra) {
        Z_Free(omaha.channel_extra);
        omaha.channel_extra = NULL;
    }
    omaha.channel_extra_count    = 0;
    omaha.channel_extra_capacity = 0;
    if (omaha.overflow_3D) {
        Z_Free(omaha.overflow_3D);
        omaha.overflow_3D = NULL;
    }
    if (omaha.overflow_2D) {
        Z_Free(omaha.overflow_2D);
        omaha.overflow_2D = NULL;
    }
    if (omaha.overflow_2D_stream) {
        Z_Free(omaha.overflow_2D_stream);
        omaha.overflow_2D_stream = NULL;
    }
    omaha.overflow_3D_count = omaha.overflow_3D_capacity = 0;
    omaha.overflow_2D_count = omaha.overflow_2D_capacity = 0;
    omaha.overflow_2D_stream_count = omaha.overflow_2D_stream_capacity = 0;
    if (omaha.loop_sounds) {
        Z_Free(omaha.loop_sounds);
        omaha.loop_sounds = NULL;
    }
    omaha.loop_sounds_count = omaha.loop_sounds_capacity = 0;

    S_OMAHA_NukeContext();
    al_initialized = false;

    S_AsyncStream_Shutdown(); // Added in Omaha
}

/*
==============
S_OMAHA_FadeSound
==============
*/
void S_OMAHA_FadeSound(float fTime)
{
    Com_Printf("Called FadeSound with: %f\n", fTime);

    if (fTime > 0) {
        s_bFading        = true;
        s_fFadeStartTime = cls.realtime;
        s_fFadeVolume    = 1;
        s_fFadeStopTime  = cls.realtime + fTime;
    } else {
        s_fFadeVolume = 1;
        s_bFading     = false;
        // Fixed in OPM
        //  Make sure to restore the music volume
        //  in case the map restarts immediately after the intermission
        music_volume_changed = true;
    }
}

/*
==============
S_OMAHA_GetBaseVolume

Changed in Omaha: on the omaha mixer, s_volume and the fade are applied once on
the stereo master after the limiter (S_OMAHA_ApplyListenerGain), so voices
carry content gain only.
==============
*/
float S_OMAHA_GetBaseVolume()
{
    return 1.f;
}

/*
==============
S_OMAHA_ApplyListenerGain

Added in Omaha: Soft keeps s_volume squared on the listener (s_volume and fade
also ride on every source). Omaha applies the whole product s_volume^3 * fade as
a post-limiter master, so loudness per s_volume matches Soft but raising volume
no longer drives the limiter harder.
==============
*/
static void S_OMAHA_ApplyListenerGain(qboolean bForce)
{
    static float fLastOmahaMaster = -1.f;
    float        fGain;

    fGain = s_volume->value * Square(s_volume->value) * s_fFadeVolume;
    if (!bForce && fGain == fLastOmahaMaster) {
        return;
    }
    fLastOmahaMaster  = fGain;
    al_current_volume = fGain;
    g_sndRenderer->SetListenerGain(fGain);
}

/*
==============
S_OMAHA_IsLocalEntity

Added in Omaha: the listener's own entity or one of its child ents (use_listener).
==============
*/
static qboolean S_OMAHA_IsLocalEntity(int iEntNum)
{
    int localNum;

    if (iEntNum == ENTITYNUM_NONE || iEntNum < 0 || iEntNum >= MAX_GENTITIES) {
        return qfalse;
    }

    localNum = -1;
    if (cl.snap.valid) {
        localNum = cl.snap.ps.clientNum;
    } else if (s_iListenerNumber >= 0) {
        localNum = s_iListenerNumber;
    }
    if (localNum >= 0 && iEntNum == localNum) {
        return qtrue;
    }
    return s_entity[iEntNum].use_listener ? qtrue : qfalse;
}

/*
==============
S_OMAHA_ClampContentVolume

Added in Omaha: content volume from alias/script/pk3 stays at or below the
Miles full-knob ceiling (OM_CONTENT_VOLUME_MAX = 127/84 ≈ 1.51). OM_MIX_* / overlap / pocket
may still raise gain afterward. Negative means default (same as prior StartSound
paths). Master (s_volume) is applied post-limiter and does not widen this cap.
==============
*/
static float S_OMAHA_ClampContentVolume(float fVolume)
{
    if (fVolume < 0.f) {
        return 1.f;
    }
    if (fVolume > OM_CONTENT_VOLUME_MAX) {
        return OM_CONTENT_VOLUME_MAX;
    }
    return fVolume;
}

/*
==============
S_OMAHA_CategoryGainScale

Added in Omaha: mix-bus multipliers (hardcoded OM_MIX_*, range 0–2). FIRE splits
local vs other (weapon child ents use use_listener). NONE / unknown → 1.
s_mix 0 forces unity so stock relative levels return without disabling overlap/pocket.
==============
*/
static float S_OMAHA_CategoryGainScale(sndCategory_t cat, int iEntNum)
{
    float g;

    if (!s_mix || !s_mix->integer) {
        return 1.f;
    }

    switch (cat) {
    case SND_CAT_FIRE:
        g = S_OMAHA_IsLocalEntity(iEntNum) ? OM_MIX_FIRE_LOCAL : OM_MIX_FIRE_OTHER;
        break;
    case SND_CAT_FOLEY:
        g = OM_MIX_FOLEY;
        break;
    case SND_CAT_IMPACTS:
        g = OM_MIX_IMPACTS;
        break;
    case SND_CAT_EXPLOSIONS:
        g = OM_MIX_EXPLOSIONS;
        break;
    case SND_CAT_FOOTSTEPS:
        g = OM_MIX_FOOTSTEPS;
        break;
    case SND_CAT_BODYFALLS:
        g = OM_MIX_BODYFALLS;
        break;
    case SND_CAT_VOICE:
        g = OM_MIX_VOICE;
        break;
    case SND_CAT_VEHICLES:
        g = OM_MIX_VEHICLES;
        break;
    case SND_CAT_WORLD:
        g = OM_MIX_WORLD;
        break;
    case SND_CAT_AUTO:
    case SND_CAT_NONE:
    default:
        return 1.f;
    }

    if (g < 0.f) {
        return 0.f;
    }
    if (g > 2.f) {
        return 2.f;
    }
    return g;
}

/*
==============
S_OMAHA_ClassifySfx

Added in Omaha: category of a sample from the global aliases that play it.
cgame resolves aliases to files before S_StartSound, so the host recovers the
alias names here instead of widening the stock cgame/client sound API. Files
shared by aliases of different categories (puddle step vs grenade bounce,
gear foley vs step) are decided by the path, independent of alias order.
==============
*/
static sndCategory_t S_OMAHA_ClassifySfx(const char *sfxName)
{
    AliasList_t     *list;
    AliasListNode_t *node;
    sndCategory_t    cat   = SND_CAT_AUTO;
    qboolean         found = qfalse;

    list = Alias_GetGlobalList();
    if (list && sfxName && sfxName[0]) {
        for (node = list->data_list; node; node = node->next) {
            sndCategory_t aliasCat;

            if (Q_stricmp(node->real_name, sfxName)) {
                continue;
            }
            aliasCat = S_SoundCategory_ClassifyName(node->alias_name, sfxName);
            if (!found) {
                cat   = aliasCat;
                found = qtrue;
            } else if (aliasCat != cat) {
                const sndCategory_t pathCat = S_SoundCategory_ClassifyName(NULL, sfxName);
                return pathCat != SND_CAT_AUTO ? pathCat : cat;
            }
        }
    }

    if (!found) {
        return S_SoundCategory_ClassifyName(NULL, sfxName);
    }
    return cat;
}

/*
==============
S_OMAHA_ResolveCategory

Added in Omaha: alias / path category cached per sfx slot (keyed by a name hash
so reused slots reclassify); CHAN_* only decides samples neither one matches.
==============
*/
static struct {
    int           hash;
    sndCategory_t cat;
} s_sfxCategory[MAX_SFX];

static sndCategory_t S_OMAHA_ResolveCategory(const sfx_t *pSfx, int iEntChannel)
{
    const int     idx  = (int)(pSfx - s_knownSfx);
    const int     hash = Com_HashKey((char *)pSfx->name, MAX_RES_NAME) + 1;
    sndCategory_t cat;

    if (idx < 0 || idx >= MAX_SFX) {
        cat = S_OMAHA_ClassifySfx(pSfx->name);
        return cat != SND_CAT_AUTO ? cat : S_SoundCategory_FromChannel(iEntChannel);
    }

    if (s_sfxCategory[idx].hash != hash) {
        s_sfxCategory[idx].hash = hash;
        s_sfxCategory[idx].cat  = S_OMAHA_ClassifySfx(pSfx->name);
    }

    cat = s_sfxCategory[idx].cat;
    if (cat == SND_CAT_AUTO) {
        return S_SoundCategory_FromChannel(iEntChannel);
    }
    return cat;
}

/*
==============
S_OMAHA_NeedFullRestart
==============
*/
qboolean S_OMAHA_NeedFullRestart()
{
    return Cvar_Get("s_initsound", "1", 0)->integer != s_bLastInitSound;
}

/*
==============
S_OMAHA_PrintInfo
==============
*/
void S_OMAHA_PrintInfo()
{
    const char *dev;

    Com_Printf("----- Sound Info -----\n");

    if (s_bSoundStarted) {
        dev = S_OMAHA_DeviceName();
        if (!dev || !*dev) {
            dev = "{default}";
        }
        Com_Printf("device - %s\n", dev);
        if (al_use_reverb) {
            Com_Printf("reverb - ON\n");
        } else {
            Com_Printf("reverb - OFF\n");
        }
        Com_Printf("samplebits - 16\n");
        Com_Printf("speed - %d\n", al_frequency);
        if (s_loadas8bit->integer) {
            Com_Printf("Can NOT force all sounds to 8 bit in omaha.\n");
        }
    } else {
        Com_Printf("sound system not started\n");
    }

    Com_Printf("----------------------\n");
}

/*
==============
S_DumpStatus
==============
*/
static void S_DumpStatus(const char *pszChanName, int iChanNum, omaha_channel *channel)
{
    sfx_t *sfx;

    sfx = channel->pSfx;

    /* Changed in Omaha: query state through the channel (renderer-aware). */
    if (channel->is_playing() || channel->is_paused()) {
        const char *pszMode = channel->is_playing() ? "playing" : "paused";

        if (sfx) {
            if (sfx != (sfx_t *)-16 && sfx->name[0]) {
                Com_Printf("%s channel %i - %s sfx %s\n", pszChanName, iChanNum, pszMode, sfx->name);
            } else {
                Com_Printf("%s channel %i - %s with nameless sfx\n", pszChanName, iChanNum, pszMode);
            }
        } else {
            Com_Printf("%s channel %i - %s with NULL sfx\n", pszChanName, iChanNum, pszMode);
        }
    }
}

/*
==============
S_OMAHA_DumpInfo
==============
*/
void S_OMAHA_DumpInfo()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_3D; i++) {
        S_DumpStatus("3D", i, omaha.channel[i]);
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_2D; i++) {
        S_DumpStatus("2D", i, omaha.channel[MAX_SOUNDSYSTEM_CHANNELS_3D + i]);
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM; i++) {
        S_DumpStatus("2D stream", i, omaha.channel[MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + i]);
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_MISC_CHANNELS; i++) {
        S_DumpStatus(
            "Misc",
            i,
            omaha.channel
                [MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM + i]
        );
    }
}

/*
==============
S_OMAHA_Pitch
==============
*/
static void S_OMAHA_Pitch()
{
    Com_Printf("S_OMAHA_Pitch() needs to be implemented!\n");
}

/*
==============
S_OMAHA_LoadMP3
==============
*/
static bool S_OMAHA_LoadMP3(const char *_path, omaha_channel *chan)
{
    char          path[MAX_QPATH];
    snd_info_t    info;
    void         *data;
    sndBufferId_t bid;
    qboolean      ok;

    chan->stop();

    Q_strncpyz(path, _path, sizeof(path));
    path[MAX_QPATH - 1] = 0;

    FS_CorrectCase(path);

    /* Changed in Omaha: decode once and upload into a renderer buffer. */
    if (chan->voiceId == SND_VOICE_NONE) {
        return false;
    }

    g_sndRenderer->ClearBuffer(chan->voiceId);
    if (chan->bufferId != SND_BUFFER_NONE) {
        g_sndRenderer->DestroyBuffer(chan->bufferId);
        chan->bufferId = SND_BUFFER_NONE;
    }

    data = S_CodecLoad(path, &info);
    if (!data) {
        return false;
    }

    bid = g_sndRenderer->CreateBufferFromSfx(NULL);
    if (bid == SND_BUFFER_NONE) {
        Hunk_FreeTempMemory(data);
        return false;
    }
    if (info.size == 0) {
        byte dummyData[2] = {0};
        ok                = g_sndRenderer->UploadBuffer(bid, SND_FMT_MONO16, dummyData, 2, 22050, 0);
    } else {
        ok = g_sndRenderer->UploadBuffer(
            bid, S_SndFmtFromWidthChannels(info.width, info.channels), data, info.size, info.rate, info.dataalign
        );
    }
    Hunk_FreeTempMemory(data);
    if (!ok) {
        g_sndRenderer->DestroyBuffer(bid);
        return false;
    }

    chan->bufferId = bid;
    g_sndRenderer->SetBuffer(chan->voiceId, bid);
    chan->set_no_3d();
    return true;
}

/*
==============
S_OMAHA_PlayMP3
==============
*/
static void S_OMAHA_PlayMP3()
{
    const char *path;

    if (Cmd_Argc() != 2) {
        Com_Printf("playmp3 <mp3 file>\n");
        return;
    }

    path = Cmd_Argv(1);
    if (!S_OMAHA_LoadMP3(path, &omaha.chan_mp3)) {
        Com_Printf("Failed to play mp3 - %s\n", path);
        return;
    }

    omaha.chan_mp3.play();
    Com_Printf("Playing mp3 - %s\n", path);
}

/*
==============
S_OMAHA_StopMP3
==============
*/
static void S_OMAHA_StopMP3()
{
    S_OMAHA_NukeChannel(&omaha.chan_mp3);
}

/*
==============
S_OMAHA_MUSIC_Pause
==============
*/
void S_OMAHA_MUSIC_Pause()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        omaha.chan_song[i].pause();
    }
}

/*
==============
S_OMAHA_MUSIC_Unpause
==============
*/
void S_OMAHA_MUSIC_Unpause()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        if (omaha.chan_song[i].is_paused()) {
            omaha.chan_song[i].play();
        }
    }
}

static omaha_channel *S_OMAHA_GetPosChannel(int index);

/*
==============
S_OMAHA_PauseSound
==============
*/
void S_OMAHA_PauseSound()
{
    int i;

    if (!s_bSoundStarted) {
        return;
    }

    s_bSoundPaused = true;

    /* Changed in Omaha: include overflow voices. */
    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS + omaha.channel_extra_count; i++) {
        omaha_channel *pChannel = S_OMAHA_GetPosChannel(i);
        if (!pChannel) {
            continue;
        }

        if (pChannel->iEntChannel == CHAN_MENU) {
            continue;
        }

        if (!pChannel->is_playing()) {
            continue;
        }

        pChannel->pause();
    }

    if (omaha.chan_mp3.is_playing()) {
        omaha.chan_mp3.pause();
    }

    S_OMAHA_MUSIC_Pause();
    S_OMAHA_TriggeredMusic_Pause();
}

/*
==============
S_OMAHA_UnpauseSound
==============
*/
void S_OMAHA_UnpauseSound()
{
    int i;

    if (!s_bSoundStarted) {
        return;
    }

    /* Changed in Omaha: include overflow voices. */
    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS + omaha.channel_extra_count; i++) {
        omaha_channel *pChannel = S_OMAHA_GetPosChannel(i);
        if (!pChannel) {
            continue;
        }

        if (pChannel->is_paused() || (pChannel->iFlags & CHANNEL_FLAG_PLAY_DEFERRED)) {
            pChannel->iFlags &= ~CHANNEL_FLAG_PLAY_DEFERRED;
            pChannel->play();
        }
    }

    if (omaha.chan_mp3.is_paused()) {
        omaha.chan_mp3.play();
    }

    S_OMAHA_MUSIC_Unpause();
    S_OMAHA_TriggeredMusic_Unpause();

    s_bSoundPaused = false;
}

/*
==============
S_OMAHA_ShouldStart
==============
*/
static qboolean S_OMAHA_ShouldStart(const vec3_t vOrigin, float fMinDist, float fMaxDist)
{
    vec3_t vDir;
    vec3_t vListenerOrigin;
    vec3_t alvec;

    if (!al_initialized) {
        return false;
    }

    (void)fMinDist;
    g_sndRenderer->GetListenerPosition(alvec);

    VectorCopy(alvec, vListenerOrigin);
    VectorSubtract(vOrigin, vListenerOrigin, vDir);

    return Square(fMaxDist) > VectorLengthSquared(vDir);
}

/*
==============
S_OMAHA_GetPosChannel
Added in Omaha: baseline + overflow position channels.
==============
*/
static omaha_channel *S_OMAHA_GetPosChannel(int index)
{
    if (index < 0) {
        return NULL;
    }
    if (index < MAX_SOUNDSYSTEM_POSITION_CHANNELS) {
        return omaha.channel[index];
    }
    index -= MAX_SOUNDSYSTEM_POSITION_CHANNELS;
    if (index < omaha.channel_extra_count) {
        return omaha.channel_extra[index];
    }
    return NULL;
}

/*
==============
S_OMAHA_LevelAtListener
Added in Omaha: channel gain with clamped inverse-distance falloff at the listener.
==============
*/
static float S_OMAHA_LevelAtListener(float fVolume, const vec3_t vOrigin, float fMinDist, float *pfDist)
{
    vec3_t vDir;
    float  fDist;

    VectorSubtract(vOrigin, s_vObstructionListener, vDir);
    fDist = VectorLength(vDir);
    if (pfDist) {
        *pfDist = fDist;
    }
    if (fMinDist <= 0.f || fDist <= fMinDist) {
        return fVolume;
    }
    return fVolume * fMinDist / fDist;
}

/*
==============
S_OMAHA_OverlapGain

Added in Omaha: start gain for an entity-less 3D one-shot, reduced by how much
it overlaps same-category entity-less voices already playing (recent, at least
as loud at the listener, same direction). Fixed for the voice's lifetime.
==============
*/
static float S_OMAHA_OverlapGain(sndCategory_t cat, const vec3_t vOrigin, float fVolume, float fMinDist, float *pfOverlap)
{
    vec3_t vNewDir;
    float  fNewDist;
    float  fNewLevel;
    float  fTau;
    float  fPower;
    float  fSpread;
    float  fOverlap;
    int    iMaxAge;
    int    i;

    fOverlap = 0.f;
    if (pfOverlap) {
        *pfOverlap = 0.f;
    }

    fTau    = OM_OVERLAP_TAU;
    fPower  = OM_OVERLAP_POWER;
    fSpread = OM_OVERLAP_SPREAD;
    if (fTau < 1.f) {
        fTau = 1.f;
    }
    if (fPower <= 0.f) {
        return 1.f;
    }
    if (fSpread < 0.f) {
        fSpread = 0.f;
    }
    iMaxAge = (int)(fTau * 4.f);

    fNewLevel = S_OMAHA_LevelAtListener(fVolume, vOrigin, fMinDist, &fNewDist);
    if (fNewLevel <= 0.f) {
        return 1.f;
    }
    VectorSubtract(vOrigin, s_vObstructionListener, vNewDir);

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS + omaha.channel_extra_count; i++) {
        omaha_channel *pChannel = S_OMAHA_GetPosChannel(i);
        vec3_t          vOldDir;
        float           fOldDist;
        float           fOldLevel;
        float           fCos;
        float           fDirSim;
        float           fNear;
        float           fLevelW;
        int             iAge;

        if (!pChannel || pChannel->voiceKind != 0 || pChannel->iCategory != (int)cat) {
            continue;
        }
        if (!(pChannel->iFlags & CHANNEL_FLAG_NO_ENTITY) || (pChannel->iFlags & CHANNEL_FLAG_LOOPING)) {
            continue;
        }
        iAge = cl.serverTime - pChannel->iStartTime;
        if (iAge < 0) {
            iAge = 0;
        }
        if (iAge >= iMaxAge || !pChannel->is_playing()) {
            continue;
        }

        fOldLevel = S_OMAHA_LevelAtListener(pChannel->fVolume, pChannel->vOrigin, pChannel->fMinDist, &fOldDist);
        fLevelW   = fOldLevel / fNewLevel;
        if (fLevelW > 1.f) {
            fLevelW = 1.f;
        }

        VectorSubtract(pChannel->vOrigin, s_vObstructionListener, vOldDir);
        if (fNewDist > 0.001f && fOldDist > 0.001f) {
            fCos = DotProduct(vNewDir, vOldDir) / (fNewDist * fOldDist);
        } else {
            fCos = 1.f;
        }
        fDirSim = powf(Q_clamp_float(0.5f * (1.f + fCos), 0.f, 1.f), fSpread);

        /* Both inside their min distance: direction no longer separates them. */
        fNear = 1.f - Q_max(fNewDist / Q_max(fMinDist, 1.f), fOldDist / Q_max(pChannel->fMinDist, 1.f));
        if (fNear > fDirSim) {
            fDirSim = fNear;
        }

        fOverlap += expf(-(float)iAge / fTau) * fLevelW * fDirSim;
    }

    if (pfOverlap) {
        *pfOverlap = fOverlap;
    }
    return powf(1.f + fOverlap, -fPower);
}

/*
==============
S_OMAHA_PocketGain

Added in Omaha: start gain for other players' fire and for explosions, reduced
by recent same-category voices whose origins sit in the same pocket (origin
distance, smooth edge from R/2 to R), weighted by level at the listener.
FIRE skips local voices and the same entity, so a lone shooter sounds stock.
Clamped to OM_POCKET_MIN: never drops. Fixed for the voice's lifetime.
==============
*/
static float S_OMAHA_PocketGain(
    sndCategory_t cat, int iEntNum, const vec3_t vOrigin, float fVolume, float fMinDist, float *pfOverlap
)
{
    float fNewLevel;
    float fRadius;
    float fTau;
    float fPower;
    float fMin;
    float fOverlap;
    float g;
    int   iMaxAge;
    int   i;

    fOverlap = 0.f;
    if (pfOverlap) {
        *pfOverlap = 0.f;
    }

    if (cat == SND_CAT_FIRE) {
        fRadius = OM_POCKET_FIRE_R;
        fTau    = OM_POCKET_FIRE_TAU;
    } else {
        fRadius = OM_POCKET_EXPL_R;
        fTau    = OM_POCKET_EXPL_TAU;
    }
    fPower = OM_POCKET_POWER;
    fMin   = Q_clamp_float(OM_POCKET_MIN, 0.f, 1.f);
    if (fRadius < 1.f) {
        fRadius = 1.f;
    }
    if (fTau < 1.f) {
        fTau = 1.f;
    }
    if (fPower <= 0.f) {
        return 1.f;
    }
    iMaxAge = (int)(fTau * 4.f);

    fNewLevel = S_OMAHA_LevelAtListener(fVolume, vOrigin, fMinDist, NULL);
    if (fNewLevel <= 0.f) {
        return 1.f;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS + omaha.channel_extra_count; i++) {
        omaha_channel *pChannel = S_OMAHA_GetPosChannel(i);
        vec3_t          vDelta;
        float           fDist;
        float           fPocket;
        float           fLevelW;
        int             iAge;

        if (!pChannel || pChannel->voiceKind != 0 || pChannel->iCategory != (int)cat) {
            continue;
        }
        if (pChannel->iFlags & CHANNEL_FLAG_LOOPING) {
            continue;
        }
        if (cat == SND_CAT_FIRE && !(pChannel->iFlags & CHANNEL_FLAG_NO_ENTITY)) {
            if (pChannel->iEntNum == iEntNum || S_OMAHA_IsLocalEntity(pChannel->iEntNum)) {
                continue;
            }
        }
        iAge = cl.serverTime - pChannel->iStartTime;
        if (iAge < 0) {
            iAge = 0;
        }
        if (iAge >= iMaxAge || !pChannel->is_playing()) {
            continue;
        }

        VectorSubtract(pChannel->vOrigin, vOrigin, vDelta);
        fDist = VectorLength(vDelta);
        if (fDist >= fRadius) {
            continue;
        }
        if (fDist <= fRadius * 0.5f) {
            fPocket = 1.f;
        } else {
            float t = (fRadius - fDist) / (fRadius * 0.5f);
            fPocket = t * t * (3.f - 2.f * t);
        }

        fLevelW = S_OMAHA_LevelAtListener(pChannel->fVolume, pChannel->vOrigin, pChannel->fMinDist, NULL) / fNewLevel;
        if (fLevelW > 1.f) {
            fLevelW = 1.f;
        }

        fOverlap += expf(-(float)iAge / fTau) * fLevelW * fPocket;
    }

    if (pfOverlap) {
        *pfOverlap = fOverlap;
    }
    g = powf(1.f + fOverlap, -fPower);
    return g < fMin ? fMin : g;
}

/*
==============
S_OMAHA_GrowVoicePool
Added in Omaha: allocate another voice when baseline pools are full (no steal).
==============
*/
static int S_OMAHA_GrowVoicePool(int kind)
{
    omaha_channel *chan;
    int            idx;

    if (kind == 2) {
        omaha_channel_two_d_stream *streamChan = new omaha_channel_two_d_stream();
        chan                                    = streamChan;
        if (omaha.overflow_2D_stream_count >= omaha.overflow_2D_stream_capacity) {
            int                              newCap = omaha.overflow_2D_stream_capacity + SOUNDSYSTEM_VOICE_GROW_CHUNK;
            omaha_channel_two_d_stream    **neu =
                (omaha_channel_two_d_stream **)Z_Malloc(newCap * sizeof(*neu));
            if (omaha.overflow_2D_stream) {
                memcpy(neu, omaha.overflow_2D_stream, omaha.overflow_2D_stream_count * sizeof(*neu));
                Z_Free(omaha.overflow_2D_stream);
            }
            omaha.overflow_2D_stream          = neu;
            omaha.overflow_2D_stream_capacity = newCap;
        }
        omaha.overflow_2D_stream[omaha.overflow_2D_stream_count++] = streamChan;
    } else {
        chan = new omaha_channel();
        if (kind == 0) {
            if (omaha.overflow_3D_count >= omaha.overflow_3D_capacity) {
                int               newCap = omaha.overflow_3D_capacity + SOUNDSYSTEM_VOICE_GROW_CHUNK;
                omaha_channel **neu    = (omaha_channel **)Z_Malloc(newCap * sizeof(*neu));
                if (omaha.overflow_3D) {
                    memcpy(neu, omaha.overflow_3D, omaha.overflow_3D_count * sizeof(*neu));
                    Z_Free(omaha.overflow_3D);
                }
                omaha.overflow_3D          = neu;
                omaha.overflow_3D_capacity = newCap;
            }
            omaha.overflow_3D[omaha.overflow_3D_count++] = chan;
        } else {
            if (omaha.overflow_2D_count >= omaha.overflow_2D_capacity) {
                int               newCap = omaha.overflow_2D_capacity + SOUNDSYSTEM_VOICE_GROW_CHUNK;
                omaha_channel **neu    = (omaha_channel **)Z_Malloc(newCap * sizeof(*neu));
                if (omaha.overflow_2D) {
                    memcpy(neu, omaha.overflow_2D, omaha.overflow_2D_count * sizeof(*neu));
                    Z_Free(omaha.overflow_2D);
                }
                omaha.overflow_2D          = neu;
                omaha.overflow_2D_capacity = newCap;
            }
            omaha.overflow_2D[omaha.overflow_2D_count++] = chan;
        }
    }

    if (omaha.channel_extra_count >= omaha.channel_extra_capacity) {
        int               newCap = omaha.channel_extra_capacity + SOUNDSYSTEM_VOICE_GROW_CHUNK;
        omaha_channel **neu    = (omaha_channel **)Z_Malloc(newCap * sizeof(*neu));
        if (omaha.channel_extra) {
            memcpy(neu, omaha.channel_extra, omaha.channel_extra_count * sizeof(*neu));
            Z_Free(omaha.channel_extra);
        }
        omaha.channel_extra          = neu;
        omaha.channel_extra_capacity = newCap;
    }

    if (!S_OMAHA_InitChannel(-1, chan)) {
        return -1;
    }
    chan->voiceKind = kind;
    omaha.channel_extra[omaha.channel_extra_count++] = chan;
    omaha.position_channel_count =
        MAX_SOUNDSYSTEM_POSITION_CHANNELS + omaha.channel_extra_count;
    idx = MAX_SOUNDSYSTEM_POSITION_CHANNELS + omaha.channel_extra_count - 1;
    return idx;
}

/*
==============
S_OMAHA_PickChannelBase

Changed in Omaha: grow overflow voices instead of stealing for capacity.
==============
*/
static int S_OMAHA_PickChannelBase(int iEntNum, int iEntChannel, int iFirstChannel, int iLastChannel)
{
    int             i;
    int             iBestChannel;
    omaha_channel *pChannel;

    /* Omaha: baseline + overflow; grow instead of steal. */
    {
        const int kind =
            (iFirstChannel >= MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D) ? 2
            : (iFirstChannel >= MAX_SOUNDSYSTEM_CHANNELS_3D)                             ? 1
                                                                                       : 0;
        const int posCount = omaha.position_channel_count > 0 ? omaha.position_channel_count
                                                               : MAX_SOUNDSYSTEM_POSITION_CHANNELS;

        iBestChannel = -1;
        if (iEntNum != ENTITYNUM_NONE && iEntChannel) {
            bool bStoppedChannel = false;

            for (i = 0; i < posCount; i++) {
                if (i < MAX_SOUNDSYSTEM_POSITION_CHANNELS && (i < iFirstChannel || i > iLastChannel)) {
                    continue;
                }
                pChannel = S_OMAHA_GetPosChannel(i);
                if (!pChannel) {
                    continue;
                }
                if (i >= MAX_SOUNDSYSTEM_POSITION_CHANNELS && pChannel->voiceKind != kind) {
                    continue;
                }
                if (pChannel->is_free()) {
                    if (iBestChannel < 0 || i < MAX_SOUNDSYSTEM_POSITION_CHANNELS) {
                        iBestChannel = i;
                    }
                    continue;
                }

                if (pChannel->iEntNum == iEntNum && pChannel->iEntChannel == iEntChannel) {
                    pChannel->end_sample();
                    bStoppedChannel = 1;
                    iBestChannel    = i;
                    break;
                }
            }

            (void)bStoppedChannel;
        }

        if (iBestChannel < 0 || (iBestChannel >= 0 && !S_OMAHA_GetPosChannel(iBestChannel)->is_free()
                                 && !(iEntNum != ENTITYNUM_NONE && iEntChannel))) {
            iBestChannel = -1;
            for (i = iFirstChannel; i <= iLastChannel; i++) {
                pChannel = S_OMAHA_GetPosChannel(i);
                if (pChannel && pChannel->is_free()) {
                    return i;
                }
            }
            for (i = MAX_SOUNDSYSTEM_POSITION_CHANNELS; i < posCount; i++) {
                pChannel = S_OMAHA_GetPosChannel(i);
                if (pChannel && pChannel->voiceKind == kind && pChannel->is_free()) {
                    return i;
                }
            }

            {
                int grown = S_OMAHA_GrowVoicePool(kind);
                if (grown >= 0) {
                    return grown;
                }
            }
        }

        return iBestChannel;
    }
}

/*
==============
S_OMAHA_PickChannel3D
==============
*/
static int S_OMAHA_PickChannel3D(int iEntNum, int iEntChannel)
{
    return S_OMAHA_PickChannelBase(iEntNum, iEntChannel, 0, MAX_SOUNDSYSTEM_CHANNELS_3D - 1);
}

/*
==============
S_OMAHA_PickChannel2D
==============
*/
static int S_OMAHA_PickChannel2D(int iEntNum, int iEntChannel)
{
    return S_OMAHA_PickChannelBase(
        iEntNum, iEntChannel, MAX_SOUNDSYSTEM_CHANNELS_3D, MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D - 1
    );
}

/*
==============
S_OMAHA_PickChannel2DStreamed
==============
*/
static int S_OMAHA_PickChannel2DStreamed(int iEntNum, int iEntChannel)
{
    return S_OMAHA_PickChannelBase(
        iEntNum,
        iEntChannel,
        MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D,
        MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM - 1
    );
}

/*
==============
S_OMAHA_callbackServer
==============
*/
void S_OMAHA_callbackServer(int entnum, int channel_number, const char *name)
{
    if (!com_sv_running->integer) {
        return;
    }

    SV_SoundCallback(entnum, channel_number, name);
}

/*
==============
S_OMAHA_Start2DSound
==============
*/
static void S_OMAHA_Start2DSound(
    const vec3_t vOrigin,
    int          iEntNum,
    int          iEntChannel,
    sfx_t       *pSfx,
    float        fVolume,
    float        fMinDistance,
    float        fPitch,
    float        fMaxDistance
)
{
    int             iRealEntNum;
    int             iFreeChannel;
    omaha_channel *pChannel;
    float           fRealVolume;
    bool            bSupportWaitTillSoundDone;

    iRealEntNum = iEntNum & ~S_FLAG_DO_CALLBACK;

    if (pSfx->iFlags & SFX_FLAG_STREAMED) {
        iFreeChannel = S_OMAHA_PickChannel2DStreamed(iRealEntNum, iEntChannel);
    } else {
        iFreeChannel = S_OMAHA_PickChannel2D(iRealEntNum, iEntChannel);
    }

    if (iFreeChannel < 0) {
        Com_DPrintf(
            "Omaha: Couldn't play %s sound '%s' for entity %i on channel %s\n",
            (pSfx->iFlags & SFX_FLAG_STREAMED) ? "2Dstreamed" : "2D",
            pSfx->name,
            iRealEntNum,
            S_ChannelNumToName(iEntChannel)
        );
        return;
    }

    if (iEntNum & S_FLAG_DO_CALLBACK) {
        S_OMAHA_callbackServer(iRealEntNum, iFreeChannel, pSfx->name);
    }

    pChannel = S_OMAHA_GetPosChannel(iFreeChannel);
    if (!pChannel) {
        return;
    }
    pChannel->force_free();

    /* Changed in Omaha: content ≤ 127/84 (Miles ceiling); OM_MIX_* may raise afterward. */
    fVolume = S_OMAHA_ClampContentVolume(fVolume);

    /* Added in Omaha: category mix multipliers. */
    fVolume *= S_OMAHA_CategoryGainScale(S_OMAHA_ResolveCategory(pSfx, iEntChannel), iRealEntNum);
    fRealVolume       = S_OMAHA_GetBaseVolume() * fVolume;
    pChannel->fVolume = fVolume;

    /* Fixed in Omaha: assign channel before LOCAL_LISTENER check (was using stale iEntChannel). */
    pChannel->iEntChannel = iEntChannel;
    if (pChannel->iEntChannel == CHAN_LOCAL) {
        pChannel->iFlags |= CHANNEL_FLAG_LOCAL_LISTENER;
    } else {
        pChannel->iFlags &= ~CHANNEL_FLAG_LOCAL_LISTENER;
    }
    pChannel->iFlags &= ~CHANNEL_FLAG_PAUSED;

    if (fMinDistance < 0.0) {
        fMinDistance = 200.0;
    }
    pChannel->fMinDist = fMinDistance;

    if (fMaxDistance < 0.0) {
        fMaxDistance = pChannel->fMinDist * 64;
    }
    pChannel->fMaxDist = fMaxDistance;

    pChannel->fNewPitchMult = fPitch;
    pChannel->pSfx          = pSfx;
    pChannel->iEntNum       = iRealEntNum;
    if (iRealEntNum == ENTITYNUM_NONE) {
        VectorClear(pChannel->vOrigin);
        pChannel->iFlags |= CHANNEL_FLAG_NO_ENTITY;
        pChannel->iEntNum = 0;

        if (vOrigin) {
            VectorCopy(vOrigin, pChannel->vOrigin);
        }
    } else {
        pChannel->iFlags &= ~CHANNEL_FLAG_NO_ENTITY;
        pChannel->iEntNum = iRealEntNum;
        if (vOrigin) {
            VectorCopy(vOrigin, pChannel->vOrigin);

            bSupportWaitTillSoundDone = cl.serverTime - 1 < 0;
            pChannel->iTime           = cl.serverTime - 1;
            if (bSupportWaitTillSoundDone) {
                pChannel->iTime = 0;
            }

            if (s_entity[iEntNum].time < pChannel->iTime) {
                // Fixed in OPM
                //  Not sure if it's the real solution, but script_origin entities are usually
                //  never sent to client so the sound will immediately get stopped
                pChannel->iFlags |= CHANNEL_FLAG_MISSING_ENT;
            }
        } else {
            VectorClear(pChannel->vOrigin);
            pChannel->iTime = 0;
        }
    }

    pChannel->iStartTime = cl.serverTime;
    pChannel->iEndTime   = (int)(cl.serverTime + pSfx->time_length + 250.f);

    if (iFreeChannel > MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D) {
        // streamed
        if (!pChannel->set_sfx(pSfx)) {
            Com_DPrintf("Omaha: Set stream error - %s\n", pSfx->name);
            return;
        }

        pChannel->iBaseRate = pChannel->sample_playback_rate();
        pChannel->set_no_3d();
        fRealVolume = fRealVolume * s_fVolumeGain;
    } else {
        bool sfxOk;

        pChannel->stop();
        pChannel->set_no_3d();
        sfxOk = pChannel->set_sfx(pSfx);
        if (!sfxOk) {
            return;
        }
    }

    /* Fixed in Omaha: CHAN_LOCAL cues stay dry — OpenAL Soft spatialization/HRTF made hitmarker sound "wet". */
    if (pChannel->iFlags & CHANNEL_FLAG_LOCAL_LISTENER) {
        pChannel->set_no_virtualization();
    }

    /* Added in Omaha: 2D cues stay clear of obstruction. */
    S_OMAHA_UpdateObstruction(pChannel, s_vObstructionListener, pChannel->vOrigin, false, true);

    pChannel->set_gain(fRealVolume);
    pChannel->play();

    if (s_show_sounds->integer > 0) {
        Com_DPrintf(
            "Omaha: 2D - %d (#%i) - %s (vol %f, mindist %f, maxdist %f)\n",
            cl.serverTime,
            iFreeChannel,
            pSfx->name,
            fVolume,
            fMinDistance,
            fMaxDistance
        );
    }
}

/*
==============
S_OMAHA_TraceObstruction

Added in Omaha: fraction of three listener-to-source rays blocked by world brushes
(source origin and +/-12 units vertically). Players, entities and fences do not block.
==============
*/
static float S_OMAHA_TraceObstruction(const vec3_t vListener, const vec3_t vOrigin)
{
    static const float offsets[3] = {0.f, 12.f, -12.f};
    trace_t            tr;
    vec3_t             vEnd;
    int                i;
    int                iRays    = 0;
    int                iBlocked = 0;

    for (i = 0; i < 3; i++) {
        VectorCopy(vOrigin, vEnd);
        vEnd[2] += offsets[i];
        CM_BoxTrace(&tr, vListener, vEnd, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse);
        if (tr.startsolid) {
            // listener inside a brush (noclip): no meaningful answer
            continue;
        }
        iRays++;
        if (tr.fraction < 1.f) {
            iBlocked++;
        }
    }

    return iRays ? (float)iBlocked / (float)iRays : 0.f;
}

/*
==============
S_OMAHA_UpdateObstruction

Added in Omaha: obstruction target for one channel on the omaha mixer. bWorld
marks positional world sounds; local/UI/music channels stay clear. bForce traces
immediately (new sound) instead of waiting for the throttle.
==============
*/
static void S_OMAHA_UpdateObstruction(
    omaha_channel *pChannel, const vec3_t vListener, const vec3_t vOrigin, bool bWorld, bool bForce
)
{
    float fObstruction;
    int   iRoute;

    if (pChannel->voiceId == SND_VOICE_NONE) {
        return;
    }

    /* Added in Omaha: HRTF route refreshes at the same start/update points as obstruction. */
    iRoute = S_OMAHA_SpatialRoute(pChannel, bWorld);
    if (iRoute != pChannel->iSpatialRoute) {
        g_sndRenderer->SetParami(pChannel->voiceId, SND_PARAM_SPATIAL_ROUTE, iRoute);
        pChannel->iSpatialRoute = iRoute;
    }

    fObstruction = pChannel->fObstruction;
    if (!bWorld || !s_obstruction || !s_obstruction->integer || CM_NumInlineModels() <= 0
        || (!(pChannel->iFlags & CHANNEL_FLAG_NO_ENTITY) && pChannel->iEntNum == s_iListenerNumber)) {
        fObstruction = 0;
    } else if (bForce || cls.realtime >= pChannel->iNextObstructionCheck) {
        vec3_t vDelta;
        float  fDist;
        int    iInterval;

        iInterval = OM_OBSTRUCT_CAL_MS;
        pChannel->iNextObstructionCheck = cls.realtime + iInterval;

        VectorSubtract(vOrigin, vListener, vDelta);
        fDist = VectorLength(vDelta);
        if (fDist < 32.f || (pChannel->fMaxDist > 0.f && fDist > pChannel->fMaxDist)) {
            fObstruction = 0;
        } else {
            fObstruction = S_OMAHA_TraceObstruction(vListener, vOrigin);
        }
    }

    if (fObstruction != pChannel->fObstruction) {
        g_sndRenderer->SetParamf(pChannel->voiceId, SND_PARAM_OBSTRUCTION, fObstruction);
        pChannel->fObstruction = fObstruction;
    }
}

/*
==============
S_OMAHA_SpatialRoute

Added in Omaha: headphone HRTF route. Every entity-attached 3D sound of another
entity in a player-ish category (gunfire, foley, footsteps, bodyfalls, voices,
vehicles) gets its own HRTF filter, regardless of loudness or distance, so steps
stay localizable under heavy fire. The listener's own sounds stay dry (no HRTF /
no ambi bus) so first-person weapons and foley stay present. Everything else
(debris, impacts, explosions, world, loops, 2D) shares the ambisonic bus. The
renderer ignores the route unless omaha HRTF is active.
==============
*/
static int S_OMAHA_SpatialRoute(const omaha_channel *pChannel, bool bWorld)
{
    if (!bWorld || pChannel->voiceKind != 0) {
        return SND_ROUTE_BUS;
    }
    /* Own / head-locked: dry stereo. Full HRTF on a source at the listener is hollow. */
    if (pChannel->iFlags & CHANNEL_FLAG_LOCAL_LISTENER) {
        return SND_ROUTE_DRY;
    }
    if (pChannel->iEntNum != ENTITYNUM_NONE && S_OMAHA_IsLocalEntity(pChannel->iEntNum)) {
        return SND_ROUTE_DRY;
    }
    /*
     * cgame plays footsteps, landings and bodyfalls entity-less at the ground
     * trace. The listener's own land right below the head and stay dry; anyone
     * else's get their own HRTF filter like entity-attached sounds.
     */
    if ((pChannel->iFlags & CHANNEL_FLAG_NO_ENTITY)
        && (pChannel->iCategory == SND_CAT_FOOTSTEPS || pChannel->iCategory == SND_CAT_BODYFALLS)) {
        const float fDx = pChannel->vOrigin[0] - s_vObstructionListener[0];
        const float fDy = pChannel->vOrigin[1] - s_vObstructionListener[1];
        const float fDz = s_vObstructionListener[2] - pChannel->vOrigin[2];

        if (fDx * fDx + fDy * fDy < Square(48.f) && fDz > -16.f && fDz < 128.f) {
            return SND_ROUTE_DRY;
        }
        return SND_ROUTE_DIRECT;
    }
    if (pChannel->iFlags & CHANNEL_FLAG_NO_ENTITY) {
        return SND_ROUTE_BUS;
    }
    if (pChannel->iEntNum == ENTITYNUM_NONE) {
        return SND_ROUTE_BUS;
    }

    switch ((sndCategory_t)pChannel->iCategory) {
    case SND_CAT_FIRE:
    case SND_CAT_FOLEY:
    case SND_CAT_FOOTSTEPS:
    case SND_CAT_BODYFALLS:
    case SND_CAT_VOICE:
    case SND_CAT_VEHICLES:
        return SND_ROUTE_DIRECT;
    default:
        return SND_ROUTE_BUS;
    }
}

/*
==============
S_OMAHA_IsCombatVoAlias

Added in Omaha: narrow alias names for short combat VO (not mission dialog).
==============
*/
static qboolean S_OMAHA_IsCombatVoAlias(const char *alias)
{
    if (!alias || !alias[0]) {
        return qfalse;
    }
    if (!Q_stricmpn(alias, "player_pain", 11) || !Q_stricmpn(alias, "player_death", 12)) {
        return qtrue;
    }
    if (!Q_stricmpn(alias, "manon_pain", 10) || !Q_stricmpn(alias, "manon_death", 11)) {
        return qtrue;
    }
    if (!Q_stricmp(alias, "snd_pain")) {
        return qtrue;
    }
    return qfalse;
}

/*
==============
S_OMAHA_IsCombatVoPath

Added in Omaha: dialogue damage/death wavs only (mission VO lives elsewhere).
==============
*/
static qboolean S_OMAHA_IsCombatVoPath(const char *path)
{
    if (!path || !path[0]) {
        return qfalse;
    }
    if (!Q_stristr(path, "/dialogue/") && !Q_stristr(path, "/dialog/")) {
        return qfalse;
    }
    if (Q_stristr(path, "/damage/") || Q_stristr(path, "/death/")) {
        return qtrue;
    }
    return qfalse;
}

/*
==============
S_OMAHA_IsCombatVoSfx

Added in Omaha: path match, or any global alias that plays this file.
==============
*/
static qboolean S_OMAHA_IsCombatVoSfx(const sfx_t *pSfx)
{
    AliasList_t     *list;
    AliasListNode_t *node;

    if (!pSfx || !pSfx->name[0]) {
        return qfalse;
    }
    if (S_OMAHA_IsCombatVoPath(pSfx->name)) {
        return qtrue;
    }

    list = Alias_GetGlobalList();
    if (!list) {
        return qfalse;
    }
    for (node = list->data_list; node; node = node->next) {
        if (Q_stricmp(node->real_name, pSfx->name)) {
            continue;
        }
        if (S_OMAHA_IsCombatVoAlias(node->alias_name)) {
            return qtrue;
        }
    }
    return qfalse;
}

/*
==============
S_OMAHA_TryPromoteCombatVo

Added in Omaha: force-load streamed pain/death so they can use 3D / HRTF.
On failure, restore the streamed stub and return false (caller keeps 2D).
==============
*/
static qboolean S_OMAHA_TryPromoteCombatVo(sfx_t *pSfx)
{
    if (!pSfx || !(pSfx->iFlags & SFX_FLAG_STREAMED)) {
        return qtrue;
    }
    if (pSfx->iFlags & SFX_FLAG_MP3) {
        return qfalse;
    }
    if (!s_pain_hrtf || !s_pain_hrtf->integer) {
        return qfalse;
    }
    if (!S_OMAHA_IsCombatVoSfx(pSfx)) {
        return qfalse;
    }

    pSfx->iFlags &= ~SFX_FLAG_STREAMED;
    if (S_LoadSound(pSfx->name, pSfx, 0, qtrue) && pSfx->data) {
        pSfx->iFlags &= ~SFX_FLAG_STREAMED;
        if (s_show_sounds && s_show_sounds->integer > 0) {
            Com_DPrintf("Omaha: combat VO force-loaded for 3D/HRTF - %s\n", pSfx->name);
        }
        return qtrue;
    }

    /* Fallback: keep retail streamed 2D rather than dropping the cue. */
    Com_DPrintf("Omaha: combat VO force-load failed; keeping streamed 2D - %s\n", pSfx->name);
    S_LoadSound(pSfx->name, pSfx, 1, qfalse);
    return qfalse;
}

/*
==============
S_OMAHA_StartSound
==============
*/
void S_OMAHA_StartSound(
    const vec3_t vOrigin,
    int          iEntNum,
    int          iEntChannel,
    sfxHandle_t  sfxHandle,
    float        fVolume,
    float        fMinDist,
    float        fPitch,
    float        fMaxDist,
    qboolean     bStreamed
)
{
    int             iChannel;
    omaha_channel *pChannel;
    sfx_info_t     *pSfxInfo;
    sfx_t          *pSfx;
    U32             state;
    bool            bOnlyUpdate;
    bool            bSupportWaitTillSoundDone;
    sndCategory_t   cat;
    float           fOverlap;
    float           fOverlapGain;
    float           fPocket;
    float           fPocketGain;
    bool            bPocket;

    bOnlyUpdate = false;
    pSfx        = &s_knownSfx[sfxHandle];
    if (bStreamed) {
        pSfx->iFlags |= SFX_FLAG_STREAMED;
    }

    /* Added in Omaha: pain/death VO → resident 3D when possible (else streamed 2D). */
    if ((pSfx->iFlags & SFX_FLAG_STREAMED) && iEntChannel != CHAN_MENU && iEntChannel != CHAN_LOCAL) {
        S_OMAHA_TryPromoteCombatVo(pSfx);
    }

    if ((pSfx->iFlags & (SFX_FLAG_NO_OFFSET) || pSfx->iFlags & (SFX_FLAG_STREAMED | SFX_FLAG_MP3))
        || iEntChannel == CHAN_MENU || iEntChannel == CHAN_LOCAL) {
        S_OMAHA_Start2DSound(vOrigin, iEntNum, iEntChannel, pSfx, fVolume, fMinDist, fPitch, fMaxDist);
        return;
    }

    bSupportWaitTillSoundDone = (iEntNum & S_FLAG_DO_CALLBACK) != 0;
    iEntNum &= ~S_FLAG_DO_CALLBACK;

    pSfxInfo = &sfx_infos[pSfx->sfx_info_index];
    if (pSfx->iFlags & SFX_FLAG_STREAMED) {
        Com_DPrintf("Omaha: 3D sounds not supported - couldn't play '%s'\n", pSfx->name);
        return;
    }

    /* Changed in Omaha: resolve distances, volume and category before picking a voice (overlap ducking can drop). */
    if (fMinDist < 0.0) {
        fMinDist = 200.0;
        fMaxDist = 12800.0;
    }

    /* Changed in Omaha: content ≤ 127/84 (Miles ceiling); OM_MIX_* may raise afterward. */
    fVolume = S_OMAHA_ClampContentVolume(fVolume);

    /* Added in Omaha: category mix multipliers (local/other for FIRE). */
    cat = S_OMAHA_ResolveCategory(pSfx, iEntChannel);
    fVolume *= S_OMAHA_CategoryGainScale(cat, iEntNum);

    /* Added in Omaha: pocket ducking for other players' fire and explosions (never drops). */
    fPocket     = 0.f;
    fPocketGain = 1.f;
    bPocket     = pSfxInfo->loop_start == -1
          && (cat == SND_CAT_EXPLOSIONS || (cat == SND_CAT_FIRE && !S_OMAHA_IsLocalEntity(iEntNum)));
    if (bPocket && OM_POCKET_ENABLE) {
        const float *pPocketOrigin = vOrigin;

        if (!pPocketOrigin && iEntNum != ENTITYNUM_NONE && iEntNum >= 0 && iEntNum < MAX_GENTITIES) {
            pPocketOrigin = s_entity[iEntNum].position;
        }
        if (pPocketOrigin) {
            fPocketGain = S_OMAHA_PocketGain(cat, iEntNum, pPocketOrigin, S_OMAHA_GetBaseVolume() * fVolume, fMinDist, &fPocket);
            fVolume *= fPocketGain;
        }
    }

    /*
     * Added in Omaha: overlap ducking for entity-less one-shots. cgame plays
     * footsteps, landings and bodyfalls entity-less at the trace point; they
     * belong to a body, so they are not overlap-ducked.
     */
    fOverlap     = 0.f;
    fOverlapGain = 1.f;
    if (!bPocket && OM_OVERLAP_ENABLE && iEntNum == ENTITYNUM_NONE && vOrigin && pSfxInfo->loop_start == -1
        && cat != SND_CAT_EXPLOSIONS && cat != SND_CAT_FOOTSTEPS && cat != SND_CAT_BODYFALLS && cat != SND_CAT_AUTO
        && cat != SND_CAT_NONE) {
        fOverlapGain = S_OMAHA_OverlapGain(cat, vOrigin, S_OMAHA_GetBaseVolume() * fVolume, fMinDist, &fOverlap);
        if (fOverlapGain < OM_OVERLAP_FLOOR) {
            if (s_show_sounds->integer > 0) {
                Com_DPrintf(
                    "Omaha: %d - %s dropped (overlap %f, g %f)\n", cl.serverTime, pSfx->name, fOverlap, fOverlapGain
                );
            }
            return;
        }
        fVolume *= fOverlapGain;
    }

    iChannel = S_OMAHA_PickChannel3D(iEntNum, iEntChannel);
    if (iChannel < 0) {
        Com_DPrintf(
            "Omaha: Couldn't play %s sound '%s' for entity %i on channel %s\n",
            (pSfx->iFlags & SFX_FLAG_STREAMED) ? "3Dstreamed" : "3D",
            pSfx->name,
            iEntNum,
            S_ChannelNumToName(iEntChannel)
        );
        return;
    }

    if (bSupportWaitTillSoundDone) {
        S_OMAHA_callbackServer(iEntNum, iChannel, pSfx->name);
    }

    pChannel = S_OMAHA_GetPosChannel(iChannel);
    if (!pChannel) {
        return;
    }
    pChannel->fNewPitchMult = fPitch;
    pChannel->iEntChannel   = iEntChannel;
    pChannel->iFlags &= ~(CHANNEL_FLAG_PAUSED | CHANNEL_FLAG_LOCAL_LISTENER);
    state = pChannel->sample_status();

    if (pChannel->iEntNum == iEntNum && (state == SND_STATE_PLAYING || state == SND_STATE_PAUSED) && pChannel->pSfx == pSfx) {
        bOnlyUpdate = true;
    } else {
        pChannel->stop();
        pChannel->iFlags &= ~(CHANNEL_FLAG_LOOPING | CHANNEL_FLAG_PAUSED | CHANNEL_FLAG_LOCAL_LISTENER);
        if (!pChannel->set_sfx(pSfx)) {
            Com_DPrintf("Omaha: Set sample error - %s\n", pSfx->name);
            return;
        }
    }

    pChannel->fMinDist  = fMinDist;
    pChannel->fMaxDist  = fMaxDist;
    pChannel->iCategory = (int)cat;
    pChannel->fVolume   = S_OMAHA_GetBaseVolume() * fVolume;

    // Fixed in OPM
    //  Setup the channel for 3D
    pChannel->set_3d();
    /* Changed in Omaha: set_3d resets AL/omaha GAIN to master only — re-apply full volume after. */
    pChannel->set_gain(pChannel->fVolume);

    if (s_show_sounds->integer > 0) {
        Com_DPrintf(
            "Omaha: %d (#%i) - %s (vol %f, mindist %f, maxdist %f, overlap %f, g %f, pocket %f, pg %f)\n",
            cl.serverTime,
            iChannel,
            pSfx->name,
            pChannel->fVolume,
            fMinDist,
            fMaxDist,
            fOverlap,
            fOverlapGain,
            fPocket,
            fPocketGain
        );
    }

    pChannel->set_velocity(0, 0, 0);
    if (iEntNum == ENTITYNUM_NONE) {
        if (vOrigin) {
            VectorCopy(vOrigin, pChannel->vOrigin);
        } else {
            //VectorClear(vOrigin);
            // Fixed in OPM
            //  Tiny mistake found in original where the vOrigin parameter is set to 0
            VectorClear(pChannel->vOrigin);
        }

        pChannel->set_position(pChannel->vOrigin[0], pChannel->vOrigin[1], pChannel->vOrigin[2]);
        pChannel->iFlags |= CHANNEL_FLAG_NO_ENTITY;
        pChannel->iEntNum = 0;
    } else {
        pChannel->iFlags &= ~CHANNEL_FLAG_NO_ENTITY;
        pChannel->iEntNum = iEntNum;
        if (vOrigin) {
            VectorCopy(vOrigin, pChannel->vOrigin);

            bSupportWaitTillSoundDone = cl.serverTime - 1 < 0;
            pChannel->iTime           = cl.serverTime - 1;
            if (bSupportWaitTillSoundDone) {
                pChannel->iTime = 0;
            }

            if (s_entity[iEntNum].time < pChannel->iTime) {
                // Fixed in OPM
                //  Not sure if it's the real solution, but script_origin entities are usually
                //  never sent to client so the sound will immediately get stopped
                pChannel->iFlags |= CHANNEL_FLAG_MISSING_ENT;
            }
        } else {
            VectorClear(pChannel->vOrigin);
            pChannel->iTime = 0;
        }
        pChannel->set_position(pChannel->vOrigin[0], pChannel->vOrigin[1], pChannel->vOrigin[2]);
    }

    if (pSfxInfo->loop_start != -1) {
        pChannel->set_sample_loop_block(pSfxInfo->loop_start, pSfxInfo->loop_end);
        pChannel->set_sample_loop_count(0);

        pChannel->iFlags |= CHANNEL_FLAG_LOOPING;
        if (s_show_sounds->integer) {
            Com_DPrintf("Omaha: loopblock - %d to %d\n", pSfxInfo->loop_start, pSfxInfo->loop_end);
        }
    } else {
        pChannel->set_sample_loop_count(1);
    }

    if (!bOnlyUpdate && S_OMAHA_ShouldStart(pChannel->vOrigin, pChannel->fMinDist, pChannel->fMaxDist)) {
        // Fixed in OPM
        //  Make sure to set the pitch before playing the sound
        pChannel->iBaseRate = pSfx->info.rate;
        pChannel->set_sample_playback_rate(pChannel->iBaseRate * pChannel->fNewPitchMult);
        pChannel->fNewPitchMult = 0;

        /* Added in Omaha: start at the right obstruction instead of the previous sound's. */
        S_OMAHA_UpdateObstruction(pChannel, s_vObstructionListener, pChannel->vOrigin, true, true);

        pChannel->play();
        pChannel->iEndTime   = cl.serverTime + (int)pChannel->pSfx->time_length + 250;
        pChannel->iStartTime = cl.serverTime;
    }
}

/*
==============
S_OMAHA_GrowLoopSounds
Added in Omaha: expand loop list past the old 64-slot cap.
==============
*/
static int S_OMAHA_GrowLoopSounds(void)
{
    int                  newCap;
    omaha_loop_sound_t *neu;

    newCap = omaha.loop_sounds_capacity + SOUNDSYSTEM_VOICE_GROW_CHUNK;
    neu    = (omaha_loop_sound_t *)Z_Malloc(newCap * sizeof(*neu));
    if (omaha.loop_sounds) {
        memcpy(neu, omaha.loop_sounds, omaha.loop_sounds_count * sizeof(*neu));
        Z_Free(omaha.loop_sounds);
    }
    memset(neu + omaha.loop_sounds_count, 0, (newCap - omaha.loop_sounds_count) * sizeof(*neu));
    omaha.loop_sounds          = neu;
    omaha.loop_sounds_capacity = newCap;
    omaha.loop_sounds_count    = newCap;
    return omaha.loop_sounds_count - SOUNDSYSTEM_VOICE_GROW_CHUNK;
}

/*
==============
S_OMAHA_AddLoopingSound
==============
*/
void S_OMAHA_AddLoopingSound(
    const vec3_t vOrigin,
    const vec3_t vVelocity,
    sfxHandle_t  sfxHandle,
    float        fVolume,
    float        fMinDist,
    float        fMaxDist,
    float        fPitch,
    int          iFlags
)
{
    int                  i;
    int                  iFreeLoopSound;
    sfx_t               *pSfx;
    omaha_loop_sound_t *pLoopSound;

    iFreeLoopSound = -1;
    pSfx           = &s_knownSfx[sfxHandle];
    if (!pSfx || !pSfx->length) {
        return;
    }

    for (i = 0; i < omaha.loop_sounds_count; i++) {
        pLoopSound = &omaha.loop_sounds[i];
        if (pLoopSound->pSfx == pSfx && !pLoopSound->bInUse) {
            iFreeLoopSound = i;
            break;
        }
    }

    if (iFreeLoopSound < 0) {
        for (i = 0; i < omaha.loop_sounds_count; i++) {
            pLoopSound = &omaha.loop_sounds[i];
            if (!pLoopSound->pSfx && !pLoopSound->bInUse) {
                iFreeLoopSound       = i;
                pLoopSound->bPlaying = false;
                break;
            }
        }
    }

    /* Changed in Omaha: grow past the baseline instead of dropping when full. */
    if (iFreeLoopSound < 0) {
        iFreeLoopSound = S_OMAHA_GrowLoopSounds();
        if (iFreeLoopSound < 0) {
            return;
        }
    }

    pLoopSound = &omaha.loop_sounds[iFreeLoopSound];
    VectorCopy(vOrigin, pLoopSound->vOrigin);
    VectorCopy(vVelocity, pLoopSound->vVelocity);
    pLoopSound->pSfx       = pSfx;
    pLoopSound->bInUse     = true;
    pLoopSound->iStartTime = cls.realtime;
    /* Changed in Omaha: content ≤ 127/84 (Miles ceiling) before category mix on loops. */
    fVolume = S_OMAHA_ClampContentVolume(fVolume);
    /* Added in Omaha: category mix on loops. */
    fVolume *= S_OMAHA_CategoryGainScale(S_OMAHA_ResolveCategory(pSfx, CHAN_AUTO), ENTITYNUM_NONE);
    pLoopSound->fBaseVolume = fVolume;
    pLoopSound->fMinDist    = fMinDist;
    pLoopSound->fMaxDist    = fMaxDist;
    pLoopSound->fPitch      = fPitch;
    pLoopSound->iFlags      = iFlags;
    pLoopSound->bCombine    = VectorCompare(vVelocity, vec_zero) == 0;

    if (pLoopSound->bCombine) {
        for (i = 0; i < omaha.loop_sounds_count; i++) {
            if (omaha.loop_sounds[i].pSfx == pSfx && omaha.loop_sounds[i].bInUse) {
                pLoopSound->iStartTime = omaha.loop_sounds[i].iStartTime;
                if (omaha.loop_sounds[i].bPlaying) {
                    pLoopSound->bPlaying = true;
                    pLoopSound->iChannel = omaha.loop_sounds[i].iChannel;
                }
            }
        }
    }
}

/*
==============
S_OMAHA_StopLoopingSound
==============
*/
void S_OMAHA_StopLoopingSound(omaha_loop_sound_t *pLoopSound)
{
    bool            bMayStop;
    int             i;
    omaha_channel *pChannel;
    sfx_t          *pSfx;

    if (!pLoopSound->bPlaying) {
        return;
    }
    bMayStop = true;
    pSfx     = pLoopSound->pSfx;

    if (pLoopSound->bCombine) {
        for (i = 0; i < omaha.loop_sounds_count; i++) {
            if (&omaha.loop_sounds[i] != pLoopSound && omaha.loop_sounds[i].pSfx == pSfx
                && omaha.loop_sounds[i].bInUse) {
                bMayStop = false;
                break;
            }
        }
    }

    if (bMayStop) {
        pChannel = S_OMAHA_GetPosChannel(pLoopSound->iChannel);
        if (s_show_sounds->integer > 0 && pChannel && pChannel->pSfx) {
            Com_DPrintf(
                "Omaha: %d (#%i) - stopped loop - %s\n",
                cl.serverTime,
                pLoopSound->iChannel,
                pChannel->pSfx->name
            );
        }
        if (pChannel) {
            pChannel->force_free();
        }
    }
    pLoopSound->pSfx     = NULL;
    pLoopSound->bPlaying = false;

    if (pLoopSound->bCombine && pSfx) {
        for (i = 0; i < omaha.loop_sounds_count; i++) {
            if (omaha.loop_sounds[i].pSfx == pSfx) {
                omaha.loop_sounds[i].bPlaying = false;
                omaha.loop_sounds[i].pSfx     = NULL;
            }
        }
    }
}

/*
==============
S_OMAHA_ClearLoopingSounds
==============
*/
void S_OMAHA_ClearLoopingSounds()
{
    int i;

    for (i = 0; i < omaha.loop_sounds_count; i++) {
        omaha.loop_sounds[i].bInUse = false;
    }
}

/*
==============
S_OMAHA_StopLoopingSounds
==============
*/
void S_OMAHA_StopLoopingSounds()
{
    int i;

    for (i = 0; i < omaha.loop_sounds_count; i++) {
        omaha.loop_sounds[i].bInUse = false;
        S_OMAHA_StopLoopingSound(&omaha.loop_sounds[i]);
    }
}

/*
==============
S_OMAHA_StopSound
==============
*/
void S_OMAHA_StopSound(int iEntNum, int iEntChannel)
{
    int             i;
    const int       posCount = omaha.position_channel_count > 0 ? omaha.position_channel_count
                                                                 : MAX_SOUNDSYSTEM_POSITION_CHANNELS;

    for (i = 0; i < posCount; i++) {
        omaha_channel *pChannel = S_OMAHA_GetPosChannel(i);
        if (pChannel && !pChannel->is_free() && pChannel->iEntNum == iEntNum
            && pChannel->iEntChannel == iEntChannel) {
            pChannel->end_sample();
            break;
        }
    }
}

/*
==============
S_OMAHA_StopAllSounds
==============
*/
void S_OMAHA_StopAllSounds(qboolean bStopMusic)
{
    int i;

    if (!s_bSoundStarted) {
        return;
    }

    S_OMAHA_StopLoopingSounds();

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        omaha_channel *pChannel = omaha.channel[i];
        if (pChannel) {
            pChannel->force_free();
        }
    }
    for (i = 0; i < omaha.channel_extra_count; i++) {
        if (omaha.channel_extra[i]) {
            omaha.channel_extra[i]->force_free();
        }
    }

    if (bStopMusic) {
        S_OMAHA_MUSIC_FreeAllSongs();
        S_OMAHA_TriggeredMusic_Stop();
    }
}

/*
==============
S_OMAHA_Start2DLoopSound
==============
*/
static int S_OMAHA_Start2DLoopSound(
    omaha_loop_sound_t *pLoopSound, float fVolume, float fVolumeToPlay, float fMinDistance, const vec3_t vLoopOrigin
)
{
    int             iChannel;
    int             iSoundOffset;
    omaha_channel *pChannel;

    if (pLoopSound->pSfx->iFlags & SFX_FLAG_STREAMED) {
        iChannel = S_OMAHA_PickChannel2DStreamed(0, 0);
    } else {
        iChannel = S_OMAHA_PickChannel2D(0, 0);
    }

    if (iChannel < 0) {
        Com_DPrintf("Omaha: Could not find a free 2D sound channel\n");
        return iChannel;
    }

    pChannel = S_OMAHA_GetPosChannel(iChannel);
    if (!pChannel) {
        return -1;
    }
    pChannel->force_free();
    pChannel->fVolume = fVolume;

    if (pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN) {
        pChannel->iFlags |= CHANNEL_FLAG_LOCAL_LISTENER;
    } else {
        pChannel->iFlags &= ~CHANNEL_FLAG_LOCAL_LISTENER;
    }
    pChannel->fMinDist = fMinDistance;

    if (!pChannel->set_sfx(pLoopSound->pSfx)) {
        Com_DPrintf("Omaha: Set sample error (loopsound) - %s\n", pLoopSound->pSfx->name);
        pChannel->iFlags &= ~CHANNEL_FLAG_PLAY_DEFERRED;
        return -1;
    }

    pChannel->iEntNum     = 0;
    pChannel->iEntChannel = 0;
    pChannel->pSfx        = pLoopSound->pSfx;
    pChannel->iFlags |= CHANNEL_FLAG_PAUSED | CHANNEL_FLAG_NO_ENTITY;
    pChannel->iBaseRate = pChannel->sample_playback_rate();
    VectorCopy(vLoopOrigin, pChannel->vOrigin);

    iSoundOffset = (int)(pLoopSound->pSfx->info.width * pLoopSound->pSfx->info.rate
                         * (float)(cls.realtime - pLoopSound->iStartTime) / 1000.0);
    pChannel->set_sample_offset(iSoundOffset % pLoopSound->pSfx->length);
    pChannel->set_sample_loop_count(0);
    pChannel->fVolume = fVolumeToPlay;
    pChannel->set_gain(fVolumeToPlay);
    /* Added in Omaha: omaha obstruction before the loop starts. */
    S_OMAHA_UpdateObstruction(
        pChannel, s_vObstructionListener, vLoopOrigin, !(pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN), true
    );
    pChannel->start_sample();
    if (s_show_sounds->integer > 0) {
        Com_DPrintf(
            "Omaha: %d (#%i) - %s (vol %f)\n", cl.serverTime, pLoopSound->iChannel, pLoopSound->pSfx->name, fVolume
        );
    }

    return iChannel;
}

/*
==============
S_OMAHA_Start3DLoopSound
==============
*/
static int S_OMAHA_Start3DLoopSound(
    omaha_loop_sound_t *pLoopSound,
    float                fVolumeToPlay,
    float                fMinDistance,
    float                fMaxDistance,
    const vec3_t         vLoopOrigin,
    const vec3_t         vListenerOrigin
)
{
    int             iChannel;
    vec3_t          vDir;
    int             iSoundOffset;
    omaha_channel *pChan3D;

    if (pLoopSound->pSfx->iFlags & SFX_FLAG_STREAMED) {
        return -1;
    }

    if (!pLoopSound->pSfx->data || !pLoopSound->pSfx->length) {
        return -1;
    }

    iChannel = S_OMAHA_PickChannel3D(0, 0);
    if (iChannel < 0) {
        Com_DPrintf("Omaha: Could not find a free channel\n");
        return iChannel;
    }

    pChan3D = S_OMAHA_GetPosChannel(iChannel);
    if (!pChan3D) {
        return -1;
    }
    pChan3D->force_free();
    pChan3D->iEntNum     = 0;
    pChan3D->iEntChannel = 0;
    pChan3D->fMinDist    = fMinDistance;
    pChan3D->fMaxDist    = fMaxDistance;
    pChan3D->set_3d();

    if (!pChan3D->set_sfx(pLoopSound->pSfx)) {
        Com_DPrintf("Omaha: Set sample error - %s\n", pLoopSound->pSfx->name);
        return -1;
    }

    pChan3D->set_position(vLoopOrigin[0], vLoopOrigin[1], vLoopOrigin[2]);
    VectorScale(pLoopSound->vVelocity, s_fVelocityScale, pLoopSound->vVelocity);
    pChan3D->set_velocity(pLoopSound->vVelocity[0], pLoopSound->vVelocity[1], pLoopSound->vVelocity[2]);
    pChan3D->pSfx = pLoopSound->pSfx;
    pChan3D->iFlags |= CHANNEL_FLAG_PAUSED | CHANNEL_FLAG_NO_ENTITY;
    pChan3D->iBaseRate = pChan3D->sample_playback_rate();

    iSoundOffset = (int)((int)pLoopSound->pSfx->info.width * pLoopSound->pSfx->info.rate
                         * (float)(cls.realtime - pLoopSound->iStartTime) / 1000.0)
                 % pLoopSound->pSfx->length;
    pChan3D->set_sample_offset(iSoundOffset);
    pChan3D->set_sample_loop_count(0);
    pChan3D->fVolume = fVolumeToPlay;
    pChan3D->set_gain(fVolumeToPlay);
    /* Added in Omaha: omaha obstruction before the loop starts. */
    S_OMAHA_UpdateObstruction(
        pChan3D, s_vObstructionListener, vLoopOrigin, !(pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN), true
    );
    pChan3D->play();

    S_OMAHA_reverb(iChannel, s_iReverbType, s_fReverbLevel);

    return iChannel;
}

/*
==============
S_OMAHA_UpdateLoopSound
==============
*/
static bool S_OMAHA_UpdateLoopSound(
    omaha_loop_sound_t *pLoopSound,
    float                fVolumeToPlay,
    float                fMinDistance,
    float                fMaxDistance,
    const vec3_t         vListenerOrigin,
    const vec3_t         vTempAxis,
    const vec3_t         vLoopOrigin
)
{
    omaha_channel *pChannel;
    float           fVolume;
    float           fMaxVolume;
    vec3_t          vDir;
    float           fDistance;

    pChannel = S_OMAHA_GetPosChannel(pLoopSound->iChannel);
    if (!pChannel) {
        return false;
    }

    if (pChannel->pSfx != pLoopSound->pSfx) {
        pLoopSound->bPlaying = 0;
        return false;
    }

    pChannel->iStartTime = cl.serverTime;

    if (pLoopSound->pSfx->iFlags & (SFX_FLAG_NO_OFFSET) || pLoopSound->pSfx->iFlags & (SFX_FLAG_STREAMED | SFX_FLAG_MP3)
        || (pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN)) {
        vec3_t vOrigin;
        int    iPan;

        pChannel->fVolume = fVolumeToPlay / s_fVolumeGain;
        fVolume           = S_OMAHA_GetBaseVolume() * pChannel->fVolume;
        fMaxVolume        = fVolume;

        if (pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN) {
            // Center the sound
            iPan = 64;
        } else {
            VectorCopy(vLoopOrigin, vOrigin);
            iPan = S_OMAHA_SpatializeStereoSound(vListenerOrigin, vTempAxis, vOrigin);

            VectorSubtract(vListenerOrigin, vOrigin, vDir);
            // Clamp the volume by distance
            fDistance = VectorLength(vDir);
            if (pChannel->fMinDist >= fDistance) {
                fVolume = fMaxVolume;
            } else {
                fVolume = pChannel->fMinDist / fDistance * fMaxVolume;
            }
        }

        pChannel->set_gain(fVolume);
        pChannel->set_sample_pan(iPan);
    } else {
        pChannel->set_position(vLoopOrigin[0], vLoopOrigin[1], vLoopOrigin[2]);
        pChannel->fVolume = fVolumeToPlay;
        pChannel->set_gain(fVolumeToPlay);
    }

    /* Added in Omaha: omaha obstruction (centered loops stay clear). */
    S_OMAHA_UpdateObstruction(
        pChannel, vListenerOrigin, vLoopOrigin, !(pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN), false
    );

    if (s_bReverbChanged) {
        // Make sure to update the reverb
        S_OMAHA_reverb(pLoopSound->iChannel, s_iReverbType, s_fReverbLevel);
    }

    return true;
}

/*
==============
S_OMAHA_AddLoopSounds
==============
*/
void S_OMAHA_AddLoopSounds(const vec3_t vTempAxis)
{
    int                  i, j;
    static int           iLoopFrame = 0;
    float                fDistance;
    int                  iChannel;
    vec3_t               vListenerOrigin;
    vec3_t               vLoopOrigin;
    omaha_loop_sound_t *pLoopSound;
    float                fTotalVolume;
    float                fVolumeToPlay;
    float                fMinDistance, fMaxDistance;
    float                fVolume;
    float                fPitch;
    float                fMaxVolume, fMaxFactor;
    omaha_channel      *pChannel;
    /* Changed in Omaha: sized to growable loop list (was fixed 64). */
    static std::vector<char> bAlreadyAdded;
    vec3_t               alvec;

    g_sndRenderer->GetListenerPosition(alvec);
    VectorCopy(alvec, vListenerOrigin);

    bAlreadyAdded.assign((size_t)omaha.loop_sounds_count, 0);

    for (i = 0; i < omaha.loop_sounds_count; i++) {
        vec3_t vDir;

        if (bAlreadyAdded[(size_t)i]) {
            continue;
        }

        pLoopSound = &omaha.loop_sounds[i];
        if (!pLoopSound->pSfx) {
            continue;
        }
        pChannel = pLoopSound->bPlaying ? S_OMAHA_GetPosChannel(pLoopSound->iChannel) : NULL;

        fMinDistance = pLoopSound->fMinDist;
        if (fMinDistance < 0) {
            fMinDistance = 200;
        }

        fMaxDistance = pLoopSound->fMaxDist;
        if (fMaxDistance < 0) {
            fMaxDistance = fMinDistance * 64;
        }

        fVolume = pLoopSound->fBaseVolume;
        if (fVolume < 0) {
            fVolume = 1;
        }
        fVolume = fVolume * s_fAmbientVolume;

        fTotalVolume = 0.0;
        fMaxVolume   = 0.0;

        if (pLoopSound->bPlaying && pChannel) {
            pChannel->fNewPitchMult = pLoopSound->fPitch;
        }

        if (pLoopSound->bCombine) {
            for (j = 0; j < omaha.loop_sounds_count; j++) {
                omaha_loop_sound_t *pLoopSound2 = &omaha.loop_sounds[j];

                if (pLoopSound2->pSfx == pLoopSound->pSfx) {
                    VectorSubtract(pLoopSound2->vOrigin, vListenerOrigin, vDir);
                    VectorCopy(vDir, pLoopSound2->vRelativeOrigin);
                    fDistance = VectorLength(pLoopSound2->vRelativeOrigin);

                    if (fDistance <= fMinDistance) {
                        fVolumeToPlay = fVolume;
                    } else if (fDistance >= fMaxDistance) {
                        fVolumeToPlay = 0;
                    } else {
                        fVolumeToPlay = fMinDistance * fMinDistance * fVolume / (fDistance * fDistance);
                    }

                    if (fMaxVolume < fVolumeToPlay) {
                        fMaxVolume = fVolumeToPlay;
                    }
                    fTotalVolume += fVolumeToPlay;
                    bAlreadyAdded[(size_t)j] = 1;
                }
            }
        } else {
            VectorSubtract(pLoopSound->vOrigin, vListenerOrigin, vDir);
            VectorCopy(vDir, pLoopSound->vRelativeOrigin);
            fDistance = VectorLength(pLoopSound->vRelativeOrigin);

            if (fDistance <= fMinDistance) {
                fTotalVolume = fVolume;
            } else if (fDistance >= fMaxDistance) {
                fTotalVolume = 0;
            } else {
                fTotalVolume = fMinDistance * fMinDistance * fVolume / (fDistance * fDistance);
            }
            pLoopSound->fVolume = fTotalVolume;
            fMaxVolume          = fTotalVolume;
        }

        fMaxFactor = sfx_infos[pLoopSound->pSfx->sfx_info_index].max_factor;
        if (fMaxFactor >= 1 && fMaxVolume * fMaxFactor < fTotalVolume) {
            fTotalVolume = fMaxVolume * fMaxFactor;
        }

        if (fTotalVolume <= 0 && !(pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN)) {
            if (pLoopSound->bPlaying && pChannel) {
                if (s_show_sounds->integer > 0 && pChannel->pSfx) {
                    Com_DPrintf(
                        "Omaha: %d (#%i) - stopped loop - %s\n",
                        cl.serverTime,
                        pLoopSound->iChannel,
                        pChannel->pSfx->name
                    );
                }

                pChannel->stop();
                pLoopSound->bPlaying = false;
                if (pLoopSound->bCombine) {
                    for (j = 0; j < omaha.loop_sounds_count; j++) {
                        omaha_loop_sound_t *pLoopSound2 = &omaha.loop_sounds[j];

                        if (pLoopSound2->pSfx == pLoopSound->pSfx) {
                            pLoopSound2->bPlaying = false;
                        }
                    }
                }
            }

            continue;
        }

        VectorClear(vLoopOrigin);

        if (pLoopSound->bCombine) {
            for (j = 0; j < omaha.loop_sounds_count; j++) {
                omaha_loop_sound_t *pLoopSound2 = &omaha.loop_sounds[j];

                if (pLoopSound2->pSfx == pLoopSound->pSfx) {
                    VectorNormalize(pLoopSound2->vRelativeOrigin);

                    VectorScale(
                        pLoopSound2->vRelativeOrigin, pLoopSound2->fVolume / fTotalVolume, pLoopSound2->vRelativeOrigin
                    );
                    VectorAdd(pLoopSound2->vRelativeOrigin, vLoopOrigin, vLoopOrigin);
                }
            }

            VectorNormalize(vLoopOrigin);
            VectorMA(vListenerOrigin, fMinDistance * 0.5f, vLoopOrigin, vLoopOrigin);
        } else {
            VectorCopy(pLoopSound->vOrigin, vLoopOrigin);
        }

        if (pLoopSound->bPlaying) {
            S_OMAHA_UpdateLoopSound(
                pLoopSound,
                S_OMAHA_GetBaseVolume() * s_fVolumeGain * fTotalVolume,
                fMinDistance,
                fMaxDistance,
                vListenerOrigin,
                vTempAxis,
                vLoopOrigin
            );

            continue;
        }

        if (s_show_sounds->integer > 0) {
            Com_DPrintf(
                "Omaha: %d (#%i) - started loop - %s\n", cl.serverTime, pLoopSound->iChannel, pLoopSound->pSfx->name
            );
        }

        if (pLoopSound->pSfx->iFlags & (SFX_FLAG_NO_OFFSET)
            || pLoopSound->pSfx->iFlags & (SFX_FLAG_STREAMED | SFX_FLAG_MP3)
            || (pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN)) {
            iChannel = S_OMAHA_Start2DLoopSound(
                pLoopSound, fVolume, S_OMAHA_GetBaseVolume() * s_fVolumeGain * fTotalVolume, fMinDistance, vLoopOrigin
            );
        } else {
            iChannel = S_OMAHA_Start3DLoopSound(
                pLoopSound,
                S_OMAHA_GetBaseVolume() * s_fVolumeGain * fTotalVolume,
                fMinDistance,
                fMaxDistance,
                vLoopOrigin,
                vListenerOrigin
            );
        }

        if (iChannel < 0) {
            continue;
        }

        pLoopSound->bPlaying = true;
        pLoopSound->iChannel = iChannel;

        if (pLoopSound->bCombine) {
            for (j = 0; j < omaha.loop_sounds_count; j++) {
                omaha_loop_sound_t *pLoopSound2 = &omaha.loop_sounds[j];

                if (pLoopSound2->pSfx == pLoopSound->pSfx) {
                    pLoopSound2->bPlaying = true;
                    pLoopSound2->iChannel = iChannel;
                }
            }
        }
    }

    (void)iLoopFrame;
    (void)fPitch;
}

/*
==============
S_OMAHA_Respatialize
==============
*/
void S_OMAHA_Respatialize(int iEntNum, const vec3_t vHeadPos, const vec3_t vAxis[3])
{
    int             i;
    vec3_t          vOrigin;
    vec3_t          vVelocity;
    vec3_t          vEntOrigin;
    vec3_t          vEntVelocity;
    vec3_t          vDir;
    vec3_t          vUp;
    vec3_t          vListenerOrigin;
    int             iPan;
    vec3_t          vTempAxis;
    float           fMaxVolume;
    float           fVolume;
    float           fDist;
    omaha_channel *pChannel;
    vec3_t          alvec {0};
    vec3_t          alorientation[2];

    if (cls.no_menus) {
        return;
    }

    s_iListenerNumber = iEntNum;

    //
    // Velocity / position / orientation — Changed in Omaha: via renderer.
    //
    VectorCopy(s_entity[iEntNum].velocity, alvec);
    VectorScale(alvec, s_fVelocityScale, alvec);
    VectorCopy(vHeadPos, vListenerOrigin);
    alorientation[0][0] = vAxis[0][0];
    alorientation[0][1] = vAxis[0][1];
    alorientation[0][2] = vAxis[0][2];
    alorientation[1][0] = vAxis[2][0];
    alorientation[1][1] = vAxis[2][1];
    alorientation[1][2] = vAxis[2][2];
    g_sndRenderer->SetListener(vListenerOrigin, alvec, alorientation[0], alorientation[1]);

    VectorCopy(vAxis[1], vTempAxis);
    VectorCopy(vListenerOrigin, s_vObstructionListener); // Added in Omaha

    fVolume = 1;
    iPan    = 64;

    {
    const int posCount = omaha.position_channel_count > 0 ? omaha.position_channel_count
                                                           : MAX_SOUNDSYSTEM_POSITION_CHANNELS;
    for (i = 0; i < posCount; i++) {
        pChannel = S_OMAHA_GetPosChannel(i);
        if (!pChannel) {
            continue;
        }
        fMaxVolume = S_OMAHA_GetBaseVolume() * pChannel->fVolume;

        if (!pChannel->is_playing()) {
            continue;
        }

        if (pChannel->iFlags & CHANNEL_FLAG_PAUSED) {
            continue;
        }

        if (pChannel->iFlags & CHANNEL_FLAG_NO_ENTITY) {
            VectorCopy(pChannel->vOrigin, vOrigin);

            if (pChannel->iFlags & CHANNEL_FLAG_LOCAL_LISTENER) {
                VectorCopy(vListenerOrigin, vOrigin);
                /* Fixed in Omaha: classify by voiceKind — overflow 3D indices are >= 32. */
                if (pChannel->voiceKind != 0) {
                    fVolume = fMaxVolume;
                    iPan    = 64;
                } else {
                    pChannel->set_position(vOrigin[0], vOrigin[1], vOrigin[2]);
                }
            } else {
                if (pChannel->voiceKind != 0) {
                    iPan = S_OMAHA_SpatializeStereoSound(vListenerOrigin, vTempAxis, vOrigin);
                    VectorSubtract(vListenerOrigin, vOrigin, vDir);

                    fDist = VectorLength(vDir);
                    if (fDist <= pChannel->fMinDist + 0.001f) {
                        fVolume = fMaxVolume;
                    } else if (fDist >= pChannel->fMaxDist - 0.001f) {
                        fVolume = 0;
                    } else {
                        fVolume = (1.0 - (fDist - pChannel->fMinDist) / (pChannel->fMaxDist - pChannel->fMinDist))
                                * fMaxVolume;
                    }
                } else {
                    pChannel->set_position(vOrigin[0], vOrigin[1], vOrigin[2]);
                }
            }
        } else if (pChannel->iFlags & CHANNEL_FLAG_LOCAL_LISTENER) {
            VectorCopy(vListenerOrigin, vOrigin);
            if (pChannel->voiceKind != 0) {
                fVolume = fMaxVolume;
                iPan    = 64;
            } else {
                pChannel->set_position(vOrigin[0], vOrigin[1], vOrigin[2]);
            }
        } else {
            if (s_entity[pChannel->iEntNum].time < pChannel->iTime) {
                VectorCopy(pChannel->vOrigin, vOrigin);
                if (!(pChannel->iFlags & CHANNEL_FLAG_LOOPING) && !(pChannel->iFlags & (CHANNEL_FLAG_MISSING_ENT))) {
                    pChannel->end_sample();
                    continue;
                }
            } else {
                pChannel->iFlags &= ~CHANNEL_FLAG_MISSING_ENT;

                VectorCopy(s_entity[pChannel->iEntNum].position, vEntOrigin);
                VectorCopy(vEntOrigin, vOrigin);
                VectorCopy(vOrigin, pChannel->vOrigin);
                pChannel->iTime = s_entity[pChannel->iEntNum].time;
            }

            if (s_entity[pChannel->iEntNum].use_listener) {
                VectorCopy(vListenerOrigin, vOrigin);
            }

            if (pChannel->iEntNum == s_iListenerNumber) {
                if (vListenerOrigin[0] == vOrigin[0] && vListenerOrigin[2] == vOrigin[2]) {
                    float fDelta = vListenerOrigin[1] - vOrigin[1];

                    if (fDelta > 89.9f && fDelta < 90.09f) {
                        VectorCopy(vListenerOrigin, vOrigin);
                    }
                }
            }

            if (pChannel->voiceKind != 0) {
                iPan = S_OMAHA_SpatializeStereoSound(vListenerOrigin, vTempAxis, vOrigin);
                VectorSubtract(vListenerOrigin, vOrigin, vDir);
                fDist = VectorLength(vDir);
                if (fDist <= pChannel->fMinDist + 0.001f) {
                    fVolume = fMaxVolume;
                } else if (fDist >= pChannel->fMaxDist - 0.001f) {
                    fVolume = 0;
                } else {
                    fVolume =
                        (1.0 - (fDist - pChannel->fMinDist) / (pChannel->fMaxDist - pChannel->fMinDist)) * fMaxVolume;
                }
            } else {
                pChannel->set_position(vOrigin[0], vOrigin[1], vOrigin[2]);
                VectorCopy(s_entity[pChannel->iEntNum].velocity, vEntVelocity);
                VectorCopy(vEntVelocity, vVelocity);
                VectorScale(vVelocity, s_fVelocityScale, vVelocity);
                pChannel->set_velocity(vVelocity[0], vVelocity[1], vVelocity[2]);
            }
        }

        if (pChannel->voiceKind != 0) {
            pChannel->set_gain(fVolume);
            /* Fixed in Omaha: do not spatialize LOCAL_LISTENER (set_sample_pan at z=1 colors dry UI clicks). */
            if (!(pChannel->iFlags & CHANNEL_FLAG_LOCAL_LISTENER)) {
                pChannel->set_sample_pan(iPan);
            }
        }

        /* Added in Omaha: omaha obstruction for world sounds. */
        S_OMAHA_UpdateObstruction(
            pChannel, vListenerOrigin, vOrigin, !(pChannel->iFlags & CHANNEL_FLAG_LOCAL_LISTENER), false
        );

		if (s_bReverbChanged) {
			S_OMAHA_reverb(i, s_iReverbType, s_fReverbLevel);
		}
    }
    } /* posCount scope — Added in Omaha */

    S_OMAHA_AddLoopSounds(vTempAxis);
    s_bReverbChanged = false;
}

/*
==============
S_OMAHA_SpatializeStereoSound
==============
*/
static int S_OMAHA_SpatializeStereoSound(const vec3_t listener_origin, const vec3_t listener_left, const vec3_t origin)
{
    float  lscale, rscale;
    vec3_t source_vec;
    float  pan;

    VectorSubtract(origin, listener_origin, source_vec);
    VectorNormalize(source_vec);

    pan = s_separation->value + (1.f - s_separation->value) * -DotProduct(listener_left, source_vec);

    if (pan < 0) {
        pan = 0;
    }

    return ceilf(pan * 127);
}

/*
==============
S_OMAHA_reverb
==============
*/
static void S_OMAHA_reverb(int iChannel, int iReverbType, float fReverbLevel)
{
    // FIXME: Connect source to effect slot
    //  see https://github.com/kcat/openal-soft/blob/master/examples/alreverb.c

    // No reverb currently.
}

/*
==============
S_OMAHA_SetReverb
==============
*/
void S_OMAHA_SetReverb(int iType, float fLevel)
{
    s_fReverbLevel = fLevel;
    s_iReverbType  = iType;
    if (al_use_reverb) {
        s_bReverbChanged = true;
    }

    // FIXME: generate effect and auxiliary effect slot
    //  or destroy them
    //  see https://github.com/kcat/openal-soft/blob/master/examples/alreverb.c
}

/*
==============
S_OMAHA_Update
==============
*/
void S_OMAHA_Update()
{
    int             i;
    omaha_channel *pChannel;

    if (cl.snap.ps.stats[STAT_CINEMATIC]) {
        S_SetGlobalAmbientVolumeLevel(0.5f);
    } else {
        S_SetGlobalAmbientVolumeLevel(1.f);
    }

    if (paused->integer && !s_bSoundPaused) {
        S_OMAHA_PauseSound();
    } else if (!paused->integer && s_bSoundPaused) {
        S_OMAHA_UnpauseSound();
    }

    if (s_bFading) {
        s_fFadeVolume = 1.f - (cls.realtime - s_fFadeStartTime) / (s_fFadeStopTime - s_fFadeStartTime);
        if (s_fFadeVolume < 0) {
            s_fFadeVolume = 0;
        }
        music_volume_changed = true;
    }

    if (s_volume->modified) {
        if (s_volume->value > 1) {
            Cvar_Set("s_volume", "1.0");
        } else if (s_volume->value < 0) {
            Cvar_Set("s_volume", "0.0");
        }

        music_volume_changed = true;
        s_volume->modified   = 0;
        S_OMAHA_ApplyListenerGain(qtrue);
    }

    /* Added in Omaha: fades move the post-limiter master (no-op when unchanged). */
    S_OMAHA_ApplyListenerGain(qfalse);

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        pChannel = omaha.channel[i];
        if (!pChannel) {
            continue;
        }

        if (!pChannel->is_playing()) {
            continue;
        }

        if (pChannel->fNewPitchMult <= 0) {
            continue;
        }

        pChannel->set_sample_playback_rate(pChannel->iBaseRate * pChannel->fNewPitchMult);
        pChannel->fNewPitchMult = 0;
    }
    for (i = 0; i < omaha.channel_extra_count; i++) {
        pChannel = omaha.channel_extra[i];
        if (!pChannel || !pChannel->is_playing() || pChannel->fNewPitchMult <= 0) {
            continue;
        }
        pChannel->set_sample_playback_rate(pChannel->iBaseRate * pChannel->fNewPitchMult);
        pChannel->fNewPitchMult = 0;
    }

    if (s_speaker_type->modified) {
        Cbuf_AddText("snd_restart\n");
        s_speaker_type->modified = false;
    }

    if (s_reverb->modified) {
        s_reverb->modified = false;

        if (s_reverb->integer) {
            if (al_use_reverb) {
                S_OMAHA_SetReverb(s_iReverbType, s_fReverbLevel);
            } else {
                Com_Printf("Omaha: No reverb support. Reverb is disabled.\n");
            }
        } else if (al_use_reverb) {
            S_OMAHA_SetReverb(0, 0);
        }
    }

    if (s_show_num_active_sounds->integer == 1) {
        int num = 0;

        for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
            pChannel = omaha.channel[i];
            if (pChannel && pChannel->is_playing()) {
                ++num;
            }
        }
        for (i = 0; i < omaha.channel_extra_count; i++) {
            pChannel = omaha.channel_extra[i];
            if (pChannel && pChannel->is_playing()) {
                ++num;
            }
        }
        Com_DPrintf("Omaha: Number of active sounds = %d\n", num);
    }

    S_OMAHA_Music_Update();

    for (i = 0; i < omaha.loop_sounds_count; i++) {
        if (!omaha.loop_sounds[i].bInUse) {
            S_OMAHA_StopLoopingSound(&omaha.loop_sounds[i]);
        }
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS; i++) {
        omaha.channel[i]->update();
    }
    for (i = 0; i < omaha.channel_extra_count; i++) {
        if (omaha.channel_extra[i]) {
            omaha.channel_extra[i]->update();
        }
    }
}

/*
==============
S_OMAHA_GetMusicFilename
==============
*/
const char *S_OMAHA_GetMusicFilename()
{
    // Don't call into the channel if QAL initialisation did not succeed.
    if (!al_initialized) {
        return "";
    }

    if (!omaha.chan_trig_music.is_playing() && !omaha.chan_trig_music.is_paused()) {
        return "";
    }

    return omaha.tm_filename;
}

/*
==============
S_OMAHA_GetMusicLoopCount
==============
*/
int S_OMAHA_GetMusicLoopCount()
{
    return omaha.tm_loopcount;
}

/*
==============
S_OMAHA_GetMusicOffset
==============
*/
unsigned int S_OMAHA_GetMusicOffset()
{
    return omaha.chan_trig_music.sample_offset();
}

/*
==============
S_OMAHA_IsSoundPlaying
==============
*/
qboolean S_OMAHA_IsSoundPlaying(int channel_number, const char *sfxName)
{
    omaha_channel *pChannel = S_OMAHA_GetPosChannel(channel_number);
    if (!pChannel && channel_number >= 0 && channel_number < MAX_SOUNDSYSTEM_CHANNELS) {
        pChannel = omaha.channel[channel_number];
    }
    if (!pChannel) {
        return false;
    }

    if (s_bSoundPaused) {
        return false;
    }

    if (!pChannel->is_playing()) {
        return false;
    }

    if (!pChannel->pSfx) {
        return false;
    }

    if (strcmp(sfxName, pChannel->pSfx->name)) {
        return false;
    }

    return true;
}

/*
==============
S_StoreBase
==============
*/
static void S_StoreBase(channelbasesavegame_t *pBase, omaha_channel *pChannel)
{
    if (!pChannel) {
        return;
    }

    if (pChannel->iEntChannel == CHAN_MENU || pChannel->is_free()) {
        pBase->bPlaying      = false;
        pBase->iOffset       = 0;
        pBase->iLoopCount    = 0;
        pBase->sfx.szName[0] = 0;
        pBase->sfx.iFlags    = 0;
        pBase->fNewPitchMult = 1.f;
        pBase->iBaseRate     = pChannel->iBaseRate;
        pBase->iStatus       = 0;
    } else {
        pBase->bPlaying   = true;
        pBase->iOffset    = pChannel->sample_offset();
        pBase->iLoopCount = pChannel->sample_loop_count();
        memcpy(pBase->sfx.szName, pChannel->pSfx->name, sizeof(pBase->sfx.szName));
        pBase->sfx.iFlags    = pChannel->pSfx->iFlags;
        pBase->iBaseRate     = pChannel->iBaseRate;
        pBase->iStatus       = 0;
        pBase->fNewPitchMult = (float)pBase->iBaseRate / (float)pChannel->sample_playback_rate();
    }

    pBase->iStartTime                = pChannel->iStartTime - cl.serverTime;
    pBase->iEndTime                  = pChannel->iEndTime - cl.serverTime;
    pBase->iEntChannel               = pChannel->iEntChannel;
    pBase->iEntNum                   = pChannel->iEntNum;
    pBase->iFlags                    = pChannel->iFlags;
    pBase->fMaxDist                  = pChannel->fMaxDist;
    pBase->fMinDist                  = pChannel->fMinDist;
    pBase->iNextCheckObstructionTime = 0;
    VectorCopy(pChannel->vOrigin, pBase->vOrigin);
    pBase->iTime   = pChannel->iTime - cl.serverTime;
    pBase->fVolume = pChannel->fVolume;
}

/*
==============
S_StartSoundFromBase
==============
*/
static void
S_StartSoundFromBase(channelbasesavegame_t *pBase, omaha_channel *pChannel, sfx_t *pSfx, bool bStartUnpaused)
{
    if (!pChannel->set_sfx(pSfx)) {
        Com_DPrintf("Omaha: Set sample error - %s\n", pSfx->name);
        pChannel->iFlags &= ~CHANNEL_FLAG_PLAY_DEFERRED;
        return;
    }

    pChannel->iBaseRate = pChannel->sample_playback_rate();

    if (sfx_infos[pSfx->sfx_info_index].loop_start != -1) {
        pChannel->set_sample_loop_block(
            sfx_infos[pSfx->sfx_info_index].loop_start, sfx_infos[pSfx->sfx_info_index].loop_end
        );
        pChannel->set_sample_loop_count(1);
        pChannel->iFlags |= CHANNEL_FLAG_LOOPING;
        if (s_show_sounds->integer > 0) {
            Com_DPrintf(
                "Omaha: loopblock - %d to %d\n",
                sfx_infos[pSfx->sfx_info_index].loop_start,
                sfx_infos[pSfx->sfx_info_index].loop_end
            );
        }
    } else {
        pChannel->set_sample_loop_count(1);
    }

    pChannel->set_gain(pChannel->fVolume);
    pChannel->set_sample_offset(pBase->iOffset);
    pChannel->set_sample_playback_rate(pChannel->iBaseRate * pBase->fNewPitchMult);

    if (bStartUnpaused) {
        pChannel->resume_sample();
    } else {
        pChannel->iFlags |= CHANNEL_FLAG_PLAY_DEFERRED;
    }

    if (pChannel->iEntNum != ENTITYNUM_NONE) {
        s_entity[pChannel->iEntNum].time = pChannel->iTime;
        VectorCopy(pChannel->vOrigin, s_entity[pChannel->iEntNum].position);
    }
}

/*
==============
S_LoadBase
==============
*/
static void S_LoadBase(channelbasesavegame_t *pBase, omaha_channel *pChannel, bool bStartUnpaused)
{
    sfxHandle_t handle;

    if (!pChannel) {
        return;
    }

    if (!pBase->bPlaying) {
        return;
    }

    if (strstr(pBase->sfx.szName, "null.wav")) {
        return;
    }

    handle = S_RegisterSound(pBase->sfx.szName, (pBase->sfx.iFlags & SFX_FLAG_STREAMED), false);

    pChannel->iBaseRate     = pBase->iBaseRate;
    pChannel->iStartTime    = pBase->iStartTime;
    pChannel->iEndTime      = pBase->iEndTime;
    pChannel->iEntChannel   = pBase->iEntChannel;
    pChannel->iEntNum       = pBase->iEntNum;
    pChannel->iFlags        = pBase->iFlags;
    pChannel->fMaxDist      = pBase->fMaxDist;
    pChannel->fMinDist      = pBase->fMinDist;
    pChannel->fNewPitchMult = pBase->fNewPitchMult;
    VectorCopy(pBase->vOrigin, pChannel->vOrigin);
    pChannel->iTime   = pBase->iTime;
    pChannel->fVolume = pBase->fVolume;
    pChannel->pSfx    = &s_knownSfx[handle];

    S_StartSoundFromBase(pBase, pChannel, &s_knownSfx[handle], bStartUnpaused);
}

/*
==============
S_OMAHA_SaveData
==============
*/
void S_OMAHA_SaveData(soundsystemsavegame_t *pSave)
{
    int  i;
    bool bSoundWasUnpaused;

    bSoundWasUnpaused = !s_bSoundPaused;
    if (!s_bSoundPaused) {
        S_OMAHA_PauseSound();
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        S_StoreBase(&pSave->Channels[i], omaha.channel[i]);
    }

    /* Added in Omaha: overflow voices fill free slots of their own kind; extras are dropped. */
    for (i = 0; i < omaha.channel_extra_count; i++) {
        omaha_channel *pChannel = omaha.channel_extra[i];
        int             first, last, j;

        if (!pChannel || !pChannel->pSfx || pChannel->iEntChannel == CHAN_MENU || pChannel->is_free()) {
            continue;
        }

        if (pChannel->voiceKind == 2) {
            first = MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D;
            last  = MAX_SOUNDSYSTEM_POSITION_CHANNELS;
        } else if (pChannel->voiceKind == 1) {
            first = MAX_SOUNDSYSTEM_CHANNELS_3D;
            last  = MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D;
        } else {
            first = 0;
            last  = MAX_SOUNDSYSTEM_CHANNELS_3D;
        }

        for (j = first; j < last; j++) {
            if (!pSave->Channels[j].bPlaying) {
                S_StoreBase(&pSave->Channels[j], pChannel);
                break;
            }
        }
    }

    if (bSoundWasUnpaused) {
        S_OMAHA_UnpauseSound();
    }
}

/*
==============
S_OMAHA_ReLoad
==============
*/
void S_OMAHA_ReLoad(soundsystemsavegame_t *pSave)
{
    int  i;
    bool bSoundWasUnpaused;

    bSoundWasUnpaused = !s_bSoundPaused;
    if (!s_bSoundPaused) {
        S_OMAHA_PauseSound();
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        S_LoadBase(&pSave->Channels[i], omaha.channel[i], bSoundWasUnpaused);
    }

    if (bSoundWasUnpaused) {
        S_OMAHA_UnpauseSound();
    }
}

/*
==============
S_InitBase
==============
*/
static void S_InitBase(channelbasesavegame_t *pBase)
{
    if (!pBase->bPlaying) {
        return;
    }

    if (strstr(pBase->sfx.szName, "null.wav")) {
        return;
    }

    SV_AddSvsTimeFixup(&pBase->iStartTime);
    SV_AddSvsTimeFixup(&pBase->iEndTime);
    SV_AddSvsTimeFixup(&pBase->iTime);
}

/*
==============
S_OMAHA_LoadData
==============
*/
void S_OMAHA_LoadData(soundsystemsavegame_t *pSave)
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        S_InitBase(&pSave->Channels[i]);
    }
}

/*
==============
omaha_channel::set_velocity
==============
*/
void omaha_channel::set_velocity(float v0, float v1, float v2)
{
    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    g_sndRenderer->SetParam3f(voiceId, SND_PARAM_VELOCITY, v0, v1, v2);
}

/*
==============
omaha_channel::set_position
==============
*/
void omaha_channel::set_position(float v0, float v1, float v2)
{
    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    g_sndRenderer->SetParam3f(voiceId, SND_PARAM_POSITION, v0, v1, v2);
}

/*
==============
omaha_channel::set_gain
==============
*/
void omaha_channel::set_gain(float gain)
{
    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    /* Fixed in Omaha: lift the 1.0 max gain so volume > 1 (e.g. hit notify 2.0) takes effect. */
    g_sndRenderer->SetParamf(voiceId, SND_PARAM_MAX_GAIN, 16.0f);
    g_sndRenderer->SetParamf(voiceId, SND_PARAM_GAIN, gain);
}

/*
==============
omaha_channel::set_no_3d
==============
*/
void omaha_channel::set_no_3d()
{
    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    g_sndRenderer->SetParam3f(voiceId, SND_PARAM_POSITION, 0, 0, 0);
    g_sndRenderer->SetParam3f(voiceId, SND_PARAM_VELOCITY, 0, 0, 0);
    g_sndRenderer->SetParami(voiceId, SND_PARAM_SOURCE_RELATIVE, 1);
    g_sndRenderer->SetParami(voiceId, SND_PARAM_LOOPING, 0);
    g_sndRenderer->SetParamf(voiceId, SND_PARAM_ROLLOFF_FACTOR, 0);
    g_sndRenderer->SetParamf(voiceId, SND_PARAM_GAIN, S_OMAHA_GetBaseVolume());
}

/*
==============
omaha_channel::set_3d
==============
*/
void omaha_channel::set_3d()
{
    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    g_sndRenderer->SetParami(voiceId, SND_PARAM_SOURCE_RELATIVE, 0);
    g_sndRenderer->SetParami(voiceId, SND_PARAM_LOOPING, 0);
    g_sndRenderer->SetParamf(voiceId, SND_PARAM_ROLLOFF_FACTOR, 1.0f);
    g_sndRenderer->SetParamf(voiceId, SND_PARAM_GAIN, S_OMAHA_GetBaseVolume());
    g_sndRenderer->SetParami(voiceId, SND_PARAM_DISTANCE_MODEL, SND_DISTANCE_LINEAR_CLAMPED);
    if (fMinDist > 0) {
        g_sndRenderer->SetParamf(voiceId, SND_PARAM_REFERENCE_DISTANCE, fMinDist);
    }
    if (fMaxDist > 0) {
        g_sndRenderer->SetParamf(voiceId, SND_PARAM_MAX_DISTANCE, fMaxDist);
    }
}

/*
==============
omaha_channel::play
==============
*/
void omaha_channel::play()
{
    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    g_sndRenderer->Play(voiceId);
}

/*
==============
omaha_channel::set_no_virtualization
==============
*/
void omaha_channel::set_no_virtualization()
{
    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    g_sndRenderer->SetParami(voiceId, SND_PARAM_DIRECT_CHANNELS, SND_DIRECT_CHANNELS_REMIX);
}

/*
==============
omaha_channel::set_virtualization
==============
*/
void omaha_channel::set_virtualization()
{
    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    g_sndRenderer->SetParami(voiceId, SND_PARAM_DIRECT_CHANNELS, SND_DIRECT_CHANNELS_OFF);
}

/*
==============
omaha_channel::pause
==============
*/
void omaha_channel::pause()
{
    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    g_sndRenderer->Pause(voiceId);
}

/*
==============
omaha_channel::stop
==============
*/
void omaha_channel::stop()
{
    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    g_sndRenderer->Stop(voiceId);
}

/*
==============
omaha_channel::is_free
==============
*/
bool omaha_channel::is_free()
{
    U32 state = sample_status();

    return state == SND_STATE_INITIAL || state == SND_STATE_STOPPED;
}

/*
==============
omaha_channel::is_paused
==============
*/
bool omaha_channel::is_paused()
{
    return sample_status() == SND_STATE_PAUSED;
}

/*
==============
omaha_channel::is_playing
==============
*/
bool omaha_channel::is_playing()
{
    return sample_status() == SND_STATE_PLAYING;
}

/*
==============
omaha_channel::force_free
==============
*/
void omaha_channel::force_free()
{
    stop();
    iFlags = 0;
}

/*
==============
omaha_channel::set_sfx
==============
*/
bool omaha_channel::set_sfx(sfx_t *pSfx)
{
    sndBufferId_t bid;

    assert(pSfx->length);

    this->pSfx = pSfx;

    if (voiceId == SND_VOICE_NONE) {
        return false;
    }

    /* Changed in Omaha: pSfx->buffer holds renderer buffer index + 1; 0 = not uploaded yet. */
    if (!pSfx->buffer) {
        bid = g_sndRenderer->CreateBufferFromSfx(pSfx);
        if (bid == SND_BUFFER_NONE) {
            return false;
        }

        if (pSfx->iFlags & SFX_FLAG_MP3) {
            snd_info_t info;
            void      *data = S_CodecLoad(pSfx->name, &info);
            qboolean   ok;

            if (!data) {
                g_sndRenderer->DestroyBuffer(bid);
                Com_DPrintf("Sound: Failed to load MP3.\n");
                return false;
            }
            if (info.size == 0) {
                byte dummyData[2] = {0};
                ok = g_sndRenderer->UploadBuffer(bid, SND_FMT_MONO16, dummyData, 2, 22050, 0);
            } else {
                ok = g_sndRenderer->UploadBuffer(
                    bid,
                    S_SndFmtFromWidthChannels(info.width, info.channels),
                    data,
                    info.size,
                    info.rate,
                    info.dataalign
                );
            }
            Hunk_FreeTempMemory(data);
            if (!ok) {
                g_sndRenderer->DestroyBuffer(bid);
                Com_DPrintf("Sound: Failed to load MP3.\n");
                return false;
            }
        } else if (!g_sndRenderer->UploadBuffer(
                       bid,
                       S_SndFmtFromWidthChannels(pSfx->info.width, pSfx->info.channels),
                       &pSfx->data[pSfx->info.dataofs],
                       pSfx->info.datasize - pSfx->info.dataofs,
                       pSfx->info.rate,
                       pSfx->info.dataalign
                   )) {
            g_sndRenderer->DestroyBuffer(bid);
            Com_Printf(
                "Sound: Bad Wave file (%d channels, %d bits) [%s].\n",
                pSfx->info.channels,
                (int)(pSfx->info.width * 8),
                pSfx->name
            );
            return false;
        }
        pSfx->buffer = (unsigned int)(bid + 1);
    } else {
        bid = (sndBufferId_t)(pSfx->buffer - 1);
    }

    bufferId = bid;
    g_sndRenderer->Stop(voiceId);
    g_sndRenderer->SetBuffer(voiceId, bid);
    g_sndRenderer->SetParamf(voiceId, SND_PARAM_PITCH, 1.0f);
    iBaseRate = g_sndRenderer->BufferFrequency(bid);
    return true;
}

/*
==============
omaha_channel::start_sample
==============
*/
void omaha_channel::start_sample()
{
    play();
}

/*
==============
omaha_channel::stop_sample
==============
*/
void omaha_channel::stop_sample()
{
    pause();
}

/*
==============
omaha_channel::resume_sample
==============
*/
void omaha_channel::resume_sample()
{
    play();
}

/*
==============
omaha_channel::end_sample
==============
*/
void omaha_channel::end_sample()
{
    stop();
}

/*
==============
omaha_channel::set_sample_pan
==============
*/
void omaha_channel::set_sample_pan(S32 pan)
{
    const float panning = (pan - 64) / 127.f;
    const float z       = sqrtf(1.f - Square(panning));

    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    g_sndRenderer->SetParamf(voiceId, SND_PARAM_ROLLOFF_FACTOR, 0);
    g_sndRenderer->SetParami(voiceId, SND_PARAM_SOURCE_RELATIVE, 1);
    g_sndRenderer->SetParam3f(voiceId, SND_PARAM_POSITION, panning, 0, z);
}

/*
==============
omaha_channel::set_sample_playback_rate
==============
*/
void omaha_channel::set_sample_playback_rate(S32 rate)
{
    float pitch = rate / (float)(iBaseRate > 0 ? iBaseRate : rate);

    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    g_sndRenderer->SetParamf(voiceId, SND_PARAM_PITCH, pitch);
}

/*
==============
omaha_channel::sample_playback_rate
==============
*/
S32 omaha_channel::sample_playback_rate()
{
    float pitch = 1;

    if (voiceId != SND_VOICE_NONE) {
        pitch = g_sndRenderer->GetParamf(voiceId, SND_PARAM_PITCH);
    }
    return buffer_frequency() * pitch;
}

/*
==============
omaha_channel::buffer_frequency
==============
*/
U32 omaha_channel::buffer_frequency() const
{
    if (bufferId != SND_BUFFER_NONE) {
        return (U32)g_sndRenderer->BufferFrequency(bufferId);
    }
    if (pSfx && pSfx->buffer) {
        return (U32)g_sndRenderer->BufferFrequency((sndBufferId_t)(pSfx->buffer - 1));
    }
    return 22050;
}

/*
==============
omaha_channel::sample_volume
==============
*/
S32 omaha_channel::sample_volume()
{
    float gain = 0;

    if (voiceId != SND_VOICE_NONE) {
        gain = g_sndRenderer->GetParamf(voiceId, SND_PARAM_GAIN);
    }
    return (gain / S_OMAHA_GetBaseVolume()) * 100;
}

/*
==============
omaha_channel::sample_offset
==============
*/
U32 omaha_channel::sample_offset()
{
    if (voiceId == SND_VOICE_NONE) {
        return 0;
    }
    return g_sndRenderer->GetSampleOffset(voiceId);
}

/*
==============
omaha_channel::sample_ms_offset
==============
*/
U32 omaha_channel::sample_ms_offset()
{
    if (!is_playing()) {
        return 0;
    }

    return sample_offset() * 1000ull / sample_playback_rate();
}

/*
==============
omaha_channel::sample_loop_count
==============
*/
U32 omaha_channel::sample_loop_count()
{
    S32 left;

    if (voiceId == SND_VOICE_NONE) {
        return 0;
    }

    left = g_sndRenderer->StreamBuffersQueued(voiceId) - g_sndRenderer->StreamBuffersProcessed(voiceId) - 1;
    return left < 0 ? 0 : left;
}

/*
==============
omaha_channel::set_sample_offset
==============
*/
void omaha_channel::set_sample_offset(U32 offset)
{
    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    g_sndRenderer->SetSampleOffset(voiceId, offset);
}

/*
==============
omaha_channel::set_sample_ms_offset
==============
*/
void omaha_channel::set_sample_ms_offset(U32 offset)
{
    set_sample_offset(offset * sample_playback_rate() / 1000);
}

/*
==============
omaha_channel::set_sample_loop_count
==============
*/
void omaha_channel::set_sample_loop_count(S32 count)
{
    int processed;

    stop();

    if (voiceId == SND_VOICE_NONE) {
        return;
    }

    /* Changed in Omaha: unqueue processed stream buffers plus the looping flag. */
    processed = g_sndRenderer->StreamBuffersProcessed(voiceId);
    if (processed) {
        g_sndRenderer->UnqueueProcessed(voiceId, processed);
    }
    g_sndRenderer->SetParami(voiceId, SND_PARAM_LOOPING, count == 0);
}

/*
==============
omaha_channel::set_sample_loop_block
==============
*/
void omaha_channel::set_sample_loop_block(S32 start_offset, S32 end_offset)
{
    /* Added in Omaha: honor Miles/soundinfo loopstart/loopend (sample frames). */
    loopBlockStart = start_offset > 0 ? (int)start_offset : 0;
    loopBlockEnd   = (int)end_offset;

    if (voiceId == SND_VOICE_NONE) {
        return;
    }
    g_sndRenderer->SetLoopPoints(voiceId, loopBlockStart, loopBlockEnd);
}

/*
==============
omaha_channel::sample_status
==============
*/
U32 omaha_channel::sample_status()
{
    if (voiceId == SND_VOICE_NONE) {
        return SND_STATE_INITIAL;
    }
    return (U32)g_sndRenderer->GetState(voiceId);
}

/*
==============
S_OMAHA_MUSIC_LoadSoundtrackFile
==============
*/
qboolean S_OMAHA_MUSIC_LoadSoundtrackFile(const char *filename)
{
    song_t *psong = NULL;
    char    args[MAX_MUSIC_SONGS][MAX_RES_NAME];
    int     numargs;
    char    com_token[MAX_STRING_CHARS];
    char    alias[128];
    char    file[128];
    char    load_path[128];
    char   *buffer;
    char    path[MAX_RES_NAME];
    int     i;
    byte   *data;

    if (strrchr(filename, '.')) {
        Com_sprintf(path, sizeof(path), "%s", filename);
    } else {
        Com_sprintf(path, sizeof(path), "%s.mus", filename);
    }

    music_numsongs = 0;

    FS_ReadFile(path, (void **)&data);
    if (!data) {
        Com_DPrintf("Omaha: Couldn't load %s\n", path);
        return false;
    }
    Com_DPrintf("SOUNDTRACK: Loading %s\n", path);

    S_OMAHA_MUSIC_StopAllSongs();

    buffer       = (char *)data;
    load_path[0] = 0;

    while (1) {
        Q_strncpyz(com_token, COM_GetToken(&buffer, true), sizeof(com_token));
        if (!com_token[0]) {
            break;
        }

        if (strlen(com_token) >= MAX_RES_NAME) {
            Com_Printf("S_OMAHA_MUSIC_LoadSoundtrackFile: argument too long, truncating in %s\n", path);
            com_token[MAX_RES_NAME - 1] = 0;
        }

        numargs = 1;
        Q_strncpyz(args[0], com_token, sizeof(args[0]));

        while (1) {
            Q_strncpyz(com_token, COM_GetToken(&buffer, false), sizeof(com_token));
            if (!com_token[0]) {
                break;
            }

            if (strlen(com_token) >= MAX_RES_NAME) {
                Com_Printf("S_OMAHA_MUSIC_LoadSoundtrackFile: argument too long, truncating in %s\n", path);
                com_token[MAX_RES_NAME - 1] = 0;
            }

            Q_strncpyz(args[numargs], com_token, sizeof(args[numargs]));
            numargs++;
        }

        if (!Q_stricmp(args[0], "path")) {
            Q_strncpyz(load_path, args[1], sizeof(load_path));
            if (load_path[strlen(load_path) - 1] != '/' && load_path[strlen(load_path) - 1] != '\\') {
                Q_strcat(load_path, sizeof(load_path), "/");
            }
        } else if (args[0][0] == '!') {
            for (i = 0; i < music_numsongs; i++) {
                psong = &music_songs[i];
                if (!Q_stricmp(psong->alias, &args[0][1])) {
                    break;
                }
            }

            if (i == music_numsongs) {
                Com_Printf("S_OMAHA_MUSIC_LoadSoundtrackFile: song %s not found, command skipped in %s.\n", &args[0][1], path);
                continue;
            }

            if (!Q_stricmp(args[1], "volume")) {
                psong->volume = atof(args[2]);
            } else if (!Q_stricmp(args[1], "fadetime")) {
                psong->fadetime = atof(args[2]);
            } else if (!Q_stricmp(args[1], "loop")) {
                psong->flags |= 1;
            } else if (!Q_stricmp(args[1], "restart")) {
                psong->flags |= 2;
            } else if (!Q_stricmp(args[1], "interrupt")) {
                psong->fadetime = 0;
                psong->flags |= 4 | 2;
            } else {
                Com_Printf(
                    "S_OMAHA_MUSIC_LoadSoundtrackFile: unknown command %s for song %s in %s.\n", args[1], &args[0][1], path
                );
            }
        } else {
            if (numargs > 1) {
                Q_strncpyz(alias, args[0], sizeof(alias));
                Q_strncpyz(file, load_path, sizeof(file));
                Q_strcat(file, sizeof(file), args[1]);
            } else {
                size_t len;

                Q_strncpyz(file, load_path, sizeof(file));
                Q_strcat(file, sizeof(file), args[1]);

                len = strlen(args[0]);
                if (len >= 4) {
                    Q_strncpyz(alias, args[0], len - 4);
                } else {
                    alias[0] = 0;
                }
            }

            if (music_numsongs >= MAX_MUSIC_SONGS) {
                Com_Printf("S_OMAHA_MUSIC_LoadSoundtrackFile: Too many songs in %s, skipping %s.\n", path, alias);
                continue;
            }

            psong = &music_songs[music_numsongs];
            Q_strncpyz(psong->alias, alias, sizeof(psong->alias));
            Q_strncpyz(psong->path, file, sizeof(psong->path));
            music_songs[music_numsongs].fadetime    = 1.0;
            music_songs[music_numsongs].volume      = 1.0;
            music_songs[music_numsongs].flags       = 0;
            music_songs[music_numsongs].current_pos = 0;
            music_songs[music_numsongs].mood_num    = MusicMood_NameToNum(alias);
            music_numsongs++;
        }
    }

    if (!music_numsongs) {
        Com_Printf("S_OMAHA_MUSIC_LoadSoundtrackFile: could not load %s, no songs defined.\n", path);
        FS_FreeFile(data);
        return false;
    }

    music_currentsong = -1;
    FS_FreeFile(data);

    if (music_current_mood == mood_none) {
        S_OMAHA_MUSIC_UpdateMood(mood_normal, mood_normal);
    }

    return true;
}

/*
==============
S_OMAHA_MUSIC_SongValid
==============
*/
qboolean S_OMAHA_MUSIC_SongValid(const char *mood)
{
    return S_OMAHA_MUSIC_FindSong(mood) != -1;
}

/*
==============
S_OMAHA_MUSIC_Loaded
==============
*/
qboolean S_OMAHA_MUSIC_Loaded()
{
    return music_loaded;
}

/*
==============
S_OMAHA_Music_Update
==============
*/
void S_OMAHA_Music_Update()
{
    int currentsong;

    if (!s_bSoundStarted) {
        return;
    }

    if (!music_active) {
        return;
    }

    if (s_bSoundPaused) {
        return;
    }

    S_OMAHA_MUSIC_CheckForStoppedSongs();

    if (!S_OMAHA_MUSIC_Loaded() && music_active && strlen(current_soundtrack)) {
        S_OMAHA_MUSIC_LoadSoundtrackFile(current_soundtrack);
        music_loaded = true;
    }

    if (music_currentsong >= 0) {
        currentsong = music_songs[music_currentsong].mood_num;
    } else {
        currentsong = -1;
    }

    if (music_current_mood == mood_none) {
        if (S_OMAHA_MUSIC_Playing()) {
            S_OMAHA_MUSIC_StopAllSongs();
        }
    } else if (music_current_mood != currentsong) {
        const char *mood = MusicMood_NumToName(music_current_mood);
        if (S_OMAHA_MUSIC_SongValid(mood) && S_OMAHA_MUSIC_Loaded() && strlen(current_soundtrack)) {
            Com_DebugPrintf("Playing %s.\n", mood);
            S_OMAHA_MUSIC_PlaySong(mood);
        }
    }

    if (new_music_volume != music_volume) {
        if (music_volume_fade_time > 0) {
            if (music_volume_direction == 0) {
                music_volume = (cls.realtime - music_volume_start_time) * (new_music_volume - old_music_volume)
                                 / (music_volume_fade_time * 1000.f)
                             + old_music_volume;

                if (music_volume > new_music_volume) {
                    music_volume         = new_music_volume;
                    music_volume_changed = 1;
                } else {
                    music_volume_changed = 1;
                }
            } else if (music_volume_direction == 1) {
                music_volume = 1.0
                             - (cls.realtime - music_volume_start_time) * (old_music_volume - new_music_volume)
                                   / (music_volume_fade_time * 1000.f);

                if (music_volume >= new_music_volume) {
                    music_volume_changed = true;
                } else {
                    music_volume         = new_music_volume;
                    music_volume_changed = true;
                }
            } else {
                music_volume_changed = true;
            }
        } else {
            music_volume         = new_music_volume;
            music_volume_changed = true;
        }
    }

    S_OMAHA_MUSIC_UpdateMusicVolumes();
}

/*
==============
S_OMAHA_MUSIC_SongEnded
==============
*/
void S_OMAHA_MUSIC_SongEnded()
{
    Com_DPrintf(
        "MUSIC: Song ended, changing from [ %s ] to [ %s ]\n",
        MusicMood_NumToName(music_current_mood),
        MusicMood_NumToName(music_fallback_mood)
    );
    music_current_mood = music_fallback_mood;
}

/*
==============
S_OMAHA_MUSIC_NewSoundtrack
==============
*/
void S_OMAHA_MUSIC_NewSoundtrack(const char *name)
{
    if (!s_bSoundStarted) {
        return;
    }

    if (!Q_stricmp(name, current_soundtrack)) {
        return;
    }

    // Fixed in OPM
    //  Make sure to not get past the end of the buffer
    //strcpy(current_soundtrack, name);
    Q_strncpyz(current_soundtrack, name, sizeof(current_soundtrack));

    if (!Q_stricmp(current_soundtrack, "none") || !Q_stricmp(current_soundtrack, "")) {
        music_active = qfalse;
        if (S_OMAHA_MUSIC_Playing()) {
            S_OMAHA_MUSIC_StopAllSongs();
        }
    } else {
        music_active = qtrue;
        S_OMAHA_MUSIC_LoadSoundtrackFile(name);
        music_loaded = qtrue;
    }
}

/*
==============
S_OMAHA_MUSIC_UpdateMood
==============
*/
void S_OMAHA_MUSIC_UpdateMood(int current, int fallback)
{
    static int last_current_mood  = -1;
    static int last_fallback_mood = -1;
    static int current_mood       = -1;
    static int fallback_mood      = -1;
    qboolean   was_action;

    if (current == current_mood && fallback == fallback_mood) {
        return;
    }

    was_action          = current == last_current_mood && fallback == last_fallback_mood && current_mood == mood_action;
    last_current_mood   = current_mood;
    last_fallback_mood  = fallback_mood;
    current_mood        = current;
    music_current_mood  = current;
    fallback_mood       = fallback;
    music_fallback_mood = fallback;
    music_active        = qtrue;

    Com_DPrintf(
        "MUSIC: changing from [ %s | %s ] to [ %s | %s ]\n",
        MusicMood_NumToName(last_current_mood),
        MusicMood_NumToName(last_fallback_mood),
        MusicMood_NumToName(current_mood),
        MusicMood_NumToName(fallback)
    );

    if (was_action) {
        int songnum = S_OMAHA_MUSIC_FindSong(MusicMood_NumToName(current_mood));

        if (songnum != -1 && (music_songs[songnum].flags & 4)) {
            Com_DPrintf(
                "MUSIC: restoring music from action state, skipping [ %s ] for [ %s ]\n",
                MusicMood_NumToName(current_mood),
                MusicMood_NumToName(fallback_mood)
            );
            music_current_mood = music_fallback_mood;
        }
    }
}

/*
==============
S_OMAHA_MUSIC_UpdateVolume
==============
*/
void S_OMAHA_MUSIC_UpdateVolume(float volume, float fade_time)
{
    if (new_music_volume == volume && music_volume_fade_time == fade_time) {
        return;
    }

    old_music_volume        = music_volume;
    new_music_volume        = volume;
    music_volume_fade_time  = fade_time;
    music_volume_start_time = cls.realtime;

    if (volume > music_volume) {
        music_volume_direction = 0;
    } else if (volume < music_volume) {
        music_volume_direction = 1;
    }
}

/*
==============
S_OMAHA_MUSIC_StopAllSongs
==============
*/
void S_OMAHA_MUSIC_StopAllSongs()
{
    for (int i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        S_OMAHA_MUSIC_StopChannel(i);
    }

    music_currentsong = -1;
}

/*
==============
S_OMAHA_MUSIC_FreeAllSongs
==============
*/
void S_OMAHA_MUSIC_FreeAllSongs()
{
    S_OMAHA_MUSIC_StopAllSongs();
    S_OMAHA_MUSIC_UpdateMood(mood_none, mood_none);
    current_soundtrack[0] = 0;
    music_loaded          = false;

    // Added in OPM
    //  Clear the list of songs so the songs from previous maps
    //  won't get played
    music_numsongs = 0;
}

/*
==============
S_OMAHA_MUSIC_Playing
==============
*/
qboolean S_OMAHA_MUSIC_Playing()
{
    return S_OMAHA_MUSIC_CurrentSongChannel() != -1;
}

/*
==============
S_OMAHA_MUSIC_FindSong
==============
*/
int S_OMAHA_MUSIC_FindSong(const char *name)
{
    int i;

    for (i = 0; i < music_numsongs; i++) {
        if (!Q_stricmp(music_songs[i].alias, name)) {
            return i;
        }
    }

    return -1;
}

/*
==============
S_OMAHA_loadsoundtrack
==============
*/
void S_OMAHA_loadsoundtrack()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("loadsoundtrack <sound track file>\n");
        return;
    }

    S_OMAHA_MUSIC_LoadSoundtrackFile(Cmd_Argv(1));
    music_loaded = true;
    Q_strncpyz(current_soundtrack, Cmd_Argv(1), sizeof(current_soundtrack));
}

/*
==============
S_OMAHA_CurrentSoundtrack
==============
*/
const char *S_OMAHA_CurrentSoundtrack()
{
    return current_soundtrack;
}

/*
==============
S_OMAHA_PlaySong
==============
*/
void S_OMAHA_PlaySong()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("playsong <song alias>\n");
        return;
    }

    S_OMAHA_MUSIC_PlaySong(Cmd_Argv(1));
    music_active = true;
}

/*
==============
S_OMAHA_MUSIC_CurrentSongChannel
==============
*/
int S_OMAHA_MUSIC_CurrentSongChannel()
{
    int channel_number = -1;
    int ch_idx         = 0;

    for (ch_idx = 0; ch_idx < MAX_SOUNDSYSTEM_SONGS; ch_idx++) {
        if (omaha.chan_song[ch_idx].is_playing() && omaha.chan_song[ch_idx].song_number == music_currentsong) {
            channel_number = ch_idx;
        }
    }

    return channel_number;
}

/*
==============
S_OMAHA_MUSIC_StopChannel
==============
*/
void S_OMAHA_MUSIC_StopChannel(int channel_number)
{
    omaha_channel *channel = &omaha.chan_song[channel_number];

    channel->pause();
    if (music_songs[channel->song_number].flags & 2) {
        music_songs[channel->song_number].current_pos = 0;
    } else {
        music_songs[channel->song_number].current_pos = channel->sample_ms_offset();
    }

    channel->stop();
}

/*
==============
S_OMAHA_MUSIC_PlaySong
==============
*/
qboolean S_OMAHA_MUSIC_PlaySong(const char *alias)
{
    int                          channel_number;
    song_t                      *song;
    int                          songnum;
    int                          channel_to_play_on;
    int                          fading_song;
    omaha_channel_two_d_stream *song_channel;
    unsigned int                 loop_start;
    int                          rate;

    fading_song = 0;
    songnum     = S_OMAHA_MUSIC_FindSong(alias);

    if (songnum == -1) {
        return true;
    }

    song = &music_songs[songnum];

    if (S_OMAHA_MUSIC_Playing() && songnum == music_currentsong) {
        return true;
    }

    channel_number    = S_OMAHA_MUSIC_CurrentSongChannel();
    music_currentsong = songnum;

    if (channel_number != -1) {
        if (song->flags & 4) {
            S_OMAHA_MUSIC_StopChannel(channel_number);
        } else {
            song_channel                  = &omaha.chan_song[channel_number];
            song_channel->fading          = FADE_OUT;
            song_channel->fade_time       = (int)song->fadetime;
            song_channel->fade_start_time = cls.realtime;
            fading_song                   = true;
        }
    }
    channel_to_play_on = (fading_song != 0) && (channel_number == 0);
    song_channel       = &omaha.chan_song[channel_to_play_on];

    if (song_channel->is_playing() || song_channel->is_paused()) {
        S_OMAHA_MUSIC_StopChannel(channel_to_play_on);
    }

    if (!song_channel->queue_stream(song->path)) {
        Com_DPrintf("Could not start music file '%s'!", song->path);
        return false;
    }

    rate = song_channel->sample_playback_rate();

    song_channel->song_number = songnum;
    if (song->current_pos) {
        song_channel->set_sample_ms_offset(song->current_pos);
    } else {
        song_channel->set_sample_offset(rate * 0.063f);
    }

    if (song->flags & 1) {
        song_channel->set_sample_loop_count(0);
        song_channel->set_sample_loop_block(rate * 0.063f, -1);
    } else {
        song_channel->set_sample_loop_count(1);
    }

    if (fading_song) {
        song_channel->fading    = FADE_IN;
        song_channel->fade_time = (int)song->fadetime;
        song_channel->set_gain(0.0);
        song_channel->fade_start_time = cls.realtime;
    } else {
        song_channel->fading = FADE_NONE;
        song_channel->set_gain(S_OMAHA_GetBaseVolume() * (song->volume * s_ambientVolume->value) * s_fVolumeGain);
    }

    song_channel->play();

    return true;
}

/*
==============
S_OMAHA_MUSIC_UpdateMusicVolumes
==============
*/
void S_OMAHA_MUSIC_UpdateMusicVolumes()
{
    int          i;
    unsigned int current_time;
    float        new_volume, max_volume;

    if (s_ambientVolume->modified || music_volume_changed) {
        s_ambientVolume->modified = false;

        for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
            if (!omaha.chan_song[i].is_playing() && !omaha.chan_song[i].is_paused()) {
                continue;
            }

            if (omaha.chan_song[i].fading != FADE_NONE) {
                continue;
            }

            omaha.chan_song[i].set_gain(
                S_OMAHA_GetBaseVolume() * (music_songs[omaha.chan_song[i].song_number].volume * s_ambientVolume->value)
                * s_fVolumeGain * music_volume
            );
        }
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        if (!omaha.chan_song[i].is_playing() && !omaha.chan_song[i].is_paused()) {
            continue;
        }

        switch (omaha.chan_song[i].fading) {
        case fade_t::FADE_IN:
            max_volume = music_songs[omaha.chan_song[i].song_number].volume * s_ambientVolume->value;
            new_volume = (unsigned int)(cls.realtime - omaha.chan_song[i].fade_start_time)
                       / (omaha.chan_song[i].fade_time * 1000.f) * max_volume;

            if (new_volume > max_volume) {
                omaha.chan_song[i].set_gain(S_OMAHA_GetBaseVolume() * (max_volume * s_fVolumeGain * music_volume));
                omaha.chan_song[i].fading = FADE_NONE;
            } else {
                omaha.chan_song[i].set_gain(S_OMAHA_GetBaseVolume() * (new_volume * s_fVolumeGain * music_volume));
            }
            break;
        case fade_t::FADE_OUT:
            max_volume = music_songs[omaha.chan_song[i].song_number].volume * s_ambientVolume->value;
            new_volume = (unsigned int)(cls.realtime - omaha.chan_song[i].fade_start_time)
                       / (omaha.chan_song[i].fade_time * 1000.f) * max_volume;

            if (new_volume > 0) {
                omaha.chan_song[i].set_gain(S_OMAHA_GetBaseVolume() * (new_volume * s_fVolumeGain * music_volume));
            } else {
                S_OMAHA_MUSIC_StopChannel(i);
            }
            break;
        default:
            break;
        }
    }

    if (s_musicVolume->modified || music_volume_changed) {
        s_musicVolume->modified = false;
        if (omaha.chan_trig_music.is_playing() || omaha.chan_trig_music.is_paused()) {
            omaha.chan_trig_music.set_gain(
                S_OMAHA_GetBaseVolume() * (omaha.chan_trig_music.fVolume * s_musicVolume->value) * s_fVolumeGain
            );
        }
    }

    music_volume_changed = false;
}

/*
==============
S_OMAHA_MUSIC_CheckForStoppedSongs
==============
*/
void S_OMAHA_MUSIC_CheckForStoppedSongs()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        if (!omaha.chan_song[i].is_playing()) {
            continue;
        }

        if (omaha.chan_song[i].sample_loop_count()) {
            continue;
        }

        S_OMAHA_MUSIC_FindSong(MusicMood_NumToName(music_fallback_mood));
        if (!omaha.chan_song[i].is_playing() && !omaha.chan_song[i].is_paused()) {
            S_OMAHA_MUSIC_SongEnded();
        }
    }
}

/*
==============
S_OMAHA_TriggeredMusic_SetupHandle
==============
*/
void S_OMAHA_TriggeredMusic_SetupHandle(const char *pszName, int iLoopCount, int iOffset, qboolean autostart)
{
    char            *pszFilename;
    const char      *pszRealName;
    float            fVolume     = 1.0;
    AliasListNode_t *pSoundAlias = NULL;
    void            *stream      = NULL;

    if (!s_bSoundStarted) {
        return;
    }

    if (omaha.chan_trig_music.is_playing() || omaha.chan_trig_music.is_paused()) {
        Com_DPrintf("Didn't start new triggered music because some was already playing\n");
        return;
    }

    omaha.chan_trig_music.stop();
    // Fixed in OPM
    //  Use strncpy instead
    //strcpy(omaha.tm_filename, pszName);
    Q_strncpyz(omaha.tm_filename, pszName, sizeof(omaha.tm_filename));

    omaha.tm_loopcount = iLoopCount;
    omaha.chan_trig_music.set_sample_loop_count(iLoopCount);
    pszRealName = Alias_FindRandom(pszName, &pSoundAlias);

    if (!pszRealName) {
        pszRealName = pszName;
    } else if (pSoundAlias) {
        fVolume = random() * pSoundAlias->volumeMod + pSoundAlias->volume;
    }

    pszFilename = FS_BuildOSPath(Cvar_VariableString("fs_basepath"), FS_Gamedir(), pszRealName);

    if (!omaha.chan_trig_music.queue_stream(pszRealName)) {
        /* Fixed in Omaha: release the old renderer voice before re-creating the channel. */
        if (g_sndRenderer && omaha.chan_trig_music.voiceId != SND_VOICE_NONE) {
            g_sndRenderer->DestroyVoice(omaha.chan_trig_music.voiceId);
            omaha.chan_trig_music.voiceId = SND_VOICE_NONE;
        }
        S_OMAHA_InitChannel(SOUNDSYSTEM_CHANNEL_TRIGGER_MUSIC_ID, &omaha.chan_trig_music);
        Com_DPrintf("Could not start triggered music '%s'\n", pszName);
        return;
    }

    omaha.chan_trig_music.fVolume = fVolume;
    omaha.chan_trig_music.set_gain(S_OMAHA_GetBaseVolume() * (fVolume * s_musicVolume->value) * s_fVolumeGain);
    omaha.chan_trig_music.set_sample_loop_count(iLoopCount);
    omaha.chan_trig_music.set_sample_offset(iOffset);

    // Fixed in OPM
    //  Play the sound then pause it immediately
    //  So it can be unpaused upon loading from save
    omaha.chan_trig_music.play();

    if (!autostart) {
        omaha.chan_trig_music.pause();
    }
}

/*
==============
S_OMAHA_TriggeredMusic_Start
==============
*/
void S_OMAHA_TriggeredMusic_Start()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("tmstart <sound file>\n");
        return;
    }

    S_OMAHA_TriggeredMusic_SetupHandle(Cmd_Argv(1), 1, 0, true);
}

/*
==============
S_OMAHA_TriggeredMusic_StartLoop
==============
*/
void S_OMAHA_TriggeredMusic_StartLoop()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("tmstartloop <sound file>\n");
        return;
    }

    S_OMAHA_TriggeredMusic_SetupHandle(Cmd_Argv(1), 0, 0, true);
}

/*
==============
S_OMAHA_TriggeredMusic_Stop
==============
*/
void S_OMAHA_TriggeredMusic_Stop()
{
    if (!s_bSoundStarted) {
        return;
    }

    omaha.chan_trig_music.stop();
    // Fixed in OPM
    //  Make sure to clear the triggered music in case snd_restart is called
    omaha.tm_filename[0] = 0;
    omaha.tm_loopcount   = 0;
}

/*
==============
S_OMAHA_TriggeredMusic_Volume

Added in 2.0
==============
*/
void S_OMAHA_TriggeredMusic_Volume()
{
    if (!s_bSoundStarted) {
        return;
    }

    if (Cmd_Argc() != 2) {
        Com_Printf("tmvolume <0-1>\n\n");
        return;
    }

    omaha.chan_trig_music.fVolume = atof(Cmd_Argv(1));
    music_volume_changed           = true;
}

/*
==============
S_OMAHA_TriggeredMusic_Pause
==============
*/
void S_OMAHA_TriggeredMusic_Pause()
{
    if (!s_bSoundStarted) {
        return;
    }

    if (omaha.chan_trig_music.is_playing()) {
        omaha.chan_trig_music.pause();
    }
}

/*
==============
S_OMAHA_TriggeredMusic_Unpause
==============
*/
void S_OMAHA_TriggeredMusic_Unpause()
{
    if (!s_bSoundStarted) {
        return;
    }

    if (omaha.chan_trig_music.is_paused()) {
        omaha.chan_trig_music.play();
    }

    omaha.chan_trig_music.set_gain(
        S_OMAHA_GetBaseVolume() * (omaha.chan_trig_music.fVolume * s_musicVolume->value) * s_fVolumeGain
    );
}

/*
==============
S_OMAHA_TriggeredMusic_PlayIntroMusic
==============
*/
void S_OMAHA_TriggeredMusic_PlayIntroMusic()
{
    S_OMAHA_TriggeredMusic_SetupHandle("sound/music/mus_MainTheme.mp3", 0, 0, true);
}

/*
==============
S_OMAHA_StopMovieAudio
==============
*/
void S_OMAHA_StopMovieAudio()
{
    omaha.chan_movie.stop();
}

/*
==============
S_OMAHA_SetupMovieAudio
==============
*/
void S_OMAHA_SetupMovieAudio(const char *pszMovieName)
{
    char filename[MAX_QPATH];

    S_OMAHA_StopMovieAudio();

    COM_StripExtension(pszMovieName, filename, sizeof(filename));
    Q_strcat(filename, sizeof(filename), ".mp3");

    if (!omaha.chan_movie.queue_stream(filename)) {
        COM_StripExtension(pszMovieName, filename, sizeof(filename));
        Q_strcat(filename, sizeof(filename), ".wav");

        if (!omaha.chan_movie.queue_stream(filename)) {
            Com_DPrintf("Can't find any matching audio file for movie: %s\n", pszMovieName);
            return;
        }
    }

    omaha.chan_movie.set_gain(S_OMAHA_GetBaseVolume() * 1.5f);
    omaha.chan_movie.play();

    Com_DPrintf("Movie Audio setup: %s\n", filename);
}

/*
==============
S_OMAHA_CurrentMoviePosition
==============
*/
int S_OMAHA_CurrentMoviePosition()
{
    return omaha.chan_movie.sample_ms_offset();
}

/*
==============
omaha_channel::update
==============
*/
void omaha_channel::update() {}

/*
==============
S_SndFmtFromWidthChannels
Added in Omaha: map codec width/channels to renderer sample format.
==============
*/
static sndSampleFormat_t S_SndFmtFromWidthChannels(float width, int channels)
{
    if (width == 0.5f) {
        return channels <= 1 ? SND_FMT_MONO_IMA4 : SND_FMT_STEREO_IMA4;
    }
    if (channels <= 1) {
        return (width <= 1) ? SND_FMT_MONO8 : SND_FMT_MONO16;
    }
    return (width <= 1) ? SND_FMT_STEREO8 : SND_FMT_STEREO16;
}

/*
==============
S_StreamEnsureVoice
Added in Omaha: stream channels need a renderer voice before queueing PCM.
==============
*/
static qboolean S_StreamEnsureVoice(omaha_channel *chan)
{
    if (chan->voiceId == SND_VOICE_NONE) {
        chan->voiceId = g_sndRenderer->CreateVoice();
        if (chan->voiceId == SND_VOICE_NONE) {
            return qfalse;
        }
        g_sndRenderer->SetParami(chan->voiceId, SND_PARAM_SOURCE_RELATIVE, 1);
        g_sndRenderer->SetParamf(chan->voiceId, SND_PARAM_ROLLOFF_FACTOR, 0);
    }
    return qtrue;
}

/*
==============
omaha_channel_two_d_stream::queueChunk
Added in Omaha: queue one decoded chunk on the renderer voice and remember its size.
==============
*/
void omaha_channel_two_d_stream::queueChunk(const void *data, unsigned int size, const void *handle)
{
    const snd_stream_t *stream = (const snd_stream_t *)handle;

    g_sndRenderer->QueueStreamBuffer(
        voiceId,
        data,
        (int)size,
        S_SndFmtFromWidthChannels(stream->info.width, stream->info.channels),
        stream->info.rate,
        stream->info.dataalign
    );
    bufferSizes[currentBuf] = size;
    lastBitsPerSample       = (unsigned int)(stream->info.width * 8 * stream->info.channels);
    currentBuf              = (currentBuf + 1) % MAX_STREAM_BUFFERS;
}

/*
==============
omaha_channel_two_d_stream::attachAsync
Added in Omaha: hand the open stream to the decode-ahead worker (renderer path only).
==============
*/
void omaha_channel_two_d_stream::attachAsync()
{
    snd_stream_t *stream = (snd_stream_t *)streamHandle;
    unsigned int  chunkBytes;

    if (asyncJob || !stream || !g_sndRenderer || voiceId == SND_VOICE_NONE) {
        return;
    }

    chunkBytes = (unsigned int)Q_min(MAX_BUFFER_SAMPLES * stream->info.width * stream->info.channels, MAX_BUFFER_SAMPLES * 2 * 2);
    asyncJob   = S_AsyncStream_Attach(stream, chunkBytes);
}

/*
==============
omaha_channel_two_d_stream::detachAsync
Added in Omaha: take streamHandle back from the worker. Returns decoded bytes that
were never queued (the codec position is that far past streamNextOffset).
==============
*/
unsigned int omaha_channel_two_d_stream::detachAsync()
{
    unsigned int discarded;

    if (!asyncJob) {
        return 0;
    }

    discarded = S_AsyncStream_Detach(asyncJob);
    asyncJob  = NULL;
    return discarded;
}

/*
==============
omaha_channel_two_d_stream::stop
==============
*/
void omaha_channel_two_d_stream::stop()
{
    omaha_channel::stop();

    clear_stream();
}

/*
==============
omaha_channel_two_d_stream::set_sfx
==============
*/
bool omaha_channel_two_d_stream::set_sfx(sfx_t *pSfx)
{
    snd_stream_t     *stream;
    unsigned int      bytesToRead, bytesRead;
    char              rawData[MAX_BUFFER_SAMPLES * 2 * 2];

    stop();

    sampleLoopCount = 1;

    this->pSfx = pSfx;
    Q_strncpyz(this->fileName, pSfx->name, sizeof(this->fileName));

    streamHandle = S_CodecLoad(pSfx->name, NULL);
    if (!streamHandle) {
        Com_DPrintf("Sound: Failed to load sound file.\n");
        return false;
    }

    stream              = (snd_stream_t *)streamHandle;
    pSfx->info.channels = stream->info.channels;
    pSfx->info.dataofs  = stream->info.dataofs;
    pSfx->info.datasize = stream->info.size;
    pSfx->info.rate     = stream->info.rate;
    pSfx->info.samples  = stream->info.samples;
    pSfx->info.width    = stream->info.width;

    if (!S_StreamEnsureVoice(this)) {
        S_CodecCloseStream(stream);
        streamHandle = NULL;
        return false;
    }
    g_sndRenderer->Stop(voiceId);

    bytesToRead = Q_min(MAX_BUFFER_SAMPLES * stream->info.width * stream->info.channels, sizeof(rawData));
    /* Changed in Omaha: prefill several chunks at open so Update can drip-refill. */
    {
        int prefill;
        streamNextOffset = 0;
        for (prefill = 0; prefill < 16; prefill++) {
            bytesRead = S_CodecReadStream(stream, bytesToRead, rawData);
            if (!bytesRead) {
                break;
            }
            streamNextOffset += bytesRead;
            queueChunk(rawData, bytesRead, stream);
        }
        if (prefill == 0) {
            S_CodecCloseStream(stream);
            streamHandle = NULL;
            return true;
        }
    }
    iBaseRate = stream->info.rate;
    streaming = true;
    attachAsync();
    return true;
}

/*
==============
omaha_channel_two_d_stream::set_sample_loop_count
==============
*/
void omaha_channel_two_d_stream::set_sample_loop_count(S32 count)
{
    sampleLoopCount = count;
    sampleLooped    = 0;
}

/*
==============
omaha_channel_two_d_stream::omaha_channel_two_d_stream
==============
*/
omaha_channel_two_d_stream::omaha_channel_two_d_stream()
{
    int i;

    streamHandle = NULL;
    for (i = 0; i < MAX_STREAM_BUFFERS; i++) {
        bufferSizes[i] = 0;
    }
    lastBitsPerSample = 16;
    currentBuf       = 0;
    sampleLoopCount  = 0;
    sampleLooped     = 0;
    streamNextOffset = 0;
    streaming        = false;
    asyncJob         = NULL;
}

/*
==============
omaha_channel_two_d_stream::omaha_channel_two_d_stream
==============
*/
omaha_channel_two_d_stream::~omaha_channel_two_d_stream()
{
    clear_stream();
}

/*
==============
omaha_channel_two_d_stream::update
==============
*/
void omaha_channel_two_d_stream::update()
{
    snd_stream_t *stream = (snd_stream_t *)streamHandle;
    unsigned int  bytesToRead, bytesRead;
    int           numProcessedBuffers = 0, numQueuedBuffers = 0;
    // 2 channels with a width of 2
    char rawData[MAX_BUFFER_SAMPLES * 2 * 2];

    if (!streaming || is_paused() || voiceId == SND_VOICE_NONE) {
        return;
    }
    if (sample_status() == SND_STATE_INITIAL) {
        return;
    }

    {
        numProcessedBuffers = g_sndRenderer->StreamBuffersProcessed(voiceId);
        numQueuedBuffers    = g_sndRenderer->StreamBuffersQueued(voiceId);

        /*
         * Changed in Omaha: unqueue at most one buffer per tick. Mass-unqueue of
         * half the queue opened many free slots and caused a multi-frame decode burst.
         */
        if (numProcessedBuffers > 0) {
            g_sndRenderer->UnqueueProcessed(voiceId, 1);
            numQueuedBuffers = g_sndRenderer->StreamBuffersQueued(voiceId);
        }

        if (numQueuedBuffers >= MAX_STREAM_BUFFERS) {
            return;
        }

        if (sampleLoopCount && sampleLooped >= sampleLoopCount) {
            if (!is_playing()) {
                clear_stream();
            }
            return;
        }

        if (!stream) {
            return;
        }

        bytesToRead = Q_min(MAX_BUFFER_SAMPLES * stream->info.width * stream->info.channels, sizeof(rawData));
        /* Changed in Omaha: take a chunk the worker decoded ahead; decode inline only
         * when async is off. End of stream returns the handle for the loop logic. */
        if (asyncJob) {
            int popped = S_AsyncStream_Pop(asyncJob, rawData, sizeof(rawData), numQueuedBuffers <= 1 ? qtrue : qfalse);
            if (!popped) {
                return;
            }
            if (popped < 0) {
                detachAsync();
                bytesRead = 0;
            } else {
                bytesRead = (unsigned int)popped;
            }
        } else {
            bytesRead = S_CodecReadStream(stream, bytesToRead, rawData);
        }
        streamNextOffset += bytesRead;
        if (!bytesRead) {
            S_CodecCloseStream(stream);

            sampleLooped++;
            if (sampleLoopCount && sampleLooped >= sampleLoopCount) {
                streamHandle = NULL;
                return;
            }

            streamHandle = S_CodecLoad(this->fileName, NULL);
            if (!streamHandle) {
                clear_stream();
                return;
            }

            stream           = (snd_stream_t *)streamHandle;
            streamNextOffset = 0;
            /* Added in Omaha: restart loops at loopBlockStart (Miles sample_loop_block). */
            if (loopBlockStart > 0 && stream->info.width > 0 && stream->info.channels > 0) {
                unsigned int skip =
                    (unsigned int)loopBlockStart * stream->info.width * stream->info.channels;
                while (skip) {
                    unsigned int chunk = Q_min(skip, (unsigned int)sizeof(rawData));
                    unsigned int got   = S_CodecReadStream(stream, chunk, rawData);
                    if (!got) {
                        break;
                    }
                    skip -= got;
                    streamNextOffset += got;
                }
            }
            bytesRead = S_CodecReadStream(stream, bytesToRead, rawData);
            streamNextOffset += bytesRead;
            if (!bytesRead) {
                S_CodecCloseStream(stream);
                streamHandle = NULL;
                return;
            }
        }

        queueChunk(rawData, bytesRead, stream);
        attachAsync();

        if (!is_playing()) {
            play();
        }
    }
}

/*
==============
omaha_channel_two_d_stream::sample_offset
==============
*/
U32 omaha_channel_two_d_stream::sample_offset()
{
    snd_stream_t *stream              = (snd_stream_t *)streamHandle;
    unsigned int  playerByteOffset    = 0;
    unsigned int  totalQueueLength    = 0;
    unsigned int  bitsPerSample       = 0;
    unsigned int  offset              = 0;

    if (!streaming) {
        // no stream
        return 0;
    }

    totalQueueLength = getQueueLength();
    bitsPerSample    = getBitsPerSample();
    if (!bitsPerSample) {
        return 0;
    }

    // Get the byte offset in the queue
    /* Changed in Omaha: the renderer reports a sample offset; convert to bytes. */
    if (voiceId != SND_VOICE_NONE) {
        playerByteOffset =
            (unsigned int)((unsigned long long)g_sndRenderer->GetSampleOffset(voiceId) * bitsPerSample / 8);
    }

    assert(playerByteOffset < totalQueueLength);
    assert(streamNextOffset >= totalQueueLength || stream);

    if (streamNextOffset >= totalQueueLength) {
        offset = streamNextOffset - totalQueueLength + playerByteOffset;
    } else if (stream) {
        // it probably means the sound is looped and will restart from the beginning
        if (streamNextOffset + playerByteOffset <= totalQueueLength) {
            // near the end of the stream
            if (stream->info.size > totalQueueLength) {
                offset = stream->info.size + streamNextOffset - totalQueueLength + playerByteOffset;
            } else {
                // the buffer is really small
                offset = (streamNextOffset - totalQueueLength + playerByteOffset) % stream->info.size;
            }
        } else {
            // it is past end of stream
            // so, start from the beginning of the stream
            offset = streamNextOffset + playerByteOffset - totalQueueLength;
        }

        assert(offset < stream->info.size);
    } else {
        offset = totalQueueLength - streamNextOffset + playerByteOffset;
    }

    return offset * 8 / bitsPerSample;
}

/*
==============
omaha_channel_two_d_stream::buffer_frequency
==============
*/
U32 omaha_channel_two_d_stream::buffer_frequency() const
{
    return (U32)iBaseRate;
}

/*
==============
omaha_channel_two_d_stream::set_sample_offset
==============
*/
void omaha_channel_two_d_stream::set_sample_offset(U32 offset)
{
    snd_stream_t     *stream;
    unsigned int      bytesToRead, bytesRead;
    unsigned int      byteOffset;
    unsigned int      bitsPerSample;
    unsigned int      streamPosition;
    bool              bWasPlaying;
    char              rawData[MAX_BUFFER_SAMPLES * 2 * 2];

    if (!streaming || voiceId == SND_VOICE_NONE) {
        return;
    }

    stream         = (snd_stream_t *)streamHandle;
    streamPosition = getCurrentStreamPosition();
    bitsPerSample  = getBitsPerSample();
    byteOffset     = offset * bitsPerSample / 8;
    if (!bitsPerSample) {
        return;
    }

    if (byteOffset >= streamPosition && byteOffset < streamNextOffset) {
        //
        // Sample is in the currently streamed data
        //
        /* Changed in Omaha: the byte offset expressed as a sample offset for the renderer. */
        g_sndRenderer->SetSampleOffset(voiceId, (byteOffset - streamPosition) * 8 / bitsPerSample);
        return;
    }

    bWasPlaying = is_playing();

    /* Added in Omaha: decoded-ahead data moved the codec past streamNextOffset;
     * reopen so the forward read below starts from a known position. */
    if (detachAsync() && stream) {
        S_CodecCloseStream(stream);
        streamHandle = NULL;
        stream       = NULL;
    }

    if (stream) {
        if (byteOffset < streamPosition) {
            //
            // New offset is before the current offset,
            // reload the stream from the beginning
            //
            S_CodecCloseStream(stream);
            streamHandle = NULL;
        } else if (byteOffset >= stream->info.size) {
            //
            // This shouldn't happen unless the file is different
            // from the one used while saving
            //
            byteOffset %= stream->info.size;
            S_CodecCloseStream(stream);
            streamHandle = NULL;
        }
    }

    if (!streamHandle) {
        streamHandle = S_CodecLoad(this->fileName, NULL);
        if (!streamHandle) {
            clear_stream();
            return;
        }

        stream           = (snd_stream_t *)streamHandle;
        streamNextOffset = 0;
    }

    while (streamNextOffset <= byteOffset) {
        streamPosition = streamNextOffset;

        bytesToRead = Q_min(MAX_BUFFER_SAMPLES * stream->info.width * stream->info.channels, sizeof(rawData));
        bytesRead   = S_CodecReadStream(stream, bytesToRead, rawData);
        if (!bytesRead) {
            clear_stream();
            return;
        }

        streamNextOffset += bytesRead;
    }

    /* Changed in Omaha: stop, drop the queue, requeue the chunk through the renderer. */
    g_sndRenderer->Stop(voiceId);
    g_sndRenderer->UnqueueProcessed(voiceId, g_sndRenderer->StreamBuffersQueued(voiceId));
    queueChunk(rawData, bytesRead, stream);
    g_sndRenderer->SetSampleOffset(voiceId, (byteOffset - streamPosition) * 8 / bitsPerSample);
    attachAsync();
    if (bWasPlaying) {
        play();
    }
}

/*
==============
omaha_channel_two_d_stream::queue_stream
==============
*/
bool omaha_channel_two_d_stream::queue_stream(const char *fileName)
{
    snd_stream_t *stream;
    unsigned int  bytesToRead, bytesRead;
    char          rawData[MAX_BUFFER_SAMPLES * 2 * 2];

    // Store the filename so it can be looped later
    Q_strncpyz(this->fileName, fileName, sizeof(this->fileName));
    clear_stream();

    sampleLoopCount = 1;

    //
    // Load the file
    //
    streamHandle = S_CodecLoad(this->fileName, NULL);
    if (!streamHandle) {
        return false;
    }
    stream = (snd_stream_t *)streamHandle;

    iStartTime = cl.serverTime;
    iEndTime   = (int)(cl.serverTime + (stream->info.samples / stream->info.rate * 1000.f));
    iBaseRate  = stream->info.rate;

    if (!S_StreamEnsureVoice(this)) {
        S_CodecCloseStream(stream);
        streamHandle = NULL;
        return false;
    }

    // Read a smaller sample
    bytesToRead = Q_min(MAX_BUFFER_SAMPLES * stream->info.width * stream->info.channels, sizeof(rawData));
    bytesRead   = S_CodecReadStream(stream, bytesToRead, rawData);
    if (!bytesRead) {
        // Valid stream but no data?
        return true;
    }

    streamNextOffset = bytesRead;
    queueChunk(rawData, bytesRead, stream);
    streaming = true;
    attachAsync();
    return true;
}

/*
==============
omaha_channel_two_d_stream::clear_stream
==============
*/
void omaha_channel_two_d_stream::clear_stream()
{
    detachAsync(); // Added in Omaha

    if (!streaming) {
        return;
    }

    if (voiceId != SND_VOICE_NONE) {
        g_sndRenderer->Stop(voiceId);
        g_sndRenderer->ClearBuffer(voiceId);
    }

    if (streamHandle) {
        S_CodecCloseStream((snd_stream_t *)streamHandle);
    }

    fileName[0]  = 0;
    streamHandle = NULL;

    currentBuf       = 0;
    streamNextOffset = 0;
    streaming        = false;
    sampleLooped     = 0;
}

/*
==============
omaha_channel_two_d_stream::getQueueLength

Return the total length of all buffers being streamed.
==============
*/
unsigned int omaha_channel_two_d_stream::getQueueLength() const
{
    int          numQueuedBuffers;
    unsigned int totalQueueLength = 0;
    unsigned int bufferId;
    unsigned int i;

    if (voiceId == SND_VOICE_NONE) {
        return 0;
    }

    /* Changed in Omaha: renderer queues keep their chunk sizes in bufferSizes[]. */
    numQueuedBuffers = g_sndRenderer->StreamBuffersQueued(voiceId);
    for (i = 0; i < (unsigned int)numQueuedBuffers; i++) {
        bufferId = (currentBuf - i - 1) % MAX_STREAM_BUFFERS;
        totalQueueLength += bufferSizes[bufferId];
    }
    return totalQueueLength;
}

/*
==============
omaha_channel_two_d_stream::getCurrentStreamPosition

Return the current position in the sound being streamed.
==============
*/
unsigned int omaha_channel_two_d_stream::getCurrentStreamPosition() const
{
    unsigned int queueLength = getQueueLength();

    if (streamNextOffset >= queueLength) {
        return streamNextOffset - queueLength;
    } else if (streamHandle) {
        snd_stream_t *stream = (snd_stream_t *)streamHandle;
        return (stream->info.size + streamNextOffset - queueLength) % stream->info.size;
    } else {
        return queueLength - streamNextOffset;
    }
}

/*
==============
omaha_channel_two_d_stream::getBitsPerSample

Return the number of bytes per sample.
It assumes that all pending buffers have the same channels and bits.
==============
*/
unsigned int omaha_channel_two_d_stream::getBitsPerSample() const
{
    snd_stream_t *stream = (snd_stream_t *)streamHandle;

    if (stream) {
        return (unsigned int)(stream->info.width * 8 * stream->info.channels);
    }
    return lastBitsPerSample;
}

const sndHost_t s_sndHostOmaha = {
    "omaha",

    S_OMAHA_Init,
    S_OMAHA_Shutdown,
    S_OMAHA_StartSound,
    S_OMAHA_AddLoopingSound,
    S_OMAHA_ClearLoopingSounds,
    S_OMAHA_StopSound,
    S_OMAHA_StopAllSounds,
    S_OMAHA_Respatialize,
    S_OMAHA_SetReverb,
    S_OMAHA_Update,
    S_OMAHA_GetMusicFilename,
    S_OMAHA_GetMusicLoopCount,
    S_OMAHA_GetMusicOffset,

    S_OMAHA_MUSIC_NewSoundtrack,
    S_OMAHA_MUSIC_UpdateMood,
    S_OMAHA_MUSIC_UpdateVolume,
    S_OMAHA_CurrentSoundtrack,
    S_OMAHA_FadeSound,
    S_OMAHA_IsSoundPlaying,
    S_OMAHA_TriggeredMusic_SetupHandle,
    S_OMAHA_TriggeredMusic_PlayIntroMusic,
    S_OMAHA_TriggeredMusic_Stop,
    S_OMAHA_SetupMovieAudio,
    S_OMAHA_StopMovieAudio,
    S_OMAHA_CurrentMoviePosition,
    S_OMAHA_LoadData,
    S_OMAHA_SaveData,
    S_OMAHA_ReLoad,
    S_OMAHA_NeedFullRestart,
    S_OMAHA_PrintInfo,
    S_OMAHA_DumpInfo,

    S_OMAHA_CaptureBegin,
    S_OMAHA_CaptureEnd,
    S_OMAHA_CaptureFrame,
    S_OMAHA_CaptureActive,
    S_OMAHA_CaptureSampleRate,
};
