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
    @MainActor
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

    @MainActor
    func testImmediateResponseDuringCommandWrite() async throws {
        let transport = FakeTransport()
        let client = ChannelBankClient(transport: transport)
        transport.onWrite = { data in
            let frame = try ChannelBankFrame(data: data)
            client.receiveResponsePayload(Data("{\"v\":1,\"id\":\(frame.messageID),\"ok\":true,\"status\":200,\"body\":{\"snrThresholdDb\":8.5}}".utf8))
        }
        let state = try await client.setChannelBankSettings(["snrThresholdDb": JSONValue(.number(8.5))])
        XCTAssertEqual(state.snrThresholdDb, 8.5)
    }

    @MainActor
    func testConcurrentCommandsCompleteWithOutOfOrderReplies() async throws {
        let transport = FakeTransport()
        let client = ChannelBankClient(transport: transport)
        transport.onWrite = { data in
            let frame = try ChannelBankFrame(data: data)
            let request = try JSONDecoder().decode(ChannelBankRequest.self, from: frame.payload)
            let body = try JSONEncoder().encode(request.body ?? [:])
            let response = Data("{\"v\":1,\"id\":\(request.id),\"ok\":true,\"status\":200,\"body\":\(String(decoding: body, as: UTF8.self))}".utf8)
            Task { @MainActor in
                try await Task.sleep(nanoseconds: UInt64(25 - request.id) * 1_000_000)
                client.receiveResponsePayload(response)
            }
        }
        try await withThrowingTaskGroup(of: Void.self) { group in
            for value in 1...24 {
                group.addTask {
                    let state: ChannelBankState = try await client.request(
                        method: "POST", path: "/api/channel-bank/settings",
                        body: ["snrThresholdDb": JSONValue(.number(Double(value)))],
                        timeoutNanoseconds: 2_000_000_000)
                    XCTAssertEqual(state.snrThresholdDb, Double(value))
                }
            }
            try await group.waitForAll()
        }
        XCTAssertEqual(transport.frames.count, 24)
    }

    @MainActor
    func testCancellationReturnsWithoutWaitingForServerOrTimeout() async throws {
        let transport = FakeTransport()
        let client = ChannelBankClient(transport: transport)
        let written = expectation(description: "Command written")
        let canceled = expectation(description: "Canceled request released")
        transport.onWrite = { _ in written.fulfill() }
        let request = Task { @MainActor in
            do {
                _ = try await client.getState()
                XCTFail("Expected cancellation")
            } catch is CancellationError {
                canceled.fulfill()
            } catch {
                XCTFail("Unexpected error \(error)")
            }
        }
        await fulfillment(of: [written], timeout: 1)
        request.cancel()
        await fulfillment(of: [canceled], timeout: 1)
        client.reset()
        await request.value
    }

    @MainActor
    func testTimeoutCancelsRemainingCommandFragments() async throws {
        let transport = FakeTransport()
        transport.frameLength = 16
        let client = ChannelBankClient(transport: transport)
        let writerCanceled = expectation(description: "Suspended writer canceled")
        transport.onWrite = { _ in
            do {
                try await Task.sleep(nanoseconds: 10_000_000_000)
            } catch {
                writerCanceled.fulfill()
                throw error
            }
        }
        do {
            let _: ChannelBankState = try await client.request(
                method: "GET", path: "/api/state", timeoutNanoseconds: 50_000_000)
            XCTFail("Expected timeout")
        } catch ChannelBankClientError.requestTimedOut {
            await fulfillment(of: [writerCanceled], timeout: 1)
            XCTAssertEqual(transport.frames.count, 1)
        }
    }

    @MainActor
    func testResetReleasesRequestAndLateReplyDoesNotAffectNextRequest() async throws {
        let transport = FakeTransport()
        let client = ChannelBankClient(transport: transport)
        transport.onWrite = { _ in client.reset() }
        do {
            _ = try await client.getState()
            XCTFail("Expected disconnect error")
        } catch ChannelBankClientError.requestTimedOut(let id) {
            XCTAssertEqual(id, -1)
        }
        transport.onWrite = { data in
            let frame = try ChannelBankFrame(data: data)
            client.receiveResponsePayload(Data(#"{"v":1,"id":1,"ok":true,"status":200,"body":{"snrThresholdDb":1}}"#.utf8))
            client.receiveResponsePayload(Data("{\"v\":1,\"id\":\(frame.messageID),\"ok\":true,\"status\":200,\"body\":{\"snrThresholdDb\":9}}".utf8))
        }
        let state = try await client.getState()
        XCTAssertEqual(state.snrThresholdDb, 9)
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
        let raw = Data(#"{"seq":12,"selectedSource":"RX888","centerHz":157067450,"activeChannelCount":1,"activeChannels":[{"freqHz":157000000,"extra":true}],"newFutureField":"ok"}"#.utf8)
        let state = try JSONDecoder().decode(ChannelBankState.self, from: raw)
        XCTAssertEqual(state.seq, 12)
        XCTAssertEqual(state.selectedSource, "RX888")
        XCTAssertEqual(state.centerHz, 157067450)
        XCTAssertEqual(state.activeChannelCount, 1)
        XCTAssertEqual(state.activeChannels?.first?.freqHz, 157000000)
        XCTAssertNil(state.running)
    }

    func testStateSummaryDecodingAndMergePreservesDetails() throws {
        let raw = Data(#"""
        {
          "v":1,
          "seq":1042,
          "serverTimeMs":1788125402000,
          "running":true,
          "radioPlaying":true,
          "selectedSource":"RX888",
          "centerHz":157067450,
          "sampleRate":8000000,
          "mode":"auto",
          "demodMode":"NFM",
          "snrThresholdDb":8,
          "maxChannels":16,
          "recordingEnabled":true,
          "activeChannelCount":3,
          "playbackQueued":1,
          "playback":{"active":true,"freqHz":155475000,"name":"County Fire","fileName":"fire.wav"}
        }
        """#.utf8)
        let summary = try JSONDecoder().decode(ChannelBankStateSummary.self, from: raw)
        XCTAssertEqual(summary.seq, 1042)
        XCTAssertEqual(summary.activeChannelCount, 3)
        XCTAssertEqual(summary.playback?.fileName, "fire.wav")

        let compactResponseState = try JSONDecoder().decode(ChannelBankState.self, from: raw)
        XCTAssertTrue(compactResponseState.isSummaryShaped)
        XCTAssertEqual(compactResponseState.asSummary.seq, 1042)
        XCTAssertEqual(compactResponseState.asSummary.playback?.fileName, "fire.wav")

        var state = ChannelBankState()
        state.history = [HistoryEntry(freqHz: 155_475_000, name: "County Fire", count: 2)]
        state.sourceControls = RX888SourceControls()
        state.merge(summary: compactResponseState.asSummary)

        XCTAssertEqual(state.seq, 1042)
        XCTAssertEqual(state.running, true)
        XCTAssertEqual(state.selectedSource, "RX888")
        XCTAssertEqual(state.activeChannelCount, 3)
        XCTAssertEqual(state.settings?.snrThresholdDb, 8)
        XCTAssertEqual(state.settings?.maxChannels, 16)
        XCTAssertEqual(state.history?.first?.name, "County Fire")
        XCTAssertNotNil(state.sourceControls)
    }

    func testFullStateIsNotSummaryShaped() throws {
        var state = ChannelBankState()
        state.seq = 7
        state.running = true
        state.settings = ChannelBankSettings(snrThresholdDb: 8)
        XCTAssertFalse(state.isSummaryShaped)
    }

    func testDelayedFullStateMergesTelemetryWithoutRollingBackSummaryControls() {
        var current = ChannelBankState()
        current.seq = 12
        current.running = true
        current.snrThresholdDb = 9
        current.recordingEnabled = false

        var delayed = ChannelBankState()
        delayed.seq = 11
        delayed.running = false
        delayed.snrThresholdDb = 6
        delayed.usableSpanHz = 800_000
        delayed.snrOverview = [SNROverviewPoint(freqHz: 155_000_000, snrDb: 12)]
        delayed.activeChannels = [ChannelBankChannel(freqHz: 155_000_000, signalPresent: true)]
        delayed.history = [HistoryEntry(freqHz: 155_000_000, name: "Fire", count: 1)]

        current.mergeTelemetry(from: delayed)

        XCTAssertEqual(current.seq, 12)
        XCTAssertTrue(current.running == true)
        XCTAssertEqual(current.snrThresholdDb, 9)
        XCTAssertTrue(current.recordingEnabled == false)
        XCTAssertEqual(current.usableSpanHz, 800_000)
        XCTAssertEqual(current.snrOverview?.first?.snrDb, 12)
        XCTAssertEqual(current.activeChannels?.first?.freqHz, 155_000_000)
        XCTAssertEqual(current.history?.first?.name, "Fire")
    }

    func testParityStateDecoding() throws {
        let raw = Data(#"""
        {
          "selectedSource":"RX888",
          "sources":["RX888","SDR++ Server"],
          "sdrppServer":{"available":true,"host":"192.168.1.10","port":5259,"connected":false,"sourceRunning":false},
          "sourceOffset":{"selected":"Manual","manualOffsetHz":1250,"effectiveOffsetHz":1250,"modes":["None","Manual","LNB"]},
          "settings":{"mode":"scan","spacingId":2,"channelSpacingHz":25000,"demodMode":"NFM","snrThresholdDb":6.5,"manualLocalSnrEnabled":true,"manualStormGuardEnabled":true,"maxChannels":8,"bwUsage":0.8,"recordingEnabled":true,"minTransmissionMs":400,"signalHoldMs":700,"tailMs":500,"scanQuietSec":2.5,"scanNoSignalSec":0.5,"transcriptionBackend":0,"transcriptionBackendName":"Off"},
          "sourceControls":{"available":true,"source":"RX888","running":false,"deviceId":0,"devices":[{"id":0,"label":"RX888"}],"sampleRate":32000000,"sampleRates":[{"id":1,"value":32000000,"label":"32 MHz","selected":true}],"supportsAdcFreq":true,"adcClockMHz":64,"adcMinMHz":16,"adcMaxMHz":140,"mode":"HF","modes":["HF","VHF"],"gains":[{"name":"LNA","label":"LNA","value":8,"min":0,"max":31.5,"step":0.5,"available":true,"liveMutable":true}],"supportsNewBiasTee":true,"supportsBiasTee":true,"biasTeeHF":false,"biasTeeVHF":true,"biasTeeLiveMutable":true,"supportsDithering":true,"dithering":false,"ditheringLiveMutable":true,"r2iqWorkers":2,"telemetryIntervalSec":1,"telemetrySpeeds":[{"label":"Fast","intervalSec":1,"selected":true}],"telemetryLiveMutable":true,"toggles":[{"key":"preamp","label":"Preamp","value":true,"available":true}]},
          "currentlyPlayingFreqKey":157067,
          "history":[{"freqHz":157067000,"name":"Test","count":3,"blocked":false,"lastSeen":1798644000,"description":"ok"}],
          "scanStopIndex":4,
          "scanStopCount":12,
          "bookmarkScanStopIndex":1,
          "bookmarkScanStopCount":5
        }
        """#.utf8)
        let state = try JSONDecoder().decode(ChannelBankState.self, from: raw)
        XCTAssertEqual(state.sdrppServer?.host, "192.168.1.10")
        XCTAssertEqual(state.sourceOffset?.effectiveOffsetHz, 1250)
        XCTAssertEqual(state.settings?.minTransmissionMs, 400)
        XCTAssertTrue(state.settings?.manualLocalSnrEnabled == true)
        XCTAssertTrue(state.settings?.manualStormGuardEnabled == true)
        XCTAssertEqual(state.sourceControls?.r2iqWorkers, 2)
        XCTAssertEqual(state.sourceControls?.telemetrySpeeds?.first?.intervalSec, 1)
        XCTAssertEqual(state.sourceControls?.toggles?.first?.key, "preamp")
        XCTAssertEqual(state.history?.first?.lastSeen, 1_798_644_000)
        XCTAssertEqual(state.scanStopCount, 12)
    }

    func testAuxiliaryPayloadDecoding() throws {
        let recordingsRaw = Data(#"{"available":true,"root":"/tmp/rec","activeSession":"field","files":[{"path":"field/a.m4a","name":"a.m4a","session":"field","size":12345,"modifiedMs":1798644000000}]}"#.utf8)
        let recordings = try JSONDecoder().decode(RecordingList.self, from: recordingsRaw)
        XCTAssertEqual(recordings.files?.first?.path, "field/a.m4a")
        XCTAssertEqual(recordings.files?.first?.size, 12345)

        let audioRaw = Data(#"{"characteristic":"7d2f0005-8c4b-4d7a-9a61-8e3c4f2a1000","format":"pcm_s16le","rate":48000,"channels":1,"note":"enable notifications"}"#.utf8)
        let audio = try JSONDecoder().decode(LiveAudioDescriptor.self, from: audioRaw)
        XCTAssertEqual(audio.format, "pcm_s16le")
        XCTAssertEqual(audio.rate, 48000)
    }

    func testSNRTelemetryFrameDecoding() throws {
        var payload = Data([1])
        func append<T: FixedWidthInteger>(_ value: T) {
            var littleEndian = value.littleEndian
            withUnsafeBytes(of: &littleEndian) { payload.append(contentsOf: $0) }
        }
        func append(_ value: Double) {
            append(value.bitPattern)
        }

        append(UInt32(42))
        append(155_000_000.0)
        append(25_000.0)
        append(UInt16(2))
        append(Int16(85))
        payload.append(0x03)
        append(Int16(-15))
        payload.append(0x04)

        let frame = try SNRTelemetryFrame(payload: payload)
        XCTAssertEqual(frame.sequence, 42)
        XCTAssertEqual(frame.points.count, 2)
        XCTAssertEqual(frame.points[0].freqHz, 155_000_000)
        XCTAssertEqual(frame.points[1].freqHz, 155_025_000)
        XCTAssertEqual(frame.points[0].snrDb, 8.5)
        XCTAssertTrue(frame.points[0].detected == true)
        XCTAssertTrue(frame.points[0].rawDetected == true)
        XCTAssertTrue(frame.points[1].blocked == true)
    }

    private final class FakeTransport: ChannelBankTransport {
        var frames: [Data] = []
        var frameLength = 256
        var onWrite: ((Data) async throws -> Void)?

        func maximumCommandFrameLength() -> Int { frameLength }

        func writeCommandFrame(_ data: Data) async throws {
            XCTAssertTrue(Thread.isMainThread, "Command writes and their UI diagnostics must run on the main thread")
            frames.append(data)
            try await onWrite?(data)
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
        let pages = [
            RecordingPage(transferId: "cb-1", dataBase64: Data([1, 2]).base64EncodedString(), offset: 0, nextOffset: 2, size: 3, eof: false, name: "playback.wav", contentType: "audio/wav"),
            RecordingPage(transferId: "cb-1", dataBase64: Data([3]).base64EncodedString(), offset: 2, nextOffset: 3, size: 3, eof: true, name: "playback.wav", contentType: "audio/wav")
        ]
        var receivedTransferIds: [String?] = []
        let recording = try await RecordingPaginator().collectLeased { offset, transferId in
            receivedTransferIds.append(transferId)
            return pages[offset == 0 ? 0 : 1]
        }
        XCTAssertEqual(recording.data, Data([1, 2, 3]))
        XCTAssertEqual(recording.transferId, "cb-1")
        XCTAssertEqual(receivedTransferIds, [nil, "cb-1"])
    }

    func testCurrentPlaybackPaginationRejectsMissingTransferID() async throws {
        let page = RecordingPage(dataBase64: Data([1]).base64EncodedString(), offset: 0, nextOffset: 1, size: 1, eof: true, name: "playback.wav", contentType: "audio/wav")
        do {
            _ = try await RecordingPaginator().collectLeased { _, _ in page }
            XCTFail("Expected missing transfer ID")
        } catch RecordingPaginationError.missingTransferID {
        } catch {
            XCTFail("Unexpected error \(error)")
        }
    }

    func testRecordingPaginationRejectsOffsetMismatch() async throws {
        let page = RecordingPage(dataBase64: Data([1]).base64EncodedString(), offset: 4, nextOffset: 5, size: 5, eof: false, name: "bad.wav", contentType: "audio/wav")
        do {
            _ = try await RecordingPaginator().collect { _ in page }
            XCTFail("Expected offset mismatch")
        } catch RecordingPaginationError.offsetMismatch(let expected, let got) {
            XCTAssertEqual(expected, 0)
            XCTAssertEqual(got, 4)
        } catch {
            XCTFail("Unexpected error \(error)")
        }
    }
}
