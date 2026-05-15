cmake_minimum_required(VERSION 3.13)
project(channel_bank)

file(GLOB SRC "src/*.cpp")

# RNNoise (Mozilla) — neural network noise suppression (bundled)
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

# RNNoise headers (frequency_manager_interface.h is copied into src/ directly)
target_include_directories(channel_bank PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rnnoise/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rnnoise/src
)
# Force C compilation and suppress warnings for third-party RNNoise sources
set_source_files_properties(${RNNOISE_SRC} PROPERTIES
    LANGUAGE C
    COMPILE_FLAGS "-w -std=c11"
    SKIP_PRECOMPILE_HEADERS ON
)

# On iOS, fftw_shim is linked publicly by sdrpp_core, so FFTW headers and
# the Accelerate-backed implementation are already available transitively.
# Skip pkg-config lookup which would fail without a system FFTW install.
if (NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    find_package(PkgConfig)
    pkg_check_modules(FFTW3F REQUIRED fftw3f)
    target_link_libraries(channel_bank PRIVATE ${FFTW3F_LIBRARIES})
    target_include_directories(channel_bank PRIVATE ${FFTW3F_INCLUDE_DIRS})
else()
    # Expose ios_backend.h so playbackWavFile can call iosPlayRecordingFile()
    # (AVAudioPlayer bridge) instead of the broken second CoreAudio path.
    target_include_directories(channel_bank PRIVATE
        "${CMAKE_SOURCE_DIR}/core/backends/ios"
    )
endif()
