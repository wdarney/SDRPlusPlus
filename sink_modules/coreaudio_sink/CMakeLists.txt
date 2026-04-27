cmake_minimum_required(VERSION 3.13)
project(coreaudio_sink)

file(GLOB SRC "src/*.cpp" "src/*.mm")

include(${SDRPP_MODULE_CMAKE})

target_link_libraries(coreaudio_sink PRIVATE
    "-framework AudioToolbox"
    "-framework AVFoundation"
    "-framework CoreAudio"
    "-framework Foundation"
)
