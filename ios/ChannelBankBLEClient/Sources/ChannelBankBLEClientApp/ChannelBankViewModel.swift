#if canImport(ChannelBankCore)
import ChannelBankCore
#endif
import Combine
import Foundation

@MainActor
public final class ChannelBankViewModel: ObservableObject {
    @Published public var centerMHzText = ""
    @Published public var serverHostText = ""
    @Published public var serverPortText = ""
    @Published public var manualOffsetHzText = "0"
    @Published public var recordingSessionText = ""
    @Published public var pendingBlockPoint: WaterfallPoint?
    @Published public private(set) var waterfall = ActivityWaterfallStore()

    public let ble: BLECentralManager
    private var cancellables: Set<AnyCancellable> = []

    public init(ble: BLECentralManager = BLECentralManager()) {
        self.ble = ble
        ble.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)
    }

    public func acceptStateUpdate() {
        guard let state = ble.latestState else { return }
        waterfall.append(state: state)
        if centerMHzText.isEmpty {
            centerMHzText = state.centerHz.map { String(format: "%.6f", $0 / 1_000_000) } ?? ""
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
        ble.setSourceControls([key: JSONValue(.number(value))])
    }

    public func setSourceControl(_ key: String, int value: Int) {
        ble.setSourceControls([key: JSONValue(.number(Double(value)))])
    }

    public func setSourceControl(_ key: String, string value: String) {
        ble.setSourceControls([key: JSONValue(.string(value))])
    }

    public func setSourceControl(_ key: String, bool value: Bool) {
        ble.setSourceControls([key: JSONValue(.bool(value))])
    }

    public func setSourceToggle(_ key: String, value: Bool) {
        ble.setSourceControls(["toggles": JSONValue(.object([key: .bool(value)]))])
    }

    public func setSourceGain(_ name: String, value: Double) {
        ble.setSourceControls(["gains": JSONValue(.object([name: .number(value)]))])
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
