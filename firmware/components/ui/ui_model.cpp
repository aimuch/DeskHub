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
RingPx ring_px(RingTier t) {
    switch (t) {
    case RingTier::Thin: return {12, 7};
    case RingTier::Thick: return {28, 15};
    case RingTier::Standard: break;
    }
    return {20, 11};
}
// ---- pages ----
namespace {
char view_letter(View v) { return v == View::Switch ? 'S' : v == View::Overview ? 'O' : v == View::Clock ? 'C' : v == View::Power ? 'P' : 'U'; }
bool view_of(char c, View &v) {
    switch (c) {
    case 'S': v = View::Switch; return true;
    case 'O': v = View::Overview; return true;
    case 'C': v = View::Clock; return true;
    case 'P': v = View::Power; return true;
    case 'U': v = View::Usage; return true;
    default: return false;
    }
}
}  // namespace
PageOrder default_pages() { return {{{View::Switch, true}, {View::Overview, true}, {View::Clock, false}, {View::Power, false}, {View::Usage, false}}}; }
View home_view(const PageOrder &o) {
    for (const auto &s : o) if (s.shown) return s.view;
    return View::Switch;   // parse_pages and pages_toggle never leave none shown
}
std::vector<View> nav_views(const PageOrder &o) {
    std::vector<View> out;
    for (const auto &s : o) if (s.shown) out.push_back(s.view);
    if (out.empty()) out.push_back(View::Switch);
    return out;
}
bool pages_toggle(PageOrder &o, size_t slot) {
    if (slot >= o.size()) return false;
    size_t shown = 0;
    for (const auto &s : o) shown += s.shown ? 1 : 0;
    if (o[slot].shown && shown <= 1) return false;
    if (!o[slot].shown && shown >= kMaxShownPages) return false;   // the navigation holds four (13.12 G7)
    o[slot].shown = !o[slot].shown;
    return true;
}
bool pages_move(PageOrder &o, size_t slot, int delta) {
    if ((delta != -1 && delta != 1) || slot >= o.size()) return false;
    if ((delta < 0 && slot == 0) || (delta > 0 && slot + 1 >= o.size())) return false;
    std::swap(o[slot], o[static_cast<size_t>(static_cast<int>(slot) + delta)]);
    return true;
}
std::string serialize_pages(const PageOrder &o) {
    std::string out;
    for (const auto &s : o) { out += view_letter(s.view); out += s.shown ? '+' : '-'; }
    return out;
}
bool parse_pages(const std::string &given, PageOrder &out) {
    // 0.3.27 and earlier stored four pages: Usage comes last, hidden (13.12 G7).
    const std::string text = given.size() == 2 * (kViewCount - 1) && given.find('U') == std::string::npos ? given + "U-" : given;
    if (text.size() != 2 * kViewCount) return false;
    PageOrder o{};
    std::array<bool, kViewCount> seen{};
    bool any = false;
    for (size_t i = 0; i < kViewCount; ++i) {
        View v;
        if (!view_of(text[2 * i], v) || seen[static_cast<size_t>(v)]) return false;
        const char flag = text[2 * i + 1];
        if (flag != '+' && flag != '-') return false;
        seen[static_cast<size_t>(v)] = true;
        o[i] = {v, flag == '+'};
        any = any || flag == '+';
    }
    size_t shown = 0;
    for (const auto &s : o) shown += s.shown ? 1 : 0;
    if (!any || shown > kMaxShownPages) return false;   // five shown would not fit the navigation (13.12 G7)
    out = o;
    return true;
}

