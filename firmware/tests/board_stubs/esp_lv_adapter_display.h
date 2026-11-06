#pragma once
// Host stand-in for the adapter's display registration (display_buffers_test only): the fields the BSP sets, nothing else.
#include <cstdint>
struct lv_display_t {};
using esp_lcd_panel_handle_t = void *;
using esp_lcd_panel_io_handle_t = void *;
enum esp_lv_adapter_panel_interface_t { ESP_LV_ADAPTER_PANEL_IF_OTHER = 0, ESP_LV_ADAPTER_PANEL_IF_RGB = 1 };
enum esp_lv_adapter_rotation_t { ESP_LV_ADAPTER_ROTATE_0 = 0, ESP_LV_ADAPTER_ROTATE_90 = 1, ESP_LV_ADAPTER_ROTATE_180 = 2 };
enum esp_lv_adapter_tear_avoid_mode_t { ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE = 0, ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL = 1 };
struct esp_lv_adapter_display_profile_t {
    esp_lv_adapter_panel_interface_t interface;
    esp_lv_adapter_rotation_t rotation;
    uint16_t hor_res, ver_res;
    uint16_t buffer_height;
    bool use_psram, enable_ppa_accel, require_double_buffer;
};
struct esp_lv_adapter_display_config_t {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lv_adapter_display_profile_t profile;
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode;
};
lv_display_t *esp_lv_adapter_register_display(const esp_lv_adapter_display_config_t *config);
