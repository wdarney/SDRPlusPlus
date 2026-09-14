import Foundation

public struct BinaryAudioWindow: Codable, Equatable {
    public let transferId: String
    public let streamId: UInt32
    public let windowId: UInt32
    public let offset: Int
    public let nextOffset: Int
    public let size: Int
    public let chunkBytes: Int
    public let crc32: UInt32
}

public enum BinaryAudioError: Error, Equatable {
    case invalidDescriptor
    case incompleteWindow
    case checksumMismatch
    case conflictingPacket
    case alreadyDownloading
}

// Binary Audio File notifications have their own 16-byte header, not the JSON
// fragment header. Only one bounded window is accepted at a time; late retries
// and packets from a previous lease cannot enter the current output.
@MainActor
public final class BinaryAudioTransfer {
    public private(set) var retryCount = 0
    private var nextWindowID: UInt32 = 1
    private var streamID: UInt32?
    private var windowID: UInt32?
    private var start = 0
    private var upperBound = 0
    private var packets: [Int: Data] = [:]
    private var packetError: BinaryAudioError?
    private let windowTimeout: TimeInterval

    public init(windowTimeout: TimeInterval = 2) {
        self.windowTimeout = windowTimeout
    }

    public func receive(_ packet: Data) {
        guard packet.count > 16, packet.count <= 512 else { return }
        let bytes = [UInt8](packet)
        guard bytes[0] == 1, bytes[1] == 0, bytes[2] == 0, bytes[3] == 0 else { return }
        func u32(_ offset: Int) -> UInt32 {
            (0..<4).reduce(UInt32(0)) { $0 | UInt32(bytes[offset + $1]) << (8 * $1) }
        }
        guard u32(4) == streamID, u32(8) == windowID else { return }
        let offset = Int(u32(12))
        let data = Data(bytes.dropFirst(16))
        guard offset >= start, offset <= upperBound - data.count else { return }
        if let previous = packets[offset] {
            if previous != data { packetError = .conflictingPacket }
        } else if packets.count < 16 {
            packets[offset] = data
        }
    }

    public func collect(
        transferId: String, streamId: UInt32, size: Int,
        request: (Int, UInt32) async throws -> BinaryAudioWindow,
        progress: (Int, Int) -> Void
    ) async throws -> Data {
        guard self.streamID == nil else { throw BinaryAudioError.alreadyDownloading }
        guard !transferId.isEmpty, streamId != 0, size > 0, size <= 64 * 1024 * 1024 else {
            throw BinaryAudioError.invalidDescriptor
        }
        self.streamID = streamId
        retryCount = 0
        defer {
            self.streamID = nil
            windowID = nil
            packets.removeAll()
        }
        var output = Data()
        while output.count < size {
            var accepted: Data?
            var lastError: Error = BinaryAudioError.incompleteWindow
            for attempt in 0..<3 {
                try Task.checkCancellation()
                if attempt > 0 { retryCount += 1 }
                let id = nextWindowID
                nextWindowID &+= 1
                if nextWindowID == 0 { nextWindowID = 1 }
                windowID = id
                start = output.count
                upperBound = min(size, start + 16 * 496)
                packets.removeAll(keepingCapacity: true)
                packetError = nil
                do {
                    let window = try await request(start, id)
                    try Task.checkCancellation()
                    guard window.transferId == transferId, window.streamId == streamId,
                          window.windowId == id, window.offset == start, window.size == size,
                          window.chunkBytes > 0, window.chunkBytes <= 496,
                          window.nextOffset == min(size, start + window.chunkBytes * 16) else {
                        throw BinaryAudioError.invalidDescriptor
                    }
                    let deadline = Date().addingTimeInterval(windowTimeout)
                    while true {
                        try Task.checkCancellation()
                        if let packetError { throw packetError }
                        if let data = assembled(window) {
                            guard Self.crc32(data) == window.crc32 else { throw BinaryAudioError.checksumMismatch }
                            accepted = data
                            break
                        }
                        guard Date() < deadline else { throw BinaryAudioError.incompleteWindow }
                        try await Task.sleep(nanoseconds: 10_000_000)
                    }
                    break
                } catch is CancellationError {
                    throw CancellationError()
                } catch {
                    lastError = error
                }
            }
            guard let accepted else { throw lastError }
            output.append(accepted)
            progress(output.count, size)
        }
        return output
    }

    private func assembled(_ window: BinaryAudioWindow) -> Data? {
        var result = Data()
        var offset = window.offset
        while offset < window.nextOffset {
            let count = min(window.chunkBytes, window.nextOffset - offset)
            guard let packet = packets[offset], packet.count == count else { return nil }
            result.append(packet)
            offset += count
        }
        return result
    }

    public static func crc32(_ data: Data) -> UInt32 {
        var value = UInt32.max
        for byte in data {
            value ^= UInt32(byte)
            for _ in 0..<8 { value = (value >> 1) ^ ((value & 1) == 1 ? 0xedb88320 : 0) }
        }
        return ~value
    }
}
