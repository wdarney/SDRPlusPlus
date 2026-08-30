#if canImport(ChannelBankCore)
import ChannelBankCore
#endif
import SwiftUI

public struct SNRChartView: View {
    public var state: ChannelBankState

    public init(state: ChannelBankState) {
        self.state = state
    }

    public var body: some View {
        Panel("SNR Overview") {
            let points = state.snrOverview ?? []
            let threshold = state.snrThresholdDb ?? state.settings?.snrThresholdDb ?? 0
            let peak = points.map(\.snrDb).max() ?? 0
            let above = points.filter { $0.snrDb >= threshold }.count
            let detected = points.filter { $0.detected == true }.count
            Text(points.isEmpty ? "No SNR data yet" : "\(above)/\(points.count) above / peak \(peak, specifier: "%.1f") dB / detected \(detected)")
                .font(.caption)
                .foregroundStyle(.secondary)
            Canvas { context, size in
                guard let span = SpanInfo(state: state) else { return }
                let rect = CGRect(origin: .zero, size: size)
                context.fill(Path(rect), with: .color(Color(red: 0.03, green: 0.04, blue: 0.05)))
                let padL = 36.0
                let padB = 18.0
                let plot = CGRect(x: padL, y: 8, width: max(1, size.width - padL - 8), height: max(1, size.height - padB - 16))
                for i in 0...5 {
                    let y = plot.minY + Double(i) / 5 * plot.height
                    var path = Path()
                    path.move(to: CGPoint(x: plot.minX, y: y))
                    path.addLine(to: CGPoint(x: plot.maxX, y: y))
                    context.stroke(path, with: .color(Color(red: 0.10, green: 0.15, blue: 0.19)), lineWidth: 1)
                }
                let maxDb = max(30, threshold + 5, ceil((peak) + 2))
                let minDb = -5.0
                func y(_ db: Double) -> Double {
                    plot.minY + (1 - ((db - minDb) / (maxDb - minDb))) * plot.height
                }
                let baseY = y(0)
                let barWidth = max(1, min(8, plot.width / Double(max(1, points.count)) * 0.82))
                for point in points {
                    guard span.contains(point.freqHz) else { continue }
                    let x = plot.minX + span.x(for: point.freqHz, width: plot.width)
                    let db = max(minDb, min(maxDb, point.snrDb))
                    let top = y(max(0, db))
                    let color: Color = point.blocked == true ? .red : point.detected == true ? .green : point.rawDetected == true ? .orange : db >= threshold ? .blue : Color(red: 0.20, green: 0.30, blue: 0.42)
                    context.fill(Path(CGRect(x: x - barWidth / 2, y: min(baseY, top), width: barWidth, height: max(1, abs(baseY - top)))), with: .color(color))
                }
                var thresholdLine = Path()
                thresholdLine.move(to: CGPoint(x: plot.minX, y: y(threshold)))
                thresholdLine.addLine(to: CGPoint(x: plot.maxX, y: y(threshold)))
                context.stroke(thresholdLine, with: .color(.yellow), style: StrokeStyle(lineWidth: 1, dash: [7, 5]))
            }
            .frame(height: 170)
        }
    }
}

public struct ActivityWaterfallView: View {
    @ObservedObject public var store: ActivityWaterfallStore
    public var onTapFraction: (Double) -> Void

    public init(store: ActivityWaterfallStore, onTapFraction: @escaping (Double) -> Void) {
        self.store = store
        self.onTapFraction = onTapFraction
    }

    public var body: some View {
        Panel("Activity Waterfall") {
            if let span = store.spanInfo {
                Text("\(ChannelBankFormatters.mhz(span.lowHz)) to \(ChannelBankFormatters.mhz(span.highHz))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                GeometryReader { geometry in
                    Canvas { context, size in
                        let rect = CGRect(origin: .zero, size: size)
                        context.fill(Path(rect), with: .color(Color(red: 0.03, green: 0.04, blue: 0.05)))
                        for i in 0...8 {
                            let x = Double(i) / 8 * size.width
                            context.fill(Path(CGRect(x: x, y: 0, width: 1, height: size.height)), with: .color(Color(red: 0.07, green: 0.10, blue: 0.13)))
                        }
                        for marker in store.historyMarkers {
                            let x = span.x(for: marker.freqHz, width: size.width)
                            let alpha = marker.blocked ? 0.42 : min(0.24, marker.strength)
                            context.fill(Path(CGRect(x: x - 1, y: 0, width: 2, height: size.height)), with: .color((marker.blocked ? Color.red : Color.blue).opacity(alpha)))
                        }
                        let rowHeight = size.height / Double(ActivityWaterfallStore.maximumRows)
                        for (index, row) in store.rows.enumerated() {
                            let y = size.height - Double(store.rows.count - index) * rowHeight
                            for point in row.points {
                                let x = span.x(for: point.freqHz, width: size.width)
                                let color = point.blocked ? Color.red : point.live ? Color.green : Color.orange
                                context.fill(Path(CGRect(x: x - 3, y: y, width: 6, height: max(2, rowHeight + 1))), with: .color(color.opacity(point.blocked ? 0.95 : point.strength)))
                            }
                        }
                        if let playback = store.latestSelectablePoints.first(where: { $0.name == "Playback" }) {
                            let x = span.x(for: playback.freqHz, width: size.width)
                            var triangle = Path()
                            triangle.move(to: CGPoint(x: x, y: 8))
                            triangle.addLine(to: CGPoint(x: x - 7, y: 20))
                            triangle.addLine(to: CGPoint(x: x + 7, y: 20))
                            triangle.closeSubpath()
                            context.fill(triangle, with: .color(.cyan))
                        }
                    }
                    .contentShape(Rectangle())
                    .gesture(DragGesture(minimumDistance: 0).onEnded { gesture in
                        onTapFraction(gesture.location.x / max(1, geometry.size.width))
                    })
                }
                .frame(height: 180)
                .clipShape(RoundedRectangle(cornerRadius: 7))
                .overlay(RoundedRectangle(cornerRadius: 7).stroke(Color(red: 0.17, green: 0.20, blue: 0.25)))
            } else {
                Text("Waiting for span data").foregroundStyle(.secondary)
            }
        }
    }
}
