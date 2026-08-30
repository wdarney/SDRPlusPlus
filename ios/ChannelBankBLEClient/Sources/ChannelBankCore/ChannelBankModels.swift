import Foundation

public struct JSONValue: Codable, Equatable, Sendable {
    public var value: AnySendableJSON

    public init(_ value: AnySendableJSON) {
        self.value = value
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() {
            value = .null
        } else if let bool = try? container.decode(Bool.self) {
            value = .bool(bool)
        } else if let number = try? container.decode(Double.self) {
            value = .number(number)
        } else if let string = try? container.decode(String.self) {
            value = .string(string)
        } else if let array = try? container.decode([JSONValue].self) {
            value = .array(array.map(\.value))
        } else {
            let object = try container.decode([String: JSONValue].self)
            value = .object(object.mapValues(\.value))
        }
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch value {
        case .null: try container.encodeNil()
        case .bool(let value): try container.encode(value)
        case .number(let value): try container.encode(value)
        case .string(let value): try container.encode(value)
        case .array(let values): try container.encode(values.map(JSONValue.init))
        case .object(let values): try container.encode(values.mapValues(JSONValue.init))
        }
    }
}

public enum AnySendableJSON: Equatable, Sendable {
    case null
    case bool(Bool)
    case number(Double)
    case string(String)
    case array([AnySendableJSON])
    case object([String: AnySendableJSON])
}

public struct ChannelBankRequest: Codable, Equatable {
    public var v: Int = 1
    public var id: Int64
    public var method: String
    public var path: String
    public var query: [String: JSONValue]?
    public var body: [String: JSONValue]?

    public init(id: Int64, method: String, path: String, query: [String: JSONValue]? = nil, body: [String: JSONValue]? = nil) {
        self.id = id
        self.method = method
        self.path = path
        self.query = query
        self.body = body
    }
}

public struct ChannelBankErrorBody: Codable, Error, Equatable {
    public var code: String
    public var message: String
}

public struct ChannelBankResponse<Body: Codable & Equatable>: Codable, Equatable {
    public var v: Int
    public var id: Int64
    public var ok: Bool
    public var status: Int
    public var body: Body?
    public var error: ChannelBankErrorBody?
}

public struct EmptyBody: Codable, Equatable {
    public init() {}
}

public struct ChannelBankState: Codable, Equatable {
    public var module: String?
    public var enabled: Bool?
    public var running: Bool?
    public var mode: String?
    public var demodMode: String?
    public var centerHz: Double?
    public var waterfallCenterHz: Double?
    public var sampleRate: Double?
    public var usableSpanHz: Double?
    public var bwUsage: Double?
    public var radioPlaying: Bool?
    public var sdrppHeartbeat: Int?
    public var serverTimeMs: Int64?
    public var selectedSource: String?
    public var sources: [String]?
    public var sdrppServer: SDRPPServerState?
    public var sourceControls: RX888SourceControls?
    public var sourceOffset: SourceOffsetState?
    public var settings: ChannelBankSettings?
    public var snrThresholdDb: Double?
    public var maxChannels: Int?
    public var recordingEnabled: Bool?
    public var activeChannels: [ChannelBankChannel]?
    public var recentChannels: [RecentChannel]?
    public var detectedSlots: Int?
    public var manualDetected: Int?
    public var snrOverview: [SNROverviewPoint]?
    public var playbackQueued: Int?
    public var playback: PlaybackState?
    public var playbackLock: PlaybackLock?
    public var currentlyPlayingFreqKey: Int64?
    public var history: [HistoryEntry]?
    public var diagnostics: Diagnostics?
    public var scanStopIndex: Int?
    public var scanStopCount: Int?
    public var bookmarkScanStopIndex: Int?
    public var bookmarkScanStopCount: Int?
    public var lastTranscriptName: String?
    public var lastTranscriptText: String?
    public var extra: [String: JSONValue] = [:]

