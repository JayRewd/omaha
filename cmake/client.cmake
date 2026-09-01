if(NOT BUILD_CLIENT)
    return()
endif()

include(utils/add_git_dependency)
include(utils/set_output_dirs)
include(shared_sources)
include(shared_script)

include(renderer_common)
include(libraries/libtess2)

set(CLIENT_SOURCES
    ${SOURCE_DIR}/client/cl_avi.cpp
    ${SOURCE_DIR}/client/cl_cgame.cpp
    ${SOURCE_DIR}/client/cl_cin.cpp
    ${SOURCE_DIR}/client/cl_consolecmds.cpp
    ${SOURCE_DIR}/client/cl_curl.c
    ${SOURCE_DIR}/client/cl_input.cpp
    ${SOURCE_DIR}/client/cl_instantAction.cpp
    ${SOURCE_DIR}/client/cl_inv.cpp
    ${SOURCE_DIR}/client/cl_invrender.cpp
    ${SOURCE_DIR}/client/cl_keys.cpp
    ${SOURCE_DIR}/client/cl_main.cpp
    ${SOURCE_DIR}/client/cl_net_chan.cpp
    ${SOURCE_DIR}/client/cl_parse.cpp
    ${SOURCE_DIR}/client/cl_scrn.cpp
    ${SOURCE_DIR}/client/cl_ui.cpp
    ${SOURCE_DIR}/client/cl_uibind.cpp
    ${SOURCE_DIR}/client/cl_uidmbox.cpp
    ${SOURCE_DIR}/client/cl_uifilepicker.cpp
    ${SOURCE_DIR}/client/cl_uigamespy.cpp
    ${SOURCE_DIR}/client/cl_uigmbox.cpp
    ${SOURCE_DIR}/client/cl_uilangame.cpp
    ${SOURCE_DIR}/client/cl_uiloadsave.cpp
    ${SOURCE_DIR}/client/cl_uimaprotationsetup.cpp
    ${SOURCE_DIR}/client/cl_uimaprunner.cpp
    ${SOURCE_DIR}/client/cl_uiminicon.cpp
    ${SOURCE_DIR}/client/cl_uimpmappicker.cpp
    ${SOURCE_DIR}/client/cl_uiplayermodelpicker.cpp
    ${SOURCE_DIR}/client/cl_uiradar.cpp
    ${SOURCE_DIR}/client/cl_uirender.cpp
    ${SOURCE_DIR}/client/cl_uimenu_dispatcher.cpp
    ${SOURCE_DIR}/client/cl_hud_registry.cpp
    ${SOURCE_DIR}/client/cl_hud_host.cpp
    ${SOURCE_DIR}/client/cl_objectives_host.cpp
    ${SOURCE_DIR}/client/cl_messages_host.cpp
    ${SOURCE_DIR}/client/cl_killfeed_classify.cpp
    ${SOURCE_DIR}/client/cl_killfeed.cpp
    ${SOURCE_DIR}/client/cl_modern_browser.cpp
    ${SOURCE_DIR}/client/cl_scoreboard_host.cpp
    ${SOURCE_DIR}/client/cl_uiserverlist.cpp
    ${SOURCE_DIR}/client/cl_uisoundpicker.cpp
    ${SOURCE_DIR}/client/cl_uistd.cpp
    ${SOURCE_DIR}/client/cl_uiview3d.cpp
    ${SOURCE_DIR}/client/libmumblelink.c
    ${SOURCE_DIR}/client/qal.c
    ${SOURCE_DIR}/client/snd_codec_mp3.c
    ${SOURCE_DIR}/client/snd_codec_ogg.c
    ${SOURCE_DIR}/client/snd_codec_opus.c
    ${SOURCE_DIR}/client/snd_codec_wav.c
    ${SOURCE_DIR}/client/snd_codec.c
    ${SOURCE_DIR}/client/snd_dma_new.cpp
    ${SOURCE_DIR}/client/snd_info.cpp
    ${SOURCE_DIR}/client/snd_mem_new.cpp
    ${SOURCE_DIR}/client/snd_miles_new.cpp
    ${SOURCE_DIR}/client/snd_openal_new.cpp
    ${SOURCE_DIR}/client/usignal.cpp
    ${SOURCE_DIR}/sdl/sdl_input.c
    ${SOURCE_DIR}/sdl/sdl_mouse.c
    ${CLIENT_PLATFORM_SOURCES}
)

