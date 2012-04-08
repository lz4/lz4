PROJECT(LZ4)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ASN.1 Compiler")
set(CPACK_PACKAGE_VERSION_MAJOR 0)
set(CPACK_PACKAGE_VERSION_MINOR 0)
set(CPACK_PACKAGE_VERSION_PATCH r51)
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


set(SRC_DIR ../)
set(LZ4_SRCS_LIB ${SRC_DIR}lz4.c ${SRC_DIR}lz4.h )
set(LZ4_SRCS ${LZ4_SRCS_LIB} ${SRC_DIR}bench.c ${SRC_DIR}lz4demo.c )

# EXECUTABLES FOR 32 Bit and 64 versions
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "64bit")
	add_executable(lz4demo64 ${LZ4_SRCS})
        install(TARGETS lz4demo64 RUNTIME DESTINATION "./")
endif()

	add_executable(lz4demo32 ${LZ4_SRCS})
        install(TARGETS lz4demo32 RUNTIME DESTINATION "./")

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "64bit")
SET_TARGET_PROPERTIES(lz4demo32 PROPERTIES
	COMPILE_FLAGS PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()


#warnings

ADD_DEFINITIONS("-Wall")
ADD_DEFINITIONS("-W")
ADD_DEFINITIONS("-Wundef")
ADD_DEFINITIONS("-Wcast-align")
ADD_DEFINITIONS("-Wno-implicit-function-declaration")
ADD_DEFINITIONS("-Os -march=native -std=c99")
INCLUDE_DIRECTORIES (${SRC_DIR})


#target_link_libraries(lz4 ${LZ4_SRCS_LIB})


