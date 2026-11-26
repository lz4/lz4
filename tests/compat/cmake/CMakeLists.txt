cmake_minimum_required(VERSION 3.10...4.0.2)

project(cmake_install_test LANGUAGES C)

# Find LZ4 installation in Config mode
find_package(lz4 CONFIG REQUIRED)

# Verify that the universal target always exists
if (NOT TARGET LZ4::lz4)
  message(FATAL_ERROR "LZ4::lz4 target does not exist!")
endif()

# Validate each concrete library that this installation provides.
set(LZ4_CONCRETE_TARGETS)
foreach(library_type IN ITEMS shared static)
  set(library_target LZ4::lz4_${library_type})
  if(TARGET ${library_target})
    list(APPEND LZ4_CONCRETE_TARGETS ${library_target})
    get_property(LZ4_LOCATION TARGET ${library_target} PROPERTY LOCATION)
    if(NOT LZ4_LOCATION)
      message(FATAL_ERROR "Missing LOCATION property for ${library_target}!")
    endif()
    get_property(LZ4_INCLUDE_DIRECTORIES TARGET ${library_target} PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT LZ4_INCLUDE_DIRECTORIES)
      message(FATAL_ERROR "Missing INTERFACE_INCLUDE_DIRECTORIES property for ${library_target}!")
    endif()
    add_executable(decompress-partial-${library_type} "${CMAKE_CURRENT_SOURCE_DIR}/decompress-partial.c")
    target_link_libraries(decompress-partial-${library_type} ${library_target})
  endif()
endforeach()
if(NOT LZ4_CONCRETE_TARGETS)
  message(FATAL_ERROR "No concrete LZ4 library target is available!")
endif()

# Build against the universal targets as well as the concrete libraries.
add_executable(decompress-partial "${CMAKE_CURRENT_SOURCE_DIR}/decompress-partial.c")
target_link_libraries(decompress-partial LZ4::lz4)
add_executable(decompress-partial-lz4 "${CMAKE_CURRENT_SOURCE_DIR}/decompress-partial.c")
target_link_libraries(decompress-partial-lz4 lz4::lz4)
