// The keyboard for the name of an input source (docs/design v2 SourceEdit, keys as Dev-Keyboard). The keys are ASCII only:
// the font has no input method, so a Chinese name has to come from the App. Built on ticks, never inside a release event.
#include "ui_internal.hpp"
#include "ui_keyboard.hpp"
#include <cstdio>

namespace dh::ui::detail {
namespace {
Keyboard editor;
lv_obj_t *root = nullptr, *input = nullptr, *count = nullptr, *done = nullptr;
bool opened = false, dirty = false, closing = false;
int key_page = 0;
int64_t delete_due = 0;
const Palette &P() { return look.palette; }
int event_id(lv_event_t *e) { return static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))); }
lv_obj_t *key(lv_obj_t *parent, const std::string &s, int x, int y, int w, int h, lv_event_cb_t cb, int id = 0, bool symbol = false) {
    auto *b = button(parent, cb, reinterpret_cast<void *>(static_cast<intptr_t>(id)), w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_radius(b, 14, 0);
    auto *l = label(b, s, w - 8, symbol ? &lv_font_montserrat_24 : &btr_ui_20, P().ink);
    lv_obj_center(l);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return b;
}
void refresh_input() {
    if (!input) return;
    set_text(input, editor.value());
    char n[16];
    snprintf(n, sizeof(n), "%u/%u", static_cast<unsigned>(editor.size()), static_cast<unsigned>(editor.limit()));
    set_text(count, n);
    if (editor.valid()) lv_obj_remove_state(done, LV_STATE_DISABLED); else lv_obj_add_state(done, LV_STATE_DISABLED);
    lv_obj_set_style_opa(done, editor.valid() ? LV_OPA_COVER : LV_OPA_40, 0);
}
void key_event(lv_event_t *e) {
    const int id = event_id(e);
    note_activity();
    if (id == 256) { key_page = key_page == 1 ? 0 : 1; dirty = true; }
    else if (id == 257) { key_page = key_page >= 2 ? 0 : 2; dirty = true; }
    else if (id == 258) { key_page = key_page == 2 ? 3 : 2; dirty = true; }
    else if (id == 259) editor.erase();
    else editor.append(static_cast<char>(id));
    refresh_input();
}
void delete_event(lv_event_t *e) {
    const auto code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) { note_activity(); delete_due = monotonic_ms() + 500; }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) delete_due = 0;
}
void back_event(lv_event_t *) { note_activity(); rename_back(); }
void done_event(lv_event_t *) {
    note_activity();
    if (!editor.valid()) return;
    request_source_name(editor.value());   // a failure is said on the edit page
    editor.clear();
    closing = true;
    request_update();
}
void build() {
    delete_due = 0;
    input = count = done = nullptr;
    if (root) lv_obj_delete(root);
    root = panel(lv_layer_top(), 480, 480, P().bg, P().bg, 0);
    lv_obj_set_style_bg_grad_color(root, hex(P().bg2), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(root, 0, 0);
    key(root, LV_SYMBOL_LEFT, 42, 10, 56, 56, back_event, 0, true);   // x: outside the rounded corner at this height
    auto *t = label_at(root, tr(look.lang, Str::SourceKeyboardTitle), 102, 25, 318, &btr_ui_20, P().accent);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_height(t, 26);
    auto *field = panel(root, 430, 56, P().card, P().accent, 16);
    lv_obj_set_pos(field, 25, 66);
    input = label_at(field, "", 14, 17, 330, &btr_ui_20, P().ink);
    lv_label_set_long_mode(input, LV_LABEL_LONG_CLIP);
    lv_obj_set_height(input, 27);
    count = label_at(field, "", 346, 20, 72, &btr_ui_16, P().muted);
    lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_RIGHT, 0);
    auto *hint = label_at(root, tr(look.lang, Str::SourceKeyboardHint), 25, 130, 430, &btr_ui_16, P().muted);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_CLIP);
    lv_obj_set_height(hint, 22);
    const auto rows = Keyboard::rows(key_page);
    for (int r = 0; r < 4; ++r) {
        const int offset = r == 3 ? 1 : 0;
        for (size_t c = 0; c < rows[static_cast<size_t>(r)].size(); ++c)
            key(root, std::string(1, rows[static_cast<size_t>(r)][c]), 25 + (static_cast<int>(c) + offset) * 62, 156 + r * 60, 58, 56, key_event, rows[static_cast<size_t>(r)][c]);
    }
    auto *shift = key(root, key_page < 2 ? LV_SYMBOL_UP : key_page == 2 ? "1/2" : "2/2", 25, 336, 58, 56, key_event, key_page < 2 ? 256 : 258, key_page < 2);
    if (key_page == 1) { set_bg(shift, P().selected); set_border(shift, P().accent); }
    auto *del = key(root, LV_SYMBOL_BACKSPACE, 397, 336, 58, 56, key_event, 259, true);
    lv_obj_add_event_cb(del, delete_event, LV_EVENT_ALL, nullptr);
    key(root, key_page < 2 ? "123" : "abc", 25, 396, 120, 56, key_event, 257);
    key(root, look.lang == Lang::EnUS ? "Space" : "空格", 149, 396, 182, 56, key_event, ' ');
    done = key(root, look.lang == Lang::EnUS ? "Done" : "完成", 335, 396, 120, 56, done_event);
    set_bg(done, P().selected);
    set_border(done, P().accent);
    refresh_input();
    dirty = false;
}
void close_now() {
    editor.clear();
    if (root) lv_obj_delete(root);
    root = input = count = done = nullptr;
    opened = dirty = closing = false;
    delete_due = 0;
}
}  // namespace

void rename_start(const std::string &current) {
    note_activity();
    editor.reset(false);
    for (char c : current) editor.append(c);   // only what the keys can type is kept: the rest of a Chinese name is dropped
    key_page = 0;
    opened = dirty = true;
    closing = false;
    request_update();
}
bool rename_opened() { return opened; }
void rename_tick(bool blocked, bool pairing) {
    if (!opened) return;
    if (closing || pairing) { close_now(); return; }
    if (blocked) delete_due = 0;
    if (dirty) build();
    const int64_t now = monotonic_ms();
    if (delete_due && !blocked && now >= delete_due) { editor.erase(); delete_due = now + 80; refresh_input(); }
}
void rename_reset() { close_now(); }
bool rename_open() { return opened && root; }
void rename_back() { editor.clear(); closing = true; request_update(); }
void rename_erase() { editor.erase(); refresh_input(); }
}  // namespace dh::ui::detail
