// Failure modes of the device UI's pure layers (theme, text, view models), written BEFORE the code.
// Rules: docs/rules.md K3 (unknown stays unknown, stale stays stale, nothing invented), R9 (contrast, packs).
//
// T1  Theme palettes: ink, muted and accent are readable (>= 4.5:1) on the background and the card, for all themes.
// T2  Contrast maths: black/white is 21, equal colours are 1, the ratio is symmetric; 4.5 passes, 3 warns, below refuses.
// T3  A custom accent that is unreadable is refused, so the UI can never be made unreadable. 0 keeps the theme's own.
// T4  Every accent the Appearance page offers is readable on every theme, and text on the "selected" tint stays readable.
// T5  Appearance text round-trips for every combination. Malformed text (empty, wrong field count, bad hex, upper-case
//     hex, unknown theme/language/size, too long) gives the default appearance instead of a half-parsed one.
// T6  Font tiers use only the three compiled-in sizes and grow with the tier.
// T7  Every string exists in both languages. English is ASCII only. Chinese uses only glyphs of the shipped font.
//     printf placeholders agree between the languages.
// T8  Dynamic text from the core/Agent: English translates the known sentences, shows an error code as its code,
//     leaves unknown text alone. Chinese never changes. Translations are ASCII.
// T9  Power freshness: never sampled, older than 6 s, or from the future is stale.
// T10 Header: an unknown level is never drawn as 0 %; a stale sample, an absent battery, a PMIC fault and an
//     out-of-range level draw no fill. Tones follow the level (>20 ok, 11-20 warn, <=10 danger).
// T11 Ring: a segment is lit only when its channel is healthy; a stale or faulty power sample leaves power dark.
// T12 Clock: invalid time shows "--:--" and "not synced", never 1970. Valid time uses the local time zone.
// T13 Percent parsing: only "<0..100>%" counts. "--", text, NaN, inf, negative, over 100, trailing junk, a missing
//     "%" and stale data are invalid and show "--", never 0 %.
// T14 Computer: not sampled, expired and fresh are three different footers. Gauges, rates are "--" unless fresh.
// T15 Power readings: "--" with a reason note when the ADC failed, the sample is stale or the PMIC faulted;
//     values outside the plausible range are refused even when the board says ADC_OK; rounding is correct.
// T16 Connection rows: no IP or signal while Wi-Fi is down; a signal outside -127..-1 is "--".
// T17 Actions: at most 12, an empty label shows the id, offline actions are disabled with a reason.
// T18 Command notice: only the last 15 seconds, only a real command, only forward in time.
// T19 Charge protection view (docs/charge-protection-design.md, U1 to U4): every state has its words, in both languages;
//     "stopped" and "restored" are never claimed when the state of the switch is not known; the switch-on button is
//     disabled, with the reason, when it could not work; the switch-off button is always there while protection is on;
//     the failsafe words say why, and say nothing when the reason is not known.
#include "ui_model.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace dh;
using namespace dh::ui;

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++failures;                                                               \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                             \
    } while (0)
#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        const auto va_ = (a);                                                                             \
        const auto vb_ = (b);                                                                             \
        if (!(va_ == vb_)) {                                                                              \
            ++failures;                                                                                   \
            std::ostringstream o_;                                                                        \
            o_ << "FAIL " << __FILE__ << ':' << __LINE__ << "  " << #a << " == " << #b << "  got \"" << va_ \
               << "\" want \"" << vb_ << "\"";                                                            \
            std::cerr << o_.str() << std::endl;                                                           \
        }                                                                                                 \
    } while (0)

static const int64_t NOW = 100000;
static const std::array<ThemeId, 3> kThemes = {ThemeId::Mint, ThemeId::Ocean, ThemeId::Amber};

static PowerState fresh_power() {
    PowerState p;
    p.battery = "present";
    p.vbus = "present";
    p.charging = "charging";
    p.reason = "ok";
    p.soc_percent = 78;
    p.battery_mv = 3921;
    p.vbus_mv = 5120;
    p.chip_temp_c = 41.26f;
    p.measure_reason = "ADC_OK";
    p.observed_ms = NOW - 1000;
    return p;
}
static Snapshot online_snapshot() {
    Snapshot s;
    s.device_id = "dh-0a1b2c";
    s.wifi = "已连接";
    s.wifi_ready = true;
    s.rssi = -61;
    s.ip = "10.10.10.23";
    s.agent = "已连接";
    s.agent_ready = true;
    s.time_valid = true;
    s.cpu = "23.4%";
    s.memory = "61.0%";
    s.network = "1.2 MB/s";
    s.network_send = "0.3 MB/s";
    s.sampled_ms = NOW - 2000;
    s.metrics_expires_ms = NOW + 8000;
    return s;
}

// ---- UTF-8 helpers ----
static std::set<uint32_t> decode(const std::string &s, bool *ok = nullptr) {
    std::set<uint32_t> out;
    bool good = true;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        uint32_t cp = 0;
        int n = 0;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0f; n = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
        else { good = false; ++i; continue; }
        if (i + n > s.size()) { good = false; break; }
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        out.insert(cp);
        i += n;
    }
    if (ok) *ok = good;
    return out;
}
static std::set<uint32_t> font_glyphs() {
    std::ifstream in(UI_CHARS_FILE);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
    return decode(text);
}
static bool ascii(const std::string &s) {
    for (unsigned char c : s) if (c < 0x20 || c > 0x7e) return false;
    return true;
}
static bool renderable(const std::string &s, const std::set<uint32_t> &glyphs) {
    bool ok = true;
    for (uint32_t cp : decode(s, &ok)) if (!glyphs.count(cp)) return false;
    return ok;
}
static std::string specifiers(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') continue;
        size_t j = i + 1;
        while (j < s.size() && (s[j] == '.' || (s[j] >= '0' && s[j] <= '9'))) ++j;
        out += s.substr(i, j + 1 - i);
        i = j;
    }
    return out;
}
static std::string without_placeholders(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') { out += s[i]; continue; }
        size_t j = i + 1;
        while (j < s.size() && (s[j] == '.' || (s[j] >= '0' && s[j] <= '9'))) ++j;
        i = j;
    }
    return out;
}

