#pragma once
#include "driver/i2c_master.h"
#if defined(DESKHUB_TEST_REAL_LVGL)
#include "lvgl.h"
#else
struct lv_display_t {};
struct lv_indev_t {};
struct lv_obj_t {};
enum lv_display_rotation_t { LV_DISPLAY_ROTATION_0,LV_DISPLAY_ROTATION_90,LV_DISPLAY_ROTATION_180,LV_DISPLAY_ROTATION_270 };
void lv_display_set_rotation(lv_display_t *,lv_display_rotation_t);
void lv_indev_wait_release(lv_indev_t *);
lv_obj_t *lv_screen_active();
void lv_obj_invalidate(lv_obj_t *);
#endif
enum esp_lv_adapter_auto_sleep_mode_t { ESP_LV_ADAPTER_AUTO_SLEEP_MODE_DISABLED=0 };
struct esp_lv_adapter_auto_sleep_callbacks_t {
    esp_err_t (*on_enter_sleep)(void *);
    esp_err_t (*on_exit_sleep)(void *);
    void *user_ctx;
};
struct esp_lv_adapter_config_t {
    uint32_t task_stack_size,task_priority;
    int task_core_id;
    uint32_t tick_period_ms,task_min_delay_ms,task_max_delay_ms;
    bool stack_in_psram;
    struct {
        bool enable;
        esp_lv_adapter_auto_sleep_mode_t mode;
        uint32_t idle_timeout_ms;
        esp_lv_adapter_auto_sleep_callbacks_t callbacks;
    } auto_sleep;
};
constexpr uint32_t ESP_LV_ADAPTER_DEFAULT_STACK_SIZE=8*1024,
    ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY=6,
    ESP_LV_ADAPTER_DEFAULT_TICK_PERIOD_MS=1,
    ESP_LV_ADAPTER_DEFAULT_TASK_MIN_DELAY_MS=1,
    ESP_LV_ADAPTER_DEFAULT_TASK_MAX_DELAY_MS=15,
    ESP_LV_ADAPTER_DEFAULT_AUTO_SLEEP_TIMEOUT_MS=5000;
constexpr int ESP_LV_ADAPTER_DEFAULT_TASK_CORE_ID=-1;
enum esp_lv_adapter_rotation_t { ESP_LV_ADAPTER_ROTATE_0=0 };
enum esp_lv_adapter_tear_avoid_mode_t { ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE=0 };
struct bsp_display_cfg_t {
    esp_lv_adapter_config_t lv_adapter_cfg;
    esp_lv_adapter_rotation_t rotation;
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode;
    struct { unsigned swap_xy,mirror_x,mirror_y; } touch_flags;
};
esp_err_t bsp_i2c_init();
i2c_master_bus_handle_t bsp_i2c_get_handle();
lv_display_t *bsp_display_start();
lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *);
lv_indev_t *bsp_display_get_input_dev();
