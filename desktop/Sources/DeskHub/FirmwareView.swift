// The firmware page of the settings window (docs/design/app SettingsFirmware, SettingsFirmwareFlashing;
// docs/desktop-app-design.md 13.10): USB flashing through Flasher, the same code as deskhub-flash.
import AppKit
import DeskHubCore
import Flasher
import SwiftUI

// Asked by the App delegate before it quits: a write in progress is never killed (W2).
enum FlashGuard {
    private static let lock = NSLock()
    private static var _busy = false
    static var busy: Bool {
        get { lock.lock(); defer { lock.unlock() }; return _busy }
        set { lock.lock(); _busy = newValue; lock.unlock() }
    }
}

@MainActor
final class FirmwareModel: ObservableObject {
    enum Phase: Equatable {
        case idle, checking, probing, writing(Double), restarting
        case done(String), failed(String)
    }
    @Published private(set) var device: DeviceIdentity?
    @Published private(set) var deviceNote = "还没读"
    @Published private(set) var reading = false
    @Published private(set) var package: FirmwarePackage?
    @Published private(set) var packageName = ""
    @Published private(set) var packageProblem: String?
    @Published private(set) var ports: [String] = []
    @Published var port = ""
    @Published private(set) var phase: Phase = .idle
    @Published private(set) var key: UpdateKey?
    @Published private(set) var keyNote: String?
    private var folder: PackageFolder?

    // ---- the Bluetooth update (docs/desktop-app-design.md 13.11) ----
    func loadKey() { if key == nil { key = UpdateKey.load() } }
    // K1: the public key goes to the device over USB only.
    func writeKey() {
        guard !port.isEmpty, !busy else { return }
        do { key = try UpdateKey.loadOrCreate() } catch { keyNote = "这台 Mac 建不了钥匙（需要 Apple 芯片的安全隔区）。"; return }
        guard let key else { return }
        keyNote = "正在用 USB 写入公钥…"
        let port = self.port, pem = key.publicKeyPEM, mine = key.fingerprint
        Task.detached {
            let r = USBStatus.writeUpdateKey(port: port, pem: pem)
            await MainActor.run {
                switch r {
                case .success(let fp): self.keyNote = fp == mine ? "设备收下了这台 Mac 的公钥。重新连上蓝牙后就能用蓝牙升级。" : "设备存的指纹和这台 Mac 的不一样。"
                case .failure(let e): self.keyNote = e.words
                }
            }
        }
    }
    func keyOnDevice(_ link: TouchScreenLink) -> Bool {
        guard let key, let fp = link.info?.updateKey, !fp.isEmpty else { return false }
        return fp == key.fingerprint
    }
    func bluetoothBlocker(_ link: TouchScreenLink) -> String? {
        guard case .connected = link.state else { return "触摸屏没用蓝牙连上。" }
        guard link.hasUpdateCharacteristic else { return "触摸屏的固件还不支持蓝牙升级：先用 USB 刷 0.3.22 以后的版本。" }
        guard key != nil else { return "这台 Mac 还没有签名钥匙：点“写入公钥（USB）”。" }
        guard keyOnDevice(link) else { return "触摸屏上没有这台 Mac 的公钥：插上 USB，点“写入公钥（USB）”。" }
        guard let package else { return "先选一个固件包。" }
        if let running = link.info?.firmware, let c = FirmwareVersion.compare(package.version, running), c <= 0 {
            return "固件包 \(package.version) 不比触摸屏上的 \(running) 新。蓝牙只能升级；降级请用 USB。"
        }
        return nil
    }
    func bluetoothUpdate(_ link: TouchScreenLink) {
        guard bluetoothBlocker(link) == nil, let key, let package, let folder,
              let app = package.images.first(where: { $0.offset == 0x20000 }) else { return }
        // The bytes that are signed and sent are checked again now (P7): the folder could have changed since it was chosen.
        guard case .success(let again) = folder.verify(), again == package, let image = try? Data(contentsOf: folder.url(of: app)),
              FirmwareCheck.sha256HexForTests(image) == app.sha256 else { keyNote = PackageProblem.digestMismatch.words; return }
        do { link.startUpdate(envelope: try key.envelope(for: package), image: image, version: package.version) }
        catch { keyNote = "签名失败：\(error)" }
    }

