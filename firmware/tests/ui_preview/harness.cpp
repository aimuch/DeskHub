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
//  A6  Navigation: the buttons of the navigation, the header buttons, the settings rows and the power segments reach their pages.
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
//  A27 The overview (docs/design v2 HomeRing, as Dev-HomeEmpty was): the ring is 252 px and the ring with its line under it sits
//      in the middle of the body, so that no blank band is left; actions or none, nothing moves; the taps still work.
//  A26 The bolt is inside the battery (docs/design Dev-Header): bolt and digits are one group in the middle of the battery,
//      the bolt in front of the digits with a gap; no bolt slot is left outside the battery, so the USB icon is next to it.
//  A25 The sound page (docs/design Dev-Sound): a row in Settings; the alert sound is off until it is turned on, and turning it
//      on or off is stored (a failure to store leaves the choice as it was and says so); "listen" asks the speaker whatever the
//      choice; with no speaker nothing can be turned on or listened to, and the page says why; a command text that calls for a
//      sound reaches the speaker once per change of the text, with the night flag.
//      The volume: three levels, low until one is chosen; a tap stores the level and plays one test tone at that level, with the
//      alert sound off too; a level that cannot be stored stays as it was, says so and plays nothing; with no speaker it is
//      dimmed and a tap does nothing; the page shows the stored level when it is opened. The old "fixed volume" line is gone.
//  A24 The ring is explained (docs/design Dev-RingInfo): the line under it says "tap the ring" or names the first dark segment;
//      a tap on the ring opens a sheet with one row per segment; "close", or a tap outside the sheet, closes it; the clock in
//      the middle still opens the clock page; another note under the ring (a command, an error) has priority over the line.
//  A23 No "set up network and pairing" card: with no input sources the switch page says there are none in one line of text.
//      The header icons and the Connect page already say what is missing.
//  A22 While the pairing page is shown the screen stays awake: the person has to read the code and the hotspot password,
//      and the window is five minutes, as long as the screen would be off. Nothing else keeps it awake.
//  A21 The digits sit in the middle of the battery icon, measured on the pixels that are drawn (a text box that is
//      centred is not enough: the line of a font has room under the digits).
//  A20 No "experimental" word anywhere on the power pages and in the confirm dialog.
//  A28 Pages and the home page (docs/design v2 Pages): the device starts on the home page, the first SHOWN page (Switch by
//      default). The navigation is the shown pages in their order, then Settings; there is no computer page. Showing, hiding
//      and moving a page is stored at once and changes the navigation at once; the last shown page cannot be hidden and the
//      page says so; a failed save keeps the old order and says so; a restart starts on the new home page.
//  A29 Wake goes home: when the screen comes back from the clock page that idling forced, it shows the home page. A page that
//      the person chose with a navigation button is not overridden.
//  A30 The switch page: a list or tiles with icons (Settings > Input sources). Each source shows its name and its port; a tap
//      runs it once; offline nothing runs. The style is stored and survives a restart.
//  A31 Name and icon of a source (Settings > Input sources > a source): the keyboard edits the name (ASCII, 1 to 32 bytes);
//      Done stores it and the switch page shows it; back closes the keyboard and stores nothing; an icon tap stores the icon
//      and the switch page draws it; "restore" brings back the App's name and icon; a failed save keeps the old name and says so.
//  A32 Themes: night (pure black) is the default; the three classic themes can still be chosen and change the background;
//      what 0.3.x stored opens in the night theme with its colour.
//  A33 No computer metrics anywhere: no CPU or memory on any page, and the Agent is never asked for them.
//  A34 The clock page (docs/design v2 HomeClock): the four dots follow the ring (lit only for a healthy channel, dark when
//      the channel is down or its data is stale); the time and the date are there; an unknown time lights no time dot.
//  A16 Charge protection flow: off shows its button; the dialog passes the limit on; the controller's state is what the
//      page says (on, failsafe, unconfirmed); turning it off asks nothing; a save that fails is shown and changes nothing.
#include "agent_client.hpp"
#include "board.hpp"
#include "audio.hpp"
#include "charge.hpp"
#include "core.hpp"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "ui_internal.hpp"
#include "ui_store.hpp"
#include "wifi_change.hpp"
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
// The speaker, simulated: what the page asks of it is recorded.
static bool audio_available = true, audio_enabled = false, audio_store_ok = true;
static int audio_tests = 0, audio_paired = 0, audio_volume = 0;
static std::vector<std::string> audio_commands;
static std::vector<int> audio_test_levels;   // the volume each test tone was played at
static dh::charge::Settings charge_last;
static bool charge_apply_ok = true;
static unsigned charge_applies = 0;
static bool power_auto = true;
static bool mock_paired = true;
static esp_err_t mock_reset_result = ESP_OK;
static std::string mock_time_set;
static bool mock_time_ok = true;
static int mock_rtc_writes = 0;
static int mock_brightness = -1;
static esp_err_t mock_brightness_result = ESP_OK, mock_rotation_result = ESP_OK, mock_save_result = ESP_OK, mock_lock_result = ESP_OK;
static int mock_rotation = 0;
static unsigned rotation_calls = 0, brightness_calls = 0, save_calls = 0, reset_calls = 0, submit_calls = 0, lock_calls = 0, unlock_calls = 0;
static bool submit_result = true;
static std::string stored_text, stored_sources;
static bool store_ok = true, sources_ok = true;
static unsigned sources_saves = 0;
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
esp_err_t reset_config() { ++reset_calls; if (mock_reset_result != ESP_OK) return mock_reset_result; mock_config = {}; return ESP_OK; }
bool set_time_utc(const char *s, const char *quality) { if (!mock_time_ok || strcmp(quality, "manual")) return false; mock_time_set = s; return true; }
bool board_rtc_write(time_t) { ++mock_rtc_writes; return true; }
Secrets secrets() {
    Secrets s;
    if (mock_paired) { s.ssid = "Office"; s.url = "https://x"; s.token = "t"; s.ca_pem = "p"; }
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
namespace audio {   // the speaker, simulated
bool available() { return audio_available; }
bool enabled() { return audio_enabled; }
bool set_enabled(bool on) {
    if (!audio_store_ok) return false;
    audio_enabled = on;
    return true;
}
int volume() { return audio_volume; }
bool reset_settings() { if (!audio_store_ok) return false; audio_enabled = false; audio_volume = 0; return true; }
bool set_volume(int level) {
    if (!audio_store_ok || level < 0 || level >= kVolumeLevels) return false;
    audio_volume = level;
    return true;
}
bool test() {
    if (!audio_available) return false;
    ++audio_tests;
    audio_test_levels.push_back(audio_volume);
    return true;
}
// Like the real one: a text without a tone ("尚无操作", "已接收，等待执行") is not a result and makes no sound.
void on_command(const std::string &text, bool quiet_hours, int64_t) {
    if (tone_for_command(text)) audio_commands.push_back(text + (quiet_hours ? "|quiet" : ""));
}
void on_pairing_done(int64_t) { ++audio_paired; }
}  // namespace audio
esp_err_t board_brightness(int percent) { ++brightness_calls; if (mock_brightness_result == ESP_OK) mock_brightness = percent; return mock_brightness_result; }
esp_err_t board_rotation(int degrees) { ++rotation_calls; if (mock_rotation_result == ESP_OK) mock_rotation = degrees; return mock_rotation_result; }
static std::string last_submit_id;
bool agent_submit(const Action &a) {
    ++submit_calls;
    last_submit_id = a.id;
    if (submit_result) mock_snapshot.command = "已接收，等待执行";
    return submit_result;
}
namespace ui {
std::string store_load() { return stored_text; }
bool store_clear() { if (!store_ok) return false; stored_text.clear(); stored_sources.clear(); return true; }
std::string sources_load() { return stored_sources; }
bool sources_save(const std::string &text) {
    ++sources_saves;
    if (!sources_ok) return false;
    stored_sources = text;
    return true;
}
bool store_save(const std::string &text) {
    ++store_saves;
    if (!store_ok) return false;
    stored_text = text;
    return true;
}
}  // namespace ui
}  // namespace dh

