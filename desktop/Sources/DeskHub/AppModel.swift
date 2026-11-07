// The state of the App and everything it asks the monitor to do. The rules come from DeskHubCore (K2: binding, white
// list, one switch at a time; K3: results); the DDC module does the talking. Slow work runs off the main thread.
import AppKit
import DDC
import DeskHubCore
import Foundation
import ServiceManagement

struct LastResult: Equatable {
    var name: String
    var result: SwitchResult
    var at: Date
}

@MainActor
final class AppModel: ObservableObject {
    @Published private(set) var config: AppConfig
    @Published private(set) var configProblem: String?
    @Published private(set) var displays: [DisplayIdentity] = []
    @Published private(set) var ddcProblem: String?
    @Published private(set) var match: BindingMatch = .notBound
    @Published private(set) var currentValue: UInt16? {     // the input read back from the bound display; nil when not known
        didSet { link.sendCurrent(currentInputID(config, readBack: currentValue)) }   // the touch screen marks it (A39)
    }
    @Published private(set) var busyID: String?              // the input being switched or tried
    @Published private(set) var trialStatus: [String: String] = [:]
    @Published private(set) var last: LastResult?
    @Published private(set) var saveProblem: String?
    @Published private(set) var launchAtLoginProblem: String?
    @Published var now = Date()

    private var ddc: [DDCDisplay] = []
    private let gate = SwitchGate()
    private let store = ConfigStore()
    private let work = DispatchQueue(label: "deskhub.ddc")   // one queue: one transaction on the bus at a time

