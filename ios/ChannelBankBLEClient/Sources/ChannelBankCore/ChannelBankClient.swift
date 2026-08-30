import Foundation

public enum ChannelBankClientError: Error, Equatable {
    case requestTimedOut(Int64)
    case requestIDOutOfRange(Int64)
    case mismatchedResponse(expected: Int64, got: Int64)
    case server(ChannelBankErrorBody, status: Int)
    case missingBody
}

public protocol ChannelBankTransport: AnyObject {
    func maximumCommandFrameLength() -> Int
    func writeCommandFrame(_ data: Data) async throws
}

public final class ChannelBankClient {
    private let transport: ChannelBankTransport
    private let encoder = JSONEncoder()
    private let decoder = JSONDecoder()
    private var nextID: Int64 = 1
    private var pending: [Int64: CheckedContinuation<Data, Error>] = [:]

    public init(transport: ChannelBankTransport) {
        self.transport = transport
    }

    public func reset() {
        pending.values.forEach { $0.resume(throwing: ChannelBankClientError.requestTimedOut(-1)) }
        pending.removeAll()
    }

    public func receiveResponsePayload(_ payload: Data) {
        guard let envelope = try? decoder.decode(ChannelBankResponse<JSONValue>.self, from: payload) else { return }
        pending.removeValue(forKey: envelope.id)?.resume(returning: payload)
    }

    @discardableResult
    public func request<Body: Codable & Equatable>(
        method: String,
        path: String,
        query: [String: JSONValue]? = nil,
        body: [String: JSONValue]? = nil,
        responseBody: Body.Type = Body.self,
        timeoutNanoseconds: UInt64 = 5_000_000_000
    ) async throws -> Body {
        let id = nextID
        nextID += 1
        guard id <= Int64(UInt16.max) else { throw ChannelBankClientError.requestIDOutOfRange(id) }

        let request = ChannelBankRequest(id: id, method: method, path: path, query: query, body: body)
        let payload = try encoder.encode(request)
        let framer = ChannelBankFramer(maximumFrameLength: transport.maximumCommandFrameLength())
        let frames = try framer.frames(for: payload, messageID: UInt16(id))

        return try await withThrowingTaskGroup(of: Data.self) { group in
            group.addTask {
                try await withCheckedThrowingContinuation { continuation in
                    self.pending[id] = continuation
                    Task {
                        do {
                            for frame in frames {
                                try await self.transport.writeCommandFrame(frame)
                            }
                        } catch {
                            self.pending.removeValue(forKey: id)?.resume(throwing: error)
                        }
                    }
                }
            }
            group.addTask {
                try await Task.sleep(nanoseconds: timeoutNanoseconds)
                if let continuation = self.pending.removeValue(forKey: id) {
                    continuation.resume(throwing: ChannelBankClientError.requestTimedOut(id))
                }
                throw ChannelBankClientError.requestTimedOut(id)
            }
            guard let raw = try await group.next() else { throw ChannelBankClientError.requestTimedOut(id) }
            group.cancelAll()
            let envelope = try decoder.decode(ChannelBankResponse<Body>.self, from: raw)
            guard envelope.id == id else { throw ChannelBankClientError.mismatchedResponse(expected: id, got: envelope.id) }
            if !envelope.ok {
                throw ChannelBankClientError.server(envelope.error ?? ChannelBankErrorBody(code: "request_failed", message: "Request failed"), status: envelope.status)
            }
            guard let body = envelope.body else {
                if Body.self == EmptyBody.self { return EmptyBody() as! Body }
                throw ChannelBankClientError.missingBody
            }
            return body
        }
    }

    public func getState() async throws -> ChannelBankState {
        try await request(method: "GET", path: "/api/state", responseBody: ChannelBankState.self)
    }

    public func setChannelBankRunning(_ running: Bool) async throws -> ChannelBankState {
        try await request(method: "POST", path: running ? "/api/start" : "/api/stop", responseBody: ChannelBankState.self)
    }

    public func setRadioRunning(_ running: Bool) async throws -> ChannelBankState {
        try await request(method: "POST", path: running ? "/api/play" : "/api/stop-radio", responseBody: ChannelBankState.self)
    }

    public func setCenterHz(_ hz: Double) async throws -> ChannelBankState {
        try await request(method: "POST", path: "/api/center", body: ["hz": JSONValue(.number(hz))], responseBody: ChannelBankState.self)
    }

    public func setFrequency(_ hz: Double, blocked: Bool) async throws -> ChannelBankState {
        try await request(
            method: "POST",
            path: "/api/frequency/block",
            body: ["hz": JSONValue(.number(hz)), "blocked": JSONValue(.bool(blocked))],
            responseBody: ChannelBankState.self
        )
    }

    public func recordingPage(file: String, offset: Int, limit: Int = 4096) async throws -> RecordingPage {
        try await request(
            method: "GET",
            path: "/api/recordings/download",
            query: ["file": JSONValue(.string(file))],
            body: ["offset": JSONValue(.number(Double(offset))), "limit": JSONValue(.number(Double(limit)))],
            responseBody: RecordingPage.self
        )
    }

    public func currentPlaybackPage(offset: Int, limit: Int = 4096) async throws -> RecordingPage {
        try await request(
            method: "GET",
            path: "/api/audio/current-playback",
            body: ["offset": JSONValue(.number(Double(offset))), "limit": JSONValue(.number(Double(limit)))],
            responseBody: RecordingPage.self
        )
    }
}
