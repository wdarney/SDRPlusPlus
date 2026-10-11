if(NOT TARGET sdrpp_rtlsdr_blog)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../vendor/rtl-sdr-blog
        ${CMAKE_BINARY_DIR}/vendor/rtl-sdr-blog)
endif()
