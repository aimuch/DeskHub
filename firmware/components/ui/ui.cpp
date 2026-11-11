// The UI task side: one LVGL timer that reads the snapshot, the power state and the configuration, applies the
// display policy (brightness, dimming, rotation), keeps the pages current, and acknowledges applied configuration.
// What is drawn comes from ui_model.cpp (rules) and ui_pages.cpp (widgets).
#include "ui.hpp"
#include "agent_client.hpp"
#include "audio.hpp"
#include "bsp/esp-bsp.h"
#include "charge.hpp"
#include "esp_lv_adapter.h"
#include "esp_system.h"
#include "ui_internal.hpp"
#include "ui_store.hpp"
#include "ble_link.hpp"
#include "flash_worker.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace dh::ui::detail {
namespace {
Page page = Page::Home, desired = Page::Home;
PowerTab tab = PowerTab::Level, desired_tab = PowerTab::Level;
Appearance appearance;
SourceNames sources;          // the local names and icons of the input sources
std::string edit_id;          // the source of the edit page
bool clock_forced = false;    // idling put the clock page up: waking goes back to the home page
bool confirm_forget = false;  // "forget computers" tapped once
int ble_mode = 0;
std::string built_current;   // the current input the switch page was drawn with             // the pairing page shown: 0 none, 1 waiting, 2 digits
lv_obj_t *ble_view = nullptr;
std::string todo_sel, todo_dialog_id;   // the to-do board (13.13): the selected row; the row of the dialog
int todo_dialog = 0, todo_dialog_built = 0;   // 0 none, 1 the actions, 2 the delete question
int64_t todo_dialog_at = 0;
lv_obj_t *todo_view = nullptr;
constexpr int64_t kTodoAskMs = 10000;   // the delete question goes away by itself (L14)
ble::Todo todo_item;                 // 13.14: the to-do the edit page is about, as it was when it opened
TodoDraft todo_draft_edit, todo_draft_sub;   // the edit, and the copy the page of the due time, the repeat or the list changes
int todo_rev = 0, todo_rev_built = -1;       // a change of the edit pages rebuilds them
TodoRinger ringer;
lv_obj_t *ring_view = nullptr;
ble::Todo ring_item;
int64_t ring_at = 0;
constexpr int64_t kRingShowMs = 60000;       // the ring page closes by itself
time_t test_wall = 0;
time_t wall_now() { return test_wall ? test_wall : time(nullptr); }
std::string fmt_ring_head(const char *hhmm) { char b[96]; snprintf(b, sizeof(b), tr(look.lang, Str::TodoRingNow), hhmm); return b; }
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
void (*pairing_request)() = nullptr;

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

// With a secure Bluetooth link the App is the computer: its sources are the actions of the switch page and the computer
// counts as connected (the header, the ring). Without one nothing from Bluetooth is shown. A copy: the core is untouched.
void overlay_ble(Snapshot &s) {
    if (!ble::status().secure) return;
    const auto list = ble::sources();
    s.agent_ready = true;
    s.agent = "已连接";
    s.action_count = std::min(list.size(), s.actions.size());
    for (size_t i = 0; i < s.action_count; ++i) s.actions[i] = Action{list[i].id, list[i].name, "app", list[i].port, 0};
}

// The pages that keep their own message (a save that failed there), cleared when the page changes.
bool own_error(Page p) { return p == Page::Appearance || p == Page::Pages || p == Page::Sources || p == Page::SourceEdit; }
Page home_page() { return page_of(home_view(appearance.pages)); }

// Everything the pages read, taken once per tick.
Frame make_frame(const Snapshot &s, const DisplayConfig &c, int64_t now, bool want_paired) {
    Frame f;
    f.s = ordered(s, c);
    f.c = c;
    f.power = board_power();
    f.charge = charge::status();
    f.charge_error = charge_error;
    f.now_ms = now;
    f.wall = wall_now();
    if (want_paired) { const auto stored = secrets(); f.paired = stored.ready(); f.wifi_name = stored.ssid; }
    f.command_changed_ms = command_changed_ms;
    f.config_pending = config_due != 0;
    f.effective_brightness = policy.effective;
    f.mode = policy.mode;
    f.last_error = own_error(desired) ? appearance_error : last_error;
    f.rotation_error = rotation_error;
    f.confirm_reset = confirm_reset;
    f.sources = sources;
    f.edit_id = edit_id;
    f.ble = ble::status();
    f.ble_sources = ble::sources().size();
    f.confirm_forget = confirm_forget;
    f.current_id = ble::current();
    f.has_usage = ble::usage(f.usage, f.usage_age_ms);
    f.todos = ble::todos();
    f.todo_selected = todo_sel;
    return f;
}

void rebuild_shell() {
    if (wifi_opened()) wifi_close();
    wifi_reset();
    rename_reset();
    if (body_scroll_y() >= 0) remember_scroll(body_scroll_y());   // a new look keeps the place on the page
    lv_obj_clean(screen);
    forget_body();   // deleted with the screen: nothing may read it until build_page makes the next one
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

// The to-do dialogs, the edit pages and the ring (13.13, 13.14). A function of its own, not inlined into update(): its strings and
// views take stack only while it runs (update() runs on the LVGL task every tick).
__attribute__((noinline)) void todo_overlays(const Frame &f, int64_t now, const DisplayConfig &c, const Snapshot &s, const struct tm &t, bool pairing_now) {
    {   // the to-do dialog: only on the to-do page, only while the list can still act; the delete question times out
        if (todo_dialog) {
            const TodoView tv = make_todos(look.lang, f.todos.has_list ? &f.todos.list : nullptr, f.todos.off_why, f.todos.age_ms, f.ble.secure);
            bool there = false;
            if (f.todos.has_list) for (const auto &t : f.todos.list.items) there = there || t.id == todo_dialog_id;
            if (page != Page::Todo || !tv.can_act || !there || (todo_dialog == 2 && now - todo_dialog_at >= kTodoAskMs)) todo_dialog = 0;
        }
        if (todo_dialog != todo_dialog_built || (todo_dialog >= 3 && todo_rev != todo_rev_built)) {
            if (todo_view) { lv_obj_delete(todo_view); todo_view = nullptr; }
            todo_rev_built = todo_rev;
            if (todo_dialog >= 3) {
                TodoEditView v;
                v.page = todo_dialog;
                v.title = todo_item.title;
                v.draft = todo_dialog == 3 ? todo_draft_edit : todo_draft_sub;
                struct tm lt{};
                localtime_r(&f.wall, &lt);
                v.now_year = f.s.time_valid ? lt.tm_year + 1900 : v.draft.year;
                v.months = month_choices(look.lang, f.s.time_valid, lt.tm_year + 1900, lt.tm_mon + 1, v.draft.year, v.draft.month);
                v.lists = f.todos.lists;
                v.when = draft_when_text(look.lang, v.draft, v.now_year);
                v.repeat = repeat_text(look.lang, v.draft.repeat);
                v.list_name = todo_item.list;
                for (const auto &l : f.todos.lists) if (l.id == v.draft.list_id) v.list_name = l.name;
                todo_view = build_todo_edit(lv_layer_top(), v);
            } else if (todo_dialog) {
                std::string title, detail;
                for (const auto &t : f.todos.list.items) if (t.id == todo_dialog_id) title = t.title;
                const TodoView tv = make_todos(look.lang, &f.todos.list, "", 0, true);
                for (const auto &g : tv.groups) for (const auto &r : g.rows) if (r.id == todo_dialog_id) detail = r.detail;
                todo_view = build_todo_dialog(lv_layer_top(), todo_dialog, title, detail, request_todo_choice);
            }
            todo_dialog_built = todo_dialog;
        }
        if (todo_view) policy.last_activity_ms = now;
    }
    {   // 13.14: a to-do with a ring at this minute (the device's own clock and list); one page at a time
        if (ring_view && now - ring_at >= kRingShowMs) { lv_obj_delete(ring_view); ring_view = nullptr; }
        if (!ring_view && !todo_view && !pairing_now && !wifi_opened() && !rename_opened() && f.todos.has_list && f.todos.off_why.empty()) {
            if (auto due = ringer.due(f.todos.list.items, s.time_valid, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour * 60 + t.tm_min, now)) {
                ring_item = *due;
                ring_at = now;
                const TodoView tv = make_todos(look.lang, &f.todos.list, "", f.todos.age_ms, f.ble.secure);
                std::string detail;
                for (const auto &g : tv.groups) for (const auto &r : g.rows) if (r.id == due->id) detail = r.detail;
                char head[48];
                snprintf(head, sizeof(head), "%02d:%02d", due->minute / 60, due->minute % 60);
                ring_view = build_todo_ring(lv_layer_top(), fmt_ring_head(head), due->title, detail, tv.can_act && !f.todos.waiting);
                audio::ring(quiet_hours(c, s.time_valid, static_cast<uint16_t>(t.tm_hour * 60 + t.tm_min)));
            }
        }
        if (ring_view) policy.last_activity_ms = now;   // M8: the screen lights up, with or without the sound
    }
}

// One pass over the world. `beat` is false for the first pass inside ui_start, so ui_alive() stays false until the timer ran.
void update() {
    const int64_t now = monotonic_ms();
    auto s = snapshot();
    overlay_ble(s);
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
            last_error = tr(look.lang, flashed(ESP_FAIL, [&] { return save_config(c); }) == ESP_OK ? Str::Saved : Str::SaveFailed);
            config_due = 0;
        }
    }
    const PowerState power = board_power();
    policy.fault = power.fault;
    time_t wall = wall_now();
    struct tm t{};
    localtime_r(&wall, &t);
    {   // The pairing page is read, not touched: the code and the hotspot password must not vanish with the screen.
        // Night limits and power protection are policy rules of their own and still apply.
        std::lock_guard<std::mutex> lock(pairing_mutex);
        if (pairing_on || wifi_opened() || rename_opened()) policy.last_activity_ms = now;
    }
    policy.update(c, now, s.time_valid, static_cast<uint16_t>(t.tm_hour * 60 + t.tm_min));
    // The device shows no metrics of the computer any more: the Agent is never asked for them.
    if (s.metrics_visible) mutate([](Snapshot &v, void *) { v.metrics_visible = false; });
    const bool blocked = policy.mode == DisplayMode::Off || policy.mode == DisplayMode::Dimmed || policy.gesture_blocked;
    set_hidden(shield, !blocked);
    if (old_brightness != policy.effective) {
        if (board_brightness(policy.effective) == ESP_OK) old_brightness = policy.effective;
        else last_error = tr(look.lang, Str::BrightnessFailed);
    }
    if (policy.mode == DisplayMode::Clock && page != Page::Clock && !clock_forced) { desired = Page::Clock; clock_forced = true; }
    if (policy.mode == DisplayMode::Active && clock_forced) {   // awake again: home, unless a button already chose a page
        clock_forced = false;
        if (desired == Page::Clock) desired = home_page();
    }

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

