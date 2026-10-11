/*
===========================================================================
Copyright (C) 2005-2006 Tim Angus

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "client.h"
#include "cl_ui.h"
#include "cl_uimenu_dispatcher.h"
#include "cl_uivars.h"
#include "cl_videoenc.h"
#include "snd_local.h"

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#ifndef WAV_FORMAT_PCM
#define WAV_FORMAT_PCM 1
#endif

#define INDEX_FILE_EXTENSION ".index.dat"

#define MAX_RIFF_CHUNKS 16

typedef struct audioFormat_s
{
  int rate;
  int format;
  int channels;
  int bits;

  int sampleSize;
  int totalBytes;
} audioFormat_t;

typedef struct aviFileData_s
{
  qboolean      fileOpen;
  fileHandle_t  f;
  char          fileName[ MAX_QPATH ];
  int           fileSize;
  int           moviOffset;
  int           moviSize;

  fileHandle_t  idxF;
  int           numIndices;

  int           frameRate;
  int           framePeriod;
  int           width, height;
  int           numVideoFrames;
  int           maxRecordSize;
  qboolean      motionJpeg;

  qboolean      audio;
  audioFormat_t a;
  int           numAudioFrames;

  int           chunkStack[ MAX_RIFF_CHUNKS ];
  int           chunkStackTop;

  byte          *cBuffer, *eBuffer;

  /* Added in Omaha: H.264 MP4 mode for DEMOS-browser export. */
  qboolean      h264;
} aviFileData_t;

static aviFileData_t afd;

/* Added in Omaha: restore FPS/vsync caps after unattended demo export. */
static qboolean s_exportBoostActive;
static char     s_savedMaxFps[ MAX_CVAR_VALUE_STRING ];
static char     s_savedMaxFpsUnfocused[ MAX_CVAR_VALUE_STRING ];
static char     s_savedMaxFpsMinimized[ MAX_CVAR_VALUE_STRING ];
static int      s_savedSwapInterval;

#define MAX_AVI_BUFFER 2048

static byte buffer[ MAX_AVI_BUFFER ];
static int  bufIndex;

/*
===============
SafeFS_Write
===============
*/
static ID_INLINE void SafeFS_Write( const void *buf, int len, fileHandle_t f )
{
  if( FS_Write( buf, len, f ) < len )
    Com_Error( ERR_DROP, "Failed to write avi file\n" );
}

/*
===============
WRITE_STRING
===============
*/
static ID_INLINE void WRITE_STRING( const char *s )
{
  Com_Memcpy( &buffer[ bufIndex ], s, strlen( s ) );
  bufIndex += strlen( s );
}

/*
===============
WRITE_4BYTES
===============
*/
static ID_INLINE void WRITE_4BYTES( int x )
{
  buffer[ bufIndex + 0 ] = (byte)( ( x >>  0 ) & 0xFF );
  buffer[ bufIndex + 1 ] = (byte)( ( x >>  8 ) & 0xFF );
  buffer[ bufIndex + 2 ] = (byte)( ( x >> 16 ) & 0xFF );
  buffer[ bufIndex + 3 ] = (byte)( ( x >> 24 ) & 0xFF );
  bufIndex += 4;
}

/*
===============
WRITE_2BYTES
===============
*/
static ID_INLINE void WRITE_2BYTES( int x )
{
  buffer[ bufIndex + 0 ] = (byte)( ( x >>  0 ) & 0xFF );
  buffer[ bufIndex + 1 ] = (byte)( ( x >>  8 ) & 0xFF );
  bufIndex += 2;
}

/*
===============
WRITE_1BYTES
===============
*/
static ID_INLINE void WRITE_1BYTES( int x )
{
  buffer[ bufIndex ] = x;
  bufIndex += 1;
}

/*
===============
START_CHUNK
===============
*/
static ID_INLINE void START_CHUNK( const char *s )
{
  if( afd.chunkStackTop == MAX_RIFF_CHUNKS )
  {
    Com_Error( ERR_DROP, "ERROR: Top of chunkstack breached\n" );
  }

  afd.chunkStack[ afd.chunkStackTop ] = bufIndex;
  afd.chunkStackTop++;
  WRITE_STRING( s );
  WRITE_4BYTES( 0 );
}

