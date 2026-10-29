// Failure modes of charge protection (N1), written before the code (docs/charge-protection-design.md section 4).
// All pure logic: the chip is a fake switch that can fail in every way the real one can.
//
// S  Settings: limits, the 5 point gap, the stored text, a bad text is "off" and never "on".
// R  Readings: stale, absent, unreadable or implausible values are not used; one sample is never counted twice.
// P  Policy: three new samples, the 60 s gap, the dead band, first enable, switching off, a new limit.
// H  Hard limits (4.30 V, 65 C): at once, whatever the level says; separate locks; only while protection is on.
// F  Failure direction: unreadable data, a failed or mismatching write, repeated failures, a failed restore.
// B  Boot: the chip keeps its charge switch across a reset of the MCU, so a switch found off is restored first.
// L  Log: every write is recorded with request, result, reason and time; no write, no record.
// I  A random walk (a million steps) over every input: five invariants that must never break.
#include "charge_policy.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

using namespace dh;
using namespace dh::charge;

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++failures;                                                               \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                             \
    } while (0)

static bool is(const char *a, const char *b) { return std::strcmp(a, b) == 0; }

// ---- the world ----
struct FakeSwitch : Switch {
    bool charging = true;      // what the chip really does
    bool readable = true;      // the chip answers at all
    std::deque<Write> script;  // results for the next writes, front first; empty means Ok
    int sets = 0;
    std::vector<bool> requests;
    bool read(bool &value) override {
        if (!readable) return false;
        value = charging;
        return true;
    }
    Write set(bool enable) override {
        ++sets;
        requests.push_back(enable);
        Write w = readable ? Write::Ok : Write::Failed;
        if (!script.empty()) { w = script.front(); script.pop_front(); }
        if (w == Write::Ok) charging = enable;
        return w;
    }
};
struct FakeLog : Log {
    std::vector<Event> events;
    void record(const Event &e) override { events.push_back(e); }
};

static PowerState power(int soc, int64_t observed_ms, int mv = 4000, float temp = 30.0f) {
    PowerState p;
    p.battery = "present";
    p.vbus = "present";
    p.charging = "charging";
    p.reason = "SOC_PMIC_ESTIMATE";
    p.soc_percent = soc;
    p.battery_mv = mv;
    p.chip_temp_c = temp;
    p.measure_reason = "ADC_OK";
    p.observed_ms = observed_ms;
    return p;
}

struct Rig {
    FakeSwitch sw;
    FakeLog log;
    Controller c{sw, log};
    int64_t t = 100000;
    PowerState last = power(50, 99900);
    explicit Rig(bool charging = true, bool enabled_at_boot = false, bool start = true) {
        sw.charging = charging;
        if (enabled_at_boot) configure(true, 80, 75);
        if (start) c.start(t);
    }
    void configure(bool on, int limit = 80, int resume = 75) {
        Settings s;
        s.enabled = on;
        s.limit = limit;
        s.resume = resume;
        c.configure(s, t);
    }
    void enable(int limit = 80, int resume = 75) { configure(true, limit, resume); }
    void disable() { configure(false); }
    // One new sample, two seconds after the last step.
    void sample(int soc, int mv = 4000, float temp = 30.0f) {
        t += 2000;
        last = power(soc, t - 100, mv, temp);
        c.step(last, t);
    }
    void feed(int n, int soc, int mv = 4000, float temp = 30.0f) { for (int i = 0; i < n; ++i) sample(soc, mv, temp); }
    // Time passes and the old sample is all there is.
    void age(int64_t ms) {
        t += ms;
        c.step(last, t);
    }
    bool stopped() const { return !sw.charging; }
    int stops() const { int n = 0; for (bool r : sw.requests) n += r ? 0 : 1; return n; }
};

