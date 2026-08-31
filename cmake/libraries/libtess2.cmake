# libtess2 — SGI polygon tessellator (memonen/libtess2)

set(LIBTESS2_DIR ${SOURCE_DIR}/thirdparty/libtess2)

set(LIBTESS2_SOURCES
    ${LIBTESS2_DIR}/Source/bucketalloc.c
    ${LIBTESS2_DIR}/Source/dict.c
    ${LIBTESS2_DIR}/Source/geom.c
    ${LIBTESS2_DIR}/Source/mesh.c
    ${LIBTESS2_DIR}/Source/priorityq.c
    ${LIBTESS2_DIR}/Source/sweep.c
    ${LIBTESS2_DIR}/Source/tess.c
)

set(LIBTESS2_INCLUDES
    ${LIBTESS2_DIR}/Include
    ${LIBTESS2_DIR}/Source
)
