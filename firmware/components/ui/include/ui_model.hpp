#pragma once
// View models of the device UI: everything the screen says, computed from the snapshot, the power state and the
// config, with no LVGL. The rules in docs/rules.md (K3: unknown stays unknown, stale stays stale) live here, so
// they can be tested exhaustively on the host. The LVGL layer only draws what these structures say.
#include "ble_protocol.hpp"
#include "board.hpp"
#include "charge_policy.hpp"
#include "core.hpp"
#include "ui_text.hpp"
#include "ui_theme.hpp"
#include <array>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace dh::ui {
// ---- appearance: what a user can change on the device (D12) ----
enum class SizeTier { Small, Standard, Large };
struct FontPx { int title, body, caption; };  // each one of the compiled-in 16, 20, 28 px fonts
FontPx font_px(SizeTier);
// How thick the rings are: the status ring on the Home page and the gauge on the power page.
enum class RingTier { Thin, Standard, Thick };
struct RingPx { int home, gauge; };
RingPx ring_px(RingTier);  // Thin 12/7, Standard 20/11 (what it was before the tiers), Thick 28/15
// ---- pages (docs/design v2 Pages): which pages are in the navigation, in which order; the first shown one is the home page ----
enum class View : uint8_t { Switch, Overview, Clock, Power, Usage, Todo };
constexpr size_t kMaxShownPages = 4;   // the navigation holds 4 pages and Settings (13.12 G7)
constexpr size_t kViewCount = 6;
struct PageSlot { View view; bool shown; };
using PageOrder = std::array<PageSlot, kViewCount>;
PageOrder default_pages();                          // Switch and Overview shown, Clock and Power hidden
View home_view(const PageOrder &);                  // the first shown page
std::vector<View> nav_views(const PageOrder &);     // the shown pages, in order (Settings comes after them, always)
bool pages_toggle(PageOrder &, size_t slot);        // false: out of range, or it is the last shown page
bool pages_move(PageOrder &, size_t slot, int delta);   // delta -1 or +1; false at the ends or out of range
std::string serialize_pages(const PageOrder &);     // "S+O+C-P-U-T-"
bool parse_pages(const std::string &, PageOrder &); // false (and `out` untouched) unless every page is there once and one is shown
// How the switch page shows the input sources.
enum class SwitchStyle : uint8_t { List, Icons };

struct Appearance {
    ThemeId theme = ThemeId::Night;
    uint32_t accent = 0;  // 0 = the theme's own
    Lang lang = Lang::ZhCN;
    SizeTier size = SizeTier::Standard;
    bool show_percent = true;  // the digits inside the battery icon of the header
    RingTier ring = RingTier::Standard;
    PageOrder pages = default_pages();
    SwitchStyle style = SwitchStyle::List;
    bool clock_hands = true;   // the clock page draws hands on its dial, with small digits under them
};
std::string serialize(const Appearance &);            // "night|-|zh-CN|standard|pct|standard|S+O+C-P-|list|hands"
// What 0.3.x stored (4 or 6 fields) is still understood: the theme becomes night and keeps its own colour as the accent,
// unless an accent was chosen. 0.3.17 stored 8 fields (no hands field: the hands are on). Anything else that is malformed
// is the default appearance.
Appearance parse_appearance(const std::string &);