// ---- appearance ----
std::string serialize(const Appearance &a) {
    return std::string(theme_name(a.theme)) + "|" + (a.accent ? fmt("%06x", static_cast<unsigned>(a.accent & 0xffffff)) : std::string("-")) + "|" +
           lang_name(a.lang) + "|" + size_name(a.size) + "|" + (a.show_percent ? "pct" : "nopct") + "|" +
           (a.ring == RingTier::Thin ? "thin" : a.ring == RingTier::Thick ? "thick" : "standard") + "|" + serialize_pages(a.pages) + "|" +
           (a.style == SwitchStyle::Icons ? "icons" : "list") + "|" + (a.clock_hands ? "hands" : "nohands");
}
Appearance parse_appearance(const std::string &text) {
    Appearance out;
    if (text.empty() || text.size() > 80) return out;
    std::vector<std::string> field;
    size_t from = 0;
    for (;;) {
        const auto bar = text.find('|', from);
        field.push_back(text.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
        if (bar == std::string::npos) break;
        from = bar + 1;
    }
    if (field.size() != 4 && field.size() != 6 && field.size() != 8 && field.size() != 9) return Appearance{};
    const bool old = field.size() < 8;   // 0.3.x: the classic themes were the only ones
    Appearance a;
    if (field[0] == "mint") a.theme = ThemeId::Mint;
    else if (field[0] == "ocean") a.theme = ThemeId::Ocean;
    else if (field[0] == "amber") a.theme = ThemeId::Amber;
    else if (field[0] == "night" && !old) a.theme = ThemeId::Night;
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
    if (field.size() >= 6) {
        if (field[4] == "pct") a.show_percent = true;
        else if (field[4] == "nopct") a.show_percent = false;
        else return Appearance{};
        if (field[5] == "thin") a.ring = RingTier::Thin;
        else if (field[5] == "standard") a.ring = RingTier::Standard;
        else if (field[5] == "thick") a.ring = RingTier::Thick;
        else return Appearance{};
    }
    if (field.size() >= 8) {
        if (!parse_pages(field[6], a.pages)) return Appearance{};
        if (field[7] == "list") a.style = SwitchStyle::List;
        else if (field[7] == "icons") a.style = SwitchStyle::Icons;
        else return Appearance{};
    }
    if (field.size() == 9) {
        if (field[8] == "hands") a.clock_hands = true;
        else if (field[8] == "nohands") a.clock_hands = false;
        else return Appearance{};
    }
    if (old) {   // the new look, with the colour the person had: ocean's blue is close to night's own
        if (a.accent == 0 && a.theme != ThemeId::Ocean) a.accent = theme_palette(a.theme).accent;
        a.theme = ThemeId::Night;
    }
    return a;
}

// ---- input sources ----
const std::array<SourceIcon, 8> kIconChoices = {SourceIcon::Hdmi, SourceIcon::Dp, SourceIcon::UsbC, SourceIcon::Laptop,
                                                SourceIcon::Desktop, SourceIcon::Monitor, SourceIcon::Console, SourceIcon::Tv};
const char *icon_key(SourceIcon i) {
    switch (i) {
    case SourceIcon::Hdmi: return "hdmi";
    case SourceIcon::Dp: return "dp";
    case SourceIcon::UsbC: return "usbc";
    case SourceIcon::Laptop: return "laptop";
    case SourceIcon::Desktop: return "desktop";
    case SourceIcon::Monitor: return "monitor";
    case SourceIcon::Console: return "console";
    case SourceIcon::Tv: return "tv";
    case SourceIcon::Auto: break;
    }
    return "auto";
}
namespace {
bool icon_of(const std::string &key, SourceIcon &out) {
    for (SourceIcon i : {SourceIcon::Auto, SourceIcon::Hdmi, SourceIcon::Dp, SourceIcon::UsbC, SourceIcon::Laptop, SourceIcon::Desktop,
                         SourceIcon::Monitor, SourceIcon::Console, SourceIcon::Tv})
        if (key == icon_key(i)) { out = i; return true; }
    return false;
}
// The words of an input id, in lower case, with a trailing number taken off: "Mini-DP2" is {"mini", "dp"}.
std::vector<std::string> id_words(const std::string &id) {
    std::vector<std::string> words;
    std::string w;
    auto flush = [&] {
        while (!w.empty() && w.back() >= '0' && w.back() <= '9') w.pop_back();
        if (!w.empty()) words.push_back(w);
        w.clear();
    };
    for (char c : id) {
        const char l = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9')) w += l;
        else flush();
    }
    flush();
    return words;
}
bool valid_utf8_text(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 0;
        uint32_t cp = 0;
        if (c < 0x80) { if (c < 0x20 || c == 0x7f) return false; ++i; continue; }
        if ((c >> 5) == 6) { n = 2; cp = c & 0x1f; }
        else if ((c >> 4) == 14) { n = 3; cp = c & 0x0f; }
        else if ((c >> 3) == 30) { n = 4; cp = c & 0x07; }
        else return false;
        if (i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d >> 6) != 2) return false;
            cp = (cp << 6) | (d & 0x3f);
        }
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff) ||
            (cp >= 0x80 && cp < 0xa0))
            return false;
        i += n;
    }
    return true;
}
bool valid_source_id(const std::string &id) { return !id.empty() && id.size() <= kMaxSourceId && valid_utf8_text(id); }
SourceLocal *find_source(SourceNames &names, const std::string &id) {
    for (auto &n : names) if (n.id == id) return &n;
    return nullptr;
}
// The entry for `id`, made when there is room; nullptr for a bad id or a 13th source.
SourceLocal *entry_for(SourceNames &names, const std::string &id) {
    if (!valid_source_id(id)) return nullptr;
    if (auto *e = find_source(names, id)) return e;
    if (names.size() >= kMaxSourceNames) return nullptr;
    names.push_back({id, "", SourceIcon::Auto});
    return &names.back();
}
void drop_empty(SourceNames &names) {
    names.erase(std::remove_if(names.begin(), names.end(), [](const SourceLocal &n) { return n.name.empty() && n.icon == SourceIcon::Auto; }), names.end());
}
}  // namespace
SourceIcon port_icon(const std::string &input_id) {
    const auto w = id_words(input_id);
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == "hdmi") return SourceIcon::Hdmi;
        if (w[i] == "dp" || w[i] == "displayport") return SourceIcon::Dp;
        if (w[i] == "usbc" || w[i] == "typec" || w[i] == "thunderbolt") return SourceIcon::UsbC;
        if ((w[i] == "usb" || w[i] == "type") && i + 1 < w.size() && w[i + 1] == "c") return SourceIcon::UsbC;
    }
    return SourceIcon::Monitor;
}
std::string port_tag(const std::string &input_id) {
    switch (port_icon(input_id)) {
    case SourceIcon::Hdmi: return "HDMI";
    case SourceIcon::Dp: return "DisplayPort";
    case SourceIcon::UsbC: return "USB-C";
    default: return "";
    }
}
bool valid_source_name(const std::string &name) { return !name.empty() && name.size() <= kMaxSourceName && valid_utf8_text(name); }
bool set_source_name(SourceNames &names, const std::string &id, const std::string &name) {
    if (!name.empty() && !valid_source_name(name)) return false;
    if (name.empty() && valid_source_id(id) && !find_source(names, id)) return true;   // nothing to forget
    auto *e = entry_for(names, id);
    if (!e) return false;
    e->name = name;
    drop_empty(names);
    return true;
}
bool set_source_icon(SourceNames &names, const std::string &id, SourceIcon icon) {
    if (icon == SourceIcon::Auto && valid_source_id(id) && !find_source(names, id)) return true;
    auto *e = entry_for(names, id);
    if (!e) return false;
    e->icon = icon;
    drop_empty(names);
    return true;
}
void reset_source(SourceNames &names, const std::string &id) {
    names.erase(std::remove_if(names.begin(), names.end(), [&](const SourceLocal &n) { return n.id == id; }), names.end());
}
// Records end with 0x1e, fields are split by 0x1f: neither can be in an id or a name (no control characters).
std::string serialize_sources(const SourceNames &names) {
    std::string out;
    for (const auto &n : names) out += n.id + '\x1f' + n.name + '\x1f' + icon_key(n.icon) + '\x1e';
    return out;
}
SourceNames parse_sources(const std::string &text) {
    SourceNames out;
    if (text.size() > 2048) return {};
    size_t from = 0;
    while (from < text.size()) {
        const size_t end = text.find('\x1e', from);
        if (end == std::string::npos) return {};
        const std::string record = text.substr(from, end - from);
        from = end + 1;
        const size_t a = record.find('\x1f');
        const size_t b = a == std::string::npos ? std::string::npos : record.find('\x1f', a + 1);
        if (b == std::string::npos || record.find('\x1f', b + 1) != std::string::npos) return {};
        SourceLocal n;
        n.id = record.substr(0, a);
        n.name = record.substr(a + 1, b - a - 1);
        if (!valid_source_id(n.id) || (!n.name.empty() && !valid_source_name(n.name)) || !icon_of(record.substr(b + 1), n.icon)) return {};
        if (n.name.empty() && n.icon == SourceIcon::Auto) return {};
        if (find_source(out, n.id) || out.size() >= kMaxSourceNames) return {};
        out.push_back(n);
    }
    return out;
}
std::vector<SourceView> make_sources(Lang l, const Snapshot &s, const SourceNames &names) {
    std::vector<SourceView> out;
    const size_t n = std::min<size_t>(s.action_count, s.actions.size());
    for (size_t i = 0; i < n; ++i) {
        const Action &a = s.actions[i];
        SourceView v;
        v.id = a.id;
        v.name = a.label.empty() ? a.id : dynamic_text(l, a.label);
        v.icon = port_icon(a.input);
        v.tag = port_tag(a.input);
        for (const auto &local : names) {
            if (local.id != a.id || a.id.empty()) continue;
            if (!local.name.empty()) { v.name = local.name; v.renamed = true; }
            if (local.icon != SourceIcon::Auto) v.icon = local.icon;
        }
        v.enabled = s.agent_ready;
        if (!v.enabled) v.sub = T(l, Str::ActionOffline);
        out.push_back(v);
    }
    return out;
}

