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
// T14 (gone in 0.3.17: the device no longer shows the computer's CPU, memory and network.)
// T15 Power readings: "--" with a reason note when the ADC failed, the sample is stale or the PMIC faulted;
//     values outside the plausible range are refused even when the board says ADC_OK; rounding is correct.
// T16 Connection rows: no IP or signal while Wi-Fi is down; a signal outside -127..-1 is "--".
// T17 Actions: at most 12, an empty label shows the id, offline actions are disabled with a reason.
// T18 Command notice: only the last 15 seconds, only a real command, only forward in time.
// T19 Charge protection view (docs/charge-protection-design.md, U1 to U4): every state has its words, in both languages;
//     "stopped" and "restored" are never claimed when the state of the switch is not known; the switch-on button is
//     disabled, with the reason, when it could not work; the switch-off button is always there while protection is on;
//     the failsafe words say why, and say nothing when the reason is not known.
// T24 Pages (docs/design v2 Pages): the default is Switch and Overview shown, Clock and Power hidden; the home page is the
//     first SHOWN page, not the first slot; the last shown page cannot be hidden; a move swaps with the neighbour and is
//     refused at the ends and for a slot out of range (nothing changes); the navigation lists the shown pages in order,
//     never empty, never twice. Every order and every non-empty choice round-trips. A text with a page missing or twice,
//     an unknown letter, a bad flag, the wrong length or no page shown is refused, never half-read.
// T25 Appearance, 0.3.17 format (8 fields: + pages, + switch style). Round-trips. The default is the night theme. What
//     0.3.x stored (4 or 6 fields) is still read: the theme becomes night and keeps its colour as the accent (an accent
//     that was chosen stays), the pages and the style are the defaults. 7 or 9 fields, a bad style, bad pages: default.
// T30 The to-do page (13.13 L15): a sixth page "T", last and hidden by default; what 0.3.27 (4 pages) and 0.3.28 to
//     0.3.30 (5 pages) stored is still read, with To-do added last and hidden; still at most four shown.
// T31 The words of the to-do page (13.13 L3 to L7, L11): the states (waiting, off and why, empty, fresh, stale), the groups
//     and their counts in order, the "when" of each kind in both languages, "N more", actions only while fresh and the link
//     is secure, the result lines. Every word is in the font.
// T32 Characters the font does not have (13.13 L8): each one is drawn as "?", variation selectors and joiners are dropped,
//     the rest is left alone; broken UTF-8 never reads past the end.
// T33 The edit page (13.14): the repeat in words ("x" is custom); the month wheel lists 13 months from this month (from the
//     to-do's own month when that is earlier, or when the time is not known), the next year's with the year; the draft starts
//     from the to-do (an undated one at today 09:00), keeps the day inside its month, drops the ring without a time, and
//     gives an edit the protocol accepts; the "when" in words.
// T34 Ringing (M7 to M9, M12): only a to-do with a ring, a time, today's date and this minute; not when the time is not known;
//     once per to-do and due time; one at a time in the list's order; "again in 10 minutes" rings once more, 10 minutes later.
// T35 After the design review (13.15): the weekday of a date (Gregorian, leap years); the head of the due page says the
//     weekday of the chosen date, the day kept inside its month ("到期 · 周四", "Due | Thu"), and only "到期" without a date;
//     the line under the title of the ring page is the list and the repeat ("工作 · 每周"), each only when there is one.
// T26 The night theme: the tokens of docs/design/README.md (v2), pure black, readable like the others (T1, T4).
// T27 Input sources: the port of an input id is read from its words (hdmi, dp/displayport, usbc/usb-c/type-c/thunderbolt,
//     with or without a number), never from a part of a word; anything else is a plain display, with no port tag.
//     The name on the device is the local one, else the App label, else the id; the icon is the local one, else the port's.
//     A local entry for a source that the App no longer sends is not shown. Offline: disabled, with the reason. At most 12.
//     A name is 1 to 32 bytes of printable text (no control characters, valid UTF-8); "" means "use the App's name". An entry
//     with neither a name nor an icon is dropped. At most 12 entries: a 13th source is refused, a known one can still change.
//     The stored text round-trips; a damaged one (bad icon, duplicate id, empty id, control characters, too many, too long,
//     a missing field) is read as nothing at all, never as half a list.
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
static const std::array<ThemeId, 4> kThemes = {ThemeId::Night, ThemeId::Mint, ThemeId::Ocean, ThemeId::Amber};

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
// ---- T26 ----
static void t26_night_theme() {
    const Palette &n = theme_palette(ThemeId::Night);
    CHECK_EQ(n.bg, 0x000000u); CHECK_EQ(n.bg2, 0x000000u);   // pure black: no gradient, nothing lit on an AMOLED
    CHECK_EQ(n.card, 0x070b10u); CHECK_EQ(n.edge, 0x15212bu);
    CHECK_EQ(n.ink, 0xe8f3f7u); CHECK_EQ(n.muted, 0x7f97a3u);
    CHECK_EQ(n.accent, 0x2fd8ffu);
    CHECK_EQ(n.wifi, 0x2fd8ffu); CHECK_EQ(n.agent, 0x59f0a6u); CHECK_EQ(n.power, 0xffb547u); CHECK_EQ(n.clock, 0xb18cffu);
    CHECK(std::string(theme_name(ThemeId::Night)) == "night");
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
                        for (RingTier r : {RingTier::Thin, RingTier::Standard, RingTier::Thick})
                            for (SwitchStyle st : {SwitchStyle::List, SwitchStyle::Icons})
                                for (bool hands : {true, false}) {
                                    Appearance a;
                                    a.theme = t; a.accent = accent; a.lang = l; a.size = z; a.show_percent = percent; a.ring = r; a.style = st; a.clock_hands = hands;
                                    const Appearance b = parse_appearance(serialize(a));
                                    CHECK(b.theme == t && b.accent == accent && b.lang == l && b.size == z && b.show_percent == percent && b.ring == r && b.style == st &&
                                          b.clock_hands == hands);
                                    ++combos;
                                }
    CHECK_EQ(combos, 4 * 3 * 2 * 3 * 2 * 3 * 2 * 2);
    Appearance d;
    CHECK(d.show_percent && d.ring == RingTier::Standard && d.theme == ThemeId::Night && d.style == SwitchStyle::List && d.clock_hands);
    CHECK_EQ(serialize(d), std::string("night|-|zh-CN|standard|pct|standard|S+O+C-P-U-T-|list|hands"));
    Appearance a;
    a.theme = ThemeId::Ocean; a.accent = 0x5ee6b8; a.lang = Lang::EnUS; a.size = SizeTier::Large; a.show_percent = false; a.ring = RingTier::Thick;
    a.style = SwitchStyle::Icons;
    a.pages = {{{View::Clock, true}, {View::Switch, true}, {View::Overview, false}, {View::Power, true}, {View::Usage, false}, {View::Todo, false}}};
    a.clock_hands = false;
    CHECK_EQ(serialize(a), std::string("ocean|5ee6b8|en-US|large|nopct|thick|C+S+O-P+U-T-|icons|nohands"));
    const Appearance back = parse_appearance(serialize(a));
    CHECK_EQ(serialize_pages(back.pages), std::string("C+S+O-P+U-T-"));
    const std::vector<std::string> bad = {
        "", "garbage", "mint|-|zh-CN", "mint|-|zh-CN|standard|pct", "mint|-|zh-CN|standard|pct|standard|extra", "mint|zzzzzz|zh-CN|standard|pct|standard",
        "mint|5EE6B8|zh-CN|standard|pct|standard", "mint|5ee6b|zh-CN|standard|pct|standard", "mint|5ee6b8f|zh-CN|standard|pct|standard",
        "pink|-|zh-CN|standard|pct|standard", "mint|-|fr-FR|standard|pct|standard", "mint|-|zh-CN|huge|pct|standard", "||||||", "mint|-|zh-CN|standard|pct|standard\n",
        "mint|-|zh-CN|standard|PCT|standard", "ocean|-|en-US|large|pct", "ocean|5ee6b8|en-US|large|nopct|thick|", "ocean|-|en-US|large|nopct|", "mint|-|zh-CN|standard|maybe|standard", "mint|-|zh-CN|standard|pct|huge", "mint|-|zh-CN|standard||",
        "mint|zzzzzz|zh-CN|standard", "mint|-|zh-CN|standard\n", std::string(500, 'a'), "mint|-|zh-CN|standard|pct|" + std::string(100, 'x'),
        // the 0.3.17 format: 8 fields exactly, a known style, pages that parse
        "night|-|zh-CN|standard|pct|standard|S+O+C-P-", "night|-|zh-CN|standard|pct|standard|S+O+C-P-|list|", "night|-|zh-CN|standard|pct|standard|S+O+C-P-|grid",
        "night|-|zh-CN|standard|pct|standard|S+S+C-P-|list", "night|-|zh-CN|standard|pct|standard|S-O-C-P-|list", "night|-|zh-CN|standard|pct|standard||list",
        "night|-|zh-CN|standard|pct|standard|S+O+C-P-|LIST", "dusk|-|zh-CN|standard|pct|standard|S+O+C-P-|list", "night|-|zh-CN|standard|pct|standard|S+O+C-P-|list|x",
        // the 0.3.19 format: 9 fields exactly, the hands named exactly
        "night|-|zh-CN|standard|pct|standard|S+O+C-P-|list|nohands|", "night|-|zh-CN|standard|pct|standard|S+O+C-P-|list|", "night|-|zh-CN|standard|pct|standard|S+O+C-P-|list|HANDS",
        "night|-|zh-CN|standard|pct|standard|S+O+C-P-|list|no", "night|-|zh-CN|standard|pct|standard|S+O+C-P-|grid|nohands", "night|-|zh-CN|standard|pct|standard|S+S+C-P-|list|nohands",
        "night|-|zh-CN|standard|pct|standard|S+O+C-P-|list|nohands|nohands", "night|-|zh-CN|standard|pct|standard|S+O+C-P-|list| nohands",
        // a bad hands word throws away the whole text, not only the hands (the other fields here are not the defaults)
        "ocean|5ee6b8|en-US|large|nopct|thick|C+S+O-P+|icons|HANDS", "ocean|5ee6b8|en-US|large|nopct|thick|C+S+O-P+|icons|no",
        // 0.3.x never stored the night theme: a short text that names it is damaged
        "night|-|zh-CN|standard", "night|-|zh-CN|standard|pct|standard"};
    for (const auto &s : bad) {
        const Appearance p = parse_appearance(s);
        const bool same = p.theme == d.theme && p.accent == d.accent && p.lang == d.lang && p.size == d.size && p.show_percent == d.show_percent && p.ring == d.ring &&
                          p.style == d.style && p.clock_hands == d.clock_hands && serialize_pages(p.pages) == serialize_pages(d.pages);
        if (!same) std::fprintf(stderr, "appearance text half-read: [%s]\n", s.c_str());
        CHECK(same);
    }
    CHECK(theme_from_name("nonsense") == ThemeId::Night);
    CHECK(lang_from_name("nonsense") == Lang::ZhCN);
    for (ThemeId t : kThemes) CHECK(theme_from_name(theme_name(t)) == t);
    for (Lang l : {Lang::ZhCN, Lang::EnUS}) CHECK(lang_from_name(lang_name(l)) == l);
}
// ---- T25: what 0.3.x stored moves to the night theme and keeps its colour ----
static void t25_appearance_upgrade() {
    struct Case { const char *stored; uint32_t accent; };
    for (Case c : {Case{"mint|-|zh-CN|standard", 0x5ee6b8u}, Case{"ocean|-|zh-CN|standard", 0u}, Case{"amber|-|zh-CN|standard", 0xff9e4au},
                   Case{"mint|-|en-US|large|nopct|thick", 0x5ee6b8u}, Case{"ocean|-|zh-CN|standard|pct|standard", 0u}, Case{"amber|ff7eb6|zh-CN|small|pct|thin", 0xff7eb6u},
                   Case{"ocean|5ee6b8|en-US|large", 0x5ee6b8u}}) {
        const Appearance p = parse_appearance(c.stored);
        CHECK(p.theme == ThemeId::Night);
        if (p.accent != c.accent) std::fprintf(stderr, "upgrade of %s: accent %06x\n", c.stored, static_cast<unsigned>(p.accent));
        CHECK_EQ(p.accent, c.accent);
        CHECK(p.style == SwitchStyle::List);
        CHECK_EQ(serialize_pages(p.pages), std::string("S+O+C-P-U-T-"));
    }
    const Appearance six = parse_appearance("mint|-|en-US|large|nopct|thick");
    CHECK(six.lang == Lang::EnUS && six.size == SizeTier::Large && !six.show_percent && six.ring == RingTier::Thick);
    // The 0.3.17 format is taken as it is: a classic theme chosen there stays.
    CHECK(parse_appearance("mint|-|zh-CN|standard|pct|standard|S+O+C-P-|list").theme == ThemeId::Mint);
    // Before 0.3.19 there were no hands: what was stored gets them, as a new device does.
    for (const char *s : {"mint|-|zh-CN|standard", "mint|-|en-US|large|nopct|thick", "night|-|zh-CN|standard|pct|standard|S+O+C-P-|icons"}) CHECK(parse_appearance(s).clock_hands);
    const Appearance nine = parse_appearance("night|-|en-US|small|nopct|thin|C+S-O+P-|icons|nohands");
    CHECK(!nine.clock_hands && nine.style == SwitchStyle::Icons && nine.lang == Lang::EnUS && serialize_pages(nine.pages) == "C+S-O+P-U-T-");
    CHECK(parse_appearance("night|-|en-US|small|nopct|thin|C+S-O+P-|icons|hands").clock_hands);
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
// ---- T23: the time zone and the time set by hand (docs/design Dev-Time) ----
// Failure modes: the sign of POSIX TZ is the opposite of what people call the zone, so UTC+8 shown as UTC-8; a step past the end of
// the table; a stored value that is no zone taken as one; a day that does not exist in the new month (Jan 31 + 1 month); a leap day
// that is refused, or accepted in a year that has none; the local time saved as if it were UTC; a year the core will refuse offered.
static void t23_time() {
    CHECK_EQ(dh::kDefaultTzMinutes, 480);
    CHECK(dh::valid_tz(480) && dh::valid_tz(0) && dh::valid_tz(330) && dh::valid_tz(345) && dh::valid_tz(-210) && dh::valid_tz(-720) && dh::valid_tz(840));
    for (int bad : {1, 481, 7, -721, 841, 900, 100000}) CHECK(!dh::valid_tz(bad));
    { DisplayConfig c; CHECK(dh::valid_config(c)); c.tz_minutes = 7; CHECK(!dh::valid_config(c)); }   // no value that is no zone is saved
    CHECK_EQ(dh::tz_step(480, 1), 525); CHECK_EQ(dh::tz_step(525, -1), 480); CHECK_EQ(dh::tz_step(480, -1), 420);
    CHECK_EQ(dh::tz_step(840, 1), 840); CHECK_EQ(dh::tz_step(-720, -1), -720);        // the ends do not wrap
    CHECK_EQ(dh::tz_step(7, 1), dh::kDefaultTzMinutes);                               // not a zone: back to the default
    int walked = -720, steps = 0; while (walked != 840 && steps < 60) { const int next = dh::tz_step(walked, 1); CHECK(next > walked); walked = next; ++steps; }
    CHECK_EQ(walked, 840);
    CHECK_EQ(dh::tz_env(480), std::string("UTC-08:00")); CHECK_EQ(dh::tz_env(345), std::string("UTC-05:45"));
    CHECK_EQ(dh::tz_env(-210), std::string("UTC+03:30")); CHECK_EQ(dh::tz_env(0), std::string("UTC+00:00"));
    CHECK_EQ(dh::tz_env(-720), std::string("UTC+12:00")); CHECK_EQ(dh::tz_env(840), std::string("UTC-14:00"));
    {   // The C library reads it the way we mean it.
        const char *old = getenv("TZ"); const std::string saved = old ? old : "";
        setenv("TZ", dh::tz_env(330).c_str(), 1); tzset();
        time_t epoch = 0; struct tm local{}; localtime_r(&epoch, &local);
        CHECK_EQ(local.tm_hour, 5); CHECK_EQ(local.tm_min, 30);
        if (old) setenv("TZ", saved.c_str(), 1); else unsetenv("TZ");
        tzset();
    }
    CHECK_EQ(tz_label(480), std::string("UTC+8")); CHECK_EQ(tz_label(345), std::string("UTC+5:45"));
    CHECK_EQ(tz_label(-210), std::string("UTC-3:30")); CHECK_EQ(tz_label(0), std::string("UTC"));
    // The first numbers: the local time now, or a start when the time is not known.
    TimeDraft d = time_draft(true, 1793860320, 480);        // 2026-11-05 06:32 UTC
    CHECK(d.year == 2026 && d.month == 11 && d.day == 5 && d.hour == 14 && d.minute == 32);
    d = time_draft(true, 1793860320, 0); CHECK(d.hour == 6 && d.minute == 32);
    d = time_draft(false, 1793860320, 480); CHECK(d.year == 2026 && d.month == 1 && d.day == 1 && d.hour == 0 && d.minute == 0);
    // Steps: the fields wrap, the year does not, and a day never leaves its month.
    d = TimeDraft{}; d.month = 1; d.day = 31; time_draft_step(d, 1, 1); CHECK(d.month == 2 && d.day == 28);
    d = TimeDraft{}; d.year = 2028; d.month = 1; d.day = 31; time_draft_step(d, 1, 1); CHECK(d.month == 2 && d.day == 29);   // a leap year
    d.year = 2028; d.month = 2; d.day = 29; time_draft_step(d, 0, 1); CHECK(d.year == 2029 && d.day == 28);                 // the leap day goes with the year
    d = TimeDraft{}; d.month = 12; time_draft_step(d, 1, 1); CHECK(d.month == 1); time_draft_step(d, 1, -1); CHECK(d.month == 12);
    d = TimeDraft{}; d.month = 4; d.day = 30; time_draft_step(d, 2, 1); CHECK(d.day == 1); time_draft_step(d, 2, -1); CHECK(d.day == 30);
    d = TimeDraft{}; d.hour = 23; time_draft_step(d, 3, 1); CHECK(d.hour == 0); time_draft_step(d, 3, -1); CHECK(d.hour == 23);
    d = TimeDraft{}; d.minute = 59; time_draft_step(d, 4, 1); CHECK(d.minute == 0); time_draft_step(d, 4, -1); CHECK(d.minute == 59);
    d = TimeDraft{}; d.year = 2099; time_draft_step(d, 0, 1); CHECK(d.year == 2099);
    d.year = 2024; time_draft_step(d, 0, -1); CHECK(d.year == 2024);
    const TimeDraft before = d; time_draft_step(d, 5, 1); time_draft_step(d, -1, 1); CHECK(d.year == before.year && d.month == before.month && d.minute == before.minute);
    // Saving: the local time becomes UTC by the zone, across a day and a year; a time the core refuses is not offered.
    d = TimeDraft{}; d.year = 2026; d.month = 11; d.day = 5; d.hour = 14; d.minute = 32;
    CHECK_EQ(time_draft_utc(d, 480), std::string("2026-11-05T06:32:00Z"));
    CHECK_EQ(time_draft_utc(d, -210), std::string("2026-11-05T18:02:00Z"));
    d = TimeDraft{}; d.year = 2027; d.month = 1; d.day = 1; d.hour = 0; d.minute = 1;
    CHECK_EQ(time_draft_utc(d, 480), std::string("2026-12-31T16:01:00Z"));
    d = TimeDraft{}; d.year = 2024; d.month = 1; d.day = 1; CHECK_EQ(time_draft_utc(d, 480), std::string(""));              // 2023 in UTC
    d = TimeDraft{}; d.year = 2026; d.month = 2; d.day = 30; CHECK_EQ(time_draft_utc(d, 480), std::string(""));             // no such day
    d = TimeDraft{}; d.year = 2099; d.month = 12; d.day = 31; d.hour = 23; CHECK_EQ(time_draft_utc(d, -720), std::string(""));   // 2100 in UTC
}

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
                          {2, "电源", "无读数", "还没有新的电源读数", "电源没有读数"}, {3, "时间", "未校准", "请到设置里校时", "时间没校准"}};
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