// ---- S ----
static void s_settings() {
    Settings s;
    s.enabled = true;
    s.limit = 69; s.resume = 60; CHECK(!valid(s));       // S1
    s.limit = 70; s.resume = 65; CHECK(valid(s));
    s.limit = 100; s.resume = 95; CHECK(valid(s));
    s.limit = 101; s.resume = 95; CHECK(!valid(s));
    s.limit = 80; s.resume = 76; CHECK(!valid(s));       // S2: a gap of 4
    s.resume = 75; CHECK(valid(s));
    s.resume = 0; CHECK(!valid(s));
    s.resume = -5; CHECK(!valid(s));
    s.resume = 1; CHECK(valid(s));
    s.limit = 80; s.resume = 80; CHECK(!valid(s));
    s.resume = 90; CHECK(!valid(s));

    const Settings samples[] = {{true, 80, 75}, {false, 90, 85}, {true, 70, 65}, {true, 100, 50}, {false, 80, 75}};
    for (const Settings &a : samples) {                  // S3
        const Settings b = parse_settings(serialize(a));
        CHECK(b.enabled == a.enabled && b.limit == a.limit && b.resume == a.resume);
    }
    CHECK(serialize(Settings{true, 80, 75}) == "on|80|75");
    CHECK(serialize(Settings{false, 90, 85}) == "off|90|85");

    const char *bad[] = {"", "on", "on|80", "on|80|75|1", "on|eighty|75", "on|-80|75", "on|80|-75",
                         "on|99999999999999999999|75", " on|80|75", "on|80|75 ", "ON|80|75", "yes|80|75", "on||75",
                         "on|80|", "|||", "on|80|75\n", "on|080|75", "on|+80|75", "on|80|75|", "on 80 75", "on|7:|75", "on|80|6:", "yes|90|85"};
    for (const char *text : bad) {                       // S4
        const Settings d = parse_settings(text);
        CHECK(!d.enabled && d.limit == 80 && d.resume == 75);
    }
    const char *invalid_numbers[] = {"on|69|60", "on|80|78", "on|80|0", "on|101|90", "on|80|80", "off|200|5", "off|69|60"};
    for (const char *text : invalid_numbers) {           // S5: never "on" with numbers that are not allowed
        const Settings d = parse_settings(text);
        CHECK(!d.enabled && d.limit == 80 && d.resume == 75);
    }
    const Settings kept = parse_settings("off|90|85");
    CHECK(!kept.enabled && kept.limit == 90 && kept.resume == 85);
    CHECK(Settings{}.enabled == false);
}

// ---- R ----
static void r_readings() {
    const int64_t now = 100000;
    Reading r = reading_of(power(50, 99900), now);
    CHECK(r.usable && r.soc == 50 && r.mv == 4000 && r.temp_c == 30.0f && r.why == nullptr);

    CHECK(reading_of(power(50, now - 6000), now).usable);        // R1: exactly 6 s old is still fresh
    r = reading_of(power(50, now - 6001), now); CHECK(!r.usable && is(r.why, "SAMPLE_STALE"));
    r = reading_of(power(50, 0), now); CHECK(!r.usable && is(r.why, "NO_SAMPLE"));
    r = reading_of(power(50, now + 1), now); CHECK(!r.usable);

    PowerState p = power(50, 99900);
    p.battery = "absent"; r = reading_of(p, now); CHECK(!r.usable && is(r.why, "NO_BATTERY"));   // R2
    p.battery = "unknown"; r = reading_of(p, now); CHECK(!r.usable && is(r.why, "NO_BATTERY"));

    r = reading_of(power(-1, 99900), now); CHECK(!r.usable && is(r.why, "LEVEL_UNREADABLE"));   // R3
    r = reading_of(power(101, 99900), now); CHECK(!r.usable);
    r = reading_of(power(0, 99900), now); CHECK(r.usable && r.soc == 0);
    r = reading_of(power(100, 99900), now); CHECK(r.usable && r.soc == 100);
    p = power(50, 99900); p.reason = "SOC_OUT_OF_RANGE"; CHECK(!reading_of(p, now).usable);
    p = power(50, 99900); p.reason = "SOC_READ_FAILED"; CHECK(!reading_of(p, now).usable);

    r = reading_of(power(50, 99900, 2499), now); CHECK(r.usable && r.mv == -1);                  // R4
    r = reading_of(power(50, 99900, 2500), now); CHECK(r.mv == 2500);
    r = reading_of(power(50, 99900, 4600), now); CHECK(r.mv == 4600);
    r = reading_of(power(50, 99900, 4601), now); CHECK(r.usable && r.mv == -1);
    r = reading_of(power(50, 99900, 4000, -41.0f), now); CHECK(r.usable && std::isnan(r.temp_c));
    r = reading_of(power(50, 99900, 4000, -40.0f), now); CHECK(r.temp_c == -40.0f);
    r = reading_of(power(50, 99900, 4000, 150.0f), now); CHECK(r.temp_c == 150.0f);
    r = reading_of(power(50, 99900, 4000, 151.0f), now); CHECK(r.usable && std::isnan(r.temp_c));
    r = reading_of(power(50, 99900, 4000, NAN), now); CHECK(r.usable && std::isnan(r.temp_c));
    r = reading_of(power(50, 99900, -1), now); CHECK(r.usable && r.mv == -1);

    p = power(50, 99900); p.fault = true; r = reading_of(p, now); CHECK(!r.usable && is(r.why, "PMIC_FAULT"));   // R5

    Rig rig;                                                      // R6: one observation is one sample
    rig.enable();
    rig.last = power(85, rig.t - 100);
    for (int i = 0; i < 40; ++i) { rig.t += 250; rig.c.step(rig.last, rig.t); }
    CHECK(rig.sw.sets == 0);
    rig.sample(85); rig.sample(85);
    CHECK(rig.sw.sets == 0);
    rig.sample(85);
    CHECK(rig.stopped() && rig.stops() == 1);
}

