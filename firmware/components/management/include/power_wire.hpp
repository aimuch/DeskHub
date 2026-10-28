#pragma once
// The "power" object that the device sends to the Agent (device report) and to the USB tool (status). Pure: it reads a
// PowerState and the clock and builds JSON, nothing else. The rules (docs/rules.md K3: unknown stays unknown, stale
// stays stale, nothing invented) are checked on the host in firmware/tests/power_wire_test.cpp, against the same
// rules the Agent enforces, so the firmware never sends a report the Agent would reject.
#include "board.hpp"
#include "cJSON.h"
#include <cstdint>

namespace dh {
constexpr int64_t kPowerFreshMs = 6000;
// A sample older than 6 s, from the future, or never taken is stale.
bool power_sample_fresh(const PowerState &, int64_t now_ms);
// Always the same nine members; unknown numbers are null. The caller owns the result.
cJSON *power_report_json(const PowerState &, int64_t now_ms);
}  // namespace dh
