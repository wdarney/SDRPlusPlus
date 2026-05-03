cmake_minimum_required(VERSION 3.18)

# Defines the iOS app bundle target. Configure with the Xcode generator and an
# iOS toolchain — see ios/build.sh.

set(APP_SHELL_SRC
    AppShell/AppDelegate.mm
    AppShell/ViewController.mm
)

add_executable(SDRPP_iOS MACOSX_BUNDLE ${APP_SHELL_SRC})

target_link_libraries(SDRPP_iOS PRIVATE sdrpp_app)

target_include_directories(SDRPP_iOS PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/backends/ios"
)

set_target_properties(SDRPP_iOS PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/AppShell/Info.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.sdrpp.client"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "14.0"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "iPhone Developer"
)

# Bundle the /res tree so ViewController.mm can copy it on first launch.
# CMake's MACOSX_PACKAGE_LOCATION on a directory was a no-op; instead use a
# post-build copy. cp -R lands the entire bandplans/colormaps/fonts/icons/
# themes tree under .app/res, which is where copyBundleResources: looks.
add_custom_command(TARGET SDRPP_iOS POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/root/res"
            "$<TARGET_BUNDLE_DIR:SDRPP_iOS>/res"
    COMMENT "Copying SDR++ resources (bandplans/colormaps/fonts/icons/themes)"
)
