// The pages of the device UI (docs/design Dev-*). Each page has a builder, which makes the objects once, and a
// refresher, which only sets text and colours that the view models in ui_model.cpp say. A refresher never builds.
#include "audio.hpp"
#include "wifi_change.hpp"
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
    lv_obj_t *left{}, *right{}, *wifi{}, *pc{}, *pc_screen{}, *pc_neck{}, *pc_base{}, *usb{}, *bolt{}, *box{}, *body{}, *tip{}, *fill{}, *digits{}, *nav{}, *hint{};
    int bolt_ink_w = 0, bolt_ofs_x = 0;   // the bolt glyph: width of its ink and where the ink starts in its box
    std::array<lv_obj_t *, 2> wifi_cut{}, pc_cut{}, bat_cut{};  // the slash: a line in the colour of the background under a thin one
    std::array<lv_obj_t *, 5> nav_btn{}, nav_label{};
    std::array<Page, 5> nav_page{};
    int nav_count = 0;
} sh;
struct Ring { std::array<lv_obj_t *, 4> seg{}, glow{}; lv_obj_t *track{}; } ring;
int notice_y = 226;   // where the line under the ring of the home page is in the body
struct Widgets {
    lv_obj_t *clock{}, *seconds{}, *date{}, *notice{}, *notice_dot{}, *meta{}, *clock_hit{}, *minute_dot{}, *error{};
    lv_obj_t *digits_group{}, *hands_group{}, *small_clock{}, *small_seconds{};   // the clock page: large digits, or hands with small digits
    std::array<lv_obj_t *, 3> hand{};                                              // hour, minute, second
    std::array<lv_obj_t *, 4> clock_dot{};
    lv_obj_t *soc_gauge{}, *soc_value{}, *soc_title{}, *soc_charge{}, *soc_note{}, *strip{};
    lv_obj_t *protect_head{}, *protect_note{}, *protect_button{}, *protect_label{}, *protect_error{};
    lv_obj_t *bat_v{}, *bat_note{}, *usb_v{}, *usb_note{}, *temp_v{}, *temp_note{}, *charge_card{};
    lv_obj_t *brightness{}, *detail{}, *slider{};
    lv_obj_t *first_action{};
    std::vector<lv_obj_t *> switch_rows, switch_tiles, switch_icons;
    int switch_current = -1;
    std::array<std::array<lv_obj_t *, 3>, 4> pages_ctrl{};
    std::array<lv_obj_t *, 9> icon_btn{};
    lv_obj_t *ble_state{}, *ble_bonds{};
    std::vector<lv_obj_t *> connect_values;
    lv_obj_t *wifi_change_button{}, *wifi_change_note{};
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
void on_percent(lv_event_t *e) { Appearance a = current_appearance(); a.show_percent = data_of(e) == 0; apply_appearance(a); }
void on_ring(lv_event_t *e) { Appearance a = current_appearance(); a.ring = static_cast<RingTier>(data_of(e)); apply_appearance(a); }
void on_pages_toggle(lv_event_t *e) { request_pages_toggle(data_of(e)); }
void on_pages_up(lv_event_t *e) { request_pages_move(data_of(e), -1); }
void on_pages_down(lv_event_t *e) { request_pages_move(data_of(e), +1); }
void on_style(lv_event_t *e) { request_switch_style(static_cast<SwitchStyle>(data_of(e))); }
void on_source(lv_event_t *e) { request_source_edit(static_cast<size_t>(data_of(e))); }
void on_icon(lv_event_t *e) { request_source_icon(static_cast<SourceIcon>(data_of(e))); }
void on_restore(lv_event_t *) { request_source_restore(); }
Str view_name(View v) {
    switch (v) {
    case View::Switch: return Str::NavActions;
    case View::Clock: return Str::NavClock;
    case View::Power: return Str::NavPower;
    case View::Overview: break;
    }
    return Str::NavOverview;
}
Str icon_name(SourceIcon i) {
    switch (i) {
    case SourceIcon::Hdmi: return Str::IconHdmi;
    case SourceIcon::Dp: return Str::IconDp;
    case SourceIcon::UsbC: return Str::IconUsbC;
    case SourceIcon::Laptop: return Str::IconLaptop;
    case SourceIcon::Desktop: return Str::IconDesktop;
    case SourceIcon::Console: return Str::IconConsole;
    case SourceIcon::Tv: return Str::IconTv;
    default: break;
    }
    return Str::IconMonitor;
}

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

// ---- Home ----
void build_home(lv_obj_t *body, const Frame &f) {
    // With no actions nothing is drawn under the ring (docs/design Dev-HomeEmpty): the ring is larger, and the ring with the line
    // under it sits in the middle of the body, so that no blank band is left.
    // The overview has the ring only (docs/design v2 HomeRing): the input sources have a page of their own. The ring and the line
    // under it sit in the middle of the body (as Dev-HomeEmpty did), so that no blank band is left.
    (void)f;
    const int d = 252, x = (kContentW - d) / 2, g = 4, width = (ring_px(look.appearance.ring).home * d + 108) / 216;
    const int y = (kBodyH - (d + 10 + lh(look.caption))) / 2;   // top of the ring in the body; 10: the gap to the line
    auto *track = arc_segment(body, d, 0, 360, P().track, width, LV_OPA_COVER);
    ring.track = track;
    lv_obj_set_pos(track, x, y);
    static constexpr int kStart[4] = {278, 8, 98, 188}, kSweep = 74;
    const int trim = static_cast<int>(1.5f * g / (d / 2.0f) * 57.3f + 0.5f);
    const uint32_t colors[4] = {P().wifi, P().agent, P().power, P().clock};
    for (size_t i = 0; i < 4; ++i) {
        ring.glow[i] = arc_segment(body, d + 2 * g, kStart[i] + trim, kStart[i] + kSweep - trim, colors[i], width + 3 * g, LV_OPA_TRANSP);
        lv_obj_set_pos(ring.glow[i], x - g, y - g);
        ring.seg[i] = arc_segment(body, d, kStart[i], kStart[i] + kSweep, colors[i], width, LV_OPA_TRANSP);
        lv_obj_set_pos(ring.seg[i], x, y);
    }
    // One button for the ring and the clock in it, so that no two touch areas overlap: a tap on the clock opens the clock page,
    // a tap anywhere else on the ring explains the ring. The area is a circle, not its square.
    auto *ring_hit = button(body, [](lv_event_t *) {
        lv_point_t at{};
        lv_indev_get_point(lv_indev_active(), &at);
        lv_area_t clock{};
        lv_obj_get_coords(w.clock_hit, &clock);
        if (at.x >= clock.x1 && at.x <= clock.x2 && at.y >= clock.y1 && at.y <= clock.y2) request_page(Page::Clock);
        else request_ring_info();
    }, data(0), d, d);
    lv_obj_set_pos(ring_hit, x, y);
    flat(ring_hit);
    lv_obj_set_style_radius(ring_hit, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(ring_hit, LV_OBJ_FLAG_ADV_HITTEST);
    auto *hit = holder(body, 176, 124);   // the place of the clock and the date; it takes no touch of its own
    w.clock_hit = hit;
    lv_obj_set_pos(hit, (kContentW - 176) / 2, y + d / 2 - 68);   // 68: the clock and the date are centred in the ring
    w.clock = label_at(hit, "--:--", 0, 22, 176, &lv_font_montserrat_48, P().ink);
    centered(w.clock);
    // the seconds after the time, in the accent, on the same baseline (docs/design v2 OverviewSeconds); only when chosen
    w.seconds = label_at(hit, "", 0, 22 + lv_font_get_line_height(&lv_font_montserrat_48) - lv_font_get_line_height(&lv_font_montserrat_24) - 3, 40,
                         &lv_font_montserrat_24, P().accent);
    lv_obj_add_flag(w.seconds, LV_OBJ_FLAG_HIDDEN);
    w.date = label_at(hit, "", 0, 22 + 54 + 6, 176, look.caption, P().muted);
    centered(w.date);
    single_line(w.date, lh(look.caption));
    notice_y = y + d + 10;
    w.notice = label_at(body, "", 0, notice_y, kContentW, look.caption, P().muted);
    centered(w.notice);
    single_line(w.notice, lh(look.caption));
    w.notice_dot = holder(body, 10, 10);   // in the colour of the dark segment that the line names
    lv_obj_set_style_radius(w.notice_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(w.notice_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(w.notice_dot, LV_OBJ_FLAG_HIDDEN);
}
std::string notice_for(const Frame &f, Tone &tone, bool &ring_line) {
    ring_line = false;
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
    ring_line = true;   // nothing else to say: name the first dark segment, or say where to tap (docs/design Dev-RingInfo)
    return ring_note(look.lang, make_ring_info(look.lang, f.s, f.power, f.now_ms));
}
// The time and, when chosen, its seconds after it. The pair stays centred as one line: the centred label of the time gets
// narrower on its right by what the seconds take, so it never leaves its place. `clock` is `width` wide without seconds.
void place_seconds(lv_obj_t *clock, lv_obj_t *secs, const ClockView &c, bool shown, const lv_font_t *big, const lv_font_t *small, int width) {
    set_text(clock, c.hhmm);
    set_hidden(secs, !shown);
    int narrow = width;
    if (shown) {
        set_text(secs, c.seconds);
        lv_point_t t{}, s{};
        lv_text_get_size(&t, c.hhmm.c_str(), big, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_text_get_size(&s, c.seconds.c_str(), small, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        narrow = width - (4 + s.x);
        const int sx = lv_obj_get_x(clock) + (narrow + t.x) / 2 + 4;
        if (lv_obj_get_x(secs) != sx) lv_obj_set_x(secs, sx);
    }
    if (lv_obj_get_width(clock) != narrow) lv_obj_set_width(clock, narrow);
}
void refresh_home(const Frame &f) {
    const ClockView c = make_clock(look.lang, f.s.time_valid, f.wall);
    place_seconds(w.clock, w.seconds, c, f.c.layout.show_seconds && c.valid, &lv_font_montserrat_48, &lv_font_montserrat_24, 176);
    set_text(w.date, c.date);
    Tone tone;
    bool ring_line = false;
    const std::string n = notice_for(f, tone, ring_line);
    set_optional_text(w.notice, n);
    set_ink(w.notice, tone_color(tone));
    const RingInfoView info = make_ring_info(look.lang, f.s, f.power, f.now_ms);
    if (w.notice_dot) {
        const bool dot = ring_line && info.first_problem >= 0 && !n.empty();
        if (dot) {   // a dot before the words, in the colour of the segment: it is the dark one on the ring
            const uint32_t colors[4] = {P().wifi, P().agent, P().power, P().clock};
            lv_point_t size{};
            lv_text_get_size(&size, n.c_str(), look.caption, 0, 0, kContentW, LV_TEXT_FLAG_NONE);
            lv_obj_set_pos(w.notice_dot, std::max<int>(0, static_cast<int>((kContentW - size.x) / 2 - 18)), notice_y + (lh(look.caption) - 10) / 2);
            lv_obj_set_style_bg_color(w.notice_dot, hex(colors[info.first_problem]), 0);
        }
        if (lv_obj_has_flag(w.notice_dot, LV_OBJ_FLAG_HIDDEN) == dot) { if (dot) lv_obj_remove_flag(w.notice_dot, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(w.notice_dot, LV_OBJ_FLAG_HIDDEN); }
    }
    const RingView r = make_ring(f.s, f.power, f.now_ms);
    const bool lit[4] = {r.wifi, r.agent, r.power, r.time};
    for (size_t i = 0; i < 4; ++i) {
        const lv_opa_t seg = lit[i] ? LV_OPA_COVER : LV_OPA_TRANSP, halo = lit[i] ? LV_OPA_20 : LV_OPA_TRANSP;
        if (lv_obj_get_style_arc_opa(ring.seg[i], LV_PART_INDICATOR) != seg) lv_obj_set_style_arc_opa(ring.seg[i], seg, LV_PART_INDICATOR);
        if (lv_obj_get_style_arc_opa(ring.glow[i], LV_PART_INDICATOR) != halo) lv_obj_set_style_arc_opa(ring.glow[i], halo, LV_PART_INDICATOR);
    }
}

// ---- Clock (docs/design v2 HomeClock) ----
constexpr int kDial = 300;   // the tick ring
// The hands (docs/design v2 ClockHands): length ahead of the centre, length behind it, width. The second hand only with seconds.
struct HandShape { int ahead, behind, width; };
constexpr HandShape kHands[3] = {{67, 0, 8}, {106, 0, 5}, {116, 22, 2}};
std::array<std::array<lv_point_precise_t, 2>, 3> g_hand_points{};
std::array<int, 3> g_hand_deg{-1, -1, -1};   // what is drawn, for the tests
void build_clock(lv_obj_t *body) {
    const int x = (kContentW - kDial) / 2, y = (kBodyH - kDial) / 2, cx = kContentW / 2, cy = y + kDial / 2;
    auto *ticks = tick_ring(body, kDial, P().edge, P().muted);
    lv_obj_set_pos(ticks, x, y);
    g_hand_deg = {-1, -1, -1};
    // The hands, with the digits small under the centre. Shown only when chosen and while the time is known.
    w.hands_group = holder(body, kContentW, kBodyH);
    for (size_t i = 0; i < 3; ++i) {
        auto *h = lv_line_create(w.hands_group);
        g_hand_points[i] = {lv_point_precise_t{cx, cy}, lv_point_precise_t{cx, cy}};
        lv_line_set_points(h, g_hand_points[i].data(), 2);
        lv_obj_set_style_line_width(h, kHands[i].width, 0);
        lv_obj_set_style_line_color(h, hex(i == 2 ? P().accent : P().ink), 0);
        lv_obj_set_style_line_rounded(h, true, 0);
        w.hand[i] = h;
    }
    auto *hub = holder(w.hands_group, 14, 14);
    lv_obj_set_pos(hub, cx - 7, cy - 7);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, hex(P().accent), 0);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
    w.small_clock = label_at(w.hands_group, "--:--", 0, cy + 62, kContentW, &lv_font_montserrat_24, P().ink);
    centered(w.small_clock);
    w.small_seconds = label_at(w.hands_group, "", 0, cy + 62 + lv_font_get_line_height(&lv_font_montserrat_24) - lv_font_get_line_height(&btr_ui_16) - 1, 30,
                               &btr_ui_16, P().accent);
    lv_obj_add_flag(w.small_seconds, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(w.hands_group, LV_OBJ_FLAG_HIDDEN);
    // The large digits with the date, the four dots and where the time comes from (docs/design v2 HomeClock).
    w.digits_group = holder(body, kContentW, kBodyH);
    body = w.digits_group;
    w.minute_dot = holder(body, 10, 10);   // where the minute hand would be, in the accent
    lv_obj_set_style_radius(w.minute_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(w.minute_dot, hex(P().accent), 0);
    lv_obj_set_style_bg_opa(w.minute_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(w.minute_dot, LV_OBJ_FLAG_HIDDEN);
    const int clock_h = lv_font_get_line_height(&font_clock_72);
    w.clock = label_at(body, "--:--", 0, cy - clock_h + 6, kContentW, &font_clock_72, P().ink);
    centered(w.clock);
    w.seconds = label_at(body, "", 0, cy - 30, 40, &lv_font_montserrat_24, P().accent);
    lv_obj_add_flag(w.seconds, LV_OBJ_FLAG_HIDDEN);
    w.date = label_at(body, "", 0, cy + 10, kContentW, look.body, P().muted);
    centered(w.date);
    single_line(w.date, lh(look.body));
    // Four dots in the colours of the ring: lit when that channel is healthy (make_ring), with its name.
    const Str names[4] = {Str::ClockDotWifi, Str::ClockDotAgent, Str::ClockDotPower, Str::ClockDotTime};
    int widths[4], total = 0;
    for (int i = 0; i < 4; ++i) {
        lv_point_t size{};
        lv_text_get_size(&size, T(names[i]).c_str(), look.caption, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        widths[i] = 8 + 5 + size.x;
        total += widths[i] + (i ? 12 : 0);
    }
    const int dots_y = cy + 10 + lh(look.body) + 8;
    int dx = cx - total / 2;
    for (int i = 0; i < 4; ++i) {
        w.clock_dot[static_cast<size_t>(i)] = dot(body, 8, P().edge);
        lv_obj_set_pos(w.clock_dot[static_cast<size_t>(i)], dx, dots_y + (lh(look.caption) - 8) / 2);
        auto *l = label_at(body, T(names[i]), dx + 13, dots_y, widths[i] - 13 + 2, look.caption, P().muted);
        single_line(l, lh(look.caption));
        dx += widths[i] + 12;
    }
    w.meta = label_at(body, "", 40, dots_y + lh(look.caption) + 6, kContentW - 80, look.caption, P().muted);
    centered(w.meta);
    single_line(w.meta, lh(look.caption));
}
void refresh_clock(const Frame &f) {
    ClockView c = make_clock(look.lang, f.s.time_valid, f.wall);
    const bool secs = f.c.layout.show_seconds && c.valid, hands = look.appearance.clock_hands && c.valid;
    set_hidden(w.hands_group, !hands);
    set_hidden(w.digits_group, hands);
    if (hands) {
        place_seconds(w.small_clock, w.small_seconds, c, secs, &lv_font_montserrat_24, &btr_ui_16, kContentW);
        struct tm t{};
        localtime_r(&f.wall, &t);
        const int deg[3] = {(t.tm_hour % 12) * 30 + t.tm_min / 2, t.tm_min * 6 + t.tm_sec / 10, secs ? t.tm_sec * 6 : -1};
        const int cx = kContentW / 2, cy = kBodyH / 2;
        for (size_t i = 0; i < 3; ++i) {
            set_hidden(w.hand[i], deg[i] < 0);
            if (deg[i] < 0 || deg[i] == g_hand_deg[i]) { g_hand_deg[i] = deg[i]; continue; }
            const float a = static_cast<float>(deg[i]) * 3.14159265f / 180.0f, dx = std::sin(a), dy = -std::cos(a);
            g_hand_points[i][0] = {static_cast<lv_value_precise_t>(cx - std::lround(dx * kHands[i].behind)), static_cast<lv_value_precise_t>(cy - std::lround(dy * kHands[i].behind))};
            g_hand_points[i][1] = {static_cast<lv_value_precise_t>(cx + std::lround(dx * kHands[i].ahead)), static_cast<lv_value_precise_t>(cy + std::lround(dy * kHands[i].ahead))};
            lv_line_set_points(w.hand[i], g_hand_points[i].data(), 2);
            g_hand_deg[i] = deg[i];
        }
    } else {
        g_hand_deg = {-1, -1, -1};
    }
    place_seconds(w.clock, w.seconds, c, secs && !hands, &font_clock_72, &lv_font_montserrat_24, kContentW);
    set_text(w.date, c.date);
    if (c.valid) {
        struct tm t{};
        localtime_r(&f.wall, &t);
        const float a = static_cast<float>(t.tm_min) * 6.0f * 3.14159265f / 180.0f, r = kDial / 2.0f - 22;
        const int x = kContentW / 2 + static_cast<int>(std::lround(std::sin(a) * r)) - 5, y = (kBodyH - kDial) / 2 + kDial / 2 - static_cast<int>(std::lround(std::cos(a) * r)) - 5;
        if (lv_obj_get_x(w.minute_dot) != x || lv_obj_get_y(w.minute_dot) != y) lv_obj_set_pos(w.minute_dot, x, y);
    }
    set_hidden(w.minute_dot, !c.valid);
    const RingView r = make_ring(f.s, f.power, f.now_ms);
    const bool lit[4] = {r.wifi, r.agent, r.power, r.time};
    const uint32_t colors[4] = {P().wifi, P().agent, P().power, P().clock};
    for (size_t i = 0; i < 4; ++i) set_bg(w.clock_dot[i], lit[i] ? colors[i] : P().edge);
    const std::string &q = f.s.time_quality;
    Str source = Str::TimeValid;
    if (q == "manual") source = Str::TimeSourceUsb;
    else if (q == "rtc") source = Str::TimeSourceRtc;
    else if (q == "sntp" || q == "ntp") source = Str::TimeSourceNetwork;
    set_text(w.meta, T(c.valid ? source : Str::TimeInvalidHint));
}

// ---- the switch page (docs/design v2 Main, MainIcons) ----
// The input the monitor is on, as the App read it back: a green edge and "current" (the user asked for green, 2026-11-07).
void mark_current(lv_obj_t *b, int index) {
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, hex(P().agent), 0);
    lv_obj_set_style_bg_color(b, hex(blend(P().agent, P().card, 14)), 0);
    w.switch_current = index;
}
void build_actions(lv_obj_t *body, const Frame &f) {
    w.switch_rows.clear(); w.switch_tiles.clear(); w.switch_icons.clear();
    heading(body, T(Str::SwitchTitle), P().accent);
    const auto sources = make_sources(look.lang, f.s, f.sources);
    if (sources.empty()) {   // a line of text, not a card: there is nothing to tap here
        caption(body, T(Str::NoActions), P().ink);
        caption(body, T(Str::NoActionsHint), P().muted);
        w.notice = caption(body, "", P().muted);
        return;
    }
    const size_t n = sources.size();
    if (look.appearance.style == SwitchStyle::List) {
        const int h = std::max(76, lh(look.body) + lh(look.caption) + 20);
        for (size_t i = 0; i < n; ++i) {
            const SourceView &v = sources[i];
            auto *b = button(body, on_action, data(static_cast<int>(i)), kContentW, h);
            lv_obj_set_style_radius(b, 18, 0);
            if (!v.enabled) { lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0); lv_obj_add_state(b, LV_STATE_DISABLED); }
            const bool now_on = v.enabled && !f.current_id.empty() && v.id == f.current_id;   // read back by the App (K3)
            if (now_on) mark_current(b, static_cast<int>(i));
            auto *icon = source_icon(b, v.icon, 34, now_on ? P().agent : v.enabled ? P().ink : P().muted, 2);
            lv_obj_set_pos(icon, 18, (h - 34) / 2);
            const int ty = (h - lh(look.body) - lh(look.caption)) / 2;
            auto *name = label_at(b, v.name, 68, ty, kContentW - 68 - 24 - (now_on ? 64 : 0), look.body, v.enabled ? P().ink : P().muted);
            single_line(name, lh(look.body));
            const std::string sub = v.enabled ? v.tag : v.sub;
            auto *tag = label_at(b, sub, 68, ty + lh(look.body), kContentW - 68 - 24, look.caption, P().muted);
            single_line(tag, lh(look.caption));
            if (now_on) {
                auto *tag = pill(b, T(Str::SourceCurrent), P().bg, P().agent, P().agent, look.caption);
                lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -14, 0);
            }
            w.switch_rows.push_back(b);
            w.switch_icons.push_back(icon);
            if (i == 0) w.first_action = b;
        }
    } else {
        // Up to three sources side by side, more in two columns that scroll.
        const int cols = n <= 3 ? static_cast<int>(n) : 2, gap = 10, tw = (kContentW - (cols - 1) * gap) / cols;
        const int th = n <= 3 ? 196 : 140, isz = n <= 3 ? 72 : 52;
        const int rows = static_cast<int>((n + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols));
        auto *grid = holder(body, kContentW, rows * th + (rows - 1) * gap);
        for (size_t i = 0; i < n; ++i) {
            const SourceView &v = sources[i];
            auto *b = button(grid, on_action, data(static_cast<int>(i)), tw, th);
            lv_obj_set_pos(b, static_cast<int>(i % static_cast<size_t>(cols)) * (tw + gap), static_cast<int>(i / static_cast<size_t>(cols)) * (th + gap));
            lv_obj_set_style_radius(b, 22, 0);
            if (!v.enabled) { lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0); lv_obj_add_state(b, LV_STATE_DISABLED); }
            // A tile is narrow: the name stops at 20 px, as the navigation does, so "MacBook M1" fits three in a row.
            const lv_font_t *nf = nav_font();
            const int ny = th - lh(nf) - lh(look.caption) - 12;
            const bool now_on = v.enabled && !f.current_id.empty() && v.id == f.current_id;
            if (now_on) {
                mark_current(b, static_cast<int>(i));
                auto *tag = pill(b, T(Str::SourceCurrent), P().bg, P().agent, P().agent, &btr_ui_16);
                lv_obj_set_pos(tag, 8, 8);
            }
            auto *icon = source_icon(b, v.icon, isz, now_on ? P().agent : v.enabled ? P().ink : P().muted, n <= 3 ? 3 : 2);
            lv_obj_set_pos(icon, (tw - isz) / 2, std::max(8, (ny - isz) / 2 + 2));   // in the middle of the room over the name
            auto *name = label_at(b, v.name, 8, ny, tw - 16, nf, v.enabled ? P().ink : P().muted);
            centered(name);
            single_line(name, lh(nf));
            auto *tag = label_at(b, v.enabled ? v.tag : v.sub, 8, ny + lh(nf), tw - 16, look.caption, P().muted);
            centered(tag);
            single_line(tag, lh(look.caption));
            w.switch_tiles.push_back(b);
            w.switch_icons.push_back(icon);
            if (i == 0) w.first_action = b;
        }
    }
    w.notice = caption(body, "", P().muted);
}
void refresh_actions(const Frame &f) {
    if (!w.notice) return;
    Tone tone;
    bool ring_line = false;
    std::string n = notice_for(f, tone, ring_line);
    if (ring_line) n.clear();   // the ring is not on this page: only a command result or an error is said here
    set_optional_text(w.notice, n);
    set_ink(w.notice, tone_color(tone));
}

// ---- Settings ----
void build_settings(lv_obj_t *body) {
    heading(body, T(Str::SettingsTitle), P().accent);
    struct Row { Str name, sub; Page page; uint32_t tone; };
    const Row rows[] = {{Str::SettingPages, Str::SettingPagesSub, Page::Pages, P().accent},
                        {Str::SettingSources, Str::SettingSourcesSub, Page::Sources, P().wifi},
                        {Str::SettingDisplay, Str::SettingDisplaySub, Page::Display, P().accent},
                        {Str::SettingAppearance, Str::SettingAppearanceSub, Page::Appearance, P().clock},
                        {Str::SettingConnection, Str::SettingConnectionSub, Page::Connect, P().wifi},
                        {Str::SettingPower, Str::SettingPowerSub, Page::Power, P().power},
                        {Str::SettingSound, Str::SettingSoundSub, Page::Sound, P().agent},
                        {Str::SettingClock, Str::SettingClockSub, Page::Clock, P().agent},
                        {Str::SettingTime, Str::SettingTimeSub, Page::Time, P().clock},
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

// ---- Pages (docs/design v2 Pages): shown or not, the order; the first shown one is the home page ----
void build_pages(lv_obj_t *body, const Frame &f) {
    heading(body, T(Str::SettingPages), P().accent);
    const PageOrder &o = look.appearance.pages;
    const View home = home_view(o);
    const int h = std::max(64, lh(look.body) + 20);
    for (size_t i = 0; i < o.size(); ++i) {
        auto *row = card(body, h);
        lv_obj_set_style_radius(row, 18, 0);
        auto *num = label_at(row, std::to_string(i + 1), 12, (h - lh(look.caption)) / 2 - 1, 20, look.caption, P().muted);
        single_line(num, lh(look.caption));
        auto *name = label_at(row, T(view_name(o[i].view)), 36, (h - lh(look.body)) / 2 - 1, 120, look.body, o[i].shown ? P().ink : P().muted);
        single_line(name, lh(look.body));
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(name, LV_SIZE_CONTENT);
        if (o[i].shown && o[i].view == home) {
            lv_obj_update_layout(name);
            auto *tag = pill(row, T(Str::PagesHome), P().bg, P().accent, P().accent, look.caption);
            lv_obj_update_layout(tag);   // its height is known only after a layout pass
            lv_obj_set_pos(tag, 36 + lv_obj_get_width(name) + 8, (h - lv_obj_get_height(tag)) / 2 - 1);
        }
        // the switch, then up and down: 56 px targets that do not overlap
        auto *sw = button(row, on_pages_toggle, data(static_cast<int>(i)), 60, 56);
        lv_obj_set_pos(sw, kContentW - 2 - 56 - 2 - 56 - 4 - 60, (h - 2 - 56) / 2);
        flat(sw);
        auto *track = holder(sw, 44, 26);
        lv_obj_center(track);
        lv_obj_set_style_radius(track, 13, 0);
        lv_obj_set_style_bg_color(track, hex(o[i].shown ? P().accent : P().edge), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        auto *knob = holder(track, 20, 20);
        lv_obj_set_pos(knob, o[i].shown ? 21 : 3, 3);
        lv_obj_set_style_radius(knob, 10, 0);
        lv_obj_set_style_bg_color(knob, hex(o[i].shown ? P().bg : P().muted), 0);
        lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
        lv_obj_t *moves[2] = {nullptr, nullptr};
        for (int k = 0; k < 2; ++k) {
            auto *b = button(row, k == 0 ? on_pages_up : on_pages_down, data(static_cast<int>(i)), 56, 56);
            lv_obj_set_pos(b, kContentW - 2 - 56 - (k == 0 ? 56 + 2 : 0), (h - 2 - 56) / 2);
            lv_obj_set_style_radius(b, 14, 0);
            auto *arrow = label(b, k == 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, 24, &lv_font_montserrat_14, P().muted);
            lv_obj_center(arrow);
            const bool end = (k == 0 && i == 0) || (k == 1 && i + 1 == o.size());
            if (end) { lv_obj_add_state(b, LV_STATE_DISABLED); lv_obj_set_style_opa(b, LV_OPA_30, 0); }
            moves[k] = b;
        }
        w.pages_ctrl[i] = {sw, moves[0], moves[1]};
    }
    caption(body, T(Str::PagesHint), P().muted);
    w.error = caption(body, "", P().power);
    set_optional_text(w.error, f.last_error);
}
// ---- Input sources (docs/design v2 Sources): the style of the switch page, and each source ----
std::vector<lv_obj_t *> g_style_buttons;
void build_sources(lv_obj_t *body, const Frame &f) {
    heading(body, T(Str::SettingSources), P().accent);
    g_style_buttons.clear();
    segmented(body, {T(Str::StyleList), T(Str::StyleIcons)}, static_cast<int>(look.appearance.style), on_style,
              {static_cast<int>(SwitchStyle::List), static_cast<int>(SwitchStyle::Icons)}, {look.body, look.body}, &g_style_buttons);
    const auto sources = make_sources(look.lang, f.s, f.sources);
    if (sources.empty()) {
        caption(body, T(Str::NoActions), P().ink);
        caption(body, T(Str::NoActionsHint), P().muted);
    }
    const int h = std::max(64, lh(look.body) + 20);
    for (size_t i = 0; i < sources.size(); ++i) {
        const SourceView &v = sources[i];
        auto *b = button(body, on_source, data(static_cast<int>(i)), kContentW, h);
        lv_obj_set_style_radius(b, 18, 0);
        auto *icon = source_icon(b, v.icon, 28, P().muted, 2);
        lv_obj_set_pos(icon, 16, (h - 28) / 2);
        auto *name = label_at(b, v.name, 56, (h - lh(look.body)) / 2 - 1, 210, look.body, P().ink);
        single_line(name, lh(look.body));
        auto *tag = label_at(b, v.tag, 270, (h - lh(look.caption)) / 2 - 1, 110, look.caption, P().muted);
        lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_RIGHT, 0);
        single_line(tag, lh(look.caption));
        label_at(b, LV_SYMBOL_RIGHT, kContentW - 36, (h - 14) / 2 - 2, 20, &lv_font_montserrat_14, P().muted);
    }
    caption(body, T(Str::SourcesHint), P().muted);
    w.error = caption(body, "", P().power);
    set_optional_text(w.error, f.last_error);
}
// ---- Edit a source (docs/design v2 SourceEdit): the name through the keyboard, the icon from eight ----
std::string g_edit_name;
void on_rename(lv_event_t *) { rename_start(g_edit_name); }
void build_source_edit(lv_obj_t *body, const Frame &f) {
    w.icon_btn = {};
    heading(body, T(Str::SourceEditTitle), P().accent);
    const auto sources = make_sources(look.lang, f.s, f.sources);
    const SourceView *v = nullptr;
    for (const auto &s : sources) if (s.id == f.edit_id) v = &s;
    if (!v) { caption(body, T(Str::SourceGone), P().muted); return; }
    g_edit_name = v->name;
    const int h = std::max(64, lh(look.body) + 20);
    auto *row = button(body, on_rename, data(0), kContentW, h);
    lv_obj_set_style_radius(row, 18, 0);
    auto *key = label_at(row, T(Str::SourceName), 16, (h - lh(look.caption)) / 2 - 1, 70, look.caption, P().muted);
    single_line(key, lh(look.caption));
    auto *name = label_at(row, v->name, 90, (h - lh(look.body)) / 2 - 1, kContentW - 90 - 50, look.body, P().ink);
    single_line(name, lh(look.body));
    label_at(row, LV_SYMBOL_KEYBOARD, kContentW - 44, (h - 24) / 2 - 2, 32, &lv_font_montserrat_24, P().muted);
    auto *tag = label(body, v->tag, kContentW, look.caption, P().muted);
    single_line(tag, lh(look.caption));
    section(body, T(Str::SourceIconTitle));
    const int gap = 8, bw = (kContentW - 3 * gap) / 4, bh = std::max(72, 34 + lh(look.caption) + 10);
    auto *grid = holder(body, kContentW, 2 * bh + gap);
    for (size_t i = 0; i < kIconChoices.size(); ++i) {
        const SourceIcon ic = kIconChoices[i];
        const bool on = ic == v->icon;
        auto *b = button(grid, on_icon, data(static_cast<int>(ic)), bw, bh);
        lv_obj_set_pos(b, static_cast<int>(i % 4) * (bw + gap), static_cast<int>(i / 4) * (bh + gap));
        lv_obj_set_style_radius(b, 16, 0);
        select(b, on, false);
        auto *icon = source_icon(b, ic, 30, on ? P().accent : P().muted, 2);
        lv_obj_set_pos(icon, (bw - 30) / 2 - 1, 6);
        auto *l = label_at(b, T(icon_name(ic)), 2, 40, bw - 6, look.caption, on ? P().accent : P().muted);
        centered(l);
        single_line(l, lh(look.caption));
        w.icon_btn[static_cast<size_t>(ic)] = b;
    }
    auto *restore = button(body, on_restore, data(0), kContentW, 56);
    lv_obj_set_style_radius(restore, 18, 0);
    auto *rl = label(restore, T(Str::SourceRestore), kContentW - 24, look.body, P().muted);
    centered(rl);
    single_line(rl, lh(look.body));
    lv_obj_center(rl);
    w.error = caption(body, "", P().power);
    set_optional_text(w.error, f.last_error);
}
void refresh_own_error(const Frame &f) { if (w.error) set_optional_text(w.error, f.last_error); }

// ---- Power ----
void build_power(lv_obj_t *body, PowerTab tab, const Frame &f) {
    heading(body, T(Str::PowerTitle), P().power);
    segmented(body, {T(Str::SegLevel), T(Str::SegReadings), T(Str::SegProtect)}, static_cast<int>(tab), on_tab,
              {static_cast<int>(PowerTab::Level), static_cast<int>(PowerTab::Readings), static_cast<int>(PowerTab::Protect)},
              {look.body, look.body, look.body});
    if (tab == PowerTab::Level) {
        const int h = 150;  // the design's 186 px would push the protection strip under the tabs
        auto *c = card(body, h);
        w.soc_gauge = gauge(c, 112, ring_px(look.appearance.ring).gauge, P().power);
        lv_obj_set_pos(w.soc_gauge, 20, 18);
        w.soc_value = label_at(c, "--", 20, 18 + 56 - lh(look.title) / 2, 112, look.title, P().ink);
        centered(w.soc_value);
        w.soc_title = label_at(c, "", 150, 14, 264, look.title, P().ink);
        single_line(w.soc_title, lh(look.title));
        w.soc_charge = pill(c, "", P().agent, P().card, P().edge, look.caption);
        lv_obj_set_pos(w.soc_charge, 150, 14 + lh(look.title) + 6);
        w.soc_note = label_at(c, "", 150, 14 + lh(look.title) + 6 + lh(look.caption) + 14, 264, look.caption, P().muted);
        auto *b = button(body, on_tab, data(static_cast<int>(PowerTab::Protect)), kContentW, 56);
        w.strip = label_at(b, T(Str::ProtectionOff), 18, (56 - lh(look.caption)) / 2, kContentW - 36, look.caption, P().ink);
        single_line(w.strip, lh(look.caption));
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
        char title[48];
        snprintf(title, sizeof(title), tr(look.lang, Str::BatteryLevel), static_cast<int>(v.level.percent + 0.5f));
        set_text(w.soc_title, v.level.valid ? std::string(title) : v.level_note);
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
void refresh_ble_rows(const Frame &f);
void build_connect(lv_obj_t *body, const Frame &f) {
    heading(body, T(Str::ConnectTitle), P().wifi);
    const auto rows = make_connect(look.lang, f.s, f.paired, f.wifi_name);
    const int row_h = std::max(44, lh(look.body) + 8);
    // The device ID is on the Device page. Wi-Fi and pairing are rows with an action on the right.
    auto *c = card(body, row_h * 3 + 56 * 2 + 12);
    int y = 6;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto &r = rows[i];
        if (i == 5) { w.connect_values.push_back(nullptr); continue; }
        if (i == 4) {
            auto *pair = button(c, [](lv_event_t *) { request_pairing(); }, nullptr, kContentW - 24, 56);
            lv_obj_set_pos(pair, 12, y);
            label_at(pair, r.key, 8, 18, 70, look.caption, P().muted);
            auto *v = label_at(pair, r.value, 86, 16, 150, &btr_ui_20, tone_color(r.tone));
            single_line(v, 26); w.connect_values.push_back(v);
            auto *go = label_at(pair, T(f.paired ? Str::PairAgain : Str::PairStart), 244, 18, 156, &btr_ui_16, P().accent);
            lv_obj_set_style_text_align(go, LV_TEXT_ALIGN_RIGHT, 0);
            y += 56; continue;
        }
        if (i == 0) {
            w.wifi_change_button = button(c, [](lv_event_t *) { wifi_open(); }, nullptr, kContentW - 24, 56);
            lv_obj_set_pos(w.wifi_change_button, 12, y);
            label_at(w.wifi_change_button, r.key, 8, 18, 70, look.caption, P().muted);
            auto *v = label_at(w.wifi_change_button, r.value, 86, 16, 250, &btr_ui_20, tone_color(r.tone));
            single_line(v, 26); w.connect_values.push_back(v);
            auto *change = label_at(w.wifi_change_button, T(Str::WifiChange), 340, 18, 60, &btr_ui_16, P().accent);
            lv_obj_set_style_text_align(change, LV_TEXT_ALIGN_RIGHT, 0);
            y += 56; continue;
        }
        label_at(c, r.key, 20, y + (row_h - lh(look.caption)) / 2, 150, look.caption, P().muted);
        auto *v = label_at(c, r.value, 170, y + (row_h - lh(look.body)) / 2, 242, look.body, r.tone == Tone::Off ? P().ink : tone_color(r.tone));
        single_line(v, lh(look.body));
        w.connect_values.push_back(v);
        y += row_h;
    }
    w.wifi_change_note = caption(body, "", P().power);
    // The computer over Bluetooth (docs/design v2 ConnectComputer): its state, "pair a computer", "forget computers".
    section(body, T(Str::BleTitle));
    auto *bc = card(body, row_h + 56 * 2 + 12);
    label_at(bc, T(Str::BleComputer), 20, 6 + (row_h - lh(look.caption)) / 2, 150, look.caption, P().muted);
    w.ble_state = label_at(bc, "", 120, 6 + (row_h - lh(&btr_ui_20)) / 2, 292, &btr_ui_20, P().ink);
    single_line(w.ble_state, lh(&btr_ui_20));
    auto *pair = button(bc, [](lv_event_t *) { request_ble_pairing(); }, nullptr, kContentW - 24, 56);
    lv_obj_set_pos(pair, 12, 6 + row_h);
    label_at(pair, T(Str::BlePair), 8, 16, 220, &btr_ui_20, P().ink);
    auto *note = label_at(pair, T(Str::BlePairNote), 230, 18, 170, &btr_ui_16, P().accent);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_RIGHT, 0);
    auto *forget = button(bc, [](lv_event_t *) { request_ble_forget(); }, nullptr, kContentW - 24, 56);
    lv_obj_set_pos(forget, 12, 6 + row_h + 56);
    label_at(forget, T(Str::BleForget), 8, 16, 200, &btr_ui_20, P().ink);
    w.ble_bonds = label_at(forget, "", 210, 18, 190, &btr_ui_16, P().muted);
    lv_obj_set_style_text_align(w.ble_bonds, LV_TEXT_ALIGN_RIGHT, 0);
    caption(body, T(Str::ConnectHint), P().muted);
    w.error = caption(body, "", P().power);
    refresh_ble_rows(f);
}
void refresh_ble_rows(const Frame &f) {
    if (!w.ble_state) return;
    char buf[64];
    if (!f.ble.running) set_text(w.ble_state, T(Str::BleOff));
    else if (f.ble.secure) { snprintf(buf, sizeof(buf), T(Str::BleConnected).c_str(), static_cast<int>(f.ble_sources)); set_text(w.ble_state, buf); }
    else set_text(w.ble_state, T(Str::BleNotConnected));
    set_ink(w.ble_state, f.ble.secure ? P().agent : P().ink);
    if (f.confirm_forget) set_text(w.ble_bonds, T(Str::BleForgetConfirm));
    else { snprintf(buf, sizeof(buf), T(Str::BleBonds).c_str(), f.ble.bonds); set_text(w.ble_bonds, buf); }
    set_ink(w.ble_bonds, f.confirm_forget ? P().power : P().muted);
}
void refresh_connect(const Frame &f) {
    refresh_ble_rows(f);
    const bool can_change = wifi_change::available();
    if (can_change) lv_obj_remove_state(w.wifi_change_button, LV_STATE_DISABLED);
    else lv_obj_add_state(w.wifi_change_button, LV_STATE_DISABLED);
    set_optional_text(w.wifi_change_note, can_change ? "" : wifi_change::hotspot_open() ? (look.lang == Lang::EnUS ? "Close the pairing hotspot to change Wi-Fi" : "配对热点打开时不能更换 Wi-Fi") : (look.lang == Lang::EnUS ? "Wi-Fi cannot be changed now. Try again later." : "现在不能更换 Wi-Fi，请稍后再试"));
    const auto rows = make_connect(look.lang, f.s, f.paired, f.wifi_name);
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
    auto *themes = holder(body, kContentW, 2 * 56 + 8);
    const ThemeId ids[4] = {ThemeId::Night, ThemeId::Mint, ThemeId::Ocean, ThemeId::Amber};
    const Str theme_names[4] = {Str::ThemeNight, Str::ThemeMint, Str::ThemeOcean, Str::ThemeAmber};
    for (int i = 0; i < 4; ++i) {
        const Palette &tp = theme_palette(ids[i]);
        const bool on = a.theme == ids[i];
        auto *b = button(themes, on_theme, data(static_cast<int>(ids[i])), 212, 56);
        lv_obj_set_pos(b, (i % 2) * 220, (i / 2) * 64);
        lv_obj_set_style_radius(b, 18, 0);
        select(b, on, false);
        auto *swatch = holder(b, 30, 30);   // the theme's background with its accent: what the screen will look like
        lv_obj_set_pos(swatch, 14, 12);
        lv_obj_set_style_radius(swatch, 9, 0);
        lv_obj_set_style_bg_color(swatch, hex(tp.card), 0);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(swatch, hex(tp.accent), 0);
        lv_obj_set_style_border_width(swatch, 3, 0);
        auto *l = label_at(b, T(theme_names[i]), 54, (54 - lh(look.caption)) / 2, 150, look.caption, on ? P().accent : P().ink);
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
    section(body, T(Str::KeyBatteryPercent));
    segmented(body, {T(Str::PercentShow), T(Str::PercentHide)}, a.show_percent ? 0 : 1, on_percent, {0, 1}, {look.body, look.body});
    section(body, T(Str::KeyRingThickness));
    segmented(body, {T(Str::RingThin), T(Str::SizeStandard), T(Str::RingThick)}, static_cast<int>(a.ring), on_ring,
              {static_cast<int>(RingTier::Thin), static_cast<int>(RingTier::Standard), static_cast<int>(RingTier::Thick)}, {look.body, look.body, look.body});
    // docs/design v2 AppearanceTime: the seconds are a setting of the device (config), the hands a part of the look
    section(body, T(Str::KeyShowSeconds));
    segmented(body, {T(Str::PercentShow), T(Str::PercentHide)}, f.c.layout.show_seconds ? 0 : 1, [](lv_event_t *e) { request_show_seconds(data_of(e) == 0); }, {0, 1},
              {look.body, look.body});
    section(body, T(Str::KeyClockHands));
    segmented(body, {T(Str::PercentShow), T(Str::PercentHide)}, a.clock_hands ? 0 : 1, [](lv_event_t *e) { request_clock_hands(data_of(e) == 0); }, {0, 1},
              {look.body, look.body});
    w.error = caption(body, "", P().power);
    set_optional_text(w.error, f.last_error);
}
void refresh_appearance(const Frame &f) { set_optional_text(w.error, f.last_error); }

// ---- Sound (docs/design Dev-Sound) ----
std::vector<lv_obj_t *> g_sound_switch, g_sound_volume;   // the two halves of the switch and the three levels, for the tests
void build_sound(lv_obj_t *body, const Frame &) {
    const bool speaker = audio::available(), on = audio::enabled();
    heading(body, T(Str::SoundTitle), P().accent);
    section(body, T(Str::SoundLabel));
    g_sound_switch.clear();
    auto *track = segmented(body, {T(Str::SoundOff), T(Str::SoundOn)}, on ? 1 : 0,
                            [](lv_event_t *e) { request_sound_set(data_of(e) == 1); }, {0, 1}, {look.body, look.body}, &g_sound_switch);
    section(body, T(Str::SoundVolume));
    g_sound_volume.clear();
    auto *levels = segmented(body, {T(Str::SoundLow), T(Str::SoundMid), T(Str::SoundHigh)}, audio::volume(),
                             [](lv_event_t *e) { request_sound_volume(data_of(e)); }, {0, 1, 2}, {look.body, look.body, look.body}, &g_sound_volume);
    w.error = caption(body, "", P().power);   // under the two choices it is about, so a failed save is seen without scrolling
    set_optional_text(w.error, "");
    caption(body, T(speaker ? Str::SoundNote : Str::SoundNoSpeaker), speaker ? P().muted : P().power);
    auto *listen = button(body, [](lv_event_t *) { request_sound_test(); }, data(0), kContentW, 56);
    lv_obj_set_style_radius(listen, 28, 0);
    select(listen, true, false);
    auto *l = label(listen, T(Str::SoundListen), kContentW - 8, look.body, P().accent);
    centered(l);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    if (!speaker) {   // nothing to turn on and nothing to hear: the controls stay, dimmed
        lv_obj_set_style_opa(track, LV_OPA_40, 0);
        lv_obj_set_style_opa(levels, LV_OPA_40, 0);
        lv_obj_set_style_opa(listen, LV_OPA_40, 0);
    }
}
void refresh_sound(const Frame &f) { set_optional_text(w.error, f.last_error); }

// ---- Date and time (docs/design Dev-Time) ----
// The numbers belong to the visit: a rebuild keeps them, another page starts them over from the time now.
TimeDraft g_time_draft;
int g_time_field = 4;
bool g_time_seeded = false;
std::vector<lv_obj_t *> g_time_fields;
void build_time(lv_obj_t *body, const Frame &f) {
    if (!g_time_seeded) { g_time_draft = time_draft(f.s.time_valid, f.wall, f.c.tz_minutes); g_time_field = 4; g_time_seeded = true; }
    heading(body, T(Str::TimeTitle), P().clock);
    w.meta = caption(body, "", P().muted);
    section(body, T(Str::TimeZoneLabel));
    auto *zone = holder(body, kContentW, 56);
    const auto stepper = [](lv_obj_t *parent, const char *symbol, int x, int delta) {
        auto *b = button(parent, [](lv_event_t *e) { g_time_seeded = false; request_time_zone(data_of(e) ? 1 : -1); }, data(delta > 0 ? 1 : 0), 56, 56);
        lv_obj_set_pos(b, x, 0);
        lv_obj_set_style_radius(b, 14, 0);
        auto *l = label(b, symbol, 40, &lv_font_montserrat_24, P().ink);
        centered(l);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    };
    stepper(zone, LV_SYMBOL_MINUS, 0, -1);
    stepper(zone, LV_SYMBOL_PLUS, kContentW - 56, 1);
    auto *name = label_at(zone, tz_label(f.c.tz_minutes), 64, (56 - lh(look.body)) / 2, kContentW - 128, look.body, P().ink);
    centered(name);
    section(body, T(Str::TimeSetLabel));
    char text[5][8];
    snprintf(text[0], sizeof(text[0]), "%04d", g_time_draft.year);
    snprintf(text[1], sizeof(text[1]), "%02d", g_time_draft.month);
    snprintf(text[2], sizeof(text[2]), "%02d", g_time_draft.day);
    snprintf(text[3], sizeof(text[3]), "%02d", g_time_draft.hour);
    snprintf(text[4], sizeof(text[4]), "%02d", g_time_draft.minute);
    g_time_fields.clear();
    segmented(body, {text[0], text[1], text[2], text[3], text[4]}, g_time_field, [](lv_event_t *e) { g_time_field = data_of(e); request_time_redraw(); }, {0, 1, 2, 3, 4},
              {look.body, look.body, look.body, look.body, look.body}, &g_time_fields);
    auto *steps = holder(body, kContentW, 56);
    for (int i = 0; i < 2; ++i) {
        auto *b = button(steps, [](lv_event_t *e) { time_draft_step(g_time_draft, g_time_field, data_of(e) ? 1 : -1); request_time_redraw(); }, data(i), (kContentW - 12) / 2, 56);
        lv_obj_set_pos(b, i * (kContentW + 12) / 2, 0);
        lv_obj_set_style_radius(b, 14, 0);
        auto *l = label(b, i ? "+1" : "-1", 80, look.body, P().ink);
        centered(l);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    }
    auto *save = button(body, [](lv_event_t *) { if (request_time_save(time_draft_utc(g_time_draft, config().tz_minutes))) g_time_seeded = false; }, data(0), kContentW, 56);
    lv_obj_set_style_radius(save, 28, 0);
    select(save, true, false);
    auto *sl = label(save, T(Str::TimeSave), kContentW - 8, look.body, P().accent);
    centered(sl);
    lv_obj_align(sl, LV_ALIGN_CENTER, 0, 0);
    w.error = caption(body, "", P().power);
    set_optional_text(w.error, "");
    caption(body, T(Str::TimeNote), P().muted);
}
void refresh_time(const Frame &f) {
    if (f.s.time_valid) {
        struct tm t{};
        localtime_r(&f.wall, &t);
        char now[48];
        snprintf(now, sizeof(now), "%s %04d-%02d-%02d %02d:%02d", T(Str::TimeNow).c_str(), t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
        set_text(w.meta, now);
    } else set_text(w.meta, T(Str::TimeUnset));
    set_optional_text(w.error, f.last_error);
}

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
    caption(body, T(Str::DisplayCycleHint), P().muted);
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
    // Words from the code only when they say something: not "ok", and not an update that is not going on.
    snprintf(buf, sizeof(buf), "%s\n%s %u KiB", f.s.device_id.c_str(), T(Str::FreeHeap).c_str(), static_cast<unsigned>(esp_get_free_heap_size() / 1024));
    std::string detail = buf;
    if (!f.power.reason.empty() && f.power.reason != "ok") detail += "\n" + f.power.reason;
    if (!f.s.update_state.empty() && f.s.update_state != "idle") {
        snprintf(buf, sizeof(buf), "\n%s %s %d%%", T(Str::UpdateLabel).c_str(), f.s.update_state.c_str(), f.s.update_progress);
        detail += buf;
    }
    if (!f.s.update_error.empty()) detail += "\n" + f.s.update_error;
    set_text(w.detail, detail);
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
    // The header is icons only (docs/design: Dev-Header). The corners of the panel are rounded (kCornerR): the icons sit
    // 14 px from the top, and where the curve still reaches them they move in by what it needs.
    constexpr int kIconTop = 14, kHeaderH = 62;
    static const lv_point_precise_t kSlashIcon[2] = {{3, 3}, {23, 21}}, kSlashBattery[2] = {{6, 21}, {44, 3}};
    auto slash = [&](lv_obj_t *parent, int x, int y, const lv_point_precise_t *points, std::array<lv_obj_t *, 2> &out) {
        for (int i = 0; i < 2; ++i) {
            auto *line = lv_line_create(parent);
            lv_line_set_points(line, points, 2);
            lv_obj_set_pos(line, x, y);
            lv_obj_set_style_line_width(line, i == 0 ? 6 : 2, 0);
            lv_obj_set_style_line_color(line, hex(i == 0 ? p.bg : p.muted), 0);
            lv_obj_set_style_line_rounded(line, true, 0);
            lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
            out[static_cast<size_t>(i)] = line;
        }
    };
    const int left_abs = std::max(kMargin, corner_inset(kIconTop) + 3);               // the first icon
    const int right_abs = kScreen - std::max(kMargin, corner_inset(kIconTop) + 1);    // the tip of the battery
    sh.left = button(screen, on_page, data(static_cast<int>(Page::Connect)), 212, kHeaderH);
    lv_obj_set_pos(sh.left, kMargin, 0);
    flat(sh.left);
    const int lx = left_abs - kMargin;
    sh.wifi = label_at(sh.left, LV_SYMBOL_WIFI, lx, kIconTop - 2, 30, &lv_font_montserrat_24, p.muted);
    slash(sh.left, lx, kIconTop, kSlashIcon, sh.wifi_cut);
    const int px = lx + 40;                                                           // the computer
    sh.pc = holder(sh.left, 26, 24);
    lv_obj_set_pos(sh.pc, px, kIconTop);
    sh.pc_screen = panel(sh.pc, 22, 15, p.bg, p.muted, 4);
    lv_obj_set_pos(sh.pc_screen, 2, 1);
    lv_obj_set_style_bg_opa(sh.pc_screen, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sh.pc_screen, 2, 0);
    sh.pc_neck = panel(sh.pc, 2, 4, p.muted, p.muted, 0);
    lv_obj_set_pos(sh.pc_neck, 12, 16);
    sh.pc_base = panel(sh.pc, 11, 2, p.muted, p.muted, 1);
    lv_obj_set_pos(sh.pc_base, 7, 20);
    slash(sh.pc, 0, 0, kSlashIcon, sh.pc_cut);

    sh.right = button(screen, on_page, data(static_cast<int>(Page::Power)), 212, kHeaderH);
    lv_obj_set_pos(sh.right, 244, 0);
    flat(sh.right);
    const int bx = right_abs - 51 - 244;                                              // the battery box, 51 x 24
    sh.usb = label_at(sh.right, LV_SYMBOL_USB, bx - 8 - 22, kIconTop - 2, 28, &lv_font_montserrat_24, p.ink);   // next to the battery: the bolt is inside it
    sh.box = holder(sh.right, 51, 24);
    lv_obj_set_pos(sh.box, bx, kIconTop);
    sh.body = panel(sh.box, 46, 22, p.bg, p.muted, 7);
    lv_obj_set_pos(sh.body, 0, 1);
    lv_obj_set_style_bg_opa(sh.body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sh.body, 2, 0);
    sh.fill = holder(sh.box, 40, 16);                                                 // the level, in the colour of its tone at 40 %
    lv_obj_set_pos(sh.fill, 3, 4);
    lv_obj_set_style_radius(sh.fill, 4, 0);
    lv_obj_set_style_bg_color(sh.fill, hex(p.agent), 0);
    lv_obj_set_style_bg_opa(sh.fill, static_cast<lv_opa_t>(kBatteryFillPercent * 255 / 100), 0);
    lv_obj_add_flag(sh.fill, LV_OBJ_FLAG_HIDDEN);
    sh.tip = panel(sh.box, 3, 9, p.muted, p.muted, 1);
    lv_obj_set_pos(sh.tip, 47, 7);
    // Centre the ink, not the line: the line of the font has room under the digits, so a centred box puts them too high.
    int digits_y = 1 + (22 - lh(&btr_ui_16)) / 2;
    lv_font_glyph_dsc_t zero;
    if (lv_font_get_glyph_dsc(&btr_ui_16, &zero, '0', 0))
        digits_y = 12 - (btr_ui_16.line_height - btr_ui_16.base_line - zero.box_h - zero.ofs_y) - (zero.box_h + 1) / 2;   // 12: the middle of the body
    sh.digits = label_at(sh.box, "", 2, digits_y, 42, &btr_ui_16, p.ink);   // the digits are drawn over the fill
    lv_obj_set_style_text_align(sh.digits, LV_TEXT_ALIGN_CENTER, 0);
    single_line(sh.digits, lh(&btr_ui_16));
    {   // The bolt is in front of the digits, in their ink; the two are one group in the middle of the body (refresh_shell).
        const lv_font_t *bolt_font = &lv_font_montserrat_14;
        int bolt_y = 1 + (22 - lh(bolt_font)) / 2, bolt_adv = 10;
        lv_font_glyph_dsc_t g;
        sh.bolt_ink_w = 9; sh.bolt_ofs_x = 0;
        if (lv_font_get_glyph_dsc(bolt_font, &g, 0xF0E7, 0)) {   // LV_SYMBOL_CHARGE
            sh.bolt_ink_w = g.box_w; sh.bolt_ofs_x = g.ofs_x; bolt_adv = g.adv_w;
            bolt_y = 12 - (bolt_font->line_height - bolt_font->base_line - g.box_h - g.ofs_y) - (g.box_h + 1) / 2;
        }
        sh.bolt = label_at(sh.box, LV_SYMBOL_CHARGE, 0, bolt_y, bolt_adv + 2, bolt_font, p.ink);
        single_line(sh.bolt, lh(bolt_font));
        lv_obj_add_flag(sh.bolt, LV_OBJ_FLAG_HIDDEN);
    }
    slash(sh.box, 0, 0, kSlashBattery, sh.bat_cut);

    sh.nav = panel(screen, kContentW, 64, p.card, p.edge, 32);
    lv_obj_set_pos(sh.nav, kMargin, kNavTop);
    const auto views = nav_views(look.appearance.pages);
    sh.nav_count = static_cast<int>(views.size()) + 1;
    const int n = sh.nav_count, each = (kContentW - 8 - 3 * (n - 1)) / n;
    // Five buttons are narrow: the English names drop to the small font there.
    const lv_font_t *font = n >= 5 && look.lang == Lang::EnUS ? &btr_ui_16 : nav_font();
    for (int i = 0; i < n; ++i) {
        const size_t k = static_cast<size_t>(i);
        const bool settings = i == n - 1;
        sh.nav_page[k] = settings ? Page::Settings : page_of(views[k]);
        sh.nav_btn[k] = button(sh.nav, on_page, data(static_cast<int>(sh.nav_page[k])), each, 56);
        lv_obj_set_pos(sh.nav_btn[k], 3 + i * (each + 3), 3);
        lv_obj_set_style_radius(sh.nav_btn[k], 28, 0);
        lv_obj_set_style_border_width(sh.nav_btn[k], 1, 0);
        auto *l = label(sh.nav_btn[k], T(settings ? Str::NavSettings : view_name(views[k])), each - 4, font, p.muted);
        centered(l);
        single_line(l, lh(font));
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        sh.nav_label[k] = l;
    }
    sh.hint = label_at(screen, LV_SYMBOL_DOWN, 230, 386, 20, &lv_font_montserrat_14, p.accent);
    lv_obj_add_flag(sh.hint, LV_OBJ_FLAG_HIDDEN);
}
void refresh_shell(const Frame &f, Page page, bool pairing_active) {
    const Palette &p = P();
    const HeaderView h = make_header(f.s, f.power, f.now_ms, look.appearance.show_percent);
    const auto cut_shown = [](const std::array<lv_obj_t *, 2> &lines, bool on) { for (auto *l : lines) set_hidden(l, !on); };
    const auto set_text_opa = [](lv_obj_t *o, lv_opa_t opa) { if (o && lv_obj_get_style_text_opa(o, LV_PART_MAIN) != opa) lv_obj_set_style_text_opa(o, opa, 0); };
    set_ink(sh.wifi, h.wifi == LinkIcon::Off ? p.muted : p.wifi);
    set_text_opa(sh.wifi, h.wifi == LinkIcon::Busy && !blink_on(f.now_ms) ? LV_OPA_20 : LV_OPA_COVER);   // connecting blinks, connected is steady
    cut_shown(sh.wifi_cut, h.wifi == LinkIcon::Off);
    const uint32_t ac = h.agent == LinkIcon::Ok ? p.agent : p.muted;
    set_border(sh.pc_screen, ac);
    set_bg(sh.pc_neck, ac); set_border(sh.pc_neck, ac);
    set_bg(sh.pc_base, ac); set_border(sh.pc_base, ac);
    cut_shown(sh.pc_cut, h.agent != LinkIcon::Ok);
    set_hidden(sh.usb, !h.usb);
    set_hidden(sh.bolt, !h.battery.charging);
    const uint32_t bc = tone_color(h.battery.tone);
    set_border(sh.body, bc);
    set_bg(sh.tip, bc); set_border(sh.tip, bc);
    set_bg(sh.fill, bc);
    if (h.battery.percent >= 0) {
        const int width = std::max(2, 40 * h.battery.percent / 100);
        if (lv_obj_get_width(sh.fill) != width) lv_obj_set_width(sh.fill, width);
        set_hidden(sh.fill, false);
    } else set_hidden(sh.fill, true);
    set_optional_text(sh.digits, h.battery.digits);
    {   // Bolt and digits are one group, centred in the body (46 wide: the middle is 23): the bolt first, a gap, the digits.
        int digits_w = 0;
        if (!h.battery.digits.empty()) {
            lv_point_t size{};
            lv_text_get_size(&size, h.battery.digits.c_str(), &btr_ui_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            digits_w = size.x;
        }
        const bool bolt = h.battery.charging;
        const int gap = bolt && digits_w ? 2 : 0, total = (bolt ? sh.bolt_ink_w : 0) + gap + digits_w, start = 23 - total / 2;
        int centre = 23;   // the middle of the digits
        if (bolt) {
            const int bx = start - sh.bolt_ofs_x;
            if (lv_obj_get_x(sh.bolt) != bx) lv_obj_set_x(sh.bolt, bx);
            centre = start + sh.bolt_ink_w + gap + digits_w / 2;
        }
        if (lv_obj_get_x(sh.digits) != centre - 21) lv_obj_set_x(sh.digits, centre - 21);
    }
    cut_shown(sh.bat_cut, h.battery.kind == BatteryView::Kind::None);
    // The lit button: the page itself when it is in the navigation; any other page belongs to Settings, except the clock
    // that idling shows, which lights nothing when it is not in the navigation.
    int active = -1;
    for (int i = 0; i < sh.nav_count; ++i) if (sh.nav_page[static_cast<size_t>(i)] == page) active = i;
    if (active < 0 && page != Page::Clock && page != Page::Home && page != Page::Actions) active = sh.nav_count - 1;
    for (int i = 0; i < sh.nav_count; ++i) {
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
    if (page != Page::Time) g_time_seeded = false;
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
    case Page::Settings: build_settings(body); break;
    case Page::Pages: build_pages(body, f); break;
    case Page::Sources: build_sources(body, f); break;
    case Page::SourceEdit: build_source_edit(body, f); break;
    case Page::Power: build_power(body, tab, f); break;
    case Page::Connect: build_connect(body, f); break;
    case Page::Appearance: build_appearance(body, f); break;
    case Page::Sound: build_sound(body, f); break;
    case Page::Time: build_time(body, f); break;
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
    case Page::Settings: break;
    case Page::Pages: case Page::Sources: case Page::SourceEdit: refresh_own_error(f); break;
    case Page::Power: refresh_power(tab, f); break;
    case Page::Connect: refresh_connect(f); break;
    case Page::Appearance: refresh_appearance(f); break;
    case Page::Sound: refresh_sound(f); break;
    case Page::Time: refresh_time(f); break;
    case Page::Display: refresh_display(f); break;
    case Page::Clock: refresh_clock(f); break;
    case Page::Device: refresh_device(f); break;
    }
    const bool more = page != Page::Home && page != Page::Clock && g_body && lv_obj_get_scroll_bottom(g_body) > 3;
    set_hidden(sh.hint, !more);
}

// ---------------------------------------------------------------- state pages
// ---- pairing a computer over Bluetooth (docs/design v2 PairWaiting, PairConfirm; K1) ----
struct BlePage { lv_obj_t *root{}, *countdown{}, *digits{}, *left{}; int mode = 0; } g_ble;
lv_obj_t *build_ble_pairing(lv_obj_t *layer, const ble::Status &b) {
    g_ble = BlePage{};
    g_ble.mode = b.confirm_pending ? 2 : 1;
    auto *root = panel(layer, kScreen, kScreen, P().bg, P().bg, 0);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);   // nothing under it takes a touch
    g_ble.root = root;
    if (g_ble.mode == 1) {
        auto *ring = holder(root, 150, 150);
        lv_obj_set_pos(ring, (kScreen - 150) / 2, 70);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(ring, hex(P().accent), 0);
        lv_obj_set_style_border_width(ring, 2, 0);
        auto *bt = label(ring, LV_SYMBOL_BLUETOOTH, 60, &lv_font_montserrat_48, P().accent);
        lv_obj_center(bt);
        auto *t = label_at(root, T(Str::BleWaitTitle), 40, 236, kScreen - 80, look.title, P().ink);
        centered(t);
        auto *h = label_at(root, T(Str::BleWaitHint), 50, 236 + lh(look.title) + 6, kScreen - 100, look.caption, P().muted);
        centered(h);
        g_ble.countdown = label_at(root, "", 40, 320, kScreen - 80, &lv_font_montserrat_24, P().accent);
        centered(g_ble.countdown);
        auto *cancel = button(root, [](lv_event_t *) { request_ble_cancel(); }, nullptr, 180, 56);
        lv_obj_set_pos(cancel, (kScreen - 180) / 2, 368);
        lv_obj_set_style_radius(cancel, 28, 0);
        auto *cl = label(cancel, T(Str::BleCancel), 170, nav_font(), P().muted);
        centered(cl);
        lv_obj_center(cl);
    } else {
        auto *t = label_at(root, T(Str::BleConfirmTitle), 60, 56, kScreen - 120, look.title, P().ink);
        centered(t);
        auto *h = label_at(root, T(Str::BleConfirmHint), 50, 56 + lh(look.title) + 6, kScreen - 100, look.caption, P().muted);
        centered(h);
        g_ble.digits = label_at(root, "", 20, 150, kScreen - 40, &font_clock_72, P().accent);
        centered(g_ble.digits);
        g_ble.left = label_at(root, "", 40, 236, kScreen - 80, look.caption, P().muted);
        centered(g_ble.left);
        auto *no = button(root, [](lv_event_t *) { request_ble_answer(false); }, nullptr, 180, 60);
        lv_obj_set_pos(no, 50, 280);
        lv_obj_set_style_radius(no, 18, 0);
        auto *nl = label(no, T(Str::BleNotSame), 170, nav_font(), P().muted);
        centered(nl);
        lv_obj_center(nl);
        auto *yes = button(root, [](lv_event_t *) { request_ble_answer(true); }, nullptr, 180, 60);
        lv_obj_set_pos(yes, 250, 280);
        lv_obj_set_style_radius(yes, 18, 0);
        lv_obj_set_style_bg_color(yes, hex(P().accent), 0);
        auto *yl = label(yes, T(Str::BleSame), 170, nav_font(), P().bg);
        centered(yl);
        lv_obj_center(yl);
        auto *n = label_at(root, T(Str::BleConfirmNote), 60, 352, kScreen - 120, look.caption, P().muted);
        centered(n);
    }
    refresh_ble_pairing(b);
    return root;
}
void refresh_ble_pairing(const ble::Status &b) {
    char buf[48];
    if (g_ble.countdown) {
        const int s = static_cast<int>((b.pairing_left_ms + 999) / 1000);
        snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
        set_text(g_ble.countdown, buf);
    }
    if (g_ble.digits) {
        snprintf(buf, sizeof(buf), "%03u %03u", static_cast<unsigned>(b.digits / 1000 % 1000), static_cast<unsigned>(b.digits % 1000));
        set_text(g_ble.digits, buf);
        snprintf(buf, sizeof(buf), T(Str::BleConfirmLeft).c_str(), static_cast<int>((b.confirm_left_ms + 999) / 1000));
        set_text(g_ble.left, buf);
    }
}
int ble_overlay_mode() { return g_ble.root && lv_obj_is_valid(g_ble.root) ? g_ble.mode : 0; }

lv_obj_t *build_pairing(lv_obj_t *screen, const PairingInfo &info, int64_t, void (*on_cancel)()) {
    const Palette &p = P();
    auto *root = panel(screen, kScreen, kScreen, p.bg, p.bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_pos(root, 0, 0);
    const int title_x = std::max(kMargin, corner_inset(20)), sub_x = std::max(kMargin, corner_inset(20 + lh(look.title)));   // rounded corners
    label_at(root, T(Str::PairingTitle), title_x, 20, kContentW + kMargin - title_x, look.title, p.accent);
    label_at(root, T(Str::PairingSub), sub_x, 20 + lh(look.title), kContentW + kMargin - sub_x, look.caption, p.muted);
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

// The ring explained (docs/design Dev-RingInfo): one row per segment, in the colour of the segment.
static lv_obj_t *g_ring_sheet = nullptr, *g_ring_dots[4] = {nullptr, nullptr, nullptr, nullptr};
bool ring_info_open() { return g_ring_sheet && lv_obj_is_valid(g_ring_sheet) && !lv_obj_has_flag(g_ring_sheet, LV_OBJ_FLAG_HIDDEN); }
static void close_ring_sheet() {
    static void (*closed)() = nullptr;
    (void)closed;
    if (!ring_info_open()) return;
    auto *d = g_ring_sheet;
    g_ring_sheet = nullptr;
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(d);
}
lv_obj_t *build_ring_info(lv_obj_t *layer, const RingInfoView &v, void (*on_close)()) {
    const Palette &p = P();
    static void (*closed)() = nullptr;
    closed = on_close;
    if (ring_info_open()) close_ring_sheet();
    auto *dimmer = lv_obj_create(layer);
    g_ring_sheet = dimmer;
    lv_obj_remove_style_all(dimmer);
    lv_obj_set_size(dimmer, kScreen, kScreen);
    lv_obj_set_pos(dimmer, 0, 0);
    lv_obj_set_style_bg_color(dimmer, hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dimmer, LV_OPA_60, 0);
    lv_obj_add_flag(dimmer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dimmer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(dimmer, [](lv_event_t *e) {
        if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;   // a tap on the sheet itself stays on the sheet
        close_ring_sheet();
        if (closed) closed();
    }, LV_EVENT_CLICKED, nullptr);
    auto *box = panel(dimmer, 408, LV_SIZE_CONTENT, p.card, p.edge, 28);
    lv_obj_set_pos(box, 36, 40);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 20, 0);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_set_style_max_height(box, kScreen - 48, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);   // a tap on the sheet stays on the sheet; only the space around it closes it
    label(box, T(Str::RingTitle), 360, look.body, p.ink);
    label(box, T(Str::RingLegend), 360, look.caption, p.muted);
    const uint32_t colors[4] = {p.wifi, p.agent, p.power, p.clock};
    for (int i = 0; i < 4; ++i) {
        const RingRow &r = v.row[i];
        if (i > 0) {
            auto *line = holder(box, 360, 1);
            lv_obj_set_style_bg_color(line, hex(p.edge), 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        }
        auto *row = holder(box, 360, lh(look.caption) * 2 + 8);
        auto *dot = holder(row, 18, 18);   // filled in the colour of its segment when it is lit, an empty ring when it is dark
        lv_obj_set_pos(dot, 0, 4);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(dot, r.ok ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        g_ring_dots[i] = dot;
        lv_obj_set_style_border_width(dot, r.ok ? 0 : 2, 0);
        lv_obj_set_style_border_color(dot, hex(p.muted), 0);
        label_at(row, r.name, 34, 0, 150, look.caption, p.ink);
        auto *state = label_at(row, r.state, 34, 0, 326, look.caption, r.ok ? colors[i] : p.muted);
        lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_RIGHT, 0);
        auto *note = label_at(row, r.note, 34, lh(look.caption) + 4, 326, look.caption, p.muted);
        single_line(note, lh(look.caption));
    }
    auto *b = button(box, [](lv_event_t *) {
        close_ring_sheet();
        if (closed) closed();
    }, data(0), 360, 56);
    lv_obj_set_style_radius(b, 28, 0);
    auto *l = label(b, T(Str::Close), 356, look.body, p.ink);
    centered(l);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    return dimmer;
}

// ---------------------------------------------------------------- test hooks
int header_fill_width() { return sh.fill && !lv_obj_has_flag(sh.fill, LV_OBJ_FLAG_HIDDEN) ? lv_obj_get_width(sh.fill) : 0; }
static bool visible(lv_obj_t *o) { return o && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); }
bool header_battery_body(int *x1, int *y1, int *x2, int *y2) {
    if (!sh.body) return false;
    lv_area_t a;
    lv_obj_get_coords(sh.body, &a);
    *x1 = a.x1; *y1 = a.y1; *x2 = a.x2; *y2 = a.y2;
    return true;
}
std::string header_battery_text() { return visible(sh.digits) ? lv_label_get_text(sh.digits) : ""; }
bool header_bolt_shown() { return visible(sh.bolt); }
static bool area_of(lv_obj_t *o, int *x1, int *y1, int *x2, int *y2) {
    if (!visible(o)) return false;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    *x1 = a.x1; *y1 = a.y1; *x2 = a.x2; *y2 = a.y2;
    return true;
}
bool header_bolt_area(int *x1, int *y1, int *x2, int *y2) { return area_of(sh.bolt, x1, y1, x2, y2); }
bool header_usb_area(int *x1, int *y1, int *x2, int *y2) { return area_of(sh.usb, x1, y1, x2, y2); }
bool home_ring_area(int *x1, int *y1, int *x2, int *y2) { return area_of(ring.track, x1, y1, x2, y2); }
bool home_notice_area(int *x1, int *y1, int *x2, int *y2) { return area_of(w.notice, x1, y1, x2, y2); }
static bool dimmed(const std::vector<lv_obj_t *> &parts) {
    return !parts.empty() && lv_obj_get_style_opa(lv_obj_get_parent(parts[0]), LV_PART_MAIN) < LV_OPA_COVER;
}
bool sound_controls_dimmed() { return current_page() == Page::Sound && dimmed(g_sound_switch) && dimmed(g_sound_volume); }
static int selected_of(const std::vector<lv_obj_t *> &parts, Page page = Page::Sound) {
    if (current_page() != page) return -1;
    for (size_t i = 0; i < parts.size(); ++i)
        if (lv_obj_get_style_border_width(parts[i], LV_PART_MAIN) == 2) return static_cast<int>(i);   // the selected part has the border
    return -1;
}
int sound_selected() { return selected_of(g_sound_switch); }
int sound_volume_selected() { return selected_of(g_sound_volume); }
int time_field_selected() { return selected_of(g_time_fields, Page::Time); }
std::string time_draft_text() {
    char text[24];
    snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d", g_time_draft.year, g_time_draft.month, g_time_draft.day, g_time_draft.hour, g_time_draft.minute);
    return text;
}
bool home_notice_dot_area(int *x1, int *y1, int *x2, int *y2) { return area_of(w.notice_dot, x1, y1, x2, y2); }
bool ring_info_dot_filled(int i) { return ring_info_open() && i >= 0 && i < 4 && g_ring_dots[i] && lv_obj_get_style_bg_opa(g_ring_dots[i], LV_PART_MAIN) == LV_OPA_COVER; }
bool notice_dot_shown() { return visible(w.notice_dot); }
bool header_usb_shown() { return visible(sh.usb); }
bool header_slashed(int which) { return visible(which == 0 ? sh.wifi_cut[1] : which == 1 ? sh.pc_cut[1] : which == 2 ? sh.bat_cut[1] : nullptr); }
int header_icon_opa(int which) { return which == 0 && sh.wifi ? lv_obj_get_style_text_opa(sh.wifi, LV_PART_MAIN) : 255; }
int clock_hand_deg(int which) {
    if (current_page() != Page::Clock || which < 0 || which > 2 || !w.hands_group || lv_obj_has_flag(w.hands_group, LV_OBJ_FLAG_HIDDEN)) return -1;
    return lv_obj_has_flag(w.hand[static_cast<size_t>(which)], LV_OBJ_FLAG_HIDDEN) ? -1 : g_hand_deg[static_cast<size_t>(which)];
}
std::string clock_seconds_text() {
    if (current_page() != Page::Clock || !w.digits_group) return "";
    const bool hands = !lv_obj_has_flag(w.hands_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *s = hands ? w.small_seconds : w.seconds;
    return lv_obj_has_flag(s, LV_OBJ_FLAG_HIDDEN) ? std::string() : std::string(lv_label_get_text(s));
}
bool clock_big_digits() { return current_page() == Page::Clock && w.digits_group && !lv_obj_has_flag(w.digits_group, LV_OBJ_FLAG_HIDDEN); }
bool home_seconds_shown() { return current_page() == Page::Home && w.seconds && !lv_obj_has_flag(w.seconds, LV_OBJ_FLAG_HIDDEN); }
std::string home_seconds_text() {
    if (current_page() != Page::Home || !w.seconds || lv_obj_has_flag(w.seconds, LV_OBJ_FLAG_HIDDEN)) return "";
    return lv_label_get_text(w.seconds);
}
int home_ring_width() { return ring.track ? lv_obj_get_style_arc_width(ring.track, LV_PART_INDICATOR) : 0; }
int power_gauge_width() { return w.soc_gauge ? lv_obj_get_style_arc_width(w.soc_gauge, LV_PART_MAIN) : 0; }
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
int nav_count() { return sh.nav ? sh.nav_count : 0; }
std::string nav_text(int i) { return i >= 0 && i < sh.nav_count && sh.nav_label[static_cast<size_t>(i)] ? lv_label_get_text(sh.nav_label[static_cast<size_t>(i)]) : ""; }
bool nav_area(int i, int *x1, int *y1, int *x2, int *y2) { return i >= 0 && i < sh.nav_count && area_of(sh.nav_btn[static_cast<size_t>(i)], x1, y1, x2, y2); }
bool pages_control_area(int slot, int which, int *x1, int *y1, int *x2, int *y2) {
    if (current_page() != Page::Pages || slot < 0 || slot > 3 || which < 0 || which > 2) return false;
    auto *o = w.pages_ctrl[static_cast<size_t>(slot)][static_cast<size_t>(which)];
    if (!o) return false;
    lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
    lv_obj_update_layout(screen_object());
    return area_of(o, x1, y1, x2, y2);
}
bool icon_choice_area(int icon, int, int *x1, int *y1, int *x2, int *y2) {
    if (current_page() != Page::SourceEdit || icon < 1 || icon > 8) return false;
    auto *o = w.icon_btn[static_cast<size_t>(icon)];
    if (!o) return false;
    lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
    lv_obj_update_layout(screen_object());
    return area_of(o, x1, y1, x2, y2);
}
bool clock_dot_lit(int i) {
    if (current_page() != Page::Clock || i < 0 || i > 3 || !w.clock_dot[static_cast<size_t>(i)]) return false;
    return !lv_color_eq(lv_obj_get_style_bg_color(w.clock_dot[static_cast<size_t>(i)], LV_PART_MAIN), hex(P().edge));
}
int switch_current_index() { return current_page() == Page::Actions ? w.switch_current : -1; }
int switch_row_count() { return current_page() == Page::Actions ? static_cast<int>(w.switch_rows.size()) : 0; }
int switch_tile_count() { return current_page() == Page::Actions ? static_cast<int>(w.switch_tiles.size()) : 0; }
int switch_icon(int i) {
    if (current_page() != Page::Actions || i < 0 || static_cast<size_t>(i) >= w.switch_icons.size()) return -1;
    return static_cast<int>(source_icon_of(w.switch_icons[static_cast<size_t>(i)]));
}
}  // namespace dh::ui::detail
