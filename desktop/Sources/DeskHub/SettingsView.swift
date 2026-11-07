// The settings window (docs/design/app: SettingsDisplays, SettingsInputs, SettingsDevice).
import DeskHubCore
import SwiftUI

enum SettingsPage: String, CaseIterable, Identifiable {
    case displays = "显示器", inputs = "输入源", device = "触摸屏", general = "通用"
    var id: String { rawValue }
}

// @State is a macro in the macOS 27 SDK and the command line tools have no macro plugin: small observable objects instead.
final class PageChoice: ObservableObject { @Published var page: SettingsPage = .displays }
final class RowForm: ObservableObject {
    @Published var name = ""
    @Published var value = ""
    @Published var nameRefused = false
}

struct SettingsView: View {
    @EnvironmentObject var model: AppModel
    @StateObject private var choice = PageChoice()

    var body: some View {
        NavigationSplitView {
            List(SettingsPage.allCases, selection: Binding<SettingsPage?>(get: { choice.page }, set: { p in if let p, p != choice.page { DispatchQueue.main.async { choice.page = p } } })) { p in Text(p.rawValue).tag(p) }
                .navigationSplitViewColumnWidth(160)
        } detail: {
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    switch choice.page {
                    case .displays: DisplaysPage()
                    case .inputs: InputsPage()
                    case .device: DevicePage()
                    case .general: GeneralPage()
                    }
                    if let p = model.saveProblem { Text(p).foregroundStyle(badColor).font(.caption) }
                    if let p = model.configProblem { Text("原来的配置文件坏了（\(p)），已移到旁边保留，现在从空的开始。").foregroundStyle(warnColor).font(.caption) }
                }
                .padding(24)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .frame(minWidth: 720, minHeight: 500)
        .onAppear { DispatchQueue.main.async { model.refresh() } }   // never change published state while a view is drawn
    }
}

private struct Card<Content: View>: View {
    var highlight = false
    @ViewBuilder var content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: 10) { content }
            .padding(14).frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 10).fill(Color.primary.opacity(0.06)))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(highlight ? accent.opacity(0.6) : .clear, lineWidth: 1.5))
    }
}

// ---- displays ----
private struct DisplaysPage: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        Text("显示器").font(.title2.weight(.semibold))
        Text("只切绑定的显示器。绑定按三样东西认它：UUID、序列号、IO 位置（规则 K2）。三样都对上才发命令。")
            .font(.caption).foregroundStyle(.secondary)
        if let b = model.config.binding {
            Card(highlight: true) {
                HStack {
                    Image(systemName: "display").font(.title2).foregroundStyle(accent)
                    VStack(alignment: .leading) {
                        Text(b.name.isEmpty ? "显示器" : b.name).font(.headline)
                        Text(status).font(.caption).foregroundStyle(statusColor)
                    }
                    Spacer()
                    Button("解除绑定") { model.unbind() }.disabled(model.busyID != nil)
                }
                Grid(alignment: .leading, verticalSpacing: 4) {
                    GridRow { Text("UUID").foregroundStyle(.secondary); Text(b.uuid).textSelection(.enabled) }
                    GridRow { Text("序列号").foregroundStyle(.secondary); Text(b.serial.isEmpty ? "（没有）" : b.serial).textSelection(.enabled) }
                    GridRow { Text("IO 位置").foregroundStyle(.secondary); Text(b.location).textSelection(.enabled).lineLimit(2) }
                }
                .font(.system(.caption, design: .monospaced))
            }
        }
        Text(model.config.binding == nil ? "这台 Mac 上的显示器" : "这台 Mac 上的其他显示器").font(.caption).foregroundStyle(.secondary)
        ForEach(model.displays.filter { $0 != model.config.binding }, id: \.self) { d in
            Card {
                HStack {
                    Image(systemName: "display").font(.title3)
                    VStack(alignment: .leading) {
                        Text(d.name.isEmpty ? "显示器" : d.name)
                        Text("没有绑定 · DDC 有回应").font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button(model.config.binding == nil ? "绑定" : "改绑这台") { model.bind(d) }
                        .buttonStyle(.borderedProminent).tint(accent).disabled(model.busyID != nil)
                }
            }
        }
        if model.displays.isEmpty { Text(model.ddcProblem ?? "没有找到回应 DDC 的外接显示器。").font(.caption).foregroundStyle(warnColor) }
        Text("只支持 Apple 芯片的 Mac。内建屏和不回应 DDC 的显示器不列出来。改绑会清掉原来的输入源。").font(.caption).foregroundStyle(.secondary)
    }
    private var status: String {
        switch model.match {
        case .found: return "已绑定 · DDC 有回应"
        case .missing: return "找不到了：没连在这台 Mac 上，或者 DDC 不回应"
        case .changed(let w): return "换了\(w == "location" ? "接口（IO 位置）" : w == "serial" ? "序列号" : "UUID")：不发命令，请重新绑定"
        case .notBound: return ""
        }
    }
    private var statusColor: Color { if case .found = model.match { return okColor }; return warnColor }
}

