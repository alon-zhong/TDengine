aux_source_directory(src COMMON_SRC)
add_library(common STATIC ${COMMON_SRC})

if (DEFINED GRANT_CFG_INCLUDE_DIR)
  add_definitions(-DGRANTS_CFG)
endif()

IF (TD_GRANT)
  ADD_DEFINITIONS(-D_GRANT)
ENDIF ()

target_include_directories(
    common
    PUBLIC "${TD_SOURCE_DIR}/include/common"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/inc"
    PRIVATE "${GRANT_CFG_INCLUDE_DIR}"
)
IF(${TD_WINDOWS})
    target_include_directories(
        common
        PRIVATE "${TD_SOURCE_DIR}/contrib/pthread"
        PRIVATE "${TD_SOURCE_DIR}/contrib/msvcregex"
    )
ENDIF ()

target_link_libraries(
    common
    PUBLIC os
    PUBLIC util
    INTERFACE api
)

if(${BUILD_TEST})
    ADD_SUBDIRECTORY(test)
endif(${BUILD_TEST})