// ---- T24 ----
static void t24_pages() {
    const PageOrder d = default_pages();
    CHECK_EQ(serialize_pages(d), std::string("S+O+C-P-U-T-"));
    CHECK(home_view(d) == View::Switch);
    CHECK(nav_views(d) == (std::vector<View>{View::Switch, View::Overview}));
    // The home page is the first shown one, not the first slot.
    PageOrder o = d;
    CHECK(pages_toggle(o, 0));                       // hide Switch
    CHECK_EQ(serialize_pages(o), std::string("S-O+C-P-U-T-"));
    CHECK(home_view(o) == View::Overview);
    CHECK(nav_views(o) == std::vector<View>{View::Overview});
    // The last shown page cannot be hidden.
    CHECK(!pages_toggle(o, 1));
    CHECK_EQ(serialize_pages(o), std::string("S-O+C-P-U-T-"));
    CHECK(pages_toggle(o, 2));                       // show Clock
    CHECK(pages_toggle(o, 1));                       // now Overview can go
    CHECK(home_view(o) == View::Clock);
    CHECK(!pages_toggle(o, 6) && !pages_toggle(o, 99));
    // Moves swap with the neighbour; the ends and slots out of range are refused and change nothing.
    o = d;
    CHECK(pages_move(o, 1, -1));
    CHECK_EQ(serialize_pages(o), std::string("O+S+C-P-U-T-"));
    CHECK(home_view(o) == View::Overview);
    CHECK(pages_move(o, 0, +1));
    CHECK_EQ(serialize_pages(o), std::string("S+O+C-P-U-T-"));
    CHECK(!pages_move(o, 0, -1));
    CHECK(!pages_move(o, 5, +1));
    CHECK(!pages_move(o, 6, -1) && !pages_move(o, 7, +1) && !pages_move(o, 1, 0) && !pages_move(o, 1, 2) && !pages_move(o, 2, -2));
    CHECK_EQ(serialize_pages(o), std::string("S+O+C-P-U-T-"));
    CHECK(pages_move(o, 3, -1));                     // Power moves up past the hidden Clock: still hidden, still not in the navigation
    CHECK_EQ(serialize_pages(o), std::string("S+O+P-C-U-T-"));
    CHECK(nav_views(o) == (std::vector<View>{View::Switch, View::Overview}));
    // Every order and every non-empty choice round-trips; the navigation is the shown ones in order, each once.
    std::array<View, 6> perm = {View::Switch, View::Overview, View::Clock, View::Power, View::Usage, View::Todo};
    std::sort(perm.begin(), perm.end());
    int seen = 0;
    do {
        for (int mask = 1; mask < 64; ++mask) {
            PageOrder p{};
            for (size_t i = 0; i < 6; ++i) p[i] = {perm[i], ((mask >> i) & 1) != 0};
            PageOrder q = default_pages();
            if (__builtin_popcount(static_cast<unsigned>(mask)) > static_cast<int>(kMaxShownPages)) {   // five shown: refused
                CHECK(!parse_pages(serialize_pages(p), q));
                continue;
            }
            CHECK(parse_pages(serialize_pages(p), q));
            CHECK_EQ(serialize_pages(q), serialize_pages(p));
            const auto nav = nav_views(q);
            CHECK_EQ(nav.size(), static_cast<size_t>(__builtin_popcount(static_cast<unsigned>(mask))));
            CHECK(!nav.empty() && nav.front() == home_view(q));
            std::set<View> once(nav.begin(), nav.end());
            CHECK_EQ(once.size(), nav.size());
            ++seen;
        }
    } while (std::next_permutation(perm.begin(), perm.end()));
    CHECK_EQ(seen, 720 * 56);   // 6! orders, and 1 to 4 of 6 shown
    for (const char *bad : {"", "S+O+C-", "S+O+C-P-X+", "S+S+C-P-", "S+O+C-Q-", "S*O+C-P-", "S-O-C-P-", "s+o+c-p-", "S+O+C-P", "S+O+C-P-\n", "SOCP", "S+O+C-P-S+"}) {
        PageOrder q = default_pages();
        q[0].shown = false; q[2].shown = true;   // not the default, to see that a refused text leaves it alone
        const std::string before = serialize_pages(q);
        const bool taken = parse_pages(bad, q);
        if (taken) std::fprintf(stderr, "pages text accepted: [%s]\n", bad);
        CHECK(!taken);
        CHECK_EQ(serialize_pages(q), before);
    }
}
// ---- T27 ----
static void t27_sources() {
    struct Port { const char *id; SourceIcon icon; const char *tag; };
    for (Port p : {Port{"mac-usbc", SourceIcon::UsbC, "USB-C"}, Port{"usb-c", SourceIcon::UsbC, "USB-C"}, Port{"USBC2", SourceIcon::UsbC, "USB-C"},
                   Port{"pc_type-c", SourceIcon::UsbC, "USB-C"}, Port{"typec", SourceIcon::UsbC, "USB-C"}, Port{"thunderbolt3", SourceIcon::UsbC, "USB-C"},
                   Port{"hdmi1", SourceIcon::Hdmi, "HDMI"}, Port{"HDMI-2", SourceIcon::Hdmi, "HDMI"}, Port{"mbp.hdmi", SourceIcon::Hdmi, "HDMI"},
                   Port{"dp", SourceIcon::Dp, "DisplayPort"}, Port{"mini-dp2", SourceIcon::Dp, "DisplayPort"}, Port{"DisplayPort1", SourceIcon::Dp, "DisplayPort"},
                   Port{"adp", SourceIcon::Monitor, ""}, Port{"dpms", SourceIcon::Monitor, ""}, Port{"hdmix", SourceIcon::Monitor, ""}, Port{"vga", SourceIcon::Monitor, ""},
                   Port{"", SourceIcon::Monitor, ""}, Port{"mac", SourceIcon::Monitor, ""}, Port{"usb", SourceIcon::Monitor, ""}, Port{"c", SourceIcon::Monitor, ""}}) {
        if (port_icon(p.id) != p.icon) std::fprintf(stderr, "port of [%s]\n", p.id);
        CHECK(port_icon(p.id) == p.icon);
        CHECK_EQ(port_tag(p.id), std::string(p.tag));
    }
    Snapshot s = online_snapshot();
    s.actions[0] = {"to-mini", "切到 Mac mini", "u2720", "mac-dp", 1};
    s.actions[1] = {"to-m1", "", "u2720", "m1-hdmi", 1};
    s.actions[2] = {"to-m4", "MacBook M4", "u2720", "m4-usbc", 1};
    s.action_count = 3;
    SourceNames names;
    auto v = make_sources(Lang::ZhCN, s, names);
    CHECK_EQ(v.size(), static_cast<size_t>(3));
    CHECK_EQ(v[0].name, std::string("切到 Mac mini")); CHECK(v[0].icon == SourceIcon::Dp); CHECK_EQ(v[0].tag, std::string("DisplayPort")); CHECK(!v[0].renamed);
    CHECK_EQ(v[0].id, std::string("to-mini"));
    CHECK_EQ(v[1].name, std::string("to-m1"));        // no label: the id, never a blank tile
    CHECK(v[1].icon == SourceIcon::Hdmi);
    CHECK(v[0].enabled && v[0].sub.empty());
    // Local names and icons.
    CHECK(set_source_name(names, "to-mini", "Mac mini"));
    CHECK(set_source_icon(names, "to-mini", SourceIcon::Desktop));
    CHECK(set_source_icon(names, "to-m4", SourceIcon::Laptop));
    v = make_sources(Lang::ZhCN, s, names);
    CHECK_EQ(v[0].name, std::string("Mac mini")); CHECK(v[0].icon == SourceIcon::Desktop); CHECK(v[0].renamed);
    CHECK_EQ(v[0].tag, std::string("DisplayPort"));   // the port tag stays: it is a fact about the cable
    CHECK_EQ(v[2].name, std::string("MacBook M4")); CHECK(v[2].icon == SourceIcon::Laptop);
    // "" brings the App name back; an entry with neither a name nor an icon is dropped.
    CHECK(set_source_name(names, "to-mini", ""));
    v = make_sources(Lang::ZhCN, s, names);
    CHECK_EQ(v[0].name, std::string("切到 Mac mini")); CHECK(v[0].icon == SourceIcon::Desktop);
    CHECK(set_source_icon(names, "to-mini", SourceIcon::Auto));
    CHECK_EQ(names.size(), static_cast<size_t>(1));
    reset_source(names, "to-m4");
    CHECK(names.empty());
    reset_source(names, "nothing-here");
    SourceNames one;                                   // a name taken back, with no icon: nothing is left
    CHECK(set_source_name(one, "a", "x"));
    CHECK(set_source_name(one, "a", ""));
    CHECK(one.empty());
    // Names: 1 to 32 bytes of printable text.
    for (const std::string &bad : {std::string(33, 'a'), std::string("tab\there"), std::string("new\nline"), std::string("\x7f"), std::string("\xe4\xb8"), std::string(1, '\0')})
        CHECK(!set_source_name(names, "to-m1", bad));
    CHECK(names.empty());
    CHECK(set_source_name(names, "to-m1", std::string(32, 'z')));
    CHECK(set_source_name(names, "to-m1", "\xe7\x94\xb5\xe8\xa7\x86"));   // UTF-8 from the App later: allowed
    CHECK(!set_source_name(names, "", "x") && !set_source_icon(names, "", SourceIcon::Tv));
    CHECK(!set_source_name(names, "bad\x1fid", "x") && !set_source_name(names, std::string(49, 'i'), "x"));
    // A local entry for a source that is not sent any more is not shown; offline sources are disabled.
    CHECK(set_source_name(names, "gone", "Old PC"));
    s.agent_ready = false;
    v = make_sources(Lang::ZhCN, s, names);
    CHECK_EQ(v.size(), static_cast<size_t>(3));
    for (const auto &x : v) CHECK(x.name != "Old PC" && !x.enabled && x.sub == "Agent 未连接");
    CHECK_EQ(make_sources(Lang::EnUS, s, names)[0].sub, std::string("Agent offline"));
    s.action_count = 99;
    CHECK_EQ(make_sources(Lang::ZhCN, s, names).size(), static_cast<size_t>(12));
    // At most 12 entries: a 13th source is refused, the ones there can still change.
    SourceNames full;
    for (int i = 0; i < 12; ++i) CHECK(set_source_name(full, "id" + std::to_string(i), "n" + std::to_string(i)));
    CHECK(!set_source_name(full, "id12", "n12"));
    CHECK(!set_source_icon(full, "id12", SourceIcon::Tv));
    CHECK(set_source_icon(full, "id3", SourceIcon::Tv));
    CHECK_EQ(full.size(), static_cast<size_t>(12));
    // Stored text: round-trips; damage reads as nothing.
    const SourceNames back = parse_sources(serialize_sources(full));
    CHECK_EQ(back.size(), static_cast<size_t>(12));
    CHECK_EQ(serialize_sources(back), serialize_sources(full));
    CHECK(parse_sources("").empty());
    const std::string good = serialize_sources(names);
    CHECK_EQ(parse_sources(good).size(), names.size());
    std::string many;
    for (int i = 0; i < 13; ++i) many += "id" + std::to_string(i) + "\x1f" "n\x1f" "auto\x1e";
    for (const std::string &bad : {std::string("a\x1fname\x1f" "banana\x1e"), std::string("a\x1fx\x1f" "auto\x1e" "a\x1fy\x1f" "auto\x1e"), std::string("\x1fx\x1f" "auto\x1e"),
                                   std::string("a\x1fx\x1e"), std::string("a\x1fx\x1f" "auto"), std::string("a\x1fx\ty\x1f" "auto\x1e"), many,
                                   std::string("a\x1f\x1f" "auto\x1e"), good + std::string(5000, 'x'), std::string("a\x1fx\x1f" "auto\x1f" "extra\x1e")}) {
        const bool taken = !parse_sources(bad).empty();
        if (taken) std::fprintf(stderr, "sources text accepted: %zu bytes\n", bad.size());
        CHECK(!taken);
    }
    for (SourceIcon i : kIconChoices) CHECK(i != SourceIcon::Auto);
    CHECK_EQ(kIconChoices.size(), static_cast<size_t>(8));
}