    // The test hook (docs/desktop-app-design.md 13.11): `DeskHub --ble-update <package> [--log <file>]` runs the App's own
    // Bluetooth update once the touch screen is connected, and writes what happened. Nothing else changes.
    private var autoTimer: Timer?
    private var autoLog: [[String: Any]] = []
    private var autoStart = Date()
    func autoRun(_ link: TouchScreenLink) {
        let args = CommandLine.arguments
        guard let i = args.firstIndex(of: "--ble-update"), i + 1 < args.count else { return }
        let path = args[i + 1]
        let logPath = args.firstIndex(of: "--log").flatMap { $0 + 1 < args.count ? args[$0 + 1] : nil }
        load(URL(fileURLWithPath: path))
        loadKey()
        autoStart = Date()
        func note(_ s: String) {
            autoLog.append(["t": (Date().timeIntervalSince(autoStart) * 10).rounded() / 10, "line": s])
            if let logPath, let d = try? JSONSerialization.data(withJSONObject: ["tool": "DeskHub --ble-update", "package": URL(fileURLWithPath: path).lastPathComponent, "events": autoLog], options: [.prettyPrinted, .sortedKeys]) {
                try? d.write(to: URL(fileURLWithPath: logPath))
            }
        }
        link.onUpdateLog = { note($0) }
        note("waiting for the touch screen")
        var started = false
        autoTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] t in
            Task { @MainActor in
                guard let self else { t.invalidate(); return }
                if !started {
                    if let why = self.bluetoothBlocker(link) {
                        if case .connected = link.state { note("blocked: \(why)"); t.invalidate() }
                        return
                    }
                    started = true
                    note("info before: \(link.info.map { "\($0.device) \($0.firmware) key \($0.updateKey.prefix(12))" } ?? "none")")
                    self.bluetoothUpdate(link)
                } else {
                    switch link.update {
                    case .confirmed, .failed: note("done: \(link.update)"); t.invalidate()
                    default: break
                    }
                }
            }
        }
    }

    var busy: Bool {
        switch phase {
        case .checking, .probing, .writing, .restarting: return true
        default: return false
        }
    }

    func refreshPorts() {
        ports = USBStatus.ports()
        if !ports.contains(port) { port = ports.count == 1 ? ports[0] : "" }   // one device: that one; more: the person picks
    }

    func readDevice() {
        guard !port.isEmpty, !reading, !busy else { return }
        reading = true
        deviceNote = "正在读…"
        let port = self.port
        Task.detached {
            let r = USBStatus.identify(port: port)
            await MainActor.run {
                self.reading = false
                switch r {
                case .success(let id): self.device = id; self.deviceNote = "\(id.deviceID) · 从 USB 读到"
                case .failure(let e):
                    self.device = nil
                    self.deviceNote = e == .noReadBack ? "没回答：固件没在运行，或者在下载模式（照样能刷）" : e.words
                }
            }
        }
    }

    func choosePackage() {
        let panel = NSOpenPanel()
        panel.title = "选择固件包"
        panel.message = "GitHub 出的 deskhub-<版本>-esp32s3.zip，或解压后的文件夹"
        panel.canChooseDirectories = true
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [.zip, .folder]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        load(url)
    }

    func load(_ url: URL) {
        folder?.remove()
        folder = nil
        package = nil
        packageName = url.lastPathComponent
        packageProblem = nil
        do {
            let f = try PackageFolder.open(url)
            switch f.verify() {
            case .success(let p): folder = f; package = p
            case .failure(let e): f.remove(); packageProblem = e.words
            }
        } catch {
            packageProblem = FlashError.notAPackage.words
        }
        if case .done = phase { phase = .idle }
        if case .failed = phase { phase = .idle }
    }

    var olderThanDevice: Bool {
        guard let package, let device, let c = FirmwareVersion.compare(package.version, device.version) else { return false }
        return c < 0
    }

    func flash() {
        guard let folder, let package, !port.isEmpty, !busy else { return }
        if olderThanDevice {   // P9
            let a = NSAlert()
            a.messageText = "固件包 \(package.version) 比设备上的 \(device?.version ?? "") 旧"
            a.informativeText = "旧固件可能读不懂新版本存的设置，外观等会回到默认。还要刷吗？"
            a.addButton(withTitle: "刷入旧版本")
            a.addButton(withTitle: "取消")
            guard a.runModal() == .alertFirstButtonReturn else { return }
        }
        guard let tool = Esptool.find() else { phase = .failed(FlashError.tool.words); return }
        phase = .checking
        FlashGuard.busy = true
        let run = FlashRun(package: folder, port: port, tool: tool)
        Task.detached {
            let result = run.run { e in
                Task { @MainActor in self.apply(e) }
            }
            await MainActor.run {
                FlashGuard.busy = false
                switch result {
                case .success(let id):
                    self.device = id
                    self.deviceNote = "\(id.deviceID) · 刷完从 USB 读到"
                    self.phase = .done("已刷入 \(id.version)（读回确认）。设备编号 \(id.deviceID)。")
                case .failure(let e): self.phase = .failed(e.words)
                }
            }
        }
    }

    private func apply(_ e: FlashEvent) {
        guard busy else { return }
        switch e {
        case .checking, .packageOK: phase = .checking
        case .before(let id): if let id { device = id; deviceNote = "\(id.deviceID) · 刷之前从 USB 读到" }
        case .probing: phase = .probing
        case .writing(let f): phase = .writing(f)
        case .restarting: phase = .restarting
        case .confirmed, .log: break
        }
    }
}

