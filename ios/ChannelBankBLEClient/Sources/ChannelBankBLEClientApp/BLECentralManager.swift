#if canImport(ChannelBankCore)
import ChannelBankCore
#endif
import AVFoundation
import CoreBluetooth
import Foundation

public struct DiscoveredPeripheral: Identifiable, Equatable {
    public let id: UUID
    public let name: String
    public let rssi: Int
    public let advertisesChannelBankService: Bool

    fileprivate let peripheral: CBPeripheral

    public static func == (lhs: DiscoveredPeripheral, rhs: DiscoveredPeripheral) -> Bool {
        lhs.id == rhs.id &&
        lhs.name == rhs.name &&
        lhs.rssi == rhs.rssi &&
        lhs.advertisesChannelBankService == rhs.advertisesChannelBankService
    }
}

public enum BLEConnectionStatus: Equatable {
    case unavailable(String)
    case idle
    case waitingForBluetooth
    case scanning
    case connecting(String)
    case discovering(String)
    case connected(String)
    case disconnected(String)

    public var label: String {
        switch self {
        case .unavailable(let reason): return "Bluetooth unavailable: \(reason)"
        case .idle: return "Idle"
        case .waitingForBluetooth: return "Waiting for Bluetooth"
        case .scanning: return "Scanning"
        case .connecting(let name): return "Connecting to \(name)"
        case .discovering(let name): return "Discovering \(name)"
        case .connected(let name): return "Connected to \(name)"
        case .disconnected(let reason): return "Disconnected: \(reason)"
        }
    }
}

@MainActor
public final class BLECentralManager: NSObject, ObservableObject, ChannelBankTransport {
    public static let serviceUUID = CBUUID(string: "7d2f0000-8c4b-4d7a-9a61-8e3c4f2a1000")
    private static let protocolUUID = CBUUID(string: "7d2f0001-8c4b-4d7a-9a61-8e3c4f2a1000")
    private static let commandUUID = CBUUID(string: "7d2f0002-8c4b-4d7a-9a61-8e3c4f2a1000")
    private static let responseUUID = CBUUID(string: "7d2f0003-8c4b-4d7a-9a61-8e3c4f2a1000")
    private static let stateUUID = CBUUID(string: "7d2f0004-8c4b-4d7a-9a61-8e3c4f2a1000")
    private static let audioUUID = CBUUID(string: "7d2f0005-8c4b-4d7a-9a61-8e3c4f2a1000")
    private static let stateSummaryUUID = CBUUID(string: "7d2f0006-8c4b-4d7a-9a61-8e3c4f2a1000")
    private static let snrTelemetryUUID = CBUUID(string: "7d2f0007-8c4b-4d7a-9a61-8e3c4f2a1000")
    private static let audioFileUUID = CBUUID(string: "7d2f0008-8c4b-4d7a-9a61-8e3c4f2a1000")

    @Published public private(set) var status: BLEConnectionStatus = .idle
    @Published public private(set) var discovered: [DiscoveredPeripheral] = []
    @Published public private(set) var connectedName: String?
    @Published public private(set) var protocolDocument: String?
    @Published public private(set) var latestState: ChannelBankState?
    @Published public private(set) var recordings: RecordingList?
    @Published public private(set) var liveAudioDescriptor: LiveAudioDescriptor?
    @Published public private(set) var audioMonitorStatus: String = "Idle"
    @Published public private(set) var lastError: String?
    @Published public private(set) var broadScanActive = false
    @Published public private(set) var scanDiagnostics: [String] = []

    private lazy var central = CBCentralManager(delegate: self, queue: .main)
    private var peripheral: CBPeripheral?
    private var protocolCharacteristic: CBCharacteristic?
    private var commandCharacteristic: CBCharacteristic?
    private var responseCharacteristic: CBCharacteristic?
    private var stateCharacteristic: CBCharacteristic?
    private var audioCharacteristic: CBCharacteristic?
    private var stateSummaryCharacteristic: CBCharacteristic?
    private var snrTelemetryCharacteristic: CBCharacteristic?
    private var audioFileCharacteristic: CBCharacteristic?
    private var audioFileNotificationsEnabled = false
    private let binaryAudioTransfer = BinaryAudioTransfer()
    private let responseAssembler = ChannelBankFrameAssembler()
    private let stateAssembler = ChannelBankFrameAssembler()
    private let stateSummaryAssembler = ChannelBankFrameAssembler()
    private let snrTelemetryAssembler = ChannelBankFrameAssembler()
    private let decoder = JSONDecoder()
    private lazy var client = ChannelBankClient(transport: self)
    private var broadScanFallbackTask: Task<Void, Never>?
    private var scanRequested = false
    private var responseNotificationsEnabled = false
    private var stateNotificationsEnabled = false
    private var stateSummaryNotificationsEnabled = false
    private var snrTelemetryNotificationsEnabled = false
    private var latestSnrTelemetry: SNRTelemetryFrame?
    private var latestSnrTelemetryAt: Date?
    private var snrTelemetryFrameCount = 0
    private var latestSequence: Int64?
    private var initialStateFallbackTask: Task<Void, Never>?
    private var hasReceivedFullState = false
    private var lastResponseCompletionTime: Date?
    private var pendingCenterTuneHz: Double?
    private var centerTuneRequestInFlight = false
    private var playbackTask: Task<Void, Never>?
    private var activePlaybackIdentity: String?
    private var deferredPlaybackIdentity: String?
    private var completedPlaybackIdentities: Set<String> = []
    private var audioPlayer: AVAudioPlayer?

    private final class PlaybackTransferLease: @unchecked Sendable {
        private let lock = NSLock()
        private var id: String?

        func set(_ id: String) {
            lock.lock()
            self.id = id
            lock.unlock()
        }

        func value() -> String? {
            lock.lock()
            defer { lock.unlock() }
            return id
        }
    }

    public override init() {
        super.init()
        _ = central
    }

    public func startScan() {
        discovered.removeAll()
        lastError = nil
        scanDiagnostics.removeAll()
        broadScanFallbackTask?.cancel()
        broadScanActive = false
        appendDiagnostic("Bluetooth state: \(central.state.diagnosticLabel)")
        scanRequested = true
        guard central.state == .poweredOn else {
            status = .waitingForBluetooth
            appendDiagnostic("Scan queued until Bluetooth is powered on")
            return
        }
        beginServiceScan()
    }

    public func stopScan() {
        broadScanFallbackTask?.cancel()
        broadScanFallbackTask = nil
        broadScanActive = false
        scanRequested = false
        central.stopScan()
        if case .scanning = status { status = .idle }
        if case .waitingForBluetooth = status { status = .idle }
    }

