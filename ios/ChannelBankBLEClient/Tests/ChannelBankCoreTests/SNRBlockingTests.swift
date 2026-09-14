import XCTest
@testable import ChannelBankCore

final class SNRBlockingTests: XCTestCase {
    func testBlockedHistorySurvivesMissingOrUnflaggedSNRSamples() throws {
        let json = #"{"history":[{"freqHz":155475100,"blocked":true},{"freqHz":155500000,"blocked":false}],"snrOverview":[{"freqHz":155475000,"snrDb":0,"blocked":false}]}"#
        var state = try JSONDecoder().decode(ChannelBankState.self, from: Data(json.utf8))
        XCTAssertEqual(state.snrBlockedFrequencyKeys, [155475])
        state.snrOverview = []
        XCTAssertEqual(state.snrBlockedFrequencyKeys, [155475])
        state.history = []
        XCTAssertTrue(state.snrBlockedFrequencyKeys.isEmpty)
    }

    func testTelemetryFlagsWorkWithoutHistoryAndIgnoreInvalidFrequencies() throws {
        let json = #"{"snrOverview":[{"freqHz":155475000,"snrDb":-5,"blocked":true},{"freqHz":155500000,"snrDb":30,"blocked":false},{"freqHz":0,"snrDb":0,"blocked":true}]}"#
        let state = try JSONDecoder().decode(ChannelBankState.self, from: Data(json.utf8))
        XCTAssertEqual(state.snrBlockedFrequencyKeys, [155475])
    }
}
