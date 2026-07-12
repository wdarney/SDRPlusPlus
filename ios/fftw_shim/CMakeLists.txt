cmake_minimum_required(VERSION 3.18)
project(fftw_shim)

# Static library implementing the slice of single-precision FFTW3 that
# SDR++ uses, on top of vDSP. Built on every host (macOS too — Accelerate
# ships there) so the host sanity-build works without external FFTW.
add_library(fftw_shim STATIC fftw_shim.cpp)
target_include_directories(fftw_shim PUBLIC include)
target_link_libraries(fftw_shim PUBLIC "-framework Accelerate")
