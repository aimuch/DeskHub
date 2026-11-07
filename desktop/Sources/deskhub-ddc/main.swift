// deskhub-ddc: the DDC code of the App on the command line, for experiments and evidence. One JSON object per line.
//   deskhub-ddc list                                every external display: identity, and whether it has DDC
//   deskhub-ddc get <uuid> <vcp-hex>                read one VCP code (0x60 is the input source); changes nothing
//   deskhub-ddc switch <uuid> <value> <home> <s>    the App's switch to <value>, then after <s> seconds back to <home>;
//                                                   prints both results. CHANGES THE SCREEN: only with someone watching.
import DDC
import DeskHubCore
import Foundation

func emit(_ fields: [String: Any]) {
    let data = try! JSONSerialization.data(withJSONObject: fields, options: [.sortedKeys])
    print(String(data: data, encoding: .utf8)!)
    fflush(stdout)
}
func fail(_ why: String) -> Never {
    emit(["error": why])
    exit(1)
}
func result(_ r: SwitchResult) -> [String: Any] {
    var f: [String: Any] = ["outcome": r.outcome.rawValue, "detail": r.detail]
    if let v = r.readBack { f["read_back"] = Int(v) }
    return f
}

let args = Array(CommandLine.arguments.dropFirst())
guard let command = args.first else { fail("usage: deskhub-ddc list | get <uuid> <vcp-hex> | switch <uuid> <value> <home> <seconds>") }
do {
    let displays = try DDCDisplay.list()
    func display(_ uuid: String) -> DDCDisplay {
        guard let d = displays.first(where: { $0.identity.uuid == uuid.uppercased() }) else { fail("no display with that uuid") }
        return d
    }
    switch command {
    case "list":
        for d in displays {
            emit(["uuid": d.identity.uuid, "serial": d.identity.serial, "location": d.identity.location, "name": d.identity.name, "ddc": d.hasDDC])
        }
    case "get":
        guard args.count == 3, let vcp = UInt8(args[2].replacingOccurrences(of: "0x", with: ""), radix: 16) else { fail("usage: get <uuid> <vcp-hex>") }
        let d = display(args[1])
        let v = try d.read(vcp: vcp)
        emit(["uuid": d.identity.uuid, "vcp": String(format: "0x%02x", vcp), "current": Int(v.current), "maximum": Int(v.maximum)])
    case "switch":
        guard args.count == 5, let value = UInt16(args[2]), let home = UInt16(args[3]), let wait = Double(args[4]),
              (1...255).contains(value), (1...255).contains(home), wait >= 3, wait <= 120 else { fail("usage: switch <uuid> <value 1-255> <home 1-255> <seconds 3-120>") }
        let d = display(args[1])
        let before = readInput(d)
        emit(["step": "before", "read": (try? before.get()).map { Int($0) } ?? -1, "time": ISO8601DateFormatter().string(from: Date())])
        emit(["step": "to", "target": Int(value), "time": ISO8601DateFormatter().string(from: Date())].merging(result(performSwitch(d, to: value))) { $1 })
        Thread.sleep(forTimeInterval: wait)
        emit(["step": "back", "target": Int(home), "time": ISO8601DateFormatter().string(from: Date())].merging(result(performSwitch(d, to: home))) { $1 })
    default:
        fail("unknown command \(command)")
    }
} catch {
    fail("\(error)")
}
