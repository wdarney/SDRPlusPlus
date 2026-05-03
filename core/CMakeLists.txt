cmake_minimum_required(VERSION 3.18)
project(sdrpp_core)

if (USE_INTERNAL_LIBCORRECT)
    add_subdirectory("libcorrect/")
endif (USE_INTERNAL_LIBCORRECT)

# Main code. Exclude server.cpp — that's the SDR++ *server* mode which an
# iOS client never runs, and which is the only consumer of zstd's compressor.
# The server:: namespace functions still referenced from core.cpp /
# signal_path/source.cpp get no-op stubs from server_ios_stub.cpp instead.
file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")
list(FILTER SRC EXCLUDE REGEX "src/server\\.cpp$")

add_definitions(-DSDRPP_IS_CORE)
add_definitions(-DFLOG_ANDROID_TAG="SDR++") # harmless leftover, used by flog macros

# Backend sources. Only compiled when targeting iOS — UIKit isn't available
# in the host macOS SDK, so gating this lets developers do partial host
# builds (modules + DSP + signal_path) for sanity-checking before deploying.
if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    file(GLOB_RECURSE BACKEND_SRC
        "backends/ios/*.cpp"
        "backends/ios/*.mm"
        "backends/ios/*.m"
        "backends/ios/imgui/*.cpp"
        "backends/ios/imgui/*.mm"
    )
else ()
    set(BACKEND_SRC "backends/ios/host_stub.cpp")
endif ()

add_library(sdrpp_core STATIC ${SRC} ${BACKEND_SRC})
target_compile_options(sdrpp_core PRIVATE ${SDRPP_COMPILER_FLAGS})

# Includes
target_include_directories(sdrpp_core PUBLIC "src/")
target_include_directories(sdrpp_core PUBLIC "src/imgui")
target_include_directories(sdrpp_core PUBLIC "backends/ios")
target_include_directories(sdrpp_core PUBLIC "backends/ios/imgui")

# libcorrect
if (USE_INTERNAL_LIBCORRECT)
    target_include_directories(sdrpp_core PUBLIC "libcorrect/include")
    target_link_libraries(sdrpp_core PUBLIC correct_static)
endif (USE_INTERNAL_LIBCORRECT)

# Apple frameworks needed by the iOS Metal backend (skipped on host builds).
if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_link_libraries(sdrpp_core PUBLIC
        "-framework UIKit"
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
        "-framework CoreGraphics"
    )
endif ()

# Cross-compiled deps live under SDRPP_IOS_DEPS_ROOT (see ios/README.md).
# Required: fftw3f, zstd. Optional: volk (link only if present, otherwise the
# generic fallback in the DSP layer is used).
if (NOT DEFINED SDRPP_IOS_DEPS_ROOT)
    message(FATAL_ERROR "SDRPP_IOS_DEPS_ROOT must point to the iOS prebuilt deps dir — see ios/README.md")
endif ()
target_include_directories(sdrpp_core PUBLIC "${SDRPP_IOS_DEPS_ROOT}/include")
target_link_directories(sdrpp_core PUBLIC    "${SDRPP_IOS_DEPS_ROOT}/lib")
target_link_libraries(sdrpp_core PUBLIC fftw3f zstd)

find_library(_VOLK_LIB NAMES volk PATHS "${SDRPP_IOS_DEPS_ROOT}/lib" NO_DEFAULT_PATH)
if (_VOLK_LIB)
    target_link_libraries(sdrpp_core PUBLIC volk)
else ()
    message(WARNING "VOLK not found in SDRPP_IOS_DEPS_ROOT/lib — DSP will use generic kernels")
endif ()
