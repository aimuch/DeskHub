// The device UI on native LVGL, drawn into memory. Hardware, Agent, clock and storage are simulated by this file, so
// a pass proves layout, routing, state handling and redraw behaviour of the real UI code, and nothing about the panel,
// the touch controller, the power chip or the network.
//
// Failure modes, written before the UI code (the model-level ones are in firmware/tests/ui_model_test.cpp):
//  A1  Init: a failed display lock (timeout, invalid state) builds no screen, creates no timer, unlocks nothing and
//      leaves UI_LOCK_FAILED in the snapshot. A good start locks once and unlocks once.
//  A2  Heartbeat: dead before the first tick, alive after, dead after 2 s, and right across the 32-bit wrap.
//  A3  Every page, in every theme x language x text size, draws: no static text is cut off, no text leaves the card
//      or button that holds it, nothing leaves the screen, text in English is ASCII, text in Chinese only uses glyphs
//      of the font subset.
//  A4  Every button and slider has a touch area of at least 56x56 px, and no two touch areas overlap.
//  A5  The screen says what the model says: unknown battery is not 0 %, stale power is "pending", an absent battery
//      draws no fill, the ring lights only healthy channels, gauges stay empty for expired metrics.
//  A6  Navigation: the four tabs, the header buttons, the settings rows and the power segments reach their pages.
//  A7  Wake: while the screen is dimmed or off the first touch only wakes it. The second one acts.
//  A8  Actions: an offline Agent cannot run an action; an online one runs it once; a refused action shows a notice.
//  A9  Appearance: a choice is applied, saved, and survives a restart. A failed save keeps the old look and says so.
//      Corrupt stored text gives the default look. An unreadable accent is never applied.
//  A10 Factory reset needs two taps and the first one does not erase anything. Restart is one tap.
//  A11 Idle: with nothing changing, many ticks draw nothing. Switching pages 1000 times leaks no objects.
//  A12 Settings changes (rotation, brightness, always-on) are applied once, failures stay visible, and the
//      configuration is acknowledged only after the screen shows it.
//  A13 Pairing page: the code (4 to 8 characters) and the hotspot password (8 to 12) are shown only when well formed, never cut off, the countdown is right, the
//      shell is hidden, cancel calls back, hide restores the normal page. It fits in every theme, language and size.
//  A14 Protect dialog: dims everything, offers 80 % and 90 %, passes the chosen limit, cancel closes it.
//  A15 Power page: a control that would do nothing is disabled and explained, never silently inert.
//  A17 Rounded corners: every text and small mark that sits in one of the four corners of the screen is inside the visible
//      quarter circle (detail::kCornerR), on every page, theme, language and text size.
//  A18 Header icons: no words; USB, bolt and battery follow the power state and never show old data; a slash for
//      "not there"; Wi-Fi blinks while it connects and stops when it is connected; the percent can be switched off.
//  A19 Appearance options: the battery percent and the ring thickness are stored, applied, and read back by a restart.
//  A21 The digits sit in the middle of the battery icon, measured on the pixels that are drawn (a text box that is
//      centred is not enough: the line of a font has room under the digits).
//  A20 No "experimental" word anywhere on the power pages and in the confirm dialog.
//  A16 Charge protection flow: off shows its button; the dialog passes the limit on; the controller's state is what the
//      page says (on, failsafe, unconfirmed); turning it off asks nothing; a save that fails is shown and changes nothing.
#include "agent_client.hpp"
#include "board.hpp"
#include "charge.hpp"
#include "core.hpp"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "ui_internal.hpp"
#include "ui_store.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ui = dh::ui;
namespace detail = dh::ui::detail;

// ---------------------------------------------------------------- simulated services
unsigned restart_calls = 0;
static int64_t mock_ms = 1000;
static lv_display_t *display;
static lv_indev_t *pointer_device;
static lv_point_t pointer_point{0, 0};
static bool pointer_pressed = false;
static std::array<uint8_t, 480 * 480 * 3> framebuffer{};
static std::array<uint8_t, 480 * 50 * 2> draw_a{}, draw_b{};
static uint64_t flush_calls = 0;

static dh::Snapshot mock_snapshot;
static dh::DisplayConfig mock_config;
static dh::PowerState mock_power;
static dh::charge::Status mock_charge;
static dh::charge::Settings charge_last;
static bool charge_apply_ok = true;
static unsigned charge_applies = 0;
static bool power_auto = true;
static bool mock_paired = true;
static int mock_brightness = -1;
static esp_err_t mock_brightness_result = ESP_OK, mock_rotation_result = ESP_OK, mock_save_result = ESP_OK, mock_lock_result = ESP_OK;
static int mock_rotation = 0;
static unsigned rotation_calls = 0, brightness_calls = 0, save_calls = 0, reset_calls = 0, submit_calls = 0, lock_calls = 0, unlock_calls = 0;
static bool submit_result = true;
static std::string stored_text;
static bool store_ok = true;
static unsigned store_saves = 0;

extern "C" time_t time(time_t *out) {
    const time_t value = 1793156400 + (mock_ms - 1000) / 1000;  // 2026-10-28 03:00 UTC = 11:00 CST, a Wednesday
    if (out) *out = value;
    return value;
}
esp_err_t esp_lv_adapter_lock(int32_t) {
    ++lock_calls;
    return mock_lock_result;
}
void esp_lv_adapter_unlock() { ++unlock_calls; }
lv_indev_t *bsp_display_get_input_dev() { return pointer_device; }

namespace dh {
const char *const VERSION = "0.3.0-preview";
int64_t monotonic_ms() { return mock_ms; }
Snapshot snapshot() { return mock_snapshot; }
DisplayConfig config() { return mock_config; }
void mutate(void (*fn)(Snapshot &, void *), void *ctx) { fn(mock_snapshot, ctx); }
esp_err_t save_config(DisplayConfig value) {
    ++save_calls;
    if (mock_save_result != ESP_OK) return mock_save_result;
    if (!valid_config(value)) return ESP_FAIL;
    value.revision++;
    mock_config = value;
    return ESP_OK;
}
esp_err_t reset_config() { ++reset_calls; mock_config = {}; return ESP_OK; }
Secrets secrets() {
    Secrets s;
    if (mock_paired) { s.ssid = "x"; s.url = "https://x"; s.token = "t"; s.ca_pem = "p"; }
    return s;
}
PowerState board_power() { return mock_power; }
namespace charge {
Status status() { return mock_charge; }
bool apply(const Settings &s) {
    ++charge_applies;
    charge_last = s;
    if (!charge_apply_ok && s.enabled) return false;       // saving fails: nothing changes
    mock_charge.enabled = s.enabled && valid(s);
    mock_charge.state = mock_charge.enabled ? State::Charging : State::Off;
    mock_charge.limit = s.limit;
    mock_charge.resume = s.resume;
    return charge_apply_ok || s.enabled;
}
}  // namespace charge
esp_err_t board_brightness(int percent) { ++brightness_calls; if (mock_brightness_result == ESP_OK) mock_brightness = percent; return mock_brightness_result; }
esp_err_t board_rotation(int degrees) { ++rotation_calls; if (mock_rotation_result == ESP_OK) mock_rotation = degrees; return mock_rotation_result; }
bool agent_submit(const Action &) {
    ++submit_calls;
    if (submit_result) mock_snapshot.command = "已接收，等待执行";
    return submit_result;
}
namespace ui {
std::string store_load() { return stored_text; }
bool store_save(const std::string &text) {
    ++store_saves;
    if (!store_ok) return false;
    stored_text = text;
    return true;
}
}  // namespace ui
}  // namespace dh

