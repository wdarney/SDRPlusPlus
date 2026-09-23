import Foundation

/// An explicit draft: incoming telemetry never overwrites edits to the scanner.
public struct MultiReceiverScannerDraft {
    public var discovery = ""
    public var pool: [String] = []
    public var ranges: [ChannelBankScanRange] = []
    public var maximumMonitorSec = 0.0
    public var scanSettleMs = 0

    public init(settings: ChannelBankSettings, selectedSource: String?) {
        discovery = settings.discoveryReceiver.flatMap { $0.isEmpty ? nil : $0 } ?? selectedSource ?? ""
        pool = settings.transmissionReceiverPool ?? []
        ranges = settings.scanRanges ?? []
        maximumMonitorSec = settings.maximumMonitorSec ?? 0
        scanSettleMs = settings.scanSettleMs ?? 250
    }

    public func requestBody(state: ChannelBankState) throws -> [String: JSONValue] {
        guard state.running == false else { throw DraftError.invalid("Stop Channel Bank before applying scanner settings.") }
        guard state.sources?.contains(discovery) == true else { throw DraftError.invalid("Choose an available discovery SDR.") }
        let changedDiscovery = discovery != state.selectedSource || discovery != state.settings?.discoveryReceiver
        guard !changedDiscovery || state.radioPlaying == false else { throw DraftError.invalid("Stop the radio before changing discovery SDR.") }
        guard Set(pool).count == pool.count, !pool.contains(discovery),
              pool.allSatisfy({ state.sources?.contains($0) == true }) else {
            throw DraftError.invalid("Choose distinct transmission receivers other than discovery.")
        }
        guard ranges.count <= 64, ranges.allSatisfy({ $0.start.isFinite && $0.stop.isFinite && $0.start > 0 && $0.start < $0.stop }) else {
            throw DraftError.invalid("Use at most 64 ranges with positive From MHz below To MHz.")
        }
        guard maximumMonitorSec.isFinite, (0...86400).contains(maximumMonitorSec),
              (250...2000).contains(scanSettleMs) else { throw DraftError.invalid("Use 0–86400 monitor seconds and 250–2000 settling milliseconds.") }
        var body: [String: JSONValue] = [
            "mode": .init(.string("multi_receiver_scan")),
            "transmissionReceiverPool": .init(.array(pool.map { .string($0) })),
            "scanRanges": .init(.array(ranges.map { .object(["start": .number($0.start), "stop": .number($0.stop)]) })),
            "maximumMonitorSec": .init(.number(maximumMonitorSec)),
            "scanSettleMs": .init(.number(Double(scanSettleMs)))
        ]
        if changedDiscovery { body["discoveryReceiver"] = .init(.string(discovery)) }
        return body
    }

    public enum DraftError: LocalizedError {
        case invalid(String)
        public var errorDescription: String? { if case .invalid(let text) = self { return text }; return nil }
    }
}

public struct ScannerBandPlan {
    public let range: ChannelBankScanRange
    public let mode: String
    public let centerHz: Double
    public let stopCount: Int
    public let settingsBody: [String: JSONValue]

    public init(lowMHz: String, highMHz: String, sampleRate: Double?, settings: ChannelBankSettings, airband: Bool) throws {
        guard let low = Double(lowMHz.trimmingCharacters(in: .whitespacesAndNewlines)),
              let high = Double(highMHz.trimmingCharacters(in: .whitespacesAndNewlines)),
              low.isFinite, high.isFinite, low > 0, high > low, high < 1_000_000 else {
            throw PlanError.invalidRange
        }
        guard let rate = sampleRate, rate.isFinite, rate > 0,
              let usage = settings.bwUsage, usage.isFinite, (0.5...1).contains(usage) else {
            throw PlanError.missingBandwidth
        }
        let start = low * 1_000_000, stop = high * 1_000_000
        let width = stop - start
        let capacity = rate * usage
        let count = ceil(width / capacity)
        guard count.isFinite, count <= 4096 else { throw PlanError.tooManyStops }
        let scanning = width > capacity
        guard !scanning || settings.supportsScanRanges == true else { throw PlanError.unsupportedServer }
        range = ChannelBankScanRange(start: start, stop: stop)
        mode = scanning ? "scan" : "auto"
        stopCount = max(1, Int(count))
        // Match the existing server's evenly divided scan windows.
        centerHz = start + width / Double(stopCount) / 2
        var body: [String: JSONValue] = ["mode": JSONValue(.string(mode))]
        if scanning {
            body["scanRanges"] = JSONValue(.array([.object(["start": .number(start), "stop": .number(stop)])]))
        }
        if airband {
            body["demodMode"] = JSONValue(.string("AM"))
            body["spacingId"] = JSONValue(.number(2))
        }
        settingsBody = body
    }

    public enum PlanError: LocalizedError {
        case invalidRange, missingBandwidth, tooManyStops, unsupportedServer, alreadyRunning
        public var errorDescription: String? {
            switch self {
            case .invalidRange: return "Enter positive MHz values with From below To."
            case .missingBandwidth: return "Waiting for the source sample rate and usable bandwidth."
            case .tooManyStops: return "This range needs more than 4,096 scan stops. Increase the source sample rate."
            case .unsupportedServer: return "This band needs scanning. Update the server to a build with scan-range support."
            case .alreadyRunning: return "Stop Channel Bank before applying a band or range."
            }
        }
    }
}
