// The to-do board's feed (docs/desktop-app-design.md 13.13): the unfinished reminders of this Mac go to the touch screen
// when it connects, when Reminders change, and every 30 s; the device's actions (done, tomorrow, delete) come back here.
// Only while the touch screen is connected. The switch is in Settings > General.
import Combine
import DeskHubCore
import EventKit
import Foundation

@MainActor
final class TodoFeed: ObservableObject {
    private static let key = "todosOnTouchScreen"
    @Published var enabled: Bool {
        didSet { UserDefaults.standard.set(enabled, forKey: Self.key); tick() }
    }
    @Published private(set) var note: String?   // what the last read found, for the settings page
    private let store = EKEventStore()
    private weak var link: TouchScreenLink?
    private var timer: Timer?
    private var watch: AnyCancellable?
    private var changes: NSObjectProtocol?
    private var shown: [String: String] = [:]   // the ids on the touch screen -> the reminders' own ids (L9, L10)
    private var asked = false
    private var busy = false

    init() {
        enabled = UserDefaults.standard.object(forKey: Self.key) as? Bool ?? true
    }

    func start(_ link: TouchScreenLink) {
        guard self.link == nil else { return }
        self.link = link
        link.onTodoAct = { [weak self] act in self?.perform(act) }
        link.onTodoEdit = { [weak self] edit in self?.apply(edit) }
        timer = Timer.scheduledTimer(withTimeInterval: TodoBoard.resendSeconds, repeats: true) { [weak self] _ in Task { @MainActor in self?.tick() } }
        watch = link.$state.sink { [weak self] state in
            if case .connected = state { DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { self?.tick() } }
        }
        changes = NotificationCenter.default.addObserver(forName: .EKEventStoreChanged, object: store, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
    }

    private var allowed: Bool {
        let s = EKEventStore.authorizationStatus(for: .reminder)
        if #available(macOS 14.0, *) { return s == .fullAccess }
        return s == .authorized
    }

    func tick() {
        guard let link, case .connected = link.state, !busy else { return }
        guard enabled else { link.sendTodos([TodoBoard.offMessage(.setting)]); note = nil; return }
        if !allowed {
            if EKEventStore.authorizationStatus(for: .reminder) == .notDetermined && !asked {
                asked = true
                let done: @Sendable (Bool, Error?) -> Void = { [weak self] _, _ in Task { @MainActor in self?.tick() } }
                if #available(macOS 14.0, *) { store.requestFullAccessToReminders(completion: done) }
                else { store.requestAccess(to: .reminder, completion: done) }
                return
            }
            link.sendTodos([TodoBoard.offMessage(.access)])
            note = "DeskHub 没有读提醒事项的权限：在 系统设置 › 隐私与安全性 › 提醒事项 里打开 DeskHub。"
            return
        }
        busy = true
        let predicate = store.predicateForIncompleteReminders(withDueDateStarting: nil, ending: nil, calendars: nil)
        store.fetchReminders(matching: predicate) { [weak self] reminders in
            let items = (reminders ?? []).map(TodoFeed.item)
            Task { @MainActor in
                guard let self else { return }
                self.busy = false
                let board = TodoBoard.lines(items, now: Date(), calendar: .current)
                self.shown = Dictionary(board.lines.map { ($0.id, $0.key) }, uniquingKeysWith: { a, _ in a })
                self.note = "触摸屏上显示 \(board.lines.count) 条（一共 \(board.total) 条未完成）；提醒事项一变就发，另外每 30 秒一次"
                let lists = self.writableLists().map { (id: $0.calendarIdentifier, name: $0.title) }
                self.link?.sendTodos(TodoBoard.listMessages(lists) + TodoBoard.messages(board))
            }
        }
    }


    // ---- 13.14 ----
    nonisolated static func spec(_ r: EKRecurrenceRule) -> RepeatSpec? {
        let f: RepeatSpec.Frequency
        switch r.frequency {
        case .daily: f = .daily
        case .weekly: f = .weekly
        case .monthly: f = .monthly
        case .yearly: f = .yearly
        @unknown default: return RepeatSpec(frequency: .daily, interval: 1, other: true)
        }
        let days = r.daysOfTheWeek
        let other = (days?.contains { $0.weekNumber != 0 } ?? false) || r.daysOfTheMonth != nil || r.monthsOfTheYear != nil ||
                    r.weeksOfTheYear != nil || r.daysOfTheYear != nil || r.setPositions != nil
        return RepeatSpec(frequency: f, interval: r.interval, weekdays: days?.map { $0.dayOfTheWeek.rawValue }, hasEnd: r.recurrenceEnd != nil, other: other)
    }
    nonisolated static func repeatCode(_ r: EKReminder) -> String {
        guard let rules = r.recurrenceRules, !rules.isEmpty else { return "" }
        if rules.count > 1 { return "x" }
        return TodoBoard.repeatCode(spec(rules[0]))
    }
    nonisolated static func item(_ r: EKReminder) -> ReminderItem {
        ReminderItem(key: r.calendarItemIdentifier, title: r.title ?? "", list: r.calendar?.title ?? "", due: r.dueDateComponents, created: r.creationDate,
                     listID: r.calendar?.calendarIdentifier ?? "", repeat: repeatCode(r), alarm: r.hasAlarms, modified: r.lastModifiedDate)
    }
    private func writableLists() -> [EKCalendar] {
        store.calendars(for: .reminder).filter { $0.allowsContentModifications }
    }

    // M1 to M6: the to-do must be the one the touch screen showed (its version), the list must still take it; then change it.
    private func apply(_ e: TodoEditRequest) {
        guard let link else { return }
        func answer(_ ok: Bool, _ detail: String) {
            link.sendResult(TodoBoard.resultMessage(req: e.req, ok: ok, detail: detail))
            tick()
        }
        guard enabled, allowed else { answer(false, "Mac 上的 DeskHub 现在不能改提醒事项"); return }
        guard let key = shown[e.id], let r = store.calendarItem(withIdentifier: key) as? EKReminder, !r.isCompleted else {
            answer(false, "这条待办已经变了")
            return
        }
        let lists = writableLists()
        let byHandle = Dictionary(lists.map { (TodoBoard.handle($0.calendarIdentifier), $0) }, uniquingKeysWith: { a, _ in a })
        if let why = TodoBoard.refusal(for: e, currentVersion: TodoBoard.version(TodoFeed.item(r)), listIDs: Set(byHandle.keys)) { answer(false, why); return }
        let spec = TodoBoard.repeatSpec(e.repeat)
        if e.due == nil && (spec != nil || (e.repeat == "x" && r.hasRecurrenceRules)) { answer(false, "重复要有日期"); return }
        let cal = Calendar.current
        func date(_ c: DateComponents?) -> Date? { c.flatMap { var x = $0; x.calendar = cal; x.timeZone = x.timeZone ?? cal.timeZone; return cal.date(from: x) } }
        let oldDue = r.dueDateComponents
        let dueChanged = date(oldDue) != date(e.due) || (oldDue?.hour == nil) != (e.due?.hour == nil)
        let had = r.hasAlarms
        do {
            if let to = byHandle[e.listID], to.calendarIdentifier != r.calendar?.calendarIdentifier { r.calendar = to }
            if e.repeat != "x" {
                for rule in r.recurrenceRules ?? [] { r.removeRecurrenceRule(rule) }
            }
            r.dueDateComponents = e.due
            if e.repeat != "x", let spec {
                let f: EKRecurrenceFrequency = spec.frequency == .daily ? .daily : spec.frequency == .weekly ? .weekly : spec.frequency == .monthly ? .monthly : .yearly
                let days = spec.weekdays?.compactMap { EKWeekday(rawValue: $0) }.map { EKRecurrenceDayOfWeek($0) }
                r.addRecurrenceRule(EKRecurrenceRule(recurrenceWith: f, interval: spec.interval, daysOfTheWeek: days, daysOfTheMonth: nil, monthsOfTheYear: nil,
                                                     weeksOfTheYear: nil, daysOfTheYear: nil, setPositions: nil, end: nil))
            }
            switch TodoBoard.alarmPlan(had: had, want: e.alarm, dueChanged: dueChanged) {
            case .removeAll:
                for a in r.alarms ?? [] { r.removeAlarm(a) }
            case .addAtDue:
                if let at = date(e.due) { r.addAlarm(EKAlarm(absoluteDate: at)) }
            case .shiftAbsolute:
                if let a = date(oldDue), let b = date(e.due) {
                    for alarm in r.alarms ?? [] where alarm.absoluteDate != nil { alarm.absoluteDate = alarm.absoluteDate?.addingTimeInterval(b.timeIntervalSince(a)) }
                }
            case .keep:
                break
            }
            try store.save(r, commit: true)
            answer(true, "")
        } catch {
            store.reset()
            answer(false, error.localizedDescription)
        }
    }

    // L9, L12, L13: the reminder must still be the one the touch screen shows, and unfinished; then change it, and say so.
    private func perform(_ act: TodoAct) {
        guard let link else { return }
        func answer(_ ok: Bool, _ detail: String) {
            link.sendResult(TodoBoard.resultMessage(req: act.req, ok: ok, detail: detail))
            tick()
        }
        guard enabled, allowed else { answer(false, "Mac 上的 DeskHub 现在不能改提醒事项"); return }
        guard let key = shown[act.id], let r = store.calendarItem(withIdentifier: key) as? EKReminder, !r.isCompleted else {
            answer(false, "这条待办已经变了")
            return
        }
        do {
            switch act.action {
            case .done:
                r.isCompleted = true
                try store.save(r, commit: true)
            case .tomorrow:
                let next = TodoBoard.postponed(due: r.dueDateComponents, now: Date(), calendar: .current)
                if let shift = TodoBoard.alarmShift(from: r.dueDateComponents, to: next, calendar: .current) {
                    for alarm in r.alarms ?? [] where alarm.absoluteDate != nil { alarm.absoluteDate = alarm.absoluteDate?.addingTimeInterval(shift) }
                }
                r.dueDateComponents = next
                try store.save(r, commit: true)
            case .delete:
                try store.remove(r, commit: true)
            }
            answer(true, "")
        } catch {
            store.reset()   // as in apply(): the reminder changed in memory must not stay half changed after a failed save
            answer(false, error.localizedDescription)
        }
    }
}
