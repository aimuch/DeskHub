#!/usr/bin/env python3
"""Mutation check for the time zone and the time set by hand (docs/design Dev-Time): break the code on purpose, the tests must go red.

Usage: scripts/mutation/time_ui.py [name-part ...]
Two parts: the rules (ui_model_test T23: the zones in core/policy.cpp, the numbers in ui_model.cpp), and the page against the real UI
code in native LVGL (the preview harness, case time). Works on a copy in a temporary directory. Exit 0 only if both baselines are
green and every mutation is caught.
"""
import os

from _engine import firmware_main

PO = "components/core/policy.cpp"
MD = "components/ui/ui_model.cpp"
PG = "components/ui/ui_pages.cpp"
UI = "components/ui/ui.cpp"

RULES = [
    ("the sign of the TZ is what people say", PO, "minutes > 0 ? '-' : '+'", "minutes > 0 ? '+' : '-'"),
    ("zero is written with a minus", PO, "minutes > 0 ? '-' : '+'", "minutes >= 0 ? '-' : '+'"),
    ("the minutes of a zone are lost", PO, "size / 60, size % 60);", "size / 60, 0);"),
    ("a step passes the top", PO, "std::min(kZoneCount - 1, std::max(0, i + delta))", "std::max(0, i + delta) % kZoneCount"),
    ("a step passes the bottom", PO, "std::min(kZoneCount - 1, std::max(0, i + delta))", "(i + delta + kZoneCount) % kZoneCount"),
    ("a value that is no zone is kept", PO, "if (i < 0) return kDefaultTzMinutes;", "if (i < 0) return minutes;"),
    ("every number is a zone", PO, "bool valid_tz(int minutes) { return zone_index(minutes) >= 0; }", "bool valid_tz(int minutes) { return minutes > -100000; }"),
    ("a zone is missing", PO, "330, 345, 360,", "330, 360,"),
    ("a config with no zone passes", PO, "return valid_tz(c.tz_minutes) && valid_layout(c.layout)", "return valid_layout(c.layout)"),
    ("the label has the wrong sign", MD, "fmt(\"UTC%c%d\", minutes > 0 ? '+' : '-', size / 60)", "fmt(\"UTC%c%d\", minutes > 0 ? '-' : '+', size / 60)"),
    ("the label loses the minutes", MD, "return size % 60 ? fmt(\"UTC%c%d:%02d\", minutes > 0 ? '+' : '-', size / 60, size % 60) : ", "return "),
    ("the start is the zone of the device", MD, "const time_t local = utc_now + static_cast<time_t>(tz_minutes) * 60;", "const time_t local = utc_now + 480 * 60; (void)tz_minutes;"),
    ("an unknown time is taken as known", MD, "    if (!time_valid) return d;\n", "    if (false && !time_valid) return d;\n"),
    ("the year has no top", MD, "std::min(2099, std::max(2024, d.year + delta))", "std::max(2024, d.year + delta)"),
    ("the year has no bottom", MD, "std::min(2099, std::max(2024, d.year + delta))", "std::min(2099, d.year + delta)"),
    ("the month does not wrap", MD, "d.month = wrap(d.month + delta, 1, 12);", "d.month = std::min(12, std::max(1, d.month + delta));"),
    ("the day wraps at 31", MD, "d.day = wrap(d.day + delta, 1, days_in_month(d.year, d.month));", "d.day = wrap(d.day + delta, 1, 31);"),
    ("the hour wraps at 24", MD, "d.hour = wrap(d.hour + delta, 0, 23);", "d.hour = wrap(d.hour + delta, 0, 24);"),
    ("the minute wraps at 60", MD, "d.minute = wrap(d.minute + delta, 0, 59);", "d.minute = wrap(d.minute + delta, 0, 60);"),
    ("a day can leave its month", MD, "    d.day = std::min(d.day, days_in_month(d.year, d.month));\n", ""),
    ("no leap years", MD, "return month == 2 && leap ? 29 : kDays[month - 1];", "(void)leap; return kDays[month - 1];"),
    ("a day that does not exist is saved", MD, "d.day > days_in_month(d.year, d.month)) return \"\";", "d.day > 31) return \"\";"),
    ("the local time is saved as UTC", MD, "const time_t utc = timegm(&t) - static_cast<time_t>(tz_minutes) * 60;", "const time_t utc = timegm(&t); (void)tz_minutes;"),
    ("the zone is added, not taken away", MD, "const time_t utc = timegm(&t) - static_cast<time_t>(tz_minutes) * 60;", "const time_t utc = timegm(&t) + static_cast<time_t>(tz_minutes) * 60;"),
    ("a year the core refuses is offered", MD, "if (u.tm_year + 1900 < 2024 || u.tm_year + 1900 > 2099) return \"\";", ""),
]

