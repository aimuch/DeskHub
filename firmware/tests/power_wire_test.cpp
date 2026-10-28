// Failure modes of the "power" object that the device sends to the Agent and to the USB tool, written before the code.
// The Agent rejects a whole report that breaks these rules (agent/tests/test_power_report.py), so the firmware must
// never send one. Rules: docs/rules.md K3 (unknown stays unknown, stale stays stale, nothing invented).
//
// W1  A fresh, healthy sample is sent as it is: level, battery mV, USB mV, chip temperature, measurement "ok".
// W2  A sample that is stale (never taken, older than 6 s, from the future) says "unknown" for USB, battery and
//     charging, "not_measured", and carries no number.
// W3  A PMIC fault sends no number either, but keeps fault = true.
// W4  The board's reason maps to the wire state: ADC_OK ok, ADC_ENABLE_FAILED adc_enable_failed, ADC_READ_FAILED
//     adc_read_failed, ADC_OUT_OF_RANGE adc_out_of_range. An empty or unknown reason is not_measured, never "ok".
// W5  No battery: no level and no battery voltage, even if the board left values behind. No USB: no USB voltage.
// W6  The board says ADC_OK but a value is outside the plausible range or missing (-1, NaN, inf): the state becomes
//     adc_out_of_range and no number is sent. The range is checked here again, not trusted from the board.
// W7  A level outside 0..100 is not sent; it does not spoil the voltages.
// W8  Words the Agent does not know (battery "weird", charging "fault2") are sent as "unknown".
// W9  The chip temperature is rounded to 0.1 and is never the battery temperature.
// W10 Over a large sweep of inputs, every object satisfies the Agent's rules, and stays small (< 400 bytes).
#include "cJSON.h"
#include "power_wire.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

using namespace dh;

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++failures;                                                               \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                             \
    } while (0)

static const int64_t NOW = 100000;

static PowerState healthy() {
    PowerState p;
    p.battery = "present";
    p.vbus = "present";
    p.charging = "charging";
    p.reason = "ok";
    p.soc_percent = 67;
    p.battery_mv = 3921;
    p.vbus_mv = 5120;
    p.chip_temp_c = 41.26f;
    p.measure_reason = "ADC_OK";
    p.observed_ms = NOW - 1000;
    return p;
}

struct Json {
    cJSON *o;
    explicit Json(const PowerState &p, int64_t now = NOW) : o(power_report_json(p, now)) {}
    ~Json() { cJSON_Delete(o); }
    Json(const Json &) = delete;
    Json &operator=(const Json &) = delete;
    cJSON *at(const char *k) const { return cJSON_GetObjectItemCaseSensitive(o, k); }
    bool null(const char *k) const { return cJSON_IsNull(at(k)); }
    std::string str(const char *k) const { auto *v = at(k); return cJSON_IsString(v) ? v->valuestring : "<not a string>"; }
    double num(const char *k) const { auto *v = at(k); return cJSON_IsNumber(v) ? v->valuedouble : -12345.0; }
    bool boolean(const char *k) const { return cJSON_IsTrue(at(k)); }
    bool numbers_null() const { return null("soc_percent") && null("battery_mv") && null("vbus_mv") && null("chip_temp_c"); }
    bool voltages_null() const { return null("battery_mv") && null("vbus_mv") && null("chip_temp_c"); }
};

