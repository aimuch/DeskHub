// The link to the 小麦 gateway (ThinkSmartView server, core/integrations/deskhub.py): the App's side, without the network.
// The App connects to the gateway, never the other way round (the Mac opens no port):
//   POST /deskhub/push    {"name", "messages": [the touch screen's own messages], "allow_todo"}   every 5 s
//   GET  /deskhub/wait    the long poll: {"ok": true, "commands": ["{\"op\":\"switch\",…}", …]}
//   POST /deskhub/result  {"message": the touch screen's own result message}
// The messages and the commands are those of the touch screen (LinkProtocol, TodoBoard), so the same parsers and the same
// white list decide. Authorization: Bearer <the connection pass phrase set in the gateway's admin page>.
import Foundation

public enum GatewayProtocol {
    public static let pushPath = "/deskhub/push", waitPath = "/deskhub/wait", resultPath = "/deskhub/result"
    public static let pushSeconds: TimeInterval = 5
    public static let minToken = 12, maxCommands = 20, maxName = 40

    // "10.10.10.224:16688", "http://hub.local:16688/" -> http://10.10.10.224:16688. Only http(s), a host, no path, query or user.
    public static func baseURL(_ text: String) -> URL? {
        var t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if t.isEmpty { return nil }
        if !t.contains("://") { t = "http://" + t }
        while t.hasSuffix("/") { t.removeLast() }
        guard let c = URLComponents(string: t), let scheme = c.scheme?.lowercased(), scheme == "http" || scheme == "https",
              let host = c.host, !host.isEmpty, c.user == nil, c.password == nil, c.path.isEmpty, c.query == nil, c.fragment == nil
        else { return nil }
        return c.url
    }

    public static func tokenOK(_ t: String) -> Bool {
        t.count >= minToken && t.count <= 200 && !t.unicodeScalars.contains { $0.value <= 0x20 || $0.value == 0x7F }
    }

    private static func json(_ o: [String: Any]) -> Data {
        (try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys, .withoutEscapingSlashes])) ?? Data()
    }

    public static func pushBody(name: String, messages: [String], allowTodo: Bool) -> Data {
        var n = ""
        for ch in name where !ch.unicodeScalars.contains(where: { $0.value < 0x20 }) {
            if n.count >= maxName { break }
            n.append(ch)
        }
        return json(["name": n.isEmpty ? "Mac" : n, "messages": messages.filter { !$0.isEmpty }, "allow_todo": allowTodo])
    }

    public static func resultBody(_ message: String) -> Data { json(["message": message]) }

    // The commands of a wait answer: strings of at most 512 bytes, at most 20. Anything else: none.
    public static func commands(_ data: Data) -> [Data] {
        guard let o = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any], let list = o["commands"] as? [Any] else { return [] }
        return list.prefix(maxCommands).compactMap { ($0 as? String).map { Data($0.utf8) } }.filter { $0.count <= LinkProtocol.maxMessage }
    }

    // What the settings page says about an answer of the gateway.
    public static func problem(status: Int) -> String? {
        switch status {
        case 200: return nil
        case 401: return "口令不对（网关回 401）"
        case 403: return "网关后台还没设 DeskHub 的连接口令，或者没打开这个插件"
        case 404: return "这个地址上没有小麦网关（404），端口一般是 16688"
        case 429: return "口令错太多次，网关 5 分钟后再让连"
        default: return "网关回 \(status)"
        }
    }
}
