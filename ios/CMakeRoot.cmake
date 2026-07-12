cmake_minimum_required(VERSION 3.18)
project(sdrpp)

# This is the iOS fork of SDR++. Single build mode: iOS-only, statically
# linked, client modules only. See ios/README.md.

# Modules are built into the main binary and selected here. Defaults match the
# "useful for a remote-server iOS client" set; flip OFF to slim further.
option(OPT_BUILD_FILE_SOURCE          "WAV file source"                                OFF)
option(OPT_BUILD_NETWORK_SOURCE       "Raw IQ over TCP/UDP"                            OFF)
option(OPT_BUILD_RTL_TCP_SOURCE       "rtl_tcp protocol"                               OFF)
option(OPT_BUILD_SDRPP_SERVER_SOURCE  "SDR++ server protocol"                          ON)
option(OPT_BUILD_SPECTRAN_HTTP_SOURCE "Aaronia Spectran HTTP source"                   OFF)
option(OPT_BUILD_SPYSERVER_SOURCE     "SpyServer protocol"                             OFF)

option(OPT_BUILD_NETWORK_SINK         "UDP/TCP audio sink"                             OFF)
option(OPT_BUILD_COREAUDIO_SINK       "CoreAudio sink (iOS/macOS)"                     ON)

option(OPT_BUILD_METEOR_DEMODULATOR   "Meteor weather satellite demodulator"           OFF)
option(OPT_BUILD_PAGER_DECODER        "POCSAG/FLEX pager decoder"                      OFF)
option(OPT_BUILD_RADIO                "Main AM/FM/SSB demodulator"                     ON)

option(OPT_BUILD_CHANNEL_BANK         "SNR-triggered multi-channel recorder"           ON)
option(OPT_BUILD_FREQUENCY_MANAGER    "Frequency bookmarks"                            ON)
option(OPT_BUILD_IQ_EXPORTER          "IQ recorder/exporter"                           OFF)
option(OPT_BUILD_RECORDER             "Audio + baseband recorder"                      OFF)
option(OPT_BUILD_RIGCTL_CLIENT        "Rigctl client (panadapter)"                     OFF)
option(OPT_BUILD_RIGCTL_SERVER        "Rigctl backend"                                 OFF)
option(OPT_BUILD_SCANNER              "Frequency scanner"                              OFF)

option(USE_INTERNAL_LIBCORRECT        "Use the bundled libcorrect"                     ON)

set(SDRPP_MODULE_CMAKE "${CMAKE_SOURCE_DIR}/ios/sdrpp_module_ios.cmake")
set(SDRPP_CORE_ROOT    "${CMAKE_SOURCE_DIR}/core/src/")

# Compiler flags. The fork only ever targets clang on Apple platforms.
if (${CMAKE_BUILD_TYPE} MATCHES "Debug")
    set(SDRPP_COMPILER_FLAGS -g -Og -std=c++17 -Wno-unused-command-line-argument)
else ()
    set(SDRPP_COMPILER_FLAGS -O3   -std=c++17 -Wno-unused-command-line-argument)
endif ()
set(SDRPP_MODULE_COMPILER_FLAGS ${SDRPP_COMPILER_FLAGS})

# ---- Accelerate-backed shims (must come before core) --------------------
add_subdirectory("ios/volk_shim")
add_subdirectory("ios/fftw_shim")

# ---- Core ----------------------------------------------------------------
add_subdirectory("core")

# ---- Modules -------------------------------------------------------------
macro(_sdrpp_maybe_add opt subdir)
    if (${opt})
        add_subdirectory("${subdir}")
    endif ()
endmacro()

