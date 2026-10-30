// The pairing task: watches the button, opens the session, moves it through join and redemption, and tears everything
// down when it ends. Every decision is the pure Session's (components/pairing); this file only reports what happened and
// switches hardware on and off. No secret is logged: nothing in this component logs at all.
#include "pairing_device.hpp"
#include "audio.hpp"
#include "internal.hpp"
#include "board.hpp"
#include "core.hpp"
#include "ui.hpp"
#include "driver/gpio.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <algorithm>
#include <atomic>
#include <memory>

namespace dh::pairing_device {
namespace {
using namespace dh::pairing;

// BOOT, active low. The board schematic was not at hand: this pin is an assumption (docs/pairing-design.md section 13).
constexpr gpio_num_t kButton = GPIO_NUM_0;
constexpr int64_t kBootGraceMs = 8000;   // an unpaired device opens its hotspot this long after boot, once
constexpr int kMaxSaveRetries = 3;       // a failing flash is not written to for the rest of the window
constexpr int kJoinTimeoutMs = 20000;

// The hardware random generator is only a true one while the radio runs: the session asks only after the driver is up.
struct HwRandom : Random {
    std::atomic<bool> up{false};
    bool fill(uint8_t *out, size_t length) override {
        if (!up.load()) return false;
        esp_fill_random(out, length);
        return true;
    }
};
// The same save path as USB pairing (dh::save_secrets): all or nothing, then read back.
struct DeviceSink : Sink {
    bool current(Secrets &out) override { out = dh::secrets(); return true; }
    bool set_time(const std::string &utc) override { return dh::set_time_utc(utc.c_str(), "manual"); }
    bool commit(const Secrets &s) override { return dh::save_secrets(s) == ESP_OK; }
    bool readback(Secrets &out) override { out = dh::secrets(); return true; }
};

HwRandom rng;
DeviceSink sink;
std::unique_ptr<Session> session;
SemaphoreHandle_t lock = nullptr;
LongPress press;
bool want_open = false, active = false, auto_tried = false, pending = false;
std::string pending_ssid, pending_password;
int save_retries = 0;
int64_t last_retry_ms = 0;
std::atomic<bool> cancel_requested{false};
std::atomic<int> shown_phase{0}, shown_why{0};

const char *const kPhase[] = {"closed", "open", "joining", "redeeming", "save_failed", "done"};
const char *const kWhy[] = {"none", "not_allowed", "random_failed", "wrong_phase", "wrong_code", "too_many_wrong_codes", "code_required", "timeout", "cancelled",
                            "invalid_input", "too_many_attempts", "wifi_failed", "agent_unreachable", "certificate_expired", "hostname_mismatch", "chain_invalid",
                            "redeem_refused", "bad_reply", "save_failed", "readback_mismatch"};
void note(Why why) {
    shown_phase.store(static_cast<int>(session ? session->phase() : Phase::Closed));
    if (why != Why::None) shown_why.store(static_cast<int>(why));
}
void wipe(std::string &s) { std::fill(s.begin(), s.end(), 0); s.clear(); }
void on_cancel() { cancel_requested.store(true); }

// Everything the session switched on goes off, whatever the reason. Holding the lock.
void teardown(bool paired_now) {
    ui_pairing_hide();
    server_stop();
    radio_close();
    rng.up.store(false);
    wipe(pending_ssid);
    wipe(pending_password);
    pending = false;
    active = false;
    save_retries = 0;
    if (paired_now) {
        dh::board_rtc_write(time(nullptr));  // the phone's clock was set at the start; keep it across a reset
        dh::audio::on_pairing_done(dh::monotonic_ms());
    }
    note(Why::None);
}

void begin(int64_t now, bool paired, bool long_press) {
    if (!radio_start_driver()) { note(Why::NotAllowed); return; }
    rng.up.store(true);
    const Why why = session->open(now, paired, long_press);
    if (why != Why::None) {
        radio_close();
        rng.up.store(false);
        note(why);
        return;
    }
    const Offer &offer = session->offer();
    if (!radio_open_hotspot(offer.hotspot_ssid, offer.hotspot_password) || !server_start()) {
        session->close(Why::NotAllowed);
        server_stop();
        radio_close();
        rng.up.store(false);
        note(Why::NotAllowed);
        return;
    }
    PairingInfo info;
    info.code = offer.setup_code;
    info.hotspot = offer.hotspot_ssid;
    info.hotspot_password = offer.hotspot_password;
    info.remaining_s = static_cast<int>(kWindowMs / 1000);
    info.total_s = info.remaining_s;
    cancel_requested.store(false);
    ui_pairing_show(info, on_cancel);   // the screen stays awake while it is shown (ui.cpp)
    active = true;
    note(Why::None);
}

// The blocking part: join the network the person typed, then redeem. No lock is held while the radio works; the
// session is in Joining or Redeeming, where the setup page can change nothing.
void connect_and_redeem() {
    std::string ssid, password;
    Package package;
    std::string ca_pem;
    xSemaphoreTake(lock, portMAX_DELAY);
    ssid = pending_ssid;
    password = pending_password;
    wipe(pending_ssid);
    wipe(pending_password);
    pending = false;
    package = session->package();
    ca_pem = session->ca_pem();
    xSemaphoreGive(lock);

    const bool joined = radio_join(ssid, password, kJoinTimeoutMs);
    wipe(ssid);
    wipe(password);

    xSemaphoreTake(lock, portMAX_DELAY);
    Why why = session->joined(dh::monotonic_ms(), joined);
    note(why);
    const bool go = joined && session->is_open() && session->phase() == Phase::Redeeming;
    xSemaphoreGive(lock);
    RedeemResult result;
    if (go) result = redeem(package, ca_pem, dh::snapshot().device_id);

    xSemaphoreTake(lock, portMAX_DELAY);
    if (go) {
        why = session->redeemed(dh::monotonic_ms(), result.tls, result.status, result.body);
        note(why);
    }
    xSemaphoreGive(lock);
    wipe(result.body);
    wipe(ca_pem);
    wipe(package.code);
    wipe(package.grant_id);
}

void service(int64_t now) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!active) {
        const bool paired = dh::secrets().ready();
        const bool boot_open = !paired && !auto_tried && now >= kBootGraceMs;
        if (want_open || boot_open) {
            if (boot_open) auto_tried = true;
            begin(now, paired, want_open);
            want_open = false;
        }
        xSemaphoreGive(lock);
        return;
    }
    if (cancel_requested.exchange(false)) {
        session->close(Why::Cancelled);
        note(Why::Cancelled);
    }
    note(session->tick(now));
    bool join_now = false;
    if (session->phase() == Phase::Joining) join_now = pending;
    if (session->phase() == Phase::SaveFailed && now - last_retry_ms >= 2000) {
        last_retry_ms = now;
        if (++save_retries > kMaxSaveRetries) {
            session->close(Why::SaveFailed);
            note(Why::SaveFailed);
        } else {
            note(session->retry_save(now));
        }
    }
    if (session->phase() == Phase::Done || !session->is_open()) {
        teardown(session->phase() == Phase::Done);
        xSemaphoreGive(lock);
        return;
    }
    xSemaphoreGive(lock);
    if (join_now) connect_and_redeem();
}

