import XCTest
@testable import ChannelBankCore

final class ChannelBankFramerTests: XCTestCase {
    func testLittleEndianFrameEncoding() throws {
        let frame = ChannelBankFrame(flags: 3, messageID: 0x1234, offset: 0x78563412, payload: Data([0xaa]))
        XCTAssertEqual([UInt8](frame.encode()), [1, 3, 0x34, 0x12, 0x12, 0x34, 0x56, 0x78, 0xaa])
    }

    func testSingleFrameMessage() throws {
        let frames = try ChannelBankFramer(maximumFrameLength: 64).frames(for: Data("{}".utf8), messageID: 42)
        XCTAssertEqual(frames.count, 1)
        let frame = try ChannelBankFrame(data: frames[0])
        XCTAssertTrue(frame.isFirst)
        XCTAssertTrue(frame.isLast)
        XCTAssertEqual(frame.messageID, 42)
        XCTAssertEqual(frame.payload, Data("{}".utf8))
    }

    func testMultiFrameFragmentationAndReassembly() throws {
        let payload = Data("abcdefghijklmnopqrstuvwxyz".utf8)
        let frames = try ChannelBankFramer(maximumFrameLength: 13).frames(for: payload, messageID: 7)
        XCTAssertGreaterThan(frames.count, 1)

        let assembler = ChannelBankFrameAssembler()
        var complete: Data?
        for frame in frames {
            complete = try assembler.push(frame)?.payload
        }
        XCTAssertEqual(complete, payload)
    }

    func testInterleavedMessageIDs() throws {
        let framer = ChannelBankFramer(maximumFrameLength: 12)
        let a = try framer.frames(for: Data("abcdefgh".utf8), messageID: 1)
        let b = try framer.frames(for: Data("wxyz1234".utf8), messageID: 2)
        let assembler = ChannelBankFrameAssembler()

        XCTAssertNil(try assembler.push(a[0]))
        XCTAssertNil(try assembler.push(b[0]))
        XCTAssertEqual(try assembler.push(a[1])?.payload, Data("abcdefgh".utf8))
        XCTAssertEqual(try assembler.push(b[1])?.payload, Data("wxyz1234".utf8))
    }

    func testInvalidOrDiscontinuousOffsets() throws {
        let first = ChannelBankFrame(flags: ChannelBankFrame.firstFlag, messageID: 9, offset: 0, payload: Data("abc".utf8)).encode()
        let gap = ChannelBankFrame(flags: ChannelBankFrame.lastFlag, messageID: 9, offset: 4, payload: Data("z".utf8)).encode()
        let assembler = ChannelBankFrameAssembler()
        XCTAssertNil(try assembler.push(first))
        XCTAssertThrowsError(try assembler.push(gap)) { error in
            XCTAssertEqual(error as? ChannelBankFrameError, .discontinuousOffset(expected: 3, got: 4))
        }
    }

    func testReplacementFirstFrame() throws {
        let firstA = ChannelBankFrame(flags: ChannelBankFrame.firstFlag, messageID: 3, offset: 0, payload: Data("old".utf8)).encode()
        let firstB = ChannelBankFrame(flags: ChannelBankFrame.firstFlag, messageID: 3, offset: 0, payload: Data("new".utf8)).encode()
        let lastB = ChannelBankFrame(flags: ChannelBankFrame.lastFlag, messageID: 3, offset: 3, payload: Data("!".utf8)).encode()
        let assembler = ChannelBankFrameAssembler()
        XCTAssertNil(try assembler.push(firstA))
        XCTAssertNil(try assembler.push(firstB))
        XCTAssertEqual(try assembler.push(lastB)?.payload, Data("new!".utf8))
    }

    func testDisconnectCleanup() throws {
        let assembler = ChannelBankFrameAssembler()
        let first = ChannelBankFrame(flags: ChannelBankFrame.firstFlag, messageID: 4, offset: 0, payload: Data("abc".utf8)).encode()
        let last = ChannelBankFrame(flags: ChannelBankFrame.lastFlag, messageID: 4, offset: 3, payload: Data("def".utf8)).encode()
        XCTAssertNil(try assembler.push(first))
        assembler.reset()
        XCTAssertThrowsError(try assembler.push(last))
    }
}

final class ChannelBankClientTests: XCTestCase {
    func testRequestTimeoutHandling() async throws {
        let transport = FakeTransport()
        let client = ChannelBankClient(transport: transport)
        do {
            let _: EmptyBody = try await client.request(method: "GET", path: "/api/state", responseBody: EmptyBody.self, timeoutNanoseconds: 1_000_000)
            XCTFail("Expected timeout")
        } catch ChannelBankClientError.requestTimedOut {
            XCTAssertEqual(transport.frames.count, 1)
        } catch {
            XCTFail("Unexpected error \(error)")
        }
    }

