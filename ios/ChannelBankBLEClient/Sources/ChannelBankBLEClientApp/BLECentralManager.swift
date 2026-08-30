#if canImport(ChannelBankCore)
import ChannelBankCore
#endif
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

public final class BLECentralManager: NSObject, ObservableObject, ChannelBankTransport {
    public static let serviceUUID = CBUUID(string: "7d2f0000-8c4b-4d7a-9a61-8e3c4f2a1000")
    private static let protocolUUID = CBUUID(string: "7d2f0001-8c4b-4d7a-9a61-8e3c4f2a1000")
    private static let commandUUID = CBUUID(string: "7d2f0002-8c4b-4d7a-9a61-8e3c4f2a1000")
    private static let responseUUID = CBUUID(string: "7d2f0003-8c4b-4d7a-9a61-8e3c4f2a1000")
    private static let stateUUID = CBUUID(string: "7d2f0004-8c4b-4d7a-9a61-8e3c4f2a1000")
    private static let audioUUID = CBUUID(string: "7d2f0005-8c4b-4d7a-9a61-8e3c4f2a1000")

    @Published public private(set) var status: BLEConnectionStatus = .idle
    @Published public private(set) var discovered: [DiscoveredPeripheral] = []
    @Published public private(set) var connectedName: String?
    @Published public private(set) var protocolDocument: String?
    @Published public private(set) var latestState: ChannelBankState?
    @Published public private(set) var recordings: RecordingList?
    @Published public private(set) var liveAudioDescriptor: LiveAudioDescriptor?
    @Published public private(set) var lastError: String?
    @Published public private(set) var broadScanActive = false
    @Published public private(set) var scanDiagnostics: [String] = []

    private lazy var central = CBCentralManager(delegate: self, queue: nil)
    private var peripheral: CBPeripheral?
    private var protocolCharacteristic: CBCharacteristic?
    private var commandCharacteristic: CBCharacteristic?
    private var responseCharacteristic: CBCharacteristic?
    private var stateCharacteristic: CBCharacteristic?
    private var audioCharacteristic: CBCharacteristic?
    private let responseAssembler = ChannelBankFrameAssembler()
    private let stateAssembler = ChannelBankFrameAssembler()
    private let decoder = JSONDecoder()
    private lazy var client = ChannelBankClient(transport: self)
    private var broadScanFallbackTask: Task<Void, Never>?
    private var scanRequested = false
    private var responseNotificationsEnabled = false
    private var stateNotificationsEnabled = false
    private var lastResponseCompletionTime: Date?

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
        Task { await performStateRefresh() }
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
        Task { await apply { try await self.client.setCenterHz(hz) } }
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

    public func setChannelBankSettings(_ body: [String: JSONValue]) {
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

    @MainActor
    private func apply(_ operation: @escaping () async throws -> ChannelBankState) async {
        do {
            latestState = try await operation()
            lastError = nil
        } catch {
            lastError = userVisibleError(error)
        }
    }

    @MainActor
    private func performStateRefresh() async {
        do {
            latestState = try await client.getState()
            lastError = nil
        } catch {
            lastError = userVisibleError(error)
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
        protocolCharacteristic = nil
        commandCharacteristic = nil
        responseCharacteristic = nil
        stateCharacteristic = nil
        audioCharacteristic = nil
        responseAssembler.reset()
        stateAssembler.reset()
        client.reset()
        responseNotificationsEnabled = false
        stateNotificationsEnabled = false
        lastResponseCompletionTime = nil
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

extension BLECentralManager: CBCentralManagerDelegate {
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

extension BLECentralManager: CBPeripheralDelegate {
    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            lastError = error.localizedDescription
            return
        }
        guard let service = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else {
            lastError = "SDR++ service not found"
            return
        }
        peripheral.discoverCharacteristics([Self.protocolUUID, Self.commandUUID, Self.responseUUID, Self.stateUUID, Self.audioUUID], for: service)
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
        }
    }

    public func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            lastError = error.localizedDescription
            return
        }
        guard let value = characteristic.value else { return }
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
        }
    }

    private func acceptStatePayload(_ payload: Data) throws {
        let envelope = try decoder.decode(ChannelBankResponse<ChannelBankState>.self, from: payload)
        if envelope.ok, let body = envelope.body {
            latestState = body
            lastError = nil
        } else if let error = envelope.error {
            lastError = "\(envelope.status) \(error.code): \(error.message)"
        }
    }

    private func looksLikeJSON(_ data: Data) -> Bool {
        data.first { !$0.isWhitespace } == UInt8(ascii: "{")
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

private extension UInt8 {
    var isWhitespace: Bool {
        self == UInt8(ascii: " ") ||
        self == UInt8(ascii: "\n") ||
        self == UInt8(ascii: "\r") ||
        self == UInt8(ascii: "\t")
    }
}
