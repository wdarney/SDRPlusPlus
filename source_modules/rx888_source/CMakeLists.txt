cmake_minimum_required(VERSION 3.13)
project(rx888_source)

if(ANDROID)
    file(GLOB SRC "src/android_main.cpp")
else()
    file(GLOB SRC "src/main.cpp")
endif()

include(${SDRPP_MODULE_CMAKE})

if(ANDROID)
    file(GLOB SDDC_CORE_SRC
        "sddc_core/*.cpp"
        "sddc_core/radio/*.cpp"
        "sddc_core/pffft/*.cpp"
        "sddc_core/arch/android/*.cpp"
    )

    list(FILTER SDDC_CORE_SRC EXCLUDE REGEX "fft_mt_r2iq_avx.*")
    if(NOT ANDROID_ABI MATCHES "arm64-v8a|armeabi-v7a")
        list(FILTER SDDC_CORE_SRC EXCLUDE REGEX "fft_mt_r2iq_neon.cpp")
    endif()

    add_library(rx888_sddc_core STATIC ${SDDC_CORE_SRC})
    set_target_properties(rx888_sddc_core PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
    target_compile_definitions(rx888_sddc_core PUBLIC _CRT_SECURE_NO_WARNINGS)
    if(ANDROID_ABI MATCHES "arm64-v8a|armeabi-v7a")
        target_compile_definitions(rx888_sddc_core PRIVATE PFFFT_ENABLE_NEON)
        set_source_files_properties(sddc_core/pffft/pf_mixer.cpp PROPERTIES COMPILE_FLAGS "-Wno-strict-aliasing")
        if(ANDROID_ABI STREQUAL "armeabi-v7a")
            set_source_files_properties(sddc_core/fft_mt_r2iq_neon.cpp PROPERTIES COMPILE_FLAGS "-mfpu=neon-vfpv4")
            set_source_files_properties(sddc_core/pffft/pf_mixer.cpp APPEND_STRING PROPERTY COMPILE_FLAGS " -mfpu=neon-vfpv4")
        endif()
    else()
        target_compile_definitions(rx888_sddc_core PRIVATE NO_SIMD_OPTIM)
    endif()
    target_include_directories(rx888_sddc_core PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/sddc_core"
        "/sdr-kit/${ANDROID_ABI}/include"
    )
    target_link_libraries(rx888_sddc_core PUBLIC
        "/sdr-kit/${ANDROID_ABI}/lib/libusb1.0.so"
        "/sdr-kit/${ANDROID_ABI}/lib/libfftw3f.so"
    )

    target_include_directories(rx888_source PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "/sdr-kit/${ANDROID_ABI}/include"
    )
    target_link_libraries(rx888_source PRIVATE rx888_sddc_core)
elseif(WIN32)
    find_package(SoapySDR REQUIRED)
    target_include_directories(rx888_source PRIVATE ${SoapySDR_INCLUDE_DIRS})
    target_link_libraries(rx888_source PRIVATE SoapySDR)
else()
    find_package(PkgConfig)
    pkg_check_modules(SOAPY REQUIRED SoapySDR)
    target_include_directories(rx888_source PRIVATE ${SOAPY_INCLUDE_DIRS})
    target_link_directories(rx888_source PRIVATE ${SOAPY_LIBRARY_DIRS})
    target_link_libraries(rx888_source PRIVATE ${SOAPY_LIBRARIES})
endif()

# macOS: rewrite hardcoded pkg-config / MacPorts / Homebrew library paths
# to @rpath so the dylib is portable inside the .app bundle.
if(APPLE AND NOT ANDROID)
    add_custom_command(TARGET rx888_source POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DDYLIB=$<TARGET_FILE:rx888_source>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/fix_rpath.cmake
        COMMENT "Fixing rx888_source library paths for macOS bundle portability"
    )
endif()
