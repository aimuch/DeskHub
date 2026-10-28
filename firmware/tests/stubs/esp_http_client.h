#pragma once
#include "esp_err.h"
enum { HTTP_EVENT_ON_DATA = 1, HTTP_METHOD_POST = 1 };
struct esp_http_client_event_t { int event_id; void *user_data; void *data; int data_len; };
struct esp_http_client_config_t {
    const char *url = nullptr, *cert_pem = nullptr;
    int timeout_ms = 0;
    esp_err_t (*event_handler)(esp_http_client_event_t *) = nullptr;
    void *user_data = nullptr;
    bool disable_auto_redirect = false;
    int buffer_size = 0;
};
struct TestHttpHandle;
using esp_http_client_handle_t = TestHttpHandle *;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
int esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
int esp_http_client_set_method(esp_http_client_handle_t, int);
int esp_http_client_set_post_field(esp_http_client_handle_t, const char *, int);
int esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t, int *, int *);
int esp_http_client_cleanup(esp_http_client_handle_t);
