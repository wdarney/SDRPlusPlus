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
# On iOS, icons are loaded via Metal (backends/ios/icons_metal.mm).
# Exclude the OpenGL implementation to avoid duplicate symbol errors.
if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(FILTER SRC EXCLUDE REGEX "src/gui/icons\\.cpp$")
endif ()

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
    # imgui_impl_metal.mm and our backend.mm both rely on ARC features
    # (__weak, autoreleased blocks). Apply -fobjc-arc to all .mm/.m sources
    # in the iOS backend — there is no non-ARC Obj-C++ in the iOS path so
    # this is safe to set globally per file.
    foreach (_src ${BACKEND_SRC})
        if (_src MATCHES "\\.mm?$")
            set_source_files_properties(${_src} PROPERTIES
                COMPILE_FLAGS "-fobjc-arc")
        endif ()
    endforeach ()
else ()
    set(BACKEND_SRC "backends/ios/host_stub.cpp")
endif ()

add_library(sdrpp_core STATIC ${SRC} ${BACKEND_SRC})
target_compile_options(sdrpp_core PRIVATE ${SDRPP_COMPILER_FLAGS})
# Public C++17 — consumers (modules, the iOS app target) need this too.
# DSP templates use std::is_same_v / `if constexpr`, which are C++17.
target_compile_features(sdrpp_core PUBLIC cxx_std_17)

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

# Apple frameworks needed by core + the iOS Metal backend (skipped on host).
# Done at core scope (rather than per-OBJECT-library) so PRIVATE link entries
# on OBJECT libs don't get silently dropped during transitive linking.
if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_link_libraries(sdrpp_core PUBLIC
        "-framework UIKit"
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
        "-framework CoreGraphics"
        # OpenGL ES — icons.cpp and waterfall.cpp store ImGui textures via
        # glGenTextures / glBindTexture / glTexImage2D. The Metal renderer
        # ignores these handles, but the call sites still need to link.
        "-framework OpenGLES"
        # AudioToolbox / AVFoundation — coreaudio_sink uses AudioUnit. These
        # belong with the module but per-OBJECT-lib frameworks don't reach
        # the final exe link, so they're hoisted here.
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework CoreAudio"
        "-framework Speech"
    )
endif ()

# Cross-compiled deps live under SDRPP_IOS_DEPS_ROOT (see ios/README.md).
# Only libzstd is needed externally — VOLK and FFTW3 are replaced by
# Accelerate-backed shims under ios/volk_shim/ and ios/fftw_shim/.
if (NOT DEFINED SDRPP_IOS_DEPS_ROOT)
    message(FATAL_ERROR "SDRPP_IOS_DEPS_ROOT must point to the iOS prebuilt deps dir — see ios/README.md")
endif ()
target_include_directories(sdrpp_core PUBLIC "${SDRPP_IOS_DEPS_ROOT}/include")
target_link_directories(sdrpp_core PUBLIC    "${SDRPP_IOS_DEPS_ROOT}/lib")
target_link_libraries(sdrpp_core PUBLIC zstd)

target_link_libraries(sdrpp_core PUBLIC volk_shim fftw_shim)
