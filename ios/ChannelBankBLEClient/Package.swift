// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "ChannelBankBLEClient",
    platforms: [
        .iOS(.v16),
        .macOS(.v13)
    ],
    products: [
        .library(name: "ChannelBankCore", targets: ["ChannelBankCore"]),
        .executable(name: "ChannelBankBLEClientApp", targets: ["ChannelBankBLEClientApp"])
    ],
    targets: [
        .target(name: "ChannelBankCore"),
        .executableTarget(
            name: "ChannelBankBLEClientApp",
            dependencies: ["ChannelBankCore"]
        ),
        .testTarget(
            name: "ChannelBankCoreTests",
            dependencies: ["ChannelBankCore"]
        )
    ]
)