// ---- T28: the fifth page (13.12 G7) ----
static void t28_usage_page_order() {
    PageOrder d = default_pages();
    CHECK_EQ(serialize_pages(d), std::string("S+O+C-P-U-T-"));          // Usage is there, hidden
    PageOrder o{};
    CHECK(parse_pages("S+O+C-P-U+T-", o) && serialize_pages(o) == "S+O+C-P-U+T-");
    // what 0.3.27 and earlier stored: four pages; Usage comes last, hidden
    CHECK(parse_pages("C+S-O+P+", o) && serialize_pages(o) == "C+S-O+P+U-T-");
    for (const char *bad : {"S+O+C+P+U+T-", "S+O+C-P-U", "S+O+C-P-U+X-", "S+O+C-P-S-", "S-O-C-P-U-T-", "S+O+C-P-u+", "S+O+C-U-", "S+O+C-P-U-U-"}) {
        PageOrder keep = default_pages();
        CHECK(!parse_pages(bad, keep));
        CHECK_EQ(serialize_pages(keep), std::string("S+O+C-P-U-T-"));   // untouched
    }
    // at most four shown
    PageOrder four{};
    CHECK(parse_pages("S+O+C+P+U-T-", four));
    CHECK(!pages_toggle(four, 4) && !four[4].shown);                   // a fifth is refused
    CHECK(pages_toggle(four, 3) && pages_toggle(four, 4) && four[4].shown);   // after one is hidden it may be shown
    CHECK(nav_views(four).size() == 4);
    // an Appearance text keeps working both ways
    Appearance a = parse_appearance("night|-|zh-CN|standard|pct|standard|S+O+C-P-|list|hands");
    CHECK_EQ(serialize_pages(a.pages), std::string("S+O+C-P-U-T-"));
    a.pages[4].shown = true;
    CHECK(parse_appearance(serialize(a)).pages[4].shown);
}

