import XCTest
@testable import ChannelBankCore

final class ScannerTuningRangeTests: XCTestCase {
    func testRangeMapsToExistingCenterAndBandwidthSettings() throws {
        let range = try ScannerTuningRange(lowMHz: " 154 ", highMHz: "160", sampleRate: 8_000_000)
        XCTAssertEqual(range.centerHz, 157_000_000)
        XCTAssertEqual(range.bandwidthUsage, 0.75)
        XCTAssertEqual(try ScannerTuningRange(lowMHz: "154", highMHz: "158", sampleRate: 8_000_000).bandwidthUsage, 0.5)
        XCTAssertEqual(try ScannerTuningRange(lowMHz: "154", highMHz: "162", sampleRate: 8_000_000).bandwidthUsage, 1)
    }

    func testInvalidRangesDoNotProduceCommands() {
        for (low, high) in [("", "160"), ("nan", "160"), ("154", "inf"), ("0", "4"), ("160", "154"), ("154", "154"), ("154", "155"), ("154", "170")] {
            XCTAssertThrowsError(try ScannerTuningRange(lowMHz: low, highMHz: high, sampleRate: 8_000_000))
        }
        for rate in [nil, 0, -.infinity, .nan] as [Double?] {
            XCTAssertThrowsError(try ScannerTuningRange(lowMHz: "154", highMHz: "160", sampleRate: rate))
        }
    }
}
