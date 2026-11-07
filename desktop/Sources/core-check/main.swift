// The checks of DeskHubCore: the rules of the App, without a monitor. Failure modes written BEFORE the code
// (docs/desktop-app-design.md 13, rules K2 and K3). `swift run core-check` exits 1 when one fails.
//
// F1  A reply of the monitor is taken only when it checks out: checksum, opcode, VCP code, result. The DDC null message is
//     "not ready", never a value. A short reply is refused.
// F2  The input that was read back matches the target on the low byte when the target fits in a byte (Dell reports 15 as
//     0x0F0F); a larger target must match exactly. 0x0F10 is not 15.
// F3  K3: "confirmed" only when a read-back matches the target. A failed send is "not sent", with its reason. A send
//     with no read-back, or with a read-back that differs, is "sent, not confirmed" and says what was read. A later
//     matching read-back wins over an earlier wrong one; an error after a match does not undo it.
// F4  Binding (K2): a display is found only when the system UUID, the serial and the IO location all match. The same UUID
//     with another serial or location is "changed" and names the field; nothing matching is "missing"; no binding is
//     "not bound". Names never count.
// F5  White list (K2): only an input that passed its trial can be switched from the menu (and later from the touch screen).
//     A trial needs this Mac's input (the way back) and a target that is not it. Nothing can be switched when the binding
//     is not found. An unknown id is refused.
// F6  One switch at a time (K2): a second begin while one runs is refused; after end, begin works again. No queue.
// F7  The configuration round-trips. A damaged file (not JSON, unknown version, more than 12 inputs, ids twice, values
//     twice, a value outside 1...255, a bad name, two "this Mac" inputs, a binding without UUID or location) is read as
//     the empty configuration and says why: never half a list.
// F8  The menu lists the inputs that passed their trial, in their order. An input whose switch was "not sent", or that
//     did not come back, is not listed. Neither is one where the monitor read back ANOTHER input after the switch (it
//     stayed where it was: nothing on that port, 2026-11-07 DP2). Only a trial with no read-back at all may pass
//     unconfirmed (a monitor that cannot report its input). A trial stored by 0.1.0 without that field reads as before.
// F9  The last result reads in words: the input, the outcome, how long ago. A time in the future is "just now".
// F10 The standard values of the ports: DP1 15, DP2 16, HDMI1 17, HDMI2 18, USB-C 27; "other" has none.
// F11 The touch screen link (docs/desktop-app-design.md 13.3), the App's side: the source list goes out as begin, one
//     message per input, end; only inputs that passed their trial, at most 12, in menu order, each message at most 512
//     bytes, valid JSON with exactly the fields the firmware wants (ble_protocol.cpp), the port as its key.
// F12 A switch request from the touch screen is parsed strictly: op "switch", a whole req in 1...4294967295, an id of 1 to
//     48 bytes without control characters, nothing else. Anything else is ignored (never "half a request").
// F13 A request is answered with exactly one result for its req. A request the App refuses (not in the white list, the
//     display missing, a switch already running) is answered "not_sent" with the reason, and nothing is sent to the monitor.
//     The outcome words are those the firmware knows: confirmed, sent_unconfirmed, not_sent. A long detail is cut on a
//     character boundary to 120 bytes.
// F14 The current input for the touch screen: the menu input that matches the read-back (low byte, as F2), else "" (not
//     known: no read-back, or a value no listed input has). Only inputs in the menu can be current.
import DeskHubCore
import Foundation

var failures = 0
func check(_ ok: @autoclosure () -> Bool, _ what: String, line: Int = #line) {
    if !ok() { failures += 1; print("FAIL line \(line): \(what)") }
}

