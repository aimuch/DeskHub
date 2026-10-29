// The setup page (docs/pairing-design.md section 12, item 2): HTTP inside the hotspot only, two routes, a small body.
// Nothing the phone sends is logged or echoed. The page answers only to addresses of the hotspot network.
#include "internal.hpp"
#include "esp_http_server.h"
#include "esp_log.h"
#include <algorithm>
#include <cstring>

namespace dh::pairing_device {
namespace {
using dh::pairing::FormFields;
httpd_handle_t server = nullptr;

void headers(httpd_req_t *req, const char *type) {
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; form-action 'self'; base-uri 'none'");
}
esp_err_t send_page(httpd_req_t *req, const char *status, const std::string &html) {
    headers(req, "text/html; charset=utf-8");
    httpd_resp_set_status(req, status);
    return httpd_resp_send(req, html.c_str(), static_cast<ssize_t>(html.size()));
}
esp_err_t refuse(httpd_req_t *req, const char *status) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, nullptr, 0);
}
bool allowed(httpd_req_t *req) { return peer_in_hotspot(httpd_req_to_sockfd(req)); }

esp_err_t get_root(httpd_req_t *req) {
    if (!allowed(req)) return refuse(req, "403 Forbidden");
    return send_page(req, "200 OK", dh::pairing::setup_page_html());
}

esp_err_t post_setup(httpd_req_t *req) {
    if (!allowed(req)) return refuse(req, "403 Forbidden");
    char type[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK || std::strncmp(type, "application/x-www-form-urlencoded", 33) != 0)
        return refuse(req, "415 Unsupported Media Type");
    const size_t length = req->content_len;
    if (length == 0 || length > dh::pairing::kMaxFormBytes) return refuse(req, "413 Payload Too Large");
    std::string body(length, '\0');
    size_t got = 0;
    while (got < length) {
        const int n = httpd_req_recv(req, &body[got], length - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            std::fill(body.begin(), body.end(), 0);
            return ESP_FAIL;
        }
        got += static_cast<size_t>(n);
    }
    FormFields form;
    const auto error = dh::pairing::parse_form(body, form);
    std::fill(body.begin(), body.end(), 0);  // the password and the package were in it
    bool accepted = false;
    if (error == dh::pairing::FormError::None) accepted = handle_setup(form);
    std::fill(form.wifi_password.begin(), form.wifi_password.end(), 0);
    std::fill(form.package.begin(), form.package.end(), 0);
    return send_page(req, accepted ? "200 OK" : "400 Bad Request", dh::pairing::setup_result_html(accepted));
}

// A phone that asks for its own connectivity page (a captive portal check) is sent to the setup page.
esp_err_t not_found(httpd_req_t *req, httpd_err_code_t) {
    if (!allowed(req)) return refuse(req, "403 Forbidden");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, nullptr, 0);
}
}  // namespace

bool server_start() {
    if (server) return true;
    // The server logs addresses and the size of what arrives; nothing of this port is worth keeping.
    for (const char *tag : {"httpd", "httpd_uri", "httpd_txrx", "httpd_sess", "httpd_parse"}) esp_log_level_set(tag, ESP_LOG_NONE);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 3;
    config.max_uri_handlers = 2;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.uri_match_fn = nullptr;  // exact paths
    if (httpd_start(&server, &config) != ESP_OK) {
        server = nullptr;
        return false;
    }
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = get_root, .user_ctx = nullptr};
    const httpd_uri_t setup = {.uri = "/setup", .method = HTTP_POST, .handler = post_setup, .user_ctx = nullptr};
    if (httpd_register_uri_handler(server, &root) != ESP_OK || httpd_register_uri_handler(server, &setup) != ESP_OK ||
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found) != ESP_OK) {
        httpd_stop(server);
        server = nullptr;
        return false;
    }
    return true;
}

void server_stop() {
    if (!server) return;
    httpd_stop(server);
    server = nullptr;
}
}  // namespace dh::pairing_device
