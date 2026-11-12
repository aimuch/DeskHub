// Widget helpers: equality-guarded setters and the few shapes every page is made of.
#include "ui_internal.hpp"
#include <algorithm>
#include <cstring>

namespace dh::ui::detail {
Look look;

const lv_font_t *font_for(int px) { return px <= 16 ? &btr_ui_16 : px <= 20 ? &btr_ui_20 : &btr_ui_28; }
Look make_look(const Appearance &a) {
    Look l;
    l.appearance = a;
    l.lang = a.lang;
    l.palette = apply_accent(theme_palette(a.theme), a.accent);
    const FontPx px = font_px(a.size);
    l.title = font_for(px.title);
    l.body = font_for(px.body);
    l.caption = font_for(px.caption);
    return l;
}

// LVGL 9.4 invalidates even when a style is set to the value it already has, so every per-tick update is guarded.
void set_text(lv_obj_t *o, const std::string &text) {
    if (o && strcmp(lv_label_get_text(o), text.c_str())) lv_label_set_text(o, text.c_str());
}
void set_hidden(lv_obj_t *o, bool hidden) {
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}
void set_optional_text(lv_obj_t *o, const std::string &text) {
    if (!o) return;
    set_text(o, text);
    set_hidden(o, text.empty());
}
void set_bg(lv_obj_t *o, uint32_t rgb) {
    if (o && !lv_color_eq(lv_obj_get_style_bg_color(o, LV_PART_MAIN), hex(rgb))) lv_obj_set_style_bg_color(o, hex(rgb), 0);
}
void set_border(lv_obj_t *o, uint32_t rgb) {
    if (o && !lv_color_eq(lv_obj_get_style_border_color(o, LV_PART_MAIN), hex(rgb))) lv_obj_set_style_border_color(o, hex(rgb), 0);
}
void set_ink(lv_obj_t *o, uint32_t rgb) {
    if (o && !lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), hex(rgb))) lv_obj_set_style_text_color(o, hex(rgb), 0);
}
void set_translate(lv_obj_t *o, int x, int y) {
    if (!o) return;
    if (lv_obj_get_style_translate_x(o, LV_PART_MAIN) != x) lv_obj_set_style_translate_x(o, x, 0);
    if (lv_obj_get_style_translate_y(o, LV_PART_MAIN) != y) lv_obj_set_style_translate_y(o, y, 0);
}
uint32_t tone_color(Tone t) {
    const Palette &p = look.palette;
    switch (t) {
    case Tone::Ok: return p.agent;
    case Tone::Info: return p.wifi;
    case Tone::Warn: return p.power;
    case Tone::Danger: return p.danger;
    case Tone::Off: break;
    }
    return p.muted;
}

lv_obj_t *panel(lv_obj_t *parent, int w, int h, uint32_t bg, uint32_t edge, int radius) {
    auto *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, hex(edge), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}
// A transparent container: no fill, no border, so the background gradient shows through.
lv_obj_t *holder(lv_obj_t *parent, int w, int h) {
    auto *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}
lv_obj_t *dot(lv_obj_t *parent, int size, uint32_t rgb, bool lit) {
    auto *o = holder(parent, size, size);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, hex(rgb), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (lit) {
        lv_obj_set_style_shadow_width(o, 14, 0);
        lv_obj_set_style_shadow_color(o, hex(rgb), 0);
        lv_obj_set_style_shadow_opa(o, LV_OPA_50, 0);
        lv_obj_set_style_shadow_spread(o, 1, 0);
    }
    return o;
}
lv_obj_t *label(lv_obj_t *parent, const std::string &text, int w, const lv_font_t *font, uint32_t rgb) {
    auto *o = lv_label_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, hex(rgb), 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
    lv_label_set_text(o, text.c_str());
    return o;
}
lv_obj_t *label_at(lv_obj_t *parent, const std::string &text, int x, int y, int w, const lv_font_t *font, uint32_t rgb) {
    auto *o = label(parent, text, w, font, rgb);
    lv_obj_set_pos(o, x, y);
    return o;
}
void ensure_touch_target(lv_obj_t *o) {
    lv_obj_update_layout(o);
    const int w = lv_obj_get_width(o), h = lv_obj_get_height(o);
    const int ext = std::max({0, (kMinTarget - w + 1) / 2, (kMinTarget - h + 1) / 2});
    if (ext) lv_obj_set_ext_click_area(o, ext);
}
lv_obj_t *button(lv_obj_t *parent, lv_event_cb_t cb, void *data, int w, int h) {
    const Palette &p = look.palette;
    auto *o = lv_button_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, hex(p.card), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, hex(p.selected), LV_STATE_PRESSED);
    lv_obj_set_style_radius(o, 24, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, hex(p.edge), 0);
    lv_obj_set_style_border_color(o, hex(p.accent), LV_STATE_PRESSED);
    lv_obj_set_style_opa(o, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, data);
    ensure_touch_target(o);
    return o;
}
lv_obj_t *pill(lv_obj_t *parent, const std::string &text, uint32_t ink, uint32_t fill, uint32_t edge, const lv_font_t *font) {
    auto *o = lv_label_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, hex(ink), 0);
    lv_obj_set_style_bg_color(o, hex(fill), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, hex(edge), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(o, 12, 0);
    lv_obj_set_style_pad_ver(o, 2, 0);
    lv_label_set_text(o, text.c_str());
    return o;
}
// An arc drawn as a stroke only (the ring segments and their glow).
lv_obj_t *arc_segment(lv_obj_t *parent, int size, int start, int end, uint32_t rgb, int width, lv_opa_t opa) {
    auto *a = lv_arc_create(parent);
    lv_obj_set_size(a, size, size);
    lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(a, 0, 0);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, hex(rgb), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(a, opa, LV_PART_INDICATOR);
    lv_arc_set_rotation(a, 0);
    lv_arc_set_bg_angles(a, start, end);
    lv_arc_set_angles(a, start, end);
    return a;
}
// A 270 degree gauge, gap at the bottom, empty until it gets a real reading.
lv_obj_t *gauge(lv_obj_t *parent, int size, int width, uint32_t rgb) {
    auto *a = lv_arc_create(parent);
    lv_obj_set_size(a, size, size);
    lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(a, 0, 0);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, hex(look.palette.track), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, hex(rgb), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_arc_set_rotation(a, 135);
    lv_arc_set_bg_angles(a, 0, 270);
    lv_arc_set_range(a, 0, 100);
    lv_arc_set_value(a, 0);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    return a;
}
void gauge_set(lv_obj_t *a, bool valid, float percent, uint32_t rgb) {
    if (!a) return;
    const lv_opa_t opa = valid ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (lv_obj_get_style_arc_opa(a, LV_PART_INDICATOR) != opa) lv_obj_set_style_arc_opa(a, opa, LV_PART_INDICATOR);
    const int value = valid ? static_cast<int>(std::min(100.0f, std::max(0.0f, percent)) + 0.5f) : 0;
    if (lv_arc_get_value(a) != value) lv_arc_set_value(a, value);
    if (!lv_color_eq(lv_obj_get_style_arc_color(a, LV_PART_INDICATOR), hex(rgb))) lv_obj_set_style_arc_color(a, hex(rgb), LV_PART_INDICATOR);
}
}  // namespace dh::ui::detail