// ---- T29: the words of the Usage page (13.12 G2 to G6) ----
static ble::Usage sample_usage() {
    ble::Usage u;
    u.ok = true; u.provider = "codex"; u.plan = "plus";
    u.windows = {{300, 100, 15120}, {10080, 37, 300000}};
    u.calls = 128; u.errors = 2; u.tokens = 1234567; u.ttft_ms = 820;
    u.latest_model = "gpt-6.1-sol"; u.latest_status = 200; u.latest_ms = 3200; u.latest_age_s = 120;
    return u;
}
static void t29_usage_words() {
    CHECK_EQ(short_count(0), std::string("0"));
    CHECK_EQ(short_count(128), std::string("128"));
    CHECK_EQ(short_count(9999), std::string("10.0K"));
    CHECK_EQ(short_count(1234), std::string("1.2K"));
    CHECK_EQ(short_count(12345), std::string("12.3K"));
    CHECK_EQ(short_count(1234567), std::string("1.2M"));
    CHECK_EQ(short_count(987654321), std::string("987.7M"));
    CHECK_EQ(short_count(1234567890), std::string("1.2B"));
    setenv("TZ", "CST-8", 1);
    tzset();
    const time_t now = 1794120000;   // 2026-11-08 14:40 CST
    // G4: nothing yet
    UsageView w = make_usage(Lang::ZhCN, nullptr, 0, true, now);
    CHECK(w.state == UsageView::State::Waiting && w.status == "等 App 发来用量" && w.quotas.empty());
    // fresh
    ble::Usage u = sample_usage();
    UsageView v = make_usage(Lang::ZhCN, &u, 15000, true, now);
    CHECK(v.state == UsageView::State::Fresh && !v.dim && v.status == "15 秒前更新" && v.title == "Codex · plus");
    CHECK(make_usage(Lang::ZhCN, &u, 2000, true, now).status == "刚刚更新");
    CHECK(v.quotas.size() == 2 && v.quotas[0].label == "5 小时" && v.quotas[0].remaining == 100 && v.quotas[1].label == "7 天" && v.quotas[1].remaining == 37);
    CHECK_EQ(v.quotas[0].reset, std::string("4 小时 11 分后重置"));   // 15120 s at the App, 15 s ago: 15105 s left (4 h 11 min 45 s)
    {   // more than a day away: the day and the time it resets, in local time
        const time_t at = now + 300000 - 15;
        struct tm t{};
        localtime_r(&at, &t);
        static const char *days[] = {"日", "一", "二", "三", "四", "五", "六"};
        char want[64];
        snprintf(want, sizeof(want), "周%s %02d:%02d 重置", days[t.tm_wday], t.tm_hour, t.tm_min);
        CHECK_EQ(v.quotas[1].reset, std::string(want));
    }
    CHECK(v.calls == "128" && v.tokens == "1.2M" && v.success == "98.4%" && v.ttft == "820");
    CHECK(v.model == "gpt-6.1-sol" && v.code == "200" && v.latency == "3.2 秒" && v.ago == "2 分钟前" && !v.failed);
    // G5: the time is not set: no weekday; a reset that is not known
    UsageView nt = make_usage(Lang::ZhCN, &u, 15000, false, now);
    CHECK_EQ(nt.quotas[1].reset, std::string("3 天后重置"));
    ble::Usage unknown = u; unknown.windows[0].reset_s = 0;
    CHECK_EQ(make_usage(Lang::ZhCN, &unknown, 0, true, now).quotas[0].reset, std::string("重置时间未知"));
    ble::Usage soon = u; soon.windows[0].reset_s = 600;
    CHECK_EQ(make_usage(Lang::ZhCN, &soon, 0, true, now).quotas[0].reset, std::string("10 分后重置"));
    CHECK_EQ(make_usage(Lang::ZhCN, &soon, 700000, true, now).quotas[0].reset, std::string("马上重置"));   // it has passed by now
    // labels of other windows
    ble::Usage odd = u; odd.windows = {{45, 50, 100}, {1440, 80, 100}};
    UsageView ov = make_usage(Lang::ZhCN, &odd, 0, true, now);
    CHECK(ov.quotas[0].label == "45 分钟" && ov.quotas[1].label == "1 天");
    // G6: no calls today
    ble::Usage idle = u; idle.calls = 0; idle.errors = 0; idle.ttft_ms = 0; idle.tokens = 0;
    UsageView iv = make_usage(Lang::ZhCN, &idle, 0, true, now);
    CHECK(iv.calls == "0" && iv.tokens == "0" && iv.success == "—" && iv.ttft == "—");
    // the latest request: failed, fast, none
    ble::Usage f = u; f.latest_status = 429; f.latest_ms = 820; f.latest_age_s = 5;
    UsageView fv = make_usage(Lang::ZhCN, &f, 0, true, now);
    CHECK(fv.failed && fv.code == "429" && fv.latency == "820 ms" && fv.ago == "刚才");
    ble::Usage none = u; none.latest_model.clear(); none.latest_status = 0; none.latest_ms = 0;
    UsageView nv = make_usage(Lang::ZhCN, &none, 0, true, now);
    CHECK(nv.model == "—" && !nv.failed && nv.code.empty());
    ble::Usage hours = u; hours.latest_age_s = 3 * 3600 + 100;
    CHECK_EQ(make_usage(Lang::ZhCN, &hours, 0, true, now).ago, std::string("3 小时前"));
    // G3: stale
    UsageView sv = make_usage(Lang::ZhCN, &u, 3 * 60000 + 5000, true, now);
    CHECK(sv.state == UsageView::State::Stale && sv.dim && sv.status == "数据过期 · 3 分钟前" && sv.calls == "128");
    CHECK(make_usage(Lang::ZhCN, &u, ble::kUsageStaleMs, true, now).state == UsageView::State::Fresh);
    // G2: Magpie is not running: no old numbers
    ble::Usage off; off.ok = false;
    UsageView fo = make_usage(Lang::ZhCN, &off, 1000, true, now);
    CHECK(fo.state == UsageView::State::Offline && fo.status == "Magpie 没有运行" && fo.quotas.empty() && fo.calls == "—");
    // title without a plan, without a provider
    ble::Usage np = u; np.plan.clear();
    CHECK_EQ(make_usage(Lang::ZhCN, &np, 0, true, now).title, std::string("Codex"));
    np.provider.clear();
    CHECK_EQ(make_usage(Lang::ZhCN, &np, 0, true, now).title, std::string("Magpie"));
    // English
    UsageView e = make_usage(Lang::EnUS, &u, 15000, true, now);
    CHECK(e.status == "Updated 15 s ago" && e.quotas[0].label == "5 h" && e.quotas[1].label == "7 d" && e.quotas[0].reset == "Resets in 4 h 11 min");
    CHECK(make_usage(Lang::EnUS, nullptr, 0, true, now).status == "Waiting for the App" && make_usage(Lang::EnUS, &off, 0, true, now).status == "Magpie is not running");
    // every word is in the font
    const auto glyphs = font_glyphs();
    for (const UsageView &x : {w, v, nt, iv, fv, sv, fo, e})
        for (const std::string &s : {x.status, x.title, x.ago, x.latency, x.success, x.ttft})
            CHECK(renderable(s, glyphs));
    for (const QuotaCard &q : v.quotas) CHECK(renderable(q.label, glyphs) && renderable(q.reset, glyphs));
}

