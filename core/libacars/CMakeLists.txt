cmake_minimum_required(VERSION 3.13)

set(LA_VERSION_MAJOR 2)
set(LA_VERSION_MINOR 2)
set(LA_VERSION_PATCH 1)
set(LA_VERSION "${LA_VERSION_MAJOR}.${LA_VERSION_MINOR}.${LA_VERSION_PATCH}")
set(LA_SUBDIR "libacars-${LA_VERSION_MAJOR}")

set(CMAKE_C_STANDARD 11)

option(EMIT_ASN_DEBUG "Enable debugging of ASN.1 decoder" OFF)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Vendored zlib (MIAM decompression)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/zlib EXCLUDE_FROM_ALL)
set(ZLIB_VENDORED_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/zlib"
    "${CMAKE_CURRENT_BINARY_DIR}/3rdparty/zlib"
)

# Vendored jansson (OHMA decoding)
set(JANSSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(JANSSON_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(JANSSON_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JANSSON_WITHOUT_TESTS ON CACHE BOOL "" FORCE)
set(JANSSON_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/jansson EXCLUDE_FROM_ALL)
set(JANSSON_VENDORED_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/jansson/src"
    "${CMAKE_CURRENT_BINARY_DIR}/3rdparty/jansson/include"
)

add_subdirectory(libacars)