    if (s.command != seen_command) {
        seen_command = s.command; command_changed_ms = now;
        // A new result of a command is the one thing that makes the speaker sound here. Quiet hours are the night limit.
        audio::on_command(s.command, quiet_hours(c, s.time_valid, static_cast<uint16_t>(t.tm_hour * 60 + t.tm_min)), static_cast<int64_t>(now));
    }
    bool actions_changed = observed_count != s.action_count;
    for (size_t i = 0; !actions_changed && i < s.action_count && i < 12; ++i) {
        const auto &a = observed_actions[i], &b = s.actions[i];
        actions_changed = a.id != b.id || a.target != b.target || a.input != b.input || a.label != b.label || a.generation != b.generation;
    }
    const bool need_build = page != desired || tab != desired_tab || rebuilding || rendered_revision != c.revision ||
                            ((page == Page::Actions || page == Page::Sources || page == Page::SourceEdit) && actions_changed) ||
                            (page == Page::Actions && ble::current() != built_current) || !built || built_agent != s.agent_ready;
    Frame f = make_frame(s, c, now, desired == Page::Connect);
    if (need_build) {
        const bool same = page == desired;
        if (same && body_scroll_y() >= 0) remember_scroll(body_scroll_y());   // after a new look the body is gone: keep what rebuild_shell remembered
        else appearance_error.clear();
        page = desired;
        tab = desired_tab;
        if (page != Page::Device) confirm_reset = false;
        if (page != Page::Connect) confirm_forget = false;
        f.confirm_reset = confirm_reset;
        f.last_error = own_error(page) ? appearance_error : last_error;
        build_page(body, page, tab, f);
        rendered_revision = c.revision;
        rebuilding = false;
        built = true;
        built_agent = s.agent_ready;
        built_current = f.current_id;
        shown_actions = f.s.actions;
        shown_count = f.s.action_count;
        observed_actions = s.actions;
        observed_count = s.action_count;
    }
    refresh_shell(f, page, pairing_now);
    if (!pairing_now) refresh_page(page, tab, f);
    if (pairing_view) refresh_pairing(info, deadline, now);
    wifi_tick(screen, now, blocked, pairing_now);
    {   // the pairing page of Bluetooth: while the window is open, or while digits wait for the person
        const ble::Status b = ble::status();
        const int mode = b.confirm_pending ? 2 : b.pairing_open ? 1 : 0;
        if (mode != ble_mode) {
            if (ble_view) { lv_obj_delete(ble_view); ble_view = nullptr; }
            if (mode) ble_view = build_ble_pairing(lv_layer_top(), b);
            ble_mode = mode;
        }
        if (ble_view) { refresh_ble_pairing(b); policy.last_activity_ms = now; }   // the person has to read it: keep it awake
    }
    todo_overlays(f, now, c, s, t, pairing_now);
    rename_tick(blocked, pairing_now);
    if (blocked && shield) lv_obj_move_foreground(shield);

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
Page page_of(View v) {
    switch (v) {
    case View::Switch: return Page::Actions;
    case View::Clock: return Page::Clock;
    case View::Power: return Page::Power;
    case View::Usage: return Page::Usage;
    case View::Todo: return Page::Todo;
    case View::Overview: break;
    }
    return Page::Home;
}
void request_update() { if (ui_timer) lv_timer_ready(ui_timer); }
void request_todo_tap(const std::string &id) {
    note_activity();
    todo_sel = todo_sel == id ? std::string() : id;
    request_update();
}
void request_todo_menu(const std::string &id) {
    note_activity();
    const ble::TodoState t = ble::todos();
    const TodoView v = make_todos(look.lang, t.has_list ? &t.list : nullptr, t.off_why, t.age_ms, ble::status().secure);
    if (!v.can_act || t.waiting || todo_dialog) return;   // L4, L5, L10: nothing to do; the page says why
    todo_dialog_id = id;
    todo_dialog = 1;
    request_update();
}
void request_todo_choice(int choice) {
    note_activity();
    const std::string id = todo_dialog_id;
    if (choice == 5 && todo_dialog == 1) {   // "Change...": the edit page, from the to-do as the list has it now (13.14)
        const ble::TodoState t = ble::todos();
        bool found = false;
        for (const auto &x : t.list.items) if (x.id == id) { todo_item = x; found = true; }
        if (!found) { todo_dialog = 0; request_update(); return; }
        time_t w = wall_now();
        struct tm lt{};
        localtime_r(&w, &lt);
        todo_draft_edit = todo_draft(todo_item, snapshot().time_valid, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
        todo_dialog = 3;
        ++todo_rev;
        request_update();
        return;
    }
    if (choice == 3 && todo_dialog == 1) { todo_dialog = 2; todo_dialog_at = monotonic_ms(); request_update(); return; }   // L14: ask again
    const bool send = (choice == 1 || choice == 2) ? todo_dialog == 1 : choice == 4 ? todo_dialog == 2 : false;
    todo_dialog = 0;
    if (send) ble::todo_act(id, choice == 1 ? ble::TodoAction::Done : choice == 2 ? ble::TodoAction::Tomorrow : ble::TodoAction::Delete);
    request_update();
}
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
    last_error = tr(look.lang, flashed(ESP_FAIL, [&] { return save_config(c); }) == ESP_OK ? Str::Saved : Str::SaveFailed);
    desired = Page::Display;
    rebuilding = true;
    note_activity();
    request_update();
}
void apply_appearance(const Appearance &next) {
    note_activity();
    if (!flashed(false, [&] { return store_save(serialize(next)); })) {
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
    charge_error = flashed(false, [&] { return charge::apply(s); }) ? std::string() : std::string(tr(look.lang, Str::ProtectNotSaved));
    request_update();
}
void protect_cancel() { request_update(); }
// Date and time (docs/design Dev-Time).
void request_time_zone(int delta) {
    note_activity();
    auto c = config_due ? pending_config : config();
    config_due = 0;
    c.tz_minutes = static_cast<int16_t>(tz_step(c.tz_minutes, delta));
    last_error = tr(look.lang, flashed(ESP_FAIL, [&] { return save_config(c); }) == ESP_OK ? Str::Saved : Str::SaveFailed);
    desired = Page::Time;
    rebuilding = true;
    request_update();
}
bool request_time_save(const std::string &utc) {
    note_activity();
    const bool ok = !utc.empty() && flashed(false, [&] { return set_time_utc(utc.c_str(), "manual"); });
    if (ok) board_rtc_write(time(nullptr));
    last_error = tr(look.lang, ok ? Str::TimeSavedDone : Str::SoundNotSaved);
    rebuilding = true;
    request_update();
    return ok;
}
void request_time_redraw() {
    note_activity();
    last_error.clear();
    rebuilding = true;
    request_update();
}
// The Pages page. Hiding the last shown page is refused there and said there; the ends of the list do nothing.
void request_pages_toggle(int slot) {
    Appearance next = appearance;
    if (slot < 0 || !pages_toggle(next.pages, static_cast<size_t>(slot))) {
        note_activity();
        // which rule refused it: the last shown page stays, or a fifth would not fit the navigation (13.12 G7)
        const bool showing = slot >= 0 && static_cast<size_t>(slot) < next.pages.size() && !next.pages[static_cast<size_t>(slot)].shown;
        appearance_error = tr(look.lang, showing ? Str::PagesAtMost : Str::PagesKeepOne);
        rebuilding = true;
        request_update();
        return;
    }
    apply_appearance(next);
}
void request_pages_move(int slot, int delta) {
    Appearance next = appearance;
    if (slot < 0 || !pages_move(next.pages, static_cast<size_t>(slot), delta)) { note_activity(); return; }
    apply_appearance(next);
}
void request_switch_style(SwitchStyle style) {
    Appearance next = appearance;
    next.style = style;
    apply_appearance(next);
}
// Seconds are a setting of the device (the App can set them too, layout.show_seconds), not a part of the look: one place for one fact.
void request_show_seconds(bool on) {
    note_activity();
    auto c = config_due ? pending_config : config();
    config_due = 0;
    c.layout.show_seconds = on;
    appearance_error = tr(look.lang, flashed(ESP_FAIL, [&] { return save_config(c); }) == ESP_OK ? Str::Saved : Str::SaveFailed);
    rebuilding = true;
    request_update();
}
void request_clock_hands(bool on) {
    Appearance next = appearance;
    next.clock_hands = on;
    apply_appearance(next);
}
// The names and icons of the sources. A change that cannot be stored leaves the old one and says so on the page.
namespace {
void store_sources(const SourceNames &next, bool valid) {
    note_activity();
    if (!valid || !flashed(false, [&] { return sources_save(serialize_sources(next)); })) appearance_error = tr(look.lang, Str::SaveFailed);
    else { sources = next; appearance_error.clear(); }
    rebuilding = true;
    request_update();
}
}  // namespace
void request_source_edit(size_t index) {
    note_activity();
    if (index >= shown_count) return;
    edit_id = shown_actions[index].id;
    desired = Page::SourceEdit;
    request_update();
}
void request_source_icon(SourceIcon icon) {
    SourceNames next = sources;
    const bool ok = set_source_icon(next, edit_id, icon);
    store_sources(next, ok);
}
void request_source_restore() {
    SourceNames next = sources;
    reset_source(next, edit_id);
    store_sources(next, true);
}
bool request_source_name(const std::string &name) {
    SourceNames next = sources;
    const bool ok = set_source_name(next, edit_id, name);
    store_sources(next, ok);
    return ok && appearance_error.empty();
}
void request_ble_pairing() { note_activity(); ble::open_pairing(); request_update(); }
void request_ble_cancel() { note_activity(); ble::close_pairing(); request_update(); }
void request_ble_answer(bool same) { note_activity(); ble::answer_digits(same); request_update(); }
void request_ble_forget() {
    note_activity();
    if (!confirm_forget) { confirm_forget = true; rebuilding = true; request_update(); return; }
    confirm_forget = false;
    flashed(false, [] { return ble::forget_all(); });
    rebuilding = true;
    request_update();
}
void request_pairing() {
    note_activity();
    if (pairing_request) pairing_request();
}
// The sound page. A switch that cannot be stored stays as it was and says so; with no speaker nothing is asked of it.
void request_sound_set(bool on) {
    note_activity();
    if (!audio::available()) return;
    last_error = flashed(false, [&] { return audio::set_enabled(on); }) ? std::string() : std::string(tr(look.lang, Str::SoundNotSaved));
    rebuilding = true;
    request_update();
}
void request_sound_test() {
    note_activity();
    if (audio::available()) audio::test();
}
// A level of the volume: stored, then heard at once. One that cannot be stored stays as it was, says so and plays nothing.
void request_sound_volume(int level) {
    note_activity();
    if (!audio::available()) return;
    const bool ok = flashed(false, [&] { return audio::set_volume(level); });
    last_error = ok ? std::string() : std::string(tr(look.lang, Str::SoundNotSaved));
    if (ok) audio::test();
    rebuilding = true;
    request_update();
}
// A tap on the ring: the sheet that says what each of the four segments means and why one is dark (docs/design Dev-RingInfo).
void request_ring_info() {
    note_activity();
    if (ring_info_open()) return;
    Snapshot s = snapshot();
    overlay_ble(s);
    build_ring_info(screen, make_ring_info(look.lang, s, board_power(), monotonic_ms()), [] { request_update(); });
}
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
    charge_error = flashed(false, [&] { return charge::apply(off); }) ? std::string() : std::string(tr(look.lang, Str::ProtectOffNotSaved));
    request_update();
}
void do_restart() { esp_restart(); }
void do_factory_reset() {
    if (!confirm_reset) { confirm_reset = true; rebuilding = true; request_update(); return; }
    if (flashed(ESP_FAIL, [] { return reset_config(); }) == ESP_OK) {
        // The sound and the appearance live in namespaces of their own, apart from the configuration: forget them too.
        const bool sound = flashed(false, [] { return audio::reset_settings(); }), shown = flashed(false, [] { return store_clear() && ble::forget_all(); });
        if (shown) { appearance = Appearance{}; sources.clear(); look = make_look(appearance); look_dirty = true; }
        last_error = tr(look.lang, sound && shown ? Str::ResetDone : Str::ResetPartial);  // stay on the page, so the message is read
        confirm_reset = false;
    } else last_error = tr(look.lang, Str::ResetFailed);
    rebuilding = true;
    request_update();
}
void submit_action(size_t index) {
    note_activity();
    if (index >= shown_count) return;
    if (ble::status().secure) {   // the App on the Mac switches; the request carries the App's own id (K2 on both sides)
        if (!ble::request(shown_actions[index].id)) mutate([](Snapshot &s, void *) { s.command = "电脑还在处理上一个请求，或者没有连上"; });
        request_update();
        return;
    }
    if (!snapshot().agent_ready) return;
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
void request_todo_edit_row(int row) {
    note_activity();
    if (todo_dialog != 3) return;
    if (row == 2 && ble::todos().lists.empty()) return;   // M2: nothing to choose from
    todo_draft_sub = todo_draft_edit;
    todo_dialog = row == 0 ? 4 : row == 1 ? 5 : 6;
    ++todo_rev;
    request_update();
}
void request_todo_ring(bool on) {
    note_activity();
    if (todo_dialog != 3) return;
    todo_draft_edit.alarm = on && todo_draft_edit.when == TodoDraft::When::Timed;
    ++todo_rev;
    request_update();
}
void request_todo_when(int which) {
    note_activity();
    if (todo_dialog != 4) return;
    todo_draft_sub.when = which == 0 ? TodoDraft::When::Timed : which == 1 ? TodoDraft::When::AllDay : TodoDraft::When::None;
    ++todo_rev;
    request_update();
}
void request_todo_roller(int which, int selected) {
    note_activity();
    TodoDraft &d = todo_draft_sub;
    if (todo_dialog == 4) {
        if (which == 0) {
            time_t w = wall_now();
            struct tm lt{};
            localtime_r(&w, &lt);
            const auto months = month_choices(look.lang, snapshot().time_valid, lt.tm_year + 1900, lt.tm_mon + 1, todo_draft_edit.year, todo_draft_edit.month);
            if (selected >= 0 && static_cast<size_t>(selected) < months.size()) { d.year = months[static_cast<size_t>(selected)].year; d.month = months[static_cast<size_t>(selected)].month; }
            d.day = std::min(d.day, ble::days_in_month(d.year, d.month));
        } else if (which == 1) d.day = std::clamp(selected + 1, 1, ble::days_in_month(d.year, d.month));
        else if (which == 2) d.minute = std::clamp(selected, 0, 23) * 60 + d.minute % 60;
        else if (which == 3) d.minute = d.minute / 60 * 60 + std::clamp(selected, 0, 59);
    } else if (todo_dialog == 5 && (which == 4 || which == 5)) {
        int n = 1, unit = 0;
        if (ble::valid_repeat(d.repeat) && d.repeat.size() >= 2 && d.repeat != "wd") { n = std::atoi(d.repeat.c_str() + 1); unit = static_cast<int>(std::string("dwmy").find(d.repeat[0])); }
        if (which == 4) n = std::clamp(selected + 1, 1, 99);
        else unit = std::clamp(selected, 0, 3);
        d.repeat = std::string(1, "dwmy"[unit]) + std::to_string(n);
    } else return;
    ++todo_rev;
    request_update();
}
void request_todo_repeat(int preset) {
    note_activity();
    if (todo_dialog != 5) return;
    static const char *const presets[] = {"", "d1", "wd", "w1", "m1", "y1"};
    if (preset >= 0 && preset < 6) todo_draft_sub.repeat = presets[preset];
    ++todo_rev;
    request_update();
}
void request_todo_list_pick(int index) {
    note_activity();
    const auto lists = ble::todos().lists;
    if (todo_dialog != 6 || index < 0 || static_cast<size_t>(index) >= lists.size()) return;
    todo_draft_sub.list_id = lists[static_cast<size_t>(index)].id;
    ++todo_rev;
    request_update();
}
void request_todo_sub(bool ok) {
    note_activity();
    if (todo_dialog < 4) return;
    if (ok) {
        todo_draft_edit = todo_draft_sub;
        if (todo_draft_edit.when != TodoDraft::When::Timed) todo_draft_edit.alarm = false;   // M1: no ring without a time
    }
    todo_dialog = 3;
    ++todo_rev;
    request_update();
}
void request_todo_edit_done(bool save) {
    note_activity();
    if (todo_dialog != 3) return;
    todo_dialog = 0;
    if (save) ble::todo_edit(todo_edit_of(todo_item, todo_draft_edit));
    request_update();
}
void request_todo_ring_choice(int choice) {
    note_activity();
    if (!ring_view) return;
    if (choice == 1) ble::todo_act(ring_item.id, ble::TodoAction::Done);
    if (choice == 2) ringer.snooze(ring_item, monotonic_ms());
    lv_obj_add_flag(ring_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(ring_view);
    ring_view = nullptr;
    request_update();
}
Page current_page() { return page; }
std::string todo_selected() { return todo_sel; }
int todo_dialog_mode() { return todo_dialog_built; }
bool todo_ring_shown() { return ring_view != nullptr; }
void set_wall_for_test(time_t t) { test_wall = t; }
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
    wifi_reset();
    if (screen) lv_obj_delete(screen);
    forget_body();
    if (blank) lv_obj_delete(blank);
    blank = next_blank;
    lv_obj_clean(lv_layer_top());
    screen = body = shield = pairing_view = nullptr;
    page = desired = Page::Home;
    tab = desired_tab = PowerTab::Level;
    appearance = Appearance{};
    sources.clear();
    edit_id.clear();
    clock_forced = false;
    confirm_forget = false;
    ble_mode = 0;
    ble_view = nullptr;
    todo_sel.clear(); todo_dialog_id.clear();
    todo_dialog = todo_dialog_built = 0;
    todo_view = nullptr;
    todo_rev = 0; todo_rev_built = -1;
    ringer.reset();
    ring_view = nullptr;
    ring_at = 0;
    rename_reset();
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
    sources = ui::parse_sources(ui::sources_load());
    look = make_look(appearance);
    page = desired = home_page();   // the device starts on the home page
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
void ui_set_pairing_request(void (*request)()) { ui::detail::pairing_request = request; }
void ui_pairing_hide() {
    using namespace ui::detail;
    std::lock_guard<std::mutex> lock(pairing_mutex);
    pairing_on = false;
}
}  // namespace dh