// ---- input sources: the App's switch actions, with a name and an icon the person may set on the device ----
enum class SourceIcon : uint8_t { Auto, Hdmi, Dp, UsbC, Laptop, Desktop, Monitor, Console, Tv };
extern const std::array<SourceIcon, 8> kIconChoices;   // what the edit page offers (Auto is "restore")
const char *icon_key(SourceIcon);                       // "auto", "hdmi", ... as stored
SourceIcon port_icon(const std::string &input_id);      // read from the words of the id; Monitor when no port is named
std::string port_tag(const std::string &input_id);      // "HDMI", "DisplayPort", "USB-C"; "" when no port is named
struct SourceLocal { std::string id, name; SourceIcon icon = SourceIcon::Auto; };
using SourceNames = std::vector<SourceLocal>;
constexpr size_t kMaxSourceNames = 12, kMaxSourceName = 32, kMaxSourceId = 48;
bool valid_source_name(const std::string &);            // 1 to 32 bytes of valid UTF-8 without control characters
// "" for the name, or Auto for the icon, gives the App's back. false: a bad id or name, or a 13th source.
bool set_source_name(SourceNames &, const std::string &id, const std::string &name);
bool set_source_icon(SourceNames &, const std::string &id, SourceIcon);
void reset_source(SourceNames &, const std::string &id);
std::string serialize_sources(const SourceNames &);
SourceNames parse_sources(const std::string &);         // a damaged text is nothing at all
struct SourceView {
    std::string id, name, tag, sub;
    SourceIcon icon = SourceIcon::Monitor;   // never Auto: what is drawn
    bool enabled = false, renamed = false;
};
std::vector<SourceView> make_sources(Lang, const Snapshot &, const SourceNames &);   // at most 12, in the order of the snapshot

// ---- shared ----
enum class Tone { Ok, Info, Warn, Danger, Off };
constexpr int64_t kPowerStaleMs = 6000;
bool power_fresh(const PowerState &, int64_t now_ms);  // a sample older than 6 s, from the future, or never taken is stale

// ---- header: icons only (the words are on the Connect and Power pages) ----
enum class LinkIcon { Ok, Busy, Off };  // Off is drawn with a slash; Busy blinks
struct BatteryView {
    enum class Kind { Level, Unknown, None, Fault };
    Kind kind = Kind::Unknown;
    int percent = -1;      // 0..100 only for Level; the icon is filled to this. -1 draws no fill
    std::string digits;    // inside the icon: the level without a percent sign, "?" when unknown, "!" on a fault, "" otherwise
    Tone tone = Tone::Off;
    bool charging = false; // the bolt: only while the chip says "charging" and the sample can be trusted
};
struct HeaderView {
    LinkIcon wifi = LinkIcon::Off, agent = LinkIcon::Off;
    Tone wifi_tone = Tone::Off, agent_tone = Tone::Off;
    bool usb = false;      // USB power is there, in a fresh sample without a fault
    BatteryView battery;
};
HeaderView make_header(const Snapshot &, const PowerState &, int64_t now_ms, bool show_percent);
// The icons that are connecting blink: on for the first half of each second.
bool blink_on(int64_t now_ms);
// The four ring segments: a lit segment means that channel is healthy.
struct RingView { bool wifi = false, agent = false, power = false, time = false; };
RingView make_ring(const Snapshot &, const PowerState &, int64_t now_ms);
// The ring explained (docs/design Dev-RingInfo). One row per segment in the order of the ring: Wi-Fi, computer, power, time.
// A row is dark exactly when make_ring says the segment is dark. The reasons are what the device knows; none is invented.
struct RingRow { std::string name, state, note; bool ok = false; };
struct RingInfoView { std::array<RingRow, 4> row; int first_problem = -1; bool power_fault = false; };
RingInfoView make_ring_info(Lang, const Snapshot &, const PowerState &, int64_t now_ms);
// The line under the ring: the first dark segment by name, else where to tap.
std::string ring_note(Lang, const RingInfoView &);
// The quiet hours of the sound: the night limit is on, and the time is inside the window of the night or not known (as for the display).
bool quiet_hours(const DisplayConfig &, bool time_valid, uint16_t minute_of_day);