    init() {
        let (c, problem) = store.load()
        config = c
        configProblem = problem
        link.sources = { [weak self] in self.map { LinkProtocol.sourceMessages(menuEntries($0.config)) } ?? [] }
        link.onRequest = { [weak self] r in self?.remoteSwitch(r) }
        link.current = { [weak self] in self.map { currentInputID($0.config, readBack: $0.currentValue) } ?? "" }
        refresh()
        NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
        Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.now = Date() }
        }
        // While the touch screen is connected, the monitor's input is read every 10 s (about 0.1 s each), so a switch made
        // with the monitor's own buttons shows up on the touch screen too.
        Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self, case .connected = self.link.state else { return }
                self.readCurrent()
            }
        }
    }

    // ---- displays ----
    func refresh() {
        work.async {
            let found: Result<[DDCDisplay], Error> = Result { try DDCDisplay.list().filter(\.hasDDC) }
            DispatchQueue.main.async {
                // Publish only what changed: every published change redraws the menu and the settings window.
                switch found {
                case .success(let list):
                    self.ddc = list
                    self.setIfChanged(\.displays, list.map(\.identity))
                    self.setIfChanged(\.ddcProblem, nil)
                case .failure(let e):
                    self.ddc = []
                    self.setIfChanged(\.displays, [])
                    self.setIfChanged(\.ddcProblem, "\(e)")
                }
                self.setIfChanged(\.match, matchBinding(self.config.binding, among: self.displays))
                self.readCurrent()
            }
        }
    }

    private func setIfChanged<T: Equatable>(_ key: ReferenceWritableKeyPath<AppModel, T>, _ value: T) {
        if self[keyPath: key] != value { self[keyPath: key] = value }
    }

    var boundDisplay: DDCDisplay? {
        guard case .found(let id) = match else { return nil }
        return ddc.first { $0.identity == id }
    }

    // The mark "current" keeps its last value while a new read runs, so the menu does not blink. A failed read makes it
    // unknown (K3: no mark without a read-back).
    func readCurrent() {
        guard let d = boundDisplay, busyID == nil else { setIfChanged(\.currentValue, nil); return }
        work.async {
            let v = try? readInput(d).get()
            DispatchQueue.main.async { if self.busyID == nil { self.setIfChanged(\.currentValue, v) } }
        }
    }

    func isCurrent(_ input: InputSource) -> Bool {
        currentValue.map { inputMatches(readBack: $0, target: input.value) } ?? false
    }

    // ---- switching (K2: the white list, one at a time; K3: the result as the monitor confirms it) ----
    func switchTo(_ id: String) { _ = startSwitch(id, from: "menu") { _ in } }

    // The one switch path for the menu and the touch screen. Returns the refusal when nothing was sent.
    @discardableResult
    private func startSwitch(_ id: String, from: String, done: @escaping (SwitchResult) -> Void) -> SwitchResult? {
        if busyID != nil || gate.isBusy { return LinkProtocol.busy }
        let allowed = canSwitch(config, match: match, inputID: id)
        if case .failure(let refusal) = allowed { return LinkProtocol.refused(refusal) }
        guard case .success(let input) = allowed, let d = boundDisplay else { return LinkProtocol.refused(.displayMissing) }
        guard gate.begin() else { return LinkProtocol.busy }
        busyID = id
        work.async {
            let r = performSwitch(d, to: input.value)
            SwitchLog.write(["kind": from == "menu" ? "switch" : "touch", "input": input.name, "value": Int(input.value)], r)
            DispatchQueue.main.async {
                self.last = LastResult(name: input.name, result: r, at: Date())
                self.currentValue = r.readBack
                self.busyID = nil
                self.gate.end()
                done(r)
            }
        }
        return nil
    }

    // ---- the touch screen (Bluetooth) ----
    let link = TouchScreenLink()

    // A request from the touch screen: the same white list and the same switch as the menu. Exactly one result goes back.
    private func remoteSwitch(_ r: SwitchRequest) {
        if let refused = startSwitch(r.id, from: "touch", done: { [weak self] result in
            self?.link.sendResult(LinkProtocol.resultMessage(req: r.req, result: result))
        }) {
            SwitchLog.write(["kind": "touch_refused", "input": r.id, "value": 0], refused)
            link.sendResult(LinkProtocol.resultMessage(req: r.req, result: refused))
        }
    }

    // The trial: switch to the input, wait, switch back to this Mac. Only a trial that was sent and came back puts the
    // input in the menu (passedTrial). The way back is this Mac's own input.
    nonisolated static let trialSeconds = 20

    func tryInput(_ id: String) {
        guard busyID == nil, case .success(let plan) = canTrial(config, match: match, inputID: id), let d = boundDisplay, gate.begin() else { return }
        busyID = id
        trialStatus[id] = "正在切过去…"
        work.async {
            let reached = performSwitch(d, to: plan.target.value)
            SwitchLog.write(["kind": "trial_to", "input": plan.target.name, "value": Int(plan.target.value)], reached)
            for left in stride(from: Self.trialSeconds, to: 0, by: -1) {
                DispatchQueue.main.async { self.trialStatus[id] = "\(outcomeText(reached)) · \(left) 秒后切回" }
                Thread.sleep(forTimeInterval: 1)
            }
            DispatchQueue.main.async { self.trialStatus[id] = "正在切回…" }
            let back = performSwitch(d, to: plan.home.value)
            SwitchLog.write(["kind": "trial_back", "input": plan.home.name, "value": Int(plan.home.value)], back)
            DispatchQueue.main.async {
                let t = trialResult(reached: reached, back: back, at: Date())
                self.update { c in if let i = c.inputs.firstIndex(where: { $0.id == id }) { c.inputs[i].trial = t } }
                self.trialStatus[id] = t.returned ? nil : "没有确认切回：\(outcomeText(back))"
                self.currentValue = back.readBack
                self.busyID = nil
                self.gate.end()
            }
        }
    }

    // ---- binding ----
    // Binding reads the input this Mac is on now: that is the way back. The other standard ports are added, not tried.
    func bind(_ identity: DisplayIdentity) {
        guard busyID == nil, let d = ddc.first(where: { $0.identity == identity }) else { return }
        work.async {
            let read = readInput(d)
            DispatchQueue.main.async {
                var c = AppConfig()
                c.launchAtLogin = self.config.launchAtLogin
                c.binding = identity
                var used = Set<UInt16>()
                if case .success(let v) = read {
                    let value = v & 0xFF
                    let port = InputPort.allCases.first { $0.standardValue == value } ?? .other
                    let name = String((Host.current().localizedName ?? "这台 Mac").prefix(maxNameBytes / 3))
                    c.inputs.append(InputSource(id: UUID().uuidString, name: name, port: port, value: value, icon: "desktop", isThisMac: true,
                                                trial: Trial(at: Date(), reached: .confirmed, returned: true)))
                    used.insert(value)
                } else {
                    self.ddcProblem = "读不到这台显示器当前的输入，所以不知道切回来的路。可以先绑定，再在输入源里手动标出这台 Mac。"
                }
                for port in InputPort.allCases {
                    guard let v = port.standardValue, !used.contains(v) else { continue }
                    c.inputs.append(InputSource(id: UUID().uuidString, name: port.label, port: port, value: v, icon: "monitor", isThisMac: false, trial: nil))
                }
                self.replace(c)
                self.refresh()
            }
        }
    }

    func unbind() {
        var c = AppConfig()
        c.launchAtLogin = config.launchAtLogin
        replace(c)
        refresh()
    }

    // ---- editing inputs ----
    func rename(_ id: String, to name: String) -> Bool {
        let n = name.trimmingCharacters(in: .whitespaces)
        guard !n.isEmpty, n.utf8.count <= maxNameBytes, !n.unicodeScalars.contains(where: { $0.value < 0x20 }) else { return false }
        update { c in if let i = c.inputs.firstIndex(where: { $0.id == id }) { c.inputs[i].name = n } }
        return true
    }
    func setIcon(_ id: String, _ icon: String) {
        update { c in if let i = c.inputs.firstIndex(where: { $0.id == id }) { c.inputs[i].icon = icon } }
    }
    // A new port or value is a new input: the old trial no longer says anything about it.
    func setPort(_ id: String, _ port: InputPort, value: UInt16?) -> Bool {
        guard let v = value ?? port.standardValue, (1...255).contains(v),
              !config.inputs.contains(where: { $0.id != id && $0.value == v }) else { return false }
        update { c in
            if let i = c.inputs.firstIndex(where: { $0.id == id }) {
                c.inputs[i].port = port
                c.inputs[i].value = v
                if !c.inputs[i].isThisMac { c.inputs[i].trial = nil }
            }
        }
        return true
    }
    func addInput() {
        guard config.inputs.count < maxInputs else { return }
        let used = Set(config.inputs.map(\.value))
        let port = InputPort.allCases.first { $0.standardValue.map { !used.contains($0) } ?? false } ?? .other
        guard let v = port.standardValue ?? (1...255).map({ UInt16($0) }).first(where: { !used.contains($0) }) else { return }
        update { $0.inputs.append(InputSource(id: UUID().uuidString, name: port.label, port: port, value: v, icon: "monitor", isThisMac: false, trial: nil)) }
    }
    func remove(_ id: String) {
        update { $0.inputs.removeAll { $0.id == id && !$0.isThisMac } }
    }

    // ---- launch at login ----
    // SMAppService can take seconds for an ad-hoc signed App (it looks up signatures): never on the main thread, never
    // from a view's body. The status is read once and kept; nil while it is being read.
    @Published private(set) var launchAtLogin: Bool?
    private let system = DispatchQueue(label: "deskhub.system")

    func readLaunchAtLogin() {
        system.async {
            let on = SMAppService.mainApp.status == .enabled
            DispatchQueue.main.async { self.launchAtLogin = on }
        }
    }
    func setLaunchAtLogin(_ on: Bool) {
        launchAtLogin = nil
        system.async {
            var problem: String?
            do {
                if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
            } catch {
                problem = "没有设置成功：\(error.localizedDescription)"
            }
            let now = SMAppService.mainApp.status == .enabled
            DispatchQueue.main.async {
                self.launchAtLoginProblem = problem
                self.launchAtLogin = now
                if problem == nil { self.update { $0.launchAtLogin = now } }
            }
        }
    }

    var configPath: String { store.file.path }

    // ---- saving: a change that cannot be saved is not kept, and the App says so ----
    private func update(_ edit: (inout AppConfig) -> Void) {
        var c = config
        edit(&c)
        replace(c)
    }
    private func replace(_ c: AppConfig) {
        do {
            try store.save(c)
            let menuBefore = menuEntries(config)
            config = c
            saveProblem = nil
            match = matchBinding(c.binding, among: displays)
            if menuEntries(c) != menuBefore { link.resendSources() }   // the touch screen shows what the menu shows
        } catch {
            saveProblem = "没有保存：\(error.localizedDescription)"
        }
    }
}
