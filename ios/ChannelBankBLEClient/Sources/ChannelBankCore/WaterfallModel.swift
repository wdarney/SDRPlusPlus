import Foundation
import Combine

public struct SpanInfo: Equatable {
    public var centerHz: Double
    public var spanHz: Double
    public var lowHz: Double { centerHz - spanHz / 2 }
    public var highHz: Double { centerHz + spanHz / 2 }

    public init?(state: ChannelBankState) {
        let center = state.centerHz ?? state.waterfallCenterHz ?? 0
        let span = state.usableSpanHz ?? ((state.sampleRate ?? 0) * (state.bwUsage ?? 0.8))
        guard center.isFinite, span.isFinite, center > 0, span > 0 else { return nil }
        centerHz = center
        spanHz = span
    }

    public func x(for frequencyHz: Double, width: Double) -> Double {
        ((frequencyHz - lowHz) / spanHz) * width
    }

    public func contains(_ frequencyHz: Double) -> Bool {
        frequencyHz >= lowHz && frequencyHz <= highHz
    }

    public var roundedKey: String {
        "\(Int(centerHz.rounded())):\(Int(spanHz.rounded()))"
    }
}

public struct WaterfallPoint: Equatable, Identifiable {
    public var id = UUID()
    public var freqHz: Double
    public var name: String
    public var blocked: Bool
    public var live: Bool
    public var recent: Bool
    public var history: Bool
    public var strength: Double

    public init(freqHz: Double, name: String = "", blocked: Bool = false, live: Bool = false, recent: Bool = false, history: Bool = false, strength: Double = 0.25) {
        self.freqHz = freqHz
        self.name = name
        self.blocked = blocked
        self.live = live
        self.recent = recent
        self.history = history
        self.strength = max(0.06, min(1, strength))
    }
}

public struct WaterfallRow: Equatable, Identifiable {
    public var id = UUID()
    public var points: [WaterfallPoint]
}

public final class ActivityWaterfallStore: ObservableObject {
    public static let maximumRows = 72

    @Published public private(set) var rows: [WaterfallRow] = []
    @Published public private(set) var historyMarkers: [WaterfallPoint] = []
    @Published public private(set) var latestSelectablePoints: [WaterfallPoint] = []
    @Published public private(set) var spanInfo: SpanInfo?

    private var key: String?

    public init() {}

    public func reset() {
        rows.removeAll()
        historyMarkers.removeAll()
        latestSelectablePoints.removeAll()
        spanInfo = nil
        key = nil
    }

    public func append(state: ChannelBankState) {
        guard let span = SpanInfo(state: state) else { return }
        if key != span.roundedKey {
            rows.removeAll()
            key = span.roundedKey
        }

        let points = Self.collectPoints(state: state, span: span)
        let liveRow = points.filter { $0.live || $0.recent }
        rows.append(WaterfallRow(points: liveRow))
        if rows.count > Self.maximumRows {
            rows.removeFirst(rows.count - Self.maximumRows)
        }
        historyMarkers = points.filter(\.history)
        latestSelectablePoints = points.filter { $0.live || $0.recent || $0.blocked }
        spanInfo = span
    }

    public static func collectPoints(state: ChannelBankState, span: SpanInfo) -> [WaterfallPoint] {
        var points: [WaterfallPoint] = []
        func add(_ point: WaterfallPoint) {
            guard point.freqHz.isFinite, span.contains(point.freqHz) else { return }
            points.append(point)
        }

        for item in state.history ?? [] {
            let strength = min(0.32, 0.08 + Double(item.count ?? 0) * 0.025)
            add(WaterfallPoint(freqHz: item.freqHz, name: item.name ?? "", blocked: item.blocked ?? false, history: true, strength: strength))
        }
        for item in state.recentChannels ?? [] {
            let age = Double(min(30_000, max(0, item.ageMs ?? 0)))
            let strength = max(0.18, 0.58 * (1 - age / 30_000))
            add(WaterfallPoint(freqHz: item.freqHz, name: item.name ?? "", blocked: item.blocked ?? false, recent: true, strength: strength))
        }
        for item in state.activeChannels ?? [] {
            let strength = (item.signalPresent ?? false) ? 1.0 : 0.65
            add(WaterfallPoint(freqHz: item.gridFreqHz ?? item.freqHz, name: item.name ?? "", blocked: item.blocked ?? false, live: true, strength: strength))
        }
        if let playback = state.playback, playback.active == true, let freq = playback.freqHz {
            add(WaterfallPoint(freqHz: freq, name: playback.name ?? "Playback", live: true, strength: 1.0))
        }
        return points
    }

    public func nearestSelectablePoint(to frequencyHz: Double, channelSpacingHz: Double?) -> WaterfallPoint? {
        guard let spanInfo else { return nil }
        let tolerance = max((channelSpacingHz ?? 0) / 2, spanInfo.spanHz / 160)
        return latestSelectablePoints
            .map { ($0, abs($0.freqHz - frequencyHz)) }
            .filter { $0.1 <= tolerance }
            .min { $0.1 < $1.1 }?
            .0
    }
}
