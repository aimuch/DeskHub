#include "audio.hpp"
#include "bsp/esp32_s3_touch_amoled_2_16.h"
#include "core.hpp"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include <atomic>
#include <mutex>
#include <vector>

namespace dh::audio {
namespace {
constexpr int kRate = 16000;      // the codec is run at this rate for every tone
constexpr int kTailMs = 80;       // silence after the tone, so the last samples leave the DMA before the codec is closed
constexpr const char *kNamespace = "dh_snd";
constexpr const char *kKey = "on", *kVolumeKey = "vol";

std::atomic<bool> g_available{false}, g_enabled{false}, g_started{false};
std::atomic<int> g_volume{0};     // the level (audio_policy); the codec's volume for it is read when a tone is played
QueueHandle_t g_queue = nullptr;
std::mutex g_gate_mutex;
Gate g_gate;

// The stored text of a key; "" when there is none. The parsers turn "" into the default (off, low).
std::string load(const char *key) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return "";
    char text[8];
    size_t length = sizeof(text);
    const bool ok = nvs_get_str(h, key, text, &length) == ESP_OK;
    nvs_close(h);
    return ok ? std::string(text) : std::string();
}

bool store(const char *key, const std::string &text) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_str(h, key, text.c_str()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void play(esp_codec_dev_handle_t dev, Tone tone) {
    std::vector<int16_t> samples = render(tone, kRate, 100);
    if (samples.empty()) return;
    samples.insert(samples.end(), static_cast<size_t>(kRate) * kTailMs / 1000, 0);
    esp_codec_dev_sample_info_t format{};
    format.bits_per_sample = 16;
    format.channel = 1;
    format.sample_rate = kRate;
    esp_codec_dev_set_out_vol(dev, codec_volume(g_volume.load()));   // the tone itself is capped at 30 % of full scale
    if (esp_codec_dev_open(dev, &format) != ESP_CODEC_DEV_OK) return;
    esp_codec_dev_write(dev, samples.data(), static_cast<int>(samples.size() * sizeof(int16_t)));
    esp_codec_dev_close(dev);
}

void task(void *) {
    esp_codec_dev_handle_t dev = bsp_audio_codec_speaker_init();   // I2C, I2S and the amplifier pin; NULL when the codec does not answer
    g_available.store(dev != nullptr);
    for (;;) {
        Tone tone;
        if (xQueueReceive(g_queue, &tone, portMAX_DELAY) != pdTRUE) continue;
        if (dev) play(dev, tone);
    }
}

bool ask(Tone tone) {
    if (!g_available.load() || !g_queue) return false;
    return xQueueSend(g_queue, &tone, 0) == pdTRUE;   // never waits: a full queue (two tones waiting) drops this one
}
}  // namespace

void init() {
    if (g_started.exchange(true)) return;
    g_enabled.store(parse_enabled(load(kKey)));
    g_volume.store(parse_volume(load(kVolumeKey)));
    g_queue = xQueueCreate(2, sizeof(Tone));
    if (g_queue) xTaskCreate(task, "dh_audio", 6144, nullptr, 3, nullptr);
}

bool available() { return g_available.load(); }
bool enabled() { return g_enabled.load(); }

bool set_enabled(bool on) {
    const bool ok = store(kKey, serialize_enabled(on));
    if (ok) g_enabled.store(on);
    return ok;
}

int volume() { return g_volume.load(); }

bool reset_settings() {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) { g_enabled.store(false); g_volume.store(0); }
    return ok;
}

bool set_volume(int level) {
    if (level < 0 || level >= kVolumeLevels) return false;
    const bool ok = store(kVolumeKey, serialize_volume(level));
    if (ok) g_volume.store(level);
    return ok;
}

bool test() {
    std::lock_guard<std::mutex> lock(g_gate_mutex);
    if (!g_available.load() || !g_gate.allow_test(dh::monotonic_ms())) return false;   // the clock of on_command and on_pairing_done: one gate, one clock
    return ask(Tone::Confirm);
}

void on_command(const std::string &text, bool quiet_hours, int64_t now_ms) {
    const auto tone = tone_for_command(text);
    if (!tone) return;
    std::lock_guard<std::mutex> lock(g_gate_mutex);
    if (g_available.load() && g_gate.allow(now_ms, g_enabled.load(), quiet_hours)) ask(*tone);
}

void on_pairing_done(int64_t now_ms) {
    std::lock_guard<std::mutex> lock(g_gate_mutex);
    if (g_available.load() && g_gate.allow(now_ms, g_enabled.load(), false)) ask(Tone::Confirm);
}
}  // namespace dh::audio
