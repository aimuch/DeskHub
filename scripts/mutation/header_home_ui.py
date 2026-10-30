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
    ("the ring stays small without actions", PG, "const int d = none ? 252 : 216,", "const int d = 216,"),
    ("the ring stays at the top without actions", PG, "const int y = none ? (kBodyH - (d + 10 + lh(look.caption))) / 2 : 0;", "const int y = 0;"),
    ("the ring is not in the middle of the space", PG, "const int y = none ? (kBodyH - (d + 10 + lh(look.caption))) / 2 : 0;", "const int y = none ? (kBodyH - (d + 10 + lh(look.caption))) / 4 : 0;"),
    ("the ring moves when there are actions", PG, "const int y = none ? (kBodyH - (d + 10 + lh(look.caption))) / 2 : 0;", "const int y = (kBodyH - (d + 10 + lh(look.caption))) / 2;"),
    ("the ring gets large with actions", PG, "const int d = none ? 252 : 216,", "const int d = none ? 252 : 252,"),
    ("the line stays at the old place", PG, "    notice_y = y + d + 10;", "    notice_y = 226;"),
    ("the dot stays at the old place", PG, "notice_y + (lh(look.caption) - 10) / 2);", "226 + (lh(look.caption) - 10) / 2);"),
    ("the clock stays at the old place", PG, "y + d / 2 - 68);", "40);"),
    ("the touch area of the ring stays small", PG, "    }, data(0), d, d);\n    lv_obj_set_pos(ring_hit, x, y);", "    }, data(0), 216, 216);\n    lv_obj_set_pos(ring_hit, x, y);"),
    ("the thickness does not follow the ring", PG, "width = (ring_px(look.appearance.ring).home * d + 108) / 216;", "width = ring_px(look.appearance.ring).home;"),
]


def main():
    return 1 if firmware_main(PAGES, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja") else 0


if __name__ == "__main__":
    raise SystemExit(main())
