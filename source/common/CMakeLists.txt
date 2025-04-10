aux_source_directory(src COMMON_SRC)
aux_source_directory(src/msg COMMON_MSG_SRC)

LIST(APPEND COMMON_SRC ${COMMON_MSG_SRC})

if(TD_ENTERPRISE)
  LIST(APPEND COMMON_SRC ${TD_ENTERPRISE_DIR}/src/plugins/common/src/tglobal.c)
endif()

add_library(common STATIC ${COMMON_SRC})

add_dependencies(common lemon_sql)

if(DEFINED GRANT_CFG_INCLUDE_DIR)
  add_definitions(-DGRANTS_CFG)
endif()

if(${BUILD_WITH_ANALYSIS})
  add_definitions(-DUSE_ANALYTICS)
endif()

if(TD_GRANT)
  ADD_DEFINITIONS(-D_GRANT)
endif()

if(TD_STORAGE)
  ADD_DEFINITIONS(-D_STORAGE)
  TARGET_LINK_LIBRARIES(common PRIVATE storage)
endif()

if(TD_ENTERPRISE)
  if(${BUILD_WITH_S3})
    add_definitions(-DUSE_S3)
  ELSEIF(${BUILD_WITH_COS})
    add_definitions(-DUSE_COS)
  endif()
endif()

target_include_directories(
  common
  PUBLIC "$ENV{HOME}/.cos-local.2/include"
  PUBLIC "${TD_SOURCE_DIR}/include/common"

  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/inc"
  PRIVATE "${GRANT_CFG_INCLUDE_DIR}"
)

if(${TD_WINDOWS})
  target_include_directories(
    common
    PRIVATE "${TD_SOURCE_DIR}/contrib/pthread"
    PRIVATE "${TD_SOURCE_DIR}/contrib/msvcregex"
  )

  target_link_libraries(
    common

    PUBLIC os
    PUBLIC util
    INTERFACE api
  )

else()
  find_library(CURL_LIBRARY curl $ENV{HOME}/.cos-local.2/lib NO_DEFAULT_PATH)
  find_library(SSL_LIBRARY ssl $ENV{HOME}/.cos-local.2/lib64 $ENV{HOME}/.cos-local.2/lib NO_DEFAULT_PATH)
  find_library(CRYPTO_LIBRARY crypto $ENV{HOME}/.cos-local.2/lib64 $ENV{HOME}/.cos-local.2/lib NO_DEFAULT_PATH)

  target_link_libraries(
    common

    PUBLIC ${CURL_LIBRARY}
    PUBLIC ${SSL_LIBRARY}
    PUBLIC ${CRYPTO_LIBRARY}

    PUBLIC os
    PUBLIC util
    INTERFACE api
  )
endif()

if(${BUILD_S3})
  if(${BUILD_WITH_S3})
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    set(CMAKE_PREFIX_PATH $ENV{HOME}/.cos-local.2)
    find_library(S3_LIBRARY s3)
    target_link_libraries(
      common

      # s3
      PUBLIC ${S3_LIBRARY}
      PUBLIC ${CURL_LIBRARY}
      PUBLIC ${SSL_LIBRARY}
      PUBLIC ${CRYPTO_LIBRARY}
      PUBLIC _libxml2
    )

    add_definitions(-DUSE_S3)
  endif()

  if(${BUILD_WITH_COS})
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    find_library(APR_LIBRARY apr-1 PATHS /usr/local/apr/lib/)
    find_library(APR_UTIL_LIBRARY aprutil-1 PATHS /usr/local/apr/lib/)
    find_library(MINIXML_LIBRARY mxml)
    target_link_libraries(
      common

      # s3
      PUBLIC cos_c_sdk_static
      PUBLIC ${APR_UTIL_LIBRARY}
      PUBLIC ${APR_LIBRARY}
      PUBLIC ${MINIXML_LIBRARY}
      PUBLIC ${CURL_LIBRARY}
    )

    # s3
    FIND_PROGRAM(APR_CONFIG_BIN NAMES apr-config apr-1-config PATHS /usr/bin /usr/local/bin /usr/local/apr/bin/)

    if(APR_CONFIG_BIN)
      EXECUTE_PROCESS(
        COMMAND ${APR_CONFIG_BIN} --includedir
        OUTPUT_VARIABLE APR_INCLUDE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
    endif()

    include_directories(${APR_INCLUDE_DIR})
    target_include_directories(
      common
      PUBLIC "${TD_SOURCE_DIR}/contrib/cos-c-sdk-v5/cos_c_sdk"
      PUBLIC "$ENV{HOME}/.cos-local.1/include"
    )

    add_definitions(-DUSE_COS)
  endif(${BUILD_WITH_COS})
endif()

if(${BUILD_TEST})
  ADD_SUBDIRECTORY(test)
endif(${BUILD_TEST})
