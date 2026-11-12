// The icons of the input sources (docs/design v2 SourceEdit) and the tick ring of the clock page. Both are drawn as strokes
// in the draw event of one plain object, so an icon costs one object and no image data, and it takes any colour and size.
#include "ui_internal.hpp"
#include <cmath>
#include <vector>

namespace dh::ui::detail {
namespace {
struct Seg { float x1, y1, x2, y2; };
using Strokes = std::vector<Seg>;

void line(Strokes &s, float x1, float y1, float x2, float y2) { s.push_back({x1, y1, x2, y2}); }
void poly(Strokes &s, std::initializer_list<std::pair<float, float>> pts, bool closed) {
    const auto *p = pts.begin();
    for (size_t i = 0; i + 1 < pts.size(); ++i) line(s, p[i].first, p[i].second, p[i + 1].first, p[i + 1].second);
    if (closed && pts.size() > 2) line(s, p[pts.size() - 1].first, p[pts.size() - 1].second, p[0].first, p[0].second);
}
// A rounded rectangle: four sides and four quarter circles of four steps each.
void rrect(Strokes &s, float x, float y, float w, float h, float r) {
    // Corners in screen angles (y down): top right -90..0, bottom right 0..90, bottom left 90..180, top left 180..270.
    const float cx[4] = {x + w - r, x + w - r, x + r, x + r}, cy[4] = {y + r, y + h - r, y + h - r, y + r};
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 4; ++i) {
            const float a0 = (-90.0f + 90.0f * c + 22.5f * i) * 3.14159265f / 180.0f, a1 = a0 + 22.5f * 3.14159265f / 180.0f;
            line(s, cx[c] + r * std::cos(a0), cy[c] + r * std::sin(a0), cx[c] + r * std::cos(a1), cy[c] + r * std::sin(a1));
        }
    line(s, x + r, y, x + w - r, y);
    line(s, x + w, y + r, x + w, y + h - r);
    line(s, x + w - r, y + h, x + r, y + h);
    line(s, x, y + h - r, x, y + r);
}
const Strokes &strokes(SourceIcon icon) {
    static std::vector<Strokes> table;
    if (table.empty()) {
        table.resize(9);
        Strokes *s = &table[static_cast<size_t>(SourceIcon::Hdmi)];
        poly(*s, {{3, 8.5f}, {21, 8.5f}, {21, 13}, {18.5f, 15.5f}, {5.5f, 15.5f}, {3, 13}}, true);
        line(*s, 7, 11.5f, 17, 11.5f);
        s = &table[static_cast<size_t>(SourceIcon::Dp)];
        poly(*s, {{3, 8.5f}, {21, 8.5f}, {21, 15.5f}, {6.5f, 15.5f}, {3, 12}}, true);
        line(*s, 8, 12, 17, 12);
        s = &table[static_cast<size_t>(SourceIcon::UsbC)];
        rrect(*s, 3, 9, 18, 6.5f, 3.25f);
        line(*s, 8, 12.25f, 16, 12.25f);
        s = &table[static_cast<size_t>(SourceIcon::Laptop)];
        rrect(*s, 5, 5.5f, 14, 9.5f, 1.5f);
        line(*s, 2.5f, 18.5f, 21.5f, 18.5f);
        s = &table[static_cast<size_t>(SourceIcon::Desktop)];
        rrect(*s, 3.5f, 9, 17, 7, 2.5f);
        line(*s, 8, 19, 16, 19);
        s = &table[static_cast<size_t>(SourceIcon::Monitor)];
        rrect(*s, 3, 4.5f, 18, 12, 2);
        line(*s, 12, 16.5f, 12, 20.5f);
        line(*s, 8.5f, 20.5f, 15.5f, 20.5f);
        s = &table[static_cast<size_t>(SourceIcon::Console)];
        rrect(*s, 3, 8, 18, 9, 4.5f);
        line(*s, 7.5f, 10.5f, 7.5f, 14.5f);
        line(*s, 5.5f, 12.5f, 9.5f, 12.5f);
        line(*s, 15.5f, 11, 15.6f, 11);
        line(*s, 17.5f, 13.5f, 17.6f, 13.5f);
        s = &table[static_cast<size_t>(SourceIcon::Tv)];
        rrect(*s, 2.5f, 5, 19, 12, 1.5f);
        line(*s, 7, 20.5f, 17, 20.5f);
        poly(*s, {{9, 2.5f}, {12, 5}, {15, 2.5f}}, false);
        table[static_cast<size_t>(SourceIcon::Auto)] = table[static_cast<size_t>(SourceIcon::Monitor)];
    }
    return table[static_cast<size_t>(icon)];
}
void draw_icon(lv_event_t *e) {
    auto *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    const auto icon = static_cast<SourceIcon>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(obj)));
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const float scale = static_cast<float>(lv_area_get_width(&a)) / 24.0f;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_obj_get_style_line_color(obj, LV_PART_MAIN);
    d.width = lv_obj_get_style_line_width(obj, LV_PART_MAIN);
    d.opa = lv_obj_get_style_opa(obj, LV_PART_MAIN);
    d.round_start = d.round_end = 1;
    for (const Seg &s : strokes(icon)) {
        d.p1.x = a.x1 + static_cast<int32_t>(std::lround(s.x1 * scale));
        d.p1.y = a.y1 + static_cast<int32_t>(std::lround(s.y1 * scale));
        d.p2.x = a.x1 + static_cast<int32_t>(std::lround(s.x2 * scale));
        d.p2.y = a.y1 + static_cast<int32_t>(std::lround(s.y2 * scale));
        lv_draw_line(lv_event_get_layer(e), &d);
    }
}
void draw_ticks(lv_event_t *e) {
    auto *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const float cx = (a.x1 + a.x2) / 2.0f, cy = (a.y1 + a.y2) / 2.0f, r = lv_area_get_width(&a) / 2.0f - 2;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.round_start = d.round_end = 1;
    for (int i = 0; i < 60; ++i) {
        const bool major = i % 5 == 0;
        const float angle = static_cast<float>(i) * 6.0f * 3.14159265f / 180.0f, len = major ? 12.0f : 6.0f;
        const float sx = std::sin(angle), sy = -std::cos(angle);
        d.color = major ? lv_obj_get_style_text_color(obj, LV_PART_MAIN) : lv_obj_get_style_line_color(obj, LV_PART_MAIN);
        d.width = major ? 3 : 2;
        d.p1.x = static_cast<int32_t>(std::lround(cx + sx * r));
        d.p1.y = static_cast<int32_t>(std::lround(cy + sy * r));
        d.p2.x = static_cast<int32_t>(std::lround(cx + sx * (r - len)));
        d.p2.y = static_cast<int32_t>(std::lround(cy + sy * (r - len)));
        lv_draw_line(lv_event_get_layer(e), &d);
    }
}
lv_obj_t *drawing(lv_obj_t *parent, int size) {
    auto *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    return o;
}
}  // namespace