_sdrpp_maybe_add(OPT_BUILD_FILE_SOURCE          "source_modules/file_source")
_sdrpp_maybe_add(OPT_BUILD_NETWORK_SOURCE       "source_modules/network_source")
_sdrpp_maybe_add(OPT_BUILD_RTL_TCP_SOURCE       "source_modules/rtl_tcp_source")
_sdrpp_maybe_add(OPT_BUILD_SDRPP_SERVER_SOURCE  "source_modules/sdrpp_server_source")
_sdrpp_maybe_add(OPT_BUILD_SPECTRAN_HTTP_SOURCE "source_modules/spectran_http_source")
_sdrpp_maybe_add(OPT_BUILD_SPYSERVER_SOURCE     "source_modules/spyserver_source")

_sdrpp_maybe_add(OPT_BUILD_NETWORK_SINK         "sink_modules/network_sink")
_sdrpp_maybe_add(OPT_BUILD_COREAUDIO_SINK       "sink_modules/coreaudio_sink")

_sdrpp_maybe_add(OPT_BUILD_METEOR_DEMODULATOR   "decoder_modules/meteor_demodulator")
_sdrpp_maybe_add(OPT_BUILD_PAGER_DECODER        "decoder_modules/pager_decoder")
_sdrpp_maybe_add(OPT_BUILD_RADIO                "decoder_modules/radio")

_sdrpp_maybe_add(OPT_BUILD_CHANNEL_BANK         "misc_modules/channel_bank")
_sdrpp_maybe_add(OPT_BUILD_FREQUENCY_MANAGER    "misc_modules/frequency_manager")
_sdrpp_maybe_add(OPT_BUILD_IQ_EXPORTER          "misc_modules/iq_exporter")
_sdrpp_maybe_add(OPT_BUILD_RECORDER             "misc_modules/recorder")
_sdrpp_maybe_add(OPT_BUILD_RIGCTL_CLIENT        "misc_modules/rigctl_client")
_sdrpp_maybe_add(OPT_BUILD_RIGCTL_SERVER        "misc_modules/rigctl_server")
_sdrpp_maybe_add(OPT_BUILD_SCANNER              "misc_modules/scanner")

# ---- Generate the static-modules registry --------------------------------
get_property(_sdrpp_static_mods GLOBAL PROPERTY SDRPP_STATIC_MODULE_TARGETS)
set(SDRPP_STATIC_MODULE_DECLS "")
set(SDRPP_STATIC_MODULE_REGS  "")
foreach (_m IN LISTS _sdrpp_static_mods)
    string(APPEND SDRPP_STATIC_MODULE_DECLS "SDRPP_DECLARE_STATIC_MOD(${_m}_)\n")
    string(APPEND SDRPP_STATIC_MODULE_REGS  "    SDRPP_REGISTER_STATIC_MOD(${_m}_)\n")
endforeach ()
configure_file(
    "${CMAKE_SOURCE_DIR}/core/src/static_modules.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/static_modules.cpp"
    @ONLY
)

# ---- App library ---------------------------------------------------------
# The Xcode app under ios/ links this static lib and calls sdrpp_main().
set(SDRPP_APP_SRC
    "src/main.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/static_modules.cpp"
)
set(SDRPP_APP_OBJECTS "")
foreach (_m IN LISTS _sdrpp_static_mods)
    list(APPEND SDRPP_APP_OBJECTS "$<TARGET_OBJECTS:${_m}>")
endforeach ()

add_library(sdrpp_app STATIC ${SDRPP_APP_SRC} ${SDRPP_APP_OBJECTS})
target_link_libraries(sdrpp_app PUBLIC sdrpp_core)
target_compile_options(sdrpp_app PRIVATE ${SDRPP_COMPILER_FLAGS})

# Module cmake file (for out-of-tree modules, if anyone wants to write one).
configure_file(${CMAKE_SOURCE_DIR}/sdrpp_module.cmake ${CMAKE_CURRENT_BINARY_DIR}/sdrpp_module.cmake @ONLY)

# ---- iOS app bundle ------------------------------------------------------
# Only added when actually targeting iOS — the AppShell files import UIKit,
# which doesn't exist in the host macOS SDK. Host builds stop at sdrpp_app.
if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_subdirectory(ios)
endif ()