PAGES = [
    ("the settings page has no row", PG, "                        {Str::SettingTime, Str::SettingTimeSub, Page::Time, P().clock},\n", ""),
    ("the zone goes the wrong way", PG, "request_time_zone(data_of(e) ? 1 : -1);", "request_time_zone(data_of(e) ? -1 : 1);"),
    ("the zone is not saved", UI, "last_error = tr(look.lang, flashed(ESP_FAIL, [&] { return save_config(c); }) == ESP_OK ? Str::Saved : Str::SaveFailed);\n    desired = Page::Time;", "last_error = tr(look.lang, Str::Saved);\n    desired = Page::Time;"),
    ("a failed save of the zone is not said", UI, "save_config(c); }) == ESP_OK ? Str::Saved : Str::SaveFailed);\n    desired = Page::Time;", "save_config(c); }) == ESP_OK ? Str::Saved : Str::Saved);\n    desired = Page::Time;"),
    ("the page shows the wrong zone", PG, "tz_label(f.c.tz_minutes), 64,", "tz_label(480), 64,"),
    ("the numbers change the time at once", PG, "time_draft_step(g_time_draft, g_time_field, data_of(e) ? 1 : -1); request_time_redraw();", "time_draft_step(g_time_draft, g_time_field, data_of(e) ? 1 : -1); request_time_save(time_draft_utc(g_time_draft, config().tz_minutes));"),
    ("the steps go the wrong way", PG, "time_draft_step(g_time_draft, g_time_field, data_of(e) ? 1 : -1);", "time_draft_step(g_time_draft, g_time_field, data_of(e) ? -1 : 1);"),
    ("the chosen number stays the minute", PG, "g_time_field = data_of(e); request_time_redraw();", "(void)data_of(e); request_time_redraw();"),
    ("a redraw loses the numbers", PG, "    if (!g_time_seeded) { g_time_draft =", "    if (true) { g_time_draft ="),
    ("another page keeps the numbers", PG, "    if (page != Page::Time) g_time_seeded = false;\n", ""),
    ("the first number chosen is the year", PG, "g_time_field = 4; g_time_seeded = true; }", "g_time_field = 0; g_time_seeded = true; }"),
    ("a refused time loses the numbers", PG, "if (request_time_save(time_draft_utc(g_time_draft, config().tz_minutes))) g_time_seeded = false;", "request_time_save(time_draft_utc(g_time_draft, config().tz_minutes)); g_time_seeded = false;"),
    ("the saved time uses another zone", PG, "time_draft_utc(g_time_draft, config().tz_minutes))) g_time_seeded", "time_draft_utc(g_time_draft, 0))) g_time_seeded"),
    ("the RTC is not written", UI, "    if (ok) board_rtc_write(time(nullptr));\n", ""),
    ("a refused time writes the RTC", UI, "    if (ok) board_rtc_write(time(nullptr));\n", "    board_rtc_write(time(nullptr));\n"),
    ("a refused time is said to be set", UI, "ok ? Str::TimeSavedDone : Str::SoundNotSaved", "Str::TimeSavedDone"),
    ("the result is not shown", PG, "    } else set_text(w.meta, T(Str::TimeUnset));\n    set_optional_text(w.error, f.last_error);\n", "    } else set_text(w.meta, T(Str::TimeUnset));\n"),
]


def main():
    first = firmware_main(RULES, "ui_model_test")
    os.environ["DESKHUB_PREVIEW_CASE"] = "time"
    second = firmware_main(PAGES, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja")
    return 1 if first or second else 0


if __name__ == "__main__":
    raise SystemExit(main())