// ---- T1-T4 ----
static void t1_palettes_readable() {
    for (ThemeId id : kThemes) {
        const Palette &p = theme_palette(id);
        for (uint32_t bg : {p.bg, p.bg2, p.card}) {
            CHECK(contrast_ratio(p.ink, bg) >= 4.5);
            CHECK(contrast_ratio(p.muted, bg) >= 4.5);
            CHECK(contrast_ratio(p.accent, bg) >= 4.5);
        }
        CHECK(contrast_ratio(p.accent, p.selected) >= 4.5);  // nav label on the selected pill
        CHECK(contrast_ratio(p.ink, p.selected) >= 4.5);
        for (uint32_t c : {p.wifi, p.agent, p.power, p.clock}) CHECK(contrast_ratio(c, p.card) >= 3.0);
        CHECK(contrast_ratio(p.danger, p.card) >= 4.5);
    }
    CHECK_EQ(theme_palette(ThemeId::Mint).accent, 0x5ee6b8u);  // the tokens of docs/design/README.md
    CHECK_EQ(theme_palette(ThemeId::Ocean).card, 0x0a2130u);
    CHECK_EQ(theme_palette(ThemeId::Amber).ink, 0xf5e9dcu);
}
static void t2_contrast_maths() {
    CHECK(std::fabs(contrast_ratio(0x000000, 0xffffff) - 21.0) < 0.01);
    CHECK(std::fabs(contrast_ratio(0x336699, 0x336699) - 1.0) < 1e-9);
    CHECK(std::fabs(contrast_ratio(0x5ee6b8, 0x0b1d1a) - contrast_ratio(0x0b1d1a, 0x5ee6b8)) < 1e-12);
    CHECK(contrast_ratio(0x777777, 0xffffff) > 4.4 && contrast_ratio(0x777777, 0xffffff) < 4.6);  // known: 4.48
    CHECK(classify_contrast(4.5) == Contrast::Ok);
    CHECK(classify_contrast(4.49) == Contrast::Warn);
    CHECK(classify_contrast(3.0) == Contrast::Warn);
    CHECK(classify_contrast(2.99) == Contrast::Refuse);
    CHECK(classify_contrast(1.0) == Contrast::Refuse);
}
static void t3_accent_guard() {
    const Palette &mint = theme_palette(ThemeId::Mint);
    const Palette same = apply_accent(mint, 0);
    CHECK_EQ(same.accent, mint.accent);
    CHECK_EQ(same.selected, mint.selected);
    // Dark green on a dark card: unreadable, ignored.
    CHECK(accent_contrast(0x0e2a24, mint) == Contrast::Refuse);
    const Palette refused = apply_accent(mint, 0x0e2a24);
    CHECK_EQ(refused.accent, mint.accent);
    CHECK_EQ(refused.selected, mint.selected);
    // A readable accent is applied, the rest of the palette stays.
    const Palette pink = apply_accent(mint, 0xff7eb6);
    CHECK_EQ(pink.accent, 0xff7eb6u);
    CHECK(pink.selected != mint.selected);
    CHECK_EQ(pink.bg, mint.bg);
    CHECK_EQ(pink.card, mint.card);
    CHECK_EQ(pink.ink, mint.ink);
    // The worst surface decides, not the best one: white is perfect on black and invisible on white.
    CHECK(accent_contrast(0x1b3d37, mint) == Contrast::Refuse);
    Palette split = mint;
    split.bg = 0x000000; split.bg2 = 0x000000; split.card = 0x000000;
    CHECK(accent_contrast(0xffffff, split) == Contrast::Ok);
    Palette light_bg = split;
    light_bg.bg = 0xffffff;
    CHECK(accent_contrast(0xffffff, light_bg) == Contrast::Refuse);
    Palette light_bg2 = split;
    light_bg2.bg2 = 0xffffff;
    CHECK(accent_contrast(0xffffff, light_bg2) == Contrast::Refuse);
    Palette light_card = split;
    light_card.card = 0xffffff;
    CHECK(accent_contrast(0xffffff, light_card) == Contrast::Refuse);
}
static void t4_accent_choices() {
    CHECK(kAccentChoices.size() == 6);
    std::set<uint32_t> seen(kAccentChoices.begin(), kAccentChoices.end());
    CHECK(seen.size() == kAccentChoices.size());  // no duplicates
    for (ThemeId id : kThemes) {
        for (uint32_t accent : kAccentChoices) {
            const Palette &base = theme_palette(id);
            CHECK(accent_contrast(accent, base) == Contrast::Ok);
            const Palette p = apply_accent(base, accent);
            CHECK_EQ(p.accent, accent);
            CHECK(contrast_ratio(p.ink, p.selected) >= 4.5);
            CHECK(contrast_ratio(p.accent, p.selected) >= 3.0);
        }
    }
}

// ---- T5-T6 ----
static void t5_appearance() {
    int combos = 0;
    for (ThemeId t : kThemes)
        for (uint32_t accent : {0u, kAccentChoices[0], kAccentChoices[5]})
            for (Lang l : {Lang::ZhCN, Lang::EnUS})
                for (SizeTier z : {SizeTier::Small, SizeTier::Standard, SizeTier::Large})
                    for (bool percent : {true, false})
                        for (RingTier r : {RingTier::Thin, RingTier::Standard, RingTier::Thick}) {
                            Appearance a;
                            a.theme = t; a.accent = accent; a.lang = l; a.size = z; a.show_percent = percent; a.ring = r;
                            const Appearance b = parse_appearance(serialize(a));
                            CHECK(b.theme == t && b.accent == accent && b.lang == l && b.size == z && b.show_percent == percent && b.ring == r);
                            ++combos;
                        }
    CHECK_EQ(combos, 54 * 2 * 3);
    Appearance d;
    CHECK(d.show_percent && d.ring == RingTier::Standard);   // the battery percent is shown unless you turn it off
    CHECK_EQ(serialize(d), std::string("mint|-|zh-CN|standard|pct|standard"));
    Appearance a;
    a.theme = ThemeId::Ocean; a.accent = 0x5ee6b8; a.lang = Lang::EnUS; a.size = SizeTier::Large; a.show_percent = false; a.ring = RingTier::Thick;
    CHECK_EQ(serialize(a), std::string("ocean|5ee6b8|en-US|large|nopct|thick"));
    // What 0.3.x stored has four fields. It is still understood, and the new options are the defaults.
    const Appearance old = parse_appearance("ocean|5ee6b8|en-US|large");
    CHECK(old.theme == ThemeId::Ocean && old.accent == 0x5ee6b8u && old.lang == Lang::EnUS && old.size == SizeTier::Large);
    CHECK(old.show_percent && old.ring == RingTier::Standard);
    const std::vector<std::string> bad = {
        "", "garbage", "mint|-|zh-CN", "mint|-|zh-CN|standard|pct", "mint|-|zh-CN|standard|pct|standard|extra", "mint|zzzzzz|zh-CN|standard|pct|standard",
        "mint|5EE6B8|zh-CN|standard|pct|standard", "mint|5ee6b|zh-CN|standard|pct|standard", "mint|5ee6b8f|zh-CN|standard|pct|standard",
        "pink|-|zh-CN|standard|pct|standard", "mint|-|fr-FR|standard|pct|standard", "mint|-|zh-CN|huge|pct|standard", "||||||", "mint|-|zh-CN|standard|pct|standard\n",
        "mint|-|zh-CN|standard|PCT|standard", "ocean|-|en-US|large|pct", "ocean|5ee6b8|en-US|large|nopct|thick|", "ocean|-|en-US|large|nopct|", "mint|-|zh-CN|standard|maybe|standard", "mint|-|zh-CN|standard|pct|huge", "mint|-|zh-CN|standard||",
        "mint|zzzzzz|zh-CN|standard", "mint|-|zh-CN|standard\n", std::string(500, 'a'), "mint|-|zh-CN|standard|pct|" + std::string(100, 'x')};
    for (const auto &s : bad) {
        const Appearance p = parse_appearance(s);
        CHECK(p.theme == d.theme && p.accent == d.accent && p.lang == d.lang && p.size == d.size && p.show_percent == d.show_percent && p.ring == d.ring);
    }
    CHECK(theme_from_name("nonsense") == ThemeId::Mint);
    CHECK(lang_from_name("nonsense") == Lang::ZhCN);
    for (ThemeId t : kThemes) CHECK(theme_from_name(theme_name(t)) == t);
    for (Lang l : {Lang::ZhCN, Lang::EnUS}) CHECK(lang_from_name(lang_name(l)) == l);
}
static void t6_font_tiers() {
    const std::set<int> allowed = {16, 20, 28};
    FontPx prev{0, 0, 0};
    for (SizeTier z : {SizeTier::Small, SizeTier::Standard, SizeTier::Large}) {
        const FontPx f = font_px(z);
        CHECK(allowed.count(f.title) && allowed.count(f.body) && allowed.count(f.caption));
        CHECK(f.title >= f.body && f.body >= f.caption);
        CHECK(f.body > prev.body);  // each tier makes the body larger
        prev = f;
    }
    CHECK_EQ(font_px(SizeTier::Standard).body, 20);
}

