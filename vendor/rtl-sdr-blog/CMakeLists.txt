cmake_minimum_required(VERSION 3.13)
project(sdrpp_rtlsdr_blog LANGUAGES C)

add_library(sdrpp_rtlsdr_blog STATIC
    src/librtlsdr.c src/tuner_e4k.c src/tuner_fc0012.c
    src/tuner_fc0013.c src/tuner_fc2580.c src/tuner_r82xx.c)
set_target_properties(sdrpp_rtlsdr_blog PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_compile_definitions(sdrpp_rtlsdr_blog PUBLIC rtlsdr_STATIC)
target_include_directories(sdrpp_rtlsdr_blog BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(ANDROID)
    target_include_directories(sdrpp_rtlsdr_blog PRIVATE /sdr-kit/${ANDROID_ABI}/include /sdr-kit/${ANDROID_ABI}/include/libusb-1.0)
    target_link_libraries(sdrpp_rtlsdr_blog PUBLIC /sdr-kit/${ANDROID_ABI}/lib/libusb1.0.so)
elseif(MSVC)
    find_path(SDRPP_RTL_USB_INCLUDE libusb.h PATH_SUFFIXES libusb-1.0
        HINTS "C:/Program Files/PothosSDR/include")
    find_library(SDRPP_RTL_USB_LIBRARY NAMES usb-1.0 libusb-1.0
        HINTS "C:/Program Files/PothosSDR/lib")
    if(NOT SDRPP_RTL_USB_INCLUDE OR NOT SDRPP_RTL_USB_LIBRARY)
        message(FATAL_ERROR "RTL-SDR Blog requires libusb headers and library")
    endif()
    target_include_directories(sdrpp_rtlsdr_blog PRIVATE ${SDRPP_RTL_USB_INCLUDE})
    target_link_libraries(sdrpp_rtlsdr_blog PUBLIC ${SDRPP_RTL_USB_LIBRARY})
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDRPP_RTL_USB REQUIRED IMPORTED_TARGET libusb-1.0)
    target_link_libraries(sdrpp_rtlsdr_blog PUBLIC PkgConfig::SDRPP_RTL_USB m)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(sdrpp_rtlsdr_blog PRIVATE DETACH_KERNEL_DRIVER)
    endif()
endif()

option(SDRPP_RTL_LINK_CHECK "Build a hardware-free driver link check" OFF)
if(SDRPP_RTL_LINK_CHECK)
    enable_testing()
    add_executable(rtl_blog_link_check link_check.c)
    target_link_libraries(rtl_blog_link_check PRIVATE sdrpp_rtlsdr_blog)
    add_test(NAME rtl_blog_link_check COMMAND rtl_blog_link_check)
endif()
