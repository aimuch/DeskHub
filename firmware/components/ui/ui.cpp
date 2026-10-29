// The UI task side: one LVGL timer that reads the snapshot, the power state and the configuration, applies the
// display policy (brightness, dimming, rotation), keeps the pages current, and acknowledges applied configuration.
// What is drawn comes from ui_model.cpp (rules) and ui_pages.cpp (widgets).
#include "ui.hpp"
#include "agent_client.hpp"
#include "bsp/esp-bsp.h"
#include "charge.hpp"
#include "esp_lv_adapter.h"
#include "esp_system.h"
#include "ui_internal.hpp"
#include "ui_store.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace dh::ui::detail {
namespace {
Page page = Page::Home, desired = Page::Home;
PowerTab tab = PowerTab::Level, desired_tab = PowerTab::Level;
Appearance appearance;
lv_obj_t *screen = nullptr, *body = nullptr, *shield = nullptr, *pairing_view = nullptr, *blank = nullptr;
lv_timer_t *ui_timer = nullptr;
DisplayPolicy policy;
int old_brightness = -1, applied_rotation = 0, attempted_rotation = -1;
std::string rotation_error, last_error, appearance_error, charge_error, seen_command;
int64_t command_changed_ms = 0, config_due = 0;
DisplayConfig pending_config;
uint32_t rendered_revision = 0;
bool rebuilding = false, look_dirty = false, confirm_reset = false, built = false, built_agent = false;
std::array<Action, 12> shown_actions{}, observed_actions{};
size_t shown_count = 0, observed_count = 0;
std::atomic<uint32_t> heartbeat_ms{0}, applied_revision{0};
std::atomic<bool> tick_seen{false}, applied_seen{false};

std::mutex pairing_mutex;
bool pairing_on = false, pairing_dirty = false;
PairingInfo pairing_info;
int64_t pairing_deadline = 0;
void (*pairing_cancel)() = nullptr;

constexpr int64_t kConfigDebounceMs = 2000;

// The actions in the order the managed layout wants them: the chosen ones first, then the rest.
Snapshot ordered(const Snapshot &s, const DisplayConfig &c) {
    Snapshot r = s;
    const size_t n = std::min<size_t>(s.action_count, s.actions.size());
    std::array<bool, 12> taken{};
    size_t out = 0;
    for (size_t i = 0; i < c.layout.action_count; ++i)
        for (size_t j = 0; j < n; ++j)
            if (!taken[j] && s.actions[j].id == c.layout.action_ids[i]) { r.actions[out++] = s.actions[j]; taken[j] = true; break; }
    for (size_t j = 0; j < n; ++j) if (!taken[j]) r.actions[out++] = s.actions[j];
    r.action_count = n;
    return r;
}

void shield_touch(lv_event_t *ev) {
    const auto code = lv_event_get_code(ev);
    if (code == LV_EVENT_PRESSED) policy.touch_down(monotonic_ms());
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) policy.touch_up();
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) request_update();
}
void active_touch(lv_event_t *ev) {
    if (lv_event_get_code(ev) == LV_EVENT_PRESSED) policy.last_activity_ms = monotonic_ms();
}

// Everything the pages read, taken once per tick.
Frame make_frame(const Snapshot &s, const DisplayConfig &c, int64_t now, bool want_paired) {
    Frame f;
    f.s = ordered(s, c);
    f.c = c;
    f.power = board_power();
    f.charge = charge::status();
    f.charge_error = charge_error;
    f.now_ms = now;
    f.wall = time(nullptr);
    f.paired = want_paired && secrets().ready();
    f.command_changed_ms = command_changed_ms;
    f.config_pending = config_due != 0;
    f.effective_brightness = policy.effective;
    f.mode = policy.mode;
    f.last_error = desired == Page::Appearance ? appearance_error : last_error;
    f.rotation_error = rotation_error;
    f.confirm_reset = confirm_reset;
    return f;
}

