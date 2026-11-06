#!/usr/bin/env python3
"""Mutation check for the internal draw buffers and the LVGL stack (board_support): break the code on purpose, the host
tests must go red.

Usage: scripts/mutation/display_buffers.py [name-part ...]
display_buffers_test checks display_buffers.cpp; board_cache_test checks the LVGL task configuration in board.cpp.
Works on a copy in a temporary directory. Exit 0 only if both baselines are green and every mutation is caught.
"""
from _engine import firmware_main

D = "components/board_support/display_buffers.cpp"
B = "components/board_support/board.cpp"

BUFFERS = [
    ("the buffers stay in PSRAM", D, "    internal.profile.use_psram = false;\n", ""),
    ("the BSP's 50 lines stay", D, "internal.profile.buffer_height > 20 ? 20 : internal.profile.buffer_height;", "internal.profile.buffer_height;"),
    ("a smaller height is raised", D, "internal.profile.buffer_height > 20 ? 20 : internal.profile.buffer_height;", "20;"),
    ("an odd height reaches the panel", D, "static_cast<uint16_t>(lines & ~1u);", "lines;"),
    ("the limit is 22 lines", D, "internal.profile.buffer_height > 20 ? 20 :", "internal.profile.buffer_height > 22 ? 22 :"),
    ("the caller's configuration is changed", D, "    auto internal = *config;\n", "    auto &internal = const_cast<esp_lv_adapter_display_config_t &>(*config);\n"),
    ("double buffering is dropped", D, "    internal.profile.use_psram = false;\n", "    internal.profile.use_psram = false;\n    internal.profile.require_double_buffer = false;\n"),
    ("the rotation is reset", D, "    internal.profile.use_psram = false;\n", "    internal.profile.use_psram = false;\n    internal.profile.rotation = {};\n"),
    ("the adapter's answer is lost", D, "    return __real_esp_lv_adapter_register_display(&internal);", "    (void)__real_esp_lv_adapter_register_display(&internal);\n    return nullptr;"),
    ("a null configuration is not passed on", D, "    if (!config) return __real_esp_lv_adapter_register_display(config);", "    if (!config) return nullptr;"),
]
STACK = [
    ("the LVGL stack stays 8 KB", B, "    adapter.task_stack_size = 12 * 1024;\n", "    adapter.task_stack_size = ESP_LV_ADAPTER_DEFAULT_STACK_SIZE;\n"),
    ("the LVGL stack is internal", B, "    adapter.stack_in_psram = true;\n", ""),
]


def main():
    first = firmware_main(BUFFERS, "display_buffers_test")
    second = firmware_main(STACK, "board_cache_test")
    return 1 if first or second else 0


if __name__ == "__main__":
    raise SystemExit(main())