// ---- P ----
static void p_policy() {
    {   // P1
        Rig r; r.enable();
        r.feed(20, 79);
        CHECK(r.sw.sets == 0 && r.c.status().state == State::Charging);
        CHECK(r.c.status().enabled && r.c.status().limit == 80);
    }
    {   // P2
        Rig r; r.enable();
        r.sample(80); r.sample(80); CHECK(!r.stopped());
        r.sample(79); CHECK(!r.stopped());                 // a low sample clears the count
        r.sample(80); r.sample(80); CHECK(!r.stopped());
        r.sample(80); CHECK(r.stopped());
        CHECK(r.sw.requests.size() == 1 && r.c.status().state == State::Holding);
    }
    {   // P3
        Rig r; r.enable();
        r.feed(3, 80); CHECK(r.stopped());
        Rig q; q.enable();
        q.feed(10, 79); CHECK(!q.stopped());
        Rig o; o.enable(90, 85);
        o.feed(10, 89); CHECK(!o.stopped());
        o.feed(3, 90); CHECK(o.stopped());
    }
    {   // P4: the dead band keeps the last confirmed state
        Rig r; r.enable();
        for (int soc = 76; soc <= 79; ++soc) r.feed(10, soc);
        CHECK(r.sw.sets == 0);
        r.feed(3, 80); CHECK(r.stopped());
        const int before = r.sw.sets;
        for (int soc = 76; soc <= 79; ++soc) r.feed(20, soc);   // 160 s in the band
        CHECK(r.stopped() && r.sw.sets == before);
        r.feed(20, 85); CHECK(r.stopped() && r.sw.sets == before);   // above the limit while stopped: nothing to do
    }
    {   // P5
        Rig r; r.enable();
        r.feed(3, 80); CHECK(r.stopped());
        r.feed(30, 76);
        r.sample(75); r.sample(75); CHECK(r.stopped());
        r.sample(76); CHECK(r.stopped());
        r.sample(75); r.sample(75); CHECK(r.stopped());
        r.sample(75); CHECK(!r.stopped());
        CHECK(r.sw.requests.size() == 2 && r.c.status().state == State::Charging);
        Rig q; q.enable();
        q.feed(3, 80); q.feed(30, 76); q.feed(5, 76);
        q.feed(3, 76); CHECK(q.stopped());                  // 76 is not 75
    }
    {   // P6: no switch within 60 s of the last one, even with three samples
        Rig r; r.enable();
        r.feed(3, 80); CHECK(r.stopped());
        const int64_t stopped_at = r.t;
        bool flipped = false;
        for (int i = 0; i < 40 && !flipped; ++i) {
            r.sample(70);
            if (r.t - stopped_at < 60000) CHECK(r.stopped());
            else { CHECK(!r.stopped()); flipped = true; CHECK(r.t - stopped_at == 60000); }
        }
        CHECK(flipped);
        const int64_t resumed_at = r.t;
        Rig g; g.enable();                                 // between two samples: still inside the gap at 59.9 s, out of it at 60 s
        g.feed(3, 80);
        const int64_t gap_start = g.t;
        g.feed(29, 70);
        CHECK(g.stopped());
        g.t = gap_start + 59900; g.c.step(g.last, g.t); CHECK(g.stopped());
        g.t = gap_start + 60000; g.c.step(g.last, g.t); CHECK(!g.stopped());
        flipped = false;
        for (int i = 0; i < 40 && !flipped; ++i) {
            r.sample(90);
            if (r.t - resumed_at < 60000) CHECK(!r.stopped());
            else { CHECK(r.stopped()); flipped = true; }
        }
        CHECK(flipped);
    }
    {   // P6b: three low samples inside the gap do not resume later if the level has come back into the dead band
        Rig r; r.enable();
        r.feed(3, 80); CHECK(r.stopped());
        r.feed(3, 70);                                     // three samples at the resume line, but the gap is not over
        r.feed(40, 77);                                    // then the dead band, past the gap
        CHECK(r.stopped() && r.sw.requests.size() == 1);
    }
    {   // P7: the first enable is not held back by a recent switch
        Rig r(false, false);                               // found off at boot: restored at t0
        CHECK(r.sw.requests.size() == 1 && r.sw.requests[0]);
        const int64_t t0 = r.t;
        r.t += 10000;
        r.enable();
        r.feed(3, 85);
        CHECK(r.stopped() && r.t - t0 < 60000);
        r.disable();                                       // back to normal charging at once
        CHECK(!r.stopped());
        r.t += 5000;
        r.enable();
        r.feed(3, 85);
        CHECK(r.stopped());                                // a new enable is a first enable again
    }
    {   // P8
        Rig r; r.enable();
        r.feed(3, 80); CHECK(r.stopped());
        r.disable();
        CHECK(!r.stopped() && r.sw.requests.size() == 2 && r.sw.requests[1]);
        CHECK(r.c.status().state == State::Off && !r.c.status().enabled);
        r.feed(20, 99); CHECK(!r.stopped() && r.sw.sets == 2);   // off stays off
        Rig q; q.enable();
        q.feed(3, 80); CHECK(q.stopped());
        q.t += 1000;                                        // no new sample, no count, no gap: off is off
        q.disable();
        CHECK(!q.stopped());
    }
    {   // P9
        Rig r; r.enable();
        r.feed(2, 85);
        r.enable(90, 85);                                  // a new limit clears the count and applies at once
        r.feed(10, 85); CHECK(!r.stopped());
        r.feed(3, 90); CHECK(r.stopped());
        Rig q; q.enable(70, 65);
        q.feed(3, 72); CHECK(q.stopped());
        Rig bad; bad.configure(true, 80, 78);              // numbers that are not allowed mean off
        bad.feed(10, 99); CHECK(bad.sw.sets == 0 && bad.c.status().state == State::Off && !bad.c.status().enabled);
        CHECK(bad.c.status().limit == 80 && bad.c.status().resume == 75);
        Rig h; h.enable();                                 // a new limit while stopped keeps the stop; 85 is in the new dead band
        h.feed(3, 85); CHECK(h.stopped());
        h.enable(90, 80);
        h.feed(40, 85); CHECK(h.stopped() && h.sw.requests.size() == 1);
    }
}