// ---- T7-T8 ----
static void t7_strings() {
    const auto glyphs = font_glyphs();
    CHECK(glyphs.size() > 1000);  // the file was read
    const int count = static_cast<int>(Str::Count);
    CHECK(count > 100);
    for (int i = 0; i < count; ++i) {
        const Str id = static_cast<Str>(i);
        const std::string zh = tr(Lang::ZhCN, id), en = tr(Lang::EnUS, id);
        if (zh.empty() || en.empty()) std::fprintf(stderr, "string %d is empty\n", i);
        CHECK(!zh.empty() && !en.empty());
        if (!ascii(en)) std::fprintf(stderr, "string %d is not ASCII in English: %s\n", i, en.c_str());
        CHECK(ascii(en));
        if (!renderable(without_placeholders(zh), glyphs)) std::fprintf(stderr, "string %d has a glyph outside the font: %s\n", i, zh.c_str());
        CHECK(renderable(without_placeholders(zh), glyphs));
        CHECK_EQ(specifiers(zh), specifiers(en));
    }
    for (int d = 0; d < 7; ++d) {
        CHECK(renderable(weekday_name(Lang::ZhCN, d), glyphs));
        CHECK(ascii(weekday_name(Lang::EnUS, d)) && std::string(weekday_name(Lang::EnUS, d)).size() == 3);
    }
    for (int m = 0; m < 12; ++m) CHECK(ascii(month_name(Lang::EnUS, m)) && std::string(month_name(Lang::EnUS, m)).size() == 3);
    // Out-of-range indices must not read outside the table.
    CHECK(weekday_name(Lang::ZhCN, -1) != nullptr && weekday_name(Lang::EnUS, 7) != nullptr);
    CHECK(month_name(Lang::EnUS, 12) != nullptr && month_name(Lang::ZhCN, -3) != nullptr);
    CHECK(tr(Lang::ZhCN, Str::Count) != nullptr && tr(Lang::EnUS, static_cast<Str>(60000)) != nullptr);
}
static void t8_dynamic_text() {
    const std::vector<std::string> known = {
        "尚无操作", "已接收，等待执行", "当前连接不可用，请重新点击", "接收结果未知，请通过Agent查询", "操作已过期，请重新点击",
        "状态未确认，可通过Agent查询结果", "连接中断，正在查询结果", "当前不可用或已有操作，请稍后点击", "Agent不可达或认证失败",
        "等待校时", "已连接", "正在连接", "未配置", "未配对", "管理台不可用或未授权", "管理台已连接", "尚未连接管理台",
        "不可用"};
    for (const auto &s : known) {
        const std::string en = dynamic_text(Lang::EnUS, s);
        if (en == s || !ascii(en)) std::fprintf(stderr, "no English for: %s\n", s.c_str());
        CHECK(en != s && ascii(en) && !en.empty());
        CHECK_EQ(dynamic_text(Lang::ZhCN, s), s);  // Chinese never changes
    }
    CHECK_EQ(dynamic_text(Lang::EnUS, "尚无操作"), std::string("No action yet"));
    CHECK_EQ(dynamic_text(Lang::EnUS, "CONFIG_STORAGE_FAULT：请通过USB恢复"), std::string("CONFIG_STORAGE_FAULT"));
    CHECK_EQ(dynamic_text(Lang::EnUS, "CLOCK_SKEW：请通过USB校时"), std::string("CLOCK_SKEW"));
    CHECK_EQ(dynamic_text(Lang::EnUS, "INVALID_AGENT_JSON"), std::string("INVALID_AGENT_JSON"));
    CHECK_EQ(dynamic_text(Lang::EnUS, "lower_case：不是错误码"), std::string("lower_case：不是错误码"));  // not a code
    CHECK_EQ(dynamic_text(Lang::EnUS, "某个不认识的句子"), std::string("某个不认识的句子"));
    CHECK_EQ(dynamic_text(Lang::EnUS, ""), std::string(""));
    CHECK_EQ(dynamic_text(Lang::EnUS, "Open Terminal"), std::string("Open Terminal"));
    CHECK_EQ(dynamic_text(Lang::ZhCN, "CONFIG_STORAGE_FAULT：请通过USB恢复"), std::string("CONFIG_STORAGE_FAULT：请通过USB恢复"));
}

// ---- T9-T11 ----
static void t9_freshness() {
    PowerState p = fresh_power();
    CHECK(power_fresh(p, NOW));
    p.observed_ms = NOW - kPowerStaleMs;
    CHECK(power_fresh(p, NOW));    // exactly 6 s is still fresh
    p.observed_ms = NOW - kPowerStaleMs - 1;
    CHECK(!power_fresh(p, NOW));
    p.observed_ms = 0;
    CHECK(!power_fresh(p, NOW));   // never sampled
    p.observed_ms = NOW + 1;
    CHECK(!power_fresh(p, NOW));   // from the future
    p.observed_ms = -5;
    CHECK(!power_fresh(p, NOW));
}
static void t10_header() {
    using Kind = BatteryView::Kind;
    const Snapshot s = online_snapshot();
    HeaderView h = make_header(s, fresh_power(), NOW, true);
    CHECK(h.wifi == LinkIcon::Ok && h.agent == LinkIcon::Ok && h.wifi_tone == Tone::Ok && h.agent_tone == Tone::Ok);
    CHECK(h.usb);
    CHECK(h.battery.kind == Kind::Level && h.battery.percent == 78 && h.battery.tone == Tone::Ok && h.battery.charging);
    CHECK_EQ(h.battery.digits, std::string("78"));            // digits only, no percent sign
    h = make_header(s, fresh_power(), NOW, false);             // the percent switched off: the fill stays, the digits go
    CHECK(h.battery.kind == Kind::Level && h.battery.percent == 78 && h.battery.digits.empty());

    // Tones follow the level; the digits are the level.
    for (auto [soc, tone] : {std::pair<int, Tone>{100, Tone::Ok}, {21, Tone::Ok}, {20, Tone::Warn}, {11, Tone::Warn}, {10, Tone::Danger}, {0, Tone::Danger}}) {
        PowerState p = fresh_power();
        p.soc_percent = soc;
        h = make_header(s, p, NOW, true);
        CHECK(h.battery.tone == tone);
        CHECK_EQ(h.battery.percent, soc);
        CHECK_EQ(h.battery.digits, std::to_string(soc));
    }
    // Unknown is never 0 %: no fill, and a question mark instead of digits, also when the percent is switched off.
    PowerState p = fresh_power();
    p.soc_percent = -1;
    for (bool percent : {true, false}) {
        h = make_header(s, p, NOW, percent);
        CHECK(h.battery.kind == Kind::Unknown && h.battery.percent == -1 && h.battery.tone == Tone::Off);
        CHECK_EQ(h.battery.digits, std::string("?"));
    }
    // A level outside 0..100 is not a level.
    for (int bad : {101, 150, 255, -2, -100}) {
        p = fresh_power();
        p.soc_percent = bad;
        h = make_header(s, p, NOW, true);
        CHECK(h.battery.kind == Kind::Unknown && h.battery.percent == -1);
        CHECK_EQ(h.battery.digits, std::string("?"));
    }
    // No battery: the level in the sample does not count, and nothing charges.
    p = fresh_power();
    p.battery = "absent";
    p.soc_percent = 55;
    h = make_header(s, p, NOW, true);
    CHECK(h.battery.kind == Kind::None && h.battery.percent == -1 && h.battery.digits.empty() && !h.battery.charging);
    CHECK(h.usb);                                              // the USB is still there
    // The battery state itself unknown: no level either.
    p = fresh_power();
    p.battery = "unknown";
    h = make_header(s, p, NOW, true);
    CHECK(h.battery.kind == Kind::Unknown && h.battery.percent == -1 && h.battery.digits != "78" && !h.battery.charging);   // no bolt for a battery not known to be there
    // Stale: nothing about the old sample is shown, not the USB and not the bolt either.
    p = fresh_power();
    p.observed_ms = NOW - 60000;
    h = make_header(s, p, NOW, true);
    CHECK(h.battery.kind == Kind::Unknown && h.battery.percent == -1 && !h.usb && !h.battery.charging);
    // Fault.
    p = fresh_power();
    p.fault = true;
    h = make_header(s, p, NOW, true);
    CHECK(h.battery.kind == Kind::Fault && h.battery.percent == -1 && h.battery.tone == Tone::Warn && !h.usb && !h.battery.charging);
    CHECK_EQ(h.battery.digits, std::string("!"));
    // USB and the bolt: the bolt is only for "charging", the USB icon only for a USB that is there.
    p = fresh_power(); p.vbus = "absent"; p.charging = "idle";
    h = make_header(s, p, NOW, true);
    CHECK(!h.usb && !h.battery.charging);
    p = fresh_power(); p.charging = "full";
    h = make_header(s, p, NOW, true);
    CHECK(h.usb && !h.battery.charging && h.battery.kind == Kind::Level);
    p = fresh_power(); p.charging = "idle";
    CHECK(make_header(s, p, NOW, true).usb && !make_header(s, p, NOW, true).battery.charging);
    p = fresh_power(); p.vbus = "unknown"; p.charging = "unknown";
    h = make_header(s, p, NOW, true);
    CHECK(!h.usb && !h.battery.charging);
    p = fresh_power(); p.soc_percent = -1;                     // charging, level unknown: the chip says charging, so the bolt shows
    h = make_header(s, p, NOW, true);
    CHECK(h.battery.kind == Kind::Unknown && h.battery.charging);

    // Wi-Fi and the computer: an icon with a slash (Off) unless it is connected; connecting is its own state (it blinks).
    Snapshot w = s;
    for (int rssi : {0, 1, 20, -128, -500, -61}) {             // the signal strength is not part of the icon
        w.rssi = rssi;
        CHECK(make_header(w, fresh_power(), NOW, true).wifi == LinkIcon::Ok);
    }
    w = s; w.wifi_ready = false; w.wifi = "未配置";
    h = make_header(w, fresh_power(), NOW, true);
    CHECK(h.wifi == LinkIcon::Off && h.wifi_tone == Tone::Off);
    w.wifi = "正在连接Wi-Fi";
    h = make_header(w, fresh_power(), NOW, true);
    CHECK(h.wifi == LinkIcon::Busy && h.wifi_tone == Tone::Info);
    w.wifi = "Wi-Fi初始化失败";
    h = make_header(w, fresh_power(), NOW, true);
    CHECK(h.wifi == LinkIcon::Off && h.wifi_tone == Tone::Warn);
    w.wifi = "";
    h = make_header(w, fresh_power(), NOW, true);
    CHECK(h.wifi == LinkIcon::Off && h.wifi_tone == Tone::Off);
    w = s; w.agent_ready = false;
    h = make_header(w, fresh_power(), NOW, true);
    CHECK(h.agent == LinkIcon::Off && h.agent_tone == Tone::Off && h.wifi == LinkIcon::Ok);
}
// The icons that are connecting blink: on for the first half of each second.
static void t10b_blink() {
    CHECK(blink_on(0) && blink_on(1) && blink_on(499));
    CHECK(!blink_on(500) && !blink_on(999));
    CHECK(blink_on(1000) && blink_on(1499) && !blink_on(1500));
    CHECK(blink_on(-1) && blink_on(-500));                      // a clock that runs backwards must not break it
    for (int64_t t = 0; t < 10000; t += 100) CHECK(blink_on(t) == blink_on(t + 1000));
}
static void t11_ring() {
    const Snapshot s = online_snapshot();
    RingView r = make_ring(s, fresh_power(), NOW);
    CHECK(r.wifi && r.agent && r.power && r.time);
    Snapshot w = s;
    w.wifi_ready = false; r = make_ring(w, fresh_power(), NOW); CHECK(!r.wifi && r.agent && r.power && r.time);
    w = s; w.agent_ready = false; r = make_ring(w, fresh_power(), NOW); CHECK(r.wifi && !r.agent && r.power && r.time);
    w = s; w.time_valid = false; r = make_ring(w, fresh_power(), NOW); CHECK(r.wifi && r.agent && r.power && !r.time);
    PowerState p = fresh_power();
    p.observed_ms = NOW - 7000; CHECK(!make_ring(s, p, NOW).power);
    p = fresh_power(); p.fault = true; CHECK(!make_ring(s, p, NOW).power);
    p = fresh_power(); p.battery = "absent"; p.vbus = "absent"; CHECK(!make_ring(s, p, NOW).power);
    p = fresh_power(); p.battery = "absent"; CHECK(make_ring(s, p, NOW).power);   // USB alone powers the desk hub
    p = fresh_power(); p.vbus = "absent"; CHECK(make_ring(s, p, NOW).power);      // battery alone
    p = fresh_power(); p.vbus = "unknown"; p.battery = "unknown"; CHECK(!make_ring(s, p, NOW).power);
}

