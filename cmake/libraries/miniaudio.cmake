if(NOT BUILD_CLIENT)
    return()
endif()

set(MINIAUDIO_DIR ${SOURCE_DIR}/thirdparty/miniaudio)

find_package(Threads REQUIRED)

add_library(miniaudio STATIC
    ${MINIAUDIO_DIR}/miniaudio_impl.c
)

target_include_directories(miniaudio PUBLIC ${MINIAUDIO_DIR})

if(UNIX AND NOT APPLE)
    target_link_libraries(miniaudio PUBLIC Threads::Threads m ${CMAKE_DL_LIBS})
elseif(APPLE)
    target_link_libraries(miniaudio PUBLIC Threads::Threads "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")
else()
    target_link_libraries(miniaudio PUBLIC Threads::Threads)
endif()

list(APPEND CLIENT_LIBRARIES miniaudio)
list(APPEND CLIENT_INCLUDE_DIRS ${MINIAUDIO_DIR})
