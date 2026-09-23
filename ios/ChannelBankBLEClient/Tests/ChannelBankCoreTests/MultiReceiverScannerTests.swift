import XCTest
@testable import ChannelBankCore

final class MultiReceiverScannerTests: XCTestCase {
    private func state() throws -> ChannelBankState {
        try JSONDecoder().decode(ChannelBankState.self, from: Data(#"""
        {"running":false,"radioPlaying":false,"selectedSource":"RX888","sources":["RX888","Airspy","RTL-SDR"],
         "settings":{"mode":"multi_receiver_scan","discoveryReceiver":"RX888","transmissionReceiverPool":["Airspy","RTL-SDR"],"scanRanges":[{"start":118000000,"stop":137000000}],"maximumMonitorSec":30,"scanSettleMs":500,"recordingEnabled":true},
         "receiverControls":{"RX888":{"available":false},"Airspy":{"available":true,"gains":[{"name":"VGA","value":8}]}},
         "multiReceiverScan":{"discoveryReceiver":"RX888","discoveryState":"SCANNING","currentDiscoveryHz":121000000,"receivers":[{"id":"Airspy","available":true,"state":"ACTIVE","channels":[121500000],"channelDetails":[{"gridHz":121500000,"tunedHz":121500000,"held":true,"signalPresent":true,"recording":true,"releasing":false}]}],"dispatches":[{"frequencyHz":121500000,"receiverId":"Airspy","state":"ACTIVE"}],"noCapacityCount":2,"failureCount":0}}
        """#.utf8))
    }

    func testFullStateRoundTripAndSummaryPreservesReceiverActivity() throws {
        let original = try state()
        XCTAssertFalse(original.isSummaryShaped)
        XCTAssertEqual(original.receiverControls?["Airspy"]?.gains?.first?.value, 8)
        XCTAssertEqual(original.multiReceiverScan?.receivers?.first?.channelDetails?.first?.recording, true)
        let encoded = try JSONEncoder().encode(original)
        XCTAssertEqual(try JSONDecoder().decode(ChannelBankState.self, from: encoded), original)
        var merged = original
        merged.merge(summary: ChannelBankStateSummary(running: true))
        XCTAssertEqual(merged.multiReceiverScan, original.multiReceiverScan)
        XCTAssertEqual(merged.settings?.transmissionReceiverPool, ["Airspy", "RTL-SDR"])
        var delayed = ChannelBankState()
        delayed.settings = ChannelBankSettings()
        delayed.mergeTelemetry(from: original)
        XCTAssertEqual(delayed.receiverControls, original.receiverControls)
        XCTAssertEqual(delayed.multiReceiverScan, original.multiReceiverScan)
        XCTAssertEqual(delayed.settings?.transmissionReceiverPool, ["Airspy", "RTL-SDR"])
    }

    func testOrderedPoolAndHzRangesPreserveOtherPreferences() throws {
        let state = try state()
        var draft = MultiReceiverScannerDraft(settings: try XCTUnwrap(state.settings), selectedSource: state.selectedSource)
        draft.pool.swapAt(0, 1)
        let body = try draft.requestBody(state: state)
        XCTAssertEqual(body["transmissionReceiverPool"]?.value, .array([.string("RTL-SDR"), .string("Airspy")]))
        XCTAssertEqual(body["scanRanges"]?.value, .array([.object(["start": .number(118000000), "stop": .number(137000000)])]))
        XCTAssertEqual(body["mode"]?.value, .string("multi_receiver_scan"))
        XCTAssertNil(body["discoveryReceiver"])
        XCTAssertNil(body["recordingEnabled"])
        XCTAssertNil(body["demodMode"])
        draft.ranges = []
        XCTAssertEqual(try draft.requestBody(state: state)["scanRanges"]?.value, .array([]))
    }

    func testStopGuardsAndInvalidDrafts() throws {
        var state = try state()
        var draft = MultiReceiverScannerDraft(settings: try XCTUnwrap(state.settings), selectedSource: state.selectedSource)
        state.radioPlaying = true
        XCTAssertNoThrow(try draft.requestBody(state: state))
        draft.discovery = "Airspy"
        draft.pool = ["RTL-SDR"]
        XCTAssertThrowsError(try draft.requestBody(state: state))
        state.radioPlaying = false
        XCTAssertEqual(try draft.requestBody(state: state)["discoveryReceiver"]?.value, .string("Airspy"))
        state.running = true
        XCTAssertThrowsError(try draft.requestBody(state: state))
        state.running = false
        draft.pool = ["RTL-SDR", "RTL-SDR"]
        XCTAssertThrowsError(try draft.requestBody(state: state))
        draft.pool = ["RTL-SDR"]
        draft.ranges = [.init(start: .nan, stop: 137000000)]
        XCTAssertThrowsError(try draft.requestBody(state: state))
        draft.ranges = []
        draft.scanSettleMs = 249
        XCTAssertThrowsError(try draft.requestBody(state: state))
        draft.scanSettleMs = 2000
        draft.maximumMonitorSec = .infinity
        XCTAssertThrowsError(try draft.requestBody(state: state))
    }
}
