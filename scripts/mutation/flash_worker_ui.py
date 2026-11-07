#!/usr/bin/env python3
"""Mutation check for A38: every save of the screen goes through the flash worker. Each mutation calls the store directly
again, as 0.3.17 and the first 0.3.18 did (that restarted the device). The preview harness, case flash_worker, must go red.

Usage: scripts/mutation/flash_worker_ui.py [name-part ...]
"""
import os

from _engine import firmware_main

UI = "components/ui/ui.cpp"
M = [
    ("the appearance is saved directly", UI, "if (!flashed(false, [&] { return store_save(serialize(next)); })) {", "if (!store_save(serialize(next))) {"),
    ("source names are saved directly", UI, "!flashed(false, [&] { return sources_save(serialize_sources(next)); }))", "!sources_save(serialize_sources(next)))"),
    ("the sound is saved directly", UI, "flashed(false, [&] { return audio::set_enabled(on); }) ?", "audio::set_enabled(on) ?"),
    ("the volume is saved directly", UI, "flashed(false, [&] { return audio::set_volume(level); });", "audio::set_volume(level);"),
    ("charge protection is saved directly", UI, "flashed(false, [&] { return charge::apply(off); }) ?", "charge::apply(off) ?"),
    ("computers are forgotten directly", UI, "    flashed(false, [] { return ble::forget_all(); });\n", "    ble::forget_all();\n"),
    ("factory reset erases directly", UI, "flashed(ESP_FAIL, [] { return reset_config(); }) == ESP_OK", "reset_config() == ESP_OK"),
]

if __name__ == "__main__":
    os.environ["DESKHUB_PREVIEW_CASE"] = "flash_worker"
    raise SystemExit(firmware_main(M, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja"))
