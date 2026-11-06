// Test builds only (docs/desktop-app-design.md section 12, variants F to H): SmartHub's display memory on DeskHub's driver.
// The vendor BSP registers the display with two 480 x 50 draw buffers in PSRAM. The SPI DMA cannot read PSRAM, so every
// transfer asks the internal heap for a bounce buffer, and that request fails once Wi-Fi or BLE have split the heap (the
// stripes of 0.3.9 to 0.3.12). Here the registration is intercepted (-Wl,--wrap) and the two buffers are 480 x 20 lines
// of internal memory, taken once at boot, as SmartHub's esp_lvgl_port does. The BSP keeps its own panel handles, so its
// brightness, rotation and touch functions are unchanged.
//
// Variant H also intercepts the adapter's start: the LVGL task gets 12 KB of stack (8 KB overflowed in F, and SmartHub
// found 7 KB too small on this 480 x 480 panel), and that stack lives in PSRAM so it costs no internal memory.
//
// Failure modes this has to avoid (checked on the device, see docs/evidence/ble-memory.json):
//   F1 the buffers end up in PSRAM anyway (use_psram not cleared): the bounce requests come back;
//   F2 buffers too small for the panel's even-line rounding (an odd height): torn or shifted lines;
//   F3 buffers too large for internal memory (the BSP's 50 lines = 2 x 48 KB): registration fails, no screen;
//   F4 any other field of the BSP's profile changed (rotation, resolution, double buffering): a wrong picture;
//   F5 the LVGL stack too small (an overflow, the device restarts) or taken from internal memory (less for BLE).
#include "sdkconfig.h"
#include "esp_lv_adapter.h"

lv_display_t *__real_esp_lv_adapter_register_display(const esp_lv_adapter_display_config_t *config);

lv_display_t *__wrap_esp_lv_adapter_register_display(const esp_lv_adapter_display_config_t *config) {
#if CONFIG_DESKHUB_PROBE_DISPLAY_INTERNAL
    if (config) {
        esp_lv_adapter_display_config_t copy = *config;   // everything else stays the BSP's (F4)
        copy.profile.use_psram = false;                   // internal, DMA-capable memory (F1)
        copy.profile.buffer_height = 20;                  // 2 x 19.2 KB, even (F2, F3)
        return __real_esp_lv_adapter_register_display(&copy);
    }
#endif
    return __real_esp_lv_adapter_register_display(config);
}

#if CONFIG_DESKHUB_PROBE_LVGL_STACK_PSRAM
esp_err_t __real_esp_lv_adapter_init(const esp_lv_adapter_config_t *config);

esp_err_t __wrap_esp_lv_adapter_init(const esp_lv_adapter_config_t *config) {
    if (!config) return __real_esp_lv_adapter_init(config);
    esp_lv_adapter_config_t copy = *config;
    copy.task_stack_size = 12 * 1024;   // F5
    copy.stack_in_psram = true;
    return __real_esp_lv_adapter_init(&copy);
}
#endif
