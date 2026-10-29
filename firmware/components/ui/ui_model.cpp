#include "ui_model.hpp"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace dh::ui {
namespace {
constexpr time_t kClockEpoch = 1577836800;  // 2020-01-01: a clock earlier than this was never set
constexpr int kBatteryMinMv = 2500, kBatteryMaxMv = 4600, kUsbMinMv = 3000, kUsbMaxMv = 6500;
constexpr float kChipMinC = -40.0f, kChipMaxC = 150.0f;

__attribute__((format(printf, 1, 2))) std::string fmt(const char *format, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    return buf;
}
std::string T(Lang l, Str s) { return tr(l, s); }
bool contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }
bool level_valid(int soc) { return soc >= 0 && soc <= 100; }
bool rssi_valid(int rssi) { return rssi < 0 && rssi >= -127; }

enum class WifiKind { Connected, Connecting, NotConfigured, Failed, Down };
WifiKind wifi_kind(const Snapshot &s) {
    if (s.wifi_ready) return WifiKind::Connected;
    if (contains(s.wifi, "未配置")) return WifiKind::NotConfigured;
    if (contains(s.wifi, "失败") || contains(s.wifi, "错误")) return WifiKind::Failed;
    if (contains(s.wifi, "正在") || contains(s.wifi, "连接中")) return WifiKind::Connecting;
    return WifiKind::Down;
}
std::string wifi_text(Lang l, WifiKind k) {
    switch (k) {
    case WifiKind::Connected: return T(l, Str::ValueConnected);
    case WifiKind::Connecting: return T(l, Str::WifiConnecting);
    case WifiKind::NotConfigured: return T(l, Str::WifiNotConfigured);
    case WifiKind::Failed: return T(l, Str::WifiFailed);
    case WifiKind::Down: break;
    }
    return T(l, Str::ValueNotConnected);
}
Tone wifi_tone(WifiKind k) {
    switch (k) {
    case WifiKind::Connected: return Tone::Ok;
    case WifiKind::Connecting: return Tone::Info;
    case WifiKind::Failed: return Tone::Warn;
    default: return Tone::Off;
    }
}
std::string charge_text(Lang l, const std::string &charging) {
    if (charging == "charging") return T(l, Str::ChargeCharging);
    if (charging == "full") return T(l, Str::ChargeFull);
    if (charging == "idle") return T(l, Str::ChargeIdle);
    return T(l, Str::Unknown);
}
Tone charge_tone(const std::string &charging) {
    return charging == "charging" ? Tone::Info : charging == "full" ? Tone::Ok : Tone::Off;
}
Tone level_tone(int soc) { return soc > 20 ? Tone::Ok : soc > 10 ? Tone::Warn : Tone::Danger; }
std::string volts(int mv) {
    const int centi = (mv + 5) / 10;  // round half up
    return fmt("%d.%02d V", centi / 100, centi % 100);
}
}  // namespace

