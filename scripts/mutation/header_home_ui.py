#!/usr/bin/env python3
"""Mutation check for the bolt inside the battery (docs/design Dev-Header) and the home page without actions (Dev-HomeEmpty).

Usage: scripts/mutation/header_home_ui.py [name-part ...]
Breaks the real UI code on purpose and runs it in native LVGL (the preview harness); every mutation must turn a check red.
Works on a copy in a temporary directory. Exit 0 only if the baseline is green and every mutation is caught.
"""
from _engine import firmware_main

PG = "components/ui/ui_pages.cpp"

PAGES = [
    # the bolt
    ("a slot is left for the bolt next to the battery", PG, "bx - 8 - 22, kIconTop - 2, 28, &lv_font_montserrat_24, p.ink);", "bx - 8 - 14 - 8 - 22, kIconTop - 2, 28, &lv_font_montserrat_24, p.ink);"),
    ("the bolt is never shown", PG, "set_hidden(sh.bolt, !h.battery.charging);", "set_hidden(sh.bolt, true);"),
    ("the bolt is shown without charging", PG, "set_hidden(sh.bolt, !h.battery.charging);", "set_hidden(sh.bolt, false);"),
    ("the digits stay in the middle with the bolt", PG, "centre = start + sh.bolt_ink_w + gap + digits_w / 2;", "centre = 23;"),
    ("bolt and digits touch", PG, "const int gap = bolt && digits_w ? 2 : 0,", "const int gap = 0,"),
    ("the bolt stays at the left edge", PG, "const int bx = start - sh.bolt_ofs_x;", "const int bx = 3;"),
    ("the group is as wide as the bolt only", PG, "total = (bolt ? sh.bolt_ink_w : 0) + gap + digits_w,", "total = (bolt ? sh.bolt_ink_w : 0) + gap,"),
    ("the bolt is outside the middle of the body", PG, "const int gap = bolt && digits_w ? 2 : 0, total = (bolt ? sh.bolt_ink_w : 0) + gap + digits_w, start = 23 - total / 2;", "const int gap = bolt && digits_w ? 2 : 0, total = (bolt ? sh.bolt_ink_w : 0) + gap + digits_w, start = 23 - total;"),
    ("the bolt is not centred in height", PG, "bolt_y = 12 - (bolt_font->line_height - bolt_font->base_line - g.box_h - g.ofs_y) - (g.box_h + 1) / 2;", "bolt_y = 1;"),
    ("the bolt is in the colour of the fill", PG, "sh.bolt = label_at(sh.box, LV_SYMBOL_CHARGE, 0, bolt_y, bolt_adv + 2, bolt_font, p.ink);", "sh.bolt = label_at(sh.box, LV_SYMBOL_CHARGE, 0, bolt_y, bolt_adv + 2, bolt_font, p.card);"),
    # the home page without actions
    # v2: the overview has the ring only, always 252 px in the middle of the body (docs/design v2 HomeRing)
    # the overview of 0.3.21 (docs/design v2 OverviewLarge): the ring fills the body, the time and the date are larger
    ("the ring stays small", PG, "const int d = 316, x =", "const int d = 252, x ="),
    ("the ring stays at the top", PG, "const int y = (kBodyH - d) / 2;   // top of the ring in the body", "const int y = 0;   // top of the ring in the body"),
    ("the ring leaves room for a line under it", PG, "const int y = (kBodyH - d) / 2;   // top of the ring in the body", "const int y = (kBodyH - (d + 10 + lh(look.caption))) / 2;   // top of the ring in the body"),
    ("the time stays small", PG, 'w.clock = label_at(hit, "--:--", 0, 0, kHomeTextW, &font_clock_72, P().ink);', 'w.clock = label_at(hit, "--:--", 0, 0, kHomeTextW, &lv_font_montserrat_48, P().ink);'),
    ("the date stays small", PG, "w.date = label_at(hit, \"\", 0, time_h + 4, kHomeTextW, look.body, P().muted);\n    centered(w.date);\n    single_line(w.date, lh(look.body));",
     "w.date = label_at(hit, \"\", 0, time_h + 4, kHomeTextW, look.caption, P().muted);\n    centered(w.date);\n    single_line(w.date, lh(look.caption));"),
    ("one row of tiles keeps its old height", PG, "th = std::max(196, kBodyH - static_cast<int>(lv_obj_get_y(grid)) - below);", "th = 196; (void)below;"),
    ("the time is not in the middle of the ring", PG, "lv_obj_set_pos(hit, (kContentW - kHomeTextW) / 2, y + d / 2 - block_h / 2);", "lv_obj_set_pos(hit, (kContentW - kHomeTextW) / 2, y + 20);"),
    ("the touch area of the ring stays small", PG, "    }, data(0), d, d);\n    lv_obj_set_pos(ring_hit, x, y);", "    }, data(0), 216, 216);\n    lv_obj_set_pos(ring_hit, x, y);"),
    ("the thickness does not follow the ring", PG, "width = (ring_px(look.appearance.ring).home * d + 108) / 216;", "width = ring_px(look.appearance.ring).home;"),
]


def main():
    return 1 if firmware_main(PAGES, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja") else 0


if __name__ == "__main__":
    raise SystemExit(main())
