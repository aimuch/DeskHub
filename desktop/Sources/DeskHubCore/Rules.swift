// The rules of the App (docs/desktop-app-design.md 13; rules K2 and K3), with no AppKit and no IOKit, so core-check can
// test every one of them.
import Foundation

// ---- ports ----
public enum InputPort: String, Codable, CaseIterable, Sendable {
    case dp1, dp2, hdmi1, hdmi2, usbc, other

    // MCCS values of VCP 0x60. LG and some others use another set: those are entered as "other" with their value.
    public var standardValue: UInt16? {
        switch self {
        case .dp1: return 15
        case .dp2: return 16
        case .hdmi1: return 17
        case .hdmi2: return 18
        case .usbc: return 27
        case .other: return nil
        }
    }
    public var label: String {
        switch self {
        case .dp1: return "DisplayPort 1"
        case .dp2: return "DisplayPort 2"
        case .hdmi1: return "HDMI 1"
        case .hdmi2: return "HDMI 2"
        case .usbc: return "USB-C"
        case .other: return "其他"
        }
    }
}

// ---- results (K3) ----
public enum Outcome: String, Codable, Sendable {
    case confirmed         // a read-back matched the target
    case sentUnconfirmed   // sent; no read-back, or a different one
    case notSent           // the write failed
}

public struct SwitchResult: Equatable, Sendable {
    public var outcome: Outcome
    public var detail: String      // why, in words; "" when confirmed
    public var readBack: UInt16?   // the value that decided it, when there was one
    public init(outcome: Outcome, detail: String, readBack: UInt16?) {
        self.outcome = outcome
        self.detail = detail
        self.readBack = readBack
    }
}

// The read-back after a switch: the monitor needs time to change its input, so it is read twice.
public let readBackDelays: [Double] = [1.5, 4.0]

// Dell answers 15 as 0x0F0F: for a target that fits in a byte, the low byte decides.
public func inputMatches(readBack: UInt16, target: UInt16) -> Bool {
    target <= 0xFF ? (readBack & 0xFF) == target : readBack == target
}

// Why a read-back failed, in words ("timeout" works as a literal).
public struct ReadError: Error, Equatable, Sendable, ExpressibleByStringLiteral {
    public var why: String
    public init(_ why: String) { self.why = why }
    public init(stringLiteral value: String) { self.why = value }
}

public func classify(sendError: String?, readBacks: [Result<UInt16, ReadError>], target: UInt16) -> SwitchResult {
    if let sendError { return SwitchResult(outcome: .notSent, detail: sendError, readBack: nil) }
    var lastValue: UInt16?
    for r in readBacks {
        if case .success(let v) = r {
            if inputMatches(readBack: v, target: target) { return SwitchResult(outcome: .confirmed, detail: "", readBack: v) }
            lastValue = v
        }
    }
    if let v = lastValue { return SwitchResult(outcome: .sentUnconfirmed, detail: "读回的是 \(Int(v & 0xFF))", readBack: v) }
    return SwitchResult(outcome: .sentUnconfirmed, detail: "读不回", readBack: nil)
}

// ---- configuration ----
public struct Trial: Codable, Equatable, Sendable {
    public var at: Date
    public var reached: Outcome    // how the switch to the input went
    public var returned: Bool      // the switch back to this Mac was confirmed by a read-back
    public var readOther: Bool     // after the switch the monitor read back another input: it did not go there
    public init(at: Date, reached: Outcome, returned: Bool, readOther: Bool = false) {
        self.at = at
        self.reached = reached
        self.returned = returned
        self.readOther = readOther
    }
    enum CodingKeys: String, CodingKey { case at, reached, returned, readOther }
    // 0.1.0 stored no readOther. Its unconfirmed trials cannot say whether the monitor went there: they count as
    // contradicted, so the input has to be tried again. Its confirmed trials stay good.
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        at = try c.decode(Date.self, forKey: .at)
        reached = try c.decode(Outcome.self, forKey: .reached)
        returned = try c.decode(Bool.self, forKey: .returned)
        readOther = try c.decodeIfPresent(Bool.self, forKey: .readOther) ?? (reached == .sentUnconfirmed)
    }
}

