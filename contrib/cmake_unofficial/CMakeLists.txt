PROJECT(LZ4 C)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "LZ4 compression library")
set(CPACK_PACKAGE_VERSION_MAJOR 1)
set(CPACK_PACKAGE_VERSION_MINOR 7)
set(CPACK_PACKAGE_VERSION_PATCH 2)
set(VERSION_STRING	" \"${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}\" ")
include(CPack)

cmake_minimum_required (VERSION 2.6)
INCLUDE (CheckTypeSize)
check_type_size("void *" SIZEOF_VOID_P)
IF(SIZEOF_VOID_P STREQUAL  "8")
    set (CMAKE_SYSTEM_PROCESSOR "64bit")
    MESSAGE( STATUS "64 bit architecture detected size of void * is " ${SIZEOF_VOID_P})
ENDIF()

option(BUILD_TOOLS "Build the command line tools" ON)
option(BUILD_LIBS  "Build the libraries in addition to the tools" ON)
option(LINK_TOOLS_WITH_LIB "Link the command line tools with the (shared) library" OFF)

IF(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR
   CMAKE_C_COMPILER_ID STREQUAL "Clang")
  SET(GNU_COMPATIBLE_COMPILER 1)
ENDIF()


set(LZ4_DIR ../../lib/)
set(PRG_DIR ../../programs/)
set(LZ4_SRCS_LIB ${LZ4_DIR}lz4.c ${LZ4_DIR}lz4hc.c ${LZ4_DIR}lz4.h ${LZ4_DIR}lz4hc.h ${LZ4_DIR}lz4frame.c ${LZ4_DIR}lz4frame.h ${LZ4_DIR}xxhash.c)
set(LZ4_SRCS ${LZ4_DIR}lz4frame.c ${LZ4_DIR}xxhash.c ${PRG_DIR}bench.c ${PRG_DIR}lz4cli.c ${PRG_DIR}lz4io.c ${PRG_DIR}datagen.c)

if(BUILD_TOOLS AND NOT (LINK_TOOLS_WITH_LIB AND BUILD_LIBS))
    set(LZ4_SRCS ${LZ4_SRCS} ${LZ4_SRCS_LIB})
endif()

if(BUILD_TOOLS)
    add_executable(lz4 ${LZ4_SRCS})
    install(TARGETS lz4 RUNTIME DESTINATION "bin/")
endif()

if(BUILD_LIBS)


    SET(LIBS_TARGETS "")
    IF(WIN32)
        add_library(liblz4 STATIC ${LZ4_SRCS_LIB})
        SET(LIBS_TARGETS liblz4)
    ELSE(WIN32)
        add_library(liblz4 SHARED ${LZ4_SRCS_LIB})
        add_library(liblz4_static STATIC ${LZ4_SRCS_LIB})
        SET_TARGET_PROPERTIES(liblz4_static PROPERTIES OUTPUT_NAME lz4)
        SET(LIBS_TARGETS liblz4 liblz4_static)
    ENDIF(WIN32)

    set_target_properties(liblz4 PROPERTIES
    OUTPUT_NAME lz4
    SOVERSION "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}"
    )

    install(TARGETS ${LIBS_TARGETS}
	RUNTIME DESTINATION lib #on Windows: cmake considers dlls as runtime component
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    )

    install(FILES
    ${LZ4_DIR}/lz4.h
    ${LZ4_DIR}/lz4hc.h
    ${LZ4_DIR}/lz4frame.h
    DESTINATION include
    )

    set(PREFIX ${CMAKE_INSTALL_PREFIX})
    set(LIBDIR ${CMAKE_INSTALL_PREFIX}/lib)
    set(INCLUDEDIR ${CMAKE_INSTALL_PREFIX}/include)
    string(SUBSTRING ${CPACK_PACKAGE_VERSION_PATCH} 1 -1 VERSION)
    configure_file(${LZ4_DIR}/liblz4.pc.in liblz4.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/liblz4.pc
    DESTINATION "${CMAKE_INSTALL_PREFIX}/lib${LIB_SUFFIX}/pkgconfig"
    )

  if(BUILD_TOOLS AND LINK_TOOLS_WITH_LIB)
        target_link_libraries(lz4 liblz4)
    endif()
endif()


#warnings

if(MSVC)
    ADD_DEFINITIONS("-W4")
endif()
if(GNU_COMPATIBLE_COMPILER)
    ADD_DEFINITIONS("-Wall")
endif()
if(CMAKE_COMPILER_IS_GNUCXX)
    ADD_DEFINITIONS("-Wextra")
    ADD_DEFINITIONS("-Wundef")
    ADD_DEFINITIONS("-Wshadow")
    ADD_DEFINITIONS("-Wcast-align")
    ADD_DEFINITIONS("-Wstrict-prototypes")
endif(CMAKE_COMPILER_IS_GNUCXX)
if(GNU_COMPATIBLE_COMPILER)
    # we need gnu99 instead of c99 on Linux and Solaris
    # to get C99 and POSIX definitions
    # an alternative with cmake >= 3.1/3.2 is the C_STANDARD property
    ADD_DEFINITIONS("-std=gnu99")
endif()
ADD_DEFINITIONS("-DLZ4_VERSION=\"${CPACK_PACKAGE_VERSION_PATCH}\"")
INCLUDE_DIRECTORIES (${LZ4_DIR})
