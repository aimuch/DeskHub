#include "charge_policy.hpp"

namespace dh::charge {
// ---- settings ----
bool valid(const Settings &s) {
    return s.limit >= kMinLimit && s.limit <= kMaxLimit && s.resume >= 1 && s.limit - s.resume >= kMinGap;
}
std::string serialize(const Settings &s) {
    return std::string(s.enabled ? "on" : "off") + "|" + std::to_string(s.limit) + "|" + std::to_string(s.resume);
}
namespace {
// One to three digits, no sign, no leading zero, nothing else.
bool number(const std::string &text, int &out) {
    if (text.empty() || text.size() > 3 || text[0] == '0') return false;
    int value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}
}  // namespace
Settings parse_settings(const std::string &text) {
    const size_t a = text.find('|');
    const size_t b = a == std::string::npos ? a : text.find('|', a + 1);
    if (a == std::string::npos || b == std::string::npos || text.find('|', b + 1) != std::string::npos) return Settings{};
    const std::string mode = text.substr(0, a);
    Settings s;
    if (mode == "on") s.enabled = true;
    else if (mode != "off") return Settings{};
    if (!number(text.substr(a + 1, b - a - 1), s.limit) || !number(text.substr(b + 1), s.resume) || !valid(s)) return Settings{};
    return s;
}

// ---- readings ----
Reading reading_of(const PowerState &p, int64_t now_ms) {
    Reading r;
    r.observed_ms = p.observed_ms;
    if (p.observed_ms <= 0) { r.why = "NO_SAMPLE"; return r; }
    if (p.fault) { r.why = "PMIC_FAULT"; return r; }
    if (p.observed_ms > now_ms || now_ms - p.observed_ms > kFreshMs) { r.why = "SAMPLE_STALE"; return r; }
    if (p.battery != "present") { r.why = "NO_BATTERY"; return r; }
    if (p.reason != "SOC_PMIC_ESTIMATE" || p.soc_percent < 0 || p.soc_percent > 100) { r.why = "LEVEL_UNREADABLE"; return r; }
    r.usable = true;
    r.why = nullptr;
    r.soc = p.soc_percent;
    if (p.battery_mv >= 2500 && p.battery_mv <= 4600) r.mv = p.battery_mv;
    if (!std::isnan(p.chip_temp_c) && p.chip_temp_c >= -40.0f && p.chip_temp_c <= 150.0f) r.temp_c = p.chip_temp_c;
    return r;
}

const char *state_name(State s) {
    switch (s) {
    case State::Off: return "off";
    case State::Charging: return "charging";
    case State::Holding: return "holding";
    case State::SafetyStop: return "safety_stop";
    case State::Failsafe: return "failsafe";
    case State::Unconfirmed: return "unconfirmed";
    }
    return "unknown";
}
const char *write_name(Write w) {
    switch (w) {
    case Write::Ok: return "ok";
    case Write::Failed: return "failed";
    case Write::Mismatch: return "mismatch";
    case Write::Refused: return "refused";
    }
    return "unknown";
}

// ---- the controller ----
Controller::Controller(Switch &sw, Log &log) : sw_(sw), log_(log) {}

Write Controller::write(bool charge, const char *reason, int64_t now_ms) {
    const Write w = sw_.set(charge);
    last_result_ = w;
    log_.record(Event{now_ms, charge, w, reason});
    if (w == Write::Ok) { charging_ = charge; charging_known_ = true; }
    return w;
}
void Controller::mark_switch(int64_t now_ms) {
    last_switch_ms_ = now_ms;
    have_switch_ = true;
    first_enable_ = false;
}
bool Controller::gap_ok(int64_t now_ms) const { return !have_switch_ || now_ms - last_switch_ms_ >= kMinSwitchMs; }
void Controller::reset_policy() {
    policy_hold_ = lock_voltage_ = lock_temp_ = false;
    stop_count_ = go_count_ = 0;
}

// Normal charging again, up to kRestoreBurst tries. If none reads back, the state of the switch is not known.
bool Controller::restore(const char *reason, int64_t now_ms, bool counts_as_switch) {
    for (int i = 0; i < kRestoreBurst; ++i) {
        if (write(true, reason, now_ms) == Write::Ok) {
            if (counts_as_switch) mark_switch(now_ms);
            return true;
        }
    }
    unconfirmed_ = true;
    charging_known_ = false;
    next_restore_ms_ = now_ms + kRestoreRetryMs;
    state_ = State::Unconfirmed;
    reason_ = "SWITCH_UNCONFIRMED";
    return false;
}

void Controller::start(int64_t now_ms) {
    started_ = true;
    bool enabled = false;
    if (!sw_.read(enabled)) {
        charging_known_ = false;
        restore("BOOT_UNREADABLE", now_ms, false);
    } else {
        charging_known_ = true;
        charging_ = enabled;
        if (!enabled) restore("BOOT_FOUND_STOPPED", now_ms, false);
    }
    if (!unconfirmed_) state_ = on_ ? State::Charging : State::Off;
}

void Controller::configure(const Settings &s, int64_t now_ms) {
    const bool was_on = on_;
    on_ = s.enabled && valid(s);
    settings_ = valid(s) ? s : Settings{};
    settings_.enabled = on_;
    stop_count_ = go_count_ = 0;
    lock_voltage_ = lock_temp_ = false;
    faulted_ = false;
    stop_failures_ = 0;
    if (!on_) first_enable_ = false;
    else if (!was_on) first_enable_ = true;
    policy_hold_ = on_ && charging_known_ && !charging_ && !stopped_by_lock_;
    if (!started_ || unconfirmed_) return;
    if (!on_) {
        state_ = State::Off;
        reason_ = "";
        if (!charging_) restore("PROTECTION_OFF", now_ms);
        return;
    }
    state_ = charging_ ? State::Charging : State::Holding;
    reason_ = "";
}

// A stop was decided. A failed stop is undone at once, and three in a row end the attempts. true only if it took.
bool Controller::stop(const char *reason, bool by_lock, int64_t now_ms) {
    if (write(false, reason, now_ms) == Write::Ok) {
        mark_switch(now_ms);
        stop_failures_ = 0;
        stopped_by_lock_ = by_lock;
        return true;
    }
    const char *why = last_result_ == Write::Mismatch ? "STOP_MISMATCH" : "STOP_FAILED";
    ++stop_failures_;
    reset_policy();
    if (stop_failures_ >= kMaxStopFailures) faulted_ = true;
    if (restore(why, now_ms)) {
        state_ = State::Failsafe;
        reason_ = faulted_ ? "REPEATED_FAILURE" : why;
    }
    return false;
}

void Controller::step(const PowerState &power, int64_t now_ms) {
    if (!started_) return;
    const Reading rd = reading_of(power, now_ms);
    const bool new_sample = power.observed_ms > last_observed_;
    if (new_sample) last_observed_ = power.observed_ms;

    // A restore that did not read back comes first, and nothing else happens until it has. One try every 30 s.
    if (unconfirmed_) {
        if (now_ms < next_restore_ms_) return;
        if (write(true, "RETRY_RESTORE", now_ms) == Write::Ok) {
            unconfirmed_ = false;
            mark_switch(now_ms);
            state_ = State::Failsafe;
            reason_ = "RESTORED_AFTER_FAILURE";
        } else {
            next_restore_ms_ = now_ms + kRestoreRetryMs;
        }
        return;
    }
    if (!on_) {
        reset_policy();
        state_ = State::Off;
        reason_ = "";
        return;
    }
    if (faulted_) {
        state_ = State::Failsafe;
        reason_ = "REPEATED_FAILURE";
        return;
    }
    // What is needed for a decision cannot be read: normal charging, at once, and the policy starts again.
    if (!rd.usable) {
        reset_policy();
        if (!charging_ && !restore(rd.why, now_ms)) return;
        state_ = State::Failsafe;
        reason_ = rd.why;
        return;
    }

    if (new_sample) {
        // The hard limits: a value that is not measured leaves its lock as it is.
        if (rd.mv >= 0) {
            if (rd.mv >= kStopMv) lock_voltage_ = true;
            else if (rd.mv < kReleaseMv) lock_voltage_ = false;
        }
        if (!std::isnan(rd.temp_c)) {
            if (rd.temp_c >= kStopTempC) lock_temp_ = true;
            else if (rd.temp_c < kReleaseTempC) lock_temp_ = false;
        }
        if (!policy_hold_) {
            go_count_ = 0;
            stop_count_ = rd.soc >= settings_.limit ? stop_count_ + 1 : 0;
        } else {
            stop_count_ = 0;
            go_count_ = rd.soc <= settings_.resume ? go_count_ + 1 : 0;
        }
    }
    // The policy decides after three samples, and not inside the gap of the last switch (the first enable is exempt).
    if (!policy_hold_ && stop_count_ >= kSamplesToSwitch && (first_enable_ || gap_ok(now_ms))) {
        policy_hold_ = true;
        stop_count_ = go_count_ = 0;
    } else if (policy_hold_ && go_count_ >= kSamplesToSwitch && gap_ok(now_ms)) {
        policy_hold_ = false;
        stop_count_ = go_count_ = 0;
    }

    const bool lock = lock_voltage_ || lock_temp_;
    if (lock && charging_) {                       // a lock stops at once: no count, no gap
        if (!stop(lock_voltage_ ? "VOLTAGE_LIMIT" : "TEMPERATURE_LIMIT", true, now_ms)) return;
    } else if (!lock && policy_hold_ && charging_) {
        if (!stop("LEVEL_AT_LIMIT", false, now_ms)) return;
    } else if (!lock && !policy_hold_ && !charging_ && gap_ok(now_ms)) {
        if (!restore(stopped_by_lock_ ? "LIMIT_RELEASED" : "LEVEL_AT_RESUME", now_ms)) return;
    }
    if (lock && !charging_) { state_ = State::SafetyStop; reason_ = lock_voltage_ ? "VOLTAGE_LIMIT" : "TEMPERATURE_LIMIT"; }
    else if (!charging_) { state_ = State::Holding; reason_ = "LEVEL_AT_LIMIT"; }
    else { state_ = State::Charging; reason_ = ""; }
}

Status Controller::status() const {
    Status s;
    s.state = state_;
    s.reason = reason_;
    s.enabled = on_;
    s.limit = settings_.limit;
    s.resume = settings_.resume;
    s.faulted = faulted_;
    s.stop_failures = stop_failures_;
    s.charging_known = charging_known_;
    s.charging = charging_known_ && charging_;
    return s;
}
}  // namespace dh::charge
