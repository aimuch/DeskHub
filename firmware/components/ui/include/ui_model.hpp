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
struct Appearance {
    ThemeId theme = ThemeId::Mint;
    uint32_t accent = 0;  // 0 = the theme's own
    Lang lang = Lang::ZhCN;
    SizeTier size = SizeTier::Standard;
};
std::string serialize(const Appearance &);            // "mint|5ee6b8|zh-CN|standard"
Appearance parse_appearance(const std::string &);     // anything malformed is the default appearance

// ---- shared ----
enum class Tone { Ok, Info, Warn, Danger, Off };
constexpr int64_t kPowerStaleMs = 6000;
bool power_fresh(const PowerState &, int64_t now_ms);  // a sample older than 6 s, from the future, or never taken is stale
bool metrics_fresh(const Snapshot &, int64_t now_ms);

// ---- header: two lines on each side, on every page ----
struct HeaderView {
    std::string agent, wifi, battery, charge;
    Tone agent_tone = Tone::Off, wifi_tone = Tone::Off, battery_tone = Tone::Off, charge_tone = Tone::Off;
    int battery_fill = -1;  // 0..100 only for a fresh, present battery with a valid level; -1 draws nothing
};
HeaderView make_header(Lang, const Snapshot &, const PowerState &, int64_t now_ms);
// The four ring segments: a lit segment means that channel is healthy.
struct RingView { bool wifi = false, agent = false, power = false, time = false; };
RingView make_ring(const Snapshot &, const PowerState &, int64_t now_ms);

// ---- clock ----
struct ClockView { bool valid = false; std::string hhmm = "--:--", seconds, date; };
ClockView make_clock(Lang, bool time_valid, time_t utc_now);  // local time through the TZ the core set

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
std::vector<ConnectRow> make_connect(Lang, const Snapshot &, bool paired);

// ---- actions and the transient command notice ----
struct ActionView { std::string label, sub; bool enabled = false; };
std::vector<ActionView> make_actions(Lang, const Snapshot &);  // at most 12; an empty label shows the action id
constexpr int64_t kNoticeMs = 15000;
// The last command result is a notice for 15 s, not standing state. "" means nothing to show.
std::string notice_text(Lang, const std::string &command, int64_t now_ms, int64_t changed_ms);
}  // namespace dh::ui