/*
===============
END_CHUNK
===============
*/
static ID_INLINE void END_CHUNK( void )
{
  int endIndex = bufIndex;

  if( afd.chunkStackTop <= 0 )
  {
    Com_Error( ERR_DROP, "ERROR: Bottom of chunkstack breached\n" );
  }

  afd.chunkStackTop--;
  bufIndex = afd.chunkStack[ afd.chunkStackTop ];
  bufIndex += 4;
  WRITE_4BYTES( endIndex - bufIndex - 4 );
  bufIndex = endIndex;
  bufIndex = PAD( bufIndex, 2 );
}

/*
===============
CL_WriteAVIHeader
===============
*/
void CL_WriteAVIHeader( void )
{
  bufIndex = 0;
  afd.chunkStackTop = 0;

  START_CHUNK( "RIFF" );
  {
    WRITE_STRING( "AVI " );
    {
      START_CHUNK( "LIST" );
      {
        WRITE_STRING( "hdrl" );
        WRITE_STRING( "avih" );
        WRITE_4BYTES( 56 );                     //"avih" "chunk" size
        WRITE_4BYTES( afd.framePeriod );        //dwMicroSecPerFrame
        WRITE_4BYTES( afd.maxRecordSize *
            afd.frameRate );                    //dwMaxBytesPerSec
        WRITE_4BYTES( 0 );                      //dwReserved1
        WRITE_4BYTES( 0x110 );                  //dwFlags bits HAS_INDEX and IS_INTERLEAVED
        WRITE_4BYTES( afd.numVideoFrames );     //dwTotalFrames
        WRITE_4BYTES( 0 );                      //dwInitialFrame

        if( afd.audio )                         //dwStreams
          WRITE_4BYTES( 2 );
        else
          WRITE_4BYTES( 1 );

        WRITE_4BYTES( afd.maxRecordSize );      //dwSuggestedBufferSize
        WRITE_4BYTES( afd.width );              //dwWidth
        WRITE_4BYTES( afd.height );             //dwHeight
        WRITE_4BYTES( 0 );                      //dwReserved[ 0 ]
        WRITE_4BYTES( 0 );                      //dwReserved[ 1 ]
        WRITE_4BYTES( 0 );                      //dwReserved[ 2 ]
        WRITE_4BYTES( 0 );                      //dwReserved[ 3 ]

        START_CHUNK( "LIST" );
        {
          WRITE_STRING( "strl" );
          WRITE_STRING( "strh" );
          WRITE_4BYTES( 56 );                   //"strh" "chunk" size
          WRITE_STRING( "vids" );

          if( afd.motionJpeg )
            WRITE_STRING( "MJPG" );
          else
            WRITE_4BYTES( 0 );                  // BI_RGB

          WRITE_4BYTES( 0 );                    //dwFlags
          WRITE_4BYTES( 0 );                    //dwPriority
          WRITE_4BYTES( 0 );                    //dwInitialFrame

          WRITE_4BYTES( 1 );                    //dwTimescale
          WRITE_4BYTES( afd.frameRate );        //dwDataRate
          WRITE_4BYTES( 0 );                    //dwStartTime
          WRITE_4BYTES( afd.numVideoFrames );   //dwDataLength

          WRITE_4BYTES( afd.maxRecordSize );    //dwSuggestedBufferSize
          WRITE_4BYTES( -1 );                   //dwQuality
          WRITE_4BYTES( 0 );                    //dwSampleSize
          WRITE_2BYTES( 0 );                    //rcFrame
          WRITE_2BYTES( 0 );                    //rcFrame
          WRITE_2BYTES( afd.width );            //rcFrame
          WRITE_2BYTES( afd.height );           //rcFrame

          WRITE_STRING( "strf" );
          WRITE_4BYTES( 40 );                   //"strf" "chunk" size
          WRITE_4BYTES( 40 );                   //biSize
          WRITE_4BYTES( afd.width );            //biWidth
          WRITE_4BYTES( afd.height );           //biHeight
          WRITE_2BYTES( 1 );                    //biPlanes
          WRITE_2BYTES( 24 );                   //biBitCount

          if( afd.motionJpeg )                  //biCompression
          {
            WRITE_STRING( "MJPG" );
            WRITE_4BYTES( afd.width *
                afd.height );                   //biSizeImage
          }
          else
          {
            WRITE_4BYTES( 0 );                  // BI_RGB
            WRITE_4BYTES( afd.width *
                afd.height * 3 );               //biSizeImage
          }

          WRITE_4BYTES( 0 );                    //biXPelsPetMeter
          WRITE_4BYTES( 0 );                    //biYPelsPetMeter
          WRITE_4BYTES( 0 );                    //biClrUsed
          WRITE_4BYTES( 0 );                    //biClrImportant
        }
        END_CHUNK( );

        if( afd.audio )
        {
          START_CHUNK( "LIST" );
          {
            WRITE_STRING( "strl" );
            WRITE_STRING( "strh" );
            WRITE_4BYTES( 56 );                 //"strh" "chunk" size
            WRITE_STRING( "auds" );
            WRITE_4BYTES( 0 );                  //FCC
            WRITE_4BYTES( 0 );                  //dwFlags
            WRITE_4BYTES( 0 );                  //dwPriority
            WRITE_4BYTES( 0 );                  //dwInitialFrame

            WRITE_4BYTES( afd.a.sampleSize );   //dwTimescale
            WRITE_4BYTES( afd.a.sampleSize *
                afd.a.rate );                   //dwDataRate
            WRITE_4BYTES( 0 );                  //dwStartTime
            WRITE_4BYTES( afd.a.totalBytes /
                afd.a.sampleSize );             //dwDataLength

            WRITE_4BYTES( 0 );                  //dwSuggestedBufferSize
            WRITE_4BYTES( -1 );                 //dwQuality
            WRITE_4BYTES( afd.a.sampleSize );   //dwSampleSize
            WRITE_2BYTES( 0 );                  //rcFrame
            WRITE_2BYTES( 0 );                  //rcFrame
            WRITE_2BYTES( 0 );                  //rcFrame
            WRITE_2BYTES( 0 );                  //rcFrame

            WRITE_STRING( "strf" );
            WRITE_4BYTES( 18 );                 //"strf" "chunk" size
            WRITE_2BYTES( afd.a.format );       //wFormatTag
            WRITE_2BYTES( afd.a.channels );     //nChannels
            WRITE_4BYTES( afd.a.rate );         //nSamplesPerSec
            WRITE_4BYTES( afd.a.sampleSize *
                afd.a.rate );                   //nAvgBytesPerSec
            WRITE_2BYTES( afd.a.sampleSize );   //nBlockAlign
            WRITE_2BYTES( afd.a.bits );         //wBitsPerSample
            WRITE_2BYTES( 0 );                  //cbSize
          }
          END_CHUNK( );
        }
      }
      END_CHUNK( );

      afd.moviOffset = bufIndex;

      START_CHUNK( "LIST" );
      {
        WRITE_STRING( "movi" );
      }
    }
  }
}

