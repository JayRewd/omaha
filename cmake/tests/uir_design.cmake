#
# Unit tests for XML design-format runtime (parser/layout/widgets/bindings)
#

include(libraries/libtess2)

set(UIDESIGN_TEST_SOURCES
    ${SOURCE_DIR}/uidesign/tests/test_uir_design.cpp
    ${SOURCE_DIR}/client/cl_killfeed_classify.cpp
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
    ${SOURCE_DIR}/uirender/uir_viewport.c
    ${SOURCE_DIR}/uirender/uir_path.c
    ${SOURCE_DIR}/uirender/uir_svg.c
    ${SOURCE_DIR}/uirender/uir_image.c
    ${SOURCE_DIR}/uirender/uir_gradient.c
    ${SOURCE_DIR}/uirender/uir_stencil.c
    ${SOURCE_DIR}/uirender/uir_layer.c
    ${SOURCE_DIR}/uirender/uir_compositor.c
    ${SOURCE_DIR}/uirender/uir_draw2d.c
    ${SOURCE_DIR}/uirender/uir_debug.c
    ${SOURCE_DIR}/uirender/uir_meshcache.c
    ${SOURCE_DIR}/uirender/uir_pathcache.c
    ${SOURCE_DIR}/uirender/uir_batch.c
    ${SOURCE_DIR}/uirender/uir_tess.c
    ${LIBTESS2_SOURCES}
    ${SOURCE_DIR}/uirender/uir_menu_map_view.c
    ${SOURCE_DIR}/uirender/tests/uir_modelpreview_stub.c
    ${SOURCE_DIR}/uirender/tests/uir_menuworld_stub.c
    ${SOURCE_DIR}/qcommon/q_shared.c
)

add_executable(test_uir_design ${UIDESIGN_TEST_SOURCES})

target_include_directories(test_uir_design PRIVATE
    ${SOURCE_DIR}/uidesign
    ${SOURCE_DIR}/uirender
    ${SOURCE_DIR}/qcommon
    ${SOURCE_DIR}/client
    ${SOURCE_DIR}/thirdparty/tinyxml2
    ${LIBTESS2_INCLUDES}
)

target_compile_features(test_uir_design PRIVATE cxx_std_17)

target_compile_definitions(test_uir_design PRIVATE
    "UID_TEST_FIXTURE_DIR=\"${CMAKE_SOURCE_DIR}/assets/main/ui/modern\""
)

add_test(NAME test_uir_design COMMAND test_uir_design)
set_tests_properties(test_uir_design PROPERTIES TIMEOUT 60)
