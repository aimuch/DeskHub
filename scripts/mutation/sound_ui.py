#!/usr/bin/env python3
"""Mutation check for the sound page and its triggers (docs/design Dev-Sound): break the code on purpose, the tests must go red.

Usage: scripts/mutation/sound_ui.py [name-part ...]
Two parts: the quiet hours (ui_model_test), and the page, the switch, the listen button and the trigger of a command text,
run against the real UI code in native LVGL (the preview harness). The rules of the tones themselves are checked by
scripts/mutation/audio_policy.py. Works on a copy in a temporary directory. Exit 0 only if both baselines are green and
every mutation is caught.
"""
from _engine import firmware_main

MD = "components/ui/ui_model.cpp"
PG = "components/ui/ui_pages.cpp"
UI = "components/ui/ui.cpp"

MODEL = [
    ("a night limit that is off still keeps quiet", MD, "return c.night_enabled && (!time_valid || in_night(minute_of_day, c.night_start, c.night_end));", "return (!time_valid || in_night(minute_of_day, c.night_start, c.night_end));"),
    ("an unknown time is not quiet", MD, "return c.night_enabled && (!time_valid || in_night(minute_of_day, c.night_start, c.night_end));", "return c.night_enabled && (time_valid && in_night(minute_of_day, c.night_start, c.night_end));"),
    ("the day is quiet and the night is not", MD, "return c.night_enabled && (!time_valid || in_night(minute_of_day, c.night_start, c.night_end));", "return c.night_enabled && (!time_valid || !in_night(minute_of_day, c.night_start, c.night_end));"),
    ("the window of the night is the display one reversed", MD, "in_night(minute_of_day, c.night_start, c.night_end));", "in_night(minute_of_day, c.night_end, c.night_start));"),
]

PAGES = [
    ("the switch sets the opposite", UI, "last_error = audio::set_enabled(on) ?", "last_error = audio::set_enabled(!on) ?"),
    ("a failed save is not said", UI, ": std::string(tr(look.lang, Str::SoundNotSaved));", ": std::string();"),
    ("a good save is said to have failed", UI, "last_error = audio::set_enabled(on) ? std::string() :", "last_error = !audio::set_enabled(on) ? std::string() :"),
    ("the switch works with no speaker", UI, "    note_activity();\n    if (!audio::available()) return;\n    last_error", "    note_activity();\n    last_error"),
    ("listen needs the sound on", UI, "if (audio::available()) audio::test();", "if (audio::available() && audio::enabled()) audio::test();"),
    ("listen does nothing", UI, "if (audio::available()) audio::test();", "if (false) audio::test();"),
    ("the page shows the sound as off", PG, "{T(Str::SoundOff), T(Str::SoundOn)}, on ? 1 : 0,", "{T(Str::SoundOff), T(Str::SoundOn)}, on ? 0 : 0,"),
    ("the page shows the sound as on", PG, "{T(Str::SoundOff), T(Str::SoundOn)}, on ? 1 : 0,", "{T(Str::SoundOff), T(Str::SoundOn)}, on ? 1 : 1,"),
    ("the note ignores the speaker", PG, "T(speaker ? Str::SoundNote : Str::SoundNoSpeaker), speaker ? P().muted : P().power);", "T(Str::SoundNote), P().muted);"),
    ("the controls are not dimmed without a speaker", PG, "    if (!speaker) {   // nothing to turn on", "    if (false) {   // nothing to turn on"),
    ("the error of a save is not shown", PG, "void refresh_sound(const Frame &f) { set_optional_text(w.error, f.last_error); }", "void refresh_sound(const Frame &) {}"),
    ("the settings page has no sound row", PG, "                        {Str::SettingSound, Str::SettingSoundSub, Page::Sound, P().agent},\n", ""),
    # The preview is built with -Werror: a mutation must still compile (no unused variable or function, no switch case left out).
    ("the sound page is empty", PG, "    case Page::Sound: build_sound(body, f); break;\n", "    case Page::Sound: (void)build_sound; break;\n"),
    ("a command text never reaches the speaker", UI, "        audio::on_command(s.command,", "        audio::on_command(std::string(),"),
    ("the night is never said to the speaker", UI, "audio::on_command(s.command, quiet_hours(c, s.time_valid, static_cast<uint16_t>(t.tm_hour * 60 + t.tm_min)),", "audio::on_command(s.command, false,"),
    ("the night is always said to the speaker", UI, "audio::on_command(s.command, quiet_hours(c, s.time_valid, static_cast<uint16_t>(t.tm_hour * 60 + t.tm_min)),", "audio::on_command(s.command, true,"),
    ("the same text sounds on every tick", UI, "        seen_command = s.command; command_changed_ms = now;\n        // A new result", "        command_changed_ms = now;\n        // A new result"),
    ("the minute of the night is the hour", UI, "quiet_hours(c, s.time_valid, static_cast<uint16_t>(t.tm_hour * 60 + t.tm_min))", "quiet_hours(c, s.time_valid, static_cast<uint16_t>(t.tm_hour))"),
]


def main():
    first = firmware_main(MODEL, "ui_model_test")
    second = firmware_main(PAGES, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja")
    return 1 if first or second else 0


if __name__ == "__main__":
    raise SystemExit(main())
