// The to-do board, the App's side (docs/desktop-app-design.md 13.13): which reminders go to the touch screen, in which
// order and in which words, and how the device's actions are read. No EventKit here: the App reads the reminders and
// turns them into ReminderItem, so all of this is tested on its own (core-check F22). The firmware's side is
// firmware/components/ble_link/ble_protocol.cpp: the field names and limits must stay the same.
import CryptoKit
import Foundation

public struct ReminderItem: Sendable {
    public var key: String              // the reminder's own id (calendarItemIdentifier)
    public var title: String
    public var list: String
    public var due: DateComponents?     // with an hour for a timed one; without for an all-day one; nil: no date
    public var created: Date?
    public var listID: String           // the list's own id (calendarIdentifier), 13.14
    public var `repeat`: String         // TodoBoard.repeatCode of its rules
    public var alarm: Bool              // it has at least one alarm
    public var modified: Date?
    public init(key: String, title: String, list: String, due: DateComponents?, created: Date?, listID: String = "", repeat: String = "", alarm: Bool = false, modified: Date? = nil) {
        self.key = key
        self.title = title
        self.list = list
        self.due = due
        self.created = created
        self.listID = listID
        self.repeat = `repeat`
        self.alarm = alarm
        self.modified = modified
    }
}

// A recurrence rule as far as the touch screen can say it (13.14). EventKit's EKRecurrenceRule is turned into this in the App.
public struct RepeatSpec: Equatable, Sendable {
    public enum Frequency: String, Sendable { case daily = "d", weekly = "w", monthly = "m", yearly = "y" }
    public var frequency: Frequency
    public var interval: Int
    public var weekdays: [Int]?         // EventKit's numbering: 1 Sunday ... 7 Saturday
    public var hasEnd: Bool
    public var other: Bool              // days of the month, months of the year, set positions: more than the screen can say
    public init(frequency: Frequency, interval: Int, weekdays: [Int]? = nil, hasEnd: Bool = false, other: Bool = false) {
        self.frequency = frequency
        self.interval = interval
        self.weekdays = weekdays
        self.hasEnd = hasEnd
        self.other = other
    }
}

public struct TodoEditRequest: Equatable, Sendable {
    public var req: UInt32
    public var id: String
    public var version: String
    public var due: DateComponents?     // nil: no date; no hour: all day
    public var `repeat`: String         // "x": leave the rules as they are
    public var listID: String           // the handle of a list
    public var alarm: Bool
}

public enum AlarmPlan: Equatable, Sendable { case keep, removeAll, addAtDue, shiftAbsolute }

public enum TodoKind: String, Sendable { case overdue = "o", today = "t", tomorrow = "m", later = "l", none = "n" }

public struct TodoLine: Equatable, Sendable {
    public var id: String               // 12 lowercase hex digits
    public var key: String              // the reminder's own id: stays in the App
    public var title: String
    public var list: String
    public var kind: TodoKind
    public var month: Int, day: Int, minute: Int   // 0, 0, -1 without a date; minute -1 all day
    public var year: Int = 0
    public var `repeat`: String = ""
    public var listID: String = ""      // the handle of its list (12 hex digits)
    public var alarm: Bool = false      // only with a time
    public var version: String = ""
}

public struct TodoBoardList: Equatable, Sendable {
    public var lines: [TodoLine]
    public var total: Int
}

public enum TodoAction: String, Sendable { case done, tomorrow, delete }
public struct TodoAct: Equatable, Sendable {
    public var req: UInt32
    public var id: String
    public var action: TodoAction
    public init(req: UInt32, id: String, action: TodoAction) {
        self.req = req
        self.id = id
        self.action = action
    }
}

public enum TodoOff: String, Sendable { case access, setting }

public enum TodoBoard {
    public static let maxItems = 30, maxTitle = 96, maxList = 32, maxDetail = 64
    public static let resendSeconds: TimeInterval = 30   // the device calls a list older than 2 minutes out of date (L4)

    public static func handle(_ key: String) -> String {
        SHA256.hash(data: Data(key.utf8)).prefix(6).map { String(format: "%02x", $0) }.joined()
    }

