if (DEFINED DSN_CMAKE_INCLUDED)
else()

    set(DSN_ROOT "$ENV{DSN_ROOT}")
    if(NOT EXISTS "${DSN_ROOT}/")
        message(FATAL_ERROR "Please make sure that ${DSN_ROOT} exists.")
    endif()

    include("${DSN_ROOT}/bin/dsn.cmake")
endif()

set(MY_PROJ_NAME dsn.dist.service.stateless)

# Source files under CURRENT project directory will be automatically included.
# You can manually set MY_PROJ_SRC to include source files under other directories.
set(MY_PROJ_SRC "")

# Search mode for source files under CURRENT project directory?
# "GLOB_RECURSE" for recursive search
# "GLOB" for non-recursive search
set(MY_SRC_SEARCH_MODE "GLOB")

set(MY_PROJ_INC_PATH "")

set(MY_PROJ_LIBS dsn.dist.service.fd.multimaster
                 dsn.dist.service.fd
                 dsn.dist.service.common
                 GTest::GTest
                 )

set(MY_PROJ_LIB_PATH "")

# Extra files that will be installed
set(MY_BINPLACES "")

set(MY_BOOST_PACKAGES "")

dsn_add_shared_library()

if(NOT ("$ENV{APPVEYOR}" STREQUAL ""))
    message(STATUS "APPVEYOR = $ENV{APPVEYOR}, skip unit test for ${MY_PROJ_NAME}")
else()
    file(COPY test/ DESTINATION "${CMAKE_BINARY_DIR}/test/${MY_PROJ_NAME}")
endif()



