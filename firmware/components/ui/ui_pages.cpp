// The pages of the device UI (docs/design Dev-*). Each page has a builder, which makes the objects once, and a
// refresher, which only sets text and colours that the view models in ui_model.cpp say. A refresher never builds.
#include "ui_internal.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "esp_system.h"

namespace dh::ui::detail {
namespace {
int lh(const lv_font_t *f) { return lv_font_get_line_height(f) + 1; }
const Palette &P() { return look.palette; }
std::string T(Str s) { return tr(look.lang, s); }
// The tab labels sit in 104 px buttons: the large text size would cut "Settings", so they stop at 20 px.
const lv_font_t *nav_font() { return look.body == &btr_ui_28 ? &btr_ui_20 : look.body; }
intptr_t as_data(int v) { return static_cast<intptr_t>(v); }
void *data(int v) { return reinterpret_cast<void *>(as_data(v)); }
int data_of(lv_event_t *e) { return static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))); }

lv_obj_t *g_body = nullptr;
int g_scroll = 0;

struct Shell {
    lv_obj_t *left{}, *right{}, *agent_dot{}, *agent{}, *wifi_icon{}, *wifi{}, *battery{}, *charge{}, *outline{}, *terminal{}, *fill{}, *nav{}, *hint{};
    std::array<lv_obj_t *, 4> nav_btn{}, nav_label{};
} sh;
struct Ring { std::array<lv_obj_t *, 4> seg{}, glow{}; } ring;
struct Widgets {
    lv_obj_t *clock{}, *date{}, *notice{}, *meta{};
    lv_obj_t *cpu_gauge{}, *mem_gauge{}, *cpu_value{}, *mem_value{}, *rx{}, *tx{}, *footer{}, *error{};
    lv_obj_t *soc_gauge{}, *soc_value{}, *soc_title{}, *soc_charge{}, *soc_note{}, *strip{};
    lv_obj_t *protect_head{}, *protect_note{}, *protect_button{}, *protect_label{}, *protect_error{};
    lv_obj_t *bat_v{}, *bat_note{}, *usb_v{}, *usb_note{}, *temp_v{}, *temp_note{}, *charge_card{};
    lv_obj_t *brightness{}, *detail{}, *slider{};
    lv_obj_t *first_action{};
    std::vector<lv_obj_t *> connect_values;
    std::array<lv_obj_t *, 6> swatch{};
} w;
lv_obj_t *g_pairing_bar = nullptr, *g_pairing_time = nullptr, *g_pairing_fill = nullptr;
int g_pairing_percent = -1;

// ---- callbacks: they only ask ui.cpp for something ----
void on_page(lv_event_t *e) { request_page(static_cast<Page>(data_of(e))); }
void on_tab(lv_event_t *e) { request_power_tab(static_cast<PowerTab>(data_of(e))); }
void on_action(lv_event_t *e) { submit_action(static_cast<size_t>(data_of(e))); }
void on_setting(lv_event_t *e) { change_setting(data_of(e)); }
void on_slider(lv_event_t *e) { schedule_brightness(lv_slider_get_value(static_cast<lv_obj_t *>(lv_event_get_target(e)))); }
void on_off(lv_event_t *) { user_screen_off(); }
void on_reset(lv_event_t *) { do_factory_reset(); }
void on_restart(lv_event_t *) { do_restart(); }
void on_theme(lv_event_t *e) { Appearance a = current_appearance(); a.theme = static_cast<ThemeId>(data_of(e)); apply_appearance(a); }
void on_accent(lv_event_t *e) { Appearance a = current_appearance(); a.accent = kAccentChoices[static_cast<size_t>(data_of(e))]; apply_appearance(a); }
void on_lang(lv_event_t *e) { Appearance a = current_appearance(); a.lang = static_cast<Lang>(data_of(e)); apply_appearance(a); }
void on_size(lv_event_t *e) { Appearance a = current_appearance(); a.size = static_cast<SizeTier>(data_of(e)); apply_appearance(a); }
void on_protect(lv_event_t *) { request_protect_toggle(); }

// ---- small building blocks ----
lv_obj_t *heading(lv_obj_t *parent, const std::string &text, uint32_t rgb) {
    auto *row = holder(parent, kContentW, lh(look.title) + 4);
    label_at(row, text, 0, 0, kContentW, look.title, rgb);
    return row;
}
lv_obj_t *caption(lv_obj_t *parent, const std::string &text, uint32_t rgb) {
    return label(parent, text, kContentW, look.caption, rgb);
}
lv_obj_t *section(lv_obj_t *parent, const std::string &text) {
    auto *row = holder(parent, kContentW, lh(look.caption) + 6);
    label_at(row, text, 2, 2, kContentW - 4, look.caption, P().muted);
    return row;
}
lv_obj_t *card(lv_obj_t *parent, int h) { return panel(parent, kContentW, h, P().card, P().edge); }
void centered(lv_obj_t *l) { lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0); }
void single_line(lv_obj_t *l, int h) { lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); lv_obj_set_height(l, h); }
void flat(lv_obj_t *b) {  // a button without its own card look
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_PRESSED);
}
// A chosen option has the accent border. An unchosen one is transparent inside a track, or a plain card on its own.
void select(lv_obj_t *b, bool on, bool in_track = true) {
    lv_obj_set_style_bg_color(b, hex(on ? P().selected : P().card), 0);
    lv_obj_set_style_bg_opa(b, on || !in_track ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b, on ? 2 : in_track ? 0 : 1, 0);
    lv_obj_set_style_border_color(b, hex(on ? P().accent : P().edge), 0);
}
// A row of equal buttons in one rounded track. Returns the buttons through `out`.
lv_obj_t *segmented(lv_obj_t *parent, const std::vector<std::string> &names, int selected, lv_event_cb_t cb, const std::vector<int> &ids,
                    const std::vector<const lv_font_t *> &fonts, std::vector<lv_obj_t *> *out = nullptr) {
    auto *track = panel(parent, kContentW, 56, P().card, P().edge, 28);
    const int n = static_cast<int>(names.size()), each = (kContentW - 2) / n;
    for (int i = 0; i < n; ++i) {
        auto *b = button(track, cb, data(ids[static_cast<size_t>(i)]), each, 54);
        lv_obj_set_pos(b, i * each, 0);
        lv_obj_set_style_radius(b, 27, 0);
        select(b, i == selected);
        auto *l = label(b, names[static_cast<size_t>(i)], each - 8, fonts[static_cast<size_t>(i)], i == selected ? P().accent : P().muted);
        centered(l);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_height(l, lh(fonts[static_cast<size_t>(i)]));
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        if (out) out->push_back(b);
    }
    return track;
}
lv_obj_t *info_card(lv_obj_t *parent, const std::string &title, const std::string &body, uint32_t body_rgb, lv_obj_t **body_out = nullptr) {
    auto *c = panel(parent, kContentW, LV_SIZE_CONTENT, P().card, P().edge);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_style_pad_row(c, 8, 0);
    label(c, title, kContentW - 34, look.body, P().ink);
    auto *b = label(c, body, kContentW - 34, look.caption, body_rgb);
    if (body_out) *body_out = b;
    return c;
}
// A settings row: name on the left, value on the right.
lv_obj_t *setting_row(lv_obj_t *parent, const std::string &name, const std::string &value, int which, bool active, bool as_pill) {
    const int h = std::max(64, lh(look.body) + 24);
    auto *b = button(parent, on_setting, data(which), kContentW, h);
    auto *n = label_at(b, name, 20, (h - lh(look.body)) / 2, 250, look.body, P().ink);
    single_line(n, lh(look.body));
    auto *v = pill(b, value, active ? P().accent : P().muted, as_pill ? (active ? P().selected : P().card) : P().card, as_pill ? (active ? P().accent : P().edge) : P().card, look.caption);
    if (!as_pill) { lv_obj_set_style_border_width(v, 0, 0); lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0); }
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -14, 0);
    return b;
}

