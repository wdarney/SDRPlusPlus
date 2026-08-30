import Foundation

public enum ChannelBankFrameError: Error, Equatable {
    case frameTooShort
    case unsupportedVersion(UInt8)
    case discontinuousOffset(expected: UInt32, got: UInt32)
    case missingFirstFrame
    case emptyPayloadBudget
    case invalidUTF8
}

public struct ChannelBankFrame: Equatable {
    public static let headerSize = 8
    public static let version: UInt8 = 1
    public static let firstFlag: UInt8 = 1 << 0
    public static let lastFlag: UInt8 = 1 << 1

    public var flags: UInt8
    public var messageID: UInt16
    public var offset: UInt32
    public var payload: Data

    public var isFirst: Bool { flags & Self.firstFlag != 0 }
    public var isLast: Bool { flags & Self.lastFlag != 0 }

    public init(flags: UInt8, messageID: UInt16, offset: UInt32, payload: Data) {
        self.flags = flags
        self.messageID = messageID
        self.offset = offset
        self.payload = payload
    }

    public init(data: Data) throws {
        guard data.count >= Self.headerSize else { throw ChannelBankFrameError.frameTooShort }
        let bytes = [UInt8](data)
        guard bytes[0] == Self.version else { throw ChannelBankFrameError.unsupportedVersion(bytes[0]) }
        flags = bytes[1]
        messageID = UInt16(bytes[2]) | (UInt16(bytes[3]) << 8)
        offset = UInt32(bytes[4]) | (UInt32(bytes[5]) << 8) | (UInt32(bytes[6]) << 16) | (UInt32(bytes[7]) << 24)
        payload = data.dropFirst(Self.headerSize)
    }

    public func encode() -> Data {
        var out = Data()
        out.reserveCapacity(Self.headerSize + payload.count)
        out.append(Self.version)
        out.append(flags)
        out.append(UInt8(messageID & 0xff))
        out.append(UInt8((messageID >> 8) & 0xff))
        out.append(UInt8(offset & 0xff))
        out.append(UInt8((offset >> 8) & 0xff))
        out.append(UInt8((offset >> 16) & 0xff))
        out.append(UInt8((offset >> 24) & 0xff))
        out.append(payload)
        return out
    }
}

public struct ChannelBankFramer {
    public var maximumFrameLength: Int

    public init(maximumFrameLength: Int) {
        self.maximumFrameLength = maximumFrameLength
    }

    public func frames(for payload: Data, messageID: UInt16) throws -> [Data] {
        let budget = maximumFrameLength - ChannelBankFrame.headerSize
        guard budget > 0 else { throw ChannelBankFrameError.emptyPayloadBudget }
        if payload.isEmpty {
            return [ChannelBankFrame(flags: ChannelBankFrame.firstFlag | ChannelBankFrame.lastFlag, messageID: messageID, offset: 0, payload: Data()).encode()]
        }

        var out: [Data] = []
        var offset = 0
        while offset < payload.count {
            let end = min(offset + budget, payload.count)
            var flags: UInt8 = 0
            if offset == 0 { flags |= ChannelBankFrame.firstFlag }
            if end == payload.count { flags |= ChannelBankFrame.lastFlag }
            let slice = payload[offset..<end]
            out.append(ChannelBankFrame(flags: flags, messageID: messageID, offset: UInt32(offset), payload: Data(slice)).encode())
            offset = end
        }
        return out
    }
}

public final class ChannelBankFrameAssembler {
    private struct Assembly {
        var nextOffset: UInt32
        var payload: Data
    }

    private var assemblies: [UInt16: Assembly] = [:]

    public init() {}

    public func reset() {
        assemblies.removeAll()
    }

    public func push(_ data: Data) throws -> (messageID: UInt16, payload: Data)? {
        let frame = try ChannelBankFrame(data: data)
        if frame.isFirst {
            assemblies[frame.messageID] = Assembly(nextOffset: 0, payload: Data())
        }
        guard var assembly = assemblies[frame.messageID] else {
            throw ChannelBankFrameError.missingFirstFrame
        }
        guard frame.offset == assembly.nextOffset else {
            assemblies.removeValue(forKey: frame.messageID)
            throw ChannelBankFrameError.discontinuousOffset(expected: assembly.nextOffset, got: frame.offset)
        }
        assembly.payload.append(frame.payload)
        assembly.nextOffset += UInt32(frame.payload.count)
        if frame.isLast {
            assemblies.removeValue(forKey: frame.messageID)
            return (frame.messageID, assembly.payload)
        }
        assemblies[frame.messageID] = assembly
        return nil
    }
}
