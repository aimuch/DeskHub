// The link to the 小麦 gateway (DeskHubCore/GatewayProtocol.swift): the ThinkSmart screen in the study shows the inputs, the
// Magpie usage and the reminders, and asks for a switch or a finished to-do, through the gateway. It is the touch screen's
// link over HTTP instead of Bluetooth: the same messages go out, the same requests come in, the same white list decides.
// The pass phrase is kept in the Keychain; the address and the switches in the defaults.
import DeskHubCore
import Combine
import Foundation
import Security

// The 控制终端 (ThinkSmartView design canvas board 70): one App, several 小麦 gateways (the study's, the office's, …) and the
// touch screen over Bluetooth. Each gateway gets the same inputs, usage and reminders; a request comes back from one of
// them and its answer goes back to that one only. The list is in the defaults, each pass phrase in the Keychain under the
// terminal's id. A Mac that had one gateway (App 0.4.4) keeps it as the first terminal.
@MainActor
final class GatewayLink: ObservableObject {
    @Published private(set) var terminals: [GatewayTerminal] = []
    @Published private(set) var state: GatewayTerminal.State = .off    // .connected when any terminal is

    var onRequest: ((SwitchRequest) -> Void)?
    var onTodoAct: ((TodoAct) -> Void)?
    var sources: () -> [String] = { [] }
    var current: () -> String = { "" }
    var inputName: (String) -> String = { $0 }

    private var asked: [UInt32: GatewayTerminal] = [:]        // req -> the terminal it came from
    private var watches: [AnyCancellable] = []

    init() {
        if let list = UserDefaults.standard.array(forKey: "gatewayTerminals") as? [[String: Any]] {
            terminals = list.compactMap { d in
                guard let id = d["id"] as? String else { return nil }
                return GatewayTerminal(id: id, name: d["name"] as? String ?? "小麦网关", address: d["address"] as? String ?? "",
                                       enabled: d["enabled"] as? Bool ?? false, allowTodo: d["allowTodo"] as? Bool ?? true)
            }
        } else if let old = UserDefaults.standard.string(forKey: "gatewayAddress"), !old.isEmpty {
            // App 0.4.4 had one gateway: its pass phrase stays where it was (Keychain account "gateway")
            terminals = [GatewayTerminal(id: "gateway", name: "小麦网关", address: old, enabled: UserDefaults.standard.bool(forKey: "gatewayOn"),
                                         allowTodo: UserDefaults.standard.object(forKey: "gatewayAllowTodo") as? Bool ?? true)]
        }
        terminals.forEach(adopt)
        save()
        terminals.forEach { $0.begin() }
        recount()
    }

    var isLive: Bool { terminals.contains { $0.isLive } }

    // ---- the settings page ----
    @discardableResult
    func add() -> GatewayTerminal {
        let t = GatewayTerminal(id: UUID().uuidString, name: terminals.isEmpty ? "小麦网关" : "小麦网关 \(terminals.count + 1)",
                                address: "", enabled: false, allowTodo: true)
        adopt(t)
        terminals.append(t)
        save()
        return t
    }
    func remove(_ t: GatewayTerminal) {
        t.forget()
        terminals.removeAll { $0 === t }
        asked = asked.filter { $0.value !== t }
        save()
        rewatch()
        recount()
    }

    // ---- the same calls as TouchScreenLink, to every terminal ----
    func sendCurrent(_ id: String) { terminals.forEach { $0.sendCurrent(id) } }
    func resendSources() { terminals.forEach { $0.resendSources() } }
    func sendUsage(_ message: String) { terminals.forEach { $0.sendUsage(message) } }
    func sendTodos(_ messages: [String]) { terminals.forEach { $0.sendTodos(messages) } }
    // an answer: to the terminal that asked (by its req), or to all when it is not known any more
    func sendResult(_ message: String) {
        let req = ((try? JSONSerialization.jsonObject(with: Data(message.utf8))) as? [String: Any])?["req"] as? NSNumber
        if let r = req?.uint32Value, let t = asked.removeValue(forKey: r) {
            t.sendResult(message)
        } else {
            terminals.forEach { $0.sendResult(message) }
        }
    }

