#pragma once
#include <cmath>
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
    // Read-only AXP2101 ADC measurements. -1 and NaN mean "not measured": the channel is off and could not be
    // enabled, the read failed, the value is implausible, or there is nothing to measure (no battery, no USB).
    int battery_mv = -1;
    int vbus_mv = -1;
    float chip_temp_c = NAN;  // the power chip's die temperature, never the cell's
    // ADC_OK, ADC_ENABLE_FAILED, ADC_READ_FAILED or ADC_OUT_OF_RANGE; empty before a sample and on a PMIC fault.
    std::string measure_reason;
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
// The charge switch of the power chip (AXP2101 register 0x18 bit 1, docs/rules.md R3). It is the only thing here that
// can stop a battery from charging, and the only write that is not part of measuring.
enum class ChargeWrite {
    Ok,            // the chip now has the requested value
    NotIdentified, // not an AXP2101, not answering, or the board is not ready: nothing was read from 0x18 or written
    ReadFailed,    // 0x18 could not be read: nothing was written
    WriteFailed,   // the write did not go through
    NotConfirmed,  // written, but the read-back differs or could not be read
};
// The switch as the chip reports it. false means it could not be read; `enabled` is then not touched.
bool board_charge_enabled(bool &enabled);
// Read, change bit 1 only, write, read back. Nothing is written when the bit already has the value.
ChargeWrite board_set_charging(bool enable);
}
