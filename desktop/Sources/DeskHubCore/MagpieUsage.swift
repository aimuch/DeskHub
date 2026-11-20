// The Usage page, the App's side (docs/desktop-app-design.md 13.12): from what the local Magpie answers to the one message
// the touch screen takes. The device's side is firmware/components/ble_link/ble_protocol.cpp ("usage"): keep the fields
// and limits the same. Nothing private goes out: no key, no account, no e-mail, no request content.
import Foundation

public enum MagpieUsage {
    public static let base = URL(string: "http://127.0.0.1:3425")!
    public static let paths = ["/v1/magpie/quotas", "/v1/magpie/usage", "/v1/magpie/usage/requests"]
    public static let offline = #"{"ok":false,"op":"usage"}"#

    // quotas and usage must be there; requests may be missing (no latest request then). nil data: Magpie did not answer.
    public static func message(quotas: Data?, usage: Data?, requests: Data?, now: Date, calendar: Calendar = .current) -> String {
        guard let q = json(quotas), let u = json(usage) else { return offline }
        // G9: the provider whose window is used the most; its windows (not unlimited, with a number left), by length, at most 2
        var best: (provider: [String: Any], windows: [(m: Int, r: Int, s: Int)], low: Int)?
        for case let provider as [String: Any] in q["data"] as? [Any] ?? [] {
            let windows = windowsOf(provider, now: now)
            let low = windows.map(\.r).min() ?? 101
            if best == nil || low < best!.low { best = (provider, windows, low) }
        }
        let windows = (best?.windows ?? []).sorted { $0.m < $1.m }.prefix(2)
        // G8: today only when the last bucket is today
        var calls = 0, errors = 0, tokens = 0, ttft = 0
        if let today = (u["series"] as? [Any])?.last as? [String: Any], let t = (today["time"] as? String).flatMap(date), calendar.isDate(t, inSameDayAs: now) {
            calls = clamp(int(today["calls"]), 0, 1_000_000_000)
            errors = clamp(int(today["errors"]), 0, calls)
            tokens = clamp(["input", "output", "cache_read", "cache_write", "reasoning"].reduce(0) { $0 + max(0, int(today[$1])) }, 0, 1_000_000_000_000_000)
            let timed = int(today["timed"])
            ttft = timed > 0 ? clamp(int(today["ttft_ms"]) / timed, 0, 600_000) : 0
        }
        // the latest request: the first row
        var model = "", status = 0, ms = 0, age = 0
        if let row = (json(requests)?["rows"] as? [Any])?.first as? [String: Any] {
            model = cut((row["served"] as? String) ?? (row["model"] as? String) ?? "", bytes: 32)
            let s = int(row["status"])
            status = (0...999).contains(s) ? s : 0
            ms = clamp(int(row["ms"]), 0, 3_600_000)
            age = (row["t"] as? String).flatMap(date).map { clamp(Int(now.timeIntervalSince($0)), 0, 31_536_000) } ?? 0
        }
        let o: [String: Any] = [
            "op": "usage", "ok": true,
            "p": name(best?.provider["provider"] as? String ?? ""), "plan": name(best?.provider["plan"] as? String ?? ""),
            "w": windows.map { ["m": $0.m, "r": $0.r, "s": $0.s] },
            "c": calls, "e": errors, "t": tokens, "f": ttft, "lm": model, "ls": status, "lms": ms, "la": age,
        ]
        guard let d = try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys, .withoutEscapingSlashes]), d.count <= 512 else { return offline }
        return String(decoding: d, as: UTF8.self)
    }

    // A provider's windows: not unlimited, with a number left, a known length; seconds to reset from the ISO time
    static func windowsOf(_ provider: [String: Any], now: Date) -> [(m: Int, r: Int, s: Int)] {
        var windows: [(m: Int, r: Int, s: Int)] = []
        for case let w as [String: Any] in provider["windows"] as? [Any] ?? [] {
            guard (w["unlimited"] as? Bool) != true, let left = number(w["remaining"]), let m = minutes(w["name"] as? String ?? "") else { continue }
            let reset = (w["resetsAt"] as? String).flatMap(date).map { Int($0.timeIntervalSince(now)) } ?? 0
            windows.append((m, clamp(Int(left.rounded()), 0, 100), clamp(reset, 0, 31_536_000)))
        }
        return windows
    }

    // Every provider, for the 小麦 gateway only (its push takes 64 KB; the touch screen keeps the one message above, 512 bytes):
    // the same today and latest fields, and "ps": [{"id", "n" (the name Magpie shows), "plan", "w": up to 3 windows by length,
    // "x": why there is no number (Magpie's error, with anything like an e-mail taken out)}], at most maxProviders.
    // nil: Magpie did not answer.
    public static let maxProviders = 8, maxAll = 8192
    public static func all(quotas: Data?, usage: Data?, requests: Data?, now: Date, calendar: Calendar = .current) -> String? {
        guard let q = json(quotas), json(usage) != nil else { return nil }
        guard var o = json(Data(message(quotas: quotas, usage: usage, requests: requests, now: now, calendar: calendar).utf8)),
              o["ok"] as? Bool == true else { return nil }
        var providers: [[String: Any]] = []
        var seen = Set<String>()
        for case let provider as [String: Any] in q["data"] as? [Any] ?? [] {
            if providers.count >= maxProviders { break }
            let id = name(provider["provider"] as? String ?? "")
            guard !id.isEmpty, seen.insert(id).inserted else { continue }
            let windows = windowsOf(provider, now: now).sorted { $0.m < $1.m }.prefix(3)
            var p: [String: Any] = ["id": id, "n": cut(provider["name"] as? String ?? id, bytes: 32),
                                    "plan": name(provider["plan"] as? String ?? ""), "w": windows.map { ["m": $0.m, "r": $0.r, "s": $0.s] }]
            if windows.isEmpty, let e = provider["error"] as? String, !e.isEmpty {
                let clean = e.replacingOccurrences(of: #"\S+@\S+"#, with: "…", options: .regularExpression)
                p["x"] = cut(clean, bytes: 120)
            }
            providers.append(p)
        }
        o["op"] = "usage_all"
        o["ps"] = providers
        o.removeValue(forKey: "p"); o.removeValue(forKey: "plan"); o.removeValue(forKey: "w")
        guard let d = try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys, .withoutEscapingSlashes]), d.count <= maxAll else { return nil }
        return String(decoding: d, as: UTF8.self)
    }

    static func json(_ d: Data?) -> [String: Any]? { d.flatMap { (try? JSONSerialization.jsonObject(with: $0)) as? [String: Any] } }
    static func number(_ v: Any?) -> Double? {
        guard let n = v as? NSNumber, CFGetTypeID(n) != CFBooleanGetTypeID() else { return nil }
        return n.doubleValue.isFinite ? n.doubleValue : nil
    }
    static func int(_ v: Any?) -> Int {
        guard let d = number(v), d > -9e18, d < 9e18 else { return 0 }
        return Int(d)
    }
    static func clamp(_ v: Int, _ lo: Int, _ hi: Int) -> Int { min(hi, max(lo, v)) }
    static func date(_ s: String) -> Date? {
        let f = ISO8601DateFormatter()
        if let d = f.date(from: s) { return d }
        f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return f.date(from: s)
    }
    // "5 hours" 300, "7 days" 10080, "daily" 1440, "weekly" 10080, "monthly" 43200, "45 minutes" 45; anything else nil
    static func minutes(_ name: String) -> Int? {
        let n = name.lowercased().trimmingCharacters(in: .whitespaces)
        if let fixed = ["hourly": 60, "daily": 1440, "weekly": 10080, "monthly": 43200][n] { return fixed }
        let parts = n.split(separator: " ")
        guard parts.count == 2, let count = Int(parts[0]), count > 0 else { return nil }
        let unit: Int
        switch parts[1] {
        case "minute", "minutes", "min", "mins": unit = 1
        case "hour", "hours", "h": unit = 60
        case "day", "days", "d": unit = 1440
        case "week", "weeks": unit = 10080
        default: return nil
        }
        let m = count * unit
        return (1...525_600).contains(m) ? m : nil
    }
    // what the device takes: 0 to 16 of a-z 0-9 . _ -
    static func name(_ s: String) -> String {
        String(s.lowercased().unicodeScalars.filter { ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "." || $0 == "_" || $0 == "-" }.prefix(16).map(Character.init))
    }
    // G10: at most `bytes` of UTF-8, whole characters, no control characters
    static func cut(_ s: String, bytes: Int) -> String {
        var out = ""
        for ch in s where !ch.unicodeScalars.contains(where: { $0.value < 0x20 || $0.value == 0x7f }) {
            if out.utf8.count + String(ch).utf8.count > bytes { break }
            out.append(ch)
        }
        return out
    }
}