struct FirmwarePage: View {
    @EnvironmentObject var fw: FirmwareModel
    @EnvironmentObject var link: TouchScreenLink
    var body: some View {
        Text("固件").font(.title2.weight(.semibold))
        Text("用 USB 线给触摸屏刷固件。设置会保留。刷完从 USB 读回版本，一样才算刷好。").font(.caption).foregroundStyle(.secondary)
        row(title: fw.device.map { "设备上：\($0.version)" } ?? "设备上：不知道", sub: fw.deviceNote, subColor: .secondary) {
            Button(fw.reading ? "正在读…" : "再读一次") { fw.readDevice() }.disabled(fw.port.isEmpty || fw.reading || fw.busy)
        }
        row(title: fw.package.map { "固件包：\($0.version)" } ?? (fw.packageProblem == nil ? "固件包：还没选" : "固件包：不能用"),
            sub: fw.packageProblem ?? (fw.package.map { "\(fw.packageName) · \($0.images.count) 个镜像的 SHA-256 都对" } ?? "GitHub 出的 deskhub-<版本>-esp32s3.zip"),
            subColor: fw.packageProblem != nil ? badColor : fw.package != nil ? okColor : .secondary) {
            Button("选择…") { fw.choosePackage() }.disabled(fw.busy)
        }
        HStack {
            Text("串口").font(.headline)
            Spacer()
            if fw.ports.isEmpty {
                Text("没找到设备，请用能传数据的线连上").font(.caption).foregroundStyle(warnColor)
            } else {
                Picker("", selection: $fw.port) {
                    Text("选一个").tag("")
                    ForEach(fw.ports, id: \.self) { Text($0).tag($0) }
                }
                .labelsHidden().frame(width: 260).disabled(fw.busy)
            }
            Button { fw.refreshPorts() } label: { Image(systemName: "arrow.clockwise") }.buttonStyle(.borderless).disabled(fw.busy)
        }
        .padding(14).background(RoundedRectangle(cornerRadius: 10).fill(Color.primary.opacity(0.06)))
        progress
        HStack(alignment: .top) {
            Text("刷的时候不要拔线，大约 1 分钟。刷坏了也能救：按住 BOOT 再插线，再刷一次。").font(.caption).foregroundStyle(.secondary)
            Spacer()
            Button(fw.package.map { "刷入 \($0.version)" } ?? "刷入") { fw.flash() }
                .buttonStyle(.borderedProminent).tint(accent)
                .disabled(fw.package == nil || fw.port.isEmpty || fw.busy)
        }
        Text("刷机工具是 Espressif 的 esptool \(FirmwareRules.esptoolVersion)（GPL-2.0，单独的程序，许可证在 App 里）。").font(.caption2).foregroundStyle(.tertiary)
        bluetooth
    }

