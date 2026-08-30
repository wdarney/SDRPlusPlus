# Xcode App Wrapper

This CMake entry point generates an installable Xcode iOS app target from the
Swift sources in the sibling package.

```sh
cmake -S XcodeApp -B build-xcode-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
xcodebuild -project build-xcode-ios/ChannelBankBLEClient.xcodeproj -scheme ChannelBankBLEClient -destination generic/platform=iOS CODE_SIGNING_ALLOWED=NO build
```

For device installation, open the generated project, select a signing team, and
run the `ChannelBankBLEClient` scheme on the paired iPhone. If you need a
different Apple team, regenerate with
`-DCHANNEL_BANK_IOS_DEVELOPMENT_TEAM=YOURTEAMID`; Xcode edits to generated
projects can be overwritten by CMake's regeneration step.