// ---- clock ----
struct ClockView { bool valid = false; std::string hhmm = "--:--", seconds, date; };
ClockView make_clock(Lang, bool time_valid, time_t utc_now);  // local time through the TZ the core set
std::string tz_label(int minutes);   // "UTC+8", "UTC+5:45", "UTC-3:30", "UTC"
// The numbers of "set the time" on the Date and time page. Field: 0 year, 1 month, 2 day, 3 hour, 4 minute.
struct TimeDraft { int year = 2026, month = 1, day = 1, hour = 0, minute = 0; };
TimeDraft time_draft(bool time_valid, time_t utc_now, int tz_minutes);    // the local time now; a start when the time is not known
void time_draft_step(TimeDraft &, int field, int delta);                  // wraps, except the year (2024 to 2099); the day stays in its month
std::string time_draft_utc(const TimeDraft &, int tz_minutes);            // "2026-11-05T06:32:00Z"; empty for a time that does not exist or that the core refuses

// ---- the Usage page (docs/design v2 Usage, UsageStale; docs/desktop-app-design.md 13.12) ----
std::string short_count(uint64_t);   // 128, 1.2K, 12.3K, 1.2M, 1.2B
struct QuotaCard {
    std::string label;   // "5 小时", "7 天"
    int remaining = 0;   // percent
    std::string reset;   // "4 小时 12 分后重置", "周三 09:40 重置", "重置时间未知"
};
struct UsageView {
    enum class State { Waiting, Offline, Fresh, Stale } state = State::Waiting;
    std::string title;           // "Codex · plus"
    std::string status;          // "15 秒前更新", "数据过期 · 3 分钟前", "Magpie 没有运行", "等 App 发来用量"
    bool dim = false;            // stale: the numbers are drawn dim
    std::vector<QuotaCard> quotas;
    std::string calls, tokens, success, ttft;     // "—" when there is nothing to say
    std::string model, code, latency, ago;        // the latest request
    bool failed = false;                          // its status is not 2xx or 3xx
};
// u: the last usage the App sent, or nullptr when none came yet; age_ms: how long ago it came.
UsageView make_usage(Lang, const ble::Usage *u, int64_t age_ms, bool time_valid, time_t now);

// ---- the to-do board (docs/design v2 Todo, TodoMenu, TodoDelete, TodoStale; docs/desktop-app-design.md 13.13) ----
// L8: each character `has` says the font lacks is drawn as "?"; variation selectors and joiners are dropped.
std::string displayable(const std::string &utf8, bool (*has)(uint32_t codepoint));
struct TodoRow {
    std::string id, title;
    std::string when;     // on the right: "11月10日", "14:30", "今天", "明天", "" (no date)
    std::string detail;   // under the title of the selected row: "工作 · 今天 14:30"
    ble::TodoKind kind = ble::TodoKind::None;
    bool tag = false;     // today's: drawn as a pill in the accent colour
};
struct TodoGroup { std::string title; ble::TodoKind kind = ble::TodoKind::None; std::vector<TodoRow> rows; };   // Later holds tomorrow too
struct TodoView {
    enum class State { Waiting, Off, Empty, Fresh, Stale } state = State::Waiting;
    std::string count;    // "未完成 9 条"
    std::string status;   // "刚刚更新", "45 秒前更新", "数据过期 · 3 分钟前"; the message of Waiting, Off and Empty
    std::string hint;     // the second line of Waiting and Off; the banner of Stale
    std::vector<TodoGroup> groups;
    std::string more;     // "还有 3 条", or ""
    bool dim = false;
    bool can_act = false; // long press opens the actions: fresh, and the link is secure
};
// list: the last complete list, or nullptr when none came; off_why: "access" or "setting" after todos_off, else "".
TodoView make_todos(Lang, const ble::TodoList *list, const std::string &off_why, int64_t age_ms, bool link_secure);
std::string todo_result_text(Lang, ble::TodoAction, bool ok, const std::string &title, const std::string &detail);
std::string todo_timeout_text(Lang);
// ---- changing a to-do (docs/design v2 TodoEdit, TodoEditWhen, TodoEditRepeat, TodoEditList, TodoRing; 13.14) ----
std::string repeat_text(Lang, const std::string &repeat);   // "每周", "每 3 天", "自定义"
struct MonthChoice { int year = 0, month = 0; std::string label; };
// The month wheel: 13 months from this month, from the to-do's own month when that is earlier or the time is not known.
std::vector<MonthChoice> month_choices(Lang, bool time_valid, int now_year, int now_month, int item_year, int item_month);
struct TodoDraft {
    enum class When { Timed, AllDay, None } when = When::None;
    int year = 0, month = 0, day = 0, minute = 540;   // kept when "no date" is chosen, so the wheels come back where they were
    std::string repeat, list_id;
    bool alarm = false;
};
TodoDraft todo_draft(const ble::Todo &, bool time_valid, int now_year, int now_month, int now_day);
ble::TodoEdit todo_edit_of(const ble::Todo &, const TodoDraft &);   // the day kept inside its month; no ring without a time
std::string draft_when_text(Lang, const TodoDraft &, int now_year);  // "11月12日 14:30", "2027年1月3日 全天", "没有日期"
// ---- after the design review (13.15) ----
int weekday_of(int year, int month, int day);                   // 0 Sunday .. 6 Saturday
std::string draft_when_head(Lang, const TodoDraft &);           // "到期 · 周四", "Due | Thu"; "到期" without a date
std::string todo_ring_line(Lang, const ble::Todo &);            // "工作 · 每周": the list and the repeat, each when there is one
class TodoRinger {
public:
    static constexpr int64_t kSnoozeMs = 600000;
    // The to-do to ring now, or nothing: a ring, a time, today and this minute, not rung yet; the first in the list's order.
    std::optional<ble::Todo> due(const std::vector<ble::Todo> &, bool time_valid, int year, int month, int day, int minute_of_day, int64_t now_ms);
    void snooze(const ble::Todo &, int64_t now_ms);   // rings once more, 10 minutes later
    void reset() { rung_.clear(); snoozed_.clear(); }
private:
    std::vector<std::string> rung_;
    std::vector<std::pair<ble::Todo, int64_t>> snoozed_;
};

