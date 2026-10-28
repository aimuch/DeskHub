#include "power_wire.hpp"
#include <cmath>
#include <initializer_list>
#include <string>

namespace dh {
namespace {
// The plausible ranges of the AXP2101 readings (docs/rules.md R3). The board already filters them; they are checked
// again here because this is the trust boundary to the Agent.
constexpr int kBatteryMinMv = 2500, kBatteryMaxMv = 4600, kUsbMinMv = 3000, kUsbMaxMv = 6500;
constexpr double kChipMinC = -40.0, kChipMaxC = 150.0;

std::string one_of(const std::string &value, std::initializer_list<const char *> set) {
    for (const char *s : set)
        if (value == s) return value;
    return "unknown";  // a word the Agent does not know is not forwarded
}
bool in_range(int v, int lo, int hi) { return v >= lo && v <= hi; }
}  // namespace

bool power_sample_fresh(const PowerState &p, int64_t now_ms) {
    return p.observed_ms > 0 && now_ms >= p.observed_ms && now_ms - p.observed_ms <= kPowerFreshMs;
}

cJSON *power_report_json(const PowerState &p, int64_t now_ms) {
    const bool fresh = power_sample_fresh(p, now_ms);
    const std::string vbus = fresh ? one_of(p.vbus, {"present", "absent", "unknown"}) : "unknown";
    const std::string battery = fresh ? one_of(p.battery, {"present", "absent", "unknown"}) : "unknown";
    const std::string charging = fresh ? one_of(p.charging, {"charging", "full", "idle", "unknown"}) : "unknown";
    const bool battery_present = battery == "present", vbus_present = vbus == "present";

    // The wire state of the ADC measurement. Only a fresh sample without a fault can be "ok" or a named failure.
    std::string measurement = "not_measured";
    if (fresh && !p.fault) {
        if (p.measure_reason == "ADC_OK") measurement = "ok";
        else if (p.measure_reason == "ADC_ENABLE_FAILED") measurement = "adc_enable_failed";
        else if (p.measure_reason == "ADC_READ_FAILED") measurement = "adc_read_failed";
        else if (p.measure_reason == "ADC_OUT_OF_RANGE") measurement = "adc_out_of_range";
    }
    if (measurement == "ok") {
        const bool plausible = (!battery_present || in_range(p.battery_mv, kBatteryMinMv, kBatteryMaxMv)) &&
                               (!vbus_present || in_range(p.vbus_mv, kUsbMinMv, kUsbMaxMv)) &&
                               std::isfinite(p.chip_temp_c) && p.chip_temp_c >= kChipMinC && p.chip_temp_c <= kChipMaxC;
        if (!plausible) measurement = "adc_out_of_range";
    }

    auto *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "vbus", vbus.c_str());
    cJSON_AddStringToObject(o, "battery", battery.c_str());
    cJSON_AddStringToObject(o, "charging", charging.c_str());
    cJSON_AddBoolToObject(o, "fault", p.fault);
    // The level is the fuel gauge's own estimate, so a failed ADC does not hide it; a fault or no battery does.
    if (fresh && !p.fault && battery_present && p.soc_percent >= 0 && p.soc_percent <= 100) cJSON_AddNumberToObject(o, "soc_percent", p.soc_percent);
    else cJSON_AddNullToObject(o, "soc_percent");
    if (measurement == "ok" && battery_present) cJSON_AddNumberToObject(o, "battery_mv", p.battery_mv);
    else cJSON_AddNullToObject(o, "battery_mv");
    if (measurement == "ok" && vbus_present) cJSON_AddNumberToObject(o, "vbus_mv", p.vbus_mv);
    else cJSON_AddNullToObject(o, "vbus_mv");
    if (measurement == "ok") {
        double tenths = std::round(static_cast<double>(p.chip_temp_c) * 10.0) / 10.0;  // the power chip, never the cell
        if (tenths == 0) tenths = 0;                                                    // no negative zero
        cJSON_AddNumberToObject(o, "chip_temp_c", tenths);
    } else cJSON_AddNullToObject(o, "chip_temp_c");
    cJSON_AddStringToObject(o, "measurement", measurement.c_str());
    return o;
}
}  // namespace dh