/*
===============
CL_OpenAVIForWriting

Creates an AVI file and gets it into a state where
writing the actual data can begin
===============
*/
qboolean CL_AVI_RendererAllowsCapture( void )
{
	const char *renderer;

	renderer = Cvar_VariableString( "cl_renderer" );
	if ( renderer && *renderer && Q_stristr( renderer, "opengl2" ) ) {
		return qfalse;
	}
	return qtrue;
}

qboolean CL_AVI_DemoExportActive( void )
{
	return cls.aviDemoExport;
}

/* Added in Omaha: uncap FPS / vsync so demo export can exceed realtime. */
static void CL_AVI_BeginExportBoost( void )
{
	if ( s_exportBoostActive ) {
		return;
	}

	Q_strncpyz( s_savedMaxFps, Cvar_VariableString( "com_maxfps" ), sizeof( s_savedMaxFps ) );
	Q_strncpyz( s_savedMaxFpsUnfocused, Cvar_VariableString( "com_maxfpsUnfocused" ),
		sizeof( s_savedMaxFpsUnfocused ) );
	Q_strncpyz( s_savedMaxFpsMinimized, Cvar_VariableString( "com_maxfpsMinimized" ),
		sizeof( s_savedMaxFpsMinimized ) );
	s_savedSwapInterval = Cvar_VariableIntegerValue( "r_swapInterval" );

	Cvar_Set( "com_maxfps", "0" );
	Cvar_Set( "com_maxfpsUnfocused", "0" );
	Cvar_Set( "com_maxfpsMinimized", "0" );
	if ( SDL_GL_SetSwapInterval( 0 ) != 0 ) {
		Com_DPrintf( "AVI export: SDL_GL_SetSwapInterval(0) failed: %s\n", SDL_GetError() );
	}
	s_exportBoostActive = qtrue;
}