public struct InputSource: Codable, Equatable, Identifiable, Sendable {
    public var id: String
    public var name: String
    public var port: InputPort
    public var value: UInt16
    public var icon: String        // a SourceIcon key of the device: hdmi, dp, usbc, laptop, desktop, monitor, console, tv
    public var isThisMac: Bool     // the input this Mac is on: the way back
    public var trial: Trial?
    public init(id: String, name: String, port: InputPort, value: UInt16, icon: String, isThisMac: Bool, trial: Trial?) {
        self.id = id
        self.name = name
        self.port = port
        self.value = value
        self.icon = icon
        self.isThisMac = isThisMac
        self.trial = trial
    }
    // In the menu (and the white list of the touch screen) only after a trial that was sent, that the monitor did not
    // contradict (another input read back), and that came back.
    public var passedTrial: Bool { trial.map { $0.reached != .notSent && !$0.readOther && $0.returned } ?? false }
}

public struct AppConfig: Codable, Equatable, Sendable {
    public var version = 1
    public var binding: DisplayIdentity?
    public var inputs: [InputSource] = []
    public var launchAtLogin = false
    public init() {}
}

public let maxInputs = 12, maxNameBytes = 32

private func goodText(_ s: String, max: Int) -> Bool {
    !s.isEmpty && s.utf8.count <= max && !s.unicodeScalars.contains { $0.value < 0x20 || $0.value == 0x7F }
}

// Every problem of a configuration; empty when it is good.
public func validate(_ c: AppConfig) -> [String] {
    var p: [String] = []
    if c.version != 1 { p.append("version \(c.version)") }
    if let b = c.binding {
        if b.uuid.isEmpty { p.append("binding without UUID") }
        if b.location.isEmpty { p.append("binding without location") }
    }
    if c.inputs.count > maxInputs { p.append("more than \(maxInputs) inputs") }
    if Set(c.inputs.map(\.id)).count != c.inputs.count { p.append("an id twice") }
    if Set(c.inputs.map(\.value)).count != c.inputs.count { p.append("a value twice") }
    if c.inputs.filter(\.isThisMac).count > 1 { p.append("two inputs of this Mac") }
    for i in c.inputs {
        if !goodText(i.id, max: 64) { p.append("a bad id") }
        if !goodText(i.name, max: maxNameBytes) { p.append("a bad name: \(i.id)") }
        if i.value < 1 || i.value > 0xFF { p.append("value \(i.value) of \(i.id)") }
    }
    return p
}

public func encodeConfig(_ c: AppConfig, validate check: Bool = true) throws -> Data {
    if check, let problem = validate(c).first { throw NSError(domain: "DeskHub", code: 1, userInfo: [NSLocalizedDescriptionKey: problem]) }
    let e = JSONEncoder()
    e.outputFormatting = [.prettyPrinted, .sortedKeys]
    e.dateEncodingStrategy = .iso8601
    return try e.encode(c)
}

// A missing file is the empty configuration. A damaged one is the empty configuration too, with the reason: never half.
public func loadConfig(_ data: Data) -> (AppConfig, String?) {
    if data.isEmpty { return (AppConfig(), nil) }
    let d = JSONDecoder()
    d.dateDecodingStrategy = .iso8601
    guard let c = try? d.decode(AppConfig.self, from: data) else { return (AppConfig(), "not a configuration file") }
    if let problem = validate(c).first { return (AppConfig(), problem) }
    return (c, nil)
}

// ---- binding (K2) ----
public enum BindingMatch: Equatable, Sendable {
    case notBound
    case found(DisplayIdentity)
    case missing
    case changed(String)   // "uuid", "serial" or "location": the display is there, but not as it was bound
}

