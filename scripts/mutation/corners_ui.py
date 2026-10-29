#!/usr/bin/env python3
"""Mutation check for the rounded-corner layout: break the code on purpose, the preview's corner test must go red.

Usage: scripts/mutation/corners_ui.py [name-part ...]
Runs against the real UI code in native LVGL (the preview harness, tests/ui_preview). Works on a copy in a temporary
directory. Exit 0 only if the baseline is green and every mutation is caught.
Only mutations that change what the screen shows at the committed radius (kCornerR) are listed: removing a shift that is
not needed at that radius would change nothing, and nothing can be said about it. (Moving the first header line
to the top edge is one of those: the text then moves in instead, by the same formula, and is not cut either.)
"""
from _engine import firmware_main

UP = "components/ui/ui_pages.cpp"
UH = "components/ui/include/ui_internal.hpp"

M = [
    ("the status dot is not moved in", UP, "const int dot_x = std::max(2, corner_inset(dot_y) - kMargin);", "const int dot_x = 2;"),
    ("the battery outline is not moved in", UP, "lv_obj_set_pos(sh.outline, 174 - shift_right, outline_y);", "lv_obj_set_pos(sh.outline, 174, outline_y);"),
    ("the battery tip is not moved in", UP, "lv_obj_set_pos(sh.terminal, 207 - shift_right, terminal_y);", "lv_obj_set_pos(sh.terminal, 207, terminal_y);"),
    ("the pairing title is not moved in", UP, "const int title_x = std::max(kMargin, corner_inset(20)),", "const int title_x = kMargin,"),
    ("the curve is ignored", UH, "return kCornerR - static_cast<int>(std::sqrt(static_cast<double>(kCornerR * kCornerR - d * d)));", "(void)d; return 0;"),
    ("the curve is too flat", UH, "return kCornerR - static_cast<int>(std::sqrt(static_cast<double>(kCornerR * kCornerR - d * d)));", "return (kCornerR - static_cast<int>(std::sqrt(static_cast<double>(kCornerR * kCornerR - d * d)))) / 2;"),
]


if __name__ == "__main__":
    raise SystemExit(firmware_main(M, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja"))