void rebuild_shell() {
    lv_obj_clean(screen);
    pairing_view = nullptr;
    build_shell(screen);
    body = holder(screen, kContentW, kBodyH);
    lv_obj_set_pos(body, kMargin, kBodyTop);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(body, [](lv_event_t *) { request_update(); }, LV_EVENT_SCROLL, nullptr);
    // The header and the navigation sit above the body in the draw order, as before: body first, then they are re-raised.
    lv_obj_move_background(body);
}

// One pass over the world. `beat` is false for the first pass inside ui_start, so ui_alive() stays false until the timer ran.
void update() {
    const int64_t now = monotonic_ms();
    auto s = snapshot();
    auto c = config();
    // Each requested angle is applied once. A failure stays visible until another angle is requested.
    if (attempted_rotation != c.rotation) {
        attempted_rotation = c.rotation;
        if (applied_rotation != c.rotation) {
            if (board_rotation(c.rotation) == ESP_OK) {
                applied_rotation = c.rotation;
                rotation_error.clear();
                mutate([](Snapshot &v, void *) { if (v.error.rfind("DISPLAY_ROTATION_FAILED:", 0) == 0) v.error.clear(); });
            } else {
                char buf[96];
                snprintf(buf, sizeof(buf), tr(look.lang, Str::RotationFailed), applied_rotation);
                rotation_error = buf;
                mutate([](Snapshot &v, void *) { v.error = "DISPLAY_ROTATION_FAILED: 屏幕方向未应用"; });
            }
        } else {
            rotation_error.clear();
            mutate([](Snapshot &v, void *) { if (v.error.rfind("DISPLAY_ROTATION_FAILED:", 0) == 0) v.error.clear(); });
        }
    }
    if (config_due && pending_config.revision != c.revision) { config_due = 0; last_error = tr(look.lang, Str::ConfigStale); }
    if (config_due) {
        c = pending_config;
        if (now >= config_due) {
            last_error = tr(look.lang, save_config(c) == ESP_OK ? Str::Saved : Str::SaveFailed);
            config_due = 0;
        }
    }
    const PowerState power = board_power();
    policy.fault = power.fault;
    time_t wall = time(nullptr);
    struct tm t{};
    localtime_r(&wall, &t);
    {   // The pairing page is read, not touched: the code and the hotspot password must not vanish with the screen.
        // Night limits and power protection are policy rules of their own and still apply.
        std::lock_guard<std::mutex> lock(pairing_mutex);
        if (pairing_on) policy.last_activity_ms = now;
    }
    policy.update(c, now, s.time_valid, static_cast<uint16_t>(t.tm_hour * 60 + t.tm_min));
    bool visible = page == Page::Computer && policy.mode == DisplayMode::Active;
    if (s.metrics_visible != visible) mutate([](Snapshot &v, void *p) { v.metrics_visible = *static_cast<bool *>(p); }, &visible);
    const bool blocked = policy.mode == DisplayMode::Off || policy.mode == DisplayMode::Dimmed || policy.gesture_blocked;
    set_hidden(shield, !blocked);
    if (old_brightness != policy.effective) {
        if (board_brightness(policy.effective) == ESP_OK) old_brightness = policy.effective;
        else last_error = tr(look.lang, Str::BrightnessFailed);
    }
    if (policy.mode == DisplayMode::Clock && page != Page::Clock) desired = Page::Clock;

    // A new look replaces the whole screen. It runs here, never inside the event that asked for it.
    if (look_dirty) {
        look_dirty = false;
        rebuild_shell();
        rebuilding = true;
        built = false;
    }
    // The pairing page, when the backend has one to show.
    bool pairing_now;
    PairingInfo info;
    int64_t deadline;
    bool dirty;
    {
        std::lock_guard<std::mutex> lock(pairing_mutex);
        pairing_now = pairing_on; info = pairing_info; deadline = pairing_deadline; dirty = pairing_dirty;
        pairing_dirty = false;
    }
    void (*cancel)() = nullptr;
    { std::lock_guard<std::mutex> lock(pairing_mutex); cancel = pairing_cancel; }
    if (pairing_view && (!pairing_now || dirty)) { lv_obj_delete(pairing_view); pairing_view = nullptr; }
    if (pairing_now && !pairing_view) pairing_view = build_pairing(screen, info, deadline, cancel);
    set_hidden(body, pairing_now);

    if (s.command != seen_command) { seen_command = s.command; command_changed_ms = now; }
    bool actions_changed = observed_count != s.action_count;
    for (size_t i = 0; !actions_changed && i < s.action_count && i < 12; ++i) {
        const auto &a = observed_actions[i], &b = s.actions[i];
        actions_changed = a.id != b.id || a.target != b.target || a.input != b.input || a.label != b.label || a.generation != b.generation;
    }
    const bool need_build = page != desired || tab != desired_tab || rebuilding || rendered_revision != c.revision ||
                            ((page == Page::Home || page == Page::Actions) && actions_changed) || !built || built_agent != s.agent_ready;
    Frame f = make_frame(s, c, now, desired == Page::Connect);
    if (need_build) {
        const bool same = page == desired;
        if (same) remember_scroll(body_scroll_y());
        else appearance_error.clear();
        page = desired;
        tab = desired_tab;
        if (page != Page::Device) confirm_reset = false;
        f.confirm_reset = confirm_reset;
        f.last_error = page == Page::Appearance ? appearance_error : last_error;
        build_page(body, page, tab, f);
        rendered_revision = c.revision;
        rebuilding = false;
        built = true;
        built_agent = s.agent_ready;
        shown_actions = f.s.actions;
        shown_count = f.s.action_count;
        observed_actions = s.actions;
        observed_count = s.action_count;
    }
    refresh_shell(f, page, pairing_now);
    if (!pairing_now) refresh_page(page, tab, f);
    if (pairing_view) refresh_pairing(info, deadline, now);

    // Always-on keeps the mode Active, so the clock page still needs its burn-in shift.
    const int offset = page == Page::Clock && c.protection && (policy.mode != DisplayMode::Active || c.off_s == 0) ? static_cast<int>((now / 60000) % 5) - 2 : 0;
    set_translate(body, offset * 2, offset * 2);
    shift_shell(offset * 2);

    const auto stored = config();
    if (!config_due && stored.revision == c.revision && stored.brightness == c.brightness && rendered_revision == c.revision &&
        applied_rotation == c.rotation && old_brightness == policy.effective && dh::ui_alive()) {
        applied_revision.store(c.revision, std::memory_order_relaxed);
        applied_seen.store(true, std::memory_order_release);
    }
}
}  // namespace

