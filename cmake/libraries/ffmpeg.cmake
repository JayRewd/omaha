# Optional static FFmpeg + x264 for H.264 demo export (Project: Omaha).
# Build deps with: misc/video/build-ffmpeg-x264.sh
# Then: cmake -DFFMPEG_ROOT=<prefix> ...

if(NOT BUILD_CLIENT)
    return()
endif()

option(USE_VIDEO_H264 "H.264 MP4 demo export via static FFmpeg/x264" ON)

if(NOT USE_VIDEO_H264)
    return()
endif()

set(FFMPEG_ROOT "" CACHE PATH "Prefix of static FFmpeg+x264 install (from misc/video/build-ffmpeg-x264.sh)")
# Release/CI builds set this so a missing FFmpeg fails the configure instead of silently shipping AVI-only.
option(FFMPEG_REQUIRED "Fail configure when the static FFmpeg/x264 libs are missing" OFF)

if(NOT FFMPEG_ROOT AND DEFINED ENV{FFMPEG_ROOT})
    set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
endif()

if(NOT FFMPEG_ROOT)
    # Default local build-deps location next to the source tree.
    set(_omaha_ffmpeg_default "${CMAKE_SOURCE_DIR}/build-deps/ffmpeg")
    if(EXISTS "${_omaha_ffmpeg_default}/lib/libavcodec.a" OR EXISTS "${_omaha_ffmpeg_default}/lib/libavcodec.lib")
        set(FFMPEG_ROOT "${_omaha_ffmpeg_default}")
    endif()
endif()

# FFmpeg/x264 may install as libfoo.a (Unix / MSVC makefile) or foo.lib.
function(_omaha_find_static_lib out_var root names)
    foreach(_n IN LISTS names)
        if(EXISTS "${root}/lib/${_n}")
            set(${out_var} "${root}/lib/${_n}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

_omaha_find_static_lib(_avformat "${FFMPEG_ROOT}" "libavformat.a;avformat.lib;libavformat.lib")
_omaha_find_static_lib(_avcodec  "${FFMPEG_ROOT}" "libavcodec.a;avcodec.lib;libavcodec.lib")
_omaha_find_static_lib(_swscale  "${FFMPEG_ROOT}" "libswscale.a;swscale.lib;libswscale.lib")
_omaha_find_static_lib(_avutil   "${FFMPEG_ROOT}" "libavutil.a;avutil.lib;libavutil.lib")
_omaha_find_static_lib(_x264     "${FFMPEG_ROOT}" "libx264.a;libx264.lib;x264.lib")

if(NOT _avcodec OR NOT _avformat OR NOT _swscale OR NOT _avutil OR NOT _x264)
    if(FFMPEG_REQUIRED)
        message(FATAL_ERROR "USE_VIDEO_H264: FFMPEG_REQUIRED is set but static FFmpeg/x264 libs were not found under '${FFMPEG_ROOT}'.")
    endif()
    message(STATUS "USE_VIDEO_H264: FFmpeg static libs not found under ${FFMPEG_ROOT}. Demo export falls back to AVI.")
    return()
endif()

list(APPEND CLIENT_DEFINITIONS USE_VIDEO_H264)
list(APPEND CLIENT_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
list(APPEND CLIENT_LIBRARIES ${_avformat} ${_avcodec} ${_swscale} ${_avutil} ${_x264})

if(UNIX AND NOT APPLE)
    list(APPEND CLIENT_LIBRARIES m pthread dl)
elseif(APPLE)
    list(APPEND CLIENT_LIBRARIES m pthread)
elseif(WIN32)
    # MSVC static FFmpeg needs Winsock / bcrypt for some avutil bits.
    list(APPEND CLIENT_LIBRARIES ws2_32 bcrypt secur32)
endif()

message(STATUS "USE_VIDEO_H264: linking static FFmpeg from ${FFMPEG_ROOT}")
