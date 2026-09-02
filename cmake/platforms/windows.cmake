# Windows specific settings

if(NOT WIN32)
    return()
endif()

# Changed in Omaha: on x86/x64, Windows 7 (0x0601) is the Win32 API floor so
# MSVC/Windows SDK builds do not hard-import Win8+ entry points (e.g. CreateFile2).
# Windows on ARM is Win10+ only — do not force the Win7 floor there (breaks MSVC
# ARM64 Interlocked* linking in deps like SDL2).
# Override with -DWIN32_WINNT_HEX=0x0A00 (etc.) only if intentionally raising the floor.
set(_omaha_win_is_arm64 FALSE)
if(CMAKE_GENERATOR_PLATFORM MATCHES "[Aa][Rr][Mm]64")
    set(_omaha_win_is_arm64 TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64|arm64")
    set(_omaha_win_is_arm64 TRUE)
elseif(DEFINED ARCH AND ARCH MATCHES "arm64")
    set(_omaha_win_is_arm64 TRUE)
endif()
if(NOT _omaha_win_is_arm64)
    if(NOT DEFINED WIN32_WINNT_HEX)
        set(WIN32_WINNT_HEX "0x0601")
    endif()
    if(NOT DEFINED NTDDI_VERSION_HEX)
        set(NTDDI_VERSION_HEX "0x06010000") # Win7 SP1
    endif()
    add_compile_definitions(
        WINVER=${WIN32_WINNT_HEX}
        _WIN32_WINNT=${WIN32_WINNT_HEX}
        NTDDI_VERSION=${NTDDI_VERSION_HEX}
    )
endif()
unset(_omaha_win_is_arm64)

list(APPEND SYSTEM_PLATFORM_SOURCES
    ${SOURCE_DIR}/sys/sys_win32.c
    ${SOURCE_DIR}/sys/win_resource.rc
)

list(APPEND CLIENT_PLATFORM_SOURCES ${SOURCE_DIR}/sys/con_passive.c)
list(APPEND SERVER_PLATFORM_SOURCES ${SOURCE_DIR}/sys/con_win32.c)


#if(USE_HTTP)
#    list(APPEND CLIENT_PLATFORM_SOURCES ${SOURCE_DIR}/client/cl_http_windows.c)
#    list(APPEND CLIENT_LIBRARIES wininet)
#endif()

list(APPEND COMMON_LIBRARIES
    ws2_32 # Windows Sockets 2
    winmm  # timeBeginPeriod/timeEndPeriod
    psapi  # EnumProcesses
)

if(MINGW)
    list(APPEND COMMON_LIBRARIES mingw32)
endif()

list(APPEND CLIENT_DEFINITIONS USE_ICON)

set_source_files_properties(${SOURCE_DIR}/sys/win_resource.rc
    PROPERTIES COMPILE_DEFINITIONS WINDOWS_ICON_PATH=${WINDOWS_ICON_PATH})

if(MSVC)
    # We have our own manifest, disable auto creation
    list(APPEND SERVER_LINK_OPTIONS "/MANIFEST:NO")
    list(APPEND CLIENT_LINK_OPTIONS "/MANIFEST:NO")
endif()

set(CLIENT_EXECUTABLE_OPTIONS WIN32)

# It's only necessary to set this on Windows; elsewhere
# CMAKE_EXECUTABLE_SUFFIX will be empty anyway, or we want
# HOST_EXECUTABLE_SUFFIX to be empty for other reasons
set(HOST_EXECUTABLE_SUFFIX ${CMAKE_EXECUTABLE_SUFFIX})

set(CPACK_GENERATOR NSIS)
set(CPACK_NSIS_MUI_ICON ${WINDOWS_ICON_PATH})
set(CPACK_NSIS_EXECUTABLES_DIRECTORY .)

#
# non-ioq3
#

if(CMAKE_BUILD_TYPE MATCHES Debug|RelWithDebInfo)
    # Enable the console in debug builds
    list(REMOVE_ITEM CLIENT_PLATFORM_SOURCES ${SOURCE_DIR}/sys/con_passive.c)
    list(APPEND CLIENT_PLATFORM_SOURCES ${SOURCE_DIR}/sys/con_win32.c)
    set(CLIENT_EXECUTABLE_OPTIONS)
endif()

list(APPEND SYSTEM_PLATFORM_SOURCES ${SOURCE_DIR}/sys/new/sys_win32_new.c)
list(APPEND COMMON_LIBRARIES dbghelp)

#
# Setup the installation directory
#
# By default, both DLLs and EXEs are in the same directory
set(CMAKE_DEFAULT_INSTALL_RUNTIME_DIR bin)
set(BIN_INSTALL_SUBDIR ".")
set(LIB_INSTALL_SUBDIR ".")