// ---------------------------------------------------------------- what the pages ask for
void request_update() { if (ui_timer) lv_timer_ready(ui_timer); }
void note_activity() { policy.last_activity_ms = monotonic_ms(); }
void request_page(Page p) { desired = p; note_activity(); request_update(); }
void request_power_tab(PowerTab t) { desired_tab = t; note_activity(); request_update(); }
void user_screen_off() { policy.user_off = true; request_update(); }
void schedule_brightness(int percent) {
    pending_config = config();
    pending_config.brightness = static_cast<uint8_t>(std::min(100, std::max(3, percent)));
    config_due = monotonic_ms() + kConfigDebounceMs;
    note_activity();
}
void change_setting(int which) {
    auto c = config_due ? pending_config : config();
    config_due = 0;
    if (which == 0) c.night_enabled = !c.night_enabled;
    if (which == 1) { c.clock_s = c.clock_s == 30 ? 60 : c.clock_s == 60 ? 120 : 30; c.dim_s = std::max(c.dim_s, c.clock_s); }
    if (which == 2) { c.dim_s = c.dim_s == 120 ? 180 : c.dim_s == 180 ? 300 : 120; c.dim_s = std::max(c.dim_s, c.clock_s); if (c.off_s) c.off_s = std::max(c.off_s, c.dim_s); }
    if (which == 3) { c.off_s = c.off_s == 300 ? 600 : c.off_s == 600 ? 1800 : c.off_s == 1800 ? 0 : 300; if (c.off_s) c.off_s = std::max(c.off_s, c.dim_s); }
    if (which == 4) c.night_start = c.night_start == 21 * 60 ? 22 * 60 : c.night_start == 22 * 60 ? 23 * 60 : 21 * 60;
    if (which == 5) c.night_end = c.night_end == 6 * 60 ? 7 * 60 : c.night_end == 7 * 60 ? 8 * 60 : 6 * 60;
    if (which == 6) c.night_cap = c.night_cap == 5 ? 10 : c.night_cap == 10 ? 15 : c.night_cap == 15 ? 25 : c.night_cap == 25 ? 50 : 5;
    if (which == 7) c.protection = !c.protection;
    if (which == 9) c.rotation = (c.rotation + 90) % 360;
    if (which == 10) c.off_s = c.off_s ? 0 : std::max<uint32_t>(300, c.dim_s);
    last_error = tr(look.lang, save_config(c) == ESP_OK ? Str::Saved : Str::SaveFailed);
    desired = Page::Display;
    rebuilding = true;
    note_activity();
    request_update();
}
void apply_appearance(const Appearance &next) {
    note_activity();
    if (!store_save(serialize(next))) {
        appearance_error = tr(look.lang, Str::SaveFailed);  // the old look stays
        rebuilding = true;
        request_update();
        return;
    }
    appearance_error.clear();
    appearance = next;
    look = make_look(next);
    look_dirty = true;
    request_update();
}
// Turning protection on asks first (rules R1: an experiment is off by default and needs a second tap). Turning it off
// does not ask. Either way the controller decides what the chip does; this only passes the user's choice on.
void protect_confirm(int limit) {
    charge::Settings s;
    s.enabled = true;
    s.limit = limit;
    s.resume = limit - charge::kMinGap;
    charge_error = charge::apply(s) ? std::string() : std::string(tr(look.lang, Str::ProtectNotSaved));
    request_update();
}
void protect_cancel() { request_update(); }
void request_protect_toggle() {
    note_activity();
    const charge::Status now = charge::status();
    if (!now.enabled) {
        charge_error.clear();
        build_protect_dialog(screen, protect_confirm, protect_cancel);
        return;
    }
    charge::Settings off;
    off.enabled = false;
    off.limit = now.limit;
    off.resume = now.resume;
    charge_error = charge::apply(off) ? std::string() : std::string(tr(look.lang, Str::ProtectOffNotSaved));
    request_update();
}
void do_restart() { esp_restart(); }
void do_factory_reset() {
    if (!confirm_reset) { confirm_reset = true; rebuilding = true; request_update(); return; }
    if (reset_config() == ESP_OK) {
        last_error = tr(look.lang, Str::ResetDone);  // stay on the page, so the message is read
        confirm_reset = false;
    } else last_error = tr(look.lang, Str::ResetFailed);
    rebuilding = true;
    request_update();
}
void submit_action(size_t index) {
    note_activity();
    if (index >= shown_count || !snapshot().agent_ready) return;
    if (!agent_submit(shown_actions[index])) mutate([](Snapshot &s, void *) { s.command = "当前不可用或已有操作，请稍后点击"; });
    request_update();
}