// ---- action cards (Home and Actions) ----
lv_obj_t *action_card(lv_obj_t *parent, size_t index, int width, int height, const ActionView &a) {
    const uint32_t tones[] = {P().accent, P().wifi, P().clock, P().power};
    auto *b = button(parent, on_action, data(static_cast<int>(index)), width, height);
    if (a.enabled && index == 0) {
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, hex(P().accent), 0);
        lv_obj_set_style_bg_color(b, hex(P().selected), 0);
    }
    if (!a.enabled) lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    const int ty = a.enabled ? (height - lh(look.body)) / 2 : 8;
    auto *t = label_at(b, a.label, 20, ty, width - 54, look.body, a.enabled ? (index == 0 ? P().accent : P().ink) : P().muted);
    single_line(t, lh(look.body));
    if (!a.enabled) {
        auto *s = label_at(b, a.sub, 20, height - lh(look.caption) - 6, width - 40, look.caption, P().muted);
        single_line(s, lh(look.caption));
    }
    auto *d = dot(b, 10, a.enabled ? tones[index % 4] : P().edge, a.enabled);
    lv_obj_set_pos(d, width - 26, height / 2 - 5);
    if (!a.enabled) lv_obj_add_state(b, LV_STATE_DISABLED);
    return b;
}

// ---- Home ----
void build_home(lv_obj_t *body, const Frame &f) {
    const int d = 216, x = (kContentW - d) / 2, g = 4, width = 20;
    auto *track = arc_segment(body, d, 0, 360, P().track, width, LV_OPA_COVER);
    lv_obj_set_pos(track, x, 0);
    static constexpr int kStart[4] = {278, 8, 98, 188}, kSweep = 74;
    const int trim = static_cast<int>(1.5f * g / (d / 2.0f) * 57.3f + 0.5f);
    const uint32_t colors[4] = {P().wifi, P().agent, P().power, P().clock};
    for (size_t i = 0; i < 4; ++i) {
        ring.glow[i] = arc_segment(body, d + 2 * g, kStart[i] + trim, kStart[i] + kSweep - trim, colors[i], width + 3 * g, LV_OPA_TRANSP);
        lv_obj_set_pos(ring.glow[i], x - g, -g);
        ring.seg[i] = arc_segment(body, d, kStart[i], kStart[i] + kSweep, colors[i], width, LV_OPA_TRANSP);
        lv_obj_set_pos(ring.seg[i], x, 0);
    }
    auto *hit = button(body, on_page, data(static_cast<int>(Page::Clock)), 176, 124);
    lv_obj_set_pos(hit, (kContentW - 176) / 2, 40);
    flat(hit);
    w.clock = label_at(hit, "--:--", 0, 22, 176, &lv_font_montserrat_48, P().ink);
    centered(w.clock);
    w.date = label_at(hit, "", 0, 22 + 54 + 6, 176, look.caption, P().muted);
    centered(w.date);
    single_line(w.date, lh(look.caption));
    w.notice = label_at(body, "", 0, 226, kContentW, look.caption, P().muted);
    centered(w.notice);
    single_line(w.notice, lh(look.caption));
    const auto actions = make_actions(look.lang, f.s);
    const int top = 256, h = 80;
    if (actions.empty()) {
        auto *b = button(body, on_page, data(static_cast<int>(Page::Connect)), kContentW, h);
        lv_obj_set_pos(b, 0, top);
        auto *t = label_at(b, T(f.s.agent_ready ? Str::NoActions : Str::SetupPairing), 20, 8, kContentW - 40, look.body, P().ink);
        single_line(t, lh(look.body));
        auto *s = label_at(b, T(f.s.agent_ready ? Str::NoActionsHint : Str::SetupHint), 20, 8 + lh(look.body), kContentW - 40, look.caption, P().muted);
        single_line(s, lh(look.caption));
    }
    for (size_t i = 0; i < std::min<size_t>(2, actions.size()); ++i) {
        auto *b = action_card(body, i, 210, h, actions[i]);
        lv_obj_set_pos(b, static_cast<int>(i) * 222, top);
        if (i == 0) w.first_action = b;
    }
}
std::string notice_for(const Frame &f, Tone &tone) {
    tone = Tone::Warn;
    if (!f.s.error.empty()) return dynamic_text(look.lang, f.s.error);
    const std::string &u = f.s.update_state;
    if (u == "downloading" || u == "writing" || u == "verifying" || u == "rebooting" || u == "pending_confirmation") {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s %d%% | %s", T(Str::UpdateLabel).c_str(), f.s.update_progress, u.c_str());
        return buf;
    }
    tone = Tone::Off;
    const std::string n = notice_text(look.lang, f.s.command, f.now_ms, f.command_changed_ms);
    if (!n.empty()) return n;
    if (!f.s.agent_ready && f.s.sampled_ms != 0 && !metrics_fresh(f.s, f.now_ms)) { tone = Tone::Warn; return T(Str::MetricsExpired); }
    return "";
}
void refresh_home(const Frame &f) {
    const ClockView c = make_clock(look.lang, f.s.time_valid, f.wall);
    set_text(w.clock, c.hhmm);
    set_text(w.date, c.date);
    Tone tone;
    const std::string n = notice_for(f, tone);
    set_optional_text(w.notice, n);
    set_ink(w.notice, tone_color(tone));
    const RingView r = make_ring(f.s, f.power, f.now_ms);
    const bool lit[4] = {r.wifi, r.agent, r.power, r.time};
    for (size_t i = 0; i < 4; ++i) {
        const lv_opa_t seg = lit[i] ? LV_OPA_COVER : LV_OPA_TRANSP, halo = lit[i] ? LV_OPA_20 : LV_OPA_TRANSP;
        if (lv_obj_get_style_arc_opa(ring.seg[i], LV_PART_INDICATOR) != seg) lv_obj_set_style_arc_opa(ring.seg[i], seg, LV_PART_INDICATOR);
        if (lv_obj_get_style_arc_opa(ring.glow[i], LV_PART_INDICATOR) != halo) lv_obj_set_style_arc_opa(ring.glow[i], halo, LV_PART_INDICATOR);
    }
}