// ---- F1 ----
func reply(cur: UInt16, max: UInt16 = 14, vcp: UInt8 = 0x60, result: UInt8 = 0, opcode: UInt8 = 0x02) -> [UInt8] {
    var b: [UInt8] = [0x6E, 0x88, opcode, result, vcp, 0x00, UInt8(max >> 8), UInt8(max & 0xFF), UInt8(cur >> 8), UInt8(cur & 0xFF), 0]
    b[10] = b[0..<10].reduce(UInt8(0x50)) { $0 ^ $1 }
    return b
}
do {
    if case .success(let v) = DDCReply.parseVCPFields(reply(cur: 0x0F0F), vcp: 0x60) { check(v.current == 0x0F0F && v.maximum == 14, "a good reply") } else { check(false, "a good reply is taken") }
    // the real one from the U2720QM (2026-11-07)
    if case .success(let v) = DDCReply.parseVCPFields([0x6e, 0x88, 0x02, 0x00, 0x60, 0x00, 0x00, 0x0e, 0x0f, 0x0f, 0x0f ^ 0x0f ^ 0x0e ^ 0x60 ^ 0x02 ^ 0x88 ^ 0x6e ^ 0x50], vcp: 0x60) {
        check(v.current == 0x0F0F, "the U2720QM reply")
    } else { check(false, "the U2720QM reply is taken") }
    var bad = reply(cur: 15); bad[10] ^= 1
    check(DDCReply.parseVCPFields(bad, vcp: 0x60) == .failure(.checksum(expected: bad[10] ^ 1, got: bad[10])), "a wrong checksum is refused")
    check(DDCReply.parseVCPFields(reply(cur: 15, opcode: 0x03), vcp: 0x60) == .failure(.opcode(0x03)), "a wrong opcode is refused")
    check(DDCReply.parseVCPFields(reply(cur: 15, vcp: 0x10), vcp: 0x60) == .failure(.code(expected: 0x60, got: 0x10)), "another VCP code is refused")
    check(DDCReply.parseVCPFields(reply(cur: 15, result: 1), vcp: 0x60) == .failure(.unsupported), "unsupported")
    check(DDCReply.parseVCPFields(reply(cur: 15, result: 2), vcp: 0x60) == .failure(.result(2)), "another result code")
    check(DDCReply.parseVCPFields([0x6e, 0x80, 0xbe, 0, 0x60, 0, 0, 0x0e, 0x0f, 0x0f, 0xda], vcp: 0x60) == .failure(.notReady), "the null message is not ready")
    check(DDCReply.parseVCPFields([0x6e, 0x88, 0x02], vcp: 0x60) == .failure(.short(3)), "a short reply")
}

// ---- F2 ----
check(inputMatches(readBack: 0x0F0F, target: 15), "Dell's 0x0F0F is 15")
check(inputMatches(readBack: 15, target: 15), "15 is 15")
check(!inputMatches(readBack: 0x0F11, target: 15), "0x0F11 is not 15")
check(!inputMatches(readBack: 17, target: 15), "17 is not 15")
check(inputMatches(readBack: 0x0110, target: 0x0110) && !inputMatches(readBack: 0x0210, target: 0x0110), "a two-byte target matches exactly")

// ---- F3 ----
check(classify(sendError: "write failed", readBacks: [], target: 17).outcome == .notSent, "a failed send is not sent")
check(classify(sendError: "write failed", readBacks: [], target: 17).detail.contains("write failed"), "with its reason")
check(classify(sendError: "write failed", readBacks: [.success(17)], target: 17).outcome == .notSent, "a read-back does not turn a failed send into a success")
check(classify(sendError: nil, readBacks: [.success(0x1111)], target: 17).outcome == .confirmed, "a matching read-back confirms")
check(classify(sendError: nil, readBacks: [.success(0x0F0F)], target: 17) == SwitchResult(outcome: .sentUnconfirmed, detail: "读回的是 15", readBack: 0x0F0F), "a different read-back says what was read")
check(classify(sendError: nil, readBacks: [], target: 17).outcome == .sentUnconfirmed, "no read-back is not confirmed")
check(classify(sendError: nil, readBacks: [.failure("timeout"), .failure("timeout")], target: 17) == SwitchResult(outcome: .sentUnconfirmed, detail: "读不回", readBack: nil), "only failed reads")
check(classify(sendError: nil, readBacks: [.success(15), .success(17)], target: 17).outcome == .confirmed, "a later match wins")
check(classify(sendError: nil, readBacks: [.success(17), .failure("timeout")], target: 17).outcome == .confirmed, "an error after a match does not undo it")
check(classify(sendError: nil, readBacks: [.success(17), .success(15)], target: 17).outcome == .confirmed, "once confirmed, it stays")
check(readBackDelays == [1.5, 4.0], "read back after 1.5 s and 4 s")

