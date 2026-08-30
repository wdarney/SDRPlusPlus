#if canImport(ChannelBankCore)
import ChannelBankCore
#endif
import SwiftUI

public struct ChannelBankRootView: View {
    @StateObject private var model = ChannelBankViewModel()
    @State private var confirmClearWavs = false

    public init() {}

    public var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 14) {
                    connectionPanel
                    if let state = model.ble.latestState {
                        radioPanel(state)
                        sourcePanel(state)
                        sdrppServerPanel(state)
                        sourceOffsetPanel(state)
                        sourceControlsPanel(state)
                        centerPanel(state)
                        channelBankPanel(state)
                        SNRChartView(state: state)
                        ActivityWaterfallView(store: model.waterfall) { fraction in
                            model.selectWaterfallFrequency(at: fraction)
                        }
                        activeChannelsPanel(state)
                        activityHistoryPanel(state)
                        playbackPanel(state)
                        recordingsPanel
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
            .confirmationDialog("Clear WAV recordings?", isPresented: $confirmClearWavs, titleVisibility: .visible) {
                Button("Clear WAVs", role: .destructive) { model.ble.clearRecordedWavs() }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("M4A files and active or queued playback files are preserved.")
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
                ScrollView(.vertical) {
                    LazyVStack(alignment: .leading, spacing: 3) {
                        ForEach(model.ble.scanDiagnostics, id: \.self) { line in
                            Text(line)
                                .font(.caption2.monospaced())
                                .foregroundStyle(.secondary)
                                .lineLimit(2)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                    .padding(.vertical, 2)
                }
                .frame(maxHeight: 170)
                .scrollIndicators(.visible)
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
            Text(connected ? "Waiting for the first complete State snapshot." : "Select a discovered SDR++ peripheral to connect.")
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

    private func sourcePanel(_ state: ChannelBankState) -> some View {
        Panel("Source") {
            MetricGrid(items: [
                ("Selected", state.selectedSource ?? "-"),
                ("Count", "\(state.sources?.count ?? 0)")
            ])
            if let sources = state.sources, !sources.isEmpty {
                Menu {
                    ForEach(sources, id: \.self) { source in
                        Button(source) { model.selectSource(source) }
                    }
                } label: {
                    Label("Select Source", systemImage: "antenna.radiowaves.left.and.right")
                }
                .buttonStyle(.bordered)
                .disabled(state.radioPlaying == true)
            }
        }
    }

    private func sdrppServerPanel(_ state: ChannelBankState) -> some View {
        let server = state.sdrppServer
        return Panel("SDR++ Server") {
            MetricGrid(items: [
                ("Available", server?.available == true ? "Yes" : "No"),
                ("Connected", server?.connected == true ? "Yes" : "No"),
                ("Running", server?.sourceRunning == true ? "Yes" : "No"),
                ("Target", "\(server?.host ?? "-"):\(server?.port.map(String.init) ?? "-")")
            ])
            HStack {
                TextField("Host", text: $model.serverHostText)
                    .textFieldStyle(.roundedBorder)
                TextField("Port", text: $model.serverPortText)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 88)
                Button("Save") { model.saveServerTarget() }
                    .buttonStyle(.bordered)
            }
            HStack {
                Button("Connect") { model.ble.setSDRPPServerConnected(true) }
                    .disabled(server?.available != true || server?.connected == true || state.radioPlaying == true)
                Button("Disconnect") { model.ble.setSDRPPServerConnected(false) }
                    .disabled(server?.available != true || server?.connected != true || state.radioPlaying == true)
            }
            .buttonStyle(.bordered)
        }
    }

    private func sourceOffsetPanel(_ state: ChannelBankState) -> some View {
        let offset = state.sourceOffset
        return Panel("Source Offset") {
            MetricGrid(items: [
                ("Selected", offset?.selected ?? "-"),
                ("Effective", ChannelBankFormatters.compactHz(offset?.effectiveOffsetHz)),
                ("Manual", ChannelBankFormatters.compactHz(offset?.manualOffsetHz)),
                ("Modes", "\(offset?.modes?.count ?? 0)")
            ])
            HStack {
                TextField("Manual Hz", text: $model.manualOffsetHzText)
                    .textFieldStyle(.roundedBorder)
                Menu {
                    ForEach(offset?.modes ?? [], id: \.self) { mode in
                        Button(mode) { model.setSourceOffset(mode) }
                    }
                } label: {
                    Label("Set Mode", systemImage: "arrow.left.arrow.right")
                }
                .buttonStyle(.bordered)
            }
        }
    }

    private func sourceControlsPanel(_ state: ChannelBankState) -> some View {
        Panel("Source Controls") {
            if let controls = state.sourceControls, controls.available == true {
                MetricGrid(items: [
                    ("Source", controls.source ?? state.selectedSource ?? "-"),
                    ("Mode", controls.mode ?? "-"),
                    ("Device", controls.deviceId.map(String.init) ?? "-"),
                    ("Telemetry", controls.telemetryIntervalSec.map { "\($0)s" } ?? "-")
                ])
                sourceControlMenus(controls, running: state.radioPlaying == true || controls.running == true)
                sourceControlLiveToggles(controls)
                sourceControlGains(controls)
            } else {
                Text(state.selectedSource == "SDR++ Server" ? "Use the SDR++ Server panel above." : "No source controls exposed for this source.")
                    .foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder
    private func sourceControlMenus(_ controls: RX888SourceControls, running: Bool) -> some View {
        HStack {
            if let devices = controls.devices, !devices.isEmpty {
                Menu {
                    ForEach(devices) { device in
                        Button(device.label) { model.setSourceControl("deviceId", int: device.id) }
                    }
                } label: {
                    Label("Device", systemImage: "externaldrive")
                }
                .disabled(running)
            }
            if let rates = controls.sampleRates, !rates.isEmpty {
                Menu {
                    ForEach(rates) { rate in
                        Button(rate.label ?? ChannelBankFormatters.compactHz(rate.value)) {
                            model.setSourceControl("sampleRateId", int: rate.id)
                        }
                    }
                } label: {
                    Label("Rate", systemImage: "speedometer")
                }
                .disabled(running)
            }
            if let modes = controls.modes, !modes.isEmpty {
                Menu {
                    ForEach(modes, id: \.self) { mode in
                        Button(mode) { model.setSourceControl("mode", string: mode) }
                    }
                } label: {
                    Label("Mode", systemImage: "waveform")
                }
                .disabled(running)
            }
            Button {
                model.ble.setSourceControls(["refresh": JSONValue(.bool(true))])
            } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
            }
            .disabled(running)
        }
        .buttonStyle(.bordered)

        if controls.supportsAdcFreq == true {
            HStack {
                Text("ADC \(String(format: "%.0f", controls.adcClockMHz ?? 0)) MHz")
                Spacer()
                Button("-") {
                    let next = max(controls.adcMinMHz ?? 16, (controls.adcClockMHz ?? 0) - 1)
                    model.setSourceControl("adcClockMHz", number: next)
                }
                .disabled(running)
                Button("+") {
                    let next = min(controls.adcMaxMHz ?? 140, (controls.adcClockMHz ?? 0) + 1)
                    model.setSourceControl("adcClockMHz", number: next)
                }
                .disabled(running)
            }
            .buttonStyle(.bordered)
        }
    }

    @ViewBuilder
    private func sourceControlLiveToggles(_ controls: RX888SourceControls) -> some View {
        let toggles = controls.toggles ?? []
        if controls.supportsNewBiasTee == true || controls.supportsBiasTee == true || controls.supportsDithering == true || !toggles.isEmpty {
            VStack(spacing: 8) {
                if controls.supportsNewBiasTee == true || controls.supportsBiasTee == true {
                    ToggleRow(label: "HF Bias Tee", value: controls.biasTeeHF == true) {
                        model.setSourceControl("biasTeeHF", bool: !(controls.biasTeeHF == true))
                    }
                    ToggleRow(label: "VHF Bias Tee", value: controls.biasTeeVHF == true) {
                        model.setSourceControl("biasTeeVHF", bool: !(controls.biasTeeVHF == true))
                    }
                }
                if controls.supportsDithering == true {
                    ToggleRow(label: "Dithering", value: controls.dithering == true) {
                        model.setSourceControl("dithering", bool: !(controls.dithering == true))
                    }
                }
                ForEach(toggles) { toggle in
                    ToggleRow(label: toggle.label ?? toggle.key, value: toggle.value == true) {
                        model.setSourceToggle(toggle.key, value: !(toggle.value == true))
                    }
                    .disabled(toggle.available == false)
                }
                if let speeds = controls.telemetrySpeeds, !speeds.isEmpty {
                    Menu {
                        ForEach(speeds) { speed in
                            if let interval = speed.intervalSec {
                                Button(speed.label ?? "\(interval)s") {
                                    model.setSourceControl("telemetryIntervalSec", int: interval)
                                }
                            }
                        }
                    } label: {
                        Label("Telemetry", systemImage: "gauge.with.dots.needle.33percent")
                    }
                    .buttonStyle(.bordered)
                    .disabled(controls.telemetryLiveMutable == false)
                }
            }
        }
    }

    @ViewBuilder
    private func sourceControlGains(_ controls: RX888SourceControls) -> some View {
        let gains = (controls.gains ?? []).filter { $0.available != false }
        if !gains.isEmpty {
            VStack(spacing: 8) {
                ForEach(gains) { gain in
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(gain.label ?? "\(gain.name) Gain")
                            Text(String(format: "%.1f dB", gain.value ?? 0))
                                .font(.caption.monospacedDigit())
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button("-") {
                            let step = gain.step ?? 0.5
                            let next = max(gain.min ?? -120, (gain.value ?? 0) - step)
                            model.setSourceGain(gain.name, value: next)
                        }
                        Button("+") {
                            let step = gain.step ?? 0.5
                            let next = min(gain.max ?? 120, (gain.value ?? 0) + step)
                            model.setSourceGain(gain.name, value: next)
                        }
                    }
                    .buttonStyle(.bordered)
                    .disabled(gain.liveMutable == false && controls.running == true)
                }
            }
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
        let activeCount = state.activeChannels?.count ?? state.activeChannelCount ?? 0
        return Panel("Channel Bank") {
            MetricGrid(items: [
                ("Bank", state.running == true ? "Running" : "Stopped"),
                ("Mode", state.mode ?? "-"),
                ("Demod", state.demodMode ?? "-"),
                ("Active", "\(activeCount) / \(state.maxChannels ?? 0)")
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
                if let activeChannelCount = state.activeChannelCount, activeChannelCount > 0 {
                    Text("\(activeChannelCount) active channels").foregroundStyle(.secondary)
                } else {
                    Text("No active channels").foregroundStyle(.secondary)
                }
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
            Text("Monitor: \(model.ble.audioMonitorStatus)")
                .font(.caption)
                .foregroundStyle(.secondary)
            HStack {
                if let hz = state.playback?.freqHz {
                    Button("Lock Current") { model.ble.setPlaybackLock(hz: hz) }
                }
                Button("Clear Lock") { model.ble.clearPlaybackLock() }
                    .disabled(state.playbackLock?.active != true)
            }
            .buttonStyle(.bordered)
        }
    }

    private var recordingsPanel: some View {
        Panel("Recordings") {
            let recordings = model.ble.recordings
            MetricGrid(items: [
                ("Available", recordings?.available == true ? "Yes" : "No"),
                ("Session", recordings?.activeSession ?? "default"),
                ("Files", "\(recordings?.files?.count ?? 0)"),
                ("Root", recordings?.root?.isEmpty == false ? "Set" : "-")
            ])
            HStack {
                TextField("Session name", text: $model.recordingSessionText)
                    .textFieldStyle(.roundedBorder)
                Button("Start") { model.setRecordingSession() }
                    .buttonStyle(.bordered)
            }
            HStack {
                Button("Refresh") { model.ble.refreshRecordings() }
                Button("Clear WAVs", role: .destructive) { confirmClearWavs = true }
            }
            .buttonStyle(.bordered)
            if let files = recordings?.files, !files.isEmpty {
                ForEach(files.prefix(6)) { file in
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(file.name ?? file.path).lineLimit(1)
                            Text("\(file.session?.isEmpty == false ? file.session! : "default") - \(ChannelBankFormatters.bytes(file.size))")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        Text(file.modifiedMs.map(ChannelBankFormatters.millisTime) ?? "-")
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                    .font(.callout)
                }
            }
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
            let settings = state.settings
            HStack {
                Menu {
                    ForEach(["auto", "manual", "scan", "bookmark_scan"], id: \.self) { mode in
                        Button(mode) { model.setSetting("mode", string: mode) }
                    }
                } label: {
                    Label("Mode", systemImage: "switch.2")
                }
                .disabled(state.running == true)
                Menu {
                    ForEach([0, 1, 2, 3, 4, 5], id: \.self) { spacingID in
                        Button(ChannelBankFormatters.spacingPreset(spacingID)) { model.setSetting("spacingId", int: spacingID) }
                    }
                } label: {
                    Label("Spacing", systemImage: "ruler")
                }
                .disabled(state.running == true)
                Menu {
                    ForEach(["AM", "NFM", "WFM", "USB", "LSB"], id: \.self) { mode in
                        Button(mode) { model.setSetting("demodMode", string: mode) }
                    }
                } label: {
                    Label("Demod", systemImage: "dot.radiowaves.left.and.right")
                }
                .disabled(state.running == true)
            }
            .buttonStyle(.bordered)

            StepperRow(label: "SNR", value: String(format: "%.1f dB", settings?.snrThresholdDb ?? state.snrThresholdDb ?? 0)) {
                let next = max(1, (settings?.snrThresholdDb ?? state.snrThresholdDb ?? 1) - 0.5)
                model.setSetting("snrThresholdDb", number: next)
            } increment: {
                let next = min(30, (settings?.snrThresholdDb ?? state.snrThresholdDb ?? 1) + 0.5)
                model.setSetting("snrThresholdDb", number: next)
            }
            StepperRow(label: "Max Channels", value: "\(settings?.maxChannels ?? state.maxChannels ?? 0)") {
                let next = max(1, (settings?.maxChannels ?? state.maxChannels ?? 1) - 1)
                model.setSetting("maxChannels", int: next)
            } increment: {
                let next = min(64, (settings?.maxChannels ?? state.maxChannels ?? 1) + 1)
                model.setSetting("maxChannels", int: next)
            }
            StepperRow(label: "Bandwidth", value: String(format: "%.0f%%", (settings?.bwUsage ?? state.bwUsage ?? 0.5) * 100)) {
                let next = max(0.5, (settings?.bwUsage ?? state.bwUsage ?? 0.5) - 0.05)
                model.setSetting("bwUsage", number: next)
            } increment: {
                let next = min(1.0, (settings?.bwUsage ?? state.bwUsage ?? 0.5) + 0.05)
                model.setSetting("bwUsage", number: next)
            }
            ToggleRow(label: "Recording", value: settings?.recordingEnabled ?? state.recordingEnabled ?? false) {
                model.setSetting("recordingEnabled", bool: !(settings?.recordingEnabled ?? state.recordingEnabled ?? false))
            }
            timingSettings(settings)
            if let backend = settings?.transcriptionBackendName {
                HStack {
                    Text("Transcription")
                    Spacer()
                    Text(backend).foregroundStyle(.secondary)
                    Button("Off") { model.setSetting("transcriptionBackend", int: 0) }
                }
                .buttonStyle(.bordered)
            }
        }
    }

    @ViewBuilder
    private func timingSettings(_ settings: ChannelBankSettings?) -> some View {
        VStack(spacing: 8) {
            StepperRow(label: "Min TX", value: "\(settings?.minTransmissionMs ?? 0) ms") {
                model.setSetting("minTransmissionMs", int: max(0, (settings?.minTransmissionMs ?? 0) - 100))
            } increment: {
                model.setSetting("minTransmissionMs", int: min(10_000, (settings?.minTransmissionMs ?? 0) + 100))
            }
            StepperRow(label: "Hold", value: "\(settings?.signalHoldMs ?? 0) ms") {
                model.setSetting("signalHoldMs", int: max(0, (settings?.signalHoldMs ?? 0) - 100))
            } increment: {
                model.setSetting("signalHoldMs", int: min(5_000, (settings?.signalHoldMs ?? 0) + 100))
            }
            StepperRow(label: "Tail", value: "\(settings?.tailMs ?? 0) ms") {
                model.setSetting("tailMs", int: max(100, (settings?.tailMs ?? 100) - 100))
            } increment: {
                model.setSetting("tailMs", int: min(2_000, (settings?.tailMs ?? 100) + 100))
            }
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

private struct ToggleRow: View {
    var label: String
    var value: Bool
    var action: () -> Void

    var body: some View {
        HStack {
            Text(label)
            Spacer()
            Button(value ? "On" : "Off", action: action)
                .buttonStyle(.borderedProminent)
                .tint(value ? .green : .gray)
        }
        .font(.callout)
    }
}

private struct StepperRow: View {
    var label: String
    var value: String
    var decrement: () -> Void
    var increment: () -> Void

    var body: some View {
        HStack {
            Text(label)
            Spacer()
            Text(value)
                .font(.callout.monospacedDigit())
                .foregroundStyle(.secondary)
            Button("-", action: decrement)
            Button("+", action: increment)
        }
        .buttonStyle(.bordered)
        .font(.callout)
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