    enum CodingKeys: String, CodingKey {
        case module, enabled, running, mode, demodMode, centerHz, waterfallCenterHz, sampleRate, usableSpanHz, bwUsage
        case radioPlaying, sdrppHeartbeat, serverTimeMs, selectedSource, sources, sdrppServer, sourceControls, sourceOffset, settings, snrThresholdDb
        case maxChannels, recordingEnabled, activeChannels, recentChannels, detectedSlots, manualDetected, snrOverview
        case playbackQueued, playback, playbackLock, currentlyPlayingFreqKey, history, diagnostics
        case scanStopIndex, scanStopCount, bookmarkScanStopIndex, bookmarkScanStopCount, lastTranscriptName, lastTranscriptText
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        module = try? container.decodeIfPresent(String.self, forKey: .module)
        enabled = try? container.decodeIfPresent(Bool.self, forKey: .enabled)
        running = try? container.decodeIfPresent(Bool.self, forKey: .running)
        mode = try? container.decodeIfPresent(String.self, forKey: .mode)
        demodMode = try? container.decodeIfPresent(String.self, forKey: .demodMode)
        centerHz = try? container.decodeIfPresent(Double.self, forKey: .centerHz)
        waterfallCenterHz = try? container.decodeIfPresent(Double.self, forKey: .waterfallCenterHz)
        sampleRate = try? container.decodeIfPresent(Double.self, forKey: .sampleRate)
        usableSpanHz = try? container.decodeIfPresent(Double.self, forKey: .usableSpanHz)
        bwUsage = try? container.decodeIfPresent(Double.self, forKey: .bwUsage)
        radioPlaying = try? container.decodeIfPresent(Bool.self, forKey: .radioPlaying)
        sdrppHeartbeat = try? container.decodeIfPresent(Int.self, forKey: .sdrppHeartbeat)
        serverTimeMs = try? container.decodeIfPresent(Int64.self, forKey: .serverTimeMs)
        selectedSource = try? container.decodeIfPresent(String.self, forKey: .selectedSource)
        sources = try? container.decodeIfPresent([String].self, forKey: .sources)
        sdrppServer = try? container.decodeIfPresent(SDRPPServerState.self, forKey: .sdrppServer)
        sourceControls = try? container.decodeIfPresent(RX888SourceControls.self, forKey: .sourceControls)
        sourceOffset = try? container.decodeIfPresent(SourceOffsetState.self, forKey: .sourceOffset)
        settings = try? container.decodeIfPresent(ChannelBankSettings.self, forKey: .settings)
        snrThresholdDb = try? container.decodeIfPresent(Double.self, forKey: .snrThresholdDb)
        maxChannels = try? container.decodeIfPresent(Int.self, forKey: .maxChannels)
        recordingEnabled = try? container.decodeIfPresent(Bool.self, forKey: .recordingEnabled)
        activeChannels = try? container.decodeIfPresent([ChannelBankChannel].self, forKey: .activeChannels)
        recentChannels = try? container.decodeIfPresent([RecentChannel].self, forKey: .recentChannels)
        detectedSlots = try? container.decodeIfPresent(Int.self, forKey: .detectedSlots)
        manualDetected = try? container.decodeIfPresent(Int.self, forKey: .manualDetected)
        snrOverview = try? container.decodeIfPresent([SNROverviewPoint].self, forKey: .snrOverview)
        playbackQueued = try? container.decodeIfPresent(Int.self, forKey: .playbackQueued)
        playback = try? container.decodeIfPresent(PlaybackState.self, forKey: .playback)
        playbackLock = try? container.decodeIfPresent(PlaybackLock.self, forKey: .playbackLock)
        currentlyPlayingFreqKey = try? container.decodeIfPresent(Int64.self, forKey: .currentlyPlayingFreqKey)
        history = try? container.decodeIfPresent([HistoryEntry].self, forKey: .history)
        diagnostics = try? container.decodeIfPresent(Diagnostics.self, forKey: .diagnostics)
        scanStopIndex = try? container.decodeIfPresent(Int.self, forKey: .scanStopIndex)
        scanStopCount = try? container.decodeIfPresent(Int.self, forKey: .scanStopCount)
        bookmarkScanStopIndex = try? container.decodeIfPresent(Int.self, forKey: .bookmarkScanStopIndex)
        bookmarkScanStopCount = try? container.decodeIfPresent(Int.self, forKey: .bookmarkScanStopCount)
        lastTranscriptName = try? container.decodeIfPresent(String.self, forKey: .lastTranscriptName)
        lastTranscriptText = try? container.decodeIfPresent(String.self, forKey: .lastTranscriptText)
    }

