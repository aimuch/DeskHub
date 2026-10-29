#pragma once
// Inside the component: the radio, the setup page server and the redemption client.
#include "pairing_glue.hpp"
#include <string>

namespace dh::pairing_device {
// ---- radio.cpp
bool radio_start_driver();   // the Wi-Fi driver runs (an unconfigured device has it off); random numbers are only good now
bool radio_open_hotspot(const std::string &ssid, const std::string &password);   // WPA2, one phone
bool radio_join(const std::string &ssid, const std::string &password, int timeout_ms);   // a network that is not stored yet
void radio_close();          // hotspot off, driver as it was, the stored configuration back in charge
bool peer_in_hotspot(int socket_fd);

// ---- setup_server.cpp
// What the page does with a good form; implemented in pairing_device.cpp. false = refused (the page says so, not why).
bool handle_setup(const dh::pairing::FormFields &);
bool server_start();
void server_stop();

// ---- redeem_client.cpp
struct RedeemResult {
    dh::pairing::Tls tls = dh::pairing::Tls::Unreachable;
    int status = 0;
    std::string body;
};
RedeemResult redeem(const dh::pairing::Package &, const std::string &ca_pem, const std::string &device_id);
}  // namespace dh::pairing_device
