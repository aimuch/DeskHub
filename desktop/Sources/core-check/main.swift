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
// F15 USB flashing, the package (13.10 P1 to P8): taken only when the manifest is JSON of at most 64 KB, schema_version the
//     integer 1, the board and chip of this device, signed_ota false, a version of 1 to 31 safe characters, the flash settings
//     dio/16MB/80m, exactly the 4 images at their fixed offsets and paths, each within its partition, no overlap, no path
//     that is absolute, has "..", a backslash, a colon or an empty part, no link, every file present with its size and
//     SHA-256, the boot and app images for ESP32-S3 (0xE9, chip 9), the app descriptor with its magic, project "deskhub"
//     and the version of the manifest. Anything else is refused with a reason, before any port is opened.
// F16 Versions compare as dotted numbers (0.3.21 > 0.3.9); anything else is "cannot tell" (nil).
// F17 The USB status (R4): only a line that is a JSON object with a boolean "ok" and a result with the device fields is an
//     answer; logs, echoes and other JSON are skipped; ok:false is "refused"; an answer with a bad id, board, protocol or
//     version is "bad". Lines come out of the byte stream whole; a frame over 16 KB or more than 128 KB in all fails.
// F18 esptool: the bundled one says 5.6.0; flash-id must show ESP32-S3 and 16 MB; the progress of the whole package is read
//     from "Writing at 0x... a/b bytes" lines, image by image, and never goes past 1.
// F19 Bluetooth update, the App's side (13.11): manifest frames are 'M' + offset (2 bytes, little endian) + bytes, in order,
//     each within the frame size, together the whole envelope, at most 4 KB; data frames are 'D' + offset (4 bytes) + bytes;
//     the window keeps at most 24 KB sent and not written. The payload has exactly the 8 fields the firmware wants, the
//     envelope exactly 3. The fingerprint is the SHA-256 of the SPKI DER in hex. Update events and the device info are read
//     strictly; anything else is not one.
// F20 Usage page, the App's side (13.12 G2, G8 to G10): from Magpie's quotas, usage and requests, exactly the fields the
//     firmware wants: the provider with the most used window, its windows (not unlimited, with a number left) by length,
//     at most 2; today only when the last bucket is today; errors never above calls; the latest model cut to 32 bytes on a
//     character boundary; seconds to reset and the age from the ISO times. No answer, or not JSON: {"op":"usage","ok":false}.
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
    // the checksum typed on its own line: Swift 6.0 to 6.2 (Xcode 16, CI) do not infer UInt8 for a chain of ^ inside the literal
    let u2720sum: UInt8 = 0x0f ^ 0x0f ^ 0x0e ^ 0x60 ^ 0x02 ^ 0x88 ^ 0x6e ^ 0x50
    if case .success(let v) = DDCReply.parseVCPFields([0x6e, 0x88, 0x02, 0x00, 0x60, 0x00, 0x00, 0x0e, 0x0f, 0x0f, u2720sum], vcp: 0x60) {
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

// ---- F15: the firmware package ----
func espImage(size: Int, chip: UInt16 = 9) -> Data {
    var d = Data(repeating: 0xff, count: size)
    d[0] = 0xE9
    d[12] = UInt8(chip & 0xff); d[13] = UInt8(chip >> 8)
    return d
}
func appImage(version: String, project: String = "deskhub", magic: UInt32 = 0xABCD5432, chip: UInt16 = 9) -> Data {
    var d = espImage(size: 4096, chip: chip)
    for i in 0..<4 { d[32 + i] = UInt8((magic >> (8 * UInt32(i))) & 0xff) }
    for i in 48..<112 { d[i] = 0 }
    for (i, b) in version.utf8.enumerated() where i < 32 { d[48 + i] = b }
    for (i, b) in project.utf8.enumerated() where i < 32 { d[80 + i] = b }
    return d
}
struct FakePackage {
    var files: [String: PackageFile] = [:]
    var manifest: [String: Any] = [:]
    init(version: String = "0.3.21") {
        let images: [(String, String, Data)] = [
            ("0x0", "bootloader/bootloader.bin", espImage(size: 2048)),
            ("0x8000", "partition_table/partition-table.bin", Data(repeating: 0xaa, count: 3072)),
            ("0xf000", "ota_data_initial.bin", Data(repeating: 0xff, count: 8192)),
            ("0x20000", "deskhub.bin", appImage(version: version)),
        ]
        var list: [[String: Any]] = []
        for (off, path, data) in images {
            files[path] = .data(data)
            list.append(["offset": off, "path": path, "size_bytes": data.count, "sha256": hex(data)])
        }
        manifest = ["schema_version": 1, "version": version, "chip": "esp32s3", "board": "ESP32-S3-Touch-AMOLED-2.16",
                    "build_verified": true, "hardware_verified": false, "signed_ota": false,
                    "flash_settings": ["flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"], "files": list]
    }
    func hex(_ d: Data) -> String { SHA256Hex.of(d) }
    func check(_ manifestData: Data? = nil) -> Result<FirmwarePackage, PackageProblem> {
        let m = manifestData ?? (try! JSONSerialization.data(withJSONObject: manifest))
        return FirmwareCheck.verify(manifest: m, file: { files[$0] ?? .missing })
    }
    mutating func setFile(_ i: Int, _ key: String, _ value: Any?) {
        var list = manifest["files"] as! [[String: Any]]
        list[i][key] = value
        manifest["files"] = list
    }
}
enum SHA256Hex { static func of(_ d: Data) -> String { FirmwareCheck.sha256HexForTests(d) } }
func refused(_ r: Result<FirmwarePackage, PackageProblem>, _ want: PackageProblem, _ what: String) {
    if case .failure(let p) = r { check(p == want, "\(what): \(p), want \(want)") } else { check(false, "\(what): taken") }
}
do {
    let good = FakePackage()
    if case .success(let pkg) = good.check() {
        check(pkg.version == "0.3.21" && pkg.images.map(\.offset) == [0x0, 0x8000, 0xF000, 0x20000], "a good package")
        check(pkg.totalBytes == 2048 + 3072 + 8192 + 4096, "its size")
    } else { check(false, "a good package is taken") }
    refused(FirmwareCheck.verify(manifest: nil, file: { _ in .missing }), .manifestMissing, "no manifest")
    refused(good.check(Data("not json".utf8)), .manifestBad, "not JSON")
    refused(good.check(Data("[1,2]".utf8)), .manifestBad, "not an object")
    refused(good.check(Data(repeating: 0x20, count: 65537)), .manifestBad, "over 64 KB")
    var p = good; p.manifest["schema_version"] = 2; refused(p.check(), .wrongSchema, "schema 2")
    p = good; p.manifest["schema_version"] = "1"; refused(p.check(), .wrongSchema, "schema as text")
    p = good; p.manifest["schema_version"] = true; refused(p.check(), .wrongSchema, "schema as true")
    p = good; p.manifest["schema_version"] = 1.5; refused(p.check(), .wrongSchema, "schema 1.5")
    p = good; p.manifest["board"] = "ESP32-S3-Box"; refused(p.check(), .wrongBoard, "another board")
    p = good; p.manifest["chip"] = "esp32"; refused(p.check(), .wrongBoard, "another chip")
    p = good; p.manifest["signed_ota"] = true; refused(p.check(), .wrongBoard, "a signed package")
    p = good; p.manifest["signed_ota"] = nil; refused(p.check(), .wrongBoard, "no signed_ota")
    for v in ["", "0.3 21", "0.3.21\n", String(repeating: "1", count: 32), "0.3/21"] {
        p = good; p.manifest["version"] = v; refused(p.check(), .badVersion, "version [\(v)]")
    }
    p = good; p.manifest["version"] = 321; refused(p.check(), .badVersion, "version as a number")
    p = good; p.manifest["flash_settings"] = ["flash_mode": "qio", "flash_size": "16MB", "flash_freq": "80m"]; refused(p.check(), .wrongFlashSettings, "qio")
    p = good; p.manifest["flash_settings"] = ["flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m", "x": 1]; refused(p.check(), .wrongFlashSettings, "an extra setting")
    p = good; var l = p.manifest["files"] as! [[String: Any]]; l.removeLast(); p.manifest["files"] = l; refused(p.check(), .wrongImageCount, "3 images")
    p = good; l = p.manifest["files"] as! [[String: Any]]; l.append(l[0]); p.manifest["files"] = l; refused(p.check(), .wrongImageCount, "5 images")
    for bad in ["/deskhub.bin", "../deskhub.bin", "a/../deskhub.bin", "deskhub\\x.bin", "c:deskhub.bin", "a//deskhub.bin", "./deskhub.bin", ""] {
        p = good; p.setFile(3, "path", bad); refused(p.check(), .badPath, "path [\(bad)]")
    }
    p = good; p.files["deskhub.bin"] = .symlink; refused(p.check(), .symlink, "a link")
    p = good; p.setFile(3, "offset", "0x30000"); refused(p.check(), .badOffset, "the app not at 0x20000")
    p = good; p.setFile(3, "offset", "131072"); refused(p.check(), .badOffset, "an offset not in hex")
    p = good; p.setFile(3, "offset", 0x20000); refused(p.check(), .badOffset, "an offset as a number")
    p = good; p.setFile(2, "path", "deskhub.bin"); refused(p.check(), .duplicate, "a path twice")
    p = good; p.setFile(2, "offset", "0x20000"); refused(p.check(), .duplicate, "an offset twice")
    p = good; p.setFile(1, "path", "ota_data_initial.bin"); p.setFile(2, "path", "partition_table/partition-table.bin"); refused(p.check(), .badOffset, "paths swapped")
    p = good; p.setFile(1, "size_bytes", 0x1001); refused(p.check(), .tooLarge, "over its partition")
    p = good; p.setFile(1, "size_bytes", 0); refused(p.check(), .tooLarge, "size 0")
    p = good; p.setFile(1, "size_bytes", "3072"); refused(p.check(), .tooLarge, "size as text")
    p = good; p.setFile(1, "sha256", "ABC"); refused(p.check(), .badDigest, "a short digest")
    p = good; p.setFile(1, "sha256", String(repeating: "A", count: 64)); refused(p.check(), .badDigest, "a digest in capitals")
    p = good; p.files["ota_data_initial.bin"] = nil; refused(p.check(), .missingFile, "a missing file")
    p = good; p.files["ota_data_initial.bin"] = .data(Data(repeating: 0xff, count: 8191)); refused(p.check(), .sizeMismatch, "a short file")
    p = good; p.files["ota_data_initial.bin"] = .data(Data(repeating: 0xfe, count: 8192)); refused(p.check(), .digestMismatch, "another file of the same size")
    func withApp(_ d: Data) -> FakePackage { var q = good; q.files["deskhub.bin"] = .data(d); q.setFile(3, "size_bytes", d.count); q.setFile(3, "sha256", SHA256Hex.of(d)); return q }
    refused(withApp(appImage(version: "0.3.21", chip: 0)).check(), .notESP32S3, "an ESP32 app")
    var noMagic = appImage(version: "0.3.21"); noMagic[0] = 0xE8; refused(withApp(noMagic).check(), .notESP32S3, "no image magic")
    refused(withApp(appImage(version: "0.3.21", magic: 0x12345678)).check(), .badDescriptor, "no descriptor magic")
    refused(withApp(appImage(version: "0.3.21", project: "other")).check(), .badDescriptor, "another project")
    refused(withApp(appImage(version: "0.3.20")).check(), .versionMismatch, "the app is another version")
    refused(withApp(Data(repeating: 0xE9, count: 100)).check(), .notESP32S3, "too short for a descriptor")
    var boot = good; let b = espImage(size: 2048, chip: 2); boot.files["bootloader/bootloader.bin"] = .data(b); boot.setFile(0, "sha256", SHA256Hex.of(b))
    refused(boot.check(), .notESP32S3, "an S2 bootloader")
    check(!PackageProblem.digestMismatch.words.isEmpty && PackageProblem.digestMismatch.words.contains("没有连设备"), "the words say nothing was touched")
}

// ---- F16: versions ----
do {
    check(FirmwareVersion.compare("0.3.21", "0.3.20") == 1 && FirmwareVersion.compare("0.3.9", "0.3.21") == -1, "dotted numbers")
    check(FirmwareVersion.compare("0.3.21", "0.3.21") == 0 && FirmwareVersion.compare("1.0", "0.9.9") == 1, "equal, and different lengths")
    check(FirmwareVersion.compare("0.3.21", "0.3.21.0") == 0, "a trailing zero")
    check(FirmwareVersion.compare("0.3.21-dev", "0.3.21") == nil && FirmwareVersion.compare("", "1") == nil && FirmwareVersion.compare("1..2", "1") == nil, "cannot tell")
}

// ---- F17: the USB status ----
do {
    func line(_ s: String) -> StatusLine { StatusParse.line(Data(s.utf8)) }
    let good = #"{"ok":true,"result":{"device_id":"deskhub-44bd8d60c598","firmware_version":"0.3.21","board_model":"ESP32-S3-Touch-AMOLED-2.16","protocol_version":"1"}}"#
    check(line(good + "\n") == .identity(DeviceIdentity(deviceID: "deskhub-44bd8d60c598", version: "0.3.21")), "an answer")
    check(line(good) == .identity(DeviceIdentity(deviceID: "deskhub-44bd8d60c598", version: "0.3.21")), "without the newline")
    check(line("I (1234) deskhub: boot\n") == .skip && line(#"{"op":"identify"}"#) == .skip && line("[1,2]") == .skip, "logs, echoes, arrays")
    check(line(#"{"ok":"yes","result":{"device_id":"deskhub-44bd8d60c598"}}"#) == .skip, "ok not a boolean")
    check(line(#"{"ok":true,"result":{"other":1}}"#) == .skip, "an answer about something else")
    check(line(#"{"ok":false,"error":"x"}"#) == .refused, "refused")
    for (field, value) in [("device_id", "\"deskhub-44BD8D60C598\""), ("device_id", "\"deskhub-44bd8d60c59\""), ("board_model", "\"other\""),
                           ("protocol_version", "\"2\""), ("protocol_version", "1"), ("firmware_version", "\"0.3 21\""), ("firmware_version", "3")] {
        let bad = good.replacingOccurrences(of: try! NSRegularExpression(pattern: "\"\(field)\":(\"[^\"]*\"|[0-9]+)").firstMatch(in: good, range: NSRange(good.startIndex..., in: good)).map { String(good[Range($0.range, in: good)!]) } ?? "", with: "\"\(field)\":\(value)")
        if case .bad = line(bad) { check(true, "") } else { check(false, "a bad \(field) \(value) is not refused") }
    }
    var r = LineReader()
    let parts = r.feed(Data("I (1) log\n{\"ok\":tr".utf8)) + r.feed(Data("ue}\nrest".utf8))
    check(parts.map { String(decoding: $0, as: UTF8.self) } == ["I (1) log\n", "{\"ok\":true}\n"] && !r.failed, "lines are whole")
    var big = LineReader(); _ = big.feed(Data(repeating: 0x41, count: StatusParse.maxFrame + 1)); check(big.failed, "a frame over 16 KB")
    var many = LineReader()
    for _ in 0..<200 { _ = many.feed(Data((String(repeating: "x", count: 1000) + "\n").utf8)) }
    check(many.failed, "more than 128 KB in all")
}

// ---- F18: esptool ----
do {
    check(EsptoolOutput.versionOK("esptool v5.6.0\n5.6.0\n") && !EsptoolOutput.versionOK("esptool v5.3.1\n5.3.1\n") && !EsptoolOutput.versionOK(""), "the version")
    let probe = "Connecting...\nChip type:          ESP32-S3 (QFN56) (revision v0.2)\nFeatures: Wi-Fi\nDetected flash size: 16MB\n"
    check(EsptoolOutput.probeOK(probe), "flash-id of this board")
    check(!EsptoolOutput.probeOK(probe.replacingOccurrences(of: "ESP32-S3", with: "ESP32-S2")), "another chip")
    check(!EsptoolOutput.probeOK(probe.replacingOccurrences(of: "16MB", with: "8MB")), "8 MB of flash")
    check(!EsptoolOutput.probeOK("A fatal error occurred: Could not open port"), "no port")
    if case .success(let pkg) = FakePackage().check() {
        let total = Double(pkg.totalBytes)
        check(EsptoolOutput.progress("Writing at 0x00000400 [==>   ] 50.0% 1024/2048 bytes...", package: pkg) == 1024 / total, "half of the first image")
        check(EsptoolOutput.progress("Writing at 0x00020800 [==>   ] 50.0% 2048/4096 bytes...", package: pkg) == (2048 + 3072 + 8192 + 2048) / total, "half of the app")
        check(EsptoolOutput.progress("Writing at 0x00021000 [=====] 100.0% 9999/4096 bytes...", package: pkg) == 1, "never past 1")
        check(EsptoolOutput.progress("Hash of data verified.", package: pkg) == nil && EsptoolOutput.progress("Writing at 0x00700000 1/2 bytes", package: pkg) == nil, "other lines")
        // esptool 5.6.0 as it really prints (2026-11-07, the run on the device): a bar, the percent, megabytes, seconds
        check(EsptoolOutput.progress("Writing at 0x00020800 ━━━━━━━━━━━━━━╸               50.0% 0.00MB/0.00MB [00s] ", package: pkg) == (2048 + 3072 + 8192 + 2048) / total, "5.6.0: half of the app")
        check(EsptoolOutput.progress("Writing at 0x00000000 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 100.0% 0.00MB/0.00MB [00s] ", package: pkg) == 2048 / total, "5.6.0: the first image done")
        check(EsptoolOutput.progress("Writing at 0x00462e60 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 100.0% 4.26MB/4.26MB [45s] ", package: pkg) == 1, "5.6.0: the end")
        check(EsptoolOutput.progress("Writing at 0x00021000 ━━━ 140.0% 1MB/1MB [01s] ", package: pkg) == 1, "5.6.0: never past 1")
    } else { check(false, "the fake package for progress") }
    // The real run on the device (docs/evidence/usb-flash-e2e/run-0.3.21.json): every progress line of esptool 5.6.0 is read,
    // the progress never goes back, and it ends at 1. The images of the 0.3.21 package, as its manifest says.
    let pkg = FirmwarePackage(version: "0.3.21", images: [
        FirmwareImage(offset: 0x0, path: "bootloader/bootloader.bin", size: 22608, sha256: ""),
        FirmwareImage(offset: 0x8000, path: "partition_table/partition-table.bin", size: 3072, sha256: ""),
        FirmwareImage(offset: 0xF000, path: "ota_data_initial.bin", size: 8192, sha256: ""),
        FirmwareImage(offset: 0x20000, path: "deskhub.bin", size: 4468320, sha256: "")])
    if let d = FileManager.default.contents(atPath: "../docs/evidence/usb-flash-e2e/run-0.3.21.json"),
       let o = try? JSONSerialization.jsonObject(with: d) as? [String: Any], let events = o["events"] as? [[String: Any]] {
        let lines = events.compactMap { $0["line"] as? String }.filter { $0.hasPrefix("Writing at") }
        let read = lines.compactMap { EsptoolOutput.progress($0, package: pkg) }
        check(lines.count > 1000 && read.count == lines.count, "every progress line of the run is read: \(read.count) of \(lines.count)")
        check(zip(read, read.dropFirst()).allSatisfy { $0 <= $1 + 1e-9 }, "the progress never goes back")
        check(read.last == 1 && (read.first ?? 1) < 0.01, "from the start to 1")
    } else { check(false, "the record of the run on the device is there") }
}

// ---- F19: the Bluetooth update ----
do {
    let env = Data((0..<1000).map { UInt8($0 % 251) })
    let frames = UpdateFrames.manifestFrames(env, maxFrame: 185)
    check(!frames.isEmpty && frames.allSatisfy { $0.count <= 185 && $0.first == 0x4D }, "manifest frames: 'M', within the size")
    var joined = Data(), expected = 0
    for f in frames {
        let off = Int(f[f.startIndex + 1]) | Int(f[f.startIndex + 2]) << 8
        check(off == expected, "manifest offset \(off), want \(expected)")
        joined.append(f.dropFirst(3))
        expected += f.count - 3
    }
    check(joined == env, "the frames carry the whole envelope")
    check(UpdateFrames.manifestFrames(Data(repeating: 1, count: 4097), maxFrame: 185).isEmpty, "over 4 KB: nothing")
    check(UpdateFrames.manifestFrames(Data(), maxFrame: 185).isEmpty && UpdateFrames.manifestFrames(env, maxFrame: 3).isEmpty, "empty, or no room")
    let d = UpdateFrames.dataFrame(offset: 0x01020304, Data([9, 8, 7]))
    check(d == Data([0x44, 0x04, 0x03, 0x02, 0x01, 9, 8, 7]), "data frame")
    check(UpdateFrames.dataPerFrame(maxFrame: 244) == 239 && UpdateFrames.dataPerFrame(maxFrame: 5) == 0, "data per frame")
    check(UpdateFrames.maysend(sent: 0, written: 0, next: 239), "start")
    check(UpdateFrames.maysend(sent: 24 * 1024 - 239, written: 0, next: 239) && !UpdateFrames.maysend(sent: 24 * 1024 - 238, written: 0, next: 239), "the window")
    check(UpdateFrames.maysend(sent: 40000, written: 20000, next: 239), "the device caught up")
    check(UpdateFrames.begin == Data([0x42]) && UpdateFrames.end == Data([0x45]) && UpdateFrames.abort == Data([0x58]), "one-byte frames")

    let sha = String(repeating: "ab", count: 32)
    let payload = UpdateManifest.payload(version: "0.3.23", size: 4479088, sha256: sha)
    if let o = try? JSONSerialization.jsonObject(with: payload) as? [String: Any] {
        check(Set(o.keys) == ["schema_version", "board_model", "chip", "version", "config_schema_min", "config_schema_max", "image_size", "image_sha256"], "the 8 fields")
        check(o["schema_version"] as? Int == 1 && o["config_schema_min"] as? Int == 1 && o["config_schema_max"] as? Int == 1, "the schemas")
        check(o["board_model"] as? String == "ESP32-S3-Touch-AMOLED-2.16" && o["chip"] as? String == "esp32s3", "board and chip")
        check(o["version"] as? String == "0.3.23" && o["image_size"] as? Int == 4479088 && o["image_sha256"] as? String == sha, "version, size, hash")
    } else { check(false, "the payload is JSON") }
    let sig = Data(repeating: 0x30, count: 70)
    let envelope = UpdateManifest.envelope(payload: payload, signatureDER: sig)
    if let o = try? JSONSerialization.jsonObject(with: envelope) as? [String: Any] {
        check(o.count == 3 && o["algorithm"] as? String == "ecdsa-p256-sha256", "the envelope's fields")
        check((o["payload"] as? String).flatMap { Data(base64Encoded: $0) } == payload && (o["signature"] as? String).flatMap { Data(base64Encoded: $0) } == sig, "base64 both ways")
    } else { check(false, "the envelope is JSON") }
    check(envelope.count < UpdateFrames.maxManifest, "the envelope fits 4 KB")
    check(UpdateManifest.fingerprint(spkiDER: Data("abc".utf8)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "fingerprint is SHA-256 hex")

    func ev(_ s: String) -> UpdateEvent? { UpdateEvent.parse(Data(s.utf8)) }
    check(ev(#"{"op":"update","state":"ready"}"#) == .ready && ev(#"{"op":"update","state":"rebooting"}"#) == .rebooting, "ready, rebooting")
    check(ev(#"{"op":"update","state":"writing","written":16384}"#) == .writing(16384), "writing")
    check(ev(#"{"op":"update","state":"refused","error":"UPDATE_NOT_NEWER"}"#) == .refused("UPDATE_NOT_NEWER"), "refused")
    check(ev(#"{"op":"update","state":"failed","error":"UPDATE_IDLE"}"#) == .failed("UPDATE_IDLE"), "failed")
    check(ev(#"{"op":"switch","req":1,"id":"x"}"#) == nil && ev(#"{"op":"update","state":"dancing"}"#) == nil && ev("junk") == nil, "not update events")
    check(ev(#"{"op":"update","state":"writing"}"#) == nil && ev(#"{"op":"update","state":"writing","written":-1}"#) == nil && ev(#"{"op":"update","state":"refused"}"#) == nil, "missing fields")
    check(!UpdateEvent.failed("UPDATE_NOT_NEWER").words.isEmpty && UpdateEvent.reason("WHAT").contains("WHAT"), "the words")

    let info = DeviceInfo.parse(Data(#"{"protocol":1,"device":"deskhub-44bd8d60c598","firmware":"0.3.22","update_key":"\#(sha)"}"#.utf8))
    check(info == DeviceInfo(device: "deskhub-44bd8d60c598", firmware: "0.3.22", updateKey: sha), "the info")
    check(DeviceInfo.parse(Data(#"{"protocol":1,"device":"deskhub-44bd8d60c598","firmware":"0.3.21"}"#.utf8))?.updateKey == "", "an older firmware: no key field")
    check(DeviceInfo.parse(Data("[]".utf8)) == nil && DeviceInfo.parse(Data(#"{"protocol":1}"#.utf8)) == nil, "not an info")
}

// ---- F20: the Usage page ----
do {
    var cal = Calendar(identifier: .gregorian)
    cal.timeZone = TimeZone(identifier: "Asia/Shanghai")!
    let now = ISO8601DateFormatter().date(from: "2026-11-08T06:40:00Z")!   // 14:40 in Shanghai
    let quotas = Data("""
    {"object":"list","data":[
      {"provider":"Codex","name":"Codex","kind":"subscription","plan":"Plus","user":"someone@example.com","windows":[
        {"name":"7 days","used":63,"remaining":37,"resetsAt":"2026-11-11T22:20:00Z"},
        {"name":"5 hours","used":0,"remaining":100,"resetsAt":"2026-11-08T10:52:00Z"}]},
      {"provider":"other","name":"Other","windows":[{"name":"daily","remaining":90,"resetsAt":"2026-11-09T00:00:00Z"},{"name":"monthly","unlimited":true}]}
    ]}
    """.utf8)
    let usage = Data("""
    {"period":"30d","bucket":"day","calls":5000,"series":[
      {"label":"Nov 7","time":"2026-11-07T00:00:00+08:00","calls":10,"errors":1,"input":1,"output":1,"cache_read":1,"cache_write":1,"reasoning":1},
      {"label":"Nov 8","time":"2026-11-08T00:00:00+08:00","calls":128,"errors":2,"input":1000000,"output":200000,"cache_read":30000,"cache_write":4000,"reasoning":567,"timed":100,"ttft_ms":82000}
    ]}
    """.utf8)
    let requests = Data("""
    {"rows":[{"t":"2026-11-08T06:38:00Z","model":"gpt-6.1-sol","served":"gpt-6.1-sol","status":200,"ms":3200,"session_account":"secret@example.com"},
             {"t":"2026-11-08T06:30:00Z","model":"older","status":500,"ms":1}]}
    """.utf8)
    func obj(_ s: String) -> [String: Any] { (try? JSONSerialization.jsonObject(with: Data(s.utf8))) as? [String: Any] ?? [:] }
    let m = MagpieUsage.message(quotas: quotas, usage: usage, requests: requests, now: now, calendar: cal)
    let o = obj(m)
    check(Set(o.keys) == ["op", "ok", "p", "plan", "w", "c", "e", "t", "f", "lm", "ls", "lms", "la"], "exactly the fields: \(o.keys.sorted())")
    check(o["op"] as? String == "usage" && o["ok"] as? Bool == true, "op and ok")
    check(o["p"] as? String == "codex" && o["plan"] as? String == "plus", "provider and plan, lower case")
    let w = o["w"] as? [[String: Any]] ?? []
    check(w.count == 2 && w.first?["m"] as? Int == 300 && w.first?["r"] as? Int == 100 && w.last?["m"] as? Int == 10080 && w.last?["r"] as? Int == 37, "the windows of the most used provider, by length: \(w)")
    check(w.first?["s"] as? Int == 4 * 3600 + 12 * 60 && w.last?["s"] as? Int == 3 * 86400 + 15 * 3600 + 40 * 60, "seconds to reset")
    check(o["c"] as? Int == 128 && o["e"] as? Int == 2 && o["t"] as? Int == 1_234_567 && o["f"] as? Int == 820, "today")
    check(o["lm"] as? String == "gpt-6.1-sol" && o["ls"] as? Int == 200 && o["lms"] as? Int == 3200 && o["la"] as? Int == 120, "the latest request")
    check(!m.contains("example.com") && !m.contains("secret"), "no account goes out")
    check(m.utf8.count <= 512, "at most 512 bytes")
    // G8: the last bucket is yesterday: today is zero
    let tomorrow = now.addingTimeInterval(86400)
    let late = obj(MagpieUsage.message(quotas: quotas, usage: usage, requests: requests, now: tomorrow, calendar: cal))
    check(late["c"] as? Int == 0 && late["e"] as? Int == 0 && late["t"] as? Int == 0 && late["f"] as? Int == 0, "a new day with no bucket yet")
    check((late["w"] as? [[String: Any]])?.first?["s"] as? Int == 0, "a reset in the past is 0 (not known), never negative")
    // errors above calls, a long model, a status out of range, no timing
    let odd = Data("""
    {"series":[{"time":"2026-11-08T00:00:00+08:00","calls":3,"errors":9,"input":5}]}
    """.utf8)
    let longModel = String(repeating: "模", count: 11) + "x"   // 34 bytes: cut to 10 characters (30 bytes)
    let oddRequests = Data("""
    {"rows":[{"t":"not a time","model":"\(longModel)","status":2000,"ms":99999999}]}
    """.utf8)
    let x = obj(MagpieUsage.message(quotas: quotas, usage: odd, requests: oddRequests, now: now, calendar: cal))
    check(x["c"] as? Int == 3 && x["e"] as? Int == 3 && x["f"] as? Int == 0, "errors never above calls")
    check(x["lm"] as? String == String(repeating: "模", count: 10), "the model cut on a character boundary: \(x["lm"] ?? "")")
    check(x["ls"] as? Int == 0 && x["lms"] as? Int == 3_600_000 && x["la"] as? Int == 0, "values put in range")
    // G9: no window with a number left
    let none = Data(#"{"data":[{"provider":"x","windows":[{"name":"5 hours","unlimited":true},{"name":"weird","remaining":10}]}]}"#.utf8)
    let n = obj(MagpieUsage.message(quotas: none, usage: usage, requests: nil, now: now, calendar: cal))
    check((n["w"] as? [Any])?.isEmpty == true && n["p"] as? String == "x", "no usable window: none sent")
    check(n["lm"] as? String == "" && n["ls"] as? Int == 0 && n["la"] as? Int == 0, "no requests: no latest")
    // names the device would refuse are cleaned
    let messy = Data(#"{"data":[{"provider":"Open AI!","plan":"Pro Max Ultra Plus Edition","windows":[]}]}"#.utf8)
    let ms = obj(MagpieUsage.message(quotas: messy, usage: usage, requests: nil, now: now, calendar: cal))
    check(ms["p"] as? String == "openai" && (ms["plan"] as? String)?.count == 16, "names cleaned and cut: \(ms["p"] ?? "") \(ms["plan"] ?? "")")
    // G2: Magpie did not answer, or not JSON
    check(MagpieUsage.message(quotas: nil, usage: usage, requests: requests, now: now) == MagpieUsage.offline, "no quotas")
    check(MagpieUsage.message(quotas: quotas, usage: Data("<html>".utf8), requests: requests, now: now) == MagpieUsage.offline, "not JSON")
    check(obj(MagpieUsage.offline).count == 2, "the offline message has two fields")
}

if failures > 0 {
    print("core-check: \(failures) check(s) failed")
    exit(1)
}
print("core-check: all checks passed")
