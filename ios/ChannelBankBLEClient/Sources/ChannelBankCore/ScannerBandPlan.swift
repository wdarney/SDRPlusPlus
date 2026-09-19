import Foundation

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
