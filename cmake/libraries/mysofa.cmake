# Added in Omaha: libmysofa (SOFA HRTF reader) for the omaha headphone HRTF path. Client only.
if(NOT BUILD_CLIENT)
    return()
endif()

include(utils/disable_warnings)

set(MYSOFA_DIR ${SOURCE_DIR}/thirdparty/libmysofa/src)

set(MYSOFA_SOURCES
    ${MYSOFA_DIR}/hrtf/reader.c
    ${MYSOFA_DIR}/hdf/superblock.c
    ${MYSOFA_DIR}/hdf/dataobject.c
    ${MYSOFA_DIR}/hdf/btree.c
    ${MYSOFA_DIR}/hdf/fractalhead.c
    ${MYSOFA_DIR}/hdf/gunzip.c
    ${MYSOFA_DIR}/hdf/gcol.c
    ${MYSOFA_DIR}/hrtf/check.c
    ${MYSOFA_DIR}/hrtf/spherical.c
    ${MYSOFA_DIR}/hrtf/lookup.c
    ${MYSOFA_DIR}/hrtf/tools.c
    ${MYSOFA_DIR}/hrtf/kdtree.c
    ${MYSOFA_DIR}/hrtf/neighbors.c
    ${MYSOFA_DIR}/hrtf/interpolate.c
    ${MYSOFA_DIR}/hrtf/resample.c
    ${MYSOFA_DIR}/hrtf/loudness.c
    ${MYSOFA_DIR}/hrtf/minphase.c
    ${MYSOFA_DIR}/hrtf/easy.c
    ${MYSOFA_DIR}/hrtf/cache.c
    ${MYSOFA_DIR}/resampler/speex_resampler.c
)
set(MYSOFA_INCLUDE_DIRS ${MYSOFA_DIR}/hrtf)

disable_warnings(${MYSOFA_SOURCES})

add_library(omaha_mysofa STATIC ${MYSOFA_SOURCES})
set_target_properties(omaha_mysofa PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_include_directories(omaha_mysofa PRIVATE ${MYSOFA_DIR} ${ZLIB_INCLUDE_DIRS})
target_include_directories(omaha_mysofa PUBLIC ${MYSOFA_INCLUDE_DIRS})
# Internal zlib objects are compiled into the client binary itself (zlib.cmake).
if(NOT USE_INTERNAL_ZLIB)
    target_link_libraries(omaha_mysofa PUBLIC ${ZLIB_LIBRARIES})
endif()
if(UNIX)
    target_link_libraries(omaha_mysofa PUBLIC m)
endif()

list(APPEND CLIENT_LIBRARIES omaha_mysofa)
list(APPEND CLIENT_INCLUDE_DIRS ${MYSOFA_INCLUDE_DIRS})
