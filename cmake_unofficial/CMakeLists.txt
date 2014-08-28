PROJECT(LZ4 C)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "LZ4 compression library")
set(CPACK_PACKAGE_VERSION_MAJOR 1)
set(CPACK_PACKAGE_VERSION_MINOR 3)
set(CPACK_PACKAGE_VERSION_PATCH r122)
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

option(BUILD_TOOLS "Build the command line tools" ON)
option(BUILD_LIBS  "Build the libraries in addition to the tools" OFF)

if(UNIX AND BUILD_LIBS)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
        add_definitions(-fPIC)
    endif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
endif()

set(LZ4_DIR ../)
set(PRG_DIR ../programs/)
set(LZ4_SRCS_LIB ${LZ4_DIR}lz4.c ${LZ4_DIR}lz4hc.c ${LZ4_DIR}lz4.h ${LZ4_DIR}lz4hc.h)
set(LZ4_SRCS ${PRG_DIR}xxhash.c ${PRG_DIR}bench.c ${PRG_DIR}lz4cli.c ${PRG_DIR}lz4io.c)

if(BUILD_TOOLS AND NOT BUILD_LIBS)
    set(LZ4_SRCS ${LZ4_SRCS} ${LZ4_SRCS_LIB})
endif()

if(BUILD_TOOLS)
    add_executable(lz4 ${LZ4_SRCS})
    set_target_properties(lz4 PROPERTIES COMPILE_DEFINITIONS DISABLE_LZ4C_LEGACY_OPTIONS)
    install(TARGETS lz4 RUNTIME DESTINATION "bin/")
    add_executable(lz4c ${LZ4_SRCS})
    install(TARGETS lz4c RUNTIME DESTINATION "bin/")
endif()

if(BUILD_LIBS)    
    add_library(liblz4 ${LZ4_SRCS_LIB})
    
    set_target_properties(liblz4 PROPERTIES
    OUTPUT_NAME lz4
    SOVERSION "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}"
    )
        
    install(TARGETS liblz4
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    )

    install(FILES
    ${LZ4_DIR}/lz4.h
    ${LZ4_DIR}/lz4hc.h
    DESTINATION include
    )
    
    if(BUILD_TOOLS)
        target_link_libraries(lz4 liblz4)
        target_link_libraries(lz4c liblz4)
    endif()
endif()


#warnings

ADD_DEFINITIONS("-Wall")
ADD_DEFINITIONS("-Wextra")
ADD_DEFINITIONS("-Wundef")
ADD_DEFINITIONS("-Wcast-align")
ADD_DEFINITIONS("-std=c99")
ADD_DEFINITIONS("-DLZ4_VERSION=\"${CPACK_PACKAGE_VERSION_PATCH}\"")
INCLUDE_DIRECTORIES (${LZ4_DIR})



