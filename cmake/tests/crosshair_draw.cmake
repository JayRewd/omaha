#
# Unit tests
#

add_executable(test_crosshair_draw
    ${SOURCE_DIR}/qcommon/tests/test_crosshair_draw.cpp
    ${SOURCE_DIR}/qcommon/crosshair_draw.c
    ${SOURCE_DIR}/qcommon/q_shared.c
    ${SOURCE_DIR}/qcommon/common_light.c
)

# Fixed in Omaha: harness only exercises pure emit helpers; CGAME_DLL skips
# client cvar registration paths that need the full Cvar_* engine.
target_compile_definitions(test_crosshair_draw PRIVATE CGAME_DLL)

target_include_directories(test_crosshair_draw PRIVATE ${SOURCE_DIR}/qcommon)
add_test(NAME test_crosshair_draw COMMAND test_crosshair_draw)
set_tests_properties(test_crosshair_draw PROPERTIES TIMEOUT 15)