// ---- freshness ----
bool power_fresh(const PowerState &p, int64_t now_ms) {
    return p.observed_ms > 0 && now_ms >= p.observed_ms && now_ms - p.observed_ms <= kPowerStaleMs;
}
bool metrics_fresh(const Snapshot &s, int64_t now_ms) { return s.sampled_ms != 0 && now_ms < s.metrics_expires_ms; }

// ---- header and ring ----
HeaderView make_header(const Snapshot &s, const PowerState &p, int64_t now_ms, bool show_percent) {
    using Kind = BatteryView::Kind;
    HeaderView h;
    h.agent = s.agent_ready ? LinkIcon::Ok : LinkIcon::Off;
    h.agent_tone = s.agent_ready ? Tone::Ok : Tone::Off;
    const WifiKind kind = wifi_kind(s);
    h.wifi = kind == WifiKind::Connected ? LinkIcon::Ok : kind == WifiKind::Connecting ? LinkIcon::Busy : LinkIcon::Off;
    h.wifi_tone = wifi_tone(kind);

    BatteryView &b = h.battery;
    if (p.fault) {
        b.kind = Kind::Fault;
        b.digits = "!";
        b.tone = Tone::Warn;
        return h;
    }
    if (!power_fresh(p, now_ms)) {  // nothing about an old sample is shown: no USB, no bolt, no level
        b.kind = Kind::Unknown;
        b.digits = "?";
        return h;
    }
    h.usb = p.vbus == "present";
    if (p.battery == "absent") {
        b.kind = Kind::None;
        return h;
    }
    if (p.battery == "present" && level_valid(p.soc_percent)) {
        b.kind = Kind::Level;
        b.percent = p.soc_percent;
        b.digits = show_percent ? std::to_string(p.soc_percent) : std::string();
        b.tone = level_tone(p.soc_percent);
    } else {
        b.kind = Kind::Unknown;
        b.digits = "?";
    }
    b.charging = p.battery == "present" && p.charging == "charging";
    return h;
}
bool blink_on(int64_t now_ms) { return now_ms < 0 || (now_ms / 500) % 2 == 0; }
RingView make_ring(const Snapshot &s, const PowerState &p, int64_t now_ms) {
    RingView r;
    r.wifi = s.wifi_ready;
    r.agent = s.agent_ready;
    r.time = s.time_valid;
    r.power = power_fresh(p, now_ms) && !p.fault && (p.vbus == "present" || p.battery == "present");
    return r;
}

