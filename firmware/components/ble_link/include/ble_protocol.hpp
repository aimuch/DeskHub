#pragma once
// The Bluetooth link between the touch screen and the DeskHub App (docs/desktop-app-design.md 13.3 and 13.4), the part
// without NimBLE: message parsing with limits, the list of input sources, one request at a time, the pairing window and
// the words of a result. Tested on the host (firmware/tests/ble_protocol_test.cpp).
//
// Messages are short JSON objects, one per GATT write or notification (at most 512 bytes):
//   App -> device (control):  {"op":"sources_begin","count":3}
//                             {"op":"source","i":0,"id":"<App id>","name":"MacBook M1","port":"hdmi1"}
//                             {"op":"sources_end"}
//                             {"op":"result","req":7,"outcome":"confirmed|sent_unconfirmed|not_sent","detail":"..."}
//                             {"op":"current","id":"<App id>"}   the input the monitor is on; "" when not known
//                             {"op":"usage","ok":true,...} / {"op":"usage","ok":false}   Magpie, 13.12
//                             {"op":"todos_begin","n":3,"total":9} {"op":"todo",...} {"op":"todos_end"}   to-dos, 13.13
//                             {"op":"todos_off","why":"access|setting"} {"op":"todo_result","req":5,"ok":true,"detail":""}
//   device -> App (events):   {"op":"switch","req":7,"id":"<App id>"}
//                             {"op":"todo_act","req":5,"id":"9f2c4e1a0b7d","a":"done|tomorrow|delete"}
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dh::ble {
constexpr int kProtocol = 1;
constexpr size_t kMaxMessage = 512, kMaxSources = 12, kMaxId = 48, kMaxName = 32;
constexpr int64_t kPairingWindowMs = 120000;   // "pair a computer" accepts a new computer for 2 minutes (K1)
constexpr int64_t kConfirmMs = 30000;          // the six digits must be confirmed within 30 s (the Mac gives up then too)
constexpr int64_t kRequestTimeoutMs = 12000;   // the App needs about 4.5 s for a switch and its read-backs
constexpr int64_t kUsageStaleMs = 60000;       // the App sends the usage every 15 s (13.12 G3)
constexpr size_t kMaxTodos = 30, kMaxTodoTotal = 9999, kTodoIdLen = 12, kMaxTodoTitle = 96, kMaxTodoList = 32, kMaxTodoDetail = 64;
constexpr int64_t kTodoStaleMs = 120000;       // the App sends the list every 30 s (13.13 L4)

struct Source {
    std::string id, name, port;   // port: dp1 dp2 hdmi1 hdmi2 usbc other
    bool operator==(const Source &o) const { return id == o.id && name == o.name && port == o.port; }
};
enum class Outcome { Confirmed, SentUnconfirmed, NotSent };
struct Result {
    uint32_t req = 0;
    Outcome outcome = Outcome::NotSent;
    std::string detail;
};

// The Usage page (docs/desktop-app-design.md 13.12): what the App read from Magpie on the Mac.
struct UsageWindow {
    uint32_t minutes = 0;     // the length of the quota window
    uint32_t remaining = 0;   // percent left, 0 to 100
    uint32_t reset_s = 0;     // seconds to its reset when the App sent it; 0: not known
    bool operator==(const UsageWindow &o) const { return minutes == o.minutes && remaining == o.remaining && reset_s == o.reset_s; }
};
struct Usage {
    bool ok = false;                  // false: Magpie is not running on the Mac
    std::string provider, plan;
    std::vector<UsageWindow> windows; // at most 2
    uint32_t calls = 0, errors = 0;   // today
    uint64_t tokens = 0;              // today
    uint32_t ttft_ms = 0;             // today's average time to first token
    std::string latest_model;
    uint32_t latest_status = 0, latest_ms = 0, latest_age_s = 0;
};

// The to-do board (docs/desktop-app-design.md 13.13): the unfinished reminders the App read on the Mac.
enum class TodoKind : uint8_t { Overdue, Today, Tomorrow, Later, None };
enum class TodoAction : uint8_t { Done, Tomorrow, Delete, Edit };
struct Todo {
    std::string id;            // 12 lowercase hex digits: the App's handle, not the reminder's own id
    std::string title, list;
    TodoKind kind = TodoKind::None;
    uint16_t year = 0;            // the due date (13.14); 0 with None
    uint8_t month = 0, day = 0;
    int16_t minute = -1;          // the due time of day; -1: all day or none
    std::string repeat;           // "" none, "wd" weekdays, "x" custom (shown, not chosen), d/w/m/y + 1..99
    std::string list_id;          // 12 lowercase hex digits
    bool alarm = false;           // a ring at the due time; only with a time
    std::string version;          // 8 lowercase hex digits: changes whenever the reminder changes on the Mac (M3)
    bool operator==(const Todo &o) const {
        return id == o.id && title == o.title && list == o.list && kind == o.kind && year == o.year && month == o.month && day == o.day &&
               minute == o.minute && repeat == o.repeat && list_id == o.list_id && alarm == o.alarm && version == o.version;
    }
};
struct TodoListName { std::string id, name; };   // a list of Reminders the person can move a to-do into (13.14)
// What the edit page sends (13.14): the to-do after the change; unchanged parts as they were.
struct TodoEdit {
    std::string id, version;
    uint16_t year = 0;
    uint8_t month = 0, day = 0;
    int16_t minute = -1;
    std::string repeat, list_id;
    bool alarm = false;
};
// Header-only, so the UI and its tests use them without the parser: 0 for a month out of range.
inline int days_in_month(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : days[month - 1];
}
inline bool lower_hex(const std::string &s, size_t len) {
    if (s.size() != len) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
// "" none, "wd" weekdays, "x" custom, or d/w/m/y with 1..99 and no leading zero.
inline bool valid_repeat(const std::string &r) {
    if (r.empty() || r == "wd" || r == "x") return true;
    if (r.size() < 2 || r.size() > 3 || std::string("dwmy").find(r[0]) == std::string::npos || r[1] < '1' || r[1] > '9') return false;
    return r.size() == 2 || (r[2] >= '0' && r[2] <= '9');
}
// A real date or none, a repeat the protocol knows ("x": unchanged), a ring only with a time.
inline bool valid_edit(const TodoEdit &e) {
    if (!lower_hex(e.id, 12) || !lower_hex(e.version, 8) || !lower_hex(e.list_id, 12) || !valid_repeat(e.repeat)) return false;
    const bool undated = e.year == 0 && e.month == 0 && e.day == 0 && e.minute == -1;
    if (!undated && (e.year < 2000 || e.year > 2100 || e.day < 1 || e.day > days_in_month(e.year, e.month) || e.minute < -1 || e.minute > 1439)) return false;
    return !(e.alarm && e.minute < 0);
}
std::string todo_edit_json(uint32_t req, const TodoEdit &);
struct TodoList {
    std::vector<Todo> items;   // at most 30, in the App's order
    size_t total = 0;          // all the unfinished ones on the Mac
};
struct TodoResult {
    uint32_t req = 0;
    bool ok = false;
    std::string detail;
};

enum class Op { SourcesBegin, Source, SourcesEnd, Result, Current, Usage, TodosBegin, Todo, TodosEnd, TodosOff, TodoResult, TodoListsBegin, TodoListItem, TodoListsEnd, Bad };
struct Message {
    Op op = Op::Bad;
    size_t count = 0;        // SourcesBegin, TodosBegin
    size_t total = 0;        // TodosBegin
    size_t index = 0;        // Source, Todo
    Source source;           // Source
    Result result;           // Result
    std::string current;     // Current: an id, or "" for "not known"
    Usage usage;             // Usage
    Todo todo;               // Todo
    std::string off_why;     // TodosOff: "access" or "setting"
    TodoResult todo_result;  // TodoResult
    TodoListName list_entry; // TodoListItem
    std::string error;       // Bad: why
};
// Strict: a known op, the fields it needs with the right types, nothing too long, valid UTF-8 without control characters.
Message parse(const std::string &bytes);
// The same, into a freshly made `out`. No Message on the stack: the NimBLE task has 4 KB (13.14).
void parse_into(const std::string &bytes, Message &out);

// The list arrives in pieces. It replaces the shown list only when it is complete: every index once, the count as
// announced. Anything out of order throws the draft away (the shown list stays as it was).
struct SourcesDraft {
    bool open = false;
    size_t expected = 0;
    std::vector<std::optional<Source>> items;
};
enum class Apply { Pending, Complete, Rejected };
Apply apply(SourcesDraft &, const Message &, std::vector<Source> *complete);

// The to-do list arrives in pieces too (13.13 L2): the same rules, plus unique ids. Other ops are ignored here.
struct TodosDraft {
    bool open = false;
    size_t expected = 0, total = 0;
    std::vector<std::optional<Todo>> items;
};
Apply apply(TodosDraft &, const Message &, TodoList *complete);

// The lists of Reminders arrive in pieces too (13.14 M2). Other ops are ignored here.
constexpr size_t kMaxTodoLists = 16;
struct ListsDraft {
    bool open = false;
    size_t expected = 0;
    std::vector<std::optional<TodoListName>> items;
};
Apply apply(ListsDraft &, const Message &, std::vector<TodoListName> *complete);

// One to-do action at a time, only for an id in the shown list (13.13 L10, L11).
class TodoActs {
public:
    struct Pending { uint32_t req; std::string id; TodoAction action; int64_t sent_ms; };
    bool begin(const std::string &id, TodoAction, const std::vector<Todo> &shown, int64_t now_ms, uint32_t *req);
    std::optional<Pending> finish(const TodoResult &);   // the action the result is for; nullopt when it is not the waiting one
    bool expire(int64_t now_ms);                          // true once, when the waiting action timed out
    bool waiting() const { return pending_.has_value(); }
    const std::optional<Pending> &pending() const { return pending_; }
    void reset() { pending_.reset(); }
private:
    uint32_t next_ = 1;
    std::optional<Pending> pending_;
};
std::string todo_act_json(uint32_t req, const std::string &id, TodoAction);

// One switch request at a time. A result counts only for the request that is waiting; a late or unknown one is ignored.
// A request that got no result in kRequestTimeoutMs is over, and says so.
class Requests {
public:
    // false: one is still waiting, or the id is not in `sources` (the device's own white list, K2).
    bool begin(const std::string &id, const std::vector<Source> &sources, int64_t now_ms, uint32_t *req);
    std::optional<std::string> finish(const Result &);   // the id the result is for; nullopt when it is not the waiting one
    bool expire(int64_t now_ms);                          // true once, when the waiting request timed out
    bool waiting() const { return pending_.has_value(); }
    void reset() { pending_.reset(); }   // the link went down
private:
    struct Pending { uint32_t req; std::string id; int64_t sent_ms; };
    uint32_t next_ = 1;
    std::optional<Pending> pending_;
};

// K1: a new computer is accepted only while the window is open, and only after the person confirmed the digits.
class PairingWindow {
public:
    void open(int64_t now_ms) { until_ = now_ms + kPairingWindowMs; }
    void close() { until_ = 0; }
    bool is_open(int64_t now_ms) const { return until_ != 0 && now_ms < until_; }
    int64_t left_ms(int64_t now_ms) const { return is_open(now_ms) ? until_ - now_ms : 0; }
private:
    int64_t until_ = 0;
};
bool accept_pairing(const PairingWindow &, int64_t now_ms, int64_t digits_shown_ms, bool person_said_same);

// The current input as the device shows it: `id` when it is in the list, else "" (not known).
std::string current_in(const std::vector<Source> &, const std::string &id);
std::string request_json(uint32_t req, const std::string &id);
// update_key: fingerprint, "" when none. boot_pending: the running firmware has not passed its self-test yet (13.11 U16).
std::string info_json(const std::string &device_id, const std::string &firmware, const std::string &update_key, bool boot_pending);
// What "boot" says: the self-test hook once main has set it, else the bootloader's state read at start (13.11 U16:
// the App can read the info before main sets the hook).
inline bool boot_pending_now(bool (*hook)(), bool pending_at_start) { return hook ? hook() : pending_at_start; }
// The line the device shows for a result: "已切到 MacBook M1，已确认" / "已发送到 …，未确认：…" / "没有切到 …：…".
std::string result_text(const Result &, const std::string &name);
}  // namespace dh::ble