// ---- T12 ----
static void t12_clock() {
    setenv("TZ", "UTC", 1);
    tzset();
    ClockView c = make_clock(Lang::ZhCN, true, 1700000000);  // 2023-11-14 22:13:20 UTC, a Tuesday
    CHECK(c.valid);
    CHECK_EQ(c.hhmm, std::string("22:13"));
    CHECK_EQ(c.seconds, std::string("20"));
    CHECK_EQ(c.date, std::string("11月14日 星期二"));
    CHECK(renderable(c.date, font_glyphs()));
    CHECK(renderable(make_clock(Lang::ZhCN, false, 0).date, font_glyphs()));
    c = make_clock(Lang::EnUS, true, 1700000000);
    CHECK_EQ(c.date, std::string("Tue, Nov 14"));
    setenv("TZ", "CST-8", 1);
    tzset();
    c = make_clock(Lang::ZhCN, true, 1700000000);  // +8 h: 2023-11-15 06:13:20, a Wednesday
    CHECK_EQ(c.hhmm, std::string("06:13"));
    CHECK_EQ(c.date, std::string("11月15日 星期三"));
    setenv("TZ", "UTC", 1);
    tzset();
    for (time_t bad : {static_cast<time_t>(0), static_cast<time_t>(-1), static_cast<time_t>(86400), static_cast<time_t>(1577836799)}) {
        c = make_clock(Lang::ZhCN, true, bad);  // before 2020-01-01 the clock was never set
        CHECK(!c.valid);
        CHECK_EQ(c.hhmm, std::string("--:--"));
        CHECK(c.seconds.empty());
        CHECK(c.date.find("1970") == std::string::npos && c.date.find("1月1日") == std::string::npos);
    }
    c = make_clock(Lang::ZhCN, false, 1700000000);
    CHECK(!c.valid);
    CHECK_EQ(c.hhmm, std::string("--:--"));
    CHECK_EQ(c.date, std::string("时间未同步"));
    CHECK_EQ(make_clock(Lang::EnUS, false, 1700000000).date, std::string("Time not synced"));
}

// ---- T13-T14 ----
static void t13_percent() {
    Gauge g = parse_percent("23.4%", true);
    CHECK(g.valid && std::fabs(g.percent - 23.4f) < 1e-4f);
    CHECK_EQ(g.text, std::string("23%"));
    CHECK(parse_percent("0%", true).valid && parse_percent("100%", true).valid && parse_percent("100.0%", true).valid);
    CHECK_EQ(parse_percent("0%", true).text, std::string("0%"));  // a measured zero is a zero
    for (const char *bad : {"--", "", "不可用", "NaN%", "nan%", "inf%", "-inf%", "-1%", "-0.1%", "100.1%", "150%", "23.4", "23.4%x", "%", ".%", "1e2%", "0x10%", "23,4%", "+23%", "23 %", " 23%", "23%\n"}) {
        g = parse_percent(bad, true);
        if (g.valid) std::fprintf(stderr, "accepted: \"%s\"\n", bad);
        CHECK(!g.valid);
        CHECK_EQ(g.text, std::string("--"));
        CHECK(g.percent == 0.0f);
    }
    g = parse_percent("23.4%", false);  // stale data is not shown even if it parses
    CHECK(!g.valid);
    CHECK_EQ(g.text, std::string("--"));
}
static void t14_computer() {
    Snapshot s = online_snapshot();
    ComputerView c = make_computer(Lang::ZhCN, s, NOW);
    CHECK(c.cpu.valid && c.memory.valid);
    CHECK_EQ(c.cpu.text, std::string("23%"));
    CHECK_EQ(c.memory.text, std::string("61%"));
    CHECK_EQ(c.rx, std::string("1.2 MB/s"));
    CHECK_EQ(c.tx, std::string("0.3 MB/s"));
    CHECK_EQ(c.footer, std::string("电脑指标 · 数据有效"));
    CHECK(c.footer_tone == Tone::Ok);
    // Expired.
    s.metrics_expires_ms = NOW;
    CHECK(!metrics_fresh(s, NOW));
    c = make_computer(Lang::ZhCN, s, NOW);
    CHECK(!c.cpu.valid && !c.memory.valid);
    CHECK_EQ(c.rx, std::string("--")); CHECK_EQ(c.tx, std::string("--"));
    CHECK_EQ(c.footer, std::string("指标已过期 · 等待刷新"));
    CHECK(c.footer_tone == Tone::Warn);
    s.metrics_expires_ms = NOW + 1;
    CHECK(metrics_fresh(s, NOW));
    // Never sampled.
    s = online_snapshot();
    s.sampled_ms = 0;
    s.metrics_expires_ms = NOW + 8000;
    CHECK(!metrics_fresh(s, NOW));
    c = make_computer(Lang::ZhCN, s, NOW);
    CHECK(!c.cpu.valid);
    CHECK_EQ(c.footer, std::string("尚未采样 · 等待电脑连接"));
    CHECK(c.footer_tone == Tone::Off);
    // The Agent says a metric is unavailable.
    s = online_snapshot();
    s.cpu = "不可用"; s.network = "不可用";
    c = make_computer(Lang::ZhCN, s, NOW);
    CHECK(!c.cpu.valid && c.memory.valid);
    CHECK_EQ(c.rx, std::string("--"));
    CHECK_EQ(c.tx, std::string("0.3 MB/s"));
    s.cpu = "99.9%"; c = make_computer(Lang::EnUS, s, NOW);
    CHECK_EQ(c.cpu.text, std::string("100%"));
    CHECK(ascii(c.footer));
}

