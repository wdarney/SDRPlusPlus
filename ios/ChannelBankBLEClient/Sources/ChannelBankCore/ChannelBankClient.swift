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
        timeoutNanoseconds: UInt64 = 20_000_000_000
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

    public func getSources() async throws -> SourceListResponse {
        try await request(method: "GET", path: "/api/sources", responseBody: SourceListResponse.self)
    }

    public func getSDRPPServer() async throws -> SDRPPServerState {
        try await request(method: "GET", path: "/api/sdrpp-server", responseBody: SDRPPServerState.self)
    }

    public func getSourceControls() async throws -> RX888SourceControls {
        try await request(method: "GET", path: "/api/source-controls", responseBody: RX888SourceControls.self)
    }

    public func getSourceOffset() async throws -> SourceOffsetState {
        try await request(method: "GET", path: "/api/source-offset", responseBody: SourceOffsetState.self)
    }

    public func getChannelBankSettings() async throws -> ChannelBankSettings {
        try await request(method: "GET", path: "/api/channel-bank/settings", responseBody: ChannelBankSettings.self)
    }

    public func getRecordings() async throws -> RecordingList {
        try await request(method: "GET", path: "/api/recordings", responseBody: RecordingList.self)
    }

    public func getLiveAudioDescriptor() async throws -> LiveAudioDescriptor {
        try await request(method: "GET", path: "/api/audio/live.pcm", responseBody: LiveAudioDescriptor.self)
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

    public func setSource(_ name: String) async throws -> ChannelBankState {
        try await request(
            method: "POST",
            path: "/api/source",
            body: ["name": JSONValue(.string(name))],
            responseBody: ChannelBankState.self
        )
    }

    public func setSDRPPServer(host: String, port: Int) async throws -> ChannelBankState {
        try await request(
            method: "POST",
            path: "/api/sdrpp-server",
            body: ["host": JSONValue(.string(host)), "port": JSONValue(.number(Double(port)))],
            responseBody: ChannelBankState.self
        )
    }

    public func setSDRPPServerConnected(_ connected: Bool) async throws -> ChannelBankState {
        try await request(
            method: "POST",
            path: connected ? "/api/sdrpp-server/connect" : "/api/sdrpp-server/disconnect",
            responseBody: ChannelBankState.self
        )
    }

    public func setSourceOffset(selected: String, manualOffsetHz: Double? = nil) async throws -> ChannelBankState {
        var body: [String: JSONValue] = ["selected": JSONValue(.string(selected))]
        if let manualOffsetHz {
            body["manualOffsetHz"] = JSONValue(.number(manualOffsetHz))
        }
        return try await request(method: "POST", path: "/api/source-offset", body: body, responseBody: ChannelBankState.self)
    }

    public func setSourceControls(_ body: [String: JSONValue]) async throws -> ChannelBankState {
        try await request(method: "POST", path: "/api/source-controls", body: body, responseBody: ChannelBankState.self)
    }

    public func setChannelBankSettings(_ body: [String: JSONValue]) async throws -> ChannelBankState {
        try await request(method: "POST", path: "/api/channel-bank/settings", body: body, responseBody: ChannelBankState.self)
    }

    public func setFrequency(_ hz: Double, blocked: Bool) async throws -> ChannelBankState {
        try await request(
            method: "POST",
            path: "/api/frequency/block",
            body: ["hz": JSONValue(.number(hz)), "blocked": JSONValue(.bool(blocked))],
            responseBody: ChannelBankState.self
        )
    }

    public func setPlaybackLock(hz: Double) async throws -> ChannelBankState {
        try await request(
            method: "POST",
            path: "/api/playback-lock",
            body: ["hz": JSONValue(.number(hz))],
            responseBody: ChannelBankState.self
        )
    }

    public func setRecordingSession(name: String) async throws -> RecordingList {
        try await request(
            method: "POST",
            path: "/api/recordings/session",
            body: ["name": JSONValue(.string(name))],
            responseBody: RecordingList.self
        )
    }

    public func clearRecordedWavs() async throws -> ClearWavsResult {
        try await request(method: "POST", path: "/api/recordings/clear-wavs", responseBody: ClearWavsResult.self)
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