// ---- F4 ----
let u2720 = DisplayIdentity(uuid: "307859BB-F28C-4B3D-BD1F-E14B288CAA50", serial: "892744780",
                            location: "IOService:/AppleARMPE/arm-io@10F00000/AppleH16GFamilyIO/dispext1@88000000/IOMobileFramebufferShim", name: "DELL U2720QM")
let u2719 = DisplayIdentity(uuid: "EF5F99D1-3826-40D2-BDC6-FC69A13A6873", serial: "1093817687",
                            location: "IOService:/AppleARMPE/arm-io@10F00000/AppleH16GFamilyIO/dispext0@84000000/IOMobileFramebufferShim", name: "DELL U2719DS")
check(matchBinding(nil, among: [u2720]) == .notBound, "no binding")
check(matchBinding(u2720, among: [u2719, u2720]) == .found(u2720), "all three match")
var renamed = u2720; renamed.name = "Something else"
check(matchBinding(u2720, among: [renamed]) == .found(renamed), "the name does not count")
var moved = u2720; moved.location = u2719.location
check(matchBinding(u2720, among: [moved]) == .changed("location"), "another port is a change of location")
var reserial = u2720; reserial.serial = "1"
check(matchBinding(u2720, among: [reserial]) == .changed("serial"), "another serial")
var reuuid = u2720; reuuid.uuid = "00000000-0000-0000-0000-000000000000"
check(matchBinding(u2720, among: [reuuid]) == .changed("uuid"), "the same serial and place with another UUID")
check(matchBinding(u2720, among: [u2719]) == .missing && matchBinding(u2720, among: []) == .missing, "missing")

