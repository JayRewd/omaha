#
# Unit tests for the server list scheduler
#

add_executable(test_serverlist_scheduler
    ${SOURCE_DIR}/gamespy/tests/test_serverlist_scheduler.cpp
    ${SOURCE_DIR}/gamespy/gserverlist_scheduler.c
)

add_test(NAME test_serverlist_scheduler COMMAND test_serverlist_scheduler)

set_tests_properties(test_serverlist_scheduler PROPERTIES TIMEOUT 15)
