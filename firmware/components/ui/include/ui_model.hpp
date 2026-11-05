#pragma once
// View models of the device UI: everything the screen says, computed from the snapshot, the power state and the
// config, with no LVGL. The rules in docs/rules.md (K3: unknown stays unknown, stale stays stale) live here, so
// they can be tested exhaustively on the host. The LVGL layer only draws what these structures say.
#include "board.hpp"
#include "charge_policy.hpp"
#include "core.hpp"
#include "ui_text.hpp"
#include "ui_theme.hpp"
#include <cstdint>
#include <ctime>
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
struct Appearance {
    ThemeId theme = ThemeId::Mint;
    uint32_t accent = 0;  // 0 = the theme's own
    Lang lang = Lang::ZhCN;
    SizeTier size = SizeTier::Standard;
    bool show_percent = true;  // the digits inside the battery icon of the header
    RingTier ring = RingTier::Standard;
};
std::string serialize(const Appearance &);            // "mint|5ee6b8|zh-CN|standard|pct|standard"
// The four fields that 0.3.x stored are still understood (the two new options are the defaults). Anything else that is
// malformed is the default appearance.
Appearance parse_appearance(const std::string &);

// ---- shared ----
enum class Tone { Ok, Info, Warn, Danger, Off };
constexpr int64_t kPowerStaleMs = 6000;
bool power_fresh(const PowerState &, int64_t now_ms);  // a sample older than 6 s, from the future, or never taken is stale
bool metrics_fresh(const Snapshot &, int64_t now_ms);

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

// ---- computer ----
struct Gauge { bool valid = false; float percent = 0; std::string text = "--"; };
Gauge parse_percent(const std::string &raw, bool fresh);  // "23.4%" is valid; "--", 150%, -1% and text are not
struct ComputerView { Gauge cpu, memory; std::string rx, tx, footer; Tone footer_tone = Tone::Off; };
ComputerView make_computer(Lang, const Snapshot &, int64_t now_ms);

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