void run(void *) {
    gpio_config_t pin{};
    pin.pin_bit_mask = 1ULL << kButton;
    pin.mode = GPIO_MODE_INPUT;
    pin.pull_up_en = GPIO_PULLUP_ENABLE;   // an unconnected pin reads "not pressed", so a wrong pin never opens the hotspot
    gpio_config(&pin);
    for (;;) {
        const int64_t now = dh::monotonic_ms();
        press.edge(gpio_get_level(kButton) == 0, now);
        if (press.poll(now)) {
            xSemaphoreTake(lock, portMAX_DELAY);
            want_open = true;
            xSemaphoreGive(lock);
        }
        service(now);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
}  // namespace

// Called by the setup page with a form that parsed. The code and the form go to the session; the radio is switched by
// the task, not here, so the answer reaches the phone before the channel can change.
bool handle_setup(const dh::pairing::FormFields &form) {
    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    bool accepted = false;
    if (session && active && session->is_open() && !pending) {
        const int64_t now = dh::monotonic_ms();
        Why why = session->enter_code(now, form.code);
        if (why == Why::None) why = session->submit(now, dh::pairing::SetupForm{form.ssid, form.wifi_password, form.package, form.utc});
        if (why == Why::None) {
            pending_ssid = form.ssid;
            pending_password = form.wifi_password;
            pending = true;
            accepted = true;
        }
        note(why);
    }
    xSemaphoreGive(lock);
    return accepted;
}

void start() {
    if (lock) return;
    lock = xSemaphoreCreateMutex();
    session = std::make_unique<Session>(rng, sink, dh::snapshot().device_id);
    xTaskCreate(run, "dh_pair", 12288, nullptr, 3, nullptr);
}

Status status() {
    Status out;
    const int phase = shown_phase.load(), why = shown_why.load();
    out.phase = phase >= 0 && phase < 6 ? kPhase[phase] : "closed";
    out.why = why >= 0 && why < 20 ? kWhy[why] : "none";
    out.open = phase == 1 || phase == 2 || phase == 3 || phase == 4;
    return out;
}
}  // namespace dh::pairing_device