    public func connect(_ discoveredPeripheral: DiscoveredPeripheral) {
        stopScan()
        peripheral = discoveredPeripheral.peripheral
        peripheral?.delegate = self
        status = .connecting(discoveredPeripheral.name)
        central.connect(discoveredPeripheral.peripheral)
    }

    public func disconnect() {
        guard let peripheral else { return }
        central.cancelPeripheralConnection(peripheral)
    }

    public func refreshState() {
        Task { await performStateSummaryRefresh() }
    }

    public func setChannelBankRunning(_ running: Bool) {
        updateLatestState { $0.running = running }
        Task { await apply { try await self.client.setChannelBankRunning(running) } }
    }

    public func setRadioRunning(_ running: Bool) {
        updateLatestState { $0.radioPlaying = running }
        Task { await apply { try await self.client.setRadioRunning(running) } }
    }

    public func tuneCenter(hz: Double) {
        guard hz.isFinite, hz > 0 else { return }
        updateLatestState {
            $0.centerHz = hz
            $0.waterfallCenterHz = hz
        }
        pendingCenterTuneHz = hz
        guard !centerTuneRequestInFlight else { return }
        centerTuneRequestInFlight = true
        Task { @MainActor [weak self] in
            guard let self else { return }
            while let hz = self.pendingCenterTuneHz {
                self.pendingCenterTuneHz = nil
                await self.apply { try await self.client.setCenterHz(hz) }
            }
            self.centerTuneRequestInFlight = false
        }
    }

    public func applyTuningRange(_ range: ScannerTuningRange) async throws {
        let settings = try await client.setChannelBankSettings(["bwUsage": JSONValue(.number(range.bandwidthUsage))])
        acceptCommandStateResponse(settings)
        let tuned = try await client.setCenterHz(range.centerHz)
        acceptCommandStateResponse(tuned)
        lastError = nil
    }

    public func applyScannerBand(lowMHz: String, highMHz: String, airband: Bool, start: Bool) async throws -> ScannerBandPlan {
        var summary = try await client.getStateSummary()
        acceptStateSummary(summary)
        guard summary.running == false else { throw ScannerBandPlan.PlanError.alreadyRunning }
        let settings = try await client.getChannelBankSettings()
        // Validate before starting the source or changing configuration.
        var plan = try ScannerBandPlan(lowMHz: lowMHz, highMHz: highMHz,
            sampleRate: summary.sampleRate, settings: settings, airband: airband)
        if start && summary.radioPlaying != true {
            acceptCommandStateResponse(try await client.setRadioRunning(true))
            summary = try await client.getStateSummary()
            acceptStateSummary(summary)
            guard summary.radioPlaying == true else { throw ScannerBandPlan.PlanError.missingBandwidth }
            plan = try ScannerBandPlan(lowMHz: lowMHz, highMHz: highMHz,
                sampleRate: summary.sampleRate, settings: settings, airband: airband)
        }
        try Task.checkCancellation()
        acceptCommandStateResponse(try await client.setChannelBankSettings(plan.settingsBody))
        try Task.checkCancellation()
        acceptCommandStateResponse(try await client.setCenterHz(plan.centerHz))
        if start {
            try Task.checkCancellation()
            acceptCommandStateResponse(try await client.setChannelBankRunning(true))
        }
        lastError = nil
        return plan
    }

    public func setSource(_ name: String) {
        updateLatestState { $0.selectedSource = name }
        Task { await apply { try await self.client.setSource(name) } }
    }

    public func setSDRPPServer(host: String, port: Int) {
        Task { await apply { try await self.client.setSDRPPServer(host: host, port: port) } }
    }

    public func setSDRPPServerConnected(_ connected: Bool) {
        Task { await apply { try await self.client.setSDRPPServerConnected(connected) } }
    }

    public func setSourceOffset(selected: String, manualOffsetHz: Double? = nil) {
        Task { await apply { try await self.client.setSourceOffset(selected: selected, manualOffsetHz: manualOffsetHz) } }
    }

    public func setSourceControls(_ body: [String: JSONValue]) {
        Task { await apply { try await self.client.setSourceControls(body) } }
    }

    public func applyMultiReceiverScanner(_ draft: MultiReceiverScannerDraft) async throws {
        let summary = try await client.getStateSummary()
        acceptStateSummary(summary)
        let settings = try await client.getChannelBankSettings()
        guard var state = latestState else { throw ScannerBandPlan.PlanError.missingBandwidth }
        state.settings = settings
        let body = try draft.requestBody(state: state)
        acceptCommandStateResponse(try await client.setChannelBankSettings(body))
        // Summary does not contain scanner configuration; refresh it explicitly.
        let saved = try await client.getChannelBankSettings()
        updateLatestState { $0.settings = saved }
    }

    public func refreshRX888Source() {
        guard latestState?.radioPlaying != true, latestState?.sourceControls?.running != true else {
            lastError = "Stop RX888 before refreshing it."
            return
        }
        Task { await apply { try await self.client.setSourceControls(["refresh": JSONValue(.bool(true))]) } }
    }

    public func setChannelBankSettings(_ body: [String: JSONValue]) {
        applyOptimisticChannelBankSettings(body)
        Task { await apply { try await self.client.setChannelBankSettings(body) } }
    }

    public func setFrequency(_ hz: Double, blocked: Bool) {
        Task { await apply { try await self.client.setFrequency(hz, blocked: blocked) } }
    }

    public func setPlaybackLock(hz: Double) {
        Task { await apply { try await self.client.setPlaybackLock(hz: hz) } }
    }

    public func clearPlaybackLock() {
        Task { await apply { try await self.client.setPlaybackLock(hz: 0) } }
    }

    public func refreshRecordings() {
        Task { await loadRecordings() }
    }

    public func setRecordingSession(name: String) {
        Task { await applyRecordingList { try await self.client.setRecordingSession(name: name) } }
    }

    public func clearRecordedWavs() {
        Task { await applyClearWavs() }
    }

    public func loadLiveAudioDescriptor() {
        Task { await applyLiveAudioDescriptor() }
    }

    public func monitorCurrentPlayback() {
        guard playbackTask == nil, audioPlayer?.isPlaying != true else {
            appendDiagnostic("Audio monitoring already active; preserving the current clip")
            return
        }
        guard let state = latestState else {
            audioMonitorStatus = "Waiting for playback State"
            return
        }
        guard let identity = playbackIdentity(for: state) else {
            audioMonitorStatus = state.playback?.active == true ? "Waiting for playback file" : "No active playback"
            return
        }
        playbackTask?.cancel()
        completedPlaybackIdentities.remove(identity)
        activePlaybackIdentity = nil
        monitorPlaybackIfNeeded(state)
    }