// ---- T15 ----
static void t15_power_view() {
    PowerView v = make_power(Lang::ZhCN, fresh_power(), NOW);
    CHECK(v.level.valid && v.level.percent == 78.0f);
    CHECK_EQ(v.level.text, std::string("78%"));
    CHECK_EQ(v.level_note, std::string("PMIC 估算，未验证"));
    CHECK_EQ(v.battery_v, std::string("3.92 V"));
    CHECK_EQ(v.usb_v, std::string("5.12 V"));
    CHECK_EQ(v.chip_temp, std::string("41.3 度"));
    CHECK_EQ(v.charge, std::string("充电中"));
    CHECK(v.charge_tone == Tone::Info);
    CHECK(v.battery_v_note.empty() && v.usb_v_note.empty() && v.temp_note.empty());
    CHECK_EQ(make_power(Lang::EnUS, fresh_power(), NOW).chip_temp, std::string("41.3 C"));
    // Rounding.
    PowerState p = fresh_power();
    p.battery_mv = 3925; p.vbus_mv = 5004;
    v = make_power(Lang::ZhCN, p, NOW);
    CHECK_EQ(v.battery_v, std::string("3.93 V"));
    CHECK_EQ(v.usb_v, std::string("5.00 V"));
    p.battery_mv = 3924; v = make_power(Lang::ZhCN, p, NOW); CHECK_EQ(v.battery_v, std::string("3.92 V"));
    // The three board failure reasons.
    const std::vector<std::pair<std::string, Str>> reasons = {{"ADC_ENABLE_FAILED", Str::ReadingEnableFailed}, {"ADC_READ_FAILED", Str::ReadingReadFailed}, {"ADC_OUT_OF_RANGE", Str::ReadingOutOfRange}};
    for (const auto &r : reasons) {
        p = fresh_power();
        p.measure_reason = r.first; p.battery_mv = -1; p.vbus_mv = -1; p.chip_temp_c = NAN;
        v = make_power(Lang::ZhCN, p, NOW);
        CHECK_EQ(v.battery_v, std::string("--")); CHECK_EQ(v.usb_v, std::string("--")); CHECK_EQ(v.chip_temp, std::string("--"));
        CHECK_EQ(v.battery_v_note, std::string(tr(Lang::ZhCN, r.second)));
        CHECK_EQ(v.usb_v_note, std::string(tr(Lang::ZhCN, r.second)));
        CHECK_EQ(v.temp_note, std::string(tr(Lang::ZhCN, r.second)));
    }
    // Values left over in a failed sample are not shown.
    p = fresh_power(); p.measure_reason = "ADC_READ_FAILED";
    v = make_power(Lang::ZhCN, p, NOW);
    CHECK_EQ(v.battery_v, std::string("--")); CHECK_EQ(v.usb_v, std::string("--")); CHECK_EQ(v.chip_temp, std::string("--"));
    // An unknown reason is not "OK".
    p = fresh_power(); p.measure_reason = "SOMETHING_NEW";
    v = make_power(Lang::ZhCN, p, NOW);
    CHECK_EQ(v.battery_v, std::string("--"));
    // The model checks the range again, even when the board says ADC_OK.
    for (int mv : {0, 1, 2499, 4601, 9999, 65535}) { p = fresh_power(); p.battery_mv = mv; v = make_power(Lang::ZhCN, p, NOW); CHECK_EQ(v.battery_v, std::string("--")); CHECK(!v.battery_v_note.empty()); }
    for (int mv : {0, 2999, 6501, 20000}) { p = fresh_power(); p.vbus_mv = mv; v = make_power(Lang::ZhCN, p, NOW); CHECK_EQ(v.usb_v, std::string("--")); CHECK(!v.usb_v_note.empty()); }
    for (float t : {-40.1f, 150.1f, 400.0f, NAN, INFINITY, -INFINITY}) { p = fresh_power(); p.chip_temp_c = t; v = make_power(Lang::ZhCN, p, NOW); CHECK_EQ(v.chip_temp, std::string("--")); CHECK(!v.temp_note.empty()); }
    p = fresh_power(); p.chip_temp_c = -40.0f; CHECK_EQ(make_power(Lang::EnUS, p, NOW).chip_temp, std::string("-40.0 C"));
    // No battery / no USB: nothing to measure, with the reason.
    p = fresh_power(); p.battery = "absent"; p.battery_mv = -1; p.soc_percent = 40;
    v = make_power(Lang::ZhCN, p, NOW);
    CHECK_EQ(v.battery_v, std::string("--")); CHECK_EQ(v.battery_v_note, std::string("未装电池"));
    CHECK(!v.level.valid); CHECK_EQ(v.level_note, std::string("未装电池"));
    p = fresh_power(); p.vbus = "absent"; p.vbus_mv = -1;
    v = make_power(Lang::ZhCN, p, NOW);
    CHECK_EQ(v.usb_v, std::string("--")); CHECK_EQ(v.usb_v_note, std::string("USB未接"));
    // Stale.
    p = fresh_power(); p.observed_ms = NOW - 9000;
    v = make_power(Lang::ZhCN, p, NOW);
    CHECK(!v.level.valid); CHECK_EQ(v.level.text, std::string("--"));
    CHECK_EQ(v.level_note, std::string("电源数据待更新"));
    CHECK_EQ(v.battery_v, std::string("--")); CHECK_EQ(v.usb_v, std::string("--")); CHECK_EQ(v.chip_temp, std::string("--"));
    CHECK_EQ(v.charge, std::string("电源待测")); CHECK(v.charge_tone == Tone::Off);
    // Fault.
    p = fresh_power(); p.fault = true; p.measure_reason = "";
    v = make_power(Lang::ZhCN, p, NOW);
    CHECK(!v.level.valid); CHECK_EQ(v.level_note, std::string("电源读取异常"));
    CHECK_EQ(v.battery_v, std::string("--")); CHECK_EQ(v.charge, std::string("读取失败")); CHECK(v.charge_tone == Tone::Warn);
    // Unknown level with a healthy battery.
    p = fresh_power(); p.soc_percent = -1;
    v = make_power(Lang::ZhCN, p, NOW);
    CHECK(!v.level.valid); CHECK_EQ(v.level.text, std::string("--")); CHECK_EQ(v.level_note, std::string("电量未知"));
    CHECK_EQ(v.battery_v, std::string("3.92 V"));  // the voltage is a separate reading
    // Charge texts.
    p = fresh_power(); p.charging = "full"; CHECK_EQ(make_power(Lang::ZhCN, p, NOW).charge, std::string("已充满"));
    p.charging = "idle"; CHECK_EQ(make_power(Lang::ZhCN, p, NOW).charge, std::string("未充电"));
    p.charging = "unknown"; CHECK_EQ(make_power(Lang::ZhCN, p, NOW).charge, std::string("未知"));
    p.charging = "garbage"; CHECK_EQ(make_power(Lang::ZhCN, p, NOW).charge, std::string("未知"));
}

