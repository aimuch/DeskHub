#pragma once
// Shared between the three LVGL files: ui_kit.cpp (widgets), ui_pages.cpp (the pages), ui.cpp (the tick,
// display policy, input). Not a public header: the preview tests include it to drive the UI.
#include "board.hpp"
#include "core.hpp"
#include "lvgl.h"
#include "ui.hpp"
#include "ui_model.hpp"
#include <array>
#include <cmath>
#include <string>

LV_FONT_DECLARE(btr_ui_16);
LV_FONT_DECLARE(btr_ui_20);
LV_FONT_DECLARE(btr_ui_28);
LV_FONT_DECLARE(font_clock_72);

namespace dh::ui::detail {
enum class Page : uint8_t { Home, Actions, Computer, Settings, Power, Connect, Appearance, Display, Clock, Device, Sound };
enum class PowerTab : uint8_t { Level, Readings, Protect };
constexpr int kScreen = 480, kMargin = 24, kContentW = 432, kMinTarget = 56;
// The panel has rounded corners (Waveshare does not publish the radius). Text and marks in the four corners must stay
// inside a quarter circle of this radius, so they are not cut off. One number; make it bigger if a corner still cuts.
constexpr int kCornerR = 80;
// How far from the left or right edge of the screen a mark must be if the outer edge of the mark is `from_edge` px from
// the top (or the bottom) of the screen, so it stays inside the rounded corner. 0 where the corner no longer matters.
inline int corner_inset(int from_edge) {
    if (from_edge >= kCornerR) return 0;
    const int d = kCornerR - (from_edge < 0 ? 0 : from_edge);
    return kCornerR - static_cast<int>(std::sqrt(static_cast<double>(kCornerR * kCornerR - d * d)));
}
constexpr int kBodyTop = 62, kBodyH = 340, kNavTop = 408;

// The look in force: the palette and fonts that follow the Appearance.
struct Look {
    Appearance appearance;
    Palette palette;
    Lang lang = Lang::ZhCN;
    const lv_font_t *title = &btr_ui_28, *body = &btr_ui_20, *caption = &btr_ui_16;
};
extern Look look;
Look make_look(const Appearance &);
const lv_font_t *font_for(int px);
inline lv_color_t hex(uint32_t rgb) { return lv_color_hex(rgb); }

// Everything the pages read in one tick. Built by ui.cpp.
struct Frame {
    Snapshot s;
    DisplayConfig c;
    PowerState power;
    std::string wifi_name;                // the stored network, on the pages that ask for it
    charge::Status charge;                // charge protection, as the controller reports it
    std::string charge_error;             // result of the last try to turn it on or off
    int64_t now_ms = 0;
    time_t wall = 0;
    bool paired = false;
    int64_t command_changed_ms = 0;       // when the last command text appeared
    bool config_pending = false;          // a brightness change waiting for its debounced save
    int effective_brightness = 0;
    DisplayMode mode = DisplayMode::Active;
    std::string last_error;               // result of the last settings action
    std::string rotation_error;
    bool confirm_reset = false;
};

// ---- ui_kit.cpp: equality-guarded setters (LVGL 9.4 invalidates even when a style is set to its current value) ----
void set_text(lv_obj_t *, const std::string &);
void set_optional_text(lv_obj_t *, const std::string &);  // hidden while empty
void set_bg(lv_obj_t *, uint32_t rgb);
void set_border(lv_obj_t *, uint32_t rgb);
void set_ink(lv_obj_t *, uint32_t rgb);
void set_font(lv_obj_t *, const lv_font_t *);
void set_translate(lv_obj_t *, int x, int y);
void set_hidden(lv_obj_t *, bool);
uint32_t tone_color(Tone);
// Builders. `parent` owns the result.
lv_obj_t *panel(lv_obj_t *parent, int w, int h, uint32_t bg, uint32_t edge, int radius = 24);
lv_obj_t *holder(lv_obj_t *parent, int w, int h);
lv_obj_t *dot(lv_obj_t *parent, int size, uint32_t rgb, bool lit = true);
lv_obj_t *label(lv_obj_t *parent, const std::string &text, int w, const lv_font_t *font, uint32_t rgb);
lv_obj_t *label_at(lv_obj_t *parent, const std::string &text, int x, int y, int w, const lv_font_t *font, uint32_t rgb);
lv_obj_t *button(lv_obj_t *parent, lv_event_cb_t cb, void *data, int w, int h);  // grows its touch area to 56 px
lv_obj_t *pill(lv_obj_t *parent, const std::string &text, uint32_t ink, uint32_t fill, uint32_t edge, const lv_font_t *font);
lv_obj_t *arc_segment(lv_obj_t *parent, int size, int start, int end, uint32_t rgb, int width, lv_opa_t opa);
lv_obj_t *gauge(lv_obj_t *parent, int size, int width, uint32_t rgb);
void gauge_set(lv_obj_t *, bool valid, float percent, uint32_t rgb);
void ensure_touch_target(lv_obj_t *);  // lv_obj_set_ext_click_area up to kMinTarget

// ---- ui_pages.cpp ----
void build_shell(lv_obj_t *screen);     // header, navigation: built once per look
void refresh_shell(const Frame &, Page, bool pairing_active);
void build_page(lv_obj_t *body, Page, PowerTab, const Frame &);
void refresh_page(Page, PowerTab, const Frame &);
void shift_shell(int dx);               // burn-in protection moves the header with the clock page
lv_obj_t *build_pairing(lv_obj_t *screen, const PairingInfo &, int64_t deadline_ms, void (*on_cancel)());
void refresh_pairing(const PairingInfo &, int64_t deadline_ms, int64_t now_ms);
// The dialog of Dev-Protect: opened by request_protect_toggle() when protection is off.
lv_obj_t *build_protect_dialog(lv_obj_t *layer, void (*on_confirm)(int limit), void (*on_cancel)());
// The sheet of Dev-RingInfo: opened by a tap on the ring, closed by its button or a tap outside it.
lv_obj_t *build_ring_info(lv_obj_t *layer, const RingInfoView &, void (*on_close)());
bool ring_info_open();
bool ring_info_dot_filled(int row);   // the dot of a row of the open sheet is filled (the segment is lit)
bool notice_dot_shown();             // the dot before the line under the ring
int body_scroll_y();
void remember_scroll(int y);

// ---- ui.cpp: what the pages ask for ----
void request_page(Page);
void request_power_tab(PowerTab);
void request_update();
void request_protect_toggle();       // the button of the Protect tab: off opens the dialog, on turns it off
void request_ring_info();            // a tap on the ring of the home page
void request_sound_set(bool on);     // the switch of the sound page
void request_sound_test();           // "listen"
void request_sound_volume(int level);   // a level of the volume: stored, then one test tone at it
void wifi_open();
void request_pairing();              // "pair" on the Connect page: asks the backend for the hotspot
void wifi_close();
bool wifi_opened();
void wifi_tick(lv_obj_t *screen, int64_t now, bool blocked, bool pairing);
void wifi_reset();
lv_obj_t *wifi_reveal_object();
void note_activity();
void user_screen_off();
void schedule_brightness(int percent);
void change_setting(int which);       // the cycle/toggle rows of the display page, same numbering as before
void apply_appearance(const Appearance &);
void do_restart();
void do_factory_reset();
void submit_action(size_t index);

// ---- test hooks (the preview harness drives these; production code does not) ----
void tick(lv_timer_t *);
lv_obj_t *screen_object();
lv_obj_t *body_object();
Page current_page();
PowerTab current_power_tab();
const Appearance &current_appearance();
int live_object_count();
int header_fill_width();            // px width of the battery fill in the header, 0 when nothing is drawn
std::string header_battery_text();  // the digits inside the battery icon of the header ("" when none)
bool header_bolt_shown();
bool header_usb_shown();
bool header_bolt_area(int *x1, int *y1, int *x2, int *y2);   // where the bolt is on the screen (false when it is not shown)
bool header_usb_area(int *x1, int *y1, int *x2, int *y2);
bool home_ring_area(int *x1, int *y1, int *x2, int *y2);     // the ring of the home page, and the line under it
bool home_notice_area(int *x1, int *y1, int *x2, int *y2);
bool sound_controls_dimmed();        // the switch of the sound page is drawn dimmed (no speaker)
int sound_selected();                // 0 off, 1 on: which half of the switch of the sound page is selected; -1 when that page is not shown
int sound_volume_selected();         // the selected level of the volume on the sound page; -1 when that page is not shown
bool home_notice_dot_area(int *x1, int *y1, int *x2, int *y2);   // the dot before the line (false when it is not shown)
bool header_slashed(int which);     // 0 Wi-Fi, 1 computer, 2 battery: the slash is drawn
bool header_battery_body(int *x1, int *y1, int *x2, int *y2);   // where the battery outline is on the screen
int header_icon_opa(int which);     // 0 Wi-Fi: how strong the icon is drawn now, 255 steady, low in the dim half of a blink
int home_ring_width();              // px thickness of the status ring on the Home page, 0 without one
int power_gauge_width();            // px thickness of the gauge on the Level tab, 0 without one
bool ring_lit(int index);           // 0 wifi, 1 agent, 2 power, 3 time; false when the page has no ring
std::string home_first_action_label();
lv_obj_t *accent_swatch(int index); // Appearance page only, otherwise nullptr
int pairing_bar_percent();          // length of the countdown bar in percent, -1 without a pairing page
void reset_for_test();  // drops the screen, timer and every piece of state, so a test can call ui_start() again
}  // namespace dh::ui::detail
