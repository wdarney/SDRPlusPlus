import Foundation

public enum RecordingPaginationError: Error, Equatable {
    case missingData
    case invalidBase64
    case offsetMismatch(expected: Int, got: Int)
    case didNotAdvance
    case missingTransferID
    case transferIDChanged(expected: String, got: String)
}

public struct RecordingPaginator {
    public struct LeasedRecording: Equatable {
        public var data: Data
        public var transferId: String
    }

    public init() {}

    public func collect(
        startingAt offset: Int = 0,
        fetch: (Int) async throws -> RecordingPage
    ) async throws -> Data {
        var output = Data()
        var next = offset
        while true {
            try Task.checkCancellation()
            let page = try await fetch(next)
            if let offset = page.offset, offset != next {
                throw RecordingPaginationError.offsetMismatch(expected: next, got: offset)
            }
            guard let base64 = page.dataBase64 else { throw RecordingPaginationError.missingData }
            guard let data = Data(base64Encoded: base64) else { throw RecordingPaginationError.invalidBase64 }
            output.append(data)
            if page.eof == true { return output }
            guard let nextOffset = page.nextOffset, nextOffset > next else { throw RecordingPaginationError.didNotAdvance }
            next = nextOffset
        }
    }

    public func collectLeased(
        startingAt offset: Int = 0,
        fetch: (Int, String?) async throws -> RecordingPage
    ) async throws -> LeasedRecording {
        var output = Data()
        var next = offset
        var transferId: String?
        while true {
            try Task.checkCancellation()
            let page = try await fetch(next, transferId)
            if let pageOffset = page.offset, pageOffset != next {
                throw RecordingPaginationError.offsetMismatch(expected: next, got: pageOffset)
            }
            guard let pageTransferId = page.transferId, !pageTransferId.isEmpty else {
                throw RecordingPaginationError.missingTransferID
            }
            if let transferId, transferId != pageTransferId {
                throw RecordingPaginationError.transferIDChanged(expected: transferId, got: pageTransferId)
            }
            transferId = pageTransferId
            guard let base64 = page.dataBase64 else { throw RecordingPaginationError.missingData }
            guard let data = Data(base64Encoded: base64) else { throw RecordingPaginationError.invalidBase64 }
            output.append(data)
            if page.eof == true {
                return LeasedRecording(data: output, transferId: pageTransferId)
            }
            guard let nextOffset = page.nextOffset, nextOffset > next else {
                throw RecordingPaginationError.didNotAdvance
            }
            next = nextOffset
        }
    }
}