// ---- H ----
static void h_hard_limits() {
    {   // H1
        Rig r; r.enable();
        r.sample(50, 4299); CHECK(!r.stopped());
        r.sample(50, 4300); CHECK(r.stopped());            // at once: no count, no gap
        CHECK(r.c.status().state == State::SafetyStop && is(r.c.status().reason, "VOLTAGE_LIMIT"));
        CHECK(!r.log.events.empty() && !r.log.events.back().charge && is(r.log.events.back().reason, "VOLTAGE_LIMIT"));
        Rig q; q.enable();                                 // 4150 is not below 4150: still stopped after the gap has passed
        q.sample(50, 4300); CHECK(q.stopped());
        for (int i = 0; i < 35; ++i) { q.sample(50, 4150); CHECK(q.stopped()); }
        const int64_t at = r.t;
        for (int i = 0; i < 5; ++i) {
            r.sample(50, 4150);                            // 4150 is not below 4150
            CHECK(r.stopped());
        }
        bool back = false;                                 // 4149 releases the lock, and the switch waits for the gap
        for (int i = 0; i < 40 && !back; ++i) {
            r.sample(50, 4149);
            if (r.t - at < 60000) CHECK(r.stopped()); else back = !r.stopped();
        }
        CHECK(back && r.t - at == 60000);
        CHECK(is(r.log.events.back().reason, "LIMIT_RELEASED"));
        Rig w; w.enable();                                 // a spike right after a switch is still at once
        w.feed(3, 80); w.feed(40, 70); CHECK(!w.stopped());
        w.sample(50, 4300); CHECK(w.stopped());
    }
    {   // H2
        Rig r; r.enable();
        r.sample(50, 4000, 64.9f); CHECK(!r.stopped());
        r.sample(50, 4000, 65.0f); CHECK(r.stopped());
        CHECK(is(r.c.status().reason, "TEMPERATURE_LIMIT"));
        CHECK(!r.log.events.empty() && is(r.log.events.back().reason, "TEMPERATURE_LIMIT"));
        for (int i = 0; i < 40; ++i) { r.sample(50, 4000, 55.0f); CHECK(r.stopped()); }
        bool back = false;
        for (int i = 0; i < 40 && !back; ++i) { r.sample(50, 4000, 54.9f); back = !r.stopped(); }
        CHECK(back);
    }
    {   // H3 and H4
        Rig r; r.enable();
        r.sample(10, 4300); CHECK(r.stopped());            // far below the limit, stopped anyway
        r.feed(40, 85, 4100);                              // released; the level is above the limit: stays stopped
        CHECK(r.stopped());
        Rig q; q.enable();
        q.sample(60, 4300); CHECK(q.stopped());
        bool back = false;
        for (int i = 0; i < 40 && !back; ++i) { q.sample(60, 4100); back = !q.stopped(); }
        CHECK(back);                                       // released, and the level says charge
    }
    {   // H5
        Rig r;
        r.sample(50, 4500, 90.0f); r.feed(5, 99, 4500, 90.0f);
        CHECK(r.sw.sets == 0);
    }
    {   // H6: two locks
        Rig r; r.enable();
        r.sample(50, 4300, 66.0f); CHECK(r.stopped());
        r.feed(40, 50, 4000, 66.0f); CHECK(r.stopped());   // voltage released, temperature still high
        r.feed(40, 50, 4000, 50.0f); CHECK(!r.stopped());
        Rig q; q.enable();
        q.sample(50, 4000, 70.0f); CHECK(q.stopped());
        q.sample(50, 4400, 50.0f);                         // temperature released, voltage high
        q.feed(40, 50, 4400, 50.0f); CHECK(q.stopped());
    }
    {   // H7: no reading of the voltage keeps the lock
        Rig r; r.enable();
        r.sample(50, 4300); CHECK(r.stopped());
        r.feed(60, 50, -1); CHECK(r.stopped());
        r.feed(60, 50, 4601); CHECK(r.stopped());          // implausible is the same as unreadable
        r.feed(40, 50, 4100); CHECK(!r.stopped());
    }
}