lv_obj_t *source_icon(lv_obj_t *parent, SourceIcon icon, int size, uint32_t rgb, int stroke) {
    auto *o = drawing(parent, size);
    lv_obj_set_user_data(o, reinterpret_cast<void *>(static_cast<intptr_t>(icon)));
    lv_obj_add_flag(o, LV_OBJ_FLAG_USER_1);   // marks an icon, for source_icon_of
    lv_obj_set_style_line_color(o, hex(rgb), 0);
    lv_obj_set_style_line_width(o, stroke, 0);
    lv_obj_add_event_cb(o, draw_icon, LV_EVENT_DRAW_MAIN, nullptr);
    return o;
}
SourceIcon source_icon_of(lv_obj_t *o) {
    if (!o || !lv_obj_has_flag(o, LV_OBJ_FLAG_USER_1)) return SourceIcon::Auto;
    return static_cast<SourceIcon>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(o)));
}
lv_obj_t *tick_ring(lv_obj_t *parent, int size, uint32_t minor, uint32_t major) {
    auto *o = drawing(parent, size);
    lv_obj_set_style_line_color(o, hex(minor), 0);
    lv_obj_set_style_text_color(o, hex(major), 0);
    lv_obj_add_event_cb(o, draw_ticks, LV_EVENT_DRAW_MAIN, nullptr);
    return o;
}
}  // namespace dh::ui::detail