// Radio service simulated at its public boundary; LVGL input and widgets are real.
static dh::wifi_change::Status wifi_state;
static bool wifi_allowed = true;
static std::string wifi_sent_ssid, wifi_sent_password;
static int wifi_begins = 0, wifi_keeps = 0, wifi_reverts = 0;
static bool wifi_hotspot = false, wifi_begin_ok = true;
static int pairing_requests = 0;
namespace dh::wifi_change {
void init() {}
bool available() { return wifi_allowed; }
bool hotspot_open() { return wifi_hotspot; }
int signal_bars(int rssi) { return rssi >= 0 ? 0 : rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1; }
bool start_scan() { if (!wifi_allowed) return false; wifi_state.phase=Phase::Typing; wifi_state.busy=true; return true; }
std::vector<Network> networks() { return {{"Office",Security::Personal,-40,true},{"Guest",Security::Open,-60,false},{"Lab",Security::Personal,-62,false},{"CorpNet",Security::Unsupported,-70,false}}; }
bool begin(const std::string &ssid,const std::string &password,bool) { if (!wifi_begin_ok) return false; ++wifi_begins; wifi_sent_ssid=ssid; wifi_sent_password=password; wifi_state.phase=Phase::Testing; wifi_state.remaining_s=20; return true; }
bool keep() { ++wifi_keeps; wifi_state.phase=Phase::Saved; return true; }
bool revert() { ++wifi_reverts; wifi_state.phase=Phase::Reverted; wifi_state.reason=Reason::UserReverted; wifi_state.restored=true; return true; }
void cancel() { wifi_state={}; }
void touch() {}
Status status() { return wifi_state; }
}

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
static void click_nav(int index) {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    if (!detail::nav_area(index, &x1, &y1, &x2, &y2)) { tap(240, 470); return; }   // no such button: a tap where there is nothing
    tap((x1 + x2) / 2, (y1 + y2) / 2);
}
static int nav_index_of(const std::string &label) {
    for (int i = 0; i < detail::nav_count(); ++i) if (detail::nav_text(i) == label) return i;
    return -1;
}
static std::string nav_line() {
    std::string out;
    for (int i = 0; i < detail::nav_count(); ++i) out += (i ? "," : "") + detail::nav_text(i);
    return out;
}
static bool tap_area(bool (*area)(int, int, int *, int *, int *, int *), int a, int b) {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    if (!area(a, b, &x1, &y1, &x2, &y2)) return false;
    tap((x1 + x2) / 2, (y1 + y2) / 2);
    step();
    return true;
}
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
    wifi_state={}; wifi_allowed=true; wifi_begins=wifi_keeps=wifi_reverts=0; wifi_sent_ssid.clear(); wifi_sent_password.clear();
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
    submit_result = true; stored_text.clear(); store_ok = true; store_saves = 0; stored_sources.clear(); sources_ok = true; sources_saves = 0; mock_reset_result = ESP_OK;
    audio_available = true; audio_enabled = false; audio_store_ok = true; audio_tests = audio_paired = audio_volume = 0; audio_commands.clear(); audio_test_levels.clear();
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
    if (page == detail::Page::SourceEdit && detail::current_page() != detail::Page::SourceEdit) {   // the edit page is about a source: the first one
        detail::request_page(detail::Page::Actions); step();
        detail::request_source_edit(0);
    } else detail::request_page(page);
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
static const std::array<std::pair<Page, PowerTab>, 16> kViews = {{
    {Page::Home, PowerTab::Level}, {Page::Actions, PowerTab::Level}, {Page::Pages, PowerTab::Level}, {Page::Settings, PowerTab::Level},
    {Page::Power, PowerTab::Level}, {Page::Power, PowerTab::Readings}, {Page::Power, PowerTab::Protect}, {Page::Connect, PowerTab::Level},
    {Page::Appearance, PowerTab::Level}, {Page::Display, PowerTab::Level}, {Page::Clock, PowerTab::Level}, {Page::Device, PowerTab::Level}, {Page::Sound, PowerTab::Level}, {Page::Time, PowerTab::Level},
    {Page::Sources, PowerTab::Level}, {Page::SourceEdit, PowerTab::Level}}};
static const char *view_name(size_t i) {
    static const char *n[] = {"home", "actions", "pages", "settings", "power-level", "power-readings", "power-protect", "connect", "appearance", "display", "clock", "device", "sound", "time", "sources", "source-edit"};
    return n[i];
}

