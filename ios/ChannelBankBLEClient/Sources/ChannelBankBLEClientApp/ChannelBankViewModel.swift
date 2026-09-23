#if canImport(ChannelBankCore)
import ChannelBankCore
#endif
import Combine
import Foundation

@MainActor
public final class ChannelBankViewModel: ObservableObject {
    @Published public var controlReceiver = ""

    private func sendReceiverControls(_ body: [String: JSONValue]) {
        // Capture the target before starting asynchronous work or switching UI receivers.
        var addressed = body
        let receiver = controlReceiver.isEmpty ? ble.latestState?.selectedSource : controlReceiver
        if let receiver { addressed["receiver"] = .init(.string(receiver)) }
        ble.setSourceControls(addressed)
    }
    @Published public var centerMHzText = ""
    @Published public var centerTuneValue = 0.0
    @Published public private(set) var centerTuneStatus = "Waiting for center"
    @Published public var serverHostText = ""
    @Published public var serverPortText = ""
    @Published public var manualOffsetHzText = "0"
    @Published public var recordingSessionText = ""
    @Published public var pendingBlockPoint: WaterfallPoint?
    @Published public var rangeLowMHzText = ""
    @Published public var rangeHighMHzText = ""
    @Published public private(set) var selectedBand = "custom"
    @Published public private(set) var bandStatus: String?
    @Published public private(set) var rangeApplying = false
    @Published public private(set) var rangeError: String?
    @Published public private(set) var waterfall = ActivityWaterfallStore()

    public let ble: BLECentralManager
    private var cancellables: Set<AnyCancellable> = []
    private var centerTuneTask: Task<Void, Never>?
    private var centerTuneCurrentHz: Double?