// ---- T30 ----
static void t30_todo_page_order() {
    PageOrder d = default_pages();
    CHECK_EQ(serialize_pages(d), std::string("S+O+C-P-U-T-"));
    CHECK(d[5].view == View::Todo && !d[5].shown);
    PageOrder o{};
    CHECK(parse_pages("S+O+C-P-U-T+", o) && serialize_pages(o) == "S+O+C-P-U-T+");
    CHECK(parse_pages("C+S-O+P+", o) && serialize_pages(o) == "C+S-O+P+U-T-");       // 0.3.27
    CHECK(parse_pages("C+S-O+U+P-", o) && serialize_pages(o) == "C+S-O+U+P-T-");     // 0.3.28 to 0.3.30
    for (const char *bad : {"S+O+C+P+U-T+", "S+O+C-P-U-T", "S+O+C-P-T-", "S+O+C-P-U-T-T-", "S+O+C-P-U-t-", "S+O+T-P-U-C-C-"}) {
        PageOrder keep = default_pages();
        CHECK(!parse_pages(bad, keep));
        CHECK_EQ(serialize_pages(keep), std::string("S+O+C-P-U-T-"));
    }
    PageOrder four{};
    CHECK(parse_pages("S+O+C+P+U-T-", four));
    CHECK(!pages_toggle(four, 5) && !four[5].shown);
    CHECK(pages_toggle(four, 2) && pages_toggle(four, 5) && four[5].shown);
    CHECK(nav_views(four) == (std::vector<View>{View::Switch, View::Overview, View::Power, View::Todo}));
    Appearance a = parse_appearance("night|-|zh-CN|standard|pct|standard|S+O+C-P-U-|list|hands");
    CHECK_EQ(serialize_pages(a.pages), std::string("S+O+C-P-U-T-"));
    a.pages[5].shown = true;
    CHECK(parse_appearance(serialize(a)).pages[5].shown);
}
// ---- T31 ----
static ble::Todo td(const char *id, const char *title, ble::TodoKind k, int m, int d, int hm, const char *list = "工作") {
    ble::Todo t;
    t.id = id; t.title = title; t.list = list; t.kind = k;
    t.month = static_cast<uint8_t>(m); t.day = static_cast<uint8_t>(d); t.minute = static_cast<int16_t>(hm);
    return t;
}
static void t31_todo_words() {
    using K = ble::TodoKind;
    ble::TodoList l;
    l.items = {td("000000000001", "交季度报告", K::Overdue, 11, 10, -1), td("000000000002", "续费域名", K::Overdue, 11, 8, 570, ""),
               td("000000000003", "回复设计评审", K::Today, 11, 11, 870), td("000000000004", "给猫买粮", K::Today, 11, 11, -1, "家"),
               td("000000000005", "预约体检", K::Tomorrow, 11, 12, -1), td("000000000006", "看牙", K::Tomorrow, 11, 12, 540),
               td("000000000007", "交房租", K::Later, 12, 1, -1), td("000000000008", "读完那本书", K::None, 0, 0, -1)};
    l.total = 11;
    // L3: nothing yet
    TodoView w = make_todos(Lang::ZhCN, nullptr, "", 0, true);
    CHECK(w.state == TodoView::State::Waiting && w.status == "等 App 发来待办" && w.groups.empty() && !w.can_act);
    // fresh
    TodoView v = make_todos(Lang::ZhCN, &l, "", 3000, true);
    CHECK(v.state == TodoView::State::Fresh && !v.dim && v.can_act && v.status == "刚刚更新" && v.count == "未完成 11 条");
    CHECK_EQ(make_todos(Lang::ZhCN, &l, "", 45000, true).status, std::string("45 秒前更新"));
    CHECK(v.groups.size() == 4);
    if (v.groups.size() == 4) {
        CHECK(v.groups[0].kind == K::Overdue && v.groups[0].title == "已过期 · 2" && v.groups[0].rows.size() == 2);
        CHECK(v.groups[1].kind == K::Today && v.groups[1].title == "今天 · 2" && v.groups[1].rows.size() == 2);
        CHECK(v.groups[2].kind == K::Later && v.groups[2].title == "以后 · 3" && v.groups[2].rows.size() == 3);   // tomorrow and later
        CHECK(v.groups[3].kind == K::None && v.groups[3].title == "没有日期 · 1" && v.groups[3].rows.size() == 1);
        const TodoRow &a = v.groups[0].rows[0], &b = v.groups[0].rows[1];
        CHECK(a.id == "000000000001" && a.title == "交季度报告" && a.when == "11月10日" && !a.tag && a.detail == "工作 · 11月10日");
        CHECK(b.when == "11月8日 09:30" && b.detail == "11月8日 09:30");          // no list name: only the time
        const TodoRow &c = v.groups[1].rows[0], &e = v.groups[1].rows[1];
        CHECK(c.when == "14:30" && c.tag && c.detail == "工作 · 今天 14:30");
        CHECK(e.when == "今天" && e.tag && e.detail == "家 · 今天");
        CHECK(v.groups[2].rows[0].when == "明天" && v.groups[2].rows[1].when == "明天 09:00" && v.groups[2].rows[2].when == "12月1日");
        CHECK(!v.groups[2].rows[0].tag && v.groups[2].rows[1].detail == "工作 · 明天 09:00");
        CHECK(v.groups[3].rows[0].when.empty() && v.groups[3].rows[0].detail == "工作");
    }
    CHECK_EQ(v.more, std::string("还有 3 条"));                                  // L7: 11 on the Mac, 8 sent
    ble::TodoList all = l; all.total = 8;
    CHECK(make_todos(Lang::ZhCN, &all, "", 0, true).more.empty());
    // the link is down but the list is fresh: no actions
    CHECK(!make_todos(Lang::ZhCN, &l, "", 3000, false).can_act);
    // L4: stale
    TodoView s = make_todos(Lang::ZhCN, &l, "", 6 * 60000 + 1000, true);
    CHECK(s.state == TodoView::State::Stale && s.dim && !s.can_act && s.status == "数据过期 · 6 分钟前" && s.groups.size() == 4);
    CHECK(!s.hint.empty());
    CHECK(make_todos(Lang::ZhCN, &l, "", ble::kTodoStaleMs, true).state == TodoView::State::Fresh);
    // L5: off, and why; no old list
    TodoView na = make_todos(Lang::ZhCN, &l, "access", 1000, true);
    CHECK(na.state == TodoView::State::Off && na.groups.empty() && !na.can_act && na.status == "App 没有读提醒事项的权限");
    CHECK(!na.hint.empty());
    TodoView ns = make_todos(Lang::ZhCN, &l, "setting", 1000, true);
    CHECK(ns.state == TodoView::State::Off && ns.status == "Mac 上的 DeskHub 关掉了待办");
    // L6: none
    ble::TodoList empty;
    TodoView ne = make_todos(Lang::ZhCN, &empty, "", 1000, true);
    CHECK(ne.state == TodoView::State::Empty && ne.status == "没有未完成的待办" && ne.groups.empty());
    // English
    TodoView en = make_todos(Lang::EnUS, &l, "", 3000, true);
    CHECK(en.count == "11 open" && en.status == "Just updated");
    if (en.groups.size() == 4) {
        CHECK(en.groups[0].title == "Overdue | 2" && en.groups[1].title == "Today | 2" && en.groups[2].title == "Later | 3" && en.groups[3].title == "No date | 1");
        CHECK(en.groups[0].rows[0].when == "Nov 10" && en.groups[0].rows[1].when == "Nov 8 09:30" && en.groups[1].rows[1].when == "Today");
        CHECK(en.groups[2].rows[0].when == "Tomorrow" && en.groups[2].rows[1].detail == "工作 | Tomorrow 09:00");
    }
    CHECK_EQ(en.more, std::string("3 more"));
    // result lines (L9, L11, L12)
    CHECK_EQ(todo_result_text(Lang::ZhCN, ble::TodoAction::Done, true, "交季度报告", ""), std::string("已完成：交季度报告"));
    CHECK_EQ(todo_result_text(Lang::ZhCN, ble::TodoAction::Tomorrow, true, "交季度报告", ""), std::string("已推迟到明天：交季度报告"));
    CHECK_EQ(todo_result_text(Lang::ZhCN, ble::TodoAction::Delete, true, "交季度报告", ""), std::string("已删除：交季度报告"));
    CHECK_EQ(todo_result_text(Lang::ZhCN, ble::TodoAction::Done, false, "交季度报告", "这条待办已经变了"), std::string("没有改成：这条待办已经变了"));
    CHECK_EQ(todo_result_text(Lang::ZhCN, ble::TodoAction::Done, false, "交季度报告", ""), std::string("没有改成"));
    CHECK_EQ(todo_result_text(Lang::EnUS, ble::TodoAction::Done, true, "Pay rent", ""), std::string("Done: Pay rent"));
    CHECK(!todo_timeout_text(Lang::ZhCN).empty() && !todo_timeout_text(Lang::EnUS).empty());
    const auto glyphs = font_glyphs();
    for (const TodoView &x : {w, v, s, na, ns, ne, en}) {
        for (const std::string &t : {x.status, x.count, x.hint, x.more}) CHECK(renderable(t, glyphs));
        for (const TodoGroup &g : x.groups) {
            CHECK(renderable(g.title, glyphs));
            for (const TodoRow &r : g.rows) CHECK(renderable(r.when, glyphs));
        }
    }
    CHECK(renderable(todo_timeout_text(Lang::ZhCN), glyphs) && renderable(todo_result_text(Lang::ZhCN, ble::TodoAction::Tomorrow, true, "", ""), glyphs));
}
// ---- T32 ----
static bool has_cjk_common(uint32_t cp) { return cp < 0x80 || cp == 0x4EA4 /* 交 */ || cp == 0x62A5 /* 报 */; }
static void t32_displayable() {
    CHECK_EQ(displayable("交报 ok", has_cjk_common), std::string("交报 ok"));
    CHECK_EQ(displayable("交\xe9\xbe\x98报", has_cjk_common), std::string("交?报"));                  // 龘: not in the font
    CHECK_EQ(displayable("\xf0\x9f\x8e\x82 ok", has_cjk_common), std::string("? ok"));                 // an emoji
    CHECK_EQ(displayable("\xe2\x9d\xa4\xef\xb8\x8f ok", has_cjk_common), std::string("? ok"));         // a heart and its variation selector
    CHECK_EQ(displayable("\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x92\xbb", has_cjk_common), std::string("??"));   // two joined by ZWJ
    CHECK_EQ(displayable("ab\xe4\xba", has_cjk_common), std::string("ab?"));                          // cut in the middle of a character
    CHECK_EQ(displayable("", has_cjk_common), std::string(""));
}

