#pragma once
// Shared between the three LVGL files: ui_kit.cpp (widgets), ui_pages.cpp (the pages), ui.cpp (the tick,
// display policy, input). Not a public header: the preview tests include it to drive the UI.
#include "board.hpp"
#include "core.hpp"
#include "lvgl.h"
#include "ui.hpp"
#include "ui_model.hpp"
#include <array>
#include <string>

LV_FONT_DECLARE(btr_ui_16);
LV_FONT_DECLARE(btr_ui_20);
LV_FONT_DECLARE(btr_ui_28);
LV_FONT_DECLARE(font_clock_72);

namespace dh::ui::detail {
enum class Page : uint8_t { Home, Actions, Computer, Settings, Power, Connect, Appearance, Display, Clock, Device };
enum class PowerTab : uint8_t { Level, Readings, Protect };
constexpr int kScreen = 480, kMargin = 24, kContentW = 432, kMinTarget = 56;
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
// The dialog of Dev-Protect. Charge protection (docs/rules.md N1) is not implemented, so no page opens it yet.
lv_obj_t *build_protect_dialog(lv_obj_t *layer, void (*on_confirm)(int limit), void (*on_cancel)());
int body_scroll_y();
void remember_scroll(int y);

// ---- ui.cpp: what the pages ask for ----
void request_page(Page);
void request_power_tab(PowerTab);
void request_update();
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
bool ring_lit(int index);           // 0 wifi, 1 agent, 2 power, 3 time; false when the page has no ring
std::string home_first_action_label();
lv_obj_t *accent_swatch(int index); // Appearance page only, otherwise nullptr
int pairing_bar_percent();          // length of the countdown bar in percent, -1 without a pairing page
void reset_for_test();  // drops the screen, timer and every piece of state, so a test can call ui_start() again
}  // namespace dh::ui::detail