    public init(ble: BLECentralManager? = nil) {
        let ble = ble ?? BLECentralManager()
        self.ble = ble
        ble.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)
    }

    public func acceptStateUpdate() {
        guard let state = ble.latestState else { return }
        waterfall.append(state: state)
        if rangeLowMHzText.isEmpty && rangeHighMHzText.isEmpty { loadCurrentRange() }
        let centerHz = state.centerHz ?? state.waterfallCenterHz ?? 0
        if centerHz > 0 {
            if centerMHzText.isEmpty || (Double(centerMHzText) ?? 0) <= 0 {
                centerMHzText = String(format: "%.6f", centerHz / 1_000_000)
            }
            if centerTuneTask == nil {
                centerTuneStatus = "Center \(ChannelBankFormatters.mhz(centerHz))"
            }
        }
        if serverHostText.isEmpty, let host = state.sdrppServer?.host {
            serverHostText = host
        }
        if serverPortText.isEmpty, let port = state.sdrppServer?.port {
            serverPortText = "\(port)"
        }
        if manualOffsetHzText == "0", let offset = state.sourceOffset?.manualOffsetHz, offset != 0 {
            manualOffsetHzText = String(format: "%.0f", offset)
        }
    }

    public func tuneCenter() {
        guard let mhz = Double(centerMHzText), mhz > 0 else { return }
        ble.tuneCenter(hz: mhz * 1_000_000)
    }

    public func loadCurrentRange() {
        guard let state = ble.latestState, let span = SpanInfo(state: state) else { return }
        rangeLowMHzText = String(format: "%.6f", span.lowHz / 1_000_000)
        rangeHighMHzText = String(format: "%.6f", span.highHz / 1_000_000)
        rangeError = nil
        selectedBand = "custom"
        bandStatus = nil
    }

    public func selectBand(_ band: String) {
        selectedBand = band
        bandStatus = nil
        rangeError = nil
        if band == "airbandUS" {
            rangeLowMHzText = "118.000"
            rangeHighMHzText = "137.000"
        }
    }

    public func startSimpleScanner() {
        if ble.latestState?.mode == "multi_receiver_scan" { ble.setChannelBankRunning(true) }
        else if selectedBand == "airbandUS" { applySimpleRange(start: true) }
        else { ble.setChannelBankRunning(true) }
    }

    public func applySimpleRange(start: Bool = false) {
        guard !rangeApplying else { return }
        guard let state = ble.latestState, state.running != true else {
            rangeError = "Stop Channel Bank before applying a band or range."
            return
        }
        let low = rangeLowMHzText, high = rangeHighMHzText
        let airband = selectedBand == "airbandUS"
        rangeError = nil
        bandStatus = nil
        rangeApplying = true
        Task {
            defer { rangeApplying = false }
            do {
                let plan = try await ble.applyScannerBand(lowMHz: low, highMHz: high, airband: airband, start: start)
                bandStatus = plan.mode == "scan" ? "Scan - \(plan.stopCount) tuning positions" : "Auto - single tuning position"
            } catch {
                rangeError = error.localizedDescription
            }
        }
    }

    public func beginCenterTune() {
        guard centerTuneTask == nil else { return }
        let centerHz = ble.latestState?.centerHz ?? ble.latestState?.waterfallCenterHz ?? 0
        guard centerHz > 0 else {
            centerTuneStatus = "Waiting for center"
            return
        }
        centerTuneCurrentHz = centerHz
        centerTuneStatus = "Center \(ChannelBankFormatters.mhz(centerHz))"
        centerTuneTask = Task { [weak self] in
            while !Task.isCancelled {
                self?.tickCenterTune()
                try? await Task.sleep(nanoseconds: 160_000_000)
            }
        }
    }

    public func updateCenterTune(_ value: Double) {
        guard abs(value) > 0 else { return }
        if centerTuneTask == nil { beginCenterTune() }
        tickCenterTune()
    }

    public func endCenterTune() {
        centerTuneTask?.cancel()
        centerTuneTask = nil
        centerTuneValue = 0
        if let centerTuneCurrentHz {
            centerTuneStatus = "Center \(ChannelBankFormatters.mhz(centerTuneCurrentHz))"
        }
        centerTuneCurrentHz = nil
    }

    private func tickCenterTune() {
        let value = centerTuneValue
        guard value != 0 else { return }
        let base = centerTuneCurrentHz ?? ble.latestState?.centerHz ?? ble.latestState?.waterfallCenterHz ?? 0
        guard base > 0 else {
            centerTuneStatus = "Waiting for center"
            return
        }
        let hz = max(1, base + centerTuneStepHz(value))
        centerTuneCurrentHz = hz
        centerMHzText = String(format: "%.6f", hz / 1_000_000)
        centerTuneStatus = "Center \(ChannelBankFormatters.mhz(hz))"
        ble.tuneCenter(hz: hz)
    }

    private func centerTuneStepHz(_ value: Double) -> Double {
        let magnitude = min(1, abs(value) / 100)
        guard magnitude > 0 else { return 0 }
        return (150 + pow(magnitude, 1.65) * 9_850) * (value < 0 ? -1 : 1)
    }

    public func selectSource(_ name: String) {
        ble.setSource(name)
    }

    public func saveServerTarget() {
        guard let port = Int(serverPortText.trimmingCharacters(in: .whitespacesAndNewlines)), port > 0 else { return }
        let host = serverHostText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !host.isEmpty else { return }
        ble.setSDRPPServer(host: host, port: port)
    }

    public func setSourceOffset(_ selected: String) {
        let manual = Double(manualOffsetHzText.trimmingCharacters(in: .whitespacesAndNewlines))
        ble.setSourceOffset(selected: selected, manualOffsetHz: manual)
    }

    public func setSetting(_ key: String, number value: Double) {
        ble.setChannelBankSettings([key: JSONValue(.number(value))])
    }

    public func setSetting(_ key: String, int value: Int) {
        ble.setChannelBankSettings([key: JSONValue(.number(Double(value)))])
    }

    public func setSetting(_ key: String, string value: String) {
        ble.setChannelBankSettings([key: JSONValue(.string(value))])
    }

    public func setSetting(_ key: String, bool value: Bool) {
        ble.setChannelBankSettings([key: JSONValue(.bool(value))])
    }

    public func setSourceControl(_ key: String, number value: Double) {
        sendReceiverControls([key: JSONValue(.number(value))])
    }

    public func setSourceControl(_ key: String, int value: Int) {
        sendReceiverControls([key: JSONValue(.number(Double(value)))])
    }

    public func setSourceControl(_ key: String, string value: String) {
        sendReceiverControls([key: JSONValue(.string(value))])
    }

    public func setSourceControl(_ key: String, bool value: Bool) {
        sendReceiverControls([key: JSONValue(.bool(value))])
    }

    public func setSourceToggle(_ key: String, value: Bool) {
        sendReceiverControls(["toggles": JSONValue(.object([key: .bool(value)]))])
    }

    public func setSourceGain(_ name: String, value: Double) {
        sendReceiverControls(["gains": JSONValue(.object([name: .number(value)]))])
    }

    public func setRecordingSession() {
        let name = recordingSessionText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else { return }
        ble.setRecordingSession(name: name)
    }

    public func selectWaterfallFrequency(at fraction: Double) {
        guard let span = waterfall.spanInfo else { return }
        let hz = span.lowHz + span.spanHz * min(1, max(0, fraction))
        pendingBlockPoint = waterfall.nearestSelectablePoint(to: hz, channelSpacingHz: ble.latestState?.settings?.channelSpacingHz)
    }

    public func confirmBlockToggle() {
        guard let point = pendingBlockPoint else { return }
        ble.setFrequency(point.freqHz, blocked: !point.blocked)
        pendingBlockPoint = nil
    }
}
