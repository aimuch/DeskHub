#!/usr/bin/env python3
"""Mutation check for the factory reset of the screen: the sound and the appearance are forgotten with the configuration.

Usage: scripts/mutation/factory_reset_ui.py [name-part ...]
Breaks the real UI code on purpose and runs it in native LVGL (the preview harness, case reset_restart). Works on a copy in a
temporary directory. Exit 0 only if the baseline is green and every mutation is caught.
"""
import os

from _engine import firmware_main

UI = "components/ui/ui.cpp"
M = [
    ("the sound is kept", UI, "const bool sound = audio::reset_settings(), shown", "const bool sound = true, shown"),
    ("the appearance is kept in the store", UI, "shown = store_clear();", "shown = true;"),
    ("the screen keeps the old appearance", UI, "        if (shown) { appearance = Appearance{}; sources.clear(); look = make_look(appearance); look_dirty = true; }\n", ""),
    ("a failed erase is said to be done", UI, "sound && shown ? Str::ResetDone : Str::ResetPartial", "sound || shown || true ? Str::ResetDone : Str::ResetPartial"),
    ("a failed sound erase is not said", UI, "sound && shown ? Str::ResetDone", "(sound || true) && shown ? Str::ResetDone"),
    ("a failed appearance erase is not said", UI, "sound && shown ? Str::ResetDone", "sound ? Str::ResetDone"),
    ("a failed configuration erase still forgets them", UI, "    if (reset_config() == ESP_OK) {\n", "    if (reset_config() == ESP_OK || true) {\n"),
]

if __name__ == "__main__":
    os.environ["DESKHUB_PREVIEW_CASE"] = "reset_restart"
    raise SystemExit(firmware_main(M, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja"))
