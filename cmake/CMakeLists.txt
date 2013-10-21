PROJECT(LZ4 C)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "LZ4 compression library")
set(CPACK_PACKAGE_VERSION_MAJOR 0)
set(CPACK_PACKAGE_VERSION_MINOR 0)
set(CPACK_PACKAGE_VERSION_PATCH r107)
#set(CPACK_RESOURCE_FILE_LICENSE  ${CMAKE_CURRENT_BINARY_DIR}/COPYING_LGPL)
set(VERSION_STRING	" \"${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}\" ")
include(CPack)


cmake_minimum_required (VERSION 2.6)
INCLUDE (CheckTypeSize)
check_type_size("void *" SIZEOF_VOID_P)
IF( ${SIZEOF_VOID_P} STREQUAL  "8" )
    set (CMAKE_SYSTEM_PROCESSOR "64bit")
    MESSAGE( STATUS "64 bit architecture detected size of void * is " ${SIZEOF_VOID_P})
ENDIF()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries")

if (BUILD_SHARED_LIBS)
    message(STATUS "Enable shared library building")
else(BUILD_SHARED_LIBS)
    message(STATUS "Disable shared library building")
endif(BUILD_SHARED_LIBS)

if(UNIX AND BUILD_SHARED_LIBS)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
        add_definitions(-fPIC)
    endif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
endif()

set(SRC_DIR ../)
set(LZ4_SRCS_LIB ${SRC_DIR}lz4.c ${SRC_DIR}lz4hc.c ${SRC_DIR}lz4.h )
set(LZ4_SRCS ${SRC_DIR}xxhash.c ${SRC_DIR}bench.c ${SRC_DIR}lz4cli.c)

if(NOT BUILD_SHARED_LIBS)
    set(LZ4_SRCS ${LZ4_SRCS} ${LZ4_SRCS_LIB})
endif()

if (CMAKE_SYSTEM_PROCESSOR STREQUAL "64bit")
    message(STATUS "Build 64bit executable binary")
    add_executable(lz4c64 ${LZ4_SRCS})
    install(TARGETS lz4c64 RUNTIME DESTINATION "bin/")
    if(NOT BUILD_SHARED_LIBS)
        message(STATUS "Build 32bit executable binary")
        add_executable(lz4c32 ${LZ4_SRCS})
        install(TARGETS lz4c32 RUNTIME DESTINATION "bin/")

        SET_TARGET_PROPERTIES(lz4c32 PROPERTIES
            COMPILE_FLAGS PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    endif()
else()
    message(STATUS "Build 32bit executable binary")
    add_executable(lz4c32 ${LZ4_SRCS})
    install(TARGETS lz4c32 RUNTIME DESTINATION "bin/")
endif()

if(BUILD_SHARED_LIBS)
    set(LZ4_SOVERSION "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}")
    if (CMAKE_SYSTEM_PROCESSOR STREQUAL "64bit")
        target_link_libraries(lz4c64 liblz4)
    else()
        target_link_libraries(lz4c32 liblz4)
    endif()
	
    # for static library
    add_library(liblz4_static STATIC ${LZ4_SRCS_LIB})
    set_target_properties(liblz4_static PROPERTIES OUTPUT_NAME lz4)

    install(TARGETS liblz4_static
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
    )

    # for shared library o
    add_library(liblz4 ${LZ4_SRCS_LIB})
    set_target_properties(liblz4 PROPERTIES
            OUTPUT_NAME lz4
            SOVERSION ${LZ4_SOVERSION})

    install(TARGETS liblz4
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
    )
    
    install(FILES
        ${SRC_DIR}/lz4.h
        ${SRC_DIR}/lz4hc.h
        DESTINATION include)
            
endif(BUILD_SHARED_LIBS)


#warnings

ADD_DEFINITIONS("-Wall")
ADD_DEFINITIONS("-W")
ADD_DEFINITIONS("-Wundef")
ADD_DEFINITIONS("-Wcast-align")
ADD_DEFINITIONS("-Wno-implicit-function-declaration")
ADD_DEFINITIONS("-Os -march=native -std=c99")
INCLUDE_DIRECTORIES (${SRC_DIR})