    @MainActor
    private func apply(_ operation: @escaping () async throws -> ChannelBankState) async {
        do {
            let state = try await operation()
            acceptCommandStateResponse(state)
            lastError = nil
        } catch {
            lastError = userVisibleError(error)
        }
    }

    @MainActor
    private func performStateRefresh(suppressTimeoutError: Bool = false) async {
        do {
            let state = try await client.getState()
            acceptFullState(state)
            lastError = nil
        } catch {
            if suppressTimeoutError, error.isRequestTimeout {
                appendDiagnostic("Initial /api/state fallback timed out; still waiting for State indications")
                return
            }
            lastError = userVisibleError(error)
        }
    }

    @MainActor
    private func performStateSummaryRefresh() async {
        do {
            acceptStateSummary(try await client.getStateSummary())
            lastError = nil
        } catch {
            lastError = userVisibleError(error)
        }
    }

    private func requestInitialStateIfReady() {
        guard responseNotificationsEnabled,
              (stateNotificationsEnabled || stateSummaryNotificationsEnabled),
              latestState == nil,
              initialStateFallbackTask == nil else { return }
        appendDiagnostic(stateSummaryNotificationsEnabled ? "Waiting for subscribed State Summary snapshot" : "Waiting for subscribed State snapshot")
        initialStateFallbackTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 8_000_000_000)
            guard let self, self.latestState == nil else { return }
            self.appendDiagnostic("Fallback requesting /api/state/summary")
            await self.performStateSummaryRefresh()
        }
    }

    @MainActor
    private func loadRecordings() async {
        await applyRecordingList { try await self.client.getRecordings() }
    }

    @MainActor
    private func applyRecordingList(_ operation: @escaping () async throws -> RecordingList) async {
        do {
            recordings = try await operation()
            lastError = nil
        } catch {
            lastError = userVisibleError(error)
        }
    }

    @MainActor
    private func applyClearWavs() async {
        do {
            let result = try await client.clearRecordedWavs()
            lastError = "Clear WAVs: deleted \(result.deleted ?? 0), skipped \(result.skipped ?? 0)"
            recordings = try? await client.getRecordings()
        } catch {
            lastError = userVisibleError(error)
        }
    }

    @MainActor
    private func applyLiveAudioDescriptor() async {
        do {
            liveAudioDescriptor = try await client.getLiveAudioDescriptor()
            lastError = nil
        } catch {
            lastError = userVisibleError(error)
        }
    }

    private func updateLatestState(_ update: (inout ChannelBankState) -> Void) {
        guard var state = latestState else { return }
        update(&state)
        latestState = state
    }

    private func applyOptimisticChannelBankSettings(_ body: [String: JSONValue]) {
        updateLatestState { state in
            var settings = state.settings ?? ChannelBankSettings()
            for (key, json) in body {
                switch (key, json.value) {
                case ("mode", .string(let value)):
                    settings.mode = value
                    state.mode = value
                case ("demodMode", .string(let value)):
                    settings.demodMode = value
                    state.demodMode = value
                case ("spacingId", .number(let value)):
                    settings.spacingId = Int(value)
                case ("snrThresholdDb", .number(let value)):
                    settings.snrThresholdDb = value
                    state.snrThresholdDb = value
                case ("maxChannels", .number(let value)):
                    let channels = Int(value)
                    settings.maxChannels = channels
                    state.maxChannels = channels
                case ("bwUsage", .number(let value)):
                    settings.bwUsage = value
                    state.bwUsage = value
                case ("recordingEnabled", .bool(let value)):
                    settings.recordingEnabled = value
                    state.recordingEnabled = value
                case ("manualLocalSnrEnabled", .bool(let value)):
                    settings.manualLocalSnrEnabled = value
                case ("manualStormGuardEnabled", .bool(let value)):
                    settings.manualStormGuardEnabled = value
                case ("minTransmissionMs", .number(let value)):
                    settings.minTransmissionMs = Int(value)
                case ("signalHoldMs", .number(let value)):
                    settings.signalHoldMs = Int(value)
                case ("tailMs", .number(let value)):
                    settings.tailMs = Int(value)
                case ("scanQuietSec", .number(let value)):
                    settings.scanQuietSec = value
                case ("scanNoSignalSec", .number(let value)):
                    settings.scanNoSignalSec = value
                case ("transcriptionBackend", .number(let value)):
                    settings.transcriptionBackend = Int(value)
                default:
                    continue
                }
            }
            state.settings = settings
        }
    }

    private func userVisibleError(_ error: Error) -> String {
        if let clientError = error as? ChannelBankClientError {
            switch clientError {
            case .server(let serverError, let status):
                return "\(status) \(serverError.code): \(serverError.message)"
            case .requestTimedOut(let id):
                return id >= 0 ? "Request timed out id=\(id)" : "Request timed out"
            default:
                return String(describing: clientError)
            }
        }
        if let frameError = error as? ChannelBankFrameError {
            switch frameError {
            case .frameTooShort:
                return "Frame error: packet shorter than 8-byte header"
            case .unsupportedVersion(let version):
                return "Frame error: unsupported version byte \(version)"
            case .discontinuousOffset(let expected, let got):
                return "Frame error: expected offset \(expected), got \(got)"
            case .missingFirstFrame:
                return "Frame error: missing first fragment"
            case .emptyPayloadBudget:
                return "Frame error: empty payload budget"
            case .invalidUTF8:
                return "Frame error: invalid UTF-8"
            }
        }
        if error is DecodingError {
            return "Decode error: \(decodingDiagnostic(error))"
        }
        return error.localizedDescription
    }

    public func maximumCommandFrameLength() -> Int {
        guard let peripheral, let commandCharacteristic else { return 180 }
        return peripheral.maximumWriteValueLength(for: commandCharacteristic.properties.contains(.write) ? .withResponse : .withoutResponse)
    }

    public func writeCommandFrame(_ data: Data) async throws {
        guard let peripheral, let commandCharacteristic else { return }
        logOutgoingCommandFrame(data)
        let type: CBCharacteristicWriteType = commandCharacteristic.properties.contains(.write) ? .withResponse : .withoutResponse
        peripheral.writeValue(data, for: commandCharacteristic, type: type)
    }

    private func resetConnectionState(reason: String) {
        connectedName = nil
        protocolDocument = nil
        latestState = nil
        recordings = nil
        liveAudioDescriptor = nil
        audioMonitorStatus = "Idle"
        protocolCharacteristic = nil
        commandCharacteristic = nil
        responseCharacteristic = nil
        stateCharacteristic = nil
        audioCharacteristic = nil
        stateSummaryCharacteristic = nil
        snrTelemetryCharacteristic = nil
        audioFileCharacteristic = nil
        audioFileNotificationsEnabled = false
        responseAssembler.reset()
        stateAssembler.reset()
        stateSummaryAssembler.reset()
        snrTelemetryAssembler.reset()
        client.reset()
        responseNotificationsEnabled = false
        stateNotificationsEnabled = false
        stateSummaryNotificationsEnabled = false
        snrTelemetryNotificationsEnabled = false
        latestSnrTelemetry = nil
        latestSnrTelemetryAt = nil
        snrTelemetryFrameCount = 0
        latestSequence = nil
        initialStateFallbackTask?.cancel()
        initialStateFallbackTask = nil
        hasReceivedFullState = false
        lastResponseCompletionTime = nil
        pendingCenterTuneHz = nil
        centerTuneRequestInFlight = false
        playbackTask?.cancel()
        playbackTask = nil
        activePlaybackIdentity = nil
        deferredPlaybackIdentity = nil
        audioPlayer?.stop()
        audioPlayer = nil
        status = .disconnected(reason)
    }

    private func beginServiceScan() {
        status = .scanning
        appendDiagnostic("Scanning for Channel Bank service UUID")
        central.scanForPeripherals(withServices: [Self.serviceUUID], options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        broadScanFallbackTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 3_000_000_000)
            guard let self,
                  self.scanRequested,
                  case .scanning = self.status,
                  self.discovered.isEmpty else { return }
            self.broadScanActive = true
            self.appendDiagnostic("No UUID match after 3s; scanning all nearby BLE")
            self.central.stopScan()
            self.central.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        }
    }

    private func appendDiagnostic(_ message: String) {
        scanDiagnostics.append("\(Self.diagnosticTimestamp()) \(message)")
        if scanDiagnostics.count > 40 {
            scanDiagnostics.removeFirst(scanDiagnostics.count - 40)
        }
    }

    private static func diagnosticTimestamp(_ date: Date = Date()) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss.SSS"
        return formatter.string(from: date)
    }
}

