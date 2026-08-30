#if canImport(ChannelBankCore)
import ChannelBankCore
#endif
import SwiftUI

public struct ChannelBankRootView: View {
    @StateObject private var model = ChannelBankViewModel()

    public init() {}

    public var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 14) {
                    connectionPanel
                    if let state = model.ble.latestState {
                        radioPanel(state)
                        centerPanel(state)
                        channelBankPanel(state)
                        SNRChartView(state: state)
                        ActivityWaterfallView(store: model.waterfall) { fraction in
                            model.selectWaterfallFrequency(at: fraction)
                        }
                        activeChannelsPanel(state)
                        activityHistoryPanel(state)
                        playbackPanel(state)
                        settingsPanel(state)
                        diagnosticsPanel(state)
                    } else {
                        emptyStatePanel
                    }
                }
                .padding(14)
            }
            .background(Color(red: 0.03, green: 0.04, blue: 0.05))
            .navigationTitle("Channel Bank")
            .iosInlineNavigationTitle()
            .preferredColorScheme(.dark)
            .onReceive(model.ble.$latestState) { _ in model.acceptStateUpdate() }
            .alert("Frequency", isPresented: Binding(get: { model.pendingBlockPoint != nil }, set: { if !$0 { model.pendingBlockPoint = nil } })) {
                Button(model.pendingBlockPoint?.blocked == true ? "Unblock" : "Block", role: model.pendingBlockPoint?.blocked == true ? .none : .destructive) {
                    model.confirmBlockToggle()
                }
                Button("Cancel", role: .cancel) { model.pendingBlockPoint = nil }
            } message: {
                Text(ChannelBankFormatters.mhz(model.pendingBlockPoint?.freqHz))
            }
        }
    }

    private var connectionPanel: some View {
        Panel("Connection") {
            HStack(spacing: 10) {
                StatusDot(color: connected ? .green : .orange)
                VStack(alignment: .leading, spacing: 4) {
                    Text(model.ble.status.label).font(.headline)
                    Text(model.ble.connectedName ?? (model.ble.broadScanActive ? "Scanning nearby BLE candidates" : "Scan by SDR++ BLE service UUID")).foregroundStyle(.secondary)
                }
                Spacer()
                Button(model.ble.discovered.isEmpty ? "Scan" : "Rescan") { model.ble.startScan() }
                    .buttonStyle(.borderedProminent)
            }
            if !model.ble.discovered.isEmpty {
                VStack(spacing: 8) {
                    ForEach(model.ble.discovered) { peripheral in
                        Button {
                            model.ble.connect(peripheral)
                        } label: {
                            HStack {
                                Text(peripheral.name).fontWeight(.semibold)
                                if peripheral.advertisesChannelBankService {
                                    Text("CB").font(.caption.bold()).foregroundStyle(.green)
                                }
                                Spacer()
                                Text("\(peripheral.rssi) dBm").foregroundStyle(.secondary)
                            }
                        }
                        .buttonStyle(.bordered)
                    }
                }
            }
            if let protocolDocument = model.ble.protocolDocument {
                Text("Protocol: \(protocolDocument.prefix(80))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
            if !model.ble.scanDiagnostics.isEmpty {
                VStack(alignment: .leading, spacing: 3) {
                    ForEach(model.ble.scanDiagnostics, id: \.self) { line in
                        Text(line)
                            .font(.caption2.monospaced())
                            .foregroundStyle(.secondary)
                            .lineLimit(2)
                    }
                }
            }
            if let lastError = model.ble.lastError {
                Text(lastError)
                    .font(.callout)
                    .foregroundStyle(Color(red: 1, green: 0.55, blue: 0.52))
            }
        }
    }

    private var emptyStatePanel: some View {
        Panel("State") {
            Text("Select a discovered SDR++ peripheral to read protocol information, enable state notifications, and request /api/state.")
                .foregroundStyle(.secondary)
        }
    }

    private func radioPanel(_ state: ChannelBankState) -> some View {
        Panel("Radio") {
            MetricGrid(items: [
                ("Source", state.selectedSource ?? "-"),
                ("Radio", state.radioPlaying == true ? "Running" : "Stopped"),
                ("Sample Rate", ChannelBankFormatters.compactHz(state.sampleRate)),
                ("RX888", state.sourceControls?.available == true ? (state.sourceControls?.mode ?? "Available") : "Unavailable")
            ])
            HStack {
                Button("Start Radio") { model.ble.setRadioRunning(true) }
                    .disabled(state.radioPlaying == true)
                Button("Stop Radio") { model.ble.setRadioRunning(false) }
                    .disabled(state.radioPlaying != true)
            }
            .buttonStyle(.bordered)
        }
    }

    private func centerPanel(_ state: ChannelBankState) -> some View {
        Panel("Center") {
            HStack {
                TextField("157.067450", text: $model.centerMHzText)
                    .textFieldStyle(.roundedBorder)
                Button("Tune") { model.tuneCenter() }
                    .buttonStyle(.borderedProminent)
            }
            Text("\(ChannelBankFormatters.mhz(state.centerHz ?? state.waterfallCenterHz)) / span \(ChannelBankFormatters.compactHz(state.usableSpanHz))")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private func channelBankPanel(_ state: ChannelBankState) -> some View {
        Panel("Channel Bank") {
            MetricGrid(items: [
                ("Bank", state.running == true ? "Running" : "Stopped"),
                ("Mode", state.mode ?? "-"),
                ("Demod", state.demodMode ?? "-"),
                ("Active", "\(state.activeChannels?.count ?? 0) / \(state.maxChannels ?? 0)")
            ])
            HStack {
                Button("Start Bank") { model.ble.setChannelBankRunning(true) }
                    .disabled(state.running == true)
                Button("Stop Bank") { model.ble.setChannelBankRunning(false) }
                    .disabled(state.running != true)
            }
            .buttonStyle(.bordered)
        }
    }

    private func activeChannelsPanel(_ state: ChannelBankState) -> some View {
        Panel("Active Channels") {
            if (state.activeChannels ?? []).isEmpty {
                Text("No active channels").foregroundStyle(.secondary)
            } else {
                ForEach(state.activeChannels ?? []) { channel in
                    HStack {
                        StatusDot(color: channel.blocked == true ? .red : channel.signalPresent == true ? .green : .orange)
                        VStack(alignment: .leading) {
                            Text(channel.name ?? ChannelBankFormatters.mhz(channel.gridFreqHz ?? channel.freqHz))
                            Text(ChannelBankFormatters.mhz(channel.gridFreqHz ?? channel.freqHz)).font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        if channel.recording == true { Text("REC").foregroundStyle(.red).font(.caption.bold()) }
                    }
                }
            }
        }
    }

    private func activityHistoryPanel(_ state: ChannelBankState) -> some View {
        Panel("Activity History") {
            let rows = Array((state.history ?? []).prefix(8))
            if rows.isEmpty {
                Text("No history yet").foregroundStyle(.secondary)
            } else {
                ForEach(rows) { row in
                    HStack {
                        Text(ChannelBankFormatters.mhz(row.freqHz)).monospacedDigit()
                        Spacer()
                        Text("\(row.count ?? 0)")
                        if row.blocked == true { Text("Blocked").foregroundStyle(.red) }
                    }
                    .font(.callout)
                }
            }
        }
    }

    private func playbackPanel(_ state: ChannelBankState) -> some View {
        Panel("Playback") {
            MetricGrid(items: [
                ("Playback", state.playback?.active == true ? (state.playback?.name ?? "Active") : "Idle"),
                ("Queued", "\(state.playbackQueued ?? state.playback?.queued ?? 0)"),
                ("Recording", state.recordingEnabled == true ? "Enabled" : "Disabled"),
                ("Lock", state.playbackLock?.active == true ? ChannelBankFormatters.mhz(state.playbackLock?.freqHz) : "Off")
            ])
        }
    }

    private func settingsPanel(_ state: ChannelBankState) -> some View {
        Panel("Settings") {
            MetricGrid(items: [
                ("Spacing", ChannelBankFormatters.compactHz(state.settings?.channelSpacingHz)),
                ("Threshold", String(format: "%.1f dB", state.snrThresholdDb ?? state.settings?.snrThresholdDb ?? 0)),
                ("BW", String(format: "%.0f%%", (state.bwUsage ?? state.settings?.bwUsage ?? 0) * 100)),
                ("Max Ch", "\(state.maxChannels ?? state.settings?.maxChannels ?? 0)")
            ])
        }
    }

    private func diagnosticsPanel(_ state: ChannelBankState) -> some View {
        Panel("Diagnostics") {
            MetricGrid(items: [
                ("Heartbeat", "\(state.sdrppHeartbeat ?? 0)"),
                ("CPU", String(format: "%.1f%%", state.diagnostics?.cpuPercent ?? 0)),
                ("Audio Clients", "\(state.diagnostics?.liveAudioClients ?? 0)"),
                ("Queued Samples", "\(state.diagnostics?.liveAudioQueuedSamples ?? 0)")
            ])
        }
    }

    private var connected: Bool {
        if case .connected = model.ble.status { return true }
        return false
    }
}

private extension View {
    @ViewBuilder
    func iosInlineNavigationTitle() -> some View {
        #if os(iOS)
        self.navigationBarTitleDisplayMode(.inline)
        #else
        self
        #endif
    }
}

struct Panel<Content: View>: View {
    var title: String
    @ViewBuilder var content: Content

    init(_ title: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title).font(.headline)
            content
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(red: 0.07, green: 0.09, blue: 0.11))
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color(red: 0.18, green: 0.22, blue: 0.26)))
    }
}

private struct StatusDot: View {
    var color: Color

    var body: some View {
        Circle().fill(color).frame(width: 10, height: 10)
    }
}

private struct MetricGrid: View {
    var items: [(String, String)]

    var body: some View {
        LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], alignment: .leading, spacing: 8) {
            ForEach(items, id: \.0) { item in
                VStack(alignment: .leading, spacing: 2) {
                    Text(item.0).font(.caption).foregroundStyle(.secondary)
                    Text(item.1).font(.callout.monospacedDigit()).lineLimit(1).minimumScaleFactor(0.72)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }
}