static void CL_AVI_EndExportBoost( void )
{
	if ( !s_exportBoostActive ) {
		return;
	}

	Cvar_Set( "com_maxfps", s_savedMaxFps );
	Cvar_Set( "com_maxfpsUnfocused", s_savedMaxFpsUnfocused );
	Cvar_Set( "com_maxfpsMinimized", s_savedMaxFpsMinimized );
	if ( SDL_GL_SetSwapInterval( s_savedSwapInterval ) != 0 ) {
		Com_DPrintf( "AVI export: restore swap interval failed: %s\n", SDL_GetError() );
	}
	s_exportBoostActive = qfalse;
}

static void CL_AVI_BuildFilename( char *filename, int filenameSize, const char *baseName, const char *ext )
{
	char stripped[ MAX_QPATH ];
	int  i;

	Q_strncpyz( stripped, baseName, sizeof( stripped ) );
	COM_StripExtension( stripped, stripped, sizeof( stripped ) );
	if ( !ext || !ext[0] ) {
		ext = "avi";
	}

	for ( i = 0; i <= 9999; i++ ) {
		if ( i == 0 ) {
			Com_sprintf( filename, filenameSize, "videos/%s.%s", stripped, ext );
		} else {
			Com_sprintf( filename, filenameSize, "videos/%s_%04d.%s", stripped, i, ext );
		}
		if ( !FS_FileExists_HomeData( filename ) ) {
			return;
		}
	}

	Com_sprintf( filename, filenameSize, "videos/%s_9999.%s", stripped, ext );
}

/* Added in Omaha: resolve qpath under fs_homedatapath for FFmpeg avio. */
static void CL_AVI_HomeDataOSPath( char *osPath, int osPathSize, const char *qpath )
{
	char *built;

	built = FS_BuildOSPath( Cvar_VariableString( "fs_homedatapath" ), FS_GetCurrentGameDir(), qpath );
	Q_strncpyz( osPath, built, osPathSize );
	FS_CreatePath( osPath );
}

static qboolean CL_AVI_OpenH264ForWriting( const char *qpath )
{
	char osPath[ MAX_OSPATH ];
	int  sampleRate;

	if ( afd.fileOpen || afd.h264 ) {
		return qfalse;
	}
	if ( !CL_VideoEnc_Available() ) {
		return qfalse;
	}
	if ( cl_aviFrameRate->integer <= 0 ) {
		Com_Printf( S_COLOR_RED "cl_aviFrameRate must be >= 1\n" );
		return qfalse;
	}

	Com_Memset( &afd, 0, sizeof( aviFileData_t ) );
	afd.frameRate = cl_aviFrameRate->integer;
	afd.width = cls.glconfig.vidWidth;
	afd.height = cls.glconfig.vidHeight;
	afd.motionJpeg = qfalse;
	afd.h264 = qtrue;

	afd.cBuffer = ( byte * )Z_Malloc( afd.width * afd.height * 4 );
	afd.eBuffer = ( byte * )Z_Malloc( afd.width * afd.height * 4 );

	sampleRate = S_OPENAL_AviSampleRate();
	if ( sampleRate <= 0 ) {
		sampleRate = 44100;
	}
	if ( !S_OPENAL_AviLoopbackActive() || !Cvar_VariableIntegerValue( "s_initsound" ) ) {
		sampleRate = 0;
	}

	CL_AVI_HomeDataOSPath( osPath, sizeof( osPath ), qpath );
	Q_strncpyz( afd.fileName, qpath, sizeof( afd.fileName ) );

	if ( !CL_VideoEnc_Open( osPath, afd.width, afd.height, afd.frameRate, sampleRate ) ) {
		Z_Free( afd.cBuffer );
		Z_Free( afd.eBuffer );
		Com_Memset( &afd, 0, sizeof( aviFileData_t ) );
		return qfalse;
	}

	afd.audio = ( sampleRate > 0 ) ? qtrue : qfalse;
	afd.fileOpen = qtrue;
	return qtrue;
}

