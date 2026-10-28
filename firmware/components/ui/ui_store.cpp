#include "ui_store.hpp"
#include "nvs.h"

namespace dh::ui {
namespace {
constexpr const char *kNamespace = "dh_ui";
constexpr const char *kKey = "appearance";
}  // namespace

std::string store_load() {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return "";
    char buf[96];
    size_t len = sizeof(buf);
    const esp_err_t e = nvs_get_str(h, kKey, buf, &len);
    nvs_close(h);
    return e == ESP_OK ? std::string(buf) : std::string();
}
bool store_save(const std::string &text) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, kKey, text.c_str()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
}  // namespace dh::ui