RingInfoView make_ring_info(Lang l, const Snapshot &s, const PowerState &p, int64_t now_ms) {
    const RingView r = make_ring(s, p, now_ms);
    RingInfoView v;
    auto set = [&](int i, Str name, bool ok, const std::string &good_state, Str good_note, const std::string &bad_state, const std::string &bad_note) {
        v.row[i] = RingRow{T(l, name), ok ? good_state : bad_state, ok ? T(l, good_note) : bad_note, ok};
    };
    set(0, Str::RingNetwork, r.wifi, T(l, Str::ValueConnected), Str::RingNoteWifi, T(l, Str::RingNotConnected), wifi_text(l, wifi_kind(s)));
    const std::string agent_why = !s.wifi_ready ? T(l, Str::RingNoteNeedWifi) : contains(s.agent, "未配对") ? T(l, Str::ValueNotPaired) : T(l, Str::ValueNotConnected);
    set(1, Str::RingComputer, r.agent, T(l, Str::ValueConnected), Str::RingNoteAgent, T(l, Str::RingNotConnected), agent_why);
    v.power_fault = !r.power && power_fresh(p, now_ms) && p.fault;
    set(2, Str::RingPower, r.power, T(l, Str::RingNormal), Str::RingNotePower, v.power_fault ? T(l, Str::RingFault) : T(l, Str::RingNoReading),
        v.power_fault ? T(l, Str::RingNoteFault) : T(l, Str::RingNoteNoPower));
    set(3, Str::RingTime, r.time, T(l, Str::RingValid), Str::RingNoteTime, T(l, Str::RingNotSet), T(l, Str::RingNoteTimeFix));
    for (int i = 0; i < 4; ++i) {
        if (!v.row[i].ok) { v.first_problem = i; break; }
    }
    return v;
}
bool quiet_hours(const DisplayConfig &c, bool time_valid, uint16_t minute_of_day) {
    return c.night_enabled && (!time_valid || in_night(minute_of_day, c.night_start, c.night_end));
}
std::string ring_note(Lang l, const RingInfoView &v) {
    switch (v.first_problem) {
    case 0: return T(l, Str::RingProblemWifi);
    case 1: return T(l, Str::RingProblemAgent);
    case 2: return T(l, v.power_fault ? Str::RingNoteFault : Str::RingProblemPower);
    case 3: return T(l, Str::RingProblemTime);
    default: break;
    }
    return T(l, Str::RingHint);
}