qboolean CL_AVI_BeginDemoExport( const char *demoFile )
{
	const char *ext;

	if ( !demoFile || !demoFile[0] ) {
		return qfalse;
	}
	if ( !CL_AVI_RendererAllowsCapture() ) {
		Com_Printf( S_COLOR_YELLOW "Demo video export requires the OpenGL1 renderer.\n" );
		CL_UIVar_Set( "ui_om_demo_phase", "NEEDS GL1" );
		return qfalse;
	}

	/* Changed in Omaha: prefer H.264 MP4 when static FFmpeg is linked. */
	ext = CL_VideoEnc_Available() ? "mp4" : "avi";
	CL_AVI_BuildFilename( cls.aviDemoPath, sizeof( cls.aviDemoPath ), demoFile, ext );
	cls.aviDemoExport = qtrue;
	CL_UIVar_Set( "ui_om_demo_overlay", "0" );
	if ( CL_UIMenu_IsOpen( "demo_overlay" ) ) {
		CL_UIMenu_Close( "demo_overlay" );
	}
	if ( !S_OPENAL_BeginAviLoopback() ) {
		Com_Printf( S_COLOR_YELLOW "Demo export: OpenAL loopback unavailable, writing video only.\n" );
	}
	CL_UIVar_Set( "ui_om_demo_phase", CL_VideoEnc_Available() ? "EXPORTING MP4..." : "EXPORTING AVI..." );
	return qtrue;
}

void CL_AVI_TryStartIfNeeded( void )
{
	if ( !cls.aviDemoExport || CL_VideoRecording() ) {
		return;
	}
	if ( clc.state != CA_ACTIVE || !clc.demoplaying ) {
		return;
	}
	/* Fixed in Omaha: wait for loading Continue (finishloadingscreen) so the
	 * continue plaque is not baked into the export. server_loading stays true
	 * until UI_FinishLoadingScreen_f. */
	if ( server_loading ) {
		return;
	}

	CL_AVI_BeginExportBoost();

	/* Prefer H.264; fall back to MJPEG AVI if encoder open fails. */
	if ( CL_VideoEnc_Available() && CL_AVI_OpenH264ForWriting( cls.aviDemoPath ) ) {
		return;
	}

	if ( CL_VideoEnc_Available() ) {
		char base[ MAX_QPATH ];
		const char *name;

		Com_Printf( S_COLOR_YELLOW "H.264 export failed, falling back to AVI.\n" );
		Q_strncpyz( base, cls.aviDemoPath, sizeof( base ) );
		COM_StripExtension( base, base, sizeof( base ) );
		name = base;
		if ( !Q_stricmpn( name, "videos/", 7 ) ) {
			name += 7;
		}
		CL_AVI_BuildFilename( cls.aviDemoPath, sizeof( cls.aviDemoPath ), name, "avi" );
		CL_UIVar_Set( "ui_om_demo_phase", "EXPORTING AVI..." );
	}

	if ( !CL_OpenAVIForWriting( cls.aviDemoPath ) ) {
		Com_Printf( S_COLOR_RED "Demo export: could not open %s\n", cls.aviDemoPath );
		CL_UIVar_Set( "ui_om_demo_phase", "EXPORT FAILED" );
		cls.aviDemoExport = qfalse;
		cls.aviDemoPath[0] = '\0';
		S_OPENAL_EndAviLoopback();
		CL_AVI_EndExportBoost();
	}
}

void CL_AVI_FinishDemoExport( void )
{
	char wrote[ MAX_OSPATH ];

	if ( !cls.aviDemoExport && !CL_VideoRecording() ) {
		return;
	}

	Q_strncpyz( wrote, cls.aviDemoPath, sizeof( wrote ) );
	if ( CL_VideoRecording() ) {
		if ( re.Set2DWindow ) {
			re.Set2DWindow( 0, 0, cls.glconfig.vidWidth, cls.glconfig.vidHeight,
				0, cls.glconfig.vidWidth, cls.glconfig.vidHeight, 0, -1, 1 );
		}
		CL_CloseAVI();
	}
	S_OPENAL_EndAviLoopback();
	CL_AVI_EndExportBoost();
	if ( wrote[0] ) {
		char status[ 128 ];

		Com_sprintf( status, sizeof( status ), "WROTE %s", wrote );
		CL_UIVar_Set( "ui_om_demo_phase", status );
		Com_Printf( "Demo export finished: %s\n", wrote );
	}
	cls.aviDemoExport = qfalse;
	cls.aviDemoPath[0] = '\0';
}