    // 13.11: the update over Bluetooth, signed by this Mac's key in the Secure Enclave.
    @ViewBuilder private var bluetooth: some View {
        Divider().padding(.vertical, 4)
        Text("蓝牙升级").font(.title3.weight(.semibold))
        Text("不用插线：固件由这台 Mac 安全隔区里的钥匙签名，触摸屏核对签名后才写入，新版本起不来会自动回到旧版本。只能升级，降级请用上面的 USB。").font(.caption).foregroundStyle(.secondary)
        row(title: fw.key.map { "签名钥匙：\($0.fingerprint.prefix(12))…" } ?? "签名钥匙：还没有",
            sub: fw.keyNote ?? (fw.keyOnDevice(link) ? "触摸屏上是这台 Mac 的公钥" : "触摸屏上还没有这台 Mac 的公钥：插上 USB，写入一次"),
            subColor: fw.keyOnDevice(link) ? okColor : .secondary) {
            Button("写入公钥（USB）") { fw.writeKey() }.disabled(fw.port.isEmpty || fw.busy)
        }
        bluetoothProgress
        HStack(alignment: .top) {
            Text(fw.bluetoothBlocker(link) ?? "大约几分钟。升级时触摸屏要插着 USB 电源。").font(.caption).foregroundStyle(fw.bluetoothBlocker(link) == nil ? Color.secondary : warnColor)
            Spacer()
            if case .sending = link.update { Button("取消") { link.cancelUpdate() } }
            Button(fw.package.map { "通过蓝牙升级到 \($0.version)" } ?? "通过蓝牙升级") { fw.bluetoothUpdate(link) }
                .buttonStyle(.borderedProminent).tint(accent)
                .disabled(fw.bluetoothBlocker(link) != nil || { switch link.update { case .manifest, .waitingReady, .sending, .finishing, .restarting: return true; default: return false } }())
        }
        .onAppear { fw.loadKey() }
    }
    @ViewBuilder private var bluetoothProgress: some View {
        switch link.update {
        case .idle: EmptyView()
        case .manifest, .waitingReady: status("正在把签名清单发给触摸屏…", nil)
        case .sending(let f): status("正在通过蓝牙写入", f)
        case .finishing: status("触摸屏正在核对整个镜像…", nil)
        case .restarting(let v): status("触摸屏正在重启到 \(v)，等它重新连上后读回版本…", nil)
        case .confirmed(let v): note("已升级到 \(v)（读回确认）。新版本开机 1 分钟内自检，不过会自动回到旧版本。", okColor, "checkmark.circle.fill")
        case .failed(let s): note(s, warnColor, "exclamationmark.triangle.fill")
        }
    }

    @ViewBuilder private var progress: some View {
        switch fw.phase {
        case .idle: EmptyView()
        case .checking: status("正在核对固件包和刷机工具…", nil)
        case .probing: status("正在识别芯片（只读）…", nil)
        case .writing(let f): status("正在写入", f)
        case .restarting: status("写入和校验通过，等设备重启后读回版本…", nil)
        case .done(let s): note(s, okColor, "checkmark.circle.fill")
        case .failed(let s): note(s, warnColor, "exclamationmark.triangle.fill")
        }
    }

    private func status(_ text: String, _ fraction: Double?) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(text).font(.headline)
                Spacer()
                if let fraction { Text("\(Int(fraction * 100))%").foregroundStyle(accent).monospacedDigit() } else { ProgressView().controlSize(.small) }
            }
            if let fraction { ProgressView(value: fraction).tint(accent) }
        }
        .padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10).fill(Color.primary.opacity(0.06)))
    }

    private func note(_ text: String, _ color: Color, _ icon: String) -> some View {
        HStack(alignment: .top) {
            Image(systemName: icon).foregroundStyle(color)
            Text(text).font(.callout).foregroundStyle(color).textSelection(.enabled)
        }
        .padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10).fill(Color.primary.opacity(0.06)))
    }

    private func row<B: View>(title: String, sub: String, subColor: Color, @ViewBuilder button: () -> B) -> some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.headline)
                Text(sub).font(.caption).foregroundStyle(subColor).textSelection(.enabled)
            }
            Spacer()
            button()
        }
        .padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10).fill(Color.primary.opacity(0.06)))
    }
}