// ---------------------------------------------------------------- cases
static void case_pages_and_home() {
    fresh();
    EXPECT(detail::current_page() == Page::Actions);              // the home page by default is the switch page
    EXPECT(nav_line() == "切换,总览,设置");
    EXPECT(nav_index_of("电脑") < 0 && !has("电脑"));
    click_nav(1); EXPECT(detail::current_page() == Page::Home);
    click_nav(2); EXPECT(detail::current_page() == Page::Settings);
    EXPECT(click_text("页面与首页")); step();
    EXPECT(detail::current_page() == Page::Pages);
    check_layout("pages"); check_targets("pages");
    // show the clock: stored, and the navigation grows at once
    EXPECT(tap_area(detail::pages_control_area, 2, 0));
    EXPECT(stored_text.find("|S+O+C+P-|") != std::string::npos);
    EXPECT(nav_line() == "切换,总览,时钟,设置");
    EXPECT(detail::current_page() == Page::Pages);
    // move the overview up: it is the home page now
    EXPECT(tap_area(detail::pages_control_area, 1, 1));
    EXPECT(stored_text.find("|O+S+C+P-|") != std::string::npos);
    EXPECT(nav_line() == "总览,切换,时钟,设置");
    EXPECT(has("首页"));
    // hide down to one page; the last one cannot be hidden, and the page says so
    EXPECT(tap_area(detail::pages_control_area, 1, 0));            // hide Switch
    EXPECT(tap_area(detail::pages_control_area, 2, 0));            // hide Clock
    const std::string one = stored_text;
    EXPECT(one.find("|O+S-C-P-|") != std::string::npos && nav_line() == "总览,设置");
    EXPECT(tap_area(detail::pages_control_area, 0, 0));            // hide Overview: refused
    EXPECT(stored_text == one && nav_line() == "总览,设置" && has("至少留一个页面"));
    // the ends do nothing
    EXPECT(tap_area(detail::pages_control_area, 0, 1));
    EXPECT(tap_area(detail::pages_control_area, 3, 2));
    EXPECT(stored_text == one);
    // a failed save keeps the old order and says so
    store_ok = false;
    EXPECT(tap_area(detail::pages_control_area, 3, 0));            // show Power: not stored
    EXPECT(nav_line() == "总览,设置" && has("保存失败"));
    // a restart starts on the new home page
    store_ok = true;
    start_ui();
    EXPECT(detail::current_page() == Page::Home && nav_line() == "总览,设置");
    // with all four shown, the five buttons fit and every one reaches its page
    fresh("night|-|zh-CN|standard|pct|standard|P+C+O+S+|list");
    EXPECT(detail::current_page() == Page::Power && nav_line() == "电源,时钟,总览,切换,设置");
    const Page pages[5] = {Page::Power, Page::Clock, Page::Home, Page::Actions, Page::Settings};
    for (int i = 0; i < 5; ++i) { click_nav(i); NOTE(detail::current_page() == pages[i], "button " << i); }
    check_layout("nav-five"); check_targets("nav-five");
}
static void case_wake_goes_home() {
    fresh();
    click_nav(1); EXPECT(detail::current_page() == Page::Home);
    step(61000);                                                  // idle: the clock page
    EXPECT(detail::current_page() == Page::Clock);
    tap(240, 200); step();                                        // a touch on the clock page: back to the home page
    EXPECT(detail::current_page() == Page::Actions);
    step(61000);
    EXPECT(detail::current_page() == Page::Clock);
    click_nav(detail::nav_count() - 1); step();                   // a navigation button wins
    EXPECT(detail::current_page() == Page::Settings);
}
static void with_ports() {
    mock_snapshot.actions[0] = {"to-mini", "Mac mini", "u2720", "mac-dp", 1};
    mock_snapshot.actions[1] = {"to-m1", "MacBook M1", "u2720", "m1-hdmi", 1};
    mock_snapshot.actions[2] = {"to-m4", "MacBook M4", "u2720", "m4-usbc", 1};
    mock_snapshot.action_count = 3;
    step(); step();
}
static void case_switch_styles() {
    fresh();
    with_ports();
    EXPECT(detail::current_page() == Page::Actions);
    EXPECT(detail::switch_row_count() == 3 && detail::switch_tile_count() == 0);
    EXPECT(has("Mac mini") && has("HDMI") && has("USB-C") && has("DisplayPort"));
    EXPECT(detail::switch_icon(0) == static_cast<int>(ui::SourceIcon::Dp) && detail::switch_icon(2) == static_cast<int>(ui::SourceIcon::UsbC));
    EXPECT(click_text("MacBook M1"));
    EXPECT(submit_calls == 1 && dh::last_submit_id == "to-m1");
    check_layout("switch-list"); check_targets("switch-list");
    // the style is chosen in Settings > Input sources
    goto_view(Page::Sources);
    check_layout("sources"); check_targets("sources");
    EXPECT(click_text("图标")); step();
    EXPECT(stored_text.find("|icons") != std::string::npos);
    click_nav(0); step();
    EXPECT(detail::current_page() == Page::Actions);
    EXPECT(detail::switch_tile_count() == 3 && detail::switch_row_count() == 0);
    EXPECT(click_text("MacBook M4"));
    EXPECT(submit_calls == 2 && dh::last_submit_id == "to-m4");
    check_layout("switch-icons"); check_targets("switch-icons");
    // offline: nothing runs
    mock_snapshot.agent_ready = false; step(); step();
    click_text("Mac mini");
    EXPECT(submit_calls == 2);
    // the style survives a restart
    const std::string saved = stored_text;
    start_ui(); with_ports();
    EXPECT(detail::switch_tile_count() == 3);
    // more sources than fit: two columns, and the page scrolls
    for (size_t i = 3; i < 7; ++i) mock_snapshot.actions[i] = {"to-" + std::to_string(i), "PC " + std::to_string(i), "u2720", "hdmi" + std::to_string(i), 1};
    mock_snapshot.action_count = 7; step(); step();
    EXPECT(detail::switch_tile_count() == 7);
    check_layout("switch-icons-many"); check_targets("switch-icons-many");
}
static void case_source_rename() {
    fresh();
    with_ports();
    goto_view(Page::Sources);
    EXPECT(click_text("MacBook M1")); step();
    EXPECT(detail::current_page() == Page::SourceEdit);
    check_layout("source-edit"); check_targets("source-edit");
    // back from the keyboard stores nothing
    EXPECT(click_text("名字")); step();
    EXPECT(detail::rename_open());
    EXPECT(has("空格"));
    for (const char *c : {"x", "y"}) EXPECT(click_text(c));
    detail::rename_back(); step();
    EXPECT(!detail::rename_open() && stored_sources.empty() && sources_saves == 0);
    // type a new name; Done stores it
    EXPECT(click_text("名字")); step();
    for (int i = 0; i < 12; ++i) detail::rename_erase();   // the App name is there to start with
    for (const char *c : {"o", "f", "f", "i", "c", "e"}) EXPECT(click_text(c));
    check_layout("rename-keyboard"); check_targets("rename-keyboard");
    EXPECT(click_text("完成")); step();
    EXPECT(!detail::rename_open() && stored_sources.find("office") != std::string::npos);
    EXPECT(detail::current_page() == Page::SourceEdit && has("office"));
    // an icon
    EXPECT(tap_area(detail::icon_choice_area, static_cast<int>(ui::SourceIcon::Laptop), 0));
    EXPECT(stored_sources.find("laptop") != std::string::npos);
    click_nav(0); step();
    EXPECT(has("office") && !has("MacBook M1") && detail::switch_icon(1) == static_cast<int>(ui::SourceIcon::Laptop));
    // it survives a restart
    start_ui(); with_ports();
    EXPECT(has("office"));
    // a failed save keeps the old name and says so
    goto_view(Page::Sources);
    EXPECT(click_text("office")); step();
    sources_ok = false;
    EXPECT(tap_area(detail::icon_choice_area, static_cast<int>(ui::SourceIcon::Tv), 0));
    EXPECT(has("保存失败") && stored_sources.find("laptop") != std::string::npos);
    click_nav(0); step();
    EXPECT(detail::switch_icon(1) == static_cast<int>(ui::SourceIcon::Laptop));   // the screen keeps what is stored, too
    goto_view(Page::Sources);
    EXPECT(click_text("office")); step();
    // restore: the App's name and icon are back
    sources_ok = true;
    EXPECT(click_text("恢复 App 的名字和图标")); step();
    EXPECT(stored_sources.find("to-m1") == std::string::npos);
    click_nav(0); step();
    EXPECT(has("MacBook M1") && detail::switch_icon(1) == static_cast<int>(ui::SourceIcon::Hdmi));
    // factory reset forgets the names too
    goto_view(Page::Sources); EXPECT(click_text("MacBook M1")); step();
    EXPECT(click_text("名字")); step();
    EXPECT(click_text("z")); EXPECT(click_text("完成")); step();
    EXPECT(!stored_sources.empty());
    goto_view(Page::Device);
    EXPECT(click_text("恢复出厂设置")); step(); EXPECT(click_text("再次点击确认删除")); step();
    EXPECT(stored_sources.empty());
    goto_view(Page::Actions);
    EXPECT(has("MacBook M1") && !has("z"));                  // the App's name is back on the screen, not only in the store
}
static void case_themes() {
    fresh();
    EXPECT(detail::current_appearance().theme == ui::ThemeId::Night);
    EXPECT(lv_color_eq(lv_obj_get_style_bg_color(detail::screen_object(), LV_PART_MAIN), lv_color_hex(0x000000)));
    goto_view(Page::Appearance);
    EXPECT(has("夜航") && has("薄荷夜色") && has("深海蓝") && has("暖橙"));
    EXPECT(click_text("薄荷夜色"));
    EXPECT(detail::current_appearance().theme == ui::ThemeId::Mint && stored_text.rfind("mint|", 0) == 0);
    EXPECT(lv_color_eq(lv_obj_get_style_bg_color(detail::screen_object(), LV_PART_MAIN), lv_color_hex(0x03100e)));
    EXPECT(click_text("夜航"));
    EXPECT(lv_color_eq(lv_obj_get_style_bg_color(detail::screen_object(), LV_PART_MAIN), lv_color_hex(0x000000)));
    // what 0.3.x stored: the night theme with the old colour
    fresh("amber|-|zh-CN|standard|pct|standard");
    EXPECT(detail::current_appearance().theme == ui::ThemeId::Night && detail::look.palette.accent == 0xff9e4a);
}
static void case_clock_page() {
    fresh();
    goto_view(Page::Clock);
    for (int i = 0; i < 4; ++i) NOTE(detail::clock_dot_lit(i), "dot " << i);
    EXPECT(has("11:00") || contains(":"));
    mock_snapshot.agent_ready = false; step();
    EXPECT(detail::clock_dot_lit(0) && !detail::clock_dot_lit(1) && detail::clock_dot_lit(2) && detail::clock_dot_lit(3));
    mock_snapshot.wifi_ready = false; mock_snapshot.wifi = "未配置"; step();
    EXPECT(!detail::clock_dot_lit(0));
    power_auto = false; mock_power.observed_ms = mock_ms - 100; step(7000);
    EXPECT(!detail::clock_dot_lit(2));
    mock_snapshot.time_valid = false; step();
    EXPECT(!detail::clock_dot_lit(3) && has("--:--"));
    for (int i = -1; i <= 4; i += 5) EXPECT(!detail::clock_dot_lit(i));
    goto_view(Page::Home);
    EXPECT(!detail::clock_dot_lit(0));                       // no dots on another page
}
static void case_no_metrics() {
    fresh();
    for (size_t i = 0; i < kViews.size(); ++i) {
        goto_view(kViews[i].first, kViews[i].second);
        NOTE(!contains("CPU") && !has("内存") && !has("23%") && !has("48%"), view_name(i));
        NOTE(!mock_snapshot.metrics_visible, view_name(i));
    }
}
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
    for (const char *theme : {"night", "mint", "ocean", "amber"})
        for (const char *lang : {"zh-CN", "en-US"})
            for (const char *size : {"small", "standard", "large"}) {
                fresh(std::string(theme) + "|-|" + lang + "|" + size + "|pct|standard|S+O+C-P-|list");
                with_ports();
                if (std::string(lang) == "en-US") {  // action names come from the user's Agent; the English check is for our own text
                    mock_snapshot.actions[0].label = "To Mac"; mock_snapshot.actions[1].label = "To PC"; mock_snapshot.actions[2].label = "To M4"; step();
                }
                for (size_t i = 0; i < kViews.size(); ++i) {
                    goto_view(kViews[i].first, kViews[i].second);
                    const std::string where = std::string(theme) + "/" + lang + "/" + size + "/" + view_name(i);
                    check_layout(where);
                    check_targets(where);
                    ++views;
                }
                {   // the switch page as tiles, and the navigation with all five buttons
                    detail::request_switch_style(ui::SwitchStyle::Icons); step(); step();
                    goto_view(Page::Actions);
                    const std::string where = std::string(theme) + "/" + lang + "/" + size + "/switch-icons";
                    check_layout(where);
                    check_targets(where);
                    fresh(std::string(theme) + "|-|" + lang + "|" + size + "|pct|standard|P+C+O+S+|icons");
                    with_ports();
                    for (Page pg : {Page::Power, Page::Clock, Page::Home, Page::Actions, Page::Settings}) {
                        goto_view(pg);
                        check_layout(where + "/nav-five");
                        check_targets(where + "/nav-five");
                    }
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
    EXPECT(views == 4 * 2 * 3 * 16);
}
static void case_state_fidelity() {
    using detail::header_battery_text;
    using detail::header_bolt_shown;
    using detail::header_slashed;
    using detail::header_usb_shown;
    fresh();
    goto_view(Page::Home);
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
    goto_view(Page::Home);
    power_auto = false; mock_power.observed_ms = mock_ms - 100; step(7000);
    EXPECT(header_battery_text() == "?" && detail::header_fill_width() == 0 && !header_usb_shown() && !header_bolt_shown() && !detail::ring_lit(2));
    // fault
    fresh();
    goto_view(Page::Home);
    mock_power.fault = true; step();
    EXPECT(header_battery_text() == "!" && !header_usb_shown() && !header_bolt_shown() && !detail::ring_lit(2));
    // channels: not configured and offline are slashed
    fresh();
    goto_view(Page::Home);
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
    // power level gauge
    fresh();
    goto_view(Page::Power);
    auto a = arcs();
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
    // offline: the switch page says why nothing can be switched
    fresh();
    mock_snapshot.agent_ready = false; mock_snapshot.metrics_expires_ms = mock_ms - 1; step(); step();
    EXPECT(!has("指标已过期 · 等待刷新") && contains("Agent 未连接"));
    for (auto *t : touch_targets()) { (void)t; }
}
static void case_navigation() {
    fresh();
    EXPECT(detail::current_page() == Page::Actions);
    click_nav(1); EXPECT(detail::current_page() == Page::Home);
    click_nav(2); EXPECT(detail::current_page() == Page::Settings);
    click_nav(0); EXPECT(detail::current_page() == Page::Actions);
    tap(100, 30); EXPECT(detail::current_page() == Page::Connect);       // header left
    tap(380, 30); EXPECT(detail::current_page() == Page::Power);         // header right
    click_nav(2);
    struct Row { const char *label; Page page; };
    for (Row r : {Row{"页面与首页", Page::Pages}, Row{"输入源", Page::Sources}, Row{"显示与亮度", Page::Display}, Row{"外观", Page::Appearance}, Row{"连接与配对", Page::Connect}, Row{"电池与电源", Page::Power}, Row{"声音", Page::Sound}, Row{"日期与时间", Page::Time}, Row{"桌面时钟", Page::Clock}, Row{"设备信息", Page::Device}}) {
        click_nav(2);
        EXPECT(detail::current_page() == Page::Settings);
        EXPECT(click_text(r.label));
        NOTE(detail::current_page() == r.page, "row " << r.label);
    }
    // tap the clock in the ring
    click_nav(1); tap(240, 210); EXPECT(detail::current_page() == Page::Clock);
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
    click_nav(1);                                   // the first touch only wakes: the home page, not the button
    EXPECT(detail::current_page() == Page::Actions);
    click_nav(1);
    EXPECT(detail::current_page() == Page::Home);
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
    EXPECT(has("还没有输入源"));
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
    EXPECT(stored_text == "ocean|-|zh-CN|standard|pct|standard|S+O+C-P-|list");
    EXPECT(detail::current_appearance().theme == dh::ui::ThemeId::Ocean);
    EXPECT(lv_color_eq(lv_obj_get_style_bg_color(detail::screen_object(), LV_PART_MAIN), lv_color_hex(0x03131c)));
    EXPECT(click_text("English"));
    EXPECT(stored_text == "ocean|-|en-US|standard|pct|standard|S+O+C-P-|list");
    EXPECT(has("Appearance") && has("Switch") && has("Status") && has("Settings") && !contains("外观"));
    EXPECT(click_text("Large"));
    EXPECT(detail::look.body == &btr_ui_28 && stored_text == "ocean|-|en-US|large|pct|standard|S+O+C-P-|list");
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
    EXPECT(detail::current_appearance().theme == dh::ui::ThemeId::Night && stored_text.empty() && has("保存失败"));
    EXPECT(lv_color_eq(lv_obj_get_style_bg_color(detail::screen_object(), LV_PART_MAIN), lv_color_hex(0x000000)));
    // corrupt storage: the default look
    for (const char *bad : {"garbage", "mint|zzzz|zh-CN|standard", "mint|-|fr|standard", "|||"}) {
        fresh(bad);
        EXPECT(detail::current_appearance().theme == dh::ui::ThemeId::Night && detail::current_appearance().lang == dh::ui::Lang::ZhCN && detail::look.body == &btr_ui_20);
    }
    // an unreadable accent in storage is not applied
    fresh("mint|0e2a24|zh-CN|standard");
    EXPECT(detail::look.palette.accent == 0x2fd8ff);   // 0.3.x text: the night theme, and its own accent
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
    EXPECT(stored_text == "night|4fc3f7|zh-CN|standard|pct|standard|S+O+C-P-|list" && detail::look.palette.accent == 0x4fc3f7);
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
    // F1 to F4: a factory reset also forgets the sound and the appearance, which live apart from the configuration.
    // F1 the sound is off and low again; F2 the appearance is the default on the screen and in the store;
    // F3 a configuration that could not be erased leaves both as they were; F4 a sound or appearance that could not be
    // erased is said, and the rest is still erased.
    fresh("ocean|4fc3f7|en-US|large|pct|thick"); audio_enabled = true; audio_volume = 2; goto_view(Page::Device);
    EXPECT(click_text("Factory reset")); EXPECT(click_text("Tap again to erase")); step();
    EXPECT(reset_calls == 1 && !audio_enabled && audio_volume == 0 && stored_text.empty());
    EXPECT(ui::serialize(detail::current_appearance()) == ui::serialize(dh::ui::Appearance{}));
    EXPECT(contains("已删除本机配置"));                       // the page is in Chinese again, the default
    fresh("ocean|4fc3f7|zh-CN|standard|pct|standard"); audio_enabled = true; audio_volume = 1; mock_reset_result = ESP_FAIL; goto_view(Page::Device);
    click_text("恢复出厂设置"); click_text("再次点击确认删除"); step();
    EXPECT(has("配置删除失败") && audio_enabled && audio_volume == 1 && stored_text == "ocean|4fc3f7|zh-CN|standard|pct|standard");
    fresh("ocean|4fc3f7|zh-CN|standard|pct|standard"); audio_enabled = true; audio_store_ok = false; goto_view(Page::Device);
    click_text("恢复出厂设置"); click_text("再次点击确认删除"); step();
    EXPECT(has("配置已删除，但声音或外观没有清除，请再试一次") && stored_text.empty());
    fresh("ocean|4fc3f7|zh-CN|standard|pct|standard"); store_ok = false; audio_enabled = true; goto_view(Page::Device);
    click_text("恢复出厂设置"); click_text("再次点击确认删除"); step();
    EXPECT(has("配置已删除，但声音或外观没有清除，请再试一次") && !audio_enabled);
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
    detail::request_page(Page::Actions); step(0); step(0);   // the home page, where it started
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
    EXPECT(!has("总览") && !has("切换"));           // the shell is hidden
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
    EXPECT(detail::current_page() == Page::Actions);
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
    EXPECT(stored_text == "night|-|zh-CN|standard|nopct|standard|S+O+C-P-|list" && detail::header_battery_text().empty() && detail::current_appearance().show_percent == false);
    EXPECT(click_text("显示"));
    step(); step();
    EXPECT(stored_text == "night|-|zh-CN|standard|pct|standard|S+O+C-P-|list" && detail::header_battery_text() == "67");
    EXPECT(click_text("粗"));
    step(); step();
    EXPECT(stored_text == "night|-|zh-CN|standard|pct|thick|S+O+C-P-|list" && chosen("粗") && !chosen("细"));
    fresh("mint|-|zh-CN|standard|nopct|thin");      // what is stored is what the page shows as chosen
    goto_view(Page::Appearance);
    EXPECT(chosen("隐藏") && !chosen("显示") && chosen("细") && !chosen("粗"));
    // the rings follow the tier, on the Home page and on the power page; a restart reads the stored tier back
    struct Tier { const char *name; int home, gauge; };
    for (const Tier &t : {Tier{"thin", 14, 7}, Tier{"standard", 23, 11}, Tier{"thick", 33, 15}}) {
        fresh(std::string("mint|-|zh-CN|standard|pct|") + t.name);
        goto_view(Page::Home);
        EXPECT(detail::home_ring_width() == t.home);
        goto_view(Page::Power, PowerTab::Level);
        EXPECT(detail::power_gauge_width() == t.gauge);
    }
    fresh("mint|-|zh-CN|standard");                 // what 0.3.x stored
    goto_view(Page::Home);
    EXPECT(detail::home_ring_width() == 23 && detail::header_battery_text() == "67");
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
static void case_bolt_in_battery() {   // A26
    struct Case { int soc; bool percent; };
    const Case cases[] = {{5, true}, {67, true}, {100, true}, {67, false}};   // 1 digit, 2, 3, and no digits at all
    for (const Case &c : cases) {
        fresh(c.percent ? "mint|-|zh-CN|standard|pct|standard" : "mint|-|zh-CN|standard|nopct|standard");
        mock_power.soc_percent = c.soc; mock_power.charging = "charging";
        step(); step();
        for (int i = 0; i < 3; ++i) { lv_obj_invalidate(lv_screen_active()); lv_refr_now(display); }
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0, bx1 = 0, by1 = 0, bx2 = 0, by2 = 0, ux1 = 0, uy1 = 0, ux2 = 0, uy2 = 0;
        EXPECT(detail::header_battery_body(&x1, &y1, &x2, &y2));
        EXPECT(detail::header_bolt_shown() && detail::header_bolt_area(&bx1, &by1, &bx2, &by2));
        NOTE(bx1 >= x1 && bx2 <= x2 && by1 >= y1 && by2 <= y2,
             "the bolt is not inside the battery for level " << c.soc << ": bolt [" << bx1 << "," << by1 << "," << bx2 << "," << by2 << "] body [" << x1 << "," << y1 << "," << x2 << "," << y2 << "]");
        // The USB icon is next to the battery: no empty slot of the old bolt is left between them.
        EXPECT(detail::header_usb_area(&ux1, &uy1, &ux2, &uy2));
        NOTE(ux2 < x1 && x1 - ux2 <= 20, "the USB icon is not next to the battery for level " << c.soc << ": usb ends at " << ux2 << ", battery starts at " << x1);
        std::vector<std::pair<int, int>> runs;   // the columns that have ink, as runs
        int min_x = 9999, max_x = -1, min_y = 9999, max_y = -1;
        bool in_run = false;
        for (int x = x1 + 3; x <= x2 - 3; ++x) {
            bool ink = false;
            for (int y = y1 + 3; y <= y2 - 3; ++y) {
                const size_t at = (static_cast<size_t>(y) * 480 + static_cast<size_t>(x)) * 3;
                if ((framebuffer[at] + framebuffer[at + 1] + framebuffer[at + 2]) / 3 > 150) {
                    ink = true;
                    min_x = std::min(min_x, x); max_x = std::max(max_x, x);
                    min_y = std::min(min_y, y); max_y = std::max(max_y, y);
                }
            }
            if (ink && !in_run) runs.push_back({x, x});
            if (ink) runs.back().second = x;
            in_run = ink;
        }
        EXPECT(max_y >= 0 && !runs.empty());
        if (runs.empty()) continue;   // nothing drawn: the check above has said so
        const double dy = (min_y + max_y) / 2.0 - (y1 + y2) / 2.0, dx = (min_x + max_x) / 2.0 - (x1 + x2) / 2.0;
        NOTE(std::fabs(dy) <= 1.0 && std::fabs(dx) <= 1.5,
             "bolt and digits are off centre for level " << c.soc << ": ink [" << min_x << "," << min_y << "," << max_x << "," << max_y << "] in body [" << x1 << "," << y1 << "," << x2 << "," << y2 << "] dx " << dx << " dy " << dy);
        const int bolt_w = runs.front().second - runs.front().first + 1;
        NOTE(bolt_w >= 5 && bolt_w <= 12, "the bolt is " << bolt_w << " px wide for level " << c.soc);
        if (c.percent) NOTE(runs.size() >= 2 && runs[1].first >= runs[0].second + 2, "bolt and digits touch for level " << c.soc);
        else NOTE(runs.size() == 1, "something besides the bolt is drawn with the percent off, " << runs.size() << " runs");
    }
    fresh();
    mock_power.charging = "idle"; step();
    EXPECT(!detail::header_bolt_shown() && detail::header_usb_shown());
    mock_power.charging = "charging"; step();
    EXPECT(detail::header_bolt_shown());
    mock_power.vbus = "absent"; mock_power.charging = "idle"; step();
    EXPECT(!detail::header_bolt_shown());
}
static void case_home_without_actions() {   // A27
    for (const size_t count : {size_t{2}, size_t{0}}) {
        fresh(); mock_snapshot.action_count = count; goto_view(Page::Home); step();
        int rx1 = 0, ry1 = 0, rx2 = 0, ry2 = 0, nx1 = 0, ny1 = 0, nx2 = 0, ny2 = 0;
        EXPECT(detail::home_ring_area(&rx1, &ry1, &rx2, &ry2) && detail::home_notice_area(&nx1, &ny1, &nx2, &ny2));
        const int d = rx2 - rx1 + 1, body_end = detail::kBodyTop + detail::kBodyH;
        NOTE(d == 252, "the ring is " << d << " px wide, not 252");
        EXPECT(std::abs((rx1 + rx2) / 2 - 240) <= 1);
        const int top = ry1 - detail::kBodyTop, bottom = body_end - ny2;
        NOTE(ny1 >= ry2 + 4 && ny2 <= body_end, "the line is not under the ring and inside the body: ring ends " << ry2 << ", line [" << ny1 << "," << ny2 << "]");
        NOTE(top >= 8 && bottom >= 8 && std::abs(top - bottom) <= 6, "a blank band is left: " << top << " px over the ring and " << bottom << " px under the line");
        EXPECT(detail::home_first_action_label().empty());                        // no cards on the overview
        EXPECT(detail::home_ring_width() == 23);                                  // the thickness grows with the ring: 20 of 216 is 23 of 252
        check_layout("overview"); save_png_source(count ? "state-overview" : "state-overview-no-actions");
        const int cy = (ry1 + ry2) / 2;
        tap(240, cy - 10); step();                                                // the clock in the middle
        EXPECT(detail::current_page() == Page::Clock);
        fresh(); mock_snapshot.action_count = count; goto_view(Page::Home); step();
        tap(240, cy + 50); step();                                                // the lower end of the place of the date
        EXPECT(detail::current_page() == Page::Clock);
        fresh(); mock_snapshot.action_count = count; mock_snapshot.agent_ready = false; mock_snapshot.agent = "未配对"; goto_view(Page::Home); step();
        int dx1 = 0, dy1 = 0, dx2 = 0, dy2 = 0;
        EXPECT(detail::home_notice_area(&nx1, &ny1, &nx2, &ny2) && detail::home_notice_dot_area(&dx1, &dy1, &dx2, &dy2));
        NOTE(dy1 >= ny1 && dy2 <= ny2 + 1, "the dot [" << dy1 << "," << dy2 << "] is not on the line [" << ny1 << "," << ny2 << "]");
        fresh(); mock_snapshot.action_count = count; goto_view(Page::Home); step();
        tap(rx1 + 10, cy); step();                                                // on the ring itself
        EXPECT(detail::ring_info_open());
        EXPECT(click_text("关闭")); step(); EXPECT(!detail::ring_info_open());
        tap(rx2 - 8, cy); step();                                                 // the far rim is part of the touch area
        EXPECT(detail::ring_info_open());
        EXPECT(click_text("关闭")); step(); EXPECT(!detail::ring_info_open());
    }
}
static void case_pairing_keeps_awake() {   // A22
    fresh();
    dh::PairingInfo info;
    info.code = "123456"; info.hotspot = "DeskHub-C598"; info.hotspot_password = "abcdefghjkmn"; info.remaining_s = 300; info.total_s = 300;
    dh::ui_pairing_show(info, [] {});
    step(); step();
    for (int minute = 0; minute < 6; ++minute) step(60000);   // no touch for six minutes: it would dim after 2 and be off after 5
    EXPECT(mock_brightness == mock_config.brightness);
    dh::ui_pairing_hide(); step();
    EXPECT(mock_brightness == mock_config.brightness);        // the idle time starts over when the page goes
    step(61000); step(60000);                                  // and then the usual rules apply again
    EXPECT(mock_brightness < mock_config.brightness);
}
static void case_no_setup_card() {   // A23
    for (const bool agent : {true, false}) {
        fresh();
        mock_snapshot.action_count = 0; mock_snapshot.agent_ready = agent; step(); step();
        goto_view(Page::Home); step();
        EXPECT(!has("设置网络与配对") && !has("首次设置") && !has("还没有输入源"));   // the overview: nothing about the sources
        goto_view(Page::Actions); step();
        EXPECT(!has("设置网络与配对") && !has("首次设置"));
        EXPECT(has("还没有输入源") && has("App 绑定显示器后，输入源会显示在这里。"));
        check_layout("no-actions"); check_targets("no-actions");
    }
}
static void case_ring_info() {   // A24
    fresh(); goto_view(Page::Home); step();
    EXPECT(has("点圆环看说明") && !detail::ring_info_open());
    tap(240, 210); EXPECT(detail::current_page() == Page::Clock);              // the clock in the middle is unchanged
    fresh(); goto_view(Page::Home); step();
    EXPECT(!detail::notice_dot_shown());                                         // the hint has no dot
    tap(142, 170); step();                                                       // on the ring, outside the clock
    EXPECT(detail::ring_info_open() && detail::ring_info_dot_filled(0) && detail::ring_info_dot_filled(1) && detail::ring_info_dot_filled(2) && detail::ring_info_dot_filled(3));
    EXPECT(has("状态环") && has("网络") && has("电脑") && has("电源") && has("时间") && has("关闭"));
    EXPECT(has("设备连上了 Wi-Fi") && has("设备能和电脑上的 Agent 通话") && has("读数是新的，没有故障") && has("时间已校准"));
    check_layout("ring-info"); save_png_source("state-ring-info");
    EXPECT(click_text("关闭")); step();
    EXPECT(!detail::ring_info_open() && !has("状态环"));
    tap(142, 170); step(); EXPECT(detail::ring_info_open());
    tap(240, 90); step(); EXPECT(detail::ring_info_open());                      // a tap on the sheet itself does not close it
    tap(240, 470); step(); EXPECT(!detail::ring_info_open());                    // a tap outside the sheet closes it
    // A dark segment is named under the ring and explained in the sheet.
    fresh(); mock_snapshot.agent_ready = false; mock_snapshot.agent = "未配对"; goto_view(Page::Home); step();
    EXPECT(has("电脑没连上") && !has("点圆环看说明") && detail::notice_dot_shown());   // the dot is the colour of the dark segment
    tap(142, 170); step();
    EXPECT(detail::ring_info_open() && has("没连上") && has("未配对") && has("已连接"));
    {   // the rows are in the order of the ring, each with its own state, and its dot is filled only when its segment is lit
        auto y = [](const std::string &t) {   // the highest label with this text: the navigation bar has a "电脑" of its own, lower down
            int best = 100000;
            std::function<void(lv_obj_t *)> walk = [&](lv_obj_t *o) {
                if (lv_obj_check_type(o, &lv_label_class) && t == lv_label_get_text(o)) { lv_area_t a{}; lv_obj_get_coords(o, &a); best = std::min<int>(best, a.y1); }
                for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) walk(lv_obj_get_child(o, i));
            };
            walk(detail::screen_object());
            return best;
        };
        EXPECT(y("网络") < y("电脑") && y("电脑") < y("电源") && y("电源") < y("时间"));
        EXPECT(std::abs(y("已连接") - y("网络")) < 6 && std::abs(y("没连上") - y("电脑")) < 6 && std::abs(y("正常") - y("电源")) < 6 && std::abs(y("有效") - y("时间")) < 6);
        EXPECT(y("设备连上了 Wi-Fi") > y("网络") && y("设备连上了 Wi-Fi") < y("电脑") && y("未配对") > y("电脑") && y("未配对") < y("电源"));
        EXPECT(detail::ring_info_dot_filled(0) && !detail::ring_info_dot_filled(1) && detail::ring_info_dot_filled(2) && detail::ring_info_dot_filled(3));
    }
    check_layout("ring-info-problem"); save_png_source("state-ring-info-problem");
    EXPECT(click_text("关闭")); step();
    // Another note has priority over the line under the ring.
    mock_snapshot.command = "已接收，等待执行"; step();
    EXPECT(has("已接收，等待执行") && !has("电脑没连上") && !detail::notice_dot_shown());   // another note has no dot
    // Wi-Fi down names the network, and the computer row says it needs Wi-Fi first.
    fresh(); mock_snapshot.wifi_ready = false; mock_snapshot.agent_ready = false; mock_snapshot.wifi = "未配置"; goto_view(Page::Home); step();
    EXPECT(has("网络没连上"));
    tap(142, 170); step();
    EXPECT(has("未配置网络") && has("要先连上 Wi-Fi"));
    EXPECT(click_text("关闭")); step();
    // English.
    fresh("mint|-|en-US|standard|pct|standard"); goto_view(Page::Home); step();
    EXPECT(has("Tap the ring for details"));
    tap(142, 170); step();
    EXPECT(has("Status ring") && has("Network") && has("Computer") && has("Close"));
    EXPECT(click_text("Close")); step();
    EXPECT(!detail::ring_info_open());
}
static void case_sound() {   // A25
    fresh(); step(); step();
    click_nav(2); EXPECT(detail::current_page() == Page::Settings);
    EXPECT(has("声音") && has("提示音、音量、试听"));
    EXPECT(click_text("声音")); step();
    EXPECT(detail::current_page() == Page::Sound);
    EXPECT(has("声音") && has("提示音") && has("关") && has("开") && has("试听") && has("音量") && has("低") && has("中") && has("高"));
    EXPECT(!has("音量固定在较低的一档。"));
    EXPECT(has("动作有结果、配对完成时响一声。夜间限亮的时间里不响。") && !has("没有检测到扬声器，试听不可用。") && !detail::sound_controls_dimmed());
    EXPECT(!audio_enabled && detail::sound_selected() == 0);                      // off until the person turns it on
    check_layout("sound");
    EXPECT(click_text("开")); step();
    EXPECT(audio_enabled && detail::sound_selected() == 1);
    EXPECT(click_text("关")); step();
    EXPECT(!audio_enabled && detail::sound_selected() == 0);
    EXPECT(click_text("试听")); EXPECT(audio_tests == 1);                        // listen works with the sound off
    EXPECT(click_text("开")); step(); EXPECT(click_text("试听")); EXPECT(audio_tests == 2 && audio_enabled);
    // The volume: low until chosen; a tap stores it and plays one tone at the new level, whatever the alert switch says.
    fresh(); goto_view(Page::Sound);
    EXPECT(audio_volume == 0 && detail::sound_volume_selected() == 0);
    EXPECT(click_text("中")); step();
    EXPECT(audio_volume == 1 && detail::sound_volume_selected() == 1 && audio_tests == 1 && audio_test_levels.back() == 1 && !audio_enabled);
    EXPECT(click_text("高")); step();
    EXPECT(audio_volume == 2 && detail::sound_volume_selected() == 2 && audio_tests == 2 && audio_test_levels.back() == 2);
    check_layout("sound-volume");
    audio_store_ok = false;
    EXPECT(click_text("低")); step();
    EXPECT(audio_volume == 2 && detail::sound_volume_selected() == 2 && has("没有保存，请再试一次") && audio_tests == 2);   // nothing stored, nothing played
    audio_store_ok = true;
    EXPECT(click_text("低")); step();
    EXPECT(audio_volume == 0 && detail::sound_volume_selected() == 0 && !has("没有保存，请再试一次") && audio_tests == 3 && audio_test_levels.back() == 0);
    fresh(); audio_volume = 2; goto_view(Page::Sound);
    EXPECT(detail::sound_volume_selected() == 2);
    fresh(); goto_view(Page::Settings); EXPECT(detail::sound_volume_selected() == -1);
    fresh(); goto_view(Page::Sound);
    // A choice that cannot be stored stays as it was, and the page says so.
    EXPECT(click_text("关")); step(); EXPECT(!audio_enabled);
    audio_store_ok = false;
    EXPECT(click_text("开")); step();
    EXPECT(!audio_enabled && has("没有保存，请再试一次") && detail::sound_selected() == 0);   // the page still shows what is true
    audio_store_ok = true;
    EXPECT(click_text("开")); step();
    EXPECT(audio_enabled && !has("没有保存，请再试一次") && detail::sound_selected() == 1);
    // A setting that was stored before is what the page shows when it is opened.
    fresh(); audio_enabled = true; goto_view(Page::Sound);
    EXPECT(detail::sound_selected() == 1);
    fresh(); goto_view(Page::Settings); EXPECT(detail::sound_selected() == -1);
    // No speaker: nothing to turn on, nothing to listen to, and the reason on the page.
    fresh(); audio_available = false; goto_view(Page::Sound);
    EXPECT(has("没有检测到扬声器，试听不可用。") && !has("动作有结果、配对完成时响一声。夜间限亮的时间里不响。") && detail::sound_controls_dimmed());
    EXPECT(click_text("开")); step(); EXPECT(!audio_enabled);
    EXPECT(click_text("试听")); EXPECT(audio_tests == 0);
    EXPECT(click_text("中")); step(); EXPECT(audio_volume == 0 && audio_tests == 0 && detail::sound_volume_selected() == 0);
    check_layout("sound-no-speaker");
    // Commands: the text goes to the speaker once per change.
    fresh(); step(); step();
    EXPECT(audio_commands.empty());
    mock_snapshot.command = "已切换（已确认）"; step(); step();
    EXPECT(audio_commands.size() == 1 && audio_commands[0] == "已切换（已确认）");
    step(); step(); EXPECT(audio_commands.size() == 1);                          // the same text again is not a new result
    mock_snapshot.command = "切换失败，请查看原因"; step();
    EXPECT(audio_commands.size() == 2 && audio_commands[1] == "切换失败，请查看原因");
    // The night flag follows the limit of the night. The harness clock is 03:00 UTC: "XXX3:30" makes it 23:30 local time, inside the
    // default window 22:00 to 07:00; "CST-8" makes it 11:00.
    fresh(); setenv("TZ", "XXX3:30", 1); tzset();
    mock_snapshot.command = "已切换（已确认）"; step(); step();
    EXPECT(audio_commands.size() == 1 && audio_commands[0] == "已切换（已确认）|quiet");
    fresh(); setenv("TZ", "CST-8", 1); tzset();
    mock_snapshot.command = "已切换（已确认）"; step(); step();
    EXPECT(audio_commands.size() == 1 && audio_commands[0] == "已切换（已确认）");
    mock_config.night_enabled = false; mock_config.revision++; setenv("TZ", "XXX3:30", 1); tzset();
    mock_snapshot.command = "切换失败，请查看原因"; step(); step();
    EXPECT(audio_commands.size() == 2 && audio_commands[1] == "切换失败，请查看原因");      // the night limit is off: no quiet hours
    // An unknown time counts as night while the limit is on, as it does for the display.
    fresh(); setenv("TZ", "CST-8", 1); tzset(); mock_snapshot.time_valid = false;
    mock_snapshot.command = "已切换（已确认）"; step(); step();
    EXPECT(audio_commands.size() == 1 && audio_commands[0] == "已切换（已确认）|quiet");
    // English.
    fresh("mint|-|en-US|standard|pct|standard"); goto_view(Page::Sound);
    EXPECT(has("Sound") && has("Alert sound") && has("Off") && has("On") && has("Listen") && has("Volume") && has("Low") && has("Medium") && has("High"));
}
// A26 failure cases written before implementation: real pointer press/release,
// secret masking on press-lost/sleep/exit; disabled completion; every ASCII page;
// unsupported AP, open network, manual SSID, backend outcomes and timeout.
//  A29 Date and time (docs/design Dev-Time): a row in Settings; the zone is saved at once and a failed save leaves it as it was and
//      says so; the five numbers change only on this page until "save time"; a field is chosen by a tap and the others stay; saving
//      turns the local time into UTC by the zone and writes the RTC; a refused time keeps the numbers and says so.
static void case_time() {
    fresh(); step(); step();
    click_nav(2); EXPECT(has("日期与时间") && has("时区、手动校时"));
    EXPECT(click_text("日期与时间")); step(); EXPECT(detail::current_page() == Page::Time);
    EXPECT(has("时区") && has("手动校时") && has("UTC+8") && has("保存时间") && has("-1") && has("+1"));
    check_layout("time"); check_targets("time");
    // The zone.
    EXPECT(mock_config.tz_minutes == 480);
    EXPECT(click_text(LV_SYMBOL_PLUS)); step();
    EXPECT(mock_config.tz_minutes == 525 && has("UTC+8:45") && has("已保存"));
    EXPECT(click_text(LV_SYMBOL_MINUS)); step(); EXPECT(mock_config.tz_minutes == 480 && has("UTC+8"));
    mock_save_result = ESP_FAIL;
    EXPECT(click_text(LV_SYMBOL_MINUS)); step();
    EXPECT(mock_config.tz_minutes == 480 && has("UTC+8") && has("保存失败"));
    mock_save_result = ESP_OK;
    for (int i = 0; i < 30; ++i) { EXPECT(click_text(LV_SYMBOL_PLUS)); step(); }
    EXPECT(mock_config.tz_minutes == 840 && has("UTC+14"));      // the end of the table
    // The numbers: from the start when the time is not known; the minute is chosen first.
    fresh(); mock_snapshot.time_valid = false; mock_time_set.clear(); mock_rtc_writes = 0; mock_time_ok = true; goto_view(Page::Time); step();
    EXPECT(detail::time_draft_text() == "2026-01-01 00:00" && detail::time_field_selected() == 4);
    EXPECT(click_text("+1")); step(); EXPECT(detail::time_draft_text() == "2026-01-01 00:01" && mock_time_set.empty() && mock_rtc_writes == 0);
    EXPECT(click_text("2026")); step(); EXPECT(detail::time_field_selected() == 0 && detail::time_draft_text() == "2026-01-01 00:01");
    EXPECT(click_text("+1")); step(); EXPECT(detail::time_draft_text() == "2027-01-01 00:01");
    EXPECT(click_text("-1")); step(); EXPECT(click_text("+1")); step(); EXPECT(detail::time_draft_text() == "2027-01-01 00:01");
    // A refused time keeps the numbers.
    mock_time_ok = false; EXPECT(click_text("保存时间")); step();
    EXPECT(has("没有保存，请再试一次") && detail::time_draft_text() == "2027-01-01 00:01" && mock_rtc_writes == 0);
    mock_time_ok = true; EXPECT(click_text("保存时间")); step();
    EXPECT(mock_time_set == "2026-12-31T16:01:00Z" && mock_rtc_writes == 1 && has("已校时") && !has("没有保存，请再试一次"));
    // Another page and back: the numbers start over from the time now.
    goto_view(Page::Settings); step(); EXPECT(detail::time_field_selected() == -1);
    goto_view(Page::Time); step(); EXPECT(detail::time_draft_text() == "2026-01-01 00:00" && detail::time_field_selected() == 4);
    // The source of the time says what it is; the hints do not send the person to USB only.
    fresh(); mock_snapshot.time_valid = true; mock_snapshot.time_quality = "manual"; goto_view(Page::Clock); step(); EXPECT(has("手动校时"));
    fresh(); mock_snapshot.time_valid = false; goto_view(Page::Clock); step(); EXPECT(has("时间无效，请到设置里校时"));
    // English.
    fresh("mint|-|en-US|standard|pct|standard"); goto_view(Page::Time); step();
    EXPECT(has("Date and time") && has("Time zone") && has("Set the time") && has("Save time"));
    check_layout("time-en");
    fresh("mint|-|en-US|large|pct|standard"); goto_view(Page::Time); step(); check_layout("time-en-large"); check_targets("time-en-large");
}
static void case_wifi_pages() {   // W17 to W22, the list, the line under the field, the Connect page
    const auto typing = [] { fresh(); wifi_state = {}; wifi_hotspot = false; wifi_allowed = true; wifi_begin_ok = true; wifi_begins = wifi_reverts = 0; goto_view(Page::Connect); };
    typing(); step();
    EXPECT(has("Office · 已连接") && has("更换") && !has("dh-0a1b2c") && has("信号") && has("配对时设备打开热点，Wi-Fi 会暂时断开。"));
    check_layout("wifi-connect");
    // Pairing starts from the screen: the backend is asked, once a tap; a paired device says it pairs again.
    dh::ui_set_pairing_request([] { ++pairing_requests; }); pairing_requests = 0;
    EXPECT(has("重新配对")); EXPECT(click_text("重新配对")); step(); EXPECT(pairing_requests == 1);
    mock_paired = false; goto_view(Page::Home); goto_view(Page::Connect); step();
    EXPECT(has("开始配对") && !has("重新配对") && has("未配对")); EXPECT(click_text("开始配对")); step(); EXPECT(pairing_requests == 2);
    dh::ui_set_pairing_request(nullptr); EXPECT(click_text("开始配对")); step(); EXPECT(pairing_requests == 2);   // no backend: nothing happens
    typing(); step();
    EXPECT(click_text("更换")); step();
    // The list: bars instead of numbers, one way back, and what cannot be found.
    EXPECT(has("Lab") && !has("-62 dBm") && !has("返回") && has("只显示 2.4 GHz 的 Wi-Fi") && has("Office") && has("当前") && has("开放"));
    save_png_source("wifi-list"); check_layout("wifi-list");
    // W21: refused by the session after it was asked: back to the list, and why.
    EXPECT(click_text("Guest")); step();
    wifi_state.phase = dh::wifi_change::Phase::Typing; wifi_state.reason = dh::wifi_change::Reason::InvalidInput; step();
    EXPECT(has("这个 Wi-Fi 的加密方式和你选的不一样") && has("Office"));
    // The line under the field, and the count.
    typing(); EXPECT(click_text("更换")); step(); EXPECT(click_text("Office")); step();
    EXPECT(has("8 到 63 个字符") && has("0/63"));
    for (int i = 0; i < 5; ++i) EXPECT(click_text("a"));
    step(); EXPECT(has("还差 3 个字符") && has("5/63"));
    for (int i = 0; i < 3; ++i) EXPECT(click_text("a"));
    step(); EXPECT(has("8 到 63 个字符") && has("8/63"));
    // W22: refused while the scan runs: the button says why instead of doing nothing.
    wifi_begin_ok = false; EXPECT(click_text("完成")); step();
    EXPECT(has("现在不能连接，请稍等") && has("空格") && wifi_begins == 0);
    wifi_begin_ok = true;
    for (int i = 0; i < 55; ++i) EXPECT(click_text("a"));
    step(); EXPECT(has("63/63") && has("8 到 63 个字符"));   // hex so far: one more is allowed
    EXPECT(click_text("a")); step(); EXPECT(has("64/64") && has("64 位十六进制密码"));
    typing(); EXPECT(click_text("更换")); step(); EXPECT(click_text("Office")); step();
    EXPECT(click_text("123")); step();
    for (int i = 0; i < 63; ++i) EXPECT(click_text("1"));
    EXPECT(click_text("-")); step(); EXPECT(has("63/63"));
    // The eye is beside the field, not on it; every key stays large enough with a full field.
    check_targets("wifi-keyboard-full");
    // W19: cancel starts going back and stays on the page until there is a result.
    typing(); EXPECT(click_text("更换")); step(); EXPECT(click_text("Guest")); step();
    EXPECT(has("正在连接") && has("取消"));
    EXPECT(click_text("取消")); step();
    EXPECT(wifi_reverts == 1 && detail::wifi_opened() && has("已改回原来的 Wi-Fi") && has("没有保存。已改回原来的 Wi-Fi。"));
    // W18: while it goes back there is nothing to press.
    wifi_state = {}; wifi_state.phase = dh::wifi_change::Phase::Asking; wifi_state.remaining_s = 60; step();
    EXPECT(has("连不上电脑") && has("仍然使用") && has("改回原来的") && has("还没有保存。\n倒计时结束后改回原来的 Wi-Fi"));
    wifi_state.restoring = true; wifi_state.reason = dh::wifi_change::Reason::UserReverted; wifi_state.remaining_s = 20; step();
    EXPECT(has("正在改回") && has("没有保存。正在连回原来的 Wi-Fi") && !has("仍然使用") && !has("改回原来的") && !has("取消"));
    // Only the going back changes (the 60 seconds ended): the page still has to change.
    wifi_state = {}; wifi_state.phase = dh::wifi_change::Phase::Asking; wifi_state.reason = dh::wifi_change::Reason::AgentUnreachable; wifi_state.remaining_s = 1; step();
    EXPECT(has("仍然使用"));
    wifi_state.restoring = true; wifi_state.remaining_s = 20; step();
    EXPECT(!has("仍然使用") && has("没有保存。正在连回原来的 Wi-Fi"));
    wifi_state.phase = dh::wifi_change::Phase::Testing; wifi_state.reason = dh::wifi_change::Reason::WrongPassword; step();
    EXPECT(has("密码错误") && has("没有保存。正在连回原来的 Wi-Fi") && !has("取消"));
    wifi_state = {}; wifi_state.phase = dh::wifi_change::Phase::Testing; step(); EXPECT(has("正在连接") && has("取消"));
    // W17: every end of the story says what happened and what is true now.
    const auto ended = [](dh::wifi_change::Reason reason, bool restored, bool previous) {
        wifi_state = {}; wifi_state.phase = dh::wifi_change::Phase::Reverted; wifi_state.reason = reason; wifi_state.restored = restored; wifi_state.had_previous = previous; step();
    };
    ended(dh::wifi_change::Reason::WrongPassword, false, false);
    EXPECT(has("密码错误") && has("没有保存。") && !has("原来的 Wi-Fi 也没连上") && !has("原来的 Wi-Fi 也没连上，设备会继续重试"));
    ended(dh::wifi_change::Reason::WrongPassword, false, true);
    EXPECT(has("密码错误") && has("没有保存。原来的 Wi-Fi 也没连上，设备会继续重试。"));
    ended(dh::wifi_change::Reason::WrongPassword, true, true);
    EXPECT(has("密码错误") && has("没有保存。已改回原来的 Wi-Fi。"));
    ended(dh::wifi_change::Reason::UserReverted, false, false);
    EXPECT(has("已取消") && !has("已改回原来的 Wi-Fi"));
    ended(dh::wifi_change::Reason::RestoreFailed, false, true);
    EXPECT(has("原来的 Wi-Fi 也没连上"));
    ended(dh::wifi_change::Reason::AgentUnreachable, true, true);
    EXPECT(has("连不上电脑") && !has("连接超时"));
    detail::wifi_close(); step();
    // W20: the Connect page says the true reason.
    typing(); wifi_allowed = false; wifi_hotspot = true; step(); EXPECT(has("配对热点打开时不能更换 Wi-Fi"));
    typing(); wifi_allowed = false; wifi_hotspot = false; step(); EXPECT(has("现在不能更换 Wi-Fi，请稍后再试") && !has("配对热点打开时不能更换 Wi-Fi"));
    wifi_allowed = true; wifi_state = {};
    // The Device page has no words from the code when nothing is going on.
    fresh(); goto_view(Page::Device); step();
    EXPECT(!contains("idle") && !contains("\nok"));
}
static void case_wifi_keyboard() {
    fresh(); goto_view(Page::Connect);
    EXPECT(click_text("更换")); step();
    EXPECT(has("Office") && has("CorpNet"));
    EXPECT(click_text("CorpNet")); step(); EXPECT(!has("空格"));
    EXPECT(click_text("Office")); step();
    EXPECT(has("空格") && has("a") && !has("A"));
    pointer(true,54,184); pointer(true,240,145); pointer(false,240,145); step();
    EXPECT(!has("*")); // dragging out cancels the character

    EXPECT(click_text("完成")); step(); EXPECT(wifi_begins==0);
    for (const char *c : {"a","b","c","d","e","f","g"}) EXPECT(click_text(c));
    EXPECT(click_text("完成")); step(); EXPECT(wifi_begins==0); // seven characters must still refuse submission
    EXPECT(click_text("h"));
    step(); EXPECT(has("********") && !has("abcdefgh"));
    pointer(true,415,94); step(); EXPECT(has("abcdefgh"));
    pointer(false,415,94); step(); EXPECT(has("********") && !has("abcdefgh"));
    pointer(true,415,94); step(); EXPECT(has("abcdefgh"));
    pointer(true,240,145); step(); // actual drag outside the eye, not a synthetic PRESS_LOST
    EXPECT(!has("abcdefgh")); pointer(false,415,94);
    pointer(true,415,94); step(); detail::user_screen_off(); step(); EXPECT(!has("abcdefgh"));
    pointer(false,415,94); tap(240,240); step(); // first touch wakes only
    EXPECT(has("********"));
    EXPECT(click_text("123")); step(); EXPECT(has("1/2"));
    EXPECT(click_text("1/2")); step(); EXPECT(has("2/2") && has("\\"));
    EXPECT(click_text("abc")); step();
    save_png_source("wifi-keyboard"); check_layout("wifi-keyboard"); check_targets("wifi-keyboard");
    EXPECT(click_text("完成")); step();
    EXPECT(wifi_begins==1 && wifi_sent_ssid=="Office" && wifi_sent_password=="abcdefgh");
    EXPECT(!has("abcdefgh") && !has("********"));
    wifi_state.phase=dh::wifi_change::Phase::Asking; wifi_state.remaining_s=60; step();
    EXPECT(has("仍然使用") && has("改回原来的")); save_png_source("wifi-asking");
    EXPECT(click_text("改回原来的")); step(); EXPECT(wifi_reverts==1);
    EXPECT(click_text("重新输入")); step(); EXPECT(!has("********"));
    detail::wifi_close(); step(); EXPECT(!has("空格"));
    EXPECT(click_text("更换")); step(); EXPECT(click_text("Guest")); step();
    EXPECT(wifi_sent_ssid=="Guest" && wifi_sent_password.empty());
    wifi_state.phase=dh::wifi_change::Phase::Reverted; wifi_state.restored=true; step();
    EXPECT(click_text("重试")); step(); EXPECT(has("Guest") && !has("空格"));
    EXPECT(click_text("Guest")); step(); EXPECT(wifi_sent_password.empty());

    wifi_state.phase=dh::wifi_change::Phase::Saved; step(); EXPECT(has("已连接")); save_png_source("wifi-saved");
    detail::wifi_close(); step(); EXPECT(click_text("更换")); step();
    EXPECT(click_text("其他（手动输入名称）")); step();
    EXPECT(click_text("a")); EXPECT(click_text("完成")); step();
    EXPECT(has("有密码") && has("没有密码"));
    EXPECT(click_text("有密码")); step();
    wifi_state.phase=dh::wifi_change::Phase::Idle; step(180001); EXPECT(!has("空格"));
    fresh(); goto_view(Page::Connect); EXPECT(click_text("更换")); step();
    wifi_state.phase=dh::wifi_change::Phase::Idle; wifi_state.reason=dh::wifi_change::Reason::ScanFailed; step();
    EXPECT(has("没有找到 Wi-Fi") && has("重扫"));
    EXPECT(click_text("重扫")); step(); EXPECT(has("Office"));
    wifi_state.phase=dh::wifi_change::Phase::Scanning; step();
    EXPECT(click_text("其他（手动输入名称）")); step();
    EXPECT(click_text("a")); EXPECT(click_text("b"));
    pointer(true,425,365); step(100);
    wifi_state.phase=dh::wifi_change::Phase::Typing; step(); // rebuild while delete is held
    pointer(false,425,365); EXPECT(click_text("c")); step(1000);
    EXPECT(has("abc")); // deleted button cannot leave a repeating deletion behind
    detail::wifi_close(); step();
    fresh(); wifi_allowed=false; wifi_hotspot=true; goto_view(Page::Connect);
    EXPECT(click_text("更换")); step(); EXPECT(!has("Office"));
    EXPECT(has("配对热点打开时不能更换 Wi-Fi"));
    fresh("ocean|-|en-US|large"); goto_view(Page::Connect);
    EXPECT(click_text("Change")); step(); EXPECT(click_text("Office")); step();
    EXPECT(has("Space") && has("Done")); check_layout("wifi-en-large"); check_targets("wifi-en-large");
}

static void case_screenshots() {
    if (out_dir.empty()) return;
    for (const char *theme : {"night", "mint", "ocean", "amber"}) {
        fresh(std::string(theme) + "|-|zh-CN|standard|pct|standard|S+O+C-P-|list");
        with_ports();
        for (size_t i = 0; i < kViews.size(); ++i) {
            goto_view(kViews[i].first, kViews[i].second);
            save_png_source(std::string(theme) + "-" + view_name(i));
        }
    }
    fresh("night|ff7eb6|en-US|large|pct|standard|S+O+C-P-|icons");
    with_ports();
    for (size_t i : {0, 1, 4, 8}) { goto_view(kViews[i].first, kViews[i].second); save_png_source(std::string("en-large-") + view_name(i)); }
    fresh("night|-|zh-CN|small|pct|standard|S+O+C-P-|icons");
    with_ports();
    for (size_t i : {0, 3, 8}) { goto_view(kViews[i].first, kViews[i].second); save_png_source(std::string("zh-small-") + view_name(i)); }
    fresh();   // offline
    with_ports();
    mock_snapshot.agent_ready = false; step(); step();
    save_png_source("state-offline");
    fresh("night|-|zh-CN|standard|pct|standard|S+O+C-P-|icons");
    with_ports();
    save_png_source("state-switch-icons");
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
        {"pages_and_home", case_pages_and_home}, {"wake_goes_home", case_wake_goes_home}, {"switch_styles", case_switch_styles},
        {"source_rename", case_source_rename}, {"themes", case_themes}, {"no_metrics", case_no_metrics}, {"clock_page", case_clock_page},
        {"battery_digits", case_battery_digits}, {"bolt_in_battery", case_bolt_in_battery}, {"home_without_actions", case_home_without_actions}, {"pairing_keeps_awake", case_pairing_keeps_awake}, {"no_setup_card", case_no_setup_card}, {"ring_info", case_ring_info}, {"sound", case_sound}, {"time", case_time}, {"wifi_pages", case_wifi_pages}, {"wifi_keyboard", case_wifi_keyboard}, {"screenshots", case_screenshots}};
    int failed = 0;
    std::ostringstream report;
    report << "{\n  \"simulation_only\": true,\n  \"cases\": [\n";
    bool first = true;
    for (const auto &c : cases) {
        if (const char *only=std::getenv("DESKHUB_PREVIEW_CASE")) {   // one case, or several split by commas
            const std::string list = std::string(",") + only + ",";
            if (list.find(std::string(",") + c.name + ",") == std::string::npos) continue;
        }
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
