#!/usr/bin/env python3
"""Mutation check for the charge protection screens: break the code on purpose, the tests must go red.

Usage: scripts/mutation/charge_ui.py [name-part ...]
Two parts: the words and states of the view model (ui_model_test), and the wiring of the page and the dialog, run against
the real UI code in native LVGL (the preview harness, tests/ui_preview). Works on a copy in a temporary directory.
Exit 0 only if both baselines are green and every mutation is caught.
"""
from _engine import firmware_main

UM = "components/ui/ui_model.cpp"
UP = "components/ui/ui_pages.cpp"
UI = "components/ui/ui.cpp"

MODEL = [
    ("off is taken for on", UM, "if (s.state == State::Off) {", "if (s.state == State::Charging) {"),
    ("no battery is not told", UM, 'if (fresh && p.battery == "absent") v.why_disabled', 'if (false) v.why_disabled'),
    ("a stale level does not disable the button", UM, 'else if (!fresh || p.battery != "present"', 'else if (p.battery != "present"'),
    ("a chip fault does not disable the button", UM, "const bool fresh = !p.fault && power_fresh(p, now_ms);", "const bool fresh = power_fresh(p, now_ms);"),
    ("an unknown level does not disable the button", UM, " || !level_valid(p.soc_percent)) v.why_disabled", ") v.why_disabled"),
    ("an unreadable switch does not disable the button", UM, "else if (!s.charging_known) v.why_disabled", "else if (false) v.why_disabled"),
    ("the turn-on button has no dots", UM, 'v.button = T(l, Str::TurnOn) + (l == Lang::ZhCN ? "…" : "...");', "v.button = T(l, Str::TurnOn);"),
    ("the limit shown is always 80", UM, 'fmt(" %d%%", s.limit)', 'fmt(" %d%%", 80)'),
    ("a safety stop looks like a normal state", UM, "v.tone = s.state == State::SafetyStop ? Tone::Warn : Tone::Info;", "v.tone = Tone::Info;"),
    ("the notes of two states are swapped", UM, "s.state == State::Holding ? Str::ProtectNoteHolding : Str::ProtectNoteSafety", "s.state == State::Holding ? Str::ProtectNoteSafety : Str::ProtectNoteHolding"),
    ("a failsafe looks calm", UM, "v.tone = Tone::Warn;\n        const std::string why = s.reason;", "v.tone = Tone::Info;\n        const std::string why = s.reason;"),
    ("a stale sample is not explained", UM, 'why == "SAMPLE_STALE" || ', ""),
    ("a chip fault is not explained", UM, ' || why == "PMIC_FAULT")', ")"),
    ("no battery is not explained", UM, 'else if (why == "NO_BATTERY")', "else if (false)"),
    ("a failed stop is not explained", UM, 'why == "STOP_FAILED" || ', ""),
    ("a repeated failure is not explained", UM, ' || why == "REPEATED_FAILURE"', ""),
    ("a restore after a failure is not explained", UM, ' || why == "RESTORED_AFTER_FAILURE")', ")"),
    ("an unknown reason is given a made-up note", UM, 'why == "STOP_FAILED" || ', 'true || '),
    ("an unsure state looks calm", UM, "v.tone = Tone::Danger;", "v.tone = Tone::Warn;"),
    ("an unsure state says it tried to write", UM, "v.note = T(l, Str::ProtectNoteRetry);", "v.note = T(l, Str::ProtectNoteWrite);"),
    ("the turn-off button says turn on", UM, "v.button = T(l, Str::TurnOff);", "v.button = T(l, Str::TurnOn);"),
    ("the turn-off button is disabled", UM, "v.button_enabled = true;\n    switch (s.state) {", "v.button_enabled = false;\n    switch (s.state) {"),
    ("the strip says something else when unsure", UM, "v.strip = v.headline = T(l, Str::ProtectionUnsure);", "v.headline = T(l, Str::ProtectionUnsure);"),
]

PAGES = [
    ("the Protect button does nothing", UP, "w.protect_button = button(c, on_protect, nullptr,", "(void)on_protect; w.protect_button = button(c, [](lv_event_t *) {}, nullptr,"),
    ("an off button asks nothing and turns off", UI, "    if (!now.enabled) {", "    if (false) {"),
    ("turning on does not ask", UI, "build_protect_dialog(screen, protect_confirm, protect_cancel);", "protect_confirm(80);"),
    ("the resume line is the limit", UI, "s.resume = limit - charge::kMinGap;", "s.resume = limit;"),
    ("the limit is always 80", UI, "    s.limit = limit;\n    s.resume = limit", "    s.limit = 80;\n    s.resume = limit"),
    ("turning off turns on", UI, "off.enabled = false;", "off.enabled = true;"),
    ("a failed save of turning on is hidden", UI, "charge_error = charge::apply(s) ? std::string() : std::string(tr(look.lang, Str::ProtectNotSaved));", "(void)charge::apply(s); charge_error.clear();"),
    ("a failed save of turning off is hidden", UI, "charge_error = charge::apply(off) ? std::string() : std::string(tr(look.lang, Str::ProtectOffNotSaved));", "(void)charge::apply(off); charge_error.clear();"),
    ("the frame has no charge state", UI, "    f.charge = charge::status();\n", ""),
    ("the frame has no charge error", UI, "    f.charge_error = charge_error;\n", ""),
    ("the strip is not updated", UP, "        set_text(w.strip, cv.strip);\n", ""),
    ("the headline is not updated", UP, "        set_text(w.protect_head, cv.headline);\n", ""),
    ("the button text is not updated", UP, "        set_text(w.protect_label, cv.button);\n", ""),
    ("the button is never disabled", UP, "            else lv_obj_add_state(w.protect_button, LV_STATE_DISABLED);\n", ""),
    ("the button is never enabled again", UP, "            if (cv.button_enabled) lv_obj_remove_state(w.protect_button, LV_STATE_DISABLED);\n", "            if (cv.button_enabled) {}\n"),
    ("the error is not shown", UP, "        set_optional_text(w.protect_error, f.charge_error);\n", ""),
    ("the reason it is disabled is not shown", UP, "cv.note.empty() ? cv.why_disabled : cv.note", "cv.note"),
]


def main():
    first = firmware_main(MODEL, "ui_model_test")
    second = firmware_main(PAGES, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja")
    return 1 if first or second else 0


if __name__ == "__main__":
    raise SystemExit(main())
