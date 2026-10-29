#!/usr/bin/env python3
"""Mutation check for charge protection: break the code on purpose, charge_test must go red.

Usage: scripts/mutation/charge_firmware.py [name-part ...]
Works on a copy in a temporary directory. The working tree is never touched.
Exit 0 only if the baseline is green and every mutation is caught.
"""
from _engine import firmware_main

CP = "components/charge/charge_policy.cpp"
CH = "components/charge/include/charge_policy.hpp"
M = [
    # settings
    ("limit of 69 allowed", CP, "s.limit >= kMinLimit", "s.limit >= kMinLimit - 1"),
    ("limit of 101 allowed", CP, "s.limit <= kMaxLimit", "s.limit <= kMaxLimit + 1"),
    ("a gap of 4 points allowed", CP, "s.limit - s.resume >= kMinGap", "s.limit - s.resume >= kMinGap - 1"),
    ("resume of 0 allowed", CP, "s.resume >= 1 &&", "s.resume >= 0 &&"),
    ("negative resume allowed", CP, "s.resume >= 1 && ", ""),
    ("on and off swapped in the text", CP, 's.enabled ? "on" : "off"', 's.enabled ? "off" : "on"'),
    ("a number with a leading zero", CP, "|| text[0] == '0') return false;", ") return false;"),
    ("a character that is not a digit", CP, "if (c < '0' || c > '9') return false;", ""),
    ("any word for off", CP, 'else if (mode != "off") return Settings{};', ""),
    ("numbers that are not allowed", CP, "|| !valid(s)) return Settings{};", ") return Settings{};"),
    ("invalid settings kept for the status", CP, "settings_ = valid(s) ? s : Settings{};", "settings_ = s;"),
    # readings
    ("no sample is not told from stale", CP, "if (p.observed_ms <= 0)", "if (p.observed_ms < 0)"),
    ("a chip fault is not noticed", CP, 'if (p.fault) { r.why = "PMIC_FAULT"; return r; }', ""),
    ("6 s old is stale", CP, "now_ms - p.observed_ms > kFreshMs", "now_ms - p.observed_ms >= kFreshMs"),
    ("6 s and 1 ms old is fresh", CP, "now_ms - p.observed_ms > kFreshMs", "now_ms - p.observed_ms > kFreshMs + 1"),
    ("a sample from the future is used", CP, "p.observed_ms > now_ms || ", ""),
    ("an unknown battery is used", CP, 'p.battery != "present"', 'p.battery == "absent"'),
    ("a level with an error reason is used", CP, 'p.reason != "SOC_PMIC_ESTIMATE" || ', ""),
    ("a level of -1 is used", CP, "p.soc_percent < 0 ||", "p.soc_percent < -1 ||"),
    ("a level of 101 is used", CP, "p.soc_percent > 100", "p.soc_percent > 101"),
    ("2499 mV is plausible", CP, "p.battery_mv >= 2500", "p.battery_mv >= 2499"),
    ("4601 mV is plausible", CP, "p.battery_mv <= 4600", "p.battery_mv <= 4601"),
    ("-41 C is plausible", CP, "p.chip_temp_c >= -40.0f", "p.chip_temp_c >= -41.0f"),
    ("151 C is plausible", CP, "p.chip_temp_c <= 150.0f", "p.chip_temp_c <= 151.0f"),
    # samples and counts
    ("one sample counted again and again", CP, "power.observed_ms > last_observed_;", "power.observed_ms >= last_observed_;"),
    ("every step is a new sample", CP, "const bool new_sample = power.observed_ms > last_observed_;", "const bool new_sample = true;"),
    ("stop above the limit, not at it", CP, "rd.soc >= settings_.limit ? stop_count_ + 1 : 0", "rd.soc > settings_.limit ? stop_count_ + 1 : 0"),
    ("two samples are enough to stop", CP, "stop_count_ >= kSamplesToSwitch &&", "stop_count_ >= kSamplesToSwitch - 1 &&"),
    ("four samples to stop", CP, "stop_count_ >= kSamplesToSwitch &&", "stop_count_ >= kSamplesToSwitch + 1 &&"),
    ("two samples are enough to resume", CP, "go_count_ >= kSamplesToSwitch &&", "go_count_ >= kSamplesToSwitch - 1 &&"),
    ("a low sample does not clear the stop count", CP, "rd.soc >= settings_.limit ? stop_count_ + 1 : 0", "rd.soc >= settings_.limit ? stop_count_ + 1 : stop_count_"),
    ("resume below the line, not at it", CP, "rd.soc <= settings_.resume ? go_count_ + 1 : 0", "rd.soc < settings_.resume ? go_count_ + 1 : 0"),
    ("resume one point above the line", CP, "rd.soc <= settings_.resume ? go_count_ + 1 : 0", "rd.soc <= settings_.resume + 1 ? go_count_ + 1 : 0"),
    ("a high sample does not clear the resume count", CP, "rd.soc <= settings_.resume ? go_count_ + 1 : 0", "rd.soc <= settings_.resume ? go_count_ + 1 : go_count_"),
    # the gap between switches
    ("no gap before a stop", CP, "(first_enable_ || gap_ok(now_ms))", "true"),
    ("no gap before a resume", CP, "go_count_ >= kSamplesToSwitch && gap_ok(now_ms))", "go_count_ >= kSamplesToSwitch)"),
    ("the gap is 59 s", CH, "kMinSwitchMs = 60000", "kMinSwitchMs = 59000"),
    ("the gap is 61 s", CH, "kMinSwitchMs = 60000", "kMinSwitchMs = 61000"),
    ("the first enable is held back by the gap", CP, "(first_enable_ || gap_ok(now_ms))", "(gap_ok(now_ms))"),
    ("the first enable is never used up", CP, "    have_switch_ = true;\n    first_enable_ = false;", "    have_switch_ = true;"),
    ("a new enable is not a first enable", CP, "else if (!was_on) first_enable_ = true;", "else if (!was_on) { }"),
    ("the boot restore counts as a switch", CP, 'restore("BOOT_FOUND_STOPPED", now_ms, false)', 'restore("BOOT_FOUND_STOPPED", now_ms, true)'),
    # switching off, a new limit
    ("switching off does not restore at once", CP, '        if (!charging_) restore("PROTECTION_OFF", now_ms);\n        return;\n    }\n    state_ = charging_', '        (void)now_ms;\n        return;\n    }\n    state_ = charging_'),
    ("a new limit forgets that the charge is stopped", CP, "policy_hold_ = on_ && charging_known_ && !charging_ && !stopped_by_lock_;", "policy_hold_ = false;"),
    ("a new enable keeps the failure count", CP, "    stop_failures_ = 0;\n    if (!on_)", "    if (!on_)"),
    ("a new enable keeps the fault", CP, "    faulted_ = false;\n    stop_failures_ = 0;", "    stop_failures_ = 0;"),
    # hard limits
    ("4300 mV does not stop", CP, "rd.mv >= kStopMv", "rd.mv > kStopMv"),
    ("4300 mV limit is 4301", CH, "kStopMv = 4300", "kStopMv = 4301"),
    ("4150 mV releases", CP, "rd.mv < kReleaseMv", "rd.mv <= kReleaseMv"),
    ("release at 4151 mV", CH, "kReleaseMv = 4150", "kReleaseMv = 4151"),
    ("the voltage lock is never released", CP, "else if (rd.mv < kReleaseMv) lock_voltage_ = false;", ""),
    ("65 C does not stop", CP, "rd.temp_c >= kStopTempC", "rd.temp_c > kStopTempC"),
    ("the temperature limit is 65.1 C", CH, "kStopTempC = 65.0f", "kStopTempC = 65.1f"),
    ("55 C releases", CP, "rd.temp_c < kReleaseTempC", "rd.temp_c <= kReleaseTempC"),
    ("release at 55.1 C", CH, "kReleaseTempC = 55.0f", "kReleaseTempC = 55.1f"),
    ("the temperature lock is never released", CP, "else if (rd.temp_c < kReleaseTempC) lock_temp_ = false;", ""),
    ("one release frees both locks", CP, "else if (rd.temp_c < kReleaseTempC) lock_temp_ = false;", "else if (rd.temp_c < kReleaseTempC) lock_temp_ = lock_voltage_ = false;"),
    ("no reading of the voltage frees the lock", CP, "if (rd.mv >= 0) {", "if (true) {"),
    ("a lock waits for the gap", CP, "if (lock && charging_) {", "if (lock && charging_ && gap_ok(now_ms)) {"),
    ("a lock released while it still holds", CP, "} else if (!lock && !policy_hold_ && !charging_ && gap_ok(now_ms)) {", "} else if (!policy_hold_ && !charging_ && gap_ok(now_ms)) {"),
    ("a release does not wait for the gap", CP, "} else if (!lock && !policy_hold_ && !charging_ && gap_ok(now_ms)) {", "} else if (!lock && !policy_hold_ && !charging_) {"),
    ("the reason of a lock is swapped", CP, 'stop(lock_voltage_ ? "VOLTAGE_LIMIT" : "TEMPERATURE_LIMIT", true, now_ms)', 'stop(lock_voltage_ ? "TEMPERATURE_LIMIT" : "VOLTAGE_LIMIT", true, now_ms)'),
    ("a lock stop is not told from a level stop", CP, 'stop(lock_voltage_ ? "VOLTAGE_LIMIT" : "TEMPERATURE_LIMIT", true, now_ms)', 'stop(lock_voltage_ ? "VOLTAGE_LIMIT" : "TEMPERATURE_LIMIT", false, now_ms)'),
    ("the stopped state is called charging", CP, 'else if (!charging_) { state_ = State::Holding;', 'else if (!charging_) { state_ = State::Charging;'),
    # failure direction
    ("an unreadable level does not restore", CP, "if (!charging_ && !restore(rd.why, now_ms)) return;", ""),
    ("an unreadable level keeps the policy", CP, "    if (!rd.usable) {\n        reset_policy();", "    if (!rd.usable) {"),
    ("an unreadable level shows no failsafe", CP, "        reason_ = rd.why;\n        return;", '        reason_ = "";\n        return;'),
    ("a failed stop is not undone", CP, "if (restore(why, now_ms)) {", "if (false && restore(why, now_ms)) {"),
    ("failed stops are not counted", CP, "    ++stop_failures_;\n", ""),
    ("the fault comes after 4 failures", CP, "stop_failures_ >= kMaxStopFailures", "stop_failures_ > kMaxStopFailures"),
    ("the fault comes after 2 failures", CP, "stop_failures_ >= kMaxStopFailures", "stop_failures_ >= kMaxStopFailures - 1"),
    ("a good stop keeps the failure count", CP, "        stop_failures_ = 0;\n        stopped_by_lock_ = by_lock;", "        stopped_by_lock_ = by_lock;"),
    ("a mismatch is called a failure", CP, 'last_result_ == Write::Mismatch ? "STOP_MISMATCH" : "STOP_FAILED"', 'last_result_ == Write::Mismatch ? "STOP_FAILED" : "STOP_MISMATCH"'),
    ("the result of the write is lost", CP, "    last_result_ = w;\n", ""),
    ("2 tries to restore", CH, "kRestoreBurst = 3", "kRestoreBurst = 2"),
    ("4 tries to restore", CH, "kRestoreBurst = 3", "kRestoreBurst = 4"),
    ("a retry after 29 s", CH, "kRestoreRetryMs = 30000", "kRestoreRetryMs = 29000"),
    ("a retry after 31 s", CH, "kRestoreRetryMs = 30000", "kRestoreRetryMs = 31000"),
    ("a retry at every step", CP, "if (now_ms < next_restore_ms_) return;", ""),
    ("an unsure switch is called failsafe", CP, "    state_ = State::Unconfirmed;\n    reason_ = \"SWITCH_UNCONFIRMED\";", "    state_ = State::Failsafe;\n    reason_ = \"SWITCH_UNCONFIRMED\";"),
    ("an unsure switch is called charging", CP, "s.charging = charging_known_ && charging_;", "s.charging = charging_;"),
    ("a retry that works is not a switch", CP, "            unconfirmed_ = false;\n            mark_switch(now_ms);", "            unconfirmed_ = false;"),
    ("no record of a write", CP, "    log_.record(Event{now_ms, charge, w, reason});\n", "    (void)now_ms; (void)reason; (void)log_;\n"),
    # boot
    ("a switch found off is left off", CP, 'if (!enabled) restore("BOOT_FOUND_STOPPED", now_ms, false);', ""),
    ("an unreadable switch at boot is not told", CP, "if (!sw_.read(enabled)) {", "if (false) {"),
]


if __name__ == "__main__":
    raise SystemExit(firmware_main(M, "charge_test"))
