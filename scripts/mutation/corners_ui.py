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
    ("the first icon is not moved in", UP, "const int left_abs = std::max(kMargin, corner_inset(kIconTop) + 3);", "const int left_abs = kMargin;"),
    ("the battery is not moved in", UP, "const int right_abs = kScreen - std::max(kMargin, corner_inset(kIconTop) + 1);", "const int right_abs = kScreen - kMargin;"),
    ("the pairing title is not moved in", UP, "const int title_x = std::max(kMargin, corner_inset(20)),", "const int title_x = kMargin,"),
    ("the curve is ignored", UH, "return kCornerR - static_cast<int>(std::sqrt(static_cast<double>(kCornerR * kCornerR - d * d)));", "(void)d; return 0;"),
    ("the curve is too flat", UH, "return kCornerR - static_cast<int>(std::sqrt(static_cast<double>(kCornerR * kCornerR - d * d)));", "return (kCornerR - static_cast<int>(std::sqrt(static_cast<double>(kCornerR * kCornerR - d * d)))) / 2;"),
]


if __name__ == "__main__":
    raise SystemExit(firmware_main(M, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja"))
