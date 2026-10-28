#pragma once
#include <string>
#include <cstdint>
#include <ctime>
#include "esp_err.h"
namespace dh {
struct PowerState {
    std::string battery = "unknown", vbus = "unknown", charging = "unknown", reason = "尚未读取";
    // Battery level in percent, or -1 when unknown. It is the PMIC fuel gauge's own
    // estimate (AXP2101 register 0xA4), not validated against this particular cell and
    // never derived from voltage. Only set while a battery is present.
    int soc_percent = -1;
    // Current and cell temperature stay absent until validated.
    bool fault = false;
    int64_t observed_ms = 0;
};
esp_err_t board_init();
esp_err_t board_display_init();
esp_err_t board_brightness(int percent);
// Called with the adapter display lock held. Quarter turns only.
esp_err_t board_rotation(int degrees);
PowerState board_power();
bool board_rtc_read(time_t &);
bool board_rtc_write(time_t);
void board_sample_power();
}
