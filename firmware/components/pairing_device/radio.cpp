// The radio during pairing: a WPA2 hotspot for one phone next to the normal station, and a temporary join of the
// network the person typed, which is not stored until the Agent has accepted the grant.
#include "internal.hpp"
#include "core.hpp"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <algorithm>
#include <cstring>

namespace dh::pairing_device {
namespace {
constexpr EventBits_t kGotIp = BIT0, kLost = BIT1;
esp_netif_t *ap_netif = nullptr;
EventGroupHandle_t events = nullptr;
bool prepared = false, started_by_us = false;

void on_event(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(events, kGotIp);
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) xEventGroupSetBits(events, kLost);
}
bool prepare() {
    if (prepared) return true;
    events = xEventGroupCreate();
    if (!events) return false;
    ap_netif = esp_netif_create_default_wifi_ap();  // 192.168.4.1/24 with a DHCP server: the network of the hotspot
    if (!ap_netif) return false;
    if (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr) != ESP_OK) return false;
    if (esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, nullptr) != ESP_OK) return false;
    prepared = true;
    return true;
}
void wipe(void *p, size_t n) {
    volatile uint8_t *v = static_cast<volatile uint8_t *>(p);
    for (size_t i = 0; i < n; ++i) v[i] = 0;
}
}  // namespace

bool radio_start_driver() {
    if (!prepare()) return false;
    dh::network_hold(true);  // the Wi-Fi task neither reconnects nor reconfigures while pairing owns the radio
    if (dh::network_wifi_started()) return true;
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) {
        dh::network_hold(false);
        return false;
    }
    started_by_us = true;
    return true;
}

bool radio_open_hotspot(const std::string &ssid, const std::string &password) {
    if (!prepared || ssid.empty() || ssid.size() > 32 || password.size() < 8 || password.size() > 63) return false;
    wifi_config_t ap{};
    std::memcpy(ap.ap.ssid, ssid.data(), ssid.size());
    ap.ap.ssid_len = static_cast<uint8_t>(ssid.size());
    std::memcpy(ap.ap.password, password.data(), password.size());
    ap.ap.channel = 6;                      // the hotspot follows the station's channel when the station is connected
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 1;               // one phone
    ap.ap.ssid_hidden = 0;
    ap.ap.pmf_cfg.required = false;
    const bool ok = esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK && esp_wifi_set_config(WIFI_IF_AP, &ap) == ESP_OK;
    wipe(&ap, sizeof(ap));
    return ok;
}

bool radio_join(const std::string &ssid, const std::string &password, int timeout_ms) {
    if (!prepared) return false;
    wifi_config_t sta{};
    std::memcpy(sta.sta.ssid, ssid.data(), std::min<size_t>(ssid.size(), 32));
    std::memcpy(sta.sta.password, password.data(), std::min<size_t>(password.size(), 63));
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));  // the disconnect of the old network reports itself; do not read that as this attempt
    xEventGroupClearBits(events, kGotIp | kLost);
    bool ok = esp_wifi_set_config(WIFI_IF_STA, &sta) == ESP_OK && esp_wifi_connect() == ESP_OK;
    wipe(&sta, sizeof(sta));
    if (!ok) return false;
    const EventBits_t bits = xEventGroupWaitBits(events, kGotIp | kLost, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & kGotIp) != 0;
}

void radio_close() {
    if (prepared) {
        esp_wifi_set_mode(WIFI_MODE_STA);  // the hotspot goes
        if (started_by_us) esp_wifi_stop();  // an unconfigured device had the driver off; it stays so until a configuration is stored
    }
    started_by_us = false;
    dh::network_hold(false);
    dh::network_reconfigure();  // the Wi-Fi task puts the stored configuration back, or, after a good pairing, the new one
}

bool peer_in_hotspot(int socket_fd) {
    if (!ap_netif) return false;
    sockaddr_storage peer{};
    socklen_t length = sizeof(peer);
    if (getpeername(socket_fd, reinterpret_cast<sockaddr *>(&peer), &length) != 0 || peer.ss_family != AF_INET) return false;
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(ap_netif, &info) != ESP_OK) return false;
    const uint32_t address = ntohl(reinterpret_cast<sockaddr_in *>(&peer)->sin_addr.s_addr);
    return dh::pairing::ipv4_in_subnet(address, ntohl(info.ip.addr), ntohl(info.netmask.addr));
}
}  // namespace dh::pairing_device