// ---- T33 ----
static void t33_todo_edit() {
    using K = ble::TodoKind;
    CHECK(repeat_text(Lang::ZhCN, "") == "不重复" && repeat_text(Lang::ZhCN, "d1") == "每天" && repeat_text(Lang::ZhCN, "wd") == "工作日");
    CHECK(repeat_text(Lang::ZhCN, "w1") == "每周" && repeat_text(Lang::ZhCN, "m1") == "每月" && repeat_text(Lang::ZhCN, "y1") == "每年");
    CHECK(repeat_text(Lang::ZhCN, "d3") == "每 3 天" && repeat_text(Lang::ZhCN, "w2") == "每 2 周" && repeat_text(Lang::ZhCN, "m12") == "每 12 个月" && repeat_text(Lang::ZhCN, "y99") == "每 99 年");
    CHECK(repeat_text(Lang::ZhCN, "x") == "自定义" && repeat_text(Lang::EnUS, "w2") == "Every 2 weeks" && repeat_text(Lang::EnUS, "d1") == "Daily");
    // the month wheel
    auto months = month_choices(Lang::ZhCN, true, 2026, 11, 2026, 12);
    CHECK(months.size() == 13 && months[0].year == 2026 && months[0].month == 11 && months[0].label == "11月");
    CHECK(months[1].label == "12月" && months[2].year == 2027 && months[2].month == 1 && months[2].label == "2027年1月" && months[12].month == 11);
    auto earlier = month_choices(Lang::ZhCN, true, 2026, 11, 2026, 8);   // an overdue one from August
    CHECK(earlier.front().month == 8 && earlier.back().year == 2027 && earlier.back().month == 11 && earlier.size() == 16);
    auto unknown = month_choices(Lang::ZhCN, false, 1970, 1, 2026, 5);   // the device does not know the date
    CHECK(unknown.size() == 13 && unknown.front().year == 2026 && unknown.front().month == 5);
    CHECK(month_choices(Lang::EnUS, true, 2026, 11, 0, 0)[2].label == "Jan 2027");
    // the draft
    ble::Todo t = td("000000000003", "回复设计评审", K::Today, 11, 11, 870);
    t.year = 2026; t.repeat = "w1"; t.list_id = "aaaaaaaaaaaa"; t.alarm = true; t.version = "0a1b2c3d";
    TodoDraft d = todo_draft(t, true, 2026, 11, 11);
    CHECK(d.when == TodoDraft::When::Timed && d.year == 2026 && d.month == 11 && d.day == 11 && d.minute == 870 && d.repeat == "w1" && d.alarm);
    ble::TodoEdit e = todo_edit_of(t, d);
    CHECK(ble::valid_edit(e) && e.id == t.id && e.version == "0a1b2c3d" && e.minute == 870 && e.alarm && e.list_id == "aaaaaaaaaaaa");
    d.when = TodoDraft::When::AllDay;
    e = todo_edit_of(t, d);
    CHECK(ble::valid_edit(e) && e.minute == -1 && !e.alarm);              // no time: no ring
    d.when = TodoDraft::When::None;
    e = todo_edit_of(t, d);
    CHECK(ble::valid_edit(e) && e.year == 0 && e.month == 0 && e.day == 0 && e.minute == -1 && !e.alarm);
    d.when = TodoDraft::When::Timed; d.month = 2; d.day = 31; d.year = 2027;
    e = todo_edit_of(t, d);
    CHECK(ble::valid_edit(e) && e.day == 28);                             // kept inside its month
    ble::Todo undated = td("000000000008", "读完那本书", K::None, 0, 0, -1);
    undated.list_id = "aaaaaaaaaaaa"; undated.version = "0a1b2c3d";
    TodoDraft u = todo_draft(undated, true, 2026, 11, 11);
    CHECK(u.when == TodoDraft::When::None && u.year == 2026 && u.month == 11 && u.day == 11 && u.minute == 540);   // ready for a date: today 09:00
    // the "when" in words
    TodoDraft w = todo_draft(t, true, 2026, 11, 11);
    CHECK(draft_when_text(Lang::ZhCN, w, 2026) == "11月11日 14:30");
    w.when = TodoDraft::When::AllDay;
    CHECK(draft_when_text(Lang::ZhCN, w, 2026) == "11月11日 全天");
    w.when = TodoDraft::When::None;
    CHECK(draft_when_text(Lang::ZhCN, w, 2026) == "没有日期");
    w = todo_draft(t, true, 2026, 11, 11); w.year = 2027; w.month = 1; w.day = 3;
    CHECK(draft_when_text(Lang::ZhCN, w, 2026) == "2027年1月3日 14:30");
    // the detail line says the repeat and the ring
    ble::TodoList l; l.items = {t}; l.total = 1;
    TodoView v = make_todos(Lang::ZhCN, &l, "", 0, true);
    CHECK(!v.groups.empty() && v.groups[0].rows[0].detail == "工作 · 今天 14:30 · 每周 · 响铃");
    const auto glyphs = font_glyphs();
    for (const char *r : {"", "d1", "wd", "w1", "m1", "y1", "d3", "w2", "m4", "y5", "x"}) CHECK(renderable(repeat_text(Lang::ZhCN, r), glyphs));
    for (const auto &m : months) CHECK(renderable(m.label, glyphs));
    CHECK(renderable(v.groups[0].rows[0].detail, glyphs));
    CHECK_EQ(todo_result_text(Lang::ZhCN, ble::TodoAction::Edit, true, "回复设计评审", ""), std::string("已更改：回复设计评审"));
}
// ---- T34 ----
static void t34_ringing() {
    using K = ble::TodoKind;
    auto ringing = [](const char *id, int y, int m, int d, int hm, bool alarm) {
        ble::Todo t = td(id, id, K::Today, m, d, hm);
        t.year = static_cast<uint16_t>(y); t.alarm = alarm;
        return t;
    };
    std::vector<ble::Todo> list = {ringing("000000000001", 2026, 11, 12, 870, true), ringing("000000000002", 2026, 11, 12, 870, true),
                                   ringing("000000000003", 2026, 11, 12, 900, false), ringing("000000000004", 2026, 11, 13, 870, true)};
    TodoRinger r;
    CHECK(!r.due(list, true, 2026, 11, 12, 869, 0).has_value());                     // not yet
    CHECK(!r.due(list, false, 2026, 11, 12, 870, 0).has_value());                    // M7: the time is not known
    auto a = r.due(list, true, 2026, 11, 12, 870, 1000);
    CHECK(a && a->id == "000000000001");                                            // M12: the first in the list
    auto b = r.due(list, true, 2026, 11, 12, 870, 2000);
    CHECK(b && b->id == "000000000002");                                            // then the next one
    CHECK(!r.due(list, true, 2026, 11, 12, 870, 3000).has_value());                  // M9: each once
    CHECK(!r.due(list, true, 2026, 11, 12, 900, 4000).has_value());                  // no ring set
    CHECK(!r.due(list, true, 2026, 11, 12, 871, 5000).has_value());                  // a minute late: it is over
    // again in 10 minutes, once
    r.snooze(*a, 10000);
    CHECK(!r.due(list, true, 2026, 11, 12, 875, 10000 + 9 * 60000).has_value());
    auto c = r.due(list, true, 2026, 11, 12, 880, 10000 + 10 * 60000);
    CHECK(c && c->id == "000000000001");
    CHECK(!r.due(list, true, 2026, 11, 12, 881, 10000 + 11 * 60000).has_value());
    // the same to-do moved to another time rings again then
    list[0].minute = 900;
    CHECK(r.due(list, true, 2026, 11, 12, 900, 20 * 60000).has_value());
    r.reset();
    list[0].minute = 870;
    CHECK(r.due(list, true, 2026, 11, 12, 870, 0).has_value());                       // after a restart it may ring once more
}
// ---- T35 ----
static void t35_review() {
    CHECK(weekday_of(2026, 11, 12) == 4);                                   // a Thursday
    CHECK(weekday_of(2000, 2, 29) == 2);                                    // a leap day, a Tuesday
    CHECK(weekday_of(2027, 1, 3) == 0);                                     // a Sunday, across the year
    CHECK(weekday_of(2100, 3, 1) == 1);                                     // 2100 is not a leap year: a Monday
    TodoDraft d;
    d.when = TodoDraft::When::Timed; d.year = 2026; d.month = 11; d.day = 12; d.minute = 870;
    CHECK(draft_when_head(Lang::ZhCN, d) == "到期 · 周四");
    CHECK(draft_when_head(Lang::EnUS, d) == "Due | Thu");
    d.when = TodoDraft::When::AllDay; d.year = 2027; d.month = 1; d.day = 3;
    CHECK(draft_when_head(Lang::ZhCN, d) == "到期 · 周日");
    d.year = 2026; d.month = 2; d.day = 31;                                 // kept inside February: the 28th, a Saturday
    CHECK(draft_when_head(Lang::ZhCN, d) == "到期 · 周六");
    d.when = TodoDraft::When::None;
    CHECK(draft_when_head(Lang::ZhCN, d) == "到期");
    CHECK(draft_when_head(Lang::EnUS, d) == "Due");
    ble::Todo t = td("000000000001", "回复设计评审", ble::TodoKind::Today, 11, 12, 870);
    t.repeat = "w1";
    CHECK(todo_ring_line(Lang::ZhCN, t) == "工作 · 每周");
    t.list = "Work";
    CHECK(todo_ring_line(Lang::EnUS, t) == "Work | Weekly");
    t.list = ""; t.repeat = "d3";
    CHECK(todo_ring_line(Lang::ZhCN, t) == "每 3 天");
    t.list = "家"; t.repeat = "";
    CHECK(todo_ring_line(Lang::ZhCN, t) == "家");
    t.list = "";
    CHECK(todo_ring_line(Lang::ZhCN, t).empty());
}
int main() {
    t35_review();
    t33_todo_edit();
    t34_ringing();
    t30_todo_page_order();
    t31_todo_words();
    t32_displayable();
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
    t28_usage_page_order();
    t29_usage_words();
    t13_percent();
    t15_power_view();
    t16_connect();
    t17_actions();
    t18_notice();
    t19_charge_view();
    t10b_blink();
    t20_battery_fill_and_rings();
    t21_ring_info();
    t22_quiet_hours();
    t23_time();
    t24_pages();
    t25_appearance_upgrade();
    t26_night_theme();
    t27_sources();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("ui_model_test: all checks passed");
    return 0;
}
