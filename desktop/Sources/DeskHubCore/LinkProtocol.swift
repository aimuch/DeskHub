// The App's side of the touch screen link (docs/desktop-app-design.md 13.3). The firmware's side is
// firmware/components/ble_link/ble_protocol.cpp: the field names, limits and outcome words must stay the same.
import Foundation

public struct SwitchRequest: Equatable, Sendable {
    public var req: UInt32
    public var id: String
    public init(req: UInt32, id: String) {
        self.req = req
        self.id = id
    }
}

public enum LinkProtocol {
    public static let maxMessage = 512, maxSources = 12, maxID = 48, maxDetail = 120

    public static func portKey(_ p: InputPort) -> String { p.rawValue }   // dp1 dp2 hdmi1 hdmi2 usbc other

    private static func encode(_ object: [String: Any]) -> String {
        let data = (try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys, .withoutEscapingSlashes])) ?? Data()
        return String(data: data, encoding: .utf8) ?? ""
    }

    // Only inputs that passed their trial go out (the caller passes menuEntries); at most 12.
    public static func sourceMessages(_ inputs: [InputSource]) -> [String] {
        let list = Array(inputs.prefix(maxSources))
        var out = [encode(["op": "sources_begin", "count": list.count])]
        for (i, input) in list.enumerated() {
            out.append(encode(["op": "source", "i": i, "id": input.id, "name": input.name, "port": portKey(input.port)]))
        }
        out.append(encode(["op": "sources_end"]))
        return out
    }

    private static func cut(_ s: String, bytes: Int) -> String {
        var out = ""
        for ch in s {
            if out.utf8.count + String(ch).utf8.count > bytes { break }
            out.append(ch)
        }
        return out
    }

    public static func resultMessage(req: UInt32, result: SwitchResult) -> String {
        let outcome: String
        switch result.outcome {
        case .confirmed: outcome = "confirmed"
        case .sentUnconfirmed: outcome = "sent_unconfirmed"
        case .notSent: outcome = "not_sent"
        }
        let detail = cut(result.detail.filter { !$0.unicodeScalars.contains { $0.value < 0x20 } }, bytes: maxDetail)
        return encode(["op": "result", "req": Int(req), "outcome": outcome, "detail": detail])
    }

    public static func currentMessage(_ id: String) -> String { encode(["op": "current", "id": id]) }

    // Strict: exactly op, req and id, of the right kinds; anything else is nil.
    public static func parseRequest(_ data: Data) -> SwitchRequest? {
        guard data.count <= maxMessage, let o = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              Set(o.keys) == ["op", "req", "id"], o["op"] as? String == "switch",
              let n = o["req"] as? NSNumber, CFGetTypeID(n) == CFNumberGetTypeID(), !CFNumberIsFloatType(n) || n.doubleValue.rounded() == n.doubleValue,
              n.doubleValue >= 1, n.doubleValue <= Double(UInt32.max),
              let id = o["id"] as? String, !id.isEmpty, id.utf8.count <= maxID, !id.unicodeScalars.contains(where: { $0.value < 0x20 || $0.value == 0x7F })
        else { return nil }
        if CFNumberIsFloatType(n) && n.doubleValue != n.doubleValue.rounded() { return nil }
        return SwitchRequest(req: UInt32(n.doubleValue), id: id)
    }

    // How a refused request is answered: not sent, and why. Nothing goes to the monitor.
    public static func refused(_ r: Refusal) -> SwitchResult {
        let why: String
        switch r {
        case .notBound: why = "电脑上还没有绑定显示器"
        case .displayMissing: why = "电脑找不到这台显示器"
        case .bindingChanged: why = "显示器换了接口，要在电脑上重新绑定"
        case .unknownInput: why = "电脑上没有这个输入源"
        case .notTried: why = "这个输入源还没有在电脑上试切"
        case .isThisMac, .noWayBack: why = "这个输入源不能从这里切"
        }
        return SwitchResult(outcome: .notSent, detail: why, readBack: nil)
    }
    public static let busy = SwitchResult(outcome: .notSent, detail: "电脑正在切换别的输入源", readBack: nil)
}