    static func date(of due: DateComponents, calendar: Calendar) -> Date? {
        var c = DateComponents(year: due.year, month: due.month, day: due.day, hour: due.hour ?? 0, minute: due.minute ?? 0)
        c.timeZone = due.timeZone ?? calendar.timeZone
        return calendar.date(from: c)
    }
    static func timed(_ due: DateComponents) -> Bool { due.hour != nil }

    public static func kind(due: DateComponents?, now: Date, calendar: Calendar) -> TodoKind {
        guard let due, due.year != nil, due.month != nil, due.day != nil, let when = date(of: due, calendar: calendar) else { return .none }
        if timed(due) && when < now { return .overdue }
        let today = calendar.startOfDay(for: now), that = calendar.startOfDay(for: when)
        let days = calendar.dateComponents([.day], from: today, to: that).day ?? 0
        if days < 0 { return .overdue }
        if days == 0 { return .today }
        if days == 1 { return .tomorrow }
        return .later
    }

    // On a character boundary, without control characters; never empty (the firmware wants at least one byte for a title).
    static func cut(_ s: String, bytes: Int) -> String {
        var out = ""
        for ch in s where !ch.unicodeScalars.contains(where: { $0.value < 0x20 || $0.value == 0x7F }) {
            if out.utf8.count + String(ch).utf8.count > bytes { break }
            out.append(ch)
        }
        return out
    }

    public static func lines(_ items: [ReminderItem], now: Date, calendar: Calendar) -> TodoBoardList {
        let order: [TodoKind: Int] = [.overdue: 0, .today: 1, .tomorrow: 2, .later: 3, .none: 4]
        struct Entry { var item: ReminderItem; var kind: TodoKind; var when: Date? }
        let entries = items.map { i -> Entry in
            let k = kind(due: i.due, now: now, calendar: calendar)
            return Entry(item: i, kind: k, when: k == .none ? nil : i.due.flatMap { date(of: $0, calendar: calendar) })
        }
        let sorted = entries.sorted { a, b in
            if a.kind != b.kind { return order[a.kind]! < order[b.kind]! }
            if let x = a.when, let y = b.when, x != y { return x < y }
            if a.kind == .none {
                switch (a.item.created, b.item.created) {
                case let (x?, y?) where x != y: return x < y
                case (.some, nil): return true
                case (nil, .some): return false
                default: break
                }
            }
            return a.item.title < b.item.title
        }
        let lines = sorted.prefix(maxItems).map { e -> TodoLine in
            var title = cut(e.item.title.trimmingCharacters(in: .whitespacesAndNewlines), bytes: maxTitle)
            if title.isEmpty { title = "-" }
            let d = e.kind == .none ? nil : e.item.due
            let minute = d.map { timed($0) ? ($0.hour ?? 0) * 60 + ($0.minute ?? 0) : -1 } ?? -1
            return TodoLine(id: handle(e.item.key), key: e.item.key, title: title, list: cut(e.item.list, bytes: maxList), kind: e.kind,
                            month: d?.month ?? 0, day: d?.day ?? 0, minute: minute, year: d?.year ?? 0,
                            repeat: validRepeat(e.item.repeat) ? e.item.repeat : "x", listID: handle(e.item.listID),
                            alarm: e.item.alarm && minute >= 0, version: version(e.item))
        }
        return TodoBoardList(lines: Array(lines), total: items.count)
    }

