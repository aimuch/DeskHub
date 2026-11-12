// The link to the 小麦 gateway (DeskHubCore/GatewayProtocol.swift): the ThinkSmart screen in the study shows the inputs, the
// Magpie usage and the reminders, and asks for a switch or a finished to-do, through the gateway. It is the touch screen's
// link over HTTP instead of Bluetooth: the same messages go out, the same requests come in, the same white list decides.
// The pass phrase is kept in the Keychain; the address and the switches in the defaults.
import DeskHubCore
import Foundation
import Security

@MainActor
final class GatewayLink: ObservableObject {
    enum State: Equatable {
        case off
        case connecting
        case connected        // the last push was taken
        case failed(String)
    }

    @Published private(set) var state: State = .off
    @Published private(set) var lastPush: Date?
    @Published private(set) var log: [String] = []          // the requests from the screen and their results, newest first
    @Published var address: String { didSet { UserDefaults.standard.set(address, forKey: "gatewayAddress") } }
    @Published var allowTodo: Bool { didSet { UserDefaults.standard.set(allowTodo, forKey: "gatewayAllowTodo"); push() } }
    @Published private(set) var enabled: Bool

    var onRequest: ((SwitchRequest) -> Void)?
    var onTodoAct: ((TodoAct) -> Void)?
    var sources: () -> [String] = { [] }
    var current: () -> String = { "" }
    var inputName: (String) -> String = { $0 }               // an input's id -> its name in the menu (for the log)

    private var usage: String?
    private var todos: [String] = []
    private var timer: Timer?
    private var waiting: Task<Void, Never>?
    private var pushing = false
    private var labels: [UInt32: String] = [:]               // req -> what was asked, for the log

    init() {
        address = UserDefaults.standard.string(forKey: "gatewayAddress") ?? ""
        allowTodo = UserDefaults.standard.object(forKey: "gatewayAllowTodo") as? Bool ?? true
        enabled = UserDefaults.standard.bool(forKey: "gatewayOn")
        if enabled { start() }
    }

    var isLive: Bool { state == .connected }
    var hasToken: Bool { Keychain.read() != nil }

    // ---- what the settings page asks ----
    func connect(address text: String, token: String) -> String? {
        guard let url = GatewayProtocol.baseURL(text) else { return "地址不对：写成 10.10.10.224:16688 这样" }
        let t = token.trimmingCharacters(in: .whitespacesAndNewlines)
        if !t.isEmpty {
            guard GatewayProtocol.tokenOK(t) else { return "口令至少 \(GatewayProtocol.minToken) 个字符，不能有空格" }
            guard Keychain.write(t) else { return "口令没能存进钥匙串" }
        } else if !hasToken { return "要填连接口令" }
        address = url.absoluteString
        enabled = true
        UserDefaults.standard.set(true, forKey: "gatewayOn")
        stop()
        start()
        return nil
    }
    func disconnect() {
        enabled = false
        UserDefaults.standard.set(false, forKey: "gatewayOn")
        stop()
        state = .off
    }

    // ---- the same calls as TouchScreenLink ----
    func sendCurrent(_ id: String) { push() }
    func resendSources() { push() }
    func sendUsage(_ message: String) { usage = message }
    func sendTodos(_ messages: [String]) { todos = messages }
    func sendResult(_ message: String) {
        if let o = (try? JSONSerialization.jsonObject(with: Data(message.utf8))) as? [String: Any], let req = (o["req"] as? NSNumber)?.uint32Value {
            let what = labels.removeValue(forKey: req) ?? "请求"
            let detail = (o["detail"] as? String).flatMap { $0.isEmpty ? nil : "：\($0)" } ?? ""
            let outcome = (o["outcome"] as? String).map { ["confirmed": "已确认", "sent_unconfirmed": "已发送（未确认）", "not_sent": "没有切"][$0] ?? $0 }
                ?? ((o["ok"] as? Bool) == true ? "已完成" : "没做成")
            note("\(what) → \(outcome)\(detail)")
        }
        Task { await post(GatewayProtocol.resultPath, GatewayProtocol.resultBody(message)) }
    }