set(UIRENDER_SOURCES
    ${SOURCE_DIR}/uirender/uir_viewport.c
    ${SOURCE_DIR}/uirender/uir_fov.c
    ${SOURCE_DIR}/uirender/uir_path.c
    ${SOURCE_DIR}/uirender/uir_svg.c
    ${SOURCE_DIR}/uirender/uir_draw2d.c
    ${SOURCE_DIR}/uirender/uir_meshcache.c
    ${SOURCE_DIR}/uirender/uir_batch.c
    ${SOURCE_DIR}/uirender/uir_tess.c
    ${LIBTESS2_SOURCES}
    ${SOURCE_DIR}/uirender/uir_compositor.c
    ${SOURCE_DIR}/uirender/uir_debug.c
    ${SOURCE_DIR}/uirender/uir_menuworld.c
    ${SOURCE_DIR}/uirender/uir_menu_map_view.c
    ${SOURCE_DIR}/uirender/uir_map_env.c
    ${SOURCE_DIR}/uirender/uir_menu_weather.c
    ${SOURCE_DIR}/uirender/uir_backend.c
    ${SOURCE_DIR}/uirender/uir_pathcache.c
    ${SOURCE_DIR}/uirender/uir_font.cpp
    ${SOURCE_DIR}/uirender/uir_gradient.c
    ${SOURCE_DIR}/uirender/uir_image.c
    ${SOURCE_DIR}/uirender/uir_stencil.c
    ${SOURCE_DIR}/uirender/uir_layer.c
    ${SOURCE_DIR}/uirender/uir_modelpreview_math.c
    ${SOURCE_DIR}/uirender/uir_modelpreview.cpp
    ${SOURCE_DIR}/uirender/uir_weapon_bake_list.c
)

set(UIDESIGN_SOURCES
    ${SOURCE_DIR}/uidesign/uid_diag.cpp
    ${SOURCE_DIR}/uidesign/uid_value.cpp
    ${SOURCE_DIR}/uidesign/uid_expr.cpp
    ${SOURCE_DIR}/uidesign/uid_expr_bool.cpp
    ${SOURCE_DIR}/uidesign/uid_invoke.cpp
    ${SOURCE_DIR}/uidesign/uid_document.cpp
    ${SOURCE_DIR}/uidesign/uid_xml.cpp
    ${SOURCE_DIR}/uidesign/uid_template.cpp
    ${SOURCE_DIR}/uidesign/uid_vars.cpp
    ${SOURCE_DIR}/uidesign/uid_compile.cpp
    ${SOURCE_DIR}/uidesign/uid_shape.cpp
    ${SOURCE_DIR}/uidesign/uid_layout.cpp
    ${SOURCE_DIR}/uidesign/uid_widget.cpp
    ${SOURCE_DIR}/uidesign/uid_input.cpp
    ${SOURCE_DIR}/uidesign/uid_action.cpp
    ${SOURCE_DIR}/uidesign/uid_invoke.cpp
    ${SOURCE_DIR}/uidesign/uid_binding.cpp
    ${SOURCE_DIR}/uidesign/uid_modal.cpp
    ${SOURCE_DIR}/uidesign/uid_collection.cpp
    ${SOURCE_DIR}/uidesign/uid_menu_map_view.cpp
    ${SOURCE_DIR}/uidesign/uid_scrollbar.cpp
    ${SOURCE_DIR}/uidesign/uid_runtime.cpp
    ${SOURCE_DIR}/uidesign/uid_profile.cpp
    ${SOURCE_DIR}/uidesign/uid_opt.cpp
    ${SOURCE_DIR}/thirdparty/tinyxml2/tinyxml2.cpp
)

# Gamespy
list(APPEND CLIENT_SOURCES
	${SOURCE_DIR}/gamespy/cl_gamespy.c
)

file(GLOB_RECURSE UI_SOURCES "${SOURCE_DIR}/uilib/*.c" "${SOURCE_DIR}/uilib/*.cpp")

add_git_dependency(${SOURCE_DIR}/client/cl_console.c)

set(CLIENT_BINARY ${CLIENT_NAME})

list(APPEND CLIENT_DEFINITIONS BOTLIB)
list(APPEND CLIENT_DEFINITIONS APP_MODULE)

if(BUILD_STANDALONE)
    list(APPEND CLIENT_DEFINITIONS STANDALONE)
endif()

if(USE_RENDERER_DLOPEN)
    list(APPEND CLIENT_DEFINITIONS USE_RENDERER_DLOPEN)
endif()

if(USE_HTTP)
    list(APPEND CLIENT_DEFINITIONS USE_HTTP)
endif()

if(USE_VOIP)
    list(APPEND CLIENT_DEFINITIONS USE_VOIP)
endif()

if(USE_MUMBLE)
    list(APPEND CLIENT_DEFINITIONS USE_MUMBLE)
    list(APPEND CLIENT_LIBRARY_SOURCES ${SOURCE_DIR}/client/libmumblelink.c)
endif()