// ---- F ----
static void f_failure_direction() {
    {   // F1
        Rig r; r.enable();
        r.feed(3, 85); CHECK(r.stopped());
        r.age(7000);
        CHECK(!r.stopped() && r.c.status().state == State::Failsafe && is(r.c.status().reason, "SAMPLE_STALE"));
        r.sample(70); CHECK(r.c.status().state == State::Charging);
    }
    {   // F2
        Rig r; r.enable();
        r.feed(3, 85); CHECK(r.stopped());
        r.sample(-1); CHECK(!r.stopped() && r.c.status().state == State::Failsafe);
        CHECK(is(r.c.status().reason, "LEVEL_UNREADABLE"));
    }
    {   // F3
        Rig r; r.enable();
        r.feed(3, 85); CHECK(r.stopped());
        r.t += 2000;
        PowerState p = power(85, r.t - 100); p.battery = "absent";
        r.c.step(p, r.t);
        CHECK(!r.stopped() && is(r.c.status().reason, "NO_BATTERY"));
        Rig q; q.enable();
        q.t += 2000; p = power(99, q.t - 100); p.battery = "absent";
        for (int i = 0; i < 10; ++i) q.c.step(p, q.t += 2000);
        CHECK(q.sw.sets == 0 && q.c.status().state == State::Failsafe);   // F5: nothing to write
    }
    {   // F4: after the restore, three new samples and the 60 s gap
        Rig r; r.enable();
        r.feed(3, 85); CHECK(r.stopped());
        r.sample(-1); CHECK(!r.stopped());
        const int64_t restored_at = r.t;
        bool stopped_again = false;
        for (int i = 0; i < 40 && !stopped_again; ++i) {
            r.sample(85);
            if (r.t - restored_at < 60000) CHECK(!r.stopped()); else stopped_again = r.stopped();
        }
        CHECK(stopped_again);
    }
    {   // F6 and F7
        Rig r; r.enable();
        r.sw.script = {Write::Failed};
        r.feed(3, 85);
        CHECK(r.sw.requests.size() == 2 && !r.sw.requests[0] && r.sw.requests[1]);
        CHECK(!r.stopped() && r.c.status().state == State::Failsafe && is(r.c.status().reason, "STOP_FAILED"));
        r.feed(28, 85); CHECK(r.sw.sets == 2);             // no new try inside the gap
        Rig q; q.enable();
        q.sw.script = {Write::Mismatch, Write::Ok};
        q.feed(3, 85);
        CHECK(q.sw.requests.size() == 2 && q.sw.requests[1] && !q.stopped());
        CHECK(is(q.c.status().reason, "STOP_MISMATCH"));
        Rig z; z.enable();
        z.sw.script = {Write::Refused};
        z.feed(3, 85);
        CHECK(z.sw.requests.size() == 2 && z.sw.requests[1] && !z.stopped());
    }
    {   // F8
        Rig r; r.enable();
        r.sw.script = {Write::Failed, Write::Ok, Write::Failed, Write::Ok, Write::Failed, Write::Ok};
        for (int i = 0; i < 400; ++i) r.sample(85);
        CHECK(r.sw.requests.size() == 6 && r.stops() == 3 && !r.stopped());
        CHECK(r.c.status().faulted && r.c.status().state == State::Failsafe && is(r.c.status().reason, "REPEATED_FAILURE"));
        r.feed(200, 95); CHECK(r.sw.requests.size() == 6);   // no new try
        r.disable(); r.enable();
        CHECK(!r.c.status().faulted && r.c.status().stop_failures == 0);
        r.feed(3, 85); CHECK(r.stopped());
    }
    {   // F9
        Rig r; r.enable();
        r.sw.script = {Write::Failed, Write::Ok, Write::Failed, Write::Ok};
        for (int i = 0; i < 200 && r.stops() < 3; ++i) r.sample(85);
        CHECK(r.stops() == 3 && r.stopped());               // the third try works
        CHECK(!r.c.status().faulted && r.c.status().stop_failures == 0);
    }
    {   // F10 and F11
        Rig r; r.enable();
        r.sw.script = {Write::Failed, Write::Failed, Write::Failed, Write::Failed};
        r.feed(3, 85);
        CHECK(r.sw.requests.size() == 4 && !r.sw.requests[0] && r.sw.requests[1] && r.sw.requests[2] && r.sw.requests[3]);
        CHECK(r.c.status().state == State::Unconfirmed && !r.c.status().charging_known && !r.c.status().charging);
        const int64_t burst_at = r.t;
        bool retried = false;
        for (int i = 0; i < 20 && !retried; ++i) {
            r.sample(95);
            if (r.t - burst_at < 30000) { CHECK(r.sw.requests.size() == 4); CHECK(r.c.status().state == State::Unconfirmed); }
            else { retried = true; CHECK(r.t - burst_at == 30000); }
        }
        CHECK(retried && r.sw.requests.size() == 5 && r.sw.requests[4]);
        CHECK(r.c.status().state == State::Failsafe && r.c.status().charging_known && r.c.status().charging && !r.stopped());
        CHECK(r.stops() == 1);                              // while unsure, no second stop
        Rig z; z.enable();                                  // the retry is due at exactly 30 s, between two samples too
        z.sw.script = {Write::Failed, Write::Failed, Write::Failed, Write::Failed};
        z.feed(3, 85);
        const int64_t due_from = z.t;
        z.t = due_from + 29900; z.c.step(z.last, z.t); CHECK(z.sw.requests.size() == 4);
        z.t = due_from + 30000; z.c.step(z.last, z.t); CHECK(z.sw.requests.size() == 5);
    }
    {   // F12
        Rig r(true, false, false);
        r.sw.readable = false;
        r.c.start(r.t);
        CHECK(r.c.status().state == State::Unconfirmed);
        CHECK(!r.log.events.empty() && is(r.log.events[0].reason, "BOOT_UNREADABLE"));
        for (bool request : r.sw.requests) CHECK(request);  // only ever "charge"
        r.sw.readable = true;
        r.feed(16, 90);
        CHECK(r.c.status().state != State::Unconfirmed);
        for (bool request : r.sw.requests) CHECK(request);
    }
}

