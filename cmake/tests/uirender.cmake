#
# Unit tests for modern UI renderer math/path/svg
#

include(libraries/libtess2)

add_executable(test_uirender
    ${SOURCE_DIR}/uirender/tests/test_uirender.cpp
    ${SOURCE_DIR}/uirender/uir_viewport.c
    ${SOURCE_DIR}/uirender/uir_fov.c
    ${SOURCE_DIR}/uirender/uir_path.c
    ${SOURCE_DIR}/uirender/uir_svg.c
    ${SOURCE_DIR}/uirender/uir_font.cpp
    ${SOURCE_DIR}/uirender/uir_gradient.c
    ${SOURCE_DIR}/uirender/uir_image.c
    ${SOURCE_DIR}/uirender/uir_stencil.c
    ${SOURCE_DIR}/uirender/uir_draw2d.c
    ${SOURCE_DIR}/uirender/uir_meshcache.c
    ${SOURCE_DIR}/uirender/uir_pathcache.c
    ${SOURCE_DIR}/uirender/uir_batch.c
    ${SOURCE_DIR}/uirender/uir_tess.c
    ${LIBTESS2_SOURCES}
    ${SOURCE_DIR}/uirender/uir_compositor.c
    ${SOURCE_DIR}/uirender/uir_debug.c
    ${SOURCE_DIR}/uirender/uir_modelpreview_math.c
    ${SOURCE_DIR}/uirender/uir_menuworld.c
    ${SOURCE_DIR}/uirender/uir_menu_map_view.c
    ${SOURCE_DIR}/uirender/uir_map_env.c
    ${SOURCE_DIR}/uirender/uir_menu_weather.c
    ${SOURCE_DIR}/uidesign/uid_profile.cpp
    ${SOURCE_DIR}/qcommon/q_shared.c
)

target_include_directories(test_uirender PRIVATE ${SOURCE_DIR}/uirender ${SOURCE_DIR}/uidesign ${SOURCE_DIR}/qcommon ${LIBTESS2_INCLUDES})
add_test(NAME test_uirender COMMAND test_uirender)
set_tests_properties(test_uirender PROPERTIES TIMEOUT 30)

# Offline stroke seam search (allied-star ring continuity across sizes/scales).
add_executable(uir_stroke_search
    ${SOURCE_DIR}/uirender/tools/uir_stroke_search.cpp
    ${SOURCE_DIR}/uirender/uir_viewport.c
    ${SOURCE_DIR}/uirender/uir_path.c
    ${SOURCE_DIR}/uirender/uir_svg.c
)
target_include_directories(uir_stroke_search PRIVATE ${SOURCE_DIR}/uirender)
add_test(NAME uir_stroke_search_quick COMMAND uir_stroke_search --quick --no-csv)
set_tests_properties(uir_stroke_search_quick PROPERTIES TIMEOUT 120)

add_executable(uir_bench_frame
    ${SOURCE_DIR}/uirender/tools/uir_bench_frame.cpp
    ${SOURCE_DIR}/uirender/uir_viewport.c
    ${SOURCE_DIR}/uirender/uir_path.c
    ${SOURCE_DIR}/uirender/uir_svg.c
    ${SOURCE_DIR}/uirender/uir_draw2d.c
    ${SOURCE_DIR}/uirender/uir_debug.c
    ${SOURCE_DIR}/uirender/uir_meshcache.c
    ${SOURCE_DIR}/uirender/uir_batch.c
    ${SOURCE_DIR}/uirender/uir_tess.c
    ${LIBTESS2_SOURCES}
    ${SOURCE_DIR}/qcommon/q_shared.c
)
target_include_directories(uir_bench_frame PRIVATE ${SOURCE_DIR}/uirender ${SOURCE_DIR}/qcommon ${LIBTESS2_INCLUDES})
add_test(NAME uir_bench_frame COMMAND uir_bench_frame)
set_tests_properties(uir_bench_frame PROPERTIES TIMEOUT 30)
