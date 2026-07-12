cmake_minimum_required(VERSION 3.13)
project(rx888_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

if(WIN32)
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