// ---- T16-T18 ----
static std::string row(const std::vector<ConnectRow> &rows, const std::string &key) {
    for (const auto &r : rows) if (r.key == key) return r.value;
    return "<missing>";
}
static void t16_connect() {
    Snapshot s = online_snapshot();
    auto rows = make_connect(Lang::ZhCN, s, true);
    CHECK_EQ(rows.size(), static_cast<size_t>(6));
    CHECK_EQ(row(rows, tr(Lang::ZhCN, Str::KeyWifi)), std::string("已连接"));
    CHECK_EQ(row(rows, tr(Lang::ZhCN, Str::KeySignal)), std::string("-61 dBm"));
    CHECK_EQ(row(rows, tr(Lang::ZhCN, Str::KeyIp)), std::string("10.10.10.23"));
    CHECK_EQ(row(rows, tr(Lang::ZhCN, Str::KeyAgent)), std::string("在线"));
    CHECK_EQ(row(rows, tr(Lang::ZhCN, Str::KeyPairing)), std::string("已配对"));
    CHECK_EQ(row(rows, tr(Lang::ZhCN, Str::KeyDeviceId)), std::string("dh-0a1b2c"));
    // The Wi-Fi row names the network; a dropped link still says which network it is about.
    CHECK_EQ(row(make_connect(Lang::ZhCN, s, true, "Office"), tr(Lang::ZhCN, Str::KeyWifi)), std::string("Office · 已连接"));
    CHECK_EQ(row(make_connect(Lang::EnUS, s, true, "Office"), tr(Lang::EnUS, Str::KeyWifi)), std::string("Office - Connected"));   // English pages stay ASCII
    { Snapshot off = s; off.wifi_ready = false; off.wifi = "正在连接Wi-Fi"; CHECK_EQ(row(make_connect(Lang::ZhCN, off, true, "Office"), tr(Lang::ZhCN, Str::KeyWifi)), std::string("Office · 正在连接")); }
    s.wifi_ready = false; s.wifi = "正在连接Wi-Fi"; s.agent_ready = false;
    rows = make_connect(Lang::ZhCN, s, false);
    CHECK_EQ(row(rows, tr(Lang::ZhCN, Str::KeyWifi)), std::string("正在连接"));
    CHECK_EQ(row(rows, tr(Lang::ZhCN, Str::KeyIp)), std::string("--"));        // the IP of a dropped link is not shown
    CHECK_EQ(row(rows, tr(Lang::ZhCN, Str::KeySignal)), std::string("--"));    // nor its signal
    CHECK_EQ(row(rows, tr(Lang::ZhCN, Str::KeyAgent)), std::string("离线"));
    CHECK_EQ(row(rows, tr(Lang::ZhCN, Str::KeyPairing)), std::string("未配对"));
    s = online_snapshot();
    for (int bad : {0, 5, -128, -999}) { s.rssi = bad; CHECK_EQ(row(make_connect(Lang::ZhCN, s, true), tr(Lang::ZhCN, Str::KeySignal)), std::string("--")); }
    s = online_snapshot(); s.ip = "";
    CHECK_EQ(row(make_connect(Lang::ZhCN, s, true), tr(Lang::ZhCN, Str::KeyIp)), std::string("--"));
    s = online_snapshot(); s.device_id = "";
    CHECK_EQ(row(make_connect(Lang::ZhCN, s, true), tr(Lang::ZhCN, Str::KeyDeviceId)), std::string("--"));
    for (const auto &r : make_connect(Lang::EnUS, online_snapshot(), true)) CHECK(ascii(r.key) && ascii(r.value));
}
static void t17_actions() {
    Snapshot s = online_snapshot();
    s.actions[0].id = "lock"; s.actions[0].label = "锁定屏幕";
    s.actions[1].id = "mute"; s.actions[1].label = "";
    s.action_count = 2;
    auto a = make_actions(Lang::ZhCN, s);
    CHECK_EQ(a.size(), static_cast<size_t>(2));
    CHECK_EQ(a[0].label, std::string("锁定屏幕")); CHECK(a[0].enabled); CHECK(a[0].sub.empty());
    CHECK_EQ(a[1].label, std::string("mute"));    // an empty label shows the id, never a blank button
    s.agent_ready = false;
    a = make_actions(Lang::ZhCN, s);
    CHECK(!a[0].enabled && !a[1].enabled);
    CHECK_EQ(a[0].sub, std::string("Agent 未连接"));
    CHECK_EQ(make_actions(Lang::EnUS, s)[0].sub, std::string("Agent offline"));
    s.action_count = 99;  // a corrupt count must not read outside the array
    CHECK_EQ(make_actions(Lang::ZhCN, s).size(), static_cast<size_t>(12));
    s.action_count = 0;
    CHECK(make_actions(Lang::ZhCN, s).empty());
    s.action_count = 1; s.actions[0].id = ""; s.actions[0].label = "";
    CHECK_EQ(make_actions(Lang::ZhCN, s).size(), static_cast<size_t>(1));  // id and label both empty: no crash
}
static void t18_notice() {
    CHECK_EQ(notice_text(Lang::ZhCN, "已接收，等待执行", NOW, NOW - 1000), std::string("已接收，等待执行"));
    CHECK_EQ(notice_text(Lang::EnUS, "已接收，等待执行", NOW, NOW - 1000), std::string("Received, waiting to run"));
    CHECK_EQ(notice_text(Lang::ZhCN, "已接收，等待执行", NOW, NOW - kNoticeMs + 1), std::string("已接收，等待执行"));
    CHECK_EQ(notice_text(Lang::ZhCN, "已接收，等待执行", NOW, NOW - kNoticeMs), std::string(""));
    CHECK_EQ(notice_text(Lang::ZhCN, "已接收，等待执行", NOW, NOW - 600000), std::string(""));
    CHECK_EQ(notice_text(Lang::ZhCN, "尚无操作", NOW, NOW), std::string(""));       // the idle text is not a notice
    CHECK_EQ(notice_text(Lang::ZhCN, "", NOW, NOW), std::string(""));
    CHECK_EQ(notice_text(Lang::ZhCN, "已接收，等待执行", NOW, NOW + 5), std::string(""));  // time must not run backwards
}

// ---- T22: the quiet hours of the sound (docs/design Dev-Sound) ----
// Failure modes: a sound at night because the clock is not known; a night limit that is off still silencing the device;
// the window across midnight read as one that does not. The display treats an unknown time as night while the limit is on
// (policy.cpp), and the sound follows it.
static void t22_quiet_hours() {
    DisplayConfig c;
    for (uint16_t minute = 0; minute < 1440; ++minute) {
        CHECK_EQ(quiet_hours(c, true, minute), dh::in_night(minute, c.night_start, c.night_end));
        CHECK(quiet_hours(c, false, minute));                        // no valid time: the night is not known, so the device stays quiet
    }
    CHECK(quiet_hours(c, true, 23 * 60 + 30) && quiet_hours(c, true, 3 * 60) && quiet_hours(c, true, 22 * 60) && !quiet_hours(c, true, 12 * 60));
    c.night_enabled = false;
    for (uint16_t minute = 0; minute < 1440; ++minute) CHECK(!quiet_hours(c, true, minute) && !quiet_hours(c, false, minute));   // the limit is off: no quiet hours
    c.night_enabled = true; c.night_start = 60; c.night_end = 300;   // a window inside one day
    CHECK(quiet_hours(c, true, 120) && !quiet_hours(c, true, 23 * 60) && !quiet_hours(c, true, 400));
}