// ---- appearance ----
FontPx font_px(SizeTier t) {
    switch (t) {
    case SizeTier::Small: return {20, 16, 16};
    case SizeTier::Large: return {28, 28, 20};
    case SizeTier::Standard: break;
    }
    return {28, 20, 16};
}
namespace {
const char *size_name(SizeTier t) { return t == SizeTier::Small ? "small" : t == SizeTier::Large ? "large" : "standard"; }
bool hex6(const std::string &s) {
    if (s.size() != 6) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
}  // namespace
std::string serialize(const Appearance &a) {
    return std::string(theme_name(a.theme)) + "|" + (a.accent ? fmt("%06x", static_cast<unsigned>(a.accent & 0xffffff)) : std::string("-")) + "|" +
           lang_name(a.lang) + "|" + size_name(a.size);
}
Appearance parse_appearance(const std::string &text) {
    Appearance out;
    if (text.empty() || text.size() > 64) return out;
    std::vector<std::string> field;
    size_t from = 0;
    for (;;) {
        const auto bar = text.find('|', from);
        field.push_back(text.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
        if (bar == std::string::npos) break;
        from = bar + 1;
    }
    if (field.size() != 4) return Appearance{};
    Appearance a;
    if (field[0] == "mint") a.theme = ThemeId::Mint;
    else if (field[0] == "ocean") a.theme = ThemeId::Ocean;
    else if (field[0] == "amber") a.theme = ThemeId::Amber;
    else return Appearance{};
    if (field[1] == "-") a.accent = 0;
    else if (hex6(field[1])) a.accent = static_cast<uint32_t>(strtoul(field[1].c_str(), nullptr, 16));
    else return Appearance{};
    if (field[2] == "zh-CN") a.lang = Lang::ZhCN;
    else if (field[2] == "en-US") a.lang = Lang::EnUS;
    else return Appearance{};
    if (field[3] == "small") a.size = SizeTier::Small;
    else if (field[3] == "standard") a.size = SizeTier::Standard;
    else if (field[3] == "large") a.size = SizeTier::Large;
    else return Appearance{};
    return a;
}

// ---- freshness ----
bool power_fresh(const PowerState &p, int64_t now_ms) {
    return p.observed_ms > 0 && now_ms >= p.observed_ms && now_ms - p.observed_ms <= kPowerStaleMs;
}
bool metrics_fresh(const Snapshot &s, int64_t now_ms) { return s.sampled_ms != 0 && now_ms < s.metrics_expires_ms; }

// ---- header and ring ----
HeaderView make_header(Lang l, const Snapshot &s, const PowerState &p, int64_t now_ms) {
    HeaderView h;
    h.agent = T(l, s.agent_ready ? Str::AgentOnline : Str::AgentOffline);
    h.agent_tone = s.agent_ready ? Tone::Ok : Tone::Off;
    const WifiKind kind = wifi_kind(s);
    h.wifi = kind == WifiKind::Connected ? T(l, Str::WifiHeader) : wifi_text(l, kind);  // short: the header is narrow
    if (kind == WifiKind::Connected && rssi_valid(s.rssi)) h.wifi += T(l, Str::Sep) + fmt("%d dBm", s.rssi);
    h.wifi_tone = wifi_tone(kind);

    const bool fresh = power_fresh(p, now_ms);
    if (p.fault) {
        h.battery = T(l, Str::PowerFault);
        h.battery_tone = Tone::Warn;
        h.charge = T(l, Str::ChargeFault);
        h.charge_tone = Tone::Warn;
        return h;
    }
    if (!fresh) {
        h.battery = T(l, Str::BatteryUnknown);
        h.charge = T(l, Str::PowerPending);
        return h;
    }
    if (p.battery == "absent") h.battery = T(l, Str::BatteryNone);
    else if (p.battery == "present" && level_valid(p.soc_percent)) {
        h.battery = fmt(tr(l, Str::BatteryLevel), p.soc_percent);
        h.battery_fill = p.soc_percent;
        h.battery_tone = level_tone(p.soc_percent);
    } else h.battery = T(l, Str::BatteryUnknown);
    const Str usb = p.vbus == "present" ? Str::UsbPresent : p.vbus == "absent" ? Str::UsbAbsent : Str::Unknown;
    h.charge = T(l, usb) + T(l, Str::Sep) + charge_text(l, p.charging);
    h.charge_tone = charge_tone(p.charging);
    return h;
}
RingView make_ring(const Snapshot &s, const PowerState &p, int64_t now_ms) {
    RingView r;
    r.wifi = s.wifi_ready;
    r.agent = s.agent_ready;
    r.time = s.time_valid;
    r.power = power_fresh(p, now_ms) && !p.fault && (p.vbus == "present" || p.battery == "present");
    return r;
}

// ---- clock ----
ClockView make_clock(Lang l, bool time_valid, time_t utc_now) {
    ClockView c;
    if (!time_valid || utc_now < kClockEpoch) {
        c.date = T(l, Str::TimeUnset);
        return c;
    }
    struct tm t{};
    localtime_r(&utc_now, &t);
    c.valid = true;
    c.hhmm = fmt("%02d:%02d", t.tm_hour, t.tm_min);
    c.seconds = fmt("%02d", t.tm_sec);
    if (l == Lang::EnUS) c.date = fmt("%s, %s %d", weekday_name(l, t.tm_wday), month_name(l, t.tm_mon), t.tm_mday);
    else c.date = fmt("%d月%d日 星期%s", t.tm_mon + 1, t.tm_mday, weekday_name(l, t.tm_wday));
    return c;
}

// ---- computer ----
Gauge parse_percent(const std::string &raw, bool fresh) {
    Gauge g;
    if (!fresh) return g;
    // Strict grammar: digits ['.' digits] '%', nothing else.
    size_t i = 0;
    const size_t n = raw.size();
    auto digits = [&]() { size_t start = i; while (i < n && raw[i] >= '0' && raw[i] <= '9') ++i; return i > start; };
    if (!digits()) return g;
    if (i < n && raw[i] == '.') { ++i; if (!digits()) return g; }
    if (i + 1 != n || raw[i] != '%') return g;
    const float v = strtof(raw.c_str(), nullptr);
    if (!(v >= 0.0f && v <= 100.0f)) return g;
    g.valid = true;
    g.percent = v;
    g.text = fmt("%.0f%%", v);
    return g;
}
ComputerView make_computer(Lang l, const Snapshot &s, int64_t now_ms) {
    ComputerView c;
    const bool fresh = metrics_fresh(s, now_ms);
    c.cpu = parse_percent(s.cpu, fresh);
    c.memory = parse_percent(s.memory, fresh);
    auto rate = [&](const std::string &v) { return fresh && !v.empty() && v != "不可用" ? v : std::string("--"); };
    c.rx = rate(s.network);
    c.tx = rate(s.network_send);
    if (s.sampled_ms == 0) { c.footer = T(l, Str::MetricsWaiting); c.footer_tone = Tone::Off; }
    else if (!fresh) { c.footer = T(l, Str::MetricsExpired); c.footer_tone = Tone::Warn; }
    else { c.footer = T(l, Str::MetricsFresh); c.footer_tone = Tone::Ok; }
    return c;
}

// ---- power ----
PowerView make_power(Lang l, const PowerState &p, int64_t now_ms) {
    PowerView v;
    auto fill_notes = [&](const std::string &note) { v.battery_v_note = v.usb_v_note = v.temp_note = note; };
    if (p.fault) {
        v.level_note = T(l, Str::LevelFault);
        v.charge = T(l, Str::ChargeFault);
        v.charge_tone = Tone::Warn;
        fill_notes(T(l, Str::PowerFault));
        v.battery_v = v.usb_v = v.chip_temp = "--";
        return v;
    }
    if (!power_fresh(p, now_ms)) {
        v.level_note = T(l, Str::LevelStale);
        v.charge = T(l, Str::PowerPending);
        fill_notes(T(l, Str::PowerPending));
        v.battery_v = v.usb_v = v.chip_temp = "--";
        return v;
    }
    // level
    if (p.battery == "absent") v.level_note = T(l, Str::LevelNoBattery);
    else if (p.battery == "present" && level_valid(p.soc_percent)) {
        v.level.valid = true;
        v.level.percent = static_cast<float>(p.soc_percent);
        v.level.text = fmt("%d%%", p.soc_percent);
        v.level_note = T(l, Str::PmicEstimate);
    } else v.level_note = T(l, Str::LevelUnknown);
    v.charge = charge_text(l, p.charging);
    v.charge_tone = charge_tone(p.charging);
    // readings
    v.battery_v = v.usb_v = v.chip_temp = "--";
    const std::string &why = p.measure_reason;
    if (why == "ADC_ENABLE_FAILED") fill_notes(T(l, Str::ReadingEnableFailed));
    else if (why == "ADC_READ_FAILED") fill_notes(T(l, Str::ReadingReadFailed));
    else if (why == "ADC_OUT_OF_RANGE") fill_notes(T(l, Str::ReadingOutOfRange));
    else if (why != "ADC_OK") fill_notes(T(l, Str::PowerPending));
    else {
        if (p.battery == "absent") v.battery_v_note = T(l, Str::LevelNoBattery);
        else if (p.battery_mv >= kBatteryMinMv && p.battery_mv <= kBatteryMaxMv) v.battery_v = volts(p.battery_mv);
        else v.battery_v_note = T(l, Str::ReadingOutOfRange);
        if (p.vbus == "absent") v.usb_v_note = T(l, Str::UsbAbsent);
        else if (p.vbus_mv >= kUsbMinMv && p.vbus_mv <= kUsbMaxMv) v.usb_v = volts(p.vbus_mv);
        else v.usb_v_note = T(l, Str::ReadingOutOfRange);
        if (std::isfinite(p.chip_temp_c) && p.chip_temp_c >= kChipMinC && p.chip_temp_c <= kChipMaxC)
            v.chip_temp = fmt(tr(l, Str::TempUnit), static_cast<double>(p.chip_temp_c));
        else v.temp_note = T(l, Str::ReadingOutOfRange);
    }
    return v;
}

// ---- charge protection ----
ChargeView make_charge(Lang l, const charge::Status &s, const PowerState &p, int64_t now_ms) {
    using charge::State;
    ChargeView v;
    const std::string limit = fmt(" %d%%", s.limit);
    if (s.state == State::Off) {
        v.strip = v.headline = T(l, Str::ProtectionOff);
        v.button = T(l, Str::TurnOn) + (l == Lang::ZhCN ? "…" : "...");
        // Turning it on needs a battery, a fresh level and a charge switch that can be read.
        const bool fresh = !p.fault && power_fresh(p, now_ms);
        if (fresh && p.battery == "absent") v.why_disabled = T(l, Str::ProtectNoteNoBattery);
        else if (!fresh || p.battery != "present" || !level_valid(p.soc_percent)) v.why_disabled = T(l, Str::ProtectNoteUnreadable);
        else if (!s.charging_known) v.why_disabled = T(l, Str::ProtectNoteSwitch);
        else v.button_enabled = true;
        return v;
    }
    v.button = T(l, Str::TurnOff);
    v.button_enabled = true;
    switch (s.state) {
    case State::Charging:
    case State::Holding:
    case State::SafetyStop:
        v.strip = v.headline = T(l, Str::ProtectionOn) + limit;
        v.tone = s.state == State::SafetyStop ? Tone::Warn : Tone::Info;
        v.note = T(l, s.state == State::Charging ? Str::ProtectNoteCharging : s.state == State::Holding ? Str::ProtectNoteHolding : Str::ProtectNoteSafety);
        break;
    case State::Failsafe: {
        v.strip = v.headline = T(l, Str::ProtectionFailsafe);
        v.tone = Tone::Warn;
        const std::string why = s.reason;
        if (why == "SAMPLE_STALE" || why == "NO_SAMPLE" || why == "LEVEL_UNREADABLE" || why == "PMIC_FAULT") v.note = T(l, Str::ProtectNoteUnreadable);
        else if (why == "NO_BATTERY") v.note = T(l, Str::ProtectNoteNoBattery);
        else if (why == "STOP_FAILED" || why == "STOP_MISMATCH" || why == "REPEATED_FAILURE" || why == "RESTORED_AFTER_FAILURE") v.note = T(l, Str::ProtectNoteWrite);
        break;
    }
    case State::Unconfirmed:
        v.strip = v.headline = T(l, Str::ProtectionUnsure);
        v.tone = Tone::Danger;
        v.note = T(l, Str::ProtectNoteRetry);
        break;
    case State::Off: break;
    }
    return v;
}

// ---- connection, actions, notice ----
std::vector<ConnectRow> make_connect(Lang l, const Snapshot &s, bool paired) {
    const WifiKind kind = wifi_kind(s);
    const bool up = kind == WifiKind::Connected;
    std::vector<ConnectRow> rows;
    rows.push_back({T(l, Str::KeyWifi), wifi_text(l, kind), wifi_tone(kind)});
    rows.push_back({T(l, Str::KeySignal), up && rssi_valid(s.rssi) ? fmt("%d dBm", s.rssi) : std::string("--"), Tone::Off});
    rows.push_back({T(l, Str::KeyIp), up && !s.ip.empty() ? s.ip : std::string("--"), Tone::Off});
    rows.push_back({T(l, Str::KeyAgent), T(l, s.agent_ready ? Str::ValueOnline : Str::ValueOffline), s.agent_ready ? Tone::Ok : Tone::Off});
    rows.push_back({T(l, Str::KeyPairing), T(l, paired ? Str::ValuePaired : Str::ValueNotPaired), paired ? Tone::Ok : Tone::Off});
    rows.push_back({T(l, Str::KeyDeviceId), s.device_id.empty() ? std::string("--") : s.device_id, Tone::Off});
    return rows;
}
std::vector<ActionView> make_actions(Lang l, const Snapshot &s) {
    std::vector<ActionView> out;
    const size_t n = std::min<size_t>(s.action_count, s.actions.size());
    for (size_t i = 0; i < n; ++i) {
        const Action &a = s.actions[i];
        ActionView v;
        v.label = a.label.empty() ? a.id : dynamic_text(l, a.label);
        v.enabled = s.agent_ready;
        if (!v.enabled) v.sub = T(l, Str::ActionOffline);
        out.push_back(v);
    }
    return out;
}
std::string notice_text(Lang l, const std::string &command, int64_t now_ms, int64_t changed_ms) {
    if (command.empty() || command == "尚无操作") return "";
    if (now_ms < changed_ms || now_ms - changed_ms >= kNoticeMs) return "";
    return dynamic_text(l, command);
}
}  // namespace dh::ui