    private static func encode(_ object: [String: Any]) -> String {
        let data = (try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys, .withoutEscapingSlashes])) ?? Data()
        return String(data: data, encoding: .utf8) ?? ""
    }

    public static func messages(_ board: TodoBoardList) -> [String] {
        var out = [encode(["op": "todos_begin", "n": board.lines.count, "total": max(board.total, board.lines.count)])]
        for (i, l) in board.lines.enumerated() {
            out.append(encode(["op": "todo", "i": i, "id": l.id, "t": l.title, "l": l.list, "k": l.kind.rawValue, "y": l.year, "m": l.month, "d": l.day,
                               "hm": l.minute, "r": l.repeat, "li": l.listID, "a": l.alarm, "v": l.version]))
        }
        out.append(encode(["op": "todos_end"]))
        return out
    }
    public static func offMessage(_ why: TodoOff) -> String { encode(["op": "todos_off", "why": why.rawValue]) }
    public static func resultMessage(req: UInt32, ok: Bool, detail: String) -> String {
        encode(["op": "todo_result", "req": Int(req), "ok": ok, "detail": cut(detail, bytes: maxDetail)])
    }

    // Strict: exactly op, req, id and a, of the right kinds; anything else is nil.
    public static func parseAct(_ data: Data) -> TodoAct? {
        guard data.count <= LinkProtocol.maxMessage, let o = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              Set(o.keys) == ["op", "req", "id", "a"], o["op"] as? String == "todo_act",
              let n = o["req"] as? NSNumber, CFGetTypeID(n) == CFNumberGetTypeID(), n.doubleValue == n.doubleValue.rounded(),
              n.doubleValue >= 1, n.doubleValue <= Double(UInt32.max),
              let id = o["id"] as? String, id.count == 12, id.allSatisfy({ "0123456789abcdef".contains($0) }),
              let a = (o["a"] as? String).flatMap(TodoAction.init(rawValue:))
        else { return nil }
        return TodoAct(req: UInt32(n.doubleValue), id: id, action: a)
    }


    // ---- 13.14: the repeat, the version, the lists, the edit ----
    public static let maxLists = 16

    public static func validRepeat(_ r: String) -> Bool {
        if r.isEmpty || r == "wd" || r == "x" { return true }
        guard let f = r.first, "dwmy".contains(f), let n = Int(r.dropFirst()), (1...99).contains(n), String(n) == String(r.dropFirst()) else { return false }
        return true
    }
    public static func repeatCode(_ spec: RepeatSpec?) -> String {
        guard let spec else { return "" }
        if spec.hasEnd || spec.other || !(1...99).contains(spec.interval) { return "x" }
        if let days = spec.weekdays {
            return spec.frequency == .weekly && spec.interval == 1 && Set(days) == [2, 3, 4, 5, 6] && days.count == 5 ? "wd" : "x"
        }
        return spec.frequency.rawValue + String(spec.interval)
    }
    public static func repeatSpec(_ code: String) -> RepeatSpec? {
        if code == "wd" { return RepeatSpec(frequency: .weekly, interval: 1, weekdays: [2, 3, 4, 5, 6]) }
        guard code != "x", validRepeat(code), let f = code.first.flatMap({ RepeatSpec.Frequency(rawValue: String($0)) }), let n = Int(code.dropFirst()) else { return nil }
        return RepeatSpec(frequency: f, interval: n)
    }
    // M3: changes whenever anything the touch screen shows or edits changes, or the reminder was saved again.
    public static func version(_ i: ReminderItem) -> String {
        let d = i.due.map { "\($0.year ?? 0)-\($0.month ?? 0)-\($0.day ?? 0)T\($0.hour ?? -1):\($0.minute ?? -1)" } ?? "none"
        let text = [i.title, d, i.repeat, i.listID, i.alarm ? "1" : "0", String(i.modified?.timeIntervalSince1970 ?? 0)].joined(separator: "\u{1f}")
        return SHA256.hash(data: Data(text.utf8)).prefix(4).map { String(format: "%02x", $0) }.joined()
    }
    public static func listMessages(_ lists: [(id: String, name: String)]) -> [String] {
        let shown = Array(lists.prefix(maxLists))
        var out = [encode(["op": "todo_lists_begin", "n": shown.count])]
        for (i, l) in shown.enumerated() {
            var name = cut(l.name.trimmingCharacters(in: .whitespacesAndNewlines), bytes: maxList)
            if name.isEmpty { name = "-" }
            out.append(encode(["op": "todo_list", "i": i, "id": handle(l.id), "name": name]))
        }
        out.append(encode(["op": "todo_lists_end"]))
        return out
    }
    // Strict, like the firmware's valid_edit: a real date or none, a repeat it knows, no ring without a time.
    public static func parseEdit(_ data: Data) -> TodoEditRequest? {
        guard data.count <= LinkProtocol.maxMessage, let o = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              Set(o.keys) == ["op", "req", "id", "v", "y", "m", "d", "hm", "r", "li", "a"], o["op"] as? String == "todo_edit" else { return nil }
        func int(_ k: String) -> Int? {
            guard let n = o[k] as? NSNumber, CFGetTypeID(n) == CFNumberGetTypeID(), n.doubleValue == n.doubleValue.rounded(), abs(n.doubleValue) < 1e10 else { return nil }
            return n.intValue
        }
        func hex(_ k: String, _ len: Int) -> String? {
            guard let s = o[k] as? String, s.count == len, s.allSatisfy({ "0123456789abcdef".contains($0) }) else { return nil }
            return s
        }
        guard let req = int("req"), req >= 1, req <= Int(UInt32.max), let id = hex("id", 12), let v = hex("v", 8), let li = hex("li", 12),
              let y = int("y"), let m = int("m"), let d = int("d"), let hm = int("hm"), let r = o["r"] as? String, validRepeat(r),
              let aNum = o["a"] as? NSNumber, CFGetTypeID(aNum) == CFBooleanGetTypeID() else { return nil }
        let alarm = aNum.boolValue
        var due: DateComponents?
        if y == 0 && m == 0 && d == 0 && hm == -1 {
            due = nil
        } else {
            var c = DateComponents(year: y, month: m, day: 1)
            c.calendar = Calendar(identifier: .gregorian)
            guard (2000...2100).contains(y), (1...12).contains(m), (-1...1439).contains(hm),
                  let first = Calendar(identifier: .gregorian).date(from: c),
                  let days = Calendar(identifier: .gregorian).range(of: .day, in: .month, for: first), days.contains(d) else { return nil }
            due = hm >= 0 ? DateComponents(year: y, month: m, day: d, hour: hm / 60, minute: hm % 60) : DateComponents(year: y, month: m, day: d)
        }
        if alarm && (due?.hour == nil) { return nil }
        return TodoEditRequest(req: UInt32(req), id: id, version: v, due: due, repeat: r, listID: li, alarm: alarm)
    }
    // nil: go ahead; else why not (sent back as the detail of todo_result).
    public static func refusal(for e: TodoEditRequest, currentVersion: String, listIDs: Set<String>) -> String? {
        if e.version != currentVersion { return "这条待办在 Mac 上改过了" }
        if !listIDs.contains(e.listID) { return "这个清单不能用了" }
        return nil
    }
    public static func alarmPlan(had: Bool, want: Bool, dueChanged: Bool) -> AlarmPlan {
        if had && !want { return .removeAll }
        if !had && want { return .addAtDue }
        if had && want && dueChanged { return .shiftAbsolute }
        return .keep
    }

    // L13: tomorrow is the Mac's tomorrow. A timed one keeps its time; an all-day or undated one becomes tomorrow, all day.
    public static func postponed(due: DateComponents?, now: Date, calendar: Calendar) -> DateComponents {
        let tomorrow = calendar.date(byAdding: .day, value: 1, to: calendar.startOfDay(for: now)) ?? now
        let d = calendar.dateComponents([.year, .month, .day], from: tomorrow)
        if let due, timed(due) {
            return DateComponents(year: d.year, month: d.month, day: d.day, hour: due.hour, minute: due.minute ?? 0)
        }
        return DateComponents(year: d.year, month: d.month, day: d.day)
    }
    // How far absolute alarms move with the due date: whole days between the old and the new day; nil without an old date.
    public static func alarmShift(from old: DateComponents?, to new: DateComponents, calendar: Calendar) -> TimeInterval? {
        guard let old, let a = date(of: old, calendar: calendar), let b = date(of: new, calendar: calendar) else { return nil }
        let days = calendar.dateComponents([.day], from: calendar.startOfDay(for: a), to: calendar.startOfDay(for: b)).day ?? 0
        return TimeInterval(days * 86400)
    }
}