// ---- T21: the ring, explained (docs/design Dev-RingInfo) ----
// Failure modes: a row that says "fine" for a segment that is dark; a reason that is made up (the Wi-Fi and pairing words come
// from what the device knows, the power words from the sample); a power fault read as "no reading"; the order of the rows
// not the order of the ring (Wi-Fi, computer, power, time); the line under the ring naming the wrong segment, or naming
// one when all four are lit; a missing English text; any row without a name, a state and a note.
static void t21_ring_info() {
    const Snapshot s = online_snapshot();
    for (Lang l : {Lang::ZhCN, Lang::EnUS}) {
        RingInfoView v = make_ring_info(l, s, fresh_power(), NOW);
        CHECK_EQ(v.first_problem, -1);
        for (const RingRow &r : v.row) CHECK(r.ok && !r.name.empty() && !r.state.empty() && !r.note.empty());
        CHECK_EQ(ring_note(l, v), tr(l, Str::RingHint));
    }
    const RingInfoView ok = make_ring_info(Lang::ZhCN, s, fresh_power(), NOW);
    CHECK_EQ(ok.row[0].name, "网络"); CHECK_EQ(ok.row[1].name, "电脑"); CHECK_EQ(ok.row[2].name, "电源"); CHECK_EQ(ok.row[3].name, "时间");
    CHECK_EQ(ok.row[0].state, "已连接"); CHECK_EQ(ok.row[1].state, "已连接"); CHECK_EQ(ok.row[2].state, "正常"); CHECK_EQ(ok.row[3].state, "有效");
    CHECK_EQ(ring_note(Lang::ZhCN, ok), "点圆环看说明");
    // Every segment on its own, and the ring agrees: a row is dark exactly when make_ring says the segment is dark.
    struct Case { int segment; const char *name; const char *state; const char *note; const char *line; };
    const Case cases[] = {{0, "网络", "没连上", nullptr, "网络没连上"}, {1, "电脑", "没连上", nullptr, "电脑没连上"},
                          {2, "电源", "无读数", "还没有新的电源读数", "电源没有读数"}, {3, "时间", "未校准", "请用 USB 校时", "时间没校准"}};
    for (const Case &c : cases) {
        Snapshot w = s; PowerState p = fresh_power();
        if (c.segment == 0) { w.wifi_ready = false; w.wifi = "未配置"; }
        if (c.segment == 1) { w.agent_ready = false; }
        if (c.segment == 2) p.observed_ms = NOW - 7000;
        if (c.segment == 3) w.time_valid = false;
        const RingInfoView v = make_ring_info(Lang::ZhCN, w, p, NOW);
        const RingView r = make_ring(w, p, NOW);
        const bool lit[4] = {r.wifi, r.agent, r.power, r.time};
        for (int i = 0; i < 4; ++i) CHECK_EQ(v.row[i].ok, lit[i]);
        CHECK_EQ(v.first_problem, c.segment);
        CHECK_EQ(v.row[c.segment].name, c.name); CHECK_EQ(v.row[c.segment].state, c.state);
        if (c.note) CHECK_EQ(v.row[c.segment].note, c.note);
        CHECK_EQ(ring_note(Lang::ZhCN, v), c.line);
        CHECK(!ring_note(Lang::EnUS, make_ring_info(Lang::EnUS, w, p, NOW)).empty());
        CHECK(ring_note(Lang::EnUS, make_ring_info(Lang::EnUS, w, p, NOW)) != ring_note(Lang::ZhCN, v));
    }
    // The reasons that come from the device. Wi-Fi: what the core reports. Computer: first the Wi-Fi it needs, then the pairing.
    Snapshot w = s; w.wifi_ready = false; w.agent_ready = false; w.wifi = "Wi-Fi密码错误";
    RingInfoView v = make_ring_info(Lang::ZhCN, w, fresh_power(), NOW);
    CHECK_EQ(v.row[0].note, "连接异常");
    CHECK_EQ(v.row[1].note, "要先连上 Wi-Fi");
    CHECK_EQ(v.first_problem, 0);                               // the first dark segment of the ring is the one named
    w.wifi = "正在连接Wi-Fi"; CHECK_EQ(make_ring_info(Lang::ZhCN, w, fresh_power(), NOW).row[0].note, "正在连接");
    w = s; w.agent_ready = false; w.agent = "未配对";
    CHECK_EQ(make_ring_info(Lang::ZhCN, w, fresh_power(), NOW).row[1].note, "未配对");
    w = s; w.agent_ready = false; w.agent = "AGENT_UNREACHABLE";
    CHECK_EQ(make_ring_info(Lang::ZhCN, w, fresh_power(), NOW).row[1].note, "未连接");
    // A power fault is a fault, not a missing reading; an old sample is a missing reading, not a fault.
    PowerState p = fresh_power(); p.fault = true;
    v = make_ring_info(Lang::ZhCN, s, p, NOW);
    CHECK(!v.row[2].ok); CHECK_EQ(v.row[2].state, "故障"); CHECK_EQ(v.row[2].note, "电源有故障");
    CHECK_EQ(ring_note(Lang::ZhCN, v), "电源有故障");              // the line says fault, not "no reading"
    CHECK_EQ(v.first_problem, 2);
    p = fresh_power(); p.fault = true; p.observed_ms = NOW - 7000;
    v = make_ring_info(Lang::ZhCN, s, p, NOW);
    CHECK_EQ(v.row[2].state, "无读数");
    // Several at once: all of them are dark, the line names the first.
    w = s; w.wifi_ready = false; w.time_valid = false; w.agent_ready = false;
    v = make_ring_info(Lang::ZhCN, w, fresh_power(), NOW);
    CHECK(!v.row[0].ok && !v.row[1].ok && v.row[2].ok && !v.row[3].ok);
    CHECK_EQ(ring_note(Lang::ZhCN, v), "网络没连上");
    // No text is empty in either language, for any combination of the four.
    for (int mask = 0; mask < 16; ++mask) {
        Snapshot c = s; PowerState q = fresh_power();
        if (mask & 1) c.wifi_ready = false;
        if (mask & 2) c.agent_ready = false;
        if (mask & 4) q.observed_ms = NOW - 7000;
        if (mask & 8) c.time_valid = false;
        for (Lang l : {Lang::ZhCN, Lang::EnUS}) {
            const RingInfoView x = make_ring_info(l, c, q, NOW);
            for (const RingRow &r : x.row) CHECK(!r.name.empty() && !r.state.empty() && !r.note.empty());
            CHECK_EQ(x.first_problem == -1, mask == 0);
            CHECK(!ring_note(l, x).empty());
        }
    }
}

// ---- T20: the digits in the battery icon stay readable; the ring tiers ----
static void t20_battery_fill_and_rings() {
    CHECK_EQ(blend(0xffffff, 0x000000, 50), 0x808080u);      // half way, rounded half up
    CHECK_EQ(blend(0x123456, 0xabcdef, 0), 0xabcdefu);
    CHECK_EQ(blend(0x123456, 0xabcdef, 100), 0x123456u);
    CHECK_EQ(blend(0xff0000, 0x000000, 40), 0x660000u);
    CHECK_EQ(blend(0x00ff00, 0x0000ff, 25), 0x0040bfu);
    // The digits are drawn in ink over the level colour at kBatteryFillPercent: that must pass the 4.5:1 of rules R9,
    // on every surface it can sit on, in every theme, for every colour a level can have.
    for (ThemeId id : kThemes) {
        const Palette &p = theme_palette(id);
        for (uint32_t tone : {p.agent, p.power, p.danger})
            for (uint32_t under : {p.bg, p.bg2, p.card})
                CHECK(contrast_ratio(p.ink, blend(tone, under, kBatteryFillPercent)) >= 4.5);
    }
    // A stronger fill would fail: the number is not chosen to hide the problem.
    bool some_fail = false;
    for (ThemeId id : kThemes) {
        const Palette &p = theme_palette(id);
        for (uint32_t tone : {p.agent, p.power, p.danger}) some_fail = some_fail || contrast_ratio(p.ink, blend(tone, p.bg, 60)) < 4.5;
    }
    CHECK(some_fail);

    const RingPx thin = ring_px(RingTier::Thin), standard = ring_px(RingTier::Standard), thick = ring_px(RingTier::Thick);
    CHECK(thin.home < standard.home && standard.home < thick.home);
    CHECK(thin.gauge < standard.gauge && standard.gauge < thick.gauge);
    CHECK(standard.home == 20 && standard.gauge == 11);        // what the screen had before the tiers
    CHECK(thin.home == 12 && thin.gauge == 7 && thick.home == 28 && thick.gauge == 15);   // the numbers of docs/design/README.md
    CHECK(thin.gauge >= 4 && thin.home >= 6);                   // never a hair line
    CHECK(thick.home <= 30 && thick.gauge <= 16);               // and the clock in the ring still has room: 216 px ring, 176 px text
}

