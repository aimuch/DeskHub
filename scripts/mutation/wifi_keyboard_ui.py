#!/usr/bin/env python3
"""Run only the real LVGL Wi-Fi touch flow on temporary broken implementations."""
import os
from _engine import firmware_main
H = "components/ui/include/ui_keyboard.hpp"
U = "components/ui/ui_wifi.cpp"
M = [
    ("short passwords submitted", H, "length_ >= 8 &&", "length_ >= 1 &&"),
    ("released eye keeps password visible", U, "code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST || code==LV_EVENT_DEFOCUSED", "code==LV_EVENT_DEFOCUSED"),
    ("drag outside still activates key", U, "lv_obj_remove_flag(b,LV_OBJ_FLAG_PRESS_LOCK);", "lv_obj_add_flag(b,LV_OBJ_FLAG_PRESS_LOCK);"),
    ("rebuild keeps deleting", U, "redact(); delete_due=0;", "redact();"),
]
if __name__ == "__main__":
    os.environ["DESKHUB_PREVIEW_CASE"] = "wifi_keyboard"
    raise SystemExit(firmware_main(M, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja"))
