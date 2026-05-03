cmake_minimum_required(VERSION 3.13)
project(selcal_decoder)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

if (ANDROID)
    target_include_directories(selcal_decoder PRIVATE /sdr-kit/${ANDROID_ABI}/include)
    target_link_libraries(selcal_decoder PRIVATE /sdr-kit/${ANDROID_ABI}/lib/libfftw3f.so)
else()
    find_package(PkgConfig)
    pkg_check_modules(FFTW3F REQUIRED fftw3f)
    target_link_libraries(selcal_decoder PRIVATE ${FFTW3F_LIBRARIES})
    target_include_directories(selcal_decoder PRIVATE ${FFTW3F_INCLUDE_DIRS})
endif()
