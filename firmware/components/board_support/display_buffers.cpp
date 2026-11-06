// The vendor BSP registers the display with two 480 x 50 draw buffers in PSRAM. The SPI DMA cannot read PSRAM, so every
// transfer asked the internal heap for a bounce buffer, and that request failed once Wi-Fi or BLE had split the heap: the
// stripes of 0.3.9 to 0.3.12 (docs/evidence/display-corruption.md). Take two buffers of at most 20 even lines from internal,
// DMA-capable memory once, at registration, as SmartHub's esp_lvgl_port does; transfers then ask for nothing. Everything
// else stays the BSP's, so its panel handles, brightness, rotation and touch are unchanged (docs/evidence/ble-memory.json).
#include "esp_lv_adapter_display.h"
extern "C" lv_display_t *__real_esp_lv_adapter_register_display(const esp_lv_adapter_display_config_t *config);
extern "C" lv_display_t *__wrap_esp_lv_adapter_register_display(const esp_lv_adapter_display_config_t *config) {
    if (!config) return __real_esp_lv_adapter_register_display(config);
    auto internal = *config;
    internal.profile.use_psram = false;
    const uint16_t lines = internal.profile.buffer_height > 20 ? 20 : internal.profile.buffer_height;
    internal.profile.buffer_height = static_cast<uint16_t>(lines & ~1u);   // the panel draws in even lines
    return __real_esp_lv_adapter_register_display(&internal);
}