    public init() {}

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encodeIfPresent(module, forKey: .module)
        try container.encodeIfPresent(enabled, forKey: .enabled)
        try container.encodeIfPresent(running, forKey: .running)
        try container.encodeIfPresent(mode, forKey: .mode)
        try container.encodeIfPresent(demodMode, forKey: .demodMode)
        try container.encodeIfPresent(centerHz, forKey: .centerHz)
        try container.encodeIfPresent(waterfallCenterHz, forKey: .waterfallCenterHz)
        try container.encodeIfPresent(sampleRate, forKey: .sampleRate)
        try container.encodeIfPresent(usableSpanHz, forKey: .usableSpanHz)
        try container.encodeIfPresent(bwUsage, forKey: .bwUsage)
        try container.encodeIfPresent(radioPlaying, forKey: .radioPlaying)
        try container.encodeIfPresent(sdrppHeartbeat, forKey: .sdrppHeartbeat)
        try container.encodeIfPresent(serverTimeMs, forKey: .serverTimeMs)
        try container.encodeIfPresent(selectedSource, forKey: .selectedSource)
        try container.encodeIfPresent(sources, forKey: .sources)
        try container.encodeIfPresent(sdrppServer, forKey: .sdrppServer)
        try container.encodeIfPresent(sourceControls, forKey: .sourceControls)
        try container.encodeIfPresent(sourceOffset, forKey: .sourceOffset)
        try container.encodeIfPresent(settings, forKey: .settings)
        try container.encodeIfPresent(snrThresholdDb, forKey: .snrThresholdDb)
        try container.encodeIfPresent(maxChannels, forKey: .maxChannels)
        try container.encodeIfPresent(recordingEnabled, forKey: .recordingEnabled)
        try container.encodeIfPresent(activeChannels, forKey: .activeChannels)
        try container.encodeIfPresent(recentChannels, forKey: .recentChannels)
        try container.encodeIfPresent(detectedSlots, forKey: .detectedSlots)
        try container.encodeIfPresent(manualDetected, forKey: .manualDetected)
        try container.encodeIfPresent(snrOverview, forKey: .snrOverview)
        try container.encodeIfPresent(playbackQueued, forKey: .playbackQueued)
        try container.encodeIfPresent(playback, forKey: .playback)
        try container.encodeIfPresent(playbackLock, forKey: .playbackLock)
        try container.encodeIfPresent(currentlyPlayingFreqKey, forKey: .currentlyPlayingFreqKey)
        try container.encodeIfPresent(history, forKey: .history)
        try container.encodeIfPresent(diagnostics, forKey: .diagnostics)
        try container.encodeIfPresent(scanStopIndex, forKey: .scanStopIndex)
        try container.encodeIfPresent(scanStopCount, forKey: .scanStopCount)
        try container.encodeIfPresent(bookmarkScanStopIndex, forKey: .bookmarkScanStopIndex)
        try container.encodeIfPresent(bookmarkScanStopCount, forKey: .bookmarkScanStopCount)
        try container.encodeIfPresent(lastTranscriptName, forKey: .lastTranscriptName)
        try container.encodeIfPresent(lastTranscriptText, forKey: .lastTranscriptText)
    }
}

public struct SourceListResponse: Codable, Equatable {
    public var selected: String?
    public var sources: [String]?
}

public struct SDRPPServerState: Codable, Equatable {
    public var available: Bool?
    public var host: String?
    public var port: Int?
    public var connected: Bool?
    public var sourceRunning: Bool?
}

public struct SourceOffsetState: Codable, Equatable {
    public var selected: String?
    public var manualOffsetHz: Double?
    public var effectiveOffsetHz: Double?
    public var modes: [String]?
}

public struct ChannelBankChannel: Codable, Equatable, Identifiable {
    public var id: Double { gridFreqHz ?? freqHz }
    public var slot: Int?
    public var freqHz: Double
    public var gridFreqHz: Double?
    public var name: String?
    public var blocked: Bool?
    public var recording: Bool?
    public var signalPresent: Bool?
    public var rawSignalPresent: Bool?
    public var file: String?

    public init(slot: Int? = nil, freqHz: Double, gridFreqHz: Double? = nil, name: String? = nil, blocked: Bool? = nil, recording: Bool? = nil, signalPresent: Bool? = nil, rawSignalPresent: Bool? = nil, file: String? = nil) {
        self.slot = slot
        self.freqHz = freqHz
        self.gridFreqHz = gridFreqHz
        self.name = name
        self.blocked = blocked
        self.recording = recording
        self.signalPresent = signalPresent
        self.rawSignalPresent = rawSignalPresent
        self.file = file
    }
}

