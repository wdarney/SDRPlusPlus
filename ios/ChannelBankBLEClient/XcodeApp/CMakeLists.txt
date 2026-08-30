cmake_minimum_required(VERSION 3.25)

project(ChannelBankBLEClient LANGUAGES Swift)

set(CMAKE_Swift_LANGUAGE_VERSION 5)

set(CHANNEL_BANK_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(MACOSX_BUNDLE_GUI_IDENTIFIER "org.sdrpp.channelbankbleclient")
set(MACOSX_BUNDLE_BUNDLE_NAME "ChannelBankBLEClient")
set(MACOSX_BUNDLE_EXECUTABLE_NAME "ChannelBankBLEClient")
set(MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1")
set(MACOSX_BUNDLE_BUNDLE_VERSION "1")
set(CHANNEL_BANK_IOS_DEVELOPMENT_TEAM "7WP947RA97" CACHE STRING "Apple development team for iOS signing")

add_executable(ChannelBankBLEClient MACOSX_BUNDLE
    "${CHANNEL_BANK_CLIENT_ROOT}/Sources/ChannelBankCore/ChannelBankClient.swift"
    "${CHANNEL_BANK_CLIENT_ROOT}/Sources/ChannelBankCore/ChannelBankFramer.swift"
    "${CHANNEL_BANK_CLIENT_ROOT}/Sources/ChannelBankCore/ChannelBankModels.swift"
    "${CHANNEL_BANK_CLIENT_ROOT}/Sources/ChannelBankCore/RecordingPaginator.swift"
    "${CHANNEL_BANK_CLIENT_ROOT}/Sources/ChannelBankCore/WaterfallModel.swift"
    "${CHANNEL_BANK_CLIENT_ROOT}/Sources/ChannelBankBLEClientApp/BLECentralManager.swift"
    "${CHANNEL_BANK_CLIENT_ROOT}/Sources/ChannelBankBLEClientApp/ChannelBankBLEClientApp.swift"
    "${CHANNEL_BANK_CLIENT_ROOT}/Sources/ChannelBankBLEClientApp/ChannelBankRootView.swift"
    "${CHANNEL_BANK_CLIENT_ROOT}/Sources/ChannelBankBLEClientApp/ChannelBankViewModel.swift"
    "${CHANNEL_BANK_CLIENT_ROOT}/Sources/ChannelBankBLEClientApp/Charts.swift"
)

set_target_properties(ChannelBankBLEClient PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CHANNEL_BANK_CLIENT_ROOT}/Support/ChannelBankBLEClient-Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${MACOSX_BUNDLE_GUI_IDENTIFIER}"
    MACOSX_BUNDLE_BUNDLE_NAME "${MACOSX_BUNDLE_BUNDLE_NAME}"
    MACOSX_BUNDLE_EXECUTABLE_NAME "${MACOSX_BUNDLE_EXECUTABLE_NAME}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${MACOSX_BUNDLE_SHORT_VERSION_STRING}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${MACOSX_BUNDLE_BUNDLE_VERSION}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.sdrpp.channelbankbleclient"
    XCODE_ATTRIBUTE_PRODUCT_NAME "ChannelBankBLEClient"
    XCODE_ATTRIBUTE_INFOPLIST_KEY_CFBundleDisplayName "Channel Bank"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.0"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${CHANNEL_BANK_IOS_DEVELOPMENT_TEAM}"
    XCODE_ATTRIBUTE_SUPPORTS_MACCATALYST "NO"
    XCODE_ATTRIBUTE_SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD "NO"
)

add_custom_command(TARGET ChannelBankBLEClient PRE_BUILD
    COMMAND /usr/bin/xattr -cr "$<TARGET_BUNDLE_DIR:ChannelBankBLEClient>"
    COMMENT "Clearing extended attributes before iOS codesign"
)