    // ---- the loops ----
    private func start() {
        guard enabled, timer == nil else { return }
        state = .connecting
        timer = Timer.scheduledTimer(withTimeInterval: GatewayProtocol.pushSeconds, repeats: true) { [weak self] _ in Task { @MainActor in self?.push() } }
        push()
        waiting = Task { [weak self] in await self?.waitLoop() }
    }
    private func stop() {
        timer?.invalidate()
        timer = nil
        waiting?.cancel()
        waiting = nil
    }

    func push() {
        guard enabled, !pushing else { return }
        pushing = true
        let messages = sources() + [LinkProtocol.currentMessage(current())] + (usage.map { [$0] } ?? []) + todos
        let body = GatewayProtocol.pushBody(name: Host.current().localizedName ?? "Mac", messages: messages, allowTodo: allowTodo)
        Task {
            let status = await post(GatewayProtocol.pushPath, body)
            pushing = false
            guard enabled else { return }
            if status == 200 {
                state = .connected
                lastPush = Date()
            } else {
                state = .failed(status == 0 ? "连不上 \(GatewayProtocol.baseURL(address)?.host ?? address)" : GatewayProtocol.problem(status: status) ?? "")
            }
        }
    }

    private func waitLoop() async {
        while !Task.isCancelled {
            guard enabled, state == .connected, let req = request(GatewayProtocol.waitPath, timeout: 35) else {
                try? await Task.sleep(nanoseconds: 2_000_000_000)
                continue
            }
            guard let (data, response) = try? await URLSession.shared.data(for: req), (response as? HTTPURLResponse)?.statusCode == 200 else {
                try? await Task.sleep(nanoseconds: 3_000_000_000)
                continue
            }
            for command in GatewayProtocol.commands(data) { handle(command) }
        }
    }

    private func handle(_ command: Data) {
        if let r = LinkProtocol.parseRequest(command) {
            labels[r.req] = "切到 \(inputName(r.id))"
            onRequest?(r)
        } else if let t = TodoBoard.parseAct(command) {
            labels[t.req] = "待办\(t.action == .done ? "完成" : t.action.rawValue)"
            guard allowTodo, t.action == .done else {   // the gateway asks only for "done"; anything else is refused
                sendResult(TodoBoard.resultMessage(req: t.req, ok: false, detail: allowTodo ? "只能在屏上完成待办" : "Mac 上没允许在屏上完成待办"))
                return
            }
            onTodoAct?(t)
        }
    }

    private func note(_ s: String) {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        log = Array(([f.string(from: Date()) + "  " + s] + log).prefix(20))
    }

    // ---- HTTP ----
    private func request(_ path: String, timeout: TimeInterval) -> URLRequest? {
        guard let base = GatewayProtocol.baseURL(address), let url = URL(string: base.absoluteString + path), let token = Keychain.read() else { return nil }
        var r = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: timeout)
        r.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        r.setValue("application/json", forHTTPHeaderField: "Content-Type")
        return r
    }
    // -> the HTTP status, 0 when there was no answer
    @discardableResult
    private func post(_ path: String, _ body: Data) async -> Int {
        guard var r = request(path, timeout: 8) else { return 0 }
        r.httpMethod = "POST"
        r.httpBody = body
        guard let (_, response) = try? await URLSession.shared.data(for: r) else { return 0 }
        return (response as? HTTPURLResponse)?.statusCode ?? 0
    }
}

// The pass phrase in the login Keychain (a generic password of this App).
enum Keychain {
    private static let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                               kSecAttrService as String: "DeskHub 小麦网关", kSecAttrAccount as String: "gateway"]
    static func read() -> String? {
        var q = query
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
        return String(data: d, encoding: .utf8)
    }
    static func write(_ value: String) -> Bool {
        SecItemDelete(query as CFDictionary)
        var q = query
        q[kSecValueData as String] = Data(value.utf8)
        q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        return SecItemAdd(q as CFDictionary, nil) == errSecSuccess
    }
}
