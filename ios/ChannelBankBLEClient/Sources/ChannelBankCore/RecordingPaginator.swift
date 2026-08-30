import Foundation

public enum RecordingPaginationError: Error, Equatable {
    case missingData
    case invalidBase64
    case offsetMismatch(expected: Int, got: Int)
    case didNotAdvance
}

public struct RecordingPaginator {
    public init() {}

    public func collect(
        startingAt offset: Int = 0,
        fetch: (Int) async throws -> RecordingPage
    ) async throws -> Data {
        var output = Data()
        var next = offset
        while true {
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
}
