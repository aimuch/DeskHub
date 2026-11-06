// Draw buffer failure modes, before implementation (docs/evidence/ble-memory.json, variants F0 and H2):
// B1 the buffers stay in PSRAM: every SPI transfer asks the internal heap for a bounce buffer, and the stripes come back
//    when Wi-Fi or BLE have split it;
// B2 the buffers stay at the BSP's 50 lines (2 x 48 KB): too large for internal memory, so the display does not register;
// B3 a smaller height from the caller is raised, or an odd height reaches the panel's even-line rounding: torn lines;
// B4 any other field of the BSP's registration changes (panel, IO, rotation, resolution, double buffering, PPA, tearing);
// B5 the caller's configuration is modified in place, a null configuration crashes, or the adapter's result is lost.
#include "esp_lv_adapter_display.h"
#include <cassert>
#include <initializer_list>

extern "C" lv_display_t *__wrap_esp_lv_adapter_register_display(const esp_lv_adapter_display_config_t *);
static const esp_lv_adapter_display_config_t *original;
static uint16_t expected_height;
static lv_display_t display, *result;
static int calls;
extern "C" lv_display_t *__real_esp_lv_adapter_register_display(const esp_lv_adapter_display_config_t *cfg) {
    ++calls;
    if (!original) { assert(!cfg); return result; }
    assert(cfg && cfg != original);                                       // a copy, never the caller's (B5)
    assert(!cfg->profile.use_psram);                                      // B1
    assert(cfg->profile.buffer_height == expected_height);                // B2, B3
    assert(cfg->profile.buffer_height % 2 == 0 && cfg->profile.buffer_height >= 2);
    assert(cfg->panel == original->panel && cfg->panel_io == original->panel_io);   // B4
    assert(cfg->profile.interface == original->profile.interface && cfg->profile.rotation == original->profile.rotation);
    assert(cfg->profile.hor_res == original->profile.hor_res && cfg->profile.ver_res == original->profile.ver_res);
    assert(cfg->profile.require_double_buffer == original->profile.require_double_buffer);
    assert(cfg->profile.enable_ppa_accel == original->profile.enable_ppa_accel && cfg->tear_avoid_mode == original->tear_avoid_mode);
    return result;
}
int main() {
    int panel = 0, io = 0;
    for (uint16_t height : {50, 480, 21, 20, 15, 10, 2}) for (bool psram : {true, false}) for (bool twice : {true, false})
        for (auto *answer : {&display, static_cast<lv_display_t *>(nullptr)}) {
            esp_lv_adapter_display_config_t cfg{&panel, &io, {ESP_LV_ADAPTER_PANEL_IF_OTHER, ESP_LV_ADAPTER_ROTATE_180, 480, 480, height, psram, false, twice},
                                                ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL};
            const auto before = cfg;
            original = &cfg; result = answer;
            expected_height = height > 20 ? 20 : static_cast<uint16_t>(height & ~1u);
            calls = 0;
            assert(__wrap_esp_lv_adapter_register_display(&cfg) == answer && calls == 1);    // B5: the adapter's answer
            assert(cfg.profile.buffer_height == before.profile.buffer_height && cfg.profile.use_psram == before.profile.use_psram);
        }
    original = nullptr; result = &display; calls = 0;
    assert(__wrap_esp_lv_adapter_register_display(nullptr) == &display && calls == 1);           // B5: null passes through
}