// ---------------------------------------------------------------- display and input
static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *pixels) {
    ++flush_calls;
    const int width = lv_area_get_width(area);
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) {
            if (x < 0 || y < 0 || x >= 480 || y >= 480) continue;
            const int src = ((y - area->y1) * width + (x - area->x1)) * 2, dst = (y * 480 + x) * 3;
            const uint16_t c = pixels[src] | (pixels[src + 1] << 8);
            framebuffer[dst] = ((c >> 11) & 31) * 255 / 31;
            framebuffer[dst + 1] = ((c >> 5) & 63) * 255 / 63;
            framebuffer[dst + 2] = (c & 31) * 255 / 31;
        }
    lv_display_flush_ready(d);
}
static void round_invalidation(lv_event_t *e) {  // the same 2 px bounds as the board's CO5300 callback
    auto *a = static_cast<lv_area_t *>(lv_event_get_param(e));
    a->x1 = (a->x1 >> 1) << 1; a->y1 = (a->y1 >> 1) << 1;
    a->x2 = ((a->x2 >> 1) << 1) + 1; a->y2 = ((a->y2 >> 1) << 1) + 1;
}
static void read_pointer(lv_indev_t *, lv_indev_data_t *data) {
    data->state = pointer_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point = pointer_point;
}

// ---------------------------------------------------------------- test plumbing
static std::vector<std::string> failures_now;
static std::string current_case;
#define EXPECT(cond)                                                                          \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::ostringstream o_;                                                            \
            o_ << current_case << ": " << __FILE__ << ':' << __LINE__ << "  " << #cond;       \
            failures_now.push_back(o_.str());                                                 \
        }                                                                                     \
    } while (0)
#define NOTE(cond, why)                                                                       \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::ostringstream o_;                                                            \
            o_ << current_case << ": " << __FILE__ << ':' << __LINE__ << "  " << #cond << "  -- " << why; \
            failures_now.push_back(o_.str());                                                 \
        }                                                                                     \
    } while (0)

static void step(int ms = 100) {
    mock_ms += ms;
    lv_tick_inc(ms);
    if (power_auto) mock_power.observed_ms = mock_ms - 100;
    detail::tick(nullptr);
    lv_timer_handler();  // also runs deferred deletes and the redraw, like the device's LVGL task
    if (detail::screen_object()) lv_obj_update_layout(detail::screen_object());
    lv_obj_update_layout(lv_layer_top());
}
static void pointer(bool pressed, int x, int y) {
    pointer_pressed = pressed;
    pointer_point = {x, y};
    mock_ms += 40;
    lv_tick_inc(40);
    if (power_auto) mock_power.observed_ms = mock_ms - 100;
    lv_indev_read(pointer_device);
    detail::tick(nullptr);
    lv_obj_update_layout(detail::screen_object());
}
static void tap(int x, int y) { pointer(true, x, y); pointer(false, x, y); }

static void walk(lv_obj_t *o, const std::function<void(lv_obj_t *)> &f) {
    f(o);
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) walk(lv_obj_get_child(o, i), f);
}
static bool shown(lv_obj_t *o) {
    for (; o; o = lv_obj_get_parent(o)) if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}
static std::vector<lv_obj_t *> labels() {
    std::vector<lv_obj_t *> out;
    for (lv_obj_t *root : {lv_screen_active(), lv_layer_top()})
        walk(root, [&](lv_obj_t *o) { if (lv_obj_check_type(o, &lv_label_class) && shown(o)) out.push_back(o); });
    return out;
}
static std::vector<std::string> texts() {
    std::vector<std::string> out;
    for (auto *l : labels()) out.push_back(lv_label_get_text(l));
    return out;
}
static bool has(const std::string &t) { for (auto &x : texts()) if (x == t) return true; return false; }
static bool contains(const std::string &t) { for (auto &x : texts()) if (x.find(t) != std::string::npos) return true; return false; }
static lv_obj_t *find(const std::string &t) {
    for (auto *l : labels()) if (t == lv_label_get_text(l)) return l;
    return nullptr;
}
static lv_obj_t *clickable_ancestor(lv_obj_t *o) {
    for (; o; o = lv_obj_get_parent(o)) if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) return o;
    return nullptr;
}
static bool click_text(const std::string &t) {
    auto *l = find(t);
    if (!l) return false;
    auto *target = clickable_ancestor(l);
    if (!target) return false;
    lv_obj_scroll_to_view_recursive(target, LV_ANIM_OFF);
    lv_obj_update_layout(detail::screen_object());
    lv_area_t a;
    lv_obj_get_coords(target, &a);
    tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    return true;
}
static void click_nav(int index) { tap(24 + 3 + index * 108 + 54, 440); }
static bool top_layer_blocks() {  // a visible full-screen clickable layer (a dialog's dimmer, the wake shield) takes every touch
    for (uint32_t i = 0; i < lv_obj_get_child_count(lv_layer_top()); ++i) {
        auto *o = lv_obj_get_child(lv_layer_top(), i);
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        if (shown(o) && lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && a.x1 <= 0 && a.y1 <= 0 && a.x2 >= 479 && a.y2 >= 479) return true;
    }
    return false;
}
static std::vector<lv_obj_t *> touch_targets() {
    std::vector<lv_obj_t *> out;
    const bool blocked = top_layer_blocks();
    for (lv_obj_t *root : {lv_screen_active(), lv_layer_top()})
        if (!(blocked && root == lv_screen_active()))
        walk(root, [&](lv_obj_t *o) {
            if ((lv_obj_check_type(o, &lv_button_class) || lv_obj_check_type(o, &lv_slider_class)) && shown(o)) out.push_back(o);
        });
    return out;
}
static std::vector<lv_obj_t *> arcs() {
    std::vector<lv_obj_t *> out;
    walk(lv_screen_active(), [&](lv_obj_t *o) { if (lv_obj_check_type(o, &lv_arc_class) && shown(o)) out.push_back(o); });
    return out;
}
static bool arc_filled(lv_obj_t *a) { return lv_obj_get_style_arc_opa(a, LV_PART_INDICATOR) != LV_OPA_TRANSP; }

// ---------------------------------------------------------------- mock world
static void reset_world() {
    mock_ms = 1000; pointer_pressed = false; power_auto = true; mock_paired = true;
    mock_snapshot = {};
    mock_snapshot.device_id = "dh-0a1b2c";
    mock_snapshot.time_valid = true; mock_snapshot.time_quality = "manual";
    mock_snapshot.wifi_ready = true; mock_snapshot.agent_ready = true;
    mock_snapshot.wifi = "已连接"; mock_snapshot.agent = "已连接"; mock_snapshot.ip = "10.10.10.23"; mock_snapshot.rssi = -61;
    mock_snapshot.cpu = "23.4%"; mock_snapshot.memory = "48.2%"; mock_snapshot.network = "1.20 MiB/s"; mock_snapshot.network_send = "0.31 MiB/s";
    mock_snapshot.sampled_ms = 900; mock_snapshot.metrics_expires_ms = 1000000;
    mock_snapshot.actions[0] = {"desktop.switch_to_mac", "切到 Mac", "monitor", "mac", 1};
    mock_snapshot.actions[1] = {"desktop.switch_to_pc", "切到 PC", "monitor", "pc", 1};
    mock_snapshot.action_count = 2;
    mock_config = {};
    mock_power = {};
    mock_power.battery = "present"; mock_power.vbus = "present"; mock_power.charging = "charging"; mock_power.reason = "ok";
    mock_power.soc_percent = 67; mock_power.battery_mv = 3921; mock_power.vbus_mv = 5120; mock_power.chip_temp_c = 41.3f;
    mock_power.measure_reason = "ADC_OK"; mock_power.observed_ms = mock_ms - 100;
    mock_charge = {}; mock_charge.charging_known = true; mock_charge.charging = true; charge_last = {}; charge_apply_ok = true; charge_applies = 0;
    mock_brightness = -1; mock_rotation = 0;
    mock_brightness_result = mock_rotation_result = mock_save_result = mock_lock_result = ESP_OK;
    rotation_calls = brightness_calls = save_calls = reset_calls = submit_calls = lock_calls = unlock_calls = restart_calls = 0;
    submit_result = true; stored_text.clear(); store_ok = true; store_saves = 0;
}
static void start_ui() {
    detail::reset_for_test();
    dh::ui_start();
    step();
}
static void fresh(const std::string &stored = "") {
    reset_world();
    stored_text = stored;
    start_ui();
}
static std::set<uint32_t> font_glyphs() {
    std::ifstream in(UI_CHARS_FILE);
    std::stringstream ss;
    ss << in.rdbuf();
    std::set<uint32_t> out;
    const std::string s = ss.str();
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        uint32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; } else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; } else if ((c >> 4) == 14) { cp = c & 0xf; n = 3; } else { cp = c & 7; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        out.insert(cp);
        i += n;
    }
    return out;
}
static std::vector<uint32_t> codepoints(const std::string &s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        uint32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; } else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; } else if ((c >> 4) == 14) { cp = c & 0xf; n = 3; } else { cp = c & 7; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        out.push_back(cp);
        i += n;
    }
    return out;
}