public struct RecentChannel: Codable, Equatable {
    public var freqHz: Double
    public var name: String?
    public var blocked: Bool?
    public var ageMs: Int?

    public init(freqHz: Double, name: String? = nil, blocked: Bool? = nil, ageMs: Int? = nil) {
        self.freqHz = freqHz
        self.name = name
        self.blocked = blocked
        self.ageMs = ageMs
    }
}

public struct HistoryEntry: Codable, Equatable, Identifiable {
    public var id: Double { freqHz }
    public var freqHz: Double
    public var name: String?
    public var count: Int?
    public var blocked: Bool?
    public var lastSeen: Int64?
    public var description: String?

    public var lastSeenDisplay: String {
        guard let lastSeen, lastSeen > 0 else { return "-" }
        return Date(timeIntervalSince1970: TimeInterval(lastSeen)).formatted(date: .omitted, time: .standard)
    }

    public init(freqHz: Double, name: String? = nil, count: Int? = nil, blocked: Bool? = nil, lastSeen: Int64? = nil, description: String? = nil) {
        self.freqHz = freqHz
        self.name = name
        self.count = count
        self.blocked = blocked
        self.lastSeen = lastSeen
        self.description = description
    }

    enum CodingKeys: String, CodingKey {
        case freqHz, name, count, blocked, lastSeen, description
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        freqHz = try container.decode(Double.self, forKey: .freqHz)
        name = try? container.decodeIfPresent(String.self, forKey: .name)
        count = try? container.decodeIfPresent(Int.self, forKey: .count)
        blocked = try? container.decodeIfPresent(Bool.self, forKey: .blocked)
        description = try? container.decodeIfPresent(String.self, forKey: .description)
        if let value = try? container.decodeIfPresent(Int64.self, forKey: .lastSeen) {
            lastSeen = value
        } else if let value = try? container.decodeIfPresent(Double.self, forKey: .lastSeen) {
            lastSeen = Int64(value)
        } else if let value = try? container.decodeIfPresent(String.self, forKey: .lastSeen), let parsed = Int64(value) {
            lastSeen = parsed
        } else {
            lastSeen = nil
        }
    }
}

public struct SNROverviewPoint: Codable, Equatable, Identifiable {
    public var id: Double { freqHz }
    public var freqHz: Double
    public var snrDb: Double
    public var detected: Bool?
    public var rawDetected: Bool?
    public var blocked: Bool?
}

public struct ChannelBankSettings: Codable, Equatable {
    public var mode: String?
    public var spacingId: Int?
    public var demodMode: String?
    public var snrThresholdDb: Double?
    public var maxChannels: Int?
    public var bwUsage: Double?
    public var recordingEnabled: Bool?
    public var channelSpacingHz: Double?
    public var minTransmissionMs: Int?
    public var signalHoldMs: Int?
    public var tailMs: Int?
    public var scanQuietSec: Double?
    public var scanNoSignalSec: Double?
    public var transcriptionBackend: Int?
    public var transcriptionBackendName: String?
}

public struct RX888SourceControls: Codable, Equatable {
    public var available: Bool?
    public var source: String?
    public var running: Bool?
    public var cleanupBusy: Bool?
    public var deviceId: Int?
    public var devices: [RX888Device]?
    public var sampleRate: Double?
    public var sampleRates: [RX888SampleRate]?
    public var supportsAdcFreq: Bool?
    public var adcClockMHz: Double?
    public var adcMinMHz: Double?
    public var adcMaxMHz: Double?
    public var mode: String?
    public var modes: [String]?
    public var gains: [RX888Gain]?
    public var supportsBiasTee: Bool?
    public var supportsNewBiasTee: Bool?
    public var biasTeeHF: Bool?
    public var biasTeeVHF: Bool?
    public var biasTeeLiveMutable: Bool?
    public var supportsDithering: Bool?
    public var dithering: Bool?
    public var ditheringLiveMutable: Bool?
    public var r2iqWorkers: Int?
    public var telemetryIntervalSec: Int?
    public var telemetrySpeeds: [TelemetrySpeed]?
    public var telemetryLiveMutable: Bool?
    public var toggles: [SourceControlToggle]?
}

