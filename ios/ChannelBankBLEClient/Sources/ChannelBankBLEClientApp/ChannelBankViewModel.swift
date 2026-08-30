#if canImport(ChannelBankCore)
import ChannelBankCore
#endif
import Combine
import Foundation

@MainActor
public final class ChannelBankViewModel: ObservableObject {
    @Published public var centerMHzText = ""
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
    }

    public func tuneCenter() {
        guard let mhz = Double(centerMHzText), mhz > 0 else { return }
        ble.tuneCenter(hz: mhz * 1_000_000)
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
