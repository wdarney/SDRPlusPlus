cmake_minimum_required(VERSION 3.13)
project(rtl_sdr_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/rtl_sdr_blog.cmake)
target_include_directories(rtl_sdr_source BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/rtl-sdr-blog/include)
target_link_libraries(rtl_sdr_source PRIVATE sdrpp_rtlsdr_blog)
