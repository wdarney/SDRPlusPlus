cmake_minimum_required(VERSION 3.13)
project(rx888_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

find_package(PkgConfig)
pkg_check_modules(SOAPY REQUIRED SoapySDR)

target_include_directories(rx888_source PRIVATE ${SOAPY_INCLUDE_DIRS})
target_link_directories(rx888_source PRIVATE ${SOAPY_LIBRARY_DIRS})
target_link_libraries(rx888_source PRIVATE ${SOAPY_LIBRARIES})