// CoreBluetooth delivers these Objective-C delegate calls on the configured main queue.
extension BLECentralManager: @preconcurrency CBCentralManagerDelegate {
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            appendDiagnostic("Bluetooth powered on")
            if scanRequested {
                beginServiceScan()
            } else {
                status = .idle
            }
        case .unsupported:
            status = .unavailable("unsupported")
            appendDiagnostic("Bluetooth unsupported")
        case .unauthorized:
            status = .unavailable("permission denied")
            appendDiagnostic("Bluetooth permission denied")
        case .poweredOff:
            status = .unavailable("powered off")
            appendDiagnostic("Bluetooth powered off")
        default:
            status = .unavailable("not ready")
            appendDiagnostic("Bluetooth not ready: \(central.state.diagnosticLabel)")
        }
    }

    public func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        let advertisedServices = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
        let advertisesChannelBankService = advertisedServices.contains(Self.serviceUUID)
        let name = peripheral.name ?? advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? "Nearby BLE Device"
        let item = DiscoveredPeripheral(
            id: peripheral.identifier,
            name: name,
            rssi: RSSI.intValue,
            advertisesChannelBankService: advertisesChannelBankService,
            peripheral: peripheral
        )
        if let index = discovered.firstIndex(where: { $0.id == item.id }) {
            discovered[index] = item
        } else {
            discovered.append(item)
        }
        appendDiagnostic("\(broadScanActive ? "Broad" : "UUID") discovery: \(name), \(RSSI.intValue) dBm, services=\(advertisedServices.count)\(advertisesChannelBankService ? ", CB" : "")")
    }

    public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectedName = peripheral.name ?? "SDR++ Channel Bank"
        status = .discovering(connectedName ?? "SDR++ Channel Bank")
        peripheral.discoverServices([Self.serviceUUID])
    }

    public func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        resetConnectionState(reason: error?.localizedDescription ?? "failed to connect")
    }

    public func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        resetConnectionState(reason: error?.localizedDescription ?? "peripheral disconnected")
    }
}

private extension CBManagerState {
    var diagnosticLabel: String {
        switch self {
        case .unknown: return "unknown"
        case .resetting: return "resetting"
        case .unsupported: return "unsupported"
        case .unauthorized: return "unauthorized"
        case .poweredOff: return "powered off"
        case .poweredOn: return "powered on"
        @unknown default: return "unrecognized"
        }
    }
}