// ---- B ----
static void b_boot() {
    {   // B1: found off, protection on or off
        Rig r(false, true);
        CHECK(r.sw.requests.size() == 1 && r.sw.requests[0] && r.sw.charging);
        CHECK(r.log.events.size() == 1 && is(r.log.events[0].reason, "BOOT_FOUND_STOPPED"));
        Rig q(false, false);
        CHECK(q.sw.requests.size() == 1 && q.sw.requests[0] && q.sw.charging);
    }
    {   // B2
        Rig r(true, true);
        CHECK(r.sw.sets == 0 && r.log.events.empty());
    }
    {   // B3
        Rig r(false, true);
        r.feed(3, 90);
        CHECK(r.stopped() && r.c.status().state == State::Holding);
    }
}

// ---- L ----
static void l_log() {
    Rig r;
    r.enable();
    r.feed(30, 79);
    CHECK(r.log.events.empty());                           // L2
    r.feed(3, 80);
    const int64_t stop_at = r.t;
    CHECK(r.log.events.size() == 1);
    for (int i = 0; i < 40 && r.log.events.size() < 2; ++i) r.sample(70);
    CHECK(r.log.events.size() == 2);                       // L1
    if (r.log.events.size() == 2) {
        const Event &a = r.log.events[0], &b = r.log.events[1];
        CHECK(!a.charge && a.result == Write::Ok && is(a.reason, "LEVEL_AT_LIMIT") && a.at_ms == stop_at);
        CHECK(b.charge && b.result == Write::Ok && is(b.reason, "LEVEL_AT_RESUME") && b.at_ms == r.t);
    }
    Rig f; f.enable();
    f.sw.script = {Write::Failed};
    f.feed(3, 85);
    CHECK(f.log.events.size() == 2 && f.log.events[0].result == Write::Failed && !f.log.events[0].charge);   // failures are logged too
    CHECK(f.log.events[1].charge && f.log.events[1].result == Write::Ok);
}