// ---- gauges ----
struct Gauge { bool valid = false; float percent = 0; std::string text = "--"; };
Gauge parse_percent(const std::string &raw, bool fresh);  // "23.4%" is valid; "--", 150%, -1% and text are not

// ---- power ----
struct PowerView {
    Gauge level;
    std::string level_note, charge, battery_v, usb_v, chip_temp, battery_v_note, usb_v_note, temp_note;
    Tone charge_tone = Tone::Off;
};
PowerView make_power(Lang, const PowerState &, int64_t now_ms);

// ---- charge protection (N1, experimental) ----
// Everything the power page says about it. It reports the controller's state as it is: "stopped" and "restored" are never
// claimed when the switch is not known, and an unknown reason of a failure says nothing.
struct ChargeView {
    std::string strip;       // the line under the level gauge
    std::string headline;    // the first line of the Protect tab
    std::string note;        // the second line; empty when there is nothing to add
    Tone tone = Tone::Off;
    std::string button;      // "Turn on..." while off, "Turn off" while on
    bool button_enabled = false;
    std::string why_disabled;  // set when "Turn on..." cannot work
};
ChargeView make_charge(Lang, const charge::Status &, const PowerState &, int64_t now_ms);

// ---- connection ----
struct ConnectRow { std::string key, value; Tone tone = Tone::Off; };
// `paired` comes from the stored credentials (the snapshot does not carry it).
std::vector<ConnectRow> make_connect(Lang, const Snapshot &, bool paired, const std::string &ssid = {});   // the Wi-Fi row names `ssid`

// ---- actions and the transient command notice ----
struct ActionView { std::string label, sub; bool enabled = false; };
std::vector<ActionView> make_actions(Lang, const Snapshot &);  // at most 12; an empty label shows the action id
constexpr int64_t kNoticeMs = 15000;
// The last command result is a notice for 15 s, not standing state. "" means nothing to show.
std::string notice_text(Lang, const std::string &command, int64_t now_ms, int64_t changed_ms);
}  // namespace dh::ui
