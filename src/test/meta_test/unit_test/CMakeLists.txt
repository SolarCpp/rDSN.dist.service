if (DEFINED DSN_CMAKE_INCLUDED)
else()
    
    set(DSN_ROOT "$ENV{DSN_ROOT}")
    if(NOT EXISTS "${DSN_ROOT}/")
        message(FATAL_ERROR "Please make sure that ${DSN_ROOT} exists.")
    endif()

    include("${DSN_ROOT}/bin/dsn.cmake")
endif()

set(MY_PROJ_NAME dsn.dist.service.meta.test)

# Source files under CURRENT project directory will be automatically included.
# You can manually set MY_PROJ_SRC to include source files under other directories.
set(MY_PROJ_SRC ../misc/misc.cpp)

# Search mode for source files under CURRENT project directory?
# "GLOB_RECURSE" for recursive search
# "GLOB" for non-recursive search
set(MY_SRC_SEARCH_MODE "GLOB")

set(MY_PROJ_INC_PATH 
    ../../../meta_server/meta_server_lib 
    #../../../meta_server/zookeeper 
    ../../../common 
    ${GTEST_INCLUDE_DIR}
    )

set(MY_PROJ_LIBS dsn.dist.service.meta_server_lib
                 dsn.dist.service.zookeeper_provider
                 dsn.dist.service.common
                 dsn.dist.service.fd
                 zookeeper
                 GTest::GTest)

set(MY_PROJ_LIB_PATH "")

# Extra files that will be installed
set(MY_BINPLACES test.sh config-test.ini)

dsn_add_executable()
#add_dependencies(${MY_PROJ_NAME} googletest)
