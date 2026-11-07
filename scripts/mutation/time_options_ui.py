#!/usr/bin/env python3
"""Mutation check for A40: seconds and clock hands are choices (docs/design v2 OverviewSeconds, ClockHands, AppearanceTime).
Break the code on purpose, the tests must go red.

Usage: scripts/mutation/time_options_ui.py [name-part ...]
Two parts: the stored look (ui_model_test T5 and T25: the hands field in ui_model.cpp), and the pages against the real UI code in
native LVGL (the preview harness, case time_options). Works on a copy in a temporary directory. Exit 0 only if both baselines are
green and every mutation is caught.
"""
import os

from _engine import firmware_main

MD = "components/ui/ui_model.cpp"
MH = "components/ui/include/ui_model.hpp"
PG = "components/ui/ui_pages.cpp"
UI = "components/ui/ui.cpp"

RULES = [
    ("the hands are not written", MD, ' + "|" + (a.clock_hands ? "hands" : "nohands");', ";"),
    ("the hands are always written on", MD, '(a.clock_hands ? "hands" : "nohands")', '(a.clock_hands || true ? "hands" : "nohands")'),
    ("nine fields are refused", MD, " && field.size() != 8 && field.size() != 9) return Appearance{};", " && field.size() != 8) return Appearance{};"),
    ("nine fields are taken as 0.3.x", MD, "const bool old = field.size() < 8;", "const bool old = field.size() != 8;"),
    ("nine fields lose the pages and the style", MD, "    if (field.size() >= 8) {\n", "    if (field.size() == 8) {\n"),
    ("an unknown hands word is taken", MD, '        else if (field[8] == "nohands") a.clock_hands = false;\n        else return Appearance{};\n',
     '        else if (field[8] == "nohands") a.clock_hands = false;\n'),
    ("nohands is read as hands", MD, 'else if (field[8] == "nohands") a.clock_hands = false;', 'else if (field[8] == "nohands") a.clock_hands = true;'),
    ("a new device has no hands", MH, "    bool clock_hands = true;", "    bool clock_hands = false;"),
]

SAVE = "appearance_error = tr(look.lang, flashed(ESP_FAIL, [&] { return save_config(c); }) == ESP_OK ? Str::Saved : Str::SaveFailed);\n    rebuilding = true;\n    request_update();\n}\nvoid request_clock_hands"
PAGES = [
    ("the seconds are not saved", UI, "    c.layout.show_seconds = on;\n", "    (void)on;\n"),
    ("the seconds are saved outside the flash worker", UI, SAVE,
     SAVE.replace("flashed(ESP_FAIL, [&] { return save_config(c); }) == ESP_OK", "save_config(c) == ESP_OK")),
    ("a failed save of the seconds is not said", UI, SAVE, SAVE.replace("? Str::Saved : Str::SaveFailed", "? Str::Saved : Str::Saved")),
    ("the hands are not saved", UI, "    next.clock_hands = on;\n", "    (void)on;\n"),
    ("the seconds choice shows off always", PG, "f.c.layout.show_seconds ? 0 : 1, [](lv_event_t *e) { request_show_seconds", "1, [](lv_event_t *e) { request_show_seconds"),
    ("the hands choice shows on always", PG, "a.clock_hands ? 0 : 1, [](lv_event_t *e) { request_clock_hands", "0, [](lv_event_t *e) { request_clock_hands"),
    ("the seconds buttons are swapped", PG, "request_show_seconds(data_of(e) == 0)", "request_show_seconds(data_of(e) == 1)"),
    ("the hands buttons are swapped", PG, "request_clock_hands(data_of(e) == 0)", "request_clock_hands(data_of(e) == 1)"),
    ("hands without a known time", PG, "hands = look.appearance.clock_hands && c.valid;", "hands = look.appearance.clock_hands;"),
    ("hands whatever is chosen", PG, "hands = look.appearance.clock_hands && c.valid;", "hands = c.valid;"),
    ("the second hand without seconds", PG, "secs ? t.tm_sec * 6 : -1", "t.tm_sec * 6"),
    ("the hour hand jumps by the hour", PG, "(t.tm_hour % 12) * 30 + t.tm_min / 2", "(t.tm_hour % 12) * 30"),
    ("the hour hand on a 24-hour dial", PG, "(t.tm_hour % 12) * 30 + t.tm_min / 2", "t.tm_hour * 15 + t.tm_min / 4"),
    ("the minute hand jumps by the minute", PG, "t.tm_min * 6 + t.tm_sec / 10", "t.tm_min * 6"),
    ("the large digits stay under the hands", PG, "    set_hidden(w.digits_group, hands);\n", "    set_hidden(w.digits_group, false);\n"),
    ("the hands stay over the large digits", PG, "    set_hidden(w.hands_group, !hands);\n", "    set_hidden(w.hands_group, false);\n"),
    ("the overview shows seconds whatever is chosen", PG, "c, f.c.layout.show_seconds && c.valid, &font_clock_72, &lv_font_montserrat_24, kHomeTextW", "c, c.valid, &font_clock_72, &lv_font_montserrat_24, kHomeTextW"),
    ("the overview shows seconds of an unknown time", PG, "c, f.c.layout.show_seconds && c.valid, &font_clock_72, &lv_font_montserrat_24, kHomeTextW", "c, f.c.layout.show_seconds, &font_clock_72, &lv_font_montserrat_24, kHomeTextW"),
    ("the large digits show no seconds", PG, "c, secs && !hands, &font_clock_72", "c, false, &font_clock_72"),
    ("the small digits show no seconds", PG, "c, secs, &lv_font_montserrat_24, &btr_ui_16", "c, false, &lv_font_montserrat_24, &btr_ui_16"),
    ("the time leaves its place for the seconds", PG, "        narrow = width - (4 + s.x);\n", "        lv_obj_set_x(clock, -(4 + s.x) / 2);\n"),
]


def main():
    first = firmware_main(RULES, "ui_model_test")
    os.environ["DESKHUB_PREVIEW_CASE"] = "time_options"
    second = firmware_main(PAGES, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja")
    return 1 if first or second else 0


if __name__ == "__main__":
    raise SystemExit(main())