public struct RX888Device: Codable, Equatable, Identifiable {
    public var id: Int
    public var label: String
}

public struct RX888SampleRate: Codable, Equatable, Identifiable {
    public var id: Int
    public var value: Double
    public var label: String?
    public var selected: Bool?
}

public struct RX888Gain: Codable, Equatable, Identifiable {
    public var id: String { name }
    public var name: String
    public var label: String?
    public var value: Double?
    public var min: Double?
    public var max: Double?
    public var step: Double?
    public var available: Bool?
    public var liveMutable: Bool?
}

public struct TelemetrySpeed: Codable, Equatable, Identifiable {
    public var id: Int { intervalSec ?? -1 }
    public var label: String?
    public var intervalSec: Int?
    public var selected: Bool?
}

public struct SourceControlToggle: Codable, Equatable, Identifiable {
    public var id: String { key }
    public var key: String
    public var label: String?
    public var value: Bool?
    public var available: Bool?
    public var liveMutable: Bool?
}

public struct PlaybackState: Codable, Equatable {
    public var active: Bool?
    public var freqKey: Int64?
    public var positionMs: Int?
    public var queued: Int?
    public var freqHz: Double?
    public var name: String?
    public var file: String?
    public var fileName: String?
}

public struct PlaybackLock: Codable, Equatable {
    public var active: Bool?
    public var freqHz: Double?
    public var name: String?
}

public struct Diagnostics: Codable, Equatable {
    public var rssBytes: Int64?
    public var cpuPercent: Double?
    public var activeChannels: Int?
    public var maxChannels: Int?
    public var playbackQueued: Int?
    public var webClientThreads: Int?
    public var transcriptionJobs: Int?
    public var pendingEncodes: Int?
    public var liveAudioClients: Int?
    public var liveAudioQueuedSamples: Int?
}

public struct RecordingPage: Codable, Equatable {
    public var dataBase64: String?
    public var offset: Int?
    public var nextOffset: Int?
    public var size: Int?
    public var eof: Bool?
    public var name: String?
    public var contentType: String?
}

public struct RecordingList: Codable, Equatable {
    public var available: Bool?
    public var root: String?
    public var activeSession: String?
    public var files: [RecordingItem]?
}

public struct RecordingItem: Codable, Equatable, Identifiable {
    public var id: String { path }
    public var path: String
    public var name: String?
    public var session: String?
    public var size: Int64?
    public var modifiedMs: Int64?
}

public struct ClearWavsResult: Codable, Equatable {
    public var ok: Bool?
    public var deleted: Int?
    public var skipped: Int?
    public var error: String?
}

public struct LiveAudioDescriptor: Codable, Equatable {
    public var format: String?
    public var rate: Int?
    public var channels: Int?
    public var characteristic: String?
    public var note: String?
}

public enum ChannelBankFormatters {
    public static func mhz(_ hz: Double?) -> String {
        guard let hz, hz.isFinite, hz > 0 else { return "-" }
        return String(format: "%.6f MHz", hz / 1_000_000)
    }

    public static func compactHz(_ hz: Double?) -> String {
        guard let hz, hz.isFinite, hz > 0 else { return "-" }
        if hz >= 1_000_000 { return String(format: "%.3f MHz", hz / 1_000_000) }
        if hz >= 1_000 { return String(format: "%.1f kHz", hz / 1_000) }
        return String(format: "%.0f Hz", hz)
    }

    public static func bytes(_ bytes: Int64?) -> String {
        guard let bytes, bytes > 0 else { return "0 B" }
        let value = Double(bytes)
        if value >= 1_048_576 { return String(format: "%.2f MB", value / 1_048_576) }
        if value >= 1024 { return String(format: "%.1f KB", value / 1024) }
        return "\(bytes) B"
    }

    public static func millisTime(_ ms: Int64) -> String {
        guard ms > 0 else { return "-" }
        return Date(timeIntervalSince1970: TimeInterval(ms) / 1000).formatted(date: .omitted, time: .shortened)
    }

    public static func spacingPreset(_ id: Int) -> String {
        switch id {
        case 0: return "8.333 kHz"
        case 1: return "12.5 kHz"
        case 2: return "25 kHz"
        case 3: return "50 kHz"
        case 4: return "100 kHz"
        case 5: return "200 kHz"
        default: return "\(id)"
        }
    }
}