// The Agent's rules (agent/src/deskhub_agent/management_models.py PowerReport), written out again here.
static bool member_of(const std::string &v, std::initializer_list<const char *> set) {
    for (const char *s : set) if (v == s) return true;
    return false;
}
static bool agent_would_accept(const Json &j) {
    if (!cJSON_IsObject(j.o)) return false;
    const char *allowed[] = {"vbus", "battery", "charging", "fault", "soc_percent", "battery_mv", "vbus_mv", "chip_temp_c", "measurement"};
    int members = 0;
    for (cJSON *c = j.o->child; c; c = c->next) {
        ++members;
        bool known = false;
        for (const char *a : allowed) if (!strcmp(a, c->string)) known = true;
        if (!known) return false;
    }
    if (members != 9) return false;  // every field is always present, null when unknown
    if (!member_of(j.str("vbus"), {"present", "absent", "unknown"})) return false;
    if (!member_of(j.str("battery"), {"present", "absent", "unknown"})) return false;
    if (!member_of(j.str("charging"), {"charging", "full", "idle", "fault", "unknown"})) return false;
    if (!cJSON_IsBool(j.at("fault"))) return false;
    if (!member_of(j.str("measurement"), {"ok", "adc_enable_failed", "adc_read_failed", "adc_out_of_range", "not_measured"})) return false;
    for (const char *k : {"soc_percent", "battery_mv", "vbus_mv", "chip_temp_c"})
        if (!j.null(k) && !cJSON_IsNumber(j.at(k))) return false;
    if (!j.null("soc_percent") && (j.num("soc_percent") < 0 || j.num("soc_percent") > 100 || j.num("soc_percent") != std::floor(j.num("soc_percent")))) return false;
    if (!j.null("battery_mv") && (j.num("battery_mv") < 2500 || j.num("battery_mv") > 4600 || j.num("battery_mv") != std::floor(j.num("battery_mv")))) return false;
    if (!j.null("vbus_mv") && (j.num("vbus_mv") < 3000 || j.num("vbus_mv") > 6500 || j.num("vbus_mv") != std::floor(j.num("vbus_mv")))) return false;
    if (!j.null("chip_temp_c") && (!std::isfinite(j.num("chip_temp_c")) || j.num("chip_temp_c") < -40 || j.num("chip_temp_c") > 150)) return false;
    if (j.str("measurement") != "ok" && !j.voltages_null()) return false;
    if (j.str("battery") != "present" && (!j.null("soc_percent") || !j.null("battery_mv"))) return false;
    if (j.str("vbus") != "present" && !j.null("vbus_mv")) return false;
    return true;
}