    func testJSONResponseDecodingAndStructuredErrors() throws {
        let success = Data(#"{"v":1,"id":42,"ok":true,"status":200,"body":{"module":"Channel Bank","running":true,"unknown":123}}"#.utf8)
        let decoded = try JSONDecoder().decode(ChannelBankResponse<ChannelBankState>.self, from: success)
        XCTAssertEqual(decoded.body?.module, "Channel Bank")
        XCTAssertEqual(decoded.body?.running, true)

        let failure = Data(#"{"v":1,"id":42,"ok":false,"status":409,"error":{"code":"request_failed","message":"stop SDR before changing source"}}"#.utf8)
        let error = try JSONDecoder().decode(ChannelBankResponse<EmptyBody>.self, from: failure)
        XCTAssertEqual(error.error?.code, "request_failed")
        XCTAssertEqual(error.error?.message, "stop SDR before changing source")
    }

    func testStateDecodingWithMissingAndUnknownFields() throws {
        let raw = Data(#"{"selectedSource":"RX888","centerHz":157067450,"activeChannels":[{"freqHz":157000000,"extra":true}],"newFutureField":"ok"}"#.utf8)
        let state = try JSONDecoder().decode(ChannelBankState.self, from: raw)
        XCTAssertEqual(state.selectedSource, "RX888")
        XCTAssertEqual(state.centerHz, 157067450)
        XCTAssertEqual(state.activeChannels?.first?.freqHz, 157000000)
        XCTAssertNil(state.running)
    }

    private final class FakeTransport: ChannelBankTransport {
        var frames: [Data] = []

        func maximumCommandFrameLength() -> Int { 256 }

        func writeCommandFrame(_ data: Data) async throws {
            frames.append(data)
        }
    }
}

final class WaterfallTests: XCTestCase {
    func testFrequencyToScreenMapping() {
        var state = ChannelBankState()
        state.centerHz = 100
        state.usableSpanHz = 40
        let span = SpanInfo(state: state)
        XCTAssertEqual(span?.lowHz, 80)
        XCTAssertEqual(span?.highHz, 120)
        XCTAssertEqual(span?.x(for: 100, width: 200), 100)
    }

    func testWaterfallRetentionAndResetAfterCenterChange() {
        let store = ActivityWaterfallStore()
        var state = baseState()
        state.activeChannels = [ChannelBankChannel(freqHz: 100, signalPresent: true)]
        for _ in 0..<80 {
            store.append(state: state)
        }
        XCTAssertEqual(store.rows.count, 72)

        state.centerHz = 200
        state.activeChannels = [ChannelBankChannel(freqHz: 200, signalPresent: true)]
        store.append(state: state)
        XCTAssertEqual(store.rows.count, 1)
    }

    func testRecentStrengthDecay() {
        var state = baseState()
        state.recentChannels = [
            RecentChannel(freqHz: 100, ageMs: 0),
            RecentChannel(freqHz: 101, ageMs: 30_000)
        ]
        let points = ActivityWaterfallStore.collectPoints(state: state, span: SpanInfo(state: state)!)
        XCTAssertEqual(try XCTUnwrap(points.first(where: { $0.freqHz == 100 })?.strength), 0.58, accuracy: 0.001)
        XCTAssertEqual(try XCTUnwrap(points.first(where: { $0.freqHz == 101 })?.strength), 0.18, accuracy: 0.001)
    }

    private func baseState() -> ChannelBankState {
        var state = ChannelBankState()
        state.centerHz = 100
        state.usableSpanHz = 40
        state.settings = ChannelBankSettings(channelSpacingHz: 1)
        return state
    }
}

final class RecordingPaginatorTests: XCTestCase {
    func testRecordingPagination() async throws {
        let pages = [
            RecordingPage(dataBase64: Data("hello ".utf8).base64EncodedString(), offset: 0, nextOffset: 6, size: 11, eof: false, name: "a.m4a", contentType: "audio/mp4"),
            RecordingPage(dataBase64: Data("world".utf8).base64EncodedString(), offset: 6, nextOffset: 11, size: 11, eof: true, name: "a.m4a", contentType: "audio/mp4")
        ]
        let data = try await RecordingPaginator().collect { offset in
            pages[offset == 0 ? 0 : 1]
        }
        XCTAssertEqual(String(data: data, encoding: .utf8), "hello world")
    }

    func testCurrentPlaybackPagination() async throws {
        let page = RecordingPage(dataBase64: Data([1, 2, 3]).base64EncodedString(), offset: 0, nextOffset: 3, size: 3, eof: true, name: "playback.wav", contentType: "audio/wav")
        let data = try await RecordingPaginator().collect { _ in page }
        XCTAssertEqual(data, Data([1, 2, 3]))
    }
}
