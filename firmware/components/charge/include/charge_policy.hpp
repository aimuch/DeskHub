#pragma once
// Charge protection (docs/charge-protection-design.md, rules R2 and R3), the pure logic. The chip is reached only through
// `Switch`, so every way the real one can fail is tested on the host in firmware/tests/charge_test.cpp.
#include "board.hpp"
#include <cmath>
#include <cstdint>
#include <string>

namespace dh::charge {
constexpr int kMinLimit = 70, kMaxLimit = 100, kMinGap = 5;
constexpr int kStopMv = 4300, kReleaseMv = 4150;                    // the hard limit of the cell voltage
constexpr float kStopTempC = 65.0f, kReleaseTempC = 55.0f;          // the hard limit of the chip temperature
constexpr int kSamplesToSwitch = 3, kMaxStopFailures = 3, kRestoreBurst = 3;
constexpr int64_t kFreshMs = 6000, kMinSwitchMs = 60000, kRestoreRetryMs = 30000;

// ---- settings: what the user chose, kept as text in its own NVS entry ----
struct Settings {
    bool enabled = false;
    int limit = 80;   // stop at this level
    int resume = 75;  // charge again at this level
};
// limit 70..100, resume 1.., and at least 5 points between them.
bool valid(const Settings &);
std::string serialize(const Settings &);            // "on|80|75"
// Anything malformed, or with numbers that are not allowed, is the default: off. "on" with bad numbers never happens.
Settings parse_settings(const std::string &);

// ---- one reading of the power chip, reduced to what a decision may use ----
struct Reading {
    bool usable = false;
    const char *why = "NO_SAMPLE";  // nullptr when usable; otherwise NO_SAMPLE, PMIC_FAULT, SAMPLE_STALE, NO_BATTERY, LEVEL_UNREADABLE
    int soc = -1;
    int mv = -1;                    // -1: not measured, or not plausible (outside 2500..4600)
    float temp_c = NAN;             // NaN: not measured, or not plausible (outside -40..150)
    int64_t observed_ms = 0;
};
Reading reading_of(const PowerState &, int64_t now_ms);

// ---- the chip's charge switch ----
enum class Write { Ok, Failed, Mismatch, Refused };  // Mismatch: written but read back different. Refused: the chip is not an AXP2101.
class Switch {
public:
    virtual ~Switch() = default;
    virtual bool read(bool &charging_enabled) = 0;
    // Writes, reads back and compares. Ok only if the chip now has the requested value.
    virtual Write set(bool charging_enabled) = 0;
};
// One line per write, successful or not (rules K6).
struct Event {
    int64_t at_ms = 0;
    bool charge = true;  // what was asked: true charge, false stop
    Write result = Write::Ok;
    const char *reason = "";
};
class Log {
public:
    virtual ~Log() = default;
    virtual void record(const Event &) = 0;
};

enum class State {
    Off,          // protection is off
    Charging,     // on, charging allowed
    Holding,      // on, stopped at the limit
    SafetyStop,   // stopped by the voltage or temperature limit
    Failsafe,     // could not decide or could not write; normal charging is back
    Unconfirmed,  // the restore of normal charging did not read back: the state of the switch is not known
};
// The words of the USB status.
const char *state_name(State);  // off, charging, holding, safety_stop, failsafe, unconfirmed
const char *write_name(Write);  // ok, failed, mismatch, refused
struct Status {
    State state = State::Off;
    const char *reason = "";
    bool enabled = false;
    int limit = 80, resume = 75;
    bool faulted = false;  // repeated failed stops: protection gives up until it is switched on again or the device restarts
    int stop_failures = 0;
    bool charging_known = false, charging = false;
};

class Controller {
public:
    Controller(Switch &, Log &);
    // Reads the switch. A switch found off is switched on again at once: the chip keeps it across a reset of the MCU.
    void start(int64_t now_ms);
    // Applies at once. Invalid numbers mean off. Switching off restores normal charging in this call.
    void configure(const Settings &, int64_t now_ms);
    // Called often (every 250 ms is fine); only a sample with a newer observation time counts as a new sample.
    void step(const PowerState &, int64_t now_ms);
    Status status() const;

private:
    Write write(bool charge, const char *reason, int64_t now_ms);
    bool restore(const char *reason, int64_t now_ms, bool counts_as_switch = true);
    bool stop(const char *reason, bool by_lock, int64_t now_ms);
    void mark_switch(int64_t now_ms);
    bool gap_ok(int64_t now_ms) const;
    void reset_policy();

    Switch &sw_;
    Log &log_;
    Settings settings_;
    bool on_ = false, started_ = false;
    bool charging_known_ = false, charging_ = true;
    bool unconfirmed_ = false;
    int64_t next_restore_ms_ = 0;
    bool policy_hold_ = false, lock_voltage_ = false, lock_temp_ = false, stopped_by_lock_ = false;
    int stop_count_ = 0, go_count_ = 0;
    int64_t last_observed_ = 0, last_switch_ms_ = 0;
    bool have_switch_ = false, first_enable_ = false;
    int stop_failures_ = 0;
    bool faulted_ = false;
    Write last_result_ = Write::Ok;
    State state_ = State::Off;
    const char *reason_ = "";
};
}  // namespace dh::charge