extension BLECentralManager: @preconcurrency CBPeripheralDelegate {
    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            lastError = error.localizedDescription
            return
        }
        guard let service = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else {
            lastError = "SDR++ service not found"
            return
        }
        peripheral.discoverCharacteristics([
            Self.protocolUUID,
            Self.commandUUID,
            Self.responseUUID,
            Self.stateUUID,
            Self.audioUUID,
            Self.stateSummaryUUID,
            Self.snrTelemetryUUID,
            Self.audioFileUUID
        ], for: service)
    }

    public func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error {
            lastError = error.localizedDescription
            return
        }
        for characteristic in service.characteristics ?? [] {
            switch characteristic.uuid {
            case Self.protocolUUID: protocolCharacteristic = characteristic
            case Self.commandUUID: commandCharacteristic = characteristic
            case Self.responseUUID: responseCharacteristic = characteristic
            case Self.stateUUID: stateCharacteristic = characteristic
            case Self.audioUUID: audioCharacteristic = characteristic
            case Self.stateSummaryUUID: stateSummaryCharacteristic = characteristic
            case Self.snrTelemetryUUID: snrTelemetryCharacteristic = characteristic
            case Self.audioFileUUID: audioFileCharacteristic = characteristic
            default: break
            }
        }
        guard protocolCharacteristic != nil, commandCharacteristic != nil, responseCharacteristic != nil, stateCharacteristic != nil, audioCharacteristic != nil else {
            lastError = "Not all Channel Bank characteristics were discovered"
            return
        }
        if let protocolCharacteristic {
            peripheral.readValue(for: protocolCharacteristic)
        }
        if let responseCharacteristic {
            peripheral.setNotifyValue(true, for: responseCharacteristic)
        }
        if let stateCharacteristic {
            peripheral.setNotifyValue(true, for: stateCharacteristic)
        }
        if let stateSummaryCharacteristic {
            peripheral.setNotifyValue(true, for: stateSummaryCharacteristic)
        } else {
            appendDiagnostic("State Summary characteristic not present")
        }
        if let snrTelemetryCharacteristic {
            peripheral.setNotifyValue(true, for: snrTelemetryCharacteristic)
        } else {
            appendDiagnostic("High-rate SNR telemetry characteristic not present")
        }
        if let audioFileCharacteristic {
            peripheral.setNotifyValue(true, for: audioFileCharacteristic)
        } else {
            appendDiagnostic("Binary Audio File unavailable; using paged audio")
        }
        connectedName = peripheral.name ?? connectedName
        status = .connected(connectedName ?? "SDR++ Channel Bank")
    }

    public func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            lastError = error.localizedDescription
            return
        }
        if characteristic.uuid == Self.responseUUID {
            responseNotificationsEnabled = characteristic.isNotifying
            appendDiagnostic("Response indications \(characteristic.isNotifying ? "enabled" : "disabled")")
        } else if characteristic.uuid == Self.stateUUID {
            stateNotificationsEnabled = characteristic.isNotifying
            appendDiagnostic("State indications \(characteristic.isNotifying ? "enabled" : "disabled")")
        } else if characteristic.uuid == Self.stateSummaryUUID {
            stateSummaryNotificationsEnabled = characteristic.isNotifying
            appendDiagnostic("State Summary indications \(characteristic.isNotifying ? "enabled" : "disabled")")
        } else if characteristic.uuid == Self.snrTelemetryUUID {
            snrTelemetryNotificationsEnabled = characteristic.isNotifying
            appendDiagnostic("High-rate SNR telemetry notifications \(characteristic.isNotifying ? "enabled" : "disabled")")
        } else if characteristic.uuid == Self.audioFileUUID {
            audioFileNotificationsEnabled = characteristic.isNotifying
            appendDiagnostic("Binary Audio File notifications \(characteristic.isNotifying ? "enabled" : "disabled")")
        }
        requestInitialStateIfReady()
    }

    public func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            lastError = error.localizedDescription
            return
        }
        guard let value = characteristic.value else { return }
        if characteristic.uuid == Self.audioFileUUID {
            binaryAudioTransfer.receive(value)
            return
        }
        if characteristic.uuid == Self.protocolUUID {
            protocolDocument = String(data: value, encoding: .utf8)
            return
        }
        if characteristic.uuid == Self.responseUUID {
            do {
                logIncomingFrame(value, label: "Response")
                if let complete = try responseAssembler.push(value) {
                    lastResponseCompletionTime = Date()
                    appendDiagnostic("RX Response complete id=\(complete.messageID) bytes=\(complete.payload.count)")
                    client.receiveResponsePayload(complete.payload)
                }
            } catch {
                if looksLikeJSON(value) {
                    appendDiagnostic("RX raw Response JSON bytes=\(value.count)")
                    client.receiveResponsePayload(value)
                } else if let frameError = error as? ChannelBankFrameError,
                          frameError.isRecoverableFragmentLoss {
                    appendDiagnostic("Dropped partial Response frame: \(frameError.diagnosticLabel)")
                } else {
                    lastError = userVisibleError(error)
                }
            }
        } else if characteristic.uuid == Self.stateUUID {
            do {
                logIncomingFrame(value, label: "State")
                if let complete = try stateAssembler.push(value) {
                    appendDiagnostic("RX State complete id=\(complete.messageID) bytes=\(complete.payload.count)")
                    try acceptStatePayload(complete.payload)
                }
            } catch {
                if looksLikeJSON(value) {
                    do {
                        appendDiagnostic("RX raw State JSON bytes=\(value.count)")
                        try acceptStatePayload(value)
                    } catch {
                        appendDiagnostic("Ignored undecodable State JSON: \(decodingDiagnostic(error))")
                    }
                } else if let frameError = error as? ChannelBankFrameError,
                          frameError.isRecoverableFragmentLoss {
                    appendDiagnostic("Dropped partial State frame: \(frameError.diagnosticLabel)")
                } else {
                    appendDiagnostic("Ignored State frame: \(userVisibleError(error))")
                }
            }
        } else if characteristic.uuid == Self.stateSummaryUUID {
            do {
                logIncomingFrame(value, label: "State Summary")
                if let complete = try stateSummaryAssembler.push(value) {
                    appendDiagnostic("RX State Summary complete id=\(complete.messageID) bytes=\(complete.payload.count)")
                    try acceptStateSummaryPayload(complete.payload)
                }
            } catch {
                if looksLikeJSON(value) {
                    do {
                        appendDiagnostic("RX raw State Summary JSON bytes=\(value.count)")
                        try acceptStateSummaryPayload(value)
                    } catch {
                        appendDiagnostic("Ignored undecodable State Summary JSON: \(decodingDiagnostic(error))")
                    }
                } else if let frameError = error as? ChannelBankFrameError,
                          frameError.isRecoverableFragmentLoss {
                    appendDiagnostic("Dropped partial State Summary frame: \(frameError.diagnosticLabel)")
                } else {
                    appendDiagnostic("Ignored State Summary frame: \(userVisibleError(error))")
                }
            }
        } else if characteristic.uuid == Self.snrTelemetryUUID {
            do {
                if let complete = try snrTelemetryAssembler.push(value) {
                    try acceptSNRTelemetryPayload(complete.payload)
                }
            } catch let error as ChannelBankFrameError where error.isRecoverableFragmentLoss {
                appendDiagnostic("Dropped partial SNR telemetry frame: \(error.diagnosticLabel)")
            } catch {
                appendDiagnostic("Ignored SNR telemetry frame: \(userVisibleError(error))")
            }
        }
    }

    private func acceptStatePayload(_ payload: Data) throws {
        let envelope = try decoder.decode(ChannelBankResponse<ChannelBankState>.self, from: payload)
        if envelope.ok, let body = envelope.body {
            acceptFullState(body)
            lastError = nil
        } else if let error = envelope.error {
            lastError = "\(envelope.status) \(error.code): \(error.message)"
        }
    }

    private func acceptStateSummaryPayload(_ payload: Data) throws {
        if !topLevelJSONHasKey("ok", in: payload),
           let summary = try? decoder.decode(ChannelBankStateSummary.self, from: payload) {
            acceptStateSummary(summary)
            return
        }

        let envelope = try decoder.decode(ChannelBankResponse<ChannelBankStateSummary>.self, from: payload)
        if envelope.ok, let body = envelope.body {
            acceptStateSummary(body)
            lastError = nil
        } else if let error = envelope.error {
            lastError = "\(envelope.status) \(error.code): \(error.message)"
        }
    }

    private func acceptSNRTelemetryPayload(_ payload: Data) throws {
        let frame = try SNRTelemetryFrame(payload: payload)
        latestSnrTelemetry = frame
        latestSnrTelemetryAt = Date()
        snrTelemetryFrameCount += 1
        var state = latestState ?? ChannelBankState()
        state.snrOverview = frame.points
        latestState = state
        if snrTelemetryFrameCount == 1 || snrTelemetryFrameCount.isMultiple(of: 20) {
            appendDiagnostic("RX high-rate SNR telemetry seq=\(frame.sequence) points=\(frame.points.count) frames=\(snrTelemetryFrameCount)")
        }
    }

    private func preserveFreshSNRTelemetry(in state: inout ChannelBankState) {
        guard let frame = latestSnrTelemetry,
              let receivedAt = latestSnrTelemetryAt,
              Date().timeIntervalSince(receivedAt) < 3 else { return }
        state.snrOverview = frame.points
    }

    private func acceptFullState(_ state: ChannelBankState) {
        if state.isSummaryShaped {
            acceptStateSummary(state.asSummary)
            return
        }
        if let seq = state.seq {
            if let latestSequence, seq < latestSequence {
                var merged = latestState ?? ChannelBankState()
                merged.mergeTelemetry(from: state)
                preserveFreshSNRTelemetry(in: &merged)
                latestState = merged
                hasReceivedFullState = true
                appendDiagnostic("Merged delayed State telemetry seq=\(seq) latest=\(latestSequence)")
                monitorPlaybackIfNeeded(merged)
                return
            }
            latestSequence = seq
        }
        hasReceivedFullState = true
        var merged = state
        preserveFreshSNRTelemetry(in: &merged)
        latestState = merged
        initialStateFallbackTask?.cancel()
        initialStateFallbackTask = nil
        monitorPlaybackIfNeeded(merged)
    }

    private func acceptCommandStateResponse(_ state: ChannelBankState) {
        if state.isSummaryShaped {
            appendDiagnostic("Merging compact State response seq=\(state.seq.map(String.init) ?? "-")")
            acceptStateSummary(state.asSummary)
        } else {
            acceptFullState(state)
        }
    }

    private func acceptStateSummary(_ summary: ChannelBankStateSummary) {
        if let seq = summary.seq {
            if let latestSequence, seq <= latestSequence {
                appendDiagnostic("Ignored stale State Summary seq=\(seq) latest=\(latestSequence)")
                return
            }
            latestSequence = seq
        }
        var state = latestState ?? ChannelBankState()
        state.merge(summary: summary)
        latestState = state
        initialStateFallbackTask?.cancel()
        initialStateFallbackTask = nil
        monitorPlaybackIfNeeded(state)
        lastError = nil
    }

    private func monitorPlaybackIfNeeded(_ state: ChannelBankState) {
        guard let identity = playbackIdentity(for: state) else { return }
        guard identity != activePlaybackIdentity, !completedPlaybackIdentities.contains(identity) else { return }

        // The server lease keeps this file readable even after playback advances.
        // Restarting on every new filename can prevent any BLE download reaching EOF.
        guard playbackTask == nil, audioPlayer?.isPlaying != true else {
            if deferredPlaybackIdentity != identity {
                deferredPlaybackIdentity = identity
                appendDiagnostic("Audio source advanced; finishing current clip before following latest playback")
            }
            return
        }

        playbackTask?.cancel()
        deferredPlaybackIdentity = nil
        activePlaybackIdentity = identity
        audioMonitorStatus = "Pulling \(state.playback?.fileName ?? state.playback?.name ?? "playback")"
        appendDiagnostic("Audio pull start id=\(identity)")
        playbackTask = Task { [weak self, state, identity] in
            await self?.pullAndPlay(state: state, identity: identity)
        }
    }

    private func playbackIdentity(for state: ChannelBankState) -> String? {
        guard state.playback?.active == true else { return nil }
        if let file = state.playback?.file, !file.isEmpty { return file }
        if let fileName = state.playback?.fileName, !fileName.isEmpty {
            if let key = state.playback?.freqKey { return "\(key):\(fileName)" }
            return fileName
        }
        return nil
    }

    private func pullAndPlay(state: ChannelBankState, identity: String) async {
        defer {
            if activePlaybackIdentity == identity { playbackTask = nil }
        }
        do {
            let download = try await pullCurrentPlaybackWithRetry()
            try Task.checkCancellation()
            let url = try await writePlaybackFile(download, fallbackName: state.playback?.fileName ?? state.playback?.name)
            try Task.checkCancellation()
            await play(url: url, identity: identity, bytes: download.data.count)
        } catch is CancellationError {
            await MainActor.run {
                if activePlaybackIdentity == identity {
                    audioMonitorStatus = "Canceled"
                }
            }
        } catch {
            appendDiagnostic("Current playback pull failed: \(userVisibleError(error))")
            do {
                let download = try await pullRecordingFallback(state: state)
                try Task.checkCancellation()
                let url = try await writePlaybackFile(download, fallbackName: state.playback?.fileName ?? state.playback?.name)
                try Task.checkCancellation()
                await play(url: url, identity: identity, bytes: download.data.count)
            } catch is CancellationError {
                await MainActor.run {
                    if activePlaybackIdentity == identity {
                        audioMonitorStatus = "Canceled"
                    }
                }
            } catch {
                await MainActor.run {
                    if activePlaybackIdentity == identity {
                        audioMonitorStatus = "Playback pull failed"
                        lastError = userVisibleError(error)
                    }
                }
            }
        }
    }

    private struct PulledAudio: Sendable {
        var data: Data
        var name: String?
        var contentType: String?
    }

    private func pullCurrentPlaybackWithRetry() async throws -> PulledAudio {
        let delays: [UInt64] = [0, 250_000_000, 500_000_000, 1_000_000_000]
        var lastError: Error?
        for delay in delays {
            if delay > 0 { try await Task.sleep(nanoseconds: delay) }
            do {
                return try await pullLeasedCurrentPlayback()
            } catch {
                lastError = error
                guard error.isHTTPStatus(404) else { throw error }
            }
        }
        throw lastError ?? ChannelBankClientError.requestTimedOut(-1)
    }

    private func pullLeasedCurrentPlayback() async throws -> PulledAudio {
        let lease = PlaybackTransferLease()
        let cancelLease: @Sendable () -> Void = { [weak self, lease] in
            guard let self, let transferId = lease.value() else { return }
            Task {
                try? await self.client.cancelCurrentPlayback(transferId: transferId)
            }
        }
        return try await withTaskCancellationHandler(operation: {
            do {
                var firstPage: RecordingPage?
                var cachedPage: RecordingPage?
                if audioFileNotificationsEnabled {
                    let preparingSince = Date()
                    var page: RecordingPage
                    repeat {
                        try Task.checkCancellation()
                        page = try await client.currentPlaybackPage(offset: 0, transferId: lease.value(), binary: true)
                        if let token = page.transferId {
                            if let previous = lease.value(), previous != token {
                                throw RecordingPaginationError.transferIDChanged(expected: previous, got: token)
                            }
                            lease.set(token)
                        }
                        guard lease.value() != nil else { throw RecordingPaginationError.missingTransferID }
                        if page.preparing == true {
                            updateAudioProgress(page)
                            guard Date().timeIntervalSince(preparingSince) < 20 else {
                                throw RecordingPaginationError.preparationTimedOut
                            }
                            try await Task.sleep(nanoseconds: UInt64(min(1000, max(100, page.retryAfterMs ?? 250))) * 1_000_000)
                        }
                    } while page.preparing == true
                    if let streamID = page.streamId, let size = page.size, let token = lease.value() {
                        let started = Date()
                        appendDiagnostic("Binary audio start size=\(size) preparationMs=\(Int(started.timeIntervalSince(preparingSince) * 1000))")
                        do {
                            let data = try await binaryAudioTransfer.collect(
                                transferId: token, streamId: streamID, size: size,
                                request: { offset, windowID in
                                    try await self.client.audioWindow(transferId: token, offset: offset, windowId: windowID)
                                }, progress: { received, total in
                                    var progress = page
                                    progress.nextOffset = received
                                    self.updateAudioProgress(progress)
                                })
                            let seconds = max(0.001, Date().timeIntervalSince(started))
                            appendDiagnostic(String(format: "Binary audio complete bytes=%d seconds=%.2f KB/s=%.1f retries=%d", data.count, seconds, Double(data.count) / seconds / 1024, binaryAudioTransfer.retryCount))
                            cancelLease()
                            return PulledAudio(data: data, name: page.name, contentType: page.contentType)
                        } catch is CancellationError {
                            throw CancellationError()
                        } catch {
                            try Task.checkCancellation()
                            appendDiagnostic("Binary audio failed; falling back on same lease: \(userVisibleError(error))")
                        }
                    } else if page.dataBase64 != nil {
                        cachedPage = page
                    }
                }
                let fallbackToken = lease.value()
                let started = Date()
                let recording = try await RecordingPaginator().collectLeased { offset, transferId in
                    let page: RecordingPage
                    if let cached = cachedPage {
                        page = cached
                        cachedPage = nil
                    } else {
                        page = try await self.client.currentPlaybackPage(offset: offset, transferId: transferId ?? fallbackToken)
                    }
                    try Task.checkCancellation()
                    if let pageTransferId = page.transferId { lease.set(pageTransferId) }
                    if firstPage == nil, page.preparing != true { firstPage = page }
                    self.updateAudioProgress(page)
                    self.appendDiagnostic("Audio page transfer=\(page.transferId ?? "missing") offset=\(page.offset ?? offset) eof=\(page.eof == true)")
                    return page
                }
                let seconds = max(0.001, Date().timeIntervalSince(started))
                appendDiagnostic(String(format: "Paged audio complete bytes=%d seconds=%.2f KB/s=%.1f", recording.data.count, seconds, Double(recording.data.count) / seconds / 1024))
                return PulledAudio(data: recording.data, name: firstPage?.name, contentType: firstPage?.contentType)
            } catch {
                cancelLease()
                throw error
            }
        }, onCancel: cancelLease)
    }

    private func pullRecordingFallback(state: ChannelBankState) async throws -> PulledAudio {
        await MainActor.run { audioMonitorStatus = "Trying recordings fallback" }
        let list = try await client.getRecordings()
        await MainActor.run { recordings = list }
        guard let file = matchingRecordingFile(state: state, list: list) else {
            throw ChannelBankClientError.server(
                ChannelBankErrorBody(code: "recording_not_found", message: "Current playback file was not found in recordings"),
                status: 404
            )
        }
        return try await pullPages { offset in
            try await self.client.recordingPage(file: file.path, offset: offset)
        }
    }

    private func matchingRecordingFile(state: ChannelBankState, list: RecordingList) -> RecordingItem? {
        guard let files = list.files else { return nil }
        let playbackFile = state.playback?.file
        let playbackName = state.playback?.fileName ?? state.playback?.name
        if let playbackFile, let exact = files.first(where: { $0.path == playbackFile }) {
            return exact
        }
        if let playbackFile, let suffix = files.first(where: { playbackFile.hasSuffix($0.path) || $0.path.hasSuffix(playbackFile) }) {
            return suffix
        }
        if let playbackName, let named = files.first(where: { $0.name == playbackName || $0.path.hasSuffix(playbackName) }) {
            return named
        }
        return nil
    }

    private func pullPages(fetch: @escaping (Int) async throws -> RecordingPage) async throws -> PulledAudio {
        var firstPage: RecordingPage?
        let data = try await RecordingPaginator().collect { offset in
            let page = try await fetch(offset)
            try Task.checkCancellation()
            if firstPage == nil { firstPage = page }
            self.updateAudioProgress(page)
            return page
        }
        return PulledAudio(data: data, name: firstPage?.name, contentType: firstPage?.contentType)
    }

    private func updateAudioProgress(_ page: RecordingPage) {
        if page.preparing == true {
            audioMonitorStatus = "Preparing compressed audio: \(page.name ?? "audio")"
            return
        }
        let received = page.nextOffset ?? page.offset ?? 0
        let receivedText = ByteCountFormatter.string(fromByteCount: Int64(received), countStyle: .file)
        let totalText = page.size.map { ByteCountFormatter.string(fromByteCount: Int64($0), countStyle: .file) }
        audioMonitorStatus = "Downloading \(page.name ?? "audio"): \(receivedText)\(totalText.map { " of \($0)" } ?? "")"
    }

    nonisolated private func writePlaybackFile(_ audio: PulledAudio, fallbackName: String?) async throws -> URL {
        let rawName = audio.name ?? fallbackName ?? "channel-bank-playback"
        let ext = playbackFileExtension(contentType: audio.contentType, name: rawName)
        let safeName = rawName
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { !$0.isEmpty }
            .joined(separator: "-")
        let base = safeName.isEmpty ? "channel-bank-playback" : safeName
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("\(base)-\(UUID().uuidString)")
            .appendingPathExtension(ext)
        try audio.data.write(to: url, options: .atomic)
        return url
    }

    nonisolated private func playbackFileExtension(contentType: String?, name: String) -> String {
        let lowerName = name.lowercased()
        if lowerName.hasSuffix(".m4a") { return "m4a" }
        if lowerName.hasSuffix(".wav") { return "wav" }
        let lowerType = contentType?.lowercased() ?? ""
        if lowerType.contains("mp4") || lowerType.contains("m4a") { return "m4a" }
        return "wav"
    }

    @MainActor
    private func play(url: URL, identity: String, bytes: Int) async {
        #if os(iOS)
        do {
            try AVAudioSession.sharedInstance().setCategory(.playback, mode: .default, options: [])
            try AVAudioSession.sharedInstance().setActive(true)
        } catch {
            lastError = error.localizedDescription
            audioMonitorStatus = "Audio session failed: \(error.localizedDescription)"
            return
        }
        #endif
        do {
            audioPlayer?.stop()
            let player = try AVAudioPlayer(contentsOf: url)
            player.delegate = self
            guard player.prepareToPlay(), player.play() else {
                audioMonitorStatus = "Playback failed: unable to start the downloaded audio"
                appendDiagnostic(audioMonitorStatus)
                return
            }
            audioPlayer = player
            completedPlaybackIdentities.insert(identity)
            audioMonitorStatus = "Playing \(url.lastPathComponent)"
            appendDiagnostic("Audio pull complete id=\(identity) bytes=\(bytes)")
        } catch {
            audioMonitorStatus = "Playback failed"
            lastError = error.localizedDescription
        }
    }

    private func looksLikeJSON(_ data: Data) -> Bool {
        data.first { !$0.isWhitespace } == UInt8(ascii: "{")
    }

    private func topLevelJSONHasKey(_ key: String, in data: Data) -> Bool {
        guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return false }
        return object[key] != nil
    }

    private func logOutgoingCommandFrame(_ data: Data) {
        guard let frame = try? ChannelBankFrame(data: data) else {
            appendDiagnostic("TX Command raw bytes=\(data.count)")
            return
        }
        guard frame.isFirst else { return }
        var details = ""
        if frame.isLast,
           let request = try? decoder.decode(ChannelBankRequest.self, from: frame.payload) {
            details = " \(request.method) \(request.path)"
        }
        appendDiagnostic("TX Command first id=\(frame.messageID) offset=\(frame.offset) bytes=\(data.count)\(details)")
    }

    private func logIncomingFrame(_ data: Data, label: String) {
        guard let frame = try? ChannelBankFrame(data: data) else { return }
        if frame.isFirst {
            var suffix = ""
            if label == "State", let responseTime = lastResponseCompletionTime {
                let ms = Int(Date().timeIntervalSince(responseTime) * 1000)
                suffix = " afterResponseMs=\(ms)"
            }
            appendDiagnostic("RX \(label) first id=\(frame.messageID) offset=\(frame.offset) bytes=\(data.count)\(suffix)")
        }
    }

    private func decodingDiagnostic(_ error: Error) -> String {
        guard let decodingError = error as? DecodingError else {
            return userVisibleError(error)
        }
        switch decodingError {
        case .typeMismatch(let type, let context):
            return "typeMismatch expected=\(type) path=\(Self.codingPath(context.codingPath)) \(context.debugDescription)"
        case .valueNotFound(let type, let context):
            return "valueNotFound expected=\(type) path=\(Self.codingPath(context.codingPath)) \(context.debugDescription)"
        case .keyNotFound(let key, let context):
            return "keyNotFound key=\(key.stringValue) path=\(Self.codingPath(context.codingPath)) \(context.debugDescription)"
        case .dataCorrupted(let context):
            return "dataCorrupted path=\(Self.codingPath(context.codingPath)) \(context.debugDescription)"
        @unknown default:
            return String(describing: decodingError)
        }
    }

    private static func codingPath(_ path: [CodingKey]) -> String {
        guard !path.isEmpty else { return "$" }
        return path.map(\.stringValue).joined(separator: ".")
    }
}