// ---- inputs ----
private struct InputsPage: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        Text("输入源" + (model.config.binding.map { " · \($0.name)" } ?? "")).font(.title2.weight(.semibold))
        Text("试切成功的输入源才会出现在菜单和触摸屏上。试切：切过去，读回当前输入；\(AppModel.trialSeconds) 秒后自动切回这台 Mac，读回确认切回来了。")
            .font(.caption).foregroundStyle(.secondary)
        if model.config.binding == nil {
            Text("先在“显示器”里绑定一台显示器。").foregroundStyle(warnColor)
        } else {
            ForEach(model.config.inputs) { input in InputRow(input: input) }
            HStack {
                Button("添加输入源") { model.addInput() }.disabled(model.config.inputs.count >= maxInputs)
                Spacer()
                Text("数值是 DDC 的输入编号（VCP 0x60）。DELL 用标准值；LG 等用另一套，选“其他”再填。").font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}

private struct InputRow: View {
    @EnvironmentObject var model: AppModel
    let input: InputSource
    @StateObject private var form = RowForm()

    var body: some View {
        Card(highlight: model.busyID == input.id) {
            HStack(spacing: 10) {
                Picker("", selection: Binding(get: { input.icon }, set: { model.setIcon(input.id, $0) })) {
                    ForEach(iconChoices, id: \.key) { c in Label(c.name, systemImage: symbol(c.key)).tag(c.key) }
                }
                .labelsHidden().frame(width: 110)
                TextField("名字", text: $form.name, onCommit: { form.nameRefused = !model.rename(input.id, to: form.name) })
                    .textFieldStyle(.roundedBorder).frame(width: 150)
                Picker("", selection: Binding(get: { input.port }, set: { _ = model.setPort(input.id, $0, value: $0 == .other ? UInt16(form.value) : nil) })) {
                    ForEach(InputPort.allCases, id: \.self) { p in Text(p.label).tag(p) }
                }
                .labelsHidden().frame(width: 130).disabled(input.isThisMac)
                TextField("数值", text: $form.value, onCommit: { _ = model.setPort(input.id, .other, value: UInt16(form.value)) })
                    .textFieldStyle(.roundedBorder).frame(width: 50).disabled(input.port != .other || input.isThisMac)
                Spacer()
                if input.isThisMac {
                    Text("这台 Mac").font(.caption).foregroundStyle(okColor)
                } else {
                    Button(input.trial == nil ? "试切" : "再试") { model.tryInput(input.id) }.disabled(model.busyID != nil || model.boundDisplay == nil)
                    Button(role: .destructive) { model.remove(input.id) } label: { Image(systemName: "trash") }
                        .buttonStyle(.borderless).disabled(model.busyID != nil).help("删除")
                }
            }
            Text(statusText).font(.caption).foregroundStyle(statusColor)
            if form.nameRefused { Text("名字要 1 到 32 个字节，不能有控制字符。").font(.caption).foregroundStyle(badColor) }
        }
        .onAppear { DispatchQueue.main.async { form.name = input.name; form.value = "\(input.value)" } }
        .onChange(of: input) { new in form.name = new.name; form.value = "\(new.value)" }
    }

    private var statusText: String {
        if let s = model.trialStatus[input.id] { return s }
        if input.isThisMac { return "切回来的路：绑定时读到这台 Mac 在这个输入上" }
        guard let t = input.trial else { return "还没有试切，不在菜单里" }
        if t.reached == .notSent { return "试切没有发出去，不在菜单里" }
        if t.readOther { return "试切时显示器没有切过去（读回的是别的输入，或者是旧版本记录的未确认结果），不在菜单里。可以再试" }
        if !t.returned { return "没有确认切回这台 Mac，不在菜单里" }
        return (t.reached == .confirmed ? "试切过 · 读回确认 · 能切回" : "试切过 · 没有读回确认 · 能切回") + " · 在菜单里"
    }
    private var statusColor: Color {
        if model.trialStatus[input.id] != nil { return accent }
        return input.passedTrial ? okColor : .secondary
    }
}

// ---- the touch screen (until the firmware has Bluetooth) ----
private struct DevicePage: View {
    var body: some View {
        Text("触摸屏").font(.title2.weight(.semibold))
        Text("触摸屏通过蓝牙连这台 Mac。在触摸屏上点输入源，由这个 App 切。").font(.caption).foregroundStyle(.secondary)
        Card {
            HStack {
                Image(systemName: "dot.radiowaves.left.and.right").font(.title2).foregroundStyle(.secondary)
                VStack(alignment: .leading) {
                    Text("还没有配对").font(.headline)
                    Text("触摸屏的蓝牙固件还在做。做好以后在这里配对。").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Button("配对…") {}.disabled(true)
            }
        }
        Text("以后的配对步骤：1. 在触摸屏上打开“设置 › 连接与配对 › 配对电脑”（2 分钟内有效）。2. 点这里的“配对…”，两边各显示 6 位数字。3. 数字一样，30 秒内两边都点确认。")
            .font(.caption).foregroundStyle(.secondary)
    }
}

// ---- general ----
private struct GeneralPage: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        Text("通用").font(.title2.weight(.semibold))
        HStack {
            Toggle("开机时启动 DeskHub", isOn: Binding(get: { model.launchAtLogin ?? false }, set: { model.setLaunchAtLogin($0) }))
                .disabled(model.launchAtLogin == nil)
            if model.launchAtLogin == nil { ProgressView().controlSize(.small) }
        }
        .onAppear { DispatchQueue.main.async { model.readLaunchAtLogin() } }
        if let p = model.launchAtLoginProblem { Text(p).font(.caption).foregroundStyle(badColor) }
        Text("配置文件：\(model.configPath)").font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
        Text("DDC 用的是系统私有接口（IOAVService），只支持 Apple 芯片；macOS 升级后可能失效。").font(.caption).foregroundStyle(.secondary)
    }
}
