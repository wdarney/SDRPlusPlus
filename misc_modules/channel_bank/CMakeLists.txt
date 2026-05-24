cmake_minimum_required(VERSION 3.13)
project(channel_bank)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

if(WIN32)
    find_package(FFTW3 CONFIG REQUIRED)
    target_link_libraries(channel_bank PRIVATE FFTW3::fftw3f)
else()
    find_package(PkgConfig)
    pkg_check_modules(FFTW3F REQUIRED fftw3f)
    target_link_libraries(channel_bank PRIVATE ${FFTW3F_LIBRARIES})
    target_include_directories(channel_bank PRIVATE ${FFTW3F_INCLUDE_DIRS})
endif()
