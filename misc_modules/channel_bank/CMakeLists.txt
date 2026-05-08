cmake_minimum_required(VERSION 3.13)
project(channel_bank)

file(GLOB SRC "src/*.cpp")

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

# FFTW3F — pulled from android-sdr-kit on Android, system pkg-config elsewhere.
# Pattern matches core/CMakeLists.txt and the source_modules/* convention.
if (ANDROID)
    target_include_directories(channel_bank PRIVATE
        /sdr-kit/${ANDROID_ABI}/include
    )
    target_link_libraries(channel_bank PRIVATE
        /sdr-kit/${ANDROID_ABI}/lib/libfftw3f.so
    )
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
    add_custom_command(TARGET channel_bank POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DDYLIB=$<TARGET_FILE:channel_bank>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/fix_rpath.cmake
        COMMENT "Fixing channel_bank library paths for macOS bundle portability"
    )
endif()
