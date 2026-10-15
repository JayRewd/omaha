# Added in Omaha: PFFFT (single precision) for the omaha headphone HRTF convolution. Client only.
if(NOT BUILD_CLIENT)
    return()
endif()

include(utils/disable_warnings)

set(PFFFT_DIR ${SOURCE_DIR}/thirdparty/pffft)

set(PFFFT_SOURCES
    ${PFFFT_DIR}/pffft.c
    ${PFFFT_DIR}/pffft_common.c
)

disable_warnings(${PFFFT_SOURCES})

add_library(omaha_pffft STATIC ${PFFFT_SOURCES})
set_target_properties(omaha_pffft PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(omaha_pffft PRIVATE _USE_MATH_DEFINES)
target_compile_definitions(omaha_pffft PUBLIC PFFFT_STATIC_DEFINE)
target_include_directories(omaha_pffft PUBLIC ${PFFFT_DIR})
if(UNIX)
    target_link_libraries(omaha_pffft PUBLIC m)
endif()

list(APPEND CLIENT_LIBRARIES omaha_pffft)
list(APPEND CLIENT_INCLUDE_DIRS ${PFFFT_DIR})
list(APPEND CLIENT_DEFINITIONS PFFFT_STATIC_DEFINE)
