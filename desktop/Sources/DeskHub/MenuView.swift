// The menu (docs/design/app: Main, MenuSwitching, MenuProblems).
import DeskHubCore
import SwiftUI

let accent = Color(red: 0x2f / 255, green: 0xd8 / 255, blue: 0xff / 255)
let okColor = Color(red: 0x59 / 255, green: 0xf0 / 255, blue: 0xa6 / 255)
let warnColor = Color(red: 0xff / 255, green: 0xb5 / 255, blue: 0x47 / 255)
let badColor = Color(red: 0xff / 255, green: 0x7b / 255, blue: 0x7b / 255)

// The icon keys are the device's (SourceIcon); here they are SF Symbols.
func symbol(_ icon: String) -> String {
    switch icon {
    case "desktop": return "macmini"
    case "laptop": return "laptopcomputer"
    case "tv": return "tv"
    case "console": return "gamecontroller"
    case "hdmi", "dp", "usbc": return "cable.connector"
    default: return "display"
    }
}
let iconChoices: [(key: String, name: String)] = [("desktop", "主机"), ("laptop", "笔记本"), ("monitor", "显示器"), ("tv", "电视"),
                                                   ("console", "游戏机"), ("hdmi", "HDMI"), ("dp", "DP"), ("usbc", "USB-C")]

func color(_ o: Outcome) -> Color { o == .confirmed ? okColor : o == .sentUnconfirmed ? warnColor : badColor }

// The touch screen's state in words, for the menu and the settings window.
func linkText(_ s: TouchScreenLink.State) -> (String, Color) {
    switch s {
    case .connected(let name): return ("已连接 · \(name)", okColor)
    case .connecting: return ("正在连接…", .secondary)
    case .searching: return ("正在找触摸屏…", accent)
    case .notPaired: return ("还没有配对", .secondary)
    case .unavailable(let why): return (why, warnColor)
    }
}

struct MenuView: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var link: TouchScreenLink
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Divider().padding(.vertical, 6)
            content
            if let last = model.last {
                Text(lastResultText(name: last.name, result: last.result, at: last.at, now: model.now))
                    .font(.caption).foregroundStyle(color(last.result.outcome))
                    .padding(.horizontal, 14).padding(.top, 6)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Divider().padding(.vertical, 6)
            HStack {
                Image(systemName: "dot.radiowaves.left.and.right").foregroundStyle(.secondary)
                Text("触摸屏")
                Spacer()
                Text(linkText(link.state).0).foregroundStyle(linkText(link.state).1).lineLimit(1)
            }
            .font(.callout).padding(.horizontal, 14)
            Divider().padding(.vertical, 6)
            Button("设置…") { openSettings() }.keyboardShortcut(",")
                .buttonStyle(.plain).padding(.horizontal, 14).padding(.vertical, 3)
            Button("退出 DeskHub") { NSApp.terminate(nil) }.keyboardShortcut("q")
                .buttonStyle(.plain).padding(.horizontal, 14).padding(.vertical, 3)
        }
        .padding(.vertical, 10)
        .frame(width: 320)
        .onAppear { DispatchQueue.main.async { model.now = Date(); model.refresh() } }   // never change published state while a view is drawn
    }

    private var header: some View {
        HStack(spacing: 10) {
            Image(systemName: problem == nil ? "display" : "display.trianglebadge.exclamationmark")
                .font(.title2).foregroundStyle(problem == nil ? accent : warnColor)
            VStack(alignment: .leading, spacing: 2) {
                Text(boundName).font(.headline)
                Text(problem ?? "已绑定 · DDC 可用").font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(.horizontal, 14)
    }

    @ViewBuilder private var content: some View {
        switch model.match {
        case .notBound:
            VStack(alignment: .leading, spacing: 8) {
                Text("还没有绑定显示器").font(.callout.weight(.semibold))
                Text("在设置里选一台显示器，再试切它的输入源。试切成功的才会出现在这里。").font(.caption).foregroundStyle(.secondary)
                Button("打开设置") { openSettings() }.buttonStyle(.borderedProminent).tint(accent)
            }
            .padding(.horizontal, 14)
        default:
            let entries = menuEntries(model.config)
            if entries.isEmpty {
                Text("还没有试切过的输入源。在设置 › 输入源里试切。").font(.caption).foregroundStyle(.secondary).padding(.horizontal, 14)
            }
            VStack(spacing: 2) {
                ForEach(entries) { input in row(input) }
            }
            .padding(.horizontal, 6)
            .disabled(model.boundDisplay == nil)
            .opacity(model.boundDisplay == nil ? 0.4 : 1)
        }
    }

    private func row(_ input: InputSource) -> some View {
        let busy = model.busyID == input.id
        let current = model.isCurrent(input)
        return Button {
            model.switchTo(input.id)
        } label: {
            HStack(spacing: 12) {
                if busy { ProgressView().controlSize(.small).frame(width: 24) }
                else { Image(systemName: symbol(input.icon)).font(.title3).frame(width: 24).foregroundStyle(current ? accent : .primary) }
                VStack(alignment: .leading, spacing: 1) {
                    Text(input.name).font(.callout.weight(.medium))
                    Text(busy ? "正在切换…" : input.port.label + (input.isThisMac ? " · 这台 Mac" : "")).font(.caption)
                        .foregroundStyle(busy ? accent : .secondary)
                }
                Spacer()
                if current && !busy {
                    Text("当前").font(.caption2.weight(.semibold)).foregroundStyle(.black)
                        .padding(.horizontal, 7).padding(.vertical, 2).background(Capsule().fill(accent))
                }
            }
            .padding(.horizontal, 8).frame(height: 44)
            .contentShape(Rectangle())
            .background(RoundedRectangle(cornerRadius: 7).fill(current ? accent.opacity(0.14) : .clear))
        }
        .buttonStyle(.plain)
        .disabled(model.busyID != nil)
        .opacity(model.busyID != nil && !busy ? 0.4 : 1)
    }

    private var boundName: String {
        if case .found(let d) = model.match { return d.name.isEmpty ? "显示器" : d.name }
        return model.config.binding?.name ?? "DeskHub"
    }
    private var problem: String? {
        if let p = model.ddcProblem { return p }
        switch model.match {
        case .missing: return "找不到了：没连在这台 Mac 上，或者 DDC 不回应"
        case .changed(let what): return "换了\(what == "location" ? "接口" : what == "serial" ? "序列号" : "UUID")，请在设置里重新绑定"
        default: return nil
        }
    }

    private func openSettings() { SettingsWindow.open(openWindow) }
}
