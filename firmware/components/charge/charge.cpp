#include "charge.hpp"
#include "charge_store.hpp"
#include "board.hpp"
#include "core.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <array>

namespace dh::charge {
namespace {
constexpr const char *kTag = "charge";
constexpr size_t kRing = 16;

// `step_lock` serialises the controller (and the slow chip I/O inside it). `view_lock` guards only the copies that
// other tasks read, so status() never waits for the power chip.
SemaphoreHandle_t step_lock, view_lock;
Status view;
std::array<Event, kRing> ring;
size_t ring_count = 0, ring_next = 0;

struct BoardSwitch : Switch {
    bool read(bool &enabled) override { return board_charge_enabled(enabled); }
    Write set(bool enable) override {
        switch (board_set_charging(enable)) {
        case ChargeWrite::Ok: return Write::Ok;
        case ChargeWrite::NotIdentified: return Write::Refused;
        case ChargeWrite::NotConfirmed: return Write::Mismatch;
        default: return Write::Failed;
        }
    }
};
// The log of rules K6. The console is the USB protocol port, so nothing is printed at the default level (warnings only);
// the writes are kept in a ring for the USB status, and logged at info level for a developer who raises it.
struct RingLog : Log {
    void record(const Event &e) override {
        ESP_LOGI(kTag, "request=%s result=%d reason=%s at=%lld", e.charge ? "charge" : "stop", static_cast<int>(e.result), e.reason,
                 static_cast<long long>(e.at_ms));
        xSemaphoreTake(view_lock, portMAX_DELAY);
        ring[ring_next] = e;
        ring_next = (ring_next + 1) % kRing;
        if (ring_count < kRing) ++ring_count;
        xSemaphoreGive(view_lock);
    }
};

BoardSwitch board_switch;
RingLog ring_log;
Controller &controller() {
    static Controller instance(board_switch, ring_log);
    return instance;
}
void publish() {
    const Status s = controller().status();
    xSemaphoreTake(view_lock, portMAX_DELAY);
    view = s;
    xSemaphoreGive(view_lock);
}
}  // namespace

void init() {
    step_lock = xSemaphoreCreateMutex();
    view_lock = xSemaphoreCreateMutex();
    if (!step_lock || !view_lock) return;  // no controller: protection stays off and nothing is ever written
    const int64_t now = monotonic_ms();
    xSemaphoreTake(step_lock, portMAX_DELAY);
    controller().configure(parse_settings(store_load()), now);
    controller().start(now);
    publish();
    xSemaphoreGive(step_lock);
}

void tick() {
    if (!step_lock) return;
    const PowerState power = board_power();
    xSemaphoreTake(step_lock, portMAX_DELAY);
    controller().step(power, monotonic_ms());
    publish();
    xSemaphoreGive(step_lock);
}

Status status() {
    if (!view_lock) return Status{};
    xSemaphoreTake(view_lock, portMAX_DELAY);
    const Status s = view;
    xSemaphoreGive(view_lock);
    return s;
}

std::vector<Event> recent_events() {
    std::vector<Event> out;
    if (!view_lock) return out;
    xSemaphoreTake(view_lock, portMAX_DELAY);
    const size_t first = ring_count < kRing ? 0 : ring_next;
    for (size_t i = 0; i < ring_count; ++i) out.push_back(ring[(first + i) % kRing]);
    xSemaphoreGive(view_lock);
    return out;
}

bool apply(const Settings &wanted) {
    if (!step_lock) return false;
    if (wanted.enabled && !valid(wanted)) return false;
    if (wanted.enabled && !store_save(serialize(wanted))) return false;  // on: saved first, so a restart cannot lose it
    xSemaphoreTake(step_lock, portMAX_DELAY);
    controller().configure(wanted, monotonic_ms());
    publish();
    xSemaphoreGive(step_lock);
    return wanted.enabled || store_save(serialize(wanted));              // off: already in force, now remembered
}
}  // namespace dh::charge
