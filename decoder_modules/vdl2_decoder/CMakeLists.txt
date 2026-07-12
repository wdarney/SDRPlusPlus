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
find_library(SQLITE3_LIB sqlite3)
if (SQLITE3_LIB)
    target_link_libraries(vdl2_decoder PRIVATE ${SQLITE3_LIB})
endif ()