// ---- clock ----
std::string tz_label(int minutes) {
    if (minutes == 0) return "UTC";
    const int size = std::abs(minutes);
    return size % 60 ? fmt("UTC%c%d:%02d", minutes > 0 ? '+' : '-', size / 60, size % 60) : fmt("UTC%c%d", minutes > 0 ? '+' : '-', size / 60);
}
static int days_in_month(int year, int month) {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : kDays[month - 1];
}
TimeDraft time_draft(bool time_valid, time_t utc_now, int tz_minutes) {
    TimeDraft d;
    if (!time_valid) return d;
    const time_t local = utc_now + static_cast<time_t>(tz_minutes) * 60;
    struct tm t{};
    gmtime_r(&local, &t);
    d.year = t.tm_year + 1900; d.month = t.tm_mon + 1; d.day = t.tm_mday; d.hour = t.tm_hour; d.minute = t.tm_min;
    return d;
}
void time_draft_step(TimeDraft &d, int field, int delta) {
    const auto wrap = [](int value, int low, int high) { const int span = high - low + 1; return low + ((value - low) % span + span) % span; };
    switch (field) {
    case 0: d.year = std::min(2099, std::max(2024, d.year + delta)); break;
    case 1: d.month = wrap(d.month + delta, 1, 12); break;
    case 2: d.day = wrap(d.day + delta, 1, days_in_month(d.year, d.month)); break;
    case 3: d.hour = wrap(d.hour + delta, 0, 23); break;
    case 4: d.minute = wrap(d.minute + delta, 0, 59); break;
    default: return;
    }
    d.day = std::min(d.day, days_in_month(d.year, d.month));
}
std::string time_draft_utc(const TimeDraft &d, int tz_minutes) {
    if (d.month < 1 || d.month > 12 || d.day < 1 || d.day > days_in_month(d.year, d.month)) return "";
    struct tm t{};
    t.tm_year = d.year - 1900; t.tm_mon = d.month - 1; t.tm_mday = d.day; t.tm_hour = d.hour; t.tm_min = d.minute;
    const time_t utc = timegm(&t) - static_cast<time_t>(tz_minutes) * 60;
    struct tm u{};
    gmtime_r(&utc, &u);
    if (u.tm_year + 1900 < 2024 || u.tm_year + 1900 > 2099) return "";
    return fmt("%04d-%02d-%02dT%02d:%02d:00Z", u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min);
}
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