static charge::Status charge_state(charge::State state, const char *reason = "", int limit = 80, bool switch_known = true) {
    charge::Status s;
    s.state = state;
    s.reason = reason;
    s.enabled = state != charge::State::Off;
    s.limit = limit;
    s.resume = limit - 5;
    s.charging_known = switch_known;
    s.charging = switch_known && state != charge::State::Holding && state != charge::State::SafetyStop;
    return s;
}
static void t19_charge_view() {
    using charge::State;
    const PowerState good = fresh_power();
    ChargeView v = make_charge(Lang::ZhCN, charge_state(State::Off), good, NOW);       // U2: off
    CHECK_EQ(v.strip, std::string("充电保护：未开启"));
    CHECK_EQ(v.headline, std::string("充电保护：未开启"));
    CHECK_EQ(v.button, std::string("开启…"));
    CHECK(v.button_enabled && v.tone == Tone::Off && v.why_disabled.empty());
    v = make_charge(Lang::EnUS, charge_state(State::Off), good, NOW);
    CHECK_EQ(v.strip, std::string("Charge protection: off"));
    CHECK_EQ(v.button, std::string("Turn on..."));

    PowerState p = good;                                                                // U3: it could not work
    p.battery = "absent";
    v = make_charge(Lang::ZhCN, charge_state(State::Off), p, NOW);
    CHECK(!v.button_enabled); CHECK_EQ(v.why_disabled, std::string("未检测到电池。"));
    p = good; p.battery = "unknown";
    CHECK(!make_charge(Lang::ZhCN, charge_state(State::Off), p, NOW).button_enabled);
    p = good; p.observed_ms = NOW - 6001;
    v = make_charge(Lang::ZhCN, charge_state(State::Off), p, NOW);
    CHECK(!v.button_enabled); CHECK_EQ(v.why_disabled, std::string("读不到电量。"));
    p = good; p.observed_ms = 0;
    CHECK(!make_charge(Lang::ZhCN, charge_state(State::Off), p, NOW).button_enabled);
    p = good; p.fault = true;
    CHECK(!make_charge(Lang::ZhCN, charge_state(State::Off), p, NOW).button_enabled);
    p = good; p.soc_percent = -1;
    CHECK(!make_charge(Lang::ZhCN, charge_state(State::Off), p, NOW).button_enabled);
    v = make_charge(Lang::ZhCN, charge_state(State::Off, "", 80, false), good, NOW);
    CHECK(!v.button_enabled); CHECK_EQ(v.why_disabled, std::string("读不到充电开关。"));
    CHECK_EQ(make_charge(Lang::EnUS, charge_state(State::Off, "", 80, false), good, NOW).why_disabled, std::string("The charge switch cannot be read."));

    v = make_charge(Lang::ZhCN, charge_state(State::Charging), good, NOW);              // on
    CHECK_EQ(v.strip, std::string("充电保护中：上限 80%"));
    CHECK_EQ(v.headline, std::string("充电保护中：上限 80%"));
    CHECK_EQ(v.note, std::string("电量到上限后停止充电。"));
    CHECK_EQ(v.button, std::string("关闭"));
    CHECK(v.button_enabled && v.tone == Tone::Info);
    CHECK_EQ(make_charge(Lang::ZhCN, charge_state(State::Charging, "", 90), good, NOW).strip, std::string("充电保护中：上限 90%"));
    CHECK_EQ(make_charge(Lang::EnUS, charge_state(State::Charging), good, NOW).strip, std::string("Charge protection on: limit 80%"));
    CHECK_EQ(make_charge(Lang::EnUS, charge_state(State::Charging), good, NOW).button, std::string("Turn off"));
    v = make_charge(Lang::ZhCN, charge_state(State::Holding, "LEVEL_AT_LIMIT"), good, NOW);
    CHECK_EQ(v.strip, std::string("充电保护中：上限 80%"));
    CHECK_EQ(v.note, std::string("已停止充电，低于回充线后恢复。"));
    CHECK(v.tone == Tone::Info && v.button_enabled);
    v = make_charge(Lang::ZhCN, charge_state(State::SafetyStop, "VOLTAGE_LIMIT"), good, NOW);
    CHECK_EQ(v.note, std::string("电压或温度过高，已停止充电。"));
    CHECK(v.tone == Tone::Warn && v.button_enabled);

    p = good; p.battery = "absent";                                                     // switching off is always possible
    CHECK(make_charge(Lang::ZhCN, charge_state(State::Failsafe, "NO_BATTERY"), p, NOW).button_enabled);
    CHECK_EQ(make_charge(Lang::ZhCN, charge_state(State::Failsafe, "NO_BATTERY"), p, NOW).button, std::string("关闭"));

    const char *unreadable[] = {"SAMPLE_STALE", "NO_SAMPLE", "LEVEL_UNREADABLE", "PMIC_FAULT"};   // failsafe says why
    for (const char *why : unreadable) {
        v = make_charge(Lang::ZhCN, charge_state(State::Failsafe, why), good, NOW);
        CHECK_EQ(v.headline, std::string("充电保护失效，已恢复正常充电"));
        CHECK_EQ(v.strip, v.headline);
        CHECK_EQ(v.note, std::string("读不到电量。"));
        CHECK(v.tone == Tone::Warn);
    }
    CHECK_EQ(make_charge(Lang::ZhCN, charge_state(State::Failsafe, "NO_BATTERY"), good, NOW).note, std::string("未检测到电池。"));
    const char *writes[] = {"STOP_FAILED", "STOP_MISMATCH", "REPEATED_FAILURE", "RESTORED_AFTER_FAILURE"};
    for (const char *why : writes) CHECK_EQ(make_charge(Lang::ZhCN, charge_state(State::Failsafe, why), good, NOW).note, std::string("写入充电开关失败。"));
    CHECK_EQ(make_charge(Lang::ZhCN, charge_state(State::Failsafe, "SOMETHING_NEW"), good, NOW).note, std::string(""));
    CHECK_EQ(make_charge(Lang::EnUS, charge_state(State::Failsafe, "SAMPLE_STALE"), good, NOW).headline,
             std::string("Charge protection failed; normal charging is back"));

    v = make_charge(Lang::ZhCN, charge_state(State::Unconfirmed, "SWITCH_UNCONFIRMED", 80, false), good, NOW);
    CHECK_EQ(v.headline, std::string("无法确认充电状态"));                              // never "stopped" or "restored" here
    CHECK_EQ(v.strip, std::string("无法确认充电状态"));
    CHECK(v.strip.find("已停止") == std::string::npos && v.strip.find("已恢复") == std::string::npos);
    CHECK(v.note.find("已停止") == std::string::npos && v.note.find("已恢复") == std::string::npos);
    CHECK_EQ(v.note, std::string("已多次尝试恢复正常充电。"));
    CHECK(v.tone == Tone::Danger && v.button_enabled);
    CHECK_EQ(make_charge(Lang::EnUS, charge_state(State::Unconfirmed, "", 80, false), good, NOW).headline, std::string("The charge state cannot be confirmed"));

    for (State st : {State::Off, State::Charging, State::Holding, State::SafetyStop, State::Failsafe, State::Unconfirmed}) {   // U4
        for (Lang l : {Lang::ZhCN, Lang::EnUS}) {
            v = make_charge(l, charge_state(st), good, NOW);
            CHECK(!v.strip.empty() && !v.headline.empty() && !v.button.empty());
        }
    }
}

int main() {
    t1_palettes_readable();
    t2_contrast_maths();
    t3_accent_guard();
    t4_accent_choices();
    t5_appearance();
    t6_font_tiers();
    t7_strings();
    t8_dynamic_text();
    t9_freshness();
    t10_header();
    t11_ring();
    t12_clock();
    t13_percent();
    t14_computer();
    t15_power_view();
    t16_connect();
    t17_actions();
    t18_notice();
    t19_charge_view();
    t10b_blink();
    t20_battery_fill_and_rings();
    t21_ring_info();
    t22_quiet_hours();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("ui_model_test: all checks passed");
    return 0;
}