static void w1_healthy() {
    Json j(healthy());
    CHECK(agent_would_accept(j));
    CHECK(j.str("vbus") == "present" && j.str("battery") == "present" && j.str("charging") == "charging");
    CHECK(!j.boolean("fault"));
    CHECK(j.num("soc_percent") == 67 && j.num("battery_mv") == 3921 && j.num("vbus_mv") == 5120);
    CHECK(std::fabs(j.num("chip_temp_c") - 41.3) < 1e-9);
    CHECK(j.str("measurement") == "ok");
    CHECK(power_sample_fresh(healthy(), NOW));
}
static void w2_stale() {
    for (int64_t observed : {int64_t(0), int64_t(-5), NOW - 6001, NOW - 60000, NOW + 1}) {
        PowerState p = healthy();
        p.observed_ms = observed;
        Json j(p);
        CHECK(agent_would_accept(j));
        CHECK(j.str("vbus") == "unknown" && j.str("battery") == "unknown" && j.str("charging") == "unknown");
        CHECK(j.str("measurement") == "not_measured");
        CHECK(j.numbers_null());
        CHECK(!power_sample_fresh(p, NOW));
    }
    // Just after boot the clock is small: a sample that was never taken must still not look fresh.
    for (int64_t observed : {int64_t(0), int64_t(-5)}) {
        PowerState boot = healthy();
        boot.observed_ms = observed;
        CHECK(!power_sample_fresh(boot, 3000));
        Json j(boot, 3000);
        CHECK(j.str("measurement") == "not_measured" && j.numbers_null() && j.str("battery") == "unknown");
    }
    PowerState edge = healthy();
    edge.observed_ms = NOW - 6000;
    CHECK(power_sample_fresh(edge, NOW));  // exactly 6 s is still fresh
    Json j(edge);
    CHECK(j.str("measurement") == "ok");
}
static void w3_fault() {
    PowerState p = healthy();
    p.fault = true;
    Json j(p);
    CHECK(agent_would_accept(j));
    CHECK(j.boolean("fault"));
    CHECK(j.str("measurement") == "not_measured" && j.numbers_null());
}
static void w4_reasons() {
    const std::pair<const char *, const char *> map[] = {{"ADC_OK", "ok"}, {"ADC_ENABLE_FAILED", "adc_enable_failed"}, {"ADC_READ_FAILED", "adc_read_failed"}, {"ADC_OUT_OF_RANGE", "adc_out_of_range"}};
    for (auto &m : map) {
        PowerState p = healthy();
        p.measure_reason = m.first;
        if (std::strcmp(m.first, "ADC_OK") != 0) { p.battery_mv = -1; p.vbus_mv = -1; p.chip_temp_c = NAN; }
        Json j(p);
        CHECK(agent_would_accept(j));
        CHECK(j.str("measurement") == m.second);
        if (std::strcmp(m.first, "ADC_OK") != 0) CHECK(j.voltages_null());
    }
    // the board left numbers behind after a failure: they are not sent
    PowerState p = healthy();
    p.measure_reason = "ADC_READ_FAILED";
    Json failed(p);
    CHECK(failed.voltages_null() && failed.str("measurement") == "adc_read_failed" && agent_would_accept(failed));
    CHECK(failed.num("soc_percent") == 67);  // the gauge's level is its own register
    for (const char *r : {"", "adc_ok", "OK", "SOMETHING_NEW", "ADC_OK "}) {
        PowerState q = healthy();
        q.measure_reason = r;
        Json j(q);
        CHECK(j.str("measurement") == "not_measured" && j.voltages_null() && agent_would_accept(j));
    }
}
static void w5_absent() {
    PowerState p = healthy();
    p.battery = "absent";
    p.charging = "idle";
    Json j(p);  // soc 67 and 3921 mV are still in the board's state
    CHECK(agent_would_accept(j));
    CHECK(j.null("soc_percent") && j.null("battery_mv"));
    CHECK(j.num("vbus_mv") == 5120 && j.str("measurement") == "ok");
    PowerState q = healthy();
    q.vbus = "absent";
    Json k(q);
    CHECK(agent_would_accept(k) && k.null("vbus_mv") && k.num("battery_mv") == 3921);
    PowerState r = healthy();
    r.battery = "unknown";
    r.vbus = "unknown";
    Json u(r);
    CHECK(agent_would_accept(u) && u.null("soc_percent") && u.null("battery_mv") && u.null("vbus_mv"));
}
static void w6_out_of_range() {
    struct Case { int battery, vbus; float temp; };
    const Case cases[] = {{2499, 5120, 41.f}, {4601, 5120, 41.f}, {0, 5120, 41.f}, {-1, 5120, 41.f}, {3921, 2999, 41.f}, {3921, 6501, 41.f},
                          {3921, -1, 41.f}, {3921, 5120, NAN}, {3921, 5120, INFINITY}, {3921, 5120, -INFINITY}, {3921, 5120, -40.1f}, {3921, 5120, 150.1f}};
    for (const Case &c : cases) {
        PowerState p = healthy();
        p.battery_mv = c.battery; p.vbus_mv = c.vbus; p.chip_temp_c = c.temp;
        Json j(p);
        CHECK(agent_would_accept(j));
        CHECK(j.str("measurement") == "adc_out_of_range");
        CHECK(j.voltages_null());
    }
    for (float t : {-40.0f, 150.0f}) { PowerState p = healthy(); p.chip_temp_c = t; Json j(p); CHECK(j.str("measurement") == "ok" && j.num("chip_temp_c") == t); }
    for (int mv : {2500, 4600}) { PowerState p = healthy(); p.battery_mv = mv; Json j(p); CHECK(j.str("measurement") == "ok" && j.num("battery_mv") == mv); }
    for (int mv : {3000, 6500}) { PowerState p = healthy(); p.vbus_mv = mv; Json j(p); CHECK(j.str("measurement") == "ok" && j.num("vbus_mv") == mv); }
    // No battery and the board has -1 for it: that is normal, not out of range.
    PowerState none = healthy();
    none.battery = "absent"; none.battery_mv = -1; none.soc_percent = -1;
    Json j(none);
    CHECK(j.str("measurement") == "ok" && j.num("vbus_mv") == 5120);
}
static void w7_level() {
    for (int soc : {-1, 101, 255, -100}) {
        PowerState p = healthy();
        p.soc_percent = soc;
        Json j(p);
        CHECK(agent_would_accept(j) && j.null("soc_percent"));
        CHECK(j.str("measurement") == "ok" && j.num("battery_mv") == 3921);
    }
    for (int soc : {0, 100}) { PowerState p = healthy(); p.soc_percent = soc; Json j(p); CHECK(j.num("soc_percent") == soc); }
}
static void w8_words() {
    PowerState p = healthy();
    p.battery = "weird"; p.vbus = "PRESENT"; p.charging = "fault2";
    Json j(p);
    CHECK(agent_would_accept(j));
    CHECK(j.str("battery") == "unknown" && j.str("vbus") == "unknown" && j.str("charging") == "unknown");
    CHECK(j.null("soc_percent") && j.null("battery_mv") && j.null("vbus_mv"));
    for (const char *c : {"charging", "full", "idle", "unknown"}) { PowerState q = healthy(); q.charging = c; Json k(q); CHECK(k.str("charging") == c); }
}
static void w9_temperature() {
    for (auto [raw, want] : {std::pair<float, double>{41.26f, 41.3}, {41.24f, 41.2}, {-0.04f, 0.0}, {-39.96f, -40.0}, {149.94f, 149.9}}) {
        PowerState p = healthy();
        p.chip_temp_c = raw;
        Json j(p);
        CHECK(std::fabs(j.num("chip_temp_c") - want) < 1e-9);
    }
    PowerState p = healthy();
    Json j(p);
    CHECK(j.at("battery_temp_c") == nullptr);  // there is no cell temperature probe
}
static void w10_sweep() {
    long objects = 0, rejected = 0;
    const char *words[] = {"present", "absent", "unknown", "weird"};
    const char *charging[] = {"charging", "full", "idle", "unknown", "fault2"};
    const char *reasons[] = {"ADC_OK", "ADC_ENABLE_FAILED", "ADC_READ_FAILED", "ADC_OUT_OF_RANGE", "", "NEW"};
    for (int64_t observed : {int64_t(0), NOW - 500, NOW - 9000})
        for (const char *b : words) for (const char *v : words) for (const char *c : charging) for (bool fault : {false, true})
            for (const char *r : reasons) for (int mv : {-1, 2499, 3921, 4601}) for (int vmv : {-1, 2999, 5120, 6501})
                for (float t : {NAN, -41.f, 41.3f, 151.f}) for (int soc : {-1, 67, 101}) {
                    PowerState p;
                    p.battery = b; p.vbus = v; p.charging = c; p.fault = fault; p.measure_reason = r;
                    p.battery_mv = mv; p.vbus_mv = vmv; p.chip_temp_c = t; p.soc_percent = soc; p.observed_ms = observed;
                    Json j(p);
                    ++objects;
                    if (!agent_would_accept(j)) { ++rejected; if (rejected < 4) std::fprintf(stderr, "sweep: rejected b=%s v=%s r=%s mv=%d vmv=%d\n", b, v, r, mv, vmv); }
                    char *text = cJSON_PrintUnformatted(j.o);
                    if (std::strlen(text) >= 400) { ++rejected; std::fprintf(stderr, "sweep: too long (%zu)\n", std::strlen(text)); }
                    cJSON_free(text);
                }
    CHECK(objects > 300000);
    CHECK(rejected == 0);
}

int main() {
    w1_healthy();
    w2_stale();
    w3_fault();
    w4_reasons();
    w5_absent();
    w6_out_of_range();
    w7_level();
    w8_words();
    w9_temperature();
    w10_sweep();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("power_wire_test: all checks passed");
    return 0;
}