/*
===============
CL_OpenAVIForWriting

Creates an AVI file and gets it into a state where
writing the actual data can begin
===============
*/
qboolean CL_OpenAVIForWriting( const char *fileName )
{
  int sampleRate;

  if( afd.fileOpen )
    return qfalse;

  if( !CL_AVI_RendererAllowsCapture() )
  {
    Com_Printf( S_COLOR_YELLOW "AVI capture requires the OpenGL1 renderer.\n" );
    return qfalse;
  }

  Com_Memset( &afd, 0, sizeof( aviFileData_t ) );

  // Don't start if a framerate has not been chosen
  if( cl_aviFrameRate->integer <= 0 )
  {
    Com_Printf( S_COLOR_RED "cl_aviFrameRate must be >= 1\n" );
    return qfalse;
  }

  if( ( afd.f = FS_FOpenFileWrite_HomeData( fileName ) ) <= 0 )
    return qfalse;

  if( ( afd.idxF = FS_FOpenFileWrite_HomeData(
          va( "%s" INDEX_FILE_EXTENSION, fileName ) ) ) <= 0 )
  {
    FS_FCloseFile( afd.f );
    return qfalse;
  }

  Q_strncpyz( afd.fileName, fileName, MAX_QPATH );

  afd.frameRate = cl_aviFrameRate->integer;
  afd.framePeriod = (int)( 1000000.0f / afd.frameRate );
  afd.width = cls.glconfig.vidWidth;
  afd.height = cls.glconfig.vidHeight;

  if( cl_aviMotionJpeg->integer )
    afd.motionJpeg = qtrue;
  else
    afd.motionJpeg = qfalse;

  afd.cBuffer = ( byte * )Z_Malloc( afd.width * afd.height * 4 );
  afd.eBuffer = ( byte * )Z_Malloc( afd.width * afd.height * 4 );

  /* Changed in Omaha: FAKK OpenAL has no DMA mixer; loopback supplies PCM. */
  sampleRate = S_OPENAL_AviSampleRate();
  if( sampleRate <= 0 )
    sampleRate = 44100;
  afd.a.rate = sampleRate;
  afd.a.format = WAV_FORMAT_PCM;
  afd.a.channels = 2;
  afd.a.bits = 16;
  afd.a.sampleSize = ( afd.a.bits / 8 ) * afd.a.channels;

  if( afd.a.rate % afd.frameRate )
  {
    int suggestRate = afd.frameRate;

    while( ( afd.a.rate % suggestRate ) && suggestRate >= 1 )
      suggestRate--;

    Com_Printf( S_COLOR_YELLOW "WARNING: cl_aviFrameRate is not a divisor "
        "of the audio rate, suggest %d\n", suggestRate );
  }

  if( !Cvar_VariableIntegerValue( "s_initsound" ) )
  {
    afd.audio = qfalse;
  }
  else if( S_OPENAL_AviLoopbackActive() )
  {
    afd.audio = qtrue;
  }
  else
  {
    afd.audio = qfalse;
    Com_Printf( S_COLOR_YELLOW "WARNING: AVI audio needs OpenAL Soft loopback.\n" );
  }

  // This doesn't write a real header, but allocates the
  // correct amount of space at the beginning of the file
  CL_WriteAVIHeader( );

  SafeFS_Write( buffer, bufIndex, afd.f );
  afd.fileSize = bufIndex;

  bufIndex = 0;
  START_CHUNK( "idx1" );
  SafeFS_Write( buffer, bufIndex, afd.idxF );

  afd.moviSize = 4; // For the "movi"
  afd.fileOpen = qtrue;

  return qtrue;
}