extension BLECentralManager: @preconcurrency AVAudioPlayerDelegate {
    public func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        guard audioPlayer === player else { return }
        audioPlayer = nil
        audioMonitorStatus = flag ? "Finished" : "Playback ended with an error"
        appendDiagnostic(audioMonitorStatus)
        if let state = latestState { monitorPlaybackIfNeeded(state) }
    }

    public func audioPlayerDecodeErrorDidOccur(_ player: AVAudioPlayer, error: Error?) {
        guard audioPlayer === player else { return }
        audioPlayer = nil
        audioMonitorStatus = "Playback decode failed: \(error?.localizedDescription ?? "Invalid audio file")"
        appendDiagnostic(audioMonitorStatus)
    }
}

private extension ChannelBankFrameError {
    var isRecoverableFragmentLoss: Bool {
        switch self {
        case .missingFirstFrame, .discontinuousOffset:
            return true
        default:
            return false
        }
    }

    var diagnosticLabel: String {
        switch self {
        case .frameTooShort:
            return "short packet"
        case .unsupportedVersion(let version):
            return "version \(version)"
        case .discontinuousOffset(let expected, let got):
            return "expected offset \(expected), got \(got)"
        case .missingFirstFrame:
            return "missing first fragment"
        case .emptyPayloadBudget:
            return "empty payload budget"
        case .invalidUTF8:
            return "invalid UTF-8"
        }
    }
}

private extension Error {
    func isHTTPStatus(_ status: Int) -> Bool {
        guard let error = self as? ChannelBankClientError else { return false }
        if case .server(_, let actualStatus) = error {
            return actualStatus == status
        }
        return false
    }

    var isRequestTimeout: Bool {
        guard let error = self as? ChannelBankClientError else { return false }
        if case .requestTimedOut = error {
            return true
        }
        return false
    }
}

private extension UInt8 {
    var isWhitespace: Bool {
        self == UInt8(ascii: " ") ||
        self == UInt8(ascii: "\n") ||
        self == UInt8(ascii: "\r") ||
        self == UInt8(ascii: "\t")
    }
}