// ---- F5, F8 ----
func trial(_ reached: Outcome, returned: Bool) -> Trial { Trial(at: Date(timeIntervalSince1970: 1_800_000_000), reached: reached, returned: returned) }
var config = AppConfig()
config.binding = u2720
config.inputs = [
    InputSource(id: "mini", name: "Mac mini", port: .dp1, value: 15, icon: "desktop", isThisMac: true, trial: trial(.confirmed, returned: true)),
    InputSource(id: "m1", name: "MacBook M1", port: .hdmi1, value: 17, icon: "laptop", isThisMac: false, trial: trial(.confirmed, returned: true)),
    InputSource(id: "m4", name: "MacBook M4", port: .usbc, value: 27, icon: "laptop", isThisMac: false, trial: trial(.sentUnconfirmed, returned: true)),
    InputSource(id: "dp2", name: "DP 2", port: .dp2, value: 16, icon: "monitor", isThisMac: false, trial: nil),
    InputSource(id: "h2", name: "HDMI 2", port: .hdmi2, value: 18, icon: "monitor", isThisMac: false, trial: trial(.notSent, returned: true)),
    InputSource(id: "lost", name: "Lost", port: .other, value: 99, icon: "monitor", isThisMac: false, trial: trial(.confirmed, returned: false)),
]
check(menuEntries(config).map(\.id) == ["mini", "m1", "m4"], "the menu: trial passed, in order")
let found = BindingMatch.found(u2720)
check((try? canSwitch(config, match: found, inputID: "m1").get())?.value == 17, "a listed input can be switched")
check(canSwitch(config, match: found, inputID: "dp2") == .failure(.notTried), "an input without a trial cannot")
check(canSwitch(config, match: found, inputID: "h2") == .failure(.notTried), "a trial that was not sent does not count")
check(canSwitch(config, match: found, inputID: "lost") == .failure(.notTried), "an input that did not come back does not count")
check(canSwitch(config, match: found, inputID: "nope") == .failure(.unknownInput), "an unknown id")
check(canSwitch(config, match: .missing, inputID: "m1") == .failure(.displayMissing), "nothing while the display is missing")
check(canSwitch(config, match: .changed("location"), inputID: "m1") == .failure(.bindingChanged("location")), "nothing while the binding changed")
check(canSwitch(config, match: .notBound, inputID: "m1") == .failure(.notBound), "nothing without a binding")
if case .success(let plan) = canTrial(config, match: found, inputID: "dp2") { check(plan.target.value == 16 && plan.home.value == 15, "a trial goes to the target and back to this Mac") } else { check(false, "a trial of a new input is allowed") }
check(canTrial(config, match: found, inputID: "mini") == .failure(.isThisMac), "this Mac's own input is not tried by switching")
var noHome = config; noHome.inputs.removeAll { $0.isThisMac }
check(canTrial(noHome, match: found, inputID: "dp2") == .failure(.noWayBack), "no trial without the way back")
check(canTrial(config, match: .missing, inputID: "dp2") == .failure(.displayMissing), "no trial while the display is missing")
check(trialResult(reached: SwitchResult(outcome: .confirmed, detail: "", readBack: 16), back: SwitchResult(outcome: .confirmed, detail: "", readBack: 15), at: Date()).returned, "back confirmed is returned")
do {   // F8: the monitor stayed on this Mac's input
    let stayed = trialResult(reached: SwitchResult(outcome: .sentUnconfirmed, detail: "读回的是 15", readBack: 0x0F0F), back: SwitchResult(outcome: .confirmed, detail: "", readBack: 0x0F0F), at: Date())
    check(stayed.readOther, "a different read-back is remembered")
    var c = config
    c.inputs[3].trial = stayed
    check(!c.inputs[3].passedTrial && !menuEntries(c).contains { $0.id == "dp2" }, "an input the monitor did not go to is not in the menu")
    check(canSwitch(c, match: found, inputID: "dp2") == .failure(.notTried), "and cannot be switched")
    let blind = trialResult(reached: SwitchResult(outcome: .sentUnconfirmed, detail: "读不回", readBack: nil), back: SwitchResult(outcome: .confirmed, detail: "", readBack: 15), at: Date())
    check(!blind.readOther, "no read-back is not a different read-back")
    c.inputs[3].trial = blind
    check(c.inputs[3].passedTrial, "a trial with no read-back may pass unconfirmed")
    // what 0.1.0 stored has no readOther: an unconfirmed trial of then cannot say whether the monitor went there, so it
    // must be tried again; a confirmed one stays good
    let d = JSONDecoder(); d.dateDecodingStrategy = .iso8601
    let oldUnconfirmed = try? d.decode(Trial.self, from: Data(#"{"at":"2026-11-07T02:11:30Z","reached":"sentUnconfirmed","returned":true}"#.utf8))
    check(oldUnconfirmed?.readOther == true, "a 0.1.0 unconfirmed trial counts as contradicted: try again")
    let oldConfirmed = try? d.decode(Trial.self, from: Data(#"{"at":"2026-11-07T02:10:56Z","reached":"confirmed","returned":true}"#.utf8))
    check(oldConfirmed?.readOther == false, "a 0.1.0 confirmed trial stays good")
}
check(!trialResult(reached: SwitchResult(outcome: .confirmed, detail: "", readBack: 16), back: SwitchResult(outcome: .sentUnconfirmed, detail: "读不回", readBack: nil), at: Date()).returned, "back not confirmed is not returned")

// ---- F6 ----
let gate = SwitchGate()
check(gate.begin(), "the first begin")
check(!gate.begin(), "a second begin is refused")
gate.end()
check(gate.begin(), "begin again after end")
gate.end()

// ---- F7 ----
do {
    let data = try encodeConfig(config)
    let (back, problem) = loadConfig(data)
    check(problem == nil && back == config, "the configuration round-trips")
    check(loadConfig(Data()).0 == AppConfig() && loadConfig(Data()).1 == nil, "no file is the empty configuration, no problem")
    func damaged(_ edit: (inout AppConfig) -> Void, _ what: String) {
        var c = config
        edit(&c)
        let r = loadConfig((try? encodeConfig(c, validate: false)) ?? Data())
        check(r.0 == AppConfig() && r.1 != nil, "damaged: \(what)")
    }
    check(loadConfig(Data("not json".utf8)).0 == AppConfig() && loadConfig(Data("not json".utf8)).1 != nil, "damaged: not JSON")
    damaged({ $0.version = 2 }, "unknown version")
    damaged({ c in for i in 0..<7 { c.inputs.append(InputSource(id: "x\(i)", name: "X\(i)", port: .other, value: UInt16(100 + i), icon: "monitor", isThisMac: false, trial: nil)) } }, "13 inputs")
    damaged({ $0.inputs[1].id = "mini" }, "an id twice")
    damaged({ $0.inputs[1].value = 15 }, "a value twice")
    damaged({ $0.inputs[1].value = 0 }, "value 0")
    damaged({ $0.inputs[1].value = 256 }, "value 256")
    damaged({ $0.inputs[1].name = "" }, "an empty name")
    damaged({ $0.inputs[1].name = String(repeating: "a", count: 33) }, "a long name")
    damaged({ $0.inputs[1].name = "tab\there" }, "a name with a control character")
    damaged({ $0.inputs[1].isThisMac = true }, "two this-Mac inputs")
    damaged({ $0.binding?.uuid = "" }, "a binding without UUID")
    damaged({ $0.binding?.location = "" }, "a binding without location")
    damaged({ $0.inputs[1].id = "" }, "an empty id")
    check(validate(config).isEmpty, "the good one has no problem")
} catch {
    check(false, "encode: \(error)")
}

// ---- F9 ----
let now = Date(timeIntervalSince1970: 1_800_000_000)
check(lastResultText(name: "Mac mini", result: SwitchResult(outcome: .confirmed, detail: "", readBack: 15), at: now.addingTimeInterval(-120), now: now) == "上次：切到 Mac mini · 已确认 · 2 分钟前", "two minutes ago")
check(lastResultText(name: "MacBook M1", result: SwitchResult(outcome: .sentUnconfirmed, detail: "读不回", readBack: nil), at: now.addingTimeInterval(-5), now: now) == "上次：切到 MacBook M1 · 已发送（未确认）：读不回 · 刚才", "seconds ago")
check(lastResultText(name: "X", result: SwitchResult(outcome: .notSent, detail: "DDC 写失败", readBack: nil), at: now.addingTimeInterval(-7200), now: now) == "上次：切到 X · 没有发出：DDC 写失败 · 2 小时前", "hours ago")
check(lastResultText(name: "X", result: SwitchResult(outcome: .confirmed, detail: "", readBack: nil), at: now.addingTimeInterval(60), now: now).hasSuffix("刚才"), "the future is just now")

// ---- F10 ----
check(InputPort.dp1.standardValue == 15 && InputPort.dp2.standardValue == 16 && InputPort.hdmi1.standardValue == 17 && InputPort.hdmi2.standardValue == 18 && InputPort.usbc.standardValue == 27 && InputPort.other.standardValue == nil, "standard values")

// ---- F11 ----
do {
    let msgs = LinkProtocol.sourceMessages(menuEntries(config))
    check(msgs.count == 5, "begin, three inputs, end")
    func json(_ s: String) -> [String: Any]? { (try? JSONSerialization.jsonObject(with: Data(s.utf8))) as? [String: Any] }
    check(json(msgs[0])?["op"] as? String == "sources_begin" && json(msgs[0])?["count"] as? Int == 3 && json(msgs[0])?.count == 2, "begin with the count")
    let m1 = json(msgs[2]) ?? [:]
    check(m1["op"] as? String == "source" && m1["i"] as? Int == 1 && m1["id"] as? String == "m1" && m1["name"] as? String == "MacBook M1" && m1["port"] as? String == "hdmi1" && m1.count == 5, "one input")
    check(json(msgs[3])?["port"] as? String == "usbc", "USB-C is usbc")
    check(json(msgs[4])?["op"] as? String == "sources_end" && json(msgs[4])?.count == 1, "end")
    check(!msgs.contains { $0.contains("dp2") || $0.contains("\"h2\"") || $0.contains("lost") }, "inputs without a passed trial are not sent")
    check(msgs.allSatisfy { $0.utf8.count <= 512 }, "each message fits one write")
    var many = AppConfig()
    for i in 0..<12 { many.inputs.append(InputSource(id: "i\(i)", name: String(repeating: "名", count: 10), port: .other, value: UInt16(100 + i), icon: "tv", isThisMac: false, trial: trial(.confirmed, returned: true))) }
    let big = LinkProtocol.sourceMessages(menuEntries(many))
    check(big.count == 14 && big.allSatisfy { $0.utf8.count <= 512 }, "12 inputs with long names")
    check(json(big[1])?["port"] as? String == "other", "other is other")
    check(LinkProtocol.sourceMessages([]) == [#"{"count":0,"op":"sources_begin"}"#, #"{"op":"sources_end"}"#], "an empty list")
}

// ---- F12 ----
do {
    check(LinkProtocol.parseRequest(Data(#"{"op":"switch","req":7,"id":"m1"}"#.utf8)) == SwitchRequest(req: 7, id: "m1"), "a request")
    for bad in [#""#, #"{}"#, #"{"op":"switch","req":7}"#, #"{"op":"switch","req":0,"id":"m1"}"#, #"{"op":"switch","req":7.5,"id":"m1"}"#,
                #"{"op":"switch","req":-1,"id":"m1"}"#, #"{"op":"switch","req":4294967296,"id":"m1"}"#, #"{"op":"switch","req":"7","id":"m1"}"#,
                #"{"op":"switch","req":7,"id":""}"#, #"{"op":"switch","req":7,"id":"m1","x":1}"#, #"{"op":"reboot","req":7,"id":"m1"}"#,
                #"{"op":"switch","req":7,"id":"tab\there"}"#, "{\"op\":\"switch\",\"req\":7,\"id\":\"" + String(repeating: "i", count: 49) + "\"}", "[1,2]", "not json"] {
        if LinkProtocol.parseRequest(Data(bad.utf8)) != nil { print("accepted: \(bad)") }
        check(LinkProtocol.parseRequest(Data(bad.utf8)) == nil, "refused: \(bad)")
    }
}

// ---- F13 ----
do {
    func json(_ s: String) -> [String: Any]? { (try? JSONSerialization.jsonObject(with: Data(s.utf8))) as? [String: Any] }
    let ok = json(LinkProtocol.resultMessage(req: 7, result: SwitchResult(outcome: .confirmed, detail: "", readBack: 17))) ?? [:]
    check(ok["op"] as? String == "result" && ok["req"] as? Int == 7 && ok["outcome"] as? String == "confirmed" && ok["detail"] as? String == "" && ok.count == 4, "a confirmed result")
    check(json(LinkProtocol.resultMessage(req: 8, result: SwitchResult(outcome: .sentUnconfirmed, detail: "读不回", readBack: nil)))?["outcome"] as? String == "sent_unconfirmed", "unconfirmed")
    check(json(LinkProtocol.resultMessage(req: 9, result: SwitchResult(outcome: .notSent, detail: "x", readBack: nil)))?["outcome"] as? String == "not_sent", "not sent")
    let long = LinkProtocol.resultMessage(req: 1, result: SwitchResult(outcome: .notSent, detail: String(repeating: "长", count: 100), readBack: nil))
    let d = json(long)?["detail"] as? String ?? ""
    check(d.utf8.count <= 120 && d.hasPrefix("长长") && d.allSatisfy { $0 == "长" }, "a long detail is cut on a character")
    check(LinkProtocol.refused(.notTried).outcome == .notSent && LinkProtocol.refused(.notTried).detail.contains("试切"), "not in the white list")
    check(LinkProtocol.refused(.displayMissing).outcome == .notSent && !LinkProtocol.refused(.displayMissing).detail.isEmpty, "display missing")
    check(LinkProtocol.busy.outcome == .notSent && !LinkProtocol.busy.detail.isEmpty, "busy")
}

// ---- F14 ----
do {
    check(currentInputID(config, readBack: 0x0F11) == "m1", "HDMI read back is MacBook M1")
    check(currentInputID(config, readBack: 0x0F0F) == "mini", "DP read back is this Mac")
    check(currentInputID(config, readBack: nil) == "", "no read-back: not known")
    check(currentInputID(config, readBack: 0x0F10) == "", "DP2 is not in the menu: not known")
    check(currentInputID(config, readBack: 99) == "", "a value nobody has")
    check(LinkProtocol.currentMessage("m1") == #"{"id":"m1","op":"current"}"#, "the message")
    check(LinkProtocol.currentMessage("") == #"{"id":"","op":"current"}"#, "not known")
}

if failures > 0 {
    print("core-check: \(failures) check(s) failed")
    exit(1)
}
print("core-check: all checks passed")