// ---------------------------------------------------------------- the checks shared by many cases
// A3: static text must fit. Labels that clip or dot must be wide enough for their text; no label leaves its holder.
// A17: the panel's corners are rounded. A point in a corner square is visible only if it is inside the quarter circle.
static bool corner_visible(int x, int y) {
    const int r = detail::kCornerR;
    const bool left = x < r, right = x > 479 - r, top = y < r, bottom = y > 479 - r;
    if (!(left || right) || !(top || bottom)) return true;
    const int cx = left ? r : 479 - r, cy = top ? r : 479 - r;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}
static bool body_clips(lv_obj_t *o, const lv_area_t &a) {   // inside the body but outside what it shows: hidden, not cut by a corner
    auto *body = detail::body_object();
    for (auto *p = o; p; p = lv_obj_get_parent(p)) {
        if (p != body) continue;
        lv_area_t b;
        lv_obj_get_coords(body, &b);
        return a.y1 < b.y1 || a.y2 > b.y2;
    }
    return false;
}
static void check_corners(const std::string &where) {
    auto all_visible = [&](int x1, int y1, int x2, int y2) {
        return corner_visible(x1, y1) && corner_visible(x2, y1) && corner_visible(x1, y2) && corner_visible(x2, y2);
    };
    for (auto *l : labels()) {   // the rectangle the text really fills, not the label's whole box
        const std::string text = lv_label_get_text(l);
        if (text.empty()) continue;
        lv_area_t a;
        lv_obj_get_coords(l, &a);
        if (a.y1 < 0 || a.y2 >= 480) continue;   // scrolled out of the body, clipped there and not by the corner
        if (lv_obj_get_parent(l) != lv_layer_top() && body_clips(l, a)) continue;
        lv_point_t size;
        lv_text_get_size(&size, text.c_str(), lv_obj_get_style_text_font(l, LV_PART_MAIN), lv_obj_get_style_text_letter_space(l, LV_PART_MAIN),
                         lv_obj_get_style_text_line_space(l, LV_PART_MAIN), lv_obj_get_width(l), LV_TEXT_FLAG_NONE);
        const int w = std::min<int>(size.x, lv_obj_get_width(l)), align = lv_obj_get_style_text_align(l, LV_PART_MAIN);
        int x1 = a.x1;
        if (align == LV_TEXT_ALIGN_RIGHT) x1 = a.x2 - w + 1;
        else if (align == LV_TEXT_ALIGN_CENTER) x1 = (a.x1 + a.x2 - w) / 2;
        NOTE(all_visible(x1, a.y1, x1 + w - 1, a.y1 + size.y - 1), where << " cut by a rounded corner: \"" << text << "\" [" << x1 << "," << a.y1 << "," << x1 + w - 1 << "," << a.y1 + size.y - 1 << "]");
    }
    for (lv_obj_t *root : {lv_screen_active(), lv_layer_top()})   // small marks: the status dot, the battery outline
        walk(root, [&](lv_obj_t *o) {
            if (lv_obj_check_type(o, &lv_label_class) || !shown(o) || lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) return;
            lv_area_t a;
            lv_obj_get_coords(o, &a);
            const int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
            if (w > 40 || h > 20 || a.y1 < 0 || a.y2 >= 480) return;
            if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) == LV_OPA_TRANSP && lv_obj_get_style_border_width(o, LV_PART_MAIN) == 0) return;
            NOTE(all_visible(a.x1, a.y1, a.x2, a.y2), where << " mark cut by a rounded corner [" << a.x1 << "," << a.y1 << "," << a.x2 << "," << a.y2 << "]");
        });
}
static void check_layout(const std::string &where) {
    const auto glyphs = font_glyphs();
    const bool english = detail::look.lang == dh::ui::Lang::EnUS;
    for (auto *l : labels()) {
        const std::string text = lv_label_get_text(l);
        lv_area_t a;
        lv_obj_get_coords(l, &a);
        const auto mode = lv_label_get_long_mode(l);
        if (mode == LV_LABEL_LONG_DOT && text.size() >= 3 && text.compare(text.size() - 3, 3, "...") == 0)
            NOTE(false, where << " cut off with dots: \"" << text << "\"");  // LVGL rewrites the text of a dotted label in place
        if (mode == LV_LABEL_LONG_DOT || mode == LV_LABEL_LONG_CLIP) {
            lv_point_t size;
            lv_text_get_size(&size, text.c_str(), lv_obj_get_style_text_font(l, LV_PART_MAIN), lv_obj_get_style_text_letter_space(l, LV_PART_MAIN), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            NOTE(size.x <= lv_obj_get_width(l) + 1, where << " cut off: \"" << text << "\" needs " << size.x << " has " << lv_obj_get_width(l));
        }
        auto *parent = lv_obj_get_parent(l);
        if (parent && parent != detail::body_object() && parent != detail::screen_object() && parent != lv_layer_top()) {
            lv_area_t p;
            lv_obj_get_coords(parent, &p);
            NOTE(a.x1 >= p.x1 - 1 && a.x2 <= p.x2 + 1 && a.y1 >= p.y1 - 1 && a.y2 <= p.y2 + 1,
                 where << " leaves its holder: \"" << text << "\" [" << a.x1 << "," << a.y1 << "," << a.x2 << "," << a.y2 << "] in [" << p.x1 << "," << p.y1 << "," << p.x2 << "," << p.y2 << "]");
        }
        NOTE(a.x1 >= 0 && a.x2 < 480 && a.y2 < 480 + 2000, where << " off screen: \"" << text << "\"");
        for (uint32_t cp : codepoints(text)) {
            if (cp >= 0xF000 && cp <= 0xF8FF) continue;  // LVGL symbol font
            if (english) NOTE((cp >= 0x20 && cp < 0x7f) || cp == 0x0a, where << " English page has \"" << text << "\"");
            else NOTE(glyphs.count(cp) > 0, where << " glyph U+" << std::hex << cp << std::dec << " is not in the font: \"" << text << "\"");
        }
    }
    check_corners(where);
}
// A4: touch areas.
static void check_targets(const std::string &where) {
    const auto targets = touch_targets();
    std::vector<lv_area_t> areas;
    for (auto *t : targets) {
        lv_area_t a;
        lv_obj_get_click_area(t, &a);
        NOTE(a.x2 - a.x1 + 1 >= 56 && a.y2 - a.y1 + 1 >= 56, where << " small target " << (a.x2 - a.x1 + 1) << "x" << (a.y2 - a.y1 + 1));
        // What a finger can hit is what its parents show: LVGL only looks for children inside the parent's area.
        bool visible = true;
        for (lv_obj_t *p = lv_obj_get_parent(t); p && visible; p = lv_obj_get_parent(p)) {
            lv_area_t pa, cut;
            lv_obj_get_coords(p, &pa);
            cut = {std::max(a.x1, pa.x1), std::max(a.y1, pa.y1), std::min(a.x2, pa.x2), std::min(a.y2, pa.y2)};
            visible = cut.x1 <= cut.x2 && cut.y1 <= cut.y2;
            if (visible) a = cut;
        }
        if (visible) areas.push_back(a);
    }
    for (size_t i = 0; i < areas.size(); ++i)
        for (size_t j = i + 1; j < areas.size(); ++j) {
            const int w = std::min(areas[i].x2, areas[j].x2) - std::max(areas[i].x1, areas[j].x1) + 1;
            const int h = std::min(areas[i].y2, areas[j].y2) - std::max(areas[i].y1, areas[j].y1) + 1;
            NOTE(!(w > 4 && h > 4), where << " touch areas overlap by " << w << "x" << h << " [" << areas[i].x1 << "," << areas[i].y1 << "," << areas[i].x2 << "," << areas[i].y2 << "] and [" << areas[j].x1 << "," << areas[j].y1 << "," << areas[j].x2 << "," << areas[j].y2 << "]");
        }
}
static void goto_view(detail::Page page, detail::PowerTab tab = detail::PowerTab::Level) {
    detail::request_page(page);
    detail::request_power_tab(tab);
    step();
    step();
}

static std::filesystem::path out_dir;
static void save_png_source(const std::string &name) {
    if (out_dir.empty()) return;
    for (int i = 0; i < 3; ++i) { lv_obj_invalidate(lv_screen_active()); lv_refr_now(display); }
    std::ofstream image(out_dir / (name + ".ppm"), std::ios::binary);
    image << "P6\n480 480\n255\n";
    image.write(reinterpret_cast<const char *>(framebuffer.data()), static_cast<std::streamsize>(framebuffer.size()));
}

using detail::Page;
using detail::PowerTab;
static const std::array<std::pair<Page, PowerTab>, 12> kViews = {{
    {Page::Home, PowerTab::Level}, {Page::Actions, PowerTab::Level}, {Page::Computer, PowerTab::Level}, {Page::Settings, PowerTab::Level},
    {Page::Power, PowerTab::Level}, {Page::Power, PowerTab::Readings}, {Page::Power, PowerTab::Protect}, {Page::Connect, PowerTab::Level},
    {Page::Appearance, PowerTab::Level}, {Page::Display, PowerTab::Level}, {Page::Clock, PowerTab::Level}, {Page::Device, PowerTab::Level}}};
static const char *view_name(size_t i) {
    static const char *n[] = {"home", "actions", "computer", "settings", "power-level", "power-readings", "power-protect", "connect", "appearance", "display", "clock", "device"};
    return n[i];
}

// ---------------------------------------------------------------- cases
static void case_init_lock() {
    for (esp_err_t bad : {ESP_ERR_TIMEOUT, ESP_ERR_INVALID_STATE}) {
        reset_world();
        mock_lock_result = bad;
        detail::reset_for_test();
        dh::ui_start();
        EXPECT(detail::screen_object() == nullptr);
        EXPECT(unlock_calls == 0);
        EXPECT(mock_snapshot.error.rfind("UI_LOCK_FAILED:", 0) == 0);
        EXPECT(!dh::ui_alive());
    }
    reset_world();
    detail::reset_for_test();
    dh::ui_start();
    EXPECT(detail::screen_object() != nullptr);
    EXPECT(lock_calls == 1 && unlock_calls == 1);
    EXPECT(mock_snapshot.error.empty());
}
static void case_heartbeat() {
    reset_world();
    mock_ms = 4294967000LL;  // 296 ms before the 32-bit wrap
    detail::reset_for_test();
    dh::ui_start();
    EXPECT(!dh::ui_alive());  // no tick yet
    step(50);
    EXPECT(dh::ui_alive());
    mock_ms += 500;           // across the wrap
    EXPECT(dh::ui_alive());
    mock_ms += 1600;
    EXPECT(!dh::ui_alive());
    step(0);
    EXPECT(dh::ui_alive());
}
static void case_matrix() {
    int views = 0;
    for (const char *theme : {"mint", "ocean", "amber"})
        for (const char *lang : {"zh-CN", "en-US"})
            for (const char *size : {"small", "standard", "large"}) {
                fresh(std::string(theme) + "|-|" + lang + "|" + size);
                if (std::string(lang) == "en-US") {  // action names come from the user's Agent; the English check is for our own text
                    mock_snapshot.actions[0].label = "To Mac"; mock_snapshot.actions[1].label = "To PC"; step();
                }
                for (size_t i = 0; i < kViews.size(); ++i) {
                    goto_view(kViews[i].first, kViews[i].second);
                    const std::string where = std::string(theme) + "/" + lang + "/" + size + "/" + view_name(i);
                    check_layout(where);
                    check_targets(where);
                    ++views;
                }
                {   // the pairing page, with the longest strings it can get
                    dh::PairingInfo info; info.code = "ZZZZZZZZ"; info.hotspot = "DeskHub-C598"; info.hotspot_password = "ABCDEFGHJKMN"; info.remaining_s = 299;
                    dh::ui_pairing_show(info, [] {}); step(); step();
                    const std::string where = std::string(theme) + "/" + lang + "/" + size + "/pairing";
                    check_layout(where);
                    check_targets(where);
                    EXPECT(has("ABCDEFGHJKMN"));
                    dh::ui_pairing_hide(); step();
                }
            }
    EXPECT(views == 3 * 2 * 3 * 12);
}
static void case_state_fidelity() {
    using detail::header_battery_text;
    using detail::header_bolt_shown;
    using detail::header_slashed;
    using detail::header_usb_shown;
    fresh();
    // The header is icons: none of the old words is on the screen.
    EXPECT(!contains("电脑在线") && !contains("已连接 ·") && !contains("USB接入") && !contains("电量 ") && !contains("dBm"));
    EXPECT(header_battery_text() == "67" && detail::header_fill_width() > 0 && header_bolt_shown() && header_usb_shown());
    EXPECT(!header_slashed(0) && !header_slashed(1) && !header_slashed(2));
    for (int i = 0; i < 4; ++i) EXPECT(detail::ring_lit(i));
    // charged, and idle: the USB stays, the bolt goes
    mock_power.charging = "full"; step();
    EXPECT(header_usb_shown() && !header_bolt_shown() && header_battery_text() == "67");
    mock_power.charging = "idle"; step();
    EXPECT(header_usb_shown() && !header_bolt_shown());
    mock_power.charging = "charging"; mock_power.vbus = "absent"; step();
    EXPECT(!header_usb_shown());
    // unknown level: a question mark, no fill, never 0
    fresh();
    mock_power.soc_percent = -1; step();
    EXPECT(header_battery_text() == "?" && detail::header_fill_width() == 0 && !has("0%") && !contains("电量 0"));
    // absent battery: slash on the battery, the USB is still there
    mock_power.soc_percent = 55; mock_power.battery = "absent"; step();
    EXPECT(header_battery_text().empty() && detail::header_fill_width() == 0 && header_slashed(2) && !header_bolt_shown() && header_usb_shown());
    // stale power: nothing from the old sample, no USB, no bolt
    fresh();
    power_auto = false; mock_power.observed_ms = mock_ms - 100; step(7000);
    EXPECT(header_battery_text() == "?" && detail::header_fill_width() == 0 && !header_usb_shown() && !header_bolt_shown() && !detail::ring_lit(2));
    // fault
    fresh();
    mock_power.fault = true; step();
    EXPECT(header_battery_text() == "!" && !header_usb_shown() && !header_bolt_shown() && !detail::ring_lit(2));
    // channels: not configured and offline are slashed
    fresh();
    mock_snapshot.wifi_ready = false; mock_snapshot.wifi = "未配置"; mock_snapshot.agent_ready = false; mock_snapshot.time_valid = false; step();
    EXPECT(!detail::ring_lit(0) && !detail::ring_lit(1) && detail::ring_lit(2) && !detail::ring_lit(3));
    EXPECT(header_slashed(0) && header_slashed(1) && has("--:--") && has("时间未同步"));
    // connecting: no slash, and the icon blinks; connected: steady
    fresh();
    mock_snapshot.wifi_ready = false; mock_snapshot.wifi = "正在连接Wi-Fi"; step();
    EXPECT(!header_slashed(0));
    bool bright = false, dim = false;
    for (int i = 0; i < 40; ++i) {
        step(100);
        const int opa = detail::header_icon_opa(0);
        bright = bright || opa >= 250;
        dim = dim || opa <= 80;
    }
    EXPECT(bright && dim);
    mock_snapshot.wifi_ready = true; mock_snapshot.wifi = "已连接"; step();
    for (int i = 0; i < 40; ++i) { step(100); EXPECT(detail::header_icon_opa(0) >= 250); }
    // the percent switched off: the fill stays, the digits go
    fresh("mint|-|zh-CN|standard|nopct|standard");
    EXPECT(header_battery_text().empty() && detail::header_fill_width() > 0);
    // expired metrics on the Computer page
    fresh();
    goto_view(Page::Computer);
    auto a = arcs();
    EXPECT(a.size() == 2 && arc_filled(a[0]) && arc_filled(a[1]) && has("23%") && has("48%") && has("电脑指标 · 数据有效"));
    mock_snapshot.metrics_expires_ms = mock_ms - 1; step();
    a = arcs();
    EXPECT(a.size() == 2 && !arc_filled(a[0]) && !arc_filled(a[1]) && !has("23%") && has("指标已过期 · 等待刷新"));
    mock_snapshot.sampled_ms = 0; step();
    EXPECT(has("尚未采样 · 等待电脑连接"));
    // power level gauge
    fresh();
    goto_view(Page::Power);
    a = arcs();
    EXPECT(a.size() == 1 && arc_filled(a[0]) && has("67%") && has("PMIC 估算，未验证"));
    mock_power.soc_percent = -1; step();
    EXPECT(!arc_filled(arcs()[0]) && has("--") && has("电量未知"));
    // readings
    goto_view(Page::Power, PowerTab::Readings);
    EXPECT(has("3.92 V") && has("5.12 V") && has("41.3 度") && has("这是电源芯片的温度，不是电池温度。"));
    mock_power.measure_reason = "ADC_ENABLE_FAILED"; mock_power.battery_mv = -1; mock_power.vbus_mv = -1; mock_power.chip_temp_c = NAN; step();
    EXPECT(!has("3.92 V") && contains("测量通道未能开启"));
    // command notice
    fresh();
    mock_snapshot.command = "已接收，等待执行"; step();
    EXPECT(has("已接收，等待执行"));
    step(15000);
    EXPECT(!has("已接收，等待执行"));
    // offline Home variant
    fresh();
    mock_snapshot.agent_ready = false; mock_snapshot.metrics_expires_ms = mock_ms - 1; step();
    EXPECT(has("指标已过期 · 等待刷新") && contains("Agent 未连接"));
    for (auto *t : touch_targets()) { (void)t; }
}
static void case_navigation() {
    fresh();
    EXPECT(detail::current_page() == Page::Home);
    click_nav(1); EXPECT(detail::current_page() == Page::Actions);
    click_nav(2); EXPECT(detail::current_page() == Page::Computer);
    click_nav(3); EXPECT(detail::current_page() == Page::Settings);
    click_nav(0); EXPECT(detail::current_page() == Page::Home);
    tap(100, 30); EXPECT(detail::current_page() == Page::Connect);       // header left
    tap(380, 30); EXPECT(detail::current_page() == Page::Power);         // header right
    click_nav(3);
    struct Row { const char *label; Page page; };
    for (Row r : {Row{"显示与亮度", Page::Display}, Row{"外观", Page::Appearance}, Row{"连接与配对", Page::Connect}, Row{"电池与电源", Page::Power}, Row{"桌面时钟", Page::Clock}, Row{"设备信息", Page::Device}}) {
        click_nav(3);
        EXPECT(detail::current_page() == Page::Settings);
        EXPECT(click_text(r.label));
        NOTE(detail::current_page() == r.page, "row " << r.label);
    }
    // tap the clock in the ring
    click_nav(0); tap(240, 150); EXPECT(detail::current_page() == Page::Clock);
    // power segments
    goto_view(Page::Power);
    EXPECT(click_text("读数")); EXPECT(detail::current_power_tab() == PowerTab::Readings);
    EXPECT(click_text("保护")); EXPECT(detail::current_power_tab() == PowerTab::Protect);
    EXPECT(click_text("电量")); EXPECT(detail::current_power_tab() == PowerTab::Level);
}
static void case_wake() {
    fresh();
    step(61000);                                    // idle: the clock page after 60 s
    EXPECT(detail::current_page() == Page::Clock);
    step(60000);                                    // dimmed after 120 s
    EXPECT(mock_brightness < mock_config.brightness);
    click_nav(1);                                   // the first touch only wakes
    EXPECT(detail::current_page() == Page::Clock);
    click_nav(1);
    EXPECT(detail::current_page() == Page::Actions);
    EXPECT(mock_brightness == mock_config.brightness);
}
static void case_actions() {
    fresh();
    EXPECT(has("切到 Mac") && has("切到 PC"));
    EXPECT(click_text("切到 Mac"));
    EXPECT(submit_calls == 1 && has("已接收，等待执行"));
    submit_result = false; mock_snapshot.command = "尚无操作"; step(16000);
    EXPECT(click_text("切到 PC"));
    EXPECT(submit_calls == 2 && has("当前不可用或已有操作，请稍后点击"));
    // offline Agent: nothing is submitted
    fresh();
    mock_snapshot.agent_ready = false; step();
    EXPECT(click_text("切到 Mac"));
    EXPECT(submit_calls == 0);
    goto_view(Page::Actions);
    EXPECT(has("切到 Mac"));
    click_text("切到 Mac");
    EXPECT(submit_calls == 0);
    // no actions at all
    fresh();
    mock_snapshot.action_count = 0; step();
    goto_view(Page::Actions);
    EXPECT(has("尚无可用动作"));
    // layout order from the managed configuration
    fresh();
    mock_config.layout.action_count = 1; mock_config.layout.action_ids[0] = "desktop.switch_to_pc"; mock_config.revision++; step(); step();
    EXPECT(detail::home_first_action_label() == "切到 PC");
    // a sub text is shown for an empty label
    fresh();
    mock_snapshot.actions[0].label = ""; step();
    goto_view(Page::Actions);
    EXPECT(has("desktop.switch_to_mac"));
}
static void case_appearance() {
    fresh();
    goto_view(Page::Appearance);
    EXPECT(click_text("深海蓝"));
    EXPECT(stored_text == "ocean|-|zh-CN|standard|pct|standard");
    EXPECT(detail::current_appearance().theme == dh::ui::ThemeId::Ocean);
    EXPECT(lv_color_eq(lv_obj_get_style_bg_color(detail::screen_object(), LV_PART_MAIN), lv_color_hex(0x03131c)));
    EXPECT(click_text("English"));
    EXPECT(stored_text == "ocean|-|en-US|standard|pct|standard");
    EXPECT(has("Appearance") && has("Home") && has("Settings") && !contains("外观"));
    EXPECT(click_text("Large"));
    EXPECT(detail::look.body == &btr_ui_28 && stored_text == "ocean|-|en-US|large|pct|standard");
    EXPECT(detail::current_page() == Page::Appearance);  // the page stays
    // a restart restores it
    const std::string saved = stored_text;
    start_ui();
    EXPECT(detail::current_appearance().lang == dh::ui::Lang::EnUS && detail::look.body == &btr_ui_28 && detail::current_appearance().theme == dh::ui::ThemeId::Ocean);
    // a failed save keeps the old look and says so
    fresh();
    goto_view(Page::Appearance);
    store_ok = false;
    EXPECT(click_text("暖橙"));
    EXPECT(detail::current_appearance().theme == dh::ui::ThemeId::Mint && stored_text.empty() && has("保存失败"));
    EXPECT(lv_color_eq(lv_obj_get_style_bg_color(detail::screen_object(), LV_PART_MAIN), lv_color_hex(0x03100e)));
    // corrupt storage: the default look
    for (const char *bad : {"garbage", "mint|zzzz|zh-CN|standard", "mint|-|fr|standard", "|||"}) {
        fresh(bad);
        EXPECT(detail::current_appearance().theme == dh::ui::ThemeId::Mint && detail::current_appearance().lang == dh::ui::Lang::ZhCN && detail::look.body == &btr_ui_20);
    }
    // an unreadable accent in storage is not applied
    fresh("mint|0e2a24|zh-CN|standard");
    EXPECT(detail::look.palette.accent == 0x5ee6b8);
    // a readable accent is applied
    fresh("mint|ff7eb6|zh-CN|standard");
    EXPECT(detail::look.palette.accent == 0xff7eb6);
    goto_view(Page::Appearance);
    EXPECT(click_text("强调色"));  // a heading is not a button: nothing happens
    EXPECT(stored_text == "mint|ff7eb6|zh-CN|standard");
    // accent swatches are buttons named by index
    {
        auto *swatch = detail::accent_swatch(1);
        EXPECT(swatch != nullptr);
        if (swatch) { lv_obj_scroll_to_view_recursive(swatch, LV_ANIM_OFF); lv_obj_update_layout(detail::screen_object()); lv_area_t a; lv_obj_get_coords(swatch, &a); tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2); }
    }
    EXPECT(stored_text == "mint|4fc3f7|zh-CN|standard|pct|standard" && detail::look.palette.accent == 0x4fc3f7);
    // the page of every tier still draws its own choices selected
    fresh("amber|-|zh-CN|small");
    goto_view(Page::Appearance);
    EXPECT(detail::look.body == &btr_ui_16 && detail::look.title == &btr_ui_20);
}
static void case_reset_restart() {
    fresh();
    goto_view(Page::Device);
    EXPECT(has("恢复出厂设置"));
    click_text("恢复出厂设置");
    EXPECT(reset_calls == 0 && has("再次点击确认删除"));
    click_nav(0); goto_view(Page::Device);   // leaving the page cancels the confirmation
    EXPECT(has("恢复出厂设置") && !has("再次点击确认删除"));
    click_text("恢复出厂设置");
    click_text("再次点击确认删除");
    EXPECT(reset_calls == 1 && contains("已删除本机配置"));
    fresh();
    goto_view(Page::Device);
    click_text("重启设备");
    EXPECT(restart_calls == 1);
}
static void case_idle_and_leaks() {
    fresh();
    for (int i = 0; i < 5; ++i) step();
    lv_refr_now(display);
    const uint64_t before = flush_calls;
    for (int i = 0; i < 30; ++i) { step(); lv_refr_now(display); }
    EXPECT(flush_calls == before);
    // data changes do redraw
    mock_power.soc_percent = 40; step(); lv_refr_now(display);   // the digits in the battery icon of the header
    EXPECT(flush_calls > before);
    // page switches do not leak
    fresh();
    const int base = detail::live_object_count();
    for (int i = 0; i < 1000; ++i) {
        const auto &v = kViews[i % kViews.size()];
        detail::request_page(v.first); detail::request_power_tab(v.second); step(0);
    }
    detail::request_page(Page::Home); step(0); step(0);
    EXPECT(detail::live_object_count() == base);
}
static void case_orchestration() {
    // brightness: applied once for the target, not every tick
    fresh();
    const unsigned calls = brightness_calls;
    for (int i = 0; i < 10; ++i) step();
    EXPECT(brightness_calls == calls);
    // rotation: each requested angle is tried once, a failure stays visible
    mock_rotation_result = ESP_FAIL; mock_config.rotation = 90; mock_config.revision++;
    for (int i = 0; i < 10; ++i) step();
    EXPECT(rotation_calls == 1 && mock_rotation == 0 && mock_snapshot.error.rfind("DISPLAY_ROTATION_FAILED:", 0) == 0);
    goto_view(Page::Display);
    EXPECT(contains("屏幕旋转失败，保持 0 度"));
    mock_rotation_result = ESP_OK; mock_config.rotation = 180; mock_config.revision++;
    for (int i = 0; i < 5; ++i) step();
    EXPECT(rotation_calls == 2 && mock_rotation == 180 && mock_snapshot.error.empty());
    // always-on: no sleep, no dimming
    fresh();
    mock_config.off_s = 0; mock_config.revision++; step();
    step(400000);
    EXPECT(mock_brightness >= mock_config.brightness || mock_brightness == mock_config.night_cap);
    // burn-in protection: always-on moves the clock page a little, a normal active screen does not
    fresh();
    mock_config.off_s = 0; mock_config.revision++;
    goto_view(Page::Clock); step();
    EXPECT(lv_obj_get_style_translate_x(detail::body_object(), LV_PART_MAIN) != 0);
    mock_config.protection = false; mock_config.revision++; step();
    EXPECT(lv_obj_get_style_translate_x(detail::body_object(), LV_PART_MAIN) == 0);
    fresh();
    goto_view(Page::Clock); step();
    EXPECT(lv_obj_get_style_translate_x(detail::body_object(), LV_PART_MAIN) == 0);
    // the setting rows save through save_config
    fresh();
    goto_view(Page::Display);
    const unsigned saves = save_calls;
    EXPECT(click_text("屏幕保护"));
    EXPECT(save_calls == saves + 1 && !mock_config.protection);
    mock_save_result = ESP_FAIL;
    click_text("屏幕保护");
    EXPECT(has("保存失败") || contains("保存失败"));
    // the slider path: debounced, one save
    fresh();
    goto_view(Page::Display);
    detail::schedule_brightness(80);
    const unsigned saves2 = save_calls;
    step(1000); EXPECT(save_calls == saves2);
    step(1500); EXPECT(save_calls == saves2 + 1 && mock_config.brightness == 80);
    // configuration acknowledgement
    fresh();
    mock_config.revision = 7;
    for (int i = 0; i < 3; ++i) step();
    EXPECT(dh::ui_configuration_applied(7));
    EXPECT(!dh::ui_configuration_applied(6));
    EXPECT(dh::ui_configuration_applied(mock_config.revision));
    EXPECT(!dh::ui_configuration_applied(mock_config.revision + 1));
}
static void case_pairing() {
    fresh();
    static int cancels = 0;
    cancels = 0;
    dh::PairingInfo info;
    info.code = "624819"; info.hotspot = "DeskHub-C598"; info.hotspot_password = "K7M2QX9TWD4P"; info.remaining_s = 272; info.total_s = 300;
    dh::ui_pairing_show(info, [] { ++cancels; });
    step();
    EXPECT(has("624819") && has("04:32") && contains("DeskHub-C598") && has("取消配对"));
    EXPECT(has("热点密码") && has("K7M2QX9TWD4P"));
    EXPECT(!has("总览") && !has("动作"));           // the shell is hidden
    EXPECT(detail::pairing_bar_percent() == 90);
    step(2000);
    EXPECT(has("04:30"));
    check_layout("pairing"); check_targets("pairing");
    for (const char *bad : {"", "12 34", "abc", "123", "123456789", "1234567890123", "62481\n"}) {
        dh::PairingInfo p = info; p.code = bad;
        dh::ui_pairing_show(p, [] { ++cancels; }); step();
        EXPECT(has("------"));
    }
    // the hotspot password is drawn only when it is 8 to 63 printable characters without a space; anything else is dashes
    for (const char *bad : {"", "short", "has space in it", "tab\there1234", "new\nline1234", "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"}) {
        dh::PairingInfo p = info; p.hotspot_password = bad;
        dh::ui_pairing_show(p, [] { ++cancels; }); step();
        EXPECT(has("------------") && !has("K7M2QX9TWD4P"));
        if (std::strlen(bad) > 4) EXPECT(!has(bad));
    }
    { dh::PairingInfo p = info; p.hotspot_password = std::string(13, 'p'); dh::ui_pairing_show(p, [] { ++cancels; }); step(); EXPECT(has("------------") && !has(std::string(13, 'p'))); }
    { dh::PairingInfo p = info; p.hotspot_password = std::string(12, 'p'); dh::ui_pairing_show(p, [] { ++cancels; }); step(); EXPECT(has(std::string(12, 'p'))); }
    dh::ui_pairing_show(info, [] { ++cancels; }); step();
    dh::PairingInfo over = info; over.remaining_s = 9999; over.total_s = 300;
    dh::ui_pairing_show(over, [] { ++cancels; }); step();
    EXPECT(detail::pairing_bar_percent() == 100);
    dh::PairingInfo neg = info; neg.remaining_s = -5;
    dh::ui_pairing_show(neg, [] { ++cancels; }); step();
    EXPECT(has("00:00") && detail::pairing_bar_percent() == 0);
    EXPECT(click_text("取消配对"));
    EXPECT(cancels == 1);
    dh::ui_pairing_hide(); step();
    EXPECT(has("总览") && !has("取消配对"));
    EXPECT(detail::current_page() == Page::Home);
    save_png_source("state-pairing-hidden");
    dh::ui_pairing_hide(); step();                 // a second hide does nothing
    EXPECT(has("总览"));
}
static void case_protect_dialog() {
    fresh();
    static int confirmed = 0, cancelled = 0;
    confirmed = cancelled = 0;
    goto_view(Page::Power, PowerTab::Protect);
    auto *dialog = detail::build_protect_dialog(lv_layer_top(), [](int limit) { confirmed = limit; }, [] { ++cancelled; });
    step();
    EXPECT(dialog != nullptr);
    EXPECT(has("开启充电保护？") && !contains("实验") && has("80%") && has("90%") && has("取消") && has("开启"));
    lv_area_t a;
    lv_obj_get_coords(dialog, &a);
    EXPECT(a.x1 == 0 && a.y1 == 0 && a.x2 == 479 && a.y2 == 479);   // the dimmer covers the whole screen
    check_layout("protect"); check_targets("protect");
    EXPECT(click_text("90%"));
    EXPECT(click_text("开启"));
    EXPECT(confirmed == 90 && cancelled == 0);
    EXPECT(!has("开启充电保护？"));
    auto *again = detail::build_protect_dialog(lv_layer_top(), [](int limit) { confirmed = limit; }, [] { ++cancelled; });
    step();
    EXPECT(again && click_text("取消"));
    EXPECT(cancelled == 1 && !has("开启充电保护？"));
    confirmed = 0;
    auto *third = detail::build_protect_dialog(lv_layer_top(), [](int limit) { confirmed = limit; }, [] { ++cancelled; });
    step();
    EXPECT(third && click_text("开启"));
    EXPECT(confirmed == 80);                        // the default limit
}
static void case_protect_tab() {
    fresh();
    goto_view(Page::Power, PowerTab::Protect);
    EXPECT(has("充电保护") && !contains("实验") && has("充电保护：未开启"));
    // It works now: the button is enabled and opens the dialog. When it could not work it is disabled and says why.
    auto *button = clickable_ancestor(find("开启…"));
    EXPECT(button && !lv_obj_has_state(button, LV_STATE_DISABLED));
    mock_power.battery = "absent"; mock_power.soc_percent = -1; step(); step();
    button = clickable_ancestor(find("开启…"));
    EXPECT(button && lv_obj_has_state(button, LV_STATE_DISABLED) && has("未检测到电池。"));
    click_text("开启…");
    EXPECT(!has("开启充电保护？") && charge_applies == 0);
    mock_power.battery = "present"; mock_power.soc_percent = 67; step(); step();   // it works again when the battery is back
    button = clickable_ancestor(find("开启…"));
    EXPECT(button && !lv_obj_has_state(button, LV_STATE_DISABLED));
    goto_view(Page::Power, PowerTab::Level);
    EXPECT(has("充电保护：未开启") && !contains("实验"));
}
static void case_protect_flow() {
    fresh();
    goto_view(Page::Power, PowerTab::Protect);
    EXPECT(click_text("开启…"));
    step();
    EXPECT(has("开启充电保护？") && charge_applies == 0);        // asking first: nothing changes before the second tap
    EXPECT(click_text("90%"));
    EXPECT(click_text("开启"));
    step(); step();
    EXPECT(charge_applies == 1 && charge_last.enabled && charge_last.limit == 90 && charge_last.resume == 85);
    EXPECT(!has("开启充电保护？") && has("充电保护中：上限 90%") && has("电量到上限后停止充电。") && has("关闭"));
    check_layout("protect-on"); check_targets("protect-on");
    goto_view(Page::Power, PowerTab::Level);
    EXPECT(has("充电保护中：上限 90%"));
    goto_view(Page::Power, PowerTab::Protect);
    mock_charge.state = dh::charge::State::Holding; mock_charge.charging = false; step(); step();
    EXPECT(has("已停止充电，低于回充线后恢复。"));
    mock_charge.state = dh::charge::State::Failsafe; mock_charge.reason = "SAMPLE_STALE"; mock_charge.charging = true; step(); step();
    EXPECT(has("充电保护失效，已恢复正常充电") && has("读不到电量。"));
    check_layout("protect-failsafe"); check_targets("protect-failsafe");
    mock_charge.state = dh::charge::State::Unconfirmed; mock_charge.reason = "SWITCH_UNCONFIRMED"; mock_charge.charging_known = false; step(); step();
    EXPECT(has("无法确认充电状态") && !has("已停止") && !has("已恢复"));
    check_layout("protect-unconfirmed"); check_targets("protect-unconfirmed");
    EXPECT(click_text("关闭"));                                // off asks nothing
    step(); step();
    EXPECT(charge_applies == 2 && !charge_last.enabled && !has("开启充电保护？") && has("充电保护：未开启"));

    fresh();                                                    // saving fails: the page says so and nothing is on
    charge_apply_ok = false;
    goto_view(Page::Power, PowerTab::Protect);
    EXPECT(click_text("开启…"));
    EXPECT(click_text("开启"));
    step(); step();
    EXPECT(charge_applies == 1 && !mock_charge.enabled && has("未能保存，充电保护没有开启。") && has("充电保护：未开启"));
    fresh();                                                    // turning off is in force even when the setting cannot be saved, and the page says so
    mock_charge.enabled = true; mock_charge.state = dh::charge::State::Charging; mock_charge.limit = 80; mock_charge.resume = 75;
    charge_apply_ok = false;
    goto_view(Page::Power, PowerTab::Protect);
    EXPECT(has("充电保护中：上限 80%"));
    EXPECT(click_text("关闭"));
    step(); step();
    EXPECT(charge_applies == 1 && !charge_last.enabled && !mock_charge.enabled && has("已关闭，但没有保存这个设置。") && has("充电保护：未开启"));
}
static bool chosen(const std::string &label) {   // a segmented choice that is selected has a 2 px border in the accent colour
    auto *b = clickable_ancestor(find(label));
    return b && lv_obj_get_style_border_width(b, LV_PART_MAIN) == 2;
}
static void case_appearance_options() {   // A19
    fresh();
    goto_view(Page::Appearance);
    EXPECT(has("电池百分比") && has("圆环粗细") && has("显示") && has("隐藏") && has("细") && has("粗"));
    EXPECT(chosen("显示") && !chosen("隐藏") && !chosen("细") && !chosen("粗"));   // the defaults: percent shown, ring standard
    EXPECT(click_text("隐藏"));
    step(); step();
    EXPECT(chosen("隐藏") && !chosen("显示"));
    EXPECT(stored_text == "mint|-|zh-CN|standard|nopct|standard" && detail::header_battery_text().empty() && detail::current_appearance().show_percent == false);
    EXPECT(click_text("显示"));
    step(); step();
    EXPECT(stored_text == "mint|-|zh-CN|standard|pct|standard" && detail::header_battery_text() == "67");
    EXPECT(click_text("粗"));
    step(); step();
    EXPECT(stored_text == "mint|-|zh-CN|standard|pct|thick" && chosen("粗") && !chosen("细"));
    fresh("mint|-|zh-CN|standard|nopct|thin");      // what is stored is what the page shows as chosen
    goto_view(Page::Appearance);
    EXPECT(chosen("隐藏") && !chosen("显示") && chosen("细") && !chosen("粗"));
    // the rings follow the tier, on the Home page and on the power page; a restart reads the stored tier back
    struct Tier { const char *name; int home, gauge; };
    for (const Tier &t : {Tier{"thin", 12, 7}, Tier{"standard", 20, 11}, Tier{"thick", 28, 15}}) {
        fresh(std::string("mint|-|zh-CN|standard|pct|") + t.name);
        goto_view(Page::Home);
        EXPECT(detail::home_ring_width() == t.home);
        goto_view(Page::Power, PowerTab::Level);
        EXPECT(detail::power_gauge_width() == t.gauge);
    }
    fresh("mint|-|zh-CN|standard");                 // what 0.3.x stored
    goto_view(Page::Home);
    EXPECT(detail::home_ring_width() == 20 && detail::header_battery_text() == "67");
    fresh();                                        // saving fails: the old look stays and the page says so
    store_ok = false;
    goto_view(Page::Appearance);
    EXPECT(click_text("隐藏"));
    step(); step();
    EXPECT(detail::header_battery_text() == "67" && has("保存失败"));
    // both new sections fit every theme, language and size (the layout matrix visits this page)
}
static void case_no_experimental_word() {   // A20
    fresh();
    for (auto [page, tab] : {std::pair<Page, PowerTab>{Page::Power, PowerTab::Level}, {Page::Power, PowerTab::Protect}, {Page::Power, PowerTab::Readings}, {Page::Settings, PowerTab::Level}}) {
        goto_view(page, tab);
        EXPECT(!contains("实验") && !contains("xperimental"));
    }
    goto_view(Page::Power, PowerTab::Protect);
    EXPECT(click_text("开启…"));
    step();
    EXPECT(has("开启充电保护？") && !contains("实验") && !contains("xperimental"));
    fresh("mint|-|en-US|standard");
    goto_view(Page::Power, PowerTab::Protect);
    EXPECT(!contains("xperimental") && !contains("实验"));
}
static void case_battery_digits() {   // A21
    fresh();
    struct Case { int soc; const char *battery; };
    const Case cases[] = {{5, "present"}, {67, "present"}, {88, "present"}, {100, "present"}, {-1, "present"}};   // 1 digit, 2, 2, 3, and the question mark
    for (const Case &c : cases) {
        mock_power.soc_percent = c.soc; mock_power.battery = c.battery;
        step(); step();
        for (int i = 0; i < 3; ++i) { lv_obj_invalidate(lv_screen_active()); lv_refr_now(display); }
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        EXPECT(detail::header_battery_body(&x1, &y1, &x2, &y2));
        int min_x = 9999, max_x = -1, min_y = 9999, max_y = -1;
        for (int y = y1 + 3; y <= y2 - 3; ++y)
            for (int x = x1 + 3; x <= x2 - 3; ++x) {
                const size_t at = (static_cast<size_t>(y) * 480 + static_cast<size_t>(x)) * 3;
                if ((framebuffer[at] + framebuffer[at + 1] + framebuffer[at + 2]) / 3 > 150) {   // ink is bright; the fill and the background are not
                    min_x = std::min(min_x, x); max_x = std::max(max_x, x);
                    min_y = std::min(min_y, y); max_y = std::max(max_y, y);
                }
            }
        EXPECT(max_y >= 0);
        const double dy = (min_y + max_y) / 2.0 - (y1 + y2) / 2.0, dx = (min_x + max_x) / 2.0 - (x1 + x2) / 2.0;
        NOTE(std::fabs(dy) <= 1.0 && std::fabs(dx) <= 1.5,
             "battery digits for level " << c.soc << " are off centre: ink [" << min_x << "," << min_y << "," << max_x << "," << max_y << "] in body [" << x1 << "," << y1 << "," << x2 << "," << y2 << "] dx " << dx << " dy " << dy);
    }
}
static void case_screenshots() {
    if (out_dir.empty()) return;
    for (const char *theme : {"mint", "ocean", "amber"}) {
        fresh(std::string(theme) + "|-|zh-CN|standard");
        for (size_t i = 0; i < kViews.size(); ++i) {
            goto_view(kViews[i].first, kViews[i].second);
            save_png_source(std::string(theme) + "-" + view_name(i));
        }
    }
    fresh("mint|ff7eb6|en-US|large");
    for (size_t i : {0, 1, 4, 8}) { goto_view(kViews[i].first, kViews[i].second); save_png_source(std::string("en-large-") + view_name(i)); }
    fresh("ocean|-|zh-CN|small");
    for (size_t i : {0, 3, 8}) { goto_view(kViews[i].first, kViews[i].second); save_png_source(std::string("zh-small-") + view_name(i)); }
    fresh();   // offline Home
    mock_snapshot.agent_ready = false; mock_snapshot.metrics_expires_ms = mock_ms - 1; step(); step();
    save_png_source("state-offline");
    fresh();
    dh::PairingInfo info; info.code = "624819"; info.hotspot = "DeskHub-C598"; info.hotspot_password = "K7M2QX9TWD4P"; info.remaining_s = 272;
    dh::ui_pairing_show(info, [] {}); step();
    save_png_source("state-pairing");
    fresh();
    goto_view(Page::Power, PowerTab::Protect);
    detail::build_protect_dialog(lv_layer_top(), [](int) {}, [] {}); step();
    save_png_source("state-protect");
}

