cmake_minimum_required(VERSION 3.13)
project(channel_bank)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

if(WIN32)
    find_package(FFTW3f CONFIG REQUIRED)
    target_include_directories(channel_bank PRIVATE ${FFTW3f_INCLUDE_DIRS})
    target_link_directories(channel_bank PRIVATE ${FFTW3f_LIBRARY_DIRS})
    target_link_libraries(channel_bank PRIVATE ${FFTW3f_LIBRARIES})
else()
    find_package(PkgConfig)
    pkg_check_modules(FFTW3F REQUIRED fftw3f)
    target_link_libraries(channel_bank PRIVATE ${FFTW3F_LIBRARIES})
    target_include_directories(channel_bank PRIVATE ${FFTW3F_INCLUDE_DIRS})
endif()
