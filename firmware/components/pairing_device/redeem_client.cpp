// The redemption (docs/pairing-design.md section 4, steps 5 and 6): one TLS request that trusts exactly the CA of the
// pairing package and checks the host name. There is no other TLS connection in this component, and none that skips
// the check (D1).
#include "internal.hpp"
#include "esp_http_client.h"
#include "esp_tls_errors.h"
#include <algorithm>

namespace dh::pairing_device {
namespace {
constexpr size_t kMaxReply = 16384;
struct Reply {
    std::string body;
    bool overflow = false;
};
esp_err_t on_event(esp_http_client_event_t *event) {
    auto *reply = static_cast<Reply *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        if (reply->body.size() + static_cast<size_t>(event->data_len) > kMaxReply) {
            reply->overflow = true;
            return ESP_FAIL;
        }
        reply->body.append(static_cast<const char *>(event->data), static_cast<size_t>(event->data_len));
    }
    return ESP_OK;
}
}  // namespace

RedeemResult redeem(const dh::pairing::Package &package, const std::string &ca_pem, const std::string &device_id) {
    RedeemResult result;
    const std::string url = dh::pairing::redeem_url(package);
    std::string body = dh::pairing::redeem_request_body(package, device_id);
    if (body.empty() || ca_pem.empty()) return result;  // nothing is sent that was not checked
    Reply reply;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.cert_pem = ca_pem.c_str();  // the only trust root; no certificate bundle is compiled in
    config.timeout_ms = 8000;
    config.event_handler = on_event;
    config.user_data = &reply;
    config.disable_auto_redirect = true;
    config.buffer_size = 1024;
    config.method = HTTP_METHOD_POST;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        std::fill(body.begin(), body.end(), 0);
        return result;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));
    const esp_err_t error = esp_http_client_perform(client);
    result.status = esp_http_client_get_status_code(client);
    int tls_code = 0, tls_flags = 0;
    esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
    esp_http_client_cleanup(client);
    std::fill(body.begin(), body.end(), 0);  // the grant code was in it
    result.tls = dh::pairing::tls_from_flags(error == ESP_OK, tls_flags);
    if (error != ESP_OK || reply.overflow) result.status = 0;
    if (result.tls == dh::pairing::Tls::Ok && !reply.overflow) result.body = std::move(reply.body);
    return result;
}
}  // namespace dh::pairing_device