// ---- N: the words the USB status uses ----
static void n_names() {
    CHECK(is(state_name(State::Off), "off") && is(state_name(State::Charging), "charging") && is(state_name(State::Holding), "holding"));
    CHECK(is(state_name(State::SafetyStop), "safety_stop") && is(state_name(State::Failsafe), "failsafe") && is(state_name(State::Unconfirmed), "unconfirmed"));
    CHECK(is(write_name(Write::Ok), "ok") && is(write_name(Write::Failed), "failed") && is(write_name(Write::Mismatch), "mismatch") && is(write_name(Write::Refused), "refused"));
}

// ---- I ----
static void i_random_walk() {
    std::mt19937_64 rng(20261029);
    auto roll = [&](int n) { return static_cast<int>(rng() % static_cast<uint64_t>(n)); };
    FakeSwitch sw;
    FakeLog log;
    Controller c(sw, log);
    int64_t t = 100000;
    c.start(t);
    Settings cur;
    bool cur_on = false;
    PowerState p = power(50, t - 100);
    int64_t seen_obs = 0, last_ok = -1;
    int ok_since_configure = 0, consecutive = 0;
    size_t seen_events = 0;
    for (int step = 0; step < 1000000; ++step) {
        t += 250 + roll(4750);
        if (roll(200) == 0) {
            Settings s;
            s.enabled = roll(3) != 0;
            s.limit = roll(8) == 0 ? 60 + roll(60) : 70 + roll(31);
            s.resume = roll(8) == 0 ? roll(120) : s.limit - 5 - roll(20);
            c.configure(s, t);
            cur = s;
            cur_on = s.enabled && valid(s);
            ok_since_configure = 0;
            consecutive = 0;
        }
        if (roll(10) == 0) sw.script.push_back(static_cast<Write>(roll(4)));
        if (roll(300) == 0) sw.readable = !sw.readable;
        if (roll(2) == 0) {
            const int soc_kind = roll(10);
            const int soc = soc_kind == 0 ? -1 : soc_kind == 1 ? 101 : soc_kind < 6 ? cur.limit - 2 + roll(5) : roll(101);
            const int mv = roll(6) == 0 ? -1 : roll(3) == 0 ? 4100 + roll(260) : 3500 + roll(900);
            const float temp = roll(6) == 0 ? NAN : roll(3) == 0 ? 50.0f + static_cast<float>(roll(200)) / 10.0f : static_cast<float>(roll(60));
            p = power(soc, t - roll(8000), mv, temp);
            if (roll(40) == 0) p.battery = "absent";
            if (roll(80) == 0) p.fault = true;
            if (roll(50) == 0) p.observed_ms = 0;
        }
        c.step(p, t);
        const Reading rd = reading_of(p, t);
        const bool is_new = p.observed_ms > seen_obs;
        if (is_new) {
            seen_obs = p.observed_ms;
            if (rd.usable && rd.soc >= cur.limit) ++consecutive; else consecutive = 0;
        }
        for (; seen_events < log.events.size(); ++seen_events) {
            const Event &e = log.events[seen_events];
            const bool normal = is(e.reason, "LEVEL_AT_LIMIT") || is(e.reason, "LEVEL_AT_RESUME") || is(e.reason, "LIMIT_RELEASED");
            if (!e.charge) {
                CHECK(cur_on);                              // I1
                CHECK(rd.usable);                           // I2
                if (is(e.reason, "LEVEL_AT_LIMIT")) CHECK(consecutive >= kSamplesToSwitch);   // I5
            }
            if (normal) CHECK(ok_since_configure == 0 || e.at_ms - last_ok >= kMinSwitchMs);   // I4
            if (!e.charge) consecutive = 0;
            if (e.result == Write::Ok) { last_ok = e.at_ms; ++ok_since_configure; }
        }
        if (!rd.usable) CHECK(sw.charging || c.status().state == State::Unconfirmed);   // I3
        if (failures > 20) break;
    }
}

int main() {
    s_settings();
    r_readings();
    p_policy();
    h_hard_limits();
    f_failure_direction();
    b_boot();
    l_log();
    n_names();
    i_random_walk();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("charge_test: all checks passed");
    return 0;
}