/*
===============
CL_CheckFileSize
===============
*/
static qboolean CL_CheckFileSize( int bytesToAdd )
{
  unsigned int newFileSize;

  newFileSize =
    afd.fileSize +                // Current file size
    bytesToAdd +                  // What we want to add
    ( afd.numIndices * 16 ) +     // The index
    4;                            // The index size

  // I assume all the operating systems
  // we target can handle a 2Gb file
  if( newFileSize > INT_MAX )
  {
    // Close the current file...
    CL_CloseAVI( );

    // ...And open a new one
    CL_OpenAVIForWriting( va( "%s_", afd.fileName ) );

    return qtrue;
  }

  return qfalse;
}

/*
===============
CL_WriteAVIVideoFrame
===============
*/
void CL_WriteAVIVideoFrame( const byte *imageBuffer, int size )
{
  int   chunkOffset = afd.fileSize - afd.moviOffset - 8;
  int   chunkSize = 8 + size;
  int   paddingSize = PAD( size, 2 ) - size;
  byte  padding[ 4 ] = { 0 };

  if( !afd.fileOpen )
    return;

  /* Added in Omaha: route demo export frames into the H.264 encoder. */
  if( afd.h264 )
  {
    CL_VideoEnc_PushVideo( imageBuffer, size );
    afd.numVideoFrames++;
    return;
  }

  // Chunk header + contents + padding
  if( CL_CheckFileSize( 8 + size + 2 ) )
    return;

  bufIndex = 0;
  WRITE_STRING( "00dc" );
  WRITE_4BYTES( size );

  SafeFS_Write( buffer, 8, afd.f );
  SafeFS_Write( imageBuffer, size, afd.f );
  SafeFS_Write( padding, paddingSize, afd.f );
  afd.fileSize += ( chunkSize + paddingSize );

  afd.numVideoFrames++;
  afd.moviSize += ( chunkSize + paddingSize );

  if( size > afd.maxRecordSize )
    afd.maxRecordSize = size;

  // Index
  bufIndex = 0;
  WRITE_STRING( "00dc" );           //dwIdentifier
  WRITE_4BYTES( 0x00000010 );       //dwFlags (all frames are KeyFrames)
  WRITE_4BYTES( chunkOffset );      //dwOffset
  WRITE_4BYTES( size );             //dwLength
  SafeFS_Write( buffer, 16, afd.idxF );

  afd.numIndices++;
}

#define PCM_BUFFER_SIZE 44100

/*
===============
CL_WriteAVIAudioFrame
===============
*/
void CL_WriteAVIAudioFrame( const byte *pcmBuffer, int size )
{
  static byte pcmCaptureBuffer[ PCM_BUFFER_SIZE ] = { 0 };
  static int  bytesInBuffer = 0;

  if( !afd.audio )
    return;

  if( !afd.fileOpen )
    return;

  /* Added in Omaha: H.264 path accepts PCM immediately (encoder frames AAC). */
  if( afd.h264 )
  {
    CL_VideoEnc_PushAudio( pcmBuffer, size );
    afd.numAudioFrames++;
    return;
  }

  // Chunk header + contents + padding
  if( CL_CheckFileSize( 8 + bytesInBuffer + size + 2 ) )
    return;

  if( bytesInBuffer + size > PCM_BUFFER_SIZE )
  {
    Com_Printf( S_COLOR_YELLOW
        "WARNING: Audio capture buffer overflow -- truncating\n" );
    size = PCM_BUFFER_SIZE - bytesInBuffer;
  }

  Com_Memcpy( &pcmCaptureBuffer[ bytesInBuffer ], pcmBuffer, size );
  bytesInBuffer += size;

  // Only write if we have a frame's worth of audio
  if( bytesInBuffer >= (int)ceil( (float)afd.a.rate / (float)afd.frameRate ) *
        afd.a.sampleSize )
  {
    int   chunkOffset = afd.fileSize - afd.moviOffset - 8;
    int   chunkSize = 8 + bytesInBuffer;
    int   paddingSize = PAD( bytesInBuffer, 2 ) - bytesInBuffer;
    byte  padding[ 4 ] = { 0 };

    bufIndex = 0;
    WRITE_STRING( "01wb" );
    WRITE_4BYTES( bytesInBuffer );

    SafeFS_Write( buffer, 8, afd.f );
    SafeFS_Write( pcmCaptureBuffer, bytesInBuffer, afd.f );
    SafeFS_Write( padding, paddingSize, afd.f );
    afd.fileSize += ( chunkSize + paddingSize );

    afd.numAudioFrames++;
    afd.moviSize += ( chunkSize + paddingSize );
    afd.a.totalBytes =+ bytesInBuffer;

    // Index
    bufIndex = 0;
    WRITE_STRING( "01wb" );           //dwIdentifier
    WRITE_4BYTES( 0 );                //dwFlags
    WRITE_4BYTES( chunkOffset );      //dwOffset
    WRITE_4BYTES( bytesInBuffer );    //dwLength
    SafeFS_Write( buffer, 16, afd.idxF );

    afd.numIndices++;

    bytesInBuffer = 0;
  }
}