// ---- gauges ----
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
std::vector<ConnectRow> make_connect(Lang l, const Snapshot &s, bool paired, const std::string &ssid) {
    const WifiKind kind = wifi_kind(s);
    const bool up = kind == WifiKind::Connected;
    std::vector<ConnectRow> rows;
    rows.push_back({T(l, Str::KeyWifi), ssid.empty() ? wifi_text(l, kind) : ssid + (l == Lang::EnUS ? " - " : " · ") + wifi_text(l, kind), wifi_tone(kind)});
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
// ---- the Usage page (docs/desktop-app-design.md 13.12) ----
std::string short_count(uint64_t n) {
    if (n < 1000) return std::to_string(n);
    static const char *units[] = {"K", "M", "B", "T"};
    double v = static_cast<double>(n);
    int u = -1;
    while (v >= 999.95 && u < 3) { v /= 1000.0; ++u; }
    return fmt("%.1f%s", v, units[u]);
}
namespace {
std::string window_label(Lang l, uint32_t minutes) {
    if (minutes % 1440 == 0) return fmt(tr(l, Str::UsageWindowDays), static_cast<int>(minutes / 1440));
    if (minutes % 60 == 0) return fmt(tr(l, Str::UsageWindowHours), static_cast<int>(minutes / 60));
    return fmt(tr(l, Str::UsageWindowMinutes), static_cast<int>(minutes));
}
// G5: left is what is left now; the weekday only when the device knows the time.
std::string reset_text(Lang l, const ble::UsageWindow &w, int64_t age_s, bool time_valid, time_t now) {
    if (w.reset_s == 0) return T(l, Str::UsageResetUnknown);
    const int64_t left = static_cast<int64_t>(w.reset_s) - age_s;
    if (left <= 0) return T(l, Str::UsageResetNow);
    if (left < 3600) return fmt(tr(l, Str::UsageResetMinutes), static_cast<int>(std::max<int64_t>(1, left / 60)));
    if (left < 86400) return fmt(tr(l, Str::UsageResetHours), static_cast<int>(left / 3600), static_cast<int>(left % 3600 / 60));
    if (!time_valid || now < kClockEpoch) return fmt(tr(l, Str::UsageResetDays), static_cast<int>(left / 86400));
    const time_t at = now + static_cast<time_t>(left);
    struct tm t{};
    localtime_r(&at, &t);
    return fmt(tr(l, Str::UsageResetAt), weekday_name(l, t.tm_wday), t.tm_hour, t.tm_min);
}
std::string ago_text(Lang l, int64_t s) {
    if (s < 10) return T(l, Str::UsageAgoNow);
    if (s < 60) return fmt(tr(l, Str::UsageAgoSeconds), static_cast<int>(s));
    if (s < 3600) return fmt(tr(l, Str::UsageAgoMinutes), static_cast<int>(s / 60));
    if (s < 86400) return fmt(tr(l, Str::UsageAgoHours), static_cast<int>(s / 3600));
    return fmt(tr(l, Str::UsageAgoDays), static_cast<int>(s / 86400));
}
}  // namespace
UsageView make_usage(Lang l, const ble::Usage *u, int64_t age_ms, bool time_valid, time_t now) {
    UsageView v;
    const std::string none = "\u2014";
    v.calls = v.tokens = v.success = v.ttft = v.model = none;
    v.title = "Magpie";
    if (!u) { v.state = UsageView::State::Waiting; v.status = T(l, Str::UsageWaiting); return v; }   // G4
    if (!u->ok) { v.state = UsageView::State::Offline; v.status = T(l, Str::UsageOffline); return v; }   // G2: no old numbers
    const int64_t age_s = std::max<int64_t>(0, age_ms / 1000);
    if (age_ms > ble::kUsageStaleMs) {   // G3
        v.state = UsageView::State::Stale;
        v.dim = true;
        v.status = fmt(tr(l, Str::UsageStaleAgo), ago_text(l, age_s).c_str());
    } else {
        v.state = UsageView::State::Fresh;
        v.status = age_ms < 5000 ? T(l, Str::UsageJustNow) : fmt(tr(l, Str::UsageUpdatedAgo), static_cast<int>(age_s));
    }
    if (!u->provider.empty()) {
        std::string p = u->provider;
        p[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
        v.title = u->plan.empty() ? p : p + (l == Lang::EnUS ? " / " : " \u00b7 ") + u->plan;   // English text stays ASCII
    }
    for (const auto &w : u->windows) v.quotas.push_back({window_label(l, w.minutes), static_cast<int>(w.remaining), reset_text(l, w, age_s, time_valid, now)});
    v.calls = short_count(u->calls);
    v.tokens = short_count(u->tokens);
    if (u->calls > 0) {   // G6
        v.success = fmt("%.1f%%", 100.0 * (u->calls - u->errors) / u->calls);
        if (u->ttft_ms > 0) v.ttft = std::to_string(u->ttft_ms);
    }
    if (!u->latest_model.empty()) v.model = u->latest_model;
    if (u->latest_status) v.code = std::to_string(u->latest_status);
    if (u->latest_ms) v.latency = u->latest_ms < 1000 ? std::to_string(u->latest_ms) + " ms" : fmt(tr(l, Str::UsageSeconds), u->latest_ms / 1000.0);
    v.ago = ago_text(l, static_cast<int64_t>(u->latest_age_s) + age_s);
    v.failed = u->latest_status != 0 && (u->latest_status < 200 || u->latest_status >= 400);
    return v;
}
}  // namespace dh::ui
