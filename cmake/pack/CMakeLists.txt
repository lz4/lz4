cmake_minimum_required (VERSION 2.8)
PROJECT(LZ4)

############################## CPACK
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "LZ4 Packer")
set(CPACK_PACKAGE_VERSION_MAJOR 0)
set(CPACK_PACKAGE_VERSION_MINOR 0)
set(CPACK_PACKAGE_VERSION_PATCH r89)
#set(CPACK_RESOURCE_FILE_LICENSE  ${CMAKE_CURRENT_BINARY_DIR}/COPYING_LGPL)
##############################
FIND_PACKAGE(Subversion)
IF(SUBVERSION_FOUND)
	Subversion_WC_INFO(${SRC_DIR}  revision)
	set(revision_MY_WC_STATUS "LZ4 has revision ${revision_WC_REVISION} at ${revision_WC_LAST_CHANGED_DATE}")
	message(STATUS ${revision_MY_WC_STATUS})
	if(NOT ${revision_WC_REVISION})
	else(NOT ${revision_WC_REVISION})
		set(CPACK_PACKAGE_VERSION_PATCH "r${revision_WC_REVISION}")
	endif(NOT ${revision_WC_REVISION})
ELSE(SUBVERSION_FOUND)
	message(WARNING "NO Subversion FOUND!!!")
ENDIF(SUBVERSION_FOUND)
set(VERSION_STRING	" \"${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}\" ")
##############################
INCLUDE (CheckTypeSize)
check_type_size("void *" SIZEOF_VOID_P)
IF( ${SIZEOF_VOID_P} STREQUAL  "8" )
    set (CMAKE_SYSTEM_PROCESSOR "64bit")
    MESSAGE( STATUS "64 bit architecture detected size of void * is " ${SIZEOF_VOID_P})
ENDIF()
###############################     warnings

ADD_DEFINITIONS("-Wall")
ADD_DEFINITIONS("-W")
ADD_DEFINITIONS("-Wundef")
ADD_DEFINITIONS("-Wcast-align")
ADD_DEFINITIONS("-Wno-implicit-function-declaration")
ADD_DEFINITIONS("-O3 -march=native -std=c99")
INCLUDE_DIRECTORIES (${SRC_DIR})



set(LZ4_SRCS_LIB ${SRC_DIR}lz4.c ${SRC_DIR}lz4hc.c ${SRC_DIR}lz4.h ${SRC_DIR}lz4_format_description.txt)
set(LZ4_SRCS ${LZ4_SRCS_LIB} ${SRC_DIR}xxhash.c ${SRC_DIR}bench.c ${SRC_DIR}lz4c.c )
set(FUZZER_SRCS ${SRC_DIR}lz4.c ${SRC_DIR}lz4hc.c ${SRC_DIR}lz4.h ${SRC_DIR}fuzzer.c)

# EXECUTABLES FOR 32 Bit and 64 versions
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "64bit")
	add_executable(lz4c32 ${LZ4_SRCS})
	install(TARGETS lz4c32 RUNTIME DESTINATION "./")
SET_TARGET_PROPERTIES(lz4c32 PROPERTIES
	COMPILE_FLAGS PROPERTIES COMPILE_FLAGS "-m32 -Os" LINK_FLAGS "-m32")
endif()

add_executable(lz4c ${LZ4_SRCS})
install(TARGETS lz4c RUNTIME DESTINATION "./")

add_executable(fuzzer ${FUZZER_SRCS})
install(TARGETS fuzzer RUNTIME DESTINATION "./")

#target_link_libraries(lz4 ${LZ4_SRCS_LIB})
####################### CPACK PACKAGING ###################
install(FILES ${SRC_DIR}lz4_format_description.txt DESTINATION "./")

set(CPACK_PACKAGE_NAME ${CMAKE_PROJECT_NAME})
set(CPACK_RESOURCE_FILE_LICENSE  "${CMAKE_CURRENT_LIST_DIR}/release_COPYING.txt")
set(CPACK_PACKAGE_VENDOR "Yann Collet")
set(CPACK_WWW_SITE "http://fastcompression.blogspot.com/p/lz4.html")
set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_WWW_SITE}")
set(CPACK_NSIS_HELP_LINK "${CPACK_WWW_SITE}")

set(CPACK_NSIS_DISPLAY_NAME ${CPACK_PACKAGE_NAME})
set(CPACK_NSIS_COMPRESSOR "/SOLID lzma \r\n SetCompressorDictSize 32")

set(CPACK_NSIS_MENU_LINKS
	"${CPACK_WWW_SITE}" "${CPACK_PACKAGE_NAME} homepage"
 	"/lz4_format_description.txt "	"lz4 format description"
 	"/"	"LZ4 directory "
)

include (CPack)
