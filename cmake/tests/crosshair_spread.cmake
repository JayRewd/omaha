#
# Unit tests
#

add_executable(test_crosshair_spread
    ${SOURCE_DIR}/qcommon/tests/test_crosshair_spread.cpp
    ${SOURCE_DIR}/qcommon/crosshair_spread.c
    ${SOURCE_DIR}/qcommon/q_shared.c
    ${SOURCE_DIR}/qcommon/common_light.c
)

target_include_directories(test_crosshair_spread PRIVATE ${SOURCE_DIR}/qcommon)
add_test(NAME test_crosshair_spread COMMAND test_crosshair_spread)
set_tests_properties(test_crosshair_spread PROPERTIES TIMEOUT 15)
