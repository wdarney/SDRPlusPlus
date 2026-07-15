cmake_minimum_required(VERSION 3.13)
project(vdl2_decoder)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(vdl2_decoder PRIVATE "src/")

# Link directly against vendored libacars (ACARS/CPDLC/ADS-C decoding)
if (USE_INTERNAL_LIBACARS)
    target_link_libraries(vdl2_decoder PRIVATE acars_static)
endif ()

# SQLite for message history
if (MSVC)
    find_package(unofficial-sqlite3 CONFIG REQUIRED)
    target_link_libraries(vdl2_decoder PRIVATE unofficial::sqlite3::sqlite3)
    target_compile_definitions(vdl2_decoder PRIVATE NOMINMAX _USE_MATH_DEFINES)
else()
    find_library(SQLITE3_LIB sqlite3 REQUIRED)
    target_link_libraries(vdl2_decoder PRIVATE ${SQLITE3_LIB})
endif ()
