import XCTest
@testable import ChannelBankCore

final class BinaryAudioTransferTests: XCTestCase {
    @MainActor
    func testCRC32MatchesIEEEGoldenVector() {
        XCTAssertEqual(BinaryAudioTransfer.crc32(Data("123456789".utf8)), 0xcbf43926)
    }

    private func packet(_ data: Data, stream: UInt32, window: UInt32, offset: Int) -> Data {
        var result = Data([1, 0, 0, 0])
        for value in [stream, window, UInt32(offset)] {
            for shift in stride(from: 0, to: 32, by: 8) { result.append(UInt8(truncatingIfNeeded: value >> shift)) }
        }
        result.append(data)
        return result
    }

    @MainActor
    private func window(_ source: Data, offset: Int, id: UInt32, chunk: Int = 496) -> (BinaryAudioWindow, [Data]) {
        let end = min(source.count, offset + chunk * 16)
        let descriptor = BinaryAudioWindow(transferId: "lease", streamId: 0x12345678, windowId: id,
            offset: offset, nextOffset: end, size: source.count, chunkBytes: chunk,
            crc32: BinaryAudioTransfer.crc32(source.subdata(in: offset..<end)))
        let packets = stride(from: offset, to: end, by: chunk).map { start in
            packet(source.subdata(in: start..<min(start + chunk, end)), stream: 0x12345678, window: id, offset: start)
        }
        return (descriptor, packets)
    }

    @MainActor
    func testOutOfOrderDuplicatesAndPacketsBeforeDescriptorAcrossMTUs() async throws {
        let source = Data((0..<17000).map { UInt8($0 % 251) })
        for chunk in [4, 169, 496] {
            let transfer = BinaryAudioTransfer(windowTimeout: 0.02)
            var acknowledged = 0
            let data = try await transfer.collect(transferId: "lease", streamId: 0x12345678, size: source.count,
                request: { offset, id in
                    XCTAssertEqual(offset, acknowledged)
                    let (descriptor, packets) = self.window(source, offset: offset, id: id, chunk: chunk)
                    for packet in packets.reversed() { transfer.receive(packet); transfer.receive(packet) }
                    return descriptor
                }, progress: { bytes, total in
                    XCTAssertGreaterThan(bytes, acknowledged)
                    XCTAssertEqual(total, source.count)
                    acknowledged = bytes
                })
            XCTAssertEqual(data, source)
            XCTAssertEqual(transfer.retryCount, 0)
        }
    }

    @MainActor
    func testMissingFirstMiddleAndLastPacketsRetransmitWithFreshWindowID() async throws {
        let source = Data(repeating: 0x5a, count: 16 * 496)
        for missing in [0, 7, 15] {
            let transfer = BinaryAudioTransfer(windowTimeout: 0.02)
            var attempts = 0
            var stale: Data?
            let data = try await transfer.collect(transferId: "lease", streamId: 0x12345678, size: source.count,
                request: { offset, id in
                    attempts += 1
                    XCTAssertEqual(offset, 0)
                    let (descriptor, packets) = self.window(source, offset: offset, id: id)
                    if let stale { transfer.receive(stale) }
                    for (index, packet) in packets.enumerated() where attempts > 1 || index != missing {
                        transfer.receive(packet)
                    }
                    stale = packets[missing]
                    return descriptor
                }, progress: { _, _ in })
            XCTAssertEqual(data, source)
            XCTAssertEqual(attempts, 2)
            XCTAssertEqual(transfer.retryCount, 1)
        }
    }

    @MainActor
    func testChecksumCorruptionRetriesWithoutAppendingBadBytes() async throws {
        let source = Data(repeating: 12, count: 700)
        let transfer = BinaryAudioTransfer(windowTimeout: 0.02)
        var attempts = 0
        let data = try await transfer.collect(transferId: "lease", streamId: 0x12345678, size: source.count,
            request: { offset, id in
                attempts += 1
                let (descriptor, packets) = self.window(source, offset: offset, id: id)
                for var packet in packets {
                    if attempts == 1 { packet[16] ^= 1 }
                    transfer.receive(packet)
                }
                return descriptor
            }, progress: { _, _ in })
        XCTAssertEqual(data, source)
        XCTAssertEqual(attempts, 2)
    }

    @MainActor
    func testOtherLeaseAndMalformedPacketsCannotCompleteWindow() async throws {
        let source = Data(repeating: 12, count: 20)
        let transfer = BinaryAudioTransfer(windowTimeout: 0.01)
        var attempts = 0
        do {
            _ = try await transfer.collect(transferId: "lease", streamId: 0x12345678, size: source.count,
                request: { offset, id in
                    attempts += 1
                    transfer.receive(Data(repeating: 0, count: 600))
                    transfer.receive(Data([1]))
                    transfer.receive(self.packet(source, stream: 999, window: id, offset: offset))
                    transfer.receive(self.packet(source, stream: 0x12345678, window: id, offset: 1_000_000))
                    return self.window(source, offset: offset, id: id).0
                }, progress: { _, _ in XCTFail("No valid window received") })
            XCTFail("Expected exhausted retries")
        } catch BinaryAudioError.incompleteWindow {}
        XCTAssertEqual(attempts, 3)
    }

    @MainActor
    func testCancellationWhileWaitingForPacketsAndNextTransferRecovery() async throws {
        let waiting = expectation(description: "Waiting for notification")
        let transfer = BinaryAudioTransfer()
        let source = Data([1, 2, 3])
        let task = Task { @MainActor in
            try await transfer.collect(transferId: "lease", streamId: 0x12345678, size: source.count,
                request: { offset, id in waiting.fulfill(); return self.window(source, offset: offset, id: id).0 },
                progress: { _, _ in })
        }
        await fulfillment(of: [waiting], timeout: 1)
        task.cancel()
        do { _ = try await task.value; XCTFail("Expected cancellation") } catch is CancellationError {}
        let data = try await transfer.collect(transferId: "lease", streamId: 0x12345678, size: source.count,
            request: { offset, id in
                let (descriptor, packets) = self.window(source, offset: offset, id: id)
                packets.forEach { transfer.receive($0) }
                return descriptor
            }, progress: { _, _ in })
        XCTAssertEqual(data, source)
    }
}
