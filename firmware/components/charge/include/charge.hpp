#pragma once
// Charge protection on the device: the controller of charge_policy.hpp wired to the power chip, the clock and flash.
// Not tested on the host (it is glue); the logic it calls is, in firmware/tests/charge_test.cpp.
#include "charge_policy.hpp"
#include <vector>

namespace dh::charge {
// Loads the stored settings, then starts the controller. A switch found off is switched on again here. After board_init().
void init();
// Called by the housekeeping task, every 250 ms: one step with the newest cached power sample.
void tick();
// A copy, safe from any task. Never waits for the power chip.
Status status();
// The last writes to the charge switch, oldest first, at most 16 (rules K6). Lost at a reset.
std::vector<Event> recent_events();
// Turns protection on or off. Turning on is saved first and only then takes effect; turning off takes effect first.
// false: the settings were not valid, or not saved (when turning off, it is off anyway).
bool apply(const Settings &);
}  // namespace dh::charge