    private func adopt(_ t: GatewayTerminal) {
        t.save = { [weak self] in self?.save() }
        t.sources = { [weak self] in self?.sources() ?? [] }
        t.current = { [weak self] in self?.current() ?? "" }
        t.inputName = { [weak self] id in self?.inputName(id) ?? id }
        t.onRequest = { [weak self, weak t] r in
            guard let self, let t else { return }
            self.asked[r.req] = t
            self.onRequest?(r)
        }
        t.onTodoAct = { [weak self, weak t] a in
            guard let self, let t else { return }
            self.asked[a.req] = t
            self.onTodoAct?(a)
        }
        rewatch(adding: t)
    }
    private func rewatch(adding: GatewayTerminal? = nil) {
        if adding == nil { watches = [] }
        for t in adding.map({ [$0] }) ?? terminals {
            watches.append(t.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() })
            watches.append(t.$state.sink { [weak self] _ in DispatchQueue.main.async { self?.recount() } })
        }
    }
    private func recount() {
        let states = terminals.map(\.state)
        let next: GatewayTerminal.State = states.contains(.connected) ? .connected
            : states.contains(.connecting) ? .connecting
            : states.first { if case .failed = $0 { return true } else { return false } } ?? .off
        if next != state { state = next }
    }
    private func save() {
        UserDefaults.standard.set(terminals.map(\.saved), forKey: "gatewayTerminals")
    }
}

// One 控制终端: one 小麦 gateway this Mac pushes to and takes requests from (the screens on that gateway use it).
@MainActor
final class GatewayTerminal: ObservableObject, Identifiable {
    enum State: Equatable {
        case off
        case connecting
        case connected        // the last push was taken
        case failed(String)
    }

    @Published private(set) var state: State = .off
    @Published private(set) var lastPush: Date?
    @Published private(set) var log: [String] = []          // the requests from the screen and their results, newest first
    let id: String
    @Published var name: String { didSet { save() } }
    @Published var address: String { didSet { save() } }
    @Published var allowTodo: Bool { didSet { save(); push() } }
    @Published private(set) var enabled: Bool
    var save: () -> Void = {}                                 // the list (GatewayLink) keeps the terminals in the defaults

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

    init(id: String, name: String, address: String, enabled: Bool, allowTodo: Bool) {
        self.id = id
        self.name = name
        self.address = address
        self.allowTodo = allowTodo
        self.enabled = enabled
    }
    func begin() { if enabled { start() } }
    var saved: [String: Any] { ["id": id, "name": name, "address": address, "enabled": enabled, "allowTodo": allowTodo] }

    var isLive: Bool { state == .connected }
    var hasToken: Bool { Keychain.read(id) != nil }

    // ---- what the settings page asks ----
    func connect(address text: String, token: String) -> String? {
        guard let url = GatewayProtocol.baseURL(text) else { return "地址不对：写成 10.10.10.224:16688 这样" }
        let t = token.trimmingCharacters(in: .whitespacesAndNewlines)
        if !t.isEmpty {
            guard GatewayProtocol.tokenOK(t) else { return "口令至少 \(GatewayProtocol.minToken) 个字符，不能有空格" }
            guard Keychain.write(id, t) else { return "口令没能存进钥匙串" }
        } else if !hasToken { return "要填连接口令" }
        address = url.absoluteString
        enabled = true
        save()
        stop()
        start()
        return nil
    }
    func disconnect() {
        enabled = false
        save()
        stop()
        state = .off
    }
    func forget() {           // removed from the list: stop, and the pass phrase leaves the Keychain
        disconnect()
        Keychain.delete(id)
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
        guard let base = GatewayProtocol.baseURL(address), let url = URL(string: base.absoluteString + path), let token = Keychain.read(id) else { return nil }
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

// The pass phrases in the login Keychain (generic passwords of this App), one per terminal: the account is the terminal's id
// ("gateway" for the one App 0.4.4 had).
enum Keychain {
    private static func query(_ account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: "DeskHub 小麦网关", kSecAttrAccount as String: account]
    }
    static func read(_ account: String) -> String? {
        var q = query(account)
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
        return String(data: d, encoding: .utf8)
    }
    static func write(_ account: String, _ value: String) -> Bool {
        SecItemDelete(query(account) as CFDictionary)
        var q = query(account)
        q[kSecValueData as String] = Data(value.utf8)
        q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        return SecItemAdd(q as CFDictionary, nil) == errSecSuccess
    }
    static func delete(_ account: String) {
        SecItemDelete(query(account) as CFDictionary)
    }
}