/*
===============
CL_TakeVideoFrame
===============
*/
void CL_TakeVideoFrame( void )
{
  // AVI file isn't open
  if( !afd.fileOpen )
    return;

  /* Fixed in Omaha: capture through the renderer export, not RE_TakeVideoFrame.
   * H.264 mode always wants raw BGR (motionJpeg = false). */
  re.TakeVideoFrame( afd.width, afd.height, afd.cBuffer, afd.eBuffer,
      afd.h264 ? qfalse : afd.motionJpeg );
}

/*
===============
CL_CloseAVI

Closes the AVI file and writes an index chunk
===============
*/
qboolean CL_CloseAVI( void )
{
  int indexRemainder;
  int indexSize = afd.numIndices * 16;
  const char *idxFileName = va( "%s" INDEX_FILE_EXTENSION, afd.fileName );

  // AVI / MP4 file isn't open
  if( !afd.fileOpen )
    return qfalse;

  /* Added in Omaha: close H.264 encoder (drains queue, writes trailer). */
  if( afd.h264 )
  {
    qboolean ok;

    afd.fileOpen = qfalse;
    ok = CL_VideoEnc_Close();
    Z_Free( afd.cBuffer );
    Z_Free( afd.eBuffer );
    Com_Printf( "Wrote %d:%d frames to %s\n", afd.numVideoFrames, afd.numAudioFrames, afd.fileName );
    Com_Memset( &afd, 0, sizeof( aviFileData_t ) );
    return ok;
  }

  afd.fileOpen = qfalse;

  FS_Seek( afd.idxF, 4, FS_SEEK_SET );
  bufIndex = 0;
  WRITE_4BYTES( indexSize );
  SafeFS_Write( buffer, bufIndex, afd.idxF );
  FS_FCloseFile( afd.idxF );

  // Write index

  // Open the temp index file
  if( ( indexSize = FS_FOpenFileRead( idxFileName,
          &afd.idxF, qtrue, qtrue ) ) <= 0 )
  {
    FS_FCloseFile( afd.f );
    return qfalse;
  }

  indexRemainder = indexSize;

  // Append index to end of avi file
  while( indexRemainder > MAX_AVI_BUFFER )
  {
    FS_Read( buffer, MAX_AVI_BUFFER, afd.idxF );
    SafeFS_Write( buffer, MAX_AVI_BUFFER, afd.f );
    afd.fileSize += MAX_AVI_BUFFER;
    indexRemainder -= MAX_AVI_BUFFER;
  }
  FS_Read( buffer, indexRemainder, afd.idxF );
  SafeFS_Write( buffer, indexRemainder, afd.f );
  afd.fileSize += indexRemainder;
  FS_FCloseFile( afd.idxF );

  // Remove temp index file
  FS_Remove_HomeData( idxFileName );

  // Write the real header
  FS_Seek( afd.f, 0, FS_SEEK_SET );
  CL_WriteAVIHeader( );

  bufIndex = 4;
  WRITE_4BYTES( afd.fileSize - 8 ); // "RIFF" size

  bufIndex = afd.moviOffset + 4;    // Skip "LIST"
  WRITE_4BYTES( afd.moviSize );

  SafeFS_Write( buffer, bufIndex, afd.f );

  Z_Free( afd.cBuffer );
  Z_Free( afd.eBuffer );
  FS_FCloseFile( afd.f );

  Com_Printf( "Wrote %d:%d frames to %s\n", afd.numVideoFrames, afd.numAudioFrames, afd.fileName );

  return qtrue;
}

/*
===============
CL_VideoRecording
===============
*/
qboolean CL_VideoRecording( void )
{
  return afd.fileOpen || CL_VideoEnc_Active();
}
