cmake_minimum_required(VERSION 3.13)
project(vdl2_decoder)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(vdl2_decoder PRIVATE "src/")

# ATN schemas share the existing libacars ASN.1 runtime (no duplicate runtime).
if(NOT TARGET acars_static)
    message(FATAL_ERROR "VDL2 ATN decoding requires the vendored libacars target acars_static")
endif()
add_subdirectory(protocol)
target_link_libraries(vdl2_decoder PRIVATE vdl2_protocol)
target_include_directories(vdl2_decoder PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/protocol")

# SQLite for message history
find_library(SQLITE3_LIB sqlite3)
if (SQLITE3_LIB)
    target_link_libraries(vdl2_decoder PRIVATE ${SQLITE3_LIB})
endif ()