void tick(lv_timer_t *) {
    // A partially applied frame must never acknowledge its configuration.
    applied_seen.store(false, std::memory_order_release);
    heartbeat_ms.store(static_cast<uint32_t>(monotonic_ms()), std::memory_order_relaxed);
    tick_seen.store(true, std::memory_order_release);
    update();
}

// ---------------------------------------------------------------- test hooks
lv_obj_t *screen_object() { return screen; }
lv_obj_t *body_object() { return body; }
Page current_page() { return page; }
PowerTab current_power_tab() { return tab; }
const Appearance &current_appearance() { return appearance; }
static int count_objects(lv_obj_t *o) {
    int n = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) n += count_objects(lv_obj_get_child(o, i));
    return n;
}
int live_object_count() { return (screen ? count_objects(screen) : 0) + count_objects(lv_layer_top()); }
void reset_for_test() {
    lv_timer_handler();  // let deferred deletes finish before their objects are removed
    if (ui_timer) { lv_timer_delete(ui_timer); ui_timer = nullptr; }
    auto *next_blank = lv_obj_create(nullptr);
    lv_screen_load(next_blank);
    if (screen) lv_obj_delete(screen);
    if (blank) lv_obj_delete(blank);
    blank = next_blank;
    lv_obj_clean(lv_layer_top());
    screen = body = shield = pairing_view = nullptr;
    page = desired = Page::Home;
    tab = desired_tab = PowerTab::Level;
    appearance = Appearance{};
    look = make_look(appearance);
    policy = DisplayPolicy{};
    old_brightness = -1; applied_rotation = 0; attempted_rotation = -1;
    rotation_error.clear(); last_error.clear(); appearance_error.clear(); seen_command.clear();
    command_changed_ms = 0; config_due = 0; rendered_revision = 0;
    rebuilding = look_dirty = confirm_reset = built = false;
    shown_count = observed_count = 0;
    heartbeat_ms = 0; applied_revision = 0; tick_seen = false; applied_seen = false;
    std::lock_guard<std::mutex> lock(pairing_mutex);
    pairing_on = pairing_dirty = false;
    pairing_cancel = nullptr;
}
}  // namespace dh::ui::detail

