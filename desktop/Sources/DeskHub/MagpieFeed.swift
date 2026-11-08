// The Usage page's feed (docs/desktop-app-design.md 13.12): every 15 s, and at once when the touch screen connects, the
// App reads the local Magpie (127.0.0.1 only) and sends one "usage" message over Bluetooth. Only while the touch screen is
// connected and the switch in Settings > General is on.
import Combine
import DeskHubCore
import Foundation

@MainActor
final class MagpieFeed: ObservableObject {
    private static let key = "magpieOnTouchScreen"
    @Published var enabled: Bool {
        didSet { UserDefaults.standard.set(enabled, forKey: Self.key); if enabled { tick() } }
    }
    @Published private(set) var note: String?          // what the last read found, for the settings page
    private weak var link: TouchScreenLink?
    private var timer: Timer?
    private var watch: AnyCancellable?
    private var busy = false

    init() {
        enabled = UserDefaults.standard.object(forKey: Self.key) as? Bool ?? true
    }

    func start(_ link: TouchScreenLink) {
        guard self.link == nil else { return }
        self.link = link
        timer = Timer.scheduledTimer(withTimeInterval: 15, repeats: true) { [weak self] _ in Task { @MainActor in self?.tick() } }
        watch = link.$state.sink { [weak self] state in
            if case .connected = state { DispatchQueue.main.asyncAfter(deadline: .now() + 1) { self?.tick() } }
        }
    }

    func tick() {
        guard enabled, !busy, let link, case .connected = link.state else { return }
        busy = true
        Task.detached {
            async let q = Self.fetch(MagpieUsage.paths[0])
            async let u = Self.fetch(MagpieUsage.paths[1])
            async let r = Self.fetch(MagpieUsage.paths[2] + "?limit=1")
            let message = MagpieUsage.message(quotas: await q, usage: await u, requests: await r, now: Date())
            await MainActor.run {
                self.busy = false
                self.note = message == MagpieUsage.offline ? "Magpie 没有运行（127.0.0.1:3425 没有回应）" : "Magpie 已连上，每 15 秒发给触摸屏"
                link.sendUsage(message)
            }
        }
    }

    // nil: no answer in 3 s, not HTTP 200, or not from this Mac
    nonisolated static func fetch(_ path: String) async -> Data? {
        guard let url = URL(string: path, relativeTo: MagpieUsage.base), url.host == "127.0.0.1" else { return nil }
        var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 3)
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        guard let (data, response) = try? await URLSession.shared.data(for: request), (response as? HTTPURLResponse)?.statusCode == 200 else { return nil }
        return data
    }
}
