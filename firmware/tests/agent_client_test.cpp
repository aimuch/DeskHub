// Compile the real client against deterministic transport/service boundaries.
// There is no hardware/network access, and no mock backend enters production.
#include "core.hpp"
#include "esp_http_client.h"
#include <cassert>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>
#include <cstdio>

static dh::Snapshot current;
static dh::Secrets paired;
static uint64_t epoch = 1;
static int64_t tick = 1000;
static std::vector<unsigned char> queued;
static size_t queue_item_size;
static unsigned transport_calls;
static std::deque<std::string> replies;
static std::function<void()> during_response;

namespace dh {
int64_t monotonic_ms() { return tick; }
Snapshot snapshot() { return current; }
void mutate(void (*fn)(Snapshot &, void *), void *p) { fn(current, p); }
bool mutate_if_generation(uint64_t value, void (*fn)(Snapshot &, void *), void *p) {
    if (value != epoch) return false;
    fn(current, p); return true;
}
uint64_t config_generation() { return epoch; }
Secrets secrets() { auto value = paired; value.generation = epoch; return value; }
bool parse_utc(const char *value, time_t &out) {
    if (!value || std::strlen(value) < 20) return false;
    struct tm date{};
    if (!strptime(std::string(value).substr(0, 19).c_str(), "%Y-%m-%dT%H:%M:%S", &date)) return false;
    out = timegm(&date); return true;
}
std::string utc_now() {
    time_t now = time(nullptr); struct tm date{}; gmtime_r(&now, &date);
    char value[32]; strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%SZ", &date); return value;
}
}

#include "../components/agent_client/agent_client.cpp"

struct TestHttpHandle { esp_http_client_config_t config; };
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) { return new TestHttpHandle{*config}; }
int esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *) { return ESP_OK; }
int esp_http_client_set_method(esp_http_client_handle_t, int) { return ESP_OK; }
int esp_http_client_set_post_field(esp_http_client_handle_t, const char *, int) { return ESP_OK; }
int esp_http_client_get_status_code(esp_http_client_handle_t) { return 200; }
int esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t, int *code, int *flags) { *code = *flags = 0; return 0; }
int esp_http_client_cleanup(esp_http_client_handle_t handle) { delete handle; return ESP_OK; }
int esp_http_client_perform(esp_http_client_handle_t handle) {
    ++transport_calls;
    assert(!replies.empty());
    auto body = replies.front(); replies.pop_front();
    esp_http_client_event_t event{HTTP_EVENT_ON_DATA, handle->config.user_data, body.data(), static_cast<int>(body.size())};
    auto result = handle->config.event_handler(&event);
    if (during_response) { auto callback = std::move(during_response); during_response = nullptr; callback(); }
    return result;
}
void esp_fill_random(void *output, size_t count) { std::memset(output, 17, count); }
uint32_t esp_random() { return 7; }
void vTaskDelay(TickType_t ms) { tick += ms; }
QueueHandle_t xQueueCreate(unsigned, unsigned size) { queue_item_size = size; return &queued; }
BaseType_t xQueueSend(QueueHandle_t, const void *item, TickType_t) {
    if (!queued.empty()) return pdFALSE;
    auto *bytes = static_cast<const unsigned char *>(item); queued.assign(bytes, bytes + queue_item_size); return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t, void *item, TickType_t) {
    if (queued.empty()) return pdFALSE;
    std::memcpy(item, queued.data(), queue_item_size); queued.clear(); return pdTRUE;
}

static dh::Action initialize() {
    current = {}; current.agent_ready = current.wifi_ready = current.time_valid = true;
    paired = {}; paired.url = "https://agent.invalid"; paired.agent_id = "agent-test";
    paired.token = std::string(43, 'x'); paired.ca_pem = "TEST-BOUNDARY";
    epoch = 10; tick = 1000; transport_calls = 0; replies.clear(); during_response = nullptr;
    queued.clear(); dh::busy = false; dh::agent_start();
    dh::Action action{"desktop.mac", "Mac", "monitor-1", "mac-usbc", epoch};
    current.actions[0] = action; current.action_count = 1; return action;
}

int main() {
    auto action = initialize();
    assert(dh::agent_submit(action));
    assert(!dh::agent_submit(action)); // no second gesture waits behind execution
    dh::Pending pending{}; assert(xQueueReceive(dh::actions, &pending, 0) == pdTRUE);
    ++epoch; // disconnect/re-pair occurred before the worker picked up the click
    dh::execute(pending, dh::secrets());
    assert(transport_calls == 0);
    dh::busy = false;
    assert(!dh::agent_submit(action)); // stale visible action cannot use new credentials

    action = initialize(); assert(dh::agent_submit(action));
    assert(xQueueReceive(dh::actions, &pending, 0) == pdTRUE);
    tick += 10001;
    dh::execute(pending, dh::secrets()); assert(transport_calls == 0);

    initialize();
    replies.push_back("{\"agent_id\":\"agent-test\",\"protocol_version\":\"1.0\",\"server_time\":\"" + dh::utc_now() + "\"}");
    during_response = [] { ++epoch; current.agent_ready = false; current.action_count = 0; };
    assert(!dh::refresh(dh::secrets()));
    assert(!current.agent_ready && current.action_count == 0 && transport_calls == 1);

    initialize();
    replies.push_back("{\"state\":\"succeeded\"}garbage");
    assert(dh::request(dh::secrets(), "/api/v1/status") == nullptr);
    assert(current.error == "INVALID_AGENT_JSON");

    initialize();
    time_t old = time(nullptr) - 5; struct tm date{}; gmtime_r(&old, &date); char observed[32];
    strftime(observed, sizeof(observed), "%Y-%m-%dT%H:%M:%SZ", &date);
    auto body = std::string("{\"hosts\":[{\"metrics\":[{\"key\":\"cpu.utilization\",\"quality\":\"valid\",\"value\":23,\"ttl_ms\":6000,\"observed_at\":\"") + observed + "\"}]}]}";
    auto *metrics = cJSON_Parse(body.c_str()); assert(metrics);
    dh::parse_metrics(metrics, epoch); cJSON_Delete(metrics);
    assert(current.metrics_expires_ms <= tick + 1000); // receiving an old sample does not grant a new six seconds
    puts("client: busy rejection, stale-generation dispatch, expired queued click, disconnect-during-response, strict JSON, original metric TTL passed");
}