int main(int argc, char **argv) {
    if (argc > 1) { out_dir = argv[1]; std::filesystem::create_directories(out_dir); }
    setenv("TZ", "CST-8", 1);
    tzset();
    lv_init();
    display = lv_display_create(480, 480);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_a.data(), draw_b.data(), draw_a.size(), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_add_event_cb(display, round_invalidation, LV_EVENT_INVALIDATE_AREA, nullptr);
    pointer_device = lv_indev_create();
    lv_indev_set_type(pointer_device, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer_device, read_pointer);

    struct Case { const char *name; void (*fn)(); };
    const Case cases[] = {
        {"init_lock", case_init_lock}, {"heartbeat", case_heartbeat}, {"layout_matrix", case_matrix},
        {"state_fidelity", case_state_fidelity}, {"navigation", case_navigation}, {"wake_on_touch", case_wake},
        {"actions", case_actions}, {"appearance", case_appearance}, {"reset_restart", case_reset_restart},
        {"idle_and_leaks", case_idle_and_leaks}, {"orchestration", case_orchestration}, {"pairing_page", case_pairing},
        {"protect_dialog", case_protect_dialog}, {"protect_tab", case_protect_tab}, {"protect_flow", case_protect_flow},
        {"appearance_options", case_appearance_options}, {"no_experimental_word", case_no_experimental_word},
        {"battery_digits", case_battery_digits}, {"screenshots", case_screenshots}};
    int failed = 0;
    std::ostringstream report;
    report << "{\n  \"simulation_only\": true,\n  \"cases\": [\n";
    bool first = true;
    for (const auto &c : cases) {
        current_case = c.name;
        failures_now.clear();
        c.fn();
        const bool ok = failures_now.empty();
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", c.name);
        if (!ok) {
            ++failed;
            const size_t shown_max = std::getenv("DESKHUB_PREVIEW_ALL") ? failures_now.size() : 12;   // all of them when asked
            for (size_t i = 0; i < failures_now.size() && i < shown_max; ++i) std::printf("    %s\n", failures_now[i].c_str());
            if (failures_now.size() > shown_max) std::printf("    ... %zu more\n", failures_now.size() - shown_max);
        }
        report << (first ? "" : ",\n") << "    {\"name\": \"" << c.name << "\", \"passed\": " << (ok ? "true" : "false") << ", \"failed_checks\": " << failures_now.size() << "}";
        first = false;
    }
    report << "\n  ]\n}\n";
    if (!out_dir.empty()) std::ofstream(out_dir / "report.json") << report.str();
    std::puts(failed ? "SIMULATION: FAILED" : "SIMULATION: all cases passed (no hardware, sensor or network was involved)");
    return failed ? 1 : 0;
}
