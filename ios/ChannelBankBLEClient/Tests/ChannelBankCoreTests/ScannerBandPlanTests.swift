import XCTest
@testable import ChannelBankCore

final class ScannerBandPlanTests: XCTestCase {
    private func settings(support: Bool = true) -> ChannelBankSettings {
        var result = ChannelBankSettings(bwUsage: 0.8, recordingEnabled: true)
        result.supportsScanRanges = support
        return result
    }

    func testAirbandUsesExistingScanModeOnNarrowSource() throws {
        let plan = try ScannerBandPlan(lowMHz: "118", highMHz: "137", sampleRate: 8_000_000, settings: settings(), airband: true)
        XCTAssertEqual(plan.mode, "scan")
        XCTAssertEqual(plan.stopCount, 3)
        XCTAssertEqual(plan.centerHz, 118_000_000 + 19_000_000 / 6.0, accuracy: 0.001)
        XCTAssertEqual(plan.range, ChannelBankScanRange(start: 118_000_000, stop: 137_000_000))
        let encoded = try JSONEncoder().encode(plan.settingsBody)
        let body = try XCTUnwrap(JSONSerialization.jsonObject(with: encoded) as? [String: Any])
        XCTAssertEqual(body["demodMode"] as? String, "AM")
        XCTAssertEqual(body["spacingId"] as? Int, 2)
        XCTAssertEqual((body["scanRanges"] as? [[String: Double]])?.first?["stop"], 137_000_000)
        XCTAssertNil(body["recordingEnabled"])
        XCTAssertNil(body["bwUsage"])
        XCTAssertNil(body["snrThresholdDb"])
    }

    func testWideSourceUsesAutoWithoutOverwritingSavedRanges() throws {
        let plan = try ScannerBandPlan(lowMHz: "118", highMHz: "137", sampleRate: 32_000_000, settings: settings(support: false), airband: true)
        XCTAssertEqual(plan.mode, "auto")
        XCTAssertEqual(plan.centerHz, 127_500_000)
        XCTAssertEqual(plan.stopCount, 1)
        XCTAssertNil(plan.settingsBody["scanRanges"])
    }

    func testExactFitUsesAutoAndCustomKeepsModulation() throws {
        let plan = try ScannerBandPlan(lowMHz: "118", highMHz: "137", sampleRate: 23_750_000, settings: settings(), airband: false)
        XCTAssertEqual(plan.mode, "auto")
        XCTAssertNil(plan.settingsBody["demodMode"])
        XCTAssertNil(plan.settingsBody["spacingId"])
    }

    func testUnsupportedOrInvalidInputsAreRejected() {
        XCTAssertThrowsError(try ScannerBandPlan(lowMHz: "118", highMHz: "137", sampleRate: 8_000_000, settings: settings(support: false), airband: true))
        for rate in [nil, 0, -1, Double.nan, Double.infinity, 1] as [Double?] {
            XCTAssertThrowsError(try ScannerBandPlan(lowMHz: "118", highMHz: "137", sampleRate: rate, settings: settings(), airband: true))
        }
        for (low, high) in [("nan", "137"), ("118", "inf"), ("137", "118"), ("0", "137"), ("118", "118")] {
            XCTAssertThrowsError(try ScannerBandPlan(lowMHz: low, highMHz: high, sampleRate: 8_000_000, settings: settings(), airband: true))
        }
    }

    func testNewServerSettingsDecodeAndSurviveSummaryMerge() throws {
        let json = #"{"settings":{"supportsScanRanges":true,"scanRanges":[{"start":118000000,"stop":137000000}],"bwUsage":0.8}}"#
        var state = try JSONDecoder().decode(ChannelBankState.self, from: Data(json.utf8))
        state.merge(summary: ChannelBankStateSummary(running: false))
        XCTAssertEqual(state.settings?.supportsScanRanges, true)
        XCTAssertEqual(state.settings?.scanRanges?.first?.start, 118_000_000)
        let legacy = try JSONDecoder().decode(ChannelBankSettings.self, from: Data("{}".utf8))
        XCTAssertNil(legacy.supportsScanRanges)
        let cleared = try JSONDecoder().decode(ChannelBankSettings.self, from: Data(#"{"scanRanges":[]}"#.utf8))
        XCTAssertEqual(cleared.scanRanges, [])
    }
}