// ---- Clock ----
void build_clock(lv_obj_t *body) {
    w.clock = label_at(body, "--:--", 0, 60, kContentW, &font_clock_72, P().ink);
    centered(w.clock);
    w.date = label_at(body, "", 0, 60 + 70, kContentW, look.title, P().ink);
    centered(w.date);
    auto *line = holder(body, 56, 3);
    lv_obj_set_pos(line, (kContentW - 56) / 2, 60 + 70 + lh(look.title) + 8);
    lv_obj_set_style_bg_color(line, hex(P().accent), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(line, 2, 0);
    w.meta = label_at(body, "", 0, 60 + 70 + lh(look.title) + 20, kContentW, look.caption, P().muted);
    centered(w.meta);
}
void refresh_clock(const Frame &f) {
    ClockView c = make_clock(look.lang, f.s.time_valid, f.wall);
    set_text(w.clock, f.c.layout.show_seconds && c.valid ? c.hhmm + ":" + c.seconds : c.hhmm);
    set_text(w.date, c.date);
    const std::string &q = f.s.time_quality;
    Str source = Str::TimeValid;
    if (q == "manual") source = Str::TimeSourceUsb;
    else if (q == "rtc") source = Str::TimeSourceRtc;
    else if (q == "sntp" || q == "ntp") source = Str::TimeSourceNetwork;
    set_text(w.meta, T(c.valid ? source : Str::TimeInvalidHint));
}

// ---- Actions ----
void build_actions(lv_obj_t *body, const Frame &f) {
    heading(body, T(Str::NavActions), P().accent);
    const auto actions = make_actions(look.lang, f.s);
    if (actions.empty()) {
        auto *b = button(body, on_page, data(static_cast<int>(Page::Connect)), kContentW, 88);
        auto *t = label_at(b, T(f.s.agent_ready ? Str::NoActions : Str::SetupPairing), 20, 8, kContentW - 40, look.body, P().ink);
        single_line(t, lh(look.body));
        auto *s = label_at(b, T(f.s.agent_ready ? Str::NoActionsHint : Str::SetupHint), 20, 8 + lh(look.body), kContentW - 40, look.caption, P().muted);
        lv_obj_set_height(s, lh(look.caption) * 2);
        return;
    }
    for (size_t i = 0; i < actions.size(); ++i) {
        auto *b = action_card(body, i, kContentW, std::max(80, lh(look.body) + lh(look.caption) + 20), actions[i]);
        if (i == 0) w.first_action = b;
    }
}
void refresh_actions(const Frame &) {}

// ---- Computer ----
void build_computer(lv_obj_t *body) {
    heading(body, T(Str::ComputerTitle), P().accent);
    auto *row = holder(body, kContentW, 172);
    const uint32_t tint[2] = {P().wifi, P().clock};
    lv_obj_t **gauges[2] = {&w.cpu_gauge, &w.mem_gauge};
    lv_obj_t **values[2] = {&w.cpu_value, &w.mem_value};
    const Str names[2] = {Str::Cpu, Str::Memory};
    for (int i = 0; i < 2; ++i) {
        auto *c = panel(row, 210, 172, P().card, P().edge);
        lv_obj_set_pos(c, i * 222, 0);
        *gauges[i] = gauge(c, 128, 12, tint[i]);
        lv_obj_set_pos(*gauges[i], 40, 8);
        *values[i] = label_at(c, "--", 0, 8 + 64 - lh(look.title) / 2, 208, look.title, P().ink);
        centered(*values[i]);
        auto *n = label_at(c, T(names[i]), 0, 138, 208, look.caption, tint[i]);
        centered(n);
    }
    const int rate_h = std::max(60, lh(look.caption) + lh(look.body) + 14);
    auto *rates = holder(body, kContentW, rate_h);
    const Str rate_names[2] = {Str::Receive, Str::Send};
    const char *icons[2] = {LV_SYMBOL_DOWNLOAD, LV_SYMBOL_UPLOAD};
    lv_obj_t **vals[2] = {&w.rx, &w.tx};
    for (int i = 0; i < 2; ++i) {
        auto *c = panel(rates, 210, rate_h, P().card, P().edge, 20);
        lv_obj_set_pos(c, i * 222, 0);
        label_at(c, icons[i], 14, (rate_h - 24) / 2, 30, &lv_font_montserrat_24, tint[i]);
        label_at(c, T(rate_names[i]), 52, 4, 150, look.caption, P().muted);
        *vals[i] = label_at(c, "--", 52, 4 + lh(look.caption), 154, look.body, P().ink);
        single_line(*vals[i], lh(look.body));
    }
    w.footer = caption(body, "", P().muted);
    w.error = caption(body, "", P().power);
}
void refresh_computer(const Frame &f) {
    const ComputerView v = make_computer(look.lang, f.s, f.now_ms);
    gauge_set(w.cpu_gauge, v.cpu.valid, v.cpu.percent, P().wifi);
    gauge_set(w.mem_gauge, v.memory.valid, v.memory.percent, P().clock);
    set_text(w.cpu_value, v.cpu.text);
    set_text(w.mem_value, v.memory.text);
    set_text(w.rx, v.rx);
    set_text(w.tx, v.tx);
    set_text(w.footer, v.footer);
    set_ink(w.footer, tone_color(v.footer_tone == Tone::Ok ? Tone::Off : v.footer_tone));
    set_optional_text(w.error, dynamic_text(look.lang, f.s.error));
}

// ---- Settings ----
void build_settings(lv_obj_t *body) {
    heading(body, T(Str::SettingsTitle), P().accent);
    struct Row { Str name, sub; Page page; uint32_t tone; };
    const Row rows[] = {{Str::SettingDisplay, Str::SettingDisplaySub, Page::Display, P().accent},
                        {Str::SettingAppearance, Str::SettingAppearanceSub, Page::Appearance, P().clock},
                        {Str::SettingConnection, Str::SettingConnectionSub, Page::Connect, P().wifi},
                        {Str::SettingPower, Str::SettingPowerSub, Page::Power, P().power},
                        {Str::SettingClock, Str::SettingClockSub, Page::Clock, P().agent},
                        {Str::SettingDevice, Str::SettingDeviceSub, Page::Device, P().muted}};
    const int h = std::max(72, lh(look.body) + lh(look.caption) + 16);
    for (const Row &r : rows) {
        auto *b = button(body, on_page, data(static_cast<int>(r.page)), kContentW, h);
        auto *chip = holder(b, 40, 40);
        lv_obj_set_pos(chip, 16, (h - 40) / 2);
        lv_obj_set_style_radius(chip, 12, 0);
        lv_obj_set_style_bg_color(chip, hex(P().selected), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_center(dot(chip, 12, r.tone));
        auto *n = label_at(b, T(r.name), 70, 6, 310, look.body, P().ink);
        single_line(n, lh(look.body));
        auto *s = label_at(b, T(r.sub), 70, 6 + lh(look.body), 310, look.caption, P().muted);
        single_line(s, lh(look.caption));
        label_at(b, LV_SYMBOL_RIGHT, kContentW - 36, (h - 14) / 2 - 2, 20, &lv_font_montserrat_14, P().muted);
    }
}

// ---- Power ----
void build_power(lv_obj_t *body, PowerTab tab, const Frame &f) {
    heading(body, T(Str::PowerTitle), P().power);
    segmented(body, {T(Str::SegLevel), T(Str::SegReadings), T(Str::SegProtect)}, static_cast<int>(tab), on_tab,
              {static_cast<int>(PowerTab::Level), static_cast<int>(PowerTab::Readings), static_cast<int>(PowerTab::Protect)},
              {look.body, look.body, look.body});
    if (tab == PowerTab::Level) {
        const int h = 150;  // the design's 186 px would push the protection strip under the tabs
        auto *c = card(body, h);
        w.soc_gauge = gauge(c, 112, 11, P().power);
        lv_obj_set_pos(w.soc_gauge, 20, 18);
        w.soc_value = label_at(c, "--", 20, 18 + 56 - lh(look.title) / 2, 112, look.title, P().ink);
        centered(w.soc_value);
        w.soc_title = label_at(c, "", 150, 14, 264, look.title, P().ink);
        single_line(w.soc_title, lh(look.title));
        w.soc_charge = pill(c, "", P().agent, P().card, P().edge, look.caption);
        lv_obj_set_pos(w.soc_charge, 150, 14 + lh(look.title) + 6);
        w.soc_note = label_at(c, "", 150, 14 + lh(look.title) + 6 + lh(look.caption) + 14, 264, look.caption, P().muted);
        auto *b = button(body, on_tab, data(static_cast<int>(PowerTab::Protect)), kContentW, 56);
        w.strip = label_at(b, T(Str::ProtectionOff), 18, (56 - lh(look.caption)) / 2, 250, look.caption, P().ink);
        single_line(w.strip, lh(look.caption));
        auto *tag = pill(b, T(Str::Experimental), P().power, P().card, P().power, look.caption);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -14, 0);
        (void)f;
    } else if (tab == PowerTab::Readings) {
        struct Reading { Str name; lv_obj_t **value; lv_obj_t **note; };
        const Reading list[] = {{Str::BatteryVoltage, &w.bat_v, &w.bat_note}, {Str::UsbVoltage, &w.usb_v, &w.usb_note}, {Str::ChipTemp, &w.temp_v, &w.temp_note}};
        const int h = lh(look.caption) * 2 + lh(look.title) + 24;
        for (const Reading &r : list) {
            auto *c = card(body, h);
            label_at(c, T(r.name), 18, 10, 396, look.caption, P().muted);
            *r.value = label_at(c, "--", 18, 10 + lh(look.caption), 396, look.title, P().ink);
            *r.note = label_at(c, "", 18, 10 + lh(look.caption) + lh(look.title), 396, look.caption, P().muted);
            single_line(*r.note, lh(look.caption));
        }
        caption(body, T(Str::ChipNotCell), P().muted);
    } else {
        auto *c = panel(body, kContentW, LV_SIZE_CONTENT, P().card, P().edge);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(c, 18, 0);
        lv_obj_set_style_pad_row(c, 10, 0);
        auto *title_row = holder(c, kContentW - 38, std::max(lh(look.body), lh(look.caption) + 6) + 4);
        lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(title_row, 12, 0);
        auto *protect_name = label(title_row, T(Str::ProtectTitle), kContentW, look.body, P().ink);
        lv_obj_set_width(protect_name, LV_SIZE_CONTENT);
        pill(title_row, T(Str::Experimental), P().power, P().card, P().power, look.caption);
        w.protect_head = label(c, "", kContentW - 38, look.caption, P().ink);
        w.protect_note = label(c, "", kContentW - 38, look.caption, P().muted);
        label(c, T(Str::ProtectionRule1), kContentW - 38, look.caption, P().muted);
        label(c, T(Str::ProtectionRule2), kContentW - 38, look.caption, P().muted);
        // The button turns protection on (through the dialog) or off. While it cannot work it stays visible and disabled,
        // and the note above says why. The text and the state come from make_charge in refresh_power.
        w.protect_button = button(c, on_protect, nullptr, kContentW - 38, 56);
        select(w.protect_button, true);
        lv_obj_set_style_radius(w.protect_button, 28, 0);
        w.protect_label = label(w.protect_button, "", kContentW - 60, look.body, P().accent);
        centered(w.protect_label);
        lv_obj_align(w.protect_label, LV_ALIGN_CENTER, 0, 0);
        w.protect_error = label(c, "", kContentW - 38, look.caption, P().power);
    }
}
void refresh_power(PowerTab tab, const Frame &f) {
    const PowerView v = make_power(look.lang, f.power, f.now_ms);
    if (tab == PowerTab::Level) {
        gauge_set(w.soc_gauge, v.level.valid, v.level.percent, P().power);
        set_text(w.soc_value, v.level.text);
        const HeaderView h = make_header(look.lang, f.s, f.power, f.now_ms);
        set_text(w.soc_title, v.level.valid ? h.battery : v.level_note);
        set_text(w.soc_charge, v.charge);
        set_ink(w.soc_charge, tone_color(v.charge_tone));
        set_border(w.soc_charge, v.charge_tone == Tone::Off ? P().edge : tone_color(v.charge_tone));
        set_optional_text(w.soc_note, v.level.valid ? v.level_note : std::string());
        const ChargeView cv = make_charge(look.lang, f.charge, f.power, f.now_ms);
        set_text(w.strip, cv.strip);
        set_ink(w.strip, cv.tone == Tone::Off ? P().ink : tone_color(cv.tone));
    } else if (tab == PowerTab::Protect) {
        const ChargeView cv = make_charge(look.lang, f.charge, f.power, f.now_ms);
        set_text(w.protect_head, cv.headline);
        set_ink(w.protect_head, cv.tone == Tone::Off ? P().ink : tone_color(cv.tone));
        set_optional_text(w.protect_note, cv.note.empty() ? cv.why_disabled : cv.note);
        set_text(w.protect_label, cv.button);
        if (cv.button_enabled == lv_obj_has_state(w.protect_button, LV_STATE_DISABLED)) {
            if (cv.button_enabled) lv_obj_remove_state(w.protect_button, LV_STATE_DISABLED);
            else lv_obj_add_state(w.protect_button, LV_STATE_DISABLED);
        }
        set_optional_text(w.protect_error, f.charge_error);
    } else if (tab == PowerTab::Readings) {
        set_text(w.bat_v, v.battery_v); set_optional_text(w.bat_note, v.battery_v_note);
        set_text(w.usb_v, v.usb_v); set_optional_text(w.usb_note, v.usb_v_note);
        set_text(w.temp_v, v.chip_temp); set_optional_text(w.temp_note, v.temp_note);
    }
}

// ---- Connect ----
void build_connect(lv_obj_t *body, const Frame &f) {
    heading(body, T(Str::ConnectTitle), P().wifi);
    const auto rows = make_connect(look.lang, f.s, f.paired);
    const int row_h = std::max(44, lh(look.body) + 8);
    auto *c = card(body, row_h * static_cast<int>(rows.size()) + 12);
    int y = 6;
    for (const auto &r : rows) {
        label_at(c, r.key, 20, y + (row_h - lh(look.caption)) / 2, 150, look.caption, P().muted);
        auto *v = label_at(c, r.value, 170, y + (row_h - lh(look.body)) / 2, 242, look.body, r.tone == Tone::Off ? P().ink : tone_color(r.tone));
        single_line(v, lh(look.body));
        w.connect_values.push_back(v);
        y += row_h;
    }
    caption(body, T(Str::ConnectHint), P().muted);
    w.error = caption(body, "", P().power);
}
void refresh_connect(const Frame &f) {
    const auto rows = make_connect(look.lang, f.s, f.paired);
    for (size_t i = 0; i < rows.size() && i < w.connect_values.size(); ++i) {
        set_text(w.connect_values[i], rows[i].value);
        set_ink(w.connect_values[i], rows[i].tone == Tone::Off ? P().ink : tone_color(rows[i].tone));
    }
    set_optional_text(w.error, dynamic_text(look.lang, f.s.error));
}

// ---- Appearance ----
void build_appearance(lv_obj_t *body, const Frame &f) {
    const Appearance &a = current_appearance();
    heading(body, T(Str::AppearanceTitle), P().clock);
    section(body, T(Str::KeyTheme));
    auto *themes = holder(body, kContentW, 56);
    const Str theme_names[3] = {Str::ThemeMint, Str::ThemeOcean, Str::ThemeAmber};
    for (int i = 0; i < 3; ++i) {
        const Palette &tp = theme_palette(static_cast<ThemeId>(i));
        auto *b = button(themes, on_theme, data(i), 136, 56);
        lv_obj_set_pos(b, i * 148, 0);
        lv_obj_set_style_radius(b, 18, 0);
        select(b, a.theme == static_cast<ThemeId>(i), false);
        auto *bar = holder(b, 56, 10);
        lv_obj_set_pos(bar, 38, 8);
        lv_obj_set_style_radius(bar, 5, 0);
        lv_obj_set_style_bg_color(bar, hex(tp.accent), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        auto *l = label_at(b, T(theme_names[i]), 0, 24, 134, look.caption, a.theme == static_cast<ThemeId>(i) ? P().accent : P().ink);
        centered(l);
        single_line(l, lh(look.caption));
    }
    section(body, T(Str::KeyAccent));
    auto *accents = holder(body, kContentW, 56);
    for (int i = 0; i < 6; ++i) {
        const uint32_t color = kAccentChoices[static_cast<size_t>(i)];
        auto *b = button(accents, on_accent, data(i), 44, 44);
        lv_obj_set_pos(b, i * 72, 6);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, hex(color), 0);
        lv_obj_set_style_bg_color(b, hex(color), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, P().accent == color ? 4 : 1, 0);
        lv_obj_set_style_border_color(b, hex(P().accent == color ? P().ink : P().edge), 0);
        w.swatch[static_cast<size_t>(i)] = b;
    }
    section(body, T(Str::KeyLanguage));
    segmented(body, {T(Str::LangZh), T(Str::LangEn)}, static_cast<int>(a.lang), on_lang, {static_cast<int>(Lang::ZhCN), static_cast<int>(Lang::EnUS)}, {look.body, look.body});
    section(body, T(Str::KeyTextSize));
    segmented(body, {T(Str::SizeSmall), T(Str::SizeStandard), T(Str::SizeLarge)}, static_cast<int>(a.size), on_size,
              {static_cast<int>(SizeTier::Small), static_cast<int>(SizeTier::Standard), static_cast<int>(SizeTier::Large)},
              {font_for(font_px(SizeTier::Small).body), font_for(font_px(SizeTier::Standard).body), font_for(font_px(SizeTier::Large).body)});
    w.error = caption(body, "", P().power);
    set_optional_text(w.error, f.last_error);
}
void refresh_appearance(const Frame &f) { set_optional_text(w.error, f.last_error); }

// ---- Display ----
void build_display(lv_obj_t *body, const Frame &f) {
    const DisplayConfig &c = f.c;
    heading(body, T(Str::DisplayTitle), P().accent);
    const int card_h = lh(look.body) + lh(look.caption) + 70;
    auto *b = card(body, card_h);
    label_at(b, T(Str::Brightness), 20, 8, 160, look.body, P().ink);
    w.brightness = label_at(b, "", 170, 8, 242, look.caption, P().accent);
    lv_obj_set_style_text_align(w.brightness, LV_TEXT_ALIGN_RIGHT, 0);
    label_at(b, T(Str::BrightnessPair), 20, 8 + lh(look.body), 250, look.caption, P().muted);
    w.slider = lv_slider_create(b);
    lv_obj_set_size(w.slider, 384, 10);
    lv_obj_set_pos(w.slider, 24, card_h - 28);
    lv_obj_set_ext_click_area(w.slider, 23);
    lv_obj_set_style_bg_color(w.slider, hex(P().track), LV_PART_MAIN);
    lv_obj_set_style_bg_color(w.slider, hex(P().accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(w.slider, hex(P().ink), LV_PART_KNOB);
    lv_obj_set_style_pad_all(w.slider, 6, LV_PART_KNOB);
    lv_slider_set_range(w.slider, 3, 100);
    lv_slider_set_value(w.slider, c.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(w.slider, on_slider, LV_EVENT_VALUE_CHANGED, nullptr);
    setting_row(body, T(Str::AlwaysOn), T(c.off_s ? Str::Off : Str::On), 10, c.off_s == 0, true);
    caption(body, T(Str::AlwaysOnHelp), P().muted);
    auto *off = button(body, on_off, nullptr, kContentW, 64);
    label_at(off, T(Str::TurnScreenOff), 20, (64 - lh(look.body)) / 2, 230, look.body, P().ink);
    auto *wake = label_at(off, T(Str::TouchToWake), 250, (64 - lh(look.caption)) / 2, 164, look.caption, P().muted);
    lv_obj_set_style_text_align(wake, LV_TEXT_ALIGN_RIGHT, 0);
    single_line(wake, lh(look.caption));
    char buf[48];
    snprintf(buf, sizeof(buf), tr(look.lang, Str::RotationValue), c.rotation);
    setting_row(body, T(Str::Rotation), buf, 9, false, false);
    w.error = caption(body, "", P().power);
    section(body, T(Str::SectionSleep));
    auto seconds = [&](uint32_t v) { snprintf(buf, sizeof(buf), tr(look.lang, Str::SecondsValue), static_cast<int>(v)); return std::string(buf); };
    setting_row(body, T(Str::ClockAfter), seconds(c.clock_s), 1, false, false);
    setting_row(body, T(Str::DimAfter), seconds(c.dim_s), 2, false, false);
    setting_row(body, T(Str::OffAfter), c.off_s ? seconds(c.off_s) : T(Str::AlwaysOnNow), 3, false, false);
    section(body, T(Str::SectionNight));
    setting_row(body, T(Str::NightLimit), T(c.night_enabled ? Str::On : Str::Off), 0, c.night_enabled, true);
    snprintf(buf, sizeof(buf), "%02u:%02u", c.night_start / 60, c.night_start % 60);
    setting_row(body, T(Str::NightStart), buf, 4, false, false);
    snprintf(buf, sizeof(buf), "%02u:%02u", c.night_end / 60, c.night_end % 60);
    setting_row(body, T(Str::NightEnd), buf, 5, false, false);
    snprintf(buf, sizeof(buf), "%u%%", c.night_cap);
    setting_row(body, T(Str::NightBrightness), buf, 6, false, false);
    setting_row(body, T(Str::ScreenProtection), T(c.protection ? Str::On : Str::Off), 7, c.protection, true);
    w.detail = caption(body, "", P().accent);
}
void refresh_display(const Frame &f) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%u%%  /  %d%%%s", f.c.brightness, f.effective_brightness, f.config_pending ? tr(look.lang, Str::PendingSave) : "");
    set_text(w.brightness, buf);
    std::string err = f.rotation_error.empty() ? std::string() : f.rotation_error;
    set_optional_text(w.error, err);
    set_optional_text(w.detail, f.last_error);
}

// ---- Device ----
void build_device(lv_obj_t *body, const Frame &f) {
    heading(body, T(Str::DeviceTitle), P().accent);
    info_card(body, std::string("DeskHub ") + VERSION, "", P().muted, &w.detail);
    auto *r = button(body, on_restart, nullptr, kContentW, 64);
    auto *l = label_at(r, T(Str::Restart), 20, (64 - lh(look.body)) / 2, 380, look.body, P().ink);
    single_line(l, lh(look.body));
    caption(body, T(Str::FactoryResetHelp), P().muted);
    auto *x = button(body, on_reset, nullptr, kContentW, 64);
    lv_obj_set_style_bg_color(x, hex(0x2a1414), 0);
    lv_obj_set_style_border_color(x, hex(0x6b2a2a), 0);
    auto *xl = label_at(x, T(f.confirm_reset ? Str::FactoryResetConfirm : Str::FactoryReset), 20, (64 - lh(look.body)) / 2, 380, look.body, P().danger);
    single_line(xl, lh(look.body));
    w.error = caption(body, "", P().power);
}
void refresh_device(const Frame &f) {
    char buf[320];
    snprintf(buf, sizeof(buf), "%s\n%s\n%s %u KiB\n%s %s %d%%\n%s", f.s.device_id.c_str(), f.power.reason.c_str(), T(Str::FreeHeap).c_str(),
             static_cast<unsigned>(esp_get_free_heap_size() / 1024), T(Str::UpdateLabel).c_str(), f.s.update_state.c_str(), f.s.update_progress, f.s.update_error.c_str());
    set_text(w.detail, buf);
    set_optional_text(w.error, f.last_error);
}
}  // namespace

// ---------------------------------------------------------------- shell
void build_shell(lv_obj_t *screen) {
    sh = Shell{};
    const Palette &p = P();
    lv_obj_set_style_bg_color(screen, hex(p.bg), 0);
    lv_obj_set_style_bg_grad_color(screen, hex(p.bg2), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, hex(p.ink), 0);
    const int hh = 62, line2 = 4 + lh(look.caption) + 4;
    sh.left = button(screen, on_page, data(static_cast<int>(Page::Connect)), 212, hh);
    lv_obj_set_pos(sh.left, kMargin, 0);
    flat(sh.left);
    sh.agent_dot = dot(sh.left, 10, p.edge, false);
    lv_obj_set_pos(sh.agent_dot, 2, 4 + (lh(look.caption) - 10) / 2);
    sh.agent = label_at(sh.left, "", 20, 4, 190, look.caption, p.muted);
    single_line(sh.agent, lh(look.caption));
    sh.wifi_icon = label_at(sh.left, LV_SYMBOL_WIFI, 0, line2 + (lh(look.caption) - 14) / 2, 22, &lv_font_montserrat_14, p.muted);
    sh.wifi = label_at(sh.left, "", 26, line2, 186, look.caption, p.muted);
    single_line(sh.wifi, lh(look.caption));
    sh.right = button(screen, on_page, data(static_cast<int>(Page::Power)), 212, hh);
    lv_obj_set_pos(sh.right, 244, 0);
    flat(sh.right);
    sh.battery = label_at(sh.right, "", 0, 4, 168, look.caption, p.muted);
    lv_obj_set_style_text_align(sh.battery, LV_TEXT_ALIGN_RIGHT, 0);
    single_line(sh.battery, lh(look.caption));
    sh.outline = panel(sh.right, 32, 17, p.bg, p.muted, 4);
    lv_obj_set_pos(sh.outline, 174, 4 + (lh(look.caption) - 17) / 2);
    lv_obj_set_style_bg_opa(sh.outline, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sh.outline, 2, 0);
    sh.terminal = panel(sh.right, 3, 8, p.muted, p.muted, 1);
    lv_obj_set_style_border_width(sh.terminal, 0, 0);
    lv_obj_set_pos(sh.terminal, 207, 4 + (lh(look.caption) - 8) / 2);
    sh.fill = holder(sh.outline, 4, 9);
    lv_obj_set_pos(sh.fill, 1, 2);
    lv_obj_set_style_radius(sh.fill, 2, 0);
    lv_obj_set_style_bg_color(sh.fill, hex(p.agent), 0);
    lv_obj_set_style_bg_opa(sh.fill, LV_OPA_COVER, 0);
    lv_obj_add_flag(sh.fill, LV_OBJ_FLAG_HIDDEN);
    sh.charge = label_at(sh.right, "", 0, line2, 212, look.caption, p.muted);
    lv_obj_set_style_text_align(sh.charge, LV_TEXT_ALIGN_RIGHT, 0);
    single_line(sh.charge, lh(look.caption));

    sh.nav = panel(screen, kContentW, 64, p.card, p.edge, 32);
    lv_obj_set_pos(sh.nav, kMargin, kNavTop);
    const Str names[4] = {Str::NavOverview, Str::NavActions, Str::NavComputer, Str::NavSettings};
    const Page targets[4] = {Page::Home, Page::Actions, Page::Computer, Page::Settings};
    for (int i = 0; i < 4; ++i) {
        sh.nav_btn[static_cast<size_t>(i)] = button(sh.nav, on_page, data(static_cast<int>(targets[i])), 104, 56);
        lv_obj_set_pos(sh.nav_btn[static_cast<size_t>(i)], 3 + i * 107, 3);
        lv_obj_set_style_radius(sh.nav_btn[static_cast<size_t>(i)], 28, 0);
        lv_obj_set_style_border_width(sh.nav_btn[static_cast<size_t>(i)], 1, 0);
        auto *l = label(sh.nav_btn[static_cast<size_t>(i)], T(names[i]), 100, nav_font(), p.muted);
        centered(l);
        single_line(l, lh(nav_font()));
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        sh.nav_label[static_cast<size_t>(i)] = l;
    }
    sh.hint = label_at(screen, LV_SYMBOL_DOWN, 230, 386, 20, &lv_font_montserrat_14, p.accent);
    lv_obj_add_flag(sh.hint, LV_OBJ_FLAG_HIDDEN);
}
void refresh_shell(const Frame &f, Page page, bool pairing_active) {
    const Palette &p = P();
    const HeaderView h = make_header(look.lang, f.s, f.power, f.now_ms);
    set_text(sh.agent, h.agent);
    set_ink(sh.agent, tone_color(h.agent_tone));
    set_bg(sh.agent_dot, h.agent_tone == Tone::Ok ? p.agent : p.edge);
    const lv_opa_t glow = h.agent_tone == Tone::Ok ? LV_OPA_50 : LV_OPA_TRANSP;
    if (lv_obj_get_style_shadow_opa(sh.agent_dot, LV_PART_MAIN) != glow) {
        lv_obj_set_style_shadow_width(sh.agent_dot, 14, 0);
        lv_obj_set_style_shadow_color(sh.agent_dot, hex(p.agent), 0);
        lv_obj_set_style_shadow_opa(sh.agent_dot, glow, 0);
    }
    set_text(sh.wifi, h.wifi);
    set_ink(sh.wifi_icon, h.wifi_tone == Tone::Off ? p.muted : tone_color(h.wifi_tone == Tone::Ok ? Tone::Info : h.wifi_tone));
    set_ink(sh.wifi, p.muted);
    set_text(sh.battery, h.battery);
    const uint32_t bc = tone_color(h.battery_tone);
    set_ink(sh.battery, h.battery_tone == Tone::Off ? p.muted : bc);
    set_border(sh.outline, bc);
    set_bg(sh.terminal, bc);
    set_border(sh.terminal, bc);
    set_bg(sh.fill, bc);
    if (h.battery_fill >= 0) {
        const int width = std::max(2, 26 * h.battery_fill / 100);
        if (lv_obj_get_width(sh.fill) != width) lv_obj_set_width(sh.fill, width);
        set_hidden(sh.fill, false);
    } else set_hidden(sh.fill, true);
    set_text(sh.charge, h.charge);
    set_ink(sh.charge, h.charge_tone == Tone::Warn ? p.power : p.muted);
    const int active = page == Page::Home || page == Page::Clock ? 0 : page == Page::Actions ? 1 : page == Page::Computer ? 2 : 3;
    for (int i = 0; i < 4; ++i) {
        const bool on = i == active;
        auto *b = sh.nav_btn[static_cast<size_t>(i)];
        set_bg(b, on ? p.selected : p.card);
        set_border(b, on ? p.accent : p.card);
        set_ink(sh.nav_label[static_cast<size_t>(i)], on ? p.accent : p.muted);
    }
    for (auto *o : {sh.left, sh.right, sh.nav}) set_hidden(o, pairing_active);
    if (pairing_active) set_hidden(sh.hint, true);
    // Burn-in protection shifts the header with the clock page; ui.cpp passes the shift through translate on the body.
}
void shift_shell(int dx) {
    set_translate(sh.left, dx, 0);
    set_translate(sh.right, dx, 0);
}

// ---------------------------------------------------------------- pages
void remember_scroll(int y) { g_scroll = y; }
int body_scroll_y() { return g_body ? lv_obj_get_scroll_y(g_body) : 0; }

void build_page(lv_obj_t *body, Page page, PowerTab tab, const Frame &f) {
    g_body = body;
    w = Widgets{};
    ring = Ring{};
    lv_obj_clean(body);
    lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
    const bool fixed = page == Page::Home || page == Page::Clock;
    lv_obj_set_layout(body, fixed ? LV_LAYOUT_NONE : LV_LAYOUT_FLEX);
    if (fixed) lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    else {
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(body, 10, 0);
        lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    }
    switch (page) {
    case Page::Home: build_home(body, f); break;
    case Page::Actions: build_actions(body, f); break;
    case Page::Computer: build_computer(body); break;
    case Page::Settings: build_settings(body); break;
    case Page::Power: build_power(body, tab, f); break;
    case Page::Connect: build_connect(body, f); break;
    case Page::Appearance: build_appearance(body, f); break;
    case Page::Display: build_display(body, f); break;
    case Page::Clock: build_clock(body); break;
    case Page::Device: build_device(body, f); break;
    }
    lv_obj_update_layout(body);
    if (!fixed && g_scroll > 0) lv_obj_scroll_to_y(body, g_scroll, LV_ANIM_OFF);
    g_scroll = 0;
}
void refresh_page(Page page, PowerTab tab, const Frame &f) {
    switch (page) {
    case Page::Home: refresh_home(f); break;
    case Page::Actions: refresh_actions(f); break;
    case Page::Computer: refresh_computer(f); break;
    case Page::Settings: break;
    case Page::Power: refresh_power(tab, f); break;
    case Page::Connect: refresh_connect(f); break;
    case Page::Appearance: refresh_appearance(f); break;
    case Page::Display: refresh_display(f); break;
    case Page::Clock: refresh_clock(f); break;
    case Page::Device: refresh_device(f); break;
    }
    const bool more = page != Page::Home && page != Page::Clock && g_body && lv_obj_get_scroll_bottom(g_body) > 3;
    set_hidden(sh.hint, !more);
}

// ---------------------------------------------------------------- state pages
lv_obj_t *build_pairing(lv_obj_t *screen, const PairingInfo &info, int64_t, void (*on_cancel)()) {
    const Palette &p = P();
    auto *root = panel(screen, kScreen, kScreen, p.bg, p.bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_pos(root, 0, 0);
    label_at(root, T(Str::PairingTitle), kMargin, 20, kContentW, look.title, p.accent);
    label_at(root, T(Str::PairingSub), kMargin, 20 + lh(look.title), kContentW, look.caption, p.muted);
    // The vertical budget is tight in the large text size, so the page is laid out from the font heights.
    const int code_h = 12 + lh(look.caption) + 8 + lh(&lv_font_montserrat_48) + 12;
    const int code_y = 20 + lh(look.title) + lh(look.caption) + 8;
    auto *code_card = panel(root, kContentW, code_h, p.selected, p.accent, 28);
    lv_obj_set_pos(code_card, kMargin, code_y);
    lv_obj_set_style_border_width(code_card, 2, 0);
    auto *key = label_at(code_card, T(Str::OneTimeCode), 0, 12, kContentW - 6, look.caption, p.muted);
    centered(key);
    bool good = info.code.size() >= 4 && info.code.size() <= 8;
    for (char c : info.code) if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) good = false;
    auto *code = label_at(code_card, good ? info.code : "------", 0, 12 + lh(look.caption) + 8, kContentW - 6, &lv_font_montserrat_48, p.ink);
    lv_obj_set_style_text_letter_space(code, 6, 0);
    centered(code);
    single_line(code, lh(&lv_font_montserrat_48));
    // The hotspot: its name and its password, both on the screen, because the person has to type them on the phone.
    bool password_good = info.hotspot_password.size() >= 8 && info.hotspot_password.size() <= 12;
    for (unsigned char c : info.hotspot_password) if (c < 33 || c > 126) password_good = false;
    const int row_h = std::max(lh(look.body), lh(look.caption));
    int y = code_y + code_h + 8;
    for (int i = 0; i < 2; ++i) {
        const std::string value = i == 0 ? (info.hotspot.empty() ? std::string("--") : info.hotspot) : (password_good ? info.hotspot_password : std::string(12, '-'));
        auto *name = label_at(root, T(i == 0 ? Str::Hotspot : Str::HotspotPassword), kMargin, y + (row_h - lh(look.caption)) / 2, 190, look.caption, p.muted);
        single_line(name, lh(look.caption));
        auto *v = label_at(root, value, kMargin + 190, y, kContentW - 190, look.body, p.ink);
        single_line(v, lh(look.body));
        y += row_h + 4;
    }
    auto *hint = label_at(root, T(Str::PairingHint), kMargin, y + 2, kContentW, look.caption, p.muted);
    lv_obj_set_height(hint, lh(look.caption) * 2);
    const int bar_y = y + 2 + lh(look.caption) * 2 + 4;
    label_at(root, T(Str::RemainingLabel), kMargin, bar_y + (lh(look.body) - lh(look.caption)) / 2, 250, look.caption, p.muted);
    g_pairing_time = label_at(root, "", kMargin + 250, bar_y, kContentW - 250, look.body, p.ink);
    lv_obj_set_style_text_align(g_pairing_time, LV_TEXT_ALIGN_RIGHT, 0);
    single_line(g_pairing_time, lh(look.body));
    g_pairing_bar = holder(root, kContentW, 8);
    lv_obj_set_pos(g_pairing_bar, kMargin, bar_y + lh(look.body) + 4);
    lv_obj_set_style_radius(g_pairing_bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_pairing_bar, hex(p.track), 0);
    lv_obj_set_style_bg_opa(g_pairing_bar, LV_OPA_COVER, 0);
    g_pairing_fill = holder(g_pairing_bar, 0, 8);
    lv_obj_set_style_radius(g_pairing_fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_pairing_fill, hex(p.accent), 0);
    lv_obj_set_style_bg_opa(g_pairing_fill, LV_OPA_COVER, 0);
    static void (*cancel)() = nullptr;
    cancel = on_cancel;
    auto *b = button(root, [](lv_event_t *) { if (cancel) cancel(); }, nullptr, kContentW, 56);
    lv_obj_set_pos(b, kMargin, kNavTop);
    lv_obj_set_style_radius(b, 28, 0);
    auto *l = label(b, T(Str::CancelPairing), kContentW - 20, look.body, p.ink);
    centered(l);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    return root;
}
void refresh_pairing(const PairingInfo &info, int64_t deadline_ms, int64_t now_ms) {
    const int64_t left_ms = std::max<int64_t>(0, deadline_ms - now_ms);
    const int total = std::max(1, info.total_s);
    const int seconds = static_cast<int>(std::min<int64_t>((left_ms + 999) / 1000, 99 * 60 + 59));
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", seconds / 60, seconds % 60);
    set_text(g_pairing_time, buf);
    const int percent = static_cast<int>(std::min<int64_t>(100, left_ms * 100 / (static_cast<int64_t>(total) * 1000)));
    g_pairing_percent = percent;
    const int width = kContentW * percent / 100;
    if (g_pairing_fill && lv_obj_get_width(g_pairing_fill) != width) lv_obj_set_width(g_pairing_fill, width);
}
int pairing_bar_percent() { return g_pairing_bar && lv_obj_is_valid(g_pairing_bar) ? g_pairing_percent : -1; }

lv_obj_t *build_protect_dialog(lv_obj_t *layer, void (*on_confirm)(int), void (*on_cancel)()) {
    const Palette &p = P();
    static int limit = 80;
    static void (*confirm)(int) = nullptr;
    static void (*cancel)() = nullptr;
    static lv_obj_t *dimmer = nullptr, *limit_buttons[2] = {nullptr, nullptr};
    limit = 80;
    confirm = on_confirm;
    cancel = on_cancel;
    dimmer = lv_obj_create(layer);
    lv_obj_remove_style_all(dimmer);
    lv_obj_set_size(dimmer, kScreen, kScreen);
    lv_obj_set_pos(dimmer, 0, 0);
    lv_obj_set_style_bg_color(dimmer, hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dimmer, LV_OPA_60, 0);
    lv_obj_add_flag(dimmer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dimmer, LV_OBJ_FLAG_SCROLLABLE);
    auto *box = panel(dimmer, 408, LV_SIZE_CONTENT, p.card, p.edge, 28);
    lv_obj_set_pos(box, 36, 40);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 22, 0);
    lv_obj_set_style_pad_row(box, 10, 0);
    label(box, T(Str::ProtectDialogTitle), 360, look.body, p.ink);
    pill(box, T(Str::Experimental), p.power, p.card, p.power, look.caption);
    label(box, T(Str::ProtectDialogBody), 360, look.caption, p.muted);
    label(box, T(Str::Limit), 360, look.caption, p.muted);
    auto *row = holder(box, 360, 56);
    for (int i = 0; i < 2; ++i) {
        auto *b = button(row, [](lv_event_t *e) {
            limit = data_of(e);
            for (int k = 0; k < 2; ++k) select(limit_buttons[k], (k == 0 ? 80 : 90) == limit);
        }, data(i == 0 ? 80 : 90), 175, 56);
        lv_obj_set_pos(b, i * 185, 0);
        lv_obj_set_style_radius(b, 18, 0);
        select(b, i == 0);
        limit_buttons[i] = b;
        auto *l = label(b, i == 0 ? "80%" : "90%", 171, look.body, p.ink);
        centered(l);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    }
    auto *actions = holder(box, 360, 56);
    for (int i = 0; i < 2; ++i) {
        auto *b = button(actions, [](lv_event_t *e) {
            const bool ok = data_of(e) == 1;
            auto *d = dimmer;
            lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
            lv_obj_delete_async(d);
            if (ok && confirm) confirm(limit);
            if (!ok && cancel) cancel();
        }, data(i), 174, 56);
        lv_obj_set_pos(b, i * 186, 0);
        lv_obj_set_style_radius(b, 28, 0);
        if (i == 1) { select(b, true); }
        auto *l = label(b, T(i == 0 ? Str::Cancel : Str::TurnOn), 170, look.body, i == 1 ? p.accent : p.ink);
        centered(l);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    }
    return dimmer;
}

// ---------------------------------------------------------------- test hooks
int header_fill_width() { return sh.fill && !lv_obj_has_flag(sh.fill, LV_OBJ_FLAG_HIDDEN) ? lv_obj_get_width(sh.fill) : 0; }
bool ring_lit(int index) {
    if (index < 0 || index > 3 || !ring.seg[static_cast<size_t>(index)]) return false;
    return lv_obj_get_style_arc_opa(ring.seg[static_cast<size_t>(index)], LV_PART_INDICATOR) != LV_OPA_TRANSP;
}
std::string home_first_action_label() {
    if (!w.first_action) return "";
    for (uint32_t i = 0; i < lv_obj_get_child_count(w.first_action); ++i) {
        auto *c = lv_obj_get_child(w.first_action, i);
        if (lv_obj_check_type(c, &lv_label_class)) return lv_label_get_text(c);
    }
    return "";
}
lv_obj_t *accent_swatch(int index) { return index >= 0 && index < 6 ? w.swatch[static_cast<size_t>(index)] : nullptr; }
}  // namespace dh::ui::detail
