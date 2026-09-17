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
target_include_directories(vdl2_decoder PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/protocol"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../core/libacars/libacars/asn1")

# SQLite for message history
if (MSVC)
    find_package(unofficial-sqlite3 CONFIG REQUIRED)
    target_link_libraries(vdl2_decoder PRIVATE unofficial::sqlite3::sqlite3)
    target_compile_definitions(vdl2_decoder PRIVATE NOMINMAX _USE_MATH_DEFINES)
else()
    find_library(SQLITE3_LIB sqlite3 REQUIRED)
    target_link_libraries(vdl2_decoder PRIVATE ${SQLITE3_LIB})
endif ()

# Development default; users can select a relocated dashboard and Python in UI.
target_compile_definitions(vdl2_decoder PRIVATE VDL2_DASHBOARD_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/dashboard")
