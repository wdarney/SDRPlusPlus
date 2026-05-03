cmake_minimum_required(VERSION 3.18)
project(volk_shim)

# Static library that provides the slice of the VOLK API SDR++ uses, backed
# by Apple's Accelerate framework. Built on every host (including macOS,
# where Accelerate also exists), so the host sanity-build doesn't need
# real VOLK installed.
add_library(volk_shim STATIC volk_shim.cpp)
target_include_directories(volk_shim PUBLIC include)
target_link_libraries(volk_shim PUBLIC "-framework Accelerate")
