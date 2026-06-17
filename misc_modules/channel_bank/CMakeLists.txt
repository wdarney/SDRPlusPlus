cmake_minimum_required(VERSION 3.13)
project(channel_bank)

file(GLOB SRC "src/*.cpp")
if (APPLE AND NOT ANDROID)
    list(APPEND SRC src/transcription.mm src/encoding.mm src/transcription_whisper.mm)
    set_source_files_properties(src/transcription.mm src/encoding.mm src/transcription_whisper.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
endif()

# RNNoise (Mozilla) — neural network noise suppression
set(RNNOISE_SRC
    src/rnnoise/src/denoise.c
    src/rnnoise/src/kiss_fft.c
    src/rnnoise/src/pitch.c
    src/rnnoise/src/celt_lpc.c
    src/rnnoise/src/rnn.c
    src/rnnoise/src/rnn_data.c
)
list(APPEND SRC ${RNNOISE_SRC})

# whisper.cpp — in-process speech-to-text (vendored under external/whisper.cpp).
# Built only on Apple for now (Metal acceleration); could be extended to Linux
# (CPU/CUDA/Vulkan) later by relaxing this guard.  Drives the new transcription
# backend in src/transcription_whisper.mm.
if (APPLE AND NOT ANDROID)
    # Force STATIC linking for whisper + ggml so everything collapses into the
    # channel_bank.dylib at link time and deployment stays one file.  Without this
    # whisper.cpp builds six separate .dylibs that we'd then have to bundle in the
    # .app, rpath-rewrite, and re-sign individually.  Set as a non-cache scope
    # variable so it ONLY affects the add_subdirectory call below — other SDR++
    # modules / the core build are unaffected.
    set(BUILD_SHARED_LIBS OFF)
    # Don't propagate whisper.cpp's options to the SDR++ root project.
    set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    set(WHISPER_CURL           OFF CACHE BOOL "" FORCE) # we use NSURLSession for downloads
    set(WHISPER_SDL2           OFF CACHE BOOL "" FORCE)
    set(WHISPER_COREML         OFF CACHE BOOL "" FORCE) # could enable later; Metal is plenty on M-series
    set(GGML_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
    set(GGML_METAL             ON  CACHE BOOL "" FORCE)
    set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)   # bake Metal shaders into the dylib
    set(GGML_OPENMP            OFF CACHE BOOL "" FORCE)    # Apple Clang ships no libomp
    set(GGML_ACCELERATE        ON  CACHE BOOL "" FORCE)    # use Apple's Accelerate BLAS
    # whisper.cpp 3rd-party warnings get noisy; we don't want them gating the build.
    set(WHISPER_ALL_WARNINGS_3RD_PARTY OFF CACHE BOOL "" FORCE)
    set(GGML_ALL_WARNINGS_3RD_PARTY    OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/whisper.cpp EXCLUDE_FROM_ALL)
endif()

include(${SDRPP_MODULE_CMAKE})

# RNNoise + cross-module FM interface
target_include_directories(channel_bank PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rnnoise/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rnnoise/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../frequency_manager/src
)
# Force C compilation and suppress warnings for third-party RNNoise sources
set_source_files_properties(${RNNOISE_SRC} PROPERTIES
    LANGUAGE C
    COMPILE_FLAGS "-w -std=c11"
    SKIP_PRECOMPILE_HEADERS ON
)

# FFTW3F — matches the per-platform pattern in core/CMakeLists.txt.
if (ANDROID)
    target_include_directories(channel_bank PRIVATE
        /sdr-kit/${ANDROID_ABI}/include
    )
    target_link_libraries(channel_bank PRIVATE
        /sdr-kit/${ANDROID_ABI}/lib/libfftw3f.so
    )
elseif (MSVC)
    find_package(FFTW3f CONFIG REQUIRED)
    target_link_libraries(channel_bank PRIVATE FFTW3::fftw3f)
else()
    find_package(PkgConfig)
    pkg_check_modules(FFTW3F REQUIRED fftw3f)
    target_link_libraries(channel_bank PRIVATE ${FFTW3F_LIBRARIES})
    target_include_directories(channel_bank PRIVATE ${FFTW3F_INCLUDE_DIRS})
endif()

# macOS: rewrite any hardcoded pkg-config / MacPorts / Homebrew library paths
# to @rpath so the dylib is portable inside the .app bundle. The main sdrpp
# executable exposes @loader_path/../Frameworks via LC_RPATH, which all
# plugins inherit at load time. Uses a cmake -P script to avoid shell escaping.
if (APPLE AND NOT ANDROID)
    target_link_libraries(channel_bank PRIVATE
        "-framework Speech"
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework Foundation"
        "-framework AppKit"
        "-framework Metal"
        "-framework MetalKit"
        "-lobjc"
        # whisper.cpp + ggml (Metal-accelerated)
        whisper
    )
    target_include_directories(channel_bank PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/whisper.cpp/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/whisper.cpp/ggml/include
    )
    add_custom_command(TARGET channel_bank POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DDYLIB=$<TARGET_FILE:channel_bank>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/fix_rpath.cmake
        COMMENT "Fixing channel_bank library paths for macOS bundle portability"
    )
endif()
