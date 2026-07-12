cmake_minimum_required(VERSION 3.13)

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    include("${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.ios.cmake")
    return()
endif ()

project(channel_bank)

file(GLOB SRC "src/*.cpp")
list(FILTER SRC EXCLUDE REGEX "src/main_ios\\.cpp$")
if (APPLE AND NOT ANDROID)
    list(APPEND SRC src/transcription.mm src/encoding.mm src/transcription_whisper.mm)
    set_source_files_properties(src/transcription.mm src/encoding.mm src/transcription_whisper.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
endif()

# RNNoise (Mozilla) — neural network noise suppression.
# Disabled on MSVC: the vendored C sources use VLAs (C99) which MSVC doesn't support.
if (NOT MSVC)
    set(RNNOISE_SRC
        src/rnnoise/src/denoise.c
        src/rnnoise/src/kiss_fft.c
        src/rnnoise/src/pitch.c
        src/rnnoise/src/celt_lpc.c
        src/rnnoise/src/rnn.c
        src/rnnoise/src/rnn_data.c
    )
    list(APPEND SRC ${RNNOISE_SRC})
endif()

# whisper.cpp — in-process speech-to-text (vendored under external/whisper.cpp).
# macOS: Metal + Accelerate.  Windows: CPU-only (no CUDA/Vulkan — Intel iGPU only).
if (APPLE AND NOT ANDROID)
    set(BUILD_SHARED_LIBS OFF)
    set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    set(WHISPER_CURL           OFF CACHE BOOL "" FORCE)
    set(WHISPER_SDL2           OFF CACHE BOOL "" FORCE)
    set(WHISPER_COREML         OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
    set(GGML_METAL             ON  CACHE BOOL "" FORCE)
    set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
    set(GGML_OPENMP            OFF CACHE BOOL "" FORCE)
    set(GGML_ACCELERATE        ON  CACHE BOOL "" FORCE)
    set(WHISPER_ALL_WARNINGS_3RD_PARTY OFF CACHE BOOL "" FORCE)
    set(GGML_ALL_WARNINGS_3RD_PARTY    OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/whisper.cpp EXCLUDE_FROM_ALL)
elseif (MSVC)
    set(BUILD_SHARED_LIBS OFF)
    set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    set(WHISPER_CURL           OFF CACHE BOOL "" FORCE)
    set(WHISPER_SDL2           OFF CACHE BOOL "" FORCE)
    set(WHISPER_COREML         OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
    set(GGML_METAL             OFF CACHE BOOL "" FORCE)
    set(GGML_CUDA              OFF CACHE BOOL "" FORCE)
    set(GGML_VULKAN            OFF CACHE BOOL "" FORCE)
    set(GGML_OPENMP            OFF CACHE BOOL "" FORCE)
    set(GGML_ACCELERATE        OFF CACHE BOOL "" FORCE)
    set(WHISPER_ALL_WARNINGS_3RD_PARTY OFF CACHE BOOL "" FORCE)
    set(GGML_ALL_WARNINGS_3RD_PARTY    OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/whisper.cpp EXCLUDE_FROM_ALL)
endif()

include(${SDRPP_MODULE_CMAKE})

# Cross-module FM interface
target_include_directories(channel_bank PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../frequency_manager/src
)

if (NOT MSVC)
    target_include_directories(channel_bank PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/rnnoise/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/rnnoise/src
    )
    set_source_files_properties(${RNNOISE_SRC} PROPERTIES
        LANGUAGE C
        COMPILE_FLAGS "-w -std=c11"
        SKIP_PRECOMPILE_HEADERS ON
    )
else()
    target_compile_definitions(channel_bank PRIVATE CB_NO_RNNOISE)
endif()

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
elseif (MSVC)
    target_link_libraries(channel_bank PRIVATE
        whisper
        shell32
    )
    target_include_directories(channel_bank PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/whisper.cpp/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/whisper.cpp/ggml/include
    )
endif()