list(APPEND CLIENT_BINARY_SOURCES
    ${SERVER_SOURCES}
    ${CLIENT_SOURCES}
    ${UIRENDER_SOURCES}
    ${UIDESIGN_SOURCES}
    ${UI_SOURCES}
    ${COMMON_SOURCES}
    ${SCRIPT_SYSTEM_SOURCES}
    ${BOTLIB_SOURCES}
    ${SYSTEM_SOURCES}
    ${ASM_SOURCES}
    ${CLIENT_LIBRARY_SOURCES})

add_executable(${CLIENT_BINARY} ${CLIENT_EXECUTABLE_OPTIONS} ${CLIENT_BINARY_SOURCES})

target_include_directories(     ${CLIENT_BINARY} PRIVATE ${CLIENT_INCLUDE_DIRS})
target_include_directories(     ${CLIENT_BINARY} PRIVATE ${SOURCE_DIR}/client)
target_include_directories(     ${CLIENT_BINARY} PRIVATE ${SOURCE_DIR}/uidesign)
target_include_directories(     ${CLIENT_BINARY} PRIVATE ${SOURCE_DIR}/thirdparty/tinyxml2)
target_include_directories(     ${CLIENT_BINARY} PRIVATE ${LIBTESS2_INCLUDES})
target_compile_definitions(     ${CLIENT_BINARY} PRIVATE ${CLIENT_DEFINITIONS})
target_compile_options(         ${CLIENT_BINARY} PRIVATE ${CLIENT_COMPILE_OPTIONS})
target_link_libraries(          ${CLIENT_BINARY} PRIVATE ${COMMON_LIBRARIES} ${CLIENT_LIBRARIES})
target_link_options(            ${CLIENT_BINARY} PRIVATE ${CLIENT_LINK_OPTIONS})

set_output_dirs(${CLIENT_BINARY})

# Ship modern UI assets next to the client binary (fs_basepath / main).
add_custom_command(TARGET ${CLIENT_BINARY} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:${CLIENT_BINARY}>/main/fonts
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:${CLIENT_BINARY}>/main/ui/modern/examples
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:${CLIENT_BINARY}>/main/sound/prom
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets/main/fonts
        $<TARGET_FILE_DIR:${CLIENT_BINARY}>/main/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets/main/ui/modern
        $<TARGET_FILE_DIR:${CLIENT_BINARY}>/main/ui/modern
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets/main/sound
        $<TARGET_FILE_DIR:${CLIENT_BINARY}>/main/sound
    COMMENT "Copy modern UI fonts, ui/modern, and sound assets"
)

# Same relative main/ layout as POST_BUILD: next to the installed client binary.
# INSTALL_BINDIR_FULL is lib*/openmohaa (Unix) or bin (Windows); game data is not
# under INSTALL_DATADIR_FULL (desktop/metainfo icons only).
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/main/fonts
    DESTINATION ${INSTALL_BINDIR_FULL}/main)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/main/ui/modern
    DESTINATION ${INSTALL_BINDIR_FULL}/main/ui)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/main/sound
    DESTINATION ${INSTALL_BINDIR_FULL}/main)

if(NOT USE_RENDERER_DLOPEN)
    target_sources(${CLIENT_BINARY} PRIVATE
        # These are never simultaneously populated
        ${RENDERER_GL1_BINARY_SOURCES}
        ${RENDERER_GL2_BINARY_SOURCES})

    target_include_directories( ${CLIENT_BINARY} PRIVATE ${RENDERER_INCLUDE_DIRS})
    target_compile_definitions( ${CLIENT_BINARY} PRIVATE ${RENDERER_DEFINITIONS})
    target_compile_options(     ${CLIENT_BINARY} PRIVATE ${RENDERER_COMPILE_OPTIONS})
    target_link_libraries(      ${CLIENT_BINARY} PRIVATE ${RENDERER_LIBRARIES})
endif()

foreach(LIBRARY IN LISTS CLIENT_DEPLOY_LIBRARIES)
    add_custom_command(TARGET ${CLIENT_BINARY} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            ${LIBRARY}
            $<TARGET_FILE_DIR:${CLIENT_BINARY}>)

    install(FILES ${LIBRARY} DESTINATION
        # install() requires a relative path hence:
        $<PATH:RELATIVE_PATH,$<TARGET_FILE_DIR:${CLIENT_BINARY}>,${CMAKE_BINARY_DIR}/$<CONFIG>>)
endforeach()

set_target_properties(${CLIENT_BINARY} PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})

INSTALL(TARGETS ${CLIENT_BINARY} DESTINATION ${INSTALL_BINDIR_FULL})

if (MSVC)
	target_link_options(${CLIENT_BINARY} PRIVATE "/MANIFEST:NO")
	INSTALL(FILES $<TARGET_PDB_FILE:${CLIENT_BINARY}> DESTINATION ${INSTALL_BINDIR_FULL} OPTIONAL)
endif()
