#pragma once
#include "policy.hpp"
#include <string>
#include <array>
#include <ctime>
#include "esp_err.h"

namespace dh {
extern const char *const VERSION;
struct Secrets {
    std::string ssid, password, url, agent_id, token, ca_pem;
    std::string update_public_key_pem;
    bool update_key_explicit = false; // USB request, never persisted as a permission.
    std::string ntp_server = "pool.ntp.org";
    uint32_t revision = 0;
    uint64_t generation = 0; // Process-local; never reused after reset/re-pair.
    bool ready() const { return !ssid.empty() && !url.empty() && !token.empty() && !ca_pem.empty(); }
};
struct Action {
    std::string id, label, target, input;
    uint64_t generation = 0;
};
struct Snapshot {
    std::string device_id, wifi = "未配置", ip, agent = "未配对", error;
    std::string time_quality = "unsynced", command = "尚无操作", command_id;
    std::string cpu = "不可用", memory = "不可用", network = "不可用", network_send = "不可用";
    int64_t sampled_ms = 0;
    int64_t metrics_expires_ms = 0;
    bool wifi_ready = false, agent_ready = false, time_valid = false;
    bool metrics_visible = false;
    std::string management = "尚未连接管理台", update_state = "idle", update_error;
    int update_progress = 0;
    int rssi = 0;
    std::array<Action, 12> actions;
    size_t action_count = 0;
};
int64_t monotonic_ms();
void core_init();
bool core_storage_healthy();
Snapshot snapshot();
void mutate(void (*fn)(Snapshot &, void *), void *ctx = nullptr);
bool mutate_if_generation(uint64_t, void (*fn)(Snapshot &, void *), void *ctx = nullptr);
uint64_t config_generation();
esp_err_t with_generation(uint64_t, esp_err_t (*fn)(void *), void *ctx);
Secrets secrets();
DisplayConfig config();
// A UI draft captures its revision and credential generation atomically.
struct ConfigDraft {
    DisplayConfig value;
    uint64_t generation = 0;
};
ConfigDraft config_draft();
esp_err_t save_config(DisplayConfig);
esp_err_t save_config(const ConfigDraft &);
esp_err_t save_managed_config(DisplayConfig, uint64_t generation);
esp_err_t save_secrets(const Secrets &);
esp_err_t reset_config();
esp_err_t reset_config(const ConfigDraft &);
enum class ConfigJobState { Unknown, Pending, Succeeded, Failed };
struct ConfigJobResult {
    uint32_t ticket = 0;
    ConfigJobState state = ConfigJobState::Unknown;
    esp_err_t error = ESP_OK;
    uint32_t revision = 0;
};
// ESP_OK acknowledges enqueueing only. One unfinished job is allowed; busy
// returns ESP_ERR_TIMEOUT and ticket=0. Poll the ticket for durable completion.
esp_err_t queue_config_save(ConfigDraft, uint32_t &ticket);
esp_err_t queue_config_reset(ConfigDraft, uint32_t &ticket);
ConfigJobResult config_job_result(uint32_t ticket);
void config_jobs_init();
bool parse_utc(const char *, time_t &);
bool set_time_utc(const char *, const char *quality);
std::string utc_now();
void network_start();
void network_reconfigure();
// While hotspot pairing runs it owns the radio (docs/pairing-design.md section 13): the Wi-Fi task neither reconnects nor
// reconfigures until the hold is released, and then applies the stored configuration. `network_wifi_started` tells whether
// the Wi-Fi driver runs; an unconfigured device keeps it off.
void network_hold(bool);
bool network_wifi_started();
}
