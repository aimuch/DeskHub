#include "ui_store.hpp"
#include "nvs.h"
#include <vector>

namespace dh::ui {
namespace {
constexpr const char *kNamespace = "dh_ui";
constexpr const char *kKey = "appearance";
constexpr const char *kSourcesKey = "sources";

std::string load(const char *key, size_t max) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return "";
    std::vector<char> buf(max + 1, 0);
    size_t len = buf.size();
    const esp_err_t e = nvs_get_str(h, key, buf.data(), &len);
    nvs_close(h);
    return e == ESP_OK ? std::string(buf.data()) : std::string();
}
bool save(const char *key, const std::string &text) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, key, text.c_str()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
}  // namespace

std::string store_load() { return load(kKey, 95); }
bool store_save(const std::string &text) { return save(kKey, text); }
std::string sources_load() { return load(kSourcesKey, 2048); }
bool sources_save(const std::string &text) { return save(kSourcesKey, text); }
bool store_clear() {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = true;
    for (const char *key : {kKey, kSourcesKey}) {
        const esp_err_t e = nvs_erase_key(h, key);
        ok = ok && (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND);
    }
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
}  // namespace dh::ui