namespace dh {
void ui_start() {
    using namespace ui::detail;
    // The BSP bool wrapper converts ESP_OK (0) to false; use esp_err_t directly.
    if (esp_lv_adapter_lock(1000) != ESP_OK) {
        mutate([](Snapshot &s, void *) { s.error = "UI_LOCK_FAILED: LVGL 界面初始化锁失败"; });
        return;
    }
    struct DisplayLockGuard { ~DisplayLockGuard() { esp_lv_adapter_unlock(); } } guard;
    mutate([](Snapshot &s, void *) { if (s.error.rfind("UI_LOCK_FAILED:", 0) == 0) s.error.clear(); });
    appearance = ui::parse_appearance(ui::store_load());
    look = make_look(appearance);
    screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, kScreen, kScreen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    rebuild_shell();
    if (auto *indev = bsp_display_get_input_dev()) lv_indev_add_event_cb(indev, active_touch, LV_EVENT_PRESSED, nullptr);
    shield = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(shield);
    lv_obj_set_size(shield, kScreen, kScreen);
    lv_obj_set_pos(shield, 0, 0);
    lv_obj_add_flag(shield, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(shield, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(shield, shield_touch, LV_EVENT_ALL, nullptr);
    update();
    lv_screen_load(screen);
    ui_timer = lv_timer_create(tick, 100, nullptr);
}
bool ui_alive() {
    using namespace ui::detail;
    if (!tick_seen.load(std::memory_order_acquire)) return false;
    return static_cast<uint32_t>(monotonic_ms()) - heartbeat_ms.load(std::memory_order_relaxed) < 2000;
}
bool ui_configuration_applied(uint32_t local_revision) {
    using namespace ui::detail;
    if (!applied_seen.load(std::memory_order_acquire)) return false;
    return applied_revision.load(std::memory_order_relaxed) == local_revision && applied_seen.load(std::memory_order_acquire) && ui_alive();
}
void ui_pairing_show(const PairingInfo &info, void (*on_cancel)()) {
    using namespace ui::detail;
    std::lock_guard<std::mutex> lock(pairing_mutex);
    pairing_info = info;
    pairing_deadline = monotonic_ms() + static_cast<int64_t>(std::max(-1, std::min(info.remaining_s, 24 * 3600))) * 1000;
    pairing_cancel = on_cancel;
    pairing_on = true;
    pairing_dirty = true;
}
void ui_pairing_hide() {
    using namespace ui::detail;
    std::lock_guard<std::mutex> lock(pairing_mutex);
    pairing_on = false;
}
}  // namespace dh