public func matchBinding(_ binding: DisplayIdentity?, among displays: [DisplayIdentity]) -> BindingMatch {
    guard let b = binding else { return .notBound }
    if let d = displays.first(where: { $0.uuid == b.uuid && $0.serial == b.serial && $0.location == b.location }) { return .found(d) }
    if let d = displays.first(where: { $0.uuid == b.uuid }) { return .changed(d.serial != b.serial ? "serial" : "location") }
    if displays.contains(where: { $0.serial == b.serial && $0.location == b.location && !b.serial.isEmpty }) { return .changed("uuid") }
    return .missing
}

// ---- the white list (K2) ----
public enum Refusal: Error, Equatable {
    case notBound, displayMissing, bindingChanged(String), unknownInput, notTried, isThisMac, noWayBack
}

public func menuEntries(_ c: AppConfig) -> [InputSource] { c.inputs.filter(\.passedTrial) }

private func ready(_ match: BindingMatch) -> Refusal? {
    switch match {
    case .found: return nil
    case .notBound: return .notBound
    case .missing: return .displayMissing
    case .changed(let what): return .bindingChanged(what)
    }
}

public func canSwitch(_ c: AppConfig, match: BindingMatch, inputID: String) -> Result<InputSource, Refusal> {
    if let r = ready(match) { return .failure(r) }
    guard let input = c.inputs.first(where: { $0.id == inputID }) else { return .failure(.unknownInput) }
    guard input.passedTrial else { return .failure(.notTried) }
    return .success(input)
}

public struct TrialPlan: Equatable, Sendable {
    public var target: InputSource
    public var home: InputSource
}

public func canTrial(_ c: AppConfig, match: BindingMatch, inputID: String) -> Result<TrialPlan, Refusal> {
    if let r = ready(match) { return .failure(r) }
    guard let input = c.inputs.first(where: { $0.id == inputID }) else { return .failure(.unknownInput) }
    if input.isThisMac { return .failure(.isThisMac) }
    guard let home = c.inputs.first(where: \.isThisMac) else { return .failure(.noWayBack) }
    return .success(TrialPlan(target: input, home: home))
}

public func trialResult(reached: SwitchResult, back: SwitchResult, at: Date) -> Trial {
    Trial(at: at, reached: reached.outcome, returned: back.outcome == .confirmed,
          readOther: reached.outcome == .sentUnconfirmed && reached.readBack != nil)
}

// The input the monitor is on, for the touch screen: only an input of the menu, only from a read-back (K3).
public func currentInputID(_ c: AppConfig, readBack: UInt16?) -> String {
    guard let v = readBack else { return "" }
    return menuEntries(c).first { inputMatches(readBack: v, target: $0.value) }?.id ?? ""
}

// ---- one switch at a time (K2) ----
public final class SwitchGate: @unchecked Sendable {
    private let lock = NSLock()
    private var busy = false
    public init() {}
    public func begin() -> Bool {
        lock.lock(); defer { lock.unlock() }
        if busy { return false }
        busy = true
        return true
    }
    public func end() {
        lock.lock(); busy = false; lock.unlock()
    }
    public var isBusy: Bool { lock.lock(); defer { lock.unlock() }; return busy }
}

// ---- words ----
public func outcomeText(_ r: SwitchResult) -> String {
    switch r.outcome {
    case .confirmed: return "已确认"
    case .sentUnconfirmed: return r.detail.isEmpty ? "已发送（未确认）" : "已发送（未确认）：\(r.detail)"
    case .notSent: return r.detail.isEmpty ? "没有发出" : "没有发出：\(r.detail)"
    }
}

public func lastResultText(name: String, result: SwitchResult, at: Date, now: Date) -> String {
    let s = Int(now.timeIntervalSince(at))
    let ago = s < 60 ? "刚才" : s < 3600 ? "\(s / 60) 分钟前" : s < 86400 ? "\(s / 3600) 小时前" : "\(s / 86400) 天前"
    return "上次：切到 \(name) · \(outcomeText(result)) · \(ago)"
}